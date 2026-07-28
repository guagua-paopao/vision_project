#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "business/camera_pipeline.h"
#include "server/app_config.h"
#include "server/model_output.h"
#include "server/model_runner.h"

namespace yolo11_server {

struct CameraInferenceResult {
    int worker_id = 0;
    CameraFrameJob job;
    ModelOutput output;
    double inference_ms = 0.0;
};

class ICameraInferenceResultHandler {
public:
    virtual ~ICameraInferenceResultHandler() = default;
    virtual bool handle(const CameraInferenceResult& result, std::string& error) = 0;
    virtual void detachCamera(
        const std::string& task_id,
        const std::string& run_id) noexcept = 0;
};

struct CameraInferencePoolSnapshot {
    bool running = false;
    int workers_configured = 0;
    int workers_ready = 0;
    std::size_t active_cameras = 0;
    std::size_t pending_cameras = 0;
    long long submitted_jobs = 0;
    long long replaced_jobs = 0;
    long long processed_jobs = 0;
    long long failed_jobs = 0;
    long long stale_results = 0;
};

using CameraModelRunnerFactory =
    std::function<std::unique_ptr<IModelRunner>(int worker_id)>;

// Startup-fixed, load-balanced inference pool. Each worker owns one model
// runner. A camera is assigned to the least-loaded shard on first submission
// and retains that affinity until detach. Each camera has at most one pending
// latest-only job in addition to an in-flight inference.
class CameraInferencePool final : public ICameraFrameJobSink {
public:
    CameraInferencePool(
        AppConfig config,
        CameraModelRunnerFactory runner_factory,
        std::shared_ptr<ICameraInferenceResultHandler> result_handler
    );
    ~CameraInferencePool() noexcept override;

    CameraInferencePool(const CameraInferencePool&) = delete;
    CameraInferencePool& operator=(const CameraInferencePool&) = delete;

    bool start(std::string& error);
    void stop() noexcept;
    bool running() const;

    bool submitLatest(
        CameraFrameJob job,
        CameraFrameJobSubmitResult& result,
        std::string& error) override;
    void detachCamera(
        const std::string& task_id,
        const std::string& run_id) noexcept override;

    CameraInferencePoolSnapshot snapshot() const;

private:
    struct ActiveGeneration {
        std::string run_id;
        std::uint64_t generation = 0;
        unsigned long long last_sequence = 0;
    };
    struct QueuedJob {
        CameraFrameJob job;
        std::uint64_t generation = 0;
    };
    struct WorkerShard {
        explicit WorkerShard(int value) : worker_id(value) {}
        int worker_id = 0;
        mutable std::mutex mutex;
        std::condition_variable cv;
        bool stop_requested = false;
        std::deque<std::string> ready_cameras;
        std::set<std::string> queued_cameras;
        std::map<std::string, QueuedJob> pending;
        std::map<std::string, ActiveGeneration> active;
        std::map<std::string, std::uint64_t> generation_counters;
        std::thread thread;
    };

    std::size_t assignShard(const std::string& task_id);
    bool findAssignedShard(
        const std::string& task_id,
        std::size_t& shard_index) const noexcept;
    void releaseShard(
        const std::string& task_id,
        std::size_t shard_index) noexcept;
    void clearShardAssignments() noexcept;
    void workerLoop(WorkerShard& shard) noexcept;
    void reportStartup(bool success, const std::string& error);
    void stopWorkersNoexcept() noexcept;

    AppConfig config_;
    CameraModelRunnerFactory runner_factory_;
    std::shared_ptr<ICameraInferenceResultHandler> result_handler_;
    std::vector<std::unique_ptr<WorkerShard>> shards_;
    std::atomic<bool> running_{ false };
    std::atomic<int> workers_ready_{ 0 };
    std::atomic<long long> submitted_jobs_{ 0 };
    std::atomic<long long> replaced_jobs_{ 0 };
    std::atomic<long long> processed_jobs_{ 0 };
    std::atomic<long long> failed_jobs_{ 0 };
    std::atomic<long long> stale_results_{ 0 };
    mutable std::mutex assignment_mutex_;
    std::map<std::string, std::size_t> shard_assignments_;
    std::vector<std::size_t> shard_assignment_loads_;
    std::size_t next_assignment_shard_ = 0;
    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    int startup_reported_ = 0;
    bool startup_failed_ = false;
    std::string startup_error_;
};

}  // namespace yolo11_server
