# 04 — Depuración y aprendizajes

> Parte de la guía de aprendizaje del proyecto. Continúa
> [01](./01-smart-contract.md) · [02](./02-bridge.md) · [03](./03-logros-y-dependencias.md).
> Este documento captura los **fallos reales** de esta fase y cómo se diagnosticaron.
> Entender los modos de fallo de un sistema blockchain local —no solo el happy
> path— es justo lo que distingue una demo sólida de una frágil.

---

## 1. El "contrato fantasma" del estado persistente

### Síntoma
Tras ampliar `GameStore` (logros, dependencias, contadores) y sincronizar el bridge,
**el código del bridge era correcto** pero las lecturas de las funciones nuevas
fallaban: al comprar, `loadProgress` reventaba en `purchasedTotal(...)` con
`execution reverted (no data present)`. Lo desconcertante: `balanceOf` sí
funcionaba, y la compra parecía ir bien.

### Causa raíz
El nodo local se arrancaba con `scripts/start-local.sh`, que **persiste el estado**
de Anvil en `.anvil/state.json` para no perder el inventario entre reinicios. Ese
fichero se había creado en una sesión anterior, cuando `GameStore` era una versión
**antigua** (catálogo de 3 items, sin `purchasedTotal`).

La lógica del script era: *"si hay bytecode en la dirección determinista, no
redespliegues"*. Al cargar el estado viejo, **sí** había bytecode (el del contrato
antiguo), así que el script lo daba por bueno y **no redesplegaba**. Estábamos
probando el bridge nuevo contra el **contrato viejo**, sin saberlo. Las funciones que
añadimos no existían en ese bytecode → revert.

> El contrato "fantasma": el que crees que está desplegado (tu última versión del
> código) no es el que la cadena tiene realmente cargado del disco.

### El trade-off de fondo
Esto no fue un descuido aislado, fue el **coste de una decisión de diseño**. La
persistencia la añadimos a propósito (doc del script): que el inventario sobreviva a
apagar y reencender es muy cómodo para iterar. Pero esa misma persistencia introduce
un riesgo nuevo: **congelar una versión vieja del contrato** en el estado guardado.
Comodidad y frescura tiran en direcciones opuestas. No hay "gratis": cada feature de
infraestructura trae su propia clase de bug.

### La solución: detección de versión (lógica de tres ramas)
Endurecimos `start-local.sh` para que no se conforme con *"¿hay bytecode?"*, sino que
compruebe *"¿es la versión correcta?"*. Tras cargar el estado, **sondea un getter que
solo existe en la versión actual** (`purchasedTotal`). El árbol de decisión:

```
cast code <addr>  ──►  ¿hay bytecode?
      │ no                       │ sí
      ▼                          ▼
 [DESPLEGAR]      probe: cast call purchasedTotal(...)
                       │ responde          │ revierte (data 0x)
                       ▼                    ▼
                  [REUTILIZAR]         [REDESPLEGAR limpio + aviso]
```

| Detección | Acción |
|-----------|--------|
| Sin bytecode | Desplegar (cadena limpia) |
| Bytecode **y** el getter responde | Reutilizar (conserva inventario) |
| Bytecode **pero** el getter revierte | Avisar, reiniciar cadena limpia y redesplegar |

Detalle importante del redeploy: **no se redespliega sobre la cadena vieja**. La
dirección determinista `0x5FbD…0aa3` solo se obtiene con la cuenta #0 a **nonce 0**;
si redesplegáramos sobre el estado viejo (nonce ya avanzado), el contrato caería en
otra dirección y rompería al bridge. Por eso el script **aparta** el estado viejo a
`.anvil/state.json.stale`, arranca una cadena limpia y entonces despliega. Y avisa
fuerte, nada silencioso:

```
⚠️  Detectado un contrato de VERSIÓN ANTERIOR en el estado persistente.
    El inventario/compras de ese estado NO son válidos para el contrato nuevo.
    Reiniciando con una CADENA LIMPIA y redesplegando la versión actual ...
```

---

## 2. `data="0x"` vs selector crudo: dos fallos del MISMO tipo de error

