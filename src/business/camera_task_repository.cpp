#include "business/camera_task_repository.h"

#include <algorithm>
#include <memory>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "business/postgres_client.h"

namespace yolo11_server {

namespace {

using json = nlohmann::json;

struct DbCloser {
    void operator()(PostgresConnection* db) const { delete db; }
};
struct StatementCloser {
    void operator()(PostgresStatement* statement) const { delete statement; }
};
using sqlite3 = PostgresConnection;
using sqlite3_stmt = PostgresStatement;
using sqlite3_int64 = long long;
using DbPtr = std::unique_ptr<PostgresConnection, DbCloser>;
using StatementPtr = std::unique_ptr<PostgresStatement, StatementCloser>;

constexpr int SQLITE_DONE = PG_STEP_DONE;
constexpr int SQLITE_ROW = PG_STEP_ROW;
constexpr int SQLITE_NULL = 5;
constexpr int SQLITE_CONSTRAINT_UNIQUE = 2067;
constexpr int SQLITE_CONSTRAINT_PRIMARYKEY = 1555;

int sqlite3_step(sqlite3_stmt* statement) { return statement->step(); }
void sqlite3_bind_int(sqlite3_stmt* statement, int index, int value) { statement->bindInt(index, value); }
void sqlite3_bind_int64(sqlite3_stmt* statement, int index, sqlite3_int64 value) { statement->bindInt64(index, value); }
void sqlite3_bind_double(sqlite3_stmt* statement, int index, double value) { statement->bindDouble(index, value); }
void sqlite3_bind_null(sqlite3_stmt* statement, int index) { statement->bindNull(index); }
int sqlite3_column_int(sqlite3_stmt* statement, int index) { return statement->columnInt(index); }
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt* statement, int index) { return statement->columnInt64(index); }
double sqlite3_column_double(sqlite3_stmt* statement, int index) { return statement->columnDouble(index); }
int sqlite3_column_type(sqlite3_stmt* statement, int index) {
    return statement->columnIsNull(index) ? SQLITE_NULL : 0;
}
int sqlite3_changes(sqlite3* db) { return db->changedRows(); }
int sqlite3_extended_errcode(sqlite3* db) {
    return postgresSqlStateIsUniqueViolation(*db) ? SQLITE_CONSTRAINT_UNIQUE : 0;
}
const char* sqlite3_errmsg(sqlite3* db) { return db->lastError().c_str(); }

const char* schemaSql() {
    return R"SQL(
CREATE TABLE IF NOT EXISTS camera_schema_version (
  version INTEGER PRIMARY KEY,
  applied_at_ms BIGINT NOT NULL
);
CREATE TABLE IF NOT EXISTS camera_tasks (
  task_id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  camera_profile TEXT NOT NULL,
  enabled SMALLINT NOT NULL CHECK (enabled IN (0,1)),
  frame_interval_ms INTEGER NOT NULL,
  output_mode TEXT NOT NULL CHECK (output_mode IN ('latest','archive','both')),
  jpeg_quality INTEGER NOT NULL,
  max_width INTEGER NOT NULL,
  max_height INTEGER NOT NULL,
  retention_days INTEGER NOT NULL,
  max_saved_frames INTEGER NOT NULL,
  desired_state TEXT NOT NULL DEFAULT 'running' CHECK (desired_state IN ('running','stopped')),
  analysis_enabled SMALLINT NOT NULL DEFAULT 0 CHECK (analysis_enabled IN (0,1)),
  target_infer_fps DOUBLE PRECISION NOT NULL DEFAULT 5,
  algorithm_profile TEXT NOT NULL DEFAULT '',
  algorithms_json JSONB NOT NULL DEFAULT '[]'::jsonb,
  callback_profile TEXT NOT NULL DEFAULT '',
  version INTEGER NOT NULL,
  created_at_ms BIGINT NOT NULL,
  updated_at_ms BIGINT NOT NULL,
  deleted_at_ms BIGINT
);
ALTER TABLE camera_tasks ADD COLUMN IF NOT EXISTS desired_state TEXT NOT NULL DEFAULT 'stopped';
ALTER TABLE camera_tasks ADD COLUMN IF NOT EXISTS analysis_enabled SMALLINT NOT NULL DEFAULT 0;
ALTER TABLE camera_tasks ADD COLUMN IF NOT EXISTS target_infer_fps DOUBLE PRECISION NOT NULL DEFAULT 5;
ALTER TABLE camera_tasks ADD COLUMN IF NOT EXISTS algorithm_profile TEXT NOT NULL DEFAULT '';
ALTER TABLE camera_tasks ADD COLUMN IF NOT EXISTS algorithms_json JSONB NOT NULL DEFAULT '[]'::jsonb;
ALTER TABLE camera_tasks ADD COLUMN IF NOT EXISTS callback_profile TEXT NOT NULL DEFAULT '';
UPDATE camera_tasks SET desired_state=CASE WHEN enabled=1 THEN 'running' ELSE 'stopped' END;
CREATE INDEX IF NOT EXISTS idx_camera_tasks_updated
  ON camera_tasks(deleted_at_ms, updated_at_ms DESC);
CREATE TABLE IF NOT EXISTS camera_task_runs (
  run_id TEXT PRIMARY KEY,
  task_id TEXT NOT NULL REFERENCES camera_tasks(task_id),
  definition_version INTEGER NOT NULL,
  definition_json TEXT NOT NULL,
  status TEXT NOT NULL,
  camera_profile TEXT NOT NULL,
  hub_instance_id TEXT,
  create_time_ms BIGINT NOT NULL,
  start_time_ms BIGINT,
  stop_time_ms BIGINT,
  last_update_ms BIGINT NOT NULL,
  worker_consumer TEXT,
  capture_backend TEXT,
  capture_fps REAL NOT NULL DEFAULT 0,
  save_fps REAL NOT NULL DEFAULT 0,
  consumed_frames BIGINT NOT NULL DEFAULT 0,
  saved_frames BIGINT NOT NULL DEFAULT 0,
  skipped_frames BIGINT NOT NULL DEFAULT 0,
  dropped_frames BIGINT NOT NULL DEFAULT 0,
  last_source_sequence BIGINT NOT NULL DEFAULT 0,
  last_frame_time_ms BIGINT,
  width INTEGER NOT NULL DEFAULT 0,
  height INTEGER NOT NULL DEFAULT 0,
  stop_reason TEXT,
  error_code TEXT,
  error_message TEXT
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_camera_task_active_run
  ON camera_task_runs(task_id)
  WHERE status IN ('queued','starting','running','reconnecting');
CREATE INDEX IF NOT EXISTS idx_camera_runs_task_time
  ON camera_task_runs(task_id, create_time_ms DESC);
CREATE TABLE IF NOT EXISTS camera_frames (
  frame_id TEXT PRIMARY KEY,
  task_id TEXT NOT NULL REFERENCES camera_tasks(task_id),
  run_id TEXT NOT NULL REFERENCES camera_task_runs(run_id),
  source_sequence BIGINT NOT NULL,
  capture_time_ms BIGINT NOT NULL,
  save_time_ms BIGINT NOT NULL,
  relative_path TEXT NOT NULL UNIQUE,
  width INTEGER NOT NULL,
  height INTEGER NOT NULL,
  size_bytes BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_camera_frames_task_time
  ON camera_frames(task_id, capture_time_ms DESC);
CREATE INDEX IF NOT EXISTS idx_camera_frames_run_sequence
  ON camera_frames(run_id, source_sequence);
CREATE TABLE IF NOT EXISTS security_alert_events (
  event_id TEXT PRIMARY KEY,
  task_id TEXT NOT NULL REFERENCES camera_tasks(task_id),
  run_id TEXT NOT NULL REFERENCES camera_task_runs(run_id),
  camera_profile TEXT NOT NULL,
  event_type TEXT NOT NULL,
  category TEXT NOT NULL,
  severity INTEGER NOT NULL CHECK (severity BETWEEN 1 AND 5),
  confidence DOUBLE PRECISION,
  track_id BIGINT,
  occurred_at_ms BIGINT NOT NULL,
  algorithm_profile TEXT NOT NULL,
  model_name TEXT NOT NULL,
  config_version TEXT NOT NULL,
  demo_classifier SMALLINT NOT NULL DEFAULT 0 CHECK (demo_classifier IN (0,1)),
  payload_json JSONB NOT NULL,
  evidence_frame_id TEXT,
  fingerprint TEXT NOT NULL,
  created_at_ms BIGINT NOT NULL,
  UNIQUE(task_id,fingerprint)
);
CREATE INDEX IF NOT EXISTS idx_security_alert_task_time
  ON security_alert_events(task_id,occurred_at_ms DESC,event_id DESC);
CREATE INDEX IF NOT EXISTS idx_security_alert_type_time
  ON security_alert_events(event_type,occurred_at_ms DESC);
CREATE TABLE IF NOT EXISTS callback_outbox (
  outbox_id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
  event_id TEXT NOT NULL UNIQUE REFERENCES security_alert_events(event_id),
  callback_profile TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending'
    CHECK (status IN ('pending','delivering','delivered','retry','dead_letter')),
  attempt INTEGER NOT NULL DEFAULT 0,
  next_attempt_at_ms BIGINT NOT NULL,
  last_attempt_at_ms BIGINT,
  delivered_at_ms BIGINT,
  last_http_status INTEGER,
  last_error_code TEXT,
  response_body_hash TEXT,
  created_at_ms BIGINT NOT NULL,
  updated_at_ms BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_callback_outbox_due
  ON callback_outbox(status,next_attempt_at_ms,outbox_id);
CREATE TABLE IF NOT EXISTS camera_idempotency_keys (
  operation_scope TEXT NOT NULL,
  idempotency_key TEXT NOT NULL,
  request_digest TEXT NOT NULL,
  resource_id TEXT NOT NULL,
  response_status INTEGER NOT NULL,
  response_json JSONB NOT NULL,
  created_at_ms BIGINT NOT NULL,
  expires_at_ms BIGINT NOT NULL,
  PRIMARY KEY(operation_scope,idempotency_key)
);
CREATE INDEX IF NOT EXISTS idx_camera_idempotency_expiry
  ON camera_idempotency_keys(expires_at_ms);
INSERT INTO camera_schema_version(version, applied_at_ms)
VALUES(2, (EXTRACT(EPOCH FROM clock_timestamp()) * 1000)::BIGINT)
ON CONFLICT(version) DO NOTHING;
)SQL";
}

bool execSql(sqlite3* db, const char* sql, std::string& error) {
    return db->exec(sql, error);
}

bool openDatabase(const CameraTasksSection& config, DbPtr& db, std::string& error) {
    auto* raw = new PostgresConnection();
    db.reset(raw);
    return raw->openFromEnvironment(config.postgres_dsn_env, error);
}

bool prepare(sqlite3* db, const char* sql, StatementPtr& statement, std::string& error) {
    auto prepared = db->prepare(sql, error);
    if (!prepared) return false;
    statement.reset(prepared.release());
    return true;
}

void bindText(sqlite3_stmt* statement, int index, const std::string& value) {
    statement->bindText(index, value);
}

void bindNullableInt64(sqlite3_stmt* statement, int index, long long value) {
    if (value > 0) sqlite3_bind_int64(statement, index, value);
    else sqlite3_bind_null(statement, index);
}

std::string columnText(sqlite3_stmt* statement, int index) {
    return statement->columnText(index);
}

CameraTaskDefinition readTask(sqlite3_stmt* statement) {
    CameraTaskDefinition task;
    task.task_id = columnText(statement, 0);
    task.name = columnText(statement, 1);
    task.camera_profile = columnText(statement, 2);
    task.enabled = sqlite3_column_int(statement, 3) != 0;
    task.frame_interval_ms = sqlite3_column_int(statement, 4);
    task.output_mode = columnText(statement, 5);
    task.jpeg_quality = sqlite3_column_int(statement, 6);
    task.max_width = sqlite3_column_int(statement, 7);
    task.max_height = sqlite3_column_int(statement, 8);
    task.retention_days = sqlite3_column_int(statement, 9);
    task.max_saved_frames = sqlite3_column_int(statement, 10);
    task.desired_state = columnText(statement, 11);
    task.analysis_enabled = sqlite3_column_int(statement, 12) != 0;
    task.target_infer_fps = sqlite3_column_double(statement, 13);
    task.algorithm_profile = columnText(statement, 14);
    const auto algorithms = json::parse(columnText(statement, 15), nullptr, false);
    if (algorithms.is_array()) {
        for (const auto& item : algorithms) {
            if (item.is_string()) task.algorithms.push_back(item.get<std::string>());
        }
    }
    task.callback_profile = columnText(statement, 16);
    task.version = sqlite3_column_int(statement, 17);
    task.created_at_ms = sqlite3_column_int64(statement, 18);
    task.updated_at_ms = sqlite3_column_int64(statement, 19);
    if (sqlite3_column_type(statement, 20) != SQLITE_NULL) {
        task.deleted_at_ms = sqlite3_column_int64(statement, 20);
    }
    return task;
}

SecurityAlertEventRecord readAlert(sqlite3_stmt* statement) {
    SecurityAlertEventRecord alert;
    alert.event_id = columnText(statement, 0);
    alert.task_id = columnText(statement, 1);
    alert.run_id = columnText(statement, 2);
    alert.camera_profile = columnText(statement, 3);
    alert.event_type = columnText(statement, 4);
    alert.category = columnText(statement, 5);
    alert.severity = sqlite3_column_int(statement, 6);
    if (sqlite3_column_type(statement, 7) != SQLITE_NULL) {
        alert.confidence = sqlite3_column_double(statement, 7);
    }
    if (sqlite3_column_type(statement, 8) != SQLITE_NULL) {
        alert.track_id = sqlite3_column_int64(statement, 8);
    }
    alert.occurred_at_ms = sqlite3_column_int64(statement, 9);
    alert.algorithm_profile = columnText(statement, 10);
    alert.model_name = columnText(statement, 11);
    alert.config_version = columnText(statement, 12);
    alert.demo_classifier = sqlite3_column_int(statement, 13) != 0;
    alert.payload_json = columnText(statement, 14);
    alert.evidence_frame_id = columnText(statement, 15);
    alert.fingerprint = columnText(statement, 16);
    alert.delivery_status = columnText(statement, 17);
    alert.created_at_ms = sqlite3_column_int64(statement, 18);
    return alert;
}

CameraTaskRunRecord readRun(sqlite3_stmt* statement) {
    CameraTaskRunRecord run;
    run.run_id = columnText(statement, 0);
    run.task_id = columnText(statement, 1);
    run.definition_version = sqlite3_column_int(statement, 2);
    run.definition_json = columnText(statement, 3);
    run.status = columnText(statement, 4);
    run.camera_profile = columnText(statement, 5);
    run.hub_instance_id = columnText(statement, 6);
    run.create_time_ms = sqlite3_column_int64(statement, 7);
    run.start_time_ms = sqlite3_column_int64(statement, 8);
    run.stop_time_ms = sqlite3_column_int64(statement, 9);
    run.last_update_ms = sqlite3_column_int64(statement, 10);
    run.worker_consumer = columnText(statement, 11);
    run.capture_backend = columnText(statement, 12);
    run.capture_fps = sqlite3_column_double(statement, 13);
    run.save_fps = sqlite3_column_double(statement, 14);
    run.consumed_frames = sqlite3_column_int64(statement, 15);
    run.saved_frames = sqlite3_column_int64(statement, 16);
    run.skipped_frames = sqlite3_column_int64(statement, 17);
    run.dropped_frames = sqlite3_column_int64(statement, 18);
    run.last_source_sequence = static_cast<unsigned long long>(sqlite3_column_int64(statement, 19));
    run.last_frame_time_ms = sqlite3_column_int64(statement, 20);
    run.width = sqlite3_column_int(statement, 21);
    run.height = sqlite3_column_int(statement, 22);
    run.stop_reason = columnText(statement, 23);
    run.error_code = columnText(statement, 24);
    run.error_message = columnText(statement, 25);
    return run;
}

CameraFrameArtifact readFrame(sqlite3_stmt* statement) {
    CameraFrameArtifact frame;
    frame.frame_id = columnText(statement, 0);
    frame.task_id = columnText(statement, 1);
    frame.run_id = columnText(statement, 2);
    frame.source_sequence = static_cast<unsigned long long>(sqlite3_column_int64(statement, 3));
    frame.capture_time_ms = sqlite3_column_int64(statement, 4);
    frame.save_time_ms = sqlite3_column_int64(statement, 5);
    frame.relative_path = columnText(statement, 6);
    frame.width = sqlite3_column_int(statement, 7);
    frame.height = sqlite3_column_int(statement, 8);
    frame.size_bytes = sqlite3_column_int64(statement, 9);
    return frame;
}

bool validDimension(int value) {
    return value == 0 || (value >= 64 && value <= 8192);
}

bool validServiceIdentifier(const std::string& value, std::size_t maximum, bool allow_empty = false) {
    if (value.empty()) return allow_empty;
    if (value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
    });
}

bool validateTask(const CameraTaskDefinition& task, std::string& error) {
    if (task.task_id.empty() || task.name.empty() || task.name.size() > 512 ||
        task.camera_profile.empty()) {
        error = "task id, name, and camera profile are required";
        return false;
    }
    if (task.frame_interval_ms < 100 || task.frame_interval_ms > 3600000 ||
        (task.output_mode != "latest" && task.output_mode != "archive" && task.output_mode != "both") ||
        task.jpeg_quality < 1 || task.jpeg_quality > 100 ||
        !validDimension(task.max_width) || !validDimension(task.max_height) ||
        task.retention_days < 1 || task.retention_days > 3650 ||
        task.max_saved_frames < 1 || task.max_saved_frames > 1000000) {
        error = "camera task definition is outside allowed bounds";
        return false;
    }
    if ((task.desired_state != "running" && task.desired_state != "stopped") ||
        task.enabled != (task.desired_state == "running") ||
        task.target_infer_fps < 0.1 || task.target_infer_fps > 120.0 ||
        !validServiceIdentifier(task.algorithm_profile, 160, !task.analysis_enabled) ||
        !validServiceIdentifier(task.callback_profile, 160, true) ||
        (task.analysis_enabled && task.algorithms.empty()) || task.algorithms.size() > 32) {
        error = "algorithm task definition is outside allowed bounds";
        return false;
    }
    std::set<std::string> unique_algorithms;
    for (const auto& algorithm : task.algorithms) {
        if (!validServiceIdentifier(algorithm, 80) || !unique_algorithms.insert(algorithm).second) {
            error = "algorithm identifiers must be unique safe identifiers";
            return false;
        }
    }
    return true;
}

const char* taskSelectSql() {
    return "SELECT task_id,name,camera_profile,enabled,frame_interval_ms,output_mode,jpeg_quality,"
        "max_width,max_height,retention_days,max_saved_frames,desired_state,analysis_enabled,"
        "target_infer_fps,algorithm_profile,algorithms_json::text,callback_profile,version,"
        "created_at_ms,updated_at_ms,deleted_at_ms "
        "FROM camera_tasks";
}

const char* alertSelectSql() {
    return "SELECT e.event_id,e.task_id,e.run_id,e.camera_profile,e.event_type,e.category,e.severity,"
        "e.confidence,e.track_id,e.occurred_at_ms,e.algorithm_profile,e.model_name,e.config_version,"
        "e.demo_classifier,e.payload_json::text,COALESCE(e.evidence_frame_id,''),e.fingerprint,"
        "COALESCE(o.status,'not_scheduled'),e.created_at_ms FROM security_alert_events e "
        "LEFT JOIN callback_outbox o ON o.event_id=e.event_id";
}

const char* runSelectSql() {
    return "SELECT run_id,task_id,definition_version,definition_json,status,camera_profile,"
        "COALESCE(hub_instance_id,''),create_time_ms,COALESCE(start_time_ms,0),COALESCE(stop_time_ms,0),"
        "last_update_ms,COALESCE(worker_consumer,''),COALESCE(capture_backend,''),capture_fps,save_fps,"
        "consumed_frames,saved_frames,skipped_frames,dropped_frames,last_source_sequence,"
        "COALESCE(last_frame_time_ms,0),width,height,COALESCE(stop_reason,''),COALESCE(error_code,''),"
        "COALESCE(error_message,'') FROM camera_task_runs";
}

const char* frameSelectSql() {
    return "SELECT frame_id,task_id,run_id,source_sequence,capture_time_ms,save_time_ms,relative_path,"
        "width,height,size_bytes FROM camera_frames";
}

}  // namespace

CameraTaskRepository::CameraTaskRepository(const CameraTasksSection& config) : config_(config) {}

bool CameraTaskRepository::initialize(std::string& error) const {
    error.clear();
    DbPtr db;
    return openDatabase(config_, db, error) && execSql(db.get(), schemaSql(), error);
}

bool CameraTaskRepository::createTask(
    const CameraTaskDefinition& task,
    std::string& error_code,
    std::string& error
) const {
    error_code.clear();
    error.clear();
    if (!validateTask(task, error)) {
        error_code = "INVALID_TASK_CONFIG";
        return false;
    }
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "INSERT INTO camera_tasks(task_id,name,camera_profile,enabled,frame_interval_ms,output_mode,"
        "jpeg_quality,max_width,max_height,retention_days,max_saved_frames,desired_state,analysis_enabled,"
        "target_infer_fps,algorithm_profile,algorithms_json,callback_profile,version,created_at_ms,updated_at_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?::jsonb,?,?,?,?);", statement, error)) return false;
    bindText(statement.get(), 1, task.task_id);
    bindText(statement.get(), 2, task.name);
    bindText(statement.get(), 3, task.camera_profile);
    sqlite3_bind_int(statement.get(), 4, task.enabled ? 1 : 0);
    sqlite3_bind_int(statement.get(), 5, task.frame_interval_ms);
    bindText(statement.get(), 6, task.output_mode);
    sqlite3_bind_int(statement.get(), 7, task.jpeg_quality);
    sqlite3_bind_int(statement.get(), 8, task.max_width);
    sqlite3_bind_int(statement.get(), 9, task.max_height);
    sqlite3_bind_int(statement.get(), 10, task.retention_days);
    sqlite3_bind_int(statement.get(), 11, task.max_saved_frames);
    bindText(statement.get(), 12, task.desired_state);
    sqlite3_bind_int(statement.get(), 13, task.analysis_enabled ? 1 : 0);
    sqlite3_bind_double(statement.get(), 14, task.target_infer_fps);
    bindText(statement.get(), 15, task.algorithm_profile);
    bindText(statement.get(), 16, json(task.algorithms).dump());
    bindText(statement.get(), 17, task.callback_profile);
    sqlite3_bind_int(statement.get(), 18, std::max(1, task.version));
    sqlite3_bind_int64(statement.get(), 19, task.created_at_ms);
    sqlite3_bind_int64(statement.get(), 20, task.updated_at_ms);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    error_code = (sqlite3_extended_errcode(db.get()) == SQLITE_CONSTRAINT_PRIMARYKEY ||
                  sqlite3_extended_errcode(db.get()) == SQLITE_CONSTRAINT_UNIQUE)
        ? "TASK_ALREADY_EXISTS" : "STORAGE_UNAVAILABLE";
    return false;
}

