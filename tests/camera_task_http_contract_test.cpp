#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "business/postgres_client.h"
#include "server/camera_task_http_controller.h"
#include "postgres_test_guard.h"

namespace {

using namespace yolo11_server;
using json = nlohmann::json;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(encoded.begin(), encoded.end());
}

crow::request request(const std::string& payload = {}, bool authorize = true,
                      const std::string& query = {}) {
    crow::request result;
    result.body = payload;
    if (authorize) result.add_header("Authorization", "Bearer contract-secret");
    if (!query.empty()) result.url_params = crow::query_string(query);
    return result;
}

json responseBody(const crow::response& response) {
    return json::parse(response.body);
}

class FakeApiControl final : public ICameraTaskApiControl {
public:
    bool submitStart(const CameraTaskCommand& command, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_submit) {
            error = "redis unavailable at 127.0.0.1";
            return false;
        }
        submitted.push_back(command);
        error.clear();
        return true;
    }

    bool requestStop(const std::string& run_id, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        stops.push_back(run_id);
        error.clear();
        return true;
    }

    bool getRunStatus(const std::string& run_id, CameraTaskRunHotStatus& status,
                      std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_run_status) {
            error = "redis://user:secret@127.0.0.1 unavailable";
            return false;
        }
        status = run_status[run_id];
        error.clear();
        return true;
    }

    bool getHubStatus(const std::string& profile, CameraHubHotStatus& status,
                      std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_hub_status) {
            error = "redis://user:secret@127.0.0.1 unavailable";
            return false;
        }
        status = hubs[profile];
        error.clear();
        return true;
    }

    bool fail_submit = false;
    bool fail_run_status = false;
    bool fail_hub_status = false;
    std::vector<CameraTaskCommand> submitted;
    std::vector<std::string> stops;
    std::map<std::string, CameraTaskRunHotStatus> run_status;
    std::map<std::string, CameraHubHotStatus> hubs;
    std::mutex mutex_;
};

}  // namespace

