[CmdletBinding()]
param(
    [string]$Root = "",
    [string]$QtRoot = $env:QT_ROOT,
    [string]$BuildDir = ".\out\build\qt-client-Release",
    [string]$VisualStudioRoot = "D:\vs2019",
    [string]$CMakeExe = "",
    [string]$NinjaExe = "",
    [string]$CompilerRoot = "",
    [switch]$CleanFirst,
    [switch]$SkipDeploy
)

$ErrorActionPreference = "Stop"
$ProjectRoot = if ($Root) { (Resolve-Path $Root).Path } else { (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$SourceDir = Join-Path $ProjectRoot "qt_client"
$BuildPath = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $BuildDir))

function Find-QtRoot {
    $candidates = @()
    foreach ($base in @("C:\Qt", "D:\Qt")) {
        if (-not (Test-Path -LiteralPath $base)) { continue }
        $candidates += Get-ChildItem -Path $base -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Get-ChildItem -Path $_.FullName -Directory -ErrorAction SilentlyContinue } |
            Where-Object { $_.Name -match '^(msvc\d+_64|mingw_64)$' -and (Test-Path (Join-Path $_.FullName "lib\cmake\Qt6")) } |
            Sort-Object FullName -Descending |
            Select-Object -ExpandProperty FullName
    }
    return $candidates | Select-Object -First 1
}

function Import-VisualStudioEnvironment([string]$VsRoot) {
    $vcvars64 = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path -LiteralPath $vcvars64)) {
        throw "vcvars64.bat not found: $vcvars64"
    }
    $environmentLines = & cmd.exe /d /s /c "`"call `"$vcvars64`" >nul && set`""
    if ($LASTEXITCODE -ne 0) { throw "Failed to import the Visual Studio x64 environment" }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf("=")
        if ($separator -le 0) { continue }
        [Environment]::SetEnvironmentVariable(
            $line.Substring(0, $separator), $line.Substring($separator + 1), "Process")
    }
}

if (-not $QtRoot) { $QtRoot = Find-QtRoot }
if (-not $QtRoot -or -not (Test-Path -LiteralPath $QtRoot)) {
    throw "Qt was not found. Pass -QtRoot or set QT_ROOT. Example: D:\Qt\6.11.1\mingw_64"
}
$QtRoot = (Resolve-Path $QtRoot).Path
$QtInstallRoot = Split-Path (Split-Path $QtRoot -Parent) -Parent
$IsMinGw = (Split-Path $QtRoot -Leaf) -match '^mingw'

$BundledCMakeRoot = Join-Path $VisualStudioRoot "Common7\IDE\CommonExtensions\Microsoft\CMake"
if (-not $CMakeExe) {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    $QtCMake = Join-Path $QtInstallRoot "Tools\CMake_64\bin\cmake.exe"
    $CMakeExe = if ($command) { $command.Source } elseif (Test-Path $QtCMake) { $QtCMake } else { Join-Path $BundledCMakeRoot "CMake\bin\cmake.exe" }
}
if (-not $NinjaExe) {
    $command = Get-Command ninja -ErrorAction SilentlyContinue
    $QtNinja = Join-Path $QtInstallRoot "Tools\Ninja\ninja.exe"
    $NinjaExe = if ($command) { $command.Source } elseif (Test-Path $QtNinja) { $QtNinja } else { Join-Path $BundledCMakeRoot "Ninja\ninja.exe" }
}
foreach ($tool in @($CMakeExe, $NinjaExe)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Build tool not found: $tool" }
}

if ($IsMinGw) {
    if (-not $CompilerRoot) {
        $CompilerRoot = Get-ChildItem -Path (Join-Path $QtInstallRoot "Tools") -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^mingw\d+_64$' -and (Test-Path (Join-Path $_.FullName "bin\g++.exe")) } |
            Sort-Object Name -Descending |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $CompilerRoot -or -not (Test-Path (Join-Path $CompilerRoot "bin\g++.exe"))) {
        throw "Matching MinGW compiler was not found under $QtInstallRoot\Tools. Pass -CompilerRoot explicitly."
    }
    $CompilerRoot = (Resolve-Path $CompilerRoot).Path
    $env:PATH = "$(Join-Path $CompilerRoot 'bin');$(Join-Path $QtRoot 'bin');$env:PATH"
    Write-Host "Using Qt MinGW toolchain: $CompilerRoot" -ForegroundColor Cyan
} else {
    Import-VisualStudioEnvironment $VisualStudioRoot
}
New-Item -ItemType Directory -Force -Path $BuildPath | Out-Null

$ConfigureArguments = @(
    "-S", $SourceDir,
    "-B", $BuildPath,
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_PREFIX_PATH=$QtRoot"
)
if ($CleanFirst) { $ConfigureArguments += "--fresh" }
if ($IsMinGw) {
    $ConfigureArguments += "-DCMAKE_CXX_COMPILER=$(Join-Path $CompilerRoot 'bin\g++.exe')"
}
& $CMakeExe @ConfigureArguments
if ($LASTEXITCODE -ne 0) { throw "Qt client CMake configure failed" }

$BuildArguments = @("--build", $BuildPath, "--config", "Release", "--target", "people_flow_qt_client")
if ($CleanFirst) { $BuildArguments += "--clean-first" }
& $CMakeExe @BuildArguments
if ($LASTEXITCODE -ne 0) { throw "Qt client build failed" }

$ExePath = Join-Path $BuildPath "people_flow_qt_client.exe"
if (-not (Test-Path -LiteralPath $ExePath)) { throw "Build output missing: $ExePath" }

if (-not $SkipDeploy) {
    $deployTool = Join-Path $QtRoot "bin\windeployqt.exe"
    if (-not (Test-Path -LiteralPath $deployTool)) { throw "windeployqt.exe not found: $deployTool" }
    & $deployTool --release --no-translations --compiler-runtime $ExePath
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }
}

Write-Host "PASS: Qt client built at $ExePath" -ForegroundColor Green
