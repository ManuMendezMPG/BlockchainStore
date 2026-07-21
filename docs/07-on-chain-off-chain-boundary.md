# 07 — The on-chain / off-chain boundary

> Part of the project's learning guide.
> [01](./01-smart-contract.md) · [02](./02-bridge.md) · [03](./03-achievements-and-dependencies.md) · [04](./04-debugging-and-learnings.md) · [05](./05-api-and-unreal-client.md) · [06](./06-login-siwe.md).
> This is the **central architecture doc**: what belongs on the blockchain and what
> belongs in the game, why, and the concrete rules we **moved** from the contract to
> the client once experience showed they belonged there. With the whys.

---

## 1. The central decision: ownership on-chain, gameplay off-chain

Every design choice in this project descends from one question — *what deserves to
live on the blockchain, and what doesn't?* Putting everything on-chain is expensive,
slow and rigid; putting nothing on-chain throws away real ownership. The rule we
follow:

> **On-chain: ownership, economy and identity. Off-chain: use, state and gameplay
> rules.**

Concretely, split across the two halves of the system:

| Concern | Where it lives | Why |
|---------|----------------|-----|
| **That you own** a sword, a bow, N arrows (item balances) | **On-chain** (`GameStore` ERC-1155) | Ownership must be verifiable, transferable and cheat-proof against the client. |
| **Purchases** (spending ETH for items) | **On-chain** (`buy`) | It's economy: value changes hands; it must be authoritative and auditable. |
| **Achievements / medallions** (Archer, Merchant, Collector) | **On-chain** (`Achievements`) | Earned progress and identity; it shouldn't be forgeable. |
| **Identity** (which wallet you are) | **On-chain proof** (SIWE, doc 06) | Proven by signature, not merely claimed. |
| **Using** items (shoot an arrow, drink a potion, break a bottle) | **Off-chain** (Unreal savegame) | Session consumption: happens many times, must be instant and free. |
| **Game state** (arrows currently in quiver, health/mana, name) | **Off-chain** (savegame) | Changes constantly; it's not ownership, it's the moment-to-moment state. |
| **Gameplay rules** (quiver capacity, backpack slots, crafting a potion) | **Off-chain** (Unreal) | Presentation and session rules about *how* you use what you own. |

The **Zelda** metaphor makes it intuitive: *having* a bow and 12 arrows is
**ownership** (on-chain); *shooting* an arrow is **use** (off-chain); the fact that
your quiver *holds only 10* is a **gameplay rule** (off-chain). You own the arrows on
the chain forever; how many fit and how you spend them is the game's business.

### Why the split (the four forces)

The boundary isn't ideology — it's driven by four concrete costs of doing something
on-chain:

- **Cost / gas.** Every on-chain write costs gas. Shooting arrows or sipping potions
  many times per session would be absurd to pay for. Ownership changes are rare and
  worth paying for; use is constant and must be free.
- **Speed / latency.** An on-chain write waits for a block to be mined. Gameplay needs
  to be instant; a savegame write is sub-millisecond. You can't gate "draw the bow" on
  block time.
- **Privacy.** The chain is public and permanent. Your item ownership being public is
  fine (it's the point); your minute-to-minute play state doesn't belong in a public,
  immutable ledger.
- **User experience.** Real ownership that the player *keeps* is the selling point of
  putting anything on-chain at all. But a game where every action pops a wallet
  signature would be unplayable. The split gives both: verifiable ownership **and** a
  fast, silent game loop.

> The demo's pitch in one line: *"the player's ownership and progress are truly theirs
> and verifiable; gameplay stays fast and cheap because it lives in the client."*

---

## 2. Rules we moved from the contract to the game (and why)

The clearest evidence that the boundary is the *right* one is that we **moved two
rules off-chain** after they caused friction on-chain. Both were originally enforced
by `GameStore`; both now live in Unreal, validated against the savegame. (These moves
are also referenced from [03 §0 and §1.2](./03-achievements-and-dependencies.md) and
[04 §4](./04-debugging-and-learnings.md); here is the full reasoning.)

### 2.1 Bottle → potion: crafting is a game rule

**Before:** buying a potion required (and **consumed**) an empty bottle on-chain. The
contract's `buy` had a dependency branch that burned a `BOTELLA_VACIA` token when you
bought a potion — the idea being "a potion is a filled bottle".

**The problem:** an empty bottle is part of your **game inventory**, and that
inventory lives in the **savegame**, not on-chain. "Crafting" a potion from a bottle
is a session/inventory rule — the kind of thing you do repeatedly as you play. Coupling
it to an on-chain purchase meant a gameplay/crafting decision was being enforced by a
contract that couldn't see the game's real inventory state.

**The move:** the bottle requirement was **removed from the contract**. On-chain, the
store now **sells potions freely** (no `_checkDependencies` branch for
`POCION_VIDA` / `POCION_MANA`, no bottle burned). The empty bottle is still a sellable
item (id 7); it just no longer gates potions at the contract level. Whether crafting a
potion consumes a bottle is now **validated in Unreal against the savegame**, where the
game inventory actually lives.

### 2.2 Arrow capacity: local use desynced the chain

**Before:** the contract limited **how many arrows fit** in the quiver — a purchase of
arrows that would exceed the quiver's capacity reverted.

**The problem — a desync bug.** The player **shoots arrows locally** in Unreal: that's
use, it's off-chain, and the contract never sees it. So the on-chain arrow balance and
the savegame's "arrows currently in the quiver" **drift apart** the moment you fire a
single shot. When the contract then checked a **capacity** rule against its *own*
(now-stale) balance, it **rejected purchases that actually fit in-game**. Example:

```
buy 10 arrows      chain balance 10   quiver (savegame) 10     ok
shoot 8 arrows     chain balance 10   quiver (savegame)  2     (use is off-chain!)
buy 8 arrows       chain thinks you'd have 18 → capacity 10 exceeded → REVERT
                   …but in-game you only have 2 and 8 fit perfectly
```

The contract was enforcing a rule about a quantity it couldn't correctly track. That's
the tell-tale sign a rule is on the wrong side of the boundary.

**The move:** the **capacity limit moved to Unreal**, validated against the savegame's
real arrow count. On-chain, buying arrows no longer checks any capacity — nothing stops
you from buying 20 arrows in one `buy` (once you have a bow + quiver). The
`quiverCapacity(address)` view still exists, but its on-chain job is reduced to
answering *"do you own any quiver at all?"* (returns `0` if none), and the **game**
reads the tier size (5/10/20) to enforce the real capacity locally.

> Note the subtlety this preserves: the **historical** arrow counter
> (`purchasedTotal`) stays on-chain and still unlocks the Archer achievement at 20
> arrows purchased. *Buying* arrows is economy (on-chain, permanent); *having them in
> the quiver right now* is game state (off-chain). See
> [03 §1.3](./03-achievements-and-dependencies.md).

### 2.3 What stayed on-chain (and why it's correct there)

Not everything moved. The rules that **remained** in the contract are the ones that are
about **ownership**, not use:

| Rule kept on-chain | Why it belongs there |
|--------------------|----------------------|
| **Arrows require a bow + a quiver** | It's an *ownership* gate: to buy arrows you must **own** a bow and **own** at least one quiver. Ownership is exactly what the chain knows authoritatively. |
| **`carcaj_10` requires owning `carcaj_5`** | A progression gate expressed purely in terms of **owned** tokens — the chain can verify it directly. |
| **`carcaj_20` requires the Archer achievement** | A purchase **gated by on-chain progress**: it reads the player's achievement (in the `Achievements` contract) to decide what they may buy. Both sides of this check are on-chain facts. |

The pattern: a rule stays on-chain when it can be decided **entirely from on-chain
facts (ownership, counters, achievements)**. It moves off-chain the moment it depends
on **game state the contract can't see** (arrows in the quiver, bottles in the
backpack, potions drunk).

