[CmdletBinding()]
param(
    [string]$Root = "",
    [string]$BuildDir = ".\out\build\backend-Release",
    [Parameter(Mandatory = $true)]
    [string]$ServerConfig,
    [Parameter(Mandatory = $true)]
    [string]$WorkerConfig,
    [string]$ApiBase = "http://127.0.0.1:8087/api/v1",
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
$ApiBase = $ApiBase.TrimEnd("/")
$serverConfigPath = (Resolve-Path $ServerConfig).Path
$workerConfigPath = (Resolve-Path $WorkerConfig).Path
if (-not $EvidenceDir) {
    $stamp = [DateTimeOffset]::UtcNow.ToString("yyyyMMddTHHmmssZ")
    $EvidenceDir = Join-Path $ProjectRoot "reports\r7\rollback\$stamp"
}
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

$processPath = [Environment]::GetEnvironmentVariable("Path", "Process")
[Environment]::SetEnvironmentVariable("PATH", $null, "Process")
[Environment]::SetEnvironmentVariable("Path", $processPath, "Process")

function New-LegacyConfig([string]$Source, [string]$Destination) {
    $text = [IO.File]::ReadAllText($Source)
    if ($text -notmatch '(?m)^  unified_camera_pipeline:\s*true\s*$') {
        throw "Rollback source config is not in unified mode: $Source"
    }
    $text = $text -replace (
        '(?m)^  unified_camera_pipeline:\s*true\s*$',
        '  unified_camera_pipeline: false')
    [IO.File]::WriteAllText(
        $Destination, $text, [Text.UTF8Encoding]::new($false))
}

function Wait-Ready([string]$ExpectedMode, [bool]$ExpectLegacyRole) {
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $ready = Invoke-RestMethod -Uri "$ApiBase/ready" -TimeoutSec 4
            $worker = @($ready.workers |
                Where-Object { $_.alive }) | Select-Object -First 1
            if ($ready.ready -and
                $ready.expected_runtime_mode -eq $ExpectedMode -and
                $ready.worker_mode_consistent -and
                $ready.single_vision_worker -and
                $ready.worker_coordination_healthy -and
                $worker.runtime_mode -eq $ExpectedMode -and
                [bool]$worker.legacy_people_flow_role -eq
                    $ExpectLegacyRole) {
                return $ready
            }
        }
        catch {
        }
        Start-Sleep -Milliseconds 500
    }
    throw "Timed out waiting for runtime mode $ExpectedMode."
}

$legacyServer = Join-Path (
    Split-Path $serverConfigPath -Parent) "server.rollback-legacy.yaml"
$legacyWorker = Join-Path (
    Split-Path $workerConfigPath -Parent) "worker.rollback-legacy.yaml"
New-LegacyConfig $serverConfigPath $legacyServer
New-LegacyConfig $workerConfigPath $legacyWorker
$workerConfigText = Get-Content -LiteralPath $workerConfigPath `
    -Raw -Encoding UTF8
$leaseTtlSeconds = 30
if ($workerConfigText -match
    '(?m)^  lease_ttl_seconds:\s*(\d+)\s*$') {
    $leaseTtlSeconds = [Math]::Max(5, [int]$Matches[1])
}
$leaseWaitSeconds = $leaseTtlSeconds + 2

$legacyReady = $null
$restoredReady = $null
try {
    & (Join-Path $PSScriptRoot "stop_demo.ps1") `
        -Root $ProjectRoot | Out-Null
    Start-Sleep -Seconds $leaseWaitSeconds
    & (Join-Path $PSScriptRoot "start_demo.ps1") `
        -Root $ProjectRoot `
        -BuildDir $BuildDir `
        -ServerConfig $legacyServer `
        -WorkerConfig $legacyWorker `
        -SkipQt
    $legacyReady = Wait-Ready "legacy_split" $true

    & (Join-Path $PSScriptRoot "stop_demo.ps1") `
        -Root $ProjectRoot | Out-Null
    Start-Sleep -Seconds $leaseWaitSeconds
    & (Join-Path $PSScriptRoot "start_demo.ps1") `
        -Root $ProjectRoot `
        -BuildDir $BuildDir `
        -ServerConfig $serverConfigPath `
        -WorkerConfig $workerConfigPath `
        -SkipQt
    $restoredReady = Wait-Ready "unified_camera_pipeline" $false

    [ordered]@{
        passed = $true
        rollback_mode = $legacyReady.expected_runtime_mode
        rollback_ready = [bool]$legacyReady.ready
        process_lease_wait_seconds = $leaseWaitSeconds
        rollback_worker_generation =
            (@($legacyReady.workers |
                Where-Object { $_.alive }) |
                Select-Object -First 1).worker_generation
        restored_mode = $restoredReady.expected_runtime_mode
        restored_ready = [bool]$restoredReady.ready
        restored_worker_generation =
            (@($restoredReady.workers |
                Where-Object { $_.alive }) |
                Select-Object -First 1).worker_generation
        single_vision_worker = [bool]$restoredReady.single_vision_worker
        coordination_healthy =
            [bool]$restoredReady.worker_coordination_healthy
    } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (
            Join-Path $EvidenceDir "summary.json") -Encoding UTF8
    Write-Host (
        "PASS: unified runtime rolled back to legacy_split and restored."
    ) -ForegroundColor Green
}
catch {
    try {
        & (Join-Path $PSScriptRoot "stop_demo.ps1") `
            -Root $ProjectRoot | Out-Null
        Start-Sleep -Seconds $leaseWaitSeconds
        & (Join-Path $PSScriptRoot "start_demo.ps1") `
            -Root $ProjectRoot `
            -BuildDir $BuildDir `
            -ServerConfig $serverConfigPath `
            -WorkerConfig $workerConfigPath `
            -SkipQt
        $restoredReady = Wait-Ready "unified_camera_pipeline" $false
    }
    catch {
    }
    throw
}
