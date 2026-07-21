# =============================================================================
# start-bridge.ps1 - Starts the bridge server on WINDOWS.
#
# Installs dependencies (npm install) ONLY when needed:
#   - first start (node_modules does not exist), or
#   - package.json changed since the last install.
# On all other starts, it goes straight to 'node server.js'.
#
# Usage (PowerShell, from any folder):
#   powershell -ExecutionPolicy Bypass -File \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge\start-bridge.ps1
# or, while in the bridge folder:
#   .\start-bridge.ps1
#
# Note: this script is pure ASCII on purpose (Windows PowerShell 5.1 reads .ps1
# files without a BOM as ANSI; using non-ASCII symbols breaks the parser).
# =============================================================================

$ErrorActionPreference = "Stop"

# The bridge folder is the one of THIS script (works from any cwd).
$BridgeDir = $PSScriptRoot
$pkg     = Join-Path $BridgeDir "package.json"
$lock    = Join-Path $BridgeDir "package-lock.json"
$modules = Join-Path $BridgeDir "node_modules"

function Info($m) { Write-Host ">> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "[OK] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[X] $m" -ForegroundColor Red; exit 1 }

# -- 0) Is Node installed? ----------------------------------------------------
if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
  Fail "'node' not found on Windows. Install Node.js (https://nodejs.org)."
}
if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {
  Fail "'npm' not found on Windows. It comes with Node.js."
}
Info ("Node " + (node --version) + "  -  folder: $BridgeDir")

# Always work from the bridge folder (npm install and node server.js must run
# here, not in the cwd from which the script was launched).
Set-Location $BridgeDir

# -- 1+2) Do we need to install dependencies? ---------------------------------
$needInstall = $false
$reason = ""
if (-not (Test-Path $modules)) {
  $needInstall = $true; $reason = "node_modules does not exist (first start)"
}
elseif (Test-Path $lock) {
  # package.json newer than the lock => dependencies changed after installing.
  if ((Get-Item $pkg).LastWriteTime -gt (Get-Item $lock).LastWriteTime) {
    $needInstall = $true; $reason = "package.json changed since the last install"
  }
}
elseif ((Get-Item $pkg).LastWriteTime -gt (Get-Item $modules).LastWriteTime) {
  # No lock: we compare against node_modules as an approximation.
  $needInstall = $true; $reason = "package.json changed since the last install"
}

if ($needInstall) {
  Warn "Installing dependencies ($reason)..."
  & npm install
  if ($LASTEXITCODE -ne 0) { Fail "'npm install' failed (code $LASTEXITCODE)." }
  Ok "Dependencies installed."
} else {
  Ok "Dependencies already present and up to date (no reinstall)."
}

# -- 3) Start the server ------------------------------------------------------
Info "Starting the bridge server (Ctrl+C to stop)..."
& node server.js
