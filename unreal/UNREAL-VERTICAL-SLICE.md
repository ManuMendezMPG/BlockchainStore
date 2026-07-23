# Vertical Slice — Unreal ↔ Blockchain (compra + inventario end-to-end)

> Estado: **VALIDADO**. Compra y lectura de inventario funcionando de punta a
> punta: botón en Unreal → bridge HTTP → MetaMask → contrato → inventario
> refrescado en pantalla.
>
> Este documento describe la **tubería mínima** que conecta el juego (UE 5.5, C++)
> con la blockchain a través del bridge HTTP. Es material de guía: explica el
> *qué*, el *cómo* y, sobre todo, el *por qué* de cada decisión. **No** describe la
> UI final ni la lógica de sesión; ver [Qué queda pendiente](#qué-queda-pendiente).

---

## 1. Visión de conjunto

```
┌──────────────────────────┐        HTTP        ┌──────────────────┐
│   Unreal Engine 5.5       │  ───────────────►  │   Bridge          │
│                           │  localhost:8787    │  (Node/Express)   │
│  WBP_StoreDebug (UI)      │  ◄───────────────  │                   │
│        ▲  │               │                    └────────┬──────────┘
│  events │  │ calls        │                             │
│        ┌┴──▼────────────┐ │                             ▼
│        │ BlockchainStore │ │                    ┌──────────────────┐
│        │ Client          │ │                    │  Navegador +     │
│        │ (Subsystem)     │ │                    │  MetaMask        │
│        └─────────────────┘ │                    │  (firma la tx)   │
└──────────────────────────┘                     └────────┬──────────┘
                                                           ▼
                                                  ┌──────────────────┐
                                                  │  Contrato (chain) │
                                                  └──────────────────┘
```

Punto clave de la arquitectura: **Unreal nunca firma nada ni toca claves
privadas**. La firma ocurre en el navegador con MetaMask, ajena al motor. Unreal
solo dispara *intenciones* de compra y *pregunta* por el estado. Esto mantiene el
juego sin secretos criptográficos y simplifica enormemente el lado C++.

---

## 2. Arquitectura del lado Unreal

### 2.1 Ficheros

Todos en `Source/BlockchainStore/`:

| Fichero | Rol |
|---|---|
| `BlockchainStore.Build.cs` | Habilita los módulos `HTTP`, `Json`, `JsonUtilities`. |
| `BlockchainStoreTypes.h` | `USTRUCT` de datos: `FStoreCatalogItem`, `FStoreInventoryEntry`. |
| `BlockchainStoreClient.h` | El subsystem, los delegates y la API expuesta a Blueprint. |
| `BlockchainStoreClient.cpp` | Implementación: HTTP + parseo JSON + polling. |

### 2.2 `UBlockchainStoreClient` — por qué un `UGameInstanceSubsystem`

El gestor es un **`UGameInstanceSubsystem`**, no un `UObject` suelto ni un `Actor`.
Razones, en orden de importancia:

1. **Vive toda la sesión y sobrevive a cambios de nivel.** El estado importante
   —`WalletAddress`, `BaseUrl`, el `requestId` de la compra en curso y el
   `FTimerHandle` del polling— no se pierde al cargar otro mapa. Un `UObject`
   suelto se lo llevaría el recolector de basura (GC) si no lo anclas a un
   `UPROPERTY`; un `Actor` moriría al cambiar de nivel.
2. **El engine gestiona su ciclo de vida.** No hay `NewObject`, ni hay que
   guardarlo en ningún sitio para protegerlo del GC. Existe desde que arranca el
   `GameInstance` hasta que se cierra el juego.
3. **Acceso global trivial desde cualquier Blueprint:** un único nodo
   `Get GameInstance Subsystem → BlockchainStoreClient`. Sin punteros que cablear
   entre widgets.
4. **Timer estable:** el polling usa `GetGameInstance()->GetTimerManager()`, que
   sobrevive a los cambios de mapa (el `FTimerManager` del `World` se recrea en
   cada carga de nivel).

> Si en el futuro quisiéramos que la conexión muriese al salir del menú principal,
> un `UWorldSubsystem` sería la alternativa. Para una tubería **global** de tienda,
> el `GameInstance` es lo correcto.

### 2.3 Delegates dinámicos multicast — por qué, y para qué

La comunicación del subsystem hacia la UI se hace **solo** mediante eventos:

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCatalogUpdated,  const TArray<FStoreCatalogItem>&,  Items);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInventoryUpdated, const TArray<FStoreInventoryEntry>&, Items);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPurchaseStateChanged, const FString&, RequestId, const FString&, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPurchaseCompleted, const FString&, TxHash);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPurchaseFailed,    const FString&, Reason);
```

- **"Dynamic"** = se pueden enganchar **desde Blueprint** (nodos rojos *Bind
  Event* / *Assign*). Un delegate normal de C++ no aparece en Blueprint.
- **"Multicast"** = varios widgets/objetos pueden suscribirse al mismo evento.
- Declarados `UPROPERTY(BlueprintAssignable)` para que el editor los exponga.

El beneficio de diseño: **el código de red no conoce a la UI**. Solo hace
`Broadcast`. Quien quiera, reacciona. Cuando lleguen SIWE, la mochila o una UI
bonita, este C++ **no cambia** — solo se suscriben nuevos widgets.

Los cinco eventos:

| Evento | Cuándo se dispara | Carga |
|---|---|---|
| `OnCatalogUpdated` | al completarse `FetchCatalog` | lista de items del catálogo |
| `OnInventoryUpdated` | al completarse `FetchInventory` (y tras una compra) | balances del address |
| `OnPurchaseStateChanged` | en cada cambio de estado de la compra | `requestId`, `status` |
| `OnPurchaseCompleted` | cuando `status == "done"` | `txHash` |
| `OnPurchaseFailed` | error del bridge, error de red o timeout | motivo legible |

### 2.4 El polling con `FTimerHandle`

La compra es asíncrona: tras enviar la intención hay que **preguntar
repetidamente** por el estado hasta que termine. Eso se hace con un timer en bucle.

```
BuyItem() ──POST /api/purchase-intent──► guarda requestId
                                          │
                                          ▼
              SetTimer(PollTimerHandle, …, 1.5 s, bLoop=true)
                                          │  cada 1.5 s
                                          ▼
              PollPurchaseStatus() ──GET /api/purchase-status?requestId=…──►
                                          │
                          ┌───────────────┴───────────────┐
                  status = done / error            pending / signing
                          │                                │
              ClearTimer + evento final          (no hace nada; el timer sigue)
