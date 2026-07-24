#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "business/camera_hub_status.h"
#include "business/camera_task_runtime_control.h"
#include "business/camera_task_types.h"
#include "server/app_config.h"
#include "server/camera_task_api_control.h"
#include "server/camera_task_manager.h"

struct redisContext;

namespace yolo11_server {

class CameraTaskQueue final : public ICameraTaskCommandSource,
                              public ICameraTaskRuntimeControl,
                              public ICameraTaskApiControl {
public:
    CameraTaskQueue(
        const RedisSection& redis_config,
        const CameraTasksSection& camera_config,
        std::string consumer_name
    );
    ~CameraTaskQueue() noexcept override;

    CameraTaskQueue(const CameraTaskQueue&) = delete;
    CameraTaskQueue& operator=(const CameraTaskQueue&) = delete;

    bool start(std::string& error) override;
    bool poll(CameraTaskCommand& command, std::string& error) override;
    bool acknowledge(const std::string& message_id, std::string& error) override;
    void interrupt() noexcept override;

    bool submitStart(const CameraTaskCommand& command, std::string& error) override;
    bool acquireRunLease(
        const std::string& task_id,
        const std::string& run_id,
        std::string& error
    ) override;
    bool refreshRunLease(
        const std::string& task_id,
        const std::string& run_id,
        std::string& error
    ) override;
    bool releaseRunLease(
        const std::string& task_id,
        const std::string& run_id,
        std::string& error
    ) override;
    bool requestStop(const std::string& run_id, std::string& error) override;
    bool isStopRequested(const std::string& run_id, bool& requested, std::string& error) override;
    bool updateRunStatus(const CameraTaskRunHotStatus& status, std::string& error) override;
    bool getRunStatus(
        const std::string& run_id,
        CameraTaskRunHotStatus& status,
        std::string& error) override;
    bool getHubStatus(
        const std::string& camera_profile,
        CameraHubHotStatus& status,
        std::string& error) override;
    bool updateHubStatus(const CameraHubStatus& status, std::string& error) override;

private:
    bool connectLocked(std::string& error);
    void disconnectLocked() noexcept;
    bool ensureGroupLocked(std::string& error);
    bool claimPendingLocked(CameraTaskCommand& command, bool& claimed, std::string& error);
    bool readNewLocked(CameraTaskCommand& command, bool& received, std::string& error);
    bool fillCommand(void* entry_reply, CameraTaskCommand& command, std::string& error) const;
    std::string activeKey(const std::string& task_id) const;
    std::string stopKey(const std::string& run_id) const;
    std::string statusKey(const std::string& run_id) const;
    std::string hubStatusKey(const std::string& camera_profile) const;
    std::string leaseValue(const std::string& run_id) const;

    RedisSection redis_config_;
    CameraTasksSection camera_config_;
    std::string consumer_name_;
    std::string lease_owner_token_;
    std::string claim_cursor_ = "0-0";
    std::atomic<bool> interrupted_{ false };
    mutable std::mutex mutex_;
    redisContext* context_ = nullptr;
};

}  // namespace yolo11_server