bool CameraTaskRepository::getTask(
    const std::string& task_id,
    bool include_deleted,
    CameraTaskDefinition& task,
    bool& found,
    std::string& error
) const {
    found = false;
    error.clear();
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    const std::string sql = std::string(taskSelectSql()) +
        " WHERE task_id=? AND (? <> 0 OR deleted_at_ms IS NULL);";
    StatementPtr statement;
    if (!prepare(db.get(), sql.c_str(), statement, error)) return false;
    bindText(statement.get(), 1, task_id);
    sqlite3_bind_int(statement.get(), 2, include_deleted ? 1 : 0);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    task = readTask(statement.get());
    found = true;
    return true;
}

bool CameraTaskRepository::listTasks(
    bool include_deleted,
    int limit,
    int offset,
    std::vector<CameraTaskDefinition>& tasks,
    std::string& error
) const {
    tasks.clear();
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    const std::string sql = std::string(taskSelectSql()) +
        " WHERE (? <> 0 OR deleted_at_ms IS NULL) ORDER BY updated_at_ms DESC LIMIT ? OFFSET ?;";
    StatementPtr statement;
    if (!prepare(db.get(), sql.c_str(), statement, error)) return false;
    sqlite3_bind_int(statement.get(), 1, include_deleted ? 1 : 0);
    sqlite3_bind_int(statement.get(), 2, std::clamp(limit, 1, 1000));
    sqlite3_bind_int(statement.get(), 3, std::max(0, offset));
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) tasks.push_back(readTask(statement.get()));
    if (result == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    return false;
}

