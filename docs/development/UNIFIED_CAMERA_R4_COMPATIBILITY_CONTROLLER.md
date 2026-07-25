# Unified Camera R4 — People Flow compatibility controller

> Status: complete; local acceptance passed
> Date: 2026-07-25
> Previous gate: R3 People Flow-equivalent Camera Pipeline output
> Next gate: R5 unified Worker default and readiness fencing

## Objective

Keep the frozen `/api/v1/people-flow/*` contract while making those routes
operate the canonical Camera Definition, Camera Run, Camera Pipeline, analysis
snapshot, alerts, and annotated JPEG.

R4 remains rollback-safe: `runtime.unified_camera_pipeline` defaults to
`false`. With that value, every People Flow handler continues to execute the
pre-R4 Redis/Worker path.

## Delivered

- Added `PeopleFlowCompatibilityController`.
- Shared one `CameraTaskRepository`, `CameraTaskQueue`,
  `CameraProfileRegistry`, and `UnifiedCameraApplicationService` between the
  Camera API and compatibility controller.
- Adapted all existing routes:
  - start;
  - stop;
  - status;
  - realtime;
  - snapshot;
  - four-stage security;
  - crossing events.
- Preserved the existing route-level authentication matrix: start/stop require
  Bearer authentication; read routes did not gain authentication.
- Preserved `pf_*` session IDs by using `session_id = run_id`.
- Preserved duplicate active-camera `409 CAMERA_ALREADY_ACTIVE`.
- Preserved terminal repeated-stop
  `409 SESSION_ALREADY_FINISHED`.
- Preserved existing JSON field names, URL fields, JPEG content type, and
  public error-code vocabulary.
- Rejected request-supplied RTSP URI/credentials exactly as before.
- Created or aligned the compatibility Camera Definition before starting.
- Forced the immutable compatibility RunSpec to contain:
  - `origin=people_flow_compat`;
  - legacy session ID;
  - People Flow plus enabled security algorithms;
  - config version;
  - initial occupancy;
  - snapshot FPS;
  - compatibility projection metadata.
- Added lookup by `legacy_session_id`.
- Added unified-first, legacy-fallback reads for session artifacts.
- Merged canonical People Flow alerts with historical
  `pf_crossing_events`, sorted by event time/event ID and deduplicated.

## Compatibility projections

For compatibility Runs, PostgreSQL keeps the legacy read model synchronized:

- Run creation and initial `pf_sessions` projection are one transaction.
- Run state transitions update the matching `pf_sessions` state.
- Analysis result persistence updates IN, OUT, occupancy, and consistency.
- A canonical People Flow alert and its `pf_crossing_events` projection are
  committed in the same alert/outbox transaction.

The canonical Camera tables remain the runtime source of truth. The `pf_*`
tables are retained for historical reads and rollback; they do not own a
second pipeline.

## Feature switches

Both server and worker YAML retain:

```yaml
runtime:
  unified_camera_pipeline: false
  people_flow_compatibility: true
  legacy_people_flow_fallback: true
  shadow_compare: false
```

R4 does not change the checked-in default. To exercise the unified
compatibility path, server and worker must both set
`unified_camera_pipeline: true`. Setting it back to `false` restores the old
People Flow producer/Worker path without a database rollback.

## Golden and real-backend contract

`people_flow_compatibility_contract_test` executes the real compatibility
controller and repositories against disposable PostgreSQL. Its control-plane
transport is deterministic in-memory so no camera or Worker is required.

The test verifies:

- start request-to-RunSpec mapping;
- exact start/status/realtime/stop JSON;
- `pf_` ID and Camera Run identity;
- duplicate start;
- annotated JPEG;
- phase1–phase4 security JSON and phase4 demo marker;
- canonical/legacy event merge and deduplication;
- terminal repeated stop;
- final `pf_sessions` projection.

The legacy start and response documents are normalized only for the generated
session ID. All remaining fields must compare equal; the observed Golden diff
is zero.

## Acceptance evidence

- Full backend configure/build: passed.
- Disposable PostgreSQL compatibility contract: passed.
- Full CTest with disposable PostgreSQL and an isolated Redis logical DB:
  21/21 passed, 0 failed, 0 skipped.
- `python tools/qt_demo_contract_test.py`: passed.
- Qt 6.11.1 MinGW Release build/deployment: passed; the unchanged client
  executable was produced at
  `out/build/qt-client-Release/people_flow_qt_client.exe`.
- Existing Camera HTTP contract and Camera Admin behavior tests: passed.

During the first full run, the existing R3 algorithm test exposed an invalid
Windows path conversion when the temporary directory contains non-ASCII
characters. The test now converts its native path to UTF-8 explicitly before
passing it to the production UTF-8 path API. No production path or algorithm
behavior changed.

## Rollback

1. Set `runtime.unified_camera_pipeline: false` in server and worker config.
2. Restart server and worker.
3. The unchanged legacy People Flow controller submits the unchanged legacy
   task and the old Worker consumes it.
4. Leave additive Camera Run columns/tables and compatibility projections in
   place; older binaries ignore them.

## R5 entry gate

R5 may begin only after this R4 version is pushed to GitHub and its source
contract checks pass. R5 must then make the Worker honor the same runtime mode,
prevent legacy and Camera consumers from owning one camera simultaneously,
fence Run leases/generations, and fail readiness on server/worker mode
mismatch.
