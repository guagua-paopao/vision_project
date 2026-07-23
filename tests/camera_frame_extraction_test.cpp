#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "business/camera_pipeline.h"
#include "business/camera_frame_retention.h"
#include "business/postgres_client.h"
#include "server/camera_task_queue.h"
#include "postgres_test_guard.h"

namespace {

using namespace yolo11_server;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(encoded.begin(), encoded.end());
}

struct SourceState {
    std::atomic<int> factory_calls{ 0 };
    std::atomic<int> opens{ 0 };
    std::atomic<bool> active{ false };
    std::atomic<unsigned long long> sequence{ 0 };
    mutable std::mutex mutex;
    SharedCameraFrame latest;
};

class TimedFrameSource final : public ICameraFrameSource {
public:
    explicit TimedFrameSource(std::shared_ptr<SourceState> state) : state_(std::move(state)) {}
    ~TimedFrameSource() noexcept override { stop(); }

    bool start(std::string& error) override {
        error.clear();
        if (running_.exchange(true)) return true;
        ++state_->opens;
        state_->active.store(true);
        thread_ = std::thread([this]() {
            while (running_.load()) {
                auto envelope = std::make_shared<FrameEnvelope>();
                envelope->sequence = ++state_->sequence;
                envelope->capture_time_ms = nowMs();
                envelope->publish_time = std::chrono::steady_clock::now();
                envelope->image = cv::Mat(240, 320, CV_8UC3,
                    cv::Scalar(envelope->sequence % 255, 90, 180)).clone();
                {
                    std::lock_guard<std::mutex> lock(state_->mutex);
                    state_->latest = envelope;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
        });
        return true;
    }

    void stop() noexcept override {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        state_->active.store(false);
    }

    SharedCameraFrame latest(std::uint64_t after_sequence) const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->latest || state_->latest->sequence <= after_sequence) return {};
        return state_->latest;
    }

    RtspCaptureMetrics metrics() const override {
        RtspCaptureMetrics result;
        result.state = running_.load() ? "running" : "stopped";
        result.backend_name = "FFMPEG";
        result.open_count = state_->opens.load();
        result.capture_fps = 25.0;
        result.source_fps = 25.0;
        result.width = 320;
        result.height = 240;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->latest) {
            result.last_frame_time_ms = state_->latest->capture_time_ms;
            result.latest_frame_age_ms = std::max(0LL, nowMs() - result.last_frame_time_ms);
        }
        return result;
    }

    bool active() const override { return running_.load(); }

private:
    std::shared_ptr<SourceState> state_;
    std::atomic<bool> running_{ false };
    std::thread thread_;
};

class FakeControl final : public ICameraTaskRuntimeControl {
public:
    bool acquireRunLease(const std::string& task, const std::string& run, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& holder = leases_[task];
        if (!holder.empty() && holder != run) { error = "lease conflict"; return false; }
        holder = run;
        error.clear();
        return true;
    }
    bool refreshRunLease(const std::string& task, const std::string& run, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (leases_[task] != run) { error = "lease lost"; return false; }
        error.clear();
        return true;
    }
    bool releaseRunLease(const std::string& task, const std::string& run, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (leases_[task] == run) leases_.erase(task);
        error.clear();
        return true;
    }
    bool isStopRequested(const std::string& run, bool& requested, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        requested = stops_[run];
        error.clear();
        return true;
    }
    bool updateRunStatus(const CameraTaskRunHotStatus& status, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        statuses_[status.run_id] = status;
        error.clear();
        return true;
    }
    bool updateHubStatus(const CameraHubStatus&, std::string& error) override {
        error.clear();
        return true;
    }
    CameraTaskRunHotStatus status(const std::string& run_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return statuses_[run_id];
    }
private:
    std::mutex mutex_;
    std::map<std::string, std::string> leases_;
    std::map<std::string, bool> stops_;
    std::map<std::string, CameraTaskRunHotStatus> statuses_;
};

class RecordingInferenceSink final : public ICameraFrameJobSink {
public:
    bool submitLatest(
        CameraFrameJob job,
        CameraFrameJobSubmitResult& result,
        std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sequences_[job.run_id].push_back(job.source_sequence);
        result.accepted = true;
        error.clear();
        return true;
    }

