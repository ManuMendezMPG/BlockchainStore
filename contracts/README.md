# /contracts — Smart contract (Foundry)

Proyecto de Solidity gestionado con [Foundry](https://book.getfoundry.sh/).
Contendrá el contrato **ERC-1155** que representa los items de la tienda del juego.

> **Estado:** aún **no** inicializado. Esta carpeta solo tiene este README y un
> `.env.example`. La inicialización con Foundry se hará más adelante.

## Qué irá aquí

Tras inicializar (`forge init` / estructura estándar de Foundry):

```
contracts/
├── src/            # Contratos (p. ej. GameItems.sol — ERC-1155)
├── test/           # Tests en Solidity (forge test)
├── script/         # Scripts de despliegue (forge script)
├── lib/            # Dependencias (forge install, p. ej. OpenZeppelin)
├── foundry.toml    # Configuración de Foundry
└── .env            # Secretos locales (NO se commitea; ver .env.example)
```

## Por qué ERC-1155

ERC-1155 permite **múltiples tipos de token en un solo contrato**: cada `id` es un
tipo de item de la tienda y el balance es la cantidad que posee el jugador. Es más
eficiente en gas que desplegar un ERC-721/ERC-20 por item y soporta transferencias
por lotes (batch), ideal para inventarios de juego.

## Próximos pasos (cuando toque inicializar)

1. `forge init` (o crear la estructura a mano para no pisar este README).
2. `forge install OpenZeppelin/openzeppelin-contracts` para la base ERC-1155.
3. Implementar `src/GameItems.sol`.
4. Tests en `test/` y script de deploy en `script/`.
5. Copiar `.env.example` a `.env` y rellenar los valores reales.

> El bytecode/ABIs compilados (`out/`), la caché (`cache/`) y los logs de deploy
> (`broadcast/`) están ignorados por git (ver `.gitignore` raíz).
