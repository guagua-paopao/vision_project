#include "business/camera_pipeline.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include "server/camera_task_queue.h"

namespace yolo11_server {

namespace {

long long wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void setWriteFailure(
    const std::shared_ptr<CameraPipeline::WriteState>& writes,
    std::string code,
    std::string message
) {
    {
        std::lock_guard<std::mutex> lock(writes->error_mutex);
        if (writes->error_code.empty()) {
            writes->error_code = std::move(code);
            writes->error_message = std::move(message);
        }
    }
    writes->fatal.store(true);
}

class ScopeExit final {
public:
    explicit ScopeExit(std::function<void()> callback) : callback_(std::move(callback)) {}
    ~ScopeExit() { if (callback_) callback_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
private:
    std::function<void()> callback_;
};

}  // namespace

CameraPipeline::CameraPipeline(
    CameraTaskCommand command,
    const CameraTasksSection& camera_config,
    int stale_frame_timeout_ms,
    std::string worker_consumer,
    std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry,
    std::shared_ptr<FrameArtifactWriter> writer,
    std::shared_ptr<CameraTaskRepository> repository,
    std::shared_ptr<ICameraTaskRuntimeControl> control,
    std::shared_ptr<ICameraFrameJobSink> inference_sink
) : command_(std::move(command)),
    camera_config_(camera_config),
    stale_frame_timeout_ms_(std::max(100, stale_frame_timeout_ms)),
    worker_consumer_(std::move(worker_consumer)),
    hub_registry_(std::move(hub_registry)),
    writer_(std::move(writer)),
    repository_(std::move(repository)),
    control_(std::move(control)),
    inference_sink_(std::move(inference_sink)) {
}

CameraPipeline::~CameraPipeline() noexcept {
    requestStop();
}

void CameraPipeline::run() noexcept {
    try {
        runImpl();
    }
    catch (const std::exception& exception) {
        spdlog::error("Camera extraction session failed: run_id={}, error={}",
            command_.run_id, exception.what());
        try {
            CameraTaskRunRecord run;
            bool found = false;
            std::string error;
            if (repository_ && repository_->getRun(command_.run_id, run, found, error) && found &&
                !isCameraRunTerminal(run.status)) {
                transition(run, { "queued", "starting", "running", "reconnecting", "stopping" },
                    "failed", "CAMERA_SESSION_EXCEPTION", exception.what());
            }
        }
        catch (...) {
        }
    }
    catch (...) {
        spdlog::error("Camera extraction session failed with unknown error: run_id={}", command_.run_id);
    }
}

void CameraPipeline::requestStop() noexcept {
    stop_requested_.store(true);
}

bool CameraPipeline::transition(
    CameraTaskRunRecord& run,
    const std::vector<std::string>& allowed_from,
    const std::string& next_status,
    const std::string& error_code,
    const std::string& error_message
) {
    CameraTaskRunRecord next = run;
    next.status = next_status;
    next.last_update_ms = wallNowMs();
    if (next_status == "starting" && next.start_time_ms <= 0) next.start_time_ms = next.last_update_ms;
    if (isCameraRunTerminal(next_status)) next.stop_time_ms = next.last_update_ms;
    if (next_status == "stopped") next.stop_reason = "requested";
    if (next_status == "failed") {
        next.stop_reason = "failed";
        next.error_code = error_code;
        next.error_message = error_message;
    }
    std::string code;
    std::string error;
    if (!repository_->transitionRun(run.run_id, allowed_from, next, code, error)) return false;
    run = std::move(next);
    return true;
}

void CameraPipeline::publishProgress(
    CameraTaskRunRecord& run,
    const CameraHubStatus& hub,
    const SubscriptionMetrics& subscription,
    long long sampled_frames,
    long long dropped_frames,
    long long inference_submit_drops,
    const std::shared_ptr<WriteState>& writes,
    std::chrono::steady_clock::time_point started
) noexcept {
    try {
        run.hub_instance_id = hub.hub_instance_id;
        run.capture_backend = hub.backend_name;
        run.capture_fps = hub.capture_fps;
        const double elapsed = std::max(0.001,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        run.save_fps = static_cast<double>(writes->saved_frames.load()) / elapsed;
        run.consumed_frames = subscription.consumed_frames;
        run.saved_frames = writes->saved_frames.load();
        run.skipped_frames = subscription.skipped_frames;
        run.dropped_frames = dropped_frames;
        run.last_source_sequence = subscription.last_seen_sequence;
        run.last_frame_time_ms = writes->last_frame_time_ms.load();
        run.width = writes->last_width.load();
        run.height = writes->last_height.load();
        run.last_update_ms = wallNowMs();
        std::string repository_error;
        if (!repository_->updateRunProgress(run, repository_error)) {
            setWriteFailure(writes, "CAMERA_STORAGE_UNAVAILABLE", repository_error);
        }

        CameraTaskRunHotStatus hot;
        hot.found = true;
        hot.run_id = run.run_id;
        hot.task_id = run.task_id;
        hot.status = run.status;
        hot.camera_profile = run.camera_profile;
        hot.hub_instance_id = hub.hub_instance_id;
        hot.hub_state = hub.state;
        hot.capture_backend = hub.backend_name;
        hot.last_update_ms = run.last_update_ms;
        hot.latest_frame_age_ms = hub.latest_frame_age_ms;
        hot.pipeline_thread_running = !isCameraRunTerminal(run.status);
        hot.pipeline_started_at_ms = run.start_time_ms;
        hot.sampled_frames = sampled_frames;
        hot.sample_fps = static_cast<double>(sampled_frames) / elapsed;
        hot.save_fps = run.save_fps;
        hot.consumed_frames = run.consumed_frames;
        hot.saved_frames = run.saved_frames;
        hot.skipped_frames = run.skipped_frames;
        hot.dropped_frames = run.dropped_frames;
        hot.inference_submit_drops = inference_submit_drops;
        hot.last_source_sequence = run.last_source_sequence;
        hot.last_frame_time_ms = run.last_frame_time_ms;
        hot.writer_queue_depth = static_cast<int>(writer_->queueDepth(run.run_id));
        std::string ignored;
        control_->updateRunStatus(hot, ignored);
        control_->updateHubStatus(hub, ignored);
    }
    catch (...) {
    }
}

void CameraPipeline::runImpl() {
    if (!hub_registry_ || !writer_ || !repository_ || !control_) {
        throw std::runtime_error("camera extraction dependencies are unavailable");
    }
    CameraTaskRunRecord run;
    bool found = false;
    std::string error;
    if (!repository_->getRun(command_.run_id, run, found, error) || !found) {
        throw std::runtime_error(error.empty() ? "camera run was not found" : error);
    }
    if (isCameraRunTerminal(run.status)) return;
    if (run.task_id != command_.task_id || run.camera_profile != command_.camera_profile ||
        run.definition_version != command_.definition_version) {
        transition(run, { "queued", "starting", "running", "reconnecting", "stopping" },
            "failed", "RUN_DEFINITION_MISMATCH", "command does not match the durable run definition");
        return;
    }
    if (!control_->acquireRunLease(command_.task_id, command_.run_id, error)) {
        transition(run, { "queued", "starting", "running", "reconnecting", "stopping" },
            "failed", "CAMERA_RUN_LEASE_CONFLICT", error);
        return;
    }
    ScopeExit release_lease([&]() {
        std::string ignored;
        control_->releaseRunLease(command_.task_id, command_.run_id, ignored);
    });
    ScopeExit detach_inference([&]() {
        if (inference_sink_) {
            inference_sink_->detachCamera(command_.task_id, command_.run_id);
        }
    });

    if (run.status == "queued" && !transition(run, { "queued" }, "starting")) return;
    std::shared_ptr<FrameSubscription> subscription;
    if (!hub_registry_->subscribe(command_.camera_profile,
        { "camera_task_" + command_.run_id, "camera_task" }, subscription, error)) {
        const std::string code = error == "CAMERA_HUB_CAPACITY_EXCEEDED"
            ? error : "CAMERA_HUB_SUBSCRIBE_FAILED";
        transition(run, { "starting", "running", "reconnecting" }, "failed", code, error);
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    auto next_extract_due = started;
    auto next_analysis_due = started;
    auto next_status = started;
    auto next_lease = started + std::chrono::seconds(std::max(1, camera_config_.lease_refresh_seconds));
    auto last_lease_success = started;
    const auto status_interval = std::chrono::milliseconds(250);
    const auto frame_interval = std::chrono::milliseconds(
        std::clamp(command_.frame_interval_ms, 100, 3600000));
    const auto analysis_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(
            1.0 / std::clamp(command_.target_infer_fps, 0.1, 120.0)));
    auto writes = std::make_shared<WriteState>();
    long long sampled_frames = 0;
    long long dropped_frames = 0;
    long long inference_submit_drops = 0;
    bool first_work_submitted = false;
    std::string terminal_code;
    std::string terminal_message;

    while (!stop_requested_.load()) {
        const auto now = std::chrono::steady_clock::now();
        CameraHubStatus hub = subscription->hubStatus();
        if (hub.state == "failed") {
            terminal_code = "CAMERA_HUB_FAILED";
            terminal_message = "shared camera hub failed";
            break;
        }
        if (writes->fatal.load()) {
            std::lock_guard<std::mutex> lock(writes->error_mutex);
            terminal_code = writes->error_code;
            terminal_message = writes->error_message;
            break;
        }

        const bool extraction_due = now >= next_extract_due;
        const bool analysis_due = command_.analysis_enabled && inference_sink_ &&
            now >= next_analysis_due;
        if (extraction_due || analysis_due) {
            FrameReadResult frame;
            if (subscription->tryReadLatest(frame) && frame.frame) {
                const long long age_ms = std::max(0LL, wallNowMs() - frame.frame->capture_time_ms);
                if (age_ms <= stale_frame_timeout_ms_) {
                    if (extraction_due) {
                        ++sampled_frames;
                        FrameArtifactJob job;
                        job.task_id = command_.task_id;
                        job.run_id = command_.run_id;
                        job.output_mode = command_.output_mode;
                        job.jpeg_quality = command_.jpeg_quality;
                        job.max_width = command_.max_width;
                        job.max_height = command_.max_height;
                        job.frame = frame.frame;
                        const bool accepted = writer_->enqueue(std::move(job), [writes](const auto& result) {
                            if (!result.success) {
                                setWriteFailure(writes,
                                    result.error_code.empty() ? "FRAME_ARTIFACT_WRITE_FAILED" : result.error_code,
                                    result.error_message);
                                return;
                            }
                            ++writes->saved_frames;
                            writes->last_width.store(result.width);
                            writes->last_height.store(result.height);
                            writes->last_frame_time_ms.store(result.save_time_ms);
                        });
                        if (accepted) {
                            first_work_submitted = true;
                        }
                        else {
                            ++dropped_frames;
                        }
                    }
                    if (analysis_due) {
                        CameraFrameJob analysis_job;
                        analysis_job.task_id = command_.task_id;
                        analysis_job.run_id = command_.run_id;
                        analysis_job.camera_profile = command_.camera_profile;
                        analysis_job.source_sequence = frame.frame->sequence;
                        analysis_job.capture_time_ms = frame.frame->capture_time_ms;
                        analysis_job.algorithm_profile = command_.algorithm_profile;
                        analysis_job.algorithms = command_.algorithms;
                        analysis_job.frame = frame.frame;
                        CameraFrameJobSubmitResult submit_result;
                        std::string submit_error;
                        if (inference_sink_->submitLatest(
                            std::move(analysis_job), submit_result, submit_error) &&
                            submit_result.accepted) {
                            first_work_submitted = true;
                            inference_submit_drops += submit_result.dropped_backlog;
                        }
                        else {
                            ++inference_submit_drops;
                        }
                    }
                    if (first_work_submitted && run.status == "starting") {
                        transition(run, { "starting" }, "running");
                    }
                }
            }
            if (extraction_due) {
                do { next_extract_due += frame_interval; } while (next_extract_due <= now);
            }
            if (analysis_due) {
                do { next_analysis_due += analysis_interval; } while (next_analysis_due <= now);
            }
        }

        if (hub.state == "reconnecting" || hub.state == "opening") {
            if (run.status == "running") transition(run, { "running" }, "reconnecting");
        }
        else if (hub.state == "running" && run.status == "reconnecting" && first_work_submitted) {
            transition(run, { "reconnecting" }, "running");
        }

        if (now >= next_status) {
            bool remote_stop = false;
            std::string control_error;
            if (control_->isStopRequested(command_.run_id, remote_stop, control_error) && remote_stop) {
                stop_requested_.store(true);
            }
            publishProgress(
                run, hub, subscription->metrics(), sampled_frames, dropped_frames,
                inference_submit_drops, writes, started);
            next_status = now + status_interval;
        }
        if (now >= next_lease) {
            std::string lease_error;
            if (control_->refreshRunLease(command_.task_id, command_.run_id, lease_error)) {
                last_lease_success = now;
            }
            else if (now - last_lease_success >=
                std::chrono::seconds(std::max(1, camera_config_.lease_ttl_seconds))) {
                terminal_code = "CAMERA_RUN_LEASE_LOST";
                terminal_message = lease_error.empty() ? "camera run lease expired" : lease_error;
                break;
            }
            next_lease = now + std::chrono::seconds(std::max(1, camera_config_.lease_refresh_seconds));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (terminal_code.empty() && !isCameraRunTerminal(run.status) && run.status != "stopping") {
        transition(run, { "queued", "starting", "running", "reconnecting" }, "stopping");
    }
    const CameraHubStatus final_hub = subscription->hubStatus();
    const SubscriptionMetrics final_subscription = subscription->metrics();
    subscription.reset();
    if (!writer_->waitForRunIdle(command_.run_id,
        std::max(5000, camera_config_.lease_ttl_seconds * 1000))) {
        terminal_code = "WRITER_DRAIN_TIMEOUT";
        terminal_message = "camera artifact writer did not drain before lease expiry";
    }
    if (writes->fatal.load() && terminal_code.empty()) {
        std::lock_guard<std::mutex> lock(writes->error_mutex);
        terminal_code = writes->error_code;
        terminal_message = writes->error_message;
    }
    publishProgress(
        run, final_hub, final_subscription, sampled_frames, dropped_frames,
        inference_submit_drops, writes, started);
    if (terminal_code.empty()) {
        transition(run, { "queued", "starting", "running", "reconnecting", "stopping" }, "stopped");
    }
    else {
        transition(run, { "queued", "starting", "running", "reconnecting", "stopping" },
            "failed", terminal_code, terminal_message);
    }
    CameraTaskRunHotStatus terminal_hot;
    terminal_hot.found = true;
    terminal_hot.run_id = run.run_id;
    terminal_hot.task_id = run.task_id;
    terminal_hot.status = run.status;
    terminal_hot.camera_profile = run.camera_profile;
    terminal_hot.hub_instance_id = final_hub.hub_instance_id;
    terminal_hot.hub_state = final_hub.state;
    terminal_hot.capture_backend = final_hub.backend_name;
    terminal_hot.error_code = run.error_code;
    terminal_hot.error_message = run.error_message;
    terminal_hot.last_update_ms = run.last_update_ms;
    terminal_hot.latest_frame_age_ms = final_hub.latest_frame_age_ms;
    terminal_hot.pipeline_thread_running = false;
    terminal_hot.pipeline_started_at_ms = run.start_time_ms;
    terminal_hot.sampled_frames = sampled_frames;
    const double pipeline_elapsed = std::max(0.001,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    terminal_hot.sample_fps = static_cast<double>(sampled_frames) / pipeline_elapsed;
    terminal_hot.inference_submit_drops = inference_submit_drops;
    terminal_hot.save_fps = run.save_fps;
    terminal_hot.consumed_frames = run.consumed_frames;
    terminal_hot.saved_frames = run.saved_frames;
    terminal_hot.skipped_frames = run.skipped_frames;
    terminal_hot.dropped_frames = run.dropped_frames;
    terminal_hot.last_source_sequence = run.last_source_sequence;
    terminal_hot.last_frame_time_ms = run.last_frame_time_ms;
    terminal_hot.writer_queue_depth = 0;
    std::string ignored;
    control_->updateRunStatus(terminal_hot, ignored);
    control_->updateHubStatus(final_hub, ignored);
}

}  // namespace yolo11_server
