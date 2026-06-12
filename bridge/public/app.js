/* =============================================================================
 * GameStore — banco de pruebas del contrato (ethers.js v6)
 *
 * Repaso rápido (más detalle en docs/02-bridge.md):
 *  - PROVIDER = solo lectura.  SIGNER = puede firmar (la clave vive en MetaMask).
 *  - READ (view): sin gas, sin popup, usa provider.
 *  - WRITE (tx): cuesta gas, MetaMask pide firma, hay que esperar a que se mine.
 *  - payable: el ETH se envía en los overrides { value: <wei> } → llega como msg.value.
 *
 * Este bridge solo toca lo ON-CHAIN (propiedad, economía, dependencias, progreso).
 * El USO de items (gastar/beber/romper) y los slots son de Unreal: NO van aquí.
 * ===========================================================================*/

// ─────────────────────────────────────────────────────────────────────────────
//  CONFIGURACIÓN  —  ⚠️ DIRECCIONES DE LOS CONTRATOS ⚠️
// ─────────────────────────────────────────────────────────────────────────────
// GameStore: primer despliegue de la cuenta #0 de Anvil (nonce 0). Determinista.
const GAMESTORE_ADDRESS = "0x5FbDB2315678afecb367f032d93F642f64180aa3";

// Achievements: SEGUNDO despliegue de la cuenta #0 (nonce 1). También determinista.
// 👉 Si cambias el orden/los despliegues, actualiza esta dirección (la imprime
//    `forge script` y está en contracts/broadcast/.../run-latest.json).
const ACHIEVEMENTS_ADDRESS = "0xe7f1725E7734CE288F8367e1Bb143E90bb3F0512";

const RPC_URL = "http://127.0.0.1:8545";
const EXPECTED_CHAIN_ID = 31337n;

// Catálogo: ids y nombres REALES del contrato (GameStore.ESPADA()=0, etc.).
const ITEMS = [
  { id: 0, name: "Espada", emoji: "⚔️" },
  { id: 1, name: "Escudo", emoji: "🛡️" },
  { id: 2, name: "Arco", emoji: "🏹" },
  { id: 3, name: "Carcaj 5", emoji: "🎒" },
  { id: 4, name: "Carcaj 10", emoji: "🎒" },
  { id: 5, name: "Carcaj 20", emoji: "🎒" },
  { id: 6, name: "Flecha", emoji: "➶" },
  { id: 7, name: "Botella vacía", emoji: "🍶" },
  { id: 8, name: "Poción de vida", emoji: "❤️" },
  { id: 9, name: "Poción de maná", emoji: "🔷" },
];
const FLECHA_ID = 6;

// Medallones (ids en el contrato Achievements).
const MEDALS = [
  { id: 0, name: "ARQUERO", note: "soulbound · desbloquea Carcaj 20" },
  { id: 1, name: "MERCADER", note: "transferible · con rareza" },
  { id: 2, name: "COLECCIONISTA", note: "soulbound · set completo" },
];
const RARITY = ["—", "Bronce", "Plata", "Oro"];

// ─────────────────────────────────────────────────────────────────────────────
//  ESTADO
// ─────────────────────────────────────────────────────────────────────────────
let gsAbi = null;
let achAbi = null;
let signer = null;
let account = null;
let storeRead = null; // GameStore (provider) — lectura
let storeWrite = null; // GameStore (signer)  — escritura
let achRead = null; //   Achievements (provider) — lectura

const $ = (id) => document.getElementById(id);
const els = {
  connectBtn: $("connectBtn"),
  account: $("account"),
  network: $("network"),
  status: $("status"),
  store: $("store"),
  inventory: $("inventory"),
  progress: $("progress"),
};

// ─────────────────────────────────────────────────────────────────────────────
//  UI / ERRORES
// ─────────────────────────────────────────────────────────────────────────────
function showStatus(message, kind = "ok") {
  els.status.textContent = message;
  els.status.className = kind;
}
function clearStatus() {
  els.status.className = "hidden";
}

// Interfaces de ethers para decodificar custom errors a partir del selector crudo.
// Se construyen una vez (perezosamente) desde los ABIs ya cargados.
let gsIface = null;
let achIface = null;
function errorInterfaces() {
  if (!gsIface && gsAbi) gsIface = new ethers.Interface(gsAbi);
  if (!achIface && achAbi) achIface = new ethers.Interface(achAbi);
  return [gsIface, achIface].filter(Boolean);
}

