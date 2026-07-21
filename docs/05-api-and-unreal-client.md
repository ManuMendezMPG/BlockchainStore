# 05 — The communication layer: bridge API and Unreal client

> Part of the project's learning guide.
> [01](./01-smart-contract.md) · [02](./02-bridge.md) · [03](./03-achievements-and-dependencies.md) · [04](./04-debugging-and-learnings.md) · [06](./06-login-siwe.md).
> Here we document **how the game talks to the blockchain**: the three-actor
> pattern, the bridge's HTTP API, and the plan to connect Unreal. With the whys.

---

## 1. The problem and the three-actor pattern

The game needs the player to **buy items on-chain**, but it hits two limits:

- **Unreal can't talk to MetaMask.** MetaMask is a **browser** extension: it only
  exposes its API (`window.ethereum`) to JavaScript on a web page. An Unreal
  executable has no browser with the extension nor a native way to request a
  signature.
- **The game must not custody the private key.** If Unreal signed, it would have to
  store the key — exactly what we want to avoid. The key must stay in MetaMask.

The solution is a **three-actor pattern**, each with a clear role and limit:

```
┌──────────┐   HTTP/JSON    ┌──────────────┐   window.ethereum   ┌──────────────┐
│  UNREAL  │ ─────────────► │    BRIDGE    │ ◄─ polls ───────────│ WEB + MetaMask│
│ (client) │ ◄───────────── │ (coordinates)│ ── intent ─────────►│   (signs)     │
└──────────┘   polling      └──────┬───────┘                     └──────┬────────┘
                                   │ read (eth_call)                    │ signs tx
                                   ▼                                    ▼
                              ┌─────────────────  blockchain (Anvil)  ─────────────┐
                              │  GameStore (ERC-1155)  +  Achievements             │
                              └────────────────────────────────────────────────────┘
```

| Actor | What it does | What it does **not** do |
|-------|--------------|-------------------------|
| **Unreal** | Requests actions (buy) and queries state, over HTTP | Doesn't sign, doesn't touch keys, doesn't talk to the chain |
| **Bridge** | Coordinates: reads the chain and stores purchase "intents" | **Doesn't custody keys**, doesn't sign |
| **Web + MetaMask** | Detects intents and **signs** the transactions | Isn't the game logic |

The underlying idea (see doc 02): the bridge is the **intermediary** that has what
Unreal lacks (access to MetaMask via the browser) without assuming what it must not
(the key).

---

## 2. The design of the bridge's HTTP API

The server (`bridge/server.js`) serves the web **and** exposes a JSON API under
`/api/*`, separate from the static files. Two families of endpoints. (For reads it
uses no dependencies; the SIWE login endpoints of doc 06 add `siwe`/`ethers`.)

### 2.1 Reads (require no signature → served by the bridge directly)

A read only needs a read-only *provider* (nothing to sign), so the bridge resolves it
on its own with `eth_call` and returns JSON.

