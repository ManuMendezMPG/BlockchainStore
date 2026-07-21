/* =============================================================================
 * Bridge server: serves the STATIC web app and exposes an HTTP/JSON API.
 *
 * Two audiences:
 *   - Browser (web + MetaMask): manual use and, additionally, SIGNS purchases.
 *   - External client (Unreal game): uses the /api/* API to read the chain and
 *     to register/query purchase intents.
 *
 * Purchase pattern (Unreal cannot sign with MetaMask):
 *   1. Unreal     → POST /api/purchase-intent        (stays "pending")
 *   2. The web    → GET  /api/pending                (discovers the intent)
 *                 → POST /api/purchase-result signing (claims it)
 *                 → triggers MetaMask, the player SIGNS
 *                 → POST /api/purchase-result done|error (with txHash)
 *   3. Unreal     → GET  /api/purchase-status?requestId=...  (polling until "done")
 *
 * No external dependencies: only Node's native modules (http, fs, path, crypto,
 * url). READS are done with raw JSON-RPC eth_call against Anvil; since all the
 * arguments are address/uint256, encoding/decoding by hand is trivial.
 *
 * ⚠️ The server NEVER handles private keys. Signing happens ONLY in MetaMask,
 *    in the browser. The server only COORDINATES (reads + intent mailbox).
 * ⚠️ Intents are stored IN MEMORY (a Map): they are lost on restart.
 * ===========================================================================*/

const http = require("http");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
// Dependencies for SIWE: the cryptographic verification of signatures MUST use
// battle-tested libraries, not homemade code. (See README: it breaks the "zero
// dependencies" goal.)
const { SiweMessage, generateNonce } = require("siwe");
const ethers = require("ethers");

const PORT = process.env.PORT || 8787;
const PUBLIC_DIR = path.join(__dirname, "public");
const RPC_URL = process.env.RPC_URL || "http://127.0.0.1:8545";

// Deterministic addresses (the same ones used by public/app.js).
const GAMESTORE_ADDRESS = "0x5FbDB2315678afecb367f032d93F642f64180aa3";
const ACHIEVEMENTS_ADDRESS = "0xe7f1725E7734CE288F8367e1Bb143E90bb3F0512";

// SIWE config (EIP-4361). The `domain` must match when building and verifying.
const SIWE_DOMAIN = "localhost:8787";
const SIWE_URI = "http://localhost:8787";
const CHAIN_ID = 31337; // Anvil

// Catalog (real ids and names from the contract).
const ITEMS = [
  { id: 0, name: "Sword" },
  { id: 1, name: "Shield" },
  { id: 2, name: "Bow" },
  { id: 3, name: "Quiver 5" },
  { id: 4, name: "Quiver 10" },
  { id: 5, name: "Quiver 20" },
  { id: 6, name: "Arrow" },
  { id: 7, name: "Empty bottle" },
  { id: 8, name: "Health potion" },
  { id: 9, name: "Mana potion" },
];
const FLECHA_ID = 6;
const MEDALS = [
  { id: 0, name: "Archer" },
  { id: 1, name: "Merchant" },
  { id: 2, name: "Collector" },
];
const RARITY = ["None", "Bronze", "Silver", "Gold"];

// Function selectors (keccak256(sig)[:4]). Hardcoded because Node cannot compute
// keccak256 natively; obtained with `cast sig "<signature>"`.
const SEL = {
  balanceOf: "0x00fdd58e", //      balanceOf(address,uint256)
  priceOf: "0xb9186d7d", //        priceOf(uint256)
  purchasedTotal: "0x44bbda2f", // purchasedTotal(address,uint256)
  totalSpent: "0xa8949b46", //     totalSpent(address)
  quiverCapacity: "0x104f437c", // quiverCapacity(address)
  mercaderRarity: "0x05d9870f", // mercaderRarity(address)
};

// Purchase intent mailbox (IN MEMORY → lost on restart).
const intents = new Map();

// SIWE login sessions and issued nonces (also IN MEMORY).
const loginSessions = new Map(); // requestId → { address, nonce, message, status, ... }
const issuedNonces = new Map(); //  nonce → { used: boolean }  (single-use)

// ─────────────────────────── JSON-RPC / read ─────────────────────────────────

