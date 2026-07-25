#include <cstdlib>
#include <iostream>
#include <string>

#include "server/app_config.h"
#include "server/app_logger.h"
#include "server/rtsp_camera_frame_source.h"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace yolo11_server;
    require(argc == 3, "server and worker YAML paths are required");

    const auto server = AppConfig::loadFromYaml(argv[1]);
    require(server.server.host == "127.0.0.1" && server.server.port == 8087,
        "server YAML must be read instead of silently falling back to defaults");
    require(server.redis.enabled && server.redis.host == "127.0.0.1" &&
        server.redis.port == 6379, "server Redis configuration must parse");
    require(server.camera_hub.enabled && server.camera_hub.require_ffmpeg_backend &&
        !server.capture.allow_backend_fallback, "server FFmpeg Hub invariant must parse");
    require(server.camera_tasks.enabled && server.worker.worker_num == 1,
        "server Camera Task and single-worker configuration must parse");
    require(server.analysis.enabled && server.analysis.inference_workers == 2 &&
            server.analysis.model_init_timeout_ms == 120000 &&
            server.analysis.supported_algorithms ==
                std::vector<std::string>({
                    "electronic_fence", "people_flow", "pose_action",
                    "security", "temporal_action"
                }),
        "server fixed inference-pool configuration must parse");
    require(!server.runtime.unified_camera_pipeline &&
            server.runtime.people_flow_compatibility &&
            server.runtime.legacy_people_flow_fallback &&
            !server.runtime.shadow_compare,
        "server R4 migration switches must retain the rollback-safe baseline");
    require(!server.callbacks.enabled &&
            server.callbacks.poll_interval_ms == 250 &&
            server.callbacks.request_timeout_ms == 5000 &&
            server.callbacks.lease_timeout_ms == 30000 &&
            server.callbacks.max_attempts == 8 &&
            server.callbacks.initial_backoff_ms == 1000 &&
            server.callbacks.max_backoff_ms == 300000 &&
            server.callbacks.request_body_limit_bytes == 1048576 &&
            server.callbacks.response_body_limit_bytes == 4096 &&
            server.callbacks.config_error.empty() &&
            server.callbacks.profiles.count("backend_primary") == 1 &&
            server.callbacks.profiles.at("backend_primary").url_env ==
                "YOLO11_CALLBACK_BACKEND_PRIMARY_URL" &&
            server.callbacks.profiles.at("backend_primary").hmac_secret_env ==
                "YOLO11_CALLBACK_BACKEND_PRIMARY_SECRET" &&
            !server.callbacks.profiles.at("backend_primary").allow_insecure_http,
        "server callback delivery configuration must parse without resolving secrets");
    require(server.people_flow.admin_token_env == server.camera_tasks.admin_token_env &&
            server.people_flow.admin_token_env == "YOLO11_CAMERA_TASK_ADMIN_TOKEN",
        "People Flow control mutations and Camera API must share one bearer-token source");
    require(!server.camera_tasks.defaults.analysis_enabled &&
            server.camera_tasks.defaults.target_infer_fps == 5.0 &&
            server.camera_tasks.defaults.algorithm_profile.empty() &&
            server.camera_tasks.defaults.algorithms.empty() &&
            server.camera_tasks.defaults.callback_profile.empty(),
        "server algorithm task defaults must parse");
    require(server.camera_tasks.admin_ui_dir == "./web/camera-admin" &&
        server.camera_tasks.storage.max_archive_bytes == 21474836480LL &&
        server.camera_tasks.storage.min_free_bytes == 1073741824LL &&
        server.camera_tasks.storage.high_watermark_percent == 85 &&
        server.camera_tasks.storage.critical_watermark_percent == 95,
        "M8 admin and M10 storage policy must parse from server YAML");
    require(server.camera_profiles_error.empty() &&
        server.camera_profiles.count("entry_camera_01") == 1 &&
        server.camera_profiles.at("entry_camera_01").url_env == "YOLO11_CAMERA_ENTRY_URL",
        "server camera profile registry must parse through the ABI-safe loader");

    const auto worker = AppConfig::loadFromYaml(argv[2]);
    require(worker.camera_tasks.enabled && worker.worker.enabled &&
        worker.worker.worker_num == 1, "worker Camera Task configuration must parse");
    require(worker.analysis.enabled && worker.analysis.inference_workers == 2 &&
            worker.analysis.model_init_timeout_ms == 120000,
        "worker fixed inference-pool configuration must parse");
    require(worker.runtime.unified_camera_pipeline ==
                server.runtime.unified_camera_pipeline &&
            worker.runtime.people_flow_compatibility ==
                server.runtime.people_flow_compatibility &&
            worker.runtime.legacy_people_flow_fallback ==
                server.runtime.legacy_people_flow_fallback,
        "server and worker R4 migration switches must match");
    require(!worker.callbacks.enabled &&
            worker.callbacks.profiles.count("backend_primary") == 1 &&
            worker.callbacks.profiles.at("backend_primary").url_env ==
                server.callbacks.profiles.at("backend_primary").url_env &&
            worker.callbacks.profiles.at("backend_primary").hmac_secret_env ==
                server.callbacks.profiles.at("backend_primary").hmac_secret_env,
        "worker callback allow-list must match the server without loading secret values");
    require(!worker.camera_tasks.defaults.analysis_enabled &&
            worker.camera_tasks.defaults.target_infer_fps == 5.0 &&
            worker.camera_tasks.defaults.algorithms.empty(),
        "worker algorithm task defaults must match the safe disabled baseline");
    require(worker.camera_profiles_error.empty() &&
        worker.camera_profiles.count("entry_camera_01") == 1,
        "worker camera profile registry must parse");
    require(worker.camera_tasks.command_stream_key == server.camera_tasks.command_stream_key &&
        worker.camera_tasks.consumer_group == server.camera_tasks.consumer_group &&
        worker.camera_tasks.storage.max_archive_bytes == server.camera_tasks.storage.max_archive_bytes &&
        worker.camera_tasks.storage.min_free_bytes == server.camera_tasks.storage.min_free_bytes,
        "server and worker Camera Task Redis routing and storage policy must match");
    std::cerr << "[TEST] constructing production FrameHub registry\n";
    auto registry = createSharedCameraFrameHubRegistry(worker);
    std::cerr << "[TEST] production FrameHub registry constructed\n";
    require(registry && registry->snapshots().empty(),
        "production FrameHub registry must construct from parsed worker config");
    registry->stopAll();
    std::cerr << "[TEST] production FrameHub registry stopped\n";
    registry.reset();

    std::string logger_error;
    require(initializeLogger(server, "config_runtime_test", logger_error),
        "production logger must initialize from parsed config: " + logger_error);
    shutdownLogger();

    bool missing_failed_closed = false;
    try {
        (void)AppConfig::loadFromYaml("definitely_missing_camera_config.yaml");
    }
    catch (const std::exception&) {
        missing_failed_closed = true;
    }
    require(missing_failed_closed, "an explicitly missing config must fail closed");

    std::cout << "Runtime YAML configuration tests passed\n";
    return 0;
}
