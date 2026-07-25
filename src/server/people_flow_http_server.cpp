#include "server/people_flow_http_server.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "server/camera_profile.h"
#include "server/uri_masker.h"
#include "server/worker_runtime_readiness.h"

namespace yolo11_server {

namespace {
using json = nlohmann::json;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

crow::response jsonResponse(int code, const json& body) {
    crow::response response(code, body.dump());
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
    return response;
}

bool safeIdentifier(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
    });
}

bool constantTimeEqual(const std::string& left, const std::string& right) {
    const std::size_t maximum = std::max(left.size(), right.size());
    unsigned char difference = static_cast<unsigned char>(left.size() ^ right.size());
    for (std::size_t index = 0; index < maximum; ++index) {
        const unsigned char lhs = index < left.size() ? left[index] : 0;
        const unsigned char rhs = index < right.size() ? right[index] : 0;
        difference |= static_cast<unsigned char>(lhs ^ rhs);
    }
    return difference == 0;
}

std::string makeSessionId() {
    static std::mt19937_64 generator(std::random_device{}());
    std::ostringstream stream;
    stream << "pf_" << std::hex << nowMs() << '_' << generator();
    return stream.str();
}

bool readFile(const std::filesystem::path& path, std::string& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !bytes.empty();
}

json sessionJson(const PeopleFlowSessionStatus& item) {
    json body;
    body["success"] = item.status != "failed";
    body["session_id"] = item.session_id;
    body["camera_id"] = item.camera_id;
    body["camera_profile"] = item.camera_profile;
    body["config_version"] = item.config_version;
    body["status"] = item.status;
    body["stop_requested"] = item.stop_requested;
    body["capture"] = {
        {"state", item.capture_state}, {"backend", item.capture_backend},
        {"shared_hub", item.shared_hub}, {"hub_instance_id", item.hub_instance_id},
        {"hub_subscribers", item.hub_subscribers},
        {"capture_fps", item.capture_fps}, {"source_fps", item.source_fps},
        {"frame_count", item.frame_count}, {"dropped_frames", item.dropped_frames},
        {"latest_frame_age_ms", item.latest_frame_age_ms},
        {"width", item.width}, {"height", item.height}
    };
    body["inference"] = {
        {"infer_fps", item.infer_fps}, {"last_inference_ms", item.last_inference_ms},
        {"live_persons", item.live_persons}
    };
    body["flow"] = {
        {"in", item.in_count}, {"out", item.out_count},
        {"initial_occupancy", item.initial_occupancy}, {"occupancy", item.occupancy}
    };
    body["storage"] = {
        {"degraded", item.storage_degraded}, {"event_queue_depth", item.event_queue_depth},
        {"snapshot_degraded", item.snapshot_degraded}
    };
    body["source"] = {{"profile", item.camera_profile}, {"masked_uri", item.masked_uri}};
    body["create_time_ms"] = item.create_time_ms;
    body["start_time_ms"] = item.start_time_ms;
    body["stop_time_ms"] = item.stop_time_ms;
    body["last_update_ms"] = item.last_update_ms;
    body["snapshot_url"] = "/api/v1/people-flow/" + item.session_id + "/snapshot";
    body["security_url"] = "/api/v1/people-flow/" + item.session_id + "/security";
    if (!item.error.empty()) body["error"] = item.error;
    if (!item.last_error.empty()) body["last_error"] = item.last_error;
    return body;
}
}  // namespace

PeopleFlowHttpServer::PeopleFlowHttpServer(const AppConfig& config)
    : config_(config), redis_(config.redis) {
    if (config_.camera_tasks.enabled) {
        camera_task_repository_ =
            std::make_shared<CameraTaskRepository>(config_.camera_tasks);
        camera_task_control_ = std::make_shared<CameraTaskQueue>(
            config_.redis, config_.camera_tasks, "camera_task_http");
        if (!config_.stream.camera_profiles_path.empty()) {
            camera_profile_registry_ = std::make_shared<CameraProfileRegistry>(
                config_.stream.camera_profiles_path);
        }
        unified_camera_application_service_ =
            std::make_shared<UnifiedCameraApplicationService>(
                config_,
                camera_task_repository_,
                camera_task_control_,
                camera_profile_registry_);
        camera_task_controller_ = std::make_unique<CameraTaskHttpController>(
            config_,
            camera_task_repository_,
            camera_task_control_,
            std::string{},
            camera_profile_registry_,
            [this](AlgorithmRuntimeSnapshot& runtime, std::string& error) {
                return readAlgorithmRuntime(runtime, error);
            },
            unified_camera_application_service_);
    }
}