int main() {
    if (const int guard = requireDisposablePostgresTestDatabase()) return guard;

    const auto root = std::filesystem::temp_directory_path() /
        ("camera_http_contract_" + std::to_string(nowMs()));
    std::filesystem::create_directories(root);

    AppConfig config;
    config.camera_tasks.enabled = true;
    config.camera_tasks.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN";
    config.camera_tasks.output_dir = pathUtf8(root / "output");
    config.stream.camera_profiles_path = pathUtf8(root / "cameras.yaml");
    config.worker.worker_num = 1;

    CameraProfile enabled;
    enabled.id = "entry_camera_01";
    enabled.url_env = "SECRET_RTSP_ENV_NAME";
    enabled.enabled = true;
    CameraProfile disabled = enabled;
    disabled.id = "disabled_camera";
    disabled.enabled = false;
    config.camera_profiles[enabled.id] = enabled;
    config.camera_profiles[disabled.id] = disabled;

    PostgresConnection database;
    std::string error;
    require(database.openFromEnvironment(config.camera_tasks.postgres_dsn_env, error),
        "test PostgreSQL connection must open: " + error);
    require(database.exec(
        "DROP TABLE IF EXISTS callback_outbox,security_alert_events,camera_idempotency_keys,"
        "camera_frames,camera_task_runs,camera_tasks,camera_schema_version CASCADE;",
        error), "test PostgreSQL schema reset must succeed: " + error);

    {
        std::ofstream profiles(root / "cameras.yaml", std::ios::binary);
        profiles << "cameras:\n"
                 << "  entry_camera_01:\n"
                 << "    source_type: rtsp\n"
                 << "    url_env: SECRET_RTSP_ENV_NAME\n"
                 << "    display_name: Main Entrance\n"
                 << "    transport: tcp\n"
                 << "    enabled: true\n"
                 << "  disabled_camera:\n"
                 << "    source_type: rtsp\n"
                 << "    url_env: DISABLED_RTSP_ENV\n"
                 << "    display_name: Disabled\n"
                 << "    transport: tcp\n"
                 << "    enabled: false\n";
    }

    auto repository = std::make_shared<CameraTaskRepository>(config.camera_tasks);
    auto control = std::make_shared<FakeApiControl>();
    CameraHubHotStatus hub;
    hub.found = true;
    hub.last_update_ms = nowMs();
    hub.snapshot.camera_profile = enabled.id;
    hub.snapshot.hub_instance_id = "hub_entry_1";
    hub.snapshot.state = "running";
    hub.snapshot.backend_name = "FFMPEG";
    hub.snapshot.open_count = 1;
    hub.snapshot.subscriber_count = 2;
    hub.snapshot.subscriber_types = { {"people_flow", 1}, {"camera_task", 1} };
    hub.snapshot.capture_fps = 25.0;
    hub.snapshot.latest_sequence = 55;
    hub.snapshot.last_error = "rtsp://user:password@example.invalid/secret";
    control->hubs[enabled.id] = hub;

    CameraTaskHttpController controller(config, repository, control, "contract-secret");
    require(controller.initialize(error), "HTTP controller must initialize: " + error);
    const auto health = controller.health();
    require(health.initialized && health.token_configured && health.storage_ok &&
            health.output_root_writable && health.worker_num_valid,
        "Camera API health prerequisites must be observable");

    auto response = controller.listProfiles(request());
    require(response.code == 200 && responseBody(response)["items"].size() == 2,
        "Camera Profiles are available through read-only handlers");
    response = controller.getProfile(request(), enabled.id);
    require(response.code == 200 && responseBody(response)["profile"]["profile_id"] == enabled.id,
        "Camera Profile detail must remain readable");

    response = controller.createTask(request("{}", false));
    require(response.code == 401 && responseBody(response)["error_code"] == "UNAUTHORIZED",
        "every Camera route must require Bearer authentication");
    require(response.body.find("contract-secret") == std::string::npos,
        "authentication errors must not echo the token");

    response = controller.createTask(request(
        R"({"camera_id":"unsafe","name":"bad","camera_profile":"entry_camera_01","rtsp_url":"rtsp://secret"})"));
    require(response.code == 400 &&
            responseBody(response)["error_code"] == "RTSP_URI_IN_REQUEST_FORBIDDEN" &&
            response.body.find("rtsp://secret") == std::string::npos,
        "Camera create must reject and not reflect RTSP secrets");
    response = controller.createTask(request(
        R"({"camera_id":"unsafe","name":"bad","camera_profile":"entry_camera_01","surprise":1})"));
    require(response.code == 400 && responseBody(response)["error_code"] == "UNKNOWN_FIELD",
        "undefined Camera fields must be rejected");

    const std::string camera_id = "entrance_extract_01";
    const std::string create_payload =
        R"({"camera_id":"entrance_extract_01","name":"Entrance extraction","camera_profile":"entry_camera_01","desired_state":"running","frame_interval_ms":1000,"output_mode":"both","jpeg_quality":88,"max_width":640,"max_height":480,"retention_days":7,"max_saved_frames":10,"analysis":{"enabled":true,"target_infer_fps":6.5,"algorithm_profile":"security_default","algorithms":["people_flow","ppe_detection"]},"callback_profile":"backend_primary"})";
    auto create_request = request(create_payload);
    create_request.add_header("Idempotency-Key", "create-entrance-001");
    response = controller.createTask(create_request);
    require(response.code == 202 && response.get_header_value("ETag") == "\"1\"" &&
            responseBody(response)["camera_id"] == camera_id && control->submitted.size() == 1,
        "creating an enabled Camera must persist it and submit one extraction generation");
    const std::string first_run_id = responseBody(response)["run_id"];
    require(control->submitted.front().task_id == camera_id &&
            control->submitted.front().run_id == first_run_id &&
            control->submitted.front().camera_profile == enabled.id &&
            control->submitted.front().analysis_enabled &&
            control->submitted.front().target_infer_fps == 6.5 &&
            control->submitted.front().algorithm_profile == "security_default" &&
            control->submitted.front().algorithms ==
                std::vector<std::string>({ "people_flow", "ppe_detection" }) &&
            control->submitted.front().callback_profile == "backend_primary",
        "the extraction command must carry the safe algorithm contract and contain no RTSP URI");
    response = controller.createTask(create_request);
    require(response.code == 202 &&
            response.get_header_value("X-Idempotent-Replay") == "true" &&
            response.get_header_value("ETag") == "\"1\"" &&
            responseBody(response)["run_id"] == first_run_id &&
            control->submitted.size() == 1,
        "same create idempotency key and payload must replay without a second Run");
    auto conflicting_create = request(
        R"({"camera_id":"different_camera","name":"Different","camera_profile":"entry_camera_01"})");
    conflicting_create.add_header("Idempotency-Key", "create-entrance-001");
    response = controller.createTask(conflicting_create);
    require(response.code == 409 &&
            responseBody(response)["error_code"] == "IDEMPOTENCY_CONFLICT",
        "reusing a create idempotency key for another payload must fail closed");

    response = controller.listTasks(request({}, true, "?limit=10&offset=0&enabled=true"));
    require(response.code == 200 && responseBody(response)["items"].size() == 1 &&
            responseBody(response)["items"][0]["camera_id"] == camera_id,
        "Camera list must expose camera_id and current Run");
    response = controller.getTask(request(), camera_id);
    require(response.code == 200 && responseBody(response)["camera"]["camera_id"] == camera_id &&
            responseBody(response)["camera"]["desired_state"] == "running" &&
            responseBody(response)["camera"]["analysis"]["enabled"] == true &&
            responseBody(response)["camera"]["analysis"]["algorithm_profile"] == "security_default" &&
            responseBody(response)["camera"]["callback_profile"] == "backend_primary" &&
            response.body.find("SECRET_RTSP_ENV_NAME") == std::string::npos,
        "Camera detail must return the durable algorithm definition without secret metadata");

    response = controller.updateTask(request(R"({"frame_interval_ms":500})"), camera_id);
    require(response.code == 428 && responseBody(response)["error_code"] == "PRECONDITION_REQUIRED",
        "Camera PATCH must require If-Match");
    auto patch_request = request(R"({"frame_interval_ms":500})");
    patch_request.add_header("If-Match", "\"99\"");
    response = controller.updateTask(patch_request, camera_id);
    require(response.code == 409 && control->stops.empty(),
        "a stale Camera PATCH must not stop the active extraction generation");

    patch_request = request(R"({"frame_interval_ms":500,"jpeg_quality":70})");
    patch_request.add_header("If-Match", "\"1\"");
    response = controller.updateTask(patch_request, camera_id);
    require(response.code == 202 && response.get_header_value("ETag") == "\"2\"" &&
            control->stops.size() == 1 && control->stops.front() == first_run_id &&
            control->submitted.size() == 2,
        "updating an active Camera must stop the old generation and submit its replacement");
    const std::string replacement_run_id = responseBody(response)["run_id"];
    require(replacement_run_id != first_run_id &&
            control->submitted.back().task_id == camera_id &&
            control->submitted.back().frame_interval_ms == 500 &&
            control->submitted.back().jpeg_quality == 70,
        "replacement generation must keep camera_id and use the new definition");

    auto start_request = request();
    start_request.add_header("Idempotency-Key", "start-entrance-001");
    response = controller.startTask(start_request, camera_id);
    require(response.code == 200 && responseBody(response)["idempotent_replay"] == true &&
            responseBody(response)["run_id"] == replacement_run_id && control->submitted.size() == 2,
        "explicit duplicate start must not create another extraction thread");
    response = controller.startTask(start_request, camera_id);
    require(response.code == 200 &&
            response.get_header_value("X-Idempotent-Replay") == "true" &&
            response.get_header_value("ETag") == "\"2\"" &&
            responseBody(response)["run_id"] == replacement_run_id &&
            control->submitted.size() == 2,
        "same start idempotency key must replay the durable response");

    control->fail_run_status = true;
    response = controller.taskStatus(request(), camera_id);
    require(response.code == 200 && responseBody(response)["runtime_stale"] == true &&
            responseBody(response)["status"] == "queued",
        "Redis status failure must fall back to PostgreSQL and mark runtime stale");
    require(response.body.find("redis://") == std::string::npos &&
            response.body.find("secret") == std::string::npos,
        "runtime fallback must not disclose connection errors");
    control->fail_run_status = false;

    CameraTaskRunHotStatus hot;
    hot.found = true;
    hot.run_id = replacement_run_id;
    hot.task_id = camera_id;
    hot.status = "running";
    hot.camera_profile = enabled.id;
    hot.hub_instance_id = "hub_entry_1";
    hot.hub_state = "running";
    hot.capture_backend = "FFMPEG";
    hot.last_update_ms = nowMs();
    hot.latest_frame_age_ms = 12;
    hot.pipeline_thread_running = true;
    hot.pipeline_started_at_ms = nowMs() - 1000;
    hot.sample_fps = 2.0;
    hot.sampled_frames = 8;
    hot.save_fps = 2.0;
    hot.consumed_frames = 8;
    hot.saved_frames = 4;
    hot.skipped_frames = 4;
    hot.last_source_sequence = 55;
    hot.last_frame_time_ms = nowMs();
    control->run_status[replacement_run_id] = hot;
    response = controller.taskStatus(request(), camera_id);
    require(response.code == 200 && responseBody(response)["runtime_stale"] == false &&
            responseBody(response)["status"] == "running" &&
            responseBody(response)["desired_state"] == "running" &&
            responseBody(response)["analysis"]["enabled"] == true &&
            responseBody(response)["pipeline"]["thread_running"] == true &&
            responseBody(response)["pipeline"]["sampled_frames"] == 8 &&
            responseBody(response)["hub"]["open_count"] == 1,
        "Camera status must merge desired algorithm configuration with Redis hot state");
    require(responseBody(response)["hub"]["last_error"] == "camera capture error",
        "Hub diagnostics must sanitize URI-shaped capture errors");

    response = controller.listRuns(request({}, true, "?limit=20&offset=0"), camera_id);
    require(response.code == 200 && responseBody(response)["items"].size() == 2 &&
            responseBody(response)["items"][0]["camera_id"] == camera_id,
        "Run history is a read-only internal audit view beneath a Camera");

    SecurityAlertEventRecord alert;
    alert.event_id = "evt_contract_55";
    alert.task_id = camera_id;
    alert.run_id = replacement_run_id;
    alert.camera_profile = enabled.id;
    alert.event_type = "ppe_violation";
    alert.category = "safety";
    alert.severity = 4;
    alert.confidence = 0.91;
    alert.track_id = 22;
    alert.occurred_at_ms = nowMs();
    alert.algorithm_profile = "security_default";
    alert.model_name = "ppe_yolo11";
    alert.config_version = "cfg_1";
    alert.payload_json = R"({"missing":["helmet"]})";
    alert.fingerprint = "entrance_extract_01:ppe_violation:22:55";
    alert.created_at_ms = alert.occurred_at_ms;
    std::string alert_code;
    require(repository->insertAlert(alert, alert_code, error),
        "HTTP contract fixture alert must persist: " + error);
    response = controller.listAlerts(
        request({}, true, "?event_type=ppe_violation&minimum_severity=4&limit=20"), camera_id);
    require(response.code == 200 && responseBody(response)["items"].size() == 1 &&
            responseBody(response)["items"][0]["event_id"] == alert.event_id &&
            responseBody(response)["items"][0]["delivery"]["status"] == "not_scheduled",
        "Camera alert query must expose the normalized event and delivery state");

    const auto latest_directory = root / "output" / std::filesystem::u8path(camera_id);
    std::filesystem::create_directories(latest_directory);
    {
        std::ofstream latest(latest_directory / "latest.jpg", std::ios::binary);
        const unsigned char jpeg[] = { 0xff, 0xd8, 0xff, 0xd9 };
        latest.write(reinterpret_cast<const char*>(jpeg), sizeof(jpeg));
    }
    response = controller.latestFrame(request(), camera_id);
    require(response.code == 200 && response.get_header_value("Content-Type") == "image/jpeg" &&
            response.body.size() == 4,
        "latest-frame must serve a complete JPEG for the Camera");

    response = controller.listHubs(request());
    require(response.code == 200 && responseBody(response)["items"].size() == 1,
        "Hub list must expose shared decode state");
    response = controller.operationsMetrics(request());
    require(response.code == 200 && responseBody(response)["profiles"]["active_hubs"] == 1 &&
            responseBody(response)["alerts"]["total"] == 1 &&
            responseBody(response)["invariants"]["subscriber_count_matches_types"] == true,
        "operations metrics must merge PostgreSQL alert, filesystem, and shared-Hub state");

    auto delete_request = request();
    delete_request.add_header("If-Match", "\"99\"");
    response = controller.deleteTask(delete_request, camera_id);
    require(response.code == 409 && control->stops.size() == 1,
        "a stale Camera DELETE must not stop the active extraction generation");
    delete_request = request();
    delete_request.add_header("If-Match", "\"2\"");
    response = controller.deleteTask(delete_request, camera_id);
    require(response.code == 202 && responseBody(response)["deleted"] == true &&
            responseBody(response)["status"] == "stopping" && control->stops.size() == 2 &&
            control->stops.back() == replacement_run_id,
        "deleting an active Camera must stop its extraction generation and soft-delete it");
    response = controller.getTask(request(), camera_id);
    require(response.code == 404, "a soft-deleted Camera must disappear from normal reads");

    response = controller.createTask(request(
        R"({"camera_id":"disabled_source","name":"disabled","camera_profile":"disabled_camera"})"));
    require(response.code == 409 && responseBody(response)["error_code"] == "CAMERA_PROFILE_DISABLED",
        "disabled profiles must be rejected without resolving their URI");

    response = controller.createTask(request(
        R"({"camera_id":"idle_camera","name":"idle","camera_profile":"entry_camera_01","enabled":false})"));
    require(response.code == 201 && responseBody(response)["camera"]["camera_id"] == "idle_camera" &&
            control->submitted.size() == 2,
        "a disabled Camera definition must remain stopped and create no Run");
    delete_request = request();
    delete_request.add_header("If-Match", "\"1\"");
    response = controller.deleteTask(delete_request, "idle_camera");
    require(response.code == 204, "deleting an idle Camera must complete synchronously");

    control->fail_submit = true;
    response = controller.createTask(request(
        R"({"camera_id":"queue_failure","name":"queue failure","camera_profile":"entry_camera_01"})"));
    require(response.code == 503 && responseBody(response)["error_code"] == "QUEUE_SUBMIT_FAILED" &&
            response.body.find("127.0.0.1") == std::string::npos,
        "automatic extraction submission failure must be durable and sanitized");
    control->fail_submit = false;
    response = controller.listRuns(request(), "queue_failure");
    require(response.code == 200 && responseBody(response)["items"].size() == 1 &&
            responseBody(response)["items"][0]["status"] == "failed",
        "a failed automatic start must retain one terminal Run audit record");

    std::string oversized_name(129, 'x');
    response = controller.createTask(request(json({
        {"camera_id", "oversized"}, {"name", oversized_name},
        {"camera_profile", "entry_camera_01"}
    }).dump()));
    require(response.code == 400 && responseBody(response)["error_code"] == "INVALID_TASK_CONFIG",
        "oversized Camera names must be rejected");

    AppConfig invalid_worker_config = config;
    invalid_worker_config.worker.worker_num = 2;
    invalid_worker_config.camera_tasks.output_dir = pathUtf8(root / "invalid_worker" / "output");
    auto invalid_repository = std::make_shared<CameraTaskRepository>(invalid_worker_config.camera_tasks);
    CameraTaskHttpController invalid_worker_controller(
        invalid_worker_config, invalid_repository, control, "contract-secret");
    require(invalid_worker_controller.initialize(error),
        "invalid-worker controller initializes so readiness can expose the failure");
    require(!invalid_worker_controller.health().worker_num_valid,
        "worker_num greater than one must remain an explicit readiness failure");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::cout << "Camera ID lifecycle HTTP contract tests passed\n";
    return 0;
}
