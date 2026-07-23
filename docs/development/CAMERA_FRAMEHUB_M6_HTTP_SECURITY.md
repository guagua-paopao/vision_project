# M6 — HTTP API, authentication, and Hub diagnostics

> Status: complete  
> Date: 2026-07-20  
> Next gate: M7 integration, fault injection, operations, and final acceptance

## Objective

Expose the Camera Task control plane through the existing HTTP server without
changing any People Flow route, resolving RTSP credentials in the server, or
adding a Camera worker process. The API must combine durable SQLite state with
Redis hot state, enforce optimistic concurrency, and fail closed when its
security prerequisites are absent.

## Delivered API surface

Routes are registered only when `camera_tasks.enabled=true`:

- `POST/GET /api/v1/camera-tasks`
- `GET/PATCH/DELETE /api/v1/camera-tasks/{task_id}`
- `POST /api/v1/camera-tasks/{task_id}/start`
- `POST /api/v1/camera-tasks/{task_id}/stop`
- `GET /api/v1/camera-tasks/{task_id}/status`
- `GET /api/v1/camera-tasks/{task_id}/latest-frame`
- `GET /api/v1/camera-tasks/{task_id}/runs`
- `GET /api/v1/camera-hubs`
- `GET /api/v1/camera-hubs/{camera_profile}`

Hub endpoints are deliberately read-only. There is no HTTP operation that can
open, close, restart, or otherwise take ownership of a shared Hub.

## Persistence and runtime semantics

- CRUD is backed by the M4 SQLite repository and returns an `ETag` after
  successful writes. PATCH requires `If-Match`; stale versions return 409.
- Active tasks cannot be modified or deleted.
- START first creates one durable queued Run, then publishes a nonsecret Redis
  command. The partial unique index and manager guard make duplicate START
  idempotent and prevent a second Subscription.
- STOP uses the Redis stop flag and advances the durable Run to `stopping`.
  Repeated requests remain safe.
- Status merges the durable task/current Run with Redis hot status. If Redis
  status is missing, the API returns the durable state with
  `runtime_stale=true` rather than inventing a live state.
- The server passes only `camera_profile` identifiers. RTSP URI lookup remains
  exclusively inside the existing worker process.
- `latest-frame` accepts only the managed latest path, rejects reparse points,
  and verifies JPEG start/end markers before responding.

## Security controls

- Every Camera Task and Camera Hub route requires the configured Bearer token.
  Comparison is constant-time, and neither token nor authorization header is
  included in an error response.
- Creation and update use strict field allowlists. URI-, URL-, credential-,
  username-, password-, token-, secret-, and path-like fields are rejected.
- Task/profile identifiers use the existing ASCII safe-identifier policy;
  names are validated as UTF-8 and capped by encoded length.
- Unified errors include a request ID and public error code while filtering
  internal messages and URI-shaped source errors.
- SQLite stores only profile references and task definitions. Redis commands
  and hot status are nonsecret; source error text is sanitized before publish.
- A missing admin token is reported only as a boolean health/readiness failure.

## Health and readiness

The existing `/api/v1/health` and `/api/v1/ready` routes retain their People
Flow fields and add Camera Task checks:

- controller/storage/schema initialization;
- output-root write probe;
- admin-token presence (boolean only);
- `worker_num == 1` deployment guard;
- a live `vision_host` heartbeat advertising the `camera_frame` role.

Camera Task readiness fails explicitly when any of those required conditions
is absent. This prevents a multi-worker deployment from falsely claiming a
single shared decode.

## Changed files

API and integration:

- `include/server/camera_task_api_control.h`
- `include/server/camera_task_http_controller.h`
- `src/server/camera_task_http_controller.cpp`
- `include/server/camera_task_queue.h`
- `src/server/camera_task_queue.cpp`
- `include/server/people_flow_http_server.h`
- `src/server/people_flow_http_server.cpp`

Runtime sanitization and test/build wiring:

- `src/business/camera_frame_extraction_session.cpp`
- `tests/camera_task_http_contract_test.cpp`
- `CMakeLists.txt`
- `scripts/build_backend.ps1`

## Contract evidence

`camera_task_http_contract_test` exercises the controller directly with a real
temporary SQLite database and a deterministic API-control fake. It verifies:

- authentication failure without token disclosure;
- secret/URI field rejection without reflecting submitted values;
- strict unknown-field rejection;
- Unicode task names, create/list/get, ETag, If-Match, and version conflict;
- START and duplicate START behavior;
- prohibition of active-task updates;
- SQLite stale fallback and Redis hot-state merge;
- sanitized Hub diagnostics and read-only list/detail endpoints;
- STOP and repeated STOP behavior;
- Run history and complete latest JPEG delivery;
- terminal transition followed by soft delete;
- disabled camera-profile rejection.

Authoritative verification:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
python .\tools\qt_demo_contract_test.py
```

- All backend targets built.
- CTest passed 9/9, including the new HTTP contract test.
- Existing Qt demo contract remains part of the build script and passed.

## Residual M7 work

- Repeat the complete regression after fault-injection additions.
- Record bounded-queue, disconnect/recovery/resolution-change, Redis/SQLite,
  and output-path failure evidence in one acceptance matrix.
- Publish user-facing API and operations documents, deployment/rollback steps,
  and an explicit live-RTSP validation procedure where local infrastructure is
  unavailable.
- Run static scans for forbidden secrets, unsafe output paths, and accidental
  `camera_frame_worker` targets.

## M7 entry criteria

- Camera Task CRUD/run/output/diagnostic routes are implemented: passed.
- Bearer authentication and strict payload policy are enforced: passed.
- ETag/If-Match and active-task guards are enforced: passed.
- SQLite/Redis status merge and stale fallback are deterministic: passed.
- Health/readiness exposes the single-worker/WorkerHost contract: passed.
- API contract suite and existing regressions pass: passed.

M7 is authorized to begin.
