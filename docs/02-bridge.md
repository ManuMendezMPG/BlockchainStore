# 02 — El puente web: MetaMask ↔ contrato

> Parte de la guía de aprendizaje del proyecto. Continúa
> [01 — El smart contract](./01-smart-contract.md). Aquí explicamos la web local
> que conecta al jugador con el contrato `GameStore`, centrándonos en el **porqué**
> de cada decisión.

---

## 1. Qué es el puente y qué problema resuelve

El juego (Unreal Engine) **no puede hablar con MetaMask directamente**. MetaMask es
una extensión de **navegador**: vive en el contexto de una página web y expone su API
(`window.ethereum`) únicamente a JavaScript que corre en esa página. Un ejecutable de
Unreal no tiene `window.ethereum`, ni un navegador embebido con la extensión, ni forma
nativa de pedirle al usuario que firme una transacción.

Además, **el juego no debe tocar la clave privada del jugador**. Si el juego firmara
transacciones, tendría que custodiar la clave — justo lo que queremos evitar. La clave
debe quedarse donde el usuario ya confía en tenerla: MetaMask.

**El puente es el intermediario** que resuelve ambos problemas:

```
┌──────────────┐   HTTP/local    ┌───────────────────────────┐  window.ethereum  ┌──────────┐
│ Juego Unreal │ ───────────────►│  Puente web (Node + web)  │ ────────────────► │ MetaMask │
│  (cliente)   │ ◄───────────────│  HTML + JS + ethers.js    │ ◄──────────────── │ (firma)  │
└──────────────┘   resultado     └───────────────────────────┘                   └────┬─────┘
                                                                                       │ JSON-RPC
                                                                                       ▼
                                                                                  ┌──────────┐
                                                                                  │ GameStore│
                                                                                  │ on-chain │
                                                                                  └──────────┘
```

El puente es una **página web servida en local**. Tiene `window.ethereum`, así que
puede pedir a MetaMask que conecte la cuenta y firme. El juego solo le pedirá acciones
("compra el item 0") y recibirá resultados, **sin ver nunca la clave**.

> En esta fase el puente funciona **de forma aislada** (lo manejas tú desde el
> navegador). La conexión juego↔puente es la siguiente pieza; la base ya está lista.

---

## 2. Arquitectura de la pieza

```
bridge/
├── server.js                 # Servidor estático mínimo (Node, sin dependencias)
└── public/
    ├── index.html            # Estructura + estilos + carga de ethers.js (CDN)
    ├── app.js                # Lógica: conectar, leer, comprar, quemar
    └── abi/GameStore.json    # ABI extraído de los artefactos de Foundry
```

Cuatro componentes, cada uno con una responsabilidad:

1. **Servidor Node estático** (`server.js`). Su único trabajo es **servir los
   ficheros** (`index.html`, `app.js`, el ABI) por HTTP en `http://localhost:8787`.
   No tiene lógica de negocio. ¿Por qué hace falta un servidor para una web tan
   simple? Porque **abrir el HTML como `file://` no funciona con MetaMask**: bajo
   ese esquema el navegador no inyecta `window.ethereum` de forma fiable y bloquea
   `fetch()` (que usamos para cargar el ABI). Servir por `http://localhost` da un
   *origen* web real y todo funciona.

2. **Página web** (`index.html`). La estructura (botón de conectar, lista de la
   tienda, lista del inventario) y los estilos. Carga ethers.js y `app.js`.

3. **Lógica JS** (`app.js`). Conecta con MetaMask, lee el estado del contrato
   (tienda e inventario), construye las transacciones de compra/quema y maneja los
   errores. Es el cerebro del puente.

4. **El ABI** (`abi/GameStore.json`). El "contrato de interfaz": le dice a ethers
   qué funciones tiene `GameStore`, qué argumentos reciben y qué devuelven. Se
   **extrae de los artefactos de Foundry** (que se generan al compilar):

   ```bash
   cd contracts
   forge inspect src/GameStore.sol:GameStore abi --json > ../bridge/public/abi/GameStore.json
   ```

   > **Por qué extraerlo y no escribirlo a mano:** el ABI es la fuente de verdad de
   > la interfaz. Si cambias el contrato y recompilas, regeneras el ABI con ese
   > comando y la web queda sincronizada. Escribirlo a mano es frágil y propenso a
   > errores de tipos.

---

## 3. Conceptos clave de ethers.js

### Provider vs Signer — la distinción fundamental

- **Provider** = conexión de **solo lectura** a la blockchain. Consulta estado
  (balances, precios, bloques). **No puede firmar nada.** En la web usamos dos:
  - `new ethers.JsonRpcProvider(RPC_URL)` → habla **directamente** con el nodo Anvil.
    Lo usamos para mostrar los precios de la tienda **antes de conectar la wallet**.
  - `new ethers.BrowserProvider(window.ethereum)` → habla **a través de MetaMask**.

