[CmdletBinding()]
param([string]$Root = "")

$ErrorActionPreference = "Continue"
$ProjectRoot = if ($Root) { (Resolve-Path $Root).Path } else { (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$pidFile = Join-Path $ProjectRoot "runtime\pids\demo.json"
if (Test-Path -LiteralPath $pidFile) {
    $payload = Get-Content -LiteralPath $pidFile -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($item in @($payload.processes)) {
        if ($item.pid) {
            Stop-Process -Id ([int]$item.pid) -Force -ErrorAction SilentlyContinue
            Write-Host "[STOP] $($item.name) pid=$($item.pid)"
        }
    }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
}
Write-Host "Demo processes stopped."
