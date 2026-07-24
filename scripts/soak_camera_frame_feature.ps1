[CmdletBinding()]
param(
    [string]$BaseUrl = "http://127.0.0.1:8087",
    [Alias("TaskId")]
    [string]$CameraId = "",
    [Alias("CreateEphemeralTask")]
    [switch]$CreateEphemeralCamera,
    [string]$CameraProfile = "entry_camera_01",
    [double]$DurationMinutes = 60,
    [int]$PollSeconds = 5,
    [switch]$RequirePeopleFlowSubscriber,
    [switch]$CaptureHostTelemetry,
    [switch]$LeaveRunning,
    [string]$EvidenceRoot = ".\reports\soak"
)

$ErrorActionPreference = "Stop"
if ($DurationMinutes -le 0 -or $PollSeconds -lt 1) { throw "DurationMinutes and PollSeconds must be positive." }
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot

function Read-SecretText([string]$Prompt) {
    $secure = Read-Host $Prompt -AsSecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
}

$token = $env:YOLO11_CAMERA_TASK_ADMIN_TOKEN
if (-not $token) { $token = Read-SecretText "Camera Task admin token" }
if (-not $token) { throw "Camera Task admin token is required." }
$headers = @{ Authorization = "Bearer $token" }
$api = $BaseUrl.TrimEnd('/') + "/api/v1"
$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
$evidenceDir = Join-Path ([IO.Path]::GetFullPath((Join-Path $ProjectRoot $EvidenceRoot))) $stamp
New-Item -ItemType Directory -Force -Path $evidenceDir | Out-Null
$samplesPath = Join-Path $evidenceDir "samples.jsonl"
$summaryPath = Join-Path $evidenceDir "summary.json"

$created = $false
$startedByHarness = $false
$violations = [System.Collections.Generic.List[string]]::new()
$samples = 0
$cameraVersion = 0
$telemetrySamples = 0
$maxProcessCpuPercent = 0.0
$maxProcessWorkingSetMiB = 0.0
$maxGpuMemoryUsedMiB = 0
$maxGpuUtilizationPercent = 0
$maxGpuTemperatureC = 0
$maxDiskUsedPercent = 0.0
$baselineHubInstanceId = ""
$baselineHubOpenCount = 0
$baselineHubReconnectCount = 0
$telemetryState = @{
    captured_at = $null
    cpu_seconds = 0.0
}

function Read-HostTelemetry {
    $capturedAt = [DateTimeOffset]::UtcNow
    $processRows = @()
    $totalCpuSeconds = 0.0
    $totalWorkingSetBytes = [int64]0
    $pidFile = Join-Path $ProjectRoot "runtime\pids\demo.json"
    if (Test-Path -LiteralPath $pidFile) {
        $pidPayload = Get-Content -LiteralPath $pidFile -Raw -Encoding UTF8 |
            ConvertFrom-Json
        foreach ($item in @($pidPayload.processes)) {
            if (-not $item.pid) { continue }
            $process = Get-Process -Id ([int]$item.pid) -ErrorAction Stop
            $cpuSeconds = if ($null -eq $process.CPU) {
                0.0
            }
            else {
                [double]$process.CPU
            }
            $totalCpuSeconds += $cpuSeconds
            $totalWorkingSetBytes += [int64]$process.WorkingSet64
            $processRows += [ordered]@{
                name = [string]$item.name
                pid = [int]$process.Id
                cpu_seconds = [Math]::Round($cpuSeconds, 3)
                working_set_mib = [Math]::Round(
                    $process.WorkingSet64 / 1MB, 2)
            }
        }
    }

    $processCpuPercent = 0.0
    if ($null -ne $telemetryState.captured_at) {
        $elapsedSeconds = (
            $capturedAt - [DateTimeOffset]$telemetryState.captured_at
        ).TotalSeconds
        $cpuDelta = $totalCpuSeconds - [double]$telemetryState.cpu_seconds
        if ($elapsedSeconds -gt 0 -and $cpuDelta -ge 0) {
            $processCpuPercent = 100.0 * $cpuDelta /
                $elapsedSeconds / [Environment]::ProcessorCount
        }
    }
    $telemetryState.captured_at = $capturedAt
    $telemetryState.cpu_seconds = $totalCpuSeconds

    $gpuText = & nvidia-smi.exe `
        --query-gpu=memory.used,utilization.gpu,temperature.gpu `
        --format=csv,noheader,nounits 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "nvidia-smi telemetry query failed"
    }
    $gpuParts = @($gpuText.Trim().Split(",") |
        ForEach-Object { $_.Trim() })
    if ($gpuParts.Count -lt 3) {
        throw "nvidia-smi telemetry response is incomplete"
    }

    $driveName = [IO.Path]::GetPathRoot($ProjectRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar).TrimEnd(":")
    $drive = Get-PSDrive -Name $driveName -ErrorAction Stop
    $diskTotal = [double]($drive.Used + $drive.Free)
    $diskUsedPercent = if ($diskTotal -gt 0) {
        100.0 * [double]$drive.Used / $diskTotal
    }
    else {
        0.0
    }

    return [ordered]@{
        processes = $processRows
        process_cpu_percent = [Math]::Round($processCpuPercent, 2)
        process_working_set_mib = [Math]::Round(
            $totalWorkingSetBytes / 1MB, 2)
        gpu = [ordered]@{
            memory_used_mib = [int]$gpuParts[0]
            utilization_percent = [int]$gpuParts[1]
            temperature_c = [int]$gpuParts[2]
        }
        disk = [ordered]@{
            drive = $driveName
            free_mib = [Math]::Round($drive.Free / 1MB, 2)
            used_percent = [Math]::Round($diskUsedPercent, 2)
        }
    }
}

