# build.ps1
# Run from the LightningDetector folder after INSTALL_DEPS.ps1 completes.

param(
    [switch]$SkipDeps,
    [string]$VcpkgRoot = "C:\vcpkg"
)

$ErrorActionPreference = "Stop"
$BuildType = "Release"

Write-Host ""
Write-Host "=== Lightning Detector - Build Script ===" -ForegroundColor Cyan

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: cmake not found. Restart PowerShell after install." -ForegroundColor Red
    exit 1
}
Write-Host "  cmake: $(cmake --version | Select-Object -First 1)"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: git not found." -ForegroundColor Red
    exit 1
}
Write-Host "  git: $(git --version)"

$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) {
    Write-Host "ERROR: Visual Studio not found." -ForegroundColor Red
    exit 1
}
$vsPath = & $vsWhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Write-Host "  VS: $vsPath"

$env:VCPKG_ROOT = $VcpkgRoot
Write-Host "  vcpkg: $VcpkgRoot"

$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir   = Join-Path $ProjectDir "build"

$generator = "Visual Studio 17 2022"
$vs2019 = & $vsWhere -latest -version "[16,17)" -property installationPath 2>$null
$vs2022 = & $vsWhere -latest -version "[17,18)" -property installationPath 2>$null
if (-not $vs2022 -and $vs2019) {
    $generator = "Visual Studio 16 2019"
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Write-Host ""
Write-Host "Configuring with CMake..." -ForegroundColor Yellow

cmake -B $BuildDir -S $ProjectDir `
    -G $generator `
    -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
    "-DVCPKG_TARGET_TRIPLET=x64-windows" `
    "-DCMAKE_BUILD_TYPE=$BuildType"

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configuration FAILED." -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "Building..." -ForegroundColor Yellow
cmake --build $BuildDir --config $BuildType --parallel

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build FAILED." -ForegroundColor Red
    exit $LASTEXITCODE
}

$ExeDir = Join-Path $BuildDir $BuildType
$DllSrc = "$VcpkgRoot\installed\x64-windows\bin"
if (Test-Path $DllSrc) {
    Write-Host "Copying DLLs..." -ForegroundColor Yellow
    Get-ChildItem $DllSrc -Filter "opencv*.dll" | Copy-Item -Destination $ExeDir -Force
    Get-ChildItem $DllSrc -Filter "*.dll" |
        Where-Object { $_.Name -match "^(avcodec|avformat|avutil|swscale|swresample)" } |
        Copy-Item -Destination $ExeDir -Force
}

$ExePath = Join-Path $ExeDir "LightningDetector.exe"
if (Test-Path $ExePath) {
    Write-Host ""
    Write-Host "Build successful!" -ForegroundColor Green
    Write-Host "  Executable: $ExePath" -ForegroundColor Green
} else {
    Write-Host "Executable not found - check build output." -ForegroundColor Red
    exit 1
}
