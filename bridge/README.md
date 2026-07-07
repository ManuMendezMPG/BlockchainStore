# /bridge — Puente web local (Node.js + ethers.js + MetaMask)

Web local que conecta con **MetaMask**, lee el inventario del jugador desde el
contrato `GameStore` (ERC-1155) desplegado en Anvil y permite **comprar** y
**vaciar** (quemar) items. De momento funciona **de forma aislada** (sin Unreal):
es la base sobre la que luego se conectará el juego.

Las claves privadas **nunca salen de MetaMask**. La web solo construye las
llamadas; MetaMask custodia la clave y firma. No hay ninguna clave en el código.

## Estructura

```
bridge/
├── server.js                 # Sirve la web estática + API JSON /api/* (Node + siwe + ethers; requiere npm install)
├── public/
│   ├── index.html            # Estructura + estilos + carga de ethers.js (CDN)
│   ├── app.js                # Web: conectar, leer/comprar/quemar + consumidor de intenciones
│   └── abi/
│       ├── GameStore.json    # ABI extraído de los artefactos de Foundry
│       └── Achievements.json
├── .env.example
└── README.md
```

Stack **mínimo**: HTML + JS, sin framework ni bundler. `ethers.js v6` se carga en el
navegador como build UMD desde un CDN (variable global `ethers`).

### Dependencias del servidor (cambio respecto al "cero dependencias")

Hasta el login SIWE, el servidor no usaba dependencias. Al añadir SIWE, el `server.js`
**sí** usa dos paquetes npm:

- **`siwe`** — construye y, sobre todo, **verifica** los mensajes EIP-4361.
- **`ethers`** — utilidades de criptografía/direcciones que usa la verificación.

**Por qué se rompe el cero-dependencias a propósito:** la verificación criptográfica de
firmas (ecrecover, parseo EIP-4361, comprobaciones de nonce/domain) **debe apoyarse en
librerías probadas y auditadas, no en código casero** — un fallo aquí es un fallo de
seguridad. Las lecturas siguen sin librerías (eth_call crudo); solo SIWE trae deps.

### Arranque (recomendado): `start-bridge.ps1`

Hay un script de arranque para Windows que **instala las dependencias solo cuando hace
falta** y luego arranca el server:

```powershell
powershell -ExecutionPolicy Bypass -File \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge\start-bridge.ps1
# o, situado en la carpeta bridge:
.\start-bridge.ps1
```

Qué hace:
1. Comprueba que Node está disponible.
2. Decide si instalar dependencias:
   - **no existe `node_modules`** (primer arranque) → `npm install`;
   - **`package.json` es más reciente que `package-lock.json`** (cambiaron las deps) →
     `npm install`;
   - en otro caso → **no reinstala** (va directo al server).
3. Arranca `node server.js` (Ctrl+C para parar).

**Primer arranque vs siguientes:** el primero hace `npm install` (crea `node_modules`,
tarda unos segundos); los siguientes ven las deps al día y arrancan directos.

### Arranque manual (alternativa)

Si prefieres hacerlo a mano:

```powershell
cd \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge
npm install      # solo la 1ª vez o cuando cambian las dependencias
node server.js
```

> `node_modules/` está en `.gitignore`. Node está en **Windows**, así que el script,
> `npm install` y `node server.js` se ejecutan **en Windows** (PowerShell).

## ABIs y direcciones de los contratos

- **ABIs** (`public/abi/GameStore.json` y `public/abi/Achievements.json`): se extraen
  de los artefactos de Foundry. **Regenéralos siempre que cambies un contrato**:
  ```bash
  cd ../contracts
  forge inspect src/GameStore.sol:GameStore abi --json    > ../bridge/public/abi/GameStore.json
  forge inspect src/Achievements.sol:Achievements abi --json > ../bridge/public/abi/Achievements.json
  ```
  Incluyen el ABI **completo con los custom errors**, lo que permite que la web
  muestre los reverts de las reglas de dependencia de forma legible.
