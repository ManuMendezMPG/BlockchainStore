# 02 — The web bridge: MetaMask ↔ contract

> Part of the project's learning guide. Continues
> [01 — The smart contract](./01-smart-contract.md). Here we explain the local web
> that connects the player with the `GameStore` contract, focusing on the **why**
> of each decision. The HTTP API for the game and the SIWE login are later pieces,
> covered in [05](./05-api-and-unreal-client.md) and [06](./06-login-siwe.md).

---

## 1. What the bridge is and what problem it solves

The game (Unreal Engine) **cannot talk to MetaMask directly**. MetaMask is a
**browser** extension: it lives in the context of a web page and exposes its API
(`window.ethereum`) only to JavaScript running on that page. An Unreal executable
has no `window.ethereum`, no embedded browser with the extension, and no native way
to ask the user to sign a transaction.

Besides, **the game must not touch the player's private key**. If the game signed
transactions, it would have to custody the key — exactly what we want to avoid. The
key must stay where the user already trusts having it: MetaMask.

**The bridge is the intermediary** that solves both problems:

```
┌──────────────┐   HTTP/local    ┌───────────────────────────┐  window.ethereum  ┌──────────┐
│ Unreal game  │ ───────────────►│  Web bridge (Node + web)  │ ────────────────► │ MetaMask │
│  (client)    │ ◄───────────────│  HTML + JS + ethers.js    │ ◄──────────────── │ (signs)  │
└──────────────┘   result        └───────────────────────────┘                   └────┬─────┘
                                                                                       │ JSON-RPC
                                                                                       ▼
                                                                                  ┌──────────┐
                                                                                  │ GameStore│
                                                                                  │ on-chain │
                                                                                  └──────────┘
```

The bridge is a **locally served web page**. It has `window.ethereum`, so it can ask
MetaMask to connect the account and sign. The game only asks it for actions ("buy
item 0") and receives results, **without ever seeing the key**.

> This document covers the browser↔contract piece. On top of it, the bridge later
> grows an **HTTP/JSON API** so the game can drive purchases and log in — see
> [05 — API and Unreal client](./05-api-and-unreal-client.md) and
> [06 — SIWE login](./06-login-siwe.md).

---

## 2. Architecture of the piece

```
bridge/
├── server.js                 # Node server: serves the web AND exposes the /api/* JSON API
└── public/
    ├── index.html            # Structure + styles + ethers.js load (CDN)
    ├── app.js                # Logic: connect, read, buy, burn, progress, intent consumer, SIWE
    └── abi/
        ├── GameStore.json    # ABI extracted from Foundry artifacts
        └── Achievements.json # ABI of the achievements contract
```

Four components, each with a responsibility:

1. **Node server** (`server.js`). Serves the **files** (`index.html`, `app.js`, the
   ABIs) over HTTP at `http://localhost:8787`, and also exposes the `/api/*` JSON
   API (docs 05/06). Why do we need a server for such a simple web at all? Because
   **opening the HTML as `file://` doesn't work with MetaMask**: under that scheme
   the browser doesn't inject `window.ethereum` reliably and blocks `fetch()` (which
   we use to load the ABI). Serving over `http://localhost` gives a real web
   *origin* and everything works.

   > On dependencies: the **reads** in `server.js` are done with raw `eth_call` over
   > JSON-RPC, with **zero dependencies**. The SIWE login (doc 06) later adds two
   > libraries (`siwe`, `ethers`) *on purpose*, because crypto verification must not
   > be homemade. That trade-off is argued in [06 §6](./06-login-siwe.md).

2. **Web page** (`index.html`). The structure (connect button, store list, inventory
   list, progress panel) and the styles. Loads ethers.js and `app.js`.

3. **JS logic** (`app.js`). Connects to MetaMask, reads the contract state (store and
   inventory), builds the buy/burn transactions and handles errors. It's the brain
   of the bridge. It has since grown to also show progress/medallions and to act as
   the automatic consumer of the game's purchase/login intents (docs 05/06).

4. **The ABIs** (`abi/*.json`). The "interface contract": they tell ethers what
   functions `GameStore` and `Achievements` have, what arguments they take and what
   they return. They are **extracted from the Foundry artifacts** (generated on
   compile):

   ```bash
   cd contracts
   forge inspect src/GameStore.sol:GameStore abi --json > ../bridge/public/abi/GameStore.json
   forge inspect src/Achievements.sol:Achievements abi --json > ../bridge/public/abi/Achievements.json
   ```

   > **Why extract it and not hand-write it:** the ABI is the source of truth for the
   > interface. If you change the contract and recompile, you regenerate the ABI with
   > that command and the web stays in sync. Hand-writing it is fragile and prone to
   > type errors.

---

## 3. Key ethers.js concepts

