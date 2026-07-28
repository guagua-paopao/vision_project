[CmdletBinding()]
param(
    [string]$BaseUrl = "http://127.0.0.1:8087",
    [string[]]$CameraIds = @("1", "1dsa"),
    [double]$TargetInferFps = 10.0,
    [double]$DurationMinutes = 30.0,
    [int]$PollSeconds = 5,
    [string]$EvidenceRoot = ".\reports\dual-camera-performance"
)

$ErrorActionPreference = "Stop"
if ($CameraIds.Count -ne 2) { throw "Exactly two camera IDs are required." }
if ($DurationMinutes -le 0 -or $PollSeconds -lt 1) {
    throw "DurationMinutes and PollSeconds must be positive."
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$token = $env:YOLO11_CAMERA_TASK_ADMIN_TOKEN
if (-not $token) { throw "YOLO11_CAMERA_TASK_ADMIN_TOKEN is required." }
$headers = @{ Authorization = "Bearer $token" }
$api = $BaseUrl.TrimEnd("/") + "/api/v1"
$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
$root = if ([IO.Path]::IsPathRooted($EvidenceRoot)) {
    [IO.Path]::GetFullPath($EvidenceRoot)
} else {
    [IO.Path]::GetFullPath((Join-Path $ProjectRoot $EvidenceRoot))
}
$evidenceDir = Join-Path $root $stamp
New-Item -ItemType Directory -Force -Path $evidenceDir | Out-Null
$samplesPath = Join-Path $evidenceDir "samples.jsonl"
$summaryPath = Join-Path $evidenceDir "summary.json"

$cpuState = @{
    captured_at = $null
    cpu_seconds = 0.0
}

function Get-Percentile([double[]]$Values, [double]$Quantile) {
    if (-not $Values -or $Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $position = $Quantile * ($sorted.Count - 1)
    $lower = [Math]::Floor($position)
    $upper = [Math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$sorted[$lower] }
    $fraction = $position - $lower
    return [double]$sorted[$lower] * (1.0 - $fraction) +
        [double]$sorted[$upper] * $fraction
}

function Get-HostTelemetry {
    $capturedAt = [DateTimeOffset]::UtcNow
    $processRows = @()
    $totalCpuSeconds = 0.0
    $totalWorkingSetBytes = [int64]0
    foreach ($name in @("four_stage_worker", "four_stage_server")) {
        foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            $cpuSeconds = if ($null -eq $process.CPU) { 0.0 } else { [double]$process.CPU }
            $totalCpuSeconds += $cpuSeconds
            $totalWorkingSetBytes += [int64]$process.WorkingSet64
            $processRows += [ordered]@{
                name = $process.ProcessName
                pid = $process.Id
                cpu_seconds = [Math]::Round($cpuSeconds, 3)
                working_set_mib = [Math]::Round($process.WorkingSet64 / 1MB, 2)
            }
        }
    }

    $processCpuPercent = 0.0
    if ($null -ne $cpuState.captured_at) {
        $elapsedSeconds = ($capturedAt - [DateTimeOffset]$cpuState.captured_at).TotalSeconds
        $cpuDelta = $totalCpuSeconds - [double]$cpuState.cpu_seconds
        if ($elapsedSeconds -gt 0 -and $cpuDelta -ge 0) {
            $processCpuPercent = 100.0 * $cpuDelta /
                $elapsedSeconds / [Environment]::ProcessorCount
        }
    }
    $cpuState.captured_at = $capturedAt
    $cpuState.cpu_seconds = $totalCpuSeconds

    $gpuText = & nvidia-smi.exe `
        --query-gpu=memory.used,utilization.gpu,temperature.gpu `
        --format=csv,noheader,nounits 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "nvidia-smi telemetry query failed." }
    $gpuParts = @($gpuText.Trim().Split(",") | ForEach-Object { $_.Trim() })

    $driveName = [IO.Path]::GetPathRoot($ProjectRoot).TrimEnd("\").TrimEnd(":")
    $drive = Get-PSDrive -Name $driveName -ErrorAction Stop
    $diskTotal = [double]($drive.Used + $drive.Free)
    $diskUsedPercent = if ($diskTotal -gt 0) {
        100.0 * [double]$drive.Used / $diskTotal
    } else { $null }

    return [ordered]@{
        processes = $processRows
        process_cpu_percent = [Math]::Round($processCpuPercent, 2)
        process_working_set_mib = [Math]::Round($totalWorkingSetBytes / 1MB, 2)
        gpu = [ordered]@{
            memory_used_mib = [int]$gpuParts[0]
            utilization_percent = [int]$gpuParts[1]
            temperature_c = [int]$gpuParts[2]
        }
        disk = [ordered]@{
            drive = $driveName
            free_mib = [Math]::Round($drive.Free / 1MB, 2)
            used_percent = if ($null -ne $diskUsedPercent) {
                [Math]::Round($diskUsedPercent, 2)
            } else { $null }
        }
    }
}

function Get-CompactCameraStatus([string]$CameraId) {
    $status = Invoke-RestMethod -Uri "$api/cameras/$CameraId/status" `
        -Headers $headers -TimeoutSec 5
    return [ordered]@{
        camera_id = $CameraId
        status = $status.status
        run_id = $status.run_id
        runtime_stale = [bool]$status.runtime_stale
        analysis = [ordered]@{
            enabled = [bool]$status.analysis.enabled
            state = $status.analysis.state
            target_infer_fps = [double]$status.analysis.target_infer_fps
            infer_fps = [double]$status.analysis.infer_fps
            last_inference_ms = [double]$status.analysis.last_inference_ms
            frame_count = [int64]$status.analysis.frame_count
            last_update_ms = [int64]$status.analysis.last_update_ms
            live_persons = [int]$status.analysis.live_persons
            in_count = [int64]$status.analysis.in_count
            out_count = [int64]$status.analysis.out_count
        }
        pipeline = [ordered]@{
            thread_running = [bool]$status.pipeline.thread_running
            inference_submit_drops = [int64]$status.pipeline.inference_submit_drops
            sampled_frames = [int64]$status.pipeline.sampled_frames
            skipped_frames = [int64]$status.pipeline.skipped_frames
        }
        extraction = [ordered]@{
            dropped_frames = [int64]$status.extraction.dropped_frames
            saved_frames = [int64]$status.extraction.saved_frames
            save_fps = [double]$status.extraction.save_fps
            writer_queue_depth = [int]$status.extraction.writer_queue_depth
        }
        hub = [ordered]@{
            camera_profile = $status.hub.camera_profile
            state = $status.hub.state
            capture_fps = [double]$status.hub.capture_fps
            source_fps = [double]$status.hub.source_fps
            latest_sequence = [int64]$status.hub.latest_sequence
            latest_frame_age_ms = [int64]$status.hub.latest_frame_age_ms
            open_count = [int]$status.hub.open_count
            reconnect_count = [int]$status.hub.reconnect_count
            width = [int]$status.hub.width
            height = [int]$status.hub.height
        }
    }
}

function Get-CompactOperations {
    $metrics = Invoke-RestMethod -Uri "$api/operations/metrics" `
        -Headers $headers -TimeoutSec 5
    return [ordered]@{
        alerts_total = [int64]$metrics.alerts.total
        inference = [ordered]@{
            active_cameras = [int]$metrics.algorithm_runtime.inference.active_cameras
            pending_cameras = [int]$metrics.algorithm_runtime.inference.pending_cameras
            processed_jobs = [int64]$metrics.algorithm_runtime.inference.processed_jobs
            submitted_jobs = [int64]$metrics.algorithm_runtime.inference.submitted_jobs
            replaced_jobs = [int64]$metrics.algorithm_runtime.inference.replaced_jobs
            failed_jobs = [int64]$metrics.algorithm_runtime.inference.failed_jobs
            stale_results = [int64]$metrics.algorithm_runtime.inference.stale_results
            workers_configured = [int]$metrics.algorithm_runtime.inference.workers_configured
            workers_ready = [int]$metrics.algorithm_runtime.inference.workers_ready
        }
        processor = [ordered]@{
            active_sessions = [int]$metrics.algorithm_runtime.processor.active_sessions
            processed_frames = [int64]$metrics.algorithm_runtime.processor.processed_frames
            failed_frames = [int64]$metrics.algorithm_runtime.processor.failed_frames
            persisted_alerts = [int64]$metrics.algorithm_runtime.processor.persisted_alerts
        }
        callbacks = [ordered]@{
            configured = [bool]$metrics.algorithm_runtime.callbacks.configured
            running = [bool]$metrics.algorithm_runtime.callbacks.running
            delivered = [int64]$metrics.algorithm_runtime.callbacks.delivered
            retries = [int64]$metrics.algorithm_runtime.callbacks.retries
            dead_letters = [int64]$metrics.algorithm_runtime.callbacks.dead_letters
        }
        invariants = $metrics.invariants
        storage = [ordered]@{
            filesystem_ok = [bool]$metrics.storage.filesystem_ok
            pressure = $metrics.storage.pressure
            free_bytes = [int64]$metrics.storage.free_bytes
        }
    }
}

$startedAt = [DateTimeOffset]::UtcNow
$allSamples = [System.Collections.Generic.List[object]]::new()
$allViolations = [System.Collections.Generic.List[string]]::new()
$failure = ""

try {
    $initialHealth = Invoke-RestMethod -Uri "$api/health" -TimeoutSec 5
    $initialReady = Invoke-RestMethod -Uri "$api/ready" -TimeoutSec 5
    if (-not $initialHealth.success -or -not $initialReady.ready) {
        throw "Service is not healthy and ready before measurement."
    }

    $baselineCameras = @($CameraIds | ForEach-Object { Get-CompactCameraStatus $_ })
    foreach ($camera in $baselineCameras) {
        if ($camera.status -ne "running" -or -not $camera.pipeline.thread_running) {
            throw "Camera $($camera.camera_id) is not running."
        }
        if (-not $camera.analysis.enabled -or
            [Math]::Abs($camera.analysis.target_infer_fps - $TargetInferFps) -gt 0.01) {
            throw "Camera $($camera.camera_id) is not configured for target $TargetInferFps FPS."
        }
    }
    $baselineOperations = Get-CompactOperations
    $deadline = $startedAt.AddMinutes($DurationMinutes)

    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        $sampleViolations = [System.Collections.Generic.List[string]]::new()
        try {
            $health = Invoke-RestMethod -Uri "$api/health" -TimeoutSec 5
            $ready = Invoke-RestMethod -Uri "$api/ready" -TimeoutSec 5
            $cameras = @($CameraIds | ForEach-Object { Get-CompactCameraStatus $_ })
            $operations = Get-CompactOperations
            $telemetry = Get-HostTelemetry

            if (-not $health.success) { $sampleViolations.Add("health_failed") }
            if (-not $ready.ready) { $sampleViolations.Add("not_ready") }
            if (-not $operations.invariants.one_hub_record_per_profile) {
                $sampleViolations.Add("hub_profile_invariant_failed")
            }
            if (-not $operations.invariants.open_count_consistent_with_reconnects) {
                $sampleViolations.Add("hub_reconnect_invariant_failed")
            }
            if (-not $operations.invariants.subscriber_count_matches_types) {
                $sampleViolations.Add("hub_subscriber_invariant_failed")
            }
            foreach ($camera in $cameras) {
                if ($camera.status -ne "running") {
                    $sampleViolations.Add("$($camera.camera_id):not_running")
                }
                if (-not $camera.pipeline.thread_running) {
                    $sampleViolations.Add("$($camera.camera_id):pipeline_stopped")
                }
                if ($camera.analysis.state -ne "running" -or $camera.runtime_stale) {
                    $sampleViolations.Add("$($camera.camera_id):analysis_unavailable")
                }
                if ($camera.hub.state -notin @("running", "reconnecting")) {
                    $sampleViolations.Add("$($camera.camera_id):hub_unavailable")
                }
            }

            $sample = [ordered]@{
                captured_at = [DateTimeOffset]::UtcNow.ToString("o")
                health_ok = [bool]$health.success
                ready = [bool]$ready.ready
                cameras = $cameras
                operations = $operations
                host_telemetry = $telemetry
                violations = @($sampleViolations)
            }
            $allSamples.Add($sample)
            $sample | ConvertTo-Json -Depth 12 -Compress |
                Add-Content -LiteralPath $samplesPath -Encoding UTF8
            foreach ($violation in $sampleViolations) {
                $allViolations.Add("$($sample.captured_at):$violation")
            }
        }
        catch {
            $message = "sample_error:$($_.Exception.Message)"
            $allViolations.Add("$([DateTimeOffset]::UtcNow.ToString("o")):$message")
            [ordered]@{
                captured_at = [DateTimeOffset]::UtcNow.ToString("o")
                error = $message
            } | ConvertTo-Json -Compress |
                Add-Content -LiteralPath $samplesPath -Encoding UTF8
        }
        Start-Sleep -Seconds $PollSeconds
    }

    $finalCameras = @($CameraIds | ForEach-Object { Get-CompactCameraStatus $_ })
    $finalOperations = Get-CompactOperations
    $completedAt = [DateTimeOffset]::UtcNow
    $elapsedSeconds = ($completedAt - $startedAt).TotalSeconds
    $cameraSummaries = @()

    foreach ($cameraId in $CameraIds) {
        $baseline = $baselineCameras | Where-Object camera_id -eq $cameraId
        $final = $finalCameras | Where-Object camera_id -eq $cameraId
        $cameraSamples = @($allSamples | ForEach-Object {
            $_.cameras | Where-Object camera_id -eq $cameraId
        })
        $latencies = [double[]]@($cameraSamples | ForEach-Object {
            $_.analysis.last_inference_ms
        })
        $observedFps = [double[]]@($cameraSamples | ForEach-Object {
            $_.analysis.infer_fps
        })
        $hubAges = [double[]]@($cameraSamples | ForEach-Object {
            $_.hub.latest_frame_age_ms
        })
        $processedFramesDelta = $final.analysis.frame_count - $baseline.analysis.frame_count
        $achievedFps = if ($elapsedSeconds -gt 0) {
            $processedFramesDelta / $elapsedSeconds
        } else { 0.0 }

        $cameraSummaries += [ordered]@{
            camera_id = $cameraId
            camera_profile = $final.hub.camera_profile
            resolution = "$($final.hub.width)x$($final.hub.height)"
            target_infer_fps = $TargetInferFps
            processed_frames_delta = $processedFramesDelta
            achieved_infer_fps = [Math]::Round($achievedFps, 3)
            target_achievement_percent = [Math]::Round(
                100.0 * $achievedFps / $TargetInferFps, 2)
            reported_infer_fps_mean = [Math]::Round(
                ($observedFps | Measure-Object -Average).Average, 3)
            inference_latency_ms = [ordered]@{
                mean = [Math]::Round(($latencies | Measure-Object -Average).Average, 3)
                p50 = [Math]::Round((Get-Percentile $latencies 0.50), 3)
                p95 = [Math]::Round((Get-Percentile $latencies 0.95), 3)
                max = [Math]::Round(($latencies | Measure-Object -Maximum).Maximum, 3)
            }
            hub_frame_age_ms = [ordered]@{
                p50 = [Math]::Round((Get-Percentile $hubAges 0.50), 3)
                p95 = [Math]::Round((Get-Percentile $hubAges 0.95), 3)
                max = [Math]::Round(($hubAges | Measure-Object -Maximum).Maximum, 3)
            }
            inference_submit_drops_delta =
                $final.pipeline.inference_submit_drops -
                $baseline.pipeline.inference_submit_drops
            extraction_drops_delta =
                $final.extraction.dropped_frames -
                $baseline.extraction.dropped_frames
            saved_frames_delta =
                $final.extraction.saved_frames -
                $baseline.extraction.saved_frames
            reconnects_delta =
                $final.hub.reconnect_count -
                $baseline.hub.reconnect_count
            source_sequence_delta =
                $final.hub.latest_sequence -
                $baseline.hub.latest_sequence
        }
    }

    $telemetrySamples = @($allSamples | ForEach-Object { $_.host_telemetry })
    $processMemory = [double[]]@($telemetrySamples | ForEach-Object {
        $_.process_working_set_mib
    })
    $summary = [ordered]@{
        started_at = $startedAt.ToString("o")
        completed_at = $completedAt.ToString("o")
        requested_duration_minutes = $DurationMinutes
        elapsed_seconds = [Math]::Round($elapsedSeconds, 3)
        poll_seconds = $PollSeconds
        sample_count = $allSamples.Count
        violation_count = $allViolations.Count
        violations = @($allViolations)
        passed_runtime_health = ($allViolations.Count -eq 0)
        target_gate_percent = 90.0
        passed_target_throughput = (@($cameraSummaries |
            Where-Object target_achievement_percent -lt 90.0).Count -eq 0)
        cameras = $cameraSummaries
        aggregate = [ordered]@{
            processed_jobs_delta =
                $finalOperations.inference.processed_jobs -
                $baselineOperations.inference.processed_jobs
            submitted_jobs_delta =
                $finalOperations.inference.submitted_jobs -
                $baselineOperations.inference.submitted_jobs
            replaced_jobs_delta =
                $finalOperations.inference.replaced_jobs -
                $baselineOperations.inference.replaced_jobs
            failed_jobs_delta =
                $finalOperations.inference.failed_jobs -
                $baselineOperations.inference.failed_jobs
            stale_results_delta =
                $finalOperations.inference.stale_results -
                $baselineOperations.inference.stale_results
            processed_frames_delta =
                $finalOperations.processor.processed_frames -
                $baselineOperations.processor.processed_frames
            failed_frames_delta =
                $finalOperations.processor.failed_frames -
                $baselineOperations.processor.failed_frames
            persisted_alerts_delta =
                $finalOperations.processor.persisted_alerts -
                $baselineOperations.processor.persisted_alerts
            alerts_delta =
                $finalOperations.alerts_total -
                $baselineOperations.alerts_total
            aggregate_inference_fps = [Math]::Round(
                ($finalOperations.inference.processed_jobs -
                    $baselineOperations.inference.processed_jobs) /
                    $elapsedSeconds, 3)
            callbacks_configured = $finalOperations.callbacks.configured
            callbacks_running = $finalOperations.callbacks.running
        }
        host_telemetry = [ordered]@{
            max_process_cpu_percent = [Math]::Round(
                ($telemetrySamples.process_cpu_percent |
                    Measure-Object -Maximum).Maximum, 2)
            process_working_set_mib_first =
                if ($processMemory.Count) { $processMemory[0] } else { 0.0 }
            process_working_set_mib_last =
                if ($processMemory.Count) { $processMemory[-1] } else { 0.0 }
            max_process_working_set_mib = [Math]::Round(
                ($processMemory | Measure-Object -Maximum).Maximum, 2)
            max_gpu_memory_used_mib = (
                $telemetrySamples.gpu.memory_used_mib |
                    Measure-Object -Maximum).Maximum
            max_gpu_utilization_percent = (
                $telemetrySamples.gpu.utilization_percent |
                    Measure-Object -Maximum).Maximum
            max_gpu_temperature_c = (
                $telemetrySamples.gpu.temperature_c |
                    Measure-Object -Maximum).Maximum
            max_disk_used_percent = if (@(
                $telemetrySamples.disk.used_percent |
                    Where-Object { $null -ne $_ }).Count -gt 0) {
                [Math]::Round(
                    ($telemetrySamples.disk.used_percent |
                        Where-Object { $null -ne $_ } |
                        Measure-Object -Maximum).Maximum, 2)
            } else { $null }
        }
        samples_file = $samplesPath
    }
    $summary | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $summaryPath -Encoding UTF8
    $summary | ConvertTo-Json -Depth 12
}
catch {
    $failure = $_.Exception.Message
    [ordered]@{
        started_at = $startedAt.ToString("o")
        completed_at = [DateTimeOffset]::UtcNow.ToString("o")
        passed_runtime_health = $false
        failure = $failure
        samples_file = $samplesPath
    } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $summaryPath -Encoding UTF8
    throw
}