bool CameraTaskRepository::updateTask(
    const std::string& task_id,
    int expected_version,
    const CameraTaskPatch& patch,
    long long update_time_ms,
    CameraTaskDefinition& updated,
    std::string& error_code,
    std::string& error
) const {
    CameraTaskDefinition current;
    bool found = false;
    if (!getTask(task_id, false, current, found, error)) return false;
    if (!found) {
        error_code = "TASK_NOT_FOUND";
        error = "camera task was not found";
        return false;
    }
    updated = current;
    if (patch.name) updated.name = *patch.name;
    if (patch.camera_profile) updated.camera_profile = *patch.camera_profile;
    if (patch.enabled) {
        updated.enabled = *patch.enabled;
        updated.desired_state = *patch.enabled ? "running" : "stopped";
    }
    if (patch.frame_interval_ms) updated.frame_interval_ms = *patch.frame_interval_ms;
    if (patch.output_mode) updated.output_mode = *patch.output_mode;
    if (patch.jpeg_quality) updated.jpeg_quality = *patch.jpeg_quality;
    if (patch.max_width) updated.max_width = *patch.max_width;
    if (patch.max_height) updated.max_height = *patch.max_height;
    if (patch.retention_days) updated.retention_days = *patch.retention_days;
    if (patch.max_saved_frames) updated.max_saved_frames = *patch.max_saved_frames;
    if (patch.desired_state) updated.desired_state = *patch.desired_state;
    if (patch.analysis_enabled) updated.analysis_enabled = *patch.analysis_enabled;
    if (patch.target_infer_fps) updated.target_infer_fps = *patch.target_infer_fps;
    if (patch.algorithm_profile) updated.algorithm_profile = *patch.algorithm_profile;
    if (patch.algorithms) updated.algorithms = *patch.algorithms;
    if (patch.callback_profile) updated.callback_profile = *patch.callback_profile;
    updated.enabled = updated.desired_state == "running";
    if (!validateTask(updated, error)) {
        error_code = "INVALID_TASK_CONFIG";
        return false;
    }
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "UPDATE camera_tasks SET name=?,camera_profile=?,enabled=?,frame_interval_ms=?,output_mode=?,"
        "jpeg_quality=?,max_width=?,max_height=?,retention_days=?,max_saved_frames=?,desired_state=?,"
        "analysis_enabled=?,target_infer_fps=?,algorithm_profile=?,algorithms_json=?::jsonb,callback_profile=?,"
        "version=version+1,updated_at_ms=? WHERE task_id=? AND version=? AND deleted_at_ms IS NULL;",
        statement, error)) return false;
    bindText(statement.get(), 1, updated.name);
    bindText(statement.get(), 2, updated.camera_profile);
    sqlite3_bind_int(statement.get(), 3, updated.enabled ? 1 : 0);
    sqlite3_bind_int(statement.get(), 4, updated.frame_interval_ms);
    bindText(statement.get(), 5, updated.output_mode);
    sqlite3_bind_int(statement.get(), 6, updated.jpeg_quality);
    sqlite3_bind_int(statement.get(), 7, updated.max_width);
    sqlite3_bind_int(statement.get(), 8, updated.max_height);
    sqlite3_bind_int(statement.get(), 9, updated.retention_days);
    sqlite3_bind_int(statement.get(), 10, updated.max_saved_frames);
    bindText(statement.get(), 11, updated.desired_state);
    sqlite3_bind_int(statement.get(), 12, updated.analysis_enabled ? 1 : 0);
    sqlite3_bind_double(statement.get(), 13, updated.target_infer_fps);
    bindText(statement.get(), 14, updated.algorithm_profile);
    bindText(statement.get(), 15, json(updated.algorithms).dump());
    bindText(statement.get(), 16, updated.callback_profile);
    sqlite3_bind_int64(statement.get(), 17, update_time_ms);
    bindText(statement.get(), 18, task_id);
    sqlite3_bind_int(statement.get(), 19, expected_version);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    if (sqlite3_changes(db.get()) == 0) {
        StatementPtr active;
        if (!prepare(db.get(), "SELECT version FROM camera_tasks WHERE task_id=? AND deleted_at_ms IS NULL;",
            active, error)) return false;
        bindText(active.get(), 1, task_id);
        const int result = sqlite3_step(active.get());
        if (result != SQLITE_ROW) error_code = "TASK_NOT_FOUND";
        else error_code = "TASK_VERSION_CONFLICT";
        error = "camera version does not match";
        return false;
    }
    updated.version = expected_version + 1;
    updated.updated_at_ms = update_time_ms;
    return true;
}

