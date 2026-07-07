# 06 — Autenticación: login con SIWE (Sign-In with Ethereum, EIP-4361)

> Parte de la guía de aprendizaje del proyecto.
> [01](./01-smart-contract.md) · [02](./02-bridge.md) · [03](./03-logros-y-dependencias.md) · [04](./04-depuracion-y-aprendizajes.md) · [05](./05-api-y-cliente-unreal.md).
> Aquí cerramos la fase de **autenticación**: qué es SIWE, por qué "conectar" no es
> lo mismo que "autenticar", y cómo el login encaja en el patrón de tres actores del
> proyecto **sin gas** y **sin claves en el servidor**. Con los porqués.

---

## 1. El problema: "connect" no es autenticación

En la web (doc 05) el jugador pulsa **Conectar MetaMask** y aparece su dirección. Es
tentador tratar eso como "login", pero **no lo es**. Conectar solo hace una cosa:

- MetaMask te **enseña** la dirección que el usuario elige exponer (`eth_accounts`).
- El navegador la **lee**. Nadie ha demostrado nada.

La diferencia es de **propiedad**. Una dirección Ethereum es pública: aparece en cada
transacción, en cualquier explorador, en logs. Que un cliente **diga** "soy
`0xAbc…`" no prueba que controle esa cuenta —solo que sabe teclear (o copiar) 42
caracteres públicos. Es como identificarte dando un número de DNI que cualquiera puede
ver: lo dices, no lo demuestras.

| | Qué ocurre | Qué prueba |
|---|---|---|
| **Connect** (`eth_accounts`) | El navegador **lee** una dirección que MetaMask expone | Nada. Solo que alguien la conoce |
| **SIWE** (firma de un mensaje) | El dueño **firma** con su clave privada; se verifica | Que **controla la clave** de esa dirección |

**Autenticar** = probar que quien dice ser `0xAbc…` **posee la clave privada** de
`0xAbc…`. Y eso solo se demuestra de una forma: pidiéndole que **firme** algo que solo
el dueño de la clave podría firmar. Ahí entra SIWE.

> Regla mental para la consultoría: *leer una dirección ≠ probar la identidad.*
> Connect es una comodidad de UI; SIWE es el mecanismo de autenticación.

---

## 2. Qué es SIWE (EIP-4361)

**SIWE** ("Sign-In with Ethereum") es el estándar **EIP-4361**: un formato de **mensaje
de texto legible** que el usuario firma con su wallet para iniciar sesión. La wallet
como sustituto de "usuario + contraseña": en vez de una contraseña, la prueba es una
**firma criptográfica**.

El mensaje NO es texto libre: tiene campos definidos por el estándar. Este es el que
construye el bridge (`server.js`, `POST /api/siwe/login-intent`):

```
localhost:8787 wants you to sign in with your Ethereum account:
0x70997970C51812dc3A010C7d01b50e0d17dc79C8

Inicia sesion en GameStore (demo). Firmar no cuesta gas.

URI: http://localhost:8787
Version: 1
Chain ID: 31337
Nonce: aB3dE9fG…
Issued At: 2026-07-07T10:00:00.000Z
```

Cada campo tiene un porqué de seguridad, no es decoración:

| Campo | Para qué sirve |
|-------|----------------|
| `domain` (`localhost:8787`) | Ata la firma a **este sitio**. Una firma para otro dominio no vale aquí (anti-phishing). |
| `address` | La dirección que **afirma** ser el dueño. Es lo que la verificación tendrá que **confirmar**. |
| `statement` | Texto humano que el usuario lee en MetaMask antes de firmar (consentimiento informado). |
| `Chain ID` (`31337`) | La red (Anvil). Evita reutilizar una firma pensada para otra cadena. |
| `Nonce` | Número de un solo uso: la pieza **anti-repetición** (§5). |
| `Issued At` | Marca temporal; permite caducar sesiones antiguas. |

