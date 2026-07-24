# Shared Camera FrameHub development journal

This directory is the durable implementation journal for the approved shared
RTSP FrameHub and camera-frame task project. Each milestone record contains its
inputs, decisions, changed files, verification evidence, residual risks, and
the gate for entering the next milestone.

Milestones must be completed in order:

1. [M0 baseline and design freeze](CAMERA_FRAMEHUB_M0_BASELINE.md)
2. [M1 shared frame model, hub, subscription, and FFmpeg open coordination](CAMERA_FRAMEHUB_M1_SHARED_HUB.md)
3. [M2 People Flow migration to the shared hub](CAMERA_FRAMEHUB_M2_PEOPLE_FLOW_MIGRATION.md)
4. [M3 multi-role VisionWorkerHost](CAMERA_FRAMEHUB_M3_VISION_WORKER_HOST.md)
5. [M4 Camera Task SQLite repository and Redis command layer](CAMERA_FRAMEHUB_M4_CAMERA_TASK_STATE.md)
6. [M5 extraction session, JPEG publishing, and retention](CAMERA_FRAMEHUB_M5_EXTRACTION_OUTPUT.md)
7. [M6 HTTP API, authentication, and hub diagnostics](CAMERA_FRAMEHUB_M6_HTTP_SECURITY.md)
8. [M7 integration, operations, and final acceptance](CAMERA_FRAMEHUB_M7_DELIVERY.md)
9. [M8 administration control plane](CAMERA_FRAMEHUB_M8_ADMIN_CONTROL.md)
10. [M9 resilience and observability](CAMERA_FRAMEHUB_M9_RESILIENCE_OBSERVABILITY.md)
11. [M10 storage lifecycle and recovery](CAMERA_FRAMEHUB_M10_STORAGE_LIFECYCLE.md)
12. [M8-M10 execution ledger](CAMERA_FRAMEHUB_M8_M10_EXECUTION.md)
13. [M11 camera-id lifecycle and PostgreSQL](CAMERA_FRAMEHUB_M11_CAMERA_ID_POSTGRESQL.md)

The authoritative approved scope and design remain:

- [Project plan](../CAMERA_FFMPEG_FRAME_TASK_PROJECT_PLAN.md)
- [Detailed design](../CAMERA_FFMPEG_FRAME_TASK_DESIGN.md)

The next integrated algorithm-service program is tracked separately so the
M0-M11 FrameHub history stays immutable:

- [Algorithm service implementation journal](ALGORITHM_SERVICE_IMPLEMENTATION_INDEX.md)
- [P0 integrated contract freeze](ALGORITHM_SERVICE_P0_CONTRACT.md)
- [P1 data model and HTTP control plane](ALGORITHM_SERVICE_P1_CONTROL_PLANE.md)
- [P2 per-camera CameraPipeline](ALGORITHM_SERVICE_P2_CAMERA_PIPELINE.md)
- [P3 fixed inference worker pool](ALGORITHM_SERVICE_P3_INFERENCE_POOL.md)
- [P4 durable alert callback](ALGORITHM_SERVICE_P4_ALERT_CALLBACK.md)
- [P5 integration, observability, and Postman](ALGORITHM_SERVICE_P5_INTEGRATION.md)
- [P6 target-hardware acceptance and release](ALGORITHM_SERVICE_P6_RELEASE.md)
