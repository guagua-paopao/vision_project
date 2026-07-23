#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "server/camera_inference_pool.h"

namespace {

using namespace yolo11_server;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

struct RunnerState {
    std::atomic<int> created{ 0 };
    std::atomic<int> initialized{ 0 };
    std::atomic<int> released{ 0 };
    std::atomic<int> inferred{ 0 };
    std::mutex mutex;
    std::condition_variable cv;
    bool block_next = false;
    bool blocked = false;
    bool release_block = false;
};

class FakeRunner final : public IModelRunner {
public:
    explicit FakeRunner(std::shared_ptr<RunnerState> state, bool fail_init = false)
        : state_(std::move(state)), fail_init_(fail_init) {
        ++state_->created;
    }

    std::string modelType() const override { return "fake"; }
    bool init(const AppConfig&, std::string& error) override {
        ++state_->initialized;
        if (fail_init_) {
            error = "synthetic model initialization failure";
            return false;
        }
        error.clear();
        return true;
    }
    ModelOutput infer(const cv::Mat&) override {
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            if (state_->block_next) {
                state_->block_next = false;
                state_->blocked = true;
                state_->release_block = false;
                state_->cv.notify_all();
                state_->cv.wait(lock, [&]() { return state_->release_block; });
                state_->blocked = false;
            }
        }
        ++state_->inferred;
        ModelOutput output;
        output.model_type = "fake";
        return output;
    }
    cv::Mat draw(const cv::Mat& image, const ModelOutput&) override {
        return image.clone();
    }
    void release() noexcept override { ++state_->released; }

private:
    std::shared_ptr<RunnerState> state_;
    bool fail_init_ = false;
};

class RecordingHandler final : public ICameraInferenceResultHandler {
public:
    bool handle(const CameraInferenceResult& result, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sequences_[result.job.task_id].push_back(result.job.source_sequence);
        workers_[result.job.task_id].push_back(result.worker_id);
        error.clear();
        return true;
    }
    void detachCamera(const std::string& task_id, const std::string& run_id) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        detached_.push_back(task_id + ":" + run_id);
    }
    std::vector<unsigned long long> sequences(const std::string& task_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sequences_.find(task_id);
        return found == sequences_.end()
            ? std::vector<unsigned long long>{} : found->second;
    }
    std::vector<int> workers(const std::string& task_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = workers_.find(task_id);
        return found == workers_.end() ? std::vector<int>{} : found->second;
    }
    bool detached(const std::string& task_id, const std::string& run_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::find(
            detached_.begin(), detached_.end(), task_id + ":" + run_id) != detached_.end();
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<unsigned long long>> sequences_;
    std::map<std::string, std::vector<int>> workers_;
    std::vector<std::string> detached_;
};

CameraFrameJob job(
    const std::string& task_id,
    const std::string& run_id,
    unsigned long long sequence
) {
    auto frame = std::make_shared<FrameEnvelope>();
    frame->image = cv::Mat(8, 8, CV_8UC3, cv::Scalar(1, 2, 3));
    frame->sequence = sequence;
    frame->capture_time_ms = 1000 + static_cast<long long>(sequence);
    CameraFrameJob value;
    value.task_id = task_id;
    value.run_id = run_id;
    value.camera_profile = "entry_camera_01";
    value.source_sequence = sequence;
    value.capture_time_ms = frame->capture_time_ms;
    value.algorithm_profile = "test_profile";
    value.algorithms = { "people_flow" };
    value.frame = std::move(frame);
    return value;
}

void setBlockNext(const std::shared_ptr<RunnerState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->block_next = true;
    state->blocked = false;
    state->release_block = false;
}

void releaseBlocked(const std::shared_ptr<RunnerState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->release_block = true;
    state->cv.notify_all();
}

}  // namespace

