# 06 — Authentication: SIWE login (Sign-In with Ethereum, EIP-4361)

> Part of the project's learning guide.
> [01](./01-smart-contract.md) · [02](./02-bridge.md) · [03](./03-achievements-and-dependencies.md) · [04](./04-debugging-and-learnings.md) · [05](./05-api-and-unreal-client.md).
> Here we close the **authentication** phase: what SIWE is, why "connecting" is not
> the same as "authenticating", and how the login fits the project's three-actor
> pattern **without gas** and **without keys on the server**. With the whys.

---

## 1. The problem: "connect" is not authentication

On the web (doc 05) the player clicks **Connect MetaMask** and their address appears.
It's tempting to treat that as "login", but **it isn't**. Connecting only does one
thing:

- MetaMask **shows** the address the user chooses to expose (`eth_accounts`).
- The browser **reads** it. Nobody has proven anything.

The difference is one of **ownership**. An Ethereum address is public: it appears in
every transaction, in any explorer, in logs. A client **saying** "I'm `0xAbc…`"
doesn't prove it controls that account — only that it knows how to type (or copy) 42
public characters. It's like identifying yourself by reading out an ID number anyone
can see: you state it, you don't prove it.

| | What happens | What it proves |
|---|---|---|
| **Connect** (`eth_accounts`) | The browser **reads** an address MetaMask exposes | Nothing. Only that someone knows it |
| **SIWE** (signing a message) | The owner **signs** with their private key; it's verified | That they **control the key** of that address |

**Authenticating** = proving that whoever claims to be `0xAbc…` **owns the private
key** of `0xAbc…`. And that's proven in only one way: by asking them to **sign**
something only the key's owner could sign. That's where SIWE comes in.

> Mental rule for the consultancy: *reading an address ≠ proving identity.* Connect
> is a UI convenience; SIWE is the authentication mechanism.

---

## 2. What SIWE (EIP-4361) is

**SIWE** ("Sign-In with Ethereum") is the **EIP-4361** standard: a format for a
**human-readable text message** that the user signs with their wallet to log in. The
wallet as a replacement for "username + password": instead of a password, the proof
is a **cryptographic signature**.

The message is NOT free text: it has fields defined by the standard. This is the one
the bridge builds (`server.js`, `POST /api/siwe/login-intent`):

```
localhost:8787 wants you to sign in with your Ethereum account:
0x70997970C51812dc3A010C7d01b50e0d17dc79C8

Inicia sesion en GameStore (demo). Firmar no cuesta gas.

URI: http://localhost:8787
Version: 1
Chain ID: 31337
Nonce: aB3dE9fG…
Issued At: 2026-07-07T10:00:00.000Z
```

Each field has a security rationale, it's not decoration:

| Field | What it's for |
|-------|---------------|
| `domain` (`localhost:8787`) | Binds the signature to **this site**. A signature for another domain isn't valid here (anti-phishing). |
| `address` | The address that **claims** to be the owner. It's what verification will have to **confirm**. |
| `statement` | Human text the user reads in MetaMask before signing (informed consent). |
| `Chain ID` (`31337`) | The network (Anvil). Prevents reusing a signature meant for another chain. |
| `Nonce` | A single-use number: the **anti-replay** piece (§5). |
| `Issued At` | Timestamp; allows expiring old sessions. |

The essential point: the message is **specific, readable and verifiable**. The user
sees exactly what they sign, and the server can later check that the signature
corresponds to that exact message and that address.