// Raw JSON-RPC call to the node (Anvil). Returns `result` or rejects with the error.
function rpcCall(method, params) {
  return new Promise((resolve, reject) => {
    const payload = JSON.stringify({ jsonrpc: "2.0", id: 1, method, params });
    const u = new URL(RPC_URL);
    const req = http.request(
      {
        hostname: u.hostname,
        port: u.port,
        path: u.pathname || "/",
        method: "POST",
        headers: { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(payload) },
      },
      (resp) => {
        let body = "";
        resp.on("data", (d) => (body += d));
        resp.on("end", () => {
          try {
            const j = JSON.parse(body);
            if (j.error) reject(new Error(j.error.message || "RPC error"));
            else resolve(j.result);
          } catch (e) {
            reject(e);
          }
        });
      }
    );
    req.on("error", reject);
    req.write(payload);
    req.end();
  });
}

// Minimal ABI encoding (everything is address/uint256 → 32 bytes left-padded).
const padArg = (hex) => hex.replace(/^0x/, "").padStart(64, "0");
const encAddress = (a) => padArg(a.toLowerCase());
const encUint = (n) => padArg(BigInt(n).toString(16));

// eth_call to a `view` function that returns a single uint256.
async function readUint(to, selector, encodedArgs = "") {
  const result = await rpcCall("eth_call", [{ to, data: selector + encodedArgs }, "latest"]);
  return BigInt(!result || result === "0x" ? "0x0" : result);
}

// wei (BigInt) → string in ETH, without libraries.
function weiToEth(wei) {
  const s = wei.toString().padStart(19, "0");
  const intPart = s.slice(0, -18);
  const frac = s.slice(-18).replace(/0+$/, "");
  return frac ? `${intPart}.${frac}` : intPart;
}

const isAddress = (a) => typeof a === "string" && /^0x[0-9a-fA-F]{40}$/.test(a);

// ─────────────────────────── Domain reads ────────────────────────────────────

async function getCatalog() {
  const items = [];
  for (const it of ITEMS) {
    const price = await readUint(GAMESTORE_ADDRESS, SEL.priceOf, encUint(it.id));
    items.push({ id: it.id, name: it.name, priceWei: price.toString(), priceEth: weiToEth(price) });
  }
  return items;
}

async function getInventory(address) {
  const items = [];
  for (const it of ITEMS) {
    const bal = await readUint(GAMESTORE_ADDRESS, SEL.balanceOf, encAddress(address) + encUint(it.id));
    items.push({ id: it.id, name: it.name, quantity: bal.toString() });
  }
  return { address, items };
}

async function getProgress(address) {
  const arrows = await readUint(GAMESTORE_ADDRESS, SEL.purchasedTotal, encAddress(address) + encUint(FLECHA_ID));
  const spent = await readUint(GAMESTORE_ADDRESS, SEL.totalSpent, encAddress(address));
  const capacity = await readUint(GAMESTORE_ADDRESS, SEL.quiverCapacity, encAddress(address));
  const rarity = await readUint(ACHIEVEMENTS_ADDRESS, SEL.mercaderRarity, encAddress(address));

  const medals = [];
  for (const m of MEDALS) {
    const owned = await readUint(ACHIEVEMENTS_ADDRESS, SEL.balanceOf, encAddress(address) + encUint(m.id));
    const entry = { id: m.id, name: m.name, owned: owned > 0n };
    if (m.id === 1 && owned > 0n) entry.rarity = RARITY[Number(rarity)] ?? "?";
    medals.push(entry);
  }

  return {
    address,
    arrowsPurchased: arrows.toString(),
    totalSpentWei: spent.toString(),
    totalSpentEth: weiToEth(spent),
    quiverCapacity: capacity.toString(),
    medals,
  };
}

// ─────────────────────────── HTTP/JSON helpers ───────────────────────────────

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
};

function sendJson(res, status, obj) {
  res.writeHead(status, { "Content-Type": "application/json; charset=utf-8", ...CORS });
  res.end(JSON.stringify(obj));
}

function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    let body = "";
    req.on("data", (d) => {
      body += d;
      if (body.length > 1e6) req.destroy(); // anti-abuse guard
    });
    req.on("end", () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch (e) {
        reject(e);
      }
    });
    req.on("error", reject);
  });
}

// ─────────────────────────────── API /api/* ──────────────────────────────────

