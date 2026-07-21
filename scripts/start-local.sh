#!/usr/bin/env bash
# =============================================================================
# start-local.sh — Starts the WSL part of the project's local environment.
#
# What it does, in order:
#   1. Starts Anvil with state PERSISTENCE on disk (survives restarts).
#   2. Waits (real polling) for the RPC to respond before continuing.
#   3. Decides whether to deploy based on the contract state (see "Decision logic"):
#        - no contract        → deploy
#        - CURRENT contract   → reuse (do not redeploy)
#        - OLD contract       → reset to a clean chain and redeploy (with a warning)
#   4. Shows a summary and the remaining steps on Windows.
#   5. Handles Ctrl+C cleanly so Anvil dumps the state before dying.
#
# Decision logic (step 3):
#   "Is there bytecode at the address?" is not enough: the persistent state may
#   have an OLD version of the contract. So, if there is bytecode, we also PROBE
#   a getter that only exists in the current version (purchasedTotal).
#     · responds            → current version → reuse
#     · reverts (data 0x)   → old version → clean chain + redeploy
#   We redeploy on a clean chain (not on top of the old state) because the
#   deterministic address 0x5FbD...0aa3 is only obtained with account #0 at nonce 0.
#
# Meant to run on WSL:  bash scripts/start-local.sh
# =============================================================================

# ── bash strict mode ─────────────────────────────────────────────────────────
# -e: abort if a command fails.  -u: error if you use an undefined variable.
# -o pipefail: in a pipe, fail if any command fails, not just the last one.
set -euo pipefail

# ── PATH: in this environment forge/anvil/cast live in ~/.foundry/bin and are
#    NOT always on the PATH of a non-interactive shell. We add it explicitly. ──
export PATH="$HOME/.foundry/bin:$PATH"

# ── Locate the repo from the location of THIS script ─────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONTRACTS_DIR="$REPO_ROOT/contracts"

# ── Configuration ────────────────────────────────────────────────────────────
RPC_URL="http://127.0.0.1:8545"

# State/log folder and files (outside git; see .gitignore → .anvil/).
ANVIL_DIR="$REPO_ROOT/.anvil"
STATE_FILE="$ANVIL_DIR/state.json"
LOG_FILE="$ANVIL_DIR/anvil.log"

# DETERMINISTIC address of GameStore: first deployment of account #0 (nonce 0).
CONTRACT_ADDRESS="0x5FbDB2315678afecb367f032d93F642f64180aa3"

# Getter that ONLY exists in the current version of GameStore. If it responds, the
# deployed contract is the current one; if it reverts, it is an older version.
VERSION_PROBE_SIG="purchasedTotal(address,uint256)(uint256)"
ZERO_ADDR="0x0000000000000000000000000000000000000000"

# Anvil account #0. ⚠️ PUBLIC TEST KEY, everyone knows it.
# For local development only. NEVER use a real key or send it real funds.
DEPLOYER_PK="0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"
DEPLOY_SCRIPT="script/DeployGameStore.s.sol:DeployGameStore"

# ── Colored output helpers ───────────────────────────────────────────────────
info()  { printf '\033[36m▶ %s\033[0m\n' "$*"; }   # cyan
ok()    { printf '\033[32m✓ %s\033[0m\n' "$*"; }   # green
warn()  { printf '\033[33m! %s\033[0m\n' "$*"; }   # yellow
fail()  { printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }

# ── Reusable functions ───────────────────────────────────────────────────────

# Starts Anvil in the background with persistence and saves its PID (global).
start_anvil() {
  info "Starting Anvil (logs → $LOG_FILE) ..."
  # --state PATH       : alias of --load-state + --dump-state (loads if it exists, dumps on exit).
  # --state-interval N : dumps every N s (robustness against kill -9 / abrupt shutdown).
  anvil --state "$STATE_FILE" --state-interval 5 >"$LOG_FILE" 2>&1 &
  ANVIL_PID=$!
}

# REAL RPC polling (not a fixed sleep): queries until it responds or times out.
wait_for_rpc() {
  info "Waiting for the RPC ($RPC_URL) to be ready ..."
  local i
  for i in $(seq 1 50); do            # 50 × 0.2s ≈ 10s max
    if ! kill -0 "$ANVIL_PID" 2>/dev/null; then
      warn "Anvil terminated unexpectedly. Last lines of the log:"
      tail -n 20 "$LOG_FILE" >&2 || true
      fail "Anvil did not start correctly."
    fi
    if cast block-number --rpc-url "$RPC_URL" >/dev/null 2>&1; then
      ok "RPC ready."
      return 0
    fi
    sleep 0.2
  done
  fail "The RPC did not respond in time. Check $LOG_FILE."
}

# Stops Anvil in an orderly way (dumps the state) and waits for it to finish.
stop_anvil() {
  if [ -n "${ANVIL_PID:-}" ] && kill -0 "$ANVIL_PID" 2>/dev/null; then
    kill -TERM "$ANVIL_PID" 2>/dev/null || true
    wait "$ANVIL_PID" 2>/dev/null || true
  fi
}

# Deploys the current version of the contracts (GameStore + Achievements).
deploy_contracts() {
  info "Deploying the CURRENT version of the contracts ..."
  if ( cd "$CONTRACTS_DIR" && forge script "$DEPLOY_SCRIPT" \
        --rpc-url "$RPC_URL" \
        --private-key "$DEPLOYER_PK" \
        --broadcast >>"$LOG_FILE" 2>&1 ); then
    ok "Contracts deployed and initial catalog created."
  else
    warn "Deployment failed. Last lines of the log:"
    tail -n 30 "$LOG_FILE" >&2 || true
    fail "The forge script deployment failed."
  fi
}

