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
#include "server/people_flow_compatibility_controller.h"
#include "server/uri_masker.h"
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

json body(const crow::response& response) {
    return json::parse(response.body);
}

crow::request request(const std::string& payload = {}) {
    crow::request result;
    result.body = payload;
    return result;
}

class FakeControl final : public ICameraTaskApiControl {
public:
    bool submitStart(
        const CameraTaskCommand& command,
        std::string& error
    ) override {
        std::lock_guard<std::mutex> lock(mutex);
        submitted.push_back(command);
        error.clear();
        return true;
    }

    bool requestStop(
        const std::string& run_id,
        std::string& error
    ) override {
        std::lock_guard<std::mutex> lock(mutex);
        stopped.push_back(run_id);
        error.clear();
        return true;
    }

    bool getRunStatus(
        const std::string& run_id,
        CameraTaskRunHotStatus& status,
        std::string& error
    ) override {
        std::lock_guard<std::mutex> lock(mutex);
        status = run_status[run_id];
        error.clear();
        return true;
    }

    bool getHubStatus(
        const std::string& profile,
        CameraHubHotStatus& status,
        std::string& error
    ) override {
        std::lock_guard<std::mutex> lock(mutex);
        status = hubs[profile];
        error.clear();
        return true;
    }

    std::vector<CameraTaskCommand> submitted;
    std::vector<std::string> stopped;
    std::map<std::string, CameraTaskRunHotStatus> run_status;
    std::map<std::string, CameraHubHotStatus> hubs;
    std::mutex mutex;
};

void normalizeSession(json& value) {
    const std::string session_id = value.at("session_id").get<std::string>();
    value["session_id"] = "<session>";
    for (const char* field : {
            "status_url", "snapshot_url", "security_url", "stop_url"}) {
        std::string url = value.at(field).get<std::string>();
        const auto at = url.find(session_id);
        require(at != std::string::npos, "session URL must contain session_id");
        url.replace(at, session_id.size(), "<session>");
        value[field] = url;
    }
}

}  // namespace

