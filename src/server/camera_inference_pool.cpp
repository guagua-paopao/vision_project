#include "server/camera_inference_pool.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

namespace yolo11_server {

CameraInferencePool::CameraInferencePool(
    AppConfig config,
    CameraModelRunnerFactory runner_factory,
    std::shared_ptr<ICameraInferenceResultHandler> result_handler
) : config_(std::move(config)),
    runner_factory_(std::move(runner_factory)),
    result_handler_(std::move(result_handler)) {
}

CameraInferencePool::~CameraInferencePool() noexcept {
    stop();
}

bool CameraInferencePool::start(std::string& error) {
    error.clear();
    if (running_.load()) return true;
    if (!config_.analysis.enabled) {
        error = "ANALYSIS_POOL_DISABLED";
        return false;
    }
    if (!runner_factory_ || !result_handler_) {
        error = "analysis pool dependencies are unavailable";
        return false;
    }
    if (!shards_.empty()) {
        error = "analysis pool has incomplete prior state";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(startup_mutex_);
        startup_reported_ = 0;
        startup_failed_ = false;
        startup_error_.clear();
    }
    workers_ready_.store(0);
    const int worker_count = std::clamp(config_.analysis.inference_workers, 1, 16);
    try {
        shards_.reserve(static_cast<std::size_t>(worker_count));
        for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
            shards_.push_back(std::make_unique<WorkerShard>(worker_id));
        }
        for (auto& shard : shards_) {
            shard->thread = std::thread([this, state = shard.get()]() {
                workerLoop(*state);
            });
        }
    }
    catch (const std::exception& exception) {
        error = std::string("failed to create inference worker: ") + exception.what();
        stopWorkersNoexcept();
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(startup_mutex_);
        const bool reported = startup_cv_.wait_for(
            lock,
            std::chrono::milliseconds(config_.analysis.model_init_timeout_ms),
            [&]() {
                return startup_failed_ ||
                    startup_reported_ == static_cast<int>(shards_.size());
            });
        if (!reported) {
            startup_failed_ = true;
            startup_error_ = "inference model initialization timed out";
        }
        if (startup_failed_) error = startup_error_;
    }
    if (!error.empty()) {
        stopWorkersNoexcept();
        return false;
    }

    running_.store(true);
    return true;
}

void CameraInferencePool::stop() noexcept {
    running_.store(false);
    if (shards_.empty()) return;

    std::vector<std::pair<std::string, std::string>> active_runs;
    try {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard->mutex);
            shard->stop_requested = true;
            for (const auto& entry : shard->active) {
                active_runs.emplace_back(entry.first, entry.second.run_id);
                ++shard->generation_counters[entry.first];
            }
            shard->active.clear();
            shard->pending.clear();
            shard->ready_cameras.clear();
            shard->queued_cameras.clear();
            shard->cv.notify_all();
        }
        for (const auto& active : active_runs) {
            result_handler_->detachCamera(active.first, active.second);
        }
    }
    catch (...) {
    }
    stopWorkersNoexcept();
}

bool CameraInferencePool::running() const {
    return running_.load();
}

std::size_t CameraInferencePool::shardIndex(const std::string& task_id) const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char value : task_id) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return shards_.empty() ? 0 : static_cast<std::size_t>(hash % shards_.size());
}

bool CameraInferencePool::submitLatest(
    CameraFrameJob job,
    CameraFrameJobSubmitResult& result,
    std::string& error
) {
    result = {};
    error.clear();
    if (!running_.load() || shards_.empty()) {
        error = "ANALYSIS_POOL_NOT_RUNNING";
        return false;
    }
    if (job.task_id.empty() || job.run_id.empty() || job.algorithm_profile.empty() ||
        job.algorithms.empty() || !job.frame || job.frame->image.empty() ||
        job.source_sequence == 0) {
        error = "INVALID_INFERENCE_JOB";
        return false;
    }

    auto& shard = *shards_[shardIndex(job.task_id)];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (shard.stop_requested || !running_.load()) {
            error = "ANALYSIS_POOL_STOPPING";
            return false;
        }

        auto active = shard.active.find(job.task_id);
        if (active == shard.active.end() || active->second.run_id != job.run_id) {
            const std::uint64_t generation = ++shard.generation_counters[job.task_id];
            active = shard.active.insert_or_assign(
                job.task_id,
                ActiveGeneration{ job.run_id, generation, 0 }).first;
        }
        if (job.source_sequence <= active->second.last_sequence) {
            error = "INFERENCE_SEQUENCE_NOT_MONOTONIC";
            return false;
        }
        active->second.last_sequence = job.source_sequence;

        const auto pending = shard.pending.find(job.task_id);
        if (pending != shard.pending.end()) {
            pending->second = QueuedJob{ std::move(job), active->second.generation };
            result.dropped_backlog = 1;
            ++replaced_jobs_;
        }
        else {
            const std::string camera_id = job.task_id;
            shard.pending.emplace(
                camera_id,
                QueuedJob{ std::move(job), active->second.generation });
            if (shard.queued_cameras.insert(camera_id).second) {
                shard.ready_cameras.push_back(camera_id);
            }
        }
        ++submitted_jobs_;
        result.accepted = true;
    }
    shard.cv.notify_one();
    return true;
}

