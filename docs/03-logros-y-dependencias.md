# 03 — Logros, dependencias y arquitectura entre contratos

> Parte de la guía de aprendizaje del proyecto. Cierra la fase de contratos.
> Continúa [01 — El smart contract](./01-smart-contract.md) y
> [02 — El puente web](./02-bridge.md). Material pensado también para **presentar
> en una consultora**: cada decisión va con su porqué.

---

## 0. La narrativa: qué va on-chain y qué no

La pregunta de diseño más importante de todo el proyecto no es técnica, es de
**modelo**: *¿qué merece vivir en la blockchain y qué no?* Meterlo todo on-chain es
caro, lento y rígido; no meter nada desaprovecha la propiedad real. La regla que
seguimos:

> **On-chain: propiedad, progreso e identidad. Off-chain: uso, sesión y reglas de
> presentación.**

La metáfora de **Zelda** lo hace intuitivo:

| Concepto Zelda | Aquí | Dónde vive | Por qué |
|----------------|------|------------|---------|
| Que **tienes** una espada, un arco, 12 flechas | Balances ERC-1155 en `GameStore` | **On-chain** | Es *propiedad*: debe ser verificable, transferible y a prueba de trampas del cliente. |
| **Medallones** de las mazmorras (progreso) | Logros en `Achievements` | **On-chain** | Es *identidad y progreso ganado*: no debería poder falsificarse. |
| **Disparar** una flecha, **beber** una poción, **romper** una botella | Lógica de uso | **Off-chain (Unreal)** | Es *consumo de sesión*: ocurre muchas veces por segundo, no puede costar gas ni esperar bloques. |
| Que la **mochila tiene 6 huecos** y cómo se colocan | Slots / UI | **Off-chain (Unreal)** | Es *regla de presentación*: cómo se muestra y organiza lo que ya posees. |

Traducido a responsabilidades del contrato: **`GameStore` gestiona propiedad,
economía y dependencias; `Achievements` gestiona identidad/progreso.** El "usar"
(gastar flechas, beber pociones) y los slots **no tocan la cadena**: son de Unreal.

Esta separación es justo el argumento de venta de la demo: *"la propiedad y el
progreso del jugador son suyos de verdad y verificables; la jugabilidad sigue siendo
rápida y barata porque vive en el cliente"*.

---

## 1. GameStore ampliado

### 1.1 Reembolso de excedente: por qué va al final

`buy()` ahora **devuelve el cambio**: si pagas de más, recuperas la diferencia. El
*dónde* se coloca ese reembolso dentro de la función es una decisión de seguridad,
no de estilo. Seguimos el patrón **checks-effects-interactions (CEI)**:

```solidity
function buy(uint256 itemId, uint256 quantity) external payable nonReentrant {
    // 1) CHECKS — validar TODO antes de tocar estado
    if (!isListed[itemId]) revert ItemNotListed(itemId);
    if (quantity == 0) revert InvalidQuantity();
    uint256 cost = priceOf[itemId] * quantity;
    if (msg.value < cost) revert InsufficientPayment(cost, msg.value);
    _checkDependencies(itemId, quantity);

    // 2) EFFECTS — cambiar el estado del contrato
    if (itemId == POCION_VIDA || itemId == POCION_MANA) _burn(msg.sender, BOTELLA_VACIA, quantity);
    _mint(msg.sender, itemId, quantity, "");
    purchasedTotal[msg.sender][itemId] += quantity;
    totalSpent[msg.sender] += cost;
    emit ItemPurchased(msg.sender, itemId, quantity, msg.value);

    // 3) INTERACTIONS — llamadas externas, SIEMPRE al final
    _checkAchievements(msg.sender);              // (a) acuñar logros
    uint256 excess = msg.value - cost;
    if (excess > 0) {                            // (b) devolver el cambio
        (bool ok,) = payable(msg.sender).call{value: excess}("");
        if (!ok) revert RefundFailed();
        emit ExcessRefunded(msg.sender, excess);
    }
}
```

