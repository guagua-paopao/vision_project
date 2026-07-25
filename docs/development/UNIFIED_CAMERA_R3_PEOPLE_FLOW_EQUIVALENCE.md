# Unified Camera R3 — People Flow-equivalent Pipeline output

> Status: complete; acceptance verification passed
> Date: 2026-07-25
> Previous gate: R2 RunSpec and migration accepted
> Next gate: R4 People Flow compatibility controller

## Objective

Complete the People Flow analysis capabilities inside the existing Camera
Pipeline without routing any `/people-flow/*` request to it. R3 produces the
state and artifacts needed by the later compatibility controller while the old
People Flow runtime remains the production owner of the legacy API.

## Delivered

- Propagated immutable RunSpec analysis fields from `CameraPipeline` to every
  `CameraFrameJob`:
  - config version;
  - target inference FPS;
  - initial occupancy;
  - snapshot FPS;
  - algorithm parameter JSON.
- Propagated capture/reconnect context with each inference job.
- Changed `CameraAlgorithmProcessor` to use RunSpec values instead of mutable
  global People Flow defaults for counters and alert config versions.
- Added a complete per-Run hot analysis snapshot:
  - inference FPS, latency, frame count, and last update;
  - initial occupancy, IN, OUT, occupancy, and live persons;
  - reconnect generation and warmup remaining;
  - phase1 through phase4 security state;
  - annotated snapshot path;
  - storage and snapshot degradation flags.
- Added an analysis-only Redis writer. Pipeline and analysis writers update
  disjoint fields in the same Run status hash, so neither can erase the
  other's state.
- Added low-frequency and event-forced persistence to
  `camera_run_analysis_results`.
- Finalized the last analysis result when an inference generation detaches.
- Added annotated JPEG output at:

```text
{camera_tasks.output_dir}/{camera_id}/{run_id}/analysis/latest.jpg
```

- Added reconnect handling that resets tracker, per-track crossing state,
  renderer markers, and security temporal state, then reapplies configured
  warmup. Accumulated IN/OUT/occupancy is retained.
- Extended Camera status JSON additively with the unified analysis snapshot.
- Kept current alert persistence, callback outbox behavior, and public error
  handling unchanged.

## Differential evidence

The Camera algorithm test feeds one deterministic frame sequence through:

1. the legacy People Flow core components
   (`PersonDetectorAdapter`, `PersonTracker`, and `LineCrossingCounter`);
2. the unified `CameraAlgorithmProcessor`.

The test requires exact equality for:

- IN count;
- OUT count;
- final occupancy;
- live-person count.

It also verifies one canonical People Flow alert, all four security phase
objects, a decodable annotated JPEG, and the Redis-ready hot snapshot.

The reconnect portion changes the capture generation, processes the first
post-reconnect frame under warmup, and requires:

- no new crossing alert;
- unchanged accumulated counts;
- updated reconnect generation.

## Redis concurrency invariant

The Redis integration test writes a Pipeline snapshot and then an analysis
snapshot to the same Run hash. A subsequent read must retain both:

- Pipeline thread and sampling fields;
- config version, counts, security state, and snapshot path.

This protects the single logical Run snapshot from lost updates between the
capture and algorithm threads.

## API compatibility

Camera status receives additive fields under `analysis`. Existing fields and
routes remain unchanged. Missing or expired analysis state is reported with
`runtime_stale=true`; it is not represented as a healthy zero-value runtime.

No `/people-flow/*` controller, response projection, Redis legacy key, or Qt
route was changed in R3.

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
- Camera/People Flow differential, Redis merged status, annotated JPEG,
  migration, alert, callback, extraction, storage, and HTTP contract tests all
  passed.

Qt compatibility command:

```powershell
python .\tools\qt_demo_contract_test.py
```

Observed result: passed.

## Rollback

Rollback is source-only:

1. deploy the R2 binary;
2. leave migration-003 columns/table in place;
3. ignore R3 analysis fields already present in Redis hashes;
4. optionally remove generated
   `{camera_id}/{run_id}/analysis/latest.jpg` artifacts through the normal
   storage lifecycle.

The old People Flow runtime never stopped owning its routes, so no legacy
session or API rollback is required.

## R4 entry gate

R4 may introduce a compatibility controller only after:

- this R3 version is pushed to GitHub;
- GitHub source-contract checks pass;
- R3 remains free of `/people-flow/*` routing changes;
- the compatibility controller is specified as a projection over
  `UnifiedCameraApplicationService`, RunSpec, and the R3 snapshot;
- old success/error JSON, session IDs, auth, calibration, events, snapshot,
  and terminal-stop semantics are covered before any runtime switch.

R4 must remain feature-flagged and default to the old People Flow path.
