#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "business/postgres_client.h"
#include "business/camera_task_repository.h"
#include "postgres_test_guard.h"

namespace {

using namespace yolo11_server;

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

std::string sourceFile(const std::string& relative_path) {
    const std::string path = std::string(YOLO11_SOURCE_DIR) + "/" + relative_path;
    std::ifstream input(path, std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    require(input.good() || input.eof(), "migration file must be readable: " + path);
    return content.str();
}

const char* resetCameraSchemaSql() {
    return
        "DROP TABLE IF EXISTS camera_run_analysis_results,callback_outbox,"
        "security_alert_events,camera_idempotency_keys,camera_frames,"
        "camera_task_runs,camera_tasks,camera_schema_version CASCADE;";
}

CameraTaskDefinition taskDefinition(const std::string& id, long long now) {
    CameraTaskDefinition task;
    task.task_id = id;
    task.name = "Entrance snapshots";
    task.camera_profile = "entry_camera_01";
    task.enabled = true;
    task.frame_interval_ms = 1000;
    task.output_mode = "both";
    task.jpeg_quality = 90;
    task.max_width = 1920;
    task.max_height = 1080;
    task.retention_days = 7;
    task.max_saved_frames = 1000;
    task.desired_state = "running";
    task.analysis_enabled = true;
    task.target_infer_fps = 6.5;
    task.algorithm_profile = "security_default";
    task.algorithms = { "people_flow", "ppe_detection" };
    task.callback_profile = "backend_primary";
    task.version = 1;
    task.created_at_ms = now;
    task.updated_at_ms = now;
    return task;
}

CameraTaskRunRecord runRecord(
    const std::string& id,
    const CameraTaskDefinition& task,
    long long now
) {
    CameraTaskRunRecord run;
    run.run_id = id;
    run.task_id = task.task_id;
    run.definition_version = task.version;
    run.definition_json = "{\"camera_profile\":\"entry_camera_01\",\"frame_interval_ms\":1000}";
    run.status = "queued";
    run.camera_profile = task.camera_profile;
    run.create_time_ms = now;
    run.last_update_ms = now;
    return run;
}

}  // namespace

int main() {
    if (const int guard = requireDisposablePostgresTestDatabase()) return guard;
    const long long stamp = nowMs();
    CameraTasksSection config;
    config.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN";
    PostgresConnection database;
    std::string error;
    require(database.openFromEnvironment(config.postgres_dsn_env, error),
        "test PostgreSQL connection must open: " + error);
    require(database.exec(resetCameraSchemaSql(), error),
        "test PostgreSQL schema reset must succeed: " + error);

    // Exercise the documented 001 -> 002 -> 003 upgrade path before testing a
    // clean database. The legacy row must survive and receive additive defaults.
    require(database.exec(sourceFile("db/postgresql/001_initial_schema.sql"), error),
        "001 migration must apply: " + error);
    require(database.exec(sourceFile("db/postgresql/002_algorithm_service_contract.sql"), error),
        "002 migration must apply: " + error);
    require(database.exec(
        "INSERT INTO camera_tasks("
        "task_id,name,camera_profile,enabled,frame_interval_ms,output_mode,jpeg_quality,"
        "max_width,max_height,retention_days,max_saved_frames,desired_state,"
        "analysis_enabled,target_infer_fps,algorithm_profile,algorithms_json,"
        "callback_profile,version,created_at_ms,updated_at_ms) VALUES("
        "'ct_legacy','Legacy camera','entry_camera_01',1,1000,'latest',90,"
        "0,0,7,1000,'running',0,5,'','[]'::jsonb,'',1,1,1);"
        "INSERT INTO camera_task_runs("
        "run_id,task_id,definition_version,definition_json,status,camera_profile,"
        "create_time_ms,last_update_ms) VALUES("
        "'cr_legacy','ct_legacy',1,'{}','stopped','entry_camera_01',1,1);",
        error), "legacy 002 rows must be created: " + error);
    CameraTaskRepository repository(config);
    std::string code;
    require(repository.initialize(error), "003 repository upgrade must succeed: " + error);
    auto migrated = database.prepare(
        "SELECT origin,legacy_session_id,analysis_config_version "
        "FROM camera_task_runs WHERE run_id='cr_legacy';", error);
    require(migrated != nullptr && migrated->step() == PG_STEP_ROW &&
            migrated->columnText(0) == "camera_api" &&
            migrated->columnIsNull(1) && migrated->columnIsNull(2),
        "002 Run must survive 003 with rollback-safe defaults");
    auto version = database.prepare(
        "SELECT COUNT(*) FROM camera_schema_version WHERE version=3;", error);
    require(version != nullptr && version->step() == PG_STEP_ROW &&
            version->columnInt64(0) == 1,
        "003 schema version must be recorded exactly once");
    require(database.exec(
            sourceFile("db/postgresql/003_people_flow_camera_unification.sql"), error) &&
            repository.initialize(error),
        "003 migration and repository initialization must be idempotent: " + error);

    require(database.exec(resetCameraSchemaSql(), error),
        "post-migration test reset must succeed: " + error);
    require(repository.initialize(error), "clean schema initialization must succeed: " + error);

    CameraTaskDefinition task = taskDefinition("ct_alpha", stamp);
    require(repository.createTask(task, code, error), "task creation must succeed: " + error);
    CameraTaskDefinition loaded;
    bool found = false;
    require(repository.getTask(task.task_id, false, loaded, found, error) && found,
        "created task must be readable");
    require(loaded.version == 1 && loaded.output_mode == "both" &&
            loaded.desired_state == "running" && loaded.analysis_enabled &&
            loaded.target_infer_fps == 6.5 &&
            loaded.algorithm_profile == "security_default" &&
            loaded.algorithms == std::vector<std::string>({ "people_flow", "ppe_detection" }) &&
            loaded.callback_profile == "backend_primary",
        "created task and algorithm contract fields must persist");

    CameraTaskPatch patch;
    patch.frame_interval_ms = 2500;
    CameraTaskDefinition updated;
    require(!repository.updateTask(task.task_id, 99, patch, stamp + 1, updated, code, error) &&
        code == "TASK_VERSION_CONFLICT", "wrong If-Match version must conflict");
    require(repository.updateTask(task.task_id, 1, patch, stamp + 2, updated, code, error),
        "matching version update must succeed: " + error);
    require(updated.version == 2 && updated.frame_interval_ms == 2500,
        "update must increment version and persist patch");

    CameraTaskRunRecord run = runRecord("cr_alpha_1", updated, stamp + 3);
    run.analysis_config_version = "entry-line-v3";
    require(repository.createRun(run, code, error), "first active run must be created: " + error);
    CameraTaskRunRecord loaded_run;
    require(repository.getRun(run.run_id, loaded_run, found, error) && found &&
            loaded_run.origin == "camera_api" &&
            loaded_run.analysis_config_version == "entry-line-v3" &&
            loaded_run.legacy_session_id.empty(),
        "additive Run unification metadata must persist");

    CameraRunAnalysisResultRecord analysis_result;
    analysis_result.run_id = run.run_id;
    analysis_result.task_id = run.task_id;
    analysis_result.initial_occupancy = 4;
    analysis_result.in_count = 3;
    analysis_result.out_count = 1;
    analysis_result.final_occupancy = 6;
    analysis_result.last_live_persons = 2;
    analysis_result.security_state_json = R"({"phase1":"safe"})";
    analysis_result.snapshot_relative_path = "ct_alpha/latest/analysis.jpg";
    analysis_result.storage_degraded = true;
    analysis_result.last_update_ms = stamp + 3;
    require(repository.upsertRunAnalysisResult(analysis_result, error),
        "analysis result must persist in the additive R2 table: " + error);
    CameraRunAnalysisResultRecord loaded_analysis;
    require(repository.getRunAnalysisResult(
            run.run_id, loaded_analysis, found, error) && found &&
            loaded_analysis.initial_occupancy == 4 &&
            loaded_analysis.final_occupancy == 6 &&
            loaded_analysis.storage_degraded &&
            nlohmann::json::parse(loaded_analysis.security_state_json)["phase1"] == "safe",
        "analysis result must round-trip without changing Camera runtime behavior");
    CameraTaskRunRecord duplicate = runRecord("cr_alpha_2", updated, stamp + 4);
    require(!repository.createRun(duplicate, code, error) && code == "ACTIVE_RUN_EXISTS",
        "partial unique index must reject a second active run");
    patch.frame_interval_ms = 3000;
    require(repository.updateTask(task.task_id, 2, patch, stamp + 5, updated, code, error) &&
        updated.version == 3,
        "active camera definition must be mutable so the API can replace its extraction thread");

    CameraTaskRunRecord starting = run;
    starting.status = "starting";
    starting.start_time_ms = stamp + 6;
    starting.last_update_ms = stamp + 6;
    starting.worker_consumer = "vision_worker_1_camera";
    require(repository.transitionRun(run.run_id, {"queued"}, starting, code, error),
        "queued run must transition to starting");
    CameraTaskRunRecord running = starting;
    running.status = "running";
    running.hub_instance_id = "hub_entry_1";
    running.capture_backend = "FFMPEG";
    running.saved_frames = 1;
    running.last_source_sequence = 42;
    running.last_update_ms = stamp + 7;
    require(repository.transitionRun(run.run_id, {"starting"}, running, code, error),
        "starting run must transition to running");

    CameraFrameArtifact frame;
    frame.frame_id = "cf_alpha_42";
    frame.task_id = task.task_id;
    frame.run_id = run.run_id;
    frame.source_sequence = 42;
    frame.capture_time_ms = stamp + 7;
    frame.save_time_ms = stamp + 8;
    frame.relative_path = "ct_alpha/archive/cr_alpha_1/2026/07/20/frame_42.jpg";
    frame.width = 1920;
    frame.height = 1080;
    frame.size_bytes = 12345;
    require(repository.insertFrame(frame, error), "frame metadata must persist: " + error);
    CameraFrameArtifact latest;
    require(repository.getLatestFrame(task.task_id, latest, found, error) && found &&
        latest.source_sequence == 42, "latest frame query must return persisted metadata");

    SecurityAlertEventRecord alert;
    alert.event_id = "evt_alpha_42";
    alert.task_id = task.task_id;
    alert.run_id = run.run_id;
    alert.camera_profile = task.camera_profile;
    alert.event_type = "ppe_violation";
    alert.category = "safety";
    alert.severity = 4;
    alert.confidence = 0.93;
    alert.track_id = 17;
    alert.occurred_at_ms = stamp + 8;
    alert.algorithm_profile = "security_default";
    alert.model_name = "ppe_yolo11";
    alert.config_version = "cfg_1";
    alert.payload_json = R"({"missing":["helmet"]})";
    alert.evidence_frame_id = frame.frame_id;
    alert.fingerprint = "ct_alpha:ppe_violation:17:42";
    alert.created_at_ms = stamp + 8;
    require(repository.insertAlert(alert, task.callback_profile, code, error),
        "security alert and callback outbox must persist atomically: " + error);
    SecurityAlertEventRecord loaded_alert;
    require(repository.getAlert(alert.event_id, loaded_alert, found, error) && found &&
            loaded_alert.event_type == "ppe_violation" &&
            loaded_alert.confidence && *loaded_alert.confidence == 0.93 &&
            loaded_alert.delivery_status == "pending",
        "security alert detail must preserve contract and delivery state");
    auto outbox = database.prepare(
        "SELECT callback_profile,status,attempt FROM callback_outbox WHERE event_id=?;", error);
    require(outbox != nullptr, "callback outbox query must prepare: " + error);
    outbox->bindText(1, alert.event_id);
    require(outbox->step() == PG_STEP_ROW &&
            outbox->columnText(0) == task.callback_profile &&
            outbox->columnText(1) == "pending" &&
            outbox->columnInt(2) == 0,
        "alert transaction must create one pending callback outbox row");
    std::vector<SecurityAlertEventRecord> alerts;
    require(repository.listAlerts(task.task_id, "ppe_violation", 4, 20, 0, alerts, error) &&
            alerts.size() == 1 && alerts.front().event_id == alert.event_id,
        "security alerts must support camera/type/severity queries");
    SecurityAlertEventRecord duplicate_alert = alert;
    duplicate_alert.event_id = "evt_alpha_duplicate";
    require(!repository.insertAlert(duplicate_alert, code, error) &&
            code == "ALERT_ALREADY_EXISTS",
        "camera/fingerprint uniqueness must suppress duplicate alerts");

    CameraIdempotencyRecord idempotency;
    idempotency.operation_scope = "camera.create";
    idempotency.idempotency_key = "create-alpha-001";
    idempotency.request_digest = "0123456789abcdef";
    idempotency.resource_id = task.task_id;
    idempotency.response_status = 202;
    idempotency.response_json = R"({"camera_id":"ct_alpha","status":"queued"})";
    idempotency.created_at_ms = stamp + 8;
    idempotency.expires_at_ms = stamp + 86400000;
    require(repository.storeIdempotencyRecord(idempotency, code, error),
        "idempotency result must persist: " + error);
    CameraIdempotencyRecord loaded_idempotency;
    require(repository.getIdempotencyRecord(
            idempotency.operation_scope, idempotency.idempotency_key,
            loaded_idempotency, found, error) && found &&
            loaded_idempotency.request_digest == idempotency.request_digest &&
            loaded_idempotency.response_status == 202,
        "idempotency replay metadata must be readable");
    require(!repository.storeIdempotencyRecord(idempotency, code, error) &&
            code == "IDEMPOTENCY_KEY_EXISTS",
        "an idempotency key cannot be overwritten");

    CameraTaskRepositoryStats stats;
    require(repository.stats(stats, error) && stats.alerts_total == 1 &&
            stats.callbacks_pending == 1 &&
            stats.callbacks_delivering == 0 &&
            stats.callbacks_delivered == 0 &&
            stats.callbacks_retry == 0 &&
            stats.callbacks_dead_letter == 0,
        "repository metrics must include alert and durable callback state");

    CameraTaskRunRecord stopped = running;
    stopped.status = "stopped";
    stopped.stop_time_ms = stamp + 9;
    stopped.last_update_ms = stamp + 9;
    stopped.stop_reason = "requested";
    require(repository.transitionRun(run.run_id, {"running"}, stopped, code, error),
        "running run must transition to stopped");
    require(repository.updateTask(task.task_id, 3, patch, stamp + 10, updated, code, error) &&
        updated.version == 4, "terminal run must remain updateable");
    require(!repository.softDeleteTask(task.task_id, 3, stamp + 11, code, error) &&
        code == "TASK_VERSION_CONFLICT", "delete must enforce optimistic version");
    require(repository.softDeleteTask(task.task_id, 4, stamp + 12, code, error),
        "matching version soft delete must succeed");
    require(repository.getTask(task.task_id, false, loaded, found, error) && !found,
        "soft-deleted task must be hidden by default");
    require(repository.getTask(task.task_id, true, loaded, found, error) && found &&
        loaded.deleted_at_ms.has_value(), "audit read must retain soft-deleted task");

    CameraTaskDefinition stale_task = taskDefinition("ct_stale", stamp - 100000);
    require(repository.createTask(stale_task, code, error), "stale test task must be created");
    CameraTaskRunRecord stale_run = runRecord("cr_stale_1", stale_task, stamp - 100000);
    require(repository.createRun(stale_run, code, error), "stale test run must be created");
    int recovered = 0;
    require(repository.recoverStaleRuns(stamp - 5000, stamp + 20, recovered, error) && recovered == 1,
        "stale nonterminal run must be recovered once");
    require(repository.getRun(stale_run.run_id, running, found, error) && found &&
        running.status == "failed" && running.error_code == "WORKER_HEARTBEAT_STALE",
        "stale recovery terminal state must survive queries");
    CameraTaskRunRecord recovered_generation =
        runRecord("cr_stale_recovered", stale_task, stamp + 21);
    require(repository.createRun(recovered_generation, code, error),
        "a desired-running camera must allow exactly one generation after stale recovery");
    CameraTaskRunRecord duplicate_recovery =
        runRecord("cr_stale_recovered_duplicate", stale_task, stamp + 22);
    require(!repository.createRun(duplicate_recovery, code, error) &&
            code == "ACTIVE_RUN_EXISTS",
        "stale recovery must not permit two replacement generations");

    CameraTaskRepository reopened(config);
    require(reopened.initialize(error), "reopened repository must validate schema");
    require(reopened.getRun(run.run_id, running, found, error) && found && running.status == "stopped",
        "terminal history must survive repository restart");

    CameraTasksSection unavailable_config = config;
    unavailable_config.postgres_dsn_env = "YOLO11_TEST_POSTGRES_DSN_MISSING";
    CameraTaskRepository unavailable(unavailable_config);
    error.clear();
    require(!unavailable.initialize(error) && !error.empty(),
        "an unavailable PostgreSQL target must fail initialization explicitly");

    std::cout << "PostgreSQL camera repository tests passed\n";
    return 0;
}