- **Direcciones**: en `public/app.js`, claramente señaladas. En Anvil son
  **deterministas**:
  - `GAMESTORE_ADDRESS = 0x5FbDB2315678afecb367f032d93F642f64180aa3` (despliegue nonce 0).
  - `ACHIEVEMENTS_ADDRESS = 0xe7f1725E7734CE288F8367e1Bb143E90bb3F0512` (despliegue nonce 1).

  Si cambias el orden/los despliegues, actualízalas (aparecen en la salida de
  `forge script` y en `contracts/broadcast/.../run-latest.json`).

## Cómo arrancar (entorno local de cero)

Necesitas Anvil corriendo y el contrato desplegado (ver `contracts/README.md`),
y luego este servidor.

```
Terminal A (WSL):   anvil
Terminal B (WSL):   cd contracts && forge script script/DeployGameStore.s.sol:DeployGameStore \
                      --rpc-url http://127.0.0.1:8545 \
                      --private-key 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80 \
                      --broadcast
Terminal C:         node server.js     # desde la carpeta bridge/
```

Luego abre **http://localhost:8787** en el navegador (no abras el `.html` como
`file://`: MetaMask no inyecta `window.ethereum` ni permite `fetch` bajo ese
esquema; por eso necesitamos el servidor HTTP).

> **Nota de entorno (este equipo):** Node está instalado en **Windows**, no en WSL.
> Arranca el servidor desde una terminal de Windows (PowerShell/cmd) situada en la
> carpeta `bridge`, o desde WSL si instalas Node allí. `localhost` se comparte
> entre Windows y WSL, así que el navegador lo verá igual.
>
> Variables de entorno (el server las lee directamente, **no** hay carga de `.env`):
> `PORT` (puerto, por defecto 8787) y `RPC_URL` (nodo RPC para las lecturas `/api/*`,
> por defecto `http://127.0.0.1:8545`). Ej.: `PORT=3000 node server.js`.

## Configurar MetaMask para Anvil

1. Añade una red personalizada: **RPC** `http://127.0.0.1:8545`, **Chain ID**
   `31337`, símbolo `ETH`.
2. Importa una de las cuentas de prueba que imprime Anvil (clave privada). La web
   avisará si estás en otra red distinta de la 31337.

> ⚠️ Las claves de Anvil son **públicas y de prueba**: solo para desarrollo local.
> Nunca importes una clave real ni envíes fondos reales a esas cuentas.

## Qué hace la web

Es un **banco de pruebas del contrato**, no la cara final (eso será Unreal). Solo
toca lo **on-chain**; el uso de items (gastar/beber/romper) y los slots son de Unreal.

- **Conectar wallet**: `window.ethereum` → muestra cuenta y red; avisa si no es 31337.
- **Tienda (10 items)**: lee `priceOf(id)`/`isListed(id)` de los ids 0–9 (espada,
  escudo, arco, carcaj_5/10/20, flecha, botella_vacia, pocion_vida, pocion_mana).
- **Comprar con cantidad**: cada item tiene un campo de cantidad; llama a
  `buy(id, cantidad)` con `value = precio × cantidad`. El contrato reembolsa el
  excedente automáticamente.
- **Inventario**: `balanceOf(cuenta, id)` de los 10 items; **Vaciar** llama a
  `burn(cuenta, id, balance)` (operación on-chain).
- **Progreso y medallones (solo lectura)**: lee de GameStore los contadores
  acumulados (`purchasedTotal`, `totalSpent`, `quiverCapacity`) y de Achievements
  los medallones que posee (`balanceOf`) y la rareza del Mercader. Sirve para
  **verificar que los logros se acuñan** al cumplir sus condiciones.
- **Errores legibles**: red incorrecta, firma rechazada, fondos insuficientes, y los
  reverts de las **reglas de dependencia** (sin arco, carcaj lleno, sin botella, sin
  el logro ARQUERO…), decodificados gracias al ABI completo con custom errors.

### Probar las reglas de dependencia desde el navegador