PeopleFlowHttpServer::~PeopleFlowHttpServer() noexcept {
    if (repository_) repository_->stop();
}

bool PeopleFlowHttpServer::initialize(std::string& error) {
    if (!config_.redis.enabled || !config_.people_flow.enabled) {
        error = "redis.enabled and people_flow.enabled must both be true";
        return false;
    }
    if (!redis_.connect(error)) return false;
    if (const char* token = std::getenv(config_.people_flow.admin_token_env.c_str())) {
        admin_token_ = token;
    }
    repository_ = std::make_unique<PeopleFlowRepository>(config_.people_flow);
    std::string storage_error;
    if (!repository_->start(false, storage_error)) {
        spdlog::warn("PostgreSQL query layer starts degraded: {}", storage_error);
    }
    if (camera_task_controller_ && !camera_task_controller_->initialize(error)) return false;
    if (config_.runtime.unified_camera_pipeline &&
        config_.runtime.people_flow_compatibility) {
        if (!camera_task_controller_ || !camera_task_repository_ ||
            !camera_task_control_ ||
            !unified_camera_application_service_) {
            error =
                "unified People Flow compatibility requires camera_tasks.enabled";
            return false;
        }
        people_flow_compatibility_controller_ =
            std::make_unique<PeopleFlowCompatibilityController>(
                config_,
                camera_task_repository_,
                camera_task_control_,
                camera_profile_registry_,
                unified_camera_application_service_,
                repository_.get());
    }
    return true;
}

void PeopleFlowHttpServer::registerRoutes(crow::SimpleApp& app) {
    CROW_ROUTE(app, "/camera-admin")([this]() {
        return adminAsset("index.html", "text/html; charset=utf-8");
    });
    CROW_ROUTE(app, "/camera-admin/app.js")([this]() {
        return adminAsset("app.js", "application/javascript; charset=utf-8");
    });
    CROW_ROUTE(app, "/camera-admin/styles.css")([this]() {
        return adminAsset("styles.css", "text/css; charset=utf-8");
    });
    CROW_ROUTE(app, "/api/v1/health")([this]() { return health(); });
    CROW_ROUTE(app, "/api/v1/ready")([this]() { return ready(); });
    CROW_ROUTE(app, "/api/v1/people-flow/start").methods(crow::HTTPMethod::POST)(
        [this](const crow::request& request) { return start(request); });
    CROW_ROUTE(app, "/api/v1/people-flow/<string>/stop").methods(crow::HTTPMethod::POST)(
        [this](const crow::request& request, const std::string& id) {
            return stop(request, id);
        });
    CROW_ROUTE(app, "/api/v1/people-flow/<string>/status")(
        [this](const std::string& id) { return status(id); });
    CROW_ROUTE(app, "/api/v1/people-flow/<string>/snapshot")(
        [this](const std::string& id) { return snapshot(id); });
    CROW_ROUTE(app, "/api/v1/people-flow/<string>/security")(
        [this](const std::string& id) { return security(id); });
    CROW_ROUTE(app, "/api/v1/people-flow/cameras/<string>/realtime")(
        [this](const std::string& camera_id) { return realtime(camera_id); });
    CROW_ROUTE(app, "/api/v1/people-flow/cameras/<string>/events")(
        [this](const crow::request& request, const std::string& camera_id) {
            return events(request, camera_id);
        });
    if (camera_task_controller_) camera_task_controller_->registerRoutes(app);
}

bool PeopleFlowHttpServer::authorized(const crow::request& request) const {
    static const std::string prefix = "Bearer ";
    const std::string header = request.get_header_value("Authorization");
    if (admin_token_.empty() || header.rfind(prefix, 0) != 0) return false;
    return constantTimeEqual(header.substr(prefix.size()), admin_token_);
}

