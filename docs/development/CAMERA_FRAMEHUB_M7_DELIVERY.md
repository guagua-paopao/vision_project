# M7 — integration, fault injection, operations, and delivery

> Status: deterministic implementation complete  
> Date: 2026-07-20  
> Production gate: live RTSP/Redis/resource measurements remain environment-owned

## Objective

Close the option-3 implementation with cross-role integration evidence,
failure injection, bounded-resource checks, security/static guards, complete
operator/API documentation, and one reproducible release command.

## Integration and resilience additions

`camera_frame_resilience_test` adds two deterministic scenarios:

1. One profile with exactly one People Flow and three Camera Task subscribers.
   Stable operation creates one Hub, one source object, one source start, and
   reports `open_count=1`. All consumers reference the same immutable frame.
2. The source enters `reconnecting`; all four consumers observe the same state.
   It recovers through the same source object, exposes a reopen count and a
   320×240 to 640×360 resolution change, then continues on every independent
   cursor.
3. Individual Camera Task and People Flow subscriptions are released in turn;
   the source remains active until the final subscriber and idle grace expire.
4. Four profiles with two consumers each create exactly four sources. Replacing
   2,000 frames per profile retains only one latest immutable envelope per Hub,
   and all sources stop cleanly.

The extraction integration test now also saturates a writer configured with
global/per-Run capacity 1. Queue depth never exceeds the bound and excess jobs
are rejected rather than accumulated or applied as Hub back-pressure.

## Fault-injection evidence

- Shared capture disconnect/reconnect and resolution change: consistent Hub
  state; source object/factory remains singular.
- Redis Run hot-state outage: HTTP falls back to durable SQLite and marks
  `runtime_stale`, without reflecting connection text.
- Redis Hub diagnostic outage: sanitized 503.
- Redis START publication failure: the durable queued Run is compensated to
  terminal `failed/QUEUE_SUBMIT_FAILED`.
- SQLite busy: a real 100 ms `BEGIN IMMEDIATE` write lock blocks the competing
  repository update; the update then succeeds inside the five-second timeout.
- SQLite invalid target: schema initialization fails explicitly.
- Output path collision: only the affected Camera Run fails while the People
  Flow cursor and Hub continue.
- Retention traversal metadata: `../outside_sentinel.txt` is rejected; the
  outside file and suspicious metadata remain for operator review.
- Stale Worker/lease: prior repository test converts nonterminal state to
  `failed/WORKER_HEARTBEAT_STALE` exactly once.
- Active task update/delete, archive-only latest, oversized names,
  `worker_num>1`, and queue-failure compensation are covered at the HTTP layer.

## Documentation and release tooling

Added:

- `docs/CAMERA_FRAME_TASK_API.md`
- `docs/CAMERA_FRAME_TASK_OPERATIONS.md`
- `docs/CAMERA_FRAME_TASK_ACCEPTANCE.md`
- `scripts/verify_camera_frame_feature.ps1`

Updated:

- `README.md`
- `docs/ARCHITECTURE.md`
- `scripts/start_demo.ps1`
- `CMakeLists.txt`
- `scripts/build_backend.ps1`

The release guard rebuilds and runs the backend suite, runs the existing Qt
contract, and statically enforces:

- no `camera_frame_worker` target/source/script;
- no Camera extraction include of CUDA, TensorRT, or ModelRunner;
- no active RTSP URI in shipped YAML;
- FFmpeg required and CAP_ANY fallback disabled;
- token referenced only by environment-variable name;
- `worker_num=1` in shipped configurations;
- all required Camera Task/Hub routes remain registered in the controller.

The start script now creates the managed Camera Frame output root but still
starts only the existing single worker.

## Changed tests

- `tests/camera_frame_resilience_test.cpp` (new)
- `tests/camera_frame_extraction_test.cpp`
- `tests/camera_task_repository_test.cpp`
- `tests/camera_task_http_contract_test.cpp`

## Final deterministic verification

Authoritative command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_camera_frame_feature.ps1
```

Result:

```text
100% tests passed, 0 tests failed out of 10
Qt demo API workflow and client endpoint wiring: passed
Camera Frame option-3 architecture/security guard: passed
```

The ten CTest targets cover existing People Flow, security analytics,
People Flow repository, shared Hub, People Flow Hub regression, Camera manager,
Camera repository, extraction/output/retention, resilience/capacity, and HTTP
contract behavior.

`pose_engine_smoke` builds successfully but remains intentionally outside CTest
because it requires an NVIDIA GPU/runtime. Camera extraction itself has no GPU
inference dependency.

## Acceptance disposition

The complete AC-01–AC-17 evidence mapping is in
`docs/CAMERA_FRAME_TASK_ACCEPTANCE.md`. All code-level and deterministic gates
are passed. This machine has FFmpeg but no `redis-server`/`redis-cli` and no
configured RTSP test endpoint/credentials, so the following production
observations are not misreported as executed:

- physical RTSP server client count;
- Redis wire-level restart/recovery;
- TCP/UDP/authentication behavior against a live camera;
- the exact 60-second 1000 ms cadence sample;
- production CPU/private-bytes/GPU/disk measurements.

The operations document supplies a numbered live acceptance procedure and
resource table for those deployment-owned gates. These do not require further
code changes unless the target environment exposes a defect.

## Final scope check

- Same-profile People Flow and extraction share one decode in the same process.
- There is one existing `four_stage_worker`, not a new worker executable.
- Camera extraction performs CPU resize/JPEG/output only and no GPU inference.
- CRUD, start/stop/status/history/latest, Hub diagnostics, persistence,
  retention, authentication, recovery, and observability are implemented.
- Camera Profile CRUD, scheduling, Qt Camera UI, and multi-worker ownership
  remain out of scope exactly as frozen in M0.

M0 through M7 implementation is complete. Production enablement remains
feature-flagged off by default until the live acceptance procedure is signed.

