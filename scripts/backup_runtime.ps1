[CmdletBinding()]
param(
    [string]$BackupDir = ".\runtime\backups",
    [string]$Label = "manual",
    [int]$RetentionCount = 7
)

$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot
$pidFile = Join-Path $ProjectRoot "runtime\pids\demo.json"
if (Test-Path -LiteralPath $pidFile) {
    $live = @((Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json).processes | Where-Object {
        Get-Process -Id ([int]$_.pid) -ErrorAction SilentlyContinue
    })
    if ($live.Count -gt 0) { throw "Offline backup requires demo processes to be stopped first." }
}
foreach ($name in @("PGHOST","PGDATABASE","PGUSER","PGPASSWORD")) {
    if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name,"Process"))) {
        throw "$name must be configured for pg_dump without exposing credentials on the command line."
    }
}
$pgDump = Get-Command pg_dump.exe -CommandType Application -ErrorAction SilentlyContinue
if (-not $pgDump) { throw "pg_dump.exe must be available on PATH." }

$safeLabel = ($Label -replace '[^A-Za-z0-9_-]', '_').Trim('_')
if (-not $safeLabel) { $safeLabel = "manual" }
$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssfffZ")
$resolvedBackup = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $BackupDir))
New-Item -ItemType Directory -Force -Path $resolvedBackup | Out-Null
$stageResolved = [IO.Path]::GetFullPath((Join-Path $resolvedBackup (".stage_" + $stamp)))
if (-not $stageResolved.StartsWith($resolvedBackup + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe backup staging path." }
New-Item -ItemType Directory -Force -Path (Join-Path $stageResolved "runtime\data") | Out-Null

try {
    $relativeFiles = [Collections.Generic.List[string]]::new()
    foreach ($relative in @("config\server.yaml","config\worker.yaml","config\cameras.yaml")) {
        $source = Join-Path $ProjectRoot $relative
        if (Test-Path -LiteralPath $source) {
            $target = Join-Path $stageResolved $relative
            New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
            Copy-Item -LiteralPath $source -Destination $target
            $relativeFiles.Add($relative)
        }
    }
    $dumpRelative = "runtime\data\postgresql.dump"
    $dumpPath = Join-Path $stageResolved $dumpRelative
    & $pgDump.Source --format=custom --no-owner --no-acl --file=$dumpPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $dumpPath)) { throw "PostgreSQL pg_dump failed." }
    $relativeFiles.Add($dumpRelative)

    $manifestFiles = @($relativeFiles | Sort-Object | ForEach-Object {
        $item = Get-Item -LiteralPath (Join-Path $stageResolved $_)
        [ordered]@{path=($_ -replace '\\','/');size_bytes=$item.Length;sha256=(Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
    [ordered]@{schema_version=2;database="postgresql";created_at=(Get-Date).ToUniversalTime().ToString("o");label=$safeLabel;offline=$true;files=$manifestFiles} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stageResolved "manifest.json") -Encoding UTF8
    $archive = Join-Path $resolvedBackup ("vision_runtime_${stamp}_${safeLabel}.zip")
    Compress-Archive -Path (Join-Path $stageResolved "*") -DestinationPath $archive -CompressionLevel Optimal
    $archives = @(Get-ChildItem -LiteralPath $resolvedBackup -Filter "vision_runtime_*.zip" -File | Sort-Object LastWriteTimeUtc -Descending)
    $retain = [Math]::Max(1, $RetentionCount)
    $oldArchives = @($archives | Select-Object -Skip $retain)
    foreach ($old in $oldArchives) {
        Remove-Item -LiteralPath $old.FullName -Force
    }
    Write-Host "PASS: PostgreSQL runtime backup created: $archive" -ForegroundColor Green
}
finally {
    if ((Test-Path -LiteralPath $stageResolved) -and (Split-Path $stageResolved -Leaf).StartsWith('.stage_') -and
        $stageResolved.StartsWith($resolvedBackup + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $stageResolved -Recurse -Force
    }
}
