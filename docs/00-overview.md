# 00 — Overview

> The entry point to the project's documentation. Read this first: it explains **what
> the project is**, the **one idea** it exists to demonstrate, the **stack** that
> implements it, and where to go next. The numbered docs that follow (01–07) go deep
> on each piece, always with the *why*.

---

## 1. What this is

A **Zelda-style prototype** built in **Unreal Engine 5.5** whose purpose is not to be
a game, but to **demonstrate both the uses AND the limits of blockchain in video
games** — as a piece to present to a consultancy's clients.

It's deliberately honest: it shows where a blockchain adds real value (verifiable
ownership, earned progress, cryptographic identity) **and** where it would only get in
the way (fast, repeated gameplay), drawing a clear line between the two. That line —
the on-chain / off-chain boundary — is the project's thesis, and it has its own doc
([07](./07-on-chain-off-chain-boundary.md)).

---

## 2. The central message (for a client)

The single most important idea: **not everything belongs on the blockchain, and
knowing where the line goes is the skill.**

**What goes ON-CHAIN** (authoritative, public, permanent):

- **Item ownership** — which items and how many each wallet owns (ERC-1155 balances).
- **Purchases** — spending ETH to acquire items (real economy).
- **Achievements / medallions** — earned progress (Archer, Merchant, Collector).
- **Identity** — which wallet you are, *proven* by signature (SIWE), not just claimed.

**What goes OFF-CHAIN** (fast, private, local to the game):

- **Game state** — current health/mana, arrows loaded in the quiver, player name.
- **Using items** — shooting an arrow, drinking a potion, breaking a bottle.
- **Gameplay rules** — quiver capacity, backpack slots, crafting a potion from a bottle.

**Why that split** (the four forces):

| Force | On-chain would… | So we keep gameplay off-chain |
|-------|-----------------|-------------------------------|
| **Cost / gas** | charge gas for every action | shooting/drinking must be free |
| **Speed** | wait for a block to mine | gameplay must be instant |
| **Privacy** | publish state forever, publicly | play state stays local |
| **User experience** | pop a wallet signature per action | the game loop stays silent |

The payoff: players get **ownership they truly keep and can verify**, while the game
**stays fast and cheap** because moment-to-moment play never touches the chain. The
full rationale, and the concrete rules we *moved* from contract to game to honor this
line, are in [07](./07-on-chain-off-chain-boundary.md).

---

## 3. The technical stack

Four pieces, each documented in depth:

| Piece | Tech | Role |
|-------|------|------|
| **Contracts** | Foundry / Solidity, **ERC-1155** (OpenZeppelin v5) | `GameStore` (catalog, purchases, ownership) + `Achievements` (medallions). The source of truth for ownership. |
| **Bridge** | **Node.js** (dependency-free reads), JSON API + **SIWE** | Serves a local web page with MetaMask, exposes an HTTP/JSON API for the game, and runs SIWE login. Never holds keys. |
| **Unreal client** | **C++** subsystem + UMG UI + **savegame** | The game: main-screen UI, per-wallet savegame for game state, HTTP calls to the bridge. Never touches keys or the chain directly. |
| **Local chain** | **Anvil** (Foundry), chain id `31337` | The local Ethereum node the contracts are deployed to for development. |

Key stack decisions (each argued in its doc): ERC-1155 for a semi-fungible inventory
([01](./01-smart-contract.md)); a bridge because Unreal can't talk to MetaMask
directly ([02](./02-bridge.md)); zero dependencies for reads but proven crypto
libraries for SIWE ([06](./06-login-siwe.md)); polling over WebSockets for the
prototype ([05](./05-api-and-unreal-client.md)).

---

## 4. How the pieces communicate

```
┌──────────────┐   HTTP / JSON     ┌───────────────────────────┐  window.ethereum  ┌──────────┐
│ Unreal (UE5) │ ─────────────────►│  Bridge (Node + web page) │ ────────────────► │ MetaMask │
│  C++ + UMG   │ ◄─────────────────│  JSON API + SIWE + ethers │ ◄──────────────── │ (signs)  │
│  + savegame  │   polling         └─────────────┬─────────────┘                   └────┬─────┘
└──────────────┘                                 │ reads (eth_call)                     │ signed tx
       │ game state                              ▼                                      ▼
       ▼                                  ┌──────────────────  Anvil (chain 31337)  ──────────────┐
  ┌─────────┐                             │   GameStore (ERC-1155)   +   Achievements             │
  │ savegame│  (per-wallet: save_<addr>)  └────────────────────────────────────────────────────────┘
  └─────────┘
```

- **Unreal ↔ bridge:** plain HTTP/JSON. The game *requests* actions (buy, log in) and
  *polls* for results. It never builds transactions or handles keys.
- **Bridge ↔ chain:** the bridge does read-only queries directly (`eth_call`), and
  serves a web page where **MetaMask signs** the writes (purchases) and messages
  (SIWE login). Keys stay in MetaMask.
- **Bridge ↔ MetaMask:** the browser injects `window.ethereum`; the web page asks it
  to connect, sign transactions and sign login messages.
- **Unreal ↔ savegame:** game state (name, inventory, health/mana) persists locally in
  a savegame **keyed by wallet address**, seeded once from the chain and autosaved
  after each action.

The three-actor pattern (Unreal requests → bridge coordinates without keys →
web+MetaMask signs) with its `pending → signing → done` state machine is the backbone
of both purchases and login — see [05](./05-api-and-unreal-client.md) and
[06](./06-login-siwe.md).

---

## 5. Recommended reading order

The docs build on one another; read them in order:

0. **[00 — Overview](./00-overview.md)** — *(you are here)* the project, the thesis,
   the stack.
1. **[01 — The smart contract: `GameStore` (ERC-1155)](./01-smart-contract.md)** — why
   ERC-1155, the contract function by function, local deployment with Anvil.
2. **[02 — The web bridge: MetaMask ↔ contract](./02-bridge.md)** — why a bridge,
   provider vs signer, connecting MetaMask, error handling, Windows/WSL setup.
3. **[03 — Achievements, dependencies and cross-contract architecture](./03-achievements-and-dependencies.md)**
   — medallions (soulbound vs transferable), dependency rules, contract-to-contract calls.
4. **[04 — Debugging and learnings](./04-debugging-and-learnings.md)** — the real
   failures: phantom contract, `data="0x"` vs revert selector, asking the chain for truth.
5. **[05 — The communication layer: bridge API and Unreal client](./05-api-and-unreal-client.md)**
   — the HTTP/JSON API, the purchase flow, the client UI and the per-wallet savegame.
6. **[06 — Authentication: SIWE login (EIP-4361)](./06-login-siwe.md)** — why connect
   ≠ authenticate, `ecrecover`, single-use nonces, login as a free off-chain signature.
7. **[07 — The on-chain / off-chain boundary](./07-on-chain-off-chain-boundary.md)** —
   **the key doc**: what goes on-chain vs off-chain, the rules we moved and why, the
   reusable criterion.

> If you only read two, read **[00](./00-overview.md)** (this one) and
> **[07](./07-on-chain-off-chain-boundary.md)**: together they carry the whole message.
> The rest is how it's actually built.
