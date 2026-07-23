#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "business/camera_task_repository.h"
#include "server/app_config.h"
#include "server/camera_inference_pool.h"

namespace yolo11_server {

struct CameraAlgorithmProcessorSnapshot {
    std::size_t active_sessions = 0;
    long long processed_frames = 0;
    long long persisted_alerts = 0;
    long long duplicate_alerts = 0;
    long long failed_frames = 0;
};

// Stateful per-camera analytics behind the fixed inference pool. Sessions are
// keyed by stable camera id and replaced only when run_id changes.
class CameraAlgorithmProcessor final : public ICameraInferenceResultHandler {
public:
    CameraAlgorithmProcessor(
        AppConfig config,
        std::shared_ptr<CameraTaskRepository> repository
    );
    ~CameraAlgorithmProcessor() noexcept override;

    bool start(std::string& error);
    void stop() noexcept;

    bool handle(const CameraInferenceResult& result, std::string& error) override;
    void detachCamera(
        const std::string& task_id,
        const std::string& run_id) noexcept override;

    CameraAlgorithmProcessorSnapshot snapshot() const;

private:
    struct Session;
    std::shared_ptr<Session> sessionFor(
        const CameraFrameJob& job,
        std::string& error
    );

    AppConfig config_;
    std::shared_ptr<CameraTaskRepository> repository_;
    mutable std::mutex sessions_mutex_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    std::atomic<bool> running_{ false };
    std::atomic<long long> processed_frames_{ 0 };
    std::atomic<long long> persisted_alerts_{ 0 };
    std::atomic<long long> duplicate_alerts_{ 0 };
    std::atomic<long long> failed_frames_{ 0 };
};

}  // namespace yolo11_server
