# 01 — The smart contract: `GameStore` (ERC-1155)

> Part of the project's learning guide. This document explains **why** the
> contract is built the way it is, not just what it does. If you're coming from
> reading the code, here you'll find the reasoning behind each decision.

---

## 1. What the contract does and its role in the architecture

`GameStore` is the **in-game item store, lived on-chain**. Each item (a sword, a
shield, a potion…) is an ERC-1155 *token*, and the balance of that token for an
address represents how many units that player owns.

The contract does four things:

1. **Keeps a catalog** of items with their price (managed by the *owner*).
2. **Sells** items: the player pays ETH and receives the tokens (`buy`).
3. **Allows burning** items to empty inventory (`burn`).
4. **Lets the owner withdraw** the proceeds (`withdraw`).

On top of that base, `buy` also enforces **dependency rules** and mints
**achievements** — both covered in depth in
[03 — Achievements and dependencies](./03-achievements-and-dependencies.md).

### Its role in the global architecture

The contract is the **source of truth for item ownership**. It doesn't trust the
game client: the truth of "how many swords I have" lives on the blockchain, not in
the game's memory (which could be tampered with). The full flow is:

```
Game (Unreal)  →  Local web bridge (Node + ethers.js)  →  MetaMask  →  GameStore (on-chain)
```

The game never talks to the chain directly nor handles keys: it delegates to the
bridge, which uses MetaMask so the player **signs** the purchase transactions
against `GameStore`. This document covers only the last piece, the contract.

---

## 2. Why ERC-1155 (and not ERC-721) for an inventory

The three most common token standards:

| Standard | Model | Typical example |
|----------|-------|-----------------|
| **ERC-20** | A single token type, fungible | A coin / currency |
| **ERC-721** | Each token is **unique** (NFT), 1 contract per collection | Unique collectible art |
| **ERC-1155** | **Multiple token types** in one contract; each type can have many units | Game inventories |

For a game inventory, ERC-1155 fits much better than ERC-721:

- **An inventory is "semi-fungible".** It doesn't matter *which* specific potion you
  have; what matters is that you have *5 potions*. That's a balance
  (`itemId → quantity`), not 5 unique NFTs. ERC-1155 models exactly that; ERC-721
  would force you to mint a distinct NFT per unit.
- **A single contract for the whole catalog.** With ERC-721 you usually deploy one
  contract per collection. With ERC-1155, all items (swords, shields, potions, and
  whatever you add tomorrow) live in **one contract**, identified by `itemId`. Fewer
  deployments, fewer addresses to manage.
- **Cheaper on gas.** Minting 100 potions in ERC-1155 is incrementing a balance; in
  ERC-721 it would be 100 NFTs with 100 ownership entries. ERC-1155 also supports
  **batch** operations (`balanceOfBatch`, `safeBatchTransferFrom`), ideal for moving
  several items at once.

**When you WOULD want ERC-721:** if each item were truly unique and irreplaceable
(e.g. "the Legendary Sword #1 with its own history"). For a store with repeatable
stock, ERC-1155 is the natural choice.

---

## 3. Why we inherit from each OpenZeppelin contract

