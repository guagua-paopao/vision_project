#include "server/people_flow_session_runner.h"

#include <chrono>
#include <utility>

namespace yolo11_server {

PeopleFlowSessionRunner::PeopleFlowSessionRunner(
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
) : worker_id_(worker_id),
    config_(config),
    redis_queue_(redis_queue),
    repository_(repository),
    runner_(model_runner),
    hub_registry_(std::move(hub_registry)),
    running_(host_running),
    processed_count_(processed_count),
    failed_count_(failed_count),
    state_callback_(std::move(state_callback)),
    heartbeat_callback_(std::move(heartbeat_callback)) {
}

void PeopleFlowSessionRunner::setWorkerState(
    const std::string& status,
    const std::string& session_id,
    const std::string& error
) {
    if (state_callback_) state_callback_(status, session_id, error);
}

void PeopleFlowSessionRunner::writeHeartbeatNoexcept() noexcept {
    try {
        if (heartbeat_callback_) heartbeat_callback_();
    }
    catch (...) {
    }
}

long long PeopleFlowSessionRunner::nowMs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

}  // namespace yolo11_server
