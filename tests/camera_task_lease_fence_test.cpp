#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

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

#ifdef _WIN32
    WSACleanup();
#endif
    std::cout << "Camera Task Worker-generation lease fence tests passed\n";
    return 0;
}
