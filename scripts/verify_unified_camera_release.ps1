[CmdletBinding()]
param(
    [string]$BuildDir = ".\out\build\backend-Release",
    [double]$ObservationMinutes = 5,
    [int]$RtspSmokeSeconds = 15,
    [int]$ExpansionCameraCount = 3,
    [switch]$SkipBuild,
    [switch]$SkipPostman,
    [switch]$KeepInfrastructure
)

$ErrorActionPreference = "Stop"
if ($ObservationMinutes -le 0) {
    throw "ObservationMinutes must be positive."
}
if ($ExpansionCameraCount -lt 1 -or $ExpansionCameraCount -gt 3) {
    throw "ExpansionCameraCount must be between 1 and 3."
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot
$stamp = [DateTimeOffset]::UtcNow.ToString("yyyyMMddTHHmmssZ")
$evidenceRoot = Join-Path $ProjectRoot "reports\r8\$stamp"
$acceptanceRoot = Join-Path $evidenceRoot "acceptance"
New-Item -ItemType Directory -Force -Path $acceptanceRoot | Out-Null

$startedAt = [DateTimeOffset]::UtcNow
$passed = $false
$failure = ""
$acceptance = $null
$stress = $null
$rollback = $null
$observation = $null

function Read-Text([string]$RelativePath) {
    return Get-Content -LiteralPath (
        Join-Path $ProjectRoot $RelativePath) -Raw -Encoding UTF8
}

try {
    $serverConfig = Read-Text "config\server.yaml"
    $workerConfig = Read-Text "config\worker.yaml"
    foreach ($entry in @{
            server = $serverConfig
            worker = $workerConfig
        }.GetEnumerator()) {
        if ($entry.Value -notmatch
            '(?m)^  unified_camera_pipeline:\s*true\s*$') {
            throw "R8 $($entry.Key) config must default to unified."
        }
        if ($entry.Value -notmatch
            '(?m)^  legacy_people_flow_fallback:\s*true\s*$') {
            throw "R8 $($entry.Key) config must retain legacy fallback."
        }
    }

    foreach ($relativePath in @(
            "src\server\people_flow_inference_worker.cpp",
            "scripts\exercise_runtime_mode_rollback.ps1")) {
        if (-not (Test-Path -LiteralPath (
                    Join-Path $ProjectRoot $relativePath) -PathType Leaf)) {
            throw "R8 rollback implementation is missing: $relativePath"
        }
    }

    $acceptanceArguments = @{
        BuildDir = $BuildDir
        DurationMinutes = $ObservationMinutes
        RtspSmokeSeconds = $RtspSmokeSeconds
        StressCameraCount = $ExpansionCameraCount
        ReportRoot = $acceptanceRoot
    }
    if ($SkipBuild) {
        $acceptanceArguments.SkipBuild = $true
    }
    if ($SkipPostman) {
        $acceptanceArguments.SkipPostman = $true
    }
    if ($KeepInfrastructure) {
        $acceptanceArguments.KeepInfrastructure = $true
    }
    $acceptanceScript = Join-Path $PSScriptRoot `
        "verify_unified_camera_pipeline.ps1"
    & $acceptanceScript @acceptanceArguments

    $acceptanceSummaryPath = Get-ChildItem -LiteralPath $acceptanceRoot `
        -Directory |
        Where-Object {
            Test-Path -LiteralPath (
                Join-Path $_.FullName "summary.json")
        } |
        Sort-Object Name -Descending |
        Select-Object -First 1 |
        ForEach-Object { Join-Path $_.FullName "summary.json" }
    if (-not $acceptanceSummaryPath) {
        throw "R8 nested acceptance summary is missing."
    }
    $acceptance = Get-Content -LiteralPath $acceptanceSummaryPath `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $acceptance.passed -or
        $acceptance.runtime_mode -ne "unified_camera_pipeline") {
        throw "R8 nested unified acceptance failed."
    }

    $acceptanceDir = Split-Path $acceptanceSummaryPath -Parent
    $stressPath = Get-ChildItem -LiteralPath $acceptanceDir -Recurse `
        -Filter "summary.json" |
        Where-Object {
            $_.DirectoryName -match 'multi_camera_stress'
        } |
        Select-Object -First 1 -ExpandProperty FullName
    $rollbackPath = Get-ChildItem -LiteralPath $acceptanceDir -Recurse `
        -Filter "summary.json" |
        Where-Object {
            $_.DirectoryName -match 'runtime_rollback'
        } |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $stressPath -or -not $rollbackPath) {
        throw "R8 staged rollout or rollback evidence is missing."
    }
    $stress = Get-Content -LiteralPath $stressPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $rollback = Get-Content -LiteralPath $rollbackPath -Raw -Encoding UTF8 |
        ConvertFrom-Json

    $expectedPipelines = 1 + $ExpansionCameraCount
    if (-not $stress.passed -or
        $stress.expected_existing_subscribers -ne 1 -or
        $stress.camera_count_created -ne $ExpansionCameraCount -or
        $stress.expected_total_pipelines -ne $expectedPipelines -or
        $stress.subscriber_types.camera_pipeline -ne $expectedPipelines -or
        $stress.subscriber_types.camera_task -ne 0 -or
        $stress.subscriber_types.people_flow -ne 0 -or
        $stress.failed_jobs_delta -ne 0) {
        throw "R8 staged single-Camera to multi-Camera rollout failed."
    }
    if (-not $rollback.passed -or
        $rollback.rollback_mode -ne "legacy_split" -or
        $rollback.restored_mode -ne "unified_camera_pipeline" -or
        -not $rollback.single_vision_worker -or
        -not $rollback.coordination_healthy) {
        throw "R8 retained rollback path failed."
    }

    $soak = $acceptance.hardware_acceptance.soak
    if (-not $soak -or -not $soak.passed -or
        $soak.violation_count -ne 0 -or
        -not (Test-Path -LiteralPath $soak.samples_file)) {
        throw "R8 observation evidence is incomplete."
    }
    $samples = @(
        [System.IO.File]::ReadAllLines($soak.samples_file) |
        ForEach-Object { $_ | ConvertFrom-Json }
    )
    $sampleErrors = @(
        $samples | Where-Object {
            $null -ne $_.PSObject.Properties["sample_error"]
        }
    )
    $validSamples = @(
        $samples | Where-Object {
            $null -eq $_.PSObject.Properties["sample_error"]
        }
    )
    if ($validSamples.Count -eq 0) {
        throw "R8 observation produced no valid sample."
    }
    $readyFailures = @(
        $validSamples | Where-Object {
            -not $_.ready.ready -or
            $_.camera.status -ne "running" -or
            -not $_.camera.success
        }
    )
    $invariantFailures = @(
        $validSamples | Where-Object {
            -not $_.operations.invariants.one_hub_record_per_profile -or
            -not $_.operations.invariants.open_count_consistent_with_reconnects -or
            -not $_.operations.invariants.subscriber_count_matches_types
        }
    )
    $maxDroppedFrames = (
        $validSamples |
        ForEach-Object {
            [double]$_.camera.extraction.dropped_frames
        } |
        Measure-Object -Maximum
    ).Maximum
    $maxFailedJobs = (
        $validSamples |
        ForEach-Object {
            [double]$_.operations.algorithm_runtime.inference.failed_jobs
        } |
        Measure-Object -Maximum
    ).Maximum
    $firstSample = $validSamples[0]
    $lastSample = $validSamples[-1]
    $sampleErrorRate = if ($samples.Count -gt 0) {
        [Math]::Round($sampleErrors.Count / $samples.Count, 6)
    }
    else {
        1.0
    }
    $dataConsistent = $sampleErrors.Count -eq 0 -and
        $readyFailures.Count -eq 0 -and
        $invariantFailures.Count -eq 0 -and
        $maxFailedJobs -eq 0
    if (-not $dataConsistent) {
        throw "R8 observation detected runtime or data inconsistency."
    }

    $observation = [ordered]@{
        duration_minutes = $ObservationMinutes
        samples = $samples.Count
        valid_samples = $validSamples.Count
        sample_errors = $sampleErrors.Count
        sample_error_rate = $sampleErrorRate
        readiness_failures = $readyFailures.Count
        hub_invariant_failures = $invariantFailures.Count
        max_dropped_frames = $maxDroppedFrames
        max_inference_failed_jobs = $maxFailedJobs
        first_alert_total = $firstSample.operations.alerts.total
        last_alert_total = $lastSample.operations.alerts.total
        callback_delivered =
            $lastSample.operations.algorithm_runtime.callbacks.delivered
        callback_retries =
            $lastSample.operations.algorithm_runtime.callbacks.retries
        callback_dead_letters =
            $lastSample.operations.algorithm_runtime.callbacks.dead_letters
        host_telemetry = $soak.host_telemetry
        data_consistent = $dataConsistent
    }
    $passed = $true
}
catch {
    $failure = $_.Exception.Message -replace (
        '(?i)rtsp[s]?://\S+', 'rtsp://***')
    throw
}
finally {
    [ordered]@{
        phase = "R8"
        started_at = $startedAt.ToString("o")
        completed_at = [DateTimeOffset]::UtcNow.ToString("o")
        passed = $passed
        failure = $failure
        unified_runtime_default = $true
        legacy_implementation_retained = $true
        legacy_switch_retained = $true
        staged_rollout = if ($stress) {
            [ordered]@{
                test_environment = $true
                single_camera_trial = $true
                expansion_camera_count = $stress.camera_count_created
                total_camera_pipelines =
                    $stress.expected_total_pipelines
                one_shared_hub = $true
                aggregate_inference_fps =
                    $stress.aggregate_inference_fps
                failed_jobs_delta = $stress.failed_jobs_delta
            }
        }
        else {
            $null
        }
        observation = $observation
        clients = [ordered]@{
            qt_contract = $passed
            web_contract = $passed
            callback_acceptance = $passed
        }
        rollback = if ($rollback) {
            [ordered]@{
                passed = $rollback.passed
                rollback_mode = $rollback.rollback_mode
                restored_mode = $rollback.restored_mode
                process_lease_wait_seconds =
                    $rollback.process_lease_wait_seconds
            }
        }
        else {
            $null
        }
        rtsp_uri_persisted = $false
        acceptance = $acceptance
    } | ConvertTo-Json -Depth 24 |
        Set-Content -LiteralPath (
            Join-Path $evidenceRoot "summary.json") -Encoding UTF8
}

if (-not $passed) {
    throw "R8 unified Camera release failed. Evidence: $evidenceRoot"
}
Write-Host (
    "PASS: R8 unified Camera release completed. Evidence: $evidenceRoot"
) -ForegroundColor Green
