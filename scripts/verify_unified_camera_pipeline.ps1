[CmdletBinding()]
param(
    [string]$BuildDir = ".\out\build\backend-Release",
    # The fast release gate defaults to five minutes. Pass 60 explicitly when
    # a long-running stability soak is required.
    [double]$DurationMinutes = 5,
    [int]$RtspSmokeSeconds = 15,
    [int]$StressCameraCount = 3,
    [string]$LocalRtspFixture = ".\out\tmp\r7_rtsp\bus.jpg",
    [string]$MediaMtxImage = "bluenviron/mediamtx:1.18.2",
    [switch]$SkipBuild,
    [switch]$SkipPostman,
    [switch]$KeepInfrastructure
)

$ErrorActionPreference = "Stop"
if ($DurationMinutes -le 0) {
    throw "DurationMinutes must be positive."
}
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot
$stamp = [DateTimeOffset]::UtcNow.ToString("yyyyMMddTHHmmssZ")
$evidenceRoot = Join-Path $ProjectRoot "reports\r7\$stamp"
New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null

$processPath = [Environment]::GetEnvironmentVariable("Path", "Process")
[Environment]::SetEnvironmentVariable("PATH", $null, "Process")
[Environment]::SetEnvironmentVariable("Path", $processPath, "Process")

function Get-FreeTcpPort {
    $listener = [Net.Sockets.TcpListener]::new(
        [Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        return ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    }
    finally {
        $listener.Stop()
    }
}

function New-RandomSecret([int]$Bytes = 24) {
    $buffer = New-Object byte[] $Bytes
    $generator = [Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $generator.GetBytes($buffer)
    }
    finally {
        $generator.Dispose()
    }
    return [Convert]::ToBase64String($buffer).TrimEnd("=").
        Replace("+", "-").Replace("/", "_")
}

function Wait-Until(
    [scriptblock]$Probe,
    [int]$TimeoutSeconds,
    [string]$Description
) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        try {
            if (& $Probe) { return }
        }
        catch {
        }
        Start-Sleep -Milliseconds 500
    }
    throw "Timed out waiting for $Description."
}

$suffix = ([Guid]::NewGuid().ToString("N")).Substring(0, 10)
$previousDockerConfig = [Environment]::GetEnvironmentVariable(
    "DOCKER_CONFIG", "Process")
$dockerConfigRoot = [IO.Path]::GetFullPath(
    (Join-Path $ProjectRoot "out\tmp\r7_docker_$suffix"))
New-Item -ItemType Directory -Force -Path $dockerConfigRoot | Out-Null
$env:DOCKER_CONFIG = $dockerConfigRoot
$postgresContainer = "vision-r7-postgres-$suffix"
$redisContainer = "vision-r7-redis-$suffix"
$mediaContainer = "vision-r7-mediamtx-$suffix"
$postgresStarted = $false
$redisStarted = $false
$mediaStarted = $false
$publisherProcess = $null
$localRtsp = $false
$previousRtsp = [Environment]::GetEnvironmentVariable(
    "YOLO11_CAMERA_ENTRY_URL", "Process")
$testEnvironmentNames = @(
    "YOLO11_TEST_POSTGRES_DSN",
    "YOLO11_ALLOW_DESTRUCTIVE_POSTGRES_TESTS",
    "YOLO11_TEST_REDIS_HOST",
    "YOLO11_TEST_REDIS_PORT",
    "YOLO11_TEST_REDIS_DB"
)
$previousTestEnvironment = @{}
foreach ($name in $testEnvironmentNames) {
    $previousTestEnvironment[$name] =
        [Environment]::GetEnvironmentVariable($name, "Process")
}
$startedAt = [DateTimeOffset]::UtcNow
$passed = $false
$failure = ""
$hardwareSummary = $null

