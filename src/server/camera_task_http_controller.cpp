#include "server/camera_task_http_controller.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef DELETE
#undef DELETE
#endif
#ifdef POST
#undef POST
#endif
#ifdef PATCH
#undef PATCH
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include "server/camera_task_queue.h"

namespace yolo11_server {

namespace {

using json = nlohmann::json;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string makeId(const char* prefix) {
    static std::atomic<unsigned long long> sequence{ 0 };
    std::ostringstream output;
    output << prefix << std::hex << nowMs() << '_' << ++sequence;
    return output.str();
}

crow::response jsonResponse(int status, json body) {
    crow::response response(status, body.dump());
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
    return response;
}

std::string publicError(const std::string& code) {
    if (code == "UNAUTHORIZED") return "valid Bearer authentication is required";
    if (code == "INVALID_JSON") return "request body must be a valid JSON object";
    if (code == "UNKNOWN_FIELD") return "request contains an unknown field";
    if (code == "INVALID_TASK_CONFIG") return "camera extraction configuration is invalid";
    if (code == "RTSP_URI_IN_REQUEST_FORBIDDEN") return "RTSP URI and credentials are forbidden";
    if (code == "CAMERA_PROFILE_NOT_FOUND") return "camera profile was not found";
    if (code == "CAMERA_PROFILE_DISABLED") return "camera profile is disabled";
    if (code == "INVALID_CAMERA_PROFILE") return "camera profile metadata is invalid";
    if (code == "CAMERA_PROFILE_ALREADY_EXISTS") return "camera profile already exists";
    if (code == "CAMERA_PROFILE_VERSION_CONFLICT") return "camera profile version does not match";
    if (code == "CAMERA_PROFILE_IN_USE") return "camera profile is referenced by a camera";
    if (code == "TASK_NOT_FOUND") return "camera was not found";
    if (code == "TASK_DISABLED") return "camera extraction is disabled";
    if (code == "TASK_ACTIVE") return "camera has an active extraction thread";
    if (code == "TASK_VERSION_CONFLICT") return "camera version does not match";
    if (code == "PRECONDITION_REQUIRED") return "If-Match is required";
    if (code == "LATEST_OUTPUT_DISABLED") return "latest output is disabled for this camera";
    if (code == "FRAME_NOT_READY") return "latest frame is not ready";
    if (code == "QUEUE_SUBMIT_FAILED") return "camera extraction command queue is unavailable";
    if (code == "STORAGE_PRESSURE") return "camera archive storage is under critical pressure";
    if (code == "RUN_NOT_FOUND") return "camera has no extraction run history";
    if (code == "INVALID_IDENTIFIER") return "identifier is invalid";
    if (code == "REQUEST_TOO_LARGE") return "request body is too large";
    if (code == "INVALID_IDEMPOTENCY_KEY") return "Idempotency-Key is invalid";
    if (code == "IDEMPOTENCY_CONFLICT") return "Idempotency-Key was already used for a different request";
    if (code == "ALERT_NOT_FOUND") return "alert was not found";
    return "camera request failed";
}

crow::response errorResponse(int status, const std::string& code, const std::string& request_id) {
    return jsonResponse(status, {
        {"success", false}, {"error_code", code}, {"error", publicError(code)},
        {"request_id", request_id}
    });
}

bool safeIdentifier(const std::string& value, const std::string& prefix = {}) {
    if (value.empty() || value.size() > 160 || (!prefix.empty() && value.rfind(prefix, 0) != 0)) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

bool constantTimeEqual(const std::string& left, const std::string& right) {
    const std::size_t count = std::max(left.size(), right.size());
    unsigned int difference = static_cast<unsigned int>(left.size() ^ right.size());
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char a = index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
        const unsigned char b = index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
        difference |= a ^ b;
    }
    return difference == 0;
}

bool utf8Length(const std::string& value, std::size_t& count) {
    count = 0;
    for (std::size_t index = 0; index < value.size();) {
        const unsigned char first = static_cast<unsigned char>(value[index]);
        std::size_t width = 0;
        unsigned int codepoint = 0;
        if (first <= 0x7f) { width = 1; codepoint = first; }
        else if (first >= 0xc2 && first <= 0xdf) { width = 2; codepoint = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { width = 3; codepoint = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { width = 4; codepoint = first & 0x07; }
        else return false;
        if (index + width > value.size()) return false;
        for (std::size_t offset = 1; offset < width; ++offset) {
            const unsigned char next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((width == 2 && codepoint < 0x80) ||
            (width == 3 && codepoint < 0x800) ||
            (width == 4 && codepoint < 0x10000) ||
            (width == 3 && codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            (width == 4 && codepoint > 0x10ffff)) return false;
        ++count;
        index += width;
    }
    return true;
}

bool containsForbiddenSecretField(const json& input) {
    static const std::set<std::string> forbidden{
        "source_uri", "rtsp_url", "uri", "url", "url_env", "username", "password", "credential"
    };
    if (input.is_array()) {
        for (const auto& item : input) if (containsForbiddenSecretField(item)) return true;
        return false;
    }
    if (!input.is_object()) return false;
    for (const auto& entry : input.items()) {
        if (forbidden.count(entry.key()) != 0 || containsForbiddenSecretField(entry.value())) return true;
    }
    return false;
}

bool containsForbiddenProfileSecretField(const json& input) {
    static const std::set<std::string> forbidden{
        "source_uri", "rtsp_url", "uri", "url", "username", "password", "credential", "secret"
    };
    for (const auto& entry : input.items()) if (forbidden.count(entry.key()) != 0) return true;
    return false;
}

bool onlyFields(const json& input, const std::set<std::string>& allowed) {
    for (const auto& entry : input.items()) if (allowed.count(entry.key()) == 0) return false;
    return true;
}

bool validDimension(int value) {
    return value == 0 || (value >= 64 && value <= 8192);
}

bool safeServiceIdentifier(const std::string& value, std::size_t maximum, bool allow_empty = false) {
    if (value.empty()) return allow_empty;
    if (value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
    });
}

bool validTaskDefinition(
    const CameraTaskDefinition& task,
    const AnalysisSection& analysis_config
) {
    std::size_t name_length = 0;
    if (!(utf8Length(task.name, name_length) && name_length >= 1 && name_length <= 128 &&
        safeIdentifier(task.camera_profile) && task.frame_interval_ms >= 100 &&
        task.frame_interval_ms <= 3600000 &&
        (task.output_mode == "latest" || task.output_mode == "archive" || task.output_mode == "both") &&
        task.jpeg_quality >= 1 && task.jpeg_quality <= 100 && validDimension(task.max_width) &&
        validDimension(task.max_height) && task.retention_days >= 1 && task.retention_days <= 3650 &&
        task.max_saved_frames >= 1 && task.max_saved_frames <= 1000000 &&
        (task.desired_state == "running" || task.desired_state == "stopped") &&
        task.enabled == (task.desired_state == "running") &&
        task.target_infer_fps >= 0.1 && task.target_infer_fps <= 120.0 &&
        safeServiceIdentifier(task.algorithm_profile, 160, !task.analysis_enabled) &&
        safeServiceIdentifier(task.callback_profile, 160, true) &&
        (!task.analysis_enabled || (analysis_config.enabled && !task.algorithms.empty())) &&
        task.algorithms.size() <= 32)) {
        return false;
    }
    const std::set<std::string> supported(
        analysis_config.supported_algorithms.begin(),
        analysis_config.supported_algorithms.end());
    std::set<std::string> unique;
    for (const auto& algorithm : task.algorithms) {
        if (!safeServiceIdentifier(algorithm, 80) ||
            !unique.insert(algorithm).second ||
            (task.analysis_enabled && supported.count(algorithm) == 0)) return false;
    }
    return true;
}

bool validIdempotencyKey(const std::string& value) {
    if (value.empty() || value.size() > 160) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x21 && ch <= 0x7e && ch != '"' && ch != '\\';
    });
}

std::string requestDigest(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : digest) output << std::setw(2) << static_cast<int>(byte);
    return output.str();
}

void storeIdempotency(
    const std::shared_ptr<CameraTaskRepository>& repository,
    const std::string& scope,
    const std::string& key,
    const std::string& digest,
    const std::string& resource_id,
    const crow::response& response
) {
    if (!repository || key.empty() || response.body.empty()) return;
    CameraIdempotencyRecord record;
    record.operation_scope = scope;
    record.idempotency_key = key;
    record.request_digest = digest;
    record.resource_id = resource_id;
    record.response_status = response.code;
    record.response_json = response.body;
    record.created_at_ms = nowMs();
    record.expires_at_ms = record.created_at_ms + 24LL * 60LL * 60LL * 1000LL;
    std::string ignored_code;
    std::string ignored_error;
    repository->storeIdempotencyRecord(record, ignored_code, ignored_error);
}

crow::response replayIdempotency(
    const std::shared_ptr<CameraTaskRepository>& repository,
    const std::string& scope,
    const std::string& key,
    const std::string& digest,
    const std::string& request_id,
    bool& handled
) {
    handled = false;
    if (key.empty()) return {};
    if (!validIdempotencyKey(key)) {
        handled = true;
        return errorResponse(400, "INVALID_IDEMPOTENCY_KEY", request_id);
    }
    CameraIdempotencyRecord record;
    bool found = false;
    std::string error;
    if (!repository->getIdempotencyRecord(scope, key, record, found, error)) {
        handled = true;
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!found) return {};
    handled = true;
    if (record.request_digest != digest) {
        return errorResponse(409, "IDEMPOTENCY_CONFLICT", request_id);
    }
    crow::response response(record.response_status, record.response_json);
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Idempotent-Replay", "true");
    CameraTaskDefinition task;
    bool task_found = false;
    if (repository->getTask(record.resource_id, false, task, task_found, error) && task_found) {
        response.set_header("ETag", "\"" + std::to_string(task.version) + "\"");
    }
    return response;
}

bool parseIfMatch(const crow::request& request, int& version) {
    std::string value = request.get_header_value("If-Match");
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    try {
        std::size_t consumed = 0;
        version = std::stoi(value, &consumed);
        return consumed == value.size() && version > 0;
    }
    catch (...) {
        return false;
    }
}

int queryInt(const crow::request& request, const char* name, int fallback, int minimum, int maximum) {
    const char* value = request.url_params.get(name);
    if (!value) return fallback;
    try { return std::clamp(std::stoi(value), minimum, maximum); }
    catch (...) { return fallback; }
}

bool queryBool(const crow::request& request, const char* name, bool fallback, bool& present) {
    const char* value = request.url_params.get(name);
    present = value != nullptr;
    if (!value) return fallback;
    return std::string(value) == "true" || (std::string(value) != "false" && fallback);
}

json taskJson(const CameraTaskDefinition& task) {
    return {
        {"camera_id", task.task_id}, {"name", task.name}, {"camera_profile", task.camera_profile},
        {"enabled", task.enabled}, {"frame_interval_ms", task.frame_interval_ms},
        {"output_mode", task.output_mode}, {"jpeg_quality", task.jpeg_quality},
        {"max_width", task.max_width}, {"max_height", task.max_height},
        {"retention_days", task.retention_days}, {"max_saved_frames", task.max_saved_frames},
        {"desired_state", task.desired_state},
        {"analysis", {
            {"enabled", task.analysis_enabled}, {"target_infer_fps", task.target_infer_fps},
            {"algorithm_profile", task.algorithm_profile}, {"algorithms", task.algorithms}
        }},
        {"callback_profile", task.callback_profile},
        {"version", task.version}, {"created_at_ms", task.created_at_ms},
        {"updated_at_ms", task.updated_at_ms},
        {"deleted_at_ms", task.deleted_at_ms ? json(*task.deleted_at_ms) : json(nullptr)}
    };
}

json alertJson(const SecurityAlertEventRecord& alert) {
    json payload = json::parse(alert.payload_json, nullptr, false);
    if (payload.is_discarded()) payload = json::object();
    json evidence = json::object();
    if (!alert.evidence_frame_id.empty()) evidence["frame_id"] = alert.evidence_frame_id;
    return {
        {"schema_version", "1.0"}, {"event_id", alert.event_id},
        {"event_kind", "algorithm_alert"}, {"task_id", alert.task_id},
        {"camera_id", alert.task_id}, {"run_id", alert.run_id},
        {"camera_profile", alert.camera_profile}, {"event_type", alert.event_type},
        {"category", alert.category}, {"severity", alert.severity},
        {"confidence", alert.confidence ? json(*alert.confidence) : json(nullptr)},
        {"track_id", alert.track_id ? json(*alert.track_id) : json(nullptr)},
        {"occurred_at_ms", alert.occurred_at_ms},
        {"algorithm", {
            {"profile", alert.algorithm_profile}, {"model", alert.model_name},
            {"config_version", alert.config_version}, {"demo_classifier", alert.demo_classifier}
        }},
        {"evidence", evidence}, {"payload", payload},
        {"delivery", {{"status", alert.delivery_status}}},
        {"created_at_ms", alert.created_at_ms}
    };
}

json profileJson(const CameraProfile& profile) {
    return {
        {"profile_id", profile.id}, {"source_type", profile.source_type},
        {"display_name", profile.display_name}, {"url_env", profile.url_env},
        {"transport", profile.transport}, {"enabled", profile.enabled},
        {"version", profile.version}, {"created_at_ms", profile.created_at_ms},
        {"updated_at_ms", profile.updated_at_ms},
        {"deleted_at_ms", profile.deleted_at_ms == 0 ? json(nullptr) : json(profile.deleted_at_ms)}
    };
}

json runJson(const CameraTaskRunRecord& run) {
    return {
        {"run_id", run.run_id}, {"camera_id", run.task_id},
        {"definition_version", run.definition_version}, {"status", run.status},
        {"camera_profile", run.camera_profile}, {"hub_instance_id", run.hub_instance_id},
        {"create_time_ms", run.create_time_ms}, {"start_time_ms", run.start_time_ms},
        {"stop_time_ms", run.stop_time_ms}, {"last_update_ms", run.last_update_ms},
        {"capture_backend", run.capture_backend}, {"capture_fps", run.capture_fps},
        {"save_fps", run.save_fps}, {"consumed_frames", run.consumed_frames},
        {"saved_frames", run.saved_frames}, {"skipped_frames", run.skipped_frames},
        {"dropped_frames", run.dropped_frames}, {"last_source_sequence", run.last_source_sequence},
        {"last_frame_time_ms", run.last_frame_time_ms}, {"width", run.width}, {"height", run.height},
        {"stop_reason", run.stop_reason}, {"error_code", run.error_code},
        {"error_message", run.error_message.empty() ? "" : publicError(run.error_code)}
    };
}

std::string sanitizedDiagnostic(std::string value) {
    if (value.find("://") != std::string::npos || value.find('@') != std::string::npos ||
        value.find("password") != std::string::npos || value.find("Password") != std::string::npos) {
        return "camera capture error";
    }
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    if (value.size() > 256) value.resize(256);
    return value;
}

json hubJson(const CameraHubHotStatus& hot, int stale_after_ms) {
    const auto& hub = hot.snapshot;
    const bool stale = !hot.found || hot.last_update_ms <= 0 || nowMs() - hot.last_update_ms > stale_after_ms;
    return {
        {"hub_instance_id", hub.hub_instance_id}, {"camera_profile", hub.camera_profile},
        {"state", hub.state}, {"backend", hub.backend_name}, {"open_count", hub.open_count},
        {"subscriber_count", hub.subscriber_count}, {"subscriber_types", hub.subscriber_types},
        {"capture_fps", hub.capture_fps}, {"source_fps", hub.source_fps},
        {"latest_sequence", hub.latest_sequence}, {"latest_frame_age_ms", hub.latest_frame_age_ms},
        {"reconnect_count", hub.reconnect_count}, {"width", hub.width}, {"height", hub.height},
        {"resolution_changed", hub.resolution_changed},
        {"resolution_change_count", hub.resolution_change_count},
        {"last_error", sanitizedDiagnostic(hub.last_error)},
        {"last_update_ms", hot.last_update_ms}, {"runtime_stale", stale}
    };
}

bool isReparsePoint(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
#endif
}

}  // namespace

CameraTaskHttpController::CameraTaskHttpController(
    const AppConfig& config,
    std::shared_ptr<CameraTaskRepository> repository,
    std::shared_ptr<ICameraTaskApiControl> control,
    std::string token_override,
    std::shared_ptr<CameraProfileRegistry> profile_registry
) : config_(config), repository_(std::move(repository)), control_(std::move(control)),
    profile_registry_(std::move(profile_registry)), token_(std::move(token_override)) {
}

bool CameraTaskHttpController::initialize(std::string& error) {
    error.clear();
    if (!config_.camera_tasks.enabled) return true;
    if (token_.empty()) {
        const char* value = std::getenv(config_.camera_tasks.admin_token_env.c_str());
        if (value) token_ = value;
    }
    if (!repository_) repository_ = std::make_shared<CameraTaskRepository>(config_.camera_tasks);
    storage_ok_ = repository_->initialize(error);
    if (!storage_ok_) return false;
    if (!profile_registry_ && !config_.stream.camera_profiles_path.empty()) {
        profile_registry_ = std::make_shared<CameraProfileRegistry>(config_.stream.camera_profiles_path);
    }
    if (profile_registry_ && !profile_registry_->initialize(error)) return false;
    if (!control_) {
        control_ = std::make_shared<CameraTaskQueue>(
            config_.redis, config_.camera_tasks, "camera_task_http");
    }
    std::error_code fs_error;
    const auto root = std::filesystem::absolute(std::filesystem::u8path(config_.camera_tasks.output_dir), fs_error);
    if (!fs_error) std::filesystem::create_directories(root, fs_error);
    if (!fs_error && !isReparsePoint(root)) {
        const auto probe = root / ".camera_task_write_probe";
        {
            std::ofstream output(probe, std::ios::binary | std::ios::trunc);
            output << "probe";
            output_root_writable_ = static_cast<bool>(output);
        }
        std::filesystem::remove(probe, fs_error);
    }
    if (!output_root_writable_) {
        error = "camera task output root is not writable";
        return false;
    }
    initialized_ = true;
    return true;
}

void CameraTaskHttpController::registerRoutes(crow::SimpleApp& app) {
    if (!config_.camera_tasks.enabled) return;
    CROW_ROUTE(app, "/api/v1/cameras").methods(crow::HTTPMethod::POST)(
        [this](const crow::request& request) { return createTask(request); });
    CROW_ROUTE(app, "/api/v1/cameras")(
        [this](const crow::request& request) { return listTasks(request); });
    CROW_ROUTE(app, "/api/v1/cameras/<string>")(
        [this](const crow::request& request, const std::string& id) { return getTask(request, id); });
    CROW_ROUTE(app, "/api/v1/cameras/<string>").methods(crow::HTTPMethod::PATCH)(
        [this](const crow::request& request, const std::string& id) { return updateTask(request, id); });
    CROW_ROUTE(app, "/api/v1/cameras/<string>").methods(crow::HTTPMethod::DELETE)(
        [this](const crow::request& request, const std::string& id) { return deleteTask(request, id); });
    CROW_ROUTE(app, "/api/v1/cameras/<string>/start").methods(crow::HTTPMethod::POST)(
        [this](const crow::request& request, const std::string& id) { return startTask(request, id); });
    CROW_ROUTE(app, "/api/v1/cameras/<string>/stop").methods(crow::HTTPMethod::POST)(
        [this](const crow::request& request, const std::string& id) { return stopTask(request, id); });
    CROW_ROUTE(app, "/api/v1/cameras/<string>/status")(
        [this](const crow::request& request, const std::string& id) { return taskStatus(request, id); });
    CROW_ROUTE(app, "/api/v1/cameras/<string>/latest-frame")(
        [this](const crow::request& request, const std::string& id) { return latestFrame(request, id); });
    CROW_ROUTE(app, "/api/v1/cameras/<string>/runs")(
        [this](const crow::request& request, const std::string& id) { return listRuns(request, id); });
    CROW_ROUTE(app, "/api/v1/cameras/<string>/alerts")(
        [this](const crow::request& request, const std::string& id) { return listAlerts(request, id); });
    CROW_ROUTE(app, "/api/v1/camera-hubs")(
        [this](const crow::request& request) { return listHubs(request); });
    CROW_ROUTE(app, "/api/v1/camera-hubs/<string>")(
        [this](const crow::request& request, const std::string& id) { return getHub(request, id); });
    CROW_ROUTE(app, "/api/v1/camera-profiles")(
        [this](const crow::request& request) { return listProfiles(request); });
    CROW_ROUTE(app, "/api/v1/camera-profiles/<string>")(
        [this](const crow::request& request, const std::string& id) { return getProfile(request, id); });
    CROW_ROUTE(app, "/api/v1/operations/metrics")(
        [this](const crow::request& request) { return operationsMetrics(request); });
    CROW_ROUTE(app, "/api/v1/operations/metrics/prometheus")(
        [this](const crow::request& request) { return prometheusMetrics(request); });
}

CameraTaskHttpHealth CameraTaskHttpController::health() const {
    CameraTaskHttpHealth result;
    result.enabled = config_.camera_tasks.enabled;
    result.initialized = initialized_;
    result.token_configured = !token_.empty();
    result.output_root_writable = output_root_writable_;
    result.worker_num_valid = config_.worker.worker_num == 1;
    if (repository_) {
        std::vector<CameraTaskDefinition> ignored;
        std::string error;
        result.storage_ok = repository_->listTasks(false, 1, 0, ignored, error);
    }
    return result;
}

bool CameraTaskHttpController::authorized(const crow::request& request) const {
    const std::string header = request.get_header_value("Authorization");
    static const std::string prefix = "Bearer ";
    if (token_.empty() || header.rfind(prefix, 0) != 0) return false;
    return constantTimeEqual(header.substr(prefix.size()), token_);
}

bool CameraTaskHttpController::getActiveRun(
    const std::string& task_id,
    CameraTaskRunRecord& run,
    bool& found,
    std::string& error,
    bool include_stopping
) const {
    found = false;
    std::vector<CameraTaskRunRecord> runs;
    if (!repository_->listRuns(task_id, 100, 0, runs, error)) return false;
    CameraTaskRunRecord stopping;
    bool stopping_found = false;
    for (const auto& candidate : runs) {
        if (!isCameraRunActive(candidate.status)) continue;
        if (candidate.status != "stopping") {
            run = candidate;
            found = true;
            return true;
        }
        if (include_stopping && !stopping_found) {
            stopping = candidate;
            stopping_found = true;
        }
    }
    if (stopping_found) {
        run = stopping;
        found = true;
    }
    return true;
}

crow::response CameraTaskHttpController::createTask(const crow::request& request) {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    const std::size_t max_bytes = static_cast<std::size_t>(
        std::max(1, config_.server.max_body_size_mb)) * 1024U * 1024U;
    if (request.body.size() > max_bytes) return errorResponse(413, "REQUEST_TOO_LARGE", request_id);
    const json input = json::parse(request.body, nullptr, false);
    if (input.is_discarded() || !input.is_object()) return errorResponse(400, "INVALID_JSON", request_id);
    if (containsForbiddenSecretField(input)) {
        return errorResponse(400, "RTSP_URI_IN_REQUEST_FORBIDDEN", request_id);
    }
    const std::string idempotency_key = request.get_header_value("Idempotency-Key");
    const std::string idempotency_scope = "camera.create";
    const std::string idempotency_digest = requestDigest(input.dump());
    bool idempotency_handled = false;
    auto idempotency_response = replayIdempotency(
        repository_, idempotency_scope, idempotency_key, idempotency_digest,
        request_id, idempotency_handled);
    if (idempotency_handled) return idempotency_response;
    static const std::set<std::string> allowed{
        "camera_id", "name", "camera_profile", "enabled", "frame_interval_ms", "output_mode", "jpeg_quality",
        "max_width", "max_height", "retention_days", "max_saved_frames", "desired_state", "analysis",
        "callback_profile"
    };
    if (!onlyFields(input, allowed)) return errorResponse(400, "UNKNOWN_FIELD", request_id);
    if (!input.contains("camera_id") || !input["camera_id"].is_string() ||
        !input.contains("name") || !input["name"].is_string() ||
        !input.contains("camera_profile") || !input["camera_profile"].is_string()) {
        return errorResponse(400, "INVALID_TASK_CONFIG", request_id);
    }
    CameraTaskDefinition task;
    try {
        task.task_id = input["camera_id"].get<std::string>();
        task.name = input["name"].get<std::string>();
        task.camera_profile = input["camera_profile"].get<std::string>();
        task.enabled = input.value("enabled", true);
        task.frame_interval_ms = input.value("frame_interval_ms", config_.camera_tasks.defaults.frame_interval_ms);
        task.output_mode = input.value("output_mode", config_.camera_tasks.defaults.output_mode);
        task.jpeg_quality = input.value("jpeg_quality", config_.camera_tasks.defaults.jpeg_quality);
        task.max_width = input.value("max_width", config_.camera_tasks.defaults.max_width);
        task.max_height = input.value("max_height", config_.camera_tasks.defaults.max_height);
        task.retention_days = input.value("retention_days", config_.camera_tasks.defaults.retention_days);
        task.max_saved_frames = input.value("max_saved_frames", config_.camera_tasks.defaults.max_saved_frames);
        task.desired_state = input.value("desired_state", task.enabled ? std::string("running") : std::string("stopped"));
        if (input.contains("enabled") && input.contains("desired_state") &&
            task.enabled != (task.desired_state == "running")) {
            return errorResponse(400, "INVALID_TASK_CONFIG", request_id);
        }
        task.enabled = task.desired_state == "running";
        task.analysis_enabled = config_.camera_tasks.defaults.analysis_enabled;
        task.target_infer_fps = config_.camera_tasks.defaults.target_infer_fps;
        task.algorithm_profile = config_.camera_tasks.defaults.algorithm_profile;
        task.algorithms = config_.camera_tasks.defaults.algorithms;
        task.callback_profile = input.value(
            "callback_profile", config_.camera_tasks.defaults.callback_profile);
        if (input.contains("analysis")) {
            const auto& analysis = input.at("analysis");
            static const std::set<std::string> analysis_allowed{
                "enabled", "target_infer_fps", "algorithm_profile", "algorithms"
            };
            if (!analysis.is_object() || !onlyFields(analysis, analysis_allowed)) {
                return errorResponse(400, analysis.is_object() ? "UNKNOWN_FIELD" : "INVALID_TASK_CONFIG", request_id);
            }
            task.analysis_enabled = analysis.value("enabled", task.analysis_enabled);
            task.target_infer_fps = analysis.value("target_infer_fps", task.target_infer_fps);
            task.algorithm_profile = analysis.value("algorithm_profile", task.algorithm_profile);
            task.algorithms = analysis.value("algorithms", task.algorithms);
        }
    }
    catch (...) {
        return errorResponse(400, "INVALID_TASK_CONFIG", request_id);
    }
    if (!safeIdentifier(task.task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    CameraProfile resolved_profile;
    bool profile_found = false;
    std::string profile_error;
    if (profile_registry_) {
        if (!profile_registry_->get(task.camera_profile, false, resolved_profile, profile_found, profile_error)) {
            return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
        }
    }
    else {
        const auto profile = config_.camera_profiles.find(task.camera_profile);
        profile_found = profile != config_.camera_profiles.end();
        if (profile_found) resolved_profile = profile->second;
    }
    if (!profile_found) return errorResponse(400, "CAMERA_PROFILE_NOT_FOUND", request_id);
    if (!resolved_profile.enabled) return errorResponse(409, "CAMERA_PROFILE_DISABLED", request_id);
    if (!validTaskDefinition(task, config_.analysis)) {
        return errorResponse(400, "INVALID_TASK_CONFIG", request_id);
    }
    task.version = 1;
    task.created_at_ms = nowMs();
    task.updated_at_ms = task.created_at_ms;
    std::string code;
    std::string error;
    if (!repository_->createTask(task, code, error)) {
        return errorResponse(code == "TASK_ALREADY_EXISTS" ? 409 : 503,
            code.empty() ? "STORAGE_UNAVAILABLE" : code, request_id);
    }
    if (task.enabled) {
        auto response = startTask(request, task.task_id);
        response.set_header("ETag", "\"" + std::to_string(task.version) + "\"");
        if (response.code >= 200 && response.code < 300) {
            storeIdempotency(repository_, idempotency_scope, idempotency_key,
                idempotency_digest, task.task_id, response);
        }
        return response;
    }
    auto response = jsonResponse(201, {
        {"success", true}, {"request_id", request_id}, {"camera", taskJson(task)},
        {"status", "stopped"}
    });
    response.set_header("ETag", "\"" + std::to_string(task.version) + "\"");
    storeIdempotency(repository_, idempotency_scope, idempotency_key,
        idempotency_digest, task.task_id, response);
    return response;
}

crow::response CameraTaskHttpController::listTasks(const crow::request& request) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    const int limit = queryInt(request, "limit", 50, 1, 200);
    const int offset = queryInt(request, "offset", 0, 0, 1000000);
    bool include_present = false;
    const bool include_deleted = queryBool(request, "include_deleted", false, include_present);
    bool enabled_present = false;
    const bool enabled = queryBool(request, "enabled", false, enabled_present);
    const char* status_filter = request.url_params.get("status");
    std::vector<CameraTaskDefinition> tasks;
    std::string error;
    if (!repository_->listTasks(include_deleted, 1000, 0, tasks, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    json items = json::array();
    int skipped = 0;
    for (const auto& task : tasks) {
        if (enabled_present && task.enabled != enabled) continue;
        CameraTaskRunRecord active;
        bool active_found = false;
        if (!getActiveRun(task.task_id, active, active_found, error)) {
            return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
        }
        if (status_filter && (!active_found || active.status != status_filter)) continue;
        if (skipped++ < offset) continue;
        json item = taskJson(task);
        item["current_run"] = active_found ? runJson(active) : json(nullptr);
        CameraHubHotStatus hub;
        std::string ignored;
        if (control_->getHubStatus(task.camera_profile, hub, ignored) && hub.found) {
            item["hub"] = hubJson(hub, std::max(5000, config_.camera_hub.status_update_interval_ms * 3));
        }
        else item["hub"] = nullptr;
        items.push_back(std::move(item));
        if (static_cast<int>(items.size()) >= limit) break;
    }
    return jsonResponse(200, {
        {"success", true}, {"request_id", request_id}, {"items", items},
        {"limit", limit}, {"offset", offset}
    });
}

crow::response CameraTaskHttpController::getTask(
    const crow::request& request,
    const std::string& task_id
) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!safeIdentifier(task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    CameraTaskDefinition task;
    bool found = false;
    std::string error;
    if (!repository_->getTask(task_id, false, task, found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!found) return errorResponse(404, "TASK_NOT_FOUND", request_id);
    CameraTaskRunRecord active;
    bool active_found = false;
    if (!getActiveRun(task_id, active, active_found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    CameraHubHotStatus hub;
    std::string ignored;
    const bool hub_found = control_->getHubStatus(task.camera_profile, hub, ignored) && hub.found;
    json body = {
        {"success", true}, {"request_id", request_id}, {"camera", taskJson(task)},
        {"current_run", active_found ? runJson(active) : json(nullptr)},
        {"hub", hub_found ? hubJson(hub, std::max(5000, config_.camera_hub.status_update_interval_ms * 3)) : json(nullptr)},
        {"links", {
            {"status", "/api/v1/cameras/" + task_id + "/status"},
            {"runs", "/api/v1/cameras/" + task_id + "/runs"},
            {"latest_frame", "/api/v1/cameras/" + task_id + "/latest-frame"},
            {"alerts", "/api/v1/cameras/" + task_id + "/alerts"}
        }}
    };
    return jsonResponse(200, body);
}

crow::response CameraTaskHttpController::updateTask(
    const crow::request& request,
    const std::string& task_id
) {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!safeIdentifier(task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    int expected_version = 0;
    if (!parseIfMatch(request, expected_version)) {
        return errorResponse(428, "PRECONDITION_REQUIRED", request_id);
    }
    const json input = json::parse(request.body, nullptr, false);
    if (input.is_discarded() || !input.is_object()) return errorResponse(400, "INVALID_JSON", request_id);
    if (containsForbiddenSecretField(input)) {
        return errorResponse(400, "RTSP_URI_IN_REQUEST_FORBIDDEN", request_id);
    }
    static const std::set<std::string> allowed{
        "name", "camera_profile", "enabled", "frame_interval_ms", "output_mode", "jpeg_quality",
        "max_width", "max_height", "retention_days", "max_saved_frames", "desired_state", "analysis",
        "callback_profile"
    };
    if (input.empty() || !onlyFields(input, allowed)) {
        return errorResponse(400, input.empty() ? "INVALID_TASK_CONFIG" : "UNKNOWN_FIELD", request_id);
    }
    CameraTaskPatch patch;
    try {
        if (input.contains("name")) patch.name = input.at("name").get<std::string>();
        if (input.contains("camera_profile")) patch.camera_profile = input.at("camera_profile").get<std::string>();
        if (input.contains("enabled")) patch.enabled = input.at("enabled").get<bool>();
        if (input.contains("frame_interval_ms")) patch.frame_interval_ms = input.at("frame_interval_ms").get<int>();
        if (input.contains("output_mode")) patch.output_mode = input.at("output_mode").get<std::string>();
        if (input.contains("jpeg_quality")) patch.jpeg_quality = input.at("jpeg_quality").get<int>();
        if (input.contains("max_width")) patch.max_width = input.at("max_width").get<int>();
        if (input.contains("max_height")) patch.max_height = input.at("max_height").get<int>();
        if (input.contains("retention_days")) patch.retention_days = input.at("retention_days").get<int>();
        if (input.contains("max_saved_frames")) patch.max_saved_frames = input.at("max_saved_frames").get<int>();
        if (input.contains("desired_state")) patch.desired_state = input.at("desired_state").get<std::string>();
        if (input.contains("callback_profile")) patch.callback_profile = input.at("callback_profile").get<std::string>();
        if (input.contains("analysis")) {
            const auto& analysis = input.at("analysis");
            static const std::set<std::string> analysis_allowed{
                "enabled", "target_infer_fps", "algorithm_profile", "algorithms"
            };
            if (!analysis.is_object() || !onlyFields(analysis, analysis_allowed) || analysis.empty()) {
                return errorResponse(400, analysis.is_object() ?
                    (analysis.empty() ? "INVALID_TASK_CONFIG" : "UNKNOWN_FIELD") :
                    "INVALID_TASK_CONFIG", request_id);
            }
            if (analysis.contains("enabled")) patch.analysis_enabled = analysis.at("enabled").get<bool>();
            if (analysis.contains("target_infer_fps")) {
                patch.target_infer_fps = analysis.at("target_infer_fps").get<double>();
            }
            if (analysis.contains("algorithm_profile")) {
                patch.algorithm_profile = analysis.at("algorithm_profile").get<std::string>();
            }
            if (analysis.contains("algorithms")) {
                patch.algorithms = analysis.at("algorithms").get<std::vector<std::string>>();
            }
        }
    }
    catch (...) {
        return errorResponse(400, "INVALID_TASK_CONFIG", request_id);
    }
    if (patch.enabled && patch.desired_state &&
        *patch.enabled != (*patch.desired_state == "running")) {
        return errorResponse(400, "INVALID_TASK_CONFIG", request_id);
    }
    if (patch.desired_state) patch.enabled = *patch.desired_state == "running";
    else if (patch.enabled) patch.desired_state = *patch.enabled ? "running" : "stopped";
    if (patch.camera_profile) {
        CameraProfile resolved_profile;
        bool profile_found = false;
        std::string profile_error;
        if (profile_registry_) {
            if (!profile_registry_->get(*patch.camera_profile, false,
                resolved_profile, profile_found, profile_error)) {
                return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
            }
        }
        else {
            const auto profile = config_.camera_profiles.find(*patch.camera_profile);
            profile_found = profile != config_.camera_profiles.end();
            if (profile_found) resolved_profile = profile->second;
        }
        if (!profile_found) return errorResponse(400, "CAMERA_PROFILE_NOT_FOUND", request_id);
        if (!resolved_profile.enabled) return errorResponse(409, "CAMERA_PROFILE_DISABLED", request_id);
    }
    CameraTaskDefinition candidate;
    bool candidate_found = false;
    std::string validation_error;
    if (!repository_->getTask(task_id, false, candidate, candidate_found, validation_error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!candidate_found) return errorResponse(404, "TASK_NOT_FOUND", request_id);
    // Reject a stale write before publishing a stop request. Without this
    // guard, an invalid If-Match could still interrupt the live Camera.
    if (candidate.version != expected_version) {
        return errorResponse(409, "TASK_VERSION_CONFLICT", request_id);
    }
    if (patch.name) candidate.name = *patch.name;
    if (patch.camera_profile) candidate.camera_profile = *patch.camera_profile;
    if (patch.enabled) candidate.enabled = *patch.enabled;
    if (patch.frame_interval_ms) candidate.frame_interval_ms = *patch.frame_interval_ms;
    if (patch.output_mode) candidate.output_mode = *patch.output_mode;
    if (patch.jpeg_quality) candidate.jpeg_quality = *patch.jpeg_quality;
    if (patch.max_width) candidate.max_width = *patch.max_width;
    if (patch.max_height) candidate.max_height = *patch.max_height;
    if (patch.retention_days) candidate.retention_days = *patch.retention_days;
    if (patch.max_saved_frames) candidate.max_saved_frames = *patch.max_saved_frames;
    if (patch.desired_state) candidate.desired_state = *patch.desired_state;
    if (patch.analysis_enabled) candidate.analysis_enabled = *patch.analysis_enabled;
    if (patch.target_infer_fps) candidate.target_infer_fps = *patch.target_infer_fps;
    if (patch.algorithm_profile) candidate.algorithm_profile = *patch.algorithm_profile;
    if (patch.algorithms) candidate.algorithms = *patch.algorithms;
    if (patch.callback_profile) candidate.callback_profile = *patch.callback_profile;
    candidate.enabled = candidate.desired_state == "running";
    if (!validTaskDefinition(candidate, config_.analysis)) {
        return errorResponse(400, "INVALID_TASK_CONFIG", request_id);
    }
    CameraTaskRunRecord active;
    bool active_found = false;
    std::string lifecycle_error;
    if (!getActiveRun(task_id, active, active_found, lifecycle_error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (active_found && active.status != "stopping") {
        if (!control_->requestStop(active.run_id, lifecycle_error)) {
            return errorResponse(503, "QUEUE_SUBMIT_FAILED", request_id);
        }
        CameraTaskRunRecord stopping = active;
        stopping.status = "stopping";
        stopping.last_update_ms = nowMs();
        std::string transition_code;
        if (!repository_->transitionRun(active.run_id,
            { "queued", "starting", "running", "reconnecting" }, stopping,
            transition_code, lifecycle_error)) {
            return errorResponse(409, transition_code.empty() ? "TASK_ACTIVE" : transition_code, request_id);
        }
    }
    CameraTaskDefinition updated;
    std::string code;
    std::string error;
    if (!repository_->updateTask(
        task_id, expected_version, patch, nowMs(), updated, code, error)) {
        const int status = code == "TASK_NOT_FOUND" ? 404 :
            (code == "TASK_ACTIVE" || code == "TASK_VERSION_CONFLICT" ? 409 :
                (code == "INVALID_TASK_CONFIG" ? 400 : 503));
        return errorResponse(status, code.empty() ? "STORAGE_UNAVAILABLE" : code, request_id);
    }
    if (updated.enabled) {
        auto response = startTask(request, task_id);
        response.set_header("ETag", "\"" + std::to_string(updated.version) + "\"");
        return response;
    }
    auto response = jsonResponse(active_found ? 202 : 200, {
        {"success", true}, {"request_id", request_id}, {"camera", taskJson(updated)},
        {"status", active_found ? "stopping" : "stopped"}
    });
    response.set_header("ETag", "\"" + std::to_string(updated.version) + "\"");
    return response;
}

crow::response CameraTaskHttpController::deleteTask(
    const crow::request& request,
    const std::string& task_id
) {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!safeIdentifier(task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    int expected_version = 0;
    if (!parseIfMatch(request, expected_version)) {
        return errorResponse(428, "PRECONDITION_REQUIRED", request_id);
    }
    CameraTaskDefinition camera;
    bool camera_found = false;
    std::string error;
    if (!repository_->getTask(task_id, false, camera, camera_found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!camera_found) return errorResponse(404, "TASK_NOT_FOUND", request_id);
    // Deletion owns the extraction lifecycle, so optimistic concurrency must
    // be checked before it is allowed to stop the current generation.
    if (camera.version != expected_version) {
        return errorResponse(409, "TASK_VERSION_CONFLICT", request_id);
    }
    CameraTaskRunRecord active;
    bool active_found = false;
    if (!getActiveRun(task_id, active, active_found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (active_found && active.status != "stopping") {
        if (!control_->requestStop(active.run_id, error)) {
            return errorResponse(503, "QUEUE_SUBMIT_FAILED", request_id);
        }
        CameraTaskRunRecord stopping = active;
        stopping.status = "stopping";
        stopping.last_update_ms = nowMs();
        std::string transition_code;
        if (!repository_->transitionRun(active.run_id,
            { "queued", "starting", "running", "reconnecting" }, stopping,
            transition_code, error)) {
            return errorResponse(409, transition_code.empty() ? "TASK_ACTIVE" : transition_code, request_id);
        }
    }
    std::string code;
    if (!repository_->softDeleteTask(task_id, expected_version, nowMs(), code, error)) {
        const int status = code == "TASK_NOT_FOUND" ? 404 :
            (code == "TASK_ACTIVE" || code == "TASK_VERSION_CONFLICT" ? 409 : 503);
        return errorResponse(status, code.empty() ? "STORAGE_UNAVAILABLE" : code, request_id);
    }
    if (active_found) {
        return jsonResponse(202, {
            {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
            {"run_id", active.run_id}, {"status", "stopping"}, {"deleted", true}
        });
    }
    crow::response response(204);
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response CameraTaskHttpController::startTask(
    const crow::request& request,
    const std::string& task_id
) {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!safeIdentifier(task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    const std::string idempotency_key = request.get_header_value("Idempotency-Key");
    const std::string idempotency_scope = "camera.start";
    const std::string idempotency_digest = requestDigest(task_id + "\n" + request.body);
    bool idempotency_handled = false;
    auto idempotency_response = replayIdempotency(
        repository_, idempotency_scope, idempotency_key, idempotency_digest,
        request_id, idempotency_handled);
    if (idempotency_handled) return idempotency_response;
    CameraTaskDefinition task;
    bool found = false;
    std::string error;
    if (!repository_->getTask(task_id, false, task, found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!found) return errorResponse(404, "TASK_NOT_FOUND", request_id);
    if (!task.enabled) {
        CameraTaskPatch start_patch;
        start_patch.enabled = true;
        start_patch.desired_state = "running";
        CameraTaskDefinition updated;
        std::string update_code;
        if (!repository_->updateTask(task_id, task.version, start_patch, nowMs(),
            updated, update_code, error)) {
            return errorResponse(update_code == "TASK_VERSION_CONFLICT" ? 409 : 503,
                update_code.empty() ? "STORAGE_UNAVAILABLE" : update_code, request_id);
        }
        task = std::move(updated);
    }
    CameraProfile resolved_profile;
    bool profile_found = false;
    std::string profile_error;
    if (profile_registry_) {
        if (!profile_registry_->get(task.camera_profile, false,
            resolved_profile, profile_found, profile_error)) {
            return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
        }
    }
    else {
        const auto profile = config_.camera_profiles.find(task.camera_profile);
        profile_found = profile != config_.camera_profiles.end();
        if (profile_found) resolved_profile = profile->second;
    }
    if (!profile_found) return errorResponse(400, "CAMERA_PROFILE_NOT_FOUND", request_id);
    if (!resolved_profile.enabled) return errorResponse(409, "CAMERA_PROFILE_DISABLED", request_id);
    CameraTaskRunRecord active;
    bool active_found = false;
    if (!getActiveRun(task_id, active, active_found, error, false)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (active_found) {
        auto response = jsonResponse(200, {
            {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
            {"run_id", active.run_id}, {"status", active.status}, {"idempotent_replay", true},
            {"status_url", "/api/v1/cameras/" + task_id + "/status"},
            {"latest_frame_url", "/api/v1/cameras/" + task_id + "/latest-frame"}
        });
        response.set_header("ETag", "\"" + std::to_string(task.version) + "\"");
        storeIdempotency(repository_, idempotency_scope, idempotency_key,
            idempotency_digest, task_id, response);
        return response;
    }
    CameraTaskRunRecord run;
    run.run_id = makeId("cr_");
    run.task_id = task.task_id;
    run.definition_version = task.version;
    run.definition_json = json({
        {"camera_profile", task.camera_profile}, {"frame_interval_ms", task.frame_interval_ms},
        {"output_mode", task.output_mode}, {"jpeg_quality", task.jpeg_quality},
        {"max_width", task.max_width}, {"max_height", task.max_height},
        {"retention_days", task.retention_days}, {"max_saved_frames", task.max_saved_frames},
        {"desired_state", task.desired_state},
        {"analysis", {
            {"enabled", task.analysis_enabled}, {"target_infer_fps", task.target_infer_fps},
            {"algorithm_profile", task.algorithm_profile}, {"algorithms", task.algorithms}
        }},
        {"callback_profile", task.callback_profile}
    }).dump();
    run.status = "queued";
    run.camera_profile = task.camera_profile;
    run.create_time_ms = nowMs();
    run.last_update_ms = run.create_time_ms;
    std::string code;
    if (!repository_->createRun(run, code, error)) {
        if (code == "ACTIVE_RUN_EXISTS" &&
            getActiveRun(task_id, active, active_found, error, false) && active_found) {
            auto response = jsonResponse(200, {
                {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
                {"run_id", active.run_id}, {"status", active.status}, {"idempotent_replay", true}
            });
            response.set_header("ETag", "\"" + std::to_string(task.version) + "\"");
            storeIdempotency(repository_, idempotency_scope, idempotency_key,
                idempotency_digest, task_id, response);
            return response;
        }
        return errorResponse(code == "TASK_DISABLED" || code == "TASK_VERSION_CONFLICT" ? 409 : 503,
            code.empty() ? "STORAGE_UNAVAILABLE" : code, request_id);
    }
    CameraTaskCommand command;
    command.task_id = task.task_id;
    command.run_id = run.run_id;
    command.camera_profile = task.camera_profile;
    command.definition_version = task.version;
    command.frame_interval_ms = task.frame_interval_ms;
    command.output_mode = task.output_mode;
    command.jpeg_quality = task.jpeg_quality;
    command.max_width = task.max_width;
    command.max_height = task.max_height;
    command.retention_days = task.retention_days;
    command.max_saved_frames = task.max_saved_frames;
    command.analysis_enabled = task.analysis_enabled;
    command.target_infer_fps = task.target_infer_fps;
    command.algorithm_profile = task.algorithm_profile;
    command.algorithms = task.algorithms;
    command.callback_profile = task.callback_profile;
    command.create_time_ms = run.create_time_ms;
    if (!control_->submitStart(command, error)) {
        CameraTaskRunRecord failed = run;
        failed.status = "failed";
        failed.stop_time_ms = nowMs();
        failed.last_update_ms = failed.stop_time_ms;
        failed.stop_reason = "queue_submit_failed";
        failed.error_code = "QUEUE_SUBMIT_FAILED";
        failed.error_message = "camera task queue submission failed";
        repository_->transitionRun(run.run_id, { "queued" }, failed, code, error);
        return errorResponse(503, "QUEUE_SUBMIT_FAILED", request_id);
    }
    auto response = jsonResponse(202, {
        {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
        {"run_id", run.run_id}, {"status", "queued"}, {"idempotent_replay", false},
        {"status_url", "/api/v1/cameras/" + task_id + "/status"},
        {"latest_frame_url", "/api/v1/cameras/" + task_id + "/latest-frame"}
    });
    response.set_header("ETag", "\"" + std::to_string(task.version) + "\"");
    storeIdempotency(repository_, idempotency_scope, idempotency_key,
        idempotency_digest, task_id, response);
    return response;
}

crow::response CameraTaskHttpController::stopTask(
    const crow::request& request,
    const std::string& task_id
) {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!safeIdentifier(task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    const std::string idempotency_key = request.get_header_value("Idempotency-Key");
    const std::string idempotency_scope = "camera.stop";
    const std::string idempotency_digest = requestDigest(task_id + "\n" + request.body);
    bool idempotency_handled = false;
    auto idempotency_response = replayIdempotency(
        repository_, idempotency_scope, idempotency_key, idempotency_digest,
        request_id, idempotency_handled);
    if (idempotency_handled) return idempotency_response;
    CameraTaskDefinition task;
    bool task_found = false;
    std::string error;
    if (!repository_->getTask(task_id, false, task, task_found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!task_found) return errorResponse(404, "TASK_NOT_FOUND", request_id);
    if (task.enabled || task.desired_state != "stopped") {
        CameraTaskPatch stop_patch;
        stop_patch.enabled = false;
        stop_patch.desired_state = "stopped";
        CameraTaskDefinition updated;
        std::string update_code;
        if (!repository_->updateTask(task_id, task.version, stop_patch, nowMs(),
            updated, update_code, error)) {
            return errorResponse(update_code == "TASK_VERSION_CONFLICT" ? 409 : 503,
                update_code.empty() ? "STORAGE_UNAVAILABLE" : update_code, request_id);
        }
        task = std::move(updated);
    }
    CameraTaskRunRecord active;
    bool active_found = false;
    if (!getActiveRun(task_id, active, active_found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!active_found) {
        std::vector<CameraTaskRunRecord> runs;
        if (!repository_->listRuns(task_id, 1, 0, runs, error)) {
            return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
        }
        auto response = jsonResponse(200, {
            {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
            {"run_id", runs.empty() ? "" : runs.front().run_id},
            {"status", runs.empty() ? "idle" : runs.front().status}, {"idempotent_replay", true}
        });
        response.set_header("ETag", "\"" + std::to_string(task.version) + "\"");
        storeIdempotency(repository_, idempotency_scope, idempotency_key,
            idempotency_digest, task_id, response);
        return response;
    }
    if (!control_->requestStop(active.run_id, error)) {
        return errorResponse(503, "QUEUE_SUBMIT_FAILED", request_id);
    }
    const bool already_stopping = active.status == "stopping";
    if (!already_stopping) {
        CameraTaskRunRecord stopping = active;
        stopping.status = "stopping";
        stopping.last_update_ms = nowMs();
        std::string transition_code;
        std::string transition_error;
        repository_->transitionRun(active.run_id,
            { "queued", "starting", "running", "reconnecting" }, stopping,
            transition_code, transition_error);
    }
    auto response = jsonResponse(already_stopping ? 200 : 202, {
        {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
        {"run_id", active.run_id}, {"status", "stopping"},
        {"idempotent_replay", already_stopping}
    });
    response.set_header("ETag", "\"" + std::to_string(task.version) + "\"");
    storeIdempotency(repository_, idempotency_scope, idempotency_key,
        idempotency_digest, task_id, response);
    return response;
}

crow::response CameraTaskHttpController::taskStatus(
    const crow::request& request,
    const std::string& task_id
) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!safeIdentifier(task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    CameraTaskDefinition task;
    bool task_found = false;
    std::string error;
    if (!repository_->getTask(task_id, false, task, task_found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!task_found) return errorResponse(404, "TASK_NOT_FOUND", request_id);
    CameraTaskRunRecord run;
    bool active_found = false;
    if (!getActiveRun(task_id, run, active_found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!active_found) {
        std::vector<CameraTaskRunRecord> runs;
        if (!repository_->listRuns(task_id, 1, 0, runs, error)) {
            return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
        }
        if (runs.empty()) {
            return jsonResponse(200, {
                {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
                {"run_id", ""}, {"status", "idle"},
                {"desired_state", task.desired_state}, {"observed_state", "stopped"},
                {"runtime_stale", true},
                {"analysis", {
                    {"enabled", task.analysis_enabled}, {"target_infer_fps", task.target_infer_fps},
                    {"algorithm_profile", task.algorithm_profile}, {"algorithms", task.algorithms}
                }},
                {"pipeline", {
                    {"thread_running", false}, {"thread_started_at_ms", 0},
                    {"thread_age_ms", 0}, {"sample_fps", 0.0},
                    {"sampled_frames", 0}, {"last_source_sequence", 0},
                    {"skipped_frames", 0}, {"inference_submit_drops", 0}
                }}
            });
        }
        run = runs.front();
    }
    CameraTaskRunHotStatus hot;
    std::string ignored;
    const bool hot_ok = control_->getRunStatus(run.run_id, hot, ignored) && hot.found;
    const bool stale = !hot_ok || nowMs() - hot.last_update_ms >
        std::max(5000, config_.camera_hub.status_update_interval_ms * 3);
    const long long pipeline_started_at_ms = hot_ok && hot.pipeline_started_at_ms > 0
        ? hot.pipeline_started_at_ms : run.start_time_ms;
    const long long pipeline_thread_age_ms = pipeline_started_at_ms > 0
        ? std::max(0LL, nowMs() - pipeline_started_at_ms) : 0;
    CameraHubHotStatus hub;
    const bool hub_ok = control_->getHubStatus(task.camera_profile, hub, ignored) && hub.found;
    return jsonResponse(200, {
        {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
        {"run_id", run.run_id}, {"status", hot_ok ? hot.status : run.status},
        {"desired_state", task.desired_state},
        {"observed_state", hot_ok ? hot.status : run.status},
        {"runtime_stale", stale},
        {"hub", hub_ok ? hubJson(hub, std::max(5000, config_.camera_hub.status_update_interval_ms * 3)) : json(nullptr)},
        {"subscription", {
            {"last_source_sequence", hot_ok ? hot.last_source_sequence : run.last_source_sequence},
            {"consumed_frames", hot_ok ? hot.consumed_frames : run.consumed_frames},
            {"skipped_frames", hot_ok ? hot.skipped_frames : run.skipped_frames}
        }},
        {"pipeline", {
            {"thread_running", hot_ok ? hot.pipeline_thread_running :
                (run.status == "starting" || run.status == "running" ||
                    run.status == "reconnecting" || run.status == "stopping")},
            {"thread_started_at_ms", pipeline_started_at_ms},
            {"thread_age_ms", pipeline_thread_age_ms},
            {"sample_fps", hot_ok ? hot.sample_fps : run.save_fps},
            {"sampled_frames", hot_ok ? hot.sampled_frames : run.consumed_frames},
            {"last_source_sequence", hot_ok ? hot.last_source_sequence : run.last_source_sequence},
            {"skipped_frames", hot_ok ? hot.skipped_frames : run.skipped_frames},
            {"inference_submit_drops", hot_ok ? hot.inference_submit_drops : 0}
        }},
        {"extraction", {
            {"save_fps", hot_ok ? hot.save_fps : run.save_fps},
            {"saved_frames", hot_ok ? hot.saved_frames : run.saved_frames},
            {"dropped_frames", hot_ok ? hot.dropped_frames : run.dropped_frames},
            {"writer_queue_depth", hot_ok ? hot.writer_queue_depth : 0},
            {"last_frame_time_ms", hot_ok ? hot.last_frame_time_ms : run.last_frame_time_ms}
        }},
        {"analysis", {
            {"enabled", task.analysis_enabled}, {"target_infer_fps", task.target_infer_fps},
            {"algorithm_profile", task.algorithm_profile}, {"algorithms", task.algorithms},
            {"state", task.analysis_enabled ? "configured" : "disabled"}
        }},
        {"error_code", hot_ok ? hot.error_code : run.error_code},
        {"error", (hot_ok ? hot.error_code : run.error_code).empty()
            ? "" : publicError(hot_ok ? hot.error_code : run.error_code)}
    });
}

crow::response CameraTaskHttpController::latestFrame(
    const crow::request& request,
    const std::string& task_id
) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!safeIdentifier(task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    CameraTaskDefinition task;
    bool found = false;
    std::string error;
    if (!repository_->getTask(task_id, false, task, found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!found) return errorResponse(404, "TASK_NOT_FOUND", request_id);
    if (task.output_mode == "archive") {
        return errorResponse(409, "LATEST_OUTPUT_DISABLED", request_id);
    }
    const auto root = std::filesystem::absolute(std::filesystem::u8path(config_.camera_tasks.output_dir));
    const auto directory = root / std::filesystem::u8path(task_id);
    const auto path = directory / "latest.jpg";
    if (isReparsePoint(root) || isReparsePoint(directory) || isReparsePoint(path)) {
        return errorResponse(404, "FRAME_NOT_READY", request_id);
    }
    std::ifstream input(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() < 4 || static_cast<unsigned char>(bytes[0]) != 0xff ||
        static_cast<unsigned char>(bytes[1]) != 0xd8 ||
        static_cast<unsigned char>(bytes[bytes.size() - 2]) != 0xff ||
        static_cast<unsigned char>(bytes.back()) != 0xd9) {
        return errorResponse(404, "FRAME_NOT_READY", request_id);
    }
    crow::response response(200, std::move(bytes));
    response.set_header("Content-Type", "image/jpeg");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response CameraTaskHttpController::listRuns(
    const crow::request& request,
    const std::string& task_id
) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!safeIdentifier(task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    CameraTaskDefinition task;
    bool found = false;
    std::string error;
    if (!repository_->getTask(task_id, false, task, found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!found) return errorResponse(404, "TASK_NOT_FOUND", request_id);
    const int limit = queryInt(request, "limit", 20, 1, 200);
    const int offset = queryInt(request, "offset", 0, 0, 1000000);
    std::vector<CameraTaskRunRecord> runs;
    if (!repository_->listRuns(task_id, limit, offset, runs, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    json items = json::array();
    for (const auto& run : runs) items.push_back(runJson(run));
    return jsonResponse(200, {
        {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
        {"items", items}, {"limit", limit}, {"offset", offset}
    });
}

crow::response CameraTaskHttpController::listAlerts(
    const crow::request& request,
    const std::string& task_id
) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!safeIdentifier(task_id)) return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    CameraTaskDefinition task;
    bool found = false;
    std::string error;
    if (!repository_->getTask(task_id, false, task, found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!found) return errorResponse(404, "TASK_NOT_FOUND", request_id);
    const int limit = queryInt(request, "limit", 20, 1, 200);
    const int offset = queryInt(request, "offset", 0, 0, 1000000);
    const int minimum_severity = queryInt(request, "minimum_severity", 1, 1, 5);
    const char* event_type_value = request.url_params.get("event_type");
    const std::string event_type = event_type_value ? event_type_value : "";
    if (!event_type.empty() && !safeServiceIdentifier(event_type, 80)) {
        return errorResponse(400, "INVALID_IDENTIFIER", request_id);
    }
    std::vector<SecurityAlertEventRecord> alerts;
    if (!repository_->listAlerts(task_id, event_type, minimum_severity,
        limit, offset, alerts, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    json items = json::array();
    for (const auto& alert : alerts) items.push_back(alertJson(alert));
    return jsonResponse(200, {
        {"success", true}, {"request_id", request_id}, {"camera_id", task_id},
        {"items", items}, {"limit", limit}, {"offset", offset},
        {"minimum_severity", minimum_severity},
        {"event_type", event_type.empty() ? json(nullptr) : json(event_type)}
    });
}

crow::response CameraTaskHttpController::listHubs(const crow::request& request) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    json items = json::array();
    std::vector<CameraProfile> profiles;
    std::string registry_error;
    if (profile_registry_) {
        if (!profile_registry_->list(false, profiles, registry_error)) {
            return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
        }
    }
    else {
        for (const auto& entry : config_.camera_profiles) profiles.push_back(entry.second);
    }
    for (const auto& profile : profiles) {
        if (!profile.enabled) continue;
        CameraHubHotStatus hot;
        std::string error;
        if (!control_->getHubStatus(profile.id, hot, error)) {
            return errorResponse(503, "QUEUE_SUBMIT_FAILED", request_id);
        }
        if (hot.found) items.push_back(hubJson(
            hot, std::max(5000, config_.camera_hub.status_update_interval_ms * 3)));
    }
    return jsonResponse(200, {
        {"success", true}, {"request_id", request_id}, {"items", items}
    });
}

crow::response CameraTaskHttpController::getHub(
    const crow::request& request,
    const std::string& camera_profile
) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!safeIdentifier(camera_profile)) return errorResponse(404, "CAMERA_PROFILE_NOT_FOUND", request_id);
    bool profile_found = false;
    CameraProfile profile;
    std::string profile_error;
    if (profile_registry_) {
        if (!profile_registry_->get(camera_profile, false, profile, profile_found, profile_error)) {
            return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
        }
    }
    else {
        const auto found = config_.camera_profiles.find(camera_profile);
        profile_found = found != config_.camera_profiles.end();
    }
    if (!profile_found) return errorResponse(404, "CAMERA_PROFILE_NOT_FOUND", request_id);
    CameraHubHotStatus hot;
    std::string error;
    if (!control_->getHubStatus(camera_profile, hot, error)) {
        return errorResponse(503, "QUEUE_SUBMIT_FAILED", request_id);
    }
    if (!hot.found) return errorResponse(404, "CAMERA_PROFILE_NOT_FOUND", request_id);
    return jsonResponse(200, {
        {"success", true}, {"request_id", request_id},
        {"hub", hubJson(hot, std::max(5000, config_.camera_hub.status_update_interval_ms * 3))}
    });
}

crow::response CameraTaskHttpController::createProfile(const crow::request& request) {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!profile_registry_) return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    const std::size_t max_bytes = static_cast<std::size_t>(
        std::max(1, config_.server.max_body_size_mb)) * 1024U * 1024U;
    if (request.body.size() > max_bytes) return errorResponse(413, "REQUEST_TOO_LARGE", request_id);
    const json input = json::parse(request.body, nullptr, false);
    if (input.is_discarded() || !input.is_object()) return errorResponse(400, "INVALID_JSON", request_id);
    if (containsForbiddenProfileSecretField(input)) {
        return errorResponse(400, "RTSP_URI_IN_REQUEST_FORBIDDEN", request_id);
    }
    static const std::set<std::string> allowed{
        "profile_id", "source_type", "display_name", "url_env", "transport", "enabled"
    };
    if (!onlyFields(input, allowed)) return errorResponse(400, "UNKNOWN_FIELD", request_id);
    if (!input.contains("profile_id") || !input.contains("display_name") || !input.contains("url_env") ||
        !input["profile_id"].is_string() || !input["display_name"].is_string() ||
        !input["url_env"].is_string()) {
        return errorResponse(400, "INVALID_CAMERA_PROFILE", request_id);
    }
    CameraProfile profile;
    try {
        profile.id = input["profile_id"].get<std::string>();
        profile.source_type = input.value("source_type", "rtsp");
        profile.display_name = input["display_name"].get<std::string>();
        profile.url_env = input["url_env"].get<std::string>();
        profile.transport = input.value("transport", "tcp");
        profile.enabled = input.value("enabled", true);
    }
    catch (...) {
        return errorResponse(400, "INVALID_CAMERA_PROFILE", request_id);
    }
    std::string code;
    std::string error;
    if (!profile_registry_->create(profile, code, error)) {
        const int status = code == "CAMERA_PROFILE_ALREADY_EXISTS" ? 409 :
            (code == "INVALID_CAMERA_PROFILE" ? 400 : 503);
        return errorResponse(status, code.empty() ? "STORAGE_UNAVAILABLE" : code, request_id);
    }
    bool found = false;
    if (!profile_registry_->get(profile.id, false, profile, found, error) || !found) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    auto response = jsonResponse(201, {
        {"success", true}, {"request_id", request_id}, {"profile", profileJson(profile)},
        {"restart_required", true}
    });
    response.set_header("ETag", "\"" + std::to_string(profile.version) + "\"");
    return response;
}

crow::response CameraTaskHttpController::listProfiles(const crow::request& request) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!profile_registry_) return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    bool present = false;
    const bool include_deleted = queryBool(request, "include_deleted", false, present);
    std::vector<CameraProfile> profiles;
    std::string error;
    if (!profile_registry_->list(include_deleted, profiles, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    json items = json::array();
    for (const auto& profile : profiles) items.push_back(profileJson(profile));
    return jsonResponse(200, {
        {"success", true}, {"request_id", request_id}, {"items", items},
        {"restart_required_after_mutation", true}
    });
}

crow::response CameraTaskHttpController::getProfile(
    const crow::request& request,
    const std::string& profile_id
) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!profile_registry_ || !CameraProfileRegistry::validIdentifier(profile_id)) {
        return errorResponse(404, "CAMERA_PROFILE_NOT_FOUND", request_id);
    }
    CameraProfile profile;
    bool found = false;
    std::string error;
    if (!profile_registry_->get(profile_id, false, profile, found, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (!found) return errorResponse(404, "CAMERA_PROFILE_NOT_FOUND", request_id);
    auto response = jsonResponse(200, {
        {"success", true}, {"request_id", request_id}, {"profile", profileJson(profile)}
    });
    response.set_header("ETag", "\"" + std::to_string(profile.version) + "\"");
    return response;
}

crow::response CameraTaskHttpController::updateProfile(
    const crow::request& request,
    const std::string& profile_id
) {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!profile_registry_ || !CameraProfileRegistry::validIdentifier(profile_id)) {
        return errorResponse(404, "CAMERA_PROFILE_NOT_FOUND", request_id);
    }
    int expected_version = 0;
    if (!parseIfMatch(request, expected_version)) {
        return errorResponse(428, "PRECONDITION_REQUIRED", request_id);
    }
    const json input = json::parse(request.body, nullptr, false);
    if (input.is_discarded() || !input.is_object()) return errorResponse(400, "INVALID_JSON", request_id);
    if (containsForbiddenProfileSecretField(input)) {
        return errorResponse(400, "RTSP_URI_IN_REQUEST_FORBIDDEN", request_id);
    }
    static const std::set<std::string> allowed{ "display_name", "url_env", "transport", "enabled" };
    if (input.empty() || !onlyFields(input, allowed)) {
        return errorResponse(400, input.empty() ? "INVALID_CAMERA_PROFILE" : "UNKNOWN_FIELD", request_id);
    }
    CameraProfilePatch patch;
    try {
        if (input.contains("display_name")) patch.display_name = input.at("display_name").get<std::string>();
        if (input.contains("url_env")) patch.url_env = input.at("url_env").get<std::string>();
        if (input.contains("transport")) patch.transport = input.at("transport").get<std::string>();
        if (input.contains("enabled")) patch.enabled = input.at("enabled").get<bool>();
    }
    catch (...) {
        return errorResponse(400, "INVALID_CAMERA_PROFILE", request_id);
    }
    CameraProfile profile;
    std::string code;
    std::string error;
    if (!profile_registry_->update(profile_id, expected_version, patch, profile, code, error)) {
        const int status = code == "CAMERA_PROFILE_NOT_FOUND" ? 404 :
            (code == "CAMERA_PROFILE_VERSION_CONFLICT" ? 409 :
                (code == "INVALID_CAMERA_PROFILE" ? 400 : 503));
        return errorResponse(status, code.empty() ? "STORAGE_UNAVAILABLE" : code, request_id);
    }
    auto response = jsonResponse(200, {
        {"success", true}, {"request_id", request_id}, {"profile", profileJson(profile)},
        {"restart_required", true}
    });
    response.set_header("ETag", "\"" + std::to_string(profile.version) + "\"");
    return response;
}

crow::response CameraTaskHttpController::deleteProfile(
    const crow::request& request,
    const std::string& profile_id
) {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    if (!profile_registry_ || !CameraProfileRegistry::validIdentifier(profile_id)) {
        return errorResponse(404, "CAMERA_PROFILE_NOT_FOUND", request_id);
    }
    int expected_version = 0;
    if (!parseIfMatch(request, expected_version)) {
        return errorResponse(428, "PRECONDITION_REQUIRED", request_id);
    }
    std::vector<CameraTaskDefinition> tasks;
    std::string error;
    if (!repository_->listTasks(false, 1000, 0, tasks, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    if (std::any_of(tasks.begin(), tasks.end(), [&](const auto& task) {
        return task.camera_profile == profile_id;
    })) return errorResponse(409, "CAMERA_PROFILE_IN_USE", request_id);
    std::string code;
    if (!profile_registry_->softDelete(profile_id, expected_version, code, error)) {
        const int status = code == "CAMERA_PROFILE_NOT_FOUND" ? 404 :
            (code == "CAMERA_PROFILE_VERSION_CONFLICT" ? 409 : 503);
        return errorResponse(status, code.empty() ? "STORAGE_UNAVAILABLE" : code, request_id);
    }
    crow::response response(204);
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Restart-Required", "true");
    return response;
}

crow::response CameraTaskHttpController::operationsMetrics(const crow::request& request) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    CameraTaskRepositoryStats stats;
    std::string error;
    if (!repository_->stats(stats, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    std::vector<CameraProfile> profiles;
    if (profile_registry_) {
        if (!profile_registry_->list(false, profiles, error)) {
            return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
        }
    }
    else {
        for (const auto& entry : config_.camera_profiles) profiles.push_back(entry.second);
    }
    json hubs = json::array();
    bool open_count_consistent = true;
    bool subscriber_count_consistent = true;
    int active_hubs = 0;
    for (const auto& profile : profiles) {
        if (!profile.enabled) continue;
        CameraHubHotStatus hot;
        std::string hub_error;
        if (!control_->getHubStatus(profile.id, hot, hub_error)) {
            return errorResponse(503, "QUEUE_SUBMIT_FAILED", request_id);
        }
        if (!hot.found) continue;
        ++active_hubs;
        long long typed_subscribers = 0;
        for (const auto& entry : hot.snapshot.subscriber_types) typed_subscribers += entry.second;
        subscriber_count_consistent = subscriber_count_consistent &&
            typed_subscribers == hot.snapshot.subscriber_count;
        open_count_consistent = open_count_consistent &&
            hot.snapshot.open_count <= static_cast<long long>(hot.snapshot.reconnect_count) + 1;
        hubs.push_back(hubJson(hot,
            std::max(5000, config_.camera_hub.status_update_interval_ms * 3)));
    }
    std::error_code fs_error;
    const auto output_root = std::filesystem::absolute(
        std::filesystem::u8path(config_.camera_tasks.output_dir), fs_error);
    std::filesystem::space_info space{};
    if (!fs_error) space = std::filesystem::space(output_root, fs_error);
    const long long high_limit = config_.camera_tasks.storage.max_archive_bytes > 0
        ? config_.camera_tasks.storage.max_archive_bytes *
            config_.camera_tasks.storage.high_watermark_percent / 100 : 0;
    const long long critical_limit = config_.camera_tasks.storage.max_archive_bytes > 0
        ? config_.camera_tasks.storage.max_archive_bytes *
            config_.camera_tasks.storage.critical_watermark_percent / 100 : 0;
    const bool free_pressure = !fs_error && config_.camera_tasks.storage.min_free_bytes > 0 &&
        static_cast<long long>(space.available) < config_.camera_tasks.storage.min_free_bytes;
    const std::string pressure = free_pressure || (critical_limit > 0 && stats.archive_bytes >= critical_limit)
        ? "critical" : ((high_limit > 0 && stats.archive_bytes >= high_limit) ? "high" : "normal");
    return jsonResponse(200, {
        {"success", true}, {"request_id", request_id}, {"generated_at_ms", nowMs()},
        {"tasks", {
            {"total", stats.tasks_total}, {"enabled", stats.tasks_enabled},
            {"deleted", stats.tasks_deleted}
        }},
        {"runs", {
            {"total", stats.runs_total}, {"active", stats.runs_active},
            {"failed", stats.runs_failed}
        }},
        {"frames", {
            {"total", stats.frames_total}, {"archive_bytes", stats.archive_bytes},
            {"latest_capture_time_ms", stats.latest_frame_time_ms}
        }},
        {"alerts", {{"total", stats.alerts_total}}},
        {"storage", {
            {"filesystem_ok", !fs_error},
            {"capacity_bytes", fs_error ? 0 : static_cast<long long>(space.capacity)},
            {"free_bytes", fs_error ? 0 : static_cast<long long>(space.free)},
            {"available_bytes", fs_error ? 0 : static_cast<long long>(space.available)},
            {"pressure", pressure},
            {"max_archive_bytes", config_.camera_tasks.storage.max_archive_bytes},
            {"min_free_bytes", config_.camera_tasks.storage.min_free_bytes},
            {"high_watermark_percent", config_.camera_tasks.storage.high_watermark_percent},
            {"critical_watermark_percent", config_.camera_tasks.storage.critical_watermark_percent}
        }},
        {"profiles", {{"configured", profiles.size()}, {"active_hubs", active_hubs}}},
        {"hubs", hubs},
        {"invariants", {
            {"one_hub_record_per_profile", true},
            {"open_count_consistent_with_reconnects", open_count_consistent},
            {"subscriber_count_matches_types", subscriber_count_consistent}
        }}
    });
}

crow::response CameraTaskHttpController::prometheusMetrics(const crow::request& request) const {
    const std::string request_id = makeId("req_");
    if (!authorized(request)) return errorResponse(401, "UNAUTHORIZED", request_id);
    CameraTaskRepositoryStats stats;
    std::string error;
    if (!repository_->stats(stats, error)) {
        return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
    }
    std::vector<CameraProfile> profiles;
    if (profile_registry_) {
        if (!profile_registry_->list(false, profiles, error)) {
            return errorResponse(503, "STORAGE_UNAVAILABLE", request_id);
        }
    }
    else {
        for (const auto& entry : config_.camera_profiles) profiles.push_back(entry.second);
    }
    std::error_code fs_error;
    const auto output_root = std::filesystem::absolute(
        std::filesystem::u8path(config_.camera_tasks.output_dir), fs_error);
    std::filesystem::space_info space{};
    if (!fs_error) space = std::filesystem::space(output_root, fs_error);
    const long long critical_limit = config_.camera_tasks.storage.max_archive_bytes > 0
        ? config_.camera_tasks.storage.max_archive_bytes *
            config_.camera_tasks.storage.critical_watermark_percent / 100 : 0;
    const long long high_limit = config_.camera_tasks.storage.max_archive_bytes > 0
        ? config_.camera_tasks.storage.max_archive_bytes *
            config_.camera_tasks.storage.high_watermark_percent / 100 : 0;
    const bool free_pressure = !fs_error && config_.camera_tasks.storage.min_free_bytes > 0 &&
        static_cast<long long>(space.available) < config_.camera_tasks.storage.min_free_bytes;
    const int pressure_level = free_pressure || (critical_limit > 0 && stats.archive_bytes >= critical_limit)
        ? 2 : ((high_limit > 0 && stats.archive_bytes >= high_limit) ? 1 : 0);
    std::ostringstream output;
    output << "# HELP yolo11_camera_tasks Camera Task definitions by state.\n"
           << "# TYPE yolo11_camera_tasks gauge\n"
           << "yolo11_camera_tasks{state=\"total\"} " << stats.tasks_total << '\n'
           << "yolo11_camera_tasks{state=\"enabled\"} " << stats.tasks_enabled << '\n'
           << "yolo11_camera_tasks{state=\"deleted\"} " << stats.tasks_deleted << '\n'
           << "# TYPE yolo11_camera_runs gauge\n"
           << "yolo11_camera_runs{state=\"total\"} " << stats.runs_total << '\n'
           << "yolo11_camera_runs{state=\"active\"} " << stats.runs_active << '\n'
           << "yolo11_camera_runs{state=\"failed\"} " << stats.runs_failed << '\n'
           << "# TYPE yolo11_camera_archive_frames gauge\n"
           << "yolo11_camera_archive_frames " << stats.frames_total << '\n'
           << "# TYPE yolo11_security_alert_events gauge\n"
           << "yolo11_security_alert_events " << stats.alerts_total << '\n'
           << "# TYPE yolo11_camera_archive_bytes gauge\n"
           << "yolo11_camera_archive_bytes " << stats.archive_bytes << '\n'
           << "# TYPE yolo11_camera_storage_available_bytes gauge\n"
           << "yolo11_camera_storage_available_bytes "
           << (fs_error ? 0 : static_cast<long long>(space.available)) << '\n'
           << "# HELP yolo11_camera_storage_pressure Storage pressure: 0 normal, 1 high, 2 critical.\n"
           << "# TYPE yolo11_camera_storage_pressure gauge\n"
           << "yolo11_camera_storage_pressure " << pressure_level << '\n';
    for (const auto& profile : profiles) {
        if (!profile.enabled) continue;
        CameraHubHotStatus hot;
        std::string hub_error;
        if (!control_->getHubStatus(profile.id, hot, hub_error)) {
            return errorResponse(503, "QUEUE_SUBMIT_FAILED", request_id);
        }
        if (!hot.found) continue;
        const auto& hub = hot.snapshot;
        output << "yolo11_camera_hub_active{camera_profile=\"" << profile.id << "\"} 1\n"
               << "yolo11_camera_hub_open_total{camera_profile=\"" << profile.id << "\"} "
               << hub.open_count << '\n'
               << "yolo11_camera_hub_reconnect_total{camera_profile=\"" << profile.id << "\"} "
               << hub.reconnect_count << '\n'
               << "yolo11_camera_hub_subscribers{camera_profile=\"" << profile.id << "\"} "
               << hub.subscriber_count << '\n'
               << "yolo11_camera_hub_latest_frame_age_ms{camera_profile=\"" << profile.id << "\"} "
               << hub.latest_frame_age_ms << '\n'
               << "yolo11_camera_hub_capture_fps{camera_profile=\"" << profile.id << "\"} "
               << hub.capture_fps << '\n';
    }
    crow::response response(200, output.str());
    response.set_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
    return response;
}

}  // namespace yolo11_server
