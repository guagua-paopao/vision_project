#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "business/people_flow_repository.h"
#include "server/app_config.h"
#include "server/model_runner.h"
#include "server/redis_task_queue.h"

namespace yolo11_server {

class SharedCameraFrameHubRegistry;

// Owns one long-running People Flow session. The command consumer creates at
// most one Runner thread, while the Runner owns the session's Hub subscription
// and all existing tracking/counting/persistence behavior.
class PeopleFlowSessionRunner final {
public:
    using StateCallback = std::function<void(
        const std::string& status,
        const std::string& session_id,
        const std::string& error)>;
    using HeartbeatCallback = std::function<void()>;

    PeopleFlowSessionRunner(
        int worker_id,
        const AppConfig& config,
        RedisTaskQueue& redis_queue,
        PeopleFlowRepository* repository,
        IModelRunner* model_runner,
        std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry,
        std::atomic<bool>& host_running,
        std::atomic<long long>& processed_count,
        std::atomic<long long>& failed_count,
        StateCallback state_callback,
        HeartbeatCallback heartbeat_callback
    );

    void run(const RedisTask& task);

private:
    void setWorkerState(
        const std::string& status,
        const std::string& session_id,
        const std::string& error
    );
    void writeHeartbeatNoexcept() noexcept;
    static long long nowMs();

    int worker_id_ = 0;
    AppConfig config_;
    RedisTaskQueue& redis_queue_;
    PeopleFlowRepository* repository_ = nullptr;
    IModelRunner* runner_ = nullptr;
    std::shared_ptr<SharedCameraFrameHubRegistry> hub_registry_;
    std::atomic<bool>& running_;
    std::atomic<long long>& processed_count_;
    std::atomic<long long>& failed_count_;
    StateCallback state_callback_;
    HeartbeatCallback heartbeat_callback_;
};

}  // namespace yolo11_server