void CameraInferencePool::detachCamera(
    const std::string& task_id,
    const std::string& run_id
) noexcept {
    if (task_id.empty() || run_id.empty() || shards_.empty()) return;
    try {
        auto& shard = *shards_[shardIndex(task_id)];
        bool detached = false;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            const auto active = shard.active.find(task_id);
            if (active != shard.active.end() && active->second.run_id == run_id) {
                ++shard.generation_counters[task_id];
                shard.active.erase(active);
                const auto pending = shard.pending.find(task_id);
                if (pending != shard.pending.end() && pending->second.job.run_id == run_id) {
                    shard.pending.erase(pending);
                }
                detached = true;
            }
        }
        if (detached) result_handler_->detachCamera(task_id, run_id);
    }
    catch (...) {
    }
}

CameraInferencePoolSnapshot CameraInferencePool::snapshot() const {
    CameraInferencePoolSnapshot result;
    result.running = running_.load();
    result.workers_configured = config_.analysis.inference_workers;
    result.workers_ready = workers_ready_.load();
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard->mutex);
        result.active_cameras += shard->active.size();
        result.pending_cameras += shard->pending.size();
    }
    result.submitted_jobs = submitted_jobs_.load();
    result.replaced_jobs = replaced_jobs_.load();
    result.processed_jobs = processed_jobs_.load();
    result.failed_jobs = failed_jobs_.load();
    result.stale_results = stale_results_.load();
    return result;
}

void CameraInferencePool::reportStartup(bool success, const std::string& error) {
    std::lock_guard<std::mutex> lock(startup_mutex_);
    ++startup_reported_;
    if (!success && !startup_failed_) {
        startup_failed_ = true;
        startup_error_ = error.empty() ? "inference model initialization failed" : error;
    }
    startup_cv_.notify_all();
}

void CameraInferencePool::workerLoop(WorkerShard& shard) noexcept {
    std::unique_ptr<IModelRunner> runner;
    try {
        runner = runner_factory_(shard.worker_id);
        if (!runner) {
            reportStartup(false, "model runner factory returned null");
            return;
        }
        std::string init_error;
        if (!runner->init(config_, init_error)) {
            reportStartup(false, init_error);
            runner->release();
            return;
        }
        ++workers_ready_;
        reportStartup(true, {});

        while (true) {
            QueuedJob queued;
            bool found = false;
            {
                std::unique_lock<std::mutex> lock(shard.mutex);
                shard.cv.wait(lock, [&]() {
                    return shard.stop_requested || !shard.ready_cameras.empty();
                });
                if (shard.stop_requested) break;
                while (!shard.ready_cameras.empty() && !found) {
                    const std::string task_id = std::move(shard.ready_cameras.front());
                    shard.ready_cameras.pop_front();
                    shard.queued_cameras.erase(task_id);
                    const auto pending = shard.pending.find(task_id);
                    if (pending == shard.pending.end()) continue;
                    queued = std::move(pending->second);
                    shard.pending.erase(pending);
                    found = true;
                }
            }
            if (!found) continue;

            CameraInferenceResult result;
            result.worker_id = shard.worker_id;
            result.job = std::move(queued.job);
            try {
                const auto started = std::chrono::steady_clock::now();
                result.output = runner->infer(result.job.frame->image);
                result.inference_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                ++processed_jobs_;

                bool current = false;
                {
                    std::lock_guard<std::mutex> lock(shard.mutex);
                    const auto active = shard.active.find(result.job.task_id);
                    current = active != shard.active.end() &&
                        active->second.run_id == result.job.run_id &&
                        active->second.generation == queued.generation;
                }
                if (!current) {
                    ++stale_results_;
                    continue;
                }

                std::string handler_error;
                if (!result_handler_->handle(result, handler_error)) {
                    ++failed_jobs_;
                    spdlog::warn(
                        "Camera inference result handling failed: camera_id={}, run_id={}, error={}",
                        result.job.task_id, result.job.run_id, handler_error);
                }
            }
            catch (const std::exception& exception) {
                ++failed_jobs_;
                spdlog::error(
                    "Camera inference failed: worker={}, camera_id={}, run_id={}, error={}",
                    shard.worker_id, result.job.task_id, result.job.run_id, exception.what());
            }
            catch (...) {
                ++failed_jobs_;
                spdlog::error(
                    "Camera inference failed: worker={}, camera_id={}, run_id={}, unknown error",
                    shard.worker_id, result.job.task_id, result.job.run_id);
            }
        }
    }
    catch (const std::exception& exception) {
        reportStartup(false, exception.what());
    }
    catch (...) {
        reportStartup(false, "unknown inference worker startup exception");
    }
    if (runner) runner->release();
    if (workers_ready_.load() > 0) --workers_ready_;
}

void CameraInferencePool::stopWorkersNoexcept() noexcept {
    try {
        for (auto& shard : shards_) {
            {
                std::lock_guard<std::mutex> lock(shard->mutex);
                shard->stop_requested = true;
            }
            shard->cv.notify_all();
        }
        for (auto& shard : shards_) {
            if (shard->thread.joinable()) shard->thread.join();
        }
    }
    catch (...) {
    }
    shards_.clear();
    workers_ready_.store(0);
}

}  // namespace yolo11_server
