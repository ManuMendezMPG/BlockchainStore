#!/usr/bin/env bash
# =============================================================================
# start-local.sh — Arranca la parte WSL del entorno local del proyecto.
#
# Qué hace, en orden:
#   1. Arranca Anvil con PERSISTENCIA de estado en disco (sobrevive a reinicios).
#   2. Espera (polling real) a que el RPC responda antes de continuar.
#   3. Decide si desplegar según el estado del contrato (ver "Lógica de decisión"):
#        - sin contrato        → desplegar
#        - contrato ACTUAL     → reutilizar (no redesplegar)
#        - contrato ANTIGUO    → reiniciar cadena limpia y redesplegar (avisando)
#   4. Muestra un resumen y los pasos que quedan en Windows.
#   5. Maneja Ctrl+C limpiamente para que Anvil vuelque el estado antes de morir.
#
# Lógica de decisión (paso 3):
#   No basta con "¿hay bytecode en la dirección?": el estado persistente puede
#   tener una versión ANTIGUA del contrato. Por eso, si hay bytecode, además
#   SONDEAMOS un getter que solo existe en la versión actual (purchasedTotal).
#     · responde            → versión actual → reutilizar
#     · revierte (data 0x)  → versión antigua → cadena limpia + redeploy
#   Redesplegamos en cadena limpia (no sobre el estado viejo) porque la dirección
#   determinista 0x5FbD...0aa3 solo se obtiene con la cuenta #0 a nonce 0.
#
# Pensado para ejecutarse en WSL:  bash scripts/start-local.sh
# =============================================================================

# ── Modo estricto de bash ────────────────────────────────────────────────────
# -e: aborta si un comando falla.  -u: error si usas una variable no definida.
# -o pipefail: en una tubería, falla si falla cualquier comando, no solo el último.
set -euo pipefail

# ── PATH: en este entorno forge/anvil/cast viven en ~/.foundry/bin y NO siempre
#    están en el PATH de un shell no interactivo. Lo añadimos explícitamente. ──
export PATH="$HOME/.foundry/bin:$PATH"

# ── Localizar el repo a partir de la ubicación de ESTE script ────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONTRACTS_DIR="$REPO_ROOT/contracts"

# ── Configuración ────────────────────────────────────────────────────────────
RPC_URL="http://127.0.0.1:8545"

# Carpeta y ficheros de estado/log (fuera de git; ver .gitignore → .anvil/).
ANVIL_DIR="$REPO_ROOT/.anvil"
STATE_FILE="$ANVIL_DIR/state.json"
LOG_FILE="$ANVIL_DIR/anvil.log"

# Dirección DETERMINISTA de GameStore: primer despliegue de la cuenta #0 (nonce 0).
CONTRACT_ADDRESS="0x5FbDB2315678afecb367f032d93F642f64180aa3"

# Getter que SOLO existe en la versión actual de GameStore. Si responde, el
# contrato desplegado es el actual; si revierte, es una versión anterior.
VERSION_PROBE_SIG="purchasedTotal(address,uint256)(uint256)"
ZERO_ADDR="0x0000000000000000000000000000000000000000"

# Cuenta #0 de Anvil. ⚠️ CLAVE PÚBLICA DE PRUEBA, la conoce todo el mundo.
# Solo para desarrollo local. NUNCA uses una clave real ni le envíes fondos reales.
DEPLOYER_PK="0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"
DEPLOY_SCRIPT="script/DeployGameStore.s.sol:DeployGameStore"

# ── Helpers de salida con color ──────────────────────────────────────────────
info()  { printf '\033[36m▶ %s\033[0m\n' "$*"; }   # cian
ok()    { printf '\033[32m✓ %s\033[0m\n' "$*"; }   # verde
warn()  { printf '\033[33m! %s\033[0m\n' "$*"; }   # amarillo
fail()  { printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }

# ── Funciones reutilizables ──────────────────────────────────────────────────

# Arranca Anvil en segundo plano con persistencia y guarda su PID (global).
start_anvil() {
  info "Arrancando Anvil (logs → $LOG_FILE) ..."
  # --state PATH       : alias de --load-state + --dump-state (carga si existe, vuelca al salir).
  # --state-interval N : vuelca cada N s (robustez ante kill -9 / cierre abrupto).
  anvil --state "$STATE_FILE" --state-interval 5 >"$LOG_FILE" 2>&1 &
  ANVIL_PID=$!
}

