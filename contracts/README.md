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
├── script/             # Scripts de despliegue (forge script)
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

> El bytecode/ABIs compilados (`out/`), la caché (`cache/`) y los logs de deploy
> (`broadcast/`) están ignorados por git (ver `.gitignore` raíz).
