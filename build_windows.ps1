# Build Script for Windows

$ProjectRoot = Get-Location
$ApiDir = Join-Path $ProjectRoot "api"
$VsiteDir = Join-Path $ProjectRoot "vsite"
$ElectronDir = Join-Path $ProjectRoot "electron"

Write-Host "--- 1. Cleaning previous builds ---" -ForegroundColor Cyan
if (Test-Path (Join-Path $ApiDir "dist")) { Remove-Item -Recurse -Force (Join-Path $ApiDir "dist") }
if (Test-Path (Join-Path $ApiDir "build")) { Remove-Item -Recurse -Force (Join-Path $ApiDir "build") }
if (Test-Path (Join-Path $VsiteDir "dist")) { Remove-Item -Recurse -Force (Join-Path $VsiteDir "dist") }
if (Test-Path (Join-Path $ElectronDir "dist")) { Remove-Item -Recurse -Force (Join-Path $ElectronDir "dist") }
if (Test-Path (Join-Path $ElectronDir "dist-built")) { Remove-Item -Recurse -Force (Join-Path $ElectronDir "dist-built") }

Write-Host "`n--- 2. Packaging Python Backend (api.exe) ---" -ForegroundColor Cyan
Set-Location $ApiDir
# Ensure pyinstaller is installed in the venv
& ".\.venv\Scripts\python.exe" -m pip install pyinstaller
& ".\.venv\Scripts\pyinstaller.exe" api.spec

Write-Host "`n--- 3. Building Frontend and Packaging Electron Application ---" -ForegroundColor Cyan
# Builds vsite, copies vsite/dist into electron/dist, then runs electron-builder
Set-Location $ElectronDir
npm run build:win

Write-Host "`n--- Build Complete! ---" -ForegroundColor Green
Write-Host "Installer located in: $(Join-Path $ElectronDir 'dist-built')"
Set-Location $ProjectRoot
