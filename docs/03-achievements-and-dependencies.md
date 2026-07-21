# 03 — Achievements, dependencies and cross-contract architecture

> Part of the project's learning guide. Closes the contracts phase. Continues
> [01 — The smart contract](./01-smart-contract.md) and
> [02 — The web bridge](./02-bridge.md). Material also meant to **present at a
> consultancy**: every decision comes with its why.

---

## 0. The narrative: what goes on-chain and what doesn't

The most important design question of the whole project isn't technical, it's about
the **model**: *what deserves to live on the blockchain and what doesn't?* Putting
everything on-chain is expensive, slow and rigid; putting nothing on-chain wastes
real ownership. The rule we follow:

> **On-chain: ownership, progress and identity. Off-chain: use, session and
> presentation rules.**

The **Zelda** metaphor makes it intuitive:

| Zelda concept | Here | Where it lives | Why |
|---------------|------|----------------|-----|
| That you **have** a sword, a bow, 12 arrows | ERC-1155 balances in `GameStore` | **On-chain** | It's *ownership*: it must be verifiable, transferable and cheat-proof against the client. |
| Dungeon **medallions** (progress) | Achievements in `Achievements` | **On-chain** | It's *identity and earned progress*: it shouldn't be forgeable. |
| **Shooting** an arrow, **drinking** a potion, **breaking** a bottle | Use logic | **Off-chain (Unreal)** | It's *session consumption*: it happens many times per second, it can't cost gas or wait for blocks. |
| That the **backpack has 6 slots**, how many arrows **fit** in the quiver, that a potion needs an empty bottle | Slots / capacity / crafting rules | **Off-chain (Unreal)** | These are *session and presentation rules*: how what you already own is used, limited and shown. |

Translated to contract responsibilities: **`GameStore` manages ownership, economy
and purchase dependencies; `Achievements` manages identity/progress.** "Using"
(spending arrows, drinking potions), the slots, the quiver **capacity limit** and the
**empty-bottle-for-a-potion** rule **don't touch the chain**: they're Unreal's.

> **Note on two rules that moved.** In an earlier version, two rules lived on-chain
> and were later moved to the game layer (validated against Unreal's savegame),
> because they created friction with local, off-chain use:
> - **Arrow capacity** (how many arrows fit in the quiver). The player shoots arrows
>   locally in Unreal; the contract never saw those shots, so enforcing a capacity
>   against its own balance rejected purchases that actually fit in-game.
> - **Empty bottle → potion** (a potion consumed an empty bottle). Crafting a potion
>   from a bottle is a session/inventory rule, so it now lives with the rest of the
>   game inventory in the savegame.
>
> What **stays** on-chain for arrows is only the *gear requirement*: you need a bow
> **and** at least one quiver to buy arrows. The rest of §1.2 reflects the current
> contract.

This separation is precisely the demo's selling point: *"the player's ownership and
progress are truly theirs and verifiable; gameplay stays fast and cheap because it
lives in the client"*.

---

## 1. GameStore, extended

### 1.1 Excess refund: why it goes last

`buy()` **returns the change**: if you overpay, you get the difference back. *Where*
that refund is placed inside the function is a security decision, not a style one. We
follow the **checks-effects-interactions (CEI)** pattern:

```solidity
function buy(uint256 itemId, uint256 quantity) external payable nonReentrant {
    // 1) CHECKS — validate EVERYTHING before touching state
    if (!isListed[itemId]) revert ItemNotListed(itemId);
    if (quantity == 0) revert InvalidQuantity();
    uint256 cost = priceOf[itemId] * quantity;
    if (msg.value < cost) revert InsufficientPayment(cost, msg.value);
    _checkDependencies(itemId);

    // 2) EFFECTS — change the contract state
    _mint(msg.sender, itemId, quantity, "");
    purchasedTotal[msg.sender][itemId] += quantity;
    totalSpent[msg.sender] += cost;
    emit ItemPurchased(msg.sender, itemId, quantity, msg.value);

    // 3) INTERACTIONS — external calls, ALWAYS last
    _checkAchievements(msg.sender);              // (a) mint achievements
    uint256 excess = msg.value - cost;
    if (excess > 0) {                            // (b) return the change
        (bool ok,) = payable(msg.sender).call{value: excess}("");
        if (!ok) revert RefundFailed();
        emit ExcessRefunded(msg.sender, excess);
    }
}
```

**Why the refund is last.** Sending ETH with `call` temporarily hands control to the
recipient: if it were a malicious contract, it could **re-enter** and call `buy()`
again before the first call finishes. If the refund happened *before* updating
balances and counters, that reentrancy would see a half-finished state and could
exploit it (e.g. buy "for free" or double-mint). Placing it **after all the
effects**, when it re-enters everything is already consolidated: a reentrancy would
be, at most, another legitimate purchase.

**Double belt:** we also apply OpenZeppelin's `nonReentrant` modifier, which blocks
any reentrancy into `buy()` outright. CEI + `nonReentrant` is the standard pattern:
the correct order *and* the lock. We use `call` (not `transfer`) for the same reason
as in `withdraw` (see doc 01): it forwards all the gas and we check the returned
boolean.

> This closes the **pending improvement** noted in doc 01 (§5.4): the excess no
> longer gets trapped in the contract.

### 1.2 Dependency rules

The contract doesn't just charge: it **models the rules of the world**. A player
can't buy anything in any order. All the checks live in `_checkDependencies` (the
*checks* phase) and revert with clear *custom errors*.