/** Mensaje legible a partir del nombre del custom error y sus args. */
function messageForRevert(name, args) {
  const a = args ?? [];
  switch (name) {
    // Economía
    case "InsufficientPayment":
      return `Pago insuficiente: hacían falta ${a[0]} wei, enviaste ${a[1]} wei.`;
    case "InvalidQuantity":
      return "La cantidad debe ser mayor que cero.";
    case "ItemNotListed":
      return `Ese item no existe en el catálogo (id ${a[0]}).`;
    // Reglas de dependencia
    case "NeedBow":
      return "🏹 Necesitas un ARCO antes de comprar flechas.";
    case "QuiverCapacityExceeded":
      return `🎒 No caben: tu carcaj admite ${a[0]}, ya llevas ${a[1]} y pides ${a[2]}. Compra un carcaj mayor o usa flechas.`;
    case "NeedEmptyBottle":
      return `🍶 Necesitas ${a[0]} botella(s) vacía(s) para esa poción; tienes ${a[1]}.`;
    case "NeedQuiver5":
      return "🎒 Necesitas el Carcaj 5 antes de comprar el Carcaj 10.";
    case "NeedArcheroAchievement":
      return "🏅 El Carcaj 20 requiere el logro ARQUERO (20 flechas compradas).";
    // Otros
    case "ERC1155MissingApprovalForAll":
      return "No puedes quemar tokens que no son tuyos.";
    case "RefundFailed":
      return "Falló el reembolso del excedente.";
    case "Error": // require(cond, "mensaje")
      return a[0] ? String(a[0]) : "El contrato revirtió.";
    default:
      return name ? `El contrato revirtió: ${name}.` : null;
  }
}

/** Rebusca el revert data crudo ("0x........") en las distintas formas del error. */
function extractRevertData(err) {
  const candidates = [
    err?.revert?.data,
    err?.data,
    err?.info?.error?.data, // forma típica de MetaMask en estimateGas
    err?.error?.data,
    err?.cause?.data,
    err?.cause?.info?.error?.data,
  ];
  for (const c of candidates) {
    if (typeof c === "string" && c.startsWith("0x") && c.length >= 10) return c;
    // A veces el data viene anidado como objeto { data: "0x..." }.
    if (c && typeof c === "object" && typeof c.data === "string" && c.data.startsWith("0x")) {
      return c.data;
    }
  }
  return null;
}

/**
 * Identifica el custom error → { name, args }. Unifica los dos caminos:
 *  - err.revert.name: ethers ya lo decodificó (tx ejecutada / call de ethers).
 *  - selector crudo en err.data / err.info.error.data: lo decodificamos nosotros
 *    con el ABI (caso estimateGas vía MetaMask).
 */
function parseRevert(err) {
  if (err?.revert?.name) return { name: err.revert.name, args: err.revert.args };

  const data = extractRevertData(err);
  if (!data) return null;
  for (const iface of errorInterfaces()) {
    try {
      const parsed = iface.parseError(data); // empareja por selector de 4 bytes
      if (parsed) return { name: parsed.name, args: parsed.args };
    } catch (_) {
      /* este ABI no conoce ese selector; probamos el siguiente */
    }
  }
  return null;
}

/** Traduce errores de ethers/MetaMask/contrato a mensajes legibles. */
function humanizeError(err) {
  if (err?.code === "ACTION_REJECTED" || err?.info?.error?.code === 4001) {
    return "Has rechazado la firma en MetaMask.";
  }
  if (err?.code === "INSUFFICIENT_FUNDS") {
    return "Fondos insuficientes para pagar el item + gas.";
  }
  // Custom error, venga ya decodificado o solo como selector crudo.
  const revert = parseRevert(err);
  if (revert) {
    const msg = messageForRevert(revert.name, revert.args);
    if (msg) return msg;
  }
  if (err?.code === "NETWORK_ERROR" || /failed to fetch/i.test(err?.message || "")) {
    return "No se pudo contactar con la red. ¿Está Anvil en 127.0.0.1:8545?";
  }
  return err?.shortMessage || err?.message || "Error desconocido.";
}

