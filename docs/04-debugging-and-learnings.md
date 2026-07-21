# 04 — Debugging and learnings

> Part of the project's learning guide. Continues
> [01](./01-smart-contract.md) · [02](./02-bridge.md) · [03](./03-achievements-and-dependencies.md).
> This document captures the **real failures** of this phase and how they were
> diagnosed. Understanding the failure modes of a local blockchain system — not just
> the happy path — is exactly what tells a solid demo apart from a fragile one.

---

## 1. The "phantom contract" of persistent state

### Symptom
After extending `GameStore` (achievements, dependencies, counters) and syncing the
bridge, **the bridge code was correct** but reads of the new functions failed: on
buying, `loadProgress` blew up in `purchasedTotal(...)` with `execution reverted (no
data present)`. The puzzling part: `balanceOf` *did* work, and the purchase seemed
to go through fine.

### Root cause
The local node was started with `scripts/start-local.sh`, which **persists Anvil's
state** in `.anvil/state.json` so the inventory isn't lost between restarts. That
file had been created in an earlier session, when `GameStore` was an **old** version
(3-item catalog, no `purchasedTotal`).

The script's logic was: *"if there's bytecode at the deterministic address, don't
redeploy"*. When loading the old state, there **was** bytecode (the old contract's),
so the script deemed it good and **didn't redeploy**. We were testing the new bridge
against the **old contract**, without knowing it. The functions we added didn't exist
in that bytecode → revert.

> The "phantom" contract: the one you think is deployed (your latest version of the
> code) isn't the one the chain actually loaded from disk.

### The underlying trade-off
This wasn't an isolated oversight, it was the **cost of a design decision**. We added
the persistence on purpose (see the script's doc): having the inventory survive
power-cycling is very convenient for iterating. But that same persistence introduces
a new risk: **freezing an old version of the contract** in the saved state.
Convenience and freshness pull in opposite directions. There's no "free lunch": every
infrastructure feature brings its own class of bug.

### The fix: version detection (three-branch logic)
We hardened `start-local.sh` so it doesn't settle for *"is there bytecode?"* but
checks *"is it the right version?"*. After loading the state, it **probes a getter
that only exists in the current version** (`purchasedTotal`). The decision tree:

```
cast code <addr>  ──►  is there bytecode?
      │ no                       │ yes
      ▼                          ▼
 [DEPLOY]         probe: cast call purchasedTotal(...)
                       │ responds          │ reverts (data 0x)
                       ▼                    ▼
                  [REUSE]              [REDEPLOY clean + warning]
```

| Detection | Action |
|-----------|--------|
| No bytecode | Deploy (clean chain) |
| Bytecode **and** the getter responds | Reuse (keeps inventory) |
| Bytecode **but** the getter reverts | Warn, reset to a clean chain and redeploy |

An important detail of the redeploy: **it doesn't redeploy on top of the old chain**.
The deterministic address `0x5FbD…0aa3` is only obtained with account #0 at **nonce
0**; if we redeployed over the old state (nonce already advanced), the contract would
land on a different address and break the bridge. That's why the script **sets aside**
the old state as `.anvil/state.json.stale`, starts a clean chain and then deploys. And
it warns loudly, nothing silent:

```
⚠️  Detected a PREVIOUS-VERSION contract in the persistent state.
    The inventory/purchases of that state are NOT valid for the new contract.
    Resetting to a CLEAN CHAIN and redeploying the current version ...
```

---

## 2. `data="0x"` vs raw selector: two failures of the SAME kind of error

Both are "the contract reverted", but they mean opposite things and are fixed
differently. Knowing how to tell them apart is half the debugging.

### Case A — `data="0x"` (no data present): the function does NOT exist
When you call a function whose selector **isn't** in the deployed bytecode, there's
nothing to execute: the EVM falls to the contract's `fallback`/`receive`, and since
that ERC-1155 returns no error data, the call **reverts without a payload** →
`data: "0x"`. ethers, finding no revert data to decode, reports it as *"execution
reverted (no data present)"* or *"require(false)"*.

This is what happened with the phantom contract: `purchasedTotal` didn't exist in the
old bytecode.

**How to diagnose it (asking the chain):**
```bash
# Is there actually a contract at that address?
cast code 0x5FbD…0aa3 --rpc-url http://127.0.0.1:8545      # 0x = nothing; 0x6080… = yes

# Does the function you think exists respond?
cast call 0x5FbD…0aa3 "purchasedTotal(address,uint256)(uint256)" 0x0…0 0 --rpc-url ...
#   → "0"                         : exists (correct contract)
#   → "execution reverted 0x"     : does NOT exist (old version / wrong address)
```

### Case B — raw selector (`0x1b9e2491`): the function DOES exist and reverts on purpose
Here the function exists and reverts with a legitimate **custom error** (a business
rule, e.g. `NeedBow()`). What travels is the error's **4-byte selector**
(`0x1b9e2491` = `keccak256("NeedBow()")[:4]`), optionally followed by its encoded
args.

The subtle detail: this error appeared **undecoded** only when the operation went
through **MetaMask** during gas estimation. It arrived in the EIP-1193 JSON-RPC error
format:
```json
{ "code": 3, "message": "execution reverted", "data": "0x1b9e2491" }
```
…nested in `err.info.error.data`, as raw hex. The message switch, which looked at
`err.revert.name`, found no name and fell to the generic message.

**The fix:** decode the selector ourselves with the already-loaded ABI:
```js
const iface = new ethers.Interface(gsAbi);
const parsed = iface.parseError("0x1b9e2491"); // → { name: "NeedBow", args: [] }
```
`parseError` matches the first 4 bytes of the `data` against the ABI's custom-error
selectors and returns name + args. From there, the same old switch produces *"🏹 You
need a BOW…"*.

### Why ethers decodes in one case and not the other
It depends on **who makes the call that reverts**:

- **ethers controls the call** (one of its own `eth_call`s, or a mined tx): it has
  both the revert `data` **and** the ABI of the `Contract` you created. It decodes on
  its own and fills `err.revert.name` / `err.revert.args`. → you get it nicely.
- **MetaMask does the gas estimation** (a write via `BrowserProvider` →
  `window.ethereum`): the failure is generated by the injected provider and arrives
  "pre-made" in EIP-1193 format, with the raw `data` in `err.info.error.data`. ethers
  wraps it but **doesn't automatically associate it** with your ABI (it wasn't its
  decoding path). → you get the bare selector.

