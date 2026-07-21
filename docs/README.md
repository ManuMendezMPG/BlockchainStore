# /docs — Project documentation

Living documentation for the project: a **learning guide** that explains not just
*what* each piece does, but **why** it's built the way it is. It walks the whole
system — the on-chain store, the bridge to MetaMask, the achievements, the debugging
lessons, the game-facing API, the Unreal client and the SIWE login — and the thesis
that ties them together: the **on-chain / off-chain boundary**.

**New here? Start with [00 — Overview](./00-overview.md).**

## The project in one paragraph

A Zelda-style Unreal Engine 5.5 prototype that demonstrates the **uses and the limits**
of blockchain in games. An in-game item store keeps **ownership and progress on-chain**:
a `GameStore` ERC-1155 contract holds the catalog and inventory, and an `Achievements`
contract mints progress medallions. A local **web bridge** connects the game to MetaMask
so the player signs purchases without the game ever touching a private key. The game
(Unreal) talks to the bridge over a small **HTTP/JSON API**, keeps game state in a
per-wallet **savegame**, and players authenticate with **SIWE** (Sign-In with Ethereum).
Gameplay itself stays fast and off-chain; only ownership, progress and identity are
anchored to the chain.

```
Game (Unreal)  →  Web bridge (Node + ethers.js)  →  MetaMask  →  GameStore (on-chain)
```

## Contents

Read them in order — each doc builds on the previous ones.

0. **[00 — Overview](./00-overview.md)**
   The entry point: what the project is, the central on-chain/off-chain message for a
   client, the technical stack, a conceptual diagram, and this reading order.
1. **[01 — The smart contract: `GameStore` (ERC-1155)](./01-smart-contract.md)**
   Why ERC-1155 for an inventory, the OpenZeppelin inheritance, function-by-function
   walkthrough (`setItem`/`buy`/`burn`/`withdraw`), design trade-offs and local
   deployment with Anvil.
2. **[02 — The web bridge: MetaMask ↔ contract](./02-bridge.md)**
   What the bridge solves, provider vs signer, connecting to MetaMask, reads vs
   writes, error handling with the full ABI, and running Node on Windows with Anvil on
   WSL.
3. **[03 — Achievements, dependencies and cross-contract architecture](./03-achievements-and-dependencies.md)**
   What goes on-chain vs off-chain, dependency rules, accumulated counters, the
   `Achievements` medallions (soulbound vs transferable), pseudo-random rarity, and
   contract-to-contract calls.
4. **[04 — Debugging and learnings](./04-debugging-and-learnings.md)**
   The real failures of the phase: the "phantom contract" of persistent state,
   `data="0x"` vs a raw revert selector, and asking the chain for the truth with
   `cast code` / `cast call`.
5. **[05 — The communication layer: bridge API and Unreal client](./05-api-and-unreal-client.md)**
   The three-actor pattern, the HTTP/JSON API (`/api/*`), the
   `pending → signing → done` purchase flow, polling vs WebSockets, the test-client,
   the four-panel client UI and the per-wallet savegame.
6. **[06 — Authentication: SIWE login (EIP-4361)](./06-login-siwe.md)**
   Why "connect" isn't authentication, what SIWE is, the login flow across the three
   actors, `ecrecover` verification, single-use nonces against replay, and login as a
   free off-chain signature vs a gas-costing purchase.
7. **[07 — The on-chain / off-chain boundary](./07-on-chain-off-chain-boundary.md)**
   The key architecture doc: what belongs on-chain vs off-chain and why, the rules we
   moved from the contract to the game (bottle→potion, arrow capacity) and the reasons,
   and the reusable criterion for deciding.

> These documents are written to double as a **consultancy presentation**: every
> decision comes with its rationale and its trade-offs. If you only read two, read
> **[00](./00-overview.md)** and **[07](./07-on-chain-off-chain-boundary.md)**.
