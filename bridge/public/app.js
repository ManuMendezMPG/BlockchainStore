/* =============================================================================
 * GameStore — contract test bench (ethers.js v6)
 *
 * Quick recap (more detail in docs/02-bridge.md):
 *  - PROVIDER = read only.  SIGNER = can sign (the key lives in MetaMask).
 *  - READ (view): no gas, no popup, uses the provider.
 *  - WRITE (tx): costs gas, MetaMask asks for a signature, you must wait for it to be mined.
 *  - payable: the ETH is sent in the overrides { value: <wei> } → arrives as msg.value.
 *
 * This bridge only touches what is ON-CHAIN (ownership, economy, dependencies, progress).
 * The USE of items (spending/drinking/breaking) and the slots belong to Unreal: NOT here.
 * ===========================================================================*/

// ─────────────────────────────────────────────────────────────────────────────
//  CONFIGURATION  —  ⚠️ CONTRACT ADDRESSES ⚠️
// ─────────────────────────────────────────────────────────────────────────────
// GameStore: first deployment of Anvil account #0 (nonce 0). Deterministic.
const GAMESTORE_ADDRESS = "0x5FbDB2315678afecb367f032d93F642f64180aa3";

// Achievements: SECOND deployment of account #0 (nonce 1). Also deterministic.
// 👉 If you change the deployment order/count, update this address (it is printed by
//    `forge script` and lives in contracts/broadcast/.../run-latest.json).
const ACHIEVEMENTS_ADDRESS = "0xe7f1725E7734CE288F8367e1Bb143E90bb3F0512";

const RPC_URL = "http://127.0.0.1:8545";
const EXPECTED_CHAIN_ID = 31337n;

// Catalog: REAL ids and names from the contract (GameStore.ESPADA()=0, etc.).
const ITEMS = [
  { id: 0, name: "Sword", emoji: "⚔️" },
  { id: 1, name: "Shield", emoji: "🛡️" },
  { id: 2, name: "Bow", emoji: "🏹" },
  { id: 3, name: "Quiver 5", emoji: "🎒" },
  { id: 4, name: "Quiver 10", emoji: "🎒" },
  { id: 5, name: "Quiver 20", emoji: "🎒" },
  { id: 6, name: "Arrow", emoji: "➶" },
  { id: 7, name: "Empty bottle", emoji: "🍶" },
  { id: 8, name: "Health potion", emoji: "❤️" },
  { id: 9, name: "Mana potion", emoji: "🔷" },
];
const FLECHA_ID = 6;

// Medallions (ids in the Achievements contract).
const MEDALS = [
  { id: 0, name: "Archer", note: "soulbound · unlocks Quiver 20" },
  { id: 1, name: "Merchant", note: "transferable · with rarity" },
  { id: 2, name: "Collector", note: "soulbound · full set" },
];
const RARITY = ["—", "Bronze", "Silver", "Gold"];

// ─────────────────────────────────────────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────────────────────────────────────────
let gsAbi = null;
let achAbi = null;
let signer = null;
let account = null;
let storeRead = null; // GameStore (provider) — read
let storeWrite = null; // GameStore (signer)  — write
let achRead = null; //   Achievements (provider) — read

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
//  UI / ERRORS
// ─────────────────────────────────────────────────────────────────────────────
function showStatus(message, kind = "ok") {
  els.status.textContent = message;
  els.status.className = kind;
}
function clearStatus() {
  els.status.className = "hidden";
}

// ethers interfaces to decode custom errors from the raw selector.
// They are built once (lazily) from the already-loaded ABIs.
let gsIface = null;
let achIface = null;
function errorInterfaces() {
  if (!gsIface && gsAbi) gsIface = new ethers.Interface(gsAbi);
  if (!achIface && achAbi) achIface = new ethers.Interface(achAbi);
  return [gsIface, achIface].filter(Boolean);
}