**Por qué el reembolso es lo último.** Enviar ETH con `call` cede temporalmente el
control al receptor: si fuera un contrato malicioso, podría **reentrar** y volver a
llamar a `buy()` antes de que la primera llamada termine. Si el reembolso ocurriera
*antes* de actualizar balances y contadores, esa reentrada vería un estado a medias y
podría explotarlo (p. ej. comprar "gratis" o duplicar). Colocándolo **después de
todos los effects**, cuando reentra ya todo está consolidado: una reentrada sería,
como mucho, otra compra legítima.

**Doble cinturón:** además aplicamos el modificador `nonReentrant` de OpenZeppelin,
que bloquea cualquier reentrada a `buy()` de raíz. CEI + `nonReentrant` es el patrón
estándar: el orden correcto *y* el candado. Usamos `call` (no `transfer`) por la
misma razón que en `withdraw` (ver doc 01): reenvía todo el gas y comprobamos el
booleano de retorno.

> Esto cierra la **MEJORA PENDIENTE** que dejamos anotada en el doc 01 (§5.4): el
> excedente ya no se queda atrapado en el contrato.

### 1.2 Reglas de dependencia

El contrato ya no solo cobra: **modela las reglas del mundo**. Un jugador no puede
comprar cualquier cosa en cualquier orden. Todas las comprobaciones viven en
`_checkDependencies` (fase de *checks*) y revierten con *custom errors* claros.

**a) Arco → Flecha, y capacidad de carcaj.** No tiene sentido tener flechas sin arco,
ni más flechas de las que cabe en tu carcaj:

```solidity
if (itemId == FLECHA) {
    if (balanceOf(msg.sender, ARCO) == 0) revert NeedBow();
    uint256 cap = quiverCapacity(msg.sender);
    uint256 current = balanceOf(msg.sender, FLECHA);
    if (current + quantity > cap) revert QuiverCapacityExceeded(cap, current, quantity);
}
```

La capacidad se deriva del **mayor carcaj que poseas** (no se suman):

```solidity
function quiverCapacity(address a) public view returns (uint256) {
    if (balanceOf(a, CARCAJ_20) > 0) return 20;
    if (balanceOf(a, CARCAJ_10) > 0) return 10;
    if (balanceOf(a, CARCAJ_5)  > 0) return 5;
    return 0;               // sin carcaj, no puedes llevar flechas
}
```

**b) Botella → Poción (consumo).** Una poción "es" una botella llena. Comprar una
poción **consume** una botella vacía (la quema) y acuña la poción:

```solidity
// check:  ¿tienes botellas suficientes?
if (have < quantity) revert NeedEmptyBottle(quantity, have);
// effect: quemar botellas y acuñar pociones
_burn(msg.sender, BOTELLA_VACIA, quantity);
_mint(msg.sender, itemId, quantity, "");
```

Esto modela un **estado** (vacía → llena) con dos tokens distintos, sin meter en la
cadena el acto de *beber* (eso es de sesión, off-chain).

**c) Evolución del carcaj.** El carcaj sube de nivel con requisitos:

- `carcaj_10` solo se compra si ya posees `carcaj_5` (`NeedQuiver5`).
- `carcaj_20` solo se compra si has **desbloqueado el logro ARQUERO**
  (`NeedArcheroAchievement`).

```solidity
} else if (itemId == CARCAJ_10) {
    if (balanceOf(msg.sender, CARCAJ_5) == 0) revert NeedQuiver5();
} else if (itemId == CARCAJ_20) {
    if (address(achievements) == address(0) || !achievements.hasArquero(msg.sender))
        revert NeedArcheroAchievement();
}
```

El carcaj_20 es el ejemplo más bonito de la demo: una compra **gateada por progreso
on-chain**. No es dinero lo que lo desbloquea, es *haberte ganado* el logro del
Arquero. Es el contrato leyendo el progreso del jugador (en el otro contrato) para
decidir qué le deja comprar.

