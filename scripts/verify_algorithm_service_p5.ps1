[CmdletBinding()]
param(
    [string]$ApiBase = "http://127.0.0.1:8087/api/v1",
    [string]$MockCallbackBase = "http://127.0.0.1:9095",
    [string]$CameraId = "",
    [string]$CameraProfile = "entry_camera_01",
    [string]$CallbackProfile = "backend_primary",
    [int]$WaitForRunningSeconds = 60,
    [int]$WaitForAlertSeconds = 120,
    [switch]$ControlPlaneOnly,
    [switch]$KeepCamera
)

$ErrorActionPreference = "Stop"
$adminToken = $env:YOLO11_CAMERA_TASK_ADMIN_TOKEN
if (-not $adminToken) {
    throw "YOLO11_CAMERA_TASK_ADMIN_TOKEN is required."
}
$mockControlToken = $env:YOLO11_MOCK_CALLBACK_CONTROL_TOKEN
if (-not $ControlPlaneOnly -and -not $mockControlToken) {
    throw "YOLO11_MOCK_CALLBACK_CONTROL_TOKEN is required for callback acceptance."
}
if (-not $CameraId) {
    $CameraId = "p5_accept_" +
        [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds().ToString()
}
$ApiBase = $ApiBase.TrimEnd("/")
$MockCallbackBase = $MockCallbackBase.TrimEnd("/")
$headers = @{ Authorization = "Bearer $adminToken" }
$created = $false
$etag = ""

function Invoke-Api {
    param(
        [string]$Method,
        [string]$Path,
        [object]$Body = $null,
        [hashtable]$ExtraHeaders = @{}
    )
    $requestHeaders = @{}
    foreach ($entry in $headers.GetEnumerator()) {
        $requestHeaders[$entry.Key] = $entry.Value
    }
    foreach ($entry in $ExtraHeaders.GetEnumerator()) {
        $requestHeaders[$entry.Key] = $entry.Value
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
            $Body | ConvertTo-Json -Depth 12 -Compress
        }
    }
    return Invoke-WebRequest @parameters
}

function Read-Json {
    param([object]$Response)
    if (-not $Response.Content) { return $null }
    return $Response.Content | ConvertFrom-Json
}

