#include "server/people_flow_compatibility_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/camera_profile.h"
#include "server/camera_run_spec.h"
#include "server/redis_task_queue.h"
#include "server/uri_masker.h"

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

std::string makeSessionId() {
    static std::atomic<unsigned long long> sequence{ 0 };
    std::ostringstream output;
    output << "pf_" << std::hex << nowMs() << '_' << ++sequence;
    return output.str();
}

bool readFile(const std::filesystem::path& path, std::string& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    bytes.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    return !bytes.empty();
}

bool pathWithin(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

bool resolveArtifact(
    const std::string& root_value,
    const std::string& relative_value,
    std::filesystem::path& resolved
) {
    if (relative_value.empty()) return false;
    const auto relative = std::filesystem::u8path(relative_value);
    if (relative.is_absolute()) return false;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        std::filesystem::absolute(std::filesystem::u8path(root_value), error),
        error);
    if (error) return false;
    resolved = std::filesystem::weakly_canonical(root / relative, error);
    return !error && pathWithin(root, resolved);
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
        {"shared_hub", item.shared_hub},
        {"hub_instance_id", item.hub_instance_id},
        {"hub_subscribers", item.hub_subscribers},
        {"capture_fps", item.capture_fps}, {"source_fps", item.source_fps},
        {"frame_count", item.frame_count},
        {"dropped_frames", item.dropped_frames},
        {"latest_frame_age_ms", item.latest_frame_age_ms},
        {"width", item.width}, {"height", item.height}
    };
    body["inference"] = {
        {"infer_fps", item.infer_fps},
        {"last_inference_ms", item.last_inference_ms},
        {"live_persons", item.live_persons}
    };
    body["flow"] = {
        {"in", item.in_count}, {"out", item.out_count},
        {"initial_occupancy", item.initial_occupancy},
        {"occupancy", item.occupancy}
    };
    body["storage"] = {
        {"degraded", item.storage_degraded},
        {"event_queue_depth", item.event_queue_depth},
        {"snapshot_degraded", item.snapshot_degraded}
    };
    body["source"] = {
        {"profile", item.camera_profile}, {"masked_uri", item.masked_uri}
    };
    body["create_time_ms"] = item.create_time_ms;
    body["start_time_ms"] = item.start_time_ms;
    body["stop_time_ms"] = item.stop_time_ms;
    body["last_update_ms"] = item.last_update_ms;
    body["snapshot_url"] =
        "/api/v1/people-flow/" + item.session_id + "/snapshot";
    body["security_url"] =
        "/api/v1/people-flow/" + item.session_id + "/security";
    if (!item.error.empty()) body["error"] = item.error;
    if (!item.last_error.empty()) body["last_error"] = item.last_error;
    return body;
}

std::string maskedUri(
    const AppConfig& config,
    const std::string& profile_id
) {
    const auto profile = config.camera_profiles.find(profile_id);
    if (profile == config.camera_profiles.end()) return {};
    std::string uri;
    std::string error;
    if (!resolveCameraProfileUri(profile->second, uri, error)) return {};
    const std::string masked = maskRtspUri(uri);
    std::fill(uri.begin(), uri.end(), '\0');
    return masked;
}

std::vector<std::string> compatibilityAlgorithms(const AppConfig& config) {
    const std::set<std::string> supported(
        config.analysis.supported_algorithms.begin(),
        config.analysis.supported_algorithms.end());
    std::vector<std::string> algorithms;
    if (supported.count("people_flow") != 0) algorithms.push_back("people_flow");
    if (config.people_flow.security.enabled &&
        supported.count("security") != 0) {
        algorithms.push_back("security");
    }
    return algorithms;
}