- **Signer** = representa **una cuenta concreta** y **puede firmar transacciones**.
  Se obtiene de MetaMask con `provider.getSigner()`. Lo más importante: **la clave
  privada vive dentro de MetaMask**. El signer solo *pide* firmas; tu código nunca
  ve ni toca la clave.

> Regla mental: si solo **lees**, te basta un *provider*. Si vas a **cambiar estado**
> (y por tanto firmar y pagar gas), necesitas un *signer*.

### Cómo se conecta con MetaMask

Son tres pasos, visibles en `connectWallet()`:

```js
// 1) Envolver el objeto que MetaMask inyecta (estándar EIP-1193).
const browserProvider = new ethers.BrowserProvider(window.ethereum);

// 2) Pedir permiso para acceder a las cuentas → ABRE EL POPUP de MetaMask.
await browserProvider.send("eth_requestAccounts", []);

// 3) Obtener el signer (la cuenta conectada que firmará).
const signer = await browserProvider.getSigner();
const account = await signer.getAddress();
```

- `window.ethereum` es la API que **inyecta** la extensión MetaMask en la página.
- `eth_requestAccounts` es la petición estándar que dispara el diálogo "¿Conectar
  esta web a tu wallet?". Sin ella, no tenemos permiso para ver la cuenta.
- Tras conectar, comprobamos la red con `await browserProvider.getNetwork()` y
  avisamos si `chainId !== 31337n`. **En ethers v6 los chainId son `BigInt`** (por
  eso el `n` final): hay que compararlos con BigInt, no con números normales.

También escuchamos cambios para no quedarnos con estado obsoleto:

```js
window.ethereum.on("accountsChanged", () => location.reload());
window.ethereum.on("chainChanged",    () => location.reload());
```

Recargar la página es la forma más simple y robusta de reflejar que el usuario
cambió de cuenta o de red en MetaMask.

### Read vs Write

| | **Read** (función `view`) | **Write** (transacción) |
|---|---|---|
| Usa | un **provider** | un **signer** |
| ¿Abre MetaMask? | No | Sí (pide **firma**) |
| ¿Cuesta gas? | No | Sí |
| Resultado | valor inmediato | hay que **esperar** a que se mine |

```js
// READ — contrato conectado a un provider. Inmediato, sin gas, sin popup.
const price = await readContract.priceOf(0);           // BigInt (wei)
const balance = await readContract.balanceOf(account, 0);

// WRITE — contrato conectado al signer. MetaMask pide firma; esperamos confirmación.
const tx = await writeContract.buy(0, 1, { value: price });
await tx.wait();   // ⏳ hasta que la transacción se incluye en un bloque
```

Creamos **dos instancias** del contrato para que la diferencia sea explícita:

```js
readContract  = new ethers.Contract(CONTRACT_ADDRESS, abi, readProvider); // solo lee
writeContract = new ethers.Contract(CONTRACT_ADDRESS, abi, signer);       // lee y escribe
```

### Construir una transacción `payable` desde JS

`buy(itemId, quantity)` es `payable`: recibe ETH. En ethers, el ETH a enviar va en
un objeto de **overrides** como **último argumento** de la llamada:

```js
const tx = await writeContract.buy(item.id, 1, { value: priceWei });
//                                ───┬───  ┬   ────────┬─────────
//                              argumentos   overrides: { value } → llega como msg.value
```

- Los primeros argumentos (`item.id`, `1`) son los parámetros de la función.
- `{ value: priceWei }` indica cuánto ETH (en **wei**) adjuntar. Ese valor llega al
  contrato como **`msg.value`** — exactamente lo que `buy` compara contra
  `precio * cantidad`.
- `priceWei` lo obtuvimos antes leyendo `priceOf(id)`, así que pagamos justo el
  precio del item.

`burn(account, id, value)` **no** es payable, así que se llama sin `{ value }`. El
jugador quema sus propios tokens, y la extensión `ERC1155Burnable` del contrato se
encarga de comprobar que es el dueño (ver doc 01).

---

## 4. Manejo de errores: `humanizeError` y el ABI completo

Las cosas fallan: el usuario rechaza la firma, está en la red equivocada, no tiene
fondos, o el contrato revierte. Una buena UI **traduce** esos fallos a algo legible.
La función `humanizeError(err)` hace ese mapeo:

```js
if (err?.code === "ACTION_REJECTED") return "Has rechazado la firma en MetaMask.";
if (err?.code === "INSUFFICIENT_FUNDS") return "Fondos insuficientes…";
if (err?.revert?.name) { /* custom error del contrato → mensaje específico */ }
if (err?.code === "NETWORK_ERROR") return "¿Está Anvil arrancado?";
```

