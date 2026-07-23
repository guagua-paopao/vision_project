# M3 — multi-role VisionWorkerHost and session scheduling

> Status: complete  
> Date: 2026-07-20  
> Next gate: M4 Camera Task SQLite repository and Redis command layer

## Objective

Turn the existing `four_stage_worker.exe` composition root into a multi-role
`VisionWorkerHost`, make the Host the sole owner of the process-local Hub
Registry, and establish non-blocking Camera Task command/session scheduling
before persistence and extraction behavior are added.

## Delivered architecture

- `four_stage_worker.exe` remains the only worker executable and now constructs
  `VisionWorkerHost`.
- `VisionWorkerHost` creates exactly one `SharedCameraFrameHubRegistry`, passes
  it to the People Flow role, and exposes the same Registry to the Camera Task
  Manager factory.
- Host startup rejects `worker.worker_num != 1` with
  `VISION_WORKER_REQUIRES_WORKER_NUM_ONE`.
- The Host starts People Flow first, then the optional Camera Task role, and
  rolls back already-started components when a later component fails.
- Shutdown order is Camera Task consumer/sessions, People Flow runner, then
  Registry `stopAll()`. This ensures sessions release subscriptions before the
  decoder is forcibly stopped.
- Worker heartbeat identity is now `vision_host`; advertised capacity is one
  People Flow session plus configured Camera Runs.
- `CameraTaskManager` has an injectable command source and session factory. Its
  consumer thread never executes a long-running session inline.
- The Manager enforces maximum active runs, idempotently acknowledges duplicate
  START/STOP commands, starts each extraction session in its own thread, and
  joins sessions outside the session-map lock.
- Command source polling, factory errors, failure callbacks, and shutdown are
  separated behind explicit exception boundaries.

## Configuration added

`CameraTasksSection` and shipped `camera_tasks` YAML now contain the approved
disabled-by-default fields for database/output paths, Redis stream/group,
run/writer capacity, status/lease timing, retention, token environment name,
and task defaults. Values are normalized and bounded during YAML loading.

The Camera Task role remains disabled in shipped configuration for this
milestone. M4 provides the production Redis/SQLite adapters, and M5 provides
the real extraction session and shared writer implementation. The scheduling
interfaces are already production-wired through the Host factory boundary, so
those milestones do not change Host ownership.

## Changed files

Host and manager:

- `include/server/vision_worker_host.h`
- `src/server/vision_worker_host.cpp`
- `include/server/camera_task_manager.h`
- `src/server/camera_task_manager.cpp`
- `src/server/main_people_flow_worker.cpp`
- `src/server/people_flow_inference_worker.cpp`

Configuration and build:

- `include/server/app_config.h`
- `src/server/app_config.cpp`
- `config/server.yaml`
- `config/worker.yaml`
- `CMakeLists.txt`
- `scripts/build_backend.ps1`

Deterministic scheduling test:

- `tests/camera_task_manager_test.cpp`

## Verification evidence

Command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
```

Result: build succeeded and CTest passed 6/6. The new
`camera_task_manager_test` verifies:

- two long-running extraction sessions execute concurrently;
- a third START is rejected with `CAMERA_RUN_CAPACITY_EXCEEDED`;
- STOP terminates only the addressed run;
- the command consumer continues to dispatch after sessions start;
- a simulated independent People Flow role remains alive through Camera Task
  START/STOP operations;
- accepted and idempotent commands are acknowledged, while a capacity-rejected
  START remains pending for later reclaim; shutdown joins every session.

Compatibility command:

```powershell
python .\tools\qt_demo_contract_test.py
```

Result: passed. Static target inspection confirms there is still only
`four_stage_worker`; no `camera_frame_worker` target or main source exists.

## Residual risks and deferred work

- The Manager is intentionally adapter-driven at M3. Its production Redis
  source and SQLite transition callbacks are M4 deliverables.
- The bounded JPEG writer and real Hub-consuming extraction session are M5
  deliverables; the Host factory boundary preserves shared ownership until
  they are installed.
- Unified heartbeat currently reuses the established People Flow Redis
  heartbeat writer. Camera run counts are represented as advertised capacity;
  detailed active counts and Hub snapshots are added to hot diagnostics in
  M6.

## M4 entry criteria

- One Host-owned Hub Registry shared by roles: passed.
- One-worker deployment guard: passed.
- Long Camera sessions do not execute in command consumer: passed.
- Capacity, independent stop, continued dispatch, and complete join: passed.
- Deterministic shutdown order: implemented and inspected.
- Build 6/6 CTest and Qt contract: passed.

M4 is authorized to begin.