```

Detalles importantes:

- `SetTimer(..., bLoop=true)` programa una llamada **recurrente** a
  `PollPurchaseStatus` cada `PollIntervalSeconds` (1.5 s).
- Cada tick lanza **un** request HTTP asíncrono. **Nunca se bloquea el hilo de
  juego**: el timer solo "despierta" para disparar la siguiente petición.
- El callback decide:
  - `done`  → `OnPurchaseCompleted(txHash)` + `ClearTimer` + `FetchInventory()`.
  - `error` → `OnPurchaseFailed(motivo)` + `ClearTimer`.
  - `pending` / `signing` → no hace nada; el timer sigue en su siguiente vuelta.
- **Timeout:** `MaxPollAttempts = 40` × 1.5 s ≈ **60 s**. Si se agotan →
  `OnPurchaseFailed("Timeout…")`.
- Un fallo **puntual** de red durante un poll **no** aborta: se deja que el
  siguiente tick reintente (salvo que ese reintento agote el timeout).
- `Deinitialize()` y `StopPolling()` limpian el timer: no quedan timers huérfanos.

### 2.5 Estructuras de datos (`USTRUCT`)

```cpp
USTRUCT(BlueprintType) struct FStoreCatalogItem  { int32 ItemId; FString Name; FString PriceWei; FString PriceEth; };
USTRUCT(BlueprintType) struct FStoreInventoryEntry { int32 ItemId; FString Name; int32 Balance; };
```

Decisión deliberada: **los precios `wei` se guardan como `FString`**. 1 ETH =
10^18 wei; eso desborda cualquier float y la UI solo necesita *mostrar* el texto,
no operar con él. `priceEth` ya viene formateado por el bridge (`"0.01"`), listo
para pantalla. Nada de `float` para dinero.

---

## 3. Mapeo función C++ ↔ llamada HTTP

| Función Blueprint | Método + endpoint | Respuesta real | Evento resultante |
|---|---|---|---|
| `FetchCatalog()` | `GET /api/catalog` | `{"items":[{"id":0,"name":"Espada","priceWei":"10000000000000000","priceEth":"0.01"}, …]}` | `OnCatalogUpdated(items)` |
| `FetchInventory()` | `GET /api/inventory?address=<addr>` | `{"address":"0x…","items":[{"id":0,"name":"Espada","quantity":"14"}, …]}` | `OnInventoryUpdated(items)` |
| `BuyItem(itemId, qty)` | `POST /api/purchase-intent` body `{address,itemId,quantity}` | `{requestId, status}` | arranca el polling |
| *(polling interno)* | `GET /api/purchase-status?requestId=…` | `{"status":"done","txHash":"0x…","error":null,"itemId":0,"quantity":1}` | `OnPurchaseStateChanged` → `OnPurchaseCompleted` / `OnPurchaseFailed` |

### Notas de parseo (nombres exactos del bridge)

- Catálogo e inventario envuelven siempre los datos en `items[]`; se lee con
  `TryGetArrayField("items", …)`.
- `quantity` llega como **string** (`"14"`) → se convierte a `int32` con
  `FCString::Atoi`. `priceWei`/`priceEth` se copian como texto sin tocar.
- En `purchase-status`, `error` es un **campo de datos normal** (`null` en éxito,
  mensaje cuando `status=="error"`), siempre con HTTP 200. Por eso ese handler
  **no** pasa por el chequeo genérico de error de `TryParseJsonObject` (ver
  [Bugs](#5-bugs-encontrados-y-resueltos)).

---

## 4. Flujo completo validado

```
Usuario pulsa "Comprar item N" en WBP_StoreDebug
        │
        ▼