> Note: the `statement` line is currently in Spanish (*"Inicia sesion en GameStore
> (demo). Firmar no cuesta gas."* — "Log in to GameStore (demo). Signing costs no
> gas."). It's the literal text the wallet displays and is reproduced here verbatim
> from `server.js`.

---

## 3. The full flow between the three actors

The login reuses **the same three-actor pattern** as the purchase (doc 05): Unreal
requests, the bridge coordinates without keys, the web + MetaMask sign. Only **what**
is signed changes (a message, not a transaction).

```
UNREAL                         BRIDGE                          WEB + MetaMask
  POST /siwe/login-intent ─────►  validates address (EIP-55)
        {address}                 generates a single-use NONCE
                                  builds the EIP-4361 MESSAGE
  ◄── { requestId, message }      stores session {pending}
                                                  GET /siwe/pending ──► sees the login
                                signing ◄───────── (CLAIMS it on read → signing)
                                                  personal_sign of the MESSAGE in MetaMask
                                                  (OFF-CHAIN, NO GAS)
  GET /siwe/login-status ─► signing               ▼ the player SIGNS
                                verifies (ecrecover) ◄─── POST /siwe/verify {signature}
                                nonce OK + domain OK + chainId OK
                                burns the nonce → {done, address}
  GET /siwe/login-status ─► done, address ✓
```

Step by step, with who does what:

1. **`POST /api/siwe/login-intent {address}`** — *Unreal* declares "I want to log in
   as this address". The bridge validates the format (with EIP-55 checksum via
   `ethers.getAddress`), **generates a nonce**, **builds the EIP-4361 message** and
   creates a session in `pending` state. Returns `{requestId, message}`.
   - Key point: **the server builds the message**, not the client. That way the
     server knows exactly what should have been signed (same nonce, same domain).
2. **`GET /api/siwe/pending`** — *the web* (with MetaMask connected) discovers pending
   logins. On reading them, the bridge marks them **`signing`**: they stop being
   offered, so two tabs don't sign the same login (same "claim-on-read" idea as in
   purchases).
3. **`personal_sign`** — *MetaMask* shows the message; the player signs. It's an
   **off-chain** signature: nothing is sent to the chain, **it costs no gas** (§7). On
   the web: `signer.signMessage(session.message)`.
4. **`POST /api/siwe/verify {requestId, signature}`** — *the web* returns the
   signature. The bridge **verifies it cryptographically** (§4), **burns the nonce**
   and moves the session to `done`, storing the confirmed `address`. If the user
   rejects, the web reports `{error}` and the session moves to `error`.
5. **`GET /api/siwe/login-status?requestId=…`** — *Unreal*, polling, sees
   `pending → signing → done` (with `address`) or `error`. On `done`, the login is
   proven.

The `signing` state serves here the **same double function** as in the purchase: it
informs Unreal that its login is progressing, and it prevents double-signing by
removing the intent from the pending list.

---

## 4. What verification does exactly (ecrecover)

This is the heart of authentication. It lives in `POST /api/siwe/verify`:

```js
const nrec = issuedNonces.get(s.nonce);          // single-use nonce record
if (!nrec || nrec.used) throw new Error("invalid or already-used nonce");

const siwe = new SiweMessage(s.message);                 // the EXACT message we issued
const result = await siwe.verify({ signature, nonce: s.nonce, domain: SIWE_DOMAIN });
if (!result.success) throw new Error("invalid signature");
if (siwe.chainId !== CHAIN_ID) throw new Error("wrong chainId");
nrec.used = true;                                        // NONCE BURNED
s.status = "done";
s.address = siwe.address;
```

The key cryptographic operation is called **`ecrecover`**. Signing a message (ECDSA)
has a very useful property: **from the message and the signature you can *recover* the
public address that signed**, without knowing the private key.

```
      message  +  signature   ──ecrecover──►   address that signed
                                                    │
                                                    ▼
                              == the `address` declared in the message?
                                    yes → authentic     no → impostor
```

- **Only the owner of `0xAbc…`'s private key** can produce a signature that, when
  recovered, yields `0xAbc…`. Nobody else can forge it.
- If an impostor says "I'm `0xAbc…`" but signs with **their** key, `ecrecover` returns
  **their** address, which **doesn't match** `0xAbc…` → verification **fails**.

Besides comparing the recovered address with the declared one, `siwe.verify` checks
that the **nonce** and the **domain** of the signed message are the ones the server
expected, and the code separately verifies the **chainId**. Everything has to line
up; if anything doesn't fit, it's rejected (`401`).

> The signature doesn't "contain" the address: the address is **derived** from the
> signature. That's why forging it is equivalent to breaking elliptic-curve
> cryptography — infeasible.

---

## 5. Why the nonce prevents replay attacks

Imagine there were **no** nonce and the message were always the same fixed text. An
attacker who **observed a valid signature just once** (in a log, in network traffic,
in a capture) could **resend** it later to `POST /api/siwe/verify` and the server,
seeing it cryptographically correct, would accept it. That's a **replay attack**:
reusing an old proof to authenticate without the key.

