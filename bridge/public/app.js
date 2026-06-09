/* =============================================================================
 * GameStore — lógica del puente web (ethers.js v6)
 *
 * Conceptos clave (lee esto antes que el código):
 *
 *  - PROVIDER: conexión de SOLO LECTURA a la blockchain. Sirve para consultar
 *    estado (balances, precios, bloques). No puede firmar nada.
 *      · BrowserProvider(window.ethereum): habla a través de MetaMask.
 *      · JsonRpcProvider(url): habla directamente con un nodo RPC (aquí Anvil).
 *
 *  - SIGNER: representa UNA CUENTA concreta y puede FIRMAR transacciones.
 *    Lo obtenemos de MetaMask con provider.getSigner(). La clave privada vive
 *    dentro de MetaMask: el signer solo pide firmas; nunca vemos la clave.
 *
 *  - READ (llamada / "call"): ejecutar una función `view` no cuesta gas, no mina
 *    bloque y no abre MetaMask. Solo lee. Usa un PROVIDER.
 *      ej: await contract.priceOf(0)
 *
 *  - WRITE (transacción): cambia estado on-chain, cuesta gas, MetaMask pide firma
 *    y hay que ESPERAR a que se mine. Usa un SIGNER.
 *      ej: const tx = await contract.buy(0, 1, { value: precio });
 *          await tx.wait();   // esperamos confirmación
 *
 *  - payable + msg.value: para enviar ETH junto a una llamada (como `buy`), se
 *    pasa un objeto de overrides con `{ value: <wei> }`. Ese value llega al
 *    contrato como msg.value.
 * ===========================================================================*/

// ─────────────────────────────────────────────────────────────────────────────
//  CONFIGURACIÓN  —  ⚠️ DIRECCIÓN DEL CONTRATO ⚠️
// ─────────────────────────────────────────────────────────────────────────────
// Dirección de GameStore desplegado en Anvil. Es DETERMINISTA: el primer contrato
// que despliega la cuenta #0 de Anvil (nonce 0) siempre cae en esta dirección.
//
// 👉 CÓMO ACTUALIZARLA si cambia (p. ej. si reinicias Anvil y haces más despliegues,
//    o despliegas desde otra cuenta):
//    1. Mira la salida de `forge script ... --broadcast` ("GameStore desplegado en: 0x...").
//    2. O léela de contracts/broadcast/DeployGameStore.s.sol/31337/run-latest.json.
//    3. Pega aquí la nueva dirección.
const CONTRACT_ADDRESS = "0x5FbDB2315678afecb367f032d93F642f64180aa3";

// Red local Anvil.
const RPC_URL = "http://127.0.0.1:8545";
const EXPECTED_CHAIN_ID = 31337n; // BigInt: en ethers v6 los chainId son BigInt.

// Catálogo a mostrar (los precios reales se LEEN del contrato, no se hardcodean).
const ITEMS = [
  { id: 0, name: "Espada", emoji: "⚔️" },
  { id: 1, name: "Escudo", emoji: "🛡️" },
  { id: 2, name: "Poción", emoji: "🧪" },
];

// ─────────────────────────────────────────────────────────────────────────────
//  ESTADO GLOBAL
// ─────────────────────────────────────────────────────────────────────────────
let abi = null; // ABI cargado desde ./abi/GameStore.json
let signer = null; // cuenta firmante (tras conectar MetaMask)
let account = null; // dirección conectada
let readContract = null; // contrato conectado a un provider (solo lectura)
let writeContract = null; // contrato conectado al signer (lectura + escritura)

// Atajos al DOM.
const $ = (id) => document.getElementById(id);
const els = {
  connectBtn: $("connectBtn"),
  account: $("account"),
  network: $("network"),
  status: $("status"),
  store: $("store"),
  inventory: $("inventory"),
};

// ─────────────────────────────────────────────────────────────────────────────
//  UTILIDADES DE UI / ERRORES
// ─────────────────────────────────────────────────────────────────────────────
function showStatus(message, kind = "ok") {
  els.status.textContent = message;
  els.status.className = kind; // ok | warn | error
}
function clearStatus() {
  els.status.className = "hidden";
}

/**
 * Traduce los errores de ethers/MetaMask a mensajes claros para el usuario.
 * ethers v6 normaliza muchos errores con un `code` y, para reverts con custom
 * errors (gracias a que el ABI los incluye), rellena `error.revert`.
 */
