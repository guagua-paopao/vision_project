# M10 - storage lifecycle, backup, and recovery

Date: 2026-07-21  
Status: accepted

## Storage policy

Both Server and Worker use the same `camera_tasks.storage` block:

```yaml
storage:
  max_archive_bytes: 21474836480
  min_free_bytes: 1073741824
  high_watermark_percent: 85
  critical_watermark_percent: 95
  pressure_cleanup_batch_size: 1000
  backup_dir: "./runtime/backups"
  backup_retention_count: 7
```

- at the high watermark the sweeper removes oldest managed archive artifacts
  until the high target is satisfied;
- at the critical watermark, or below the free-space reserve, the writer
  rejects new archive publication with `STORAGE_PRESSURE`;
- `latest.jpg` is outside the archive metadata table and is never selected by
  retention;
- the existing canonical-path and reparse-point checks remain mandatory;
- a Camera Task write failure cannot stop the shared Hub or People Flow.

The authenticated operations endpoint reports `normal`, `high`, or `critical`
and Prometheus exposes `yolo11_camera_storage_pressure` as 0, 1, or 2.

## Offline backup

Stop the demo, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\backup_runtime.ps1 `
  -Label scheduled -RetentionCount 7
```

The script validates every SQLite database with `PRAGMA integrity_check`,
copies the three nonsecret YAML registries and database/WAL companions, writes
a SHA-256/size manifest, and atomically publishes a ZIP below
`runtime/backups`. Runtime output images and environment variables are not
included.

## Guarded restore

Restore is offline-only and requires explicit confirmation:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\restore_runtime.ps1 `
  -BackupPath .\runtime\backups\vision_runtime_<stamp>.zip `
  -ConfirmRestore
```

Restore validates the allowlisted paths, every SHA-256 and size, then SQLite
integrity. Before replacement it creates a `pre_restore` rollback archive.
It refuses to run while demo PIDs are alive. Restart and run health/readiness
and the Camera Frame release gate after restoration.

## Acceptance evidence

- `camera_storage_policy_test`: high-watermark oldest-first cleanup, exact
  logical byte accounting, latest preservation, critical writer rejection and
  metadata nonpublication pass;
- Release CTest: 12/12 pass;
- actual project offline backup: both Camera Task and People Flow databases
  pass SQLite integrity; manifest/config/database entries verified;
- restore without `-ConfirmRestore`: refused before mutation;
- full restore in an isolated Unicode-path fixture: manifest, SHA-256, both
  SQLite checks, rollback backup and replacement pass;
- Windows Unicode defect found during the drill was corrected by using
  `wmain` and explicit UTF-8 conversion in the integrity tool.