PeopleFlowSessionStatus projectStatus(
    const AppConfig& config,
    const CameraTaskRunRecord& run,
    const CameraTaskRunHotStatus& hot,
    bool hot_found,
    const CameraHubHotStatus& hub,
    bool hub_found,
    const CameraRunAnalysisResultRecord& stored,
    bool stored_found
) {
    const auto definition =
        json::parse(run.definition_json, nullptr, false);
    const long long run_initial_occupancy =
        definition.is_object() &&
            definition.contains("analysis") &&
            definition["analysis"].is_object()
            ? std::max(
                0LL,
                definition["analysis"].value(
                    "initial_occupancy", 0LL))
            : 0LL;
    const bool analysis_hot =
        hot_found && hot.analysis_last_update_ms > 0;
    PeopleFlowSessionStatus item;
    item.found = true;
    item.session_id = run.legacy_session_id.empty()
        ? run.run_id : run.legacy_session_id;
    item.camera_id = run.task_id;
    item.camera_profile = run.camera_profile;
    item.masked_uri = maskedUri(config, run.camera_profile);
    item.config_version = run.analysis_config_version;
    item.status =
        hot_found && !hot.status.empty() ? hot.status : run.status;
    item.stop_requested =
        item.status == "stopping" || item.status == "stopped";
    item.capture_state = hub_found
        ? hub.snapshot.state
        : (hot_found ? hot.hub_state : run.status);
    item.capture_backend = hot_found && !hot.capture_backend.empty()
        ? hot.capture_backend
        : (hub_found ? hub.snapshot.backend_name : run.capture_backend);
    item.shared_hub = hub_found;
    item.hub_instance_id = hot_found && !hot.hub_instance_id.empty()
        ? hot.hub_instance_id
        : (hub_found ? hub.snapshot.hub_instance_id : run.hub_instance_id);
    item.hub_subscribers =
        hub_found ? hub.snapshot.subscriber_count : 0;
    item.capture_fps =
        hub_found ? hub.snapshot.capture_fps : run.capture_fps;
    item.source_fps =
        hub_found ? hub.snapshot.source_fps : run.capture_fps;
    item.frame_count = hot_found
        ? std::max(hot.analysis_frame_count, hot.consumed_frames)
        : run.consumed_frames;
    item.dropped_frames =
        hot_found ? hot.dropped_frames : run.dropped_frames;
    item.latest_frame_age_ms = hub_found
        ? hub.snapshot.latest_frame_age_ms
        : (hot_found ? hot.latest_frame_age_ms : -1);
    item.width = hub_found ? hub.snapshot.width : run.width;
    item.height = hub_found ? hub.snapshot.height : run.height;
    item.infer_fps = analysis_hot ? hot.infer_fps : 0.0;
    item.last_inference_ms =
        analysis_hot ? hot.last_inference_ms : 0.0;
    item.initial_occupancy = analysis_hot
        ? hot.initial_occupancy
        : (stored_found
            ? stored.initial_occupancy : run_initial_occupancy);
    item.in_count = analysis_hot
        ? hot.in_count : (stored_found ? stored.in_count : 0);
    item.out_count = analysis_hot
        ? hot.out_count : (stored_found ? stored.out_count : 0);
    item.occupancy = analysis_hot
        ? hot.occupancy
        : (stored_found ? stored.final_occupancy : item.initial_occupancy);
    item.live_persons = analysis_hot
        ? hot.live_persons : (stored_found ? stored.last_live_persons : 0);
    item.reconnect_count =
        analysis_hot ? hot.analysis_reconnect_count : 0;
    item.storage_degraded = analysis_hot
        ? hot.analysis_storage_degraded
        : (stored_found && stored.storage_degraded);
    item.snapshot_degraded = analysis_hot
        ? hot.analysis_snapshot_degraded
        : (stored_found && stored.snapshot_degraded);
    item.event_queue_depth = hot_found ? hot.writer_queue_depth : 0;
    item.create_time_ms = run.create_time_ms;
    item.start_time_ms = run.start_time_ms;
    item.stop_time_ms = run.stop_time_ms;
    item.last_update_ms = std::max(
        run.last_update_ms,
        hot_found
            ? std::max(hot.last_update_ms, hot.analysis_last_update_ms)
            : 0LL);
    item.error = run.status == "failed" ? run.error_message : std::string{};
    item.last_error = hot_found && !hot.error_message.empty()
        ? hot.error_message
        : (hub_found ? hub.snapshot.last_error : std::string{});
    return item;
}

}  // namespace

PeopleFlowCompatibilityController::PeopleFlowCompatibilityController(
    AppConfig config,
    std::shared_ptr<CameraTaskRepository> repository,
    std::shared_ptr<ICameraTaskApiControl> control,
    std::shared_ptr<CameraProfileRegistry> profile_registry,
    std::shared_ptr<UnifiedCameraApplicationService> application_service,
    PeopleFlowRepository* legacy_repository
) : config_(std::move(config)),
    repository_(std::move(repository)),
    control_(std::move(control)),
    profile_registry_(std::move(profile_registry)),
    application_service_(std::move(application_service)),
    legacy_repository_(legacy_repository) {
}