| Para ver… | Haz esto | Mensaje esperado |
|-----------|----------|------------------|
| Flecha necesita arco | Compra **flecha** sin tener arco | "Necesitas un ARCO…" |
| Capacidad de carcaj | Compra arco + carcaj_5, intenta comprar **6 flechas** | "No caben: tu carcaj admite 5…" |
| Botella → poción | Compra **pocion_vida** sin botellas | "Necesitas 1 botella(s) vacía(s)…" |
| Evolución de carcaj | Compra **carcaj_10** sin tener carcaj_5 | "Necesitas el Carcaj 5…" |
| Carcaj 20 gateado | Compra **carcaj_20** sin el logro ARQUERO | "El Carcaj 20 requiere el logro ARQUERO…" |
| Logro ARQUERO | Compra 20 flechas históricas (compra 10, vacía, compra 10) | Medallón ARQUERO ✅ en "Progreso" |

## API HTTP para clientes externos (Unreal)

Además de la web, el servidor expone una **API JSON en `/api/*`** para que un cliente
que **no puede firmar con MetaMask** (el juego de Unreal) interactúe con la cadena. La
API está separada de servir la web estática y lleva cabeceras **CORS** (`*`) para que
un cliente local pueda llamarla.

> El servidor **nunca** maneja claves: las lecturas las hace con un provider de solo
> lectura (eth_call); las compras solo las COORDINA (la firma sigue en MetaMask). Las
> intenciones se guardan **en memoria** (un `Map`): **se pierden al reiniciar** el server.

### Lecturas (el servidor consulta la cadena y responde JSON)

| Endpoint | Devuelve |
|----------|----------|
| `GET /api/catalog` | Los 10 items con `id`, `name`, `priceWei`, `priceEth`. |
| `GET /api/inventory?address=0x…` | Balance de cada item para esa dirección. |
| `GET /api/progress?address=0x…` | `arrowsPurchased`, `totalSpent`, `quiverCapacity` y los medallones (con rareza del Mercader). |

### Compra (patrón pending → firma en web → done)

| Endpoint | Quién | Para qué |
|----------|-------|----------|
| `POST /api/purchase-intent` `{address,itemId,quantity}` | Unreal | Registra una intención; devuelve `{requestId}` (status `pending`). |
| `GET /api/purchase-status?requestId=…` | Unreal | Hace polling: `pending` → `signing` → `done` (con `txHash`) o `error`. |
| `GET /api/pending` | La web | Lista las intenciones aún `pending`. |
| `POST /api/purchase-result` `{requestId,status,txHash?,error?}` | La web | Marca `signing` (al reclamarla) y luego `done`/`error`. |

### Login SIWE (Sign-In with Ethereum, EIP-4361)

Login = **firma de un MENSAJE** (no una transacción): prueba criptográficamente la
propiedad de la wallet, **sin gas y sin tocar la cadena**. Mismo patrón de tres actores
que las compras (Unreal pide, la web firma, el bridge **verifica**).

| Endpoint | Quién | Para qué |
|----------|-------|----------|
| `GET /api/siwe/nonce` | — | Genera un nonce aleatorio de un solo uso. |
| `POST /api/siwe/login-intent` `{address}` | Unreal | Crea el mensaje EIP-4361 (domain, address, chainId 31337, nonce, issued-at) y la sesión; devuelve `{requestId, message}`. |
| `GET /api/siwe/login-status?requestId=…` | Unreal | Polling: `pending` → `signing` → `done` (con `address`) / `error`. |
| `GET /api/siwe/pending` | La web | Lista logins pendientes y los **reclama** (→ `signing`). |
| `POST /api/siwe/verify` `{requestId, signature}` | La web | El bridge **verifica** la firma (ecrecover + nonce + domain + chainId). |

**Qué hace la verificación:** del `message` firmado + la `signature`, `siwe`/`ethers`
recuperan (ecrecover) la dirección que firmó y la comparan con la `address` del mensaje;
además comprueban que el **nonce** coincide con el emitido (y no se ha usado), el
**domain** y el **chainId**. El servidor **nunca ve la clave**: solo verifica.

**Seguridad:** el **nonce es de un solo uso** (se quema al verificar) → una firma
capturada **no se puede reenviar** (anti-repetición). Una firma de otra cuenta para un
login que dice ser tuyo **falla** (la dirección recuperada no coincide).