// ─────────────────────────────────────────────────────────────────────────────
//  INICIALIZACIÓN
// ─────────────────────────────────────────────────────────────────────────────
async function init() {
  // ABIs extraídos de los artefactos de Foundry (incluyen los custom errors).
  gsAbi = await (await fetch("./abi/GameStore.json")).json();
  achAbi = await (await fetch("./abi/Achievements.json")).json();

  // Provider de solo lectura → leemos la tienda aunque no haya wallet conectada.
  const readProvider = new ethers.JsonRpcProvider(RPC_URL);
  storeRead = new ethers.Contract(GAMESTORE_ADDRESS, gsAbi, readProvider);
  achRead = new ethers.Contract(ACHIEVEMENTS_ADDRESS, achAbi, readProvider);

  await loadStore();

  if (!window.ethereum) {
    showStatus("No se detecta MetaMask. Instálalo para conectar tu wallet.", "warn");
    els.connectBtn.disabled = true;
  }
  els.connectBtn.addEventListener("click", connectWallet);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CONEXIÓN
// ─────────────────────────────────────────────────────────────────────────────
async function connectWallet() {
  try {
    clearStatus();
    const browserProvider = new ethers.BrowserProvider(window.ethereum);
    await browserProvider.send("eth_requestAccounts", []);
    signer = await browserProvider.getSigner();
    account = await signer.getAddress();
    storeWrite = new ethers.Contract(GAMESTORE_ADDRESS, gsAbi, signer);

    const net = await browserProvider.getNetwork();
    els.account.textContent = `Cuenta: ${account}`;
    els.network.textContent = `Red: chainId ${net.chainId}${
      net.chainId === EXPECTED_CHAIN_ID ? " (Anvil ✓)" : " (¡no es Anvil!)"
    }`;
    els.connectBtn.textContent = "Wallet conectada";
    els.connectBtn.disabled = true;

    window.ethereum.removeListener?.("accountsChanged", onWalletChange);
    window.ethereum.removeListener?.("chainChanged", onWalletChange);
    window.ethereum.on?.("accountsChanged", onWalletChange);
    window.ethereum.on?.("chainChanged", onWalletChange);

    if (net.chainId !== EXPECTED_CHAIN_ID) {
      showStatus(`⚠️ Estás en la red ${net.chainId}. Cambia a Anvil (chain id 31337).`, "warn");
    } else {
      showStatus(`Conectado a Anvil como ${account}`, "ok");
    }

    await refreshAll();
    startIntentPolling(); // empieza a atender compras pedidas por Unreal
  } catch (err) {
    showStatus(humanizeError(err), "error");
    console.error(err);
  }
}

function onWalletChange() {
  window.location.reload();
}

// ─────────────────────────────────────────────────────────────────────────────
//  CONSUMIDOR DE INTENCIONES (patrón pending → firma en web → done)
//
//  Unreal no puede firmar, así que registra una intención en el servidor. ESTA
//  web (con MetaMask) la recoge, dispara la firma y reporta el resultado. Así, el
//  juego "compra" aunque la clave nunca salga de MetaMask en el navegador.
// ─────────────────────────────────────────────────────────────────────────────
const inFlight = new Set(); // requestIds que ya estamos procesando (anti-duplicado)
let intentTimer = null;

function startIntentPolling() {
  if (intentTimer) return;
  intentTimer = setInterval(pollPending, 3000); // sondeo cada 3 s
}

async function pollPending() {
  if (!storeWrite || !account) return;
  let pending;
  try {
    pending = (await (await fetch("/api/pending")).json()).pending || [];
  } catch {
    return; // si el servidor no responde, reintentamos en el siguiente tick
  }
  for (const intent of pending) {
    if (inFlight.has(intent.requestId)) continue;
    // Solo firmamos intenciones de la cuenta CONECTADA (no las de otros jugadores).
    if (intent.address.toLowerCase() !== account.toLowerCase()) continue;
    inFlight.add(intent.requestId); // marca síncrona: evita que el siguiente tick la repita
    processIntent(intent); // sin await: cada compra sigue su curso en paralelo
  }
}

async function reportResult(requestId, status, extra = {}) {
  try {
    await fetch("/api/purchase-result", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ requestId, status, ...extra }),
    });
  } catch (e) {
    console.error("No se pudo reportar el resultado al servidor:", e);
  }
}

