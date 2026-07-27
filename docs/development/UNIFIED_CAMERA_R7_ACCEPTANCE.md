# Unified Camera R7 — integration and hardware acceptance

> Status: complete; acceptance passed
> Date: 2026-07-27
> Previous gate: R6 Web feature parity
> Next gate: R8 release and observation

## Objective

Prove that the unified Camera runtime preserves the People Flow and Camera
contracts while running the complete HTTP task, frame capture, inference,
alert, callback, and response chain on target hardware.

## Acceptance scope

R7 adds repeatable PowerShell acceptance tooling for:

- the complete compact build and contract suite;
- all 22 CTest targets with disposable PostgreSQL and Redis;
- RTSP capture and TensorRT/FFmpeg interoperability;
- forced Worker restart and process-lease recovery;
- forced RTSP reader termination and shared-Hub reconnect;
- real alert callback retry, dead-letter transition, and manual replay;
- multi-Camera load through one FrameHub and a fixed inference pool;
- bounded soak telemetry and resource thresholds;
- unified-to-legacy-to-unified rollback;
- syntax validation for every PowerShell script in GitHub Actions.

The unified-mode checks require only `camera_pipeline` subscribers. A legacy
`people_flow` or `camera_task` subscriber is treated as a contract violation.

## User-approved soak duration

The approved design originally specified a 60-minute soak. On 2026-07-27, the
project owner explicitly replaced that duration with a shorter release gate.
The default R7 verifier therefore runs a five-minute soak. A long stability
run remains available by passing `-DurationMinutes 60`.

An earlier 60-minute attempt on 2026-07-25 produced 379 consecutive healthy
samples before the local HTTP test process began timing out. The last healthy
sample remained ready, running, unified, and violation-free. That interrupted
local run is retained only as diagnostic evidence and is not the R7 release
result.

## Target-hardware result

The accepted run used a credentialed local RTSP Camera without persisting its
URI or credentials. The probe reported:

- H.264 video at 1920 × 1080 and 25 FPS;
- 468 source-frame sequence advances during the 15-second capture smoke;
- 121 TensorRT inference completions during the 15-second interoperability
  smoke;
- no initial capture reconnect.

The full PostgreSQL/Redis test matrix passed 22/22 with no skipped target.
Worker crash recovery created a new Run generation, and forced reader
termination recovered through the same shared FrameHub.

The HTTP-to-alert-to-callback path completed successfully. A deliberately
failing callback reached dead-letter after two attempts and was manually
replayed to delivered.

## Stress and soak result

Three stress Cameras plus one existing compatibility Run produced four unified
Camera Pipelines on one Hub:

- `camera_pipeline`: 4 subscribers;
- `camera_task`: 0 subscribers;
- `people_flow`: 0 subscribers;
- inference workers configured/ready: 2/2;
- aggregate inference rate: 38.8 FPS;
- failed inference jobs during measurement: 0.

The five-minute soak collected 57 samples with zero violations. Observed
maxima were:

| Metric | Maximum | Approved threshold |
| --- | ---: | ---: |
| Process CPU | 7.79% | 15% |
| Process working set | 451.84 MiB | 768 MiB |
| GPU memory | 1613 MiB | 3072 MiB |
| GPU utilization | 47% | informational |
| GPU temperature | 56°C | 80°C |
| Disk used | 72.2% | 90% |

## Golden, compatibility, and rollback

The compact suite retained the deterministic People Flow golden-result and
performance-difference checks from earlier phases. Qt and Camera Admin Web
contracts both passed.

The rollback drill started in `unified_camera_pipeline`, changed to
`legacy_split`, waited for the 32-second process lease, verified ready state,
and restored `unified_camera_pipeline`. The restored runtime reported one
Vision Worker and healthy coordination.

No schema rollback or data deletion was used. The legacy implementation and
runtime switch remain available for the R8 observation period.

## Security and evidence handling

- RTSP credentials are process-only and are not written to reports.
- Generated evidence contains `rtsp_uri_persisted: false`.
- Disposable PostgreSQL and Redis containers are removed after the run.
- No P0 or P1 defect remained after acceptance.

## R8 entry gate

R8 may begin only after this R7 branch is pushed and its GitHub checks pass.
R8 will make the unified runtime the configuration default, retain the legacy
switch for at least one observation cycle, exercise a staged single-Camera to
multi-Camera rollout, and record release telemetry and rollback instructions.
