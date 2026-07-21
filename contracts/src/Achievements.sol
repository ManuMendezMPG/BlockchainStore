// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {ERC1155} from "@openzeppelin/contracts/token/ERC1155/ERC1155.sol";
import {Ownable} from "@openzeppelin/contracts/access/Ownable.sol";

/// @title Achievements — achievement medallions (ERC-1155) minted by GameStore.
/// @notice Three medallions:
///   - ARQUERO (soulbound): for buying 20 historical arrows. Unlocks carcaj_20.
///   - MERCADER (transferable): for exceeding an accumulated spend; pseudo-random rarity.
///   - COLECCIONISTA (soulbound): for owning the full set.
/// @dev Only the authorized `minter` (will be GameStore) can mint.
contract Achievements is ERC1155, Ownable {
    // ─────────────────────────── IDs ─────────────────────────────────
    uint256 public constant ARQUERO = 0;
    uint256 public constant MERCADER = 1;
    uint256 public constant COLECCIONISTA = 2;

    /// @notice MERCADER rarity. None is only the default value (no medallion).
    enum Rarity {
        None,
        Bronce,
        Plata,
        Oro
    }

    // ─────────────────────────── State ───────────────────────────────
    /// @notice Address authorized to mint (will be GameStore). Set by the owner.
    address public minter;
    /// @notice MERCADER rarity per player (set at mint time).
    mapping(address account => Rarity rarity) public mercaderRarity;
    /// @dev Counter to vary the pseudo-randomness input between calls.
    uint256 private _nonce;

    // ─────────────────────────── Errors ──────────────────────────────
    error NotMinter();
    error AlreadyUnlocked(address account, uint256 id);
    error Soulbound(uint256 id);

    // ─────────────────────────── Events ──────────────────────────────
    event MinterUpdated(address indexed minter);
    /// @notice Milestone unlocked. `rarity` is only relevant for MERCADER (0 otherwise).
    event AchievementUnlocked(address indexed player, uint256 indexed medallionId, uint8 rarity);

    constructor(string memory uri_) ERC1155(uri_) Ownable(msg.sender) {}

    // ──────────────────────── Access control ─────────────────────────
    modifier onlyMinter() {
        if (msg.sender != minter) revert NotMinter();
        _;
    }

    /// @notice Authorizes which address can mint (will be called with GameStore).
    function setMinter(address minter_) external onlyOwner {
        minter = minter_;
        emit MinterUpdated(minter_);
    }

    // ──────────────────────────── Views ──────────────────────────────
    function isUnlocked(address account, uint256 id) public view returns (bool) {
        return balanceOf(account, id) > 0;
    }

    function hasArquero(address account) external view returns (bool) {
        return balanceOf(account, ARQUERO) > 0;
    }

    // ─────────────────────────── Minting ─────────────────────────────
    // Each function checks that the achievement is not already unlocked (it is
    // not minted twice) and can only be called by the `minter`.

    function mintArquero(address to) external onlyMinter {
        if (isUnlocked(to, ARQUERO)) revert AlreadyUnlocked(to, ARQUERO);
        _mint(to, ARQUERO, 1, "");
        emit AchievementUnlocked(to, ARQUERO, 0);
    }

    function mintColeccionista(address to) external onlyMinter {
        if (isUnlocked(to, COLECCIONISTA)) revert AlreadyUnlocked(to, COLECCIONISTA);
        _mint(to, COLECCIONISTA, 1, "");
        emit AchievementUnlocked(to, COLECCIONISTA, 0);
    }

    function mintMercader(address to) external onlyMinter {
        if (isUnlocked(to, MERCADER)) revert AlreadyUnlocked(to, MERCADER);
        Rarity r = _rollRarity(to);
        mercaderRarity[to] = r;
        _mint(to, MERCADER, 1, "");
        emit AchievementUnlocked(to, MERCADER, uint8(r));
    }

    /// @dev Pseudo-random rarity with weights: Bronce 60%, Plata 30%, Oro 10%.
    ///
    /// ⚠️⚠️ SECURITY WARNING ⚠️⚠️
    /// This is NOT production-safe randomness. `block.timestamp`,
    /// `block.prevrandao` and `blockhash` are known/observable and, to some
    /// degree, INFLUENCEABLE by the block proposer (validator). An actor with an
    /// economic incentive could:
    ///   - Compute the result BEFORE sending the transaction (everything is public)
    ///     and only send it when they'd get "Oro" (especially from a contract).
    ///   - Reorder/delay inclusion to bias the result.
    /// In production a verifiable RANDOMNESS ORACLE is used, e.g. Chainlink VRF,
    /// which delivers a random number with a cryptographic proof in a second
    /// transaction (callback). Here we keep it simple for teaching purposes.
    function _rollRarity(address to) internal returns (Rarity) {
        uint256 rand = uint256(
            keccak256(
                abi.encodePacked(
                    blockhash(block.number - 1),
                    block.timestamp,
                    block.prevrandao,
                    to,
                    _nonce++
                )
            )
        ) % 100;

        if (rand < 60) return Rarity.Bronce; // 60%
        if (rand < 90) return Rarity.Plata; //  30%
        return Rarity.Oro; //                    10%
    }

    // ───────────────────────────── Soulbound ─────────────────────────
    /// @dev `_update` is the central transfer hook in ERC-1155 (OZ v5):
    ///      mint => from == address(0); burn => to == address(0); real transfer
    ///      => both non-zero. We block ONLY the real transfers of ARQUERO and
    ///      COLECCIONISTA; mint and burn are still allowed, and MERCADER behaves
    ///      like a normal ERC-1155 (transferable).
    function _update(address from, address to, uint256[] memory ids, uint256[] memory values) internal override {
        if (from != address(0) && to != address(0)) {
            for (uint256 i = 0; i < ids.length; i++) {
                if (ids[i] == ARQUERO || ids[i] == COLECCIONISTA) {
                    revert Soulbound(ids[i]);
                }
            }
        }
        super._update(from, to, ids, values);
    }
}