Ambos son "el contrato revirtió", pero significan cosas opuestas y se arreglan
distinto. Saber distinguirlos es media depuración.

### Caso A — `data="0x"` (no data present): la función NO existe
Cuando llamas a una función cuyo selector **no está** en el bytecode desplegado, no
hay nada que ejecutar: la EVM cae al `fallback`/`receive` del contrato, y como ese
ERC-1155 no devuelve datos de error, la llamada **revierte sin payload** → `data:
"0x"`. ethers, al no encontrar datos de revert que decodificar, lo reporta como
*"execution reverted (no data present)"* o *"require(false)"*.

Esto es lo que pasaba con el contrato fantasma: `purchasedTotal` no existía en el
bytecode viejo.

**Cómo diagnosticarlo (preguntando a la cadena):**
```bash
# ¿Hay realmente un contrato en esa dirección?
cast code 0x5FbD…0aa3 --rpc-url http://127.0.0.1:8545      # 0x = nada; 0x6080… = sí hay

# ¿Responde la función que crees que existe?
cast call 0x5FbD…0aa3 "purchasedTotal(address,uint256)(uint256)" 0x0…0 0 --rpc-url ...
#   → "0"                         : existe (contrato correcto)
#   → "execution reverted 0x"     : NO existe (versión antigua / dirección equivocada)
```

### Caso B — selector crudo (`0x1b9e2491`): la función SÍ existe y revierte a propósito
Aquí la función existe y revierte con un **custom error** legítimo (una regla de
negocio, p. ej. `NeedBow()`). Lo que viaja es el **selector de 4 bytes** del error
(`0x1b9e2491` = `keccak256("NeedBow()")[:4]`), opcionalmente seguido de sus args
codificados.

El detalle fino: este error aparecía **sin decodificar** solo cuando la operación
pasaba por **MetaMask** durante la estimación de gas. Llegaba en el formato de error
JSON-RPC de EIP-1193:
```json
{ "code": 3, "message": "execution reverted", "data": "0x1b9e2491" }
```
…anidado en `err.info.error.data`, como hex crudo. El switch de mensajes, que miraba
`err.revert.name`, no encontraba nombre y caía al mensaje genérico.

**La solución:** decodificar el selector nosotros mismos con el ABI ya cargado:
```js
const iface = new ethers.Interface(gsAbi);
const parsed = iface.parseError("0x1b9e2491"); // → { name: "NeedBow", args: [] }
```
`parseError` empareja los primeros 4 bytes del `data` con los selectores de los
custom errors del ABI y devuelve nombre + args. A partir de ahí, el mismo switch de
siempre produce *"🏹 Necesitas un ARCO…"*.

### Por qué ethers decodifica en un caso y en el otro no
Depende de **quién hace la llamada que revierte**:

- **ethers controla la llamada** (un `eth_call` suyo, o una tx minada): tiene a la vez
  el `data` del revert **y** el ABI del `Contract` que creaste. Decodifica solo y
  rellena `err.revert.name` / `err.revert.args`. → te llega bonito.
- **MetaMask hace la estimación de gas** (escritura vía `BrowserProvider` →
  `window.ethereum`): el fallo lo genera el proveedor inyectado y llega "ya hecho" en
  formato EIP-1193, con el `data` crudo en `err.info.error.data`. ethers lo envuelve
  pero **no lo asocia automáticamente** a tu ABI (no fue su camino de decodificación).
  → te llega el selector pelado.

Moraleja: en una dApp, **no asumas que el error de revert siempre viene decodificado**.
Lleva tú el ABI y ten un `parseError` de respaldo para el `data` crudo. (En el bridge
unificamos ambos caminos en `humanizeError`.)

---

## 3. La técnica transversal: pregúntale la verdad a la cadena

Los dos problemas anteriores tienen una lección común. Ante un síntoma en la UI, la
tentación es tocar el código de la UI. **Primero pregunta a la cadena**, que es la
única fuente de verdad, y así separas el problema en capas:

> ¿El estado on-chain es correcto? Entonces el bug está en la capa de
> lectura/decodificación/UI, **no** en el contrato. Y viceversa.