BlockchainStoreClient::BuyItem(N, 1)
        │  POST /api/purchase-intent {address, itemId:N, quantity:1}
        ▼
Bridge responde {requestId, status:"pending"}
        │  → OnPurchaseStateChanged(requestId, "pending")
        │  → arranca FTimerHandle (cada 1.5 s)
        ▼
El bridge abre/usa el navegador → MetaMask pide firma al usuario
        │
        ├─ mientras tanto, polling: GET /api/purchase-status
        │     status:"pending"  → UI muestra "pending"
        │     status:"signing"  → UI muestra "signing"   (usuario firmando en MetaMask)
        ▼
Usuario firma en MetaMask → tx enviada al contrato → minada
        │
        ▼
polling: status:"done", txHash:"0x…"
        │  → OnPurchaseCompleted(txHash)   → UI muestra "OK tx: 0x…"
        │  → ClearTimer (para el polling)
        │  → FetchInventory()  (refresco automático)
        ▼
GET /api/inventory → OnInventoryUpdated → la lista en pantalla se actualiza
con el nuevo balance
```

### Estados `pending → signing → done`

- **`pending`**: el bridge registró la intención; aún no hay interacción del
  usuario en MetaMask.
- **`signing`**: el usuario tiene la ventana de MetaMask abierta / la firma está en
  curso. Es el estado donde *normalmente* se pasa más tiempo (depende del humano).
- **`done`**: la transacción se firmó, se envió y se confirmó en cadena; ya hay
  `txHash`. Aquí se refresca el inventario.
- **`error`**: el usuario rechazó la firma, la tx revirtió, o el bridge falló.
  Dispara `OnPurchaseFailed` con el motivo.

Caso límite cubierto: si el usuario nunca firma, el polling se rinde a los ~60 s
con `OnPurchaseFailed("Timeout…")` en vez de quedarse colgado para siempre.

---

## 5. Bugs encontrados y resueltos durante el montaje

Documentados a propósito: son justo el tipo de error que cuesta media hora si no
sabes dónde mirar.

### 5.1 (C++) `purchase-status` con `status:"error"` se trataba como "reintentar"

- **Síntoma:** una compra que el bridge marcaba como `error` no disparaba
  `OnPurchaseFailed`; el polling seguía reintentando hasta el timeout.
- **Causa:** el handler de status reutilizaba `TryParseJsonObject`, que considera
  *cualquier* campo `error` no vacío como fallo de parseo y devuelve "reintentable".
  Pero en este endpoint `error` es un dato normal y el estado `error` viene con
  **HTTP 200**.
- **Fix:** el handler de `purchase-status` ya **no** usa esa puerta genérica.
  Parsea el JSON directamente y solo cuenta como "fallo de red reintentable" el no
  tener respuesta o un body que no sea JSON. El `status:"error"` se trata como
  fallo **terminal** (dispara `OnPurchaseFailed` y para el timer).

### 5.2 (Blueprint) El cable de `Btn_Connect` leía `Txt_Status` en vez de `Txt_Address`

- **Síntoma:** al pulsar *Conectar*, la wallet se establecía con basura (el texto
  del label de estado) y el inventario volvía vacío o con error de address.
- **Causa:** en `Btn_Connect → OnClicked`, el nodo `SetWalletAddress` estaba
  cableado a `Txt_Status` (el `Text` de estado) en lugar de a `Txt_Address` (el
  `Editable Text Box` donde se pega el address). Dos widgets de texto con nombres
  parecidos, cable cruzado.
- **Fix:** recablear `Get Text` desde **`Txt_Address`** → `To String` →
  `SetWalletAddress`. Lección: nombrar los widgets de forma inconfundible
  (`Txt_AddressInput` vs `Txt_StatusLabel`) evita exactamente esto.

### 5.3 (Blueprint/Designer) Los botones de compra quedaban fuera de pantalla

- **Síntoma:** tras leer el inventario, los botones *Comprar* desaparecían: el
  contenido los empujaba fuera del área visible.
- **Causa:** la lista de inventario se rellenaba dentro del mismo contenedor que
  crecía sin límite, empujando los botones más abajo del borde de la pantalla; sin
  scroll efectivo sobre la parte correcta, quedaban inaccesibles.
- **Fix:** meter **solo la lista de inventario** en un `Scroll Box` de tamaño
  acotado, y dejar los botones *Comprar* **fuera** de ese scroll (en el
  `Vertical Box` raíz, debajo). Así la lista hace scroll dentro de su caja y los
  botones permanecen siempre visibles.

---

## 6. El widget de debug `WBP_StoreDebug`

> ⚠️ **UI desechable.** `WBP_StoreDebug` existe **solo** para validar la tubería.
> Botones feos pero funcionales. **No** es la interfaz final y no debe evolucionar
> hacia ella: cuando llegue la UI real, este widget se tira.

Composición mínima (todo dentro de un `Vertical Box` raíz):

- `Editable Text Box` **`Txt_Address`** — pegar el address `0x…`.
- `Button` **`Btn_Connect`** ("Conectar / Leer inventario") → `SetWalletAddress(Txt_Address)` + `FetchInventory()`.
- `Text` **`Txt_Status`** — muestra el estado de compra / errores.
- `Scroll Box` **`List_Inventory`** — filas `Name + " x" + Balance` (se limpia con `Clear Children` antes de repoblar).
- `Button` **`Btn_Buy1`** / **`Btn_Buy2`** — `BuyItem(1,1)` / `BuyItem(2,1)`. **Fuera** del scroll.

Suscripción a eventos en `Event Construct`: se obtiene el subsystem una vez, se
guarda en la variable `Client`, y se hace *Bind* de los cinco eventos a *Custom
Events* que actualizan los widgets. La regla de oro que valida el slice:
**la UI no hace polling ni conoce HTTP**; solo reacciona a los `Broadcast` del
subsystem.

---

## 7. Qué queda pendiente

Lo que **NO** cubre este vertical slice y queda para la fase de UI/juego real:

- **Diseño real de UI.** `WBP_StoreDebug` es desechable. Hace falta la tienda y la
  mochila de verdad, con arte, layout y feedback decente.
- **Login SIWE (Sign-In With Ethereum).** Ahora el address se pega a mano con un
  setter manual (`SetWalletAddress`). Falta el flujo real de autenticación con la
  wallet y la gestión de sesión asociada.
- **Slots de mochila.** Representar el inventario como rejilla de slots en vez de
  una lista de texto plano.
- **Medallones en pantalla.** Mostrar los items poseídos en el HUD del juego.
- **Usar pociones / romper botellas (lógica de sesión).** Consumir items desde el
  juego (gastar/quemar) y reflejar el efecto en la sesión y en el balance on-chain.

Todo eso se construye **encima** de esta tubería sin tocar `BlockchainStoreClient`:
nuevos widgets se suscriben a los mismos delegates y llaman a las mismas funciones.
Ese es, precisamente, el valor de haber validado el slice primero.