### Provider vs Signer — the fundamental distinction

- **Provider** = **read-only** connection to the blockchain. Queries state
  (balances, prices, blocks). **It cannot sign anything.** In the web we use two:
  - `new ethers.JsonRpcProvider(RPC_URL)` → talks **directly** to the Anvil node. We
    use it to show the store prices **before connecting the wallet**.
  - `new ethers.BrowserProvider(window.ethereum)` → talks **through MetaMask**.

- **Signer** = represents **a specific account** and **can sign transactions**. It's
  obtained from MetaMask with `provider.getSigner()`. Most importantly: **the private
  key lives inside MetaMask**. The signer only *requests* signatures; your code never
  sees or touches the key.

> Mental rule: if you only **read**, a *provider* is enough. If you're going to
> **change state** (and therefore sign and pay gas), you need a *signer*.

### How it connects to MetaMask

Three steps, visible in `connectWallet()`:

```js
// 1) Wrap the object MetaMask injects (EIP-1193 standard).
const browserProvider = new ethers.BrowserProvider(window.ethereum);

// 2) Request permission to access the accounts → OPENS THE MetaMask POPUP.
await browserProvider.send("eth_requestAccounts", []);

// 3) Get the signer (the connected account that will sign).
const signer = await browserProvider.getSigner();
const account = await signer.getAddress();
```

- `window.ethereum` is the API that the MetaMask extension **injects** into the page.
- `eth_requestAccounts` is the standard request that triggers the "Connect this site
  to your wallet?" dialog. Without it, we have no permission to see the account.
- After connecting, we check the network with `await browserProvider.getNetwork()`
  and warn if `chainId !== 31337n`. **In ethers v6 chainIds are `BigInt`** (hence the
  trailing `n`): you must compare them with BigInt, not with plain numbers.

We also listen for changes so we don't keep stale state:

```js
window.ethereum.on("accountsChanged", () => location.reload());
window.ethereum.on("chainChanged",    () => location.reload());
```

Reloading the page is the simplest and most robust way to reflect that the user
switched account or network in MetaMask.

### Read vs Write

| | **Read** (`view` function) | **Write** (transaction) |
|---|---|---|
| Uses | a **provider** | a **signer** |
| Opens MetaMask? | No | Yes (asks for a **signature**) |
| Costs gas? | No | Yes |
| Result | immediate value | you must **wait** for it to be mined |

```js
// READ — contract connected to a provider. Immediate, no gas, no popup.
const price = await readContract.priceOf(0);           // BigInt (wei)
const balance = await readContract.balanceOf(account, 0);

// WRITE — contract connected to the signer. MetaMask asks to sign; we await confirmation.
const tx = await writeContract.buy(0, 1, { value: price });
await tx.wait();   // ⏳ until the transaction is included in a block
```

We create **two instances** of the contract to make the difference explicit:

```js
readContract  = new ethers.Contract(CONTRACT_ADDRESS, abi, readProvider); // reads only
writeContract = new ethers.Contract(CONTRACT_ADDRESS, abi, signer);       // reads and writes
```

### Building a `payable` transaction from JS

`buy(itemId, quantity)` is `payable`: it receives ETH. In ethers, the ETH to send
goes in an **overrides** object as the **last argument** of the call:

```js
const tx = await writeContract.buy(item.id, 1, { value: priceWei });
//                                ───┬───  ┬   ────────┬─────────
//                              arguments   overrides: { value } → arrives as msg.value
```

- The first arguments (`item.id`, `1`) are the function parameters.
- `{ value: priceWei }` indicates how much ETH (in **wei**) to attach. That value
  reaches the contract as **`msg.value`** — exactly what `buy` compares against
  `price * quantity`.
- We got `priceWei` earlier by reading `priceOf(id)`, so we pay exactly the item's
  price. (If we send too much, the contract now **refunds the excess** — see doc 01
  §5.4.)

`burn(account, id, value)` is **not** payable, so it's called without `{ value }`.
The player burns their own tokens, and the contract's `ERC1155Burnable` extension
checks that they're the owner (see doc 01).

---

## 4. Error handling: `humanizeError` and the full ABI

Things fail: the user rejects the signature, is on the wrong network, has no funds,
or the contract reverts. A good UI **translates** those failures into something
readable. The `humanizeError(err)` function does that mapping:

```js
if (err?.code === "ACTION_REJECTED") return "You rejected the signature in MetaMask.";
if (err?.code === "INSUFFICIENT_FUNDS") return "Insufficient funds…";
if (err?.revert?.name) { /* contract custom error → specific message */ }
if (err?.code === "NETWORK_ERROR") return "Is Anvil running?";
```