crow::response PeopleFlowHttpServer::adminAsset(
    const std::string& file_name,
    const std::string& content_type
) const {
    std::string bytes;
    const auto root = std::filesystem::absolute(
        std::filesystem::u8path(config_.camera_tasks.admin_ui_dir));
    if (!readFile(root / std::filesystem::u8path(file_name), bytes)) {
        return crow::response(404, "Camera admin asset was not found");
    }
    crow::response response(200, std::move(bytes));
    response.set_header("Content-Type", content_type);
    response.set_header("Cache-Control", file_name == "index.html" ? "no-store" : "public, max-age=300");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Content-Security-Policy",
        "default-src 'self'; img-src 'self' blob:; style-src 'self'; script-src 'self'; connect-src 'self'");
    return response;
}

crow::response PeopleFlowHttpServer::health() const {
    std::string error;
    const bool redis_ok = redis_.ping(error);
    const auto storage = repository_ ? repository_->health() : PeopleFlowRepositoryHealth{};
    const auto camera = camera_task_controller_
        ? camera_task_controller_->health() : CameraTaskHttpHealth{};
    const bool camera_ok = !camera.enabled ||
        (camera.initialized && camera.storage_ok && camera.output_root_writable &&
            camera.worker_num_valid && camera.callback_config_valid);
    const bool healthy = redis_ok && camera_ok;
    return jsonResponse(healthy ? 200 : 503, {
        {"success", healthy}, {"service", "four-stage-people-flow"},
        {"redis", {{"ok", redis_ok}, {"error", error}}},
        {"people_flow_enabled", config_.people_flow.enabled},
        {"people_flow_security_enabled", config_.people_flow.security.enabled},
        {"people_flow_security_mode", config_.people_flow.security.mode},
        {"storage", {{"started", storage.started}, {"degraded", storage.degraded}}},
        {"camera_tasks", {
            {"enabled", camera.enabled}, {"initialized", camera.initialized},
            {"storage_ok", camera.storage_ok},
            {"output_root_writable", camera.output_root_writable},
            {"token_configured", camera.token_configured},
            {"worker_num_valid", camera.worker_num_valid},
            {"callback_config_valid", camera.callback_config_valid},
            {"analysis_enabled", config_.analysis.enabled},
            {"callbacks_enabled", config_.callbacks.enabled}
        }}
    });
}

