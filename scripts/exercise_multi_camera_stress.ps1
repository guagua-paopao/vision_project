[CmdletBinding()]
param(
    [string]$ApiBase = "http://127.0.0.1:8087/api/v1",
    [string]$CameraProfile = "entry_camera_01",
    [int]$CameraCount = 3,
    [int]$ExpectedExistingSubscribers = 1,
    [int]$WaitSeconds = 120,
    [int]$MeasurementSeconds = 20,
    [double]$MinAggregateInferenceFps = 2.0,
    [switch]$RequireUnifiedCameraPipeline,
    [string]$EvidenceDir = ""
)

$ErrorActionPreference = "Stop"
if ($CameraCount -lt 1 -or $CameraCount -gt 16) {
    throw "CameraCount must be between 1 and 16."
}
if ($ExpectedExistingSubscribers -lt 0) {
    throw "ExpectedExistingSubscribers cannot be negative."
}
if ($MeasurementSeconds -lt 5) {
    throw "MeasurementSeconds must be at least 5."
}
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot
$ApiBase = $ApiBase.TrimEnd("/")
$token = $env:YOLO11_CAMERA_TASK_ADMIN_TOKEN
if ([string]::IsNullOrWhiteSpace($token)) {
    throw "YOLO11_CAMERA_TASK_ADMIN_TOKEN is required."
}
$headers = @{ Authorization = "Bearer $token" }
$created = [System.Collections.Generic.List[string]]::new()
$stamp = [DateTimeOffset]::UtcNow.ToString("yyyyMMddTHHmmssZ")
if (-not $EvidenceDir) {
    $EvidenceDir = Join-Path $ProjectRoot "reports\r7\stress\$stamp"
}
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

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

function Test-ExpectedSubscribers([object]$Hub, [int]$MinimumCount) {
    if (-not $Hub -or -not $Hub.subscriber_types) {
        return $false
    }
    if ($RequireUnifiedCameraPipeline) {
        return [int]$Hub.subscriber_types.camera_pipeline -ge $MinimumCount -and
            [int]$Hub.subscriber_types.people_flow -eq 0 -and
            [int]$Hub.subscriber_types.camera_task -eq 0
    }
    return [int]$Hub.subscriber_types.camera_task -ge $CameraCount
}