function humanizeError(err) {
  // El usuario cerró/rechazó el popup de firma en MetaMask.
  if (err?.code === "ACTION_REJECTED" || err?.info?.error?.code === 4001) {
    return "Has rechazado la firma en MetaMask.";
  }
  // La cuenta no tiene ETH suficiente para el value + gas.
  if (err?.code === "INSUFFICIENT_FUNDS") {
    return "Fondos insuficientes en tu cuenta para pagar el item + gas.";
  }
  // Revert con custom error decodificado por el ABI.
  if (err?.revert?.name) {
    const r = err.revert;
    switch (r.name) {
      case "InsufficientPayment":
        return `Pago insuficiente: el contrato pedía ${r.args?.[0]} wei.`;
      case "ItemNotListed":
        return `Ese item no existe en el catálogo (id ${r.args?.[0]}).`;
      case "InvalidQuantity":
        return "La cantidad debe ser mayor que cero.";
      case "ERC1155MissingApprovalForAll":
        return "No puedes quemar tokens que no son tuyos.";
      default:
        return `El contrato revirtió: ${r.name}.`;
    }
  }
  // No se pudo contactar con el nodo (Anvil apagado, etc.).
  if (err?.code === "NETWORK_ERROR" || /failed to fetch/i.test(err?.message || "")) {
    return "No se pudo contactar con la red. ¿Está Anvil arrancado en 127.0.0.1:8545?";
  }
  return err?.shortMessage || err?.message || "Error desconocido.";
}