crow::response PeopleFlowHttpServer::ready() const {
    std::string redis_error;
    const bool redis_ok = redis_.ping(redis_error);
    std::vector<WorkerHeartbeatRecord> workers;
    std::string worker_error;
    const bool workers_ok = redis_ok && redis_.getWorkerHeartbeats(
        config_.worker.consumer_name_prefix, config_.worker.worker_num, workers, worker_error);
    int alive = 0;
    json worker_items = json::array();
    for (const auto& worker : workers) {
        if (worker.alive) ++alive;
        worker_items.push_back({
            {"consumer_name", worker.consumer_name}, {"alive", worker.alive},
            {"status", worker.status}, {"runner_model_type", worker.runner_model_type},
            {"worker_group", worker.worker_group},
            {"runtime_mode", worker.runtime_mode},
            {"worker_generation", worker.worker_generation},
            {"legacy_people_flow_role", worker.legacy_people_flow_role},
            {"camera_task_manager_running",
                worker.camera_task_manager_running},
            {"hub_registry_ready", worker.hub_registry_ready},
            {"coordination_healthy", worker.coordination_healthy},
            {"last_error", worker.last_error}
        });
    }
    const auto camera = camera_task_controller_
        ? camera_task_controller_->health() : CameraTaskHttpHealth{};
    const auto worker_readiness = evaluateWorkerRuntimeReadiness(
        workers,
        camera.enabled,
        config_.runtime.unified_camera_pipeline);
    const std::string expected_runtime_mode =
        config_.runtime.unified_camera_pipeline
            ? "unified_camera_pipeline" : "legacy_split";
    const bool legacy_role_absent =
        !config_.runtime.unified_camera_pipeline ||
        !worker_readiness.legacy_people_flow_worker_detected;
    const auto& algorithm_runtime =
        worker_readiness.algorithm_runtime;
    const bool algorithm_runtime_available =
        !camera.enabled || algorithm_runtime.generated_at_ms > 0;
    const bool algorithm_runtime_fresh = !camera.enabled ||
        (algorithm_runtime_available &&
            nowMs() - algorithm_runtime.generated_at_ms <=
                std::max(5000, config_.worker.heartbeat_interval_ms * 3));
    const bool inference_pool_ready =
        !camera.enabled || !config_.analysis.enabled ||
        (algorithm_runtime.inference_running &&
            algorithm_runtime.processor_running &&
            algorithm_runtime.inference_workers_configured ==
                config_.analysis.inference_workers &&
            algorithm_runtime.inference_workers_ready ==
                config_.analysis.inference_workers);
    const bool callback_delivery_ready =
        !camera.enabled || !config_.callbacks.enabled ||
        (algorithm_runtime.callback_running &&
            algorithm_runtime.callback_profiles_ready > 0);
    const bool camera_ready = !camera.enabled ||
        (camera.initialized && camera.token_configured && camera.storage_ok &&
            camera.output_root_writable && camera.worker_num_valid &&
            camera.callback_config_valid &&
            worker_readiness.camera_role_alive &&
            worker_readiness.single_vision_worker &&
            worker_readiness.mode_consistent &&
            legacy_role_absent &&
            worker_readiness.coordination_healthy &&
            worker_readiness.camera_task_manager_running &&
            worker_readiness.hub_registry_ready &&
            algorithm_runtime_available && algorithm_runtime_fresh &&
            algorithm_runtime.host_running && inference_pool_ready &&
            callback_delivery_ready);
    const bool is_ready = redis_ok && workers_ok && alive >= config_.worker.min_alive_workers && camera_ready;
    return jsonResponse(is_ready ? 200 : 503, {
        {"success", is_ready}, {"ready", is_ready}, {"redis_ok", redis_ok},
        {"alive_workers", alive}, {"required_workers", config_.worker.min_alive_workers},
        {"worker_error", worker_error}, {"workers", worker_items},
        {"security_enabled", config_.people_flow.security.enabled},
        {"camera_tasks_ready", camera_ready},
        {"camera_frame_role_alive", worker_readiness.camera_role_alive},
        {"expected_runtime_mode", expected_runtime_mode},
        {"worker_mode_consistent", worker_readiness.mode_consistent},
        {"single_vision_worker", worker_readiness.single_vision_worker},
        {"legacy_people_flow_worker_detected",
            worker_readiness.legacy_people_flow_worker_detected},
        {"worker_coordination_healthy",
            worker_readiness.coordination_healthy},
        {"camera_task_manager_running",
            worker_readiness.camera_task_manager_running},
        {"hub_registry_ready", worker_readiness.hub_registry_ready},
        {"algorithm_runtime_available", algorithm_runtime_available},
        {"algorithm_runtime_fresh", algorithm_runtime_fresh},
        {"algorithm_runtime_generated_at_ms", algorithm_runtime.generated_at_ms},
        {"inference_pool_ready", inference_pool_ready},
        {"callback_delivery_ready", callback_delivery_ready}
    });
}

bool PeopleFlowHttpServer::readAlgorithmRuntime(
    AlgorithmRuntimeSnapshot& runtime,
    std::string& error
) const {
    runtime = {};
    std::vector<WorkerHeartbeatRecord> workers;
    if (!redis_.getWorkerHeartbeats(
            config_.worker.consumer_name_prefix,
            config_.worker.worker_num,
            workers,
            error)) {
        return false;
    }
    for (const auto& worker : workers) {
        if (!worker.alive || worker.worker_kind != "vision_host") continue;
        if (worker.algorithm_runtime.generated_at_ms > runtime.generated_at_ms) {
            runtime = worker.algorithm_runtime;
        }
    }
    if (runtime.generated_at_ms <= 0) {
        error = "ALGORITHM_RUNTIME_UNAVAILABLE";
        return false;
    }
    error.clear();
    return true;
}