### 1.3 Contadores acumulados: por qué no basta el balance

Añadimos `purchasedTotal[jugador][item]`, que **solo incrementa** en cada compra. Es
una métrica distinta del **balance** (lo que tienes *ahora*), y la diferencia es la
clave del logro del Arquero:

> **El balance sube y baja; el contador histórico solo sube.**

El caso concreto: ARQUERO requiere **20 flechas compradas históricas**, pero tu
carcaj solo llega a 10 (sin carcaj_20). ¿Cómo llegas a 20? Comprando, gastando y
volviendo a comprar:

```
compra 10 flechas   → balance 10, histórico 10   (carcaj_10, lleno)
quema 10 flechas    → balance  0, histórico 10   (las "usaste")
compra 10 flechas   → balance 10, histórico 20   → ¡ARQUERO desbloqueado!
```

Si el logro mirara el **balance**, sería imposible (nunca pasas de 10 a la vez). Al
mirar el **acumulado**, premia el *esfuerzo a lo largo del tiempo*, que es justo lo
que representa un logro. Y encaja con la narrativa: gastar flechas es off-chain
(Unreal), pero el *haberlas comprado* queda registrado on-chain para siempre.

`totalSpent[jugador]` es el mismo patrón para el dinero: acumula el gasto real (sin
contar el excedente reembolsado) y alimenta el logro del Mercader.

---

## 2. Achievements: medallones de progreso

Contrato ERC-1155 **separado** (sus ids empiezan en 0 sin chocar con los items). Tres
medallones:

| Medallón | Condición | Tipo | Por qué ese tipo |
|----------|-----------|------|------------------|
| **ARQUERO** (0) | 20 flechas compradas históricas | **Soulbound** | Es mérito personal; desbloquea el carcaj_20. No tiene sentido "vender tu maestría". |
| **MERCADER** (1) | Gasto acumulado > umbral | **Transferible** + rareza | Es un coleccionable/estatus; puede tener mercado secundario. |
| **COLECCIONISTA** (2) | Poseer el set completo (espada+escudo+arco+algún carcaj) | **Soulbound** | Identidad de "completista"; ligada a la cuenta. |

La mezcla **soulbound/transferible** es deliberada y es un buen punto de
conversación en la demo: *no todos los activos digitales deben comportarse igual*. El
progreso ganado se queda contigo (soulbound); el coleccionable puede circular.

### 2.1 Cómo se implementa soulbound

En ERC-1155 de OpenZeppelin v5, **toda** transferencia, mint y burn pasa por un único
hook interno: `_update(from, to, ids, values)`. Sobrescribiéndolo controlamos los
movimientos. La clave es leer los **tres casos** por los extremos:

| Caso | `from` | `to` | ¿Permitido para soulbound? |
|------|--------|------|----------------------------|
| **Mint** (acuñar) | `address(0)` | jugador | ✅ Sí — así nace el medallón |
| **Burn** (quemar) | jugador | `address(0)` | ✅ Sí — el dueño puede destruirlo |
| **Transferencia** | jugador A | jugador B | ❌ No — esto es lo que bloqueamos |

```solidity
function _update(address from, address to, uint256[] memory ids, uint256[] memory values)
    internal override
{
    if (from != address(0) && to != address(0)) {      // transferencia REAL
        for (uint256 i = 0; i < ids.length; i++)
            if (ids[i] == ARQUERO || ids[i] == COLECCIONISTA) revert Soulbound(ids[i]);
    }
    super._update(from, to, ids, values);              // sigue el flujo normal
}
```

Solo entramos en el bucle de bloqueo cuando **ambos** extremos son distintos de cero
(transferencia real), y solo revertimos para ARQUERO y COLECCIONISTA. MERCADER nunca
entra en la condición → se comporta como un ERC-1155 normal y **es transferible**. Es
soulbound *selectivo* por id, en un solo contrato.