ethers v6 normaliza muchos errores con un **`code`** estable
(`ACTION_REJECTED`, `INSUFFICIENT_FUNDS`, `NETWORK_ERROR`…), lo que nos permite
distinguirlos sin parsear cadenas frágiles.

### Por qué incluir el ABI **completo** (con los custom errors)

Cuando el contrato revierte con un *custom error* (p. ej. `InsufficientPayment`,
`ItemNotListed`, `ERC1155MissingApprovalForAll`), lo que viaja por el cable es un
**selector de 4 bytes** + sus datos codificados. Por sí solo, eso es ilegible.

Si el ABI que le diste a ethers **incluye la definición de esos errores**, ethers
puede **decodificarlos** y rellenar `err.revert.name` y `err.revert.args`:

```js
case "InsufficientPayment":
  return `Pago insuficiente: el contrato pedía ${r.args?.[0]} wei.`;
case "ItemNotListed":
  return `Ese item no existe en el catálogo (id ${r.args?.[0]}).`;
```

Por eso guardamos el ABI **entero** (no solo las 4 funciones que usamos): trae todos
los errores del contrato y de OpenZeppelin, y eso convierte un críptico
`0xb99e2ab7` en *"Pago insuficiente: el contrato pedía 10000000000000000 wei"*. Sin
el ABI completo, solo verías un hash. **El coste es trivial** (un fichero JSON algo
más grande) y la mejora de DX es enorme.

---

## 5. Decisiones de stack y sus porqués

### ethers v6 por CDN, sin bundler
Cargamos ethers como build **UMD** desde un CDN:

```html
<script src="https://cdn.jsdelivr.net/npm/ethers@6.13.4/dist/ethers.umd.min.js"></script>
```

- **UMD** expone una variable global `ethers` (`window.ethers`), así que `app.js` la
  usa directamente sin `import`.
- **Por qué:** mantener el stack mínimo. Sin `npm`, sin `webpack`/`vite`, sin paso de
  build. Editas un `.js`, recargas el navegador, listo. Para una pieza de aprendizaje
  y un puente local, la simplicidad gana.
- **Trade-off:** dependes de un CDN (necesitas red la primera vez; luego el navegador
  cachea) y fijas la versión en la URL. En producción se suele *bundlear* y servir
  ethers desde tu propio dominio, pero aquí no compensa la complejidad.

### Servidor sin dependencias
`server.js` usa **solo módulos integrados de Node** (`http`, `fs`, `path`).

- **Por qué:** evita `npm install` y la carpeta `node_modules` por completo. Un
  fichero, cero dependencias, cero superficie de mantenimiento o vulnerabilidades de
  terceros. Su única tarea es devolver ficheros estáticos.
- **Bonus en este entorno:** al solo *leer* ficheros, esquiva el problema de permisos
  de WSL (carpetas creadas como `root` desde Windows) que sí afectaría a un
  `npm install` corriendo como tu usuario de WSL.
- **Trade-off:** no trae *live-reload* ni middlewares. Para esto no hacen falta.

### La dirección determinista del contrato
`CONTRACT_ADDRESS` está fijada en `app.js`:

```js
const CONTRACT_ADDRESS = "0x5FbDB2315678afecb367f032d93F642f64180aa3";
```

- **Por qué funciona:** la dirección de un contrato se deriva de `(dirección del
  deployer, nonce)`. En una Anvil **recién arrancada**, la cuenta #0 tiene nonce 0,
  así que su **primer** despliegue **siempre** cae en esa dirección. Es estable entre
  reinicios *mientras* despliegues lo mismo en el mismo orden con la misma cuenta.
- **Cuándo cambia:** si despliegas desde otra cuenta, o no es el primer despliegue
  (nonce distinto). Entonces hay que actualizar la constante con la dirección que
  imprime `forge script` (o leerla de `contracts/broadcast/.../run-latest.json`).
- **Trade-off:** hardcodear es lo más simple para desarrollo local. En testnet/mainnet
  la dirección se gestionaría por red (p. ej. un fichero de despliegues por chainId).

---

## 6. Nota de entorno: Node en Windows, Anvil en WSL

⚠️ **Importante en este equipo**, porque las piezas viven en sistemas distintos:

- **Anvil y Foundry corren en WSL** (Ubuntu). Ahí se compila, testea y despliega el
  contrato.
- **Node está instalado en Windows**, no en WSL. Por eso el **servidor del puente se
  arranca desde una terminal de Windows** (PowerShell), no desde WSL.
- **MetaMask corre en el navegador de Windows.**

