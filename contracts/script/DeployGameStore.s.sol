// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Script, console} from "forge-std/Script.sol";
import {GameStore} from "../src/GameStore.sol";

/// @title DeployGameStore — script de despliegue de GameStore en una red local.
/// @notice Despliega el contrato y precarga el catálogo con 3 items de ejemplo
///         para que la tienda tenga contenido desde el primer bloque.
/// @dev Se ejecuta con `forge script`. Todo lo que va entre `vm.startBroadcast()`
///      y `vm.stopBroadcast()` se convierte en transacciones REALES firmadas por
///      la cuenta que indiques en la línea de comandos (--private-key / --sender)
///      y enviadas al --rpc-url. El resto del código (logs, lecturas) NO genera
///      transacciones: corre solo en la simulación local de forge.
contract DeployGameStore is Script {
    // ── Catálogo de ejemplo ──────────────────────────────────────────
    // Precios usando el sufijo `ether` de Solidity, que el compilador convierte
    // a wei (1 ether = 1e18 wei). Así evitamos escribir los ceros a mano.
    uint256 internal constant ITEM_SWORD = 0; // "Espada"
    uint256 internal constant ITEM_SHIELD = 1; // "Escudo"
    uint256 internal constant ITEM_POTION = 2; // "Poción"

    uint256 internal constant PRICE_SWORD = 0.01 ether; // 10_000_000_000_000_000 wei (1e16)
    uint256 internal constant PRICE_SHIELD = 0.005 ether; //  5_000_000_000_000_000 wei (5e15)
    uint256 internal constant PRICE_POTION = 0.001 ether; //  1_000_000_000_000_000 wei (1e15)

    function run() external returns (GameStore store) {
        // Inicio del "broadcast": a partir de aquí, cada llamada que cambie
        // estado on-chain se firma y se envía como transacción real.
        // Sin argumento, usa la cuenta que pasas por CLI (--private-key/--sender),
        // que será el OWNER del contrato (GameStore es Ownable(msg.sender)).
        vm.startBroadcast();

        // 1) Desplegar el contrato. El string es la URI base de metadatos ERC-1155;
        //    `{id}` lo sustituye el cliente por el id del token en hexadecimal.
        store = new GameStore("ipfs://game-items/{id}.json");

        // 2) Dar de alta el catálogo. setItem es onlyOwner; funciona porque el
        //    firmante del broadcast es el owner recién asignado en el constructor.
        store.setItem(ITEM_SWORD, PRICE_SWORD);
        store.setItem(ITEM_SHIELD, PRICE_SHIELD);
        store.setItem(ITEM_POTION, PRICE_POTION);

        // Fin del broadcast: lo de abajo ya no genera transacciones.
        vm.stopBroadcast();

        // 3) Logs informativos (solo consola; no son transacciones).
        console.log("==========================================");
        console.log("GameStore desplegado en:", address(store));
        console.log("Owner del contrato:      ", store.owner());
        console.log("------------------------------------------");
        console.log("Items dados de alta (precio en wei):");
        console.log("  id %s (Espada)  precio:", ITEM_SWORD, PRICE_SWORD);
        console.log("  id %s (Escudo)  precio:", ITEM_SHIELD, PRICE_SHIELD);
        console.log("  id %s (Pocion)  precio:", ITEM_POTION, PRICE_POTION);
        console.log("==========================================");
    }
}
