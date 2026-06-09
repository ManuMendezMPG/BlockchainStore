# /bridge — Puente web local (Node.js + ethers.js + MetaMask)

Puente que conecta el **juego de Unreal** con la blockchain a través de **MetaMask**.
El juego no maneja claves ni habla JSON-RPC directamente: delega en este puente.

> **Estado:** aún **no** inicializado. Esta carpeta solo tiene este README y un
> `.env.example`. No se ha hecho `npm init` ni instalado dependencias todavía.

## Cómo funciona

El puente tiene dos mitades:

1. **Servidor Node.js** (local): expone un endpoint (HTTP y/o WebSocket) que el
   juego de Unreal consume desde `localhost`. Sirve también la página web.
2. **Página web** (HTML + [ethers.js](https://docs.ethers.org/)): se abre en el
   navegador, se conecta a **MetaMask** vía `window.ethereum` y firma las
   transacciones contra el contrato ERC-1155 de `/contracts`.

Las claves privadas **nunca salen de MetaMask**: el juego y el servidor solo ven
peticiones y resultados, nunca el secreto del jugador.

## Qué irá aquí

```
bridge/
├── package.json        # Dependencias (express/ws, ethers, ...)
├── server.js           # Servidor local que habla con el juego
├── public/
│   └── index.html      # Página con ethers.js + conexión MetaMask
├── abi/                # ABI del contrato ERC-1155 (copiado de /contracts/out)
└── .env                # Config local (NO se commitea; ver .env.example)
```

## Próximos pasos (cuando toque inicializar)

1. `npm init -y` y añadir dependencias (p. ej. `ethers`, `express` o `ws`).
2. Implementar `server.js` y `public/index.html`.
3. Copiar el ABI del contrato desde `contracts/out/` a `abi/`.
4. Copiar `.env.example` a `.env` y rellenar valores.

> `node_modules/` está ignorado por git (ver `.gitignore` raíz).