crow::response PeopleFlowHttpServer::start(const crow::request& request) {
    if (!authorized(request)) {
        return jsonResponse(401, {{"success", false}, {"error_code", "UNAUTHORIZED"}});
    }
    if (people_flow_compatibility_controller_) {
        return people_flow_compatibility_controller_->start(request);
    }
    const auto max_bytes = static_cast<std::size_t>(std::max(1, config_.server.max_body_size_mb)) * 1024U * 1024U;
    if (request.body.size() > max_bytes) {
        return jsonResponse(413, {{"success", false}, {"error_code", "REQUEST_TOO_LARGE"}});
    }
    json input = request.body.empty() ? json::object() : json::parse(request.body, nullptr, false);
    if (input.is_discarded() || !input.is_object()) {
        return jsonResponse(400, {{"success", false}, {"error_code", "INVALID_JSON"}});
    }
    if (input.contains("source_uri") || input.contains("rtsp_url") || input.contains("password")) {
        return jsonResponse(400, {{"success", false}, {"error_code", "RTSP_URI_IN_REQUEST_FORBIDDEN"}});
    }

    const std::string profile_id = input.value("camera_profile", config_.people_flow.camera_profile);
    const std::string camera_id = input.value("camera_id", config_.people_flow.camera_id);
    const std::string version = input.value("config_version", config_.people_flow.config_version);
    const long long initial = input.value("initial_occupancy", config_.people_flow.initial_occupancy);
    if (!safeIdentifier(profile_id) || camera_id != config_.people_flow.camera_id ||
        profile_id != config_.people_flow.camera_profile || version != config_.people_flow.config_version ||
        initial < 0 || initial > 1000000000LL) {
        return jsonResponse(400, {{"success", false}, {"error_code", "INVALID_SESSION_CONFIG"}});
    }

    const auto profile_it = config_.camera_profiles.find(profile_id);
    if (profile_it == config_.camera_profiles.end()) {
        return jsonResponse(400, {{"success", false}, {"error_code", "CAMERA_PROFILE_NOT_FOUND"}});
    }
    std::string uri;
    std::string profile_error;
    if (!resolveCameraProfileUri(profile_it->second, uri, profile_error) || !isRtspUri(uri)) {
        std::fill(uri.begin(), uri.end(), '\0');
        return jsonResponse(503, {
            {"success", false}, {"error_code", "CAMERA_PROFILE_UNAVAILABLE"},
            {"error", profile_error.empty() ? "camera URI must use rtsp:// or rtsps://" : profile_error}
        });
    }
    const std::string masked_uri = maskRtspUri(uri);
    std::fill(uri.begin(), uri.end(), '\0');

    const std::string session_id = makeSessionId();
    const std::filesystem::path session_dir = std::filesystem::path(config_.people_flow.output_dir) /
        camera_id / session_id;
    std::error_code directory_error;
    std::filesystem::create_directories(session_dir, directory_error);
    if (directory_error) {
        return jsonResponse(500, {{"success", false}, {"error_code", "OUTPUT_DIRECTORY_FAILED"}});
    }

    PeopleFlowStartRequest command;
    command.session_id = session_id;
    command.camera_id = camera_id;
    command.camera_profile = profile_id;
    command.source_ref = "env:" + profile_it->second.url_env;
    command.masked_uri = masked_uri;
    command.config_version = version;
    command.snapshot_path = (session_dir / "latest.jpg").string();
    command.initial_occupancy = initial;
    command.create_time_ms = nowMs();
    command.active_ttl_seconds = config_.people_flow.active_ttl_seconds;
    command.session_ttl_seconds = config_.people_flow.session_ttl_seconds;
    std::string redis_error;
    if (!redis_.submitPeopleFlowTask(command, redis_error)) {
        const bool active = redis_error.rfind("CAMERA_ALREADY_ACTIVE", 0) == 0;
        return jsonResponse(active ? 409 : 500, {
            {"success", false}, {"error_code", active ? "CAMERA_ALREADY_ACTIVE" : "REDIS_ERROR"},
            {"error", redis_error}
        });
    }
    spdlog::info("Queued people-flow session {} for {} ({})", session_id, camera_id, masked_uri);
    return jsonResponse(202, {
        {"success", true}, {"session_id", session_id}, {"camera_id", camera_id},
        {"camera_profile", profile_id}, {"config_version", version}, {"status", "queued"},
        {"status_url", "/api/v1/people-flow/" + session_id + "/status"},
        {"snapshot_url", "/api/v1/people-flow/" + session_id + "/snapshot"},
        {"security_url", "/api/v1/people-flow/" + session_id + "/security"},
        {"stop_url", "/api/v1/people-flow/" + session_id + "/stop"}
    });
}

