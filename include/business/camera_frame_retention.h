#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "business/camera_task_repository.h"

namespace yolo11_server {

class CameraFrameRetentionSweeper final {
public:
    CameraFrameRetentionSweeper(
        const CameraTasksSection& config,
        std::shared_ptr<CameraTaskRepository> repository
    );
    ~CameraFrameRetentionSweeper() noexcept;

    bool start(std::string& error);
    void stop() noexcept;
    bool sweepOnce(long long now_ms, int& deleted_count, std::string& error);

private:
    void loop() noexcept;
    bool removeManagedArchive(const CameraFrameArtifact& frame, std::string& error) const;

    CameraTasksSection config_;
    std::shared_ptr<CameraTaskRepository> repository_;
    std::filesystem::path output_root_;
    std::atomic<bool> running_{ false };
    std::thread thread_;
};

}  // namespace yolo11_server
