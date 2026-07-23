#pragma once

#include <functional>
#include <string>

namespace yolo11_server {

// Process-local algorithm-service metrics flattened for Redis heartbeat
// transport. It intentionally contains counters and stable error codes only;
// endpoints, credentials, request bodies, and model paths are excluded.
struct AlgorithmRuntimeSnapshot {
    long long generated_at_ms = 0;
    bool host_running = false;
    long long active_pipelines = 0;

    bool inference_configured = false;
    bool inference_running = false;
    int inference_workers_configured = 0;
    int inference_workers_ready = 0;
    long long inference_active_cameras = 0;
    long long inference_pending_cameras = 0;
    long long inference_submitted_jobs = 0;
    long long inference_replaced_jobs = 0;
    long long inference_processed_jobs = 0;
    long long inference_failed_jobs = 0;
    long long inference_stale_results = 0;

    bool processor_running = false;
    long long processor_active_sessions = 0;
    long long processor_processed_frames = 0;
    long long processor_persisted_alerts = 0;
    long long processor_duplicate_alerts = 0;
    long long processor_failed_frames = 0;

    bool callbacks_configured = false;
    bool callback_running = false;
    int callback_profiles_ready = 0;
    long long callback_claimed = 0;
    long long callback_delivered = 0;
    long long callback_retries = 0;
    long long callback_dead_letters = 0;
    long long callback_transport_failures = 0;
    long long callback_lease_conflicts = 0;
    long long callback_last_success_at_ms = 0;
    std::string callback_last_error_code;
};

using AlgorithmRuntimeSnapshotProvider =
    std::function<AlgorithmRuntimeSnapshot()>;
using AlgorithmRuntimeSnapshotReader =
    std::function<bool(AlgorithmRuntimeSnapshot&, std::string&)>;

}  // namespace yolo11_server
