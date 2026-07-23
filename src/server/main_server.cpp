#include <exception>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

#include <crow.h>
#include <spdlog/spdlog.h>

#include "server/app_config.h"
#include "server/app_logger.h"
#include "server/people_flow_http_server.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#endif
    int result = 0;
    try {
        const std::string config_path = argc > 1 ? argv[1] : "config/server.yaml";
        const auto config = yolo11_server::AppConfig::loadFromYaml(config_path);
        std::cerr << "[BOOT] server configuration loaded\n";
        std::string logger_error;
        yolo11_server::initializeLogger(config, "server", logger_error);
        std::cerr << "[BOOT] server logger initialized\n";

        yolo11_server::PeopleFlowHttpServer controller(config);
        std::cerr << "[BOOT] server controller constructed\n";
        std::string error;
        if (!controller.initialize(error)) {
            throw std::runtime_error("server initialization failed: " + error);
        }
        std::cerr << "[BOOT] server dependencies initialized\n";

        crow::SimpleApp app;
        app.loglevel(crow::LogLevel::Warning);
        controller.registerRoutes(app);
        spdlog::info("Four-stage HTTP server: http://{}:{}", config.server.host, config.server.port);
        app.bindaddr(config.server.host)
            .port(static_cast<uint16_t>(config.server.port))
            .concurrency(static_cast<unsigned int>(config.server.threads))
            .run();
        yolo11_server::shutdownLogger();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal server error: " << e.what() << std::endl;
        result = 1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
