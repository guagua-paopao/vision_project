# Camera ID Frame Extraction Operations

> M11 replaces the old public Camera Task CRUD contract. Operators manage a
> stable Camera resource; extraction Runs are internal execution/audit records.

## Runtime invariant

The selected option-3 architecture remains unchanged:

- one `four_stage_worker` process (`worker.worker_num: 1`);
- one process-local FrameHub and one FFmpeg decode per Camera Profile;
- People Flow and Camera extraction subscribe to the same decoded frames;
- no separate GPU or non-GPU `camera_frame_worker` process;
- PostgreSQL is the durable store, Redis is command/hot state, and the
  filesystem stores JPEG artifacts;
- RTSP URI, PostgreSQL DSN, Redis credentials, and admin token are supplied
  only through environment variables.

## PostgreSQL bootstrap

The repository includes a local PostgreSQL 17 deployment:

```powershell
$secure = Read-Host "PostgreSQL password" -AsSecureString
$credential = [System.Net.NetworkCredential]::new("", $secure)
$env:YOLO11_POSTGRES_PASSWORD = $credential.Password
docker compose -f .\deploy\postgresql\compose.yaml up -d
$env:YOLO11_POSTGRES_DSN = "host=127.0.0.1 port=5432 dbname=vision_project user=vision_app password=$($credential.Password) sslmode=disable"
$credential = $null
$secure.Dispose()
```

Do not print or persist the DSN. Production should inject it using a secret
manager. Both Server and Worker read the environment-variable name configured
by `camera_tasks.postgres_dsn_env` and
`people_flow.storage.postgres_dsn_env`.

For integration tests, create a separate disposable database and point
`YOLO11_TEST_POSTGRES_DSN` at it. Test executables drop/recreate their tables;
never set the test DSN to a production database.

The local Compose deployment creates `vision_project_test` on its first
initialization. Run the complete destructive integration suite explicitly:

```powershell
$env:YOLO11_TEST_POSTGRES_DSN = "host=127.0.0.1 port=5432 dbname=vision_project_test user=vision_app password=<password> sslmode=disable"
powershell -ExecutionPolicy Bypass -File .\scripts\test_postgresql_connection.ps1 `
  -ConfirmDisposableDatabase
```

## Build and start

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\verify_camera_frame_feature.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\start_demo.ps1 -SkipQt
```

`start_demo.ps1` prompts without echo for a missing RTSP URI, admin token, and
PostgreSQL DSN. It starts the existing Worker and Server only. Remove `-SkipQt`
to start the existing People Flow Qt client at the same time.

## Camera lifecycle contract

All routes require `Authorization: Bearer <admin token>`.

| Operation | Route | Extraction lifecycle |
|---|---|---|
| Create | `POST /api/v1/cameras` | Persist Camera; if enabled, create one internal Run and start its thread |
| Read | `GET /api/v1/cameras[/{camera_id}]` | Read Camera plus current Run/Hub state |
| Update | `PATCH /api/v1/cameras/{camera_id}` | Require ETag; stop/join old generation and start replacement under the same ID |
| Delete | `DELETE /api/v1/cameras/{camera_id}` | Require ETag; stop the current generation and soft-delete the Camera |
| Start/stop | `POST .../{camera_id}/start` or `/stop` | Idempotent explicit lifecycle control |
| History | `GET .../{camera_id}/runs` | Read-only internal execution history; no Run CRUD |

Camera Profiles are deployment-owned and read-only over HTTP. There are no
registered Profile POST/PATCH/DELETE routes and no `/api/v1/camera-tasks`
routes.

## Acceptance sequence

1. Confirm `GET /api/v1/ready` reports `ready: true`.
2. Open `http://127.0.0.1:8087/camera-admin`, save the Bearer token locally,
   and create an enabled Camera such as `entrance_extract_01`.
3. Confirm its status becomes `running` and `latest-frame` returns a JPEG.
4. Run People Flow for the same Profile. In Hub diagnostics, confirm
   `open_count=1` and subscriber types include both roles.
5. PATCH the Camera interval with its current ETag. Confirm the old Run becomes
   `stopping/stopped`, a new Run appears, and the Camera ID does not change.
6. Submit the same START twice. Confirm the active Run ID is reused.
7. DELETE the Camera with its current ETag. Confirm it disappears from normal
   list/detail reads and its extraction thread is reclaimed.
8. Confirm People Flow remains functional while Camera extraction is stopped,
   and extraction remains functional when a People Flow session stops.

## Failure behavior

| Fault | Expected behavior |
|---|---|
| PostgreSQL unavailable | repository initialization fails closed; readiness stays false |
| Redis unavailable | durable definitions remain; start/stop returns a sanitized 503 and hot status becomes stale |
| RTSP disconnect | the single shared Hub reconnects; consumers observe the same source state |
| Queue/storage pressure | extraction samples may be dropped and counted; People Flow and Hub stay alive |
| Worker crash | startup recovery marks stale nonterminal Runs failed before accepting new work |
| Stale ETag | PATCH/DELETE returns 409 without stopping the live generation |

## Legacy SQLite migration

Stop Server and Worker, back up the two SQLite files, initialize PostgreSQL,
then run the one-time read-only importer:

```powershell
python -m pip install -r .\scripts\requirements-postgresql-migration.txt
python .\scripts\migrate_sqlite_to_postgresql.py `
  --camera-db .\runtime\data\camera_tasks.db `
  --people-flow-db .\runtime\data\people_flow.db
```

After row-count and application acceptance, retain the SQLite backups for the
rollback window. Production binaries do not link SQLite.

## Backup and restore

With Server and Worker stopped and `PGHOST`, `PGPORT`, `PGDATABASE`, `PGUSER`,
and `PGPASSWORD` set:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\backup_runtime.ps1 -Label scheduled
powershell -ExecutionPolicy Bypass -File .\scripts\restore_runtime.ps1 `
  -BackupPath .\runtime\backups\vision_runtime_<stamp>.zip -ConfirmRestore
```

Backup uses `pg_dump` custom format plus a SHA-256 manifest. Restore validates
the archive, creates a pre-restore backup, then uses `pg_restore --clean
--if-exists`. JPEG output is excluded and should be copied separately when
archive-image recovery is required.
