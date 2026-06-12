/* =============================================================================
 * Servidor del puente: sirve la web ESTÁTICA y expone una API HTTP/JSON.
 *
 * Dos públicos:
 *   - Navegador (web + MetaMask): uso manual y, además, FIRMA las compras.
 *   - Cliente externo (juego de Unreal): usa la API /api/* para leer la cadena
 *     y para registrar/consultar intenciones de compra.
 *
 * Patrón de compra (Unreal no puede firmar con MetaMask):
 *   1. Unreal     → POST /api/purchase-intent        (queda "pending")
 *   2. La web     → GET  /api/pending                (descubre la intención)
 *                 → POST /api/purchase-result signing (la reclama)
 *                 → dispara MetaMask, el jugador FIRMA
 *                 → POST /api/purchase-result done|error (con txHash)
 *   3. Unreal     → GET  /api/purchase-status?requestId=...  (polling hasta "done")
 *
 * Sin dependencias externas: solo módulos nativos de Node (http, fs, path, crypto,
 * url). Las LECTURAS se hacen con eth_call por JSON-RPC crudo contra Anvil; como
 * todos los argumentos son address/uint256, codificar/decodificar a mano es trivial.
 *
 * ⚠️ El servidor NUNCA maneja claves privadas. La firma ocurre SOLO en MetaMask,
 *    en el navegador. El servidor solo COORDINA (lecturas + buzón de intenciones).
 * ⚠️ Las intenciones se guardan EN MEMORIA (un Map): se pierden al reiniciar.
 * ===========================================================================*/

const http = require("http");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const PORT = process.env.PORT || 8787;
const PUBLIC_DIR = path.join(__dirname, "public");
const RPC_URL = process.env.RPC_URL || "http://127.0.0.1:8545";

// Direcciones deterministas (las mismas que usa public/app.js).
const GAMESTORE_ADDRESS = "0x5FbDB2315678afecb367f032d93F642f64180aa3";
const ACHIEVEMENTS_ADDRESS = "0xe7f1725E7734CE288F8367e1Bb143E90bb3F0512";

// Catálogo (ids y nombres reales del contrato).
const ITEMS = [
  { id: 0, name: "Espada" },
  { id: 1, name: "Escudo" },
  { id: 2, name: "Arco" },
  { id: 3, name: "Carcaj 5" },
  { id: 4, name: "Carcaj 10" },
  { id: 5, name: "Carcaj 20" },
  { id: 6, name: "Flecha" },
  { id: 7, name: "Botella vacia" },
  { id: 8, name: "Pocion de vida" },
  { id: 9, name: "Pocion de mana" },
];
const FLECHA_ID = 6;
const MEDALS = [
  { id: 0, name: "ARQUERO" },
  { id: 1, name: "MERCADER" },
  { id: 2, name: "COLECCIONISTA" },
];
const RARITY = ["None", "Bronce", "Plata", "Oro"];

// Selectores de función (keccak256(sig)[:4]). Hardcodeados porque Node no puede
// calcular keccak256 nativamente; obtenidos con `cast sig "<firma>"`.
const SEL = {
  balanceOf: "0x00fdd58e", //      balanceOf(address,uint256)
  priceOf: "0xb9186d7d", //        priceOf(uint256)
  purchasedTotal: "0x44bbda2f", // purchasedTotal(address,uint256)
  totalSpent: "0xa8949b46", //     totalSpent(address)
  quiverCapacity: "0x104f437c", // quiverCapacity(address)
  mercaderRarity: "0x05d9870f", // mercaderRarity(address)
};

// Buzón de intenciones de compra (EN MEMORIA → se pierde al reiniciar).
const intents = new Map();

// ─────────────────────────── JSON-RPC / lectura ──────────────────────────────

// Llamada JSON-RPC cruda al nodo (Anvil). Devuelve `result` o rechaza con el error.
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

// Codificación ABI mínima (todo es address/uint256 → 32 bytes left-padded).
const padArg = (hex) => hex.replace(/^0x/, "").padStart(64, "0");
const encAddress = (a) => padArg(a.toLowerCase());
const encUint = (n) => padArg(BigInt(n).toString(16));

// eth_call a una función `view` que devuelve un único uint256.
async function readUint(to, selector, encodedArgs = "") {
  const result = await rpcCall("eth_call", [{ to, data: selector + encodedArgs }, "latest"]);
  return BigInt(!result || result === "0x" ? "0x0" : result);
}

