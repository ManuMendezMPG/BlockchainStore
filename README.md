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

## Estado actual

Estructura base recién inicializada. **Aún no** se ha inicializado el proyecto
Foundry ni instalado dependencias de Node. Cada carpeta tiene un README explicando
qué irá dentro. Consulta el README de cada pieza para los próximos pasos.

## Estructura del repo

```
.
├── contracts/   # Proyecto Foundry (ERC-1155)
├── bridge/      # Puente web local (Node + ethers.js + MetaMask)
├── unreal/      # Placeholder; el proyecto Unreal se sincroniza desde Windows
├── docs/        # Documentación del proyecto
├── .gitignore
└── README.md
```