ethers v6 normalizes many errors with a stable **`code`**
(`ACTION_REJECTED`, `INSUFFICIENT_FUNDS`, `NETWORK_ERROR`…), which lets us
distinguish them without parsing fragile strings.

### Why include the **full** ABI (with the custom errors)

When the contract reverts with a *custom error* (e.g. `InsufficientPayment`,
`ItemNotListed`, `ERC1155MissingApprovalForAll`), what travels over the wire is a
**4-byte selector** + its encoded data. On its own, that's unreadable.

If the ABI you gave ethers **includes the definition of those errors**, ethers can
**decode them** and fill in `err.revert.name` and `err.revert.args`:

```js
case "InsufficientPayment":
  return `Insufficient payment: the contract required ${r.args?.[0]} wei.`;
case "ItemNotListed":
  return `That item does not exist in the catalog (id ${r.args?.[0]}).`;
```

That's why we keep the **whole** ABI (not just the 4 functions we use): it carries
all the contract's and OpenZeppelin's errors, and that turns a cryptic `0xb99e2ab7`
into *"Insufficient payment: the contract required 10000000000000000 wei"*. Without
the full ABI, you'd only see a hash. **The cost is trivial** (a slightly larger JSON
file) and the DX improvement is huge. (The subtleties of *when* ethers decodes for
you vs. when it hands you the raw selector are covered in
[04 §2](./04-debugging-and-learnings.md).)

---

## 5. Stack decisions and their whys

### ethers v6 via CDN, no bundler
We load ethers as a **UMD** build from a CDN:

```html
<script src="https://cdn.jsdelivr.net/npm/ethers@6.13.4/dist/ethers.umd.min.js"></script>
```

- **UMD** exposes a global `ethers` variable (`window.ethers`), so `app.js` uses it
  directly without `import`.
- **Why:** keep the front-end stack minimal. No `npm`, no `webpack`/`vite`, no build
  step for the page. You edit a `.js`, reload the browser, done. For a learning piece
  and a local bridge, simplicity wins.
- **Trade-off:** you depend on a CDN (you need network the first time; then the
  browser caches) and you pin the version in the URL. In production you'd usually
  *bundle* and serve ethers from your own domain, but here it's not worth the
  complexity.

