[CmdletBinding()]
param(
    [string]$Root = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$ProjectRoot = if ($Root) {
    (Resolve-Path $Root).Path
} else {
    (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build_backend.ps1") -Root $ProjectRoot
    if ($LASTEXITCODE -ne 0) { throw "Camera Frame backend release tests failed" }
} else {
    & (Join-Path $PSScriptRoot "test_all.ps1") -Root $ProjectRoot
    if ($LASTEXITCODE -ne 0) { throw "Camera Frame existing-build tests failed" }
}

if (-not $SkipBuild) {
    python (Join-Path $ProjectRoot "tools\qt_demo_contract_test.py")
    if ($LASTEXITCODE -ne 0) { throw "Qt API compatibility contract failed" }
}

$cmake = Get-Content -LiteralPath (Join-Path $ProjectRoot "CMakeLists.txt") -Raw -Encoding UTF8
if ($cmake -match '(?im)add_executable\s*\(\s*camera_frame_worker') {
    throw "Architecture violation: a camera_frame_worker target was added"
}

$forbiddenFile = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "src"),
    (Join-Path $ProjectRoot "include"), (Join-Path $ProjectRoot "scripts") -Recurse -File |
    Where-Object { $_.Name -match 'camera_frame_worker' }
if ($forbiddenFile) {
    throw "Architecture violation: camera_frame_worker source/script exists: $($forbiddenFile.FullName)"
}

foreach ($configuration in @("config\server.yaml", "config\worker.yaml")) {
    $path = Join-Path $ProjectRoot $configuration
    $text = Get-Content -LiteralPath $path -Raw -Encoding UTF8
    if ($text -notmatch '(?im)^\s*require_ffmpeg_backend:\s*true\s*$' -or
        $text -notmatch '(?im)^\s*allow_backend_fallback:\s*false\s*$') {
        throw "FFmpeg-only capture invariant is missing from $configuration"
    }
    if ($text -notmatch '(?im)^\s*admin_token_env:\s*[\"'']YOLO11_CAMERA_TASK_ADMIN_TOKEN[\"'']\s*$') {
        throw "Camera Task token must be referenced by environment-variable name in $configuration"
    }
    if ($text -notmatch '(?im)^\s*worker_num:\s*1\s*$') {
        throw "Single WorkerHost invariant is missing from $configuration"
    }
    foreach ($storageGuard in @('max_archive_bytes', 'min_free_bytes',
        'high_watermark_percent', 'critical_watermark_percent')) {
        if ($text -notmatch "(?im)^\s*$storageGuard\s*:\s*[0-9]+\s*$") {
            throw "M10 storage guard '$storageGuard' is missing from $configuration"
        }
    }
}

foreach ($yaml in Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "config") -Filter "*.yaml" -File) {
    $activeYaml = (Get-Content -LiteralPath $yaml.FullName -Encoding UTF8 |
        Where-Object { -not $_.TrimStart().StartsWith("#") }) -join "`n"
    if ($activeYaml -match '(?i)rtsps?://') {
        throw "Secret policy violation: literal RTSP URI found in $($yaml.Name)"
    }
}

$cameraSources = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "src\business"),
    (Join-Path $ProjectRoot "src\server"), (Join-Path $ProjectRoot "include\business"),
    (Join-Path $ProjectRoot "include\server") -File |
    Where-Object { $_.Name -match 'camera_(?:frame|task)' }
$gpuInclude = $cameraSources | Select-String -Pattern '#include\s*[<\"](?:cuda|NvInfer|server/model_runner)'
if ($gpuInclude) {
    throw "Camera extraction must not introduce a GPU inference dependency: $($gpuInclude.Path)"
}

