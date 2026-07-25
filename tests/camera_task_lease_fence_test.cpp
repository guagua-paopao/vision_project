#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include <hiredis/hiredis.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

#include "server/camera_task_queue.h"

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
    try {
        return std::stoi(value);
    }
    catch (...) {
        return fallback;
    }
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

    RedisSection redis;
    redis.host = host;
    redis.port = envInt("YOLO11_TEST_REDIS_PORT", 6379);
    redis.db = envInt("YOLO11_TEST_REDIS_DB", 0);
    if (const char* password = std::getenv("YOLO11_TEST_REDIS_PASSWORD")) {
        redis.password = password;
    }
    CameraTasksSection cameras;
    cameras.lease_ttl_seconds = 5;

    // Both objects deliberately use the same logical consumer name. They model
    // two Worker process generations and must still receive distinct owners.
    CameraTaskQueue first(redis, cameras, "p6_camera_worker");
    CameraTaskQueue second(redis, cameras, "p6_camera_worker");
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::string task_id = "lease_fence_" + std::to_string(unique);
    const std::string run_id = "run_fence_" + std::to_string(unique);
    std::string error;

    require(first.acquireRunLease(task_id, run_id, error),
        "first Worker generation must acquire the Run lease: " + error);
    error.clear();
    require(!second.acquireRunLease(task_id, run_id, error),
        "second Worker generation must not re-enter a live Run lease");
    error.clear();
    require(!second.refreshRunLease(task_id, run_id, error),
        "non-owner Worker generation must not refresh the Run lease");
    error.clear();
    require(!second.releaseRunLease(task_id, run_id, error),
        "non-owner Worker generation must not release the Run lease");
    error.clear();
    require(first.refreshRunLease(task_id, run_id, error),
        "owning Worker generation must refresh the Run lease: " + error);
    error.clear();
    require(first.releaseRunLease(task_id, run_id, error),
        "owning Worker generation must release the Run lease: " + error);
    error.clear();
    require(second.acquireRunLease(task_id, run_id, error),
        "replacement Worker generation must acquire after release: " + error);
    error.clear();
    require(second.releaseRunLease(task_id, run_id, error),
        "replacement Worker generation must clean up its lease: " + error);

    CameraTasksSection command_config;
    command_config.command_stream_key =
        "yolo:test:camera-r2-command:" + std::to_string(unique);
    command_config.consumer_group =
        "camera-r2-command-group-" + std::to_string(unique);
    command_config.lease_ttl_seconds = 5;
    CameraTaskQueue command_queue(redis, command_config, "camera_r2_codec");
    require(command_queue.start(error),
        "R2 command queue must initialize: " + error);

    CameraTaskCommand submitted;
    submitted.task_id = "camera_r2";
    submitted.run_id = "cr_r2";
    submitted.camera_profile = "entry_camera_01";
    submitted.definition_version = 3;
    submitted.analysis_enabled = true;
    submitted.algorithm_profile = "security_default";
    submitted.algorithms = { "people_flow" };
    submitted.origin = "camera_api";
    submitted.analysis_config_version = "entry-line-v3";
    submitted.initial_occupancy = 9;
    submitted.snapshot_fps = 2;
    submitted.algorithm_parameters_json = R"({"line_id":"main"})";
    submitted.create_time_ms = 1774412345000LL;
    require(command_queue.submitStart(submitted, error),
        "command_version 3 must serialize: " + error);
    CameraTaskCommand received;
    require(command_queue.poll(received, error),
        "command_version 3 must deserialize: " + error);
    require(received.origin == "camera_api" &&
            received.analysis_config_version == "entry-line-v3" &&
            received.initial_occupancy == 9 &&
            received.snapshot_fps == 2 &&
            received.algorithm_parameters_json == R"({"line_id":"main"})",
        "command_version 3 RunSpec fields must round-trip through Redis");
    require(command_queue.acknowledge(received.message_id, error),
        "command_version 3 must acknowledge");

    redisContext* raw = redisConnect(redis.host.c_str(), redis.port);
    require(raw != nullptr && raw->err == 0,
        "raw Redis connection must open for legacy command injection");
    if (!redis.password.empty()) {
        auto* auth = static_cast<redisReply*>(
            redisCommand(raw, "AUTH %s", redis.password.c_str()));
        require(auth != nullptr && auth->type != REDIS_REPLY_ERROR,
            "raw Redis authentication must succeed");
        freeReplyObject(auth);
    }
    auto* select = static_cast<redisReply*>(
        redisCommand(raw, "SELECT %d", redis.db));
    require(select != nullptr && select->type != REDIS_REPLY_ERROR,
        "raw Redis DB selection must succeed");
    freeReplyObject(select);
    auto* legacy = static_cast<redisReply*>(redisCommand(raw,
        "XADD %s * command_kind camera_frame_start command_version 2 "
        "task_id legacy_camera run_id cr_legacy camera_profile entry_camera_01 "
        "definition_version 1 frame_interval_ms 1000 output_mode latest "
        "jpeg_quality 90 max_width 0 max_height 0 retention_days 7 "
        "max_saved_frames 100 analysis_enabled 1 target_infer_fps 5 "
        "algorithm_profile security_default algorithms_json %s "
        "callback_profile backend_primary create_time_ms 123",
        command_config.command_stream_key.c_str(), R"(["people_flow"])"));
    require(legacy != nullptr && legacy->type != REDIS_REPLY_ERROR,
        "legacy command_version 2 message must be injected");
    freeReplyObject(legacy);

    CameraTaskCommand legacy_received;
    require(command_queue.poll(legacy_received, error),
        "legacy command_version 2 must remain consumable: " + error);
    require(legacy_received.task_id == "legacy_camera" &&
            legacy_received.algorithms ==
                std::vector<std::string>({ "people_flow" }) &&
            legacy_received.origin == "camera_api" &&
            legacy_received.analysis_config_version.empty() &&
            legacy_received.initial_occupancy == 0 &&
            legacy_received.snapshot_fps == 0 &&
            legacy_received.algorithm_parameters_json == "{}",
        "missing R2 fields must receive backward-compatible defaults");
    require(command_queue.acknowledge(legacy_received.message_id, error),
        "legacy command must acknowledge");
    auto* cleanup = static_cast<redisReply*>(redisCommand(
        raw, "DEL %s", command_config.command_stream_key.c_str()));
    require(cleanup != nullptr && cleanup->type != REDIS_REPLY_ERROR,
        "temporary command stream must be removed");
    freeReplyObject(cleanup);
    redisFree(raw);

#ifdef _WIN32
    WSACleanup();
#endif
    std::cout << "Camera Task lease fence and R2 command compatibility tests passed\n";
    return 0;
}