try {
    $health = Invoke-RestMethod -Uri "$ApiBase/health" -TimeoutSec 5
    if (-not $health.success) {
        throw "service health is false"
    }
    $ready = Invoke-RestMethod -Uri "$ApiBase/ready" -TimeoutSec 5
    if (-not $ready.ready -or -not $ready.algorithm_runtime_available -or
        -not $ready.algorithm_runtime_fresh -or
        -not $ready.inference_pool_ready -or
        -not $ready.callback_delivery_ready) {
        throw "algorithm service readiness gate is not satisfied"
    }
    if (-not $ControlPlaneOnly) {
        $mockHealth = Invoke-RestMethod -Uri "$MockCallbackBase/health" `
            -TimeoutSec 5
        if (-not $mockHealth.success) {
            throw "mock callback backend health is false"
        }
    }

    $createBody = @{
        camera_id = $CameraId
        name = "P5 acceptance camera"
        camera_profile = $CameraProfile
        desired_state = "stopped"
        frame_interval_ms = 1000
        output_mode = "latest"
        jpeg_quality = 90
        max_width = 1280
        max_height = 720
        retention_days = 7
        max_saved_frames = 1000
        analysis = @{
            enabled = $true
            target_infer_fps = 5.0
            algorithm_profile = "security_default"
            algorithms = @(
                "people_flow",
                "electronic_fence",
                "pose_action"
            )
        }
        callback_profile = $CallbackProfile
    }
    $create = Invoke-Api -Method POST -Path "/cameras" -Body $createBody `
        -ExtraHeaders @{
            "Idempotency-Key" = "p5-create-$CameraId"
        }
    if ($create.StatusCode -ne 201) {
        throw "camera create expected 201, got $($create.StatusCode)"
    }
    $created = $true
    $etag = $create.Headers["ETag"].Trim('"')

    $detail = Read-Json (Invoke-Api -Method GET -Path "/cameras/$CameraId")
    if ($detail.camera.camera_id -ne $CameraId -or
        $detail.camera.desired_state -ne "stopped") {
        throw "camera create/read contract mismatch"
    }

    $patch = Invoke-Api -Method PATCH -Path "/cameras/$CameraId" `
        -Body @{ frame_interval_ms = 500; jpeg_quality = 85 } `
        -ExtraHeaders @{ "If-Match" = "`"$etag`"" }
    if ($patch.StatusCode -ne 200) {
        throw "stopped camera update expected 200, got $($patch.StatusCode)"
    }
    $etag = $patch.Headers["ETag"].Trim('"')

    $start = Invoke-Api -Method POST -Path "/cameras/$CameraId/start" `
        -ExtraHeaders @{
            "Idempotency-Key" = "p5-start-$CameraId"
        }
    if ($start.StatusCode -notin @(200, 202)) {
        throw "camera start was not accepted"
    }

    $running = $false
    $runningDeadline = (Get-Date).AddSeconds($WaitForRunningSeconds)
    while ((Get-Date) -lt $runningDeadline) {
        $status = Read-Json (
            Invoke-Api -Method GET -Path "/cameras/$CameraId/status")
        if ($status.status -eq "running" -and
            $status.pipeline.thread_running -and
            -not $status.runtime_stale) {
            $running = $true
            break
        }
        if ($status.status -eq "failed") {
            throw "camera run failed: $($status.error_code)"
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $running) {
        throw "camera pipeline did not reach running state"
    }

    $metrics = Read-Json (
        Invoke-Api -Method GET -Path "/operations/metrics")
    if (-not $metrics.algorithm_runtime.available -or
        $metrics.algorithm_runtime.runtime_stale -or
        $metrics.algorithm_runtime.inference.workers_ready -lt 1 -or
        (-not $ControlPlaneOnly -and
         -not $metrics.algorithm_runtime.callbacks.running)) {
        throw "unified runtime metrics did not expose ready components"
    }

    if (-not $ControlPlaneOnly) {
        $callbackObserved = $false
        $alertDeadline = (Get-Date).AddSeconds($WaitForAlertSeconds)
        while ((Get-Date) -lt $alertDeadline) {
            $events = Invoke-RestMethod -Uri `
                "$MockCallbackBase/api/test/events" -Headers @{
                    Authorization = "Bearer $mockControlToken"
                } -TimeoutSec 5
            foreach ($event in $events.items) {
                if ($event.payload.camera_id -eq $CameraId) {
                    $callbackObserved = $true
                    break
                }
            }
            if ($callbackObserved) { break }
            Start-Sleep -Seconds 1
        }
        if (-not $callbackObserved) {
            throw "no signed callback was observed for camera $CameraId"
        }
    }

    $runs = Read-Json (
        Invoke-Api -Method GET -Path "/cameras/$CameraId/runs?limit=20")
    if ($runs.items.Count -lt 1) {
        throw "run history is empty"
    }
    Write-Host (
        "PASS: P5 HTTP -> Pipeline -> inference -> alert -> callback acceptance " +
        "completed for $CameraId.") -ForegroundColor Green
}
finally {
    if ($created -and -not $KeepCamera) {
        try {
            Invoke-Api -Method POST -Path "/cameras/$CameraId/stop" | Out-Null
        }
        catch {
        }
        for ($attempt = 0; $attempt -lt 30; ++$attempt) {
            try {
                $status = Read-Json (
                    Invoke-Api -Method GET `
                        -Path "/cameras/$CameraId/status")
                if ($status.status -notin @(
                        "queued",
                        "starting",
                        "running",
                        "reconnecting",
                        "stopping"
                    )) {
                    break
                }
            }
            catch {
                break
            }
            Start-Sleep -Seconds 1
        }
        try {
            $current = Invoke-Api -Method GET -Path "/cameras/$CameraId"
            $currentBody = Read-Json $current
            $etag = [string]$currentBody.camera.version
            Invoke-Api -Method DELETE -Path "/cameras/$CameraId" `
                -ExtraHeaders @{ "If-Match" = "`"$etag`"" } | Out-Null
        }
        catch {
            $cleanupStatus = if ($_.Exception.Response -and
                $_.Exception.Response.StatusCode) {
                [int]$_.Exception.Response.StatusCode
            }
            else {
                0
            }
            Write-Warning (
                "P5 acceptance camera cleanup needs manual review: " +
                "$CameraId (HTTP $cleanupStatus)"
            )
        }
    }
}