bool CameraTaskRepository::softDeleteTask(
    const std::string& task_id,
    int expected_version,
    long long delete_time_ms,
    std::string& error_code,
    std::string& error
) const {
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "UPDATE camera_tasks SET deleted_at_ms=?,updated_at_ms=?,version=version+1 WHERE task_id=? "
        "AND version=? AND deleted_at_ms IS NULL;",
        statement, error)) return false;
    sqlite3_bind_int64(statement.get(), 1, delete_time_ms);
    sqlite3_bind_int64(statement.get(), 2, delete_time_ms);
    bindText(statement.get(), 3, task_id);
    sqlite3_bind_int(statement.get(), 4, expected_version);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    if (sqlite3_changes(db.get()) > 0) return true;
    CameraTaskDefinition current;
    bool found = false;
    if (!getTask(task_id, false, current, found, error)) return false;
    if (!found) error_code = "TASK_NOT_FOUND";
    else if (current.version != expected_version) error_code = "TASK_VERSION_CONFLICT";
    else error_code = "TASK_VERSION_CONFLICT";
    error = error_code;
    return false;
}

bool CameraTaskRepository::createRun(
    const CameraTaskRunRecord& run,
    std::string& error_code,
    std::string& error
) const {
    error_code.clear();
    if (run.run_id.empty() || run.task_id.empty() || run.definition_json.empty() ||
        run.camera_profile.empty() || run.definition_version <= 0) {
        error_code = "INVALID_RUN_CONFIG";
        error = "camera run definition is incomplete";
        return false;
    }
    CameraTaskDefinition task;
    bool found = false;
    if (!getTask(run.task_id, false, task, found, error)) return false;
    if (!found) {
        error_code = "TASK_NOT_FOUND";
        error = "camera task was not found";
        return false;
    }
    if (!task.enabled) {
        error_code = "TASK_DISABLED";
        error = "camera task is disabled";
        return false;
    }
    if (task.version != run.definition_version) {
        error_code = "TASK_VERSION_CONFLICT";
        error = "camera task version changed before run creation";
        return false;
    }
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "INSERT INTO camera_task_runs(run_id,task_id,definition_version,definition_json,status,"
        "camera_profile,create_time_ms,last_update_ms) VALUES(?,?,?,?,?,?,?,?);", statement, error)) return false;
    bindText(statement.get(), 1, run.run_id);
    bindText(statement.get(), 2, run.task_id);
    sqlite3_bind_int(statement.get(), 3, run.definition_version);
    bindText(statement.get(), 4, run.definition_json);
    bindText(statement.get(), 5, run.status.empty() ? std::string("queued") : run.status);
    bindText(statement.get(), 6, run.camera_profile);
    sqlite3_bind_int64(statement.get(), 7, run.create_time_ms);
    sqlite3_bind_int64(statement.get(), 8, run.last_update_ms);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    const int extended = sqlite3_extended_errcode(db.get());
    error_code = (extended == SQLITE_CONSTRAINT_UNIQUE || extended == SQLITE_CONSTRAINT_PRIMARYKEY)
        ? "ACTIVE_RUN_EXISTS" : "STORAGE_UNAVAILABLE";
    return false;
}

