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
├── server.js                 # Servidor estático mínimo (Node, sin dependencias)
├── public/
│   ├── index.html            # Estructura + estilos + carga de ethers.js (CDN)
│   ├── app.js                # Lógica: conectar, leer tienda/inventario, comprar, quemar
│   └── abi/
│       └── GameStore.json    # ABI extraído de los artefactos de Foundry
├── .env.example
└── README.md
```

Stack **mínimo**: HTML + JS, sin framework ni bundler. `ethers.js v6` se carga
como build UMD desde un CDN (variable global `ethers`). El servidor no usa
dependencias externas (solo módulos integrados de Node), así que **no hay
`npm install` ni `node_modules`**.

## El ABI y la dirección del contrato

- **ABI** (`public/abi/GameStore.json`): se extrajo del artefacto de Foundry con
  ```bash
  cd ../contracts
  forge inspect src/GameStore.sol:GameStore abi --json > ../bridge/public/abi/GameStore.json
  ```
  Si cambias el contrato y recompilas, vuelve a ejecutar ese comando para
  regenerarlo.
- **Dirección del contrato**: está en `public/app.js`, en la constante
  `CONTRACT_ADDRESS`, claramente señalada. En Anvil es **determinista** (el primer
  despliegue de la cuenta #0 siempre cae en
  `0x5FbDB2315678afecb367f032d93F642f64180aa3`). Si despliegas en otra dirección,
  actualiza esa constante (la dirección aparece en la salida de `forge script` o
  en `contracts/broadcast/.../run-latest.json`).

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
> Puerto configurable: `PORT=3000 node server.js` (por defecto 8787).

## Configurar MetaMask para Anvil

1. Añade una red personalizada: **RPC** `http://127.0.0.1:8545`, **Chain ID**
   `31337`, símbolo `ETH`.
2. Importa una de las cuentas de prueba que imprime Anvil (clave privada). La web
   avisará si estás en otra red distinta de la 31337.

> ⚠️ Las claves de Anvil son **públicas y de prueba**: solo para desarrollo local.
> Nunca importes una clave real ni envíes fondos reales a esas cuentas.

## Qué hace la web

- **Conectar wallet**: `window.ethereum` → muestra cuenta y red; avisa si no es 31337.
- **Tienda**: lee `priceOf(id)` / `isListed(id)` de cada item (0,1,2) y los muestra.
- **Comprar**: llama a `buy(id, 1)` con `value = priceOf(id)` → MetaMask pide firma →
  al confirmarse, refresca el inventario.
- **Inventario**: lee `balanceOf(cuenta, id)`; el botón **Vaciar** llama a
  `burn(cuenta, id, balance)`.
- **Errores visibles**: red incorrecta, firma rechazada, fondos insuficientes,
  reverts del contrato (decodificados gracias a los custom errors del ABI), etc.

> `node_modules/` está ignorado por git (aunque aquí no se usa).
