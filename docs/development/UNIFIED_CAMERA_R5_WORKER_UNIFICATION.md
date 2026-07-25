# Unified Camera R5 — Worker unification and fencing

> Status: complete; local acceptance passed
> Date: 2026-07-25
> Previous gate: R4 People Flow compatibility controller
> Next gate: R6 Web feature parity

## Objective

Make `VisionWorkerHost` run exactly one camera business pipeline in unified
mode. People Flow remains an algorithm inside `CameraPipeline`; the legacy
`PeopleFlowInferenceWorker` and `PeopleFlowSessionRunner` remain compiled only
for rollback when `runtime.unified_camera_pipeline=false`.

R5 does not flip the checked-in deployment default. The production default is
changed only at the R8 release gate.

## Worker composition

When `runtime.unified_camera_pipeline=true`, the Worker now starts:

- one process-level Vision Worker coordination lease;
- one unified Camera command consumer;
- one `CameraTaskManager`;
- one `CameraPipeline` per active camera;
- the existing shared `FrameHub` registry;
- the existing fixed-size inference pool;
- the existing callback and retention workers;
- one Host-owned heartbeat thread.

It does not construct or start `PeopleFlowInferenceWorker`, so it cannot
consume the old People Flow command stream or create a second business
pipeline.

With the flag set to `false`, the rollback composition remains available. The
legacy role also participates in the process-level coordination lease so an
updated legacy Worker and a unified Worker cannot run together.

## Process and Run fencing

The process lease is stored at:

```text
yolo:camera:vision-worker:lease
```

Its value contains the runtime mode plus a random process-generation token.
Acquire, refresh, and release are compare-and-set Lua operations. A second
Worker, including one configured for the opposite mode, cannot acquire the
lease while the owner is live. Loss of ownership stops Camera pipelines and
the legacy role before the Host leaves the ready state.

Per-camera Run leases remain independent and retain their existing
`run_id|owner_generation` value. Only the owner generation may refresh or
release a Run lease. The restart path waits for the old lease to expire,
closes the abandoned Run with `WORKER_RESTARTED`, and creates a new immutable
Run generation for cameras whose desired state is running.

## Heartbeat and readiness

Unified mode no longer depends on the old People Flow Worker for heartbeat
publication. `VisionWorkerHost` publishes:

- `runtime_mode`;
- `worker_generation`;
- `legacy_people_flow_role`;
- `camera_task_manager_running`;
- `hub_registry_ready`;
- `coordination_healthy`;
- the existing inference, processor, callback, and active-pipeline snapshot.

`GET /api/v1/ready` now fails when:

- server and Worker runtime modes differ;
- an old People Flow role is visible in unified mode;
- zero or more than one Vision Worker generation is alive;
- the process coordination lease is unhealthy;
- `CameraTaskManager` is not running;
- the shared Hub registry is unavailable;
- the existing inference, callback, storage, Redis, or freshness checks fail.

Old heartbeats without the additive R5 fields are interpreted as
`legacy_split`, so a new unified Server rejects an old Worker instead of
silently accepting it.

## FrameHub subscriber identity

The sole unified business subscriber is now reported as
`camera_pipeline`. Redis Hub status keeps the old `camera_task_subscribers`
field and adds `camera_pipeline_subscribers`; this preserves old data parsing
while making the new ownership model observable. The Hub still opens one
reader per camera profile and the inference pool size remains fixed at
process startup.

## Recovery drill

`camera_task_lease_fence_test` covers:

- two process generations racing with the same consumer name;
- unified versus legacy mode overlap;
- non-owner process refresh/release rejection;
- lease expiry after a simulated crash;
- unified replacement ownership after expiry;
- per-camera Run lease generation fencing.

`exercise_worker_restart.ps1` now records and requires the R5 readiness fence
fields while verifying:

- the killed Worker leaves no FFmpeg child;
- the abandoned Run becomes `failed/WORKER_RESTARTED`;
- a new Run ID reaches `running`;
- the replacement heartbeat has a new worker generation.

## Acceptance evidence

- Backend configure and build: passed.
- CTest without services: 22 targets, 0 failures; external-service tests
  skipped by their guards.
- Full CTest with disposable PostgreSQL 17 and isolated Redis DB 14:
  22/22 passed, 0 failed, 0 skipped.
- Worker readiness unit cases: healthy unified Worker, old Worker mismatch,
  duplicate generations, and lost coordination lease all passed.
- Process/Run lease integration and simulated crash takeover passed.
- Existing Camera, People Flow compatibility, callback, repository, frame,
  inference, and algorithm tests passed.

## Rollback

1. Stop accepting new starts.
2. Stop and join active unified Runs.
3. Confirm the process and Run leases are released or expired.
4. Set `runtime.unified_camera_pipeline=false` in both Server and Worker.
5. Restart the Worker; it acquires the same process lease before starting the
   retained legacy People Flow role.
6. Confirm `/ready` reports `expected_runtime_mode=legacy_split`.

No schema rollback or data deletion is required.

## R6 entry gate

R6 may begin only after the R5 branch is pushed and its CI checks pass. R6
must add Web parity using Camera APIs only; the Web application must not call
`/people-flow/*`.
