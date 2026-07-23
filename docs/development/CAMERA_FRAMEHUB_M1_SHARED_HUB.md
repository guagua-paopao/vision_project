# M1 — shared frame model, FrameHub, and FFmpeg coordination

> Status: complete  
> Date: 2026-07-20  
> Next gate: M2 People Flow migration

## Objective

Create the process-local camera sharing foundation without changing the
People Flow execution path yet. A camera profile must own at most one active
decoder in a process, while every consumer keeps an independent read cursor.

## Delivered contracts

- `FrameEnvelope` is immutable after publication and is shared as
  `std::shared_ptr<const FrameEnvelope>`.
- `FrameSubscription` owns an independent sequence cursor and skipped-frame
  counter; a slow subscriber cannot consume or delay another subscriber.
- `SharedCameraFrameHubRegistry` maps one `camera_profile` to one Hub/source,
  enforces `max_active_hubs`, and evicts an unused Hub after `idle_grace_ms`.
- `RtspCameraFrameSource` is the only production adapter that resolves the
  profile environment secret and owns `RtspCaptureReader`.
- `FfmpegOpenCoordinator` serializes the process-wide OpenCV FFmpeg option
  environment change and `VideoCapture::open()` operation.
- When `camera_hub.require_ffmpeg_backend=true`, opening with another backend
  fails with `FFMPEG_BACKEND_REQUIRED`; the failure is terminal for that source.
- No separate `camera_frame_worker` process or GPU model was introduced.

## Changed files

New runtime interfaces and implementations:

- `include/business/camera_frame_types.h`
- `include/server/ffmpeg_open_coordinator.h`
- `src/server/ffmpeg_open_coordinator.cpp`
- `include/server/shared_camera_frame_hub.h`
- `src/server/shared_camera_frame_hub.cpp`
- `include/server/rtsp_camera_frame_source.h`
- `src/server/rtsp_camera_frame_source.cpp`

Refactored capture/configuration/build files:

- `include/business/rtsp_capture_reader.h`
- `src/business/rtsp_capture_reader.cpp`
- `include/server/app_config.h`
- `src/server/app_config.cpp`
- `config/server.yaml`
- `config/worker.yaml`
- `CMakeLists.txt`
- `scripts/build_backend.ps1`

Deterministic verification:

- `tests/shared_camera_frame_hub_test.cpp`

## Test evidence

Full clean build command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1 -CleanFirst
```

Incremental verification command after the final robustness changes:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
```

Final result: build succeeded and CTest passed 4/4:

- `people_flow_core_test`
- `security_analytics_test`
- `repository_test`
- `shared_camera_frame_hub_test`

The Hub test proves concurrent same-profile subscription creates and starts
one source, subscriber cursors are independent, skipped-frame accounting is
per subscriber, capacity is enforced, subscriber types are observable, and
idle release stops and evicts the source.

## Engineering observations

The first incremental link encountered stale People Flow object code after
`CapturedFrame` changed to an alias of `FrameEnvelope`. A clean rebuild removed
the stale ABI object and passed. The final incremental build also passed, so
the repository is stable from a clean and a current build directory.

Registry subscription keeps the registry lock until the Hub has registered
the subscriber. This deliberately prevents the maintenance thread from
evicting the last-idle Hub between lookup and subscription.

## Residual risks and deferred work

- People Flow still owns its legacy private `RtspCaptureReader`; M2 is the only
  milestone authorized to replace it with a Hub subscription.
- `capture.allow_backend_fallback` remains compatible with legacy call sites.
  M2 must make the migrated People Flow path obey the mandatory Hub FFmpeg
  rule; final configuration hardening is checked again in M7.
- Real RTSP reconnect and credential tests require a configured camera and are
  deferred to the optional M7 smoke test.
- The current worker loop is intentionally still blocking. Multi-role
  concurrency belongs to M3 and must not be mixed into M2.

## M2 entry criteria

- Shared immutable frame ownership: passed.
- Independent subscription cursor behavior: passed.
- One source/start for concurrent same-profile subscribers: passed.
- Hub capacity and idle eviction: passed.
- Mandatory FFmpeg failure behavior implemented: passed.
- Full build and all deterministic tests: passed.

M2 is authorized to begin.
