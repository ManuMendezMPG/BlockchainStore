// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Test} from "forge-std/Test.sol";
import {GameStore} from "../src/GameStore.sol";
import {Achievements} from "../src/Achievements.sol";

/// @dev Tests de las reglas de dependencia, el reembolso y la integración de
///      logros (GameStore llamando a Achievements). GameStore y Achievements se
///      despliegan y conectan en setUp().
contract GameStoreRulesTest is Test {
    GameStore internal store;
    Achievements internal ach;

    address internal owner = makeAddr("owner");
    address internal player = makeAddr("player");

    // IDs de items (deben coincidir con GameStore).
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

    // IDs de medallones.
    uint256 constant ARQUERO = 0;
    uint256 constant MERCADER = 1;
    uint256 constant COLECCIONISTA = 2;

    // Precios de prueba.
    uint256 constant P_ESPADA = 0.05 ether;
    uint256 constant P_ESCUDO = 0.04 ether;
    uint256 constant P_ARCO = 0.03 ether;
    uint256 constant P_CARCAJ_5 = 0.01 ether;
    uint256 constant P_CARCAJ_10 = 0.02 ether;
    uint256 constant P_CARCAJ_20 = 0.05 ether;
    uint256 constant P_FLECHA = 0.001 ether;
    uint256 constant P_BOTELLA = 0.002 ether;
    uint256 constant P_POCION = 0.003 ether;

    function setUp() public {
        vm.startPrank(owner);
        store = new GameStore("ipfs://items/{id}.json");
        ach = new Achievements("ipfs://medals/{id}.json");

        // Conexión bidireccional: GameStore conoce a Achievements, y Achievements
        // autoriza a GameStore como su único minter.
        store.setAchievements(address(ach));
        ach.setMinter(address(store));

        // Catálogo completo.
        store.setItem(ESPADA, P_ESPADA);
        store.setItem(ESCUDO, P_ESCUDO);
        store.setItem(ARCO, P_ARCO);
        store.setItem(CARCAJ_5, P_CARCAJ_5);
        store.setItem(CARCAJ_10, P_CARCAJ_10);
        store.setItem(CARCAJ_20, P_CARCAJ_20);
        store.setItem(FLECHA, P_FLECHA);
        store.setItem(BOTELLA_VACIA, P_BOTELLA);
        store.setItem(POCION_VIDA, P_POCION);
        store.setItem(POCION_MANA, P_POCION);
        vm.stopPrank();

        vm.deal(player, 100 ether);
    }

    /// Lleva al jugador a 20 flechas compradas históricas (desbloquea ARQUERO).
    /// Demuestra el bucle realista: con carcaj_10 (cap 10), compras 10, quemas y
    /// vuelves a comprar 10 → 20 históricas aunque nunca tengas más de 10 a la vez.
    function _unlockArquero() internal {
        vm.startPrank(player);
        store.buy{value: P_ARCO}(ARCO, 1);
        store.buy{value: P_CARCAJ_5}(CARCAJ_5, 1);
        store.buy{value: P_CARCAJ_10}(CARCAJ_10, 1); // cap 10
        store.buy{value: P_FLECHA * 10}(FLECHA, 10); // 10 históricas, 10 en mano
        store.burn(player, FLECHA, 10); // vacía la mano
        store.buy{value: P_FLECHA * 10}(FLECHA, 10); // 20 históricas → ARQUERO
        vm.stopPrank();
    }

    // ───────────────────────── Reembolso ─────────────────────────────

    function test_BuyRefundsExcess() public {
        uint256 paid = P_ESPADA + 0.5 ether;
        uint256 before = player.balance;

        vm.prank(player);
        store.buy{value: paid}(ESPADA, 1);

        assertEq(address(store).balance, P_ESPADA, "el contrato retiene solo el coste");
        assertEq(player.balance, before - P_ESPADA, "se devuelve el excedente");
    }

    // ─────────────────── Dependencia: flechas/arco/carcaj ────────────

    function test_RevertWhen_BuyArrowsWithoutBow() public {
        vm.prank(player);
        vm.expectRevert(GameStore.NeedBow.selector);
        store.buy{value: P_FLECHA}(FLECHA, 1);
    }

    function test_RevertWhen_ArrowsExceedQuiverCapacity() public {
        vm.startPrank(player);
        store.buy{value: P_ARCO}(ARCO, 1);
        store.buy{value: P_CARCAJ_5}(CARCAJ_5, 1); // cap 5
        vm.expectRevert(abi.encodeWithSelector(GameStore.QuiverCapacityExceeded.selector, 5, 0, 6));
        store.buy{value: P_FLECHA * 6}(FLECHA, 6);
        vm.stopPrank();
    }

    function test_BuyArrowsWithinCapacity() public {
        vm.startPrank(player);
        store.buy{value: P_ARCO}(ARCO, 1);
        store.buy{value: P_CARCAJ_5}(CARCAJ_5, 1);
        store.buy{value: P_FLECHA * 5}(FLECHA, 5);
        vm.stopPrank();
        assertEq(store.balanceOf(player, FLECHA), 5);
    }

    function test_RevertWhen_BuyCarcaj10WithoutCarcaj5() public {
        vm.prank(player);
        vm.expectRevert(GameStore.NeedQuiver5.selector);
        store.buy{value: P_CARCAJ_10}(CARCAJ_10, 1);
    }

    function test_BuyCarcaj10WithCarcaj5() public {
        vm.startPrank(player);
        store.buy{value: P_CARCAJ_5}(CARCAJ_5, 1);
        store.buy{value: P_CARCAJ_10}(CARCAJ_10, 1);
        vm.stopPrank();
        assertEq(store.balanceOf(player, CARCAJ_10), 1);
        assertEq(store.quiverCapacity(player), 10);
    }

    // ───────────────── Dependencia: pociones/botellas ────────────────

    function test_RevertWhen_BuyPotionWithoutBottle() public {
        vm.prank(player);
        vm.expectRevert(abi.encodeWithSelector(GameStore.NeedEmptyBottle.selector, 1, 0));
        store.buy{value: P_POCION}(POCION_VIDA, 1);
    }

    function test_BuyPotionConsumesBottle() public {
        vm.startPrank(player);
        store.buy{value: P_BOTELLA * 2}(BOTELLA_VACIA, 2);
        store.buy{value: P_POCION}(POCION_VIDA, 1);
        vm.stopPrank();
        assertEq(store.balanceOf(player, BOTELLA_VACIA), 1, "consumio 1 botella");
        assertEq(store.balanceOf(player, POCION_VIDA), 1, "obtuvo la pocion");
    }

    // ──────────────── Dependencia: carcaj_20 ↔ logro ARQUERO ──────────

    function test_RevertWhen_BuyCarcaj20WithoutArchero() public {
        vm.prank(player);
        vm.expectRevert(GameStore.NeedArcheroAchievement.selector);
        store.buy{value: P_CARCAJ_20}(CARCAJ_20, 1);
    }

    function test_BuyCarcaj20AfterArchero() public {
        _unlockArquero();
        vm.prank(player);
        store.buy{value: P_CARCAJ_20}(CARCAJ_20, 1);
        assertEq(store.balanceOf(player, CARCAJ_20), 1);
        assertEq(store.quiverCapacity(player), 20);
    }

    // ─────────────────────── Logros (integración) ────────────────────

    function test_ArqueroMintedAt20Arrows() public {
        _unlockArquero();
        assertEq(ach.balanceOf(player, ARQUERO), 1, "ARQUERO acunado");
        assertTrue(ach.hasArquero(player));
    }

    function test_ArqueroNotMintedTwice() public {
        _unlockArquero(); // 20 históricas, ARQUERO ya acuñado, 10 flechas en mano
        // Ampliamos capacidad y compramos más flechas: el logro NO debe re-acuñarse
        // (y la compra NO debe revertir por intentar re-acuñar).
        vm.startPrank(player);
        store.buy{value: P_CARCAJ_20}(CARCAJ_20, 1); // cap 20
        store.buy{value: P_FLECHA * 5}(FLECHA, 5); // 25 históricas
        vm.stopPrank();
        assertEq(ach.balanceOf(player, ARQUERO), 1, "sigue siendo 1");
    }

    function test_ColeccionistaMintedOnFullSet() public {
        vm.startPrank(player);
        store.buy{value: P_ESPADA}(ESPADA, 1);
        store.buy{value: P_ESCUDO}(ESCUDO, 1);
        store.buy{value: P_ARCO}(ARCO, 1);
        store.buy{value: P_CARCAJ_5}(CARCAJ_5, 1); // completa el set
        vm.stopPrank();
        assertEq(ach.balanceOf(player, COLECCIONISTA), 1, "COLECCIONISTA acunado");
    }

    function test_MercaderMintedOnSpendThreshold() public {
        // Gasto: 3 × 0.05 = 0.15 ETH > umbral (0.1 ETH).
        vm.prank(player);
        store.buy{value: P_ESPADA * 3}(ESPADA, 3);

        assertEq(ach.balanceOf(player, MERCADER), 1, "MERCADER acunado");
        uint8 rarity = uint8(ach.mercaderRarity(player));
        assertTrue(rarity >= 1 && rarity <= 3, "rareza valida (Bronce/Plata/Oro)");
    }

    function test_MercaderNotMintedBelowThreshold() public {
        // Gasto por debajo del umbral: no se acuña.
        vm.prank(player);
        store.buy{value: P_ESPADA}(ESPADA, 1); // 0.05 < 0.1
        assertEq(ach.balanceOf(player, MERCADER), 0);
    }
}