    void detachCamera(const std::string&, const std::string& run_id) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        detached_.push_back(run_id);
    }

    std::vector<unsigned long long> sequences(const std::string& run_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return sequences_[run_id];
    }

    bool detached(const std::string& run_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::find(detached_.begin(), detached_.end(), run_id) != detached_.end();
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::vector<unsigned long long>> sequences_;
    std::vector<std::string> detached_;
};

CameraTaskDefinition makeTask(
    const std::string& id,
    int interval_ms,
    const std::string& mode,
    int max_saved,
    long long now
) {
    CameraTaskDefinition task;
    task.task_id = id;
    task.name = id;
    task.camera_profile = "entry_camera_01";
    task.frame_interval_ms = interval_ms;
    task.output_mode = mode;
    task.jpeg_quality = 85;
    task.max_width = 160;
    task.max_height = 120;
    task.retention_days = 7;
    task.max_saved_frames = max_saved;
    task.version = 1;
    task.created_at_ms = now;
    task.updated_at_ms = now;
    return task;
}

CameraTaskRunRecord makeRun(const std::string& id, const CameraTaskDefinition& task, long long now) {
    CameraTaskRunRecord run;
    run.run_id = id;
    run.task_id = task.task_id;
    run.definition_version = task.version;
    run.definition_json = "{\"camera_profile\":\"entry_camera_01\"}";
    run.status = "queued";
    run.camera_profile = task.camera_profile;
    run.create_time_ms = now;
    run.last_update_ms = now;
    return run;
}

CameraTaskCommand makeCommand(const CameraTaskDefinition& task, const CameraTaskRunRecord& run) {
    CameraTaskCommand command;
    command.task_id = task.task_id;
    command.run_id = run.run_id;
    command.camera_profile = task.camera_profile;
    command.definition_version = task.version;
    command.frame_interval_ms = task.frame_interval_ms;
    command.output_mode = task.output_mode;
    command.jpeg_quality = task.jpeg_quality;
    command.max_width = task.max_width;
    command.max_height = task.max_height;
    command.retention_days = task.retention_days;
    command.max_saved_frames = task.max_saved_frames;
    command.analysis_enabled = task.analysis_enabled;
    command.target_infer_fps = task.target_infer_fps;
    command.algorithm_profile = task.algorithm_profile;
    command.algorithms = task.algorithms;
    command.create_time_ms = run.create_time_ms;
    return command;
}

}  // namespace

