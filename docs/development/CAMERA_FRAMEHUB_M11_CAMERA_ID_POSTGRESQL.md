# M11 Camera ID lifecycle and PostgreSQL execution record

## Decision record

- Public aggregate renamed from Camera Task to Camera Instance.
- Stable `camera_id` controls the extraction object/thread.
- Run remains internal audit state.
- Worker stays in `four_stage_worker`; no `camera_frame_worker` is introduced.
- One FrameHub decoder per Profile is unchanged.
- Production SQLite dependencies are replaced by libpq/PostgreSQL.
- Profile mutation is removed from the runtime UI/API; Profile selection stays
  read-only.

## Implementation checkpoints

1. Added parameterized libpq adapter and environment-only DSN resolution.
2. Ported Camera and People Flow repositories to PostgreSQL SQL types and
   conflict semantics.
3. Keyed active extraction sessions by camera id and implemented join-before-
   replace.
4. Registered `/api/v1/cameras` and removed public Camera Task CRUD routes.
5. Replaced the Web task console with camera/thread lifecycle controls.
6. Added PostgreSQL schema, disposable integration-test gate, and legacy
   SQLite import utility.

## Verification gates

- C++ build with `PostgreSQL::PostgreSQL` and no production sqlite link.
- manager test proves same-ID replacement joins the old thread.
- PostgreSQL repository tests run only with a disposable
  `YOLO11_TEST_POSTGRES_DSN`; return 77 otherwise.
- HTTP contract verifies create auto-start, PATCH replacement, DELETE stop,
  stable camera id, and absence of Task CRUD routes.
- Qt People Flow contract remains unchanged.
- architecture guard rejects `/camera-tasks` public routes and SQLite links.

## Rollback

Keep the pre-migration SQLite files read-only until PostgreSQL row counts and
application acceptance pass. Rollback requires stopping Server/Worker and
running the previous binary/config; the new binary never writes SQLite.
