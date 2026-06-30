# build_installer.ps1
# Run this AFTER build.ps1 has succeeded (LightningDetector.exe + DLLs must
# already exist in build\Release). Produces a single, double-clickable
# installer: installer\Output\LightningDetectorSetup.exe
#
# That installer is fully self-contained - it bundles the exe and every
# DLL it needs, and the exe itself is statically linked against the C++
# runtime, so whoever runs the installer needs NO prior downloads at all
# (no Visual C++ Redistributable, no OpenCV, nothing).

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ExePath = Join-Path $ProjectDir "build\Release\LightningDetector.exe"

if (-not (Test-Path $ExePath)) {
    Write-Host "ERROR: $ExePath not found. Run .\build.ps1 first." -ForegroundColor Red
    exit 1
}

# Locate Inno Setup's command-line compiler (iscc.exe); install it via
# winget if it isn't present yet.
$iscc = Get-Command iscc -ErrorAction SilentlyContinue
if (-not $iscc) {
    $candidatePaths = @(
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    )
    $found = $candidatePaths | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($found) {
        $iscc = $found
    } else {
        Write-Host "Inno Setup not found - installing via winget (one-time)..." -ForegroundColor Yellow
        winget install --id JRSoftware.InnoSetup -e --silent --accept-source-agreements --accept-package-agreements
        $found = $candidatePaths | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($found) { $iscc = $found }
    }
}
if (-not $iscc) {
    Write-Host "ERROR: Could not find or install Inno Setup (ISCC.exe)." -ForegroundColor Red
    Write-Host "Install manually: winget install JRSoftware.InnoSetup" -ForegroundColor Yellow
    exit 1
}
$isccPath = if ($iscc -is [System.Management.Automation.CommandInfo]) { $iscc.Source } else { $iscc }

Write-Host "Compiling installer with: $isccPath" -ForegroundColor Cyan
& $isccPath "$ProjectDir\installer\LightningDetectorSetup.iss"

if ($LASTEXITCODE -ne 0) {
    Write-Host "Installer build FAILED." -ForegroundColor Red
    exit $LASTEXITCODE
}

$OutputExe = Join-Path $ProjectDir "installer\Output\LightningDetectorSetup.exe"
if (Test-Path $OutputExe) {
    Write-Host ""
    Write-Host "Installer built successfully!" -ForegroundColor Green
    Write-Host "  $OutputExe" -ForegroundColor Green
    Write-Host ""
    Write-Host "Anyone can now double-click that single file to install" -ForegroundColor Green
    Write-Host "Lightning Detector - no other downloads needed." -ForegroundColor Green
} else {
    Write-Host "Installer not found after compile - check output above." -ForegroundColor Red
    exit 1
}