---

## 3. The general criterion (how to decide, next time)

When a new rule appears, ask **which facts it depends on**:

```
Does the rule depend only on ownership / economy / on-chain progress?
        │ yes                                   │ no — it depends on game state / use
        ▼                                        ▼
   ON-CHAIN (contract)                      OFF-CHAIN (Unreal savegame)
   e.g. "own a bow to buy arrows"           e.g. "how many arrows fit",
        "spent > X unlocks Merchant"              "a potion needs an empty bottle",
                                                  "drinking a potion restores health"
```

Two litmus questions that catch most cases:

1. **Does it happen many times per session?** If yes (shooting, drinking, breaking),
   it's use → off-chain. On-chain writes are for rare, valuable events.
2. **Can the contract actually see the truth it needs?** If the rule depends on
   quantities the game mutates locally (quiver fill, bottles), the contract only ever
   sees a stale snapshot → the rule belongs where the truth lives: the savegame.

> Corollary from doc 04: when on-chain and off-chain state can drift, **don't let the
> contract enforce a rule about the side it can't observe.** The arrow-capacity revert
> bug was precisely that mistake, and moving the rule off-chain fixed the class of bug,
> not just the instance.

---

## 4. How the two halves stay coherent

If ownership is on-chain and game state is off-chain, what keeps them from lying to
each other?

- **The chain seeds the game, once.** On first login for a wallet, the savegame's
  inventory is **seeded from `GET /api/inventory`** (on-chain balances) — see
  [05 §8](./05-api-and-unreal-client.md). The authoritative ownership bootstraps the
  local state.
- **From then on, each side owns its half.** The chain remains the truth for
  *ownership* (what you have bought / burned / been minted); the savegame is the truth
  for *game state* (what's loaded in the quiver, current health, what's been used).
- **Purchases cross the boundary explicitly.** Buying is the one action that is both:
  it's an on-chain transaction (economy) **and** it updates the savegame afterward
  (the new item enters game inventory). That's why the client refreshes inventory and
  autosaves after a completed purchase ([05 §7–§8](./05-api-and-unreal-client.md)).
- **Identity ties them together.** SIWE (doc 06) proves the wallet, and the proven
  address is both the on-chain identity **and** the savegame slot key
  (`save_<address>`). One identity, two stores of state.

---

## Summary for the demo

- **The boundary is the whole point.** On-chain: ownership, economy, achievements,
  identity. Off-chain: use, game state, gameplay rules. The split is justified by
  **gas, speed, privacy and UX** — not dogma.
- **We proved the boundary by moving rules across it.** The **bottle→potion** rule and
  the **arrow-capacity** limit started on-chain and were moved to Unreal, because they
  depended on game state the contract couldn't see; the arrow-capacity one was even
  causing valid purchases to revert (a desync bug — [04](./04-debugging-and-learnings.md)).
- **What stayed on-chain is ownership-only:** bow + quiver to buy arrows,
  `carcaj_10`→`carcaj_5`, `carcaj_20`→Archer achievement. Each is decidable purely from
  on-chain facts.
- **The criterion is reusable:** depends only on ownership/economy/on-chain progress →
  on-chain; depends on game state or repeated use → off-chain.
- **The halves stay coherent** via a one-time chain→savegame seed, a clear ownership of
  each half, purchases that cross the line explicitly, and a single SIWE-proven identity
  keying both. See [01](./01-smart-contract.md) for the contract side and
  [05](./05-api-and-unreal-client.md) for the client/savegame side.
