#pragma once

#include <string>
#include <vector>

#include "server/algorithm_runtime_snapshot.h"
#include "server/redis_task_queue.h"

namespace yolo11_server {

struct WorkerRuntimeReadiness {
    int alive_vision_workers = 0;
    bool camera_role_alive = false;
    bool single_vision_worker = false;
    bool mode_consistent = false;
    bool legacy_people_flow_worker_detected = false;
    bool coordination_healthy = false;
    bool camera_task_manager_running = false;
    bool hub_registry_ready = false;
    AlgorithmRuntimeSnapshot algorithm_runtime;
};

inline WorkerRuntimeReadiness evaluateWorkerRuntimeReadiness(
    const std::vector<WorkerHeartbeatRecord>& workers,
    bool camera_tasks_enabled,
    bool unified_camera_pipeline
) {
    WorkerRuntimeReadiness result;
    if (!camera_tasks_enabled) {
        result.camera_role_alive = true;
        result.single_vision_worker = true;
        result.mode_consistent = true;
        result.coordination_healthy = true;
        result.camera_task_manager_running = true;
        result.hub_registry_ready = true;
        return result;
    }

    result.mode_consistent = true;
    const std::string expected_mode = unified_camera_pipeline
        ? "unified_camera_pipeline" : "legacy_split";
    for (const auto& worker : workers) {
        if (!worker.alive || worker.worker_kind != "vision_host") continue;
        ++result.alive_vision_workers;
        const bool mode_matches =
            worker.runtime_mode == expected_mode;
        result.mode_consistent =
            result.mode_consistent && mode_matches;
        result.legacy_people_flow_worker_detected =
            result.legacy_people_flow_worker_detected ||
            worker.legacy_people_flow_role;
        result.coordination_healthy =
            result.coordination_healthy ||
            worker.coordination_healthy;
        result.camera_task_manager_running =
            result.camera_task_manager_running ||
            worker.camera_task_manager_running;
        result.hub_registry_ready =
            result.hub_registry_ready ||
            worker.hub_registry_ready;
        const bool role_matches = unified_camera_pipeline
            ? worker.task_kind.find("camera_pipeline") !=
                std::string::npos
            : worker.task_kind.find("camera_frame") !=
                std::string::npos;
        if (mode_matches && role_matches) {
            result.camera_role_alive = true;
            if (worker.algorithm_runtime.generated_at_ms >
                result.algorithm_runtime.generated_at_ms) {
                result.algorithm_runtime = worker.algorithm_runtime;
            }
        }
    }
    result.single_vision_worker =
        result.alive_vision_workers == 1;
    return result;
}

}  // namespace yolo11_server