bool PeopleFlowCompatibilityController::findRun(
    const std::string& session_id,
    CameraTaskRunRecord& run,
    bool& found,
    std::string& error
) const {
    found = false;
    if (!repository_) {
        error = "camera task repository is unavailable";
        return false;
    }
    return repository_->getRunByLegacySessionId(
        session_id, run, found, error);
}

bool PeopleFlowCompatibilityController::ensureDefinition(
    const std::string& camera_id,
    const std::string& camera_profile,
    long long operation_time_ms,
    std::string& error_code,
    std::string& error
) {
    error_code.clear();
    CameraTaskDefinition task;
    bool found = false;
    if (!repository_->getTask(camera_id, false, task, found, error)) {
        error_code = "STORAGE_UNAVAILABLE";
        return false;
    }
    const auto algorithms = compatibilityAlgorithms(config_);
    if (algorithms.empty() || algorithms.front() != "people_flow") {
        error_code = "INVALID_SESSION_CONFIG";
        error = "people_flow is not enabled by analysis.supported_algorithms";
        return false;
    }
    const int frame_interval_ms = std::clamp(
        static_cast<int>(std::llround(
            1000.0 / std::max(0.1, static_cast<double>(
                config_.people_flow.target_infer_fps)))),
        100,
        3600000);
    const std::string algorithm_profile =
        config_.camera_tasks.defaults.algorithm_profile.empty()
            ? "people_flow_compat"
            : config_.camera_tasks.defaults.algorithm_profile;
    if (!found) {
        task.task_id = camera_id;
        task.name = "People Flow " + camera_id;
        task.camera_profile = camera_profile;
        task.enabled = false;
        task.desired_state = "stopped";
        task.frame_interval_ms = frame_interval_ms;
        task.output_mode = "latest";
        task.jpeg_quality = config_.people_flow.jpeg_quality;
        task.max_width = config_.camera_tasks.defaults.max_width;
        task.max_height = config_.camera_tasks.defaults.max_height;
        task.retention_days = config_.camera_tasks.defaults.retention_days;
        task.max_saved_frames =
            config_.camera_tasks.defaults.max_saved_frames;
        task.analysis_enabled = true;
        task.target_infer_fps = config_.people_flow.target_infer_fps;
        task.algorithm_profile = algorithm_profile;
        task.algorithms = algorithms;
        task.callback_profile =
            config_.camera_tasks.defaults.callback_profile;
        task.version = 1;
        task.created_at_ms = operation_time_ms;
        task.updated_at_ms = operation_time_ms;
        return repository_->createTask(task, error_code, error);
    }
    if (task.camera_profile != camera_profile) {
        error_code = "INVALID_SESSION_CONFIG";
        error = "camera definition uses a different camera profile";
        return false;
    }
    if (task.analysis_enabled &&
        task.target_infer_fps == config_.people_flow.target_infer_fps &&
        task.algorithm_profile == algorithm_profile &&
        task.algorithms == algorithms &&
        task.frame_interval_ms == frame_interval_ms) {
        return true;
    }
    CameraTaskPatch patch;
    patch.frame_interval_ms = frame_interval_ms;
    patch.analysis_enabled = true;
    patch.target_infer_fps = config_.people_flow.target_infer_fps;
    patch.algorithm_profile = algorithm_profile;
    patch.algorithms = algorithms;
    CameraTaskDefinition updated;
    return repository_->updateTask(
        camera_id,
        task.version,
        patch,
        operation_time_ms,
        updated,
        error_code,
        error);
}

