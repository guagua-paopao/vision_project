# M8 - Camera administration control plane

Date: 2026-07-21  
Status: accepted

## Delivered

- `/camera-admin` dependency-free web console;
- Camera Task CRUD, start/stop, status, Run history and latest-frame preview;
- shared Hub diagnostics;
- authenticated Camera Profile metadata CRUD at `/api/v1/camera-profiles`;
- YAML-backed atomic persistence, version ETags and soft delete;
- no RTSP URI or credential fields in the profile API;
- immutable-worker semantics expressed as `restart_required: true`.

## Camera Profile contract

Writable fields are `profile_id` (create only), `display_name`, `url_env`,
`transport` and `enabled`. `source_type` is fixed to `rtsp`. The `url_env`
value is an environment-variable name, not its secret value.

PATCH and DELETE require `If-Match`. DELETE is refused while a nondeleted
Camera Task references the Profile. A mutation rewrites `config/cameras.yaml`
through a same-directory temporary file and atomic replace.

Running Server/Worker processes do not reload profiles. Restart both processes
after a profile mutation. This prevents a task from silently switching its
source while a Run is active.

## UI security

- bearer token remains in a JavaScript variable only;
- no `localStorage`, `sessionStorage`, query-token or cookie storage;
- no inline script/style; the server sends a restrictive CSP;
- latest JPEG uses a temporary object URL;
- output is HTML-escaped before insertion.

## Acceptance evidence

- backend Release build: pass;
- CTest: 11/11 pass;
- `camera_task_http_contract_test`: Profile secret rejection, create, list,
  optimistic conflict, update and soft delete pass;
- JavaScript syntax check: pass;
- UI guard: task/profile/Hub panels present, no persistent token APIs and no
  inline script pass.

## Rollback

Remove the five `/api/v1/camera-profiles` routes and `/camera-admin` routes,
then restore the prior `config/cameras.yaml`. Existing Camera Task schema and
M0-M7 runtime data are unchanged.