/** Readable message from the custom error name and its args. */
function messageForRevert(name, args) {
  const a = args ?? [];
  switch (name) {
    // Economy
    case "InsufficientPayment":
      return `Insufficient payment: ${a[0]} wei were required, you sent ${a[1]} wei.`;
    case "InvalidQuantity":
      return "The quantity must be greater than zero.";
    case "ItemNotListed":
      return `That item does not exist in the catalog (id ${a[0]}).`;
    // Dependency rules
    case "NeedBow":
      return "🏹 You need a BOW before buying arrows.";
    case "QuiverCapacityExceeded":
      return `🎒 They don't fit: your quiver holds ${a[0]}, you already carry ${a[1]} and you ask for ${a[2]}. Buy a bigger quiver or use arrows.`;
    case "NeedEmptyBottle":
      return `🍶 You need ${a[0]} empty bottle(s) for that potion; you have ${a[1]}.`;
    case "NeedQuiver5":
      return "🎒 You need Quiver 5 before buying Quiver 10.";
    case "NeedArcheroAchievement":
      return "🏅 Quiver 20 requires the Archer achievement (20 arrows purchased).";
    // Others
    case "ERC1155MissingApprovalForAll":
      return "You can't burn tokens that aren't yours.";
    case "RefundFailed":
      return "The excess refund failed.";
    case "Error": // require(cond, "message")
      return a[0] ? String(a[0]) : "The contract reverted.";
    default:
      return name ? `The contract reverted: ${name}.` : null;
  }
}

/** Digs out the raw revert data ("0x........") from the different error shapes. */
function extractRevertData(err) {
  const candidates = [
    err?.revert?.data,
    err?.data,
    err?.info?.error?.data, // typical MetaMask shape on estimateGas
    err?.error?.data,
    err?.cause?.data,
    err?.cause?.info?.error?.data,
  ];
  for (const c of candidates) {
    if (typeof c === "string" && c.startsWith("0x") && c.length >= 10) return c;
    // Sometimes the data comes nested as an object { data: "0x..." }.
    if (c && typeof c === "object" && typeof c.data === "string" && c.data.startsWith("0x")) {
      return c.data;
    }
  }
  return null;
}

/**
 * Identifies the custom error → { name, args }. Unifies the two paths:
 *  - err.revert.name: ethers already decoded it (executed tx / ethers call).
 *  - raw selector in err.data / err.info.error.data: we decode it ourselves
 *    with the ABI (the estimateGas via MetaMask case).
 */
function parseRevert(err) {
  if (err?.revert?.name) return { name: err.revert.name, args: err.revert.args };

  const data = extractRevertData(err);
  if (!data) return null;
  for (const iface of errorInterfaces()) {
    try {
      const parsed = iface.parseError(data); // matches by 4-byte selector
      if (parsed) return { name: parsed.name, args: parsed.args };
    } catch (_) {
      /* this ABI doesn't know that selector; we try the next one */
    }
  }
  return null;
}

/** Translates ethers/MetaMask/contract errors into readable messages. */
function humanizeError(err) {
  if (err?.code === "ACTION_REJECTED" || err?.info?.error?.code === 4001) {
    return "You rejected the signature in MetaMask.";
  }
  if (err?.code === "INSUFFICIENT_FUNDS") {
    return "Insufficient funds to pay for the item + gas.";
  }
  // Custom error, whether already decoded or only as a raw selector.
  const revert = parseRevert(err);
  if (revert) {
    const msg = messageForRevert(revert.name, revert.args);
    if (msg) return msg;
  }
  if (err?.code === "NETWORK_ERROR" || /failed to fetch/i.test(err?.message || "")) {
    return "Could not reach the network. Is Anvil running on 127.0.0.1:8545?";
  }
  return err?.shortMessage || err?.message || "Unknown error.";
}