The **nonce** prevents this. It's a random value the server generates **for each login
attempt** and puts inside the message. Since the message changes on each attempt,
**the signature changes too**: a signature is valid **only for the nonce it was
produced with**.

And here's the decisive detail: the nonce is **single-use**. The server keeps a record
(`issuedNonces`) and, as soon as it verifies a signature successfully, it **burns** it
(`nrec.used = true`):

```js
const nrec = issuedNonces.get(s.nonce);
if (!nrec || nrec.used) {            // doesn't exist or already used
  s.status = "error";
  return sendJson(res, 400, { error: "invalid or already-used nonce" });
}
// … verification OK …
nrec.used = true;                    // burned: cannot be reused
```

Consequence: even if an attacker **captures** a valid signature, **it's useless**. On
trying to resend it, its nonce is **already used** → immediate rejection, even before
looking at the cryptography. Each signature is a one-way ticket.

| Without nonce | With single-use nonce |
|---------------|-----------------------|
| Same message always → same reusable signature | Different message each time → different signature each time |
| Capturing a signature = being able to log in forever | Capturing a signature = useless (nonce already burned) |

---

## 6. The decision to add dependencies (`siwe` + `ethers`)

The bridge was born with an explicit design rule (docs 02 and 03): **zero
dependencies**. The chain reads are done with raw `eth_call` over JSON-RPC, encoding
the arguments by hand (everything is `address`/`uint256`, it's trivial). With SIWE we
**broke that rule on purpose**, and it's worth explaining why.

`server.js` now depends on two libraries (see `bridge/package.json`):

- **`siwe`** — builds the EIP-4361 message (`SiweMessage`, `generateNonce`) and, above
  all, **verifies** it (`siwe.verify`).
- **`ethers`** — cryptography and address utilities (`getAddress` with EIP-55
  checksum) that the verification (the `ecrecover`) relies on.

**Why zero-dependencies breaks here and not before:**

- **Cryptography isn't homemade.** Encoding an `eth_call` by hand is byte arithmetic:
  if I get it wrong, the read fails obviously and visibly. Implementing `ecrecover`,
  the strict EIP-4361 parsing and the nonce/domain checks by hand is **security
  code**: a subtle bug doesn't "show", it simply leaves a door open. The cost of a
  mistake is categorically different.
- **Proven libraries > own code.** `siwe` and `ethers` are de-facto standards, audited
  and maintained by the community. Reinventing them would mean taking on huge risk to
  save one dependency. The "zero deps" rule served **simplicity**; in security, the
  correct simplicity is **relying on what's proven**.
- **The cost stays bounded.** Only SIWE brings dependencies. The reads still have no
  libraries. And installation is conditional: the Windows startup script
  (`start-bridge.ps1`) runs `npm install` **only when needed**.

> Consultancy principle: the "zero dependencies" rule is a preference, not a dogma. It
> bends exactly where the risk of doing it yourself outweighs the risk of depending on
> others: **cryptography**. There, "don't do it at home".

---

## 7. The key distinction for the demo: free login vs. gas-costing purchase

This is the most important contrast to show in a presentation, because it illuminates
how Ethereum works:

```
   LOGIN (SIWE)                          PURCHASE (buy)
   ───────────────                       ───────────────
   Signs a MESSAGE                        Signs a TRANSACTION
   personal_sign / signMessage            eth_sendTransaction
   OFF-CHAIN  (nothing touches the chain) ON-CHAIN (a block is mined)
   FREE  (no gas)                         COSTS GAS
   Proves ownership of the key            Changes state (balances, ETH)
   Reversible/ephemeral (session)         Permanent (stays on the chain)
```

- **Signing a message is off-chain.** `personal_sign` produces a cryptographic
  signature **without sending anything** to the blockchain. There's no miner executing
  anything, no state changes → **no gas**. That's why the message's `statement`
  literally says *"Firmar no cuesta gas"* ("Signing costs no gas").
- **Buying is on-chain.** `buy()` is a **transaction**: it's broadcast to the network,
  a block executes it, it changes balances and transfers ETH. That consumes **gas**
  and is permanent.

Both use "the MetaMask signature", and there lies the typical confusion worth undoing
in the demo: **signing ≠ paying**. Signing proves authorship; only when what's signed
is a *transaction* that the network executes is there gas. The login demonstrates that
this nuance is understood and exploited: robust authentication at **zero cost**.

> Line for the demo: *"Logging in is signing a piece of paper; buying is signing a
> cheque. The paper costs nothing; the cheque moves money on the chain."*

---

## 8. The Unreal side: the login is "more of the same"

The big design conclusion of doc 05 was that the game side is **just two HTTP calls +
polling**. The login **introduces nothing new in Unreal**: it's exactly the same
pattern as the purchase, with different endpoints.

Direct comparison with what already exists (`BuyItem` in `BlockchainStoreClient.cpp`):

| Step | Purchase (already implemented) | SIWE login (same mold) |
|------|--------------------------------|------------------------|
| 1st call (register intent) | `POST /api/purchase-intent` → `requestId` | `POST /api/siwe/login-intent` → `requestId` |
| 2nd call (poll the status) | `GET /api/purchase-status` until `done`/`error` | `GET /api/siwe/login-status` until `done`/`error` |
| State machine observed | `pending → signing → done` | `pending → signing → done` |
| Who signs | The web + MetaMask (outside Unreal) | The web + MetaMask (outside Unreal) |
| Result on finishing | `txHash`, refreshes inventory | **confirmed** `address` |

That is, the login **reuses the subsystem** (`UBlockchainStoreClient`,
`GameInstanceSubsystem`) as-is: the same `MakeRequest`, the same `TryParseJsonObject`,
the same stable `FTimerManager` for polling, the same `BlueprintAssignable` delegates
to notify the UI. You only need to add a `Login()` cloned from `BuyItem()` and a
`HandleLoginStatusResponse` cloned from `HandlePurchaseStatusResponse`.

### The unification with `WalletAddress`

Today, in the subsystem, the address is set **by hand** (see the header):

```cpp
/** Address de la wallet del jugador. De momento set manual; el login SIWE vendra despues. */
UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore|Config")
FString WalletAddress;

UFUNCTION(BlueprintCallable, Category = "BlockchainStore|Config")
void SetWalletAddress(const FString& InAddress);
```

That `WalletAddress` is the one already feeding `FetchInventory`, `FetchProgress` and
`BuyItem`. The login **unifies it**: instead of typing it, the flow **produces and
confirms** it.

- The player starts login with a **candidate** address.
- When `login-status` reaches `done`, the bridge returns the **verified** `address`.
- Unreal writes that address into `WalletAddress` (the same field everything else
  already uses). `SetWalletAddress` stops being a manual input and becomes the
  **authenticated result of the login**.

Result: a **single source of truth** for the player's identity. Before the login,
`WalletAddress` was an unproven claim; after it, it's an address **whose ownership is
proven** — and all the reads and purchases that already depended on that field inherit
that guarantee without changing a line.

> Current status (honest for the demo): the **bridge** and the **web** implement SIWE
> end to end (tested with `curl` playing Unreal + MetaMask signing, and with
> `ethers.Wallet.signMessage` in automated tests). The **Unreal subsystem** doesn't
> have the `Login()` yet; its design is the one in this section — two calls and
> polling, identical to the purchase — so the integration is mechanical, not
> conceptual.

---

## Summary for the demo

- **Connect reads an address; SIWE proves it.** Authenticating is proving **ownership
  of the key**, and that's achieved only with a **signature**.
- **SIWE (EIP-4361)** is a readable, structured message (domain, address, chainId,
  nonce, issued-at) the user signs to log in.
- The login **reuses the three-actor pattern**: Unreal requests, the bridge
  coordinates **without keys**, the web + MetaMask sign. States
  `pending → signing → done`.
- **Verification is `ecrecover`**: from the message + the signature the signing address
  is recovered and compared with the declared one; nonce, domain and chainId also line
  up.
- **The single-use nonce** closes off replay attacks: a captured signature is useless
  because its nonce is already burned.
- **`siwe` + `ethers`** are added on purpose: cryptography relies on proven libraries,
  never on homemade code.
- **In Unreal it's "more of the same"**: two HTTP calls + polling, cloned from the
  purchase, and the login **unifies `WalletAddress`** turning it into **proven**
  identity.
- **The distinction that crowns it:** login = signing a **message**, off-chain, **no
  gas**; purchase = signing a **transaction**, on-chain, **with gas**. Signing is not
  paying.
</invoke>