**a) Bow + quiver → Arrow.** It makes no sense to have arrows without a bow, and you
can't carry arrows without a quiver to hold them:

```solidity
if (itemId == FLECHA) {
    if (balanceOf(msg.sender, ARCO) == 0) revert NeedBow();
    if (quiverCapacity(msg.sender) == 0) revert NeedQuiver();
}
```

The **gear requirement** is what's enforced on-chain: you need a bow and at least one
quiver. The **capacity limit** (how many arrows fit) is **no longer checked
on-chain** — it moved to Unreal (see §0), because the player shoots arrows locally
and the contract can't track that. So `quiverCapacity` is still used here, but only
to answer *"do you own any quiver at all?"* (`> 0`), not to cap the amount:

```solidity
function quiverCapacity(address a) public view returns (uint256) {
    if (balanceOf(a, CARCAJ_20) > 0) return 20;
    if (balanceOf(a, CARCAJ_10) > 0) return 10;
    if (balanceOf(a, CARCAJ_5)  > 0) return 5;
    return 0;               // no quiver → can't buy arrows
}
```

The function still returns the tier size because the **game** reads it (Unreal uses
it as the capacity limit against the savegame) and because `0` is the clean signal
for "no quiver".

**b) Potions have no on-chain bottle dependency.** A potion is conceptually "a filled
bottle", but *crafting* it from an empty bottle is a session/inventory rule, so it
now lives in Unreal (validated against the savegame). On-chain, the store simply
**sells potions freely** — there is no `_checkDependencies` branch for
`POCION_VIDA` / `POCION_MANA` and no bottle is burned:

```solidity
// (no potion branch in _checkDependencies — the store just mints the potion)
```

> Earlier, buying a potion **consumed** an empty bottle on-chain (`_burn` of
> `BOTELLA_VACIA`). That coupling was removed when the rule moved to the game layer.
> The empty bottle is still a sellable item (id 7); it just no longer gates potions
> at the contract level.

**c) Quiver evolution.** The quiver levels up with requirements:

- `carcaj_10` can only be bought if you already own `carcaj_5` (`NeedQuiver5`).
- `carcaj_20` can only be bought if you've **unlocked the Archer achievement**
  (`NeedArcheroAchievement`).

```solidity
} else if (itemId == CARCAJ_10) {
    if (balanceOf(msg.sender, CARCAJ_5) == 0) revert NeedQuiver5();
} else if (itemId == CARCAJ_20) {
    if (address(achievements) == address(0) || !achievements.hasArquero(msg.sender))
        revert NeedArcheroAchievement();
}
```

`carcaj_20` is the nicest example in the demo: a purchase **gated by on-chain
progress**. It's not money that unlocks it, it's *having earned* the Archer
achievement. It's the contract reading the player's progress (in the other contract)
to decide what it lets them buy.

### 1.3 Accumulated counters: why the balance isn't enough

We added `purchasedTotal[player][item]`, which **only increments** on each purchase.
It's a different metric from the **balance** (what you have *now*), and the
difference is the key to the Archer achievement:

> **The balance goes up and down; the historical counter only goes up.**