crow::response PeopleFlowCompatibilityController::start(
    const crow::request& request
) {
    const auto max_bytes = static_cast<std::size_t>(
        std::max(1, config_.server.max_body_size_mb)) * 1024U * 1024U;
    if (request.body.size() > max_bytes) {
        return jsonResponse(413, {
            {"success", false}, {"error_code", "REQUEST_TOO_LARGE"}
        });
    }
    json input = request.body.empty()
        ? json::object()
        : json::parse(request.body, nullptr, false);
    if (input.is_discarded() || !input.is_object()) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_JSON"}
        });
    }
    if (input.contains("source_uri") || input.contains("rtsp_url") ||
        input.contains("password")) {
        return jsonResponse(400, {
            {"success", false},
            {"error_code", "RTSP_URI_IN_REQUEST_FORBIDDEN"}
        });
    }
    const std::string profile_id = input.value(
        "camera_profile", config_.people_flow.camera_profile);
    const std::string camera_id = input.value(
        "camera_id", config_.people_flow.camera_id);
    const std::string version = input.value(
        "config_version", config_.people_flow.config_version);
    const long long initial = input.value(
        "initial_occupancy",
        static_cast<long long>(config_.people_flow.initial_occupancy));
    if (!safeIdentifier(profile_id) ||
        camera_id != config_.people_flow.camera_id ||
        profile_id != config_.people_flow.camera_profile ||
        version != config_.people_flow.config_version ||
        initial < 0 || initial > 1000000000LL) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_SESSION_CONFIG"}
        });
    }
    const auto profile = config_.camera_profiles.find(profile_id);
    if (profile == config_.camera_profiles.end()) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "CAMERA_PROFILE_NOT_FOUND"}
        });
    }
    std::string uri;
    std::string error;
    if (!resolveCameraProfileUri(profile->second, uri, error) ||
        !isRtspUri(uri)) {
        std::fill(uri.begin(), uri.end(), '\0');
        return jsonResponse(503, {
            {"success", false},
            {"error_code", "CAMERA_PROFILE_UNAVAILABLE"},
            {"error", error.empty()
                ? "camera URI must use rtsp:// or rtsps://" : error}
        });
    }
    std::fill(uri.begin(), uri.end(), '\0');
    if (!application_service_ || !repository_ || !control_) {
        return jsonResponse(500, {
            {"success", false}, {"error_code", "REDIS_ERROR"},
            {"error", "unified camera control is unavailable"}
        });
    }

    auto lifecycle_lock = application_service_->lockLifecycle();
    CameraTaskRunRecord active;
    bool active_found = false;
    if (!application_service_->getActiveRun(
            camera_id, active, active_found, error, false)) {
        return jsonResponse(500, {
            {"success", false}, {"error_code", "REDIS_ERROR"},
            {"error", error}
        });
    }
    if (active_found) {
        return jsonResponse(409, {
            {"success", false}, {"error_code", "CAMERA_ALREADY_ACTIVE"},
            {"error", "CAMERA_ALREADY_ACTIVE"}
        });
    }
    const long long operation_time_ms = nowMs();
    std::string error_code;
    if (!ensureDefinition(
            camera_id,
            profile_id,
            operation_time_ms,
            error_code,
            error)) {
        const bool invalid = error_code == "INVALID_SESSION_CONFIG";
        return jsonResponse(invalid ? 400 : 500, {
            {"success", false},
            {"error_code", invalid
                ? "INVALID_SESSION_CONFIG" : "REDIS_ERROR"},
            {"error", error}
        });
    }
    const std::string session_id = makeSessionId();
    std::error_code directory_error;
    std::filesystem::create_directories(
        std::filesystem::u8path(config_.camera_tasks.output_dir) /
            camera_id / session_id / "analysis",
        directory_error);
    if (directory_error) {
        return jsonResponse(500, {
            {"success", false},
            {"error_code", "OUTPUT_DIRECTORY_FAILED"}
        });
    }
    CameraStartApplicationOptions options;
    options.run_id = session_id;
    options.override_run_spec = true;
    options.run_spec.origin = kCameraRunOriginPeopleFlowCompat;
    options.run_spec.analysis_config_version = version;
    options.run_spec.initial_occupancy = initial;
    options.run_spec.snapshot_fps = config_.people_flow.snapshot_fps;
    options.run_spec.compatibility.legacy_session_id = session_id;
    options.run_spec.compatibility.preserve_pf_projection = true;
    options.run_spec.compatibility.legacy_response_version = 1;
    CameraStartApplicationResult result;
    if (!application_service_->startCamera(
            camera_id,
            result,
            error_code,
            error,
            operation_time_ms,
            std::move(options))) {
        return jsonResponse(500, {
            {"success", false}, {"error_code", "REDIS_ERROR"},
            {"error", error.empty() ? error_code : error}
        });
    }
    if (result.idempotent_replay) {
        return jsonResponse(409, {
            {"success", false}, {"error_code", "CAMERA_ALREADY_ACTIVE"},
            {"error", "CAMERA_ALREADY_ACTIVE"}
        });
    }
    return jsonResponse(202, {
        {"success", true}, {"session_id", session_id},
        {"camera_id", camera_id}, {"camera_profile", profile_id},
        {"config_version", version}, {"status", "queued"},
        {"status_url",
            "/api/v1/people-flow/" + session_id + "/status"},
        {"snapshot_url",
            "/api/v1/people-flow/" + session_id + "/snapshot"},
        {"security_url",
            "/api/v1/people-flow/" + session_id + "/security"},
        {"stop_url",
            "/api/v1/people-flow/" + session_id + "/stop"}
    });
}

