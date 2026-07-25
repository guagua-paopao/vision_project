# Unified Camera R6 — Web feature parity

> Status: complete; local acceptance passed  
> Date: 2026-07-25  
> Previous gate: R5 unified Worker, fencing, and readiness  
> Next gate: R7 integration, performance, and hardware acceptance

## Objective

Extend the existing Camera Admin application with the People Flow and security
views previously available in Qt, while keeping the original Camera
administration functions and using only the unified Camera control plane.

The Web application does not call `/people-flow/*`. Qt remains available and
continues to use the R4 compatibility controller, so both clients read the same
immutable Camera Run and algorithm result model.

## Camera API additions

R6 adds:

```text
GET /api/v1/cameras/{camera_id}/analysis-snapshot
```

The endpoint:

- requires the existing Camera Admin Bearer token;
- resolves the active Run, or the latest Run when no Run is active;
- prefers the current in-memory annotated snapshot and falls back to the
  Run's persisted annotated snapshot;
- validates that the resolved file remains under the configured output root;
- rejects traversal, symlink, and reparse-point escape attempts;
- validates JPEG start and end markers before returning the payload;
- returns `ANALYSIS_SNAPSHOT_NOT_READY` without exposing a filesystem path.

Camera start, detail, and status responses now include additive snapshot links.
No existing field or endpoint was removed.

## Web capability

The Camera Admin application now provides:

- Camera CRUD, start/stop, raw frame, Run history, profile, Hub, and operations
  views retained from the original application;
- algorithm enablement, sampling FPS, profile, callback, and algorithm
  selection in the Camera form;
- a real-time monitor driven by `GET /api/v1/cameras/{id}/status`;
- occupancy, initial occupancy, IN, OUT, live-person, inference-FPS, and Run
  state cards;
- phase1 through phase4 result cards;
- an authenticated annotated-snapshot view using an in-memory Blob URL;
- a unified alert center driven by `GET /api/v1/cameras/{id}/alerts`;
- desktop and narrow-screen responsive layouts.

All dynamic values are written with `textContent` or escaped before HTML
composition. The administrator token exists only in JavaScript memory and is
not written to local storage, session storage, or cookies.

## Browser and response security

Camera Admin assets now include:

- `default-src 'self'`;
- `base-uri 'none'`;
- `object-src 'none'`;
- `frame-ancestors 'none'`;
- `form-action 'self'`;
- `img-src 'self' blob:`;
- `style-src 'self'`;
- `script-src 'self'`;
- `connect-src 'self'`;
- `Referrer-Policy: no-referrer`;
- a restrictive `Permissions-Policy`.

## Test coverage

`tests/web_admin_contract_test.py` verifies:

- all parity controls and result fields exist;
- Camera API endpoint wiring is present;
- `/people-flow/*` is absent from Web JavaScript;
- token persistence APIs are absent;
- dynamic DOM output is escaped;
- snapshot path confinement is retained;
- CSP and response hardening directives exist;
- responsive monitor styles exist.

`tools/mock_camera_admin_server.py` supplies deterministic authenticated Camera
data for browser acceptance without depending on production cameras.

The Camera HTTP contract additionally verifies authenticated JPEG delivery,
unauthenticated rejection, and traversal rejection for the new snapshot
endpoint. `scripts/test_all.ps1` and the GitHub contract workflow run both Qt
and Web client contracts.

## Acceptance evidence

- Backend configure and build: passed.
- Baseline CTest: 22 targets, 0 failures; service tests skipped by guards.
- Full CTest with disposable PostgreSQL 17 and isolated Redis DB 14: 22/22
  passed, 0 failed, 0 skipped.
- Qt compatibility client contract: passed.
- Camera Admin Web contract: passed.
- JavaScript syntax and Python mock/test compilation: passed.
- In-app browser desktop acceptance: Camera list, real-time metrics, phase1–4,
  decoded authenticated snapshot, and alert center passed.
- In-app browser 390 px responsive acceptance: passed.
- Browser console warnings/errors: none.

## Rollback

R6 is additive. To roll back the Web presentation, deploy the previous
`web/camera-admin` assets. The new snapshot endpoint and additive response links
may remain because old clients ignore them. No schema rollback or data deletion
is required.

## R7 entry gate

R7 may begin only after the R6 branch is pushed and its CI checks pass. R7 must
execute the full integration, recovery, hardware, soak, stress, golden-result,
and performance matrix before the deployment default can change.
