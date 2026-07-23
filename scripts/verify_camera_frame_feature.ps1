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
    '/api/v1/operations/metrics/prometheus'
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

foreach ($script in @('soak_camera_frame_feature.ps1','backup_runtime.ps1','restore_runtime.ps1')) {
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

foreach ($target in @('camera_storage_policy_test','PostgreSQL::PostgreSQL','postgres_storage')) {
    if (-not $cmake.Contains($target)) { throw "M10 build target is missing: $target" }
}
if ($cmake -match 'unofficial::sqlite3|runtime_sqlite_integrity_check') {
    throw "M11 violation: production build still links SQLite"
}
$manager = Get-Content -LiteralPath (Join-Path $ProjectRoot "src\server\camera_task_manager.cpp") -Raw -Encoding UTF8
if ($manager -notmatch 'sessions_\[command\.task_id\]' -or $manager -notmatch 'replaced->thread\.join') {
    throw "M11 camera_id thread ownership/replacement invariant is missing"
}
$postgresSchema = Join-Path $ProjectRoot "db\postgresql\001_initial_schema.sql"
if (-not (Test-Path -LiteralPath $postgresSchema)) { throw "M11 PostgreSQL schema is missing" }

Write-Host "PASS: Camera Frame M0-M11 camera-id lifecycle, PostgreSQL, architecture, security, backend, and Qt guards passed." `
    -ForegroundColor Green
