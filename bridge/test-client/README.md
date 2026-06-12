# /bridge/test-client — Cliente de prueba (simula a Unreal)

Herramienta de **validación**, **no parte del producto**. `test-client.js` reproduce
exactamente lo que hará el juego de Unreal contra la API del bridge: registra una
intención de compra y hace **polling** del estado hasta que se resuelve, mientras la
**firma real** ocurre en el navegador (web del bridge + MetaMask).

Si este script completa una compra de principio a fin, el camino de Unreal está
despejado: Unreal solo tiene que hacer las **mismas llamadas HTTP** (POST intent +
polling status) desde C++/Blueprints.

## Por qué Node y no PowerShell

Es un script **Node sin dependencias** (solo el módulo `http` nativo). Se eligió
sobre un `.ps1` porque **replica fielmente lo que hará Unreal**: peticiones HTTP
crudas, serialización/parseo de JSON y un bucle de polling. `Invoke-RestMethod` de
PowerShell ocultaría justo esos detalles. Además es coherente con el resto del bridge
(cero dependencias) y Node ya está instalado en Windows.

## Requisitos previos (todo el entorno arrancado)

1. **Anvil + contratos** (en WSL): `bash scripts/start-local.sh`.
2. **Servidor del bridge** (en **Windows**, PowerShell):
   ```powershell
   cd \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge
   node server.js
   ```
3. **Web del bridge abierta** en el navegador (`http://localhost:8787`) **con MetaMask
   conectado a Anvil (chain id 31337)**. Esta web es la que **firma**: al conectar,
   sondea las intenciones pendientes y dispara MetaMask. Sin la web abierta, la compra
   se quedará en `pending` y el cliente dará **timeout**.

> Entorno: el server corre en **Windows**; ejecuta este cliente **en Windows**. Desde
> WSL no se alcanza `http://localhost:8787`.

## Uso

```powershell
cd \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge\test-client
node test-client.js <address 0x...> <itemId 0-9> <quantity >=1>

# Ejemplo: comprar 1 arco (id 2) con la cuenta #0 de Anvil
node test-client.js 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266 2 1
```

Variable opcional: `BRIDGE_URL` (por defecto `http://localhost:8787`).

## Flujo esperado paso a paso

1. **[1]** Lee `GET /api/inventory` y muestra el balance del item **ANTES**.
2. **[2]** Hace `POST /api/purchase-intent` y obtiene el `requestId` (queda `pending`).
3. **[3]** Hace polling de `GET /api/purchase-status` cada ~1,5 s, imprimiendo el
   estado en cada cambio (`pending` → `signing` → `done`/`error`). Al pasar a
   `signing` te recuerda que **firmes en MetaMask** en el navegador.
4. **[4]** Al resolver:
   - `done`: imprime el `txHash`, vuelve a leer el inventario y muestra el balance
     **DESPUÉS** y la diferencia (`Δ +n`).
   - `error`: imprime el mensaje de error del contrato.

Salida real de una compra correcta (arco, cuenta #0):
```
  [1] Balance ANTES de "Arco": 0
  [2] Intención registrada. requestId = c2219172-…
  [3] estado: done
  [4] ✅ DONE · txHash = 0x926f39767cd8…
      Balance DESPUÉS de "Arco": 1  (Δ +1)
```

## Códigos de salida (para encadenar)

| Código | Significado |
|--------|-------------|
| `0` | Compra `done` |
| `1` | El contrato revirtió (`error`) o respuesta HTTP inesperada |
| `2` | Argumentos inválidos (address/itemId/quantity) |
| `3` | Timeout (90 s sin resolver — ¿firmaste en MetaMask?) |
| `4` | No se pudo contactar con el bridge (server/web no arrancados) |

## Errores frecuentes

- **`address inválida`**: la dirección debe ser `0x` + 40 hex. Es fácil equivocarse
  copiando; el script lo valida antes de llamar.
- **`No se pudo contactar con el bridge` (exit 4)**: falta `node server.js` en Windows.
- **Timeout (exit 3)**: el server está, pero **nadie firmó**. Abre la web del bridge
  con MetaMask conectado a Anvil; su consumidor recogerá la intención y abrirá MetaMask.
