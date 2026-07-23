# Camera FrameHub M8-M10 execution ledger

Date: 2026-07-21

This ledger is the durable hand-off record for the three post-M7 milestones.
Each milestone is implemented and accepted before the next one begins.

## Frozen architecture decisions

1. `four_stage_server` remains the control plane; `four_stage_worker` remains
   the data plane and owns the in-process shared FrameHub.
2. A Camera Task continues to reference a Camera Profile. RTSP URIs and
   credentials are never accepted by HTTP, returned by HTTP, or written to the
   profile registry.
3. Camera Profile writes persist only metadata and an environment-variable
   reference. A worker restart is required to activate profile registry
   changes because a running worker intentionally keeps an immutable registry.
4. The admin UI is a static, dependency-free web application served by the
   existing HTTP process. It stores the admin token only in browser memory.
5. Metrics and operations endpoints use the same Camera Task bearer token.
6. Storage pressure may stop an affected archive Run, but it must not stop the
   shared Hub or People Flow subscriber.
7. Runtime restore is offline-only, verifies a manifest and preserves a
   rollback copy before replacing data.

## M8 - operator control plane

Scope:

- Camera Task list/create/read/update/soft-delete/start/stop;
- status, Run history, latest JPEG preview and Hub diagnostics;
- Camera Profile metadata list/create/read/update/soft-delete;
- safe YAML persistence with optimistic concurrency;
- dependency-free web admin console;
- explicit `restart_required` semantics for profile mutations.

Acceptance:

- [x] Profile CRUD rejects URI/credential fields and unsafe environment names.
- [x] Task UI never places the bearer token in URL or persistent browser
      storage.
- [x] Active tasks cannot be changed or deleted.
- [x] Existing M0-M7 API contracts remain compatible.
- [x] Deterministic M8 tests pass.

## M9 - resilience and observability

Scope:

- authenticated JSON operations summary;
- Prometheus text metrics for tasks, Runs, Hub capture and storage;
- bounded soak harness with JSONL evidence and invariant checks;
- restart/Redis/camera-failure runbook and acceptance gates;
- existing stale Run recovery retained and surfaced.

Acceptance:

- [x] Metrics expose no RTSP URI, environment-variable name or bearer token.
- [x] The one-profile/one-open invariant is automatically checked.
- [x] Soak evidence records health, readiness, task status and Hub state.
- [x] Deterministic resilience tests and all earlier tests pass.

## M10 - storage lifecycle

Scope:

- archive-byte quota and minimum-free-space admission guard;
- high/critical pressure state in operations metrics;
- pressure-first oldest-archive cleanup in addition to per-task retention;
- offline backup, manifest hashing, integrity verification and guarded restore;
- operational documentation and drills.

Acceptance:

- [x] Archive publication is denied before configured critical pressure.
- [x] `latest.jpg` is never selected by retention cleanup.
- [x] No cleanup path can escape the managed output root.
- [x] Backups contain configuration, databases and a SHA-256 manifest, but no
      RTSP secret or process environment.
- [x] Restore refuses to run while demo processes are active and requires an
      explicit confirmation switch.
- [x] Full clean build and regression suite pass.

## Evidence log

| Milestone | Build/tests | Runtime evidence | Status |
|---|---|---|---|
| M8 | Release build; CTest 11/11; JS/UI guards pass | Profile CRUD contract and static admin console verified | accepted |
| M9 | Release build; CTest 11/11; PowerShell parser pass | JSON/Prometheus contracts and soak invariant harness pass | accepted; 72h field certificate generated at deployment |
| M10 | Release build; CTest 12/12; script parsers pass | real backup plus isolated Unicode-path restore pass | accepted |

## Final gate

- clean Release build: pass (89 actions);
- CTest: 12/12 pass;
- Qt contract and M0-M10 static release guard: pass;
- TensorRT engine load: pass;
- live control-plane health/readiness, admin page and JSON/Prometheus metrics:
  pass with storage pressure `normal`;
- process-log bearer/RTSP literal scan: zero matches;
- services stopped cleanly after acceptance;
- final report: `reports/vision_project_m8_m10_acceptance_2026-07-21.md`.
