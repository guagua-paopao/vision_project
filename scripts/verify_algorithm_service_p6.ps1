[CmdletBinding()]
param(
    [string]$BuildDir = ".\out\build\backend-Release",
    [string]$CameraProfile = "entry_camera_01",
    [string]$CallbackProfile = "backend_primary",
    [double]$DurationMinutes = 60,
    [int]$RtspSmokeSeconds = 15,
    [int]$AlertWaitSeconds = 180,
    [string]$PostmanDesktopPath = "",
    [string]$PostmanRunner = "npx.cmd",
    [switch]$SkipPostman,
    [switch]$SkipLiveAlert,
    [switch]$SkipWorkerRestart,
    [switch]$SkipRtspReconnect,
    [switch]$SkipDeadLetterReplay,
    [switch]$SkipSoak,
    [switch]$KeepInfrastructure
)

$ErrorActionPreference = "Stop"
if ($DurationMinutes -le 0) {
    throw "DurationMinutes must be positive."
}
if ($RtspSmokeSeconds -lt 5) {
    throw "RtspSmokeSeconds must be at least 5."
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot
$rtspUri = $env:YOLO11_CAMERA_ENTRY_URL
if ([string]::IsNullOrWhiteSpace($rtspUri)) {
    throw "Set YOLO11_CAMERA_ENTRY_URL in the current process. It is never written to evidence."
}
if (-not ($rtspUri.StartsWith("rtsp://", [StringComparison]::OrdinalIgnoreCase) -or
          $rtspUri.StartsWith("rtsps://", [StringComparison]::OrdinalIgnoreCase))) {
    throw "YOLO11_CAMERA_ENTRY_URL must use rtsp:// or rtsps://."
}

$stamp = [DateTimeOffset]::UtcNow.ToString("yyyyMMddTHHmmssZ")
$evidenceDir = Join-Path $ProjectRoot "reports\p6\$stamp"
$tempRoot = [IO.Path]::GetFullPath((Join-Path $ProjectRoot "out\tmp"))
$tempDir = Join-Path $tempRoot "p6_$stamp"
New-Item -ItemType Directory -Force -Path $evidenceDir, $tempDir | Out-Null

function New-RandomSecret([int]$Bytes = 32) {
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

$postgresPassword = New-RandomSecret
$adminToken = New-RandomSecret
$callbackSecret = New-RandomSecret
$mockControlToken = New-RandomSecret
$postgresPort = Get-FreeTcpPort
$redisPort = Get-FreeTcpPort
$mockPort = Get-FreeTcpPort
$suffix = ([Guid]::NewGuid().ToString("N")).Substring(0, 10)
$postgresContainer = "vision-p6-postgres-$suffix"
$redisContainer = "vision-p6-redis-$suffix"
$mockProcess = $null
$peopleFlowSessionId = ""
$managedEnvironmentNames = @(
    "YOLO11_POSTGRES_DSN",
    "YOLO11_CAMERA_TASK_ADMIN_TOKEN",
    "YOLO11_CALLBACK_BACKEND_PRIMARY_URL",
    "YOLO11_CALLBACK_BACKEND_PRIMARY_SECRET",
    "YOLO11_MOCK_CALLBACK_SECRET",
    "YOLO11_MOCK_CALLBACK_CONTROL_TOKEN",
    "YOLO11_MOCK_CALLBACK_PORT",
    "YOLO11_MOCK_CALLBACK_FAIL_FIRST"
)
$previousEnvironment = @{}
foreach ($name in $managedEnvironmentNames) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable(
        $name, "Process")
}
$sensitiveValues = @(
    $rtspUri,
    $postgresPassword,
    $adminToken,
    $callbackSecret,
    $mockControlToken
)

function Protect-Text([string]$Text) {
    $protected = if ($null -eq $Text) { "" } else { $Text }
    foreach ($value in $sensitiveValues) {
        if (-not [string]::IsNullOrEmpty($value)) {
            $protected = $protected.Replace($value, "***")
        }
    }
    return $protected -replace '(?i)rtsp[s]?://[^\s"''\r\n]+', 'rtsp://***'
}

function Write-ProtectedEvidence([string]$Name, [string]$Text) {
    $path = Join-Path $evidenceDir $Name
    [IO.File]::WriteAllText(
        $path,
        (Protect-Text $Text),
        [Text.UTF8Encoding]::new($false))
    return $path
}

function New-AcceptanceConfig(
    [string]$Source,
    [string]$Destination,
    [int]$RuntimeRedisPort
) {
    $lines = [IO.File]::ReadAllLines($Source)
    $section = ""
    $callbacksEnabled = $false
    $loopbackAllowed = $false
    $redisPortChanged = $false
    $maxAttemptsChanged = $false
    $initialBackoffChanged = $false
    $maxBackoffChanged = $false
    for ($index = 0; $index -lt $lines.Length; ++$index) {
        $line = $lines[$index]
        if ($line -match '^([a-z][a-z0-9_]*):\s*$') {
            $section = $Matches[1]
        }
        if ($section -eq "callbacks" -and
            $line -match '^  enabled:\s*false\s*$') {
            $lines[$index] = "  enabled: true"
            $callbacksEnabled = $true
            continue
        }
        if ($section -eq "callbacks" -and
            $line -match '^      allow_insecure_http:\s*false\s*$') {
            $lines[$index] = "      allow_insecure_http: true"
            $loopbackAllowed = $true
            continue
        }
        if ($section -eq "callbacks" -and
            $line -match '^  max_attempts:\s*\d+\s*$') {
            $lines[$index] = "  max_attempts: 2"
            $maxAttemptsChanged = $true
            continue
        }
        if ($section -eq "callbacks" -and
            $line -match '^  initial_backoff_ms:\s*\d+\s*$') {
            $lines[$index] = "  initial_backoff_ms: 250"
            $initialBackoffChanged = $true
            continue
        }
        if ($section -eq "callbacks" -and
            $line -match '^  max_backoff_ms:\s*\d+\s*$') {
            $lines[$index] = "  max_backoff_ms: 1000"
            $maxBackoffChanged = $true
            continue
        }
        if ($section -eq "redis" -and
            $line -match '^  port:\s*\d+\s*$') {
            $lines[$index] = "  port: $RuntimeRedisPort"
            $redisPortChanged = $true
        }
    }
    if (-not $callbacksEnabled -or -not $loopbackAllowed -or
        -not $redisPortChanged -or -not $maxAttemptsChanged -or
        -not $initialBackoffChanged -or -not $maxBackoffChanged) {
        throw "Acceptance config transform did not match the expected secure defaults."
    }
    [IO.File]::WriteAllLines(
        $Destination, $lines, [Text.UTF8Encoding]::new($false))
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

function Invoke-RecordedExecutable(
    [string]$EvidenceName,
    [string]$Executable,
    [string[]]$Arguments
) {
    $output = & $Executable @Arguments 2>&1 | Out-String
    $code = $LASTEXITCODE
    Write-ProtectedEvidence $EvidenceName $output | Out-Null
    Write-Host (Protect-Text $output).Trim()
    if ($code -ne 0) {
        throw "$Executable failed with exit code $code."
    }
}

$passed = $false
$failure = ""
$startedAt = [DateTimeOffset]::UtcNow
$postmanDesktopResolved = ""
$postmanDesktopVersion = ""
$soakSummary = $null

try {
    foreach ($command in @("docker.exe", "node.exe", "ffmpeg.exe",
            "nvidia-smi.exe")) {
        if (-not (Get-Command $command -CommandType Application `
                -ErrorAction SilentlyContinue)) {
            throw "Required executable is missing from PATH: $command"
        }
    }
    $buildPath = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $BuildDir))
    foreach ($name in @(
            "four_stage_server.exe",
            "four_stage_worker.exe",
            "pose_engine_smoke.exe",
            "rtsp_capture_smoke.exe",
            "pose_rtsp_interop_smoke.exe"
        )) {
        if (-not (Test-Path -LiteralPath (Join-Path $buildPath $name))) {
            throw "Build output missing: $name"
        }
    }

    if (-not $PostmanDesktopPath) {
        $PostmanDesktopPath = Join-Path $env:LOCALAPPDATA "Postman\Postman.exe"
    }
    if (Test-Path -LiteralPath $PostmanDesktopPath -PathType Leaf) {
        $postmanDesktopResolved = (Resolve-Path $PostmanDesktopPath).Path
        $postmanDesktopVersion = (
            Get-Item -LiteralPath $postmanDesktopResolved
        ).VersionInfo.ProductVersion
    }
    elseif (-not $SkipPostman) {
        throw "Postman Desktop was not found at $PostmanDesktopPath."
    }

    $enginePath = Join-Path $ProjectRoot "engines\yolo11n-pose.engine"
    $expectedHash = (
        (Get-Content -LiteralPath "engines\SHA256SUMS.txt" -Raw).Trim() `
            -split '\s+'
    )[0].ToLowerInvariant()
    $actualHash = (Get-FileHash -LiteralPath $enginePath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($expectedHash -ne $actualHash) {
        throw "TensorRT engine SHA-256 mismatch."
    }

    $runtimePath = @(
        "D:\GPU13.3\bin",
        "D:\TensorRT-10.16.1.11\lib",
        "D:\libs\opencv\build\x64\vc16\bin"
    ) -join ";"
    $env:PATH = "$runtimePath;$env:PATH"
    $gpuInventory = & nvidia-smi.exe `
        --query-gpu=name,driver_version,memory.total `
        --format=csv,noheader 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "nvidia-smi inventory failed."
    }
    Write-ProtectedEvidence "gpu_inventory.txt" $gpuInventory | Out-Null

    Invoke-RecordedExecutable "pose_engine_smoke.txt" `
        (Join-Path $buildPath "pose_engine_smoke.exe") @($enginePath)
    Invoke-RecordedExecutable "rtsp_capture_smoke.txt" `
        (Join-Path $buildPath "rtsp_capture_smoke.exe") @(
            (Join-Path $ProjectRoot "config\worker.yaml"),
            [string]$RtspSmokeSeconds
        )
    Invoke-RecordedExecutable "pose_rtsp_interop_smoke.txt" `
        (Join-Path $buildPath "pose_rtsp_interop_smoke.exe") @(
            (Join-Path $ProjectRoot "config\worker.yaml"),
            [string]$RtspSmokeSeconds
        )

    $postgresId = & docker.exe run --rm -d `
        --name $postgresContainer `
        -e "POSTGRES_PASSWORD=$postgresPassword" `
        -e "POSTGRES_USER=vision_app" `
        -e "POSTGRES_DB=vision_project" `
        -p "127.0.0.1:${postgresPort}:5432" `
        postgres:17-alpine
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to start disposable PostgreSQL."
    }
    $redisId = & docker.exe run --rm -d `
        --name $redisContainer `
        -p "127.0.0.1:${redisPort}:6379" `
        redis:7-alpine
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to start disposable Redis."
    }
    Wait-Until {
        & docker.exe exec $postgresContainer pg_isready `
            -U vision_app -d vision_project 2>$null | Out-Null
        return $LASTEXITCODE -eq 0
    } 60 "PostgreSQL"
    Wait-Until {
        $reply = & docker.exe exec $redisContainer redis-cli ping 2>$null
        return $LASTEXITCODE -eq 0 -and $reply -eq "PONG"
    } 30 "Redis"

    $env:YOLO11_POSTGRES_DSN = (
        "host=127.0.0.1 port=$postgresPort dbname=vision_project " +
        "user=vision_app password=$postgresPassword sslmode=disable"
    )
    $env:YOLO11_CAMERA_TASK_ADMIN_TOKEN = $adminToken
    $env:YOLO11_CALLBACK_BACKEND_PRIMARY_URL = (
        "http://127.0.0.1:$mockPort/api/v1/algorithm-alerts"
    )
    $env:YOLO11_CALLBACK_BACKEND_PRIMARY_SECRET = $callbackSecret
    $env:YOLO11_MOCK_CALLBACK_SECRET = $callbackSecret
    $env:YOLO11_MOCK_CALLBACK_CONTROL_TOKEN = $mockControlToken
    $env:YOLO11_MOCK_CALLBACK_PORT = [string]$mockPort
    $env:YOLO11_MOCK_CALLBACK_FAIL_FIRST = "1"

    $serverConfig = Join-Path $tempDir "server.yaml"
    $workerConfig = Join-Path $tempDir "worker.yaml"
    New-AcceptanceConfig `
        (Join-Path $ProjectRoot "config\server.yaml") `
        $serverConfig $redisPort
    New-AcceptanceConfig `
        (Join-Path $ProjectRoot "config\worker.yaml") `
        $workerConfig $redisPort

    $mockStdout = Join-Path $evidenceDir "mock.stdout.log"
    $mockStderr = Join-Path $evidenceDir "mock.stderr.log"
    $mockProcess = Start-Process -FilePath "node.exe" `
        -ArgumentList @(
            (Join-Path $ProjectRoot "scripts\mock_callback_backend.js")
        ) -WorkingDirectory $ProjectRoot -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $mockStdout `
        -RedirectStandardError $mockStderr
    Wait-Until {
        $health = Invoke-RestMethod `
            -Uri "http://127.0.0.1:$mockPort/health" -TimeoutSec 2
        return [bool]$health.success
    } 20 "mock callback backend"

    & (Join-Path $PSScriptRoot "start_demo.ps1") `
        -Root $ProjectRoot `
        -BuildDir $BuildDir `
        -ServerConfig $serverConfig `
        -WorkerConfig $workerConfig `
        -SkipQt

    if (-not $SkipWorkerRestart) {
        try {
            $restartOutput = & (Join-Path $PSScriptRoot `
                "exercise_worker_restart.ps1") `
                -Root $ProjectRoot `
                -BuildDir $BuildDir `
                -ApiBase "http://127.0.0.1:8087/api/v1" `
                -CameraProfile $CameraProfile `
                -WaitSeconds 120 `
                -EvidenceDir (Join-Path $evidenceDir "worker_restart") `
                2>&1 | Out-String
        }
        catch {
            $restartOutput = ($_ | Out-String)
            Write-ProtectedEvidence "worker_restart.txt" `
                $restartOutput | Out-Null
            throw
        }
        Write-ProtectedEvidence "worker_restart.txt" `
            $restartOutput | Out-Null
        Write-Host (Protect-Text $restartOutput).Trim()
    }

    $peopleFlowHeaders = @{
        Authorization = "Bearer $adminToken"
    }
    $peopleFlowStart = Invoke-RestMethod -Method Post `
        -Uri "http://127.0.0.1:8087/api/v1/people-flow/start" `
        -Headers $peopleFlowHeaders `
        -ContentType "application/json" `
        -Body "{}" `
        -TimeoutSec 10
    $peopleFlowSessionId = [string]$peopleFlowStart.session_id
    if ([string]::IsNullOrWhiteSpace($peopleFlowSessionId)) {
        throw "People Flow start did not return a session ID."
    }
    Wait-Until {
        $session = Invoke-RestMethod -Uri (
            "http://127.0.0.1:8087/api/v1/people-flow/" +
            $peopleFlowSessionId + "/status"
        ) -TimeoutSec 4
        if ($session.status -eq "failed") {
            throw "People Flow session failed."
        }
        return $session.status -eq "running" -and
            [bool]$session.capture.shared_hub
    } 90 "People Flow shared-hub session"

    if (-not $SkipRtspReconnect) {
        try {
            $reconnectOutput = & (Join-Path $PSScriptRoot `
                "exercise_rtsp_reconnect.ps1") `
                -Root $ProjectRoot `
                -ApiBase "http://127.0.0.1:8087/api/v1" `
                -CameraProfile $CameraProfile `
                -WaitSeconds 120 `
                -EvidenceDir (Join-Path $evidenceDir "rtsp_reconnect") `
                2>&1 | Out-String
        }
        catch {
            $reconnectOutput = ($_ | Out-String)
            Write-ProtectedEvidence "rtsp_reconnect.txt" `
                $reconnectOutput | Out-Null
            throw
        }
        Write-ProtectedEvidence "rtsp_reconnect.txt" `
            $reconnectOutput | Out-Null
        Write-Host (Protect-Text $reconnectOutput).Trim()
    }

    $p5Arguments = @{
        ApiBase = "http://127.0.0.1:8087/api/v1"
        MockCallbackBase = "http://127.0.0.1:$mockPort"
        CameraProfile = $CameraProfile
        CallbackProfile = $CallbackProfile
        WaitForAlertSeconds = $AlertWaitSeconds
    }
    if ($SkipLiveAlert) {
        $p5Arguments.ControlPlaneOnly = $true
    }
    try {
        $p5Output = & (Join-Path $PSScriptRoot `
            "verify_algorithm_service_p5.ps1") @p5Arguments 2>&1 |
            Out-String
    }
    catch {
        $p5Output = ($_ | Out-String)
        Write-ProtectedEvidence "live_chain.txt" $p5Output | Out-Null
        throw
    }
    Write-ProtectedEvidence "live_chain.txt" $p5Output | Out-Null
    Write-Host (Protect-Text $p5Output).Trim()

    if (-not $SkipDeadLetterReplay) {
        try {
            $deadLetterOutput = & (Join-Path $PSScriptRoot `
                "exercise_dead_letter_replay.ps1") `
                -ApiBase "http://127.0.0.1:8087/api/v1" `
                -MockCallbackBase "http://127.0.0.1:$mockPort" `
                -CameraProfile $CameraProfile `
                -CallbackProfile $CallbackProfile `
                -WaitSeconds $AlertWaitSeconds `
                -EvidenceDir (Join-Path $evidenceDir "dead_letter") `
                2>&1 | Out-String
        }
        catch {
            $deadLetterOutput = ($_ | Out-String)
            Write-ProtectedEvidence "dead_letter_replay.txt" `
                $deadLetterOutput | Out-Null
            throw
        }
        Write-ProtectedEvidence "dead_letter_replay.txt" `
            $deadLetterOutput | Out-Null
        Write-Host (Protect-Text $deadLetterOutput).Trim()
    }

    if (-not $SkipPostman) {
        $runner = Get-Command $PostmanRunner -CommandType Application `
            -ErrorAction SilentlyContinue
        if (-not $runner) {
            throw "Postman collection runner is unavailable: $PostmanRunner"
        }
        $postmanCameraId = "postman_p6_$suffix"
        $runnerArguments = @()
        if ($runner.Name -match '^npx(\.cmd)?$') {
            $runnerArguments += @("--yes", "newman")
        }
        $runnerArguments += @(
            "run",
            (Join-Path $ProjectRoot `
                "postman\vision_project_p5.postman_collection.json"),
            "-e",
            (Join-Path $ProjectRoot `
                "postman\vision_project_p5.local.postman_environment.json"),
            "--env-var", "base_url=http://127.0.0.1:8087/api/v1",
            "--env-var", "admin_token=$adminToken",
            "--env-var", "camera_id=$postmanCameraId",
            "--env-var", "camera_profile=$CameraProfile",
            "--env-var", "callback_profile=$CallbackProfile",
            "--env-var", "mock_callback_url=http://127.0.0.1:$mockPort",
            "--env-var", "mock_control_token=$mockControlToken",
            "--color", "off",
            "--disable-unicode"
        )
        $postmanOutput = & $runner.Source @runnerArguments 2>&1 | Out-String
        $postmanExit = $LASTEXITCODE
        Write-ProtectedEvidence "postman_collection.txt" `
            $postmanOutput | Out-Null
        Write-Host (Protect-Text $postmanOutput).Trim()
        if ($postmanExit -ne 0) {
            throw "Postman collection failed with exit code $postmanExit."
        }
    }

    if (-not $SkipSoak) {
        try {
            $soakOutput = & (Join-Path $PSScriptRoot `
                "soak_camera_frame_feature.ps1") `
                -BaseUrl "http://127.0.0.1:8087" `
                -CreateEphemeralCamera `
                -CameraProfile $CameraProfile `
                -DurationMinutes $DurationMinutes `
                -PollSeconds 5 `
                -RequirePeopleFlowSubscriber `
                -CaptureHostTelemetry `
                -EvidenceRoot "reports\p6\soak" 2>&1 | Out-String
        }
        catch {
            $soakOutput = ($_ | Out-String)
            Write-ProtectedEvidence "soak.txt" $soakOutput | Out-Null
            throw
        }
        Write-ProtectedEvidence "soak.txt" $soakOutput | Out-Null
        Write-Host (Protect-Text $soakOutput).Trim()
        $latestSoak = Get-ChildItem -LiteralPath (
            Join-Path $ProjectRoot "reports\p6\soak"
        ) -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($latestSoak) {
            $soakSummaryPath = Join-Path $latestSoak.FullName "summary.json"
            if (Test-Path -LiteralPath $soakSummaryPath) {
                $soakSummary = Get-Content -LiteralPath $soakSummaryPath `
                    -Raw -Encoding UTF8 | ConvertFrom-Json
            }
        }
    }

    $passed = $true
}
catch {
    $failure = Protect-Text $_.Exception.Message
    throw
}
finally {
    if ($peopleFlowSessionId) {
        try {
            Invoke-RestMethod -Method Post -Uri (
                "http://127.0.0.1:8087/api/v1/people-flow/" +
                $peopleFlowSessionId + "/stop"
            ) -Headers @{ Authorization = "Bearer $adminToken" } `
                -TimeoutSec 5 | Out-Null
        }
        catch {
        }
    }
    try {
        & (Join-Path $PSScriptRoot "stop_demo.ps1") `
            -Root $ProjectRoot | Out-Null
    }
    catch {
    }
    if ($mockProcess) {
        Stop-Process -Id $mockProcess.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $mockProcess.Id -Timeout 5 `
            -ErrorAction SilentlyContinue
    }
    if (-not $KeepInfrastructure) {
        foreach ($container in @($postgresContainer, $redisContainer)) {
            & docker.exe rm -f $container 2>$null | Out-Null
        }
    }
    foreach ($name in $managedEnvironmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $previousEnvironment[$name], "Process")
    }
    $protectedFiles = @(
        Get-ChildItem -LiteralPath $evidenceDir -File -Recurse `
            -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName
    ) + @(
            (Join-Path $ProjectRoot "runtime\logs\process\server.stdout.log"),
            (Join-Path $ProjectRoot "runtime\logs\process\server.stderr.log"),
            (Join-Path $ProjectRoot "runtime\logs\process\worker.stdout.log"),
            (Join-Path $ProjectRoot "runtime\logs\process\worker.stderr.log")
        )
    foreach ($path in $protectedFiles) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $content = [IO.File]::ReadAllText($path)
            [IO.File]::WriteAllText(
                $path,
                (Protect-Text $content),
                [Text.UTF8Encoding]::new($false))
        }
    }
    $summary = [ordered]@{
        phase = "P6"
        started_at = $startedAt.ToString("o")
        completed_at = [DateTimeOffset]::UtcNow.ToString("o")
        passed = $passed
        failure = $failure
        camera_profile = $CameraProfile
        rtsp_uri_persisted = $false
        engine_sha256 = $actualHash
        postman_desktop_detected =
            -not [string]::IsNullOrWhiteSpace($postmanDesktopResolved)
        postman_desktop_executable = if ($postmanDesktopResolved) {
            Split-Path $postmanDesktopResolved -Leaf
        }
        else {
            ""
        }
        postman_desktop_version = $postmanDesktopVersion
        postman_collection_executed = -not [bool]$SkipPostman
        live_alert_required = -not [bool]$SkipLiveAlert
        worker_restart_required = -not [bool]$SkipWorkerRestart
        rtsp_reconnect_required = -not [bool]$SkipRtspReconnect
        dead_letter_replay_required = -not [bool]$SkipDeadLetterReplay
        soak_required = -not [bool]$SkipSoak
        soak = $soakSummary
        disposable_infrastructure_removed = -not [bool]$KeepInfrastructure
    }
    $summary | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath (Join-Path $evidenceDir "summary.json") `
            -Encoding UTF8

    $resolvedTemp = [IO.Path]::GetFullPath($tempDir)
    if ((Test-Path -LiteralPath $resolvedTemp) -and
        $resolvedTemp.StartsWith(
            $tempRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $resolvedTemp -Leaf).StartsWith("p6_")) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
    $postgresPassword = $null
    $adminToken = $null
    $callbackSecret = $null
    $mockControlToken = $null
    $rtspUri = $null
}

if (-not $passed) {
    throw "P6 acceptance failed. Evidence: $evidenceDir"
}
Write-Host "PASS: P6 hardware acceptance completed. Evidence: $evidenceDir" `
    -ForegroundColor Green
