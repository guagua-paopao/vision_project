# Camera Frame Task Acceptance Record

> Implementation date: 2026-07-20  
> Architecture: option 3, one `four_stage_worker`, process-local shared FrameHub  
> Deterministic release suite: passed 10/10  
> Live infrastructure gate: not executed in this workspace; see the explicit rows below

## Evidence levels

- **Passed**: executed in the current workspace by an automated deterministic
  test or release guard.
- **Code/config verified**: implementation and configuration were inspected,
  but the behavior requires deployment infrastructure not available here.
- **Live gate required**: must be measured against a real RTSP endpoint and
  Redis before production enablement; it is not reported as passed.

## Acceptance matrix

| ID | Result | Evidence |
|---|---|---|
| AC-01 | Passed in-process; live gate required | `camera_frame_resilience_test` runs 1 People Flow + 3 Camera Task subscriptions with one Hub/source and stable `open_count=1`. A physical RTSP server client count still requires the live procedure. |
| AC-02 | Passed | Resilience and extraction tests stop one Run while the source and other subscribers remain active. |
| AC-03 | Passed | People Flow Hub regression and resilience tests release People Flow while Camera Task subscribers continue receiving frames. |
| AC-04 | Passed | Hub tests verify final release, idle grace, source stop, and Registry eviction. Worker capture stop clears its secret. |
| AC-05 | Passed | Independent cursor/skipped-frame tests prove a slow consumer does not move or block another cursor; Hub stores latest-only. |
| AC-06 | Passed | Existing People Flow core, repository, security analytics, Hub equivalence, and Qt API contract all pass. GPU engine smoke is built but intentionally outside CPU-only CTest. |
| AC-07 | Passed | `camera_task_manager_test` proves command dispatch remains responsive while long sessions and a simulated People Flow role run independently. HTTP contract covers CRUD/start/stop. |
| AC-08 | Passed | Repository and HTTP contracts cover persistence, ETag/If-Match conflict, and active update/delete rejection. |
| AC-09 | Passed | Repository partial unique index, manager duplicate START guard, and HTTP idempotent START all prevent a second Run subscription. |
| AC-10 | Algorithm passed; live duration gate required | Extraction integration verifies independent monotonic 100/250 ms sampling without catch-up bursts. The required 60-second 1000 ms/25 FPS measurement is listed in operations. |
| AC-11 | Passed | Complete decodable atomic latest JPEG, archive metadata, max-count retention, latest preservation, and malicious `../` retention metadata rejection are tested. |
| AC-12 | Passed | Injected task-local output path failure produces `failed/OUTPUT_PATH_UNSAFE` while People Flow continues consuming the same Hub. |
| AC-13 | Passed in-process; live gate required | All four subscribers observe one reconnecting state, then resume through the same source object after a modeled reopen and resolution change. Physical disconnect/client count remains a live check. |
| AC-14 | Code/config verified | `FfmpegOpenCoordinator` always attempts `CAP_FFMPEG`; Hub config requires FFmpeg and forbids fallback. Live OpenCV deployment must confirm the backend reports `FFMPEG`. |
| AC-15 | Passed for automated contract/static scope | HTTP tests reject URI fields and sanitize token/Redis/Hub errors. Config contains environment-variable references only. Release guard checks config and target names. Protected deployment-log review remains operational. |
| AC-16 | Passed | HTTP health contract rejects `worker_num>1`; `VisionWorkerHost::start` fails with `VISION_WORKER_REQUIRES_WORKER_NUM_ONE`. |
| AC-17 | Passed deterministically; live resource gate required | Resilience test runs 4 Hubs/8 subscribers, publishes 2,000 replacements per Hub, observes latest-only state and clean shutdown. Production CPU/RAM/GPU/disk measurements remain a live gate. |

## Fault-injection record

| Fault | Automated result |
|---|---|
| shared disconnect/reconnect | consistent Hub state across subscribers; same source factory/start; reopen metric visible |
| resolution change | all cursors receive the new 640×360 immutable frame and change metrics |
| Redis Run-status outage | API returns durable SQLite status with `runtime_stale=true`; connection text is not disclosed |
| Redis Hub-status outage | read-only diagnostic returns sanitized 503 |
| Redis START publication failure | queued Run is compensated to terminal `failed/QUEUE_SUBMIT_FAILED` |
| SQLite 100 ms write lock | competing update waits, then succeeds within the configured busy timeout |
| invalid SQLite target | repository initialization fails explicitly |
| writer saturation | per-Run depth stays at 1 and excess jobs are rejected/dropped |
| task output path blocked | only that Run fails; shared Hub/People Flow stay active |
| retention traversal metadata | candidate is rejected; outside sentinel and metadata remain |
| stale worker/run | startup reconciliation converts stale nonterminal Run to `failed/WORKER_HEARTBEAT_STALE` once |

## Connection and bounded-resource record

Deterministic test snapshot:

| Scenario | Hubs | Source objects | Source starts | Subscribers | Stored decoded frames |
|---|---:|---:|---:|---:|---:|
| 1 Profile: People Flow + 3 tasks | 1 | 1 | 1 | 4 | 1 latest immutable envelope |
| 4 Profiles: 2 consumers each | 4 | 4 | 4 | 8 | 1 latest immutable envelope per Hub |

The writer queue is separately bounded globally and per Run; the fault test
uses capacity 1 and confirms overflow rejection. These are structural bounds,
not production resource measurements.

Production sign-off must append:

| Metric | Baseline | Peak | Steady | Limit/decision |
|---|---:|---:|---:|---|
| RTSP server concurrent clients per profile | | | | must remain 1 |
| `four_stage_worker` CPU % | | | | deployment-defined |
| process private bytes | | | | no sustained growth |
| GPU utilization/memory | | | | compare People Flow-only baseline; extraction adds no inference |
| capture/infer/save FPS | | | | meet configured cadence |
| writer queue depth/drops | | | | no sustained saturation |
| disk write rate/free space | | | | retention headroom maintained |

## Verification commands and result

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\test_all.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\verify_camera_frame_feature.ps1
```

Current deterministic result:

```text
100% tests passed, 0 tests failed out of 10
Qt/API contract: passed
Static architecture/security guard: passed
```

## External environment note

This workspace has FFmpeg available, but no `redis-server`/`redis-cli` and no
configured RTSP test server/camera credentials. Therefore Redis wire-level
recovery, RTSP server-side client counts, TCP/UDP authentication behavior, the
60-second cadence sample, and production CPU/RAM/GPU/disk numbers are correctly
left as live deployment gates. Deterministic fakes exercise the corresponding
state, isolation, and recovery contracts without claiming external telemetry.

