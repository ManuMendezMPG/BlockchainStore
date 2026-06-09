# /docs — Documentación del proyecto

Documentación viva del proyecto: diseño, decisiones de arquitectura y contratos de
integración entre las piezas.

## Contenido previsto

- **architecture.md** — Diagrama y descripción detallada de las cuatro piezas
  (contracts, bridge, unreal, docs) y cómo encajan.
- **bridge-protocol.md** — Formato de los mensajes entre el juego (`/unreal`) y el
  puente (`/bridge`): endpoints, payloads, eventos de WebSocket.
- **contract.md** — Diseño del ERC-1155: qué representa cada `id` de item, funciones
  expuestas (mint/compra/uso), permisos y roles.
- **deployments.md** — Direcciones de los contratos desplegados por red (local,
  testnet, mainnet) y cómo apuntar el puente a cada una.
- **setup.md** — Cómo levantar el entorno completo en local (nodo Anvil, deploy del
  contrato, arranque del puente, conexión del juego).

> Por ahora solo está este índice. Los documentos se irán añadiendo a medida que
> avance la implementación.