crow::response PeopleFlowCompatibilityController::stop(
    const std::string& session_id
) {
    if (!safeIdentifier(session_id) ||
        session_id.rfind("pf_", 0) != 0) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_SESSION_ID"}
        });
    }
    CameraTaskRunRecord run;
    bool found = false;
    std::string error;
    if (!findRun(session_id, run, found, error)) {
        return jsonResponse(500, {
            {"success", false}, {"error_code", "REDIS_ERROR"},
            {"error", error}
        });
    }
    if (!found) {
        return jsonResponse(404, {
            {"success", false}, {"error_code", "SESSION_NOT_FOUND"}
        });
    }
    if (run.status == "stopped" || run.status == "failed") {
        return jsonResponse(409, {
            {"success", false},
            {"error_code", "SESSION_ALREADY_FINISHED"}
        });
    }
    CameraStopApplicationResult result;
    std::string error_code;
    if (!application_service_->stopCamera(
            run.task_id, result, error_code, error)) {
        return jsonResponse(500, {
            {"success", false}, {"error_code", "REDIS_ERROR"},
            {"error", error.empty() ? error_code : error}
        });
    }
    return jsonResponse(200, {
        {"success", true}, {"session_id", session_id},
        {"camera_id", run.task_id}, {"status", "stopping"},
        {"stop_requested", true}
    });
}

crow::response PeopleFlowCompatibilityController::status(
    const std::string& session_id
) const {
    if (!safeIdentifier(session_id)) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_SESSION_ID"}
        });
    }
    CameraTaskRunRecord run;
    bool found = false;
    std::string error;
    if (!findRun(session_id, run, found, error)) {
        return jsonResponse(500, {
            {"success", false}, {"error_code", "REDIS_ERROR"},
            {"error", error}
        });
    }
    if (!found) {
        return jsonResponse(404, {
            {"success", false}, {"error_code", "SESSION_NOT_FOUND"}
        });
    }
    CameraTaskRunHotStatus hot;
    const bool hot_found =
        control_ && control_->getRunStatus(run.run_id, hot, error) &&
        hot.found;
    CameraHubHotStatus hub;
    const bool hub_found =
        control_ && control_->getHubStatus(run.camera_profile, hub, error) &&
        hub.found;
    CameraRunAnalysisResultRecord stored;
    bool stored_found = false;
    if (!repository_->getRunAnalysisResult(
            run.run_id, stored, stored_found, error)) {
        return jsonResponse(500, {
            {"success", false}, {"error_code", "REDIS_ERROR"},
            {"error", error}
        });
    }
    return jsonResponse(200, sessionJson(projectStatus(
        config_, run, hot, hot_found, hub, hub_found,
        stored, stored_found)));
}