[OpenZeppelin](https://docs.openzeppelin.com/contracts/5.x/) are reference
implementations, audited and maintained. **We don't reinvent the standards**: we
inherit from proven code and only add our store logic on top.

```solidity
contract GameStore is ERC1155, ERC1155Burnable, Ownable, ReentrancyGuard { ... }
```

### `ERC1155` — the multi-token standard
Provides all the standard machinery: balances (`balanceOf`, `balanceOfBatch`), safe
transfers, operator approvals (`setApprovalForAll`), internal hooks (`_mint`,
`_burn`, `_update`) and metadata support (`uri`). On this base, we only write the
purchase/catalog logic. Its constructor takes a metadata `uri`.

### `ERC1155Burnable` — burning tokens
Extension that adds `burn(account, id, amount)` and `burnBatch(...)`. We inherit it
to satisfy the "empty inventory" requirement **without writing our own burn logic**.
It brings something critical for security: **it already includes permission
control**. Only the owner of the tokens (or an approved operator) can burn them; if
someone else tries, it reverts with `ERC1155MissingApprovalForAll`. That's why we
didn't have to add any check: the extension does it for us (and a test verifies it).

### `Ownable` — access control
Gives the "contract owner" concept and the `onlyOwner` modifier. We use it to
protect the administrative functions: `setItem` (manage the catalog) and `withdraw`
(withdraw funds). In OpenZeppelin **v5** the constructor requires the initial owner
explicitly: `Ownable(msg.sender)` makes the deployer the owner. (In v4 it was
implicit; this is an important change between versions.)

### `ReentrancyGuard` — the reentrancy lock
Provides the `nonReentrant` modifier, which prevents a function from being
re-entered before it finishes. We apply it to `buy`, because `buy` makes external
ETH calls (the excess refund and, indirectly, the achievements mint). See
[03 §1.1](./03-achievements-and-dependencies.md) for the full reentrancy discussion.

> **Key idea:** each base contract solves **one responsibility**
> (tokens / burning / permissions / reentrancy) and we compose them by inheritance.
> This is contract composition, the usual pattern in Solidity.

---

## 4. Function-by-function walkthrough

### `setItem(uint256 itemId, uint256 price)` — *onlyOwner*

```solidity
function setItem(uint256 itemId, uint256 price) external onlyOwner {
    priceOf[itemId] = price;
    isListed[itemId] = true;
    emit ItemListed(itemId, price);
}
```

Registers a new item or **updates** the price of an existing one. It marks
`isListed[itemId] = true` to record that the item exists in the catalog. Only the
owner can call it (`onlyOwner`). It emits `ItemListed` so external clients (the
bridge, an indexer) can react.

### `buy(uint256 itemId, uint256 quantity)` — *payable, nonReentrant*

```solidity
function buy(uint256 itemId, uint256 quantity) external payable nonReentrant {
    // 1) CHECKS
    if (!isListed[itemId]) revert ItemNotListed(itemId);
    if (quantity == 0) revert InvalidQuantity();
    uint256 cost = priceOf[itemId] * quantity;
    if (msg.value < cost) revert InsufficientPayment(cost, msg.value);
    _checkDependencies(itemId);

    // 2) EFFECTS
    _mint(msg.sender, itemId, quantity, "");
    purchasedTotal[msg.sender][itemId] += quantity;
    totalSpent[msg.sender] += cost;
    emit ItemPurchased(msg.sender, itemId, quantity, msg.value);

    // 3) INTERACTIONS (external calls, always last)
    _checkAchievements(msg.sender);            // mint achievements if milestones met
    uint256 excess = msg.value - cost;
    if (excess > 0) {                          // refund the change
        (bool ok,) = payable(msg.sender).call{value: excess}("");
        if (!ok) revert RefundFailed();
        emit ExcessRefunded(msg.sender, excess);
    }
}
```

The heart of the store. It's `payable` because it **receives ETH**. It validates in
order:

1. **That the item exists** (`isListed`) → if not, `ItemNotListed`.
2. **That the quantity is > 0** → if not, `InvalidQuantity` (avoids empty purchases).
3. **That the payment covers the cost** (`msg.value >= price * quantity`) → if not,
   `InsufficientPayment(required, sent)`.
4. **That the dependency rules hold** (`_checkDependencies`) — e.g. you need a bow +
   a quiver to buy arrows. These are covered in
   [03](./03-achievements-and-dependencies.md).

If everything passes, it **mints** (`_mint`) `quantity` units of the `itemId` to the
buyer (`msg.sender`), updates the accumulated counters (`purchasedTotal`,
`totalSpent`), mints any earned achievements, and **refunds the excess** if the
player overpaid. The structure follows the *checks-effects-interactions* pattern:
validate first, change state next, external calls last. The **why** of that ordering
(and of `nonReentrant`) is the refund/reentrancy discussion in
[03 §1.1](./03-achievements-and-dependencies.md).

### `burn(address account, uint256 id, uint256 value)` — *inherited*

We didn't write it: it comes from `ERC1155Burnable`. The player calls it with their
own address to empty inventory: `burn(myAddress, itemId, quantity)`. The extension
guarantees only the token owner (or an approved operator) can burn them.

### `withdraw()` — *onlyOwner*

```solidity
function withdraw() external onlyOwner {
    uint256 balance = address(this).balance;
    if (balance == 0) revert NoFundsToWithdraw();

    (bool ok,) = payable(owner()).call{value: balance}("");
    if (!ok) revert WithdrawFailed();

    emit FundsWithdrawn(owner(), balance);
}
```

Sends **all** the collected ETH to the owner. Reverts with `NoFundsToWithdraw` if
there's no balance, and with `WithdrawFailed` if the transfer fails. The reason for
the `call` is explained in the trade-offs section.

---

## 5. Design decisions and their trade-offs

### 5.1 `isListed` separate from the price

We use **two** mappings: `priceOf[itemId]` and `isListed[itemId]`.

```solidity
mapping(uint256 => uint256) public priceOf;
mapping(uint256 => bool)    public isListed;
```

The tempting alternative would be "if the price is 0, the item doesn't exist". But
that **conflates two distinct concepts**: "free item" and "nonexistent item". With a
separate existence flag we can list items with price 0 (promotions, free items)
without ambiguity, and the "item exists" check is explicit and readable.

- **Trade-off:** an extra mapping costs slightly more gas in `setItem` (an
  additional `SSTORE`). In exchange we gain clarity and a correct data model. For a
  store, it's worth it.

### 5.2 Custom errors instead of `require(string)`

```solidity
error InsufficientPayment(uint256 required, uint256 sent);
...
if (msg.value < cost) revert InsufficientPayment(cost, msg.value);
```

Since Solidity 0.8.4 there are *custom errors*. Compared to the classic
`require(cond, "message")`:

- **Cheaper on gas**: an error is identified by a 4-byte selector, not by a text
  string stored in the bytecode.
- **They carry data**: `InsufficientPayment(required, sent)` tells you how much was
  needed and how much was sent. That's gold for debugging and for the game UI to
  show a useful message.
- **Trade-off:** they're a bit less "self-explanatory" if you only look at the error
  hash in an explorer without the ABI. With the ABI (which we do have), they decode
  perfectly.

### 5.3 The `call` pattern in `withdraw`

```solidity
(bool ok,) = payable(owner()).call{value: balance}("");
if (!ok) revert WithdrawFailed();
```

There are three ways to send ETH: `transfer`, `send` and `call`. Historically
`transfer` was used, but it **forwards only 2300 gas**. If the owner were a contract
(e.g. a multisig like Gnosis Safe), its receive function needs more gas and
`transfer` would fail. The current recommendation is to use **`call`**, which
forwards all available gas, and **check the returned boolean** (we do: if `!ok`, we
revert).

- **Trade-off / caution:** `call` opens the door to *reentrancy* (the recipient
  could re-enter). Here it's safe because `withdraw` doesn't depend on mutable state
  after the call and is protected by `onlyOwner`. In more complex functions we add a
  `nonReentrant` guard (from `ReentrancyGuard`) — which is exactly what `buy` does,
  since it also sends ETH (the refund).

### 5.4 The overpayment IS refunded (checks-effects-interactions)

```solidity
if (msg.value < cost) revert InsufficientPayment(cost, msg.value);
_mint(...);
...
uint256 excess = msg.value - cost;
if (excess > 0) {
    (bool ok,) = payable(msg.sender).call{value: excess}("");
    if (!ok) revert RefundFailed();
    emit ExcessRefunded(msg.sender, excess);
}
```

We accept `msg.value >= cost`. If the player **overpays**, the excess is **refunded**
at the end of `buy`. The refund is placed **after** the mint and all state changes
(checks-effects-interactions), and `buy` is marked `nonReentrant`, so the external
`call` cannot be exploited via reentrancy.

> Earlier versions of this doc listed the refund as a *pending improvement* — it is
> now implemented. The deeper security reasoning (why the refund goes last, CEI +
> `nonReentrant` as a "double belt") lives in
> [03 §1.1](./03-achievements-and-dependencies.md).

### 5.5 Vendoring `lib/` (no git submodules)

We install OpenZeppelin with `forge install --no-git`, which **copies** the files
into `lib/` and versions them as normal code, instead of using git *submodules*.

- **Why:** the contract lives inside a **monorepo** that already has its own `.git`
  at the root. Using submodules would create **nested** git submodules, which are
  confusing to clone and maintain.
- **Advantage:** the repo is **self-contained** and reproducible — whoever clones it
  has exactly the OZ version that compiles, with no extra steps (`git submodule
  update`).
- **Trade-off:** `lib/` adds many files to version control (in our case ~780). The
  repo is heavier and dependency-update diffs are large. For a learning project, the
  simplicity is worth it.

---

## 6. Key Solidity concepts that appear

- **wei / ether.** ETH is measured internally in **wei**; `1 ether = 1e18 wei`.
  Solidity has the `ether` suffix (`0.01 ether` = `10_000_000_000_000_000` wei),
  which avoids errors counting zeros. **All prices and payments are in wei.**
- **`payable`.** Marks a function (or address) able to **receive ETH**. `buy` is
  `payable`; without that word, sending ETH in the call would revert. To send ETH to
  an address with `.call{value:...}` that address must be `payable`.
- **`msg.value`.** The ETH (in wei) **sent along with the call**. In `buy` we compare
  it against `cost` to validate the payment.
- **`msg.sender`.** **Who** makes the current call. It's the buyer in `buy` (receives
  the tokens), and in the constructor it's the deployer (becomes owner via
  `Ownable(msg.sender)`).
