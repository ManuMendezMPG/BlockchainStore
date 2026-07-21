// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {ERC1155} from "@openzeppelin/contracts/token/ERC1155/ERC1155.sol";
import {ERC1155Burnable} from "@openzeppelin/contracts/token/ERC1155/extensions/ERC1155Burnable.sol";
import {Ownable} from "@openzeppelin/contracts/access/Ownable.sol";
import {ReentrancyGuard} from "@openzeppelin/contracts/utils/ReentrancyGuard.sol";

/// @notice Minimal Achievements interface that GameStore needs to mint
///         medallions. We use an interface (not the whole contract) to
///         decouple: GameStore only knows the functions it calls.
interface IAchievements {
    function isUnlocked(address account, uint256 id) external view returns (bool);
    function hasArquero(address account) external view returns (bool);
    function mintArquero(address to) external;
    function mintMercader(address to) external;
    function mintColeccionista(address to) external;
}

/// @title GameStore — in-game item store (ERC-1155) with dependencies,
///        accumulated purchases, excess refund and connected achievements.
/// @dev CLOSED model: the contract manages OWNERSHIP, ECONOMY and DEPENDENCIES.
///      The slot rules (6-slot backpack) and the USE (spend/drink/break) are
///      session-level and live in Unreal, not here.
contract GameStore is ERC1155, ERC1155Burnable, Ownable, ReentrancyGuard {
    // ─────────────────────────── Catalog (IDs) ───────────────────────
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

    // IDs of the medallions in the Achievements contract (must match).
    uint256 public constant ACH_ARQUERO = 0;
    uint256 public constant ACH_MERCADER = 1;
    uint256 public constant ACH_COLECCIONISTA = 2;

    // ───────────────────────── Achievement thresholds ────────────────
    /// @notice Arrows purchased (historical) to unlock ARQUERO.
    uint256 public constant ARQUERO_ARROWS = 20;
    /// @notice Accumulated spend in the store to unlock MERCADER.
    uint256 public constant MERCADER_SPEND_THRESHOLD = 0.1 ether;

    // ─────────────────────────── Errors ──────────────────────────────
    error ItemNotListed(uint256 itemId);
    error InsufficientPayment(uint256 required, uint256 sent);
    error InvalidQuantity();
    error NoFundsToWithdraw();
    error WithdrawFailed();
    error RefundFailed();
    // Dependencies:
    error NeedBow(); // buying an arrow without a bow
    error NeedQuiver(); // buying an arrow without any quiver
    error NeedQuiver5(); // carcaj_10 without carcaj_5
    error NeedArcheroAchievement(); // carcaj_20 without the ARQUERO achievement

    // ─────────────────────────── Events ──────────────────────────────
    event ItemListed(uint256 indexed itemId, uint256 price);
    event ItemPurchased(address indexed buyer, uint256 indexed itemId, uint256 quantity, uint256 amountPaid);
    event ExcessRefunded(address indexed buyer, uint256 amount);
    event FundsWithdrawn(address indexed to, uint256 amount);
    event AchievementsContractSet(address indexed achievements);

    // ─────────────────────────── State ───────────────────────────────
    mapping(uint256 itemId => uint256 price) public priceOf;
    mapping(uint256 itemId => bool listed) public isListed;

    /// @notice ACCUMULATED purchases per player and per item (only increments).
    ///         Basis for the achievements (e.g. historical arrows).
    mapping(address player => mapping(uint256 itemId => uint256 total)) public purchasedTotal;
    /// @notice ACCUMULATED spend per player (in wei, only the real cost, no excess).
    mapping(address player => uint256 spent) public totalSpent;

    /// @notice Connected achievements contract. If it is address(0), the achievement
    ///         logic is disabled (allows using GameStore in isolation / in tests).
    IAchievements public achievements;

    constructor(string memory uri_) ERC1155(uri_) Ownable(msg.sender) {}

    // ──────────────────────── Admin (owner) ──────────────────────────
    function setItem(uint256 itemId, uint256 price) external onlyOwner {
        priceOf[itemId] = price;
        isListed[itemId] = true;
        emit ItemListed(itemId, price);
    }

    /// @notice Connects the achievements contract. GameStore must be its `minter`.
    function setAchievements(address achievements_) external onlyOwner {
        achievements = IAchievements(achievements_);
        emit AchievementsContractSet(achievements_);
    }

    // ─────────────────────────── Purchase ────────────────────────────
    /// @notice Buys `quantity` units of `itemId` paying with ETH.
    /// @dev Follows checks-effects-interactions and uses `nonReentrant`:
    ///      1) CHECKS: existence, quantity, payment, dependencies.
    ///      2) EFFECTS: mint, counters.
    ///      3) INTERACTIONS: mint achievements and refund the excess (at the end).
    function buy(uint256 itemId, uint256 quantity) external payable nonReentrant {
        // ── 1) CHECKS ───────────────────────────────────────────────
        if (!isListed[itemId]) revert ItemNotListed(itemId);
        if (quantity == 0) revert InvalidQuantity();

        uint256 cost = priceOf[itemId] * quantity;
        if (msg.value < cost) revert InsufficientPayment(cost, msg.value);

        _checkDependencies(itemId);

        // ── 2) EFFECTS ──────────────────────────────────────────────
        _mint(msg.sender, itemId, quantity, "");
        purchasedTotal[msg.sender][itemId] += quantity;
        totalSpent[msg.sender] += cost;

        emit ItemPurchased(msg.sender, itemId, quantity, msg.value);

        // ── 3) INTERACTIONS ─────────────────────────────────────────
        // (a) Mint achievements if milestones are met (call to another contract).
        _checkAchievements(msg.sender);

        // (b) Refund the excess to the buyer (safe call pattern).
        uint256 excess = msg.value - cost;
        if (excess > 0) {
            (bool ok,) = payable(msg.sender).call{value: excess}("");
            if (!ok) revert RefundFailed();
            emit ExcessRefunded(msg.sender, excess);
        }
    }

    // ─────────────────────── Dependency rules ────────────────────────
    function _checkDependencies(uint256 itemId) internal view {
        if (itemId == FLECHA) {
            // Requires a bow and owning at least one quiver. The CAPACITY (how many
            // arrows fit) is NO longer validated on-chain: the player shoots arrows
            // in Unreal (local use), so the limit is checked there against the
            // savegame. Here we only require the needed gear: bow + quiver.
            if (balanceOf(msg.sender, ARCO) == 0) revert NeedBow();
            if (quiverCapacity(msg.sender) == 0) revert NeedQuiver();
        } else if (itemId == CARCAJ_10) {
            // Only if it already owns carcaj_5.
            if (balanceOf(msg.sender, CARCAJ_5) == 0) revert NeedQuiver5();
        } else if (itemId == CARCAJ_20) {
            // Only if it has unlocked the ARQUERO achievement.
            if (address(achievements) == address(0) || !achievements.hasArquero(msg.sender)) {
                revert NeedArcheroAchievement();
            }
        }
        // Note: potions (POCION_VIDA / POCION_MANA) NO longer have an on-chain
        // empty-bottle dependency. The rule "you need an empty bottle to get a
        // potion" is now validated in the game layer (Unreal), against the
        // savegame inventory.
    }

    /// @notice Arrow capacity based on the largest quiver the player owns.
    function quiverCapacity(address account) public view returns (uint256) {
        if (balanceOf(account, CARCAJ_20) > 0) return 20;
        if (balanceOf(account, CARCAJ_10) > 0) return 10;
        if (balanceOf(account, CARCAJ_5) > 0) return 5;
        return 0;
    }

    // ───────────────────────── Achievements (milestones) ─────────────
    /// @dev Checks each milestone and, if met and NOT already unlocked, asks
    ///      Achievements to mint. It is KEY to check `!isUnlocked` here: if we
    ///      called a mint that was already done, Achievements would revert and
    ///      bring down the whole purchase.
    function _checkAchievements(address buyer) internal {
        if (address(achievements) == address(0)) return;

        // ARQUERO: 20 historical arrows purchased.
        if (purchasedTotal[buyer][FLECHA] >= ARQUERO_ARROWS && !achievements.isUnlocked(buyer, ACH_ARQUERO)) {
            achievements.mintArquero(buyer);
        }
        // MERCADER: accumulated spend above the threshold.
        if (totalSpent[buyer] > MERCADER_SPEND_THRESHOLD && !achievements.isUnlocked(buyer, ACH_MERCADER)) {
            achievements.mintMercader(buyer);
        }
        // COLECCIONISTA: full set (sword + shield + bow + some quiver).
        if (_hasFullSet(buyer) && !achievements.isUnlocked(buyer, ACH_COLECCIONISTA)) {
            achievements.mintColeccionista(buyer);
        }
    }

    function _hasFullSet(address account) internal view returns (bool) {
        return balanceOf(account, ESPADA) > 0 && balanceOf(account, ESCUDO) > 0 && balanceOf(account, ARCO) > 0
            && quiverCapacity(account) > 0;
    }

    // ─────────────────────── Withdrawal (owner) ──────────────────────
    function withdraw() external onlyOwner {
        uint256 balance = address(this).balance;
        if (balance == 0) revert NoFundsToWithdraw();

        (bool ok,) = payable(owner()).call{value: balance}("");
        if (!ok) revert WithdrawFailed();

        emit FundsWithdrawn(owner(), balance);
    }
}
