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
- RTSP URI, callback endpoint/secret, PostgreSQL DSN, Redis credentials, and
  admin token are supplied only through environment variables.

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

## Callback delivery

Callback delivery is disabled by default. To enable the built-in
`backend_primary` profile, set the endpoint and HMAC secret in both Server and
Worker process environments, then set `callbacks.enabled: true` in both YAML
files:

```powershell
$env:YOLO11_CALLBACK_BACKEND_PRIMARY_URL = "https://backend.example/api/algorithm-alerts"
# Inject YOLO11_CALLBACK_BACKEND_PRIMARY_SECRET with the deployment secret
# manager; do not place its value in shell history or YAML.
```

The URL and secret values are resolved only by the Worker. The Server uses the
profile name as an allow-list so raw callback URLs can never enter Camera JSON.
Production profiles require HTTPS. `allow_insecure_http` exists only for
controlled loopback testing and should remain false in deployment.

Key controls:

- `request_timeout_ms`: timeout applied to WinHTTP phases;
- `lease_timeout_ms`: outbox ownership deadline, normalized to at least four
  request timeouts plus one second;
- `max_attempts`, `initial_backoff_ms`, `max_backoff_ms`: bounded exponential
  retry;
- `request_body_limit_bytes`: oversize alert payloads enter dead letter without
  network access;
- `response_body_limit_bytes`: caps captured response bytes; the SHA-256 hash
  still covers the complete response stream.

The receiver must verify the HMAC headers described in
`CAMERA_FRAME_TASK_API.md`, enforce timestamp freshness, and deduplicate by
`event_id`.

### Local callback receiver and Postman

P5 includes a dependency-free Node.js receiver. It binds only to loopback,
requires a control token for its test inspection routes, verifies the callback
HMAC and timestamp, and deduplicates accepted `event_id` values:

```powershell
$callbackSecret = Read-Host "Mock callback HMAC secret" -AsSecureString
$callbackCredential = [System.Net.NetworkCredential]::new("", $callbackSecret)
$controlSecret = Read-Host "Mock control token" -AsSecureString
$controlCredential = [System.Net.NetworkCredential]::new("", $controlSecret)
$env:YOLO11_MOCK_CALLBACK_SECRET = $callbackCredential.Password
$env:YOLO11_MOCK_CALLBACK_CONTROL_TOKEN = $controlCredential.Password
$env:YOLO11_MOCK_CALLBACK_PORT = "9095"
$env:YOLO11_MOCK_CALLBACK_FAIL_FIRST = "1"
node .\scripts\mock_callback_backend.js
```

Use the same HMAC value for
`YOLO11_CALLBACK_BACKEND_PRIMARY_SECRET`, set
`YOLO11_CALLBACK_BACKEND_PRIMARY_URL` to
`http://127.0.0.1:9095/api/v1/algorithm-alerts`, and enable
`allow_insecure_http` only in the loopback test profile.

The receiver itself has a deterministic acceptance script:

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\scripts\test_mock_callback_backend.ps1
```

Import both files into Postman:

- `postman/vision_project_p5.postman_collection.json`
- `postman/vision_project_p5.local.postman_environment.json`

Populate the environment's secret variables locally; do not export a filled
environment. The collection covers readiness, Camera CRUD, optimistic update,
start/stop, Run and alert audit, unified JSON/Prometheus metrics, dead-letter
inspection, and mock-backend inspection. Dead-letter replay is disabled in the
collection runner and must be invoked manually after reviewing the selected
entry.

With Server, Worker, the real camera, and the mock backend running, the complete
live chain is a single command:

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\scripts\verify_algorithm_service_p5.ps1 `
  -CameraProfile entry_camera_01 `
  -CallbackProfile backend_primary
```

It creates a temporary Camera, updates and starts it, waits for a live Pipeline,
checks unified metrics, waits until the signed alert appears at the mock
backend, validates Run history, and soft-deletes the temporary Camera in
`finally`. `-ControlPlaneOnly` skips the real-alert wait; it is not sufficient
for the P6 hardware release gate.

### Dead-letter operations

1. Query
   `GET /api/v1/operations/callbacks?status=dead_letter&limit=20`.
2. Inspect `event_id`, `camera_id`, profile, HTTP status, stable error code,
   response hash, and `attempt`.
3. Correct the external dependency or profile configuration.
4. POST `/api/v1/operations/callbacks/{outbox_id}/replay` with
   `If-Match: "<attempt>"` and `{}`.
5. Confirm the response is 202 and then observe `retry` → `delivered`.

Replay resets only the automatic attempt budget. It does not alter the stable
event ID, so the receiver's idempotency rule remains mandatory.

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

1. Confirm `GET /api/v1/ready` reports `ready: true`,
   `algorithm_runtime_fresh=true`, `inference_pool_ready=true`, and
   `callback_delivery_ready=true`.
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
| Callback timeout/408/429/5xx | durable outbox enters `retry` with exponential backoff |
| Callback other 4xx | durable outbox enters `dead_letter` immediately |
| Callback Worker crash after POST | expired lease is reclaimed; receiver deduplicates repeated `event_id` |
| Callback response body is large | capture is capped; complete response SHA-256 is stored, raw body is discarded |

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