The concrete case: Archer requires **20 historical arrows purchased**. In-game your
quiver only holds 10 at a time (Unreal's capacity rule), so you reach 20 by buying,
spending and buying again:

```
buy 10 arrows    → balance 10, historical 10   (quiver_10, full in-game)
shoot 10 arrows  → balance  0, historical 10   (spent in Unreal, burned on-chain)
buy 10 arrows    → balance 10, historical 20   → Archer unlocked!
```

If the achievement looked at the **balance**, it would depend on a moment-in-time
snapshot. By looking at the **accumulated** counter, it rewards *effort over time*,
which is exactly what an achievement represents. And it fits the narrative: spending
arrows is off-chain (Unreal), but *having bought them* is recorded on-chain forever.

> Note: on-chain, nothing now stops you from buying 20 arrows in a single `buy`
> (once you have a bow + quiver) — the capacity cap is Unreal's. The historical
> counter is what the achievement keys on, regardless of how you got there.

`totalSpent[player]` is the same pattern for money: it accumulates real spend (not
counting the refunded excess) and feeds the Merchant achievement.

---

## 2. Achievements: progress medallions

A **separate** ERC-1155 contract (its ids start at 0 without clashing with the
items). Three medallions — shown in-game as **Archer / Merchant / Collector** (their
on-chain ids 0/1/2 and Solidity constants are still `ARQUERO` / `MERCADER` /
`COLECCIONISTA`):

| Medallion (in-game) | Condition | Type | Why that type |
|---------------------|-----------|------|---------------|
| **Archer** (id 0, `ARQUERO`) | 20 historical arrows purchased | **Soulbound** | It's personal merit; it unlocks quiver_20. It makes no sense to "sell your mastery". |
| **Merchant** (id 1, `MERCADER`) | Accumulated spend > threshold | **Transferable** + rarity | It's a collectible/status; it can have a secondary market. |
| **Collector** (id 2, `COLECCIONISTA`) | Own the full set (sword+shield+bow+some quiver) | **Soulbound** | "Completist" identity; tied to the account. |

The **soulbound/transferable** mix is deliberate and a good talking point in the
demo: *not all digital assets should behave the same way*. Earned progress stays with
you (soulbound); the collectible can circulate.

### 2.1 How soulbound is implemented

In OpenZeppelin v5's ERC-1155, **every** transfer, mint and burn goes through a
single internal hook: `_update(from, to, ids, values)`. By overriding it we control
the movements. The key is to read the **three cases** by their endpoints:

| Case | `from` | `to` | Allowed for soulbound? |
|------|--------|------|------------------------|
| **Mint** | `address(0)` | player | ✅ Yes — this is how the medallion is born |
| **Burn** | player | `address(0)` | ✅ Yes — the owner can destroy it |
| **Transfer** | player A | player B | ❌ No — this is what we block |

```solidity
function _update(address from, address to, uint256[] memory ids, uint256[] memory values)
    internal override
{
    if (from != address(0) && to != address(0)) {      // REAL transfer
        for (uint256 i = 0; i < ids.length; i++)
            if (ids[i] == ARQUERO || ids[i] == COLECCIONISTA) revert Soulbound(ids[i]);
    }
    super._update(from, to, ids, values);              // continue the normal flow
}
```

We only enter the blocking loop when **both** endpoints are non-zero (a real
transfer), and we only revert for the Archer and Collector medallions. Merchant never
enters the condition → it behaves like a normal ERC-1155 and **is transferable**.
It's *selective* soulbound by id, in a single contract.

### 2.2 The Merchant's pseudo-random rarity — and why it's NOT secure

When Merchant is minted it's assigned a weighted rarity (Bronze 60% / Silver 30% /
Gold 10%):

```solidity
uint256 rand = uint256(keccak256(abi.encodePacked(
    blockhash(block.number - 1), block.timestamp, block.prevrandao, to, _nonce++
))) % 100;
if (rand < 60) return Rarity.Bronce;   // Bronze
if (rand < 90) return Rarity.Plata;    // Silver
return Rarity.Oro;                      // Gold
```

**This is fine for a local demo, but it would be a serious flaw in production**, and
it's worth saying clearly in a consulting presentation:

- **It's predictable.** All the inputs (`timestamp`, `prevrandao`, `blockhash`, the
  address, the nonce) are **public or computable**. Anyone can compute the result
  *before* sending the transaction.
- **It's abortable.** If the call came from another contract, that contract could
  **simulate the result and revert if it isn't "Gold"**, retrying until it wins.
  Randomness you can see coming and cancel isn't randomness.
- **It's influenceable.** `timestamp` and `prevrandao` are **proposed by the block
  validator**, who has some margin to bias them or to delay the inclusion of your
  transaction until a block that suits them.

**What's used in production: a verifiable randomness oracle (VRF), like Chainlink
VRF.** The pattern is **two transactions**:

1. The contract *requests* randomness (`requestRandomWords`).
2. The oracle answers in a **later transaction** (callback) with a number
   **accompanied by a cryptographic proof** that the contract verifies on-chain.

Since the number doesn't exist until the callback and comes signed, **neither the
player nor the validator can know or manipulate it in advance**. The cost is
complexity (subscription, callback handling, asynchrony), which is why here we keep
it simple — **with a big warning in the code** so nobody copies it as-is to
production.

---

## 3. Cross-contract architecture

### 3.1 One contract calling another via an interface

GameStore doesn't import the whole Achievements contract: it keeps a typed reference
with a **minimal interface**, only the functions it uses.