async function processIntent(intent) {
  const item = ITEMS.find((i) => i.id === intent.itemId) || { name: `item ${intent.itemId}` };
  try {
    await reportResult(intent.requestId, "signing"); // reclamar: deja de salir en /api/pending
    showStatus(`Unreal pidió ${intent.quantity} × ${item.name}. Confirma en MetaMask…`, "warn");

    const price = await storeRead.priceOf(intent.itemId);
    const value = price * BigInt(intent.quantity);
    const tx = await storeWrite.buy(intent.itemId, intent.quantity, { value });
    await tx.wait();

    await reportResult(intent.requestId, "done", { txHash: tx.hash });
    showStatus(`✅ Compra de Unreal completada: ${intent.quantity} × ${item.name}.`, "ok");
    await refreshAll();
  } catch (err) {
    const msg = humanizeError(err);
    await reportResult(intent.requestId, "error", { error: msg });
    showStatus(`❌ Compra de Unreal fallida (${item.name}): ${msg}`, "error");
  } finally {
    inFlight.delete(intent.requestId);
  }
}

async function refreshAll() {
  // Cada panel se refresca de forma INDEPENDIENTE: si uno falla, el otro no se ve
  // afectado. Así un problema en "progreso" nunca rompe la tienda ni el inventario.
  try {
    await loadInventory();
  } catch (err) {
    console.error("loadInventory falló:", err);
  }
  try {
    await loadProgress();
  } catch (err) {
    console.error("loadProgress falló:", err);
  }
}

/**
 * Ejecuta una lectura on-chain de forma SEGURA: si revierte (p. ej. el getter no
 * existe en el contrato desplegado, o Achievements no está conectado), no propaga
 * el error; devuelve { ok:false } para que la UI muestre "n/d" en ese campo.
 */
async function safeRead(label, fn) {
  try {
    return { ok: true, value: await fn() };
  } catch (err) {
    console.warn(`Lectura fallida (${label}):`, err?.shortMessage || err?.message || err);
    return { ok: false };
  }
}

// Formatea el resultado de safeRead: el valor (vía `fmt`) o "n/d" si falló.
function fmtRead(r, fmt = (v) => v.toString()) {
  return r.ok ? fmt(r.value) : "n/d";
}