async function handleApi(req, res, u) {
  const route = `${req.method} ${u.pathname}`;

  // ── Reads ───────────────────────────────────────────────────────────────────
  if (route === "GET /api/catalog") {
    return sendJson(res, 200, { items: await getCatalog() });
  }

  if (route === "GET /api/inventory") {
    const address = u.searchParams.get("address");
    if (!isAddress(address)) return sendJson(res, 400, { error: "invalid or missing address" });
    return sendJson(res, 200, await getInventory(address));
  }

  if (route === "GET /api/progress") {
    const address = u.searchParams.get("address");
    if (!isAddress(address)) return sendJson(res, 400, { error: "invalid or missing address" });
    return sendJson(res, 200, await getProgress(address));
  }

  // ── Purchase: Unreal registers an intent ────────────────────────────────────
  if (route === "POST /api/purchase-intent") {
    const body = await readJsonBody(req);
    const { address, itemId, quantity } = body;
    if (!isAddress(address)) return sendJson(res, 400, { error: "invalid address" });
    const id = Number(itemId);
    const qty = Number(quantity);
    if (!Number.isInteger(id) || id < 0 || id > 9) return sendJson(res, 400, { error: "itemId out of range (0-9)" });
    if (!Number.isInteger(qty) || qty < 1) return sendJson(res, 400, { error: "quantity must be >= 1" });

    const requestId = crypto.randomUUID();
    intents.set(requestId, {
      requestId,
      address,
      itemId: id,
      quantity: qty,
      status: "pending",
      txHash: null,
      error: null,
      createdAt: Date.now(),
    });
    return sendJson(res, 201, { requestId, status: "pending" });
  }

  // ── Purchase: Unreal queries the status (polling) ───────────────────────────
  if (route === "GET /api/purchase-status") {
    const requestId = u.searchParams.get("requestId");
    const intent = intents.get(requestId);
    if (!intent) return sendJson(res, 404, { error: "unknown requestId" });
    return sendJson(res, 200, {
      status: intent.status,
      txHash: intent.txHash,
      error: intent.error,
      itemId: intent.itemId,
      quantity: intent.quantity,
    });
  }

  // ── Purchase: the WEB discovers pending intents ─────────────────────────────
  if (route === "GET /api/pending") {
    const pending = [...intents.values()]
      .filter((i) => i.status === "pending")
      .map((i) => ({ requestId: i.requestId, address: i.address, itemId: i.itemId, quantity: i.quantity }));
    return sendJson(res, 200, { pending });
  }

  // ── Purchase: the WEB reports progress (signing → done|error) ───────────────
  if (route === "POST /api/purchase-result") {
    const body = await readJsonBody(req);
    const { requestId, status, txHash, error } = body;
    const intent = intents.get(requestId);
    if (!intent) return sendJson(res, 404, { error: "unknown requestId" });
    if (!["signing", "done", "error"].includes(status)) {
      return sendJson(res, 400, { error: "status must be signing|done|error" });
    }
    intent.status = status;
    if (txHash) intent.txHash = txHash;
    if (error) intent.error = error;
    return sendJson(res, 200, { ok: true, status: intent.status });
  }

  // ── SIWE: standalone nonce (for clients that build their own message) ───────
  if (route === "GET /api/siwe/nonce") {
    const nonce = generateNonce();
    issuedNonces.set(nonce, { used: false });
    return sendJson(res, 200, { nonce });
  }

  // ── SIWE: Unreal registers a login intent ───────────────────────────────────
  if (route === "POST /api/siwe/login-intent") {
    const body = await readJsonBody(req);
    let address;
    try {
      address = ethers.getAddress(body.address); // validates + EIP-55 checksum (or throws)
    } catch {
      return sendJson(res, 400, { error: "invalid address" });
    }

    const nonce = generateNonce();
    issuedNonces.set(nonce, { used: false });

    // We build the EIP-4361 message on the server (the web will sign THIS message).
    const siwe = new SiweMessage({
      domain: SIWE_DOMAIN,
      address,
      statement: "Sign in to GameStore (demo). Signing is free (no gas).",
      uri: SIWE_URI,
      version: "1",
      chainId: CHAIN_ID,
      nonce,
      issuedAt: new Date().toISOString(),
    });
    const message = siwe.prepareMessage();

    const requestId = crypto.randomUUID();
    loginSessions.set(requestId, {
      requestId,
      address,
      nonce,
      message,
      status: "pending",
      error: null,
      createdAt: Date.now(),
    });
    return sendJson(res, 201, { requestId, message });
  }

  // ── SIWE: Unreal queries the status (polling) ───────────────────────────────
  if (route === "GET /api/siwe/login-status") {
    const s = loginSessions.get(u.searchParams.get("requestId"));
    if (!s) return sendJson(res, 404, { error: "unknown requestId" });
    return sendJson(res, 200, {
      status: s.status,
      address: s.status === "done" ? s.address : undefined,
      error: s.error,
    });
  }

  // ── SIWE: the WEB discovers pending logins (claim-on-read → signing) ────────
  // When returning them we mark them "signing": that way they stop being offered
  // and two tabs won't sign the same login (same idea as "signing" in purchases).
  if (route === "GET /api/siwe/pending") {
    const pending = [];
    for (const s of loginSessions.values()) {
      if (s.status === "pending") {
        s.status = "signing";
        pending.push({ requestId: s.requestId, address: s.address, message: s.message });
      }
    }
    return sendJson(res, 200, { pending });
  }

  // ── SIWE: the WEB reports the signature → the bridge VERIFIES cryptographically ─
  if (route === "POST /api/siwe/verify") {
    const body = await readJsonBody(req);
    const { requestId, signature, error: clientError } = body;
    const s = loginSessions.get(requestId);
    if (!s) return sendJson(res, 404, { error: "unknown requestId" });

    // The web may report a failure (e.g. the user rejected the signature).
    if (clientError) {
      s.status = "error";
      s.error = String(clientError);
      return sendJson(res, 200, { ok: false, status: "error" });
    }
    if (typeof signature !== "string") return sendJson(res, 400, { error: "missing signature" });

    // Single-use nonce: if it doesn't exist or was already used, reject (anti-replay).
    const nrec = issuedNonces.get(s.nonce);
    if (!nrec || nrec.used) {
      s.status = "error";
      s.error = "invalid or already-used nonce";
      return sendJson(res, 400, { error: s.error });
    }

    try {
      // We re-parse the EXACT message we issued and verify the signature.
      // siwe.verify does ecrecover (with ethers): from the message + signature it
      // recovers the signing address and compares it with the message's `address`;
      // it also checks that the nonce and domain match the expected ones.
      const siwe = new SiweMessage(s.message);
      const result = await siwe.verify({ signature, nonce: s.nonce, domain: SIWE_DOMAIN });
      if (!result.success) throw new Error("invalid signature");
      if (siwe.chainId !== CHAIN_ID) throw new Error("wrong chainId");

      nrec.used = true; // NONCE BURNED: cannot be reused
      s.status = "done";
      s.address = siwe.address;
      return sendJson(res, 200, { ok: true, address: siwe.address });
    } catch (e) {
      // siwe.verify rejects with an object {success:false, error:{type}} instead of Error.
      const reason = e?.error?.type || e?.message || "verification failed";
      s.status = "error";
      s.error = reason;
      return sendJson(res, 401, { error: reason });
    }
  }

  return sendJson(res, 404, { error: `route not found: ${route}` });
}