Moral: in a dApp, **don't assume the revert error always comes decoded**. Carry the
ABI yourself and have a `parseError` fallback for the raw `data`. (In the bridge we
unify both paths in `humanizeError`.)

---

## 3. The cross-cutting technique: ask the chain for the truth

The two problems above share a common lesson. Facing a symptom in the UI, the
temptation is to touch the UI code. **Ask the chain first**, which is the single
source of truth, and that way you split the problem into layers:

> Is the on-chain state correct? Then the bug is in the read/decode/UI layer, **not**
> in the contract. And vice versa.

The tools: `cast code` (is there a contract, and which one?), `cast call` (what does
this function actually return?). They're **read-only**, instant and gas-free: the
"multimeter" of on-chain development.

`balanceOf` acted twice as an **independent witness** that exonerated the contract:

- **Phantom contract:** `balanceOf(account, id)` returned the correct value after
  buying → the *ownership* was correctly recorded on-chain. So the base contract
  worked; the `purchasedTotal` failure wasn't "the purchase isn't saved", but "that
  function doesn't exist in this bytecode". That redirected the search to the
  **deployment/state**, not the logic.
- **Progress panel:** the purchase was confirmed with `cast call balanceOf`
  (correct), while `loadProgress` blew up. Same conclusion: the contract did its job;
  the problem lived in the bridge's **read layer** (a function that didn't exist in
  the deployed contract + lack of robustness).

As a practical corollary we made `loadProgress` **robust**: each read is isolated
and, if one fails, it shows `n/a` in that field without taking down the inventory or
the store. A failure in one layer must not propagate and mask what works.

**The reflex to internalize:** UI symptom → `cast code` + `cast call` → is on-chain
OK? → decide which layer the bug is in → only then touch code.

---

## 4. Recorded decisions and reminders

### The bow is NOT a requirement for the quiver
For the record, to avoid future confusion: in `GameStore`, the quiver and bow
dependencies are **independent**. The bow is **only** required to buy **arrows**. The
current rules are:

| Purchase | Requirement | Does NOT require |
|----------|-------------|------------------|
| `carcaj_5` | (none) | — |
| `carcaj_10` | own `carcaj_5` | bow |
| `carcaj_20` | **Archer** achievement | bow |
| `flecha` | own a **bow** + own at least one **quiver** | a capacity check (moved to Unreal) |
| `pocion_vida` / `pocion_mana` | (none on-chain) | empty bottle (rule moved to Unreal) |

You can buy and upgrade quivers without a bow; it makes sense (the quiver is a
container; the bow is the weapon). The bow↔arrow coupling is the only one among those
items.

> Two rules that used to be here are now gone from the contract (see
> [03 §0 and §1.2](./03-achievements-and-dependencies.md)): the **arrow capacity
> limit** (how many arrows fit in the quiver) and the **empty-bottle-consumes-into-a-
> potion** rule both moved to the game layer (Unreal, validated against the savegame).
> On-chain, arrows only require bow + quiver, and potions are sold freely.

### The correct flow when changing contracts
Most of this phase's headaches came from **testing against a contract that wasn't the
one you thought**. The safe flow after touching `src/*.sol`:

1. `forge test` — make it pass green.
2. **Regenerate the bridge ABIs** (`forge inspect … abi --json` →
   `bridge/public/abi/`). The bridge uses the ABI, not the `.sol`; if you don't
   regenerate it, it decodes with an old ABI.
3. **Redeploy the new version.** With the hardened script (§1) it's enough to restart
   `start-local.sh`: it detects the version mismatch and redeploys on its own. (If in
   doubt: `rm -rf .anvil` forces a clean chain.)
4. **MetaMask → Clear activity data** if it gives *"nonce too high"* (its nonce cache
   doesn't know the chain restarted).
5. **Reload the browser** to pick up the new `app.js`/ABIs.

Skipping step 2 or 3 is exactly what produced the phantom contract and the stale ABI.
Having it as a checklist avoids repeating both.

---

## Summary for the demo

What this phase demonstrates isn't that "it works", but that **we understand how it
fails**:

- Persistent state can serve an **old version** of the contract → we detect the
  version by probing a getter, not just by checking for bytecode.
- "The contract reverted" has **two opposite flavors**: the function doesn't exist
  (`data 0x`) vs. it reverts on purpose with a custom error (raw selector) — and they
  are diagnosed and fixed differently.
- The chain is the **source of truth**: `cast code`/`cast call` separate the contract
  bug from the UI bug before you touch a line.
- Convenient infrastructure (persistence) **brings its own risk**; the craft is in
  anticipating and hardening it, not in avoiding the feature.
