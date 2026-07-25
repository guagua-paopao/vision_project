[CmdletBinding()]
param(
    [string]$Root = "",
    [string]$BuildDir = ".\out\build\backend-Release",
    [string]$ApiBase = "http://127.0.0.1:8087/api/v1",
    [string]$CameraProfile = "entry_camera_01",
    [int]$WaitSeconds = 120,
    [string]$EvidenceDir = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = if ($Root) {
    (Resolve-Path $Root).Path
}
else {
    (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
Set-Location $ProjectRoot
. (Join-Path $PSScriptRoot "process_tree_helpers.ps1")
$ApiBase = $ApiBase.TrimEnd("/")
$token = $env:YOLO11_CAMERA_TASK_ADMIN_TOKEN
if ([string]::IsNullOrWhiteSpace($token)) {
    throw "YOLO11_CAMERA_TASK_ADMIN_TOKEN is required."
}
$headers = @{ Authorization = "Bearer $token" }
$cameraId = "p6_restart_" +
    [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds().ToString()
$created = $false
$newWorker = $null
$pidFile = Join-Path $ProjectRoot "runtime\pids\demo.json"

function Invoke-Api {
    param(
        [string]$Method,
        [string]$Path,
        [object]$Body = $null,
        [hashtable]$ExtraHeaders = @{}
    )
    $requestHeaders = @{}
    foreach ($item in $headers.GetEnumerator()) {
        $requestHeaders[$item.Key] = $item.Value
    }
    foreach ($item in $ExtraHeaders.GetEnumerator()) {
        $requestHeaders[$item.Key] = $item.Value
    }
    $parameters = @{
        Uri = "$ApiBase$Path"
        Method = $Method
        Headers = $requestHeaders
        UseBasicParsing = $true
        TimeoutSec = 10
    }
    if ($null -ne $Body) {
        $parameters.ContentType = "application/json"
        $parameters.Body = if ($Body -is [string]) {
            $Body
        }
        else {
            $Body | ConvertTo-Json -Depth 10 -Compress
        }
    }
    return Invoke-WebRequest @parameters
}

function Read-Json([object]$Response) {
    if (-not $Response.Content) { return $null }
    return $Response.Content | ConvertFrom-Json
}

try {
    if (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) {
        throw "Demo PID manifest is missing."
    }
    if (-not $EvidenceDir) {
        $stamp = [DateTimeOffset]::UtcNow.ToString("yyyyMMddTHHmmssZ")
        $EvidenceDir = Join-Path $ProjectRoot "reports\p6\restart\$stamp"
    }
    New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

    $createBody = @{
        camera_id = $cameraId
        name = "P6 worker restart recovery"
        camera_profile = $CameraProfile
        desired_state = "stopped"
        frame_interval_ms = 500
        output_mode = "latest"
        jpeg_quality = 85
        max_width = 1280
        max_height = 720
        retention_days = 1
        max_saved_frames = 100
        analysis = @{
            enabled = $true
            target_infer_fps = 5.0
            algorithm_profile = "security_default"
            algorithms = @("people_flow")
        }
    }
    $create = Invoke-Api -Method POST -Path "/cameras" `
        -Body $createBody -ExtraHeaders @{
            "Idempotency-Key" = "p6-restart-create-$cameraId"
        }
    if ($create.StatusCode -ne 201) {
        throw "Restart exercise camera create failed."
    }
    $created = $true
    $start = Invoke-Api -Method POST -Path "/cameras/$cameraId/start" `
        -ExtraHeaders @{
            "Idempotency-Key" = "p6-restart-start-$cameraId"
        }
    if ($start.StatusCode -notin @(200, 202)) {
        throw "Restart exercise camera start failed."
    }
    $initialRunId = [string](Read-Json $start).run_id
    $deadline = (Get-Date).AddSeconds(60)
    do {
        $before = Read-Json (
            Invoke-Api -Method GET -Path "/cameras/$cameraId/status")
        if ($before.status -eq "running" -and
            $before.pipeline.thread_running) {
            break
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    if ($before.status -ne "running" -or
        -not $before.pipeline.thread_running) {
        throw "Restart exercise Camera did not reach running."
    }

    $manifest = Get-Content -LiteralPath $pidFile -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $worker = @($manifest.processes |
        Where-Object { $_.name -eq "worker" }) | Select-Object -First 1
    if (-not $worker -or -not $worker.pid -or
        -not $manifest.worker_config) {
        throw "Worker PID/configuration is missing from the demo manifest."
    }
    $oldWorkerId = [int]$worker.pid
    $oldFfmpegIds = @(
        Get-NativeChildProcessId -ParentProcessId $oldWorkerId `
            -ExecutableName "ffmpeg.exe"
    )
    if ($oldFfmpegIds.Count -ne 1) {
        throw "Expected exactly one FFmpeg reader before Worker termination."
    }
    Stop-Process -Id $oldWorkerId -Force -ErrorAction Stop
    Wait-Process -Id $oldWorkerId -Timeout 10 -ErrorAction SilentlyContinue
    $killedAtMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $orphanDeadline = (Get-Date).AddSeconds(10)
    do {
        $orphaned = @($oldFfmpegIds | Where-Object {
            Get-Process -Id $_ -ErrorAction SilentlyContinue
        })
        if ($orphaned.Count -eq 0) { break }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $orphanDeadline)
    if ($orphaned.Count -gt 0) {
        throw "Forced Worker exit left an orphan FFmpeg process."
    }

    $workerExe = Join-Path (
        [IO.Path]::GetFullPath((Join-Path $ProjectRoot $BuildDir))
    ) "four_stage_worker.exe"
    $stdout = Join-Path $EvidenceDir "worker.restart.stdout.log"
    $stderr = Join-Path $EvidenceDir "worker.restart.stderr.log"
    $newWorker = Start-Process -FilePath $workerExe `
        -ArgumentList @(
            [string]$manifest.worker_config,
            "--consumer-name",
            "people_flow_worker_1"
        ) -WorkingDirectory $ProjectRoot -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr

    foreach ($entry in @($manifest.processes)) {
        if ($entry.name -eq "worker") {
            $entry.pid = $newWorker.Id
            $entry.stdout = $stdout
            $entry.stderr = $stderr
        }
    }
    $manifest | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $pidFile -Encoding UTF8

    $recovered = $false
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($newWorker.HasExited) {
            throw "Replacement Worker exited during recovery."
        }
        try {
            $ready = Invoke-RestMethod -Uri "$ApiBase/ready" -TimeoutSec 4
            $status = Read-Json (
                Invoke-Api -Method GET `
                    -Path "/cameras/$cameraId/status")
            $runtimeFenceReady =
                $ready.worker_mode_consistent -and
                $ready.single_vision_worker -and
                $ready.worker_coordination_healthy -and
                $ready.camera_task_manager_running -and
                $ready.hub_registry_ready -and
                ($ready.expected_runtime_mode -ne
                    "unified_camera_pipeline" -or
                    -not $ready.legacy_people_flow_worker_detected)
            if ($ready.ready -and $runtimeFenceReady -and
                $ready.algorithm_runtime_generated_at_ms -gt $killedAtMs -and
                $status.status -eq "running" -and
                $status.pipeline.thread_running -and
                $status.run_id -ne $initialRunId -and
                -not $status.runtime_stale) {
                $recovered = $true
                break
            }
        }
        catch {
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $recovered) {
        throw "Camera did not recover to a new running generation after Worker restart."
    }

    $runs = Read-Json (
        Invoke-Api -Method GET `
            -Path "/cameras/$cameraId/runs?limit=20&offset=0")
    $oldRun = @($runs.items |
        Where-Object { $_.run_id -eq $initialRunId }) |
        Select-Object -First 1
    if (-not $oldRun -or $oldRun.status -ne "failed" -or
        $oldRun.error_code -ne "WORKER_RESTARTED") {
        throw "Old Camera Run is missing the durable Worker restart audit."
    }

    [ordered]@{
        passed = $true
        camera_id = $cameraId
        old_worker_pid = $oldWorkerId
        new_worker_pid = $newWorker.Id
        old_ffmpeg_children = $oldFfmpegIds.Count
        orphan_ffmpeg_children = 0
        old_run_id = $initialRunId
        new_run_id = $status.run_id
        old_run_status = $oldRun.status
        old_run_error_code = $oldRun.error_code
        recovered_at_ms = $ready.algorithm_runtime_generated_at_ms
        runtime_mode = $ready.expected_runtime_mode
        worker_generation = @($ready.workers |
            Where-Object { $_.alive } |
            Select-Object -First 1).worker_generation
        coordination_healthy = $ready.worker_coordination_healthy
    } | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (
            Join-Path $EvidenceDir "summary.json") -Encoding UTF8

    Write-Host (
        "PASS: forced Worker restart recovered Camera to a new Run generation."
    ) -ForegroundColor Green
}
finally {
    if ($created) {
        try {
            Invoke-Api -Method POST -Path "/cameras/$cameraId/stop" |
                Out-Null
        }
        catch {
        }
        try {
            $current = Read-Json (
                Invoke-Api -Method GET -Path "/cameras/$cameraId")
            Invoke-Api -Method DELETE -Path "/cameras/$cameraId" `
                -ExtraHeaders @{
                    "If-Match" = '"' +
                        [string]$current.camera.version + '"'
                } | Out-Null
        }
        catch {
            Write-Warning "Worker restart exercise camera cleanup needs review."
        }
    }
    $token = $null
}