# Does the deployed contract respond to the current-version getter?
# Returns 0 (success) if it responds; ≠0 if it reverts / doesn't exist.
contract_is_current() {
  cast call "$CONTRACT_ADDRESS" "$VERSION_PROBE_SIG" "$ZERO_ADDR" 0 \
    --rpc-url "$RPC_URL" >/dev/null 2>&1
}

# ── 0) Pre-checks: are the tools installed? ──────────────────────────────────
command -v anvil >/dev/null 2>&1 || fail "'anvil' not found. Is Foundry installed? Run 'foundryup'."
command -v forge >/dev/null 2>&1 || fail "'forge' not found. Is Foundry installed? Run 'foundryup'."
command -v cast  >/dev/null 2>&1 || fail "'cast' not found. Is Foundry installed? Run 'foundryup'."
[ -d "$CONTRACTS_DIR" ] || fail "The contracts folder does not exist: $CONTRACTS_DIR"

# Is there already an Anvil listening on the port? We avoid starting two.
if cast block-number --rpc-url "$RPC_URL" >/dev/null 2>&1; then
  fail "Something is already responding on $RPC_URL (another Anvil open?). Close it before starting."
fi

mkdir -p "$ANVIL_DIR"

# ── 5) Cleanup on Ctrl+C / TERM: make Anvil DUMP the state ────────────────────
# We define the trap BEFORE starting Anvil. On Ctrl+C, instead of dying abruptly,
# we send SIGTERM to Anvil and WAIT for it to dump the state to disk.
ANVIL_PID=""
cleanup() {
  echo
  if [ -n "${ANVIL_PID:-}" ] && kill -0 "$ANVIL_PID" 2>/dev/null; then
    info "Stopping Anvil and dumping state to $STATE_FILE ..."
    stop_anvil
    ok "State saved. See you next time."
  fi
  exit 0
}
trap cleanup INT TERM

# ── 1+2) Start Anvil and wait for the RPC ────────────────────────────────────
if [ -f "$STATE_FILE" ]; then
  info "Previous state found: $STATE_FILE (it will be loaded)."
else
  info "No previous state: a clean chain will be started and $STATE_FILE created."
fi
start_anvil
wait_for_rpc

# ── 3) Decide: deploy / reuse / redeploy by version ──────────────────────────
info "Checking the contract at $CONTRACT_ADDRESS ..."
CODE="$(cast code "$CONTRACT_ADDRESS" --rpc-url "$RPC_URL" 2>/dev/null || echo "0x")"

DEPLOY_STATUS=""
if [ "$CODE" = "0x" ] || [ -z "$CODE" ]; then
  # (a) No bytecode → clean chain → deploy.
  warn "No contract at that address. Deploying ..."
  deploy_contracts
  DEPLOY_STATUS="deployed now"

elif contract_is_current; then
  # (b) There is bytecode and the current getter responds → correct version → reuse.
  ok "CURRENT-version contract detected. Reusing the state (no redeploy)."
  DEPLOY_STATUS="reused (persistent state)"

else
  # (c) There is bytecode but the current getter does NOT respond → OLD version.
  warn "⚠️  Detected a PREVIOUS-VERSION contract in the persistent state."
  warn "    The inventory/purchases of that state are NOT valid for the new contract."
  warn "    Resetting to a CLEAN CHAIN and redeploying the current version ..."

  # For the redeploy to land on the same deterministic address we need account #0
  # at nonce 0, i.e. a clean chain. We stop Anvil, set the old state aside and
  # start from scratch.
  stop_anvil
  mv -f "$STATE_FILE" "$STATE_FILE.stale" 2>/dev/null || true
  warn "    (Old state saved as $STATE_FILE.stale in case you need it.)"

  start_anvil          # no state.json → clean chain, nonce 0
  wait_for_rpc
  deploy_contracts
  DEPLOY_STATUS="redeployed (current version; old state discarded)"
fi

# ── 4) Clear summary ─────────────────────────────────────────────────────────
echo
echo "──────────────────────────────────────────────────────────────"
ok   "Local environment (WSL) up and running"
echo "  • Anvil RPC:        $RPC_URL  (chain id 31337)"
echo "  • Contract:         $CONTRACT_ADDRESS"
echo "  • Contract state:   $DEPLOY_STATUS"
echo "  • State on disk:    $STATE_FILE  (persists across restarts)"
echo "  • Anvil log:        $LOG_FILE"
echo "──────────────────────────────────────────────────────────────"
echo "  Remaining steps ON WINDOWS:"
echo "   1) Start the bridge server (PowerShell):"
echo "        cd \\\\wsl.localhost\\Ubuntu\\home\\$USER\\projects\\bridge"
echo "        node server.js          # → http://localhost:8787"
echo "   2) Open http://localhost:8787 and connect MetaMask (Anvil network, 31337)."
echo "   3) (Later) start the Unreal project."
echo "──────────────────────────────────────────────────────────────"
echo "  Reminder: if you restart and MetaMask says 'nonce too high',"
echo "  use Settings → Advanced → Clear activity tab data."
echo "──────────────────────────────────────────────────────────────"
echo
info "Anvil is still running. Press Ctrl+C to STOP and dump the state."
echo

# ── Keep the script alive showing Anvil's logs live ──────────────────────────
# 'tail -f' keeps the logs visible. When you press Ctrl+C, the 'cleanup' trap
# stops Anvil in an orderly way (dumping the state).
tail -n 0 -f "$LOG_FILE" &
TAIL_PID=$!
wait "$ANVIL_PID"
kill "$TAIL_PID" 2>/dev/null || true