Lo esencial: el mensaje es **específico, legible y verificable**. El usuario ve
exactamente qué firma, y el servidor puede comprobar después que la firma corresponde a
ese mensaje exacto y a esa dirección.

---

## 3. El flujo completo entre los tres actores

El login reutiliza **el mismo patrón de tres actores** que la compra (doc 05): Unreal
pide, el bridge coordina sin claves, la web + MetaMask firman. Solo cambia **qué** se
firma (un mensaje, no una transacción).

```
UNREAL                         BRIDGE                          WEB + MetaMask
  POST /siwe/login-intent ─────►  valida address (EIP-55)
        {address}                 genera NONCE de un solo uso
                                  construye el MENSAJE EIP-4361
  ◄── { requestId, message }      guarda sesión {pending}
                                                  GET /siwe/pending ──► ve el login
                                signing ◄───────── (lo RECLAMA al leerlo → signing)
                                                  personal_sign del MENSAJE en MetaMask
                                                  (OFF-CHAIN, SIN GAS)
  GET /siwe/login-status ─► signing               ▼ el jugador FIRMA
                                verifica (ecrecover) ◄─── POST /siwe/verify {signature}
                                nonce OK + domain OK + chainId OK
                                quema el nonce → {done, address}
  GET /siwe/login-status ─► done, address ✓
```

Paso a paso, con quién hace qué:

1. **`POST /api/siwe/login-intent {address}`** — *Unreal* declara "quiero iniciar
   sesión como esta dirección". El bridge valida el formato (con checksum EIP-55 vía
   `ethers.getAddress`), **genera un nonce**, **construye el mensaje EIP-4361** y crea
   una sesión en estado `pending`. Devuelve `{requestId, message}`.
   - Clave: **el mensaje lo arma el servidor**, no el cliente. Así el servidor sabe
     exactamente qué debería haberse firmado (mismo nonce, mismo domain).
2. **`GET /api/siwe/pending`** — *la web* (con MetaMask conectado) descubre logins
   pendientes. Al leerlos, el bridge los marca **`signing`**: dejan de ofrecerse, así
   dos pestañas no firman el mismo login (misma idea "claim-on-read" que en compras).
3. **`personal_sign`** — *MetaMask* muestra el mensaje; el jugador firma. Es una firma
   **off-chain**: no se envía nada a la cadena, **no cuesta gas** (§7). En la web:
   `signer.signMessage(session.message)`.
4. **`POST /api/siwe/verify {requestId, signature}`** — *la web* devuelve la firma. El
   bridge la **verifica criptográficamente** (§4), **quema el nonce** y pasa la sesión a
   `done` guardando la `address` confirmada. Si el usuario rechaza, la web reporta
   `{error}` y la sesión pasa a `error`.
5. **`GET /api/siwe/login-status?requestId=…`** — *Unreal*, en polling, ve
   `pending → signing → done` (con `address`) o `error`. En `done`, el login está
   probado.

El estado `signing` cumple aquí la **misma doble función** que en la compra: informa a
Unreal de que su login avanza, y evita la doble firma al retirar la intención de la lista
de pendientes.

---

## 4. Qué hace exactamente la verificación (ecrecover)

Este es el corazón de la autenticación. Vive en `POST /api/siwe/verify`:

```js
const siwe = new SiweMessage(s.message);                 // el mensaje EXACTO que emitimos
const result = await siwe.verify({ signature, nonce: s.nonce, domain: SIWE_DOMAIN });
if (!result.success) throw new Error("firma no válida");
if (siwe.chainId !== CHAIN_ID) throw new Error("chainId incorrecto");
nrec.used = true;                                        // NONCE QUEMADO
s.status = "done";
s.address = siwe.address;
```

La operación criptográfica clave se llama **`ecrecover`**. La firma de un mensaje
(ECDSA) tiene una propiedad muy útil: **a partir del mensaje y de la firma se puede
*recuperar* la dirección pública que firmó**, sin conocer la clave privada.