// ────────────────────────── Static web (GET) ─────────────────────────────────

function serveStatic(req, res, u) {
  const urlPath = decodeURIComponent(u.pathname);
  const safePath = path.normalize(urlPath).replace(/^(\.\.[/\\])+/, "");
  let filePath = urlPath === "/" || urlPath === "" ? path.join(PUBLIC_DIR, "index.html") : path.join(PUBLIC_DIR, safePath);

  if (!filePath.startsWith(PUBLIC_DIR)) {
    res.writeHead(403).end("Forbidden");
    return;
  }

  const MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".ico": "image/x-icon",
  };

  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
      res.end("404 Not Found");
      return;
    }
    res.writeHead(200, { "Content-Type": MIME[path.extname(filePath).toLowerCase()] || "application/octet-stream" });
    res.end(data);
  });
}

// ───────────────────────────────── Server ────────────────────────────────────

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, "http://localhost");

  // CORS preflight so an external client (Unreal) can call the API.
  if (req.method === "OPTIONS") {
    res.writeHead(204, CORS);
    res.end();
    return;
  }

  // The API (/api/*) is separate from serving the static web.
  if (u.pathname.startsWith("/api/")) {
    try {
      await handleApi(req, res, u);
    } catch (e) {
      sendJson(res, 502, { error: e.message || "internal error" });
    }
    return;
  }

  // Static web: GET only.
  if (req.method !== "GET") {
    res.writeHead(405).end("Method Not Allowed");
    return;
  }
  serveStatic(req, res, u);
});

server.listen(PORT, () => {
  console.log(`\n  Web bridge serving at:    http://localhost:${PORT}`);
  console.log(`  JSON API at:              http://localhost:${PORT}/api/*`);
  console.log(`  Read RPC:                 ${RPC_URL}`);
  console.log(`  (Ctrl+C to stop)\n`);
});