### Server: dependency-free reads, deliberate deps only for SIWE
`server.js` resolves the on-chain **reads** using **only Node's built-in modules**
(`http`, `fs`, `path`, `crypto`, `url`), encoding the `eth_call` arguments by hand
(everything is `address`/`uint256`, so it's trivial).

- **Why:** for reads, avoiding `npm install` keeps the surface tiny — one file, zero
  third-party maintenance or vulnerability exposure.
- **The exception:** the SIWE login (doc 06) adds `siwe` and `ethers`. Cryptographic
  verification (ecrecover, strict EIP-4361 parsing) must **not** be homemade, so
  there we lean on proven libraries — the reasoning is in [06 §6](./06-login-siwe.md).
- **Bonus in this environment:** doing reads with built-ins sidesteps a WSL
  permission issue (folders created as `root` from Windows) that could affect an
  `npm install` running as your WSL user. The install is made **conditional** by the
  Windows launcher (`start-bridge.ps1`), which only runs `npm install` when needed.

### The contract's deterministic address
`CONTRACT_ADDRESS` is fixed in `app.js` (and in `server.js`):

```js
const CONTRACT_ADDRESS = "0x5FbDB2315678afecb367f032d93F642f64180aa3";
```

- **Why it works:** a contract's address is derived from `(deployer address, nonce)`.
  On a **freshly started** Anvil, account #0 has nonce 0, so its **first** deployment
  **always** lands on that address. It's stable across restarts *as long as* you
  deploy the same thing in the same order with the same account.
- **When it changes:** if you deploy from another account, or it isn't the first
  deployment (different nonce). Then you must update the constant with the address
  `forge script` prints (or read it from
  `contracts/broadcast/.../run-latest.json`).
- **Trade-off:** hardcoding is the simplest thing for local development. On
  testnet/mainnet the address would be managed per network (e.g. a deployments file
  keyed by chainId).

---

## 6. Environment note: Node on Windows, Anvil on WSL

⚠️ **Important on this machine**, because the pieces live on different systems:

- **Anvil and Foundry run on WSL** (Ubuntu). That's where the contract is compiled,
  tested and deployed.
- **Node is installed on Windows**, not on WSL. That's why the **bridge server is
  started from a Windows terminal** (PowerShell), not from WSL.
- **MetaMask runs in the Windows browser.**

It works because **`localhost` is shared between Windows and WSL**: a service
listening on `127.0.0.1:PORT` in WSL is reachable from Windows at `localhost:PORT`
and vice versa. So:

| Piece | Where it listens | Who consumes it |
|-------|------------------|-----------------|
| Anvil (RPC) | WSL, `127.0.0.1:8545` | the browser (Windows) and forge (WSL) |
| Bridge server | Windows, `localhost:8787` | the browser (Windows) |

**Which terminal starts each thing:**

| Terminal | Where | Command |
|----------|-------|---------|
| A | WSL | `anvil` (or `scripts/start-local.sh`) |
| B | WSL | `forge script … --broadcast` (deploy) |
| C | **Windows (PowerShell)** | `node server.js` in the `bridge` folder |

To start the server from PowerShell, the folder path is the UNC path of the WSL
filesystem:

```powershell
cd \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge
node server.js
# if cd to UNC gives trouble:  node \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge\server.js
```

> The browser and MetaMask also talk to Anvil over `localhost` (reads and sending
> signed transactions). Since Anvil enables **CORS**
> (`Access-Control-Allow-Origin: *`), the browser can call its RPC without blocks.

---

## 7. Startup routine for the full local environment

From scratch, in order. The project ships two helper scripts that automate this;
they're worth using instead of the raw commands.

1. **Start Anvil + deploy** (Terminal A, WSL):
   ```bash
   bash scripts/start-local.sh
   ```
   This starts Anvil **with state persistence** on disk (`.anvil/state.json`, so your
   inventory survives restarts), waits for the RPC, and **decides whether to deploy**
   based on the contract version it finds. It prints the deployed address (should be
   the usual deterministic one) and leaves Anvil running. The version-detection logic
   (and why persistence needs it) is explained in
   [04 — Debugging and learnings](./04-debugging-and-learnings.md).

   > The raw equivalent, if you prefer to run it by hand, is `anvil` in one terminal
   > and the `forge script … --broadcast` deploy (see doc 01 §7) in another.

2. **Start the bridge server** (Terminal C, **Windows PowerShell**):
   ```powershell
   \\wsl.localhost\Ubuntu\home\manumendez\projects\bridge\start-bridge.ps1
   ```
   This runs `npm install` **only when needed** (first run, or `package.json`
   changed) and then `node server.js`.

3. **Open the web**: `http://localhost:8787`. Connect MetaMask (Anvil network, chain
   id 31337) and use the store.

### ⚠️ Restarting Anvil and the persistent state

With `start-local.sh`, Anvil's state is **persisted to disk**, so restarting no
longer wipes your contract by default — the script reloads the previous state. But
that persistence brings its own risk: the saved state can hold an **old version** of
the contract. The script guards against this by probing a getter that only exists in
the current version and, if it's stale, redeploying on a clean chain. The full story
(the "phantom contract" bug) is in [04 §1](./04-debugging-and-learnings.md).

> If you run Anvil **by hand** (`anvil`, no `--state`), it keeps its state **in
> memory**: restarting starts the chain from scratch and your deployed contract is
> gone. Symptoms in the web: reads return 0 / "not available" or fail. **Fix:** run
> the deploy again. Since account #0 is back at nonce 0, the contract lands on the
> **same deterministic address**, so you usually **don't** need to touch
> `CONTRACT_ADDRESS` in `app.js`.

### ⚠️ "nonce too high" error in MetaMask → *Clear activity*

After restarting Anvil you'll often see a signing error like **"nonce too high"** (or
transactions that hang).

- **Why it happens:** MetaMask **caches the nonce** of your account per network. If
  before restarting you had made, say, 5 transactions, MetaMask thinks your next
  nonce is 5. But when Anvil restarts (from a clean chain), the chain thinks your
  account is at nonce 0. MetaMask sends with nonce 5 and the chain rejects it as "too
  high".
- **The fix:** in MetaMask, **Settings → Advanced → "Clear activity tab data"** with
  the Anvil account and network selected. This resets MetaMask's nonce counter for
  that network, without touching your funds or your key. After that, transactions
  sign with the correct nonce again.
- **Rule of thumb:** whenever the chain restarts clean → **redeploy** + **Clear
  activity** in MetaMask. These two steps avoid 90% of local-development problems.

---

## Status of this phase

✅ Node server serving the web (and, later, the JSON API — docs 05/06).
✅ MetaMask connection (`BrowserProvider` + `eth_requestAccounts` + `getSigner`).
✅ Store reads (`priceOf`/`isListed`), inventory (`balanceOf`) and progress/medallions.
✅ Purchase (`buy` payable with `value`) and empty (`burn`), signing in MetaMask.
✅ Readable error handling backed by the full ABI (custom errors).
✅ Verified end to end in the browser.

**Next pieces:** the **HTTP API** the game consumes
([05](./05-api-and-unreal-client.md)) and the **SIWE login**
([06](./06-login-siwe.md)), which let Unreal drive purchases and authenticate
instead of you operating by hand in the browser.