| Endpoint | Returns |
|----------|---------|
| `GET /api/catalog` | The 10 items: `id`, `name`, `priceWei`, `priceEth`. |
| `GET /api/inventory?address=0x…` | Balance (`balanceOf`) of each item for that account. |
| `GET /api/progress?address=0x…` | `arrowsPurchased`, `totalSpent`, `quiverCapacity` and the medallions (with the Merchant's rarity). |

> Note: the values these endpoints return (item names, medallion names
> Archer/Merchant/Collector, rarity Bronze/Silver/Gold) are display strings the
> bridge builds. The JSON **keys** (`id`, `name`, `rarity`, `owned`, …) are the stable
> contract Unreal parses.

### 2.2 Purchase (requires a signature → pending → signing → done pattern)

Since the signature lives in another process (the web), the purchase **can't be
synchronous**. It's modeled as an **intent** with states, and a mailbox both sides
consult.

| Endpoint | Who calls it | For what |
|----------|--------------|----------|
| `POST /api/purchase-intent` `{address,itemId,quantity}` | **Unreal** | Registers the intent; responds `{requestId}` (state `pending`). |
| `GET /api/purchase-status?requestId=…` | **Unreal** | Polls the state: `pending` → `signing` → `done` (with `txHash`) / `error`. |
| `GET /api/pending` | **The web** | Lists the intents still `pending`. |
| `POST /api/purchase-result` `{requestId,status,txHash?,error?}` | **The web** | Advances the state: `signing` on claiming it, then `done`/`error`. |

### 2.3 What each state means (and why `signing` matters)

```
pending ──(the web claims it)──► signing ──(tx mined)──► done   (with txHash)
                                     └────(fails/reverts)──► error (with message)
```

- **`pending`** — Unreal requested the purchase; nobody has taken it yet. It appears
  in `GET /api/pending` for the web to discover.
- **`signing`** — the web has **claimed** it and is triggering MetaMask. This serves a
  **double function**:
  1. It informs Unreal that its request is progressing (someone is signing it).
  2. **It prevents double-signing:** by marking it `signing`, it stops appearing in
     `GET /api/pending`. If there are two tabs (or the consumer polls twice), the
     intent is no longer offered again → the same purchase isn't signed twice.
- **`done`** — the transaction was mined; the state carries the `txHash`. Unreal sees
  it in its polling and refreshes its inventory.
- **`error`** — the signature was rejected or the contract reverted (e.g. a dependency
  rule); the state carries a readable message.

The full flow, end to end between the three actors:

```
UNREAL                         BRIDGE                          WEB + MetaMask
  POST /purchase-intent ───────►  stores {pending}
  ◄── { requestId }
                                                  GET /pending ──► sees the intent
                                signing ◄───────── POST /purchase-result {signing}  (claims)
                                                  triggers MetaMask → the player SIGNS buy()
  GET /purchase-status ─► signing                 waits for the tx to be mined
                                done + txHash ◄─── POST /purchase-result {done, txHash}
  GET /purchase-status ─► done, txHash ✓
```

In the prototype, the web itself (`public/app.js`) acts as the automatic consumer: on
connecting MetaMask, it polls `/api/pending` every 3 s and processes the intents **of
the connected account** (claim → sign → report). In production that consumer could be
a dedicated "signing" page.

---

## 3. Polling vs WebSockets: why polling (for now)

The communication is **asynchronous** (Unreal requests, someone signs later), so we
need to notify the result. Two approaches:

- **Polling** (chosen): the client asks every X seconds "is it done yet?". It's what
  Unreal (`/purchase-status`) and the web (`/pending`) do.
- **WebSockets / SSE**: a persistent connection that **pushes** the change as soon as
  it happens.

We chose **polling** for the prototype, consciously:

- **Simpler:** they're normal HTTP requests. No management of persistent connections,
  reconnections, or socket state. The reads server stays **dependency-free** and the
  clients (including Unreal) only need an HTTP client.
- **Enough for the demo:** a purchase takes seconds (the human signs in MetaMask);
  polling every 1-3 s gives a perfectly acceptable perceived latency.
- **Trivial to implement anywhere:** a `GET` in a loop is written the same in curl,
  JS or Unreal (a Timer). It doesn't require WebSocket libraries in the engine.
- **Evolvable:** if real-time is wanted later (many events, low latency), WebSockets/
  SSE can be added **without changing the model** of `pending/signing/done` states;
  only the notification *transport* changes.

> Prototype rule: start with the simplest thing that works and doesn't box you in.
> Polling satisfies all three here.

**Cost to keep in mind:** polling generates "empty" requests while nothing changes.
At this scale (one player, second-level intervals) it's irrelevant; at large scale
it would be the moment to move to push.

---

## 4. The test-client as validation

`bridge/test-client/test-client.js` (see its README) is a **testing** tool, not part
of the product. It occupies **Unreal's exact role**: it does `POST /purchase-intent`
and then polls `/purchase-status`, showing the inventory before and after.

**What it validates:**
- That the read endpoints respond and return correct data.
- That the purchase cycle flows: `pending → signing → done`, with `txHash` back.
- That the on-chain balance changes after the purchase (reads inventory before/after,
  `Δ`).
- Clear **exit codes** to chain/automate:

  | Code | Meaning |
  |------|---------|
  | `0` | purchase `done` |
  | `1` | the contract reverted (`error`) / unexpected HTTP |
  | `2` | invalid arguments |
  | `3` | timeout (nobody signed) |
  | `4` | bridge not reachable |

**What it does NOT cover (and how it was validated separately):** the test-client
doesn't sign — it can't, just like Unreal. The **real signature with MetaMask** was
validated **manually** by opening the web and confirming in the extension. For the
automated happy-path test, that signing step was substituted with a `buy()`
transaction sent directly to the chain (what MetaMask would do), confirming that once
signed, the whole circuit closes and the balance goes up.

> Validation split: **client + API** → test-client (automatable); **signing** →
> MetaMask in the browser (manual). Together they cover the full flow.

---

## 5. Migration plan to Unreal

The big design conclusion: **the game side is just two HTTP calls.** Everything hard
(signing, contract rules, coordination) lives outside Unreal. Migrating is
translating the test-client to Unreal:

1. **`POST /api/purchase-intent`** with JSON body `{address, itemId, quantity}` →
   save the `requestId`.
2. **`GET /api/purchase-status?requestId=…`** in a loop until `done`/`error`.

In **Unreal C++** that's `FHttpModule`:

```cpp
// 1) Register the intent
FHttpModule::Get().CreateRequest();
Req->SetURL(TEXT("http://localhost:8787/api/purchase-intent"));
Req->SetVerb(TEXT("POST"));
Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
Req->SetContentAsString(TEXT("{\"address\":\"0x…\",\"itemId\":6,\"quantity\":5}"));
Req->OnProcessRequestComplete().BindUObject(this, &UStore::OnIntentReply); // extracts requestId
Req->ProcessRequest();

// 2) Polling with a Timer (e.g. every 1.5 s) until done/error
GetWorld()->GetTimerManager().SetTimer(PollTimer, this, &UStore::PollStatus, 1.5f, true);
//   PollStatus does GET /api/purchase-status?requestId=… and, on seeing "done",
//   clears the Timer and refreshes the inventory (another GET /api/inventory).
```

(In **Blueprints** it's the same with the *HTTP Request* nodes of the HTTP plugin and
a *Set Timer by Function Name* for the loop.)

What is **not** left to solve in Unreal:
- **No blockchain:** Unreal doesn't build transactions, doesn't compute gas, doesn't
  handle ABIs or addresses. That's the bridge/contract's job.
- **No signing:** the key never touches the game.
- **Only HTTP transport + JSON parsing**, which is standard engine territory.

That's why the test-client is the proof that **the path is clear**: it validates the
same endpoints and the same cycle Unreal will consume; the only new thing in the
engine will be the HTTP client, not domain logic.

---

## 6. Environment note (to resume the project)

- **The server runs on Windows.** Node is on Windows; the server listens on
  `http://localhost:8787`. **From WSL you can't reach that `localhost`** (`localhost`
  forwarding goes Windows→WSL, not the other way). Start and test the API/client
  **from Windows** (PowerShell). Anvil, on the other hand, runs on WSL and the Windows
  server does reach it via `localhost:8545`.
