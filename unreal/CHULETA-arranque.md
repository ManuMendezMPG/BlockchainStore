# 🎮 BlockchainStore — Chuleta de arranque

Resumen rápido de cómo levantar todo el entorno para trabajar. Pensado para no perderse entre terminales.

---

## 🧠 Mapa mental: qué corre dónde

| Pieza | Mundo | ¿Se queda abierta? |
|---|---|---|
| **Anvil** (blockchain local) | WSL (Ubuntu) | ✅ Sí, proceso vivo |
| **Servidor del puente** (`node server.js`) | **Windows** (PowerShell/CMD) | ✅ Sí, proceso vivo |
| **Claude Code** | WSL (Ubuntu) | ✅ Mientras programas |
| **Deploy** (`forge script`) | WSL (Ubuntu) | ❌ Puntual, termina solo |
| **Git / commits / cast** | WSL (Ubuntu) | ❌ Puntual |
| **Unreal Editor** | Windows | (aparte, cuando toque UI) |

➡️ **Regla de oro:** blockchain en WSL, servidor del puente y Unreal en Windows. Se hablan por `localhost`.

---

## 🚀 Rutina de arranque (en orden)

> El **orden importa**. Cada vez que arrancas Anvil de cero, la cadena se borra y hay que redeployar.

### 1️⃣ Arrancar Anvil — Terminal WSL #1 (déjala abierta)
```bash
anvil
```
Escucha en `127.0.0.1:8545`, chain id `31337`. Imprime 10 cuentas de prueba.
⚠️ Si la cierras, pierdes el contrato y los items comprados.

### 2️⃣ Desplegar el contrato — Terminal WSL #2 (puntual)
```bash
cd ~/projects/contracts
export PATH="$HOME/.foundry/bin:$PATH"
forge script script/DeployGameStore.s.sol \
  --rpc-url http://127.0.0.1:8545 \
  --private-key 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80 \
  --broadcast
```
> La clave es la cuenta #0 de Anvil: PÚBLICA, solo para local. Nunca una real.

Debería desplegar en la dirección determinista:
`0x5FbDB2315678afecb367f032d93F642f64180aa3`

**Verificar (opcional):**
```bash
cast code 0x5FbDB2315678afecb367f032d93F642f64180aa3 --rpc-url http://127.0.0.1:8545
```
- Devuelve bytecode largo → ✅ contrato vivo
- Devuelve `0x` → ❌ no se desplegó, repite el paso 2

### 3️⃣ Arrancar el servidor del puente — Terminal **Windows** (déjala abierta)
```
cd <ruta-al-bridge>
node server.js
```
Sirve la web en algo como `http://localhost:3000` (mira lo que diga el README).

### 4️⃣ Abrir la web — Navegador (perfil Chrome de desarrollo con MetaMask)
- Ir a `http://localhost:3000`
- **Conectar wallet** → MetaMask popup → cuenta #0
- Comprobar red = **Anvil Local (31337)**
- Ver tienda → Comprar → firmar en MetaMask → ver inventario

---

## 🔧 Problemas típicos y arreglo rápido

| Síntoma | Causa | Arreglo |
|---|---|---|
| Web no muestra tienda/inventario, consola dice `could not decode (value="0x")` | No hay contrato (Anvil reiniciado sin redeploy) | Repetir **paso 2** (deploy) |
| MetaMask error "nonce too high" | Anvil reiniciado, MetaMask recuerda nonce viejo | MetaMask → Settings → Advanced → **Clear activity tab data** |
| MetaMask no conecta a la red | Anvil no está corriendo | Arrancar Anvil (paso 1) |
| El puente "no va sin razón" | Node corriendo en el mundo equivocado | Recordar: **Node va en Windows**, no WSL |

---

## 💾 Guardar cambios (git) — Terminal WSL (puntual)
```bash
cd ~/projects
git add .
git status        # ⚠️ revisar SIEMPRE: ningún .env real, ningún token, nada de Binaries/Intermediate/Saved
git commit -m "mensaje descriptivo"
git push
```

---

## 📋 Resumen ultra-rápido (TL;DR)
1. **Anvil** (WSL) — dejar abierto
2. **Deploy** (WSL) — `forge script ... --broadcast`
3. **Node server** (Windows) — dejar abierto
4. **Navegador** — conectar y usar

Reinicié Anvil → **redeployar** (paso 2) + **Clear activity** en MetaMask.