bool CameraTaskRepository::getRun(
    const std::string& run_id,
    CameraTaskRunRecord& run,
    bool& found,
    std::string& error
) const {
    found = false;
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    const std::string sql = std::string(runSelectSql()) + " WHERE run_id=?;";
    StatementPtr statement;
    if (!prepare(db.get(), sql.c_str(), statement, error)) return false;
    bindText(statement.get(), 1, run_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    run = readRun(statement.get());
    found = true;
    return true;
}

bool CameraTaskRepository::listRuns(
    const std::string& task_id,
    int limit,
    int offset,
    std::vector<CameraTaskRunRecord>& runs,
    std::string& error
) const {
    runs.clear();
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    const std::string sql = std::string(runSelectSql()) +
        " WHERE (?='' OR task_id=?) ORDER BY create_time_ms DESC,last_update_ms DESC,run_id DESC LIMIT ? OFFSET ?;";
    StatementPtr statement;
    if (!prepare(db.get(), sql.c_str(), statement, error)) return false;
    bindText(statement.get(), 1, task_id);
    bindText(statement.get(), 2, task_id);
    sqlite3_bind_int(statement.get(), 3, std::clamp(limit, 1, 1000));
    sqlite3_bind_int(statement.get(), 4, std::max(0, offset));
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) runs.push_back(readRun(statement.get()));
    if (result == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    return false;
}

bool CameraTaskRepository::transitionRun(
    const std::string& run_id,
    const std::vector<std::string>& allowed_from,
    const CameraTaskRunRecord& next,
    std::string& error_code,
    std::string& error
) const {
    CameraTaskRunRecord current;
    bool found = false;
    if (!getRun(run_id, current, found, error)) return false;
    if (!found) {
        error_code = "RUN_NOT_FOUND";
        error = "camera run was not found";
        return false;
    }
    if (std::find(allowed_from.begin(), allowed_from.end(), current.status) == allowed_from.end()) {
        error_code = isCameraRunTerminal(current.status) ? "RUN_TERMINAL" : "RUN_STATE_CONFLICT";
        error = "camera run state transition is not allowed";
        return false;
    }
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "UPDATE camera_task_runs SET status=?,hub_instance_id=?,start_time_ms=?,stop_time_ms=?,"
        "last_update_ms=?,worker_consumer=?,capture_backend=?,capture_fps=?,save_fps=?,"
        "consumed_frames=?,saved_frames=?,skipped_frames=?,dropped_frames=?,last_source_sequence=?,"
        "last_frame_time_ms=?,width=?,height=?,stop_reason=?,error_code=?,error_message=? "
        "WHERE run_id=? AND status=?;", statement, error)) return false;
    bindText(statement.get(), 1, next.status);
    bindText(statement.get(), 2, next.hub_instance_id);
    bindNullableInt64(statement.get(), 3, next.start_time_ms);
    bindNullableInt64(statement.get(), 4, next.stop_time_ms);
    sqlite3_bind_int64(statement.get(), 5, next.last_update_ms);
    bindText(statement.get(), 6, next.worker_consumer);
    bindText(statement.get(), 7, next.capture_backend);
    sqlite3_bind_double(statement.get(), 8, next.capture_fps);
    sqlite3_bind_double(statement.get(), 9, next.save_fps);
    sqlite3_bind_int64(statement.get(), 10, next.consumed_frames);
    sqlite3_bind_int64(statement.get(), 11, next.saved_frames);
    sqlite3_bind_int64(statement.get(), 12, next.skipped_frames);
    sqlite3_bind_int64(statement.get(), 13, next.dropped_frames);
    sqlite3_bind_int64(statement.get(), 14, static_cast<sqlite3_int64>(next.last_source_sequence));
    bindNullableInt64(statement.get(), 15, next.last_frame_time_ms);
    sqlite3_bind_int(statement.get(), 16, next.width);
    sqlite3_bind_int(statement.get(), 17, next.height);
    bindText(statement.get(), 18, next.stop_reason);
    bindText(statement.get(), 19, next.error_code);
    bindText(statement.get(), 20, next.error_message);
    bindText(statement.get(), 21, run_id);
    bindText(statement.get(), 22, current.status);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    if (sqlite3_changes(db.get()) > 0) return true;
    error_code = "RUN_STATE_CONFLICT";
    error = "camera run changed concurrently";
    return false;
}

bool CameraTaskRepository::updateRunProgress(const CameraTaskRunRecord& run, std::string& error) const {
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "UPDATE camera_task_runs SET hub_instance_id=?,last_update_ms=?,worker_consumer=?,"
        "capture_backend=?,capture_fps=?,save_fps=?,consumed_frames=?,saved_frames=?,skipped_frames=?,"
        "dropped_frames=?,last_source_sequence=?,last_frame_time_ms=?,width=?,height=? WHERE run_id=? "
        "AND status IN ('starting','running','reconnecting','stopping');", statement, error)) return false;
    bindText(statement.get(), 1, run.hub_instance_id);
    sqlite3_bind_int64(statement.get(), 2, run.last_update_ms);
    bindText(statement.get(), 3, run.worker_consumer);
    bindText(statement.get(), 4, run.capture_backend);
    sqlite3_bind_double(statement.get(), 5, run.capture_fps);
    sqlite3_bind_double(statement.get(), 6, run.save_fps);
    sqlite3_bind_int64(statement.get(), 7, run.consumed_frames);
    sqlite3_bind_int64(statement.get(), 8, run.saved_frames);
    sqlite3_bind_int64(statement.get(), 9, run.skipped_frames);
    sqlite3_bind_int64(statement.get(), 10, run.dropped_frames);
    sqlite3_bind_int64(statement.get(), 11, static_cast<sqlite3_int64>(run.last_source_sequence));
    bindNullableInt64(statement.get(), 12, run.last_frame_time_ms);
    sqlite3_bind_int(statement.get(), 13, run.width);
    sqlite3_bind_int(statement.get(), 14, run.height);
    bindText(statement.get(), 15, run.run_id);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    return false;
}

