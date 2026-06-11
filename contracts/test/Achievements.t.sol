// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Test} from "forge-std/Test.sol";
import {Achievements} from "../src/Achievements.sol";
import {Ownable} from "@openzeppelin/contracts/access/Ownable.sol";

/// @dev Tests UNITARIOS de Achievements, aislados de GameStore. El propio contrato
///      de test actúa como `minter` autorizado para poder acuñar directamente.
contract AchievementsTest is Test {
    Achievements internal ach;

    address internal owner = makeAddr("owner");
    address internal player = makeAddr("player");
    address internal other = makeAddr("other");

    uint256 constant ARQUERO = 0;
    uint256 constant MERCADER = 1;
    uint256 constant COLECCIONISTA = 2;

    function setUp() public {
        vm.prank(owner);
        ach = new Achievements("ipfs://medals/{id}.json");
        // El minter autorizado es este contrato de test (así llamamos a mint
        // directamente, sin pasar por GameStore).
        vm.prank(owner);
        ach.setMinter(address(this));
    }

    // ─────────────────────── Control de acceso ───────────────────────

    function test_OnlyOwnerCanSetMinter() public {
        vm.prank(other);
        vm.expectRevert(abi.encodeWithSelector(Ownable.OwnableUnauthorizedAccount.selector, other));
        ach.setMinter(other);
    }

    function test_RevertWhen_NonMinterMints() public {
        vm.prank(other);
        vm.expectRevert(Achievements.NotMinter.selector);
        ach.mintArquero(player);
    }

    function test_MinterCanMint() public {
        ach.mintArquero(player); // msg.sender = este contrato = minter
        assertEq(ach.balanceOf(player, ARQUERO), 1);
    }

    function test_RevertWhen_MintingTwice() public {
        ach.mintArquero(player);
        vm.expectRevert(abi.encodeWithSelector(Achievements.AlreadyUnlocked.selector, player, ARQUERO));
        ach.mintArquero(player);
    }

    // ─────────────────────────── Soulbound ───────────────────────────

    function test_ArqueroIsSoulbound() public {
        ach.mintArquero(player);
        vm.prank(player);
        vm.expectRevert(abi.encodeWithSelector(Achievements.Soulbound.selector, ARQUERO));
        ach.safeTransferFrom(player, other, ARQUERO, 1, "");
    }

    function test_ColeccionistaIsSoulbound() public {
        ach.mintColeccionista(player);
        vm.prank(player);
        vm.expectRevert(abi.encodeWithSelector(Achievements.Soulbound.selector, COLECCIONISTA));
        ach.safeTransferFrom(player, other, COLECCIONISTA, 1, "");
    }

    function test_MercaderIsTransferable() public {
        ach.mintMercader(player);
        assertEq(ach.balanceOf(player, MERCADER), 1);

        vm.prank(player);
        ach.safeTransferFrom(player, other, MERCADER, 1, "");

        assertEq(ach.balanceOf(player, MERCADER), 0);
        assertEq(ach.balanceOf(other, MERCADER), 1);
    }

    // ─────────────────────────── Rareza ──────────────────────────────

    function test_MercaderHasRarity() public {
        ach.mintMercader(player);
        uint8 rarity = uint8(ach.mercaderRarity(player));
        // Bronce(1) / Plata(2) / Oro(3); nunca None(0) tras acuñar.
        assertTrue(rarity >= 1 && rarity <= 3);
    }
}
