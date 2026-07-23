[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BackupPath,
    [switch]$ConfirmRestore
)

$ErrorActionPreference = "Stop"
if (-not $ConfirmRestore) { throw "Restore requires the explicit -ConfirmRestore switch." }
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot
$archive = (Resolve-Path -LiteralPath $BackupPath).Path
if ([IO.Path]::GetExtension($archive) -ne '.zip') { throw "BackupPath must be a .zip archive." }
$pidFile = Join-Path $ProjectRoot "runtime\pids\demo.json"
if (Test-Path -LiteralPath $pidFile) {
    $live = @((Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json).processes | Where-Object { Get-Process -Id ([int]$_.pid) -ErrorAction SilentlyContinue })
    if ($live.Count -gt 0) { throw "Restore refuses to run while demo processes are active." }
}
foreach ($name in @("PGHOST","PGDATABASE","PGUSER","PGPASSWORD")) {
    if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name,"Process"))) { throw "$name must be configured for pg_restore." }
}
$pgRestore = Get-Command pg_restore.exe -CommandType Application -ErrorAction SilentlyContinue
if (-not $pgRestore) { throw "pg_restore.exe must be available on PATH." }

$restoreResolved = [IO.Path]::GetFullPath((Join-Path $ProjectRoot "runtime\restore-stage"))
New-Item -ItemType Directory -Force -Path $restoreResolved | Out-Null
$stageResolved = [IO.Path]::GetFullPath((Join-Path $restoreResolved ("restore_" + (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssfffZ"))))
if (-not $stageResolved.StartsWith($restoreResolved + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe restore staging path." }
New-Item -ItemType Directory -Force -Path $stageResolved | Out-Null

try {
    Expand-Archive -LiteralPath $archive -DestinationPath $stageResolved
    $manifestPath = Join-Path $stageResolved "manifest.json"
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Backup manifest is missing." }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([int]$manifest.schema_version -ne 2 -or $manifest.database -ne "postgresql" -or -not $manifest.offline) { throw "Unsupported backup manifest." }
    foreach ($entry in $manifest.files) {
        $relative = [string]$entry.path
        if ($relative -notmatch '^(config/(server|worker|cameras)\.yaml|runtime/data/postgresql\.dump)$') { throw "Unsafe manifest path: $relative" }
        $source = [IO.Path]::GetFullPath((Join-Path $stageResolved ($relative -replace '/', '\')))
        if (-not $source.StartsWith($stageResolved + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $source)) { throw "Manifest source is missing: $relative" }
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne [string]$entry.sha256) { throw "SHA-256 mismatch: $relative" }
        if ((Get-Item -LiteralPath $source).Length -ne [long]$entry.size_bytes) { throw "Size mismatch: $relative" }
    }
    & (Join-Path $PSScriptRoot "backup_runtime.ps1") -Label "pre_restore"
    $dumpPath = Join-Path $stageResolved "runtime\data\postgresql.dump"
    & $pgRestore.Source --clean --if-exists --no-owner --no-acl --exit-on-error --dbname=$env:PGDATABASE $dumpPath
    if ($LASTEXITCODE -ne 0) { throw "PostgreSQL pg_restore failed." }
    foreach ($relative in @("config/server.yaml","config/worker.yaml","config/cameras.yaml")) {
        $source = Join-Path $stageResolved ($relative -replace '/', '\')
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $ProjectRoot ($relative -replace '/', '\')) -Force }
    }
    Write-Host "PASS: PostgreSQL runtime restore completed. Restart Server/Worker and run readiness acceptance." -ForegroundColor Green
}
finally {
    if ((Test-Path -LiteralPath $stageResolved) -and (Split-Path $stageResolved -Leaf).StartsWith('restore_') -and
        $stageResolved.StartsWith($restoreResolved + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $stageResolved -Recurse -Force
    }
}