### 2.2 La rareza pseudoaleatoria del Mercader — y por qué NO es segura

Al acuñar MERCADER se le asigna una rareza con pesos (Bronce 60% / Plata 30% / Oro
10%):

```solidity
uint256 rand = uint256(keccak256(abi.encodePacked(
    blockhash(block.number - 1), block.timestamp, block.prevrandao, to, _nonce++
))) % 100;
if (rand < 60) return Rarity.Bronce;
if (rand < 90) return Rarity.Plata;
return Rarity.Oro;
```

**Esto es válido para una demo local, pero sería un fallo grave en producción**, y
conviene decirlo claro en una presentación de consultoría:

- **Es predecible.** Todas las entradas (`timestamp`, `prevrandao`, `blockhash`, la
  dirección, el nonce) son **públicas o calculables**. Cualquiera puede computar el
  resultado *antes* de enviar la transacción.
- **Es abortable.** Si la llamada viniera desde otro contrato, ese contrato podría
  **simular el resultado y revertir si no sale "Oro"**, reintentando hasta ganar. La
  aleatoriedad que puedes ver venir y cancelar no es aleatoriedad.
- **Es influenciable.** `timestamp` y `prevrandao` los **propone el validador** del
  bloque, que tiene cierto margen para sesgarlos o para retrasar la inclusión de tu
  transacción hasta un bloque que le convenga.

**Qué se usa en producción: un oráculo de aleatoriedad verificable (VRF), como
Chainlink VRF.** El patrón es de **dos transacciones**:

1. El contrato *pide* aleatoriedad (`requestRandomWords`).
2. El oráculo responde en una **transacción posterior** (callback) con un número
   **acompañado de una prueba criptográfica** que el contrato verifica on-chain.

Como el número no existe hasta el callback y va firmado, **ni el jugador ni el
validador pueden conocerlo ni manipularlo por adelantado**. El coste es complejidad
(suscripción, gestión del callback, asincronía) y por eso aquí lo dejamos simple —
**con un aviso grande en el código** para que nadie lo copie tal cual a producción.

---

## 3. Arquitectura entre contratos

### 3.1 Un contrato llamando a otro vía interfaz

GameStore no importa el contrato Achievements entero: guarda una referencia tipada
con una **interfaz mínima**, solo las funciones que usa.

```solidity
interface IAchievements {
    function isUnlocked(address account, uint256 id) external view returns (bool);
    function hasArquero(address account) external view returns (bool);
    function mintArquero(address to) external;
    function mintMercader(address to) external;
    function mintColeccionista(address to) external;
}
IAchievements public achievements;   // dirección + esa interfaz
```

**Por qué interfaz y no el contrato completo:** desacopla. GameStore solo necesita
saber *cómo llamar*, no la implementación interna de Achievements. Es más limpio,
compila menos y evita dependencias circulares.

### 3.2 El salto de `msg.sender`

Cuando GameStore ejecuta `achievements.mintArquero(buyer)`, hace un **CALL externo**:
la ejecución salta al otro contrato, que corre sobre **su propio almacenamiento**. Y,
crucialmente, dentro de esa llamada:

```
msg.sender (dentro de Achievements) == address(GameStore)
```

No es la dirección del jugador: es la del **contrato que llama**. Ese desplazamiento
de `msg.sender` es lo que hace posible el control de permisos: Achievements puede
exigir que quien acuña sea GameStore y nadie más.

### 3.3 Control de permisos del minter

```solidity
address public minter;                       // fijado por el owner de Achievements
modifier onlyMinter() { if (msg.sender != minter) revert NotMinter(); _; }
function mintArquero(address to) external onlyMinter { ... }
```