// ─────────────────────────────────────────────────────────────────────────────
//  TIENDA  (READ priceOf/isListed + compra con CANTIDAD)
// ─────────────────────────────────────────────────────────────────────────────
async function loadStore() {
  els.store.innerHTML = "";
  for (const item of ITEMS) {
    try {
      const listed = await storeRead.isListed(item.id);
      const price = await storeRead.priceOf(item.id); // wei (BigInt)
      const priceEth = ethers.formatEther(price);

      const card = document.createElement("div");
      card.className = "card";
      card.innerHTML = `
        <div>
          <strong>${item.emoji} ${item.name}</strong> <span class="muted">(id ${item.id})</span><br/>
          <span class="muted">${listed ? `${priceEth} ETH / ud.` : "no disponible"}</span>
        </div>`;

      const actions = document.createElement("div");
      actions.className = "actions";

      // Campo de CANTIDAD (para comprar varias de golpe, p. ej. flechas).
      const qty = document.createElement("input");
      qty.type = "number";
      qty.min = "1";
      qty.value = "1";
      qty.title = "Cantidad";

      const btn = document.createElement("button");
      btn.textContent = "Comprar";
      btn.disabled = !listed;
      btn.addEventListener("click", () => buyItem(item, price, qty));

      actions.appendChild(qty);
      actions.appendChild(btn);
      card.appendChild(actions);
      els.store.appendChild(card);
    } catch (err) {
      showStatus(humanizeError(err), "error");
      console.error(err);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  COMPRAR  (WRITE payable: buy(itemId, quantity) con value = precio*cantidad)
// ─────────────────────────────────────────────────────────────────────────────
async function buyItem(item, priceWei, qtyInput) {
  if (!storeWrite) {
    showStatus("Conecta tu wallet antes de comprar.", "warn");
    return;
  }
  const quantity = BigInt(parseInt(qtyInput.value, 10) || 1);
  const value = priceWei * quantity; // coste exacto; el contrato reembolsa el excedente

  try {
    showStatus(`Confirma en MetaMask: comprar ${quantity} × ${item.name}…`, "warn");
    const tx = await storeWrite.buy(item.id, quantity, { value });
    showStatus(`Tx enviada (${tx.hash.slice(0, 10)}…). Esperando confirmación…`, "warn");
    await tx.wait();
    showStatus(`✅ Compradas ${quantity} × ${item.name}.`, "ok");
    await refreshAll();
  } catch (err) {
    showStatus(humanizeError(err), "error");
    console.error(err);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  INVENTARIO  (READ balanceOf)  +  VACIAR (WRITE burn — operación on-chain)
// ─────────────────────────────────────────────────────────────────────────────
async function loadInventory() {
  if (!account) return;
  els.inventory.innerHTML = "";
  for (const item of ITEMS) {
    try {
      const balance = await storeRead.balanceOf(account, item.id);

      const card = document.createElement("div");
      card.className = "card";
      card.innerHTML = `
        <div>
          <strong>${item.emoji} ${item.name}</strong> <span class="muted">(id ${item.id})</span><br/>
          <span class="muted">Cantidad: ${balance.toString()}</span>
        </div>`;

      const btn = document.createElement("button");
      btn.textContent = "Vaciar";
      btn.disabled = balance === 0n;
      btn.addEventListener("click", () => emptyItem(item, balance));
      card.appendChild(btn);
      els.inventory.appendChild(card);
    } catch (err) {
      showStatus(humanizeError(err), "error");
      console.error(err);
    }
  }
}

async function emptyItem(item, balance) {
  if (!storeWrite) return;
  try {
    showStatus(`Confirma en MetaMask: quemar tus ${item.name}…`, "warn");
    const tx = await storeWrite.burn(account, item.id, balance);
    await tx.wait();
    showStatus(`🔥 Vaciados tus ${item.name}.`, "ok");
    await refreshAll();
  } catch (err) {
    showStatus(humanizeError(err), "error");
    console.error(err);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  PROGRESO Y MEDALLONES  (SOLO LECTURA — para verificar reglas y logros)
// ─────────────────────────────────────────────────────────────────────────────
async function loadProgress() {
  if (!account) return;

  // Cada lectura va envuelta en safeRead: un fallo concreto muestra "n/d" en ese
  // campo, pero NO rompe el resto del panel ni el refresco general.
  // (GameStore: contadores/umbrales; Achievements: medallones y rareza.)
  const arrowsBought = await safeRead("purchasedTotal", () => storeRead.purchasedTotal(account, FLECHA_ID));
  const spent = await safeRead("totalSpent", () => storeRead.totalSpent(account));
  const capacity = await safeRead("quiverCapacity", () => storeRead.quiverCapacity(account));
  const arqueroGoal = await safeRead("ARQUERO_ARROWS", () => storeRead.ARQUERO_ARROWS());
  const mercaderGoal = await safeRead("MERCADER_SPEND_THRESHOLD", () => storeRead.MERCADER_SPEND_THRESHOLD());
  const rarity = await safeRead("mercaderRarity", () => achRead.mercaderRarity(account));

  // Medallones: cada balance por separado.
  const medalReads = {};
  for (const m of MEDALS) {
    medalReads[m.id] = await safeRead(`medal:${m.name}`, () => achRead.balanceOf(account, m.id));
  }

  const eth = (v) => `${ethers.formatEther(v)} ETH`;
  const arrowsLabel = `${fmtRead(arrowsBought)} / ${fmtRead(arqueroGoal)}`;

  const medalsHtml = MEDALS.map((m) => {
    const r = medalReads[m.id];
    let badge;
    if (!r.ok) {
      badge = "n/d";
    } else if (r.value > 0n) {
      const extra = m.id === 1 && rarity.ok ? ` — rareza ${RARITY[Number(rarity.value)] ?? "?"}` : "";
      badge = "✅ obtenido" + extra;
    } else {
      badge = "🔒 bloqueado";
    }
    return `<div class="card">
      <div><strong>${m.name}</strong> <span class="muted">(id ${m.id} · ${m.note})</span></div>
      <span class="badge">${badge}</span>
    </div>`;
  }).join("");

  els.progress.className = "";
  els.progress.innerHTML = `
    <div class="grid2" style="margin-bottom:1rem">
      <div>Flechas compradas (históricas): <strong>${arrowsLabel}</strong></div>
      <div>Capacidad de carcaj: <strong>${fmtRead(capacity)}</strong></div>
      <div>Gasto acumulado: <strong>${fmtRead(spent, eth)}</strong></div>
      <div>Umbral Mercader: <strong>${fmtRead(mercaderGoal, eth)}</strong></div>
    </div>
    ${medalsHtml}
    <p class="muted">Lectura directa de los contratos. Si ves "n/d", ese getter no
    respondió (típicamente: el contrato desplegado es una versión antigua, o
    Achievements no está desplegado/conectado). Compra para ver avanzar los
    contadores y acuñarse los medallones.</p>`;
}

window.addEventListener("DOMContentLoaded", init);
