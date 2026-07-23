[CmdletBinding()]
param(
    [string]$Root = "",
    [string]$BuildDir = ".\out\build\backend-Release",
    [switch]$ConfirmDisposableDatabase
)

$ErrorActionPreference = "Stop"
$ProjectRoot = if ($Root) { (Resolve-Path $Root).Path } else { (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
if (-not $env:YOLO11_TEST_POSTGRES_DSN) {
    throw "YOLO11_TEST_POSTGRES_DSN is not configured. It must point to a disposable test database."
}
if (-not $ConfirmDisposableDatabase) {
    throw "Pass -ConfirmDisposableDatabase. These tests drop and recreate tables in YOLO11_TEST_POSTGRES_DSN."
}
if ($env:YOLO11_POSTGRES_DSN -and $env:YOLO11_POSTGRES_DSN -eq $env:YOLO11_TEST_POSTGRES_DSN) {
    throw "YOLO11_TEST_POSTGRES_DSN must not equal YOLO11_POSTGRES_DSN."
}
$env:YOLO11_ALLOW_DESTRUCTIVE_POSTGRES_TESTS = "1"
$buildPath = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $BuildDir))
$executables = @(
    "repository_test.exe",
    "camera_task_repository_test.exe",
    "camera_frame_extraction_test.exe",
    "camera_storage_policy_test.exe",
    "camera_task_http_contract_test.exe",
    "camera_algorithm_processor_test.exe",
    "callback_delivery_worker_test.exe"
)
foreach ($name in $executables) {
    $testExe = Join-Path $buildPath $name
    if (-not (Test-Path -LiteralPath $testExe)) { throw "PostgreSQL test executable is missing: $testExe" }
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw "PostgreSQL integration test failed: $name" }
}
Write-Host "PASS: PostgreSQL People Flow, Camera CRUD/lifecycle, extraction, analysis, alert callback, storage, and HTTP contracts are valid." -ForegroundColor Green