crow::response PeopleFlowCompatibilityController::snapshot(
    const std::string& session_id
) const {
    if (!safeIdentifier(session_id)) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_SESSION"}
        });
    }
    CameraTaskRunRecord run;
    bool found = false;
    std::string error;
    if (!findRun(session_id, run, found, error)) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_SESSION"}
        });
    }
    if (!found) {
        return jsonResponse(404, {
            {"success", false}, {"error_code", "SESSION_NOT_FOUND"}
        });
    }
    CameraTaskRunHotStatus hot;
    const bool hot_found =
        control_ && control_->getRunStatus(run.run_id, hot, error) &&
        hot.found;
    CameraRunAnalysisResultRecord stored;
    bool stored_found = false;
    if (!repository_->getRunAnalysisResult(
            run.run_id, stored, stored_found, error)) {
        stored_found = false;
    }
    const std::string relative =
        hot_found && !hot.analysis_snapshot_relative_path.empty()
            ? hot.analysis_snapshot_relative_path
            : (stored_found ? stored.snapshot_relative_path : std::string{});
    std::filesystem::path resolved;
    std::string bytes;
    if (!resolveArtifact(
            config_.camera_tasks.output_dir, relative, resolved) ||
        !readFile(resolved, bytes)) {
        return jsonResponse(404, {
            {"success", false}, {"error_code", "SNAPSHOT_NOT_READY"}
        });
    }
    crow::response response(200, std::move(bytes));
    response.set_header("Content-Type", "image/jpeg");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response PeopleFlowCompatibilityController::security(
    const std::string& session_id
) const {
    if (!safeIdentifier(session_id)) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_SESSION"}
        });
    }
    CameraTaskRunRecord run;
    bool found = false;
    std::string error;
    if (!findRun(session_id, run, found, error)) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_SESSION"}
        });
    }
    if (!found) {
        return jsonResponse(404, {
            {"success", false}, {"error_code", "SESSION_NOT_FOUND"}
        });
    }
    CameraTaskRunHotStatus hot;
    const bool hot_found =
        control_ && control_->getRunStatus(run.run_id, hot, error) &&
        hot.found;
    CameraRunAnalysisResultRecord stored;
    bool stored_found = false;
    if (!repository_->getRunAnalysisResult(
            run.run_id, stored, stored_found, error)) {
        stored_found = false;
    }
    const std::string value =
        hot_found && hot.security_state_json != "{}"
            ? hot.security_state_json
            : (stored_found ? stored.security_state_json : std::string{});
    const auto parsed = json::parse(value, nullptr, false);
    if (!parsed.is_object() || parsed.empty()) {
        return jsonResponse(404, {
            {"success", false},
            {"error_code", "SECURITY_STATE_NOT_READY"}
        });
    }
    crow::response response(200, parsed.dump());
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response PeopleFlowCompatibilityController::realtime(
    const std::string& camera_id
) const {
    if (!safeIdentifier(camera_id)) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_CAMERA_ID"}
        });
    }
    CameraTaskRunRecord run;
    bool found = false;
    std::string error;
    if (!application_service_->getActiveRun(
            camera_id, run, found, error, true)) {
        return jsonResponse(500, {
            {"success", false}, {"error_code", "REDIS_ERROR"},
            {"error", error}
        });
    }
    if (!found || run.origin != kCameraRunOriginPeopleFlowCompat) {
        return jsonResponse(404, {
            {"success", false}, {"error_code", "CAMERA_NOT_FOUND"}
        });
    }
    CameraTaskRunHotStatus hot;
    if (!control_ || !control_->getRunStatus(run.run_id, hot, error) ||
        !hot.found || hot.analysis_config_version.empty()) {
        return jsonResponse(404, {
            {"success", false}, {"error_code", "CAMERA_NOT_FOUND"}
        });
    }
    CameraHubHotStatus hub;
    const bool hub_found =
        control_->getHubStatus(run.camera_profile, hub, error) && hub.found;
    return jsonResponse(200, {
        {"success", true}, {"camera_id", camera_id},
        {"session_id", run.legacy_session_id},
        {"status", run.status}, {"in", hot.in_count},
        {"out", hot.out_count}, {"occupancy", hot.occupancy},
        {"live_persons", hot.live_persons},
        {"frame_count",
            std::max(hot.analysis_frame_count, hot.consumed_frames)},
        {"capture_fps", hub_found
            ? hub.snapshot.capture_fps : hot.sample_fps},
        {"infer_fps", hot.infer_fps},
        {"last_inference_ms", hot.last_inference_ms},
        {"latest_frame_age_ms", hub_found
            ? hub.snapshot.latest_frame_age_ms
            : hot.latest_frame_age_ms}
    });
}