- **CORS open** (`Access-Control-Allow-Origin: *`) with `OPTIONS` preflight, so a
  local client like Unreal can call the API without browser/origin blocks.
- **Intents live in memory** (a `Map` in the server). It's a prototype: **they're lost
  on restart** of `server.js`. For production they'd be persisted (file/DB) and given
  expiry (TTL) and cleanup.
- **There are never keys on the server.** Reads with a provider; purchases only
  coordinated; the signature always in MetaMask.

---

## 7. The main-screen UI

The game's main screen is a UMG widget split into **four panels**, each a framed box
with a colored border and a title. The layout makes the on-chain/off-chain split
visible at a glance: what you *own* (chain) on one side, what you *do* (game) on the
other.

```
┌───────────────────────────┐   ┌───────────────────────────┐
│ INVENTORY                 │   │ MEDALS                    │
│ (what you own, on-chain)  │   │ (achievements, on-chain)  │
│  ┌──┐┌──┐┌──┐┌──┐         │   │  🏹  💰  🏆               │
│  ┌──┐┌──┐┌──┐┌──┐         │   │  Archer Merchant Collector│
│  UniformGrid, 4 columns   │   │                           │
└───────────────────────────┘   └───────────────────────────┘
┌───────────────────────────┐   ┌───────────────────────────┐
│ ACTIONS                   │   │ STORE                     │
│ (context-dependent)       │   │ (catalog, buy)            │
│  [ Use ] [ Break ] …      │   │  ┌──┐┌──┐┌──┐┌──┐┌──┐    │
│  or  [ Buy ]              │   │  WrapBox of item cards    │
└───────────────────────────┘   └───────────────────────────┘
```