crow::response PeopleFlowHttpServer::stop(
    const crow::request& request,
    const std::string& session_id
) const {
    if (!authorized(request)) {
        return jsonResponse(401, {{"success", false}, {"error_code", "UNAUTHORIZED"}});
    }
    if (people_flow_compatibility_controller_) {
        auto response =
            people_flow_compatibility_controller_->stop(session_id);
        if (response.code != 404 ||
            !config_.runtime.legacy_people_flow_fallback) {
            return response;
        }
    }
    if (!safeIdentifier(session_id) || session_id.rfind("pf_", 0) != 0) {
        return jsonResponse(400, {{"success", false}, {"error_code", "INVALID_SESSION_ID"}});
    }
    PeopleFlowSessionStatus item;
    std::string error;
    if (!redis_.getPeopleFlowSession(session_id, item, error)) {
        return jsonResponse(500, {{"success", false}, {"error_code", "REDIS_ERROR"}, {"error", error}});
    }
    if (!item.found) return jsonResponse(404, {{"success", false}, {"error_code", "SESSION_NOT_FOUND"}});
    if (item.status == "stopped" || item.status == "failed") {
        return jsonResponse(409, {{"success", false}, {"error_code", "SESSION_ALREADY_FINISHED"}});
    }
    if (!redis_.requestStopPeopleFlow(session_id, error)) {
        return jsonResponse(500, {{"success", false}, {"error_code", "REDIS_ERROR"}, {"error", error}});
    }
    return jsonResponse(200, {
        {"success", true}, {"session_id", session_id}, {"camera_id", item.camera_id},
        {"status", "stopping"}, {"stop_requested", true}
    });
}

crow::response PeopleFlowHttpServer::status(const std::string& session_id) const {
    if (people_flow_compatibility_controller_) {
        auto response =
            people_flow_compatibility_controller_->status(session_id);
        if (response.code != 404 ||
            !config_.runtime.legacy_people_flow_fallback) {
            return response;
        }
    }
    if (!safeIdentifier(session_id)) return jsonResponse(400, {{"success", false}, {"error_code", "INVALID_SESSION_ID"}});
    PeopleFlowSessionStatus item;
    std::string error;
    if (!redis_.getPeopleFlowSession(session_id, item, error)) {
        return jsonResponse(500, {{"success", false}, {"error_code", "REDIS_ERROR"}, {"error", error}});
    }
    if (!item.found) return jsonResponse(404, {{"success", false}, {"error_code", "SESSION_NOT_FOUND"}});
    return jsonResponse(200, sessionJson(item));
}

