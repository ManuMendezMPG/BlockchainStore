# 01 — El smart contract: `GameStore` (ERC-1155)

> Parte de la guía de aprendizaje del proyecto. Este documento explica **por qué**
> está construido así el contrato, no solo qué hace. Si vienes de leer el código,
> aquí encontrarás el razonamiento detrás de cada decisión.

---

## 1. Qué hace el contrato y su papel en la arquitectura

`GameStore` es la **tienda de items del juego, vivida on-chain**. Cada item (una
espada, un escudo, una poción…) es un *token* ERC-1155, y el balance de ese token
para una dirección representa cuántas unidades posee ese jugador.

El contrato hace cuatro cosas:

1. **Mantiene un catálogo** de items con su precio (lo gestiona el *owner*).
2. **Vende** items: el jugador paga ETH y recibe los tokens (`buy`).
3. **Permite quemar** items para vaciar inventario (`burn`).
4. **Permite al owner retirar** lo recaudado (`withdraw`).

### Su papel en la arquitectura global

El contrato es la **fuente de verdad de la propiedad de items**. No confía en el
cliente del juego: la verdad de "cuántas espadas tengo" vive en la blockchain, no
en la memoria del juego (que podría manipularse). El flujo completo es:

```
Juego (Unreal)  →  Puente web local (Node + ethers.js)  →  MetaMask  →  GameStore (on-chain)
```

El juego nunca habla directamente con la cadena ni maneja claves: delega en el
puente, que usa MetaMask para que el jugador **firme** las transacciones de compra
contra `GameStore`. Este documento cubre solo la última pieza, el contrato.

---

## 2. Por qué ERC-1155 (y no ERC-721) para un inventario

Las tres normas de tokens más comunes:

| Estándar | Modelo | Ejemplo típico |
|----------|--------|----------------|
| **ERC-20** | Un solo tipo de token, fungible | Una moneda / divisa |
| **ERC-721** | Cada token es **único** (NFT), 1 contrato por colección | Arte coleccionable único |
| **ERC-1155** | **Múltiples tipos** de token en un contrato; cada tipo puede tener muchas unidades | Inventarios de juego |

Para un inventario de juego, ERC-1155 encaja mucho mejor que ERC-721:

- **Un inventario es "semi-fungible".** Da igual *qué* poción concreta tienes; lo que
  importa es que tienes *5 pociones*. Eso es un balance (`itemId → cantidad`), no 5
  NFTs únicos. ERC-1155 modela exactamente eso; ERC-721 te obligaría a acuñar un NFT
  distinto por cada unidad.
- **Un solo contrato para todo el catálogo.** Con ERC-721 normalmente despliegas un
  contrato por colección. Con ERC-1155, todos los items (espadas, escudos, pociones,
  y los que añadas mañana) viven en **un contrato**, identificados por `itemId`. Menos
  despliegues, menos direcciones que gestionar.
- **Más barato en gas.** Acuñar 100 pociones en ERC-1155 es incrementar un balance;
  en ERC-721 serían 100 NFTs con 100 entradas de propiedad. ERC-1155 además soporta
  operaciones **por lotes** (`balanceOfBatch`, `safeBatchTransferFrom`), ideal para
  mover varios items de golpe.

**Cuándo SÍ querrías ERC-721:** si cada item fuera realmente único e irrepetible
(p. ej. "la Espada Legendaria nº 1 con su historia propia"). Para una tienda con
stock repetible, ERC-1155 es la elección natural.

---

## 3. Por qué heredamos de cada contrato de OpenZeppelin

