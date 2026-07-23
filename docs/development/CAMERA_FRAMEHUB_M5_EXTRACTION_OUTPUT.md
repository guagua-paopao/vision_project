# M5 — extraction sessions, JPEG publication, and retention

> Status: complete  
> Date: 2026-07-20  
> Next gate: M6 HTTP API, authentication, and Hub diagnostics

## Objective

Implement the real Camera Task data plane inside the existing
`four_stage_worker` process: subscribe to the same FrameHub as People Flow,
sample with an independent monotonic cadence, publish JPEG artifacts without
back-pressuring capture/inference, and safely enforce retention.

## Delivered runtime

- `CameraFrameExtractionSession` obtains a `camera_task` subscription from the
  Host-owned Registry. It cannot start, stop, or reopen the Hub reader.
- First valid frame is sampled immediately. Later deadlines use
  `steady_clock`; missed deadlines advance into the future and never trigger a
  catch-up burst.
- Each Run owns its subscription cursor, skipped-frame metrics, interval,
  lease, stop flag, state transitions, progress, and terminal status.
- Hub `opening/reconnecting/running/failed` is projected into Run state without
  creating another decoder.
- Production construction now initializes Repository, stale recovery, Redis
  command source/control, writer pool, retention, and sessions through the
  existing `VisionWorkerHost` manager factory.
- START acquires/reuses a compare-safe Run lease and durably writes `starting`
  before the stream command is acknowledged. A session refreshes the lease;
  loss beyond TTL fails only that Run.

## Bounded writer and output contract

- WorkerHost-level fixed writer pool with bounded global and per-Run in-flight
  counts. Full queues reject the sample; the session increments `dropped_frames`
  and the Hub remains unblocked.
- Jobs retain the immutable shared frame. Resize preserves aspect ratio and
  never upscales.
- One `cv::imencode(.jpg)` call is made per accepted job. `both` mode reuses the
  same bytes for archive and latest.
- Output layout matches the design:

```text
{output_root}/{task_id}/latest.jpg
{output_root}/{task_id}/archive/{run_id}/YYYY/MM/DD/
  YYYYMMDDTHHMMSS.mmmZ_{source_sequence}.jpg
```

- Temporary files are in the destination directory. Windows publication uses
  `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`; metadata is inserted only
  after archive publication succeeds.
- Metadata failures create a nonsecret orphan ledger entry and fail only the
  affected Run.
- Task/Run path components use an ASCII safe-identifier policy. Existing
  symlink/junction/reparse components are rejected.

## Retention safety

- Repository retention candidates use both limits: per-task age and newest
  `max_saved_frames` rank.
- Sweeper works in configured batches and deletes individual archive files;
  it never recursively deletes a computed directory.
- Before deletion it verifies the stored path is relative, begins with the
  exact `{task_id}/archive/{run_id}` components, remains canonically under the
  task archive root, and crosses no reparse point.
- Missing archive files allow stale metadata cleanup. Removal errors preserve
  metadata for a later retry.
- `latest.jpg`, Task definitions, and Run audit records are never retention
  targets.

## Deterministic integration evidence

`camera_frame_extraction_test` uses one deterministic 25 FPS source and runs:

- one simulated People Flow subscriber;
- a 100 ms Camera Run in `both` mode;
- a 250 ms Camera Run in `archive` mode.

The test verifies one Hub, one source factory call, `open_count=1`, three
concurrent subscriptions, independent sampling ranges, resize to 160×120,
exactly one encode per accepted sample, valid complete `latest.jpg`, one
archive metadata row per saved sample, and max-count retention down to the
newest three frames. Stopping either Run leaves the source active for the
remaining subscribers; releasing the final People Flow subscriber closes the
source after idle grace.

A task-local failure is injected by placing a regular file where that task's
managed directory must be created. The Run becomes
`failed/OUTPUT_PATH_UNSAFE`, while the shared source remains active and the
People Flow cursor continues receiving frames. This directly exercises the
required output-failure isolation boundary.

## Changed files

Core runtime:

- `include/business/camera_task_runtime_control.h`
- `include/business/camera_frame_extraction_session.h`
- `src/business/camera_frame_extraction_session.cpp`
- `include/business/frame_artifact_writer.h`
- `src/business/frame_artifact_writer.cpp`
- `include/business/camera_frame_retention.h`
- `src/business/camera_frame_retention.cpp`

Composition and storage:

- `include/server/camera_task_runtime.h`
- `src/server/camera_task_runtime.cpp`
- `src/server/main_people_flow_worker.cpp`
- `include/business/camera_task_repository.h`
- `src/business/camera_task_repository.cpp`
- `src/server/camera_task_queue.cpp`

Build and verification:

- `tests/camera_frame_extraction_test.cpp`
- `CMakeLists.txt`
- `scripts/build_backend.ps1`

## Verification result

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1
python .\tools\qt_demo_contract_test.py
```

- Server, existing Worker, Camera runtime, and all test targets built.
- CTest passed 8/8; the M5 integration test took approximately 2.34 seconds.
- Existing Qt demo contract passed.
- Static target scan found no `camera_frame_worker` executable or source; only
  the existing `four_stage_worker` hosts Camera Tasks.

One tooling note: plain `cmake`/`ctest` is not on this shell's PATH. The
repository build script resolves and uses the pinned CMake/CTest binaries, so
all authoritative verification uses that script.

## M6 entry criteria

- Same-profile multi-interval tasks share one Hub/source open: passed.
- Independent cursor and monotonic no-burst sampling: passed.
- Resize and one encode for multiple output targets: passed.
- Complete atomic latest plus archive metadata: passed.
- Bounded writer and task-local failure isolation: passed.
- Retention count limit and latest preservation: passed.
- Production WorkerHost composition without another process: passed.
- Full deterministic suite and Qt contract: passed.

M6 is authorized to begin.
