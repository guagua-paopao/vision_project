# P0 integrated algorithm service contract freeze

## Outcome

P0 freezes the v1 external contract and the compatibility boundary for the
integrated algorithm service. No production runtime behavior is changed in
this phase.

## Inputs reviewed

- `docs/整体算法服务_项目计划与范围.docx`
- existing Camera API, operations, acceptance, architecture, and M0-M11 notes
- `CameraTaskHttpController`, `CameraTaskRepository`, `CameraTaskManager`,
  `SharedCameraFrameHub`, `VisionWorkerHost`, and People Flow/security code
- PostgreSQL `camera_tasks`, `camera_task_runs`, `camera_frames`, and `pf_*`
  schema
- server/worker YAML configuration and the 12-test CTest baseline

## Frozen decisions

1. The external aggregate remains `/api/v1/cameras`; v1 does not add a second
   competing Task CRUD surface.
2. `camera_id` is the stable public task identifier. `run_id` is a generated,
   immutable execution record beneath that Camera.
3. A successful create/start response means accepted, not inference-ready.
   The backend polls `status` or consumes asynchronous callbacks.
4. `enabled` remains a compatibility alias. Internally and in responses the
   service converges on `desired_state` plus `observed_state`.
5. Each running Camera owns one `CameraPipeline` thread. A shared Profile owns
   at most one FrameHub/FFmpeg reader.
6. `worker.worker_num` stays fixed at one. `analysis.inference_workers=W` is a
   separate, startup-fixed model/thread count introduced in P3.
7. Task JSON references `camera_profile`, `algorithm_profile`, and
   `callback_profile`. It never accepts raw RTSP/callback URLs, credentials,
   DSNs, tokens, or secrets.
8. Control mutations require Bearer authentication. PATCH and DELETE require
   ETag/If-Match. POST create/start/stop accept `Idempotency-Key` in P1.
9. Alerts are durable facts. `security_alert_events` and `callback_outbox` are
   written in one transaction before HTTP delivery is attempted.
10. Callback delivery is at least once. The receiver deduplicates by
    `event_id`; any 2xx response marks delivery successful.

## Machine-readable contracts

- `api/schemas/algorithm_task.v1.schema.json`
- `api/schemas/alert_event.v1.schema.json`

These schemas use JSON Schema 2020-12, reject unknown task fields, and exclude
raw endpoints and secrets by construction.

## HTTP surface frozen for v1

| Operation | Method and path | Success |
|---|---|---|
| Create | `POST /api/v1/cameras` | 201 or 202 |
| List | `GET /api/v1/cameras` | 200 |
| Detail | `GET /api/v1/cameras/{camera_id}` | 200 |
| Modify | `PATCH /api/v1/cameras/{camera_id}` | 200 or 202 |
| Delete | `DELETE /api/v1/cameras/{camera_id}` | 202 or 204 |
| Start | `POST /api/v1/cameras/{camera_id}/start` | 200 or 202 |
| Stop | `POST /api/v1/cameras/{camera_id}/stop` | 200 or 202 |
| Status | `GET /api/v1/cameras/{camera_id}/status` | 200 |
| Runs | `GET /api/v1/cameras/{camera_id}/runs` | 200 |
| Latest frame | `GET /api/v1/cameras/{camera_id}/latest-frame` | 200 or 409 |
| Alerts | `GET /api/v1/cameras/{camera_id}/alerts` | 200 |
| Redeliver | `POST /api/v1/alerts/{event_id}/redeliver` | 202 |

Compatibility People Flow routes remain available until P5, but new backend
integrations use only the Camera surface.

## Lifecycle

`desired_state` is `running` or `stopped`. `observed_state` uses:

`created/stopped -> queued -> starting -> running <-> reconnecting -> stopping -> stopped`

Any nonterminal state may enter `failed`. Soft deletion produces `deleted`
after the active generation has been asked to stop. At most one queued,
starting, running, or reconnecting Run may exist for a Camera.

## Callback contract

- Method: `POST` to the endpoint resolved from a server allow-listed
  `callback_profile`.
- Required headers: `Idempotency-Key`, `X-Event-Id`, `X-Timestamp`,
  `X-Signature-Version`, and `X-Signature`.
- Signature input: decimal timestamp, one LF byte, then the exact request body
  bytes. Algorithm: HMAC-SHA256.
- Retryable: transport failures, timeout, 408, 429, and 5xx.
- Terminal success: any 2xx.
- Terminal failure: other 4xx enters dead letter.

## Capacity and security gates

- The service rejects or explicitly degrades tasks that exceed configured
  Camera or inference capacity; it never grows model instances dynamically.
- Per-Camera pending inference capacity is one latest-only job.
- RTSP URI, callback URL, PostgreSQL DSN, Redis password, Bearer token, and
  callback secret may come only from deployment configuration/environment.
- Temporal DEMO output remains labelled `demo_classifier=true` and is not a
  production violence-classification conclusion.

## Changed files

- `api/schemas/algorithm_task.v1.schema.json`
- `api/schemas/alert_event.v1.schema.json`
- `docs/development/ALGORITHM_SERVICE_IMPLEMENTATION_INDEX.md`
- `docs/development/ALGORITHM_SERVICE_P0_CONTRACT.md`
- `docs/development/README.md`

## Verification evidence

Baseline command:

```powershell
& 'D:\vs2019\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' `
  --test-dir 'out\build\backend-Release' --output-on-failure
```

Observed before P0 changes: 12 tests, 0 failures; 7 passed. Five tests were
skipped because `YOLO11_TEST_POSTGRES_DSN` was not configured:
`repository_test`, `camera_task_repository_test`,
`camera_frame_extraction_test`, `camera_storage_policy_test`, and
`camera_task_http_contract_test`.

Schema files were parsed as JSON and checked for matching `$id`, title,
top-level object type, and `additionalProperties=false` on the task contract.

## Residual risks

- PostgreSQL integration behavior is not exercised without a disposable test
  database.
- There is no usable Git metadata in this workspace, so P0 cannot name a
  commit hash.
- P0 freezes contract intent only; P1 must implement new fields, migrations,
  idempotency, unified authentication, and alert query storage.

## Rollback

P0 adds documentation and schemas only. Rollback is removal of the five files
listed above and restoration of the prior development README entry.

## Gate to P1

P1 may start because identifiers, lifecycle, HTTP routes, security boundary,
task schema, alert schema, and compatibility decisions are frozen.