int main() {
    if (const int guard = requireDisposablePostgresTestDatabase()) return guard;
    const long long stamp = nowMs();
    const auto root = std::filesystem::temp_directory_path() /
        ("camera_frame_extraction_test_" + std::to_string(stamp));
    CameraTasksSection config;
    config.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN";
    config.output_dir = pathUtf8(root / "output");
    config.writer_threads = 2;
    config.writer_queue_capacity = 32;
    config.writer_queue_capacity_per_run = 8;
    config.lease_ttl_seconds = 5;
    config.lease_refresh_seconds = 1;
    config.retention_sweep_interval_seconds = 3600;
    config.retention_batch_size = 100;

    std::string error;
    std::string code;
    PostgresConnection database;
    require(database.openFromEnvironment(config.postgres_dsn_env, error),
        "test PostgreSQL connection must open: " + error);
    require(database.exec(
        "DROP TABLE IF EXISTS callback_outbox,security_alert_events,camera_idempotency_keys,"
        "camera_frames,camera_task_runs,camera_tasks,camera_schema_version CASCADE;",
        error), "test PostgreSQL schema reset must succeed: " + error);
    auto repository = std::make_shared<CameraTaskRepository>(config);
    require(repository->initialize(error), "repository must initialize: " + error);
    const auto fast_task = makeTask("ct_fast", 100, "both", 3, stamp);
    auto slow_task = makeTask("ct_slow", 250, "archive", 100, stamp);
    slow_task.analysis_enabled = true;
    slow_task.target_infer_fps = 10.0;
    slow_task.algorithm_profile = "security_default";
    slow_task.algorithms = { "people_flow" };
    require(repository->createTask(fast_task, code, error), "fast task must persist");
    require(repository->createTask(slow_task, code, error), "slow task must persist");
    const auto fast_run = makeRun("cr_fast", fast_task, stamp + 1);
    const auto slow_run = makeRun("cr_slow", slow_task, stamp + 2);
    require(repository->createRun(fast_run, code, error), "fast run must persist");
    require(repository->createRun(slow_run, code, error), "slow run must persist");

    auto writer = std::make_shared<FrameArtifactWriter>(config, repository);
    require(writer->start(error), "writer must start: " + error);
    auto source_state = std::make_shared<SourceState>();
    CameraHubSection hub_config;
    hub_config.max_active_hubs = 2;
    hub_config.idle_grace_ms = 50;
    auto registry = std::make_shared<SharedCameraFrameHubRegistry>(hub_config,
        [source_state](const std::string&) {
            ++source_state->factory_calls;
            return std::make_unique<TimedFrameSource>(source_state);
        });
    std::shared_ptr<FrameSubscription> people_flow;
    require(registry->subscribe("entry_camera_01", { "pf_test", "people_flow" },
        people_flow, error), "People Flow subscription must start the shared source");
    auto control = std::make_shared<FakeControl>();
    auto inference_sink = std::make_shared<RecordingInferenceSink>();
    auto fast = std::make_shared<CameraPipeline>(
        makeCommand(fast_task, fast_run), config, 500, "test_camera", registry,
        writer, repository, control);
    auto slow = std::make_shared<CameraPipeline>(
        makeCommand(slow_task, slow_run), config, 500, "test_camera", registry,
        writer, repository, control, inference_sink);

    std::atomic<bool> consume_people_flow{ true };
    std::atomic<int> people_flow_frames{ 0 };
    std::thread people_flow_thread([&]() {
        while (consume_people_flow.load()) {
            FrameReadResult result;
            if (people_flow->tryReadLatest(result)) ++people_flow_frames;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    });
    std::thread fast_thread([&]() { fast->run(); });
    std::thread slow_thread([&]() { slow->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));

    const auto live_hubs = registry->snapshots();
    require(live_hubs.size() == 1, "same profile must create one Hub");
    require(live_hubs.front().open_count == 1 && source_state->factory_calls.load() == 1 &&
        source_state->opens.load() == 1, "People Flow and both runs must share one source open");
    require(live_hubs.front().subscriber_count == 3,
        "Hub must expose one People Flow and two Camera Task subscriptions");
    const auto fast_hot = control->status(fast_run.run_id);
    const auto slow_hot = control->status(slow_run.run_id);
    require(fast_hot.pipeline_thread_running && slow_hot.pipeline_thread_running &&
            fast_hot.pipeline_started_at_ms > 0 && slow_hot.pipeline_started_at_ms > 0,
        "each running camera must publish one live CameraPipeline thread");
    require(fast_hot.sampled_frames > slow_hot.sampled_frames &&
            fast_hot.sample_fps > 0.0 && slow_hot.sample_fps > 0.0,
        "CameraPipeline hot metrics must expose independent monotonic sampling");
    const auto live_analysis_sequences = inference_sink->sequences(slow_run.run_id);
    require(live_analysis_sequences.size() >= 9 &&
            std::adjacent_find(
                live_analysis_sequences.begin(), live_analysis_sequences.end(),
                [](auto left, auto right) { return right <= left; }) ==
                live_analysis_sequences.end(),
        "analysis FrameJobs must be sampled near target FPS with strictly increasing source_sequence");

    fast->requestStop();
    fast_thread.join();
    require(source_state->active.load(), "stopping one run must not stop the shared source");
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    slow->requestStop();
    slow_thread.join();
    require(inference_sink->detached(slow_run.run_id),
        "stopping a CameraPipeline must detach its inference session");

    bool found = false;
    const auto bad_task = makeTask("ct_bad", 100, "latest", 10, stamp);
    const auto bad_run = makeRun("cr_bad", bad_task, stamp + 3);
    require(repository->createTask(bad_task, code, error), "failure-isolation task must persist");
    require(repository->createRun(bad_run, code, error), "failure-isolation run must persist");
    {
        std::ofstream blocking_file(root / "output" / "ct_bad", std::ios::binary);
        blocking_file << "not a directory";
    }
    const int people_flow_before_failure = people_flow_frames.load();
    auto bad = std::make_shared<CameraPipeline>(
        makeCommand(bad_task, bad_run), config, 500, "test_camera", registry,
        writer, repository, control);
    std::thread bad_thread([&]() { bad->run(); });
    bad_thread.join();
    CameraTaskRunRecord bad_result;
    require(repository->getRun(bad_run.run_id, bad_result, found, error) && found &&
        bad_result.status == "failed" && bad_result.error_code == "OUTPUT_PATH_UNSAFE",
        "a task-local output failure must fail only that run");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    require(source_state->active.load() && people_flow_frames.load() > people_flow_before_failure,
        "task-local write failure must not stop the shared Hub or People Flow cursor");

    consume_people_flow.store(false);
    people_flow_thread.join();
    require(people_flow_frames.load() > 20,
        "independent People Flow cursor must keep consuming while extraction writes");

    CameraTaskRunRecord fast_result;
    CameraTaskRunRecord slow_result;
    require(repository->getRun(fast_run.run_id, fast_result, found, error) && found,
        "fast run result must exist");
    require(repository->getRun(slow_run.run_id, slow_result, found, error) && found,
        "slow run result must exist");
    require(fast_result.status == "stopped" && slow_result.status == "stopped",
        "both CameraPipeline threads must stop cleanly");
    require(!control->status(fast_run.run_id).pipeline_thread_running &&
            !control->status(slow_run.run_id).pipeline_thread_running,
        "terminal hot status must prove both CameraPipeline threads exited");
    require(fast_result.saved_frames >= 11 && fast_result.saved_frames <= 18,
        "100 ms monotonic sampler must save at the expected rate without bursts");
    require(slow_result.saved_frames >= 6 && slow_result.saved_frames <= 10,
        "250 ms monotonic sampler must keep its independent cadence");
    require(inference_sink->sequences(slow_run.run_id).size() >
            static_cast<std::size_t>(slow_result.saved_frames),
        "10 FPS analysis sampling must remain independent from 4 FPS JPEG extraction");
    require(fast_result.saved_frames > slow_result.saved_frames,
        "different intervals on one Hub must produce independent output rates");
    require(fast_result.width == 160 && fast_result.height == 120,
        "writer resize must preserve aspect ratio within configured bounds");

    writer->stop();
    const auto writer_metrics = writer->metrics();
    require(writer_metrics.failed_jobs == 1 &&
        writer_metrics.encoded_jobs == static_cast<unsigned long long>(
            fast_result.saved_frames + slow_result.saved_frames + 1),
        "each accepted sample must be encoded exactly once even in both mode");

    CameraTasksSection pressure_config = config;
    pressure_config.output_dir = pathUtf8(root / "pressure_output");
    pressure_config.writer_threads = 1;
    pressure_config.writer_queue_capacity = 1;
    pressure_config.writer_queue_capacity_per_run = 1;
    FrameArtifactWriter pressure_writer(pressure_config, repository);
    require(pressure_writer.start(error), "bounded pressure writer must start: " + error);
    auto pressure_frame = std::make_shared<FrameEnvelope>();
    pressure_frame->sequence = 1;
    pressure_frame->capture_time_ms = nowMs();
    pressure_frame->publish_time = std::chrono::steady_clock::now();
    pressure_frame->image = cv::Mat(768, 1024, CV_8UC3, cv::Scalar(30, 120, 220)).clone();
    FrameArtifactJob pressure_job;
    pressure_job.task_id = "ct_pressure";
    pressure_job.run_id = "cr_pressure";
    pressure_job.output_mode = "latest";
    pressure_job.jpeg_quality = 90;
    pressure_job.frame = pressure_frame;
    int accepted_pressure_jobs = 0;
    int rejected_pressure_jobs = 0;
    for (int index = 0; index < 500; ++index) {
        if (pressure_writer.enqueue(pressure_job)) ++accepted_pressure_jobs;
        else ++rejected_pressure_jobs;
        require(pressure_writer.queueDepth("cr_pressure") <= 1,
            "per-Run writer depth must never exceed its configured bound");
    }
    require(accepted_pressure_jobs > 0 && rejected_pressure_jobs > 0,
        "writer saturation must reject extraction jobs instead of growing without bound");
    require(pressure_writer.waitForRunIdle("cr_pressure", 5000),
        "accepted pressure jobs must drain during normal operation");
    pressure_writer.stop();
    const auto latest_path = root / "output" / "ct_fast" / "latest.jpg";
    std::ifstream latest_file(latest_path, std::ios::binary);
    std::vector<unsigned char> latest_bytes(
        (std::istreambuf_iterator<char>(latest_file)), std::istreambuf_iterator<char>());
    require(!latest_bytes.empty() && !cv::imdecode(latest_bytes, cv::IMREAD_COLOR).empty(),
        "atomically published latest.jpg must be a complete JPEG");

    std::vector<CameraFrameArtifact> fast_frames;
    std::vector<CameraFrameArtifact> slow_frames;
    require(repository->listFrames(fast_task.task_id, fast_run.run_id, 100, 0, fast_frames, error),
        "fast archive metadata must be queryable");
    require(repository->listFrames(slow_task.task_id, slow_run.run_id, 100, 0, slow_frames, error),
        "slow archive metadata must be queryable");
    require(static_cast<long long>(fast_frames.size()) == fast_result.saved_frames &&
        static_cast<long long>(slow_frames.size()) == slow_result.saved_frames,
        "archive publication must persist one metadata row per saved sample");

    CameraFrameRetentionSweeper retention(config, repository);
    require(retention.start(error), "retention sweeper must start: " + error);
    int deleted = 0;
    require(retention.sweepOnce(nowMs(), deleted, error), "retention sweep must succeed: " + error);
    retention.stop();
    fast_frames.clear();
    require(repository->listFrames(fast_task.task_id, fast_run.run_id, 100, 0, fast_frames, error) &&
        fast_frames.size() == 3, "max_saved_frames retention must keep exactly the newest three archives");
    require(deleted == fast_result.saved_frames - 3,
        "retention must remove only over-limit archive artifacts");
    require(std::filesystem::exists(latest_path), "retention must never delete latest.jpg");

    const auto outside_sentinel = root / "outside_sentinel.txt";
    {
        std::ofstream sentinel(outside_sentinel, std::ios::binary);
        sentinel << "must survive";
    }
    CameraFrameArtifact traversal;
    traversal.frame_id = "cf_traversal";
    traversal.task_id = fast_task.task_id;
    traversal.run_id = fast_run.run_id;
    traversal.source_sequence = 0;
    traversal.capture_time_ms = stamp - 8LL * 24LL * 60LL * 60LL * 1000LL;
    traversal.save_time_ms = traversal.capture_time_ms;
    traversal.relative_path = "../outside_sentinel.txt";
    traversal.width = 1;
    traversal.height = 1;
    traversal.size_bytes = 12;
    require(repository->insertFrame(traversal, error),
        "malicious legacy metadata must be insertable for retention fault injection");
    error.clear();
    deleted = 0;
    require(!retention.sweepOnce(nowMs(), deleted, error) && !error.empty(),
        "retention must reject a traversal candidate and report an observable error");
    require(deleted == 0 && std::filesystem::exists(outside_sentinel),
        "retention path validation must never delete outside the managed output root");
    fast_frames.clear();
    require(repository->listFrames(fast_task.task_id, fast_run.run_id, 100, 0, fast_frames, error) &&
        fast_frames.size() == 4,
        "rejected traversal metadata must remain for operator review instead of being hidden");

    people_flow.reset();
    for (int index = 0; index < 50 && source_state->active.load(); ++index) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(!source_state->active.load(),
        "last subscription release must stop the source after idle grace");
    registry->stopAll();
    registry.reset();
    fast.reset();
    slow.reset();
    bad.reset();
    writer.reset();
    repository.reset();
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::cout << "Camera frame extraction, writer, and retention tests passed\n";
    return 0;
}