```
      mensaje  +  firma   ──ecrecover──►   dirección que firmó
                                                  │
                                                  ▼
                              ¿== la `address` declarada en el mensaje?
                                    sí → auténtico     no → impostor
```

- **Solo el dueño de la clave privada de `0xAbc…`** puede producir una firma que, al
  recuperarse, dé `0xAbc…`. Nadie más puede fabricarla.
- Si un impostor dice "soy `0xAbc…`" pero firma con **su** clave, `ecrecover` devuelve
  **su** dirección, que **no coincide** con `0xAbc…` → la verificación **falla**.

Además de comparar la dirección recuperada con la declarada, `siwe.verify` comprueba que
el **nonce** y el **domain** del mensaje firmado son los que el servidor esperaba, y el
código verifica aparte el **chainId**. Todo tiene que cuadrar; si algo no encaja, se
rechaza (`401`).

> La firma no "contiene" la dirección: la dirección **se deduce** de la firma. Por eso
> falsificarla es equivalente a romper la criptografía de curva elíptica —inviable.

---

## 5. Por qué el nonce evita ataques de repetición

Imagina que **no** hubiera nonce y el mensaje fuese siempre el mismo texto fijo. Un
atacante que **observara una firma válida una sola vez** (en un log, en tráfico de red,
en una captura) podría **reenviarla** más tarde a `POST /api/siwe/verify` y el servidor,
al verla criptográficamente correcta, la aceptaría. Eso es un **ataque de repetición**
(*replay*): reutilizar una prueba antigua para autenticarse sin la clave.

El **nonce** lo impide. Es un valor aleatorio que el servidor genera **por cada intento
de login** y mete dentro del mensaje. Como el mensaje cambia en cada intento, **la firma
también cambia**: una firma vale **solo para el nonce con el que se produjo**.

Y aquí está el detalle decisivo: el nonce es de **un solo uso**. El servidor lleva
registro (`issuedNonces`) y, en cuanto verifica una firma con éxito, lo **quema**
(`nrec.used = true`):

```js
const nrec = issuedNonces.get(s.nonce);
if (!nrec || nrec.used) {            // no existe o ya se usó
  s.status = "error";
  return sendJson(res, 400, { error: "nonce inválido o ya usado" });
}
// … verificación OK …
nrec.used = true;                    // quemado: no se puede volver a usar
```

Consecuencia: aunque un atacante **capture** una firma válida, **no le sirve**. Al
intentar reenviarla, su nonce **ya está usado** → rechazo inmediato, antes incluso de
mirar la criptografía. Cada firma es un billete de un solo viaje.

| Sin nonce | Con nonce de un solo uso |
|-----------|--------------------------|
| El mismo mensaje siempre → misma firma reutilizable | Mensaje distinto cada vez → firma distinta cada vez |
| Capturar una firma = poder entrar siempre | Capturar una firma = inútil (nonce ya quemado) |

---

## 6. La decisión de añadir dependencias (`siwe` + `ethers`)

El bridge nació con una regla de diseño explícita (docs 02 y 03): **cero dependencias**.
Las lecturas de la cadena se hacen con `eth_call` crudo por JSON-RPC, codificando a mano
los argumentos (todo es `address`/`uint256`, es trivial). Con SIWE **rompimos esa regla
a propósito**, y merece la pena explicar por qué.

`server.js` ahora depende de dos librerías (ver `bridge/package.json`):

- **`siwe`** — construye el mensaje EIP-4361 (`SiweMessage`, `generateNonce`) y, sobre
  todo, lo **verifica** (`siwe.verify`).
- **`ethers`** — utilidades de criptografía y de direcciones (`getAddress` con checksum
  EIP-55) sobre las que se apoya la verificación (el `ecrecover`).

