#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "business/camera_frame_types.h"
#include "business/camera_task_repository.h"

namespace yolo11_server {

struct FrameArtifactJob {
    std::string task_id;
    std::string run_id;
    std::string output_mode = "latest";
    int jpeg_quality = 90;
    int max_width = 0;
    int max_height = 0;
    SharedCameraFrame frame;
};

struct FrameArtifactWriteResult {
    bool success = false;
    bool latest_published = false;
    bool archive_published = false;
    std::string error_code;
    std::string error_message;
    int width = 0;
    int height = 0;
    long long save_time_ms = 0;
    std::optional<CameraFrameArtifact> artifact;
};

using FrameArtifactCompletion = std::function<void(const FrameArtifactWriteResult& result)>;

struct FrameArtifactWriterMetrics {
    std::size_t queue_depth = 0;
    unsigned long long encoded_jobs = 0;
    unsigned long long completed_jobs = 0;
    unsigned long long failed_jobs = 0;
    unsigned long long orphaned_artifacts = 0;
    unsigned long long storage_pressure_rejections = 0;
};

class FrameArtifactWriter final {
public:
    FrameArtifactWriter(
        const CameraTasksSection& config,
        std::shared_ptr<CameraTaskRepository> repository
    );
    ~FrameArtifactWriter() noexcept;

    FrameArtifactWriter(const FrameArtifactWriter&) = delete;
    FrameArtifactWriter& operator=(const FrameArtifactWriter&) = delete;

    bool start(std::string& error);
    void stop() noexcept;
    bool enqueue(FrameArtifactJob job, FrameArtifactCompletion completion = {});
    bool waitForRunIdle(const std::string& run_id, int timeout_ms);
    std::size_t queueDepth(const std::string& run_id = {}) const;
    FrameArtifactWriterMetrics metrics() const;
    std::filesystem::path outputRoot() const;

private:
    struct PendingJob {
        FrameArtifactJob job;
        FrameArtifactCompletion completion;
    };

    void workerLoop() noexcept;
    FrameArtifactWriteResult write(const FrameArtifactJob& job) noexcept;
    bool archiveAdmissionAllowed(std::size_t encoded_bytes, std::string& error) const;
    void recordOrphan(const CameraFrameArtifact& artifact, const std::string& reason) noexcept;

    CameraTasksSection config_;
    std::shared_ptr<CameraTaskRepository> repository_;
    std::filesystem::path output_root_;
    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;
    std::deque<PendingJob> queue_;
    std::map<std::string, std::size_t> in_flight_per_run_;
    std::vector<std::thread> workers_;
    bool accepting_ = false;
    bool stopping_ = false;
    unsigned long long encoded_jobs_ = 0;
    unsigned long long completed_jobs_ = 0;
    unsigned long long failed_jobs_ = 0;
    unsigned long long orphaned_artifacts_ = 0;
    unsigned long long storage_pressure_rejections_ = 0;
    mutable std::mutex orphan_mutex_;
    mutable std::mutex storage_guard_mutex_;
};

}  // namespace yolo11_server