```
UNREAL                       BRIDGE                         WEB + MetaMask
  POST /siwe/login-intent ────►  crea mensaje + nonce {pending}
  ◄── { requestId, message }
                                              GET /siwe/pending ──► ve el login (→ signing)
                                              signMessage(mensaje)  ← personal_sign, SIN gas
                              verifica (ecrecover) ◄──── POST /siwe/verify { signature }
  GET /siwe/login-status ──► done, address ✓
```

### Flujo de una compra entre los tres actores

```
UNREAL                         BRIDGE (server)                 WEB + MetaMask
  │  POST /purchase-intent ───────►  guarda {pending} ──┐
  │  ◄── { requestId }              (Map en memoria)    │
  │                                                     │  GET /pending ──► ve la intención
  │                                 marca signing ◄───── POST /purchase-result {signing}
  │                                                     │  dispara MetaMask → el jugador FIRMA buy()
  │  GET /purchase-status ──► signing                   │  espera a que se mine la tx
  │                                 marca done+txHash ◄─ POST /purchase-result {done, txHash}
  │  GET /purchase-status ──► done, txHash ✓            │
```

La web atiende esto automáticamente: al conectar, `app.js` sondea `/api/pending` cada
3 s y procesa las intenciones **de la cuenta conectada** (reclama → firma → reporta).

### Probar la API con curl (antes de meter Unreal)

> Nota de entorno: el server corre en **Windows**; usa `localhost` desde una terminal
> de Windows (PowerShell). Desde WSL, `localhost` no alcanza al server de Windows.

```bash
A=0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266
# Lecturas
curl "http://localhost:8787/api/catalog"
curl "http://localhost:8787/api/inventory?address=$A"
curl "http://localhost:8787/api/progress?address=$A"
# Compra: Unreal registra la intención
curl -X POST "http://localhost:8787/api/purchase-intent" \
     -H "Content-Type: application/json" \
     -d "{\"address\":\"$A\",\"itemId\":2,\"quantity\":1}"     # → { requestId }
# (la WEB abierta en el navegador la firma sola; o simúlala con curl:)
curl "http://localhost:8787/api/pending"
curl -X POST "http://localhost:8787/api/purchase-result" -H "Content-Type: application/json" \
     -d '{"requestId":"<RID>","status":"signing"}'
curl -X POST "http://localhost:8787/api/purchase-result" -H "Content-Type: application/json" \
     -d '{"requestId":"<RID>","status":"done","txHash":"0x..."}'
# Unreal hace polling
curl "http://localhost:8787/api/purchase-status?requestId=<RID>"
```

### Probar el LOGIN SIWE con curl + la web

La firma del login necesita la wallet, así que el camino realista es: **curl hace de
Unreal** (intent + polling) y **la web abierta (con MetaMask) firma**.

1. Abre `http://localhost:8787` y conecta MetaMask (cuenta `0xf39F…2266`, red Anvil).
2. Como "Unreal", registra la intención de login y haz polling:
   ```bash
   A=0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266
   curl -X POST "http://localhost:8787/api/siwe/login-intent" \
        -H "Content-Type: application/json" -d "{\"address\":\"$A\"}"   # → { requestId, message }
   curl "http://localhost:8787/api/siwe/login-status?requestId=<RID>"   # pending → signing → done
   ```
3. La web detecta el login pendiente y abre MetaMask para **firmar el mensaje** (sin gas).
   Al firmar, el bridge verifica y `login-status` pasa a `done` con la `address`.

> Para una prueba **sin navegador**, se puede firmar el `message` con una wallet de
> prueba (`ethers` `Wallet.signMessage` = `personal_sign`) y enviar la firma a
> `POST /api/siwe/verify {requestId, signature}`. Así se validó este módulo (incluyendo
> nonce de un solo uso y rechazo de firmante incorrecto).

## Estructura del directorio (actualizada)

```
bridge/
├── server.js                 # Web estática + API /api/* (lecturas, compras, SIWE)
├── start-bridge.ps1          # Arranque en Windows (npm install si hace falta + server)
├── package.json              # deps del server: siwe + ethers (para SIWE)
├── node_modules/             # (gitignored) creado por `npm install`
├── public/                   # web: index.html, app.js, abi/*.json
└── test-client/              # cliente de prueba que simula a Unreal
```

> `node_modules/` está ignorado por git.