**Por qué se rompe el cero-dependencias aquí y no antes:**

- **Criptografía no se hace en casa.** Codificar un `eth_call` a mano es aritmética de
  bytes: si me equivoco, la lectura falla de forma obvia y visible. Implementar
  `ecrecover`, el parseo estricto de EIP-4361 y las comprobaciones de nonce/domain a mano
  es **código de seguridad**: un fallo sutil no "se ve", simplemente deja una puerta
  abierta. El coste de un error es categóricamente distinto.
- **Librerías probadas > código propio.** `siwe` y `ethers` son estándar de facto,
  auditadas y mantenidas por la comunidad. Reinventarlas sería asumir un riesgo enorme
  para ahorrar una dependencia. La regla de "cero deps" servía a la **simplicidad**; en
  seguridad, la simplicidad correcta es **apoyarse en lo probado**.
- **El coste queda acotado.** Solo SIWE trae dependencias. Las lecturas siguen sin
  librerías. Y la instalación es condicional: el script de arranque para Windows
  (`start-bridge.ps1`) hace `npm install` **solo cuando hace falta**.

> Principio de la consultoría: la regla "cero dependencias" es una preferencia, no un
> dogma. Se dobla justo donde el riesgo de hacerlo uno mismo supera al de depender de
> otros: **criptografía**. Ahí, "no lo hagas en casa".

---

## 7. La distinción clave para la demo: login gratis vs. compra con gas

Este es el contraste más importante que mostrar en una presentación, porque ilumina cómo
funciona Ethereum:

```
   LOGIN (SIWE)                          COMPRA (buy)
   ───────────────                       ───────────────
   Firma un MENSAJE                       Firma una TRANSACCIÓN
   personal_sign / signMessage            eth_sendTransaction
   OFF-CHAIN  (nada toca la cadena)       ON-CHAIN (se mina un bloque)
   GRATIS  (sin gas)                      CUESTA GAS
   Prueba propiedad de la clave           Cambia el estado (balances, ETH)
   Reversible/efímero (sesión)            Permanente (queda en la cadena)
```

- **Firmar un mensaje es off-chain.** `personal_sign` produce una firma criptográfica
  **sin enviar nada** a la blockchain. No hay minero que ejecute nada, no cambia ningún
  estado → **no hay gas**. Por eso el `statement` del mensaje dice literalmente *"Firmar
  no cuesta gas"*.
- **Comprar es on-chain.** `buy()` es una **transacción**: se transmite a la red, un
  bloque la ejecuta, cambia balances y transfiere ETH. Eso consume **gas** y es
  permanente.

Ambas cosas usan "la firma de MetaMask", y ahí está la confusión típica que conviene
deshacer en la demo: **firmar ≠ pagar**. Firmar prueba autoría; solo cuando lo firmado
es una *transacción* que la red ejecuta hay gas. El login demuestra que ese matiz se
entiende y se aprovecha: autenticación robusta a **coste cero**.

> Frase para la demo: *"Iniciar sesión es firmar un papel; comprar es firmar un cheque.
> El papel no cuesta nada; el cheque mueve dinero en la cadena."*

---

## 8. El lado Unreal: el login es "más de lo mismo"

La gran conclusión de diseño de la doc 05 fue que el lado del juego son **solo dos
llamadas HTTP + polling**. El login **no introduce nada nuevo en Unreal**: es
exactamente el mismo patrón que la compra, con otros endpoints.

Comparación directa con lo que ya existe (`BuyItem` en `BlockchainStoreClient.cpp`):

| Paso | Compra (ya implementado) | Login SIWE (mismo molde) |
|------|--------------------------|--------------------------|
| 1ª llamada (registrar intención) | `POST /api/purchase-intent` → `requestId` | `POST /api/siwe/login-intent` → `requestId` |
| 2ª llamada (polling del estado) | `GET /api/purchase-status` hasta `done`/`error` | `GET /api/siwe/login-status` hasta `done`/`error` |
| Máquina de estados observada | `pending → signing → done` | `pending → signing → done` |
| Quién firma | La web + MetaMask (fuera de Unreal) | La web + MetaMask (fuera de Unreal) |
| Resultado al terminar | `txHash`, refresca inventario | `address` **confirmada** |

