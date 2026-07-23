[CmdletBinding()]
param(
    [string]$Root = "",
    [string]$BuildDir = ".\out\build\backend-Release",
    [string]$QtBuildDir = ".\out\build\qt-client-Release",
    [string]$CudaRoot = "D:\GPU13.3",
    [string]$TensorRtRoot = "D:\TensorRT-10.16.1.11",
    [string]$OpenCvBin = "D:\libs\opencv\build\x64\vc16\bin",
    [int]$ReadyTimeoutSeconds = 40,
    [switch]$SkipQt
)

$ErrorActionPreference = "Stop"
$ProjectRoot = if ($Root) { (Resolve-Path $Root).Path } else { (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
Set-Location $ProjectRoot

function Read-SecretText([string]$Prompt) {
    $secure = Read-Host $Prompt -AsSecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
}

if (-not $env:YOLO11_CAMERA_ENTRY_URL) {
    $env:YOLO11_CAMERA_ENTRY_URL = Read-SecretText "Enter RTSP URI (input hidden)"
}
if (-not ($env:YOLO11_CAMERA_ENTRY_URL.StartsWith("rtsp://", [StringComparison]::OrdinalIgnoreCase) -or
          $env:YOLO11_CAMERA_ENTRY_URL.StartsWith("rtsps://", [StringComparison]::OrdinalIgnoreCase))) {
    throw "Camera URI must start with rtsp:// or rtsps://"
}
if (-not $env:YOLO11_CAMERA_TASK_ADMIN_TOKEN) {
    $env:YOLO11_CAMERA_TASK_ADMIN_TOKEN = Read-SecretText "Enter Camera Task admin token (input hidden)"
}
if ([string]::IsNullOrWhiteSpace($env:YOLO11_CAMERA_TASK_ADMIN_TOKEN)) {
    throw "Camera Task admin token must not be empty"
}
if (-not $env:YOLO11_POSTGRES_DSN) {
    $env:YOLO11_POSTGRES_DSN = Read-SecretText "Enter PostgreSQL DSN (input hidden)"
}
if ([string]::IsNullOrWhiteSpace($env:YOLO11_POSTGRES_DSN)) {
    throw "PostgreSQL DSN must not be empty"
}

& (Join-Path $PSScriptRoot "stop_demo.ps1") -Root $ProjectRoot
$canonicalPath = $env:Path
[Environment]::SetEnvironmentVariable("PATH", $null, "Process")
[Environment]::SetEnvironmentVariable("Path", $canonicalPath, "Process")
$env:Path = "$(Join-Path $CudaRoot 'bin');$(Join-Path $TensorRtRoot 'lib');$OpenCvBin;$env:Path"

$ffmpegCommand = Get-Command ffmpeg.exe -CommandType Application -ErrorAction SilentlyContinue
if (-not $ffmpegCommand) {
    throw "System ffmpeg.exe is required for Windows Camera FrameHub capture and must be available on PATH."
}

$BackendPath = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $BuildDir))
$serverExe = Join-Path $BackendPath "four_stage_server.exe"
$workerExe = Join-Path $BackendPath "four_stage_worker.exe"
foreach ($exe in @($serverExe, $workerExe)) {
    if (-not (Test-Path -LiteralPath $exe)) { throw "Build output missing: $exe" }
}

$pidDir = Join-Path $ProjectRoot "runtime\pids"
$logDir = Join-Path $ProjectRoot "runtime\logs\process"
New-Item -ItemType Directory -Force -Path $pidDir, $logDir,
    (Join-Path $ProjectRoot "runtime\output\people_flow"),
    (Join-Path $ProjectRoot "runtime\output\camera_frames"),
    (Join-Path $ProjectRoot "runtime\data") | Out-Null

function Start-LoggedProcess([string]$Name, [string]$Exe, [string[]]$Arguments) {
    $stdout = Join-Path $logDir "$Name.stdout.log"
    $stderr = Join-Path $logDir "$Name.stderr.log"
    $process = Start-Process -FilePath $Exe -ArgumentList $Arguments -WorkingDirectory $ProjectRoot `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -WindowStyle Hidden
    return [ordered]@{ name=$Name; pid=$process.Id; stdout=$stdout; stderr=$stderr }
}

$processes = @()
$processes += Start-LoggedProcess "worker" $workerExe @("config\worker.yaml", "--consumer-name", "people_flow_worker_1")
Start-Sleep -Seconds 2
$processes += Start-LoggedProcess "server" $serverExe @("config\server.yaml")

$readyUrl = "http://127.0.0.1:8087/api/v1/ready"
$deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
$ready = $false
while ((Get-Date) -lt $deadline) {
    try {
        $payload = Invoke-RestMethod -Uri $readyUrl -TimeoutSec 2
        if ($payload.ready) { $ready = $true; break }
    } catch {}
    Start-Sleep -Milliseconds 300
}
if (-not $ready) {
    $processes | ForEach-Object {
        Stop-Process -Id ([int]$_.pid) -Force -ErrorAction SilentlyContinue
        Wait-Process -Id ([int]$_.pid) -Timeout 5 -ErrorAction SilentlyContinue
    }
    throw "Backend did not become ready. Inspect runtime\logs\process."
}

if (-not $SkipQt) {
    $qtExe = Join-Path ([IO.Path]::GetFullPath((Join-Path $ProjectRoot $QtBuildDir))) "people_flow_qt_client.exe"
    if (-not (Test-Path -LiteralPath $qtExe)) { throw "Qt client missing: $qtExe" }
    $qt = Start-Process -FilePath $qtExe -ArgumentList @("--base-url", "http://127.0.0.1:8087") `
        -WorkingDirectory (Split-Path $qtExe) -PassThru
    $processes += [ordered]@{ name="qt"; pid=$qt.Id }
}

$pidFile = Join-Path $pidDir "demo.json"
[ordered]@{ started_at=(Get-Date).ToString("s"); processes=$processes } |
    ConvertTo-Json -Depth 6 | Set-Content -Path $pidFile -Encoding UTF8
Write-Host "PASS: PostgreSQL + Redis -> TensorRT worker -> HTTP -> Qt demo is ready." -ForegroundColor Green
Write-Host "In Qt click Check Service, then Start Session."
Write-Host "Camera administration: http://127.0.0.1:8087/camera-admin"