bool CameraTaskRepository::recoverStaleRuns(
    long long stale_before_ms,
    long long now_ms,
    int& recovered_count,
    std::string& error
) const {
    recovered_count = 0;
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "UPDATE camera_task_runs SET status='failed',stop_time_ms=?,last_update_ms=?,"
        "stop_reason='stale_recovery',error_code='WORKER_HEARTBEAT_STALE',"
        "error_message='worker heartbeat or run lease became stale' WHERE last_update_ms<? AND status IN "
        "('queued','starting','running','reconnecting','stopping');", statement, error)) return false;
    sqlite3_bind_int64(statement.get(), 1, now_ms);
    sqlite3_bind_int64(statement.get(), 2, now_ms);
    sqlite3_bind_int64(statement.get(), 3, stale_before_ms);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    recovered_count = sqlite3_changes(db.get());
    return true;
}

bool CameraTaskRepository::insertFrame(const CameraFrameArtifact& frame, std::string& error) const {
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "INSERT INTO camera_frames(frame_id,task_id,run_id,source_sequence,capture_time_ms,save_time_ms,"
        "relative_path,width,height,size_bytes) VALUES(?,?,?,?,?,?,?,?,?,?);", statement, error)) return false;
    bindText(statement.get(), 1, frame.frame_id);
    bindText(statement.get(), 2, frame.task_id);
    bindText(statement.get(), 3, frame.run_id);
    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(frame.source_sequence));
    sqlite3_bind_int64(statement.get(), 5, frame.capture_time_ms);
    sqlite3_bind_int64(statement.get(), 6, frame.save_time_ms);
    bindText(statement.get(), 7, frame.relative_path);
    sqlite3_bind_int(statement.get(), 8, frame.width);
    sqlite3_bind_int(statement.get(), 9, frame.height);
    sqlite3_bind_int64(statement.get(), 10, frame.size_bytes);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    return false;
}

bool CameraTaskRepository::getLatestFrame(
    const std::string& task_id,
    CameraFrameArtifact& frame,
    bool& found,
    std::string& error
) const {
    found = false;
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    const std::string sql = std::string(frameSelectSql()) +
        " WHERE task_id=? ORDER BY capture_time_ms DESC,save_time_ms DESC LIMIT 1;";
    StatementPtr statement;
    if (!prepare(db.get(), sql.c_str(), statement, error)) return false;
    bindText(statement.get(), 1, task_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    frame = readFrame(statement.get());
    found = true;
    return true;
}

bool CameraTaskRepository::listFrames(
    const std::string& task_id,
    const std::string& run_id,
    int limit,
    int offset,
    std::vector<CameraFrameArtifact>& frames,
    std::string& error
) const {
    frames.clear();
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    const std::string sql = std::string(frameSelectSql()) +
        " WHERE task_id=? AND (?='' OR run_id=?) ORDER BY capture_time_ms DESC LIMIT ? OFFSET ?;";
    StatementPtr statement;
    if (!prepare(db.get(), sql.c_str(), statement, error)) return false;
    bindText(statement.get(), 1, task_id);
    bindText(statement.get(), 2, run_id);
    bindText(statement.get(), 3, run_id);
    sqlite3_bind_int(statement.get(), 4, std::clamp(limit, 1, 1000));
    sqlite3_bind_int(statement.get(), 5, std::max(0, offset));
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) frames.push_back(readFrame(statement.get()));
    if (result == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    return false;
}

bool CameraTaskRepository::listRetentionCandidates(
    long long now_ms,
    int limit,
    std::vector<CameraFrameArtifact>& frames,
    std::string& error
) const {
    frames.clear();
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "WITH ranked AS ("
        "SELECT f.frame_id,f.task_id,f.run_id,f.source_sequence,f.capture_time_ms,f.save_time_ms,"
        "f.relative_path,f.width,f.height,f.size_bytes,t.retention_days,t.max_saved_frames,"
        "ROW_NUMBER() OVER(PARTITION BY f.task_id ORDER BY f.capture_time_ms DESC,"
        "f.save_time_ms DESC,f.frame_id DESC) AS keep_rank "
        "FROM camera_frames f JOIN camera_tasks t ON t.task_id=f.task_id) "
        "SELECT frame_id,task_id,run_id,source_sequence,capture_time_ms,save_time_ms,relative_path,"
        "width,height,size_bytes FROM ranked WHERE capture_time_ms < (? - retention_days::BIGINT * 86400000) "
        "OR keep_rank > max_saved_frames ORDER BY capture_time_ms ASC,save_time_ms ASC LIMIT ?;",
        statement, error)) return false;
    sqlite3_bind_int64(statement.get(), 1, now_ms);
    sqlite3_bind_int(statement.get(), 2, std::clamp(limit, 1, 10000));
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) frames.push_back(readFrame(statement.get()));
    if (result == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    return false;
}

bool CameraTaskRepository::listOldestFrames(
    int limit,
    std::vector<CameraFrameArtifact>& frames,
    std::string& error
) const {
    frames.clear();
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    const std::string sql = std::string(frameSelectSql()) +
        " ORDER BY capture_time_ms ASC,save_time_ms ASC,frame_id ASC LIMIT ?;";
    StatementPtr statement;
    if (!prepare(db.get(), sql.c_str(), statement, error)) return false;
    sqlite3_bind_int(statement.get(), 1, std::clamp(limit, 1, 10000));
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) frames.push_back(readFrame(statement.get()));
    if (result == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    return false;
}

bool CameraTaskRepository::deleteFrameMetadata(const std::string& frame_id, std::string& error) const {
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(), "DELETE FROM camera_frames WHERE frame_id=?;", statement, error)) return false;
    bindText(statement.get(), 1, frame_id);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    return false;
}

bool CameraTaskRepository::insertAlert(
    const SecurityAlertEventRecord& alert,
    std::string& error_code,
    std::string& error
) const {
    return insertAlert(alert, {}, error_code, error);
}