int main() {
    if (const int guard = requireDisposablePostgresTestDatabase()) {
        return guard;
    }

    const auto root = std::filesystem::temp_directory_path() /
        ("people_flow_compat_" + std::to_string(nowMs()));
    std::filesystem::create_directories(root);

    AppConfig config;
    config.camera_tasks.enabled = true;
    config.camera_tasks.postgres_dsn_env =
        "YOLO11_TEST_POSTGRES_DSN";
    config.camera_tasks.output_dir = pathUtf8(root / "camera-output");
    config.camera_tasks.defaults.retention_days = 7;
    config.camera_tasks.defaults.max_saved_frames = 1000;
    config.analysis.enabled = true;
    config.analysis.supported_algorithms = {
        "people_flow", "security", "electronic_fence",
        "pose_action", "temporal_action"
    };
    config.people_flow.enabled = true;
    config.people_flow.storage.postgres_dsn_env =
        "YOLO11_TEST_POSTGRES_DSN";
    config.people_flow.camera_id = "entry_camera_01";
    config.people_flow.camera_profile = "entry_camera_01";
    config.people_flow.config_version = "entry-line-v1";
    config.people_flow.target_infer_fps = 10;
    config.people_flow.snapshot_fps = 2;
    config.people_flow.initial_occupancy = 4;
    config.people_flow.security.enabled = true;
    config.runtime.unified_camera_pipeline = true;
    config.runtime.people_flow_compatibility = true;
    config.runtime.legacy_people_flow_fallback = true;

    CameraProfile profile;
    profile.id = config.people_flow.camera_profile;
    profile.url_env = "YOLO11_TEST_COMPAT_RTSP";
    profile.enabled = true;
    config.camera_profiles[profile.id] = profile;
#ifdef _WIN32
    _putenv_s(
        profile.url_env.c_str(),
        "rtsp://camera-user:camera-pass@127.0.0.1/entry");
#else
    setenv(
        profile.url_env.c_str(),
        "rtsp://camera-user:camera-pass@127.0.0.1/entry",
        1);
#endif

    PostgresConnection database;
    std::string error;
    require(
        database.openFromEnvironment(
            config.camera_tasks.postgres_dsn_env, error),
        "test PostgreSQL connection must open: " + error);
    require(database.exec(
        "DROP TABLE IF EXISTS pf_calibration_audit,pf_aggregates_minute,"
        "pf_crossing_events,pf_sessions,schema_version,"
        "camera_run_analysis_results,callback_outbox,"
        "security_alert_events,camera_idempotency_keys,camera_frames,"
        "camera_task_runs,camera_tasks,camera_schema_version CASCADE;",
        error),
        "test schemas must reset: " + error);

    PeopleFlowRepository legacy_repository(config.people_flow);
    require(
        legacy_repository.start(false, error),
        "legacy compatibility schema must initialize: " + error);
    auto repository =
        std::make_shared<CameraTaskRepository>(config.camera_tasks);
    require(
        repository->initialize(error),
        "canonical camera schema must initialize: " + error);
    auto control = std::make_shared<FakeControl>();
    auto service = std::make_shared<UnifiedCameraApplicationService>(
        config, repository, control);
    PeopleFlowCompatibilityController controller(
        config,
        repository,
        control,
        {},
        service,
        &legacy_repository);

    const auto start_response = controller.start(request(R"({
        "camera_profile":"entry_camera_01",
        "camera_id":"entry_camera_01",
        "config_version":"entry-line-v1",
        "initial_occupancy":4
    })"));
    require(
        start_response.code == 202,
        "legacy start must remain accepted: " + start_response.body);
    json start_actual = body(start_response);
    const std::string session_id =
        start_actual.at("session_id").get<std::string>();
    require(
        session_id.rfind("pf_", 0) == 0,
        "compatibility Run must retain the pf_ session namespace");
    normalizeSession(start_actual);
    const json start_golden = {
        {"success", true}, {"session_id", "<session>"},
        {"camera_id", "entry_camera_01"},
        {"camera_profile", "entry_camera_01"},
        {"config_version", "entry-line-v1"},
        {"status", "queued"},
        {"status_url",
            "/api/v1/people-flow/<session>/status"},
        {"snapshot_url",
            "/api/v1/people-flow/<session>/snapshot"},
        {"security_url",
            "/api/v1/people-flow/<session>/security"},
        {"stop_url",
            "/api/v1/people-flow/<session>/stop"}
    };
    require(
        start_actual == start_golden,
        "legacy start response Golden diff must be zero");
    require(
        control->submitted.size() == 1 &&
            control->submitted.front().run_id == session_id &&
            control->submitted.front().origin ==
                kCameraRunOriginPeopleFlowCompat &&
            control->submitted.front().legacy_session_id == session_id &&
            control->submitted.front().analysis_enabled &&
            control->submitted.front().algorithms ==
                std::vector<std::string>({"people_flow", "security"}) &&
            control->submitted.front().initial_occupancy == 4 &&
            control->submitted.front().snapshot_fps == 2,
        "legacy start must submit one immutable unified Camera RunSpec");

    const auto duplicate_start = controller.start(request("{}"));
    require(
        duplicate_start.code == 409 &&
            body(duplicate_start).at("error_code") ==
                "CAMERA_ALREADY_ACTIVE",
        "duplicate legacy start must retain CAMERA_ALREADY_ACTIVE");
    const auto queued_status = controller.status(session_id);
    require(
        queued_status.code == 200 &&
            body(queued_status)["status"] == "queued" &&
            body(queued_status)["flow"]["initial_occupancy"] == 4 &&
            body(queued_status)["flow"]["occupancy"] == 4,
        "queued compatibility status must retain requested occupancy");

    CameraTaskRunRecord run;
    bool found = false;
    require(
        repository->getRunByLegacySessionId(
            session_id, run, found, error) &&
            found && run.run_id == session_id &&
            run.origin == kCameraRunOriginPeopleFlowCompat,
        "session_id must resolve to the same canonical Camera Run");
    CameraTaskRunRecord running = run;
    running.status = "running";
    running.start_time_ms = run.create_time_ms + 10;
    running.last_update_ms = run.create_time_ms + 10;
    running.hub_instance_id = "hub-entry-1";
    running.capture_backend = "ffmpeg";
    running.width = 1280;
    running.height = 720;
    std::string error_code;
    require(
        repository->transitionRun(
            run.run_id, {"queued"}, running, error_code, error),
        "compatibility Run must transition to running: " + error);
    run = running;

    CameraTaskRunHotStatus hot;
    hot.found = true;
    hot.run_id = run.run_id;
    hot.task_id = run.task_id;
    hot.status = "running";
    hot.camera_profile = run.camera_profile;
    hot.hub_instance_id = "hub-entry-1";
    hot.hub_state = "running";
    hot.capture_backend = "ffmpeg";
    hot.last_update_ms = run.last_update_ms + 20;
    hot.latest_frame_age_ms = 35;
    hot.consumed_frames = 12;
    hot.dropped_frames = 1;
    hot.writer_queue_depth = 2;
    hot.analysis_config_version = config.people_flow.config_version;
    hot.infer_fps = 9.5;
    hot.last_inference_ms = 3.25;
    hot.analysis_frame_count = 10;
    hot.initial_occupancy = 4;
    hot.in_count = 3;
    hot.out_count = 1;
    hot.occupancy = 6;
    hot.live_persons = 2;
    hot.analysis_reconnect_count = 1;
    hot.security_state_json = R"({
        "schema_version":"1.0",
        "stages":{
            "phase1":{"ready":true},
            "phase2":{"ready":true},
            "phase3":{"ready":true},
            "phase4":{"ready":true,"demo_classifier":true}
        },
        "events":[]
    })";
    hot.analysis_snapshot_relative_path =
        run.task_id + "/" + run.run_id + "/analysis/latest.jpg";
    hot.analysis_last_update_ms = run.last_update_ms + 20;
    control->run_status[run.run_id] = hot;

    CameraHubHotStatus hub;
    hub.found = true;
    hub.last_update_ms = hot.last_update_ms;
    hub.snapshot.hub_instance_id = "hub-entry-1";
    hub.snapshot.camera_profile = run.camera_profile;
    hub.snapshot.state = "running";
    hub.snapshot.backend_name = "ffmpeg";
    hub.snapshot.subscriber_count = 1;
    hub.snapshot.subscriber_types["camera_pipeline"] = 1;
    hub.snapshot.capture_fps = 24.5;
    hub.snapshot.source_fps = 25.0;
    hub.snapshot.latest_frame_age_ms = 35;
    hub.snapshot.width = 1280;
    hub.snapshot.height = 720;
    control->hubs[run.camera_profile] = hub;

    CameraRunAnalysisResultRecord analysis;
    analysis.run_id = run.run_id;
    analysis.task_id = run.task_id;
    analysis.initial_occupancy = 4;
    analysis.in_count = 3;
    analysis.out_count = 1;
    analysis.final_occupancy = 6;
    analysis.last_live_persons = 2;
    analysis.security_state_json = hot.security_state_json;
    analysis.snapshot_relative_path =
        hot.analysis_snapshot_relative_path;
    analysis.last_update_ms = hot.analysis_last_update_ms;
    require(
        repository->upsertRunAnalysisResult(analysis, error),
        "analysis result and pf_sessions projection must persist: " +
            error);

    const auto status_response = controller.status(session_id);
    require(
        status_response.code == 200,
        "legacy status must read the unified Run: " +
            status_response.body);
    const json status_actual = body(status_response);
    const json status_golden = {
        {"success", true},
        {"session_id", session_id},
        {"camera_id", "entry_camera_01"},
        {"camera_profile", "entry_camera_01"},
        {"config_version", "entry-line-v1"},
        {"status", "running"},
        {"stop_requested", false},
        {"capture", {
            {"state", "running"}, {"backend", "ffmpeg"},
            {"shared_hub", true},
            {"hub_instance_id", "hub-entry-1"},
            {"hub_subscribers", 1}, {"capture_fps", 24.5},
            {"source_fps", 25.0}, {"frame_count", 12},
            {"dropped_frames", 1}, {"latest_frame_age_ms", 35},
            {"width", 1280}, {"height", 720}
        }},
        {"inference", {
            {"infer_fps", 9.5}, {"last_inference_ms", 3.25},
            {"live_persons", 2}
        }},
        {"flow", {
            {"in", 3}, {"out", 1}, {"initial_occupancy", 4},
            {"occupancy", 6}
        }},
        {"storage", {
            {"degraded", false}, {"event_queue_depth", 2},
            {"snapshot_degraded", false}
        }},
        {"source", {
            {"profile", "entry_camera_01"},
            {"masked_uri", maskRtspUri(
                "rtsp://camera-user:camera-pass@127.0.0.1/entry")}
        }},
        {"create_time_ms", run.create_time_ms},
        {"start_time_ms", run.start_time_ms},
        {"stop_time_ms", 0},
        {"last_update_ms", hot.analysis_last_update_ms},
        {"snapshot_url",
            "/api/v1/people-flow/" + session_id + "/snapshot"},
        {"security_url",
            "/api/v1/people-flow/" + session_id + "/security"}
    };
    require(
        status_actual == status_golden,
        "legacy status response Golden diff must be zero");

    const auto realtime_response =
        controller.realtime(config.people_flow.camera_id);
    require(
        realtime_response.code == 200 &&
            body(realtime_response) == json({
                {"success", true},
                {"camera_id", "entry_camera_01"},
                {"session_id", session_id},
                {"status", "running"}, {"in", 3}, {"out", 1},
                {"occupancy", 6}, {"live_persons", 2},
                {"frame_count", 12}, {"capture_fps", 24.5},
                {"infer_fps", 9.5},
                {"last_inference_ms", 3.25},
                {"latest_frame_age_ms", 35}
            }),
        "legacy realtime response Golden diff must be zero");

    const auto snapshot_path =
        std::filesystem::u8path(config.camera_tasks.output_dir) /
        std::filesystem::u8path(hot.analysis_snapshot_relative_path);
    std::filesystem::create_directories(snapshot_path.parent_path());
    {
        std::ofstream output(snapshot_path, std::ios::binary);
        const char jpeg[] = {
            static_cast<char>(0xff), static_cast<char>(0xd8),
            'R', '4',
            static_cast<char>(0xff), static_cast<char>(0xd9)
        };
        output.write(jpeg, sizeof(jpeg));
    }
    auto snapshot_response = controller.snapshot(session_id);
    require(
        snapshot_response.code == 200 &&
            snapshot_response.get_header_value("Content-Type") ==
                "image/jpeg" &&
            snapshot_response.body.size() == 6,
        "legacy snapshot must return the unified annotated JPEG");
    const auto security_response = controller.security(session_id);
    require(
        security_response.code == 200 &&
            body(security_response).at("stages").size() == 4 &&
            body(security_response)["stages"]["phase4"]
                .at("demo_classifier").get<bool>(),
        "legacy security must project all four unified stages");

    SecurityAlertEventRecord alert;
    alert.event_id = "ae_people_flow_r4";
    alert.task_id = run.task_id;
    alert.run_id = run.run_id;
    alert.camera_profile = run.camera_profile;
    alert.event_type = "PEOPLE_FLOW_IN";
    alert.category = "people_flow";
    alert.severity = 1;
    alert.confidence = 0.91;
    alert.track_id = 42;
    alert.occurred_at_ms = run.create_time_ms + 100;
    alert.algorithm_profile = "people_flow_compat";
    alert.model_name = "pose";
    alert.config_version = config.people_flow.config_version;
    alert.payload_json = R"({
        "line_id":"main",
        "point_x_norm":0.4,
        "point_y_norm":0.6
    })";
    alert.fingerprint = "r4-people-flow-in-42";
    alert.created_at_ms = alert.occurred_at_ms;
    require(
        repository->insertAlert(alert, error_code, error),
        "canonical alert and legacy event projection must be atomic: " +
            error);
    const auto events_response = controller.events(
        request(), config.people_flow.camera_id);
    require(
        events_response.code == 200 &&
            body(events_response).at("count") == 1 &&
            body(events_response)["events"][0]["event_id"] ==
                alert.event_id &&
            body(events_response)["events"][0]["session_id"] ==
                session_id &&
            body(events_response)["events"][0]["direction"] == "IN",
        "new and old event reads must merge and deduplicate");

    const auto stop_response = controller.stop(session_id);
    require(
        stop_response.code == 200 &&
            body(stop_response) == json({
                {"success", true}, {"session_id", session_id},
                {"camera_id", "entry_camera_01"},
                {"status", "stopping"}, {"stop_requested", true}
            }),
        "legacy stop response Golden diff must be zero");
    CameraTaskRunRecord stopping;
    require(
        repository->getRun(
            run.run_id, stopping, found, error) &&
            found && stopping.status == "stopping",
        "legacy stop must operate on the same Camera Run");
    CameraTaskRunRecord stopped = stopping;
    stopped.status = "stopped";
    stopped.stop_time_ms = nowMs();
    stopped.last_update_ms = stopped.stop_time_ms;
    stopped.stop_reason = "requested";
    require(
        repository->transitionRun(
            run.run_id, {"stopping"}, stopped, error_code, error),
        "compatibility Run must reach stopped: " + error);
    const auto repeated_stop = controller.stop(session_id);
    require(
        repeated_stop.code == 409 &&
            body(repeated_stop).at("error_code") ==
                "SESSION_ALREADY_FINISHED",
        "terminal repeated stop must retain SESSION_ALREADY_FINISHED");

    auto projection = database.prepare(
        "SELECT status,in_count,out_count,final_occupancy "
        "FROM pf_sessions WHERE session_id=?;",
        error);
    require(projection != nullptr, "pf_sessions projection query must prepare");
    projection->bindText(1, session_id);
    require(
        projection->step() == PG_STEP_ROW &&
            projection->columnText(0) == "stopped" &&
            projection->columnInt64(1) == 3 &&
            projection->columnInt64(2) == 1 &&
            projection->columnInt64(3) == 6,
        "legacy pf_sessions projection must follow the canonical Run");

    legacy_repository.stop();
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::cout
        << "People Flow unified compatibility contract tests passed\n";
    return 0;
}
