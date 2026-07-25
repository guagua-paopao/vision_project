BEGIN;

ALTER TABLE camera_task_runs
  ADD COLUMN IF NOT EXISTS origin TEXT NOT NULL DEFAULT 'camera_api';
ALTER TABLE camera_task_runs
  ADD COLUMN IF NOT EXISTS legacy_session_id TEXT;
ALTER TABLE camera_task_runs
  ADD COLUMN IF NOT EXISTS analysis_config_version TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS uq_camera_runs_legacy_session
  ON camera_task_runs(legacy_session_id)
  WHERE legacy_session_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_camera_runs_task_origin_time
  ON camera_task_runs(task_id,origin,create_time_ms DESC);

CREATE TABLE IF NOT EXISTS camera_run_analysis_results (
  run_id TEXT PRIMARY KEY REFERENCES camera_task_runs(run_id),
  task_id TEXT NOT NULL REFERENCES camera_tasks(task_id),
  initial_occupancy BIGINT NOT NULL DEFAULT 0,
  in_count BIGINT NOT NULL DEFAULT 0,
  out_count BIGINT NOT NULL DEFAULT 0,
  final_occupancy BIGINT NOT NULL DEFAULT 0,
  last_live_persons INTEGER NOT NULL DEFAULT 0,
  security_state_json JSONB NOT NULL DEFAULT '{}'::jsonb,
  snapshot_relative_path TEXT,
  storage_degraded SMALLINT NOT NULL DEFAULT 0 CHECK (storage_degraded IN (0,1)),
  snapshot_degraded SMALLINT NOT NULL DEFAULT 0 CHECK (snapshot_degraded IN (0,1)),
  last_update_ms BIGINT NOT NULL,
  finalized_at_ms BIGINT
);
CREATE INDEX IF NOT EXISTS idx_camera_run_analysis_task_update
  ON camera_run_analysis_results(task_id,last_update_ms DESC);

INSERT INTO camera_schema_version(version,applied_at_ms)
VALUES(3,(EXTRACT(EPOCH FROM clock_timestamp())*1000)::BIGINT)
ON CONFLICT(version) DO NOTHING;

COMMIT;
