# /contracts — Smart contract (Foundry)

Proyecto de Solidity gestionado con [Foundry](https://book.getfoundry.sh/).
Contiene `GameStore.sol`, un contrato **ERC-1155** que representa los items de la
tienda del juego. El jugador compra items pagando ETH (de prueba en Anvil) y puede
quemarlos para vaciar su inventario.

## Estructura

```
contracts/
├── src/
│   └── GameStore.sol   # ERC-1155 + ERC1155Burnable + Ownable
├── test/
│   └── GameStore.t.sol # Tests (forge test) — 13 casos
├── script/
│   └── DeployGameStore.s.sol # Despliega GameStore + precarga 3 items
├── lib/                # Dependencias vendorizadas (openzeppelin-contracts, forge-std)
├── foundry.toml        # Configuración + remappings + solc
├── .env.example        # Variables necesarias (ver abajo)
└── .env                # Secretos locales (NO se commitea)
```

## El contrato `GameStore`

| Pieza | Para qué |
|-------|----------|
| `ERC1155` | Multi-token: cada `itemId` es un tipo de item; el balance = unidades poseídas. Un solo contrato para todos los items. |
| `ERC1155Burnable` | Añade `burn`/`burnBatch` para que el jugador vacíe su inventario (solo sus propios tokens o con aprobación). |
| `Ownable` | Control de acceso del owner para `setItem` (catálogo) y `withdraw` (recaudación). |

Funciones principales:
- `setItem(itemId, price)` — *(owner)* da de alta o actualiza el precio de un item.
- `buy(itemId, quantity)` — *payable*: valida existencia/cantidad/pago y mintea.
- `burn(account, itemId, amount)` — *(heredada)* el jugador quema sus items.
- `withdraw()` — *(owner)* retira el ETH recaudado.

Errores con **custom errors** (`ItemNotListed`, `InsufficientPayment`,
`InvalidQuantity`, `NoFundsToWithdraw`, `WithdrawFailed`).

## Dependencias y remappings

OpenZeppelin Contracts **v5.1.0**, vendorizado en `lib/` (sin submódulo git, para
evitar submódulos anidados dentro del monorepo). Remappings en `foundry.toml`:

```
@openzeppelin/contracts/=lib/openzeppelin-contracts/contracts/
forge-std/=lib/forge-std/src/
```

## Comandos

```bash
forge build          # compilar
forge test           # ejecutar tests
forge test -vvv      # tests con trazas detalladas
anvil                # nodo local (en otra terminal) -> http://127.0.0.1:8545
```

> Nota de entorno (WSL): si `forge` no está en el PATH, añade
> `export PATH="$HOME/.foundry/bin:$PATH"`.

## Levantar el entorno local de cero

Necesitas **dos terminales**: una para el nodo Anvil y otra para los comandos.

### 1. Arrancar Anvil (terminal A)

```bash
anvil
```

Anvil es un nodo Ethereum local (chain id **31337**) que escucha en
`http://127.0.0.1:8545`. Al arrancar imprime 10 cuentas de prueba con sus claves
privadas y 10000 ETH cada una. **Esas claves son públicas y conocidas por todo el
mundo: solo sirven para desarrollo local; nunca las uses en una red real ni les
envíes fondos reales.** Deja esta terminal abierta.

### 2. Compilar y testear (terminal B)

```bash
forge build
forge test
```

### 3. Desplegar GameStore con el catálogo de ejemplo (terminal B)

```bash
forge script script/DeployGameStore.s.sol:DeployGameStore \
  --rpc-url http://127.0.0.1:8545 \
  --private-key 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80 \
  --broadcast
```

- `--rpc-url`  → a qué nodo enviar las transacciones (el Anvil local).
- `--private-key` → cuenta que firma y paga el gas. La de arriba es la **cuenta #0
  de Anvil** (`0xf39F…2266`), una clave **pública de prueba**, NUNCA una real.
  Esa cuenta queda como **owner** del contrato (porque `GameStore` es
  `Ownable(msg.sender)`), por eso puede ejecutar los `setItem` del script.
- `--broadcast` → sin este flag, `forge script` solo simula. Con él, firma y envía
  las transacciones de verdad.

> En lugar de pegar la clave en el comando, en un flujo más limpio se carga desde
> `.env` (`PRIVATE_KEY=...`) y se pasa `--private-key $PRIVATE_KEY`. Recuerda que
> `.env` está en `.gitignore`.

El despliegue es determinista en una Anvil recién arrancada: el primer contrato
desplegado por la cuenta #0 siempre cae en
`0x5FbDB2315678afecb367f032d93F642f64180aa3`. El script imprime esa dirección y los
items creados.

### 4. Verificar que los items existen (terminal B)

`cast call` hace llamadas de solo-lectura (no cuestan gas ni minan bloques):

```bash
ADDR=0x5FbDB2315678afecb367f032d93F642f64180aa3
RPC=http://127.0.0.1:8545

cast call $ADDR "priceOf(uint256)(uint256)" 0 --rpc-url $RPC   # 10000000000000000 (0.01 ETH)
cast call $ADDR "isListed(uint256)(bool)"   0 --rpc-url $RPC   # true
cast call $ADDR "priceOf(uint256)(uint256)" 1 --rpc-url $RPC   #  5000000000000000 (0.005 ETH)
cast call $ADDR "priceOf(uint256)(uint256)" 2 --rpc-url $RPC   #  1000000000000000 (0.001 ETH)
cast call $ADDR "isListed(uint256)(bool)"  99 --rpc-url $RPC   # false (item inexistente)
cast call $ADDR "owner()(address)"            --rpc-url $RPC   # 0xf39F…2266 (cuenta #0)
```

### 5. (Opcional) Simular una compra

```bash
# Comprar 1 unidad del item 0 pagando su precio (0.01 ETH), como la cuenta #0:
cast send $ADDR "buy(uint256,uint256)" 0 1 \
  --value 0.01ether \
  --rpc-url $RPC \
  --private-key 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80

# Comprobar el balance del item 0 para esa cuenta (debe ser 1):
cast call $ADDR "balanceOf(address,uint256)(uint256)" 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266 0 --rpc-url $RPC
```

> El bytecode/ABIs compilados (`out/`), la caché (`cache/`) y los logs de deploy
> (`broadcast/`) están ignorados por git (ver `.gitignore` raíz).
