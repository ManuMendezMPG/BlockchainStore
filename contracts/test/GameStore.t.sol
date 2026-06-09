// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Test} from "forge-std/Test.sol";
import {GameStore} from "../src/GameStore.sol";
import {Ownable} from "@openzeppelin/contracts/access/Ownable.sol";
import {IERC1155Errors} from "@openzeppelin/contracts/interfaces/draft-IERC6093.sol";

/// @dev Tests escritos primero (TDD). Definen el contrato esperado de GameStore
///      antes de implementarlo. Cubren: alta de items, compra (pago correcto,
///      insuficiente y de más), balanceOf, quema, y control de acceso del owner.
contract GameStoreTest is Test {
    GameStore internal store;

    // Cuentas de prueba (EOAs deterministas creadas por forge-std).
    address internal owner = makeAddr("owner");
    address internal player = makeAddr("player");
    address internal attacker = makeAddr("attacker");

    // Item de ejemplo: una "espada" con id 1 a 0.05 ETH la unidad.
    uint256 internal constant ITEM_SWORD = 1;
    uint256 internal constant PRICE = 0.05 ether;

    // Re-declaramos el evento para poder usarlo con vm.expectEmit.
    event ItemPurchased(address indexed buyer, uint256 indexed itemId, uint256 quantity, uint256 amountPaid);

    function setUp() public {
        // El que despliega es el owner: pranqueamos para que msg.sender sea `owner`.
        vm.prank(owner);
        store = new GameStore("ipfs://game-items/{id}.json");

        // Damos saldo al jugador para que pueda pagar las compras.
        vm.deal(player, 100 ether);
    }

    // Helper: da de alta el item de ejemplo como owner.
    function _listSword() internal {
        vm.prank(owner);
        store.setItem(ITEM_SWORD, PRICE);
    }

    // ───────────────────────── Alta de items ─────────────────────────

    function test_OwnerCanListItem() public {
        _listSword();
        assertTrue(store.isListed(ITEM_SWORD), "item deberia estar listado");
        assertEq(store.priceOf(ITEM_SWORD), PRICE, "precio incorrecto");
    }

    function test_RevertWhen_NonOwnerListsItem() public {
        vm.prank(attacker);
        vm.expectRevert(abi.encodeWithSelector(Ownable.OwnableUnauthorizedAccount.selector, attacker));
        store.setItem(ITEM_SWORD, PRICE);
    }

    // ─────────────────────────── Compra ──────────────────────────────

    function test_BuyWithExactPaymentMintsTokens() public {
        _listSword();
        uint256 qty = 3;
        uint256 cost = PRICE * qty;

        vm.prank(player);
        store.buy{value: cost}(ITEM_SWORD, qty);

        // balanceOf tras la compra debe reflejar la cantidad comprada.
        assertEq(store.balanceOf(player, ITEM_SWORD), qty, "balance del comprador incorrecto");
        // El ETH pagado queda en el contrato hasta que el owner lo retire.
        assertEq(address(store).balance, cost, "el contrato deberia retener el pago");
    }

    function test_BuyEmitsItemPurchasedEvent() public {
        _listSword();
        uint256 qty = 2;
        uint256 cost = PRICE * qty;

        // Comprobamos topics indexados + data del evento.
        vm.expectEmit(true, true, false, true, address(store));
        emit ItemPurchased(player, ITEM_SWORD, qty, cost);

        vm.prank(player);
        store.buy{value: cost}(ITEM_SWORD, qty);
    }

    function test_BuyWithOverpaymentKeepsFunds() public {
        _listSword();
        uint256 qty = 1;
        uint256 paid = PRICE + 1 ether; // paga de más

        vm.prank(player);
        store.buy{value: paid}(ITEM_SWORD, qty);

        assertEq(store.balanceOf(player, ITEM_SWORD), qty);
        // Diseño: no se reembolsa el exceso; el contrato lo retiene.
        assertEq(address(store).balance, paid);
    }

    function test_RevertWhen_Underpaying() public {
        _listSword();
        uint256 qty = 2;
        uint256 cost = PRICE * qty;
        uint256 sent = cost - 1; // un wei de menos

        vm.prank(player);
        vm.expectRevert(abi.encodeWithSelector(GameStore.InsufficientPayment.selector, cost, sent));
        store.buy{value: sent}(ITEM_SWORD, qty);
    }

    function test_RevertWhen_BuyingUnlistedItem() public {
        uint256 unknownItem = 999;
        vm.prank(player);
        vm.expectRevert(abi.encodeWithSelector(GameStore.ItemNotListed.selector, unknownItem));
        store.buy{value: 1 ether}(unknownItem, 1);
    }

    function test_RevertWhen_QuantityIsZero() public {
        _listSword();
        vm.prank(player);
        vm.expectRevert(GameStore.InvalidQuantity.selector);
        store.buy{value: 0}(ITEM_SWORD, 0);
    }

    // ─────────────────────────── Quema ───────────────────────────────

    function test_PlayerCanBurnOwnItems() public {
        _listSword();
        uint256 qty = 5;
        uint256 cost = PRICE * qty;

        vm.prank(player);
        store.buy{value: cost}(ITEM_SWORD, qty);
        assertEq(store.balanceOf(player, ITEM_SWORD), qty);

        // El jugador vacía su inventario quemando sus propios tokens.
        vm.prank(player);
        store.burn(player, ITEM_SWORD, qty);
        assertEq(store.balanceOf(player, ITEM_SWORD), 0, "el inventario deberia quedar vacio");
    }

    function test_RevertWhen_BurningTokensOfAnother() public {
        _listSword();
        vm.prank(player);
        store.buy{value: PRICE}(ITEM_SWORD, 1);

        // Un tercero sin aprobación no puede quemar los tokens del jugador.
        vm.prank(attacker);
        vm.expectRevert(
            abi.encodeWithSelector(IERC1155Errors.ERC1155MissingApprovalForAll.selector, attacker, player)
        );
        store.burn(player, ITEM_SWORD, 1);
    }

    // ─────────────────────── Retirada de fondos ──────────────────────

    function test_OwnerCanWithdrawProceeds() public {
        _listSword();
        uint256 qty = 4;
        uint256 cost = PRICE * qty;

        vm.prank(player);
        store.buy{value: cost}(ITEM_SWORD, qty);

        uint256 ownerBalanceBefore = owner.balance;

        vm.prank(owner);
        store.withdraw();

        assertEq(address(store).balance, 0, "el contrato deberia quedar a cero");
        assertEq(owner.balance, ownerBalanceBefore + cost, "el owner deberia recibir lo recaudado");
    }

    function test_RevertWhen_NonOwnerWithdraws() public {
        _listSword();
        vm.prank(player);
        store.buy{value: PRICE}(ITEM_SWORD, 1);

        vm.prank(attacker);
        vm.expectRevert(abi.encodeWithSelector(Ownable.OwnableUnauthorizedAccount.selector, attacker));
        store.withdraw();
    }

    function test_RevertWhen_WithdrawingWithNoFunds() public {
        vm.prank(owner);
        vm.expectRevert(GameStore.NoFundsToWithdraw.selector);
        store.withdraw();
    }
}
