// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {ERC1155} from "@openzeppelin/contracts/token/ERC1155/ERC1155.sol";
import {ERC1155Burnable} from "@openzeppelin/contracts/token/ERC1155/extensions/ERC1155Burnable.sol";
import {Ownable} from "@openzeppelin/contracts/access/Ownable.sol";

/// @title GameStore — tienda de items de un juego como tokens ERC-1155.
/// @notice Catálogo de items con precio en wei. El jugador compra pagando ETH y
///         puede quemar sus items para vaciar inventario. El owner gestiona el
///         catálogo y retira lo recaudado.
/// @dev Hereda de:
///      - ERC1155: estándar multi-token. Cada `itemId` es un tipo de item y el
///        balance del token = unidades que posee una cuenta. Un solo contrato
///        gestiona todos los items (más barato que un contrato por item).
///      - ERC1155Burnable: añade `burn`/`burnBatch` con control de permisos
///        (solo el dueño de los tokens o un operador aprobado puede quemarlos).
///      - Ownable: control de acceso simple (un único owner) para las funciones
///        administrativas (`setItem`, `withdraw`).
contract GameStore is ERC1155, ERC1155Burnable, Ownable {
    // ─────────────────────────── Errores ─────────────────────────────
    // Custom errors en vez de require(string): más baratos en gas y permiten
    // adjuntar datos del fallo para depurar / mostrar en la UI del juego.

    /// @notice El item no está dado de alta en el catálogo.
    error ItemNotListed(uint256 itemId);
    /// @notice El ETH enviado no cubre el coste total (precio * cantidad).
    error InsufficientPayment(uint256 required, uint256 sent);
    /// @notice La cantidad a comprar debe ser mayor que cero.
    error InvalidQuantity();
    /// @notice No hay fondos en el contrato que retirar.
    error NoFundsToWithdraw();
    /// @notice La transferencia de ETH al owner falló.
    error WithdrawFailed();

    // ─────────────────────────── Eventos ─────────────────────────────

    /// @notice Emitido cuando el owner da de alta o actualiza el precio de un item.
    event ItemListed(uint256 indexed itemId, uint256 price);
    /// @notice Emitido en cada compra. `amountPaid` es el ETH realmente enviado.
    event ItemPurchased(address indexed buyer, uint256 indexed itemId, uint256 quantity, uint256 amountPaid);
    /// @notice Emitido cuando el owner retira los fondos recaudados.
    event FundsWithdrawn(address indexed to, uint256 amount);

    // ─────────────────────────── Estado ──────────────────────────────

    /// @notice Precio en wei de cada item (por unidad).
    mapping(uint256 itemId => uint256 price) public priceOf;
    /// @notice Marca si un item existe en el catálogo. Se separa del precio para
    ///         poder distinguir "item gratis (precio 0)" de "item inexistente".
    mapping(uint256 itemId => bool listed) public isListed;

    /// @param uri_ URI base de metadatos ERC-1155 (puede incluir el patrón `{id}`).
    /// @dev `Ownable(msg.sender)` fija como owner a quien despliega el contrato.
    constructor(string memory uri_) ERC1155(uri_) Ownable(msg.sender) {}

    // ──────────────────────── Catálogo (owner) ───────────────────────

    /// @notice Da de alta un item o actualiza su precio.
    /// @dev Solo el owner. Permite precio 0 (items gratuitos): la existencia se
    ///      controla con `isListed`, no con el precio.
    function setItem(uint256 itemId, uint256 price) external onlyOwner {
        priceOf[itemId] = price;
        isListed[itemId] = true;
        emit ItemListed(itemId, price);
    }

    // ─────────────────────────── Compra ──────────────────────────────

    /// @notice Compra `quantity` unidades del item `itemId` pagando ETH.
    /// @dev Comprueba existencia, cantidad y pago suficiente; luego mintea.
    ///      El exceso de pago (si lo hay) NO se reembolsa: queda recaudado.
    function buy(uint256 itemId, uint256 quantity) external payable {
        if (!isListed[itemId]) revert ItemNotListed(itemId);
        if (quantity == 0) revert InvalidQuantity();

        uint256 cost = priceOf[itemId] * quantity;
        if (msg.value < cost) revert InsufficientPayment(cost, msg.value);

        // _mint crea `quantity` unidades del token `itemId` para el comprador.
        // El último argumento (data) se reenvía a los hooks ERC-1155; vacío aquí.
        _mint(msg.sender, itemId, quantity, "");

        emit ItemPurchased(msg.sender, itemId, quantity, msg.value);
    }

    // ─────────────────────── Retirada (owner) ────────────────────────

    /// @notice Retira todo el ETH recaudado al owner.
    /// @dev Usa `call` (recomendado frente a `transfer`) y revierte si falla.
    function withdraw() external onlyOwner {
        uint256 balance = address(this).balance;
        if (balance == 0) revert NoFundsToWithdraw();

        (bool ok,) = payable(owner()).call{value: balance}("");
        if (!ok) revert WithdrawFailed();

        emit FundsWithdrawn(owner(), balance);
    }

    // Nota: `burn` y `burnBatch` se heredan de ERC1155Burnable. El jugador llama
    // a `burn(suDireccion, itemId, cantidad)` para vaciar su inventario; la
    // extensión exige que sea el dueño de los tokens o un operador aprobado.
}
