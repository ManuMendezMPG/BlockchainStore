# 05 — La capa de comunicación: API del bridge y cliente Unreal

> Parte de la guía de aprendizaje del proyecto.
> [01](./01-smart-contract.md) · [02](./02-bridge.md) · [03](./03-logros-y-dependencias.md) · [04](./04-depuracion-y-aprendizajes.md).
> Aquí documentamos **cómo habla el juego con la blockchain**: el patrón de tres
> actores, la API HTTP del bridge, y el plan para conectar Unreal. Con los porqués.

---

## 1. El problema y el patrón de los tres actores

El juego necesita que el jugador **compre items on-chain**, pero choca con dos límites:

- **Unreal no puede hablar con MetaMask.** MetaMask es una extensión de **navegador**:
  solo expone su API (`window.ethereum`) a JavaScript de una página web. Un ejecutable
  de Unreal no tiene navegador con la extensión ni forma nativa de pedir una firma.
- **El juego no debe custodiar la clave privada.** Si Unreal firmara, tendría que
  guardar la clave — justo lo que queremos evitar. La clave debe quedarse en MetaMask.

La solución es un **patrón de tres actores**, cada uno con un papel y un límite claro:

```
┌──────────┐   HTTP/JSON    ┌──────────────┐   window.ethereum   ┌──────────────┐
│  UNREAL  │ ─────────────► │    BRIDGE    │ ◄─ sondea ──────────│ WEB + MetaMask│
│ (cliente)│ ◄───────────── │ (coordina)   │ ── intención ──────►│   (firma)     │
└──────────┘   polling      └──────┬───────┘                     └──────┬────────┘
                                   │ lectura (eth_call)                 │ firma tx
                                   ▼                                    ▼
                              ┌─────────────────  blockchain (Anvil)  ─────────────┐
                              │  GameStore (ERC-1155)  +  Achievements             │
                              └────────────────────────────────────────────────────┘
```

| Actor | Qué hace | Qué **no** hace |
|-------|----------|-----------------|
| **Unreal** | Pide acciones (comprar) y consulta estado, por HTTP | No firma, no toca claves, no habla con la cadena |
| **Bridge** | Coordina: lee la cadena y guarda "intenciones" de compra | **No custodia claves**, no firma |
| **Web + MetaMask** | Detecta intenciones y **firma** las transacciones | No es la lógica de juego |

La idea de fondo (ver doc 02): el bridge es el **intermediario** que tiene lo que
Unreal no tiene (acceso a MetaMask vía navegador) sin asumir lo que no debe (la clave).

---

## 2. El diseño de la API HTTP del bridge

El servidor (`bridge/server.js`, sin dependencias) sirve la web **y** expone una API
JSON bajo `/api/*`, separada del estático. Dos familias de endpoints.

### 2.1 Lecturas (no requieren firma → las sirve el bridge directamente)

Una lectura solo necesita un *provider* de solo-lectura (no hay que firmar nada), así
que el bridge la resuelve por su cuenta con `eth_call` y responde JSON.

| Endpoint | Devuelve |
|----------|----------|
| `GET /api/catalog` | Los 10 items: `id`, `name`, `priceWei`, `priceEth`. |
| `GET /api/inventory?address=0x…` | Balance (`balanceOf`) de cada item para esa cuenta. |
| `GET /api/progress?address=0x…` | `arrowsPurchased`, `totalSpent`, `quiverCapacity` y los medallones (con la rareza del Mercader). |

### 2.2 Compra (sí requiere firma → patrón pending → signing → done)

Como la firma vive en otro proceso (la web), la compra **no puede ser síncrona**. Se
modela como una **intención** con estados, y un buzón que ambos lados consultan.

| Endpoint | Quién lo llama | Para qué |
|----------|----------------|----------|
| `POST /api/purchase-intent` `{address,itemId,quantity}` | **Unreal** | Registra la intención; responde `{requestId}` (estado `pending`). |
| `GET /api/purchase-status?requestId=…` | **Unreal** | Polling del estado: `pending` → `signing` → `done` (con `txHash`) / `error`. |
| `GET /api/pending` | **La web** | Lista las intenciones aún `pending`. |
| `POST /api/purchase-result` `{requestId,status,txHash?,error?}` | **La web** | Avanza el estado: `signing` al reclamarla, luego `done`/`error`. |

### 2.3 Qué significa cada estado (y por qué `signing` importa)