# Polling REAL del RPC (no un sleep fijo): consulta hasta que responde o agota.
wait_for_rpc() {
  info "Esperando a que el RPC ($RPC_URL) esté listo ..."
  local i
  for i in $(seq 1 50); do            # 50 × 0.2s ≈ 10s máximo
    if ! kill -0 "$ANVIL_PID" 2>/dev/null; then
      warn "Anvil terminó inesperadamente. Últimas líneas del log:"
      tail -n 20 "$LOG_FILE" >&2 || true
      fail "Anvil no arrancó correctamente."
    fi
    if cast block-number --rpc-url "$RPC_URL" >/dev/null 2>&1; then
      ok "RPC listo."
      return 0
    fi
    sleep 0.2
  done
  fail "El RPC no respondió a tiempo. Revisa $LOG_FILE."
}

# Para Anvil de forma ordenada (vuelca el estado) y espera a que termine.
stop_anvil() {
  if [ -n "${ANVIL_PID:-}" ] && kill -0 "$ANVIL_PID" 2>/dev/null; then
    kill -TERM "$ANVIL_PID" 2>/dev/null || true
    wait "$ANVIL_PID" 2>/dev/null || true
  fi
}

# Despliega la versión actual de los contratos (GameStore + Achievements).
deploy_contracts() {
  info "Desplegando la versión ACTUAL de los contratos ..."
  if ( cd "$CONTRACTS_DIR" && forge script "$DEPLOY_SCRIPT" \
        --rpc-url "$RPC_URL" \
        --private-key "$DEPLOYER_PK" \
        --broadcast >>"$LOG_FILE" 2>&1 ); then
    ok "Contratos desplegados y catálogo inicial creado."
  else
    warn "Fallo en el despliegue. Últimas líneas del log:"
    tail -n 30 "$LOG_FILE" >&2 || true
    fail "El despliegue con forge script falló."
  fi
}

# ¿El contrato desplegado responde al getter de la versión actual?
# Devuelve 0 (éxito) si responde; ≠0 si revierte / no existe.
contract_is_current() {
  cast call "$CONTRACT_ADDRESS" "$VERSION_PROBE_SIG" "$ZERO_ADDR" 0 \
    --rpc-url "$RPC_URL" >/dev/null 2>&1
}

# ── 0) Comprobaciones previas: ¿están las herramientas? ──────────────────────
command -v anvil >/dev/null 2>&1 || fail "No se encuentra 'anvil'. ¿Foundry instalado? Ejecuta 'foundryup'."
command -v forge >/dev/null 2>&1 || fail "No se encuentra 'forge'. ¿Foundry instalado? Ejecuta 'foundryup'."
command -v cast  >/dev/null 2>&1 || fail "No se encuentra 'cast'. ¿Foundry instalado? Ejecuta 'foundryup'."
[ -d "$CONTRACTS_DIR" ] || fail "No existe la carpeta de contratos: $CONTRACTS_DIR"

# ¿Hay ya un Anvil escuchando en el puerto? Evitamos arrancar dos.
if cast block-number --rpc-url "$RPC_URL" >/dev/null 2>&1; then
  fail "Ya hay algo respondiendo en $RPC_URL (¿otro Anvil abierto?). Ciérralo antes de arrancar."
fi

mkdir -p "$ANVIL_DIR"

# ── 5) Limpieza al recibir Ctrl+C / TERM: que Anvil VUELQUE el estado ────────
# Definimos el trap ANTES de arrancar Anvil. Al pulsar Ctrl+C, en vez de morir
# de golpe, mandamos SIGTERM a Anvil y ESPERAMOS a que vuelque el estado a disco.
ANVIL_PID=""
cleanup() {
  echo
  if [ -n "${ANVIL_PID:-}" ] && kill -0 "$ANVIL_PID" 2>/dev/null; then
    info "Parando Anvil y volcando estado a $STATE_FILE ..."
    stop_anvil
    ok "Estado guardado. Hasta la próxima."
  fi
  exit 0
}
trap cleanup INT TERM

