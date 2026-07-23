"""One-time import of legacy project SQLite data into PostgreSQL.

Production Server/Worker code does not link SQLite. This tool deliberately uses
Python's read-only sqlite3 module only while importing an old deployment.
The PostgreSQL DSN is read exclusively from YOLO11_POSTGRES_DSN.
"""

from __future__ import annotations

import argparse
import os
import sqlite3
from pathlib import Path
from typing import Iterable, Sequence

try:
    import psycopg
    from psycopg import sql
except ImportError as exc:  # pragma: no cover - operator setup failure
    raise SystemExit(
        "psycopg is required; run: pip install -r scripts/requirements-postgresql-migration.txt"
    ) from exc


CAMERA_TABLES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("camera_tasks", ("task_id","name","camera_profile","enabled","frame_interval_ms","output_mode","jpeg_quality","max_width","max_height","retention_days","max_saved_frames","version","created_at_ms","updated_at_ms","deleted_at_ms")),
    ("camera_task_runs", ("run_id","task_id","definition_version","definition_json","status","camera_profile","hub_instance_id","create_time_ms","start_time_ms","stop_time_ms","last_update_ms","worker_consumer","capture_backend","capture_fps","save_fps","consumed_frames","saved_frames","skipped_frames","dropped_frames","last_source_sequence","last_frame_time_ms","width","height","stop_reason","error_code","error_message")),
    ("camera_frames", ("frame_id","task_id","run_id","source_sequence","capture_time_ms","save_time_ms","relative_path","width","height","size_bytes")),
)

PEOPLE_FLOW_TABLES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("pf_sessions", ("session_id","camera_id","status","start_time_ms","stop_time_ms","initial_occupancy","in_count","out_count","final_occupancy","config_version","stop_reason","error","consistency_ok")),
    ("pf_crossing_events", ("event_id","session_id","camera_id","line_id","event_time_ms","direction","track_id","confidence","point_x_norm","point_y_norm","evidence_path","config_version")),
    ("pf_aggregates_minute", ("camera_id","bucket_start_ms","in_count","out_count","occupancy_end")),
    ("pf_calibration_audit", ("audit_id","camera_id","session_id","before_occupancy","after_occupancy","reason","operator_name","timestamp_ms")),
)


def table_exists(connection: sqlite3.Connection, table: str) -> bool:
    row = connection.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (table,)
    ).fetchone()
    return row is not None


def import_table(pg: psycopg.Connection, source: sqlite3.Connection, table: str, columns: Sequence[str]) -> int:
    if not table_exists(source, table):
        return 0
    selected = ",".join(f'"{column}"' for column in columns)
    rows = source.execute(f'SELECT {selected} FROM "{table}"').fetchall()
    if not rows:
        return 0
    statement = sql.SQL("INSERT INTO {} ({}) VALUES ({}) ON CONFLICT DO NOTHING").format(
        sql.Identifier(table),
        sql.SQL(",").join(map(sql.Identifier, columns)),
        sql.SQL(",").join(sql.Placeholder() for _ in columns),
    )
    with pg.cursor() as cursor:
        cursor.executemany(statement, rows)
    return len(rows)


def import_database(pg: psycopg.Connection, path: Path | None, tables: Iterable[tuple[str, tuple[str, ...]]]) -> int:
    if path is None:
        return 0
    if not path.is_file():
        raise FileNotFoundError(path)
    source = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True)
    try:
        return sum(import_table(pg, source, table, columns) for table, columns in tables)
    finally:
        source.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--camera-db", type=Path, default=Path("runtime/data/camera_tasks.db"))
    parser.add_argument("--people-flow-db", type=Path, default=Path("runtime/data/people_flow.db"))
    parser.add_argument("--schema", type=Path, default=Path("db/postgresql/001_initial_schema.sql"))
    args = parser.parse_args()
    dsn = os.environ.get("YOLO11_POSTGRES_DSN")
    if not dsn:
        raise SystemExit("YOLO11_POSTGRES_DSN is not configured")
    if not args.schema.is_file():
        raise SystemExit(f"schema file not found: {args.schema}")

    camera_path = args.camera_db if args.camera_db.is_file() else None
    people_path = args.people_flow_db if args.people_flow_db.is_file() else None
    if camera_path is None and people_path is None:
        raise SystemExit("no legacy SQLite database was found")

    with psycopg.connect(dsn) as pg:
        with pg.cursor() as cursor:
            cursor.execute(args.schema.read_text(encoding="utf-8"))
        camera_rows = import_database(pg, camera_path, CAMERA_TABLES)
        people_rows = import_database(pg, people_path, PEOPLE_FLOW_TABLES)
        # Explicit legacy audit_id values do not advance an identity sequence.
        # Align it before the application accepts new calibration writes.
        with pg.cursor() as cursor:
            cursor.execute(
                "SELECT setval(pg_get_serial_sequence('pf_calibration_audit','audit_id'), "
                "COALESCE((SELECT MAX(audit_id) FROM pf_calibration_audit), 1), "
                "EXISTS(SELECT 1 FROM pf_calibration_audit))"
            )
        pg.commit()
    print(f"PASS: imported {camera_rows} camera rows and {people_rows} people-flow rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