bool CameraTaskRepository::insertAlert(
    const SecurityAlertEventRecord& alert,
    const std::string& callback_profile,
    std::string& error_code,
    std::string& error
) const {
    error_code.clear();
    error.clear();
    if (!validServiceIdentifier(alert.event_id, 160) ||
        !validServiceIdentifier(alert.task_id, 160) ||
        !validServiceIdentifier(alert.run_id, 160) ||
        !validServiceIdentifier(alert.camera_profile, 160) ||
        !validServiceIdentifier(alert.event_type, 80) ||
        !validServiceIdentifier(alert.category, 80) ||
        !validServiceIdentifier(alert.algorithm_profile, 160) ||
        alert.model_name.empty() || alert.model_name.size() > 160 ||
        alert.config_version.empty() || alert.config_version.size() > 160 ||
        alert.fingerprint.empty() || alert.fingerprint.size() > 256 ||
        alert.severity < 1 || alert.severity > 5 || alert.occurred_at_ms <= 0 ||
        alert.created_at_ms <= 0 ||
        (!callback_profile.empty() && !validServiceIdentifier(callback_profile, 160)) ||
        (alert.confidence && (*alert.confidence < 0.0 || *alert.confidence > 1.0)) ||
        json::parse(alert.payload_json, nullptr, false).is_discarded()) {
        error_code = "INVALID_ALERT";
        error = "alert is outside allowed bounds";
        return false;
    }
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    if (!execSql(db.get(), "BEGIN;", error)) return false;
    bool committed = false;
    const auto rollback = [&]() {
        if (committed) return;
        std::string ignored;
        execSql(db.get(), "ROLLBACK;", ignored);
    };
    StatementPtr statement;
    if (!prepare(db.get(),
        "INSERT INTO security_alert_events(event_id,task_id,run_id,camera_profile,event_type,category,"
        "severity,confidence,track_id,occurred_at_ms,algorithm_profile,model_name,config_version,"
        "demo_classifier,payload_json,evidence_frame_id,fingerprint,created_at_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?::jsonb,?,?,?);", statement, error)) {
        rollback();
        return false;
    }
    bindText(statement.get(), 1, alert.event_id);
    bindText(statement.get(), 2, alert.task_id);
    bindText(statement.get(), 3, alert.run_id);
    bindText(statement.get(), 4, alert.camera_profile);
    bindText(statement.get(), 5, alert.event_type);
    bindText(statement.get(), 6, alert.category);
    sqlite3_bind_int(statement.get(), 7, alert.severity);
    if (alert.confidence) sqlite3_bind_double(statement.get(), 8, *alert.confidence);
    else sqlite3_bind_null(statement.get(), 8);
    if (alert.track_id) sqlite3_bind_int64(statement.get(), 9, *alert.track_id);
    else sqlite3_bind_null(statement.get(), 9);
    sqlite3_bind_int64(statement.get(), 10, alert.occurred_at_ms);
    bindText(statement.get(), 11, alert.algorithm_profile);
    bindText(statement.get(), 12, alert.model_name);
    bindText(statement.get(), 13, alert.config_version);
    sqlite3_bind_int(statement.get(), 14, alert.demo_classifier ? 1 : 0);
    bindText(statement.get(), 15, alert.payload_json);
    if (alert.evidence_frame_id.empty()) sqlite3_bind_null(statement.get(), 16);
    else bindText(statement.get(), 16, alert.evidence_frame_id);
    bindText(statement.get(), 17, alert.fingerprint);
    sqlite3_bind_int64(statement.get(), 18, alert.created_at_ms);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db.get());
        error_code = sqlite3_extended_errcode(db.get()) == SQLITE_CONSTRAINT_UNIQUE
            ? "ALERT_ALREADY_EXISTS" : "STORAGE_UNAVAILABLE";
        rollback();
        return false;
    }

    if (!callback_profile.empty()) {
        statement.reset();
        if (!prepare(db.get(),
            "INSERT INTO callback_outbox(event_id,callback_profile,status,attempt,next_attempt_at_ms,"
            "created_at_ms,updated_at_ms) VALUES(?,?,'pending',0,?,?,?);",
            statement, error)) {
            error_code = "STORAGE_UNAVAILABLE";
            rollback();
            return false;
        }
        bindText(statement.get(), 1, alert.event_id);
        bindText(statement.get(), 2, callback_profile);
        sqlite3_bind_int64(statement.get(), 3, alert.created_at_ms);
        sqlite3_bind_int64(statement.get(), 4, alert.created_at_ms);
        sqlite3_bind_int64(statement.get(), 5, alert.created_at_ms);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            error = sqlite3_errmsg(db.get());
            error_code = "STORAGE_UNAVAILABLE";
            rollback();
            return false;
        }
    }

    if (!execSql(db.get(), "COMMIT;", error)) {
        error_code = "STORAGE_UNAVAILABLE";
        rollback();
        return false;
    }
    committed = true;
    return true;
}

bool CameraTaskRepository::getAlert(
    const std::string& event_id,
    SecurityAlertEventRecord& alert,
    bool& found,
    std::string& error
) const {
    found = false;
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    const std::string sql = std::string(alertSelectSql()) + " WHERE e.event_id=?;";
    StatementPtr statement;
    if (!prepare(db.get(), sql.c_str(), statement, error)) return false;
    bindText(statement.get(), 1, event_id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    alert = readAlert(statement.get());
    found = true;
    return true;
}

bool CameraTaskRepository::listAlerts(
    const std::string& task_id,
    const std::string& event_type,
    int minimum_severity,
    int limit,
    int offset,
    std::vector<SecurityAlertEventRecord>& alerts,
    std::string& error
) const {
    alerts.clear();
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    const std::string sql = std::string(alertSelectSql()) +
        " WHERE e.task_id=? AND (?='' OR e.event_type=?) AND e.severity>=? "
        "ORDER BY e.occurred_at_ms DESC,e.event_id DESC LIMIT ? OFFSET ?;";
    StatementPtr statement;
    if (!prepare(db.get(), sql.c_str(), statement, error)) return false;
    bindText(statement.get(), 1, task_id);
    bindText(statement.get(), 2, event_type);
    bindText(statement.get(), 3, event_type);
    sqlite3_bind_int(statement.get(), 4, std::clamp(minimum_severity, 1, 5));
    sqlite3_bind_int(statement.get(), 5, std::clamp(limit, 1, 200));
    sqlite3_bind_int(statement.get(), 6, std::max(0, offset));
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) alerts.push_back(readAlert(statement.get()));
    if (result == SQLITE_DONE) return true;
    error = sqlite3_errmsg(db.get());
    return false;
}

bool CameraTaskRepository::claimDueCallback(
    long long now_ms,
    int lease_timeout_ms,
    CallbackOutboxRecord& outbox,
    bool& found,
    std::string& error
) const {
    found = false;
    outbox = {};
    error.clear();
    if (now_ms <= 0 || lease_timeout_ms < 1000 || lease_timeout_ms > 600000) {
        error = "callback claim bounds are invalid";
        return false;
    }
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "WITH candidate AS ("
        " SELECT outbox_id FROM callback_outbox"
        " WHERE ((status IN ('pending','retry') AND next_attempt_at_ms<=?)"
        "    OR (status='delivering' AND next_attempt_at_ms<=?))"
        " ORDER BY next_attempt_at_ms ASC,outbox_id ASC"
        " FOR UPDATE SKIP LOCKED LIMIT 1"
        ")"
        " UPDATE callback_outbox o"
        " SET status='delivering',attempt=o.attempt+1,last_attempt_at_ms=?,"
        "     next_attempt_at_ms=?,updated_at_ms=?"
        " FROM candidate c WHERE o.outbox_id=c.outbox_id"
        " RETURNING o.outbox_id,o.event_id,o.callback_profile,o.status,o.attempt,"
        "           o.next_attempt_at_ms,o.last_attempt_at_ms,o.created_at_ms,o.updated_at_ms;",
        statement,
        error)) {
        return false;
    }
    sqlite3_bind_int64(statement.get(), 1, now_ms);
    sqlite3_bind_int64(statement.get(), 2, now_ms);
    sqlite3_bind_int64(statement.get(), 3, now_ms);
    sqlite3_bind_int64(statement.get(), 4, now_ms + lease_timeout_ms);
    sqlite3_bind_int64(statement.get(), 5, now_ms);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    outbox.outbox_id = sqlite3_column_int64(statement.get(), 0);
    outbox.event_id = columnText(statement.get(), 1);
    outbox.callback_profile = columnText(statement.get(), 2);
    outbox.status = columnText(statement.get(), 3);
    outbox.attempt = sqlite3_column_int(statement.get(), 4);
    outbox.next_attempt_at_ms = sqlite3_column_int64(statement.get(), 5);
    outbox.last_attempt_at_ms = sqlite3_column_int64(statement.get(), 6);
    outbox.created_at_ms = sqlite3_column_int64(statement.get(), 7);
    outbox.updated_at_ms = sqlite3_column_int64(statement.get(), 8);
    found = true;
    return true;
}

bool CameraTaskRepository::markCallbackDelivered(
    long long outbox_id,
    int attempt,
    long long delivered_at_ms,
    int http_status,
    const std::string& response_body_hash,
    std::string& error
) const {
    error.clear();
    if (outbox_id <= 0 || attempt <= 0 || delivered_at_ms <= 0 ||
        http_status < 200 || http_status > 299 ||
        (!response_body_hash.empty() && response_body_hash.size() != 64)) {
        error = "callback delivery completion bounds are invalid";
        return false;
    }
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "UPDATE callback_outbox SET status='delivered',delivered_at_ms=?,"
        "last_http_status=?,last_error_code=NULL,response_body_hash=?,updated_at_ms=?"
        " WHERE outbox_id=? AND status='delivering' AND attempt=?;",
        statement,
        error)) {
        return false;
    }
    sqlite3_bind_int64(statement.get(), 1, delivered_at_ms);
    sqlite3_bind_int(statement.get(), 2, http_status);
    if (response_body_hash.empty()) sqlite3_bind_null(statement.get(), 3);
    else bindText(statement.get(), 3, response_body_hash);
    sqlite3_bind_int64(statement.get(), 4, delivered_at_ms);
    sqlite3_bind_int64(statement.get(), 5, outbox_id);
    sqlite3_bind_int(statement.get(), 6, attempt);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    if (sqlite3_changes(db.get()) == 1) return true;
    error = "callback delivery lease was lost";
    return false;
}

