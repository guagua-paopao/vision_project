# Algorithm service implementation journal

This directory is the durable implementation journal for the integrated HTTP
algorithm service defined by `docs/整体算法服务_项目计划与范围.docx`. A phase is not
complete until its code, automated verification, residual risks, and next gate
are recorded here.

## Traceability rules

- Phase records are append-only after a phase is accepted. Corrections are
  added under an Amendments heading with a date and reason.
- Every phase records changed files, exact verification commands, observed
  results, known skipped tests, rollback instructions, and the next-phase gate.
- Machine-readable contracts live under `api/schemas`; implementation and
  tests must remain compatible with their major version.
- Git history is restored. Every accepted phase is published on an
  `agent/...` branch and linked through a draft pull request in addition to
  file paths, test output, schema versions, and dated phase records.

## Phases

| Phase | Scope | State | Record |
|---|---|---|---|
| P0 | Contract and architecture freeze | Complete | [P0 contract freeze](ALGORITHM_SERVICE_P0_CONTRACT.md) |
| P1 | Data model and HTTP control plane | Complete | [P1 control plane](ALGORITHM_SERVICE_P1_CONTROL_PLANE.md) |
| P2 | Per-camera `CameraPipeline` lifecycle | Complete | [P2 CameraPipeline](ALGORITHM_SERVICE_P2_CAMERA_PIPELINE.md) |
| P3 | Fixed inference worker pool | Complete | [P3 inference pool](ALGORITHM_SERVICE_P3_INFERENCE_POOL.md) |
| P4 | Durable alert callback | Complete | [P4 alert callback](ALGORITHM_SERVICE_P4_ALERT_CALLBACK.md) |
| P5 | Integration, observability, and Postman | Complete | [P5 integration](ALGORITHM_SERVICE_P5_INTEGRATION.md) |
| P6 | Hardware acceptance and release | Complete | [P6 hardware acceptance](ALGORITHM_SERVICE_P6_RELEASE.md) |

## Current baseline

- Baseline date: 2026-07-24 (Asia/Shanghai).
- Existing public camera aggregate: `/api/v1/cameras`.
- Existing deployment composition: `four_stage_server` control plane plus one
  `four_stage_worker`/`VisionWorkerHost` compute process.
- Invariant retained through P1-P6: `worker.worker_num=1` for a single shared
  RTSP ownership domain. Inference parallelism is a separate configuration.
- P2 exit CTest on disposable PostgreSQL 17: 12 tests, 12 passed, 0 failed,
  0 skipped, 8.48 seconds.
- P3 exit CTest on disposable PostgreSQL 17: 14 tests, 14 passed, 0 failed,
  0 skipped, 8.33 seconds.
- P4 exit CTest on disposable PostgreSQL 17: 16 tests, 16 passed, 0 failed,
  0 skipped, 9.38 seconds.
- P5 exit CTest on disposable PostgreSQL 17 and Redis 7: 18 tests, 18 passed,
  0 failed, 0 skipped, 10.55 seconds.
- P6 exit CTest on disposable PostgreSQL 17 and Redis 7: 19 tests, 19 passed,
  0 failed, 0 skipped; the release guard repeated the same 19/19 result.
- P3 runtime invariant: one Pipeline thread per active camera, plus a
  startup-fixed pool of `analysis.inference_workers` model runners.
- P4 runtime invariant: alert persistence and HTTP delivery are decoupled by a
  PostgreSQL outbox with leased, fenced claims and bounded retry/dead-letter
  behavior.
- P5 runtime invariant: process-local Pipeline/inference/Processor/callback
  snapshots cross the process boundary only as expiring Redis heartbeat data;
  durable operations totals and replay state remain in PostgreSQL.
- P6 runtime invariant: Run leases include both Run ID and a unique Worker
  generation token; replacement Workers audit the old Run and recover a new
  generation only after lease ownership is fenced. Windows FFmpeg children are
  killed when their Worker Job Object closes.
- Historical P0-P2 integration: commit `1775b32`, merged pull request #1.
- P2 acceptance patch: branch `agent/p2-camera-pipeline`, commit `dbd4ab2`,
  draft pull request #3.
- P3 implementation: branch `agent/p3-fixed-inference-pool`, commit
  `ab780ab`, draft pull request #2 stacked on the P2 branch.
- P4 implementation: branch `agent/p4-durable-callback`, commit `f773296`,
  draft pull request #4 stacked on the P3 branch.
- P5 implementation: branch `agent/p5-integration-observability`, commit
  `b749a26`, draft pull request
  [#5](https://github.com/guagua-paopao/vision_project/pull/5) stacked on the
  P4 branch.
- P6 implementation: branch `agent/p6-hardware-acceptance`, commit
  `863d88a`, draft pull request
  [#6](https://github.com/guagua-paopao/vision_project/pull/6) stacked on the
  P5 branch.

## Completion definition

The program completed P6 after recording target-hardware RTSP/GPU evidence,
fault recovery, Postman, durable callback replay, and a 60-minute soak. A
future multi-physical-camera release requires a new acceptance record rather
than reusing the single-profile evidence.