try {
    foreach ($command in @(
            "docker.exe", "ffmpeg.exe", "ffprobe.exe",
            "nvidia-smi.exe", "node.exe")) {
        if (-not (Get-Command $command -CommandType Application `
                -ErrorAction SilentlyContinue)) {
            throw "Required executable is missing from PATH: $command"
        }
    }

    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot "build_backend.ps1") `
            -Root $ProjectRoot -BuildDir $BuildDir
    }
    & (Join-Path $PSScriptRoot "test_all.ps1") `
        -Root $ProjectRoot -BuildDir $BuildDir

    $postgresPort = Get-FreeTcpPort
    $redisPort = Get-FreeTcpPort
    $postgresPassword = New-RandomSecret
    & docker.exe run --rm -d `
        --name $postgresContainer `
        -e "POSTGRES_PASSWORD=$postgresPassword" `
        -e "POSTGRES_USER=vision_test" `
        -e "POSTGRES_DB=vision_test" `
        -p "127.0.0.1:${postgresPort}:5432" `
        postgres:17-alpine | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to start disposable PostgreSQL."
    }
    $postgresStarted = $true
    & docker.exe run --rm -d `
        --name $redisContainer `
        -p "127.0.0.1:${redisPort}:6379" `
        redis:7-alpine | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to start disposable Redis."
    }
    $redisStarted = $true
    Wait-Until {
        & docker.exe exec $postgresContainer pg_isready `
            -U vision_test -d vision_test 2>$null | Out-Null
        return $LASTEXITCODE -eq 0
    } 60 "R7 PostgreSQL"
    Wait-Until {
        $reply = & docker.exe exec $redisContainer redis-cli ping 2>$null
        return $LASTEXITCODE -eq 0 -and $reply -eq "PONG"
    } 30 "R7 Redis"

    $env:YOLO11_TEST_POSTGRES_DSN = (
        "host=127.0.0.1 port=$postgresPort dbname=vision_test " +
        "user=vision_test password=$postgresPassword connect_timeout=5"
    )
    $env:YOLO11_ALLOW_DESTRUCTIVE_POSTGRES_TESTS = "1"
    $env:YOLO11_TEST_REDIS_HOST = "127.0.0.1"
    $env:YOLO11_TEST_REDIS_PORT = [string]$redisPort
    $env:YOLO11_TEST_REDIS_DB = "0"
    $ctest = "D:\vs2019\Common7\IDE\CommonExtensions\Microsoft" +
        "\CMake\CMake\bin\ctest.exe"
    & $ctest --test-dir (
        [IO.Path]::GetFullPath((Join-Path $ProjectRoot $BuildDir))
    ) -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "R7 PostgreSQL/Redis integration CTest failed."
    }

    foreach ($name in $testEnvironmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $previousTestEnvironment[$name], "Process")
    }
    $postgresPassword = $null

    if ([string]::IsNullOrWhiteSpace($env:YOLO11_CAMERA_ENTRY_URL)) {
        $fixturePath = [IO.Path]::GetFullPath(
            (Join-Path $ProjectRoot $LocalRtspFixture))
        if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
            throw "YOLO11_CAMERA_ENTRY_URL is not set and the local RTSP fixture is missing: $fixturePath"
        }
        & docker.exe image inspect $MediaMtxImage | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "MediaMTX image is not installed: $MediaMtxImage"
        }
        $rtspPort = Get-FreeTcpPort
        & docker.exe run --rm -d `
            --name $mediaContainer `
            -e "MTX_RTSPTRANSPORTS=tcp" `
            -p "127.0.0.1:${rtspPort}:8554" `
            $MediaMtxImage | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to start local MediaMTX fixture."
        }
        $mediaStarted = $true
        $localRtspUri = "rtsp://127.0.0.1:$rtspPort/entry"
        $publisherStdout = Join-Path $evidenceRoot "publisher.stdout.log"
        $publisherStderr = Join-Path $evidenceRoot "publisher.stderr.log"
        $publisherProcess = Start-Process `
            -FilePath (Get-Command ffmpeg.exe).Source `
            -ArgumentList @(
                "-hide_banner", "-loglevel", "warning",
                "-re", "-loop", "1", "-i", $fixturePath,
                "-vf", "scale=1280:720,format=yuv420p",
                "-c:v", "libx264", "-preset", "ultrafast",
                "-tune", "zerolatency", "-r", "25", "-g", "25",
                "-f", "rtsp", "-rtsp_transport", "tcp",
                $localRtspUri
            ) `
            -WorkingDirectory $ProjectRoot `
            -WindowStyle Hidden `
            -RedirectStandardOutput $publisherStdout `
            -RedirectStandardError $publisherStderr `
            -PassThru
        Wait-Until {
            & ffprobe.exe -v error -rtsp_transport tcp `
                -show_entries "stream=codec_name,width,height,r_frame_rate" `
                -of json $localRtspUri 2>$null | Out-Null
            return $LASTEXITCODE -eq 0
        } 30 "local RTSP fixture"
        $env:YOLO11_CAMERA_ENTRY_URL = $localRtspUri
        $localRtsp = $true
    }

    $p6Arguments = @{
        BuildDir = $BuildDir
        CameraProfile = "entry_camera_01"
        CallbackProfile = "backend_primary"
        DurationMinutes = $DurationMinutes
        RtspSmokeSeconds = $RtspSmokeSeconds
        EvidenceRoot = (
            "reports\r7\$stamp\hardware")
        UnifiedCameraPipeline = $true
        StressCameraCount = $StressCameraCount
    }
    if ($SkipPostman) {
        $p6Arguments.SkipPostman = $true
    }
    if ($KeepInfrastructure) {
        $p6Arguments.KeepInfrastructure = $true
    }
    & (Join-Path $PSScriptRoot `
        "verify_algorithm_service_p6.ps1") @p6Arguments

    $hardwareSummaryPath = Get-ChildItem -LiteralPath (
        Join-Path $evidenceRoot "hardware") -Directory |
        Where-Object {
            Test-Path -LiteralPath (
                Join-Path $_.FullName "summary.json")
        } |
        Sort-Object Name -Descending |
        Select-Object -First 1 |
        ForEach-Object { Join-Path $_.FullName "summary.json" }
    if (-not $hardwareSummaryPath -or
        -not (Test-Path -LiteralPath $hardwareSummaryPath)) {
        throw "Unified hardware acceptance summary is missing."
    }
    $hardwareSummary = Get-Content -LiteralPath $hardwareSummaryPath `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $hardwareSummary.passed -or
        $hardwareSummary.runtime_mode -ne
            "unified_camera_pipeline") {
        throw "Unified hardware acceptance did not pass."
    }
    $passed = $true
}
catch {
    $failure = $_.Exception.Message -replace (
        '(?i)rtsp[s]?://\S+', 'rtsp://***')
    throw
}
finally {
    if ($publisherProcess) {
        Stop-Process -Id $publisherProcess.Id -Force `
            -ErrorAction SilentlyContinue
        Wait-Process -Id $publisherProcess.Id -Timeout 5 `
            -ErrorAction SilentlyContinue
    }
    if ($mediaStarted -and -not $KeepInfrastructure) {
        try {
            & docker.exe rm -f $mediaContainer 2>$null | Out-Null
        }
        catch {
        }
    }
    if ($postgresStarted -and -not $KeepInfrastructure) {
        try {
            & docker.exe rm -f $postgresContainer 2>$null | Out-Null
        }
        catch {
        }
    }
    if ($redisStarted -and -not $KeepInfrastructure) {
        try {
            & docker.exe rm -f $redisContainer 2>$null | Out-Null
        }
        catch {
        }
    }
    [Environment]::SetEnvironmentVariable(
        "YOLO11_CAMERA_ENTRY_URL", $previousRtsp, "Process")
    [Environment]::SetEnvironmentVariable(
        "DOCKER_CONFIG", $previousDockerConfig, "Process")
    foreach ($name in $testEnvironmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $previousTestEnvironment[$name], "Process")
    }
    [ordered]@{
        phase = "R7"
        started_at = $startedAt.ToString("o")
        completed_at = [DateTimeOffset]::UtcNow.ToString("o")
        passed = $passed
        failure = $failure
        runtime_mode = "unified_camera_pipeline"
        local_rtsp_fixture = $localRtsp
        rtsp_uri_persisted = $false
        duration_minutes = $DurationMinutes
        full_ctest_required = $true
        postgresql_redis_integration_required = $true
        hardware_acceptance = $hardwareSummary
        disposable_infrastructure_removed =
            -not [bool]$KeepInfrastructure
    } | ConvertTo-Json -Depth 20 |
        Set-Content -LiteralPath (
            Join-Path $evidenceRoot "summary.json") -Encoding UTF8
    $resolvedTempRoot = [IO.Path]::GetFullPath(
        (Join-Path $ProjectRoot "out\tmp"))
    if ((Test-Path -LiteralPath $dockerConfigRoot) -and
        $dockerConfigRoot.StartsWith(
            $resolvedTempRoot +
                [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $dockerConfigRoot -Leaf).StartsWith(
            "r7_docker_")) {
        Remove-Item -LiteralPath $dockerConfigRoot `
            -Recurse -Force
    }
}

if (-not $passed) {
    throw "R7 unified Camera acceptance failed. Evidence: $evidenceRoot"
}
Write-Host (
    "PASS: R7 unified Camera acceptance completed. Evidence: $evidenceRoot"
) -ForegroundColor Green
