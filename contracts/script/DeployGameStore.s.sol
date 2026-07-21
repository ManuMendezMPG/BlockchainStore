// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Script, console} from "forge-std/Script.sol";
import {GameStore} from "../src/GameStore.sol";
import {Achievements} from "../src/Achievements.sol";

/// @title DeployGameStore — deploys GameStore + Achievements and connects them.
/// @dev DEPLOYMENT ORDER MATTERS:
///      1) GameStore FIRST (nonce 0 of account #0) → lands on the deterministic
///         address 0x5FbDB2315678afecb367f032d93F642f64180aa3 that the bridge uses.
///      2) Achievements AFTER (nonce 1).
///      A contract's address depends on (deployer, nonce) at the moment of the
///      CREATE; later transactions (setters, setItem) do not change it.
contract DeployGameStore is Script {
    // Item IDs.
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

        // 1) GameStore first → deterministic address for the bridge.
        store = new GameStore("ipfs://game-items/{id}.json");

        // 2) Achievements after.
        achievements = new Achievements("ipfs://achievements/{id}.json");

        // 3) Connection between contracts:
        //    - GameStore needs to know Achievements to mint medallions.
        //    - Achievements authorizes GameStore as its only `minter`.
        store.setAchievements(address(achievements));
        achievements.setMinter(address(store));

        // 4) Full catalog (prices in wei via the `ether` suffix).
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

        // 5) Informative logs.
        console.log("==========================================");
        console.log("GameStore    deployed at:  ", address(store));
        console.log("Achievements deployed at:  ", address(achievements));
        console.log("Owner:                     ", store.owner());
        console.log("Achievements minter:       ", achievements.minter());
        console.log("------------------------------------------");
        console.log("Catalog: 10 items (ids 0-9) listed.");
        console.log("Medallions: ARQUERO(0) soulbound, MERCADER(1) transferable, COLECCIONISTA(2) soulbound.");
        console.log("==========================================");
    }
}
