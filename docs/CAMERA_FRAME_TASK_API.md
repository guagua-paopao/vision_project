# Camera Instance and Frame Extraction API

> M11 contract: there is no public Camera Task definition CRUD. A camera
> instance is addressed by stable `camera_id`; internal Runs are execution
> audit records only.

Base URL: `http://127.0.0.1:8087/api/v1`

All routes below require:

```http
Authorization: Bearer <YOLO11_CAMERA_TASK_ADMIN_TOKEN>
```

RTSP URI, database DSN, usernames, passwords, and environment-variable values
are forbidden in request bodies and responses.

The compatibility People Flow `start` and `stop` routes use this same Bearer
token. The Qt client reads it from `YOLO11_CAMERA_TASK_ADMIN_TOKEN` and sends
it only in the `Authorization` header.

## Camera CRUD

### Create and start

`POST /cameras`

```json
{
  "camera_id": "entrance_01",
  "name": "Main entrance",
  "camera_profile": "entry_camera_01",
  "desired_state": "running",
  "frame_interval_ms": 1000,
  "output_mode": "both",
  "jpeg_quality": 90,
  "max_width": 1280,
  "max_height": 720,
  "retention_days": 7,
  "max_saved_frames": 10000,
  "analysis": {
    "enabled": true,
    "target_infer_fps": 5.0,
    "algorithm_profile": "security_default",
    "algorithms": ["people_flow", "electronic_fence"]
  },
  "callback_profile": "backend_primary"
}
```

`desired_state=running` is equivalent to `enabled=true`; clients should use
`desired_state` for new integrations. A running camera is persisted and its extraction START command is submitted
in the same request. The response is normally `202` with `camera_id`, internal
`run_id`, status links, and an ETag. Duplicate camera id returns `409`.

`analysis.enabled=true` requires a safe `algorithm_profile` and at least one
algorithm identifier. RTSP URLs and callback URLs are never accepted here:
`camera_profile` and `callback_profile` resolve deployment-side configuration.

Create accepts `Idempotency-Key` (printable ASCII, at most 160 characters).
Reusing the same key with the same normalized JSON replays the stored response
and sets `X-Idempotent-Replay: true`; reusing it with another payload returns
`409 IDEMPOTENCY_CONFLICT`.

### List and detail

- `GET /cameras?limit=50&offset=0&enabled=true`
- `GET /cameras/{camera_id}`

The response merges PostgreSQL desired state/Run history with Redis hot state
and shared-Hub diagnostics.

The status response also includes a `pipeline` object with
`thread_running`, `thread_started_at_ms`, `thread_age_ms`, `sample_fps`,
`sampled_frames`, `last_source_sequence`, `skipped_frames`, and
`inference_submit_drops`. There is exactly one Pipeline thread per running
`camera_id`; multiple camera ids on one Profile still share one FrameHub.
`sample_fps` and `sampled_frames` describe the JPEG extraction cadence only;
analysis sampling is independent and is accounted for by the inference layer.

### Update and replace thread

`PATCH /cameras/{camera_id}` requires the latest ETag:

```http
If-Match: "3"
Content-Type: application/json

{"frame_interval_ms":500,"jpeg_quality":85}
```

If an extraction generation is active, it is marked stopping. The Worker
stops and joins its old thread, then starts the new generation under the same
camera id. A successful enabled update normally returns `202`.

### Delete and close thread

`DELETE /cameras/{camera_id}` requires `If-Match`. It requests stop before the
camera is soft-deleted. An active deletion returns `202`; idle deletion returns
`204`. Run and frame audit metadata are retained.

## Explicit lifecycle

- `POST /cameras/{camera_id}/start`
- `POST /cameras/{camera_id}/stop`
- `GET /cameras/{camera_id}/status`
- `GET /cameras/{camera_id}/runs?limit=20&offset=0`
- `GET /cameras/{camera_id}/alerts?event_type=PEOPLE_FLOW_IN&minimum_severity=1&limit=20&offset=0`
- `GET /cameras/{camera_id}/latest-frame`

START/STOP accept `Idempotency-Key` and are also resource-idempotent when the
header is omitted. The status response exposes `desired_state`,
`observed_state`, extraction state, and analysis configuration. Alert queries
return normalized event payload/evidence plus callback delivery state.
Latest frame returns `image/jpeg`; archive-only
cameras return `409`.

## Shared Hub diagnostics

- `GET /camera-hubs`
- `GET /camera-hubs/{camera_profile}`

These read-only routes expose open/reconnect counts, subscribers by role, FPS,
latest sequence/age, resolution, and sanitized error state. They cannot start
or stop a Hub.

## Read-only Profiles

- `GET /camera-profiles`
- `GET /camera-profiles/{profile_id}`

Profiles are deployment configuration. POST/PATCH/DELETE routes are not
registered. Adding a new physical source requires editing `config/cameras.yaml`,
setting its URI environment variable, and restarting Server/Worker.

## Operations

- `GET /operations/metrics`
- `GET /operations/metrics/prometheus`

Durable metrics come from PostgreSQL. Redis provides hot Run/Hub metrics.

## Removed routes

All `/api/v1/camera-tasks...` routes are removed. Clients must migrate to
`/api/v1/cameras...` and use `camera_id` instead of `task_id`.
