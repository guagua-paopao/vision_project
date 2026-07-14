#pragma once

#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "server/app_config.h"

namespace yolo11_server {

    bool initializeLogger(const AppConfig& config, const std::string& role, std::string& error);
    std::shared_ptr<spdlog::logger> logger();
    void shutdownLogger() noexcept;

}  // namespace yolo11_server