// ─────────────────────────────────────────────────────────────────────────────
//  INITIALIZATION
// ─────────────────────────────────────────────────────────────────────────────
async function init() {
  // ABIs extracted from the Foundry artifacts (they include the custom errors).
  gsAbi = await (await fetch("./abi/GameStore.json")).json();
  achAbi = await (await fetch("./abi/Achievements.json")).json();

  // Read-only provider → we read the store even without a connected wallet.
  const readProvider = new ethers.JsonRpcProvider(RPC_URL);
  storeRead = new ethers.Contract(GAMESTORE_ADDRESS, gsAbi, readProvider);
  achRead = new ethers.Contract(ACHIEVEMENTS_ADDRESS, achAbi, readProvider);

  await loadStore();

  if (!window.ethereum) {
    showStatus("MetaMask not detected. Install it to connect your wallet.", "warn");
    els.connectBtn.disabled = true;
  }
  els.connectBtn.addEventListener("click", connectWallet);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CONNECTION
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
    els.account.textContent = `Account: ${account}`;
    els.network.textContent = `Network: chainId ${net.chainId}${
      net.chainId === EXPECTED_CHAIN_ID ? " (Anvil ✓)" : " (not Anvil!)"
    }`;
    els.connectBtn.textContent = "Wallet connected";
    els.connectBtn.disabled = true;

    window.ethereum.removeListener?.("accountsChanged", onWalletChange);
    window.ethereum.removeListener?.("chainChanged", onWalletChange);
    window.ethereum.on?.("accountsChanged", onWalletChange);
    window.ethereum.on?.("chainChanged", onWalletChange);

    if (net.chainId !== EXPECTED_CHAIN_ID) {
      showStatus(`⚠️ You are on network ${net.chainId}. Switch to Anvil (chain id 31337).`, "warn");
    } else {
      showStatus(`Connected to Anvil as ${account}`, "ok");
    }

    await refreshAll();
    startIntentPolling(); // start handling purchases requested by Unreal
  } catch (err) {
    showStatus(humanizeError(err), "error");
    console.error(err);
  }
}

function onWalletChange() {
  window.location.reload();
}

// ─────────────────────────────────────────────────────────────────────────────
//  INTENT CONSUMER (pending → sign in web → done pattern)
//
//  Unreal cannot sign, so it registers an intent on the server. THIS web (with
//  MetaMask) picks it up, triggers the signature and reports the result. That way
//  the game "buys" even though the key never leaves MetaMask in the browser.
// ─────────────────────────────────────────────────────────────────────────────
const inFlight = new Set(); // requestIds we are already processing (anti-duplicate)
let intentTimer = null;

function startIntentPolling() {
  if (intentTimer) return;
  // Every 3 s we handle both purchases and pending SIWE logins from Unreal.
  intentTimer = setInterval(() => {
    pollPending();
    pollLogins();
  }, 3000);
}

async function pollPending() {
  if (!storeWrite || !account) return;
  let pending;
  try {
    pending = (await (await fetch("/api/pending")).json()).pending || [];
  } catch {
    return; // if the server doesn't respond, we retry on the next tick
  }
  for (const intent of pending) {
    if (inFlight.has(intent.requestId)) continue;
    // We only sign intents for the CONNECTED account (not those of other players).
    if (intent.address.toLowerCase() !== account.toLowerCase()) continue;
    inFlight.add(intent.requestId); // synchronous mark: prevents the next tick from repeating it
    processIntent(intent); // no await: each purchase proceeds in parallel
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
    console.error("Could not report the result to the server:", e);
  }
}