- **mint / `_mint`.** "Minting" = **creating** new tokens and assigning them to an
  address. `_mint(to, id, amount, data)` increments the buyer's balance. It's
  internal (`_`) because only our `buy` logic should be able to mint, never someone
  from outside.
- **`vm.*` cheatcodes (in the script and tests).** `vm` is a special Foundry object
  with "tricks" for test/scripting environments:
  - `vm.startBroadcast()` / `vm.stopBroadcast()` — delimit the operations that become
    **real transactions** signed and sent to the network.
  - (In tests) `vm.prank(addr)` — makes the **next** call appear to come from `addr`
    (to simulate different users); `vm.deal(addr, x)` — gives balance;
    `vm.expectRevert(...)` — asserts the call must revert with a certain error.

---

## 7. Local deployment flow with Anvil

**Anvil** is Foundry's local Ethereum node (chain id **31337**), meant for
development. It deploys and verifies the contract on your machine without touching
any real network.

### Start the node (terminal A)

```bash
anvil
```

Prints 10 test accounts with 10000 ETH each and listens on
`http://127.0.0.1:8545`. Leave it open.

### Deploy (terminal B)

```bash
forge script script/DeployGameStore.s.sol:DeployGameStore \
  --rpc-url http://127.0.0.1:8545 \
  --private-key 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80 \
  --broadcast
```

