# INSTALL_DEPS.ps1
# Run in an elevated PowerShell window from the LightningDetector folder.
# Installs Git, CMake, VS Build Tools, vcpkg, OpenCV, nlohmann-json.

$ErrorActionPreference = "Stop"

Write-Host "=== Installing Lightning Detector Dependencies ===" -ForegroundColor Cyan

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host "winget not found. Install App Installer from the Microsoft Store." -ForegroundColor Red
    exit 1
}

function Install-WingetPkg($id, $name) {
    $result = winget list --id $id --accept-source-agreements 2>$null | Select-String $id
    if ($result) {
        Write-Host "  [already installed] $name" -ForegroundColor DarkGray
    } else {
        Write-Host "  Installing $name ..." -ForegroundColor Yellow
        winget install --id $id -e --accept-package-agreements --accept-source-agreements
    }
}

Install-WingetPkg "Git.Git" "Git for Windows"
Install-WingetPkg "Kitware.CMake" "CMake"

Write-Host ""
Write-Host "  Checking for Visual Studio C++ tools..." -ForegroundColor Yellow
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) {
    Write-Host "  Installing Visual Studio 2022 Build Tools..." -ForegroundColor Yellow
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
        --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" `
        --accept-package-agreements --accept-source-agreements
} else {
    $hasCpp = & $vsWhere -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($hasCpp) {
        Write-Host "  [already installed] Visual Studio C++ tools" -ForegroundColor DarkGray
    } else {
        Write-Host "  C++ workload missing. Open Visual Studio Installer and add Desktop development with C++" -ForegroundColor Red
    }
}

$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" +
            [System.Environment]::GetEnvironmentVariable("Path","User")

$vcpkgRoot = "C:\vcpkg"
if (-not (Test-Path $vcpkgRoot)) {
    Write-Host ""
    Write-Host "  Cloning vcpkg to $vcpkgRoot ..." -ForegroundColor Yellow
    git clone https://github.com/microsoft/vcpkg.git $vcpkgRoot
    & "$vcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
} else {
    Write-Host "  [already exists] $vcpkgRoot" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "  Installing OpenCV and nlohmann-json via vcpkg..." -ForegroundColor Yellow
Write-Host "  This may take 15-30 minutes on first run." -ForegroundColor DarkGray

& "$vcpkgRoot\vcpkg.exe" install "opencv4[core,highgui,imgproc,videoio,ffmpeg]:x64-windows" "nlohmann-json:x64-windows"

& "$vcpkgRoot\vcpkg.exe" integrate install

Write-Host ""
Write-Host "=== All dependencies installed! ===" -ForegroundColor Green
Write-Host "Next: open a NEW PowerShell window, cd into LightningDetector, and run:" -ForegroundColor Cyan
Write-Host "  .\build.ps1"