async function processIntent(intent) {
  const item = ITEMS.find((i) => i.id === intent.itemId) || { name: `item ${intent.itemId}` };
  try {
    await reportResult(intent.requestId, "signing"); // claim: stops appearing in /api/pending
    showStatus(`Unreal requested ${intent.quantity} × ${item.name}. Confirm in MetaMask…`, "warn");

    const price = await storeRead.priceOf(intent.itemId);
    const value = price * BigInt(intent.quantity);
    const tx = await storeWrite.buy(intent.itemId, intent.quantity, { value });
    await tx.wait();

    await reportResult(intent.requestId, "done", { txHash: tx.hash });
    showStatus(`✅ Unreal purchase completed: ${intent.quantity} × ${item.name}.`, "ok");
    await refreshAll();
  } catch (err) {
    const msg = humanizeError(err);
    await reportResult(intent.requestId, "error", { error: msg });
    showStatus(`❌ Unreal purchase failed (${item.name}): ${msg}`, "error");
  } finally {
    inFlight.delete(intent.requestId);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SIWE LOGIN CONSUMER (signing a MESSAGE, no gas)
//
//  Unlike buying (signs a TRANSACTION), SIWE login signs an EIP-4361 MESSAGE
//  with personal_sign: it proves ownership of the wallet without gas or touching
//  the chain. The bridge builds the message, the web signs it, the bridge
//  verifies it (ecrecover).
// ─────────────────────────────────────────────────────────────────────────────
const loginInFlight = new Set();

async function pollLogins() {
  if (!signer || !account) return;
  let pending;
  try {
    pending = (await (await fetch("/api/siwe/pending")).json()).pending || [];
  } catch {
    return;
  }
  for (const s of pending) {
    if (loginInFlight.has(s.requestId)) continue;
    if (s.address.toLowerCase() !== account.toLowerCase()) continue; // only our account
    loginInFlight.add(s.requestId);
    processLogin(s);
  }
}

async function reportLogin(requestId, payload) {
  await fetch("/api/siwe/verify", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ requestId, ...payload }),
  });
}

async function processLogin(session) {
  try {
    showStatus("Unreal requested a login. Sign the MESSAGE in MetaMask (no gas)…", "warn");
    // signMessage = personal_sign of the exact SIWE message issued by the bridge.
    const signature = await signer.signMessage(session.message);
    await reportLogin(session.requestId, { signature }); // the bridge verifies with ecrecover
    showStatus("✅ Login verified by the bridge (valid signature).", "ok");
  } catch (err) {
    // e.g. the user rejected the signature: we report the error so it isn't left hanging.
    const msg = humanizeError(err);
    await reportLogin(session.requestId, { error: msg });
    showStatus(`❌ Login not signed: ${msg}`, "error");
  } finally {
    loginInFlight.delete(session.requestId);
  }
}

async function refreshAll() {
  // Each panel refreshes INDEPENDENTLY: if one fails, the other is unaffected.
  // That way a problem in "progress" never breaks the store or the inventory.
  try {
    await loadInventory();
  } catch (err) {
    console.error("loadInventory failed:", err);
  }
  try {
    await loadProgress();
  } catch (err) {
    console.error("loadProgress failed:", err);
  }
}

/**
 * Runs an on-chain read SAFELY: if it reverts (e.g. the getter doesn't exist in
 * the deployed contract, or Achievements isn't connected), it doesn't propagate
 * the error; it returns { ok:false } so the UI shows "n/a" in that field.
 */
async function safeRead(label, fn) {
  try {
    return { ok: true, value: await fn() };
  } catch (err) {
    console.warn(`Read failed (${label}):`, err?.shortMessage || err?.message || err);
    return { ok: false };
  }
}

// Formats the safeRead result: the value (via `fmt`) or "n/a" if it failed.
function fmtRead(r, fmt = (v) => v.toString()) {
  return r.ok ? fmt(r.value) : "n/a";
}