Las herramientas: `cast code` (¿hay contrato y cuál?), `cast call` (¿qué devuelve esta
función de verdad?). Son de **solo lectura**, instantáneas y sin gas: el "multímetro"
del desarrollo on-chain.

`balanceOf` actuó dos veces como **testigo independiente** que exoneró al contrato:

- **Contrato fantasma:** `balanceOf(cuenta, id)` devolvía el valor correcto tras
  comprar → la *propiedad* estaba bien grabada on-chain. Luego el contrato base
  funcionaba; el fallo de `purchasedTotal` no era "la compra no se guarda", sino "esa
  función no existe en este bytecode". Eso reorientó la búsqueda del contrato al
  **despliegue/estado**, no a la lógica.
- **Panel de progreso:** la compra se confirmaba con `cast call balanceOf` (correcto),
  mientras `loadProgress` reventaba. Misma conclusión: el contrato hacía su trabajo;
  el problema vivía en la **capa de lectura** del bridge (función inexistente en el
  contrato desplegado + falta de robustez).

Como corolario práctico hicimos `loadProgress` **robusto**: cada lectura va aislada y,
si una falla, muestra `n/d` en ese campo sin tumbar el inventario ni la tienda. Un
fallo de una capa no debe propagarse y enmascarar qué funciona.

**El reflejo a interiorizar:** síntoma en UI → `cast code` + `cast call` →
¿on-chain bien? → decide en qué capa está el bug → recién entonces tocas código.

---

## 4. Decisiones y recordatorios registrados

### El arco NO es requisito para el carcaj
Quede claro para evitar confusiones futuras: en `GameStore`, las dependencias de los
carcaj y del arco son **independientes**. El arco **solo** se exige para comprar
**flechas**. Las reglas reales son:

| Compra | Requisito | NO requiere |
|--------|-----------|-------------|
| `carcaj_5` | (ninguno) | — |
| `carcaj_10` | poseer `carcaj_5` | arco |
| `carcaj_20` | logro **ARQUERO** | arco |
| `flecha` | poseer **arco** + capacidad de carcaj | — |
| `pocion_vida` / `pocion_mana` | poseer (y consume) `botella_vacia` | — |

Puedes comprar y mejorar carcaj sin tener arco; tiene sentido (el carcaj es contenedor;
el arco es el arma). El acoplamiento arco↔flecha es el único entre esos items.

### El flujo correcto al cambiar contratos
La mayoría de los líos de esta fase venían de **probar contra un contrato que no era
el que creías**. El flujo seguro tras tocar `src/*.sol`:

1. `forge test` — que pase en verde.
2. **Regenerar los ABIs del bridge** (`forge inspect … abi --json` → `bridge/public/abi/`).
   El bridge usa el ABI, no el `.sol`; si no lo regeneras, decodifica con un ABI viejo.
3. **Redesplegar la versión nueva.** Con el script endurecido (§1) basta reiniciar
   `start-local.sh`: detecta el desajuste de versión y redespliega solo. (Si dudas:
   `rm -rf .anvil` fuerza cadena limpia.)
4. **MetaMask → Borrar datos de actividad** si da *"nonce too high"* (su caché de nonce
   no sabe que la cadena se reinició).
5. **Recargar el navegador** para coger el `app.js`/ABIs nuevos.

Saltarse el paso 2 o el 3 es exactamente lo que produjo el contrato fantasma y el ABI
desfasado. Tenerlo como checklist evita repetir ambos.

---

## Resumen para la demo

Lo que esta fase demuestra no es que "funciona", sino que **entendemos cómo falla**:

- El estado persistente puede servir una **versión vieja** del contrato → detectamos
  versión sondeando un getter, no solo mirando si hay bytecode.
- "El contrato revirtió" tiene **dos sabores opuestos**: la función no existe
  (`data 0x`) vs revierte a propósito con un custom error (selector crudo) — y se
  diagnostican y arreglan distinto.
- La cadena es la **fuente de verdad**: `cast code`/`cast call` separan el bug de
  contrato del bug de UI antes de tocar una línea.
- La infraestructura cómoda (persistencia) **trae su propio riesgo**; el oficio está en
  anticiparlo y blindarlo, no en evitar la feature.