- `--rpc-url` → which node to send the transactions to.
- `--private-key` → account that signs and pays gas; becomes the contract **owner**.
- `--broadcast` → without it, `forge script` only **simulates**; with it, it sends
  for real.

The script deploys **both** `GameStore` and `Achievements`, wires them together
(`setAchievements` + `setMinter`), and preloads the full catalog of 10 items (ids
0–9; e.g. Sword 0.01, Shield 0.008, Bow 0.012, Arrow 0.0005 ETH…). On a freshly
started Anvil, the first deployment of account #0 is **deterministic**: it always
lands on `0x5FbDB2315678afecb367f032d93F642f64180aa3` (which the bridge hardcodes).
`Achievements`, deployed second (nonce 1), lands on its own deterministic address.

### Verify with `cast call` (reads, no gas)

```bash
ADDR=0x5FbDB2315678afecb367f032d93F642f64180aa3
RPC=http://127.0.0.1:8545

cast call $ADDR "priceOf(uint256)(uint256)" 0 --rpc-url $RPC  # 10000000000000000 (0.01 ETH)
cast call $ADDR "isListed(uint256)(bool)"   0 --rpc-url $RPC  # true
cast call $ADDR "isListed(uint256)(bool)"  99 --rpc-url $RPC  # false (nonexistent)
cast call $ADDR "owner()(address)"            --rpc-url $RPC  # account #0
```

`cast call` runs **read-only** functions: no gas, no blocks mined, it just queries
the current state.

### ⚠️ Security note about the test keys

The private key used above (`0xac09…ff80`, account `0xf39F…2266`) is one of Anvil's
**public test keys**: literally everyone knows them. They're **only** for local
development.

- **Never** use a real private key in commands, code, scripts or versioned `.env`
  files.
- **Never** send real funds to an address derived from a test key: they can be
  stolen instantly.
- In a clean flow, the key is loaded from a `.env` (which is in `.gitignore`) and
  passed as `--private-key $PRIVATE_KEY`, never pasted in the clear.

---

## Status of this phase

✅ `GameStore.sol` — ERC-1155 with catalog, purchase (with dependency rules,
achievement minting and excess refund), burn and withdraw.
✅ 37 tests green (`forge test`), across `GameStore`, `Achievements` and the rules.
✅ Deployment script verified against Anvil (deploy of both contracts + `cast call`).

**Next piece:** the local web bridge (`/bridge`) that connects the game with
MetaMask and signs the purchases against this contract —
[02 — The web bridge](./02-bridge.md).