```
pending ──(la web reclama)──► signing ──(tx minada)──► done   (con txHash)
                                   └────(falla/revert)──► error (con mensaje)
```

- **`pending`** — Unreal pidió la compra; nadie la ha tomado aún. Aparece en
  `GET /api/pending` para que la web la descubra.
- **`signing`** — la web la ha **reclamado** y está disparando MetaMask. Esto cumple
  **doble función**:
  1. Informa a Unreal de que su petición avanza (ya hay alguien firmándola).
  2. **Evita la doble firma:** al marcarla `signing`, deja de salir en
     `GET /api/pending`. Si hay dos pestañas (o el consumidor sondea dos veces), la
     intención ya no se ofrece otra vez → no se firma dos veces la misma compra.
- **`done`** — la transacción se minó; el estado lleva el `txHash`. Unreal lo ve en su
  polling y refresca su inventario.
- **`error`** — la firma se rechazó o el contrato revirtió (p. ej. una regla de
  dependencia); el estado lleva un mensaje legible.

El flujo completo, de principio a fin entre los tres actores:

```
UNREAL                         BRIDGE                          WEB + MetaMask
  POST /purchase-intent ───────►  guarda {pending}
  ◄── { requestId }
                                                  GET /pending ──► ve la intención
                                signing ◄───────── POST /purchase-result {signing}  (reclama)
                                                  dispara MetaMask → el jugador FIRMA buy()
  GET /purchase-status ─► signing                 espera a que la tx se mine
                                done + txHash ◄─── POST /purchase-result {done, txHash}
  GET /purchase-status ─► done, txHash ✓
```

En el prototipo, la propia web (`public/app.js`) hace de consumidor automático: al
conectar MetaMask, sondea `/api/pending` cada 3 s y procesa las intenciones **de la
cuenta conectada** (reclama → firma → reporta). En producción ese consumidor podría
ser una página dedicada de "firma".

---

## 3. Polling vs WebSockets: por qué polling (por ahora)

La comunicación es **asíncrona** (Unreal pide, alguien firma después), así que hay que
notificar el resultado. Dos enfoques:

- **Polling** (elegido): el cliente pregunta cada X segundos "¿ya está?". Es lo que
  hacen Unreal (`/purchase-status`) y la web (`/pending`).
- **WebSockets / SSE**: conexión persistente que **empuja** el cambio en cuanto ocurre.

Elegimos **polling** para el prototipo, conscientemente:

- **Más simple:** son peticiones HTTP normales. Sin gestión de conexiones
  persistentes, reconexiones, ni estado de socket. El servidor sigue **sin
  dependencias** y los clientes (incluido Unreal) solo necesitan un cliente HTTP.
- **Suficiente para la demo:** una compra tarda segundos (el humano firma en
  MetaMask); sondear cada 1-3 s da una latencia percibida perfectamente aceptable.
- **Trivial de implementar en cualquier lado:** un `GET` en bucle se escribe igual en
  curl, en JS o en Unreal (un Timer). No exige librerías de WebSocket en el motor.
- **Evolucionable:** si más adelante se quiere tiempo real (muchos eventos, baja
  latencia), se puede añadir WebSockets/SSE **sin cambiar el modelo** de estados
  `pending/signing/done`; solo cambia el *transporte* de la notificación.

> Regla de prototipo: empieza por lo más simple que funcione y que no te encierre.
> Polling cumple las tres cosas aquí.

**Coste a tener presente:** el polling genera peticiones "en vacío" mientras no hay
cambios. A esta escala (un jugador, intervalos de segundos) es irrelevante; a gran
escala sería el momento de pasar a push.

---

## 4. El test-client como validación

`bridge/test-client/test-client.js` (ver su README) es una herramienta de **prueba**,
no parte del producto. Ocupa **el rol exacto de Unreal**: hace `POST /purchase-intent`
y luego polling de `/purchase-status`, mostrando el inventario antes y después.

**Qué valida:**
- Que los endpoints de lectura responden y devuelven datos correctos.
- Que el ciclo de compra fluye: `pending → signing → done`, con `txHash` de vuelta.
- Que el balance on-chain cambia tras la compra (lee inventario antes/después, `Δ`).
- **Códigos de salida** claros para encadenar/automatizar:

  | Código | Significado |
  |--------|-------------|
  | `0` | compra `done` |
  | `1` | el contrato revirtió (`error`) / HTTP inesperado |
  | `2` | argumentos inválidos |
  | `3` | timeout (nadie firmó) |
  | `4` | bridge no accesible |