Los medallones **solo deben nacer como consecuencia de la lógica de la tienda**,
jamás por un jugador llamando directamente. La autorización se reduce a una variable:
tras desplegar, el owner ejecuta `achievements.setMinter(address(gameStore))`. A
partir de ahí, gracias al salto de `msg.sender`, solo GameStore pasa el `onlyMinter`.

> El cableado es **bidireccional**: `GameStore.setAchievements(ach)` (para que sepa a
> quién llamar) y `Achievements.setMinter(store)` (para autorizarlo). Ambos los hace
> el owner en el script de despliegue, *después* de desplegar los dos contratos.

### 3.4 Atomicidad: la compra y el logro, todo o nada

La acuñación del logro ocurre **dentro de la misma transacción** que la compra. En
Ethereum una transacción es **atómica**: o se aplican *todos* sus cambios, o se
revierte *todo*. Consecuencia directa:

> No existe el estado intermedio "compré la flecha número 20 pero el medallón ARQUERO
> no se acuñó". O pasan las dos cosas, o no pasa ninguna.

Esto elimina toda una clase de bugs de sincronización que sí tendrías con un sistema
off-chain (dos bases de datos que se pueden desincronizar). Aquí la coherencia es una
propiedad del medio.

### 3.5 Defensa en dos capas

Esa atomicidad tiene un filo peligroso: **si la llamada al logro revierte, revierte
la compra entera**. Imagina a un jugador que ya tiene ARQUERO comprando más flechas:
si GameStore intentara re-acuñar, `Achievements` revertiría con `AlreadyUnlocked`
¡y la compra de flechas fallaría sin motivo aparente!

Lo evitamos con **dos capas de defensa**:

1. **GameStore comprueba antes de llamar.** `_checkAchievements` solo invoca el mint
   si el hito se cumple **y** `!isUnlocked(...)`. Nunca provoca el revert.
   ```solidity
   if (purchasedTotal[buyer][FLECHA] >= ARQUERO_ARROWS && !achievements.isUnlocked(buyer, ACH_ARQUERO))
       achievements.mintArquero(buyer);
   ```
2. **Achievements se protege solo.** Aun así, `mintArquero` revierte con
   `AlreadyUnlocked` si se le llamara dos veces. Es una red de seguridad por si otro
   minter (futuro) no hiciera la comprobación previa.

La capa 1 garantiza el funcionamiento normal; la capa 2 garantiza la **invariante**
("un medallón único nunca se acuña dos veces") pase lo que pase. Un test verifica
justo esto: comprar más flechas tras tener ARQUERO **no revierte** y **no duplica** el
medallón.

---

## 4. Estado de la fase de contratos

✅ `GameStore` — tienda ERC-1155 con compra/burn/withdraw, **reembolso de excedente**,
**reglas de dependencia** (arco/flecha/carcaj, botella/poción, evolución de carcaj,
gate por logro) y **contadores acumulados**.
✅ `Achievements` — 3 medallones, **soulbound selectivo**, **minter autorizado**,
rareza pseudoaleatoria (con su aviso).
✅ Conexión entre contratos (interfaz + minter), con atomicidad y defensa en dos capas.
✅ **36 tests en verde**; script de despliegue actualizado (ambos contratos
conectados) y verificado contra Anvil, manteniendo la dirección determinista de
GameStore para el bridge.

### Cómo encaja en la demo (resumen para presentar)

- **On-chain (verificable, del jugador):** qué posee (items), cuánto ha progresado
  (contadores, logros) y su identidad (medallones soulbound).
- **Off-chain (rápido, de sesión):** usar los items (disparar, beber, romper) y las
  reglas de presentación (mochila de 6 slots) — en Unreal.
- **El puente** (doc 02) conecta ambos mundos vía MetaMask, sin que el juego toque
  nunca la clave del jugador.

**Pendiente fuera de contratos:** actualizar el catálogo del `bridge` a los 10 items
(hoy aún muestra las etiquetas antiguas 0–2), e integrar todo con Unreal (`/unreal`).
