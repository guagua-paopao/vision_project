# Unified Camera R0-R1 — contract baseline and application boundary

> Status: implemented; disposable PostgreSQL verification pending
> Date: 2026-07-25
> Next gate: R2 immutable CameraRunSpec and additive schema

## Objective

Begin the approved People Flow and Camera business unification without changing
any public route, response, Worker role, database schema, or runtime behavior.
Move the canonical Camera start/stop orchestration behind an application
service that can later be shared by the legacy People Flow compatibility
controller.

## Delivered

- Added `UnifiedCameraApplicationService` as the non-HTTP application boundary.
- Moved Camera start orchestration into the service:
  - Camera Definition lookup and desired-state update;
  - Camera Profile validation;
  - active Run replay;
  - immutable definition snapshot creation;
  - Run persistence;
  - safe Redis command construction;
  - queue-submission failure transition.
- Moved Camera stop orchestration into the service:
  - desired-state update;
  - active/latest Run selection;
  - stop command submission;
  - transition to `stopping`;
  - existing replay behavior.
- Moved active-Run selection behind the same boundary.
- Replaced the controller-local lifecycle mutex with the application-service
  recursive lifecycle lock. Camera create/update/delete/start/stop therefore
  retain the existing nested-call serialization.
- Kept authentication, HTTP parsing, ETag, Idempotency-Key storage, response
  projection, status codes, and public error codes in the HTTP controller.
- Preserved the rare `ACTIVE_RUN_EXISTS` recovery response shape separately
  from a normal active-Run replay.
- Did not change People Flow routing or Worker execution in this phase.

## Changed files

- `include/server/unified_camera_application_service.h`
- `src/server/unified_camera_application_service.cpp`
- `include/server/camera_task_http_controller.h`
- `src/server/camera_task_http_controller.cpp`
- `CMakeLists.txt`
- `docs/PEOPLE_FLOW_CAMERA_UNIFICATION_REFACTOR_PLAN.md`

## Compatibility invariants

- Camera routes and JSON remain unchanged.
- Camera Run IDs retain the `cr_` prefix.
- Run definition JSON retains the existing fields.
- Camera commands still contain only safe Profile references and algorithm
  configuration, never RTSP credentials.
- Queue failure still leaves one durable failed Run.
- Stopping an already stopping Run still returns the existing replay response.
- The old People Flow runtime remains available and unchanged.

## Verification

Command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
```

Observed result:

- configure and build succeeded;
- `four_stage_server`, integration targets, and Camera HTTP contract target
  linked successfully;
- CTest reported 19/19 non-failing;
- 9 deterministic non-database tests passed;
- 10 PostgreSQL/Redis-dependent tests skipped because
  `YOLO11_TEST_POSTGRES_DSN` and destructive-test opt-in were not configured.

Qt compatibility command:

```powershell
python .\tools\qt_demo_contract_test.py
```

Observed result:

- passed;
- all existing People Flow endpoint wiring remains present.

## Residual verification requirement

Before R1 is accepted as complete in a release:

1. provide a disposable PostgreSQL database;
2. set `YOLO11_TEST_POSTGRES_DSN`;
3. set `YOLO11_ALLOW_DESTRUCTIVE_POSTGRES_TESTS=1`;
4. rerun the full backend suite;
5. require `camera_task_http_contract_test`,
   `algorithm_service_integration_test`, repository, callback, and frame tests
   to execute rather than skip.

The current machine had no configured disposable PostgreSQL DSN and no running
Docker daemon, so this requirement was recorded rather than bypassed.

## Rollback

Rollback is source-only:

1. restore Camera start/stop and active-Run selection to
   `CameraTaskHttpController`;
2. restore the controller-local recursive lifecycle mutex;
3. remove the application-service source from `camera_task_http`;
4. remove the two new application-service files.

No database, Redis, API, configuration, or runtime migration has occurred, so
no data rollback is required.

## R2 entry gate

R2 may introduce `CameraRunSpec` and additive persistence only after:

- the disposable PostgreSQL suite runs with zero failures;
- start/stop Golden responses match the pre-refactor baseline;
- reviewers approve the application-service boundary;
- no People Flow runtime switch is bundled into the same change.
