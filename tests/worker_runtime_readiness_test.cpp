#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "server/worker_runtime_readiness.h"

namespace {

using namespace yolo11_server;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

WorkerHeartbeatRecord unifiedWorker() {
    WorkerHeartbeatRecord worker;
    worker.alive = true;
    worker.worker_kind = "vision_host";
    worker.task_kind = "camera_pipeline";
    worker.runtime_mode = "unified_camera_pipeline";
    worker.worker_generation = "200:1234";
    worker.camera_task_manager_running = true;
    worker.hub_registry_ready = true;
    worker.coordination_healthy = true;
    worker.algorithm_runtime.generated_at_ms = 1235;
    worker.algorithm_runtime.host_running = true;
    return worker;
}

}  // namespace

int main() {
    const auto healthy = evaluateWorkerRuntimeReadiness(
        { unifiedWorker() }, true, true);
    require(healthy.camera_role_alive &&
            healthy.single_vision_worker &&
            healthy.mode_consistent &&
            !healthy.legacy_people_flow_worker_detected &&
            healthy.coordination_healthy &&
            healthy.camera_task_manager_running &&
            healthy.hub_registry_ready &&
            healthy.algorithm_runtime.host_running,
        "one fenced Camera-only Worker must satisfy unified readiness");

    auto legacy = unifiedWorker();
    legacy.task_kind = "live_people_flow,camera_frame";
    legacy.runtime_mode = "legacy_split";
    legacy.legacy_people_flow_role = true;
    const auto mismatch = evaluateWorkerRuntimeReadiness(
        { legacy }, true, true);
    require(!mismatch.camera_role_alive &&
            mismatch.single_vision_worker &&
            !mismatch.mode_consistent &&
            mismatch.legacy_people_flow_worker_detected,
        "unified /ready must reject an old People Flow Worker");

    auto duplicate = unifiedWorker();
    duplicate.worker_generation = "201:1236";
    const auto double_consumer = evaluateWorkerRuntimeReadiness(
        { unifiedWorker(), duplicate }, true, true);
    require(double_consumer.camera_role_alive &&
            !double_consumer.single_vision_worker,
        "/ready must reject two live Vision Worker generations");

    auto unfenced = unifiedWorker();
    unfenced.coordination_healthy = false;
    const auto lease_lost = evaluateWorkerRuntimeReadiness(
        { unfenced }, true, true);
    require(!lease_lost.coordination_healthy,
        "/ready must reject a Worker that lost process ownership");

    std::cout << "Worker runtime readiness tests passed\n";
    return 0;
}
