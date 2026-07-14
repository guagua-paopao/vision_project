[CmdletBinding()]
param(
    [string]$Root = "",
    [string]$BuildDir = ".\out\build\backend-Release",
    [string]$VcpkgRoot = "D:\vcpkg",
    [string]$CudaRoot = "D:\GPU13.3",
    [string]$TensorRtRoot = "D:\TensorRT-10.16.1.11",
    [string]$OpenCvDir = "D:\libs\opencv\build\x64\vc16\lib",
    [string]$CudaArchitectures = "89",
    [string]$VisualStudioRoot = "D:\vs2019",
    [switch]$InstallDependencies,
    [switch]$CleanFirst
)

$ErrorActionPreference = "Stop"
$ProjectRoot = if ($Root) { (Resolve-Path $Root).Path } else { (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$BuildPath = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $BuildDir))
$CMakeExe = Join-Path $VisualStudioRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$CTestExe = Join-Path $VisualStudioRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
$NinjaExe = Join-Path $VisualStudioRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"

function Import-VisualStudioEnvironment([string]$VsRoot) {
    $vcvars64 = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path -LiteralPath $vcvars64)) { throw "vcvars64.bat not found: $vcvars64" }
    $lines = & cmd.exe /d /s /c "`"call `"$vcvars64`" >nul && set`""
    if ($LASTEXITCODE -ne 0) { throw "Failed to import Visual Studio x64 environment" }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $lines) {
        $separator = $line.IndexOf("=")
        if ($separator -le 0) { continue }
        $name = $line.Substring(0, $separator)
        if ($seen.Add($name)) {
            [Environment]::SetEnvironmentVariable($name, $line.Substring($separator + 1), "Process")
        }
    }
}

Import-VisualStudioEnvironment $VisualStudioRoot
foreach ($path in @($CMakeExe, $CTestExe, $NinjaExe, $Toolchain)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required build tool not found: $path" }
}
if ($InstallDependencies) {
    if (-not (Test-Path -LiteralPath $VcpkgExe)) { throw "vcpkg.exe not found: $VcpkgExe" }
    & $VcpkgExe install --triplet x64-windows --x-manifest-root=$ProjectRoot
    if ($LASTEXITCODE -ne 0) { throw "vcpkg manifest install failed" }
}
New-Item -ItemType Directory -Force -Path $BuildPath | Out-Null

$configure = @(
    "-S", $ProjectRoot, "-B", $BuildPath, "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe", "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain", "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-DVCPKG_MANIFEST_MODE=$(if($InstallDependencies){'ON'}else{'OFF'})",
    "-DYOLO11_CUDA_ROOT=$CudaRoot", "-DTENSORRT_ROOT=$TensorRtRoot",
    "-DOpenCV_DIR=$OpenCvDir", "-DCMAKE_CUDA_ARCHITECTURES=$CudaArchitectures"
)
if ($CleanFirst) { $configure += "--fresh" }
& $CMakeExe @configure
if ($LASTEXITCODE -ne 0) { throw "Backend CMake configure failed" }

$build = @("--build", $BuildPath, "--config", "Release", "--target",
    "four_stage_server", "four_stage_worker",
    "people_flow_core_test", "security_analytics_test", "repository_test", "pose_engine_smoke")
if ($CleanFirst) { $build += "--clean-first" }
& $CMakeExe @build
if ($LASTEXITCODE -ne 0) { throw "Backend build failed" }

& $CTestExe --test-dir $BuildPath -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Backend tests failed" }
Write-Host "PASS: compact backend built and tested at $BuildPath" -ForegroundColor Green