```solidity
interface IAchievements {
    function isUnlocked(address account, uint256 id) external view returns (bool);
    function hasArquero(address account) external view returns (bool);
    function mintArquero(address to) external;
    function mintMercader(address to) external;
    function mintColeccionista(address to) external;
}
IAchievements public achievements;   // address + that interface
```

**Why an interface and not the full contract:** it decouples. GameStore only needs to
know *how to call*, not the internal implementation of Achievements. It's cleaner,
compiles less and avoids circular dependencies.

### 3.2 The `msg.sender` shift

When GameStore runs `achievements.mintArquero(buyer)`, it makes an **external CALL**:
execution jumps to the other contract, which runs over **its own storage**. And,
crucially, inside that call:

```
msg.sender (inside Achievements) == address(GameStore)
```

It's not the player's address: it's the **calling contract's**. That shift of
`msg.sender` is what makes permission control possible: Achievements can require that
whoever mints is GameStore and nobody else.

### 3.3 Minter permission control

```solidity
address public minter;                       // set by the Achievements owner
modifier onlyMinter() { if (msg.sender != minter) revert NotMinter(); _; }
function mintArquero(address to) external onlyMinter { ... }
```

Medallions **must only be born as a consequence of the store logic**, never from a
player calling directly. Authorization boils down to one variable: after deploying,
the owner runs `achievements.setMinter(address(gameStore))`. From then on, thanks to
the `msg.sender` shift, only GameStore passes `onlyMinter`.

> The wiring is **bidirectional**: `GameStore.setAchievements(ach)` (so it knows who
> to call) and `Achievements.setMinter(store)` (to authorize it). Both are done by
> the owner in the deployment script, *after* deploying the two contracts.

### 3.4 Atomicity: the purchase and the achievement, all or nothing

The achievement mint happens **within the same transaction** as the purchase. In
Ethereum a transaction is **atomic**: either *all* its changes apply, or *everything*
reverts. Direct consequence:

> There's no intermediate state "I bought the 20th arrow but the Archer medallion
> wasn't minted". Either both things happen, or neither does.

This eliminates a whole class of synchronization bugs you *would* have with an
off-chain system (two databases that can drift out of sync). Here consistency is a
property of the medium.

### 3.5 Two-layer defense

That atomicity has a dangerous edge: **if the achievement call reverts, the whole
purchase reverts**. Picture a player who already has Archer buying more arrows: if
GameStore tried to re-mint, `Achievements` would revert with `AlreadyUnlocked` and
the arrow purchase would fail for no apparent reason!

We prevent it with **two layers of defense**:

1. **GameStore checks before calling.** `_checkAchievements` only invokes the mint if
   the milestone is met **and** `!isUnlocked(...)`. It never triggers the revert.
   ```solidity
   if (purchasedTotal[buyer][FLECHA] >= ARQUERO_ARROWS && !achievements.isUnlocked(buyer, ACH_ARQUERO))
       achievements.mintArquero(buyer);
   ```
2. **Achievements protects itself.** Even so, `mintArquero` reverts with
   `AlreadyUnlocked` if called twice. It's a safety net in case another (future)
   minter didn't do the prior check.

Layer 1 guarantees normal operation; layer 2 guarantees the **invariant** ("a unique
medallion is never minted twice") no matter what. A test verifies exactly this:
buying more arrows after having Archer **doesn't revert** and **doesn't duplicate**
the medallion.

---

## 4. Status of the contracts phase

✅ `GameStore` — ERC-1155 store with purchase/burn/withdraw, **excess refund**,
**dependency rules** (bow+quiver → arrow, quiver evolution, achievement gate) and
**accumulated counters**. The arrow-capacity limit and the empty-bottle→potion
requirement were **moved to the game layer (Unreal)**.
✅ `Achievements` — 3 medallions (Archer/Merchant/Collector), **selective soulbound**,
**authorized minter**, pseudo-random rarity (with its warning).
✅ Cross-contract connection (interface + minter), with atomicity and two-layer defense.
✅ **37 tests green**; deployment script updated (both contracts wired) and verified
against Anvil, keeping GameStore's deterministic address for the bridge.

### How it fits the demo (summary for presenting)

- **On-chain (verifiable, the player's):** what they own (items), how far they've
  progressed (counters, achievements) and their identity (soulbound medallions).
- **Off-chain (fast, session):** using the items (shoot, drink, break), the quiver
  capacity, the empty-bottle-for-a-potion rule, and the presentation rules (6-slot
  backpack) — all in Unreal.
- **The bridge** (doc 02) connects both worlds via MetaMask, without the game ever
  touching the player's key.

**Beyond the contracts:** the `bridge` catalog now serves all 10 items with their
English names, and the game integration is covered in
[05 — API and Unreal client](./05-api-and-unreal-client.md).
