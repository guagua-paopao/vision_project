# Unified Camera R8 — release and observation

> Status: complete; release gate passed
> Date: 2026-07-27
> Previous gate: R7 integration and hardware acceptance
> Maintenance state: unified runtime is the default; legacy rollback retained

## Release decision

R8 makes `runtime.unified_camera_pipeline: true` the delivery default in:

- `config/server.yaml`;
- `config/worker.yaml`;
- the compiled `AppConfig::RuntimeSection` defaults.

Server and Worker must still use the same value. The People Flow compatibility
controller, legacy People Flow Worker implementation,
`legacy_people_flow_fallback`, and the `false` switch value remain available
for at least this release observation cycle. No endpoint, response field,
database object, or original Camera administration capability was removed.

## Repeatable release gate

Run:

```powershell
$env:YOLO11_CAMERA_ENTRY_URL = "<credentialed-rtsp-uri>"
powershell -ExecutionPolicy Bypass `
  -File .\scripts\verify_unified_camera_release.ps1 `
  -ObservationMinutes 5 `
  -ExpansionCameraCount 3 `
  -SkipPostman
```

The RTSP value is inherited by child processes and is never written to
configuration, reports, logs, or committed files.

The release verifier checks the default configuration, retained rollback
implementation, compact contracts, full external-service tests, real hardware
chain, staged rollout, observation metrics, and rollback drill. It writes a
redacted `reports/r8/<timestamp>/summary.json`.

## Accepted target-hardware run

The accepted run used a local credentialed H.264 Camera:

- resolution: 1920 × 1080;
- source rate: 25 FPS;
- 15-second capture sequence advance: 459 frames;
- 15-second TensorRT completions: 120;
- initial reconnect count: 0.

The rebuilt compact suite passed all configured targets and the disposable
PostgreSQL/Redis matrix passed 22/22 with no skipped test. Qt API workflow,
Camera Admin Web parity/security, callback delivery, forced Worker recovery,
forced RTSP reconnect, dead-letter transition/replay, and rollback all passed.

## Staged rollout result

The release gate first ran one compatibility Camera Pipeline, then added three
stress Cameras that used the same Camera Profile:

| Check | Result |
| --- | ---: |
| Total Camera Pipelines | 4 |
| Shared FrameHubs | 1 |
| `camera_pipeline` subscribers | 4 |
| Legacy subscribers | 0 |
| Aggregate inference | 35 FPS |
| Failed jobs during stress window | 0 |

The inference pool remained fixed at two ready workers; it did not grow with
the Camera count.

## Five-minute observation

The project owner explicitly approved the shortened five-minute observation
instead of the original 60-minute soak.

| Metric | Result |
| --- | ---: |
| Samples / valid samples | 57 / 57 |
| Sample error rate | 0 |
| Readiness failures | 0 |
| Hub invariant failures | 0 |
| Maximum dropped frames | 0 |
| Maximum failed inference jobs | 0 |
| Alert total, first → last | 46 → 168 |
| Callback delivered, cumulative | 30 |
| Callback retries, cumulative | 30 |
| Deliberate dead-letter counter | 1 |
| Data consistency | passed |

The cumulative dead-letter value comes from the deliberate failure scenario.
The acceptance target reached dead-letter after two attempts and was then
successfully replayed to delivered.

Resource maxima remained below the R7-approved thresholds:

| Metric | Maximum | Threshold |
| --- | ---: | ---: |
| Process CPU | 8.34% | 15% |
| Process working set | 464.06 MiB | 768 MiB |
| GPU memory | 1831 MiB | 3072 MiB |
| GPU utilization | 44% | informational |
| GPU temperature | 56°C | 80°C |
| Disk used | 72.23% | 90% |

## Rollback

The automated drill:

1. stopped the unified Worker;
2. waited 32 seconds for the process lease to expire;
3. started and verified `legacy_split`;
4. stopped the legacy Worker and waited for its lease;
5. restored and verified `unified_camera_pipeline`;
6. confirmed one Vision Worker and healthy coordination.

For an operational rollback, pause new starts, stop active unified Runs, wait
for Worker and Run leases to release, set the flag to `false` in both Server
and Worker configuration, restart both processes, and verify:

- `/ready` reports `expected_runtime_mode=legacy_split`;
- Qt and `/people-flow/*` compatibility behavior;
- Camera Admin functions;
- alert and callback delivery;
- no duplicate reader, event, or callback.

Keep all additive schema objects. Do not perform a destructive database
rollback.

## Final acceptance

- unified runtime is the tested default;
- single-Camera and expanded-Camera operation are consistent;
- Qt, Web, database, Redis, alerts, and external callback remain compatible;
- no P0/P1 defect remains;
- RTSP credentials are not persisted;
- all disposable containers and test processes were removed;
- the legacy implementation and switch remain available for the observation
  cycle.

The People Flow and Camera business unification program can now enter
long-term maintenance.