Es decir, el login **reaprovecha el subsystem** (`UBlockchainStoreClient`,
`GameInstanceSubsystem`) tal cual: el mismo `MakeRequest`, el mismo `TryParseJsonObject`,
el mismo `FTimerManager` estable para el polling, los mismos delegates
`BlueprintAssignable` para avisar a la UI. Solo hay que añadir un `Login()` calcado de
`BuyItem()` y un `HandleLoginStatusResponse` calcado de `HandlePurchaseStatusResponse`.

### La unificación con `WalletAddress`

Hoy, en el subsystem, la dirección se pone **a mano** (ver el header):

```cpp
/** Address de la wallet del jugador. De momento set manual; el login SIWE vendra despues. */
UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore|Config")
FString WalletAddress;

UFUNCTION(BlueprintCallable, Category = "BlockchainStore|Config")
void SetWalletAddress(const FString& InAddress);
```

Esa `WalletAddress` es la que ya alimenta `FetchInventory`, `FetchProgress` y
`BuyItem`. El login **la unifica**: en vez de teclearla, el flujo la **produce y la
confirma**.

- El jugador inicia login con una dirección **candidata**.
- Cuando `login-status` llega a `done`, el bridge devuelve la `address` **verificada**.
- Unreal escribe esa dirección en `WalletAddress` (el mismo campo que ya usa todo lo
  demás). `SetWalletAddress` deja de ser una entrada manual y pasa a ser el **resultado
  autenticado del login**.

Resultado: una **única fuente de verdad** para la identidad del jugador. Antes del login,
`WalletAddress` era una afirmación sin probar; después, es una dirección **cuya propiedad
está demostrada** —y todas las lecturas y compras que ya dependían de ese campo heredan
esa garantía sin cambiar una línea.

> Estado actual (honesto para la demo): el **bridge** y la **web** implementan SIWE de
> punta a punta (probado con `curl` haciendo de Unreal + MetaMask firmando, y con
> `ethers.Wallet.signMessage` en pruebas automáticas). El **subsystem de Unreal** aún no
> tiene el `Login()`; su diseño es el de esta sección —dos llamadas y polling, idéntico a
> la compra— por lo que la integración es mecánica, no conceptual.

---

## Resumen para la demo

- **Connect lee una dirección; SIWE la demuestra.** Autenticar es probar la **propiedad
  de la clave**, y eso solo se logra con una **firma**.
- **SIWE (EIP-4361)** es un mensaje legible y estructurado (domain, address, chainId,
  nonce, issued-at) que el usuario firma para iniciar sesión.
- El login **reutiliza el patrón de tres actores**: Unreal pide, el bridge coordina **sin
  claves**, la web + MetaMask firman. Estados `pending → signing → done`.
- **La verificación es `ecrecover`**: del mensaje + la firma se recupera la dirección
  firmante y se compara con la declarada; además cuadran nonce, domain y chainId.
- **El nonce de un solo uso** cierra los ataques de repetición: una firma capturada es
  inútil porque su nonce ya está quemado.
- **`siwe` + `ethers`** entran a propósito: la criptografía se apoya en librerías
  probadas, nunca en código casero.
- **En Unreal es "más de lo mismo"**: dos llamadas HTTP + polling, calcadas de la compra,
  y el login **unifica `WalletAddress`** convirtiéndola en identidad **probada**.
- **La distinción que lo corona:** login = firma de un **mensaje**, off-chain, **sin
  gas**; compra = firma de una **transacción**, on-chain, **con gas**. Firmar no es pagar.
