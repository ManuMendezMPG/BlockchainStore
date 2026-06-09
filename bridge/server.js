/* =============================================================================
 * Servidor estático mínimo para el puente web.
 *
 * ¿Por qué un servidor y no abrir el index.html como file://?
 *   MetaMask (y muchas APIs del navegador) no inyectan window.ethereum ni
 *   permiten fetch() correctamente bajo el esquema file://. Hay que servir la
 *   página por HTTP (origen http://localhost), aunque sea en local.
 *
 * Sin dependencias: usa solo módulos integrados de Node (http, fs, path).
 * No requiere `npm install` ni node_modules. Stack mínimo.
 * ===========================================================================*/

const http = require("http");
const fs = require("fs");
const path = require("path");

const PORT = process.env.PORT || 8787;
const PUBLIC_DIR = path.join(__dirname, "public");

// Tipos MIME para los ficheros que servimos.
const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".svg": "image/svg+xml",
  ".ico": "image/x-icon",
};

const server = http.createServer((req, res) => {
  // Solo servimos GET.
  if (req.method !== "GET") {
    res.writeHead(405).end("Method Not Allowed");
    return;
  }

  // Normalizamos la ruta y evitamos "path traversal" (../../etc/passwd).
  const urlPath = decodeURIComponent(req.url.split("?")[0]);
  const safePath = path
    .normalize(urlPath)
    .replace(/^(\.\.[/\\])+/, "");
  let filePath = path.join(PUBLIC_DIR, safePath);

  // "/" → index.html
  if (urlPath === "/" || urlPath === "") {
    filePath = path.join(PUBLIC_DIR, "index.html");
  }

  // El fichero debe quedar dentro de PUBLIC_DIR.
  if (!filePath.startsWith(PUBLIC_DIR)) {
    res.writeHead(403).end("Forbidden");
    return;
  }

  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
      res.end("404 Not Found");
      return;
    }
    const ext = path.extname(filePath).toLowerCase();
    res.writeHead(200, { "Content-Type": MIME[ext] || "application/octet-stream" });
    res.end(data);
  });
});

server.listen(PORT, () => {
  console.log(`\n  Puente web sirviendo en:  http://localhost:${PORT}\n`);
  console.log(`  (Ctrl+C para detener)\n`);
});