try {
    if ($CreateEphemeralCamera) {
        if ($CameraId) { throw "CameraId and CreateEphemeralCamera are mutually exclusive." }
        $CameraId = "soak_$stamp"
        $payload = @{
            camera_id = $CameraId
            name = "M9 soak $stamp"
            camera_profile = $CameraProfile
            frame_interval_ms = 1000
            output_mode = "both"
            jpeg_quality = 85
            max_width = 1280
            max_height = 720
            retention_days = 1
            max_saved_frames = 1000
        } | ConvertTo-Json
        Invoke-RestMethod -Method Post -Uri "$api/cameras" `
            -Headers $headers -ContentType "application/json" -Body $payload
        $detail = Invoke-RestMethod -Uri "$api/cameras/$CameraId" -Headers $headers
        $cameraVersion = [int]$detail.camera.version
        $created = $true
    }
    if (-not $CameraId) { throw "Pass -CameraId or -CreateEphemeralCamera." }

    $initial = Invoke-RestMethod -Uri "$api/cameras/$CameraId/status" -Headers $headers
    if ($initial.status -notin @('queued','starting','running','reconnecting','stopping')) {
        Invoke-RestMethod -Method Post -Uri "$api/cameras/$CameraId/start" -Headers $headers | Out-Null
        $startedByHarness = $true
    }

    $runtimeDeadline = (Get-Date).AddSeconds(60)
    do {
        $runtime = Invoke-RestMethod -Uri "$api/cameras/$CameraId/status" `
            -Headers $headers -TimeoutSec 4
        if ($runtime.status -eq 'failed') {
            throw "Camera entered failed before soak sampling."
        }
        if ($runtime.status -eq 'running' -and
            $runtime.pipeline.thread_running -and $runtime.hub) {
            break
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $runtimeDeadline)
    if ($runtime.status -ne 'running' -or
        -not $runtime.pipeline.thread_running -or -not $runtime.hub) {
        throw "Camera did not reach a running Hub before soak sampling."
    }
    $baselineHubInstanceId = [string]$runtime.hub.hub_instance_id
    $baselineHubOpenCount = [int]$runtime.hub.open_count
    $baselineHubReconnectCount = [int]$runtime.hub.reconnect_count

    $deadline = (Get-Date).AddMinutes($DurationMinutes)
    while ((Get-Date) -lt $deadline) {
        $capturedAt = (Get-Date).ToUniversalTime().ToString("o")
        try {
            $health = Invoke-RestMethod -Uri "$api/health" -TimeoutSec 4
            $ready = Invoke-RestMethod -Uri "$api/ready" -TimeoutSec 4
            $status = Invoke-RestMethod -Uri "$api/cameras/$CameraId/status" -Headers $headers -TimeoutSec 4
            $metrics = Invoke-RestMethod -Uri "$api/operations/metrics" -Headers $headers -TimeoutSec 4
            $hub = $status.hub
            $hostTelemetry = $null
            $sampleViolations = [System.Collections.Generic.List[string]]::new()
            if (-not $health.success) { $sampleViolations.Add("health_failed") }
            if (-not $ready.ready) { $sampleViolations.Add("not_ready") }
            if ($status.status -notin @('running','reconnecting')) {
                $sampleViolations.Add("task_not_running")
            }
            if (-not $status.pipeline.thread_running) {
                $sampleViolations.Add("pipeline_thread_not_running")
            }
            if (-not $hub) {
                $sampleViolations.Add("hub_missing")
            }
            if (-not $metrics.invariants.one_hub_record_per_profile) { $sampleViolations.Add("multiple_hubs_per_profile") }
            if (-not $metrics.invariants.open_count_consistent_with_reconnects) { $sampleViolations.Add("open_reconnect_inconsistent") }
            if (-not $metrics.invariants.subscriber_count_matches_types) { $sampleViolations.Add("subscriber_count_inconsistent") }
            if ($hub -and $hub.open_count -gt ($hub.reconnect_count + 1)) { $sampleViolations.Add("hub_reopened_without_reconnect") }
            if ($hub -and
                [string]$hub.hub_instance_id -ne $baselineHubInstanceId) {
                $sampleViolations.Add("hub_instance_changed")
            }
            if ($hub -and
                ([int]$hub.open_count -ne $baselineHubOpenCount -or
                 [int]$hub.reconnect_count -ne $baselineHubReconnectCount)) {
                $sampleViolations.Add("hub_reconnected_during_steady_soak")
            }
            if ($hub -and
                [int]$hub.subscriber_types.camera_task -lt 1) {
                $sampleViolations.Add("camera_task_subscriber_missing")
            }
            if ($RequirePeopleFlowSubscriber -and
                (-not $hub -or
                 [int]$hub.subscriber_types.people_flow -lt 1)) {
                $sampleViolations.Add("people_flow_subscriber_missing")
            }
            if ($CaptureHostTelemetry) {
                try {
                    $hostTelemetry = Read-HostTelemetry
                    ++$telemetrySamples
                    $maxProcessCpuPercent = [Math]::Max(
                        $maxProcessCpuPercent,
                        [double]$hostTelemetry.process_cpu_percent)
                    $maxProcessWorkingSetMiB = [Math]::Max(
                        $maxProcessWorkingSetMiB,
                        [double]$hostTelemetry.process_working_set_mib)
                    $maxGpuMemoryUsedMiB = [Math]::Max(
                        $maxGpuMemoryUsedMiB,
                        [int]$hostTelemetry.gpu.memory_used_mib)
                    $maxGpuUtilizationPercent = [Math]::Max(
                        $maxGpuUtilizationPercent,
                        [int]$hostTelemetry.gpu.utilization_percent)
                    $maxGpuTemperatureC = [Math]::Max(
                        $maxGpuTemperatureC,
                        [int]$hostTelemetry.gpu.temperature_c)
                    $maxDiskUsedPercent = [Math]::Max(
                        $maxDiskUsedPercent,
                        [double]$hostTelemetry.disk.used_percent)
                }
                catch {
                    $sampleViolations.Add("host_telemetry_failed")
                }
            }
            foreach ($item in $sampleViolations) { $violations.Add("$capturedAt $item") }
            [ordered]@{
                captured_at = $capturedAt
                health = $health
                ready = $ready
                camera = $status
                operations = $metrics
                host_telemetry = $hostTelemetry
                violations = @($sampleViolations)
            } | ConvertTo-Json -Depth 20 -Compress | Add-Content -Path $samplesPath -Encoding UTF8
        }
        catch {
            $message = $_.Exception.Message -replace '(?i)rtsp[s]?://\S+', 'rtsp://***'
            $violations.Add("$capturedAt sample_error")
            [ordered]@{ captured_at=$capturedAt; sample_error=$message } |
                ConvertTo-Json -Compress | Add-Content -Path $samplesPath -Encoding UTF8
        }
        ++$samples
        Start-Sleep -Seconds $PollSeconds
    }
}
finally {
    if ($CameraId -and $startedByHarness -and -not $LeaveRunning) {
        try { Invoke-RestMethod -Method Post -Uri "$api/cameras/$CameraId/stop" -Headers $headers | Out-Null } catch {}
    }
    if ($created -and -not $LeaveRunning) {
        for ($attempt = 0; $attempt -lt 30; ++$attempt) {
            try {
                $status = Invoke-RestMethod -Uri "$api/cameras/$CameraId/status" -Headers $headers
                if ($status.status -notin @('queued','starting','running','reconnecting','stopping')) { break }
            } catch { break }
            Start-Sleep -Seconds 1
        }
        try {
            $detail = Invoke-RestMethod -Uri "$api/cameras/$CameraId" -Headers $headers
            $deleteHeaders = @{ Authorization=$headers.Authorization; "If-Match"=('"' + $detail.camera.version + '"') }
            Invoke-WebRequest -Method Delete -Uri "$api/cameras/$CameraId" `
                -Headers $deleteHeaders -UseBasicParsing | Out-Null
        } catch { $violations.Add("cleanup_failed") }
    }
    [ordered]@{
        started_at = $stamp
        completed_at = (Get-Date).ToUniversalTime().ToString("o")
        camera_id = $CameraId
        duration_minutes = $DurationMinutes
        poll_seconds = $PollSeconds
        hub_baseline = [ordered]@{
            hub_instance_id = $baselineHubInstanceId
            open_count = $baselineHubOpenCount
            reconnect_count = $baselineHubReconnectCount
        }
        samples = $samples
        violation_count = $violations.Count
        violations = @($violations)
        passed = $violations.Count -eq 0
        host_telemetry = [ordered]@{
            enabled = [bool]$CaptureHostTelemetry
            samples = $telemetrySamples
            max_process_cpu_percent = [Math]::Round(
                $maxProcessCpuPercent, 2)
            max_process_working_set_mib = [Math]::Round(
                $maxProcessWorkingSetMiB, 2)
            max_gpu_memory_used_mib = $maxGpuMemoryUsedMiB
            max_gpu_utilization_percent = $maxGpuUtilizationPercent
            max_gpu_temperature_c = $maxGpuTemperatureC
            max_disk_used_percent = [Math]::Round(
                $maxDiskUsedPercent, 2)
        }
        samples_file = $samplesPath
    } | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8
    $token = $null
}

if ($violations.Count -ne 0) { throw "M9 soak failed with $($violations.Count) violation(s). Evidence: $summaryPath" }
Write-Host "PASS: M9 soak completed with $samples samples. Evidence: $summaryPath" -ForegroundColor Green
