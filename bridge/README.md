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
> Puerto configurable: `PORT=3000 node server.js` (por defecto 8787).

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

> `node_modules/` está ignorado por git (aunque aquí no se usa).
