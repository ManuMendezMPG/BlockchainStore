// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {ERC1155} from "@openzeppelin/contracts/token/ERC1155/ERC1155.sol";
import {ERC1155Burnable} from "@openzeppelin/contracts/token/ERC1155/extensions/ERC1155Burnable.sol";
import {Ownable} from "@openzeppelin/contracts/access/Ownable.sol";
import {ReentrancyGuard} from "@openzeppelin/contracts/utils/ReentrancyGuard.sol";

/// @notice Interfaz mínima de Achievements que GameStore necesita para acuñar
///         medallones. Usamos una interfaz (no el contrato entero) para
///         desacoplar: GameStore solo conoce las funciones que llama.
interface IAchievements {
    function isUnlocked(address account, uint256 id) external view returns (bool);
    function hasArquero(address account) external view returns (bool);
    function mintArquero(address to) external;
    function mintMercader(address to) external;
    function mintColeccionista(address to) external;
}

/// @title GameStore — tienda de items del juego (ERC-1155) con dependencias,
///        compras acumuladas, reembolso de excedente y logros conectados.
/// @dev Modelo CERRADO: el contrato gestiona PROPIEDAD, ECONOMÍA y DEPENDENCIAS.
///      Las reglas de slots (mochila de 6) y el USO (gastar/beber/romper) son de
///      sesión y viven en Unreal, no aquí.
contract GameStore is ERC1155, ERC1155Burnable, Ownable, ReentrancyGuard {
    // ─────────────────────────── Catálogo (IDs) ──────────────────────
    uint256 public constant ESPADA = 0;
    uint256 public constant ESCUDO = 1;
    uint256 public constant ARCO = 2;
    uint256 public constant CARCAJ_5 = 3;
    uint256 public constant CARCAJ_10 = 4;
    uint256 public constant CARCAJ_20 = 5;
    uint256 public constant FLECHA = 6;
    uint256 public constant BOTELLA_VACIA = 7;
    uint256 public constant POCION_VIDA = 8;
    uint256 public constant POCION_MANA = 9;

    // IDs de los medallones en el contrato Achievements (deben coincidir).
    uint256 public constant ACH_ARQUERO = 0;
    uint256 public constant ACH_MERCADER = 1;
    uint256 public constant ACH_COLECCIONISTA = 2;

    // ───────────────────────── Umbrales de logros ────────────────────
    /// @notice Flechas compradas (históricas) para desbloquear ARQUERO.
    uint256 public constant ARQUERO_ARROWS = 20;
    /// @notice Gasto acumulado en la tienda para desbloquear MERCADER.
    uint256 public constant MERCADER_SPEND_THRESHOLD = 0.1 ether;

    // ─────────────────────────── Errores ─────────────────────────────
    error ItemNotListed(uint256 itemId);
    error InsufficientPayment(uint256 required, uint256 sent);
    error InvalidQuantity();
    error NoFundsToWithdraw();
    error WithdrawFailed();
    error RefundFailed();
    // Dependencias:
    error NeedBow(); // comprar flecha sin arco
    error QuiverCapacityExceeded(uint256 capacity, uint256 current, uint256 requested);
    error NeedEmptyBottle(uint256 required, uint256 have); // poción sin botellas
    error NeedQuiver5(); // carcaj_10 sin carcaj_5
    error NeedArcheroAchievement(); // carcaj_20 sin logro ARQUERO

    // ─────────────────────────── Eventos ─────────────────────────────
    event ItemListed(uint256 indexed itemId, uint256 price);
    event ItemPurchased(address indexed buyer, uint256 indexed itemId, uint256 quantity, uint256 amountPaid);
    event ExcessRefunded(address indexed buyer, uint256 amount);
    event FundsWithdrawn(address indexed to, uint256 amount);
    event AchievementsContractSet(address indexed achievements);

    // ─────────────────────────── Estado ──────────────────────────────
    mapping(uint256 itemId => uint256 price) public priceOf;
    mapping(uint256 itemId => bool listed) public isListed;

    /// @notice Compras ACUMULADAS por jugador y por item (solo incrementa).
    ///         Base para los logros (p. ej. flechas históricas).
    mapping(address player => mapping(uint256 itemId => uint256 total)) public purchasedTotal;
    /// @notice Gasto ACUMULADO por jugador (en wei, solo el coste real, sin excedente).
    mapping(address player => uint256 spent) public totalSpent;

    /// @notice Contrato de logros conectado. Si es address(0), la lógica de logros
    ///         se desactiva (permite usar GameStore de forma aislada / en tests).
    IAchievements public achievements;

    constructor(string memory uri_) ERC1155(uri_) Ownable(msg.sender) {}

    // ──────────────────────── Admin (owner) ──────────────────────────
    function setItem(uint256 itemId, uint256 price) external onlyOwner {
        priceOf[itemId] = price;
        isListed[itemId] = true;
        emit ItemListed(itemId, price);
    }

    /// @notice Conecta el contrato de logros. GameStore debe ser su `minter`.
    function setAchievements(address achievements_) external onlyOwner {
        achievements = IAchievements(achievements_);
        emit AchievementsContractSet(achievements_);
    }

    // ─────────────────────────── Compra ──────────────────────────────
    /// @notice Compra `quantity` unidades de `itemId` pagando ETH.
    /// @dev Sigue checks-effects-interactions y usa `nonReentrant`:
    ///      1) CHECKS: existencia, cantidad, pago, dependencias.
    ///      2) EFFECTS: quema de botellas (pociones), mint, contadores.
    ///      3) INTERACTIONS: acuñar logros y reembolsar el excedente (al final).
    function buy(uint256 itemId, uint256 quantity) external payable nonReentrant {
        // ── 1) CHECKS ───────────────────────────────────────────────
        if (!isListed[itemId]) revert ItemNotListed(itemId);
        if (quantity == 0) revert InvalidQuantity();

        uint256 cost = priceOf[itemId] * quantity;
        if (msg.value < cost) revert InsufficientPayment(cost, msg.value);

        _checkDependencies(itemId, quantity);

        // ── 2) EFFECTS ──────────────────────────────────────────────
        // Las pociones CONSUMEN botellas vacías (1 por poción).
        if (itemId == POCION_VIDA || itemId == POCION_MANA) {
            _burn(msg.sender, BOTELLA_VACIA, quantity);
        }

        _mint(msg.sender, itemId, quantity, "");
        purchasedTotal[msg.sender][itemId] += quantity;
        totalSpent[msg.sender] += cost;

        emit ItemPurchased(msg.sender, itemId, quantity, msg.value);

        // ── 3) INTERACTIONS ─────────────────────────────────────────
        // (a) Acuñar logros si se cumplen hitos (llamada a otro contrato).
        _checkAchievements(msg.sender);

        // (b) Reembolsar el excedente al comprador (patrón call seguro).
        uint256 excess = msg.value - cost;
        if (excess > 0) {
            (bool ok,) = payable(msg.sender).call{value: excess}("");
            if (!ok) revert RefundFailed();
            emit ExcessRefunded(msg.sender, excess);
        }
    }

    // ─────────────────────── Reglas de dependencia ───────────────────
    function _checkDependencies(uint256 itemId, uint256 quantity) internal view {
        if (itemId == FLECHA) {
            // Requiere arco y no superar la capacidad del carcaj poseído.
            if (balanceOf(msg.sender, ARCO) == 0) revert NeedBow();
            uint256 cap = quiverCapacity(msg.sender);
            uint256 current = balanceOf(msg.sender, FLECHA);
            if (current + quantity > cap) {
                revert QuiverCapacityExceeded(cap, current, quantity);
            }
        } else if (itemId == CARCAJ_10) {
            // Solo si ya posee carcaj_5.
            if (balanceOf(msg.sender, CARCAJ_5) == 0) revert NeedQuiver5();
        } else if (itemId == CARCAJ_20) {
            // Solo si ha desbloqueado el logro ARQUERO.
            if (address(achievements) == address(0) || !achievements.hasArquero(msg.sender)) {
                revert NeedArcheroAchievement();
            }
        } else if (itemId == POCION_VIDA || itemId == POCION_MANA) {
            // Requiere (y consumirá) una botella vacía por poción.
            uint256 have = balanceOf(msg.sender, BOTELLA_VACIA);
            if (have < quantity) revert NeedEmptyBottle(quantity, have);
        }
    }

    /// @notice Capacidad de flechas según el mayor carcaj que posea el jugador.
    function quiverCapacity(address account) public view returns (uint256) {
        if (balanceOf(account, CARCAJ_20) > 0) return 20;
        if (balanceOf(account, CARCAJ_10) > 0) return 10;
        if (balanceOf(account, CARCAJ_5) > 0) return 5;
        return 0;
    }

    // ───────────────────────── Logros (hitos) ────────────────────────
    /// @dev Comprueba cada hito y, si se cumple y NO está ya desbloqueado, pide
    ///      a Achievements que acuñe. Es CLAVE comprobar `!isUnlocked` aquí: si
    ///      llamáramos a un mint ya hecho, Achievements revertiría y tumbaría
    ///      toda la compra.
    function _checkAchievements(address buyer) internal {
        if (address(achievements) == address(0)) return;

        // ARQUERO: 20 flechas compradas históricas.
        if (purchasedTotal[buyer][FLECHA] >= ARQUERO_ARROWS && !achievements.isUnlocked(buyer, ACH_ARQUERO)) {
            achievements.mintArquero(buyer);
        }
        // MERCADER: gasto acumulado por encima del umbral.
        if (totalSpent[buyer] > MERCADER_SPEND_THRESHOLD && !achievements.isUnlocked(buyer, ACH_MERCADER)) {
            achievements.mintMercader(buyer);
        }
        // COLECCIONISTA: set completo (espada + escudo + arco + algún carcaj).
        if (_hasFullSet(buyer) && !achievements.isUnlocked(buyer, ACH_COLECCIONISTA)) {
            achievements.mintColeccionista(buyer);
        }
    }

    function _hasFullSet(address account) internal view returns (bool) {
        return balanceOf(account, ESPADA) > 0 && balanceOf(account, ESCUDO) > 0 && balanceOf(account, ARCO) > 0
            && quiverCapacity(account) > 0;
    }

    // ─────────────────────── Retirada (owner) ────────────────────────
    function withdraw() external onlyOwner {
        uint256 balance = address(this).balance;
        if (balance == 0) revert NoFundsToWithdraw();

        (bool ok,) = payable(owner()).call{value: balance}("");
        if (!ok) revert WithdrawFailed();

        emit FundsWithdrawn(owner(), balance);
    }
}
