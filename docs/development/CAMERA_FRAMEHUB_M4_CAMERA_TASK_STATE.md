# M4 — Camera Task repository and Redis control plane

> Status: complete  
> Date: 2026-07-20  
> Next gate: M5 extraction session, JPEG publishing, and retention

## Objective

Add the durable Camera Task definition/run/frame model and the independent
Redis command, lease, stop, status, and Hub-diagnostic control plane required
by the approved option 3 architecture. Keep this layer independent of GPU
inference and do not introduce another worker executable.

## Delivered persistence model

- Added a dedicated SQLite repository using WAL, `synchronous=NORMAL`, foreign
  keys, a busy timeout, short per-operation connections, and schema versioning.
- Added `camera_tasks`, `camera_task_runs`, and `camera_frames` with the indexes
  and constraints frozen in the design document.
- Task CRUD supports soft deletion and optimistic version checks.
- Active Runs block task mutation/deletion. A partial unique index permits only
  one nonterminal Run per task.
- Run state transitions are conditional on allowed predecessor states, so a
  late writer or stale command cannot overwrite a terminal state.
- Run progress, terminal history, Frame metadata, latest/list queries, and
  stale-Run recovery are durable across repository reconstruction.
- Stale nonterminal Runs are recovered to `failed` with stable error code
  `WORKER_HEARTBEAT_STALE`.

## Delivered Redis control plane

- Added an independent hiredis connection and consumer group for the Camera
  Task stream. It is not shared with the blocking People Flow consumer.
- New commands are read with `XREADGROUP`; pending commands are reclaimed with
  `XAUTOCLAIM` after the larger of configured PEL idle time and lease TTL.
- Accepted and idempotent commands are acknowledged with `XACK`. Capacity-
  rejected START remains pending so another poll can reclaim it later.
- START publication contains only the nonsecret task-definition snapshot.
- Task/Run leases use `SET NX EX`; refresh and release use compare-and-act Lua
  scripts so another Run's lease cannot be modified.
- Stop requests, Run hot status, and Hub hot status use bounded-TTL keys.
- Task, Run, Profile, and consumer identifiers used in keys are restricted to
  safe identifier characters; RTSP URI and credentials never enter Redis.

The control-plane `CameraHubStatus` was split from OpenCV frame payload types.
This prevents the Redis layer from taking an accidental image/GPU dependency.

## Idempotency and recovery evidence

`camera_task_manager_test` now sends a duplicate START for the same Run while
two sessions are active. The duplicate is acknowledged, but the session
factory creation count stays at two. The same test verifies that a capacity-
rejected START is not acknowledged.

`camera_task_repository_test` verifies:

- schema initialization and reopening;
- task create/read/update/version conflict/soft delete;
- active-task mutation protection and active-Run uniqueness;
- conditional Run transitions and durable terminal history;
- Frame metadata and latest-frame lookup;
- stale Run recovery after a simulated worker loss;
- persistence after constructing a new repository instance.

## Changed files

Domain and persistence:

- `include/business/camera_task_types.h`
- `include/business/camera_task_repository.h`
- `src/business/camera_task_repository.cpp`
- `include/business/camera_hub_status.h`
- `include/business/camera_frame_types.h`

Redis and scheduling:

- `include/server/camera_task_queue.h`
- `src/server/camera_task_queue.cpp`
- `include/server/camera_task_manager.h`
- `tests/camera_task_manager_test.cpp`

Build and tests:

- `CMakeLists.txt`
- `scripts/build_backend.ps1`
- `tests/camera_task_repository_test.cpp`

## Verification evidence

Command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
```

Result: `camera_task_storage`, `camera_task_redis`, server, worker, and all
requested test targets built; CTest passed 7/7.

The first repository test run exposed a Windows Unicode path defect because
the temporary directory contains a Chinese user name. Test path conversion now
uses `generic_u8string()`, proving SQLite receives UTF-8 instead of relying on
the active ANSI code page.

Forcing `camera_task_redis` into the normal build then exposed an unintended
OpenCV include dependency. Moving Hub diagnostics to the control-plane header
fixed the layering, and the Redis library now compiles without OpenCV. No local
`redis-server`/`redis-cli` executable is installed in this workspace, so live
Redis round-trip, lease-expiry, and PEL failover tests remain explicitly in the
M7 container/integration gate rather than being reported as exercised here.

Repository Git metadata is malformed/unavailable (`git status` reports that
this is not a repository). These milestone journals therefore remain the
authoritative local trace until repository metadata is repaired externally.

## M5 entry criteria

- Definition and terminal state survive restart: passed.
- Optimistic locking and active-Run uniqueness: passed.
- Duplicate START creates no duplicate session/subscription: passed.
- Capacity rejection stays available for PEL reclaim: passed.
- PEL reclaim, compare-safe leases, stop/status keys: implemented and compiled.
- Camera control plane has no OpenCV/GPU dependency: passed.
- Full deterministic suite: 7/7 passed.

M5 is authorized to begin.
