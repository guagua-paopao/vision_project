# M2 — People Flow migration to the shared Hub

> Status: complete  
> Date: 2026-07-20  
> Next gate: M3 VisionWorkerHost multi-role orchestration

## Objective

Move the existing People Flow session from a private `RtspCaptureReader` to a
`people_flow` FrameHub subscription without changing inference, tracking,
counting, security analysis, snapshot, lease, event, or persistence behavior.
Split the long-running session out of the command consumer before introducing
the Camera Task role.

## Delivered behavior

- `PeopleFlowSessionRunner` owns one long-running business session and its RAII
  `FrameSubscription`.
- The People Flow command loop starts at most one session thread and remains
  separate from the long-running session. The existing independent heartbeat
  thread remains responsive.
- The Runner validates profile metadata and `source_ref`, but it neither
  resolves an RTSP URI nor constructs/stops `RtspCaptureReader`.
- Inference receives the shared `const cv::Mat&` directly. Both People Flow and
  security renderers already clone their input before drawing.
- Reconnect count changes preserve the previous tracker, counter, renderer,
  security pipeline, and warmup reset behavior.
- Session stop/failure releases only its Subscription. The Hub closes only
  after the last subscriber and idle grace.
- `capture.dropped_frames` now means this People Flow subscription's skipped
  sequence count, rather than a reader-global overwrite count.
- People Flow status adds `capture.shared_hub`, `hub_instance_id`, and
  `hub_subscribers` in Redis and HTTP JSON.
- `capture.allow_backend_fallback` now defaults to `false` in code and both
  shipped YAML configurations.
- The executable remains `four_stage_worker.exe`; no camera worker target or
  process was added.

## Changed files

New Runner boundary and regression:

- `include/server/people_flow_session_runner.h`
- `src/server/people_flow_session_runner.cpp`
- `tests/people_flow_hub_regression_test.cpp`

People Flow migration and status compatibility:

- `include/server/people_flow_inference_worker.h`
- `src/server/people_flow_inference_worker.cpp`
- `include/server/redis_task_queue.h`
- `src/server/redis_task_queue.cpp`
- `src/server/people_flow_http_server.cpp`
- `include/server/rtsp_camera_frame_source.h`
- `src/server/rtsp_camera_frame_source.cpp`

Configuration/build:

- `include/server/app_config.h`
- `config/server.yaml`
- `config/worker.yaml`
- `CMakeLists.txt`
- `scripts/build_backend.ps1`

## Verification evidence

Clean command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1 -CleanFirst
```

Result: build succeeded and CTest passed 5/5:

- `people_flow_core_test`
- `security_analytics_test`
- `repository_test`
- `shared_camera_frame_hub_test`
- `people_flow_hub_regression_test`

The new regression proves:

- a People Flow and Camera Task subscription share the exact same immutable
  envelope and one source start;
- deterministic read-only inference input equals the legacy cloned input;
- rendering a clone does not modify the shared source frame;
- People Flow skipped-frame accounting is subscription-local;
- releasing People Flow does not stop a Hub while Camera Task remains;
- the final release stops the source after idle grace.

Compatibility command:

```powershell
python .\tools\qt_demo_contract_test.py
```

Result: existing Qt demo API workflow and client endpoint wiring passed.

Static migration checks found no private capture construction, URI resolution,
`capture.start/stop`, or `CAP_ANY` configuration in the migrated People Flow
path, and no `camera_frame_worker` target or source in the project.

## Compatibility note

Existing People Flow fields and routes remain. The only intentional semantic
change is `capture.dropped_frames`: it is now the number of source sequences
skipped by this subscription while sampling at `target_infer_fps`. This makes
the value consumer-specific and prevents one subscriber from affecting
another subscriber's statistics.

## Residual risks and deferred work

- No real RTSP endpoint or TensorRT camera session was configured for this
  deterministic environment. Hardware/live-stream smoke testing remains M7.
- The People Flow command loop supports one active Runner, as required for the
  first release. M3 will place this role and the Camera Task role under one
  `VisionWorkerHost` with one externally owned Hub Registry.
- The server's legacy People Flow start path still resolves the URI to produce
  a masked display value. Removal of server-side secret resolution is part of
  the M6 security/API migration.

## M3 entry criteria

- Private People Flow reader removed: passed.
- Long-running session split behind `PeopleFlowSessionRunner`: passed.
- Shared immutable frame and clone-before-render behavior: passed.
- Reconnect/warmup, lease, event, persistence, and snapshot code retained:
  passed by code-path preservation and existing deterministic regressions.
- Shared lifecycle regression: passed.
- Full clean build, 5/5 CTest, and Qt contract: passed.

M3 is authorized to begin.