// ─────────────────────────────────────────────────────────────────────────────
//  INICIALIZACIÓN
// ─────────────────────────────────────────────────────────────────────────────
async function init() {
  // 1) Cargamos el ABI (extraído de los artefactos de Foundry).
  abi = await (await fetch("./abi/GameStore.json")).json();

  // 2) Provider de SOLO LECTURA directo a Anvil. Nos permite leer los precios
  //    de la tienda AUNQUE el usuario todavía no haya conectado MetaMask.
  const readProvider = new ethers.JsonRpcProvider(RPC_URL);
  readContract = new ethers.Contract(CONTRACT_ADDRESS, abi, readProvider);

  // 3) Pintamos la tienda (lecturas).
  await loadStore();

  // 4) ¿Hay MetaMask?
  if (!window.ethereum) {
    showStatus("No se detecta MetaMask. Instálalo para conectar tu wallet.", "warn");
    els.connectBtn.disabled = true;
  }

  els.connectBtn.addEventListener("click", connectWallet);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CONEXIÓN CON METAMASK
// ─────────────────────────────────────────────────────────────────────────────
async function connectWallet() {
  try {
    clearStatus();
    // BrowserProvider envuelve el objeto EIP-1193 que inyecta MetaMask.
    const browserProvider = new ethers.BrowserProvider(window.ethereum);

    // Pide permiso para acceder a las cuentas → abre el popup de MetaMask.
    await browserProvider.send("eth_requestAccounts", []);

    // El signer es la cuenta conectada; con él se firman transacciones.
    signer = await browserProvider.getSigner();
    account = await signer.getAddress();

    // Contrato conectado al SIGNER → puede leer Y escribir (buy/burn).
    writeContract = new ethers.Contract(CONTRACT_ADDRESS, abi, signer);

    // Comprobamos la red.
    const net = await browserProvider.getNetwork();
    renderAccount(net);

    // Reaccionar a cambios en MetaMask (cuenta o red) recargando.
    window.ethereum.removeListener?.("accountsChanged", onWalletChange);
    window.ethereum.removeListener?.("chainChanged", onWalletChange);
    window.ethereum.on?.("accountsChanged", onWalletChange);
    window.ethereum.on?.("chainChanged", onWalletChange);

    if (net.chainId !== EXPECTED_CHAIN_ID) {
      showStatus(
        `⚠️ Estás en la red ${net.chainId}. Cambia a Anvil (chain id 31337, RPC ${RPC_URL}).`,
        "warn"
      );
      // Aun así mostramos la tienda; las escrituras fallarán hasta cambiar de red.
    } else {
      showStatus(`Conectado a Anvil (31337) como ${account}`, "ok");
    }

    await loadInventory();
  } catch (err) {
    showStatus(humanizeError(err), "error");
    console.error(err);
  }
}

function onWalletChange() {
  // Lo más simple y robusto: recargar la página al cambiar cuenta o red.
  window.location.reload();
}

function renderAccount(net) {
  els.account.textContent = `Cuenta: ${account}`;
  els.network.textContent = `Red: chainId ${net.chainId}${
    net.chainId === EXPECTED_CHAIN_ID ? " (Anvil ✓)" : " (¡no es Anvil!)"
  }`;
  els.connectBtn.textContent = "Wallet conectada";
  els.connectBtn.disabled = true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  TIENDA  (READ: priceOf / isListed)
// ─────────────────────────────────────────────────────────────────────────────
async function loadStore() {
  els.store.innerHTML = "";
  for (const item of ITEMS) {
    try {
      // Lecturas: no cuestan gas, no abren MetaMask. Devuelven BigInt/bool.
      const listed = await readContract.isListed(item.id);
      const price = await readContract.priceOf(item.id); // wei (BigInt)
      const priceEth = ethers.formatEther(price); // wei → "0.01"

      const card = document.createElement("div");
      card.className = "card";
      card.innerHTML = `
        <div>
          <strong>${item.emoji} ${item.name}</strong> <span class="muted">(id ${item.id})</span><br/>
          <span class="muted">${listed ? `${priceEth} ETH` : "no disponible"}</span>
        </div>`;

      const btn = document.createElement("button");
      btn.textContent = "Comprar";
      btn.disabled = !listed;
      btn.addEventListener("click", () => buyItem(item, price));
      card.appendChild(btn);
      els.store.appendChild(card);
    } catch (err) {
      showStatus(humanizeError(err), "error");
      console.error(err);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  COMPRAR  (WRITE payable: buy(itemId, 1) con value = precio)
// ─────────────────────────────────────────────────────────────────────────────
async function buyItem(item, priceWei) {
  if (!writeContract) {
    showStatus("Conecta tu wallet antes de comprar.", "warn");
    return;
  }
  try {
    showStatus(`Confirma en MetaMask la compra de ${item.name}…`, "warn");

    // Construcción de la transacción payable:
    //   - args de la función: (itemId, quantity)
    //   - overrides: { value } → se envía como msg.value (en wei).
    // Al llamar, MetaMask abre el popup para firmar.
    const tx = await writeContract.buy(item.id, 1, { value: priceWei });

    showStatus(`Transacción enviada (${tx.hash.slice(0, 10)}…). Esperando confirmación…`, "warn");

    // Esperamos a que se mine. En Anvil es instantáneo.
    await tx.wait();

    showStatus(`✅ Has comprado 1 ${item.name}.`, "ok");
    await loadInventory(); // refrescamos el inventario tras la compra
  } catch (err) {
    showStatus(humanizeError(err), "error");
    console.error(err);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  INVENTARIO  (READ: balanceOf)  +  VACIAR  (WRITE: burn)
// ─────────────────────────────────────────────────────────────────────────────
async function loadInventory() {
  if (!account) return;
  els.inventory.innerHTML = "";

  for (const item of ITEMS) {
    try {
      // balanceOf(cuenta, id) → cuántas unidades posee el jugador (BigInt).
      const balance = await readContract.balanceOf(account, item.id);

      const card = document.createElement("div");
      card.className = "card";
      card.innerHTML = `
        <div>
          <strong>${item.emoji} ${item.name}</strong> <span class="muted">(id ${item.id})</span><br/>
          <span class="muted">Cantidad: ${balance.toString()}</span>
        </div>`;

      const btn = document.createElement("button");
      btn.textContent = "Vaciar";
      btn.disabled = balance === 0n; // nada que quemar
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
  if (!writeContract) return;
  try {
    showStatus(`Confirma en MetaMask la quema de tus ${item.name}…`, "warn");

    // burn(account, id, value): quemamos TODO el balance de ese item.
    // No es payable → no lleva { value }. El jugador quema sus propios tokens.
    const tx = await writeContract.burn(account, item.id, balance);

    showStatus(`Transacción enviada (${tx.hash.slice(0, 10)}…). Esperando confirmación…`, "warn");
    await tx.wait();

    showStatus(`🔥 Has vaciado tus ${item.name}.`, "ok");
    await loadInventory();
  } catch (err) {
    showStatus(humanizeError(err), "error");
    console.error(err);
  }
}

// Arrancamos cuando el DOM está listo.
window.addEventListener("DOMContentLoaded", init);