$controller = Get-Content -LiteralPath (Join-Path $ProjectRoot "src\server\camera_task_http_controller.cpp") `
    -Raw -Encoding UTF8
foreach ($route in @(
    '/api/v1/cameras',
    '/api/v1/cameras/<string>/start',
    '/api/v1/cameras/<string>/stop',
    '/api/v1/cameras/<string>/status',
    '/api/v1/cameras/<string>/latest-frame',
    '/api/v1/cameras/<string>/runs',
    '/api/v1/camera-hubs',
    '/api/v1/camera-hubs/<string>',
    '/api/v1/camera-profiles',
    '/api/v1/camera-profiles/<string>',
    '/api/v1/operations/metrics',
    '/api/v1/operations/metrics/prometheus',
    '/api/v1/operations/callbacks',
    '/api/v1/operations/callbacks/<string>/replay'
)) {
    if (-not $controller.Contains($route)) { throw "Required Camera API route is missing: $route" }
}
if ($controller -match 'CROW_ROUTE\s*\([^\r\n]*"/api/v1/camera-tasks') {
    throw "M11 violation: public Camera Task CRUD routes must not be registered"
}
if ($controller -match 'CROW_ROUTE\s*\([^\r\n]*"/api/v1/camera-profiles"\)\.methods') {
    throw "M11 violation: Camera Profile mutation routes must not be registered"
}

$adminRoot = Join-Path $ProjectRoot "web\camera-admin"
foreach ($asset in @('index.html','app.js','styles.css')) {
    if (-not (Test-Path -LiteralPath (Join-Path $adminRoot $asset))) {
        throw "M8 Camera admin asset is missing: $asset"
    }
}
$adminJs = Get-Content -LiteralPath (Join-Path $adminRoot "app.js") -Raw -Encoding UTF8
if ($adminJs -match 'localStorage|sessionStorage' -or $adminJs -match '(?i)rtsp[s]?://') {
    throw "M8 admin UI must not persist tokens or contain RTSP literals"
}
$adminHtml = Get-Content -LiteralPath (Join-Path $adminRoot "index.html") -Raw -Encoding UTF8
foreach ($element in @('camera-rows','profile-rows','hub-cards','operations-content')) {
    if (-not $adminHtml.Contains($element)) { throw "M8/M9 admin UI element is missing: $element" }
}

foreach ($script in @(
    'soak_camera_frame_feature.ps1',
    'backup_runtime.ps1',
    'restore_runtime.ps1',
    'test_mock_callback_backend.ps1',
    'verify_algorithm_service_p5.ps1',
    'process_tree_helpers.ps1',
    'exercise_worker_restart.ps1',
    'exercise_rtsp_reconnect.ps1',
    'exercise_dead_letter_replay.ps1',
    'exercise_multi_camera_stress.ps1',
    'exercise_runtime_mode_rollback.ps1',
    'verify_algorithm_service_p6.ps1',
    'verify_unified_camera_pipeline.ps1'
)) {
    $path = Join-Path (Join-Path $ProjectRoot 'scripts') $script
    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
        $path, [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors.Count -gt 0) { throw "M9/M10 PowerShell syntax failed: $script" }
}
$soak = Get-Content -LiteralPath (Join-Path $ProjectRoot "scripts\soak_camera_frame_feature.ps1") -Raw -Encoding UTF8
if ($soak -match '/camera-tasks') {
    throw "M11 violation: soak acceptance still calls removed Camera Task routes"
}
foreach ($soakMarker in @(
    "hub_instance_changed",
    "hub_reconnected_during_steady_soak",
    "camera_task_subscriber_missing",
    "camera_pipeline_subscriber_missing",
    "legacy_subscriber_present",
    "CaptureHostTelemetry"
)) {
    if (-not $soak.Contains($soakMarker)) {
        throw "P6 soak invariant is missing: $soakMarker"
    }
}

foreach ($target in @(
        'camera_storage_policy_test',
        'camera_task_lease_fence_test',
        'PostgreSQL::PostgreSQL',
        'postgres_storage'
    )) {
    if (-not $cmake.Contains($target)) { throw "M10 build target is missing: $target" }
}
if ($cmake -match 'unofficial::sqlite3|runtime_sqlite_integrity_check') {
    throw "M11 violation: production build still links SQLite"
}
$manager = Get-Content -LiteralPath (Join-Path $ProjectRoot "src\server\camera_task_manager.cpp") -Raw -Encoding UTF8
if ($manager -notmatch 'pipelines_\[command\.task_id\]' -or
    $manager -notmatch 'replaced->thread\.join') {
    throw "M11 camera_id thread ownership/replacement invariant is missing"
}
$cameraRuntime = Get-Content -LiteralPath (
    Join-Path $ProjectRoot "src\server\camera_task_runtime.cpp"
) -Raw -Encoding UTF8
foreach ($recoveryMarker in @(
    "CAMERA_RECOVERY_LEASE_ACTIVE",
    "worker_restart_recovery",
    "recovery_commands"
)) {
    if (-not $cameraRuntime.Contains($recoveryMarker)) {
        throw "P6 Worker restart recovery guard is missing: $recoveryMarker"
    }
}
$cameraQueue = Get-Content -LiteralPath (
    Join-Path $ProjectRoot "src\server\camera_task_queue.cpp"
) -Raw -Encoding UTF8
foreach ($leaseMarker in @(
    "lease_owner_token_",
    "leaseValue(run_id)",
    "not owned by this Worker instance"
)) {
    if (-not $cameraQueue.Contains($leaseMarker)) {
        throw "P6 Worker-generation lease fence is missing: $leaseMarker"
    }
}
$ffmpegRuntime = Get-Content -LiteralPath (
    Join-Path $ProjectRoot "src\business\ffmpeg_process_capture_reader.cpp"
) -Raw -Encoding UTF8
if (-not $ffmpegRuntime.Contains(
        "JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE")) {
    throw "P6 FFmpeg child process containment guard is missing"
}
$postgresSchema = Join-Path $ProjectRoot "db\postgresql\001_initial_schema.sql"
if (-not (Test-Path -LiteralPath $postgresSchema)) { throw "M11 PostgreSQL schema is missing" }

$postmanRoot = Join-Path $ProjectRoot "postman"
foreach ($postmanFile in @(
    'vision_project_p5.postman_collection.json',
    'vision_project_p5.local.postman_environment.json'
)) {
    $path = Join-Path $postmanRoot $postmanFile
    if (-not (Test-Path -LiteralPath $path)) {
        throw "P5 Postman artifact is missing: $postmanFile"
    }
    Get-Content -LiteralPath $path -Raw -Encoding UTF8 |
        ConvertFrom-Json | Out-Null
}
$postmanCollection = Get-Content -LiteralPath (
    Join-Path $postmanRoot "vision_project_p5.postman_collection.json"
) -Raw -Encoding UTF8
foreach ($contractMarker in @(
    "pm.collectionVariables.set('camera_etag'",
    "enable_dead_letter_replay",
    "pm.execution.skipRequest()"
)) {
    if (-not $postmanCollection.Contains($contractMarker)) {
        throw "P6 Postman lifecycle/replay guard is missing: $contractMarker"
    }
}
$postmanEnvironment = Get-Content -LiteralPath (
    Join-Path $postmanRoot "vision_project_p5.local.postman_environment.json"
) -Raw -Encoding UTF8 | ConvertFrom-Json
$replayVariable = @($postmanEnvironment.values |
    Where-Object { $_.key -eq "enable_dead_letter_replay" })
if ($replayVariable.Count -ne 1 -or $replayVariable[0].value -ne "false") {
    throw "P6 Postman dead-letter replay must default to false"
}
$p6Script = Get-Content -LiteralPath (
    Join-Path $ProjectRoot "scripts\verify_algorithm_service_p6.ps1"
) -Raw -Encoding UTF8
foreach ($contractMarker in @(
    "YOLO11_CAMERA_ENTRY_URL",
    "New-AcceptanceConfig",
    "CaptureHostTelemetry",
    "exercise_worker_restart.ps1",
    "exercise_rtsp_reconnect.ps1",
    "exercise_dead_letter_replay.ps1",
    "exercise_multi_camera_stress.ps1",
    "exercise_runtime_mode_rollback.ps1",
    "UnifiedCameraPipeline",
    "disposable_infrastructure_removed"
)) {
    if (-not $p6Script.Contains($contractMarker)) {
        throw "P6 hardware acceptance guard is missing: $contractMarker"
    }
}
if ($p6Script -match '(?i)rtsp[s]?://[^*{\s]+:[^@{\s]+@') {
    throw "P6 acceptance script must not contain embedded RTSP credentials"
}
$deadLetterExercise = Get-Content -LiteralPath (
    Join-Path $ProjectRoot "scripts\exercise_dead_letter_replay.ps1"
) -Raw -Encoding UTF8
foreach ($deadLetterMarker in @(
    "Reset-Mock 100",
    "final_status",
    "receiver_observed"
)) {
    if (-not $deadLetterExercise.Contains($deadLetterMarker)) {
        throw "P6 dead-letter acceptance invariant is missing: $deadLetterMarker"
    }
}
$mockBackend = Join-Path $ProjectRoot "scripts\mock_callback_backend.js"
& node --check $mockBackend
if ($LASTEXITCODE -ne 0) { throw "P5 mock callback JavaScript syntax failed" }
& (Join-Path $PSScriptRoot "test_mock_callback_backend.ps1") -Root $ProjectRoot
if ($LASTEXITCODE -ne 0) { throw "P5 mock callback acceptance failed" }

Write-Host "PASS: Camera Frame M0-M11 and Algorithm Service P5/P6 architecture, security, backend, Postman, and callback guards passed." `
    -ForegroundColor Green