crow::response PeopleFlowCompatibilityController::events(
    const crow::request& request,
    const std::string& camera_id
) const {
    if (!safeIdentifier(camera_id) || !repository_) {
        return jsonResponse(400, {
            {"success", false}, {"error_code", "INVALID_CAMERA_ID"}
        });
    }
    int limit = 100;
    if (const char* raw = request.url_params.get("limit")) {
        try {
            limit = std::clamp(std::stoi(raw), 1, 1000);
        }
        catch (...) {
            return jsonResponse(400, {
                {"success", false}, {"error_code", "INVALID_LIMIT"}
            });
        }
    }
    std::vector<CrossingEvent> merged;
    std::string error;
    std::vector<SecurityAlertEventRecord> alerts;
    for (const char* event_type : {
            "PEOPLE_FLOW_IN", "PEOPLE_FLOW_OUT"}) {
        std::vector<SecurityAlertEventRecord> direction_alerts;
        if (!repository_->listAlerts(
                camera_id,
                event_type,
                1,
                limit,
                0,
                direction_alerts,
                error)) {
            return jsonResponse(503, {
                {"success", false},
                {"error_code", "STORAGE_QUERY_FAILED"},
                {"error", error}
            });
        }
        alerts.insert(
            alerts.end(),
            std::make_move_iterator(direction_alerts.begin()),
            std::make_move_iterator(direction_alerts.end()));
    }
    std::map<std::string, CameraTaskRunRecord> runs;
    for (const auto& alert : alerts) {
        if (alert.category != "people_flow" ||
            (alert.event_type != "PEOPLE_FLOW_IN" &&
                alert.event_type != "PEOPLE_FLOW_OUT")) {
            continue;
        }
        auto run = runs.find(alert.run_id);
        if (run == runs.end()) {
            CameraTaskRunRecord value;
            bool found = false;
            if (!repository_->getRun(
                    alert.run_id, value, found, error) || !found ||
                value.legacy_session_id.empty()) {
                continue;
            }
            run = runs.emplace(alert.run_id, std::move(value)).first;
        }
        const auto payload =
            json::parse(alert.payload_json, nullptr, false);
        CrossingEvent event;
        event.event_id = alert.event_id;
        event.session_id = run->second.legacy_session_id;
        event.camera_id = camera_id;
        event.line_id = payload.is_object()
            ? payload.value("line_id", config_.people_flow.counting.line_id)
            : config_.people_flow.counting.line_id;
        event.track_id = alert.track_id.value_or(0);
        event.direction = alert.event_type == "PEOPLE_FLOW_IN"
            ? "IN" : "OUT";
        event.event_time_ms = alert.occurred_at_ms;
        event.confidence = alert.confidence.value_or(0.0);
        event.point_x_norm = payload.is_object()
            ? payload.value("point_x_norm", 0.0) : 0.0;
        event.point_y_norm = payload.is_object()
            ? payload.value("point_y_norm", 0.0) : 0.0;
        event.config_version = alert.config_version;
        merged.push_back(std::move(event));
    }
    if (config_.runtime.legacy_people_flow_fallback &&
        legacy_repository_) {
        std::vector<CrossingEvent> legacy;
        if (!legacy_repository_->queryEvents(
                camera_id,
                "",
                0,
                std::numeric_limits<long long>::max(),
                limit,
                0,
                legacy,
                error)) {
            return jsonResponse(503, {
                {"success", false},
                {"error_code", "STORAGE_QUERY_FAILED"},
                {"error", error}
            });
        }
        merged.insert(
            merged.end(),
            std::make_move_iterator(legacy.begin()),
            std::make_move_iterator(legacy.end()));
    }
    std::sort(
        merged.begin(),
        merged.end(),
        [](const CrossingEvent& left, const CrossingEvent& right) {
            if (left.event_time_ms != right.event_time_ms) {
                return left.event_time_ms > right.event_time_ms;
            }
            return left.event_id > right.event_id;
        });
    std::set<std::string> seen;
    json items = json::array();
    for (const auto& event : merged) {
        if (!seen.insert(event.event_id).second) continue;
        items.push_back({
            {"event_id", event.event_id},
            {"session_id", event.session_id},
            {"camera_id", event.camera_id},
            {"line_id", event.line_id},
            {"track_id", event.track_id},
            {"direction", event.direction},
            {"event_time_ms", event.event_time_ms},
            {"confidence", event.confidence},
            {"point_x_norm", event.point_x_norm},
            {"point_y_norm", event.point_y_norm}
        });
        if (static_cast<int>(items.size()) >= limit) break;
    }
    return jsonResponse(200, {
        {"success", true}, {"camera_id", camera_id},
        {"count", items.size()}, {"events", items}
    });
}

}  // namespace yolo11_server