bool CameraTaskRepository::finishCallbackAttempt(
    long long outbox_id,
    int attempt,
    const std::string& next_status,
    long long next_attempt_at_ms,
    long long update_time_ms,
    int http_status,
    const std::string& error_code,
    const std::string& response_body_hash,
    std::string& error
) const {
    error.clear();
    if (outbox_id <= 0 || attempt <= 0 ||
        (next_status != "retry" && next_status != "dead_letter") ||
        next_attempt_at_ms < update_time_ms || update_time_ms <= 0 ||
        http_status < 0 || http_status > 599 ||
        !validServiceIdentifier(error_code, 160) ||
        (!response_body_hash.empty() && response_body_hash.size() != 64)) {
        error = "callback attempt completion bounds are invalid";
        return false;
    }
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "UPDATE callback_outbox SET status=?,next_attempt_at_ms=?,"
        "last_http_status=?,last_error_code=?,response_body_hash=?,updated_at_ms=?"
        " WHERE outbox_id=? AND status='delivering' AND attempt=?;",
        statement,
        error)) {
        return false;
    }
    bindText(statement.get(), 1, next_status);
    sqlite3_bind_int64(statement.get(), 2, next_attempt_at_ms);
    if (http_status == 0) sqlite3_bind_null(statement.get(), 3);
    else sqlite3_bind_int(statement.get(), 3, http_status);
    bindText(statement.get(), 4, error_code);
    if (response_body_hash.empty()) sqlite3_bind_null(statement.get(), 5);
    else bindText(statement.get(), 5, response_body_hash);
    sqlite3_bind_int64(statement.get(), 6, update_time_ms);
    sqlite3_bind_int64(statement.get(), 7, outbox_id);
    sqlite3_bind_int(statement.get(), 8, attempt);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    if (sqlite3_changes(db.get()) == 1) return true;
    error = "callback delivery lease was lost";
    return false;
}

bool CameraTaskRepository::getIdempotencyRecord(
    const std::string& operation_scope,
    const std::string& idempotency_key,
    CameraIdempotencyRecord& record,
    bool& found,
    std::string& error
) const {
    found = false;
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "SELECT operation_scope,idempotency_key,request_digest,resource_id,response_status,"
        "response_json::text,created_at_ms,expires_at_ms FROM camera_idempotency_keys "
        "WHERE operation_scope=? AND idempotency_key=? AND expires_at_ms > "
        "(EXTRACT(EPOCH FROM clock_timestamp())*1000)::BIGINT;", statement, error)) return false;
    bindText(statement.get(), 1, operation_scope);
    bindText(statement.get(), 2, idempotency_key);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return true;
    if (result != SQLITE_ROW) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    record.operation_scope = columnText(statement.get(), 0);
    record.idempotency_key = columnText(statement.get(), 1);
    record.request_digest = columnText(statement.get(), 2);
    record.resource_id = columnText(statement.get(), 3);
    record.response_status = sqlite3_column_int(statement.get(), 4);
    record.response_json = columnText(statement.get(), 5);
    record.created_at_ms = sqlite3_column_int64(statement.get(), 6);
    record.expires_at_ms = sqlite3_column_int64(statement.get(), 7);
    found = true;
    return true;
}

bool CameraTaskRepository::storeIdempotencyRecord(
    const CameraIdempotencyRecord& record,
    std::string& error_code,
    std::string& error
) const {
    error_code.clear();
    const auto response = json::parse(record.response_json, nullptr, false);
    if (!validServiceIdentifier(record.operation_scope, 200) ||
        record.idempotency_key.empty() || record.idempotency_key.size() > 160 ||
        record.request_digest.empty() || record.resource_id.empty() ||
        record.response_status < 100 || record.response_status > 599 ||
        response.is_discarded() || record.created_at_ms <= 0 ||
        record.expires_at_ms <= record.created_at_ms) {
        error_code = "INVALID_IDEMPOTENCY_RECORD";
        error = "idempotency record is outside allowed bounds";
        return false;
    }
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "INSERT INTO camera_idempotency_keys(operation_scope,idempotency_key,request_digest,resource_id,"
        "response_status,response_json,created_at_ms,expires_at_ms) VALUES(?,?,?,?,?,?::jsonb,?,?) "
        "ON CONFLICT(operation_scope,idempotency_key) DO NOTHING;", statement, error)) return false;
    bindText(statement.get(), 1, record.operation_scope);
    bindText(statement.get(), 2, record.idempotency_key);
    bindText(statement.get(), 3, record.request_digest);
    bindText(statement.get(), 4, record.resource_id);
    sqlite3_bind_int(statement.get(), 5, record.response_status);
    bindText(statement.get(), 6, record.response_json);
    sqlite3_bind_int64(statement.get(), 7, record.created_at_ms);
    sqlite3_bind_int64(statement.get(), 8, record.expires_at_ms);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    if (sqlite3_changes(db.get()) > 0) return true;
    error_code = "IDEMPOTENCY_KEY_EXISTS";
    error = "idempotency key already exists";
    return false;
}

bool CameraTaskRepository::stats(CameraTaskRepositoryStats& stats, std::string& error) const {
    stats = {};
    DbPtr db;
    if (!openDatabase(config_, db, error)) return false;
    StatementPtr statement;
    if (!prepare(db.get(),
        "SELECT "
        "(SELECT COUNT(*) FROM camera_tasks),"
        "(SELECT COUNT(*) FROM camera_tasks WHERE deleted_at_ms IS NULL AND enabled=1),"
        "(SELECT COUNT(*) FROM camera_tasks WHERE deleted_at_ms IS NOT NULL),"
        "(SELECT COUNT(*) FROM camera_task_runs),"
        "(SELECT COUNT(*) FROM camera_task_runs WHERE status IN "
            "('queued','starting','running','reconnecting','stopping')) ,"
        "(SELECT COUNT(*) FROM camera_task_runs WHERE status='failed'),"
        "(SELECT COUNT(*) FROM camera_frames),"
        "(SELECT COUNT(*) FROM security_alert_events),"
        "(SELECT COALESCE(SUM(size_bytes),0) FROM camera_frames),"
        "(SELECT COALESCE(MAX(capture_time_ms),0) FROM camera_frames);",
        statement, error)) return false;
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        error = sqlite3_errmsg(db.get());
        return false;
    }
    stats.tasks_total = sqlite3_column_int64(statement.get(), 0);
    stats.tasks_enabled = sqlite3_column_int64(statement.get(), 1);
    stats.tasks_deleted = sqlite3_column_int64(statement.get(), 2);
    stats.runs_total = sqlite3_column_int64(statement.get(), 3);
    stats.runs_active = sqlite3_column_int64(statement.get(), 4);
    stats.runs_failed = sqlite3_column_int64(statement.get(), 5);
    stats.frames_total = sqlite3_column_int64(statement.get(), 6);
    stats.alerts_total = sqlite3_column_int64(statement.get(), 7);
    stats.archive_bytes = sqlite3_column_int64(statement.get(), 8);
    stats.latest_frame_time_ms = sqlite3_column_int64(statement.get(), 9);
    return true;
}

}  // namespace yolo11_server
