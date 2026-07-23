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
- This workspace currently has no usable Git repository metadata. Until Git is
  restored, file paths, test output, schema versions, and dated phase records
  are the authoritative trace chain.

## Phases

| Phase | Scope | State | Record |
|---|---|---|---|
| P0 | Contract and architecture freeze | Complete | [P0 contract freeze](ALGORITHM_SERVICE_P0_CONTRACT.md) |
| P1 | Data model and HTTP control plane | Complete | [P1 control plane](ALGORITHM_SERVICE_P1_CONTROL_PLANE.md) |
| P2 | Per-camera `CameraPipeline` lifecycle | In progress (DB recheck blocked) | [P2 CameraPipeline](ALGORITHM_SERVICE_P2_CAMERA_PIPELINE.md) |
| P3 | Fixed inference worker pool | Planned | `ALGORITHM_SERVICE_P3_INFERENCE_POOL.md` |
| P4 | Durable alert callback | Planned | `ALGORITHM_SERVICE_P4_ALERT_CALLBACK.md` |
| P5 | Integration, observability, and Postman | Planned | `ALGORITHM_SERVICE_P5_INTEGRATION.md` |
| P6 | Hardware acceptance and release | Planned | `ALGORITHM_SERVICE_P6_RELEASE.md` |

## Current baseline

- Baseline date: 2026-07-23 (Asia/Shanghai).
- Existing public camera aggregate: `/api/v1/cameras`.
- Existing deployment composition: `four_stage_server` control plane plus one
  `four_stage_worker`/`VisionWorkerHost` compute process.
- Invariant retained through P1-P6: `worker.worker_num=1` for a single shared
  RTSP ownership domain. Inference parallelism is a separate configuration.
- Baseline CTest: 12 tests, 0 failures; 7 passed and 5 skipped because a
  disposable PostgreSQL test DSN was not present.

## Completion definition

The program is complete only after P6 records successful target-hardware RTSP
and GPU evidence. CPU-only unit and contract tests cannot close P6.