int main() {
    AppConfig config;
    config.analysis.enabled = true;
    config.analysis.inference_workers = 2;
    config.analysis.model_init_timeout_ms = 3000;
    auto runner_state = std::make_shared<RunnerState>();
    auto handler = std::make_shared<RecordingHandler>();
    CameraInferencePool pool(
        config,
        [runner_state](int) {
            return std::make_unique<FakeRunner>(runner_state);
        },
        handler);

    std::string error;
    require(pool.start(error), "fixed inference pool must start: " + error);
    auto started = pool.snapshot();
    require(started.running && started.workers_configured == 2 &&
            started.workers_ready == 2 &&
            runner_state->created.load() == 2 &&
            runner_state->initialized.load() == 2,
        "startup must create exactly W ready model runners");

    setBlockNext(runner_state);
    CameraFrameJobSubmitResult submit;
    require(pool.submitLatest(job("camera_a", "run_a", 1), submit, error) && submit.accepted,
        "first camera job must be accepted");
    require(waitUntil([&]() {
        std::lock_guard<std::mutex> lock(runner_state->mutex);
        return runner_state->blocked;
    }), "first camera job must become in-flight");
    require(pool.submitLatest(job("camera_a", "run_a", 2), submit, error) &&
            submit.accepted && submit.dropped_backlog == 0,
        "one pending latest job must be accepted behind an in-flight job");
    require(pool.submitLatest(job("camera_a", "run_a", 3), submit, error) &&
            submit.accepted && submit.dropped_backlog == 1,
        "newest camera job must replace the one pending backlog slot");
    releaseBlocked(runner_state);
    require(waitUntil([&]() { return handler->sequences("camera_a").size() == 2; }),
        "camera A must deliver its in-flight and newest pending results");
    require(handler->sequences("camera_a") ==
            std::vector<unsigned long long>({ 1, 3 }),
        "latest-only scheduling must discard the superseded sequence");
    const auto camera_a_workers = handler->workers("camera_a");
    require(camera_a_workers.size() == 2 &&
            camera_a_workers.front() == camera_a_workers.back(),
        "one camera must retain stable single-worker affinity");

    require(!pool.submitLatest(job("camera_a", "run_a", 3), submit, error) &&
            error == "INFERENCE_SEQUENCE_NOT_MONOTONIC",
        "duplicate or regressing source sequences must be rejected");

    setBlockNext(runner_state);
    require(pool.submitLatest(job("camera_stale", "run_stale", 1), submit, error),
        "stale-result test job must be accepted");
    require(waitUntil([&]() {
        std::lock_guard<std::mutex> lock(runner_state->mutex);
        return runner_state->blocked;
    }), "stale-result test job must become in-flight");
    pool.detachCamera("camera_stale", "run_stale");
    releaseBlocked(runner_state);
    require(waitUntil([&]() { return pool.snapshot().stale_results == 1; }),
        "detached generation must suppress its in-flight result");
    require(handler->sequences("camera_stale").empty() &&
            handler->detached("camera_stale", "run_stale"),
        "detached camera must not reach result processing");

    require(runner_state->created.load() == 2,
        "workload changes must never create dynamic model runners");
    const auto live = pool.snapshot();
    require(live.submitted_jobs == 4 && live.replaced_jobs == 1 &&
            live.processed_jobs == 3 && live.failed_jobs == 0,
        "pool metrics must expose bounded scheduling and processing");

    pool.stop();
    require(!pool.running() && runner_state->released.load() == 2,
        "shutdown must join workers and release exactly W runners");

    auto failed_runner_state = std::make_shared<RunnerState>();
    auto failed_handler = std::make_shared<RecordingHandler>();
    CameraInferencePool failed_pool(
        config,
        [failed_runner_state](int worker_id) {
            return std::make_unique<FakeRunner>(
                failed_runner_state, worker_id == 1);
        },
        failed_handler);
    require(!failed_pool.start(error) &&
            error == "synthetic model initialization failure" &&
            !failed_pool.running(),
        "one failed model initialization must fail the entire fixed pool closed");
    require(failed_runner_state->created.load() == 2 &&
            failed_runner_state->initialized.load() == 2 &&
            failed_runner_state->released.load() == 2,
        "failed startup must release every model runner without partial service");

    std::cout << "Fixed camera inference pool tests passed\n";
    return 0;
}
