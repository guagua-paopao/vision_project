#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include <opencv2/core/utils/logger.hpp>
#include <spdlog/spdlog.h>

#include "server/app_config.h"
#include "server/app_logger.h"
#include "server/people_flow_inference_worker.h"

namespace {

    std::atomic<bool> stop_requested{ false };
    std::mutex stop_mutex;
    std::condition_variable stop_cv;

    void requestStop() {
        stop_requested.store(true);
        stop_cv.notify_all();
    }

#ifdef _WIN32
    BOOL WINAPI consoleCtrlHandler(DWORD type) {
        if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_SHUTDOWN_EVENT) {
            requestStop();
            return TRUE;
        }
        return FALSE;
    }
#else
    void signalHandler(int) { requestStop(); }
#endif

    std::string readConsumerName(int argc, char** argv, const std::string& fallback) {
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--consumer-name") return argv[i + 1];
        }
        return fallback;
    }

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return -1;
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#endif

    if (argc < 2) {
        std::cerr << "Usage: four_stage_worker.exe <config.yaml> [--consumer-name people_flow_worker_1]\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }

    int exit_code = 0;
    try {
        cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING);
        yolo11_server::AppConfig config = yolo11_server::AppConfig::loadFromYaml(argv[1]);
        config.redis.enabled = true;
        config.worker.enabled = true;
        config.people_flow.enabled = true;
        config.stream.enabled = false;
        if (!config.people_flow.security.enabled) config.model.type = "detect";
        const std::string consumer_name = readConsumerName(argc, argv,
            config.redis.consumer_name.empty() ? "people_flow_worker_1" : config.redis.consumer_name);

        std::string logger_error;
        if (!yolo11_server::initializeLogger(config, "people_flow_worker", logger_error)) {
            std::cerr << "Logger initialization warning: " << logger_error << '\n';
        }

        yolo11_server::PeopleFlowInferenceWorker worker(1, config, consumer_name);
        if (!worker.start()) {
            spdlog::error("Failed to start people-flow worker");
            exit_code = -1;
        }
        else {
            spdlog::info("People-flow worker started: consumer={}, stream={}, group={}",
                consumer_name, config.redis.stream_key, config.redis.consumer_group);
            std::unique_lock<std::mutex> lock(stop_mutex);
            stop_cv.wait(lock, []() { return stop_requested.load(); });
            worker.stop();
        }
        yolo11_server::shutdownLogger();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal people-flow worker error: " << e.what() << '\n';
        exit_code = -1;
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return exit_code;
}
