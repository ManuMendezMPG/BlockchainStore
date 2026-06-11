// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {ERC1155} from "@openzeppelin/contracts/token/ERC1155/ERC1155.sol";
import {Ownable} from "@openzeppelin/contracts/access/Ownable.sol";

/// @title Achievements — medallones de logros (ERC-1155) acuñados por GameStore.
/// @notice Tres medallones:
///   - ARQUERO (soulbound): por comprar 20 flechas históricas. Desbloquea carcaj_20.
///   - MERCADER (transferible): por superar un gasto acumulado; rareza pseudoaleatoria.
///   - COLECCIONISTA (soulbound): por poseer el set completo.
/// @dev Solo el `minter` autorizado (será GameStore) puede acuñar.
contract Achievements is ERC1155, Ownable {
    // ─────────────────────────── IDs ─────────────────────────────────
    uint256 public constant ARQUERO = 0;
    uint256 public constant MERCADER = 1;
    uint256 public constant COLECCIONISTA = 2;

    /// @notice Rareza del MERCADER. None solo es el valor por defecto (sin medallón).
    enum Rarity {
        None,
        Bronce,
        Plata,
        Oro
    }

    // ─────────────────────────── Estado ──────────────────────────────
    /// @notice Dirección autorizada a acuñar (será GameStore). La fija el owner.
    address public minter;
    /// @notice Rareza del MERCADER por jugador (se fija al acuñar).
    mapping(address account => Rarity rarity) public mercaderRarity;
    /// @dev Contador para variar la entrada de la pseudoaleatoriedad entre llamadas.
    uint256 private _nonce;

    // ─────────────────────────── Errores ─────────────────────────────
    error NotMinter();
    error AlreadyUnlocked(address account, uint256 id);
    error Soulbound(uint256 id);

    // ─────────────────────────── Eventos ─────────────────────────────
    event MinterUpdated(address indexed minter);
    /// @notice Hito desbloqueado. `rarity` solo es relevante para MERCADER (0 en el resto).
    event AchievementUnlocked(address indexed player, uint256 indexed medallionId, uint8 rarity);

    constructor(string memory uri_) ERC1155(uri_) Ownable(msg.sender) {}

    // ──────────────────────── Control de acceso ──────────────────────
    modifier onlyMinter() {
        if (msg.sender != minter) revert NotMinter();
        _;
    }

    /// @notice Autoriza qué dirección puede acuñar (se llamará con GameStore).
    function setMinter(address minter_) external onlyOwner {
        minter = minter_;
        emit MinterUpdated(minter_);
    }

    // ──────────────────────────── Vistas ─────────────────────────────
    function isUnlocked(address account, uint256 id) public view returns (bool) {
        return balanceOf(account, id) > 0;
    }

    function hasArquero(address account) external view returns (bool) {
        return balanceOf(account, ARQUERO) > 0;
    }

    // ─────────────────────────── Acuñación ───────────────────────────
    // Cada función comprueba que el logro no esté ya desbloqueado (no se acuña
    // dos veces) y solo la puede llamar el `minter`.

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

    /// @dev Rareza pseudoaleatoria con pesos: Bronce 60%, Plata 30%, Oro 10%.
    ///
    /// ⚠️⚠️ AVISO DE SEGURIDAD ⚠️⚠️
    /// Esto NO es aleatoriedad segura para producción. `block.timestamp`,
    /// `block.prevrandao` y `blockhash` son conocidos/observables y, en cierto
    /// grado, INFLUENCIABLES por el proponente del bloque (validador). Un actor
    /// con incentivo económico podría:
    ///   - Calcular el resultado ANTES de enviar la transacción (todo es público)
    ///     y solo enviarla cuando le toque "Oro" (especialmente desde un contrato).
    ///   - Reordenar/retrasar la inclusión para sesgar el resultado.
    /// En producción se usa un ORÁCULO DE ALEATORIEDAD verificable, p. ej.
    /// Chainlink VRF, que entrega un número aleatorio con prueba criptográfica en
    /// una segunda transacción (callback). Aquí lo dejamos simple por didáctica.
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
    /// @dev `_update` es el hook central de transferencia en ERC-1155 (OZ v5):
    ///      mint => from == address(0); burn => to == address(0); transferencia
    ///      real => ambos distintos de cero. Bloqueamos SOLO las transferencias
    ///      reales de ARQUERO y COLECCIONISTA; mint y burn siguen permitidos, y
    ///      MERCADER se comporta como un ERC-1155 normal (transferible).
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
