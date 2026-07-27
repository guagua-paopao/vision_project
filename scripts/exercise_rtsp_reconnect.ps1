[CmdletBinding()]
param(
    [string]$Root = "",
    [string]$ApiBase = "http://127.0.0.1:8087/api/v1",
    [string]$CameraProfile = "entry_camera_01",
    [int]$WaitSeconds = 90,
    [switch]$RequireUnifiedCameraPipeline,
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
$cameraId = "p6_reconnect_" +
    [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds().ToString()
$created = $false
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
            $Body | ConvertTo-Json -Depth 8 -Compress
        }
    }
    return Invoke-WebRequest @parameters
}

function Read-Json([object]$Response) {
    if (-not $Response.Content) { return $null }
    return $Response.Content | ConvertFrom-Json
}

function Test-SubscriberContract([object]$Hub) {
    if (-not $Hub -or -not $Hub.subscriber_types) {
        return $false
    }
    if ($RequireUnifiedCameraPipeline) {
        return [int]$Hub.subscriber_types.camera_pipeline -ge 1 -and
            [int]$Hub.subscriber_types.people_flow -eq 0 -and
            [int]$Hub.subscriber_types.camera_task -eq 0
    }
    return [int]$Hub.subscriber_types.people_flow -ge 1 -and
        [int]$Hub.subscriber_types.camera_task -ge 1
}

try {
    if (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) {
        throw "Demo PID manifest is missing."
    }
    if (-not $EvidenceDir) {
        $stamp = [DateTimeOffset]::UtcNow.ToString("yyyyMMddTHHmmssZ")
        $EvidenceDir = Join-Path $ProjectRoot "reports\p6\reconnect\$stamp"
    }
    New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

    $body = @{
        camera_id = $cameraId
        name = "P6 RTSP reconnect exercise"
        camera_profile = $CameraProfile
        desired_state = "stopped"
        frame_interval_ms = 500
        output_mode = "latest"
        jpeg_quality = 85
        max_width = 1280
        max_height = 720
        retention_days = 1
        max_saved_frames = 100
    }
    $create = Invoke-Api -Method POST -Path "/cameras" -Body $body `
        -ExtraHeaders @{
            "Idempotency-Key" = "p6-reconnect-create-$cameraId"
        }
    if ($create.StatusCode -ne 201) {
        throw "Reconnect exercise camera create failed."
    }
    $created = $true
    $start = Invoke-Api -Method POST -Path "/cameras/$cameraId/start" `
        -ExtraHeaders @{
            "Idempotency-Key" = "p6-reconnect-start-$cameraId"
        }
    if ($start.StatusCode -notin @(200, 202)) {
        throw "Reconnect exercise camera start failed."
    }

    $deadline = (Get-Date).AddSeconds(60)
    do {
        $before = Read-Json (
            Invoke-Api -Method GET -Path "/cameras/$cameraId/status")
        if ($before.status -eq "running" -and
            $before.pipeline.thread_running -and
            (Test-SubscriberContract $before.hub)) {
            break
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    if ($before.status -ne "running" -or
        -not (Test-SubscriberContract $before.hub)) {
        throw "Reconnect exercise did not establish the expected shared-Hub subscriber contract."
    }

    $manifest = Get-Content -LiteralPath $pidFile -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $worker = @($manifest.processes |
        Where-Object { $_.name -eq "worker" }) | Select-Object -First 1
    if (-not $worker -or -not $worker.pid) {
        throw "Worker PID is missing from the demo manifest."
    }
    $ffmpegIds = @(
        Get-NativeChildProcessId -ParentProcessId ([int]$worker.pid) `
            -ExecutableName "ffmpeg.exe"
    )
    if ($ffmpegIds.Count -ne 1) {
        throw "Expected exactly one shared FFmpeg reader child."
    }
    $oldFfmpegPid = [int]$ffmpegIds[0]
    Stop-Process -Id $oldFfmpegPid -Force -ErrorAction Stop
    Wait-Process -Id $oldFfmpegPid -Timeout 10 `
        -ErrorAction SilentlyContinue

    $recovered = $false
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ((Get-Date) -lt $deadline) {
        $after = Read-Json (
            Invoke-Api -Method GET -Path "/cameras/$cameraId/status")
        if ($after.status -eq "running" -and
            $after.pipeline.thread_running -and
            $after.hub.hub_instance_id -eq $before.hub.hub_instance_id -and
            $after.hub.open_count -ge ($before.hub.open_count + 1) -and
            $after.hub.reconnect_count -ge
                ($before.hub.reconnect_count + 1) -and
            $after.hub.latest_sequence -gt $before.hub.latest_sequence -and
            (Test-SubscriberContract $after.hub)) {
            $recovered = $true
            break
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $recovered) {
        throw "Shared RTSP Hub did not recover after reader termination."
    }

    [ordered]@{
        passed = $true
        camera_id = $cameraId
        hub_instance_id = $after.hub.hub_instance_id
        terminated_ffmpeg_pid = $oldFfmpegPid
        open_count_before = $before.hub.open_count
        open_count_after = $after.hub.open_count
        reconnect_count_before = $before.hub.reconnect_count
        reconnect_count_after = $after.hub.reconnect_count
        source_sequence_before = $before.hub.latest_sequence
        source_sequence_after = $after.hub.latest_sequence
        runtime_mode = if ($RequireUnifiedCameraPipeline) {
            "unified_camera_pipeline"
        }
        else {
            "legacy_split"
        }
        subscribers_after = $after.hub.subscriber_types
    } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (
            Join-Path $EvidenceDir "summary.json") -Encoding UTF8

    Write-Host (
        "PASS: shared RTSP Hub reconnected after forced reader termination."
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
            Write-Warning "RTSP reconnect exercise camera cleanup needs review."
        }
    }
    $token = $null
}