crow::response PeopleFlowHttpServer::snapshot(const std::string& session_id) const {
    if (people_flow_compatibility_controller_) {
        auto response =
            people_flow_compatibility_controller_->snapshot(session_id);
        if (response.code != 404 ||
            !config_.runtime.legacy_people_flow_fallback) {
            return response;
        }
    }
    PeopleFlowSessionStatus item;
    std::string error;
    if (!safeIdentifier(session_id) || !redis_.getPeopleFlowSession(session_id, item, error)) {
        return jsonResponse(400, {{"success", false}, {"error_code", "INVALID_SESSION"}});
    }
    if (!item.found) return jsonResponse(404, {{"success", false}, {"error_code", "SESSION_NOT_FOUND"}});
    std::string bytes;
    if (!readFile(item.snapshot_path, bytes)) {
        return jsonResponse(404, {{"success", false}, {"error_code", "SNAPSHOT_NOT_READY"}});
    }
    crow::response response(200, std::move(bytes));
    response.set_header("Content-Type", "image/jpeg");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response PeopleFlowHttpServer::security(const std::string& session_id) const {
    if (people_flow_compatibility_controller_) {
        auto response =
            people_flow_compatibility_controller_->security(session_id);
        if (response.code != 404 ||
            !config_.runtime.legacy_people_flow_fallback) {
            return response;
        }
    }
    PeopleFlowSessionStatus item;
    std::string error;
    if (!safeIdentifier(session_id) || !redis_.getPeopleFlowSession(session_id, item, error)) {
        return jsonResponse(400, {{"success", false}, {"error_code", "INVALID_SESSION"}});
    }
    if (!item.found) return jsonResponse(404, {{"success", false}, {"error_code", "SESSION_NOT_FOUND"}});
    const auto state_path = std::filesystem::path(item.snapshot_path).parent_path() /
        config_.people_flow.security.state_file_name;
    std::string bytes;
    if (!readFile(state_path, bytes)) {
        return jsonResponse(404, {{"success", false}, {"error_code", "SECURITY_STATE_NOT_READY"}});
    }
    crow::response response(200, std::move(bytes));
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response PeopleFlowHttpServer::realtime(const std::string& camera_id) const {
    if (people_flow_compatibility_controller_) {
        auto response =
            people_flow_compatibility_controller_->realtime(camera_id);
        if (response.code != 404 ||
            !config_.runtime.legacy_people_flow_fallback) {
            return response;
        }
    }
    if (!safeIdentifier(camera_id)) return jsonResponse(400, {{"success", false}, {"error_code", "INVALID_CAMERA_ID"}});
    PeopleFlowSessionStatus item;
    std::string error;
    if (!redis_.getPeopleFlowRealtime(camera_id, item, error)) {
        return jsonResponse(500, {{"success", false}, {"error_code", "REDIS_ERROR"}, {"error", error}});
    }
    if (!item.found) return jsonResponse(404, {{"success", false}, {"error_code", "CAMERA_NOT_FOUND"}});
    return jsonResponse(200, {
        {"success", true}, {"camera_id", camera_id}, {"session_id", item.session_id},
        {"status", item.status}, {"in", item.in_count}, {"out", item.out_count},
        {"occupancy", item.occupancy}, {"live_persons", item.live_persons},
        {"frame_count", item.frame_count}, {"capture_fps", item.capture_fps},
        {"infer_fps", item.infer_fps}, {"last_inference_ms", item.last_inference_ms},
        {"latest_frame_age_ms", item.latest_frame_age_ms}
    });
}

crow::response PeopleFlowHttpServer::events(
    const crow::request& request,
    const std::string& camera_id
) const {
    if (people_flow_compatibility_controller_) {
        return people_flow_compatibility_controller_->events(
            request, camera_id);
    }
    if (!safeIdentifier(camera_id) || !repository_) {
        return jsonResponse(400, {{"success", false}, {"error_code", "INVALID_CAMERA_ID"}});
    }
    int limit = 100;
    if (const char* raw = request.url_params.get("limit")) {
        try { limit = std::clamp(std::stoi(raw), 1, 1000); }
        catch (...) { return jsonResponse(400, {{"success", false}, {"error_code", "INVALID_LIMIT"}}); }
    }
    std::vector<CrossingEvent> rows;
    std::string error;
    if (!repository_->queryEvents(camera_id, "", 0, std::numeric_limits<long long>::max(), limit, 0, rows, error)) {
        return jsonResponse(503, {{"success", false}, {"error_code", "STORAGE_QUERY_FAILED"}, {"error", error}});
    }
    json items = json::array();
    for (const auto& event : rows) {
        items.push_back({
            {"event_id", event.event_id}, {"session_id", event.session_id},
            {"camera_id", event.camera_id}, {"line_id", event.line_id},
            {"track_id", event.track_id}, {"direction", event.direction},
            {"event_time_ms", event.event_time_ms}, {"confidence", event.confidence},
            {"point_x_norm", event.point_x_norm}, {"point_y_norm", event.point_y_norm}
        });
    }
    return jsonResponse(200, {
        {"success", true}, {"camera_id", camera_id}, {"count", items.size()}, {"events", items}
    });
}

}  // namespace yolo11_server