### The four panels

1. **Inventory** — what the wallet owns on-chain. An icon grid laid out in a
   **`UniformGridPanel` with 4 columns**: fixed, aligned cells, one per owned item,
   with the quantity badge. Its data comes from `GET /api/inventory` (i.e. on-chain
   `balanceOf`), so it's the visual mirror of chain ownership.
2. **Medals** — the three achievements (Archer / Merchant / Collector) from
   `GET /api/progress`, shown lit or dimmed depending on whether they're unlocked
   (with the Merchant's rarity when present).
3. **Actions** — a **context-dependent** panel (see below): the buttons it shows
   depend on what's currently selected.
4. **Store** — the catalog from `GET /api/catalog`, laid out as a **`WrapBox`** of
   item cards that reflow to the available width (unlike the inventory's fixed grid).
   Each card shows the icon, name and price in ETH; selecting one lets you buy it.

### Icons

Item and medal icons come from **[game-icons.net](https://game-icons.net)** (CC0
public domain, so they're safe to ship). The two **potions are tinted in code** from a
single base flask icon rather than shipping two separate art files: the health potion
is tinted **red**, the mana potion **blue**. One asset, two colored results — the same
"don't duplicate what you can derive" reflex used elsewhere in the project.

### Unified selection and the context-dependent Actions panel

Selection is **unified across inventory and store**: you click an item in either
panel and it becomes "the selected item". The **Actions panel then changes with the
context** of where the selection came from:

| Selection source | Actions offered |
|------------------|-----------------|
| **Inventory** item | `Use` / `Break` / `Shoot` (the off-chain gameplay actions that apply to what you own) |
| **Store** item | `Buy` (the on-chain purchase) |

This keeps a single, uncluttered action area instead of scattering buttons across
every card, and it reinforces the mental model: inventory actions *consume/use* what
you already own (off-chain), the store action *acquires* new ownership (on-chain).

### The unified quiver slot

Quivers get special UI treatment so the inventory reads cleanly:

- A **single slot** represents the quiver, showing the current **arrows / capacity**
  rather than one slot per quiver tier.
- Only the **highest-capacity quiver** the player owns is shown (owning `carcaj_20`
  supersedes `carcaj_10` and `carcaj_5` visually).
- The **arrow item is not shown loose** in the inventory: arrows live *inside* the
  quiver slot as its fill, not as their own grid cell.

This mirrors how a player thinks about it ("my quiver holds X arrows") instead of
exposing the several underlying token ids. The distinction between *arrows owned in
the quiver* (game state) and *arrows historically purchased* (the on-chain counter
that unlocks Archer) is exactly the on-chain/off-chain split covered in
[07 — On-chain / off-chain boundary](./07-on-chain-off-chain-boundary.md).

---

## 8. The savegame: the source of truth for game state

Ownership lives on-chain, but **game state** (the player's name, how many arrows are
currently in the quiver, current health/mana, what's been used) lives in a **local
savegame**. This is the client-side half of the on-chain/off-chain boundary
([07](./07-on-chain-off-chain-boundary.md)).

### A `USaveGame` indexed by wallet

State is stored in a `USaveGame` object, saved to a **slot keyed by the player's
wallet address**:

```
slot name = "save_" + <walletAddress>      e.g.  "save_0x7099…79C8"
```

Keying the slot by address means **each wallet has its own savegame** on the machine:
switching accounts (via logout/login) loads a different save, and two players sharing
a PC don't clobber each other's state. The saved object holds:

- the player's **name** (chosen the first time),
- the **inventory** as a `TMap<int32, int32>` (item id → quantity) — the *game-side*
  inventory, which can diverge from the chain as items are used,
- **health / mana** (and any other purely local stats).

### First-time vs. known-wallet flow

When a wallet authenticates, the client checks whether a savegame slot exists for it:

```
authenticated address
        │
        ▼
  does slot "save_<address>" exist?
     │ no (first time)                    │ yes (known wallet)
     ▼                                     ▼
  show NAME popup                     load the savegame
  seed inventory FROM THE CHAIN       (name, inventory, health/mana)
  (GET /api/inventory) → savegame            │
        │                                     ▼
        └──────────────► play with restored game state
```

- **First time (no slot):** a **popup asks for the player's name**, and the game-side
  inventory is **seeded from the chain** — it reads `GET /api/inventory` and writes
  those balances into the savegame as the starting game inventory. This bootstraps
  local state from the authoritative on-chain ownership exactly once.
- **Known wallet (slot exists):** the savegame is **loaded** and game state (name,
  inventory, health/mana) is **restored** as it was left. The chain is not re-seeded;
  the local state is authoritative for game-side quantities from here on.

### Autosave

The savegame is written back **after every purchase and every action** (use, break,
shoot, buy) so progress is never lost between sessions. Because it's keyed by address,
the autosave always targets the current wallet's slot.

> Why the savegame and not the chain for this: using an item (shooting an arrow,
> drinking a potion) happens locally, many times, and must be instant and free.
> Persisting each of those on-chain would cost gas and block on mining. The savegame
> gives fast, free, per-wallet persistence for the game-state half. The full rationale
> — and the specific rules that *moved* from the contract to the savegame — is
> [07 — On-chain / off-chain boundary](./07-on-chain-off-chain-boundary.md).

---

## 9. Login and logout (account switching)

The UI wraps the whole experience in an authentication flow built on the SIWE login of
[06 — SIWE login](./06-login-siwe.md):

- **Login screen.** Before reaching the main screen, the player goes through a **SIWE
  login** screen. The authenticated address it produces becomes `WalletAddress` (the
  single source of truth, see [06 §8](./06-login-siwe.md)) and selects which savegame
  slot to load (§8).
- **Logout.** Logging out **autosaves** the current state, **clears the auth**
  (forgets `WalletAddress` and the session) and **returns to the login screen**. This
  is what makes **account switching** clean: log out of one wallet (its state is saved
  to its `save_<address>` slot), log in with another (its own slot loads), with no
  state bleeding between accounts.

```
[ Login screen ]──SIWE done──►[ Main screen ]──Logout──► autosave + clear auth ──►[ Login screen ]
                                                              (state → save_<address>)
```

---

### Summary for the demo
The communication layer demonstrates a clean **separation of responsibilities**:
Unreal requests (HTTP), the bridge coordinates (no keys), MetaMask signs (custodies
the key), the chain is the truth. The `pending → signing → done` pattern makes the
asynchrony explicit and double-sign-proof, and the test-client proves the API
contract works — leaving the Unreal integration reduced to "making HTTP requests".

On top of that pipeline, the **client UI** makes the split tangible: four framed
panels (inventory, medals, actions, store), a unified selection driving a
context-dependent action panel, and a unified quiver slot. Game state (name,
game-side inventory, health/mana) lives in a **per-wallet savegame** (`save_<address>`)
that is seeded from the chain on first login and autosaved after every action, while
**login/logout** (SIWE) cleanly switches accounts. Where the line between chain and
savegame is drawn, and why, is the subject of
[07 — On-chain / off-chain boundary](./07-on-chain-off-chain-boundary.md).
