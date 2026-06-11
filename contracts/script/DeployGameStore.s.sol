// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Script, console} from "forge-std/Script.sol";
import {GameStore} from "../src/GameStore.sol";
import {Achievements} from "../src/Achievements.sol";

/// @title DeployGameStore — despliega GameStore + Achievements y los conecta.
/// @dev ORDEN DE DESPLIEGUE IMPORTANTE:
///      1) GameStore PRIMERO (nonce 0 de la cuenta #0) → cae en la dirección
///         determinista 0x5FbDB2315678afecb367f032d93F642f64180aa3 que usa el bridge.
///      2) Achievements DESPUÉS (nonce 1).
///      La dirección de un contrato depende de (deployer, nonce) en el momento del
///      CREATE; las transacciones posteriores (setters, setItem) no la cambian.
contract DeployGameStore is Script {
    // IDs de items.
    uint256 constant ESPADA = 0;
    uint256 constant ESCUDO = 1;
    uint256 constant ARCO = 2;
    uint256 constant CARCAJ_5 = 3;
    uint256 constant CARCAJ_10 = 4;
    uint256 constant CARCAJ_20 = 5;
    uint256 constant FLECHA = 6;
    uint256 constant BOTELLA_VACIA = 7;
    uint256 constant POCION_VIDA = 8;
    uint256 constant POCION_MANA = 9;

    function run() external returns (GameStore store, Achievements achievements) {
        vm.startBroadcast();

        // 1) GameStore primero → dirección determinista para el bridge.
        store = new GameStore("ipfs://game-items/{id}.json");

        // 2) Achievements después.
        achievements = new Achievements("ipfs://achievements/{id}.json");

        // 3) Conexión entre contratos:
        //    - GameStore necesita conocer a Achievements para acuñar medallones.
        //    - Achievements autoriza a GameStore como su único `minter`.
        store.setAchievements(address(achievements));
        achievements.setMinter(address(store));

        // 4) Catálogo completo (precios en wei vía sufijo `ether`).
        store.setItem(ESPADA, 0.01 ether);
        store.setItem(ESCUDO, 0.008 ether);
        store.setItem(ARCO, 0.012 ether);
        store.setItem(CARCAJ_5, 0.005 ether);
        store.setItem(CARCAJ_10, 0.01 ether);
        store.setItem(CARCAJ_20, 0.02 ether);
        store.setItem(FLECHA, 0.0005 ether);
        store.setItem(BOTELLA_VACIA, 0.002 ether);
        store.setItem(POCION_VIDA, 0.003 ether);
        store.setItem(POCION_MANA, 0.003 ether);

        vm.stopBroadcast();

        // 5) Logs informativos.
        console.log("==========================================");
        console.log("GameStore    desplegado en:", address(store));
        console.log("Achievements desplegado en:", address(achievements));
        console.log("Owner:                     ", store.owner());
        console.log("Minter de Achievements:    ", achievements.minter());
        console.log("------------------------------------------");
        console.log("Catalogo: 10 items (ids 0-9) dados de alta.");
        console.log("Medallones: ARQUERO(0) soulbound, MERCADER(1) transferible, COLECCIONISTA(2) soulbound.");
        console.log("==========================================");
    }
}
