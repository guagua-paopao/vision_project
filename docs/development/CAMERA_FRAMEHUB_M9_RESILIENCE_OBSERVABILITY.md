# M9 - resilience and observability

Date: 2026-07-21  
Status: implementation accepted; deployment soak is an operator field gate

## Delivered endpoints

All operations endpoints require the Camera Task bearer token.

- `GET /api/v1/operations/metrics` - JSON snapshot;
- `GET /api/v1/operations/metrics/prometheus` - Prometheus 0.0.4 text.

The snapshot merges:

- SQLite task, Run and frame totals;
- archive logical byte usage;
- output-volume capacity/free/available bytes;
- Redis Hub capture/open/reconnect/subscriber state;
- invariants for one Hub record per Profile, reopen/reconnect consistency and
  subscriber type totals.

Neither endpoint includes RTSP URIs, `url_env`, bearer tokens or raw Redis
errors.

## Soak evidence harness

`scripts/soak_camera_frame_feature.ps1` polls health, readiness, task status,
Hub and operations metrics. Every sample is written as sanitized JSONL and a
final summary records violations.

Example 72-hour shared decode acceptance:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\soak_camera_frame_feature.ps1 `
  -TaskId ct_your_task -DurationMinutes 4320 -PollSeconds 10 `
  -RequirePeopleFlowSubscriber
```

For an isolated short task, pass `-CreateEphemeralTask`. The harness creates,
starts, stops and soft-deletes its task. It never reads or records the RTSP
environment variable.

## Failure drills

Run one fault at a time while the soak harness records evidence:

1. camera network interruption: Hub enters reconnecting and returns to
   running; `open_count <= reconnect_count + 1`;
2. Redis restart: readiness becomes false, then returns; durable task/Run data
   remains queryable and stale state is explicit;
3. Worker termination/restart: stale active Run is recovered as failed and a
   new Run can start;
4. Server restart: worker capture remains independent; HTTP state reconstructs
   from SQLite and Redis;
5. output directory made unavailable in a disposable test directory: affected
   extraction Run fails without stopping People Flow or the shared Hub.

Do not combine faults until every single-fault drill passes. Preserve the
generated `reports/soak/<timestamp>` directory with the deployment report.

## Acceptance evidence

- Release build and CTest: 11/11 pass;
- operations JSON/Prometheus authentication and secret-exclusion contract:
  pass;
- durable/Hub/filesystem metrics merge contract: pass;
- soak PowerShell parser and invariant guards: pass;
- prior M7 local RTSP shared-Hub acceptance remains the live functional
  baseline; the new long-duration certificate is generated in the target
  deployment because credentials are intentionally not injected into build
  tooling.