try {
    for ($index = 1; $index -le $CameraCount; ++$index) {
        $cameraId = "r7_stress_${stamp}_$index".ToLowerInvariant()
        $body = @{
            camera_id = $cameraId
            name = "R7 multi-camera stress $index"
            camera_profile = $CameraProfile
            desired_state = "stopped"
            frame_interval_ms = 1000
            output_mode = "latest"
            jpeg_quality = 85
            max_width = 1280
            max_height = 720
            retention_days = 1
            max_saved_frames = 100
            analysis = @{
                enabled = $true
                target_infer_fps = 10
                algorithm_profile = "security_default"
                algorithms = @(
                    "people_flow",
                    "electronic_fence",
                    "pose_action"
                )
            }
            callback_profile = "backend_primary"
        }
        $create = Invoke-Api -Method POST -Path "/cameras" -Body $body `
            -ExtraHeaders @{
                "Idempotency-Key" = "r7-stress-create-$cameraId"
            }
        if ($create.StatusCode -ne 201) {
            throw "Stress camera create failed: $cameraId"
        }
        $created.Add($cameraId)
    }

    foreach ($cameraId in $created) {
        $start = Invoke-Api -Method POST -Path "/cameras/$cameraId/start" `
            -ExtraHeaders @{
                "Idempotency-Key" = "r7-stress-start-$cameraId"
            }
        if ($start.StatusCode -notin @(200, 202)) {
            throw "Stress camera start failed: $cameraId"
        }
    }

    $expectedSubscribers = $CameraCount + $ExpectedExistingSubscribers
    $statuses = @()
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ((Get-Date) -lt $deadline) {
        $statuses = @()
        $allRunning = $true
        foreach ($cameraId in $created) {
            $status = Read-Json (
                Invoke-Api -Method GET -Path "/cameras/$cameraId/status")
            $statuses += $status
            if ($status.status -ne "running" -or
                -not $status.pipeline.thread_running -or
                -not (Test-ExpectedSubscribers $status.hub $expectedSubscribers)) {
                $allRunning = $false
            }
        }
        if ($allRunning) { break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $allRunning) {
        throw "Multi-camera pipelines did not reach the expected shared-Hub state."
    }

    $hubIds = @($statuses | ForEach-Object {
        [string]$_.hub.hub_instance_id
    } | Sort-Object -Unique)
    if ($hubIds.Count -ne 1) {
        throw "Multi-camera stress opened more than one Hub for one profile."
    }
    $before = Read-Json (
        Invoke-Api -Method GET -Path "/operations/metrics")
    $processedBefore =
        [long]$before.algorithm_runtime.inference.processed_jobs
    $failedBefore =
        [long]$before.algorithm_runtime.inference.failed_jobs
    $sequenceBefore = [long]$statuses[0].hub.latest_sequence
    Start-Sleep -Seconds $MeasurementSeconds
    $after = Read-Json (
        Invoke-Api -Method GET -Path "/operations/metrics")
    $afterStatuses = @()
    foreach ($cameraId in $created) {
        $afterStatuses += Read-Json (
            Invoke-Api -Method GET -Path "/cameras/$cameraId/status")
    }
    $processedAfter =
        [long]$after.algorithm_runtime.inference.processed_jobs
    $failedAfter =
        [long]$after.algorithm_runtime.inference.failed_jobs
    $processedDelta = $processedAfter - $processedBefore
    $aggregateFps = [Math]::Round(
        $processedDelta / [double]$MeasurementSeconds, 3)

    if (-not $after.invariants.one_hub_record_per_profile -or
        -not $after.invariants.subscriber_count_matches_types) {
        throw "Operations metrics reported a Hub invariant violation."
    }
    if ([int]$after.algorithm_runtime.active_pipelines -lt
        $expectedSubscribers) {
        throw "Unified runtime did not retain every expected active pipeline."
    }
    if ([int]$after.algorithm_runtime.inference.workers_ready -ne
        [int]$after.algorithm_runtime.inference.workers_configured) {
        throw "Fixed inference pool lost a ready worker under stress."
    }
    if ($failedAfter -ne $failedBefore) {
        throw "Inference failures increased during multi-camera stress."
    }
    if ($aggregateFps -lt $MinAggregateInferenceFps) {
        throw "Aggregate inference throughput $aggregateFps FPS is below the approved threshold $MinAggregateInferenceFps FPS."
    }
    foreach ($status in $afterStatuses) {
        if ($status.status -ne "running" -or
            -not $status.pipeline.thread_running -or
            [long]$status.hub.latest_sequence -le $sequenceBefore -or
            -not (Test-ExpectedSubscribers $status.hub $expectedSubscribers)) {
            throw "A stressed Camera pipeline stopped advancing."
        }
    }

    [ordered]@{
        passed = $true
        camera_count_created = $CameraCount
        expected_existing_subscribers = $ExpectedExistingSubscribers
        expected_total_pipelines = $expectedSubscribers
        runtime_mode = if ($RequireUnifiedCameraPipeline) {
            "unified_camera_pipeline"
        }
        else {
            "legacy_split"
        }
        hub_instance_id = $hubIds[0]
        subscriber_types = $afterStatuses[0].hub.subscriber_types
        inference_workers_configured =
            $after.algorithm_runtime.inference.workers_configured
        inference_workers_ready =
            $after.algorithm_runtime.inference.workers_ready
        measurement_seconds = $MeasurementSeconds
        processed_jobs_delta = $processedDelta
        aggregate_inference_fps = $aggregateFps
        approved_min_aggregate_inference_fps =
            $MinAggregateInferenceFps
        failed_jobs_delta = $failedAfter - $failedBefore
    } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (
            Join-Path $EvidenceDir "summary.json") -Encoding UTF8
    Write-Host (
        "PASS: $CameraCount stress Cameras shared one Hub; aggregate inference " +
        "$aggregateFps FPS."
    ) -ForegroundColor Green
}
finally {
    foreach ($cameraId in $created) {
        try {
            Invoke-Api -Method POST -Path "/cameras/$cameraId/stop" |
                Out-Null
        }
        catch {
        }
    }
    foreach ($cameraId in $created) {
        for ($attempt = 0; $attempt -lt 30; ++$attempt) {
            try {
                $status = Read-Json (
                    Invoke-Api -Method GET -Path "/cameras/$cameraId/status")
                if ($status.status -notin @(
                        "queued", "starting", "running",
                        "reconnecting", "stopping")) {
                    break
                }
            }
            catch {
                break
            }
            Start-Sleep -Seconds 1
        }
        try {
            $detail = Read-Json (
                Invoke-Api -Method GET -Path "/cameras/$cameraId")
            Invoke-Api -Method DELETE -Path "/cameras/$cameraId" `
                -ExtraHeaders @{
                    "If-Match" = '"' +
                        [string]$detail.camera.version + '"'
                } | Out-Null
        }
        catch {
            Write-Warning "Stress camera cleanup needs review: $cameraId"
        }
    }
    $token = $null
}
