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

    $deadline = (Get-Date).AddMinutes($DurationMinutes)
    while ((Get-Date) -lt $deadline) {
        $capturedAt = (Get-Date).ToUniversalTime().ToString("o")
        try {
            $health = Invoke-RestMethod -Uri "$api/health" -TimeoutSec 4
            $ready = Invoke-RestMethod -Uri "$api/ready" -TimeoutSec 4
            $status = Invoke-RestMethod -Uri "$api/cameras/$CameraId/status" -Headers $headers -TimeoutSec 4
            $metrics = Invoke-RestMethod -Uri "$api/operations/metrics" -Headers $headers -TimeoutSec 4
            $hub = $status.hub
            $sampleViolations = [System.Collections.Generic.List[string]]::new()
            if (-not $health.success) { $sampleViolations.Add("health_failed") }
            if (-not $ready.ready) { $sampleViolations.Add("not_ready") }
            if ($status.status -eq 'failed') { $sampleViolations.Add("task_failed") }
            if (-not $metrics.invariants.one_hub_record_per_profile) { $sampleViolations.Add("multiple_hubs_per_profile") }
            if (-not $metrics.invariants.open_count_consistent_with_reconnects) { $sampleViolations.Add("open_reconnect_inconsistent") }
            if (-not $metrics.invariants.subscriber_count_matches_types) { $sampleViolations.Add("subscriber_count_inconsistent") }
            if ($hub -and $hub.open_count -gt ($hub.reconnect_count + 1)) { $sampleViolations.Add("hub_reopened_without_reconnect") }
            if ($RequirePeopleFlowSubscriber -and $hub -and [int]$hub.subscriber_types.people_flow -lt 1) {
                $sampleViolations.Add("people_flow_subscriber_missing")
            }
            foreach ($item in $sampleViolations) { $violations.Add("$capturedAt $item") }
            [ordered]@{
                captured_at = $capturedAt
                health = $health
                ready = $ready
                camera = $status
                operations = $metrics
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
        samples = $samples
        violation_count = $violations.Count
        violations = @($violations)
        passed = $violations.Count -eq 0
        samples_file = $samplesPath
    } | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8
    $token = $null
}

if ($violations.Count -ne 0) { throw "M9 soak failed with $($violations.Count) violation(s). Evidence: $summaryPath" }
Write-Host "PASS: M9 soak completed with $samples samples. Evidence: $summaryPath" -ForegroundColor Green
