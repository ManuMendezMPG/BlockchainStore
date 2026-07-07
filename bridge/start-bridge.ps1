# =============================================================================
# start-bridge.ps1 - Arranca el servidor del bridge en WINDOWS.
#
# Instala dependencias (npm install) SOLO cuando hace falta:
#   - primer arranque (no existe node_modules), o
#   - package.json cambio desde la ultima instalacion.
# En los demas arranques, va directo a 'node server.js'.
#
# Uso (PowerShell, desde cualquier carpeta):
#   powershell -ExecutionPolicy Bypass -File \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge\start-bridge.ps1
# o, situado en la carpeta bridge:
#   .\start-bridge.ps1
#
# Nota: este script es ASCII puro a proposito (Windows PowerShell 5.1 lee los .ps1
# sin BOM como ANSI; usar simbolos no-ASCII rompe el parser).
# =============================================================================

$ErrorActionPreference = "Stop"

# La carpeta del bridge es la de ESTE script (funciona desde cualquier cwd).
$BridgeDir = $PSScriptRoot
$pkg     = Join-Path $BridgeDir "package.json"
$lock    = Join-Path $BridgeDir "package-lock.json"
$modules = Join-Path $BridgeDir "node_modules"

function Info($m) { Write-Host ">> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "[OK] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[X] $m" -ForegroundColor Red; exit 1 }

# -- 0) Esta Node? ------------------------------------------------------------
if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
  Fail "No se encuentra 'node' en Windows. Instala Node.js (https://nodejs.org)."
}
if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {
  Fail "No se encuentra 'npm' en Windows. Viene con Node.js."
}
Info ("Node " + (node --version) + "  -  carpeta: $BridgeDir")

# Trabajar SIEMPRE desde la carpeta del bridge (npm install y node server.js
# deben ejecutarse aqui, no en el cwd desde el que se lanzo el script).
Set-Location $BridgeDir

# -- 1+2) Hace falta instalar dependencias? -----------------------------------
$needInstall = $false
$reason = ""
if (-not (Test-Path $modules)) {
  $needInstall = $true; $reason = "no existe node_modules (primer arranque)"
}
elseif (Test-Path $lock) {
  # package.json mas reciente que el lock => las dependencias cambiaron tras instalar.
  if ((Get-Item $pkg).LastWriteTime -gt (Get-Item $lock).LastWriteTime) {
    $needInstall = $true; $reason = "package.json cambio desde el ultimo install"
  }
}
elseif ((Get-Item $pkg).LastWriteTime -gt (Get-Item $modules).LastWriteTime) {
  # Sin lock: comparamos contra node_modules como aproximacion.
  $needInstall = $true; $reason = "package.json cambio desde el ultimo install"
}

if ($needInstall) {
  Warn "Instalando dependencias ($reason)..."
  & npm install
  if ($LASTEXITCODE -ne 0) { Fail "'npm install' fallo (codigo $LASTEXITCODE)." }
  Ok "Dependencias instaladas."
} else {
  Ok "Dependencias ya presentes y al dia (no se reinstala)."
}

# -- 3) Arrancar el servidor --------------------------------------------------
Info "Arrancando el servidor del bridge (Ctrl+C para parar)..."
& node server.js
