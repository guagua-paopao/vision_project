#include "server/app_logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace yolo11_server {

    namespace {
        std::shared_ptr<spdlog::logger> g_logger;

        spdlog::level::level_enum parseLogLevel(std::string level) {
            std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (level == "trace") return spdlog::level::trace;
            if (level == "debug") return spdlog::level::debug;
            if (level == "info") return spdlog::level::info;
            if (level == "warn" || level == "warning") return spdlog::level::warn;
            if (level == "error") return spdlog::level::err;
            if (level == "critical") return spdlog::level::critical;
            if (level == "off") return spdlog::level::off;
            return spdlog::level::info;
        }

        std::string logFilenameForRole(const std::string& role) {
            if (role == "server") {
                return "server.log";
            }
            if (role == "worker") {
                return "worker.log";
            }
            return role + ".log";
        }
    }  // namespace

    bool initializeLogger(const AppConfig& config, const std::string& role, std::string& error) {
        error.clear();

        try {
            if (!config.logging.enabled) {
                g_logger = spdlog::stdout_color_mt("yolo11");
                g_logger->set_level(spdlog::level::off);
                spdlog::set_default_logger(g_logger);
                return true;
            }

            std::vector<spdlog::sink_ptr> sinks;
            if (config.logging.console) {
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
            }

            if (config.logging.file) {
                std::filesystem::create_directories(config.logging.log_dir);
                const std::filesystem::path log_path =
                    std::filesystem::path(config.logging.log_dir) / logFilenameForRole(role);
                const size_t max_file_size = static_cast<size_t>(config.logging.max_file_size_mb) * 1024ULL * 1024ULL;
                sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    log_path.string(),
                    max_file_size,
                    static_cast<size_t>(config.logging.max_files)
                ));
            }

            if (sinks.empty()) {
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
            }

            g_logger = std::make_shared<spdlog::logger>("yolo11", sinks.begin(), sinks.end());
            g_logger->set_level(parseLogLevel(config.logging.level));
            g_logger->flush_on(spdlog::level::warn);
            g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [pid:%P] [tid:%t] %v");
            spdlog::register_logger(g_logger);
            spdlog::set_default_logger(g_logger);

            if (config.logging.flush_interval_sec > 0) {
                spdlog::flush_every(std::chrono::seconds(config.logging.flush_interval_sec));
            }
            return true;
        }
        catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    std::shared_ptr<spdlog::logger> logger() {
        if (!g_logger) {
            g_logger = spdlog::stdout_color_mt("yolo11_fallback");
            spdlog::set_default_logger(g_logger);
        }
        return g_logger;
    }

    void shutdownLogger() noexcept {
        try {
            if (g_logger) {
                g_logger->flush();
            }
            spdlog::shutdown();
            g_logger.reset();
        }
        catch (...) {
        }
    }

}  // namespace yolo11_server