// wei (BigInt) → string en ETH, sin librerías.
function weiToEth(wei) {
  const s = wei.toString().padStart(19, "0");
  const intPart = s.slice(0, -18);
  const frac = s.slice(-18).replace(/0+$/, "");
  return frac ? `${intPart}.${frac}` : intPart;
}

const isAddress = (a) => typeof a === "string" && /^0x[0-9a-fA-F]{40}$/.test(a);

// ─────────────────────────── Lecturas de dominio ─────────────────────────────

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

// ─────────────────────────── Helpers HTTP/JSON ───────────────────────────────

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
      if (body.length > 1e6) req.destroy(); // guard anti-abuso
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

  // ── Lecturas ──────────────────────────────────────────────────────────────
  if (route === "GET /api/catalog") {
    return sendJson(res, 200, { items: await getCatalog() });
  }

  if (route === "GET /api/inventory") {
    const address = u.searchParams.get("address");
    if (!isAddress(address)) return sendJson(res, 400, { error: "address invalida o ausente" });
    return sendJson(res, 200, await getInventory(address));
  }

  if (route === "GET /api/progress") {
    const address = u.searchParams.get("address");
    if (!isAddress(address)) return sendJson(res, 400, { error: "address invalida o ausente" });
    return sendJson(res, 200, await getProgress(address));
  }

  // ── Compra: Unreal registra una intención ──────────────────────────────────
  if (route === "POST /api/purchase-intent") {
    const body = await readJsonBody(req);
    const { address, itemId, quantity } = body;
    if (!isAddress(address)) return sendJson(res, 400, { error: "address invalida" });
    const id = Number(itemId);
    const qty = Number(quantity);
    if (!Number.isInteger(id) || id < 0 || id > 9) return sendJson(res, 400, { error: "itemId fuera de rango (0-9)" });
    if (!Number.isInteger(qty) || qty < 1) return sendJson(res, 400, { error: "quantity debe ser >= 1" });

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

  // ── Compra: Unreal consulta el estado (polling) ─────────────────────────────
  if (route === "GET /api/purchase-status") {
    const requestId = u.searchParams.get("requestId");
    const intent = intents.get(requestId);
    if (!intent) return sendJson(res, 404, { error: "requestId desconocido" });
    return sendJson(res, 200, {
      status: intent.status,
      txHash: intent.txHash,
      error: intent.error,
      itemId: intent.itemId,
      quantity: intent.quantity,
    });
  }

  // ── Compra: la WEB descubre intenciones pendientes ──────────────────────────
  if (route === "GET /api/pending") {
    const pending = [...intents.values()]
      .filter((i) => i.status === "pending")
      .map((i) => ({ requestId: i.requestId, address: i.address, itemId: i.itemId, quantity: i.quantity }));
    return sendJson(res, 200, { pending });
  }

  // ── Compra: la WEB reporta el avance (signing → done|error) ─────────────────
  if (route === "POST /api/purchase-result") {
    const body = await readJsonBody(req);
    const { requestId, status, txHash, error } = body;
    const intent = intents.get(requestId);
    if (!intent) return sendJson(res, 404, { error: "requestId desconocido" });
    if (!["signing", "done", "error"].includes(status)) {
      return sendJson(res, 400, { error: "status debe ser signing|done|error" });
    }
    intent.status = status;
    if (txHash) intent.txHash = txHash;
    if (error) intent.error = error;
    return sendJson(res, 200, { ok: true, status: intent.status });
  }

  return sendJson(res, 404, { error: `ruta no encontrada: ${route}` });
}

// ────────────────────────── Web estática (GET) ───────────────────────────────

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

  // Preflight CORS para que un cliente externo (Unreal) pueda llamar a la API.
  if (req.method === "OPTIONS") {
    res.writeHead(204, CORS);
    res.end();
    return;
  }

  // La API (/api/*) está separada de servir la web estática.
  if (u.pathname.startsWith("/api/")) {
    try {
      await handleApi(req, res, u);
    } catch (e) {
      sendJson(res, 502, { error: e.message || "error interno" });
    }
    return;
  }

  // Web estática: solo GET.
  if (req.method !== "GET") {
    res.writeHead(405).end("Method Not Allowed");
    return;
  }
  serveStatic(req, res, u);
});

server.listen(PORT, () => {
  console.log(`\n  Puente web sirviendo en:  http://localhost:${PORT}`);
  console.log(`  API JSON en:              http://localhost:${PORT}/api/*`);
  console.log(`  RPC de lectura:           ${RPC_URL}`);
  console.log(`  (Ctrl+C para detener)\n`);
});
