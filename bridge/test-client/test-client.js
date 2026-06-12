#!/usr/bin/env node
/* =============================================================================
 * test-client.js — Simula lo que hará UNREAL contra la API del bridge.
 *
 * Herramienta de VALIDACIÓN (no es parte del producto). Reproduce el camino que
 * seguirá el juego: POST de una intención de compra + polling del estado, con la
 * firma real ocurriendo en el navegador (web del bridge + MetaMask).
 *
 * Sin dependencias: solo el módulo `http` nativo de Node. Pensado para ejecutarse
 * en WINDOWS (donde corre el server del bridge y vive Node); desde WSL no se
 * alcanza http://localhost:8787.
 *
 * Uso:   node test-client.js <address 0x...> <itemId 0-9> <quantity >=1>
 * Salida: exit 0 = done · 1 = error · 2 = args inválidos · 3 = timeout · 4 = server caído
 * ===========================================================================*/

const http = require("http");

const BASE = process.env.BRIDGE_URL || "http://localhost:8787";
const POLL_MS = 1500; // intervalo de polling
const TIMEOUT_MS = 90000; // 90 s máximo esperando la firma

// ── HTTP helper (sin dependencias). Devuelve { status, json, raw }. ──────────
function request(method, urlPath, body) {
  return new Promise((resolve, reject) => {
    const u = new URL(BASE + urlPath);
    const payload = body ? JSON.stringify(body) : null; // JSON serializado: sin líos de comillas
    const opts = {
      hostname: u.hostname,
      port: u.port,
      path: u.pathname + u.search,
      method,
      headers: { Accept: "application/json" },
    };
    if (payload) {
      opts.headers["Content-Type"] = "application/json";
      opts.headers["Content-Length"] = Buffer.byteLength(payload);
    }
    const req = http.request(opts, (res) => {
      let data = "";
      res.on("data", (d) => (data += d));
      res.on("end", () => {
        let json = null;
        try {
          json = data ? JSON.parse(data) : null;
        } catch {
          /* respuesta no-JSON: se queda en raw */
        }
        resolve({ status: res.statusCode, json, raw: data });
      });
    });
    req.on("error", reject);
    if (payload) req.write(payload);
    req.end();
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const itemOf = (inv, id) => (inv?.items || []).find((i) => Number(i.id) === id);

function usage(msg) {
  if (msg) console.error(`\n  ✗ ${msg}`);
  console.error("\n  Uso: node test-client.js <address 0x...> <itemId 0-9> <quantity >=1>");
  console.error("  Ej:  node test-client.js 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266 6 5\n");
  process.exit(2);
}

function serverDown(err) {
  console.error(`\n  ✗ No se pudo contactar con el bridge en ${BASE}.`);
  console.error("    ¿Está 'node server.js' corriendo en Windows?");
  console.error("    ¿Está la web del bridge abierta con MetaMask conectado (para firmar)?");
  console.error(`    Detalle: ${err?.code || err?.message || err}`);
  process.exit(4);
}

async function main() {
  // ── Validación de argumentos ───────────────────────────────────────────────
  const [address, itemIdRaw, quantityRaw] = process.argv.slice(2);
  if (!address || itemIdRaw === undefined || quantityRaw === undefined) usage("Faltan argumentos.");
  if (!/^0x[0-9a-fA-F]{40}$/.test(address)) usage(`address inválida: "${address}" (debe ser 0x + 40 hex).`);
  const itemId = Number(itemIdRaw);
  const quantity = Number(quantityRaw);
  if (!Number.isInteger(itemId) || itemId < 0 || itemId > 9) usage(`itemId inválido: "${itemIdRaw}" (entero 0-9).`);
  if (!Number.isInteger(quantity) || quantity < 1) usage(`quantity inválida: "${quantityRaw}" (entero >= 1).`);

  console.log(`\n  Bridge:  ${BASE}`);
  console.log(`  Address: ${address}`);
  console.log(`  Compra:  itemId ${itemId} × ${quantity}\n`);

  // ── 1) Inventario ANTES ─────────────────────────────────────────────────────
  let invBefore;
  try {
    const r = await request("GET", `/api/inventory?address=${address}`);
    if (r.status !== 200) {
      console.error(`  ✗ /api/inventory → HTTP ${r.status}: ${r.json?.error || r.raw}`);
      process.exit(1);
    }
    invBefore = r.json;
  } catch (e) {
    serverDown(e);
  }
  const name = itemOf(invBefore, itemId)?.name || `item ${itemId}`;
  const balBefore = BigInt(itemOf(invBefore, itemId)?.quantity ?? "0");
  console.log(`  [1] Balance ANTES de "${name}": ${balBefore}`);

  // ── 2) POST intención de compra ─────────────────────────────────────────────
  let requestId;
  try {
    const r = await request("POST", "/api/purchase-intent", { address, itemId, quantity });
    if (r.status !== 201) {
      console.error(`  ✗ /api/purchase-intent → HTTP ${r.status}: ${r.json?.error || r.raw}`);
      process.exit(1);
    }
    requestId = r.json.requestId;
  } catch (e) {
    serverDown(e);
  }
  console.log(`  [2] Intención registrada. requestId = ${requestId}`);
  console.log("      ➜ La web del bridge (con MetaMask) debe firmarla.\n");

  // ── 3) Polling del estado ───────────────────────────────────────────────────
  const deadline = Date.now() + TIMEOUT_MS;
  let last = null;
  while (Date.now() < deadline) {
    await sleep(POLL_MS);
    let st;
    try {
      const r = await request("GET", `/api/purchase-status?requestId=${requestId}`);
      if (r.status !== 200) {
        console.error(`\n  ✗ /api/purchase-status → HTTP ${r.status}: ${r.json?.error || r.raw}`);
        process.exit(1);
      }
      st = r.json;
    } catch (e) {
      serverDown(e);
    }

    if (st.status !== last) {
      console.log(`  [3] estado: ${st.status}`);
      if (st.status === "signing") console.log("      ⏳ Firma la transacción en MetaMask (en el navegador)…");
      last = st.status;
    } else {
      process.stdout.write("."); // sigue en el mismo estado: latido
    }

    // ── 4) Resolución ──────────────────────────────────────────────────────────
    if (st.status === "done") {
      console.log(`\n  [4] ✅ DONE · txHash = ${st.txHash}`);
      try {
        const r = await request("GET", `/api/inventory?address=${address}`);
        const after = BigInt(itemOf(r.json, itemId)?.quantity ?? "0");
        console.log(`      Balance DESPUÉS de "${name}": ${after}  (Δ +${after - balBefore})\n`);
      } catch (e) {
        serverDown(e);
      }
      process.exit(0);
    }
    if (st.status === "error") {
      console.error(`\n  [4] ❌ ERROR: ${st.error || "(sin detalle)"}\n`);
      process.exit(1);
    }
  }

  console.error(`\n  ✗ TIMEOUT tras ${TIMEOUT_MS / 1000}s sin resolver (¿firmaste en MetaMask?).\n`);
  process.exit(3);
}

main();
