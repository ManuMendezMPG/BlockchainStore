#!/usr/bin/env bash
# =============================================================================
# start-local.sh — Arranca la parte WSL del entorno local del proyecto.
#
# Qué hace, en orden:
#   1. Arranca Anvil con PERSISTENCIA de estado en disco (sobrevive a reinicios).
#   2. Espera (polling real) a que el RPC responda antes de continuar.
#   3. Despliega el contrato SOLO si no está ya desplegado (estado persistente).
#   4. Muestra un resumen y los pasos que quedan en Windows.
#   5. Maneja Ctrl+C limpiamente para que Anvil vuelque el estado antes de morir.
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
# Así el script funciona aunque lo lances desde otra carpeta.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONTRACTS_DIR="$REPO_ROOT/contracts"

# ── Configuración ────────────────────────────────────────────────────────────
RPC_URL="http://127.0.0.1:8545"

# Carpeta y ficheros de estado/log (fuera de git; ver .gitignore → .anvil/).
ANVIL_DIR="$REPO_ROOT/.anvil"
STATE_FILE="$ANVIL_DIR/state.json"
LOG_FILE="$ANVIL_DIR/anvil.log"

# Dirección DETERMINISTA del contrato: primer despliegue de la cuenta #0 (nonce 0).
CONTRACT_ADDRESS="0x5FbDB2315678afecb367f032d93F642f64180aa3"

# Cuenta #0 de Anvil. ⚠️ CLAVE PÚBLICA DE PRUEBA, la conoce todo el mundo.
# Solo para desarrollo local. NUNCA uses una clave real ni le envíes fondos reales.
DEPLOYER_PK="0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"
DEPLOY_SCRIPT="script/DeployGameStore.s.sol:DeployGameStore"

# ── Helpers de salida con color ──────────────────────────────────────────────
info()  { printf '\033[36m▶ %s\033[0m\n' "$*"; }   # cian
ok()    { printf '\033[32m✓ %s\033[0m\n' "$*"; }   # verde
warn()  { printf '\033[33m! %s\033[0m\n' "$*"; }   # amarillo
fail()  { printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }

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
# de golpe, mandamos SIGTERM a Anvil y ESPERAMOS a que termine: con --state,
# Anvil vuelca el estado a disco durante su apagado ordenado.
ANVIL_PID=""
cleanup() {
  echo
  if [ -n "$ANVIL_PID" ] && kill -0 "$ANVIL_PID" 2>/dev/null; then
    info "Parando Anvil y volcando estado a $STATE_FILE ..."
    kill -TERM "$ANVIL_PID" 2>/dev/null || true
    wait "$ANVIL_PID" 2>/dev/null || true   # esperar al volcado ordenado
    ok "Estado guardado. Hasta la próxima."
  fi
  exit 0
}
trap cleanup INT TERM

# ── 1) Arrancar Anvil con persistencia ───────────────────────────────────────
# --state PATH        : alias de --load-state + --dump-state. Carga el estado del
#                       fichero si existe, y lo vuelca al salir.
# --state-interval N  : además, vuelca cada N segundos (robustez si el proceso
#                       muere de forma abrupta, p. ej. kill -9 o cierre del PC).
if [ -f "$STATE_FILE" ]; then
  info "Estado previo encontrado: $STATE_FILE (se cargará)."
else
  info "Sin estado previo: se arrancará una cadena limpia y se creará $STATE_FILE."
fi

info "Arrancando Anvil (logs → $LOG_FILE) ..."
# En segundo plano (&), redirigiendo stdout y stderr al log. Guardamos su PID.
anvil --state "$STATE_FILE" --state-interval 5 >"$LOG_FILE" 2>&1 &
ANVIL_PID=$!

# ── 2) Esperar a que el RPC responda DE VERDAD (polling, no sleep fijo) ──────
# Un 'sleep 3' es frágil: a veces Anvil tarda más, a veces menos. En su lugar
# consultamos el RPC en bucle hasta que conteste, con un límite de intentos.
info "Esperando a que el RPC ($RPC_URL) esté listo ..."
READY=0
for i in $(seq 1 50); do            # 50 intentos × 0.2s ≈ 10s máximo
  # ¿Sigue vivo el proceso de Anvil? Si murió, no tiene sentido seguir esperando.
  if ! kill -0 "$ANVIL_PID" 2>/dev/null; then
    warn "Anvil terminó inesperadamente. Últimas líneas del log:"
    tail -n 20 "$LOG_FILE" >&2 || true
    fail "Anvil no arrancó correctamente."
  fi
  # cast block-number devuelve 0 si el RPC responde; si no, reintenta.
  if cast block-number --rpc-url "$RPC_URL" >/dev/null 2>&1; then
    READY=1
    break
  fi
  sleep 0.2
done
[ "$READY" -eq 1 ] || fail "El RPC no respondió a tiempo. Revisa $LOG_FILE."
ok "RPC listo."

# ── 3) Desplegar SOLO si hace falta ──────────────────────────────────────────
# 'cast code <addr>' devuelve el bytecode desplegado en esa dirección, o '0x' si
# no hay nada. Si ya hay código → el contrato sobrevivió en el estado persistente
# y NO debemos redeployar (duplicaría el contrato y machacaría el inventario).
info "Comprobando si el contrato ya existe en $CONTRACT_ADDRESS ..."
CODE="$(cast code "$CONTRACT_ADDRESS" --rpc-url "$RPC_URL" 2>/dev/null || echo "0x")"

DEPLOY_STATUS=""
if [ "$CODE" != "0x" ] && [ -n "$CODE" ]; then
  # Ya hay contrato → reutilizamos el estado.
  DEPLOY_STATUS="reutilizado (estado persistente)"
  ok "Contrato ya desplegado. Se reutiliza el inventario existente (no se redespliega)."
else
  # Cadena limpia o estado nuevo → desplegamos.
  warn "No hay contrato en esa dirección. Desplegando ..."
  # Ejecutamos forge script desde la carpeta de contratos (necesita foundry.toml).
  if ( cd "$CONTRACTS_DIR" && forge script "$DEPLOY_SCRIPT" \
        --rpc-url "$RPC_URL" \
        --private-key "$DEPLOYER_PK" \
        --broadcast >>"$LOG_FILE" 2>&1 ); then
    DEPLOY_STATUS="desplegado ahora"
    ok "Contrato desplegado y catálogo inicial creado."
  else
    warn "Fallo en el despliegue. Últimas líneas del log:"
    tail -n 30 "$LOG_FILE" >&2 || true
    fail "El despliegue con forge script falló."
  fi
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
# 'tail -f' deja los logs visibles. Cuando pulses Ctrl+C, el trap 'cleanup' se
# encarga de parar Anvil de forma ordenada (volcando el estado).
tail -n 0 -f "$LOG_FILE" &
TAIL_PID=$!
# Esperamos a que Anvil termine (o a que Ctrl+C dispare el trap).
wait "$ANVIL_PID"
# Si Anvil terminó por su cuenta, paramos el tail y limpiamos.
kill "$TAIL_PID" 2>/dev/null || true
