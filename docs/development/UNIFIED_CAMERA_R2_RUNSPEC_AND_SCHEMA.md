# Unified Camera R2 — immutable RunSpec and additive schema

> Status: complete; acceptance verification passed
> Date: 2026-07-25
> Previous gate: R0-R1 application boundary accepted
> Next gate: R3 People Flow-equivalent Camera Pipeline output

## Objective

Make every production-created Camera Run derive from one immutable
`CameraRunSpec`, add the persistence required for later People Flow
compatibility, and extend the Redis command contract without changing current
Camera or People Flow runtime behavior.

## Delivered

- Added immutable `CameraRunSpec` with:
  - stable Run and Camera identity;
  - origin and Definition version;
  - frame-output snapshot;
  - analysis Profile, algorithm list, config version, initial occupancy,
    snapshot FPS, and parameter snapshot;
  - callback Profile;
  - compatibility metadata.
- Deleted copy/move assignment for `CameraRunSpec`; callers receive only const
  accessors and immutable projections.
- Changed both production Camera Run creation paths to use the RunSpec:
  - normal HTTP/application-service start;
  - Worker restart recovery.
- Kept the former flat Definition JSON fields and added the canonical RunSpec
  fields, so rollback readers retain their original inputs.
- Added PostgreSQL migration
  `db/postgresql/003_people_flow_camera_unification.sql`.
- Added these additive `camera_task_runs` fields:
  - `origin`, defaulting to `camera_api`;
  - nullable `legacy_session_id`;
  - nullable `analysis_config_version`.
- Added the partial unique legacy-session index and
  Camera/origin/create-time index.
- Added `camera_run_analysis_results` and repository read/upsert operations.
  R2 does not yet write this table from the live Pipeline.
- Extended Camera start commands to `command_version 3`.
- Kept command version 1/2 deserialization compatible by applying safe defaults
  when all R2 fields are absent.
- Added deterministic RunSpec tests, Redis new/old command round-trip coverage,
  and PostgreSQL 001 -> 002 -> 003 upgrade/idempotence coverage.
- Kept `/people-flow/*`, `PeopleFlowSessionRunner`, legacy Redis keys, and
  Worker selection unchanged.

## Compatibility invariants

- Existing Camera HTTP request and response JSON is unchanged.
- Existing People Flow HTTP and Qt behavior is unchanged.
- Camera Run IDs retain the `cr_` prefix.
- Camera Profile references remain credential-free.
- Current Camera capture, sampling, analysis, alert, callback, and stop behavior
  is unchanged; R2 command fields are snapshots reserved for later phases.
- Old command messages remain consumable.
- Old binaries can ignore the new table and columns, read existing Run columns,
  and insert Camera Runs using the `origin` default.
- Migration 003 contains no drop, rename, or destructive data rewrite.

## Database verification

The repository test constructs an actual 001/002 PostgreSQL schema, inserts a
legacy Camera Task and Run, and then applies the R2 repository migration.

Verified:

- the legacy Run survives;
- `origin` is backfilled by the non-destructive default;
- nullable compatibility fields remain null;
- schema version 3 is recorded once;
- standalone migration 003 can be executed repeatedly;
- repository initialization can be executed repeatedly;
- new Run metadata round-trips;
- `camera_run_analysis_results` round-trips structured state.

## Redis verification

The Redis integration test:

1. submits and consumes a command-version-3 message;
2. verifies every new RunSpec field;
3. injects a command-version-2 message without R2 fields;
4. verifies compatible defaults;
5. acknowledges both messages and removes its isolated test stream.

## Full acceptance evidence

Backend command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
```

Environment:

- disposable PostgreSQL 17;
- disposable Redis 7;
- destructive PostgreSQL test opt-in enabled;
- no persistent Docker volumes.

Observed result:

- configure and build succeeded;
- 20/20 CTest cases executed;
- 20 passed, 0 failed, 0 skipped;
- Camera HTTP Golden contract passed;
- People Flow repository and deterministic behavior tests passed;
- migration, callback, frame, storage, lease, and algorithm integration tests
  passed.

Qt compatibility command:

```powershell
python .\tools\qt_demo_contract_test.py
```

Observed result: passed.

## Rollback

Application rollback is source-only:

1. deploy the R0-R1 binary;
2. let the old binary ignore the new columns and table;
3. old Redis command consumers continue to consume the original fields;
4. do not drop migration-003 objects during rollback.

Keeping the additive database objects avoids data loss and permits a later
forward deployment. No People Flow runtime switch exists to reverse in R2.

## R3 entry gate

R3 may add People Flow-equivalent output to the unified Camera Pipeline only
after:

- this R2 version is published to GitHub;
- the GitHub source-contract checks pass;
- the R2 branch remains free of a People Flow controller/runtime switch;
- deterministic output work is kept behind the current application and
  RunSpec boundaries.

R3 must not yet route `/people-flow/*` to the unified runtime; that switch
belongs to R4.
