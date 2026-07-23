# M0 — baseline and design freeze

> Status: complete  
> Date: 2026-07-20  
> Next gate: M1 shared frame and Hub implementation

## Objective

Freeze the approved option-3 architecture, prove the original repository can
build and pass its deterministic tests, and capture constraints that later
milestones must preserve.

## Approved baseline

- Keep the existing `four_stage_worker.exe`; do not create a separate camera
  worker process.
- Refactor it into a multi-role WorkerHost.
- Within that process, one `camera_profile` maps to at most one active RTSP
  reader and one FFmpeg decode loop.
- People Flow and Camera Task consumers use independent subscriptions over an
  immutable latest frame.
- First release supports one WorkerHost process (`worker_num=1`).
- OpenCV `CAP_FFMPEG` is mandatory; `CAP_ANY` fallback is not allowed after Hub
  migration.
- Camera Task definitions/history use SQLite; commands, leases, hot status,
  Hub status, and heartbeat use Redis.
- Camera Task APIs never accept or expose RTSP credentials.

## Existing-code constraints recorded

1. `PeopleFlowInferenceWorker::loop()` calls the long-running `processTask()`
   synchronously, so it cannot dispatch a second task while a session is live.
2. `processTask()` creates a private `RtspCaptureReader`; this is the connection
   that M2 must replace with a Hub subscription.
3. `RtspCaptureReader` contains a single `delivered_sequence_`; this cannot be
   shared by independent consumers and must not survive into the Hub contract.
4. OpenCV RTSP transport uses the process-wide
   `OPENCV_FFMPEG_CAPTURE_OPTIONS`; concurrent open/reopen operations require a
   process-wide coordinator.
5. `worker.max_concurrency` is currently descriptive heartbeat data, not a
   scheduler.
6. The model runner already accepts `const cv::Mat&`, allowing read-only use of
   a shared immutable frame.

## Environment baseline

| Item | Observed value |
|---|---|
| OS/toolchain | Windows x64, MSVC 19.51.36248.0 |
| CUDA compiler | NVIDIA CUDA 13.3.73 |
| OpenCV | 4.12.0 |
| CMake/Ninja | Visual Studio bundled tools under `D:\vs2019` |
| vcpkg | `D:\vcpkg`, x64-windows dependencies available |
| TensorRT | `D:\TensorRT-10.16.1.11` |
| Build directory | `out/build/backend-Release` |
| Repository metadata | `.git` exists but is not a valid Git repository |

Because Git metadata is unavailable, milestone traceability is maintained by
these journal files, exact file lists, and repeatable build/test commands.

## Baseline verification

Command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
```

Result:

- configure succeeded;
- all original backend targets compiled;
- `people_flow_core_test` passed;
- `security_analytics_test` passed;
- `repository_test` passed;
- total: 3/3 CTest tests passed.

Contract command:

```powershell
python .\tools\qt_demo_contract_test.py
```

Result: Qt demo API workflow and client endpoint wiring passed.

## M1 entry criteria

- Approved option-3 design is recorded: passed.
- Original backend builds: passed.
- Original deterministic tests pass: passed.
- Original Qt/API contract passes: passed.
- Required compiler/SDK paths exist: passed.
- Shared-frame ownership and cursor invariants are explicit: passed.

M1 is authorized to begin.

## Residual risks carried forward

- No real RTSP credential is configured in this environment, so real-camera
  verification remains an optional M7 smoke test.
- The existing repository lacks valid Git history; destructive rewrites remain
  prohibited and file-level journals are required.
- People Flow GPU runtime smoke testing depends on the installed GPU/engine and
  will be separated from deterministic tests.

