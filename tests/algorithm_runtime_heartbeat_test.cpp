#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

#include "server/redis_task_queue.h"

namespace {

using namespace yolo11_server;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int envInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    try { return std::stoi(value); }
    catch (...) { return fallback; }
}

}  // namespace

int main() {
    const char* host = std::getenv("YOLO11_TEST_REDIS_HOST");
    const char* port = std::getenv("YOLO11_TEST_REDIS_PORT");
    if (!host || !*host || !port || !*port) {
        std::cout << "SKIP: YOLO11_TEST_REDIS_HOST/PORT are not configured\n";
        return 77;
    }

#ifdef _WIN32
    WSADATA winsock{};
    require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0,
        "Winsock must initialize");
#endif

    RedisSection config;
    config.host = host;
    config.port = envInt("YOLO11_TEST_REDIS_PORT", 6379);
    config.db = envInt("YOLO11_TEST_REDIS_DB", 0);
    if (const char* password = std::getenv("YOLO11_TEST_REDIS_PASSWORD")) {
        config.password = password;
    }
    config.consumer_name = "p5_runtime_test_1";

    RedisTaskQueue queue(config);
    std::string error;
    require(queue.connect(error), "test Redis must connect: " + error);

    WorkerHeartbeatRecord heartbeat;
    heartbeat.consumer_name = config.consumer_name;
    heartbeat.pid = "123";
    heartbeat.host = "loopback";
    heartbeat.worker_id = 1;
    heartbeat.worker_kind = "vision_host";
    heartbeat.task_kind = "live_people_flow,camera_frame";
    heartbeat.status = "idle";
    heartbeat.last_heartbeat_ms = 1784800000000LL;
    auto& runtime = heartbeat.algorithm_runtime;
    runtime.generated_at_ms = 1784800000001LL;
    runtime.host_running = true;
    runtime.active_pipelines = 3;
    runtime.inference_configured = true;
    runtime.inference_running = true;
    runtime.inference_workers_configured = 2;
    runtime.inference_workers_ready = 2;
    runtime.inference_active_cameras = 3;
    runtime.inference_pending_cameras = 1;
    runtime.inference_submitted_jobs = 101;
    runtime.inference_replaced_jobs = 4;
    runtime.inference_processed_jobs = 96;
    runtime.inference_failed_jobs = 1;
    runtime.inference_stale_results = 2;
    runtime.processor_running = true;
    runtime.processor_active_sessions = 3;
    runtime.processor_processed_frames = 96;
    runtime.processor_persisted_alerts = 7;
    runtime.processor_duplicate_alerts = 2;
    runtime.processor_failed_frames = 1;
    runtime.callbacks_configured = true;
    runtime.callback_running = true;
    runtime.callback_profiles_ready = 1;
    runtime.callback_claimed = 9;
    runtime.callback_delivered = 6;
    runtime.callback_retries = 2;
    runtime.callback_dead_letters = 1;
    runtime.callback_transport_failures = 2;
    runtime.callback_lease_conflicts = 1;
    runtime.callback_last_success_at_ms = 1784800000002LL;
    runtime.callback_last_error_code = "CALLBACK_HTTP_500";

    require(queue.writeWorkerHeartbeat(heartbeat, 10, error),
        "algorithm runtime heartbeat must write: " + error);
    std::vector<WorkerHeartbeatRecord> records;
    require(queue.getWorkerHeartbeats(
            "p5_runtime_test_",
            1,
            records,
            error) &&
            records.size() == 1 &&
            records.front().found,
        "algorithm runtime heartbeat must read: " + error);
    const auto& loaded = records.front().algorithm_runtime;
    require(loaded.generated_at_ms == runtime.generated_at_ms &&
            loaded.host_running &&
            loaded.active_pipelines == 3 &&
            loaded.inference_workers_configured == 2 &&
            loaded.inference_workers_ready == 2 &&
            loaded.inference_submitted_jobs == 101 &&
            loaded.inference_stale_results == 2 &&
            loaded.processor_active_sessions == 3 &&
            loaded.processor_persisted_alerts == 7 &&
            loaded.callback_profiles_ready == 1 &&
            loaded.callback_delivered == 6 &&
            loaded.callback_dead_letters == 1 &&
            loaded.callback_last_error_code == "CALLBACK_HTTP_500",
        "Redis heartbeat must preserve the complete P5 algorithm snapshot");

    require(queue.deleteKey(
            queue.workerHeartbeatKey(heartbeat.consumer_name),
            error),
        "test heartbeat key must be removed: " + error);
#ifdef _WIN32
    WSACleanup();
#endif
    std::cout << "Algorithm runtime Redis heartbeat tests passed\n";
    return 0;
}