**Qué NO cubre (y cómo se validó aparte):** el test-client no firma — no puede, igual
que Unreal. La **firma real con MetaMask** se validó **manualmente** abriendo la web y
confirmando en la extensión. Para la prueba automática del happy path, ese paso de
firma se sustituyó por una transacción `buy()` enviada directamente a la cadena (lo que
MetaMask haría), confirmando que, una vez firmada, todo el circuito cierra y el balance
sube.

> Reparto de validación: **cliente + API** → test-client (automatizable);
> **firma** → MetaMask en el navegador (manual). Juntos cubren el flujo completo.

---

## 5. Plan de migración a Unreal

La gran conclusión de diseño: **el lado del juego son solo dos llamadas HTTP.** Todo lo
difícil (firma, reglas del contrato, coordinación) vive fuera de Unreal. Migrar es
traducir el test-client a Unreal:

1. **`POST /api/purchase-intent`** con cuerpo JSON `{address, itemId, quantity}` →
   guardar el `requestId`.
2. **`GET /api/purchase-status?requestId=…`** en bucle hasta `done`/`error`.

En **Unreal C++** eso es `FHttpModule`:

```cpp
// 1) Registrar la intención
FHttpModule::Get().CreateRequest();
Req->SetURL(TEXT("http://localhost:8787/api/purchase-intent"));
Req->SetVerb(TEXT("POST"));
Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
Req->SetContentAsString(TEXT("{\"address\":\"0x…\",\"itemId\":6,\"quantity\":5}"));
Req->OnProcessRequestComplete().BindUObject(this, &UStore::OnIntentReply); // saca requestId
Req->ProcessRequest();

// 2) Polling con un Timer (p. ej. cada 1.5 s) hasta done/error
GetWorld()->GetTimerManager().SetTimer(PollTimer, this, &UStore::PollStatus, 1.5f, true);
//   PollStatus hace GET /api/purchase-status?requestId=… y, al ver "done",
//   limpia el Timer y refresca el inventario (otro GET /api/inventory).
```

(En **Blueprints** es lo mismo con los nodos *HTTP Request* del plugin de HTTP y un
*Set Timer by Function Name* para el bucle.)

Lo que **no** queda por resolver en Unreal:
- **Nada de blockchain:** Unreal no construye transacciones, no calcula gas, no maneja
  ABIs ni direcciones. Eso es del bridge/contrato.
- **Nada de firma:** la clave nunca toca el juego.
- **Solo transporte HTTP + parseo de JSON**, que es terreno estándar del motor.

Por eso el test-client es la prueba de que **el camino está despejado**: valida los
mismos endpoints y el mismo ciclo que Unreal consumirá; lo único nuevo en el motor será
el cliente HTTP, no lógica de dominio.

---

## 6. Nota de entorno (para retomar el proyecto)

- **El server corre en Windows.** Node está en Windows; el server escucha en
  `http://localhost:8787`. **Desde WSL no se alcanza ese `localhost`** (el reenvío de
  `localhost` va Windows→WSL, no al revés). Arranca y prueba la API/cliente **desde
  Windows** (PowerShell). Anvil, en cambio, corre en WSL y el server de Windows sí lo
  alcanza por `localhost:8545`.
- **CORS abierto** (`Access-Control-Allow-Origin: *`) con preflight `OPTIONS`, para que
  un cliente local como Unreal pueda llamar a la API sin bloqueos del navegador/origen.
- **Las intenciones viven en memoria** (un `Map` en el server). Es un prototipo:
  **se pierden al reiniciar** `server.js`. Para producción se persistirían (fichero/DB)
  y se les daría caducidad (TTL) y limpieza.
- **Nunca hay claves en el servidor.** Lecturas con provider; compras solo coordinadas;
  la firma siempre en MetaMask.

### Resumen para la demo
La capa de comunicación demuestra una **separación de responsabilidades** limpia:
Unreal pide (HTTP), el bridge coordina (sin claves), MetaMask firma (custodia la
clave), la cadena es la verdad. El patrón `pending → signing → done` hace la asincronía
explícita y a prueba de doble firma, y el test-client prueba que el contrato de la API
funciona — dejando la integración de Unreal reducida a "hacer peticiones HTTP".