// ─────────────────────────────────────────────────────────────────────────────
//  STORE  (READ priceOf/isListed + purchase with QUANTITY)
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
          <span class="muted">${listed ? `${priceEth} ETH / unit` : "not available"}</span>
        </div>`;

      const actions = document.createElement("div");
      actions.className = "actions";

      // QUANTITY field (to buy several at once, e.g. arrows).
      const qty = document.createElement("input");
      qty.type = "number";
      qty.min = "1";
      qty.value = "1";
      qty.title = "Quantity";

      const btn = document.createElement("button");
      btn.textContent = "Buy";
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
//  BUY  (WRITE payable: buy(itemId, quantity) with value = price*quantity)
// ─────────────────────────────────────────────────────────────────────────────
async function buyItem(item, priceWei, qtyInput) {
  if (!storeWrite) {
    showStatus("Connect your wallet before buying.", "warn");
    return;
  }
  const quantity = BigInt(parseInt(qtyInput.value, 10) || 1);
  const value = priceWei * quantity; // exact cost; the contract refunds the excess

  try {
    showStatus(`Confirm in MetaMask: buy ${quantity} × ${item.name}…`, "warn");
    const tx = await storeWrite.buy(item.id, quantity, { value });
    showStatus(`Tx sent (${tx.hash.slice(0, 10)}…). Waiting for confirmation…`, "warn");
    await tx.wait();
    showStatus(`✅ Bought ${quantity} × ${item.name}.`, "ok");
    await refreshAll();
  } catch (err) {
    showStatus(humanizeError(err), "error");
    console.error(err);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  INVENTORY  (READ balanceOf)  +  EMPTY (WRITE burn — on-chain operation)
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
          <span class="muted">Quantity: ${balance.toString()}</span>
        </div>`;

      const btn = document.createElement("button");
      btn.textContent = "Empty";
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
    showStatus(`Confirm in MetaMask: burn your ${item.name}…`, "warn");
    const tx = await storeWrite.burn(account, item.id, balance);
    await tx.wait();
    showStatus(`🔥 Emptied your ${item.name}.`, "ok");
    await refreshAll();
  } catch (err) {
    showStatus(humanizeError(err), "error");
    console.error(err);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  PROGRESS AND MEDALLIONS  (READ ONLY — to verify rules and achievements)
// ─────────────────────────────────────────────────────────────────────────────
async function loadProgress() {
  if (!account) return;

  // Each read is wrapped in safeRead: a specific failure shows "n/a" in that
  // field, but does NOT break the rest of the panel or the general refresh.
  // (GameStore: counters/thresholds; Achievements: medallions and rarity.)
  const arrowsBought = await safeRead("purchasedTotal", () => storeRead.purchasedTotal(account, FLECHA_ID));
  const spent = await safeRead("totalSpent", () => storeRead.totalSpent(account));
  const capacity = await safeRead("quiverCapacity", () => storeRead.quiverCapacity(account));
  const arqueroGoal = await safeRead("ARQUERO_ARROWS", () => storeRead.ARQUERO_ARROWS());
  const mercaderGoal = await safeRead("MERCADER_SPEND_THRESHOLD", () => storeRead.MERCADER_SPEND_THRESHOLD());
  const rarity = await safeRead("mercaderRarity", () => achRead.mercaderRarity(account));

  // Medallions: each balance separately.
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
      badge = "n/a";
    } else if (r.value > 0n) {
      const extra = m.id === 1 && rarity.ok ? ` — rarity ${RARITY[Number(rarity.value)] ?? "?"}` : "";
      badge = "✅ obtained" + extra;
    } else {
      badge = "🔒 locked";
    }
    return `<div class="card">
      <div><strong>${m.name}</strong> <span class="muted">(id ${m.id} · ${m.note})</span></div>
      <span class="badge">${badge}</span>
    </div>`;
  }).join("");

  els.progress.className = "";
  els.progress.innerHTML = `
    <div class="grid2" style="margin-bottom:1rem">
      <div>Arrows purchased (historical): <strong>${arrowsLabel}</strong></div>
      <div>Quiver capacity: <strong>${fmtRead(capacity)}</strong></div>
      <div>Accumulated spend: <strong>${fmtRead(spent, eth)}</strong></div>
      <div>Merchant threshold: <strong>${fmtRead(mercaderGoal, eth)}</strong></div>
    </div>
    ${medalsHtml}
    <p class="muted">Direct read from the contracts. If you see "n/a", that getter
    didn't respond (typically: the deployed contract is an old version, or
    Achievements isn't deployed/connected). Buy to see the counters advance and
    the medallions get minted.</p>`;
}

window.addEventListener("DOMContentLoaded", init);
