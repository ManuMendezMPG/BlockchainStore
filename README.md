# Proyecto: Juego Unreal + Smart Contract (ERC-1155)

Monorepo que combina un **smart contract de Solidity** (items de una tienda como
ERC-1155), un **juego de Unreal Engine** y un **puente web local** que conecta el
juego con la wallet del jugador (MetaMask).

## Arquitectura

El proyecto se compone de cuatro piezas independientes pero coordinadas:

```
┌──────────────────┐        HTTP/WS local        ┌──────────────────┐
│   /unreal        │ ──────────────────────────► │   /bridge        │
│   Juego (cliente)│ ◄────────────────────────── │   Puente web      │
└──────────────────┘                             │   (Node + HTML +  │
                                                  │    ethers.js)     │
                                                  └────────┬──────────┘
                                                           │ window.ethereum
                                                           ▼
                                                  ┌──────────────────┐
                                                  │     MetaMask      │
                                                  │   (wallet)        │
                                                  └────────┬──────────┘
                                                           │ JSON-RPC
                                                           ▼
                                                  ┌──────────────────┐
                                                  │   /contracts      │
                                                  │   ERC-1155 on-chain│
                                                  └──────────────────┘
```

### 1. `/contracts` — Smart contract (Foundry / Solidity)
Contrato **ERC-1155** que representa los items de la tienda del juego (cada `id`
de token = un tipo de item; la cantidad = unidades poseídas). Se desarrolla, testea
y despliega con [Foundry](https://book.getfoundry.sh/). Es la **fuente de verdad**
de la propiedad de items: vive on-chain y no confía en el cliente del juego.

### 2. `/bridge` — Puente web local (Node.js + HTML + ethers.js)
El juego de Unreal **no habla directamente con la blockchain**. En su lugar, el
puente expone una pequeña web local (servida desde Node) que:
- Carga `ethers.js` en el navegador y se conecta a **MetaMask** vía `window.ethereum`.
- Construye y firma transacciones contra el contrato ERC-1155 (comprar/usar items).
- Expone un endpoint local (HTTP/WebSocket) que el juego consume.

Esto mantiene las claves privadas **dentro de MetaMask** (el juego nunca las ve) y
aísla la lógica web3 del motor de juego.

### 3. `/unreal` — Juego (Unreal Engine)
El cliente del juego. **Vive en Windows** y se sincroniza a este repo (ver
`unreal/README.md`). Llama al puente local para iniciar compras y leer el inventario
on-chain del jugador, y refleja el estado en la UI del juego.

### 4. `/docs` — Documentación
Diseño, decisiones de arquitectura, formato de mensajes entre juego y puente,
direcciones de despliegue por red, etc.

## Flujo general (ejemplo: comprar un item)

1. El jugador pulsa **"Comprar"** en el juego (`/unreal`).
2. El juego envía la petición al **puente local** (`/bridge`).
3. El puente abre/usa la web local que invoca a **MetaMask**; el jugador firma la
   transacción de compra contra el contrato **ERC-1155** (`/contracts`).
4. La transacción se mina; el contrato actualiza el balance del item para esa wallet.
5. El puente notifica al juego el resultado; el juego actualiza el inventario en la UI.

## Arranque rápido del entorno local (WSL)

Hay un script que automatiza toda la parte de WSL (Anvil + despliegue) con
**persistencia de estado**:

```bash
bash scripts/start-local.sh
```

Qué hace y por qué (ver comentarios en el propio script):

1. **Arranca Anvil con estado persistente** (`--state .anvil/state.json`, alias de
   load+dump, más `--state-interval` para volcados periódicos). Así el contrato y el
   inventario **sobreviven** a apagar y reencender. El estado vive en `.anvil/`, que
   está fuera de git.
2. **Espera al RPC con polling real** (no un `sleep` fijo): consulta
   `cast block-number` en bucle hasta que `http://127.0.0.1:8545` responde de verdad,
   con un límite de intentos. Si Anvil muere durante el arranque, lo detecta y aborta
   con las últimas líneas del log.
3. **Despliega de forma inteligente** según el estado del contrato en la dirección
   determinista (`0x5FbDB2…0aa3`). No basta con "¿hay bytecode?": el estado
   persistente puede contener una versión **antigua** del contrato. La decisión es:

   | Situación (detección) | Acción |
   |------------------------|--------|
   | **No hay bytecode** (`cast code` = `0x`) → cadena limpia | **Desplegar** |
   | Hay bytecode **y** responde el getter actual (`purchasedTotal`) → versión correcta | **Reutilizar** (no redespliega; conserva inventario) |
   | Hay bytecode **pero** el getter actual revierte → versión **antigua** | **Avisar**, reiniciar cadena limpia y **redesplegar** |

   El sondeo del getter (`purchasedTotal`, que solo existe en la versión actual)
   distingue "contrato correcto" de "contrato viejo". Cuando hay desajuste de
   versión, redesplegar **sobre la misma cadena** no serviría: la dirección
   determinista solo se obtiene con la cuenta #0 a **nonce 0**, así que el script
   reinicia con cadena limpia (aparta el estado viejo a `.anvil/state.json.stale`)
   y avisa claramente de que el inventario antiguo no es válido para el contrato nuevo.
4. **Resumen** con la dirección, el estado del contrato (desplegado / reutilizado /
   redesplegado) y los pasos que quedan en Windows.
5. **Ctrl+C limpio:** un `trap` manda `SIGTERM` a Anvil y espera a que **vuelque el
   estado** antes de salir.

> Requiere Foundry en WSL. El script añade `~/.foundry/bin` al PATH por si no está.

Después, **en Windows (PowerShell)**, arranca el puente:

```powershell
cd \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge
node server.js     # → http://localhost:8787
```

Abre `http://localhost:8787` y conecta MetaMask (red Anvil, chain id 31337).

> **Reiniciar Anvil:** con persistencia, el contrato se reutiliza en la misma
> dirección. Si **cambias los contratos** y reinicias, el script lo detecta (el getter
> nuevo no responde en el contrato viejo) y **redespliega solo automáticamente** la
> versión actual, apartando el estado viejo a `.anvil/state.json.stale`. Ya no tienes
> que borrar `.anvil/` a mano. Tras un redeploy, si MetaMask da *"nonce too high"*, usa
> **Configuración → Avanzado → Borrar datos de actividad**.

Detalle de **entorno**: Anvil/Foundry corren en **WSL**; Node (el puente) y MetaMask
en **Windows**. Se comunican por `localhost`, que se comparte entre ambos.

## Estado actual

- ✅ `/contracts` — `GameStore` (ERC-1155: tienda, compra, burn, withdraw), 13 tests
  en verde y script de despliegue verificado contra Anvil.
- ✅ `/bridge` — web local (Node + ethers.js v6) que conecta MetaMask, lee la tienda y
  el inventario, compra y vacía items. Verificado de punta a punta.
- ✅ `/scripts/start-local.sh` — arranque del entorno local con persistencia.
- ✅ `/docs` — guía de aprendizaje: `01-smart-contract.md`, `02-bridge.md`.
- ⏳ `/unreal` — pendiente (placeholder; ver `unreal/README.md`).

## Estructura del repo

```
.
├── contracts/   # Proyecto Foundry (ERC-1155: GameStore)
├── bridge/      # Puente web local (Node + ethers.js + MetaMask)
├── unreal/      # Placeholder; el proyecto Unreal se sincroniza desde Windows
├── docs/        # Guía de aprendizaje del proyecto
├── scripts/     # start-local.sh (arranque del entorno local en WSL)
├── .gitignore
└── README.md
```