[OpenZeppelin](https://docs.openzeppelin.com/contracts/5.x/) son implementaciones de
referencia, auditadas y mantenidas. **No reinventamos los estándares**: heredamos de
código probado y solo añadimos nuestra lógica de tienda encima.

```solidity
contract GameStore is ERC1155, ERC1155Burnable, Ownable { ... }
```

### `ERC1155` — el estándar multi-token
Aporta toda la maquinaria del estándar: los balances (`balanceOf`,
`balanceOfBatch`), las transferencias seguras, las aprobaciones de operador
(`setApprovalForAll`), los hooks internos (`_mint`, `_burn`, `_update`) y el soporte
de metadatos (`uri`). Sobre esta base, nosotros solo escribimos la lógica de
compra/catálogo. Su constructor pide una `uri` de metadatos.

### `ERC1155Burnable` — quemar tokens
Extensión que añade `burn(account, id, amount)` y `burnBatch(...)`. La heredamos para
cumplir el requisito de "vaciar inventario" **sin escribir lógica de quema propia**.
Aporta algo crítico de seguridad: **ya trae el control de permisos**. Solo el dueño de
los tokens (o un operador que haya aprobado) puede quemarlos; si otro lo intenta,
revierte con `ERC1155MissingApprovalForAll`. Por eso no tuvimos que añadir ninguna
comprobación: la extensión la hace por nosotros (y lo verifica un test).

### `Ownable` — control de acceso
Da el concepto de "dueño del contrato" y el modificador `onlyOwner`. Lo usamos para
proteger las funciones administrativas: `setItem` (gestionar el catálogo) y
`withdraw` (retirar fondos). En OpenZeppelin **v5** el constructor exige el owner
inicial de forma explícita: `Ownable(msg.sender)` hace que quien despliega sea el
owner. (En v4 era implícito; este es un cambio importante entre versiones.)

> **Idea clave:** cada contrato base resuelve **una responsabilidad**
> (tokens / quema / permisos) y las componemos por herencia. Esto es composición de
> contratos, el patrón habitual en Solidity.

---

## 4. Explicación función por función

### `setItem(uint256 itemId, uint256 price)` — *onlyOwner*

```solidity
function setItem(uint256 itemId, uint256 price) external onlyOwner {
    priceOf[itemId] = price;
    isListed[itemId] = true;
    emit ItemListed(itemId, price);
}
```

Da de alta un item nuevo o **actualiza** el precio de uno existente. Marca
`isListed[itemId] = true` para registrar que el item existe en el catálogo. Solo el
owner puede llamarla (`onlyOwner`). Emite `ItemListed` para que clientes externos
(el puente, un indexador) puedan reaccionar.

### `buy(uint256 itemId, uint256 quantity)` — *payable*

```solidity
function buy(uint256 itemId, uint256 quantity) external payable {
    if (!isListed[itemId]) revert ItemNotListed(itemId);
    if (quantity == 0) revert InvalidQuantity();

    uint256 cost = priceOf[itemId] * quantity;
    if (msg.value < cost) revert InsufficientPayment(cost, msg.value);

    _mint(msg.sender, itemId, quantity, "");
    emit ItemPurchased(msg.sender, itemId, quantity, msg.value);
}
```

El corazón de la tienda. Es `payable` porque **recibe ETH**. Valida en orden:

1. **Que el item exista** (`isListed`) → si no, `ItemNotListed`.
2. **Que la cantidad sea > 0** → si no, `InvalidQuantity` (evita compras vacías).
3. **Que el pago cubra el coste** (`msg.value >= precio * cantidad`) → si no,
   `InsufficientPayment(required, sent)`.

Si todo pasa, **acuña** (`_mint`) `quantity` unidades del `itemId` al comprador
(`msg.sender`) y emite `ItemPurchased`. Las validaciones van **antes** del `_mint`:
en Solidity esto sigue el patrón *checks-effects-interactions* — comprueba primero,
modifica estado después.

### `burn(address account, uint256 id, uint256 value)` — *heredada*

No la escribimos nosotros: viene de `ERC1155Burnable`. El jugador la llama con su
propia dirección para vaciar inventario: `burn(miDireccion, itemId, cantidad)`. La
extensión garantiza que solo el dueño de los tokens (o un operador aprobado) pueda
quemarlos.

### `withdraw()` — *onlyOwner*

```solidity
function withdraw() external onlyOwner {
    uint256 balance = address(this).balance;
    if (balance == 0) revert NoFundsToWithdraw();

    (bool ok,) = payable(owner()).call{value: balance}("");
    if (!ok) revert WithdrawFailed();

    emit FundsWithdrawn(owner(), balance);
}
```

Envía **todo** el ETH recaudado al owner. Revierte con `NoFundsToWithdraw` si no hay
saldo, y con `WithdrawFailed` si la transferencia falla. El porqué del `call` se
explica en la sección de trade-offs.

---

## 5. Decisiones de diseño y sus trade-offs

### 5.1 `isListed` separado del precio

Usamos **dos** mappings: `priceOf[itemId]` y `isListed[itemId]`.

```solidity
mapping(uint256 => uint256) public priceOf;
mapping(uint256 => bool)    public isListed;
```

La alternativa tentadora sería "si el precio es 0, el item no existe". Pero eso
**confunde dos conceptos distintos**: "item gratuito" e "item inexistente". Con un
flag de existencia separado podemos listar items con precio 0 (promociones, items
gratis) sin ambigüedad, y la comprobación "el item existe" es explícita y legible.

- **Trade-off:** un mapping extra cuesta algo más de gas en `setItem` (un `SSTORE`
  adicional). A cambio ganamos claridad y un modelo de datos correcto. Para una
  tienda, merece la pena.

### 5.2 Custom errors en lugar de `require(string)`

```solidity
error InsufficientPayment(uint256 required, uint256 sent);
...
if (msg.value < cost) revert InsufficientPayment(cost, msg.value);
```

Desde Solidity 0.8.4 existen los *custom errors*. Frente al clásico
`require(cond, "mensaje")`:

- **Más baratos en gas**: un error se identifica por un selector de 4 bytes, no por
  una cadena de texto almacenada en el bytecode.
- **Llevan datos**: `InsufficientPayment(required, sent)` te dice cuánto hacía falta
  y cuánto se envió. Eso es oro para depurar y para que la UI del juego muestre un
  mensaje útil.
- **Trade-off:** son algo menos "auto-explicativos" si solo miras el hash del error
  en un explorador sin el ABI. Con el ABI (que sí tenemos), se decodifican perfecto.

### 5.3 Patrón `call` en `withdraw`

```solidity
(bool ok,) = payable(owner()).call{value: balance}("");
if (!ok) revert WithdrawFailed();
```

Hay tres formas de enviar ETH: `transfer`, `send` y `call`. Históricamente se usaba
`transfer`, pero **reenvía solo 2300 de gas**. Si el owner fuera un contrato (p. ej.
un multisig como Gnosis Safe), su función de recepción necesita más gas y `transfer`
fallaría. La recomendación actual es usar **`call`**, que reenvía todo el gas
disponible, y **comprobar el booleano de retorno** (lo hacemos: si `!ok`, revertimos).

- **Trade-off / cuidado:** `call` abre la puerta a *reentrancy* (el receptor podría
  re-entrar). Aquí es seguro porque `withdraw` no depende de estado mutable tras la
  llamada y está protegida por `onlyOwner`. En funciones más complejas se añadiría un
  `nonReentrant` (de `ReentrancyGuard`).

### 5.4 El exceso de pago NO se reembolsa — ⚠️ MEJORA PENDIENTE

```solidity
if (msg.value < cost) revert InsufficientPayment(cost, msg.value);
_mint(...);  // si msg.value > cost, el exceso se queda en el contrato
```

Aceptamos `msg.value >= cost`. Si el jugador paga **de más**, el exceso **se queda en
el contrato** (el owner lo retira luego). Lo hicimos así por simplicidad y porque el
requisito solo pedía "pago suficiente".

- **Trade-off:** mala UX y un pequeño footgun — un jugador que pague de más pierde la
  diferencia.
- **MEJORA PENDIENTE:** reembolsar el excedente al final de `buy`:
  ```solidity
  uint256 excess = msg.value - cost;
  if (excess > 0) {
      (bool ok,) = payable(msg.sender).call{value: excess}("");
      if (!ok) revert RefundFailed();
  }
  ```
  Implica usar de nuevo el patrón `call` y vigilar reentrancy (haz el `_mint` y los
  cambios de estado **antes** del reembolso — checks-effects-interactions).

### 5.5 Vendorización de `lib/` (sin submódulos git)

Instalamos OpenZeppelin con `forge install --no-git`, que **copia** los ficheros
dentro de `lib/` y los versiona como código normal, en vez de usar *submódulos* git.

- **Por qué:** el contrato vive dentro de un **monorepo** que ya tiene su propio
  `.git` en la raíz. Usar submódulos crearía submódulos git **anidados**, que son
  confusos de clonar y mantener.
- **Ventaja:** el repo es **autocontenido** y reproducible — quien lo clona tiene
  exactamente la versión de OZ que compila, sin pasos extra (`git submodule update`).
- **Trade-off:** `lib/` añade muchos ficheros al control de versiones (en nuestro caso
  ~780). El repo pesa más y los diffs de actualización de dependencias son grandes.
  Para un proyecto de aprendizaje, la simplicidad compensa.

---

## 6. Conceptos clave de Solidity que aparecen

- **wei / ether.** El ETH se mide internamente en **wei**; `1 ether = 1e18 wei`.
  Solidity tiene el sufijo `ether` (`0.01 ether` = `10_000_000_000_000_000` wei),
  que evita errores al contar ceros. **Todos los precios y pagos se hacen en wei.**
- **`payable`.** Marca una función (o dirección) capaz de **recibir ETH**. `buy` es
  `payable`; sin esa palabra, enviar ETH en la llamada revertiría. Para enviar ETH a
  una dirección con `.call{value:...}` esa dirección debe ser `payable`.
- **`msg.value`.** El ETH (en wei) **enviado junto con la llamada**. En `buy` lo
  comparamos con `cost` para validar el pago.
- **`msg.sender`.** **Quién** hace la llamada actual. Es el comprador en `buy`
  (recibe los tokens), y en el constructor es quien despliega (se vuelve owner vía
  `Ownable(msg.sender)`).
- **mint / `_mint`.** "Acuñar" = **crear** tokens nuevos y asignarlos a una
  dirección. `_mint(to, id, amount, data)` incrementa el balance del comprador. Es
  interno (`_`) porque solo nuestra lógica de `buy` debe poder acuñar, nunca alguien
  desde fuera.
- **Cheatcodes `vm.*` (en el script y los tests).** `vm` es un objeto especial de
  Foundry con "trucos" para entornos de prueba/scripting:
  - `vm.startBroadcast()` / `vm.stopBroadcast()` — delimitan las operaciones que se
    convierten en **transacciones reales** firmadas y enviadas a la red.
  - (En los tests) `vm.prank(addr)` — hace que la **siguiente** llamada parezca venir
    de `addr` (para simular distintos usuarios); `vm.deal(addr, x)` — da saldo;
    `vm.expectRevert(...)` — afirma que la llamada debe revertir con cierto error.

---

## 7. Flujo de despliegue local con Anvil

**Anvil** es el nodo Ethereum local de Foundry (chain id **31337**), pensado para
desarrollo. Despliega y verifica el contrato en tu máquina sin tocar ninguna red real.

### Arrancar el nodo (terminal A)

```bash
anvil
```

Imprime 10 cuentas de prueba con 10000 ETH cada una y escucha en
`http://127.0.0.1:8545`. Déjalo abierto.

### Desplegar (terminal B)

```bash
forge script script/DeployGameStore.s.sol:DeployGameStore \
  --rpc-url http://127.0.0.1:8545 \
  --private-key 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80 \
  --broadcast
```

- `--rpc-url` → a qué nodo enviar las transacciones.
- `--private-key` → cuenta que firma y paga el gas; queda como **owner** del contrato.
- `--broadcast` → sin él, `forge script` solo **simula**; con él, envía de verdad.

El script despliega `GameStore` y precarga 3 items (Espada 0.01, Escudo 0.005, Poción
0.001 ETH). En una Anvil recién arrancada el primer despliegue de la cuenta #0 es
**determinista**: siempre cae en `0x5FbDB2315678afecb367f032d93F642f64180aa3`.

### Verificar con `cast call` (lecturas, sin gas)

```bash
ADDR=0x5FbDB2315678afecb367f032d93F642f64180aa3
RPC=http://127.0.0.1:8545

cast call $ADDR "priceOf(uint256)(uint256)" 0 --rpc-url $RPC  # 10000000000000000 (0.01 ETH)
cast call $ADDR "isListed(uint256)(bool)"   0 --rpc-url $RPC  # true
cast call $ADDR "isListed(uint256)(bool)"  99 --rpc-url $RPC  # false (inexistente)
cast call $ADDR "owner()(address)"            --rpc-url $RPC  # cuenta #0
```

`cast call` ejecuta funciones de **solo lectura**: no cuesta gas ni mina bloques,
solo consulta el estado actual.

### ⚠️ Nota de seguridad sobre las claves de prueba

La clave privada usada arriba (`0xac09…ff80`, cuenta `0xf39F…2266`) es una de las
**claves públicas de prueba de Anvil**: las conoce literalmente todo el mundo. Sirven
**solo** para desarrollo local.

- **Nunca** uses una clave privada real en comandos, código, scripts ni `.env`
  versionados.
- **Nunca** envíes fondos reales a una dirección derivada de una clave de prueba: te
  los pueden robar al instante.
- En un flujo limpio, la clave se carga desde un `.env` (que está en `.gitignore`) y
  se pasa como `--private-key $PRIVATE_KEY`, nunca pegada en claro.

---

## Estado de esta fase

✅ `GameStore.sol` — ERC-1155 con catálogo, compra, burn y withdraw.
✅ 13 tests en verde (`forge test`).
✅ Script de despliegue verificado contra Anvil (deploy + `cast call`).

**Pendiente (anotado arriba):** reembolso del exceso de pago en `buy` (§5.4).

**Siguiente pieza:** el puente web local (`/bridge`) que conecta el juego con
MetaMask y firma las compras contra este contrato.