# ── 1+2) Arrancar Anvil y esperar al RPC ─────────────────────────────────────
if [ -f "$STATE_FILE" ]; then
  info "Estado previo encontrado: $STATE_FILE (se cargará)."
else
  info "Sin estado previo: se arrancará una cadena limpia y se creará $STATE_FILE."
fi
start_anvil
wait_for_rpc

# ── 3) Decidir: desplegar / reutilizar / redesplegar por versión ─────────────
info "Comprobando el contrato en $CONTRACT_ADDRESS ..."
CODE="$(cast code "$CONTRACT_ADDRESS" --rpc-url "$RPC_URL" 2>/dev/null || echo "0x")"

DEPLOY_STATUS=""
if [ "$CODE" = "0x" ] || [ -z "$CODE" ]; then
  # (a) No hay bytecode → cadena limpia → desplegar.
  warn "No hay contrato en esa dirección. Desplegando ..."
  deploy_contracts
  DEPLOY_STATUS="desplegado ahora"

elif contract_is_current; then
  # (b) Hay bytecode y responde el getter actual → versión correcta → reutilizar.
  ok "Contrato de la versión ACTUAL detectado. Se reutiliza el estado (no se redespliega)."
  DEPLOY_STATUS="reutilizado (estado persistente)"

else
  # (c) Hay bytecode pero NO responde el getter actual → versión ANTIGUA.
  warn "⚠️  Detectado un contrato de VERSIÓN ANTERIOR en el estado persistente."
  warn "    El inventario/compras de ese estado NO son válidos para el contrato nuevo."
  warn "    Reiniciando con una CADENA LIMPIA y redesplegando la versión actual ..."

  # Para que el redeploy caiga en la misma dirección determinista hace falta la
  # cuenta #0 a nonce 0, es decir, una cadena limpia. Paramos Anvil, apartamos el
  # estado viejo y arrancamos de cero.
  stop_anvil
  mv -f "$STATE_FILE" "$STATE_FILE.stale" 2>/dev/null || true
  warn "    (Estado antiguo guardado como $STATE_FILE.stale por si lo necesitas.)"

  start_anvil          # sin state.json → cadena limpia, nonce 0
  wait_for_rpc
  deploy_contracts
  DEPLOY_STATUS="redesplegado (versión actual; estado antiguo descartado)"
fi

# ── 4) Resumen claro ─────────────────────────────────────────────────────────
echo
echo "──────────────────────────────────────────────────────────────"
ok   "Entorno local (WSL) en marcha"
echo "  • RPC Anvil:        $RPC_URL  (chain id 31337)"
echo "  • Contrato:         $CONTRACT_ADDRESS"
echo "  • Estado contrato:  $DEPLOY_STATUS"
echo "  • Estado en disco:  $STATE_FILE  (persiste entre reinicios)"
echo "  • Log de Anvil:     $LOG_FILE"
echo "──────────────────────────────────────────────────────────────"
echo "  Pasos que quedan EN WINDOWS:"
echo "   1) Arrancar el servidor del puente (PowerShell):"
echo "        cd \\\\wsl.localhost\\Ubuntu\\home\\$USER\\projects\\bridge"
echo "        node server.js          # → http://localhost:8787"
echo "   2) Abrir http://localhost:8787 y conectar MetaMask (red Anvil, 31337)."
echo "   3) (Más adelante) arrancar el proyecto de Unreal."
echo "──────────────────────────────────────────────────────────────"
echo "  Recordatorio: si reinicias y MetaMask da 'nonce too high',"
echo "  usa Configuración → Avanzado → Borrar datos de actividad."
echo "──────────────────────────────────────────────────────────────"
echo
info "Anvil sigue corriendo. Pulsa Ctrl+C para PARAR y volcar el estado."
echo

# ── Mantener el script vivo mostrando los logs de Anvil en directo ───────────
# 'tail -f' deja los logs visibles. Cuando pulses Ctrl+C, el trap 'cleanup' para
# Anvil de forma ordenada (volcando el estado).
tail -n 0 -f "$LOG_FILE" &
TAIL_PID=$!
wait "$ANVIL_PID"
kill "$TAIL_PID" 2>/dev/null || true