Funciona porque **`localhost` se comparte entre Windows y WSL**: un servicio que
escucha en `127.0.0.1:PUERTO` en WSL es accesible desde Windows en `localhost:PUERTO`
y viceversa. Así:

| Pieza | Dónde escucha | Quién la consume |
|-------|---------------|------------------|
| Anvil (RPC) | WSL, `127.0.0.1:8545` | el navegador (Windows) y forge (WSL) |
| Servidor del puente | Windows, `localhost:8787` | el navegador (Windows) |

**Qué terminal arranca cada cosa:**

| Terminal | Dónde | Comando |
|----------|-------|---------|
| A | WSL | `anvil` |
| B | WSL | `forge script … --broadcast` (deploy) |
| C | **Windows (PowerShell)** | `node server.js` en la carpeta `bridge` |

Para arrancar el servidor desde PowerShell, la ruta de la carpeta es la UNC del
sistema de ficheros de WSL:

```powershell
cd \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge
node server.js
# si cd a UNC da problemas:  node \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge\server.js
```

> El navegador y MetaMask también hablan con Anvil por `localhost` (lecturas y envío
> de transacciones firmadas). Como Anvil habilita **CORS** (`Access-Control-Allow-Origin: *`),
> el navegador puede llamar a su RPC sin bloqueos.

---

## 7. Rutina de arranque del entorno local completo

De cero, en orden:

1. **Arrancar Anvil** (Terminal A, WSL):
   ```bash
   anvil
   ```
   Deja la terminal abierta. Imprime 10 cuentas de prueba (claves **públicas**, solo
   desarrollo).

2. **Desplegar el contrato + catálogo** (Terminal B, WSL):
   ```bash
   cd contracts
   forge script script/DeployGameStore.s.sol:DeployGameStore \
     --rpc-url http://127.0.0.1:8545 \
     --private-key 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80 \
     --broadcast
   ```
   Anota la dirección desplegada (debería ser la determinista de siempre).

3. **Arrancar el servidor del puente** (Terminal C, **PowerShell de Windows**):
   ```powershell
   cd \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge
   node server.js
   ```

4. **Abrir la web**: `http://localhost:8787`. Conecta MetaMask (red Anvil, chain id
   31337) y usa la tienda.

### ⚠️ Reiniciar Anvil obliga a redeployar

Anvil guarda su estado **en memoria**: al pararlo y volverlo a arrancar, la cadena
**empieza de cero**. El contrato que habías desplegado **ya no existe**. Síntomas en
la web: las lecturas devuelven 0 / "no disponible" o fallan. **Solución:** vuelve a
ejecutar el paso 2 (deploy). Como la cuenta #0 vuelve a estar a nonce 0, el contrato
recae en la **misma dirección determinista**, así que normalmente **no** hace falta
tocar `CONTRACT_ADDRESS` en `app.js`.

### ⚠️ Error "nonce too high" en MetaMask → *Clear activity*

Tras reiniciar Anvil verás a menudo un error al firmar del tipo **"nonce too high"**
(o transacciones que se quedan colgadas).

- **Por qué pasa:** MetaMask **cachea el nonce** de tu cuenta por red. Si antes de
  reiniciar habías hecho, p. ej., 5 transacciones, MetaMask cree que tu próximo nonce
  es 5. Pero al reiniciar Anvil, la cadena cree que tu cuenta está a nonce 0. MetaMask
  envía con nonce 5 y la cadena lo rechaza por "demasiado alto".
- **El fix:** en MetaMask, **Configuración → Avanzado → "Borrar datos de actividad y
  nonce"** (*Clear activity tab data*) con la cuenta y la red Anvil seleccionadas.
  Esto resetea el contador de nonce de MetaMask para esa red, sin tocar tus fondos ni
  tu clave. Tras hacerlo, las transacciones vuelven a firmarse con el nonce correcto.
- **Regla práctica:** cada vez que reinicies Anvil → **redeploy** + **Clear activity**
  en MetaMask. Son los dos pasos que evitan el 90% de los problemas de desarrollo local.

---

## Estado de esta fase

✅ Servidor estático sin dependencias sirviendo la web.
✅ Conexión con MetaMask (`BrowserProvider` + `eth_requestAccounts` + `getSigner`).
✅ Lectura de tienda (`priceOf`/`isListed`) e inventario (`balanceOf`).
✅ Compra (`buy` payable con `value`) y vaciado (`burn`), firmando en MetaMask.
✅ Manejo de errores legible apoyado en el ABI completo (custom errors).
✅ Verificado de punta a punta en el navegador.

**Siguiente pieza:** la integración con **Unreal Engine** (`/unreal`), que pedirá
acciones al puente en lugar de operar tú a mano desde el navegador.
