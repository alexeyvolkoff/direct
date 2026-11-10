---
layout: page
title: Decentralized Identity (DID)
permalink: /did/
nav_order: 1.5
---
# 🔐 Decentralized Identity (DID) & Key Management
<hr>

**On My Disk Direct** (**On My Disk 3.0**) and **On My Chat** replace traditional central-server user accounts with self-sovereign **Decentralized Identifiers (`did:gun:<pub>`)** powered by **GunDB SEA** (*Security, Encryption, Authorization* — WebCrypto `ECDSA P-256` signing, `ECDH P-256` key agreement, and `AES-GCM` encryption).

Your identity is a cryptographic keypair generated directly on your device — **not** a row in a central database.

---

## 1. How `did:gun` Works

When you create a new ID in a browser or mobile app:
1. **Client-Side Key Generation**: Your device locally generates a root cryptographic keypair (`SEA.pair()`):
   * `pub` / `priv` — `ECDSA P-256` public/private signing keys (your canonical identifier is `did:gun:<pub>`).
   * `epub` / `epriv` — `ECDH P-256` public/private encryption keys for end-to-end encrypted (E2EE) data and sharing.
2. **Encrypted Personal Graph (`~<pub>`)**: Your paired device list, assistant persona, encrypted SSH credentials, and MCP API keys live inside your signed GunDB user space (`~<pub>`). Every write is cryptographically signed and verified on the wire.
3. **Scoped Device Certificates (`SEA.certify`)**: Each browser or mobile app also generates its own local **Device Keypair** (`devicePair`) and receives a scoped capability certificate so day-to-day multi-device sync never exposes your root private key in transit.

| Mode | Identifier | Multi-Device Sync | Key Custody |
| :--- | :--- | :--- | :--- |
| **Native DID** *(Recommended)* | `did:gun:<pub>` | Yes (all paired nodes, web & mobile) | 100% client-side (`Keyfile .json` + password vault) |
| **Google / Apple Sign-In** | `did:gun:<pub>` | Yes (instant OAuth unlock) | OAuth token unlocks your `did:gun` device certificate |
| **Connect a Device (Guest)** | Ephemeral session | Single tab / direct `LinkID` only | Held in browser RAM only; cleared on close |

---

## 2. Display Nicknames & `#tag` Fingerprints (No "Username Taken")

In centralized cloud services, usernames act as primary database keys — forcing users to hunt for unoccupied handles like `alice_1998_official`.

In **On My Disk Direct**, your **Nickname / Display Name** is purely human-readable metadata, just like in decentralized protocols (Nostr, Scuttlebutt, or Matrix display names):
* **No Uniqueness Restrictions**: Any number of people can choose the display name `Alice`, `Max`, or `Studio` — with spaces, capital letters, or any language script. You will never see a *"Username already taken"* error.
* **Cryptographic `#tag` Badge**: To visually distinguish people who share the same display name, the app derives a deterministic 4-character alphanumeric fingerprint directly from your public key — for example, **`Alice #8k2p`** (from `did:gun:8k2p...`).
* **Zero-Knowledge Password Sign-In**:
  * When you set a password during DID creation, your root private key is encrypted **locally in your browser** using `PBKDF2` (`SEA.work`) + `AES-GCM` (`SEA.encrypt`) and published to `~<pub>/auth`, while adding your `~<pub>` pointer to the public `~@nickname` and `~@nickname#tag` index sets.
  * The server never sees or stores your plaintext password.
  * When signing in from a phone or another laptop with **Nickname + Password**, your client fetches the encrypted vaults under `~@alice`, tests decryption locally in WebCrypto, and unlocks the exact DID whose vault matches your password.
  * You can also sign in explicitly with **`Name#tag`** (e.g., `Alice#8k2p`) or paste your full **`did:gun:...`** identifier.

> Your display nickname and login alias can be updated at any time in **Settings → Profile & Identity** without changing your underlying `did:gun:<pub>` or re-pairing any of your storage nodes.
{: .note }

---

## 3. Portable `.json` Keyfile (One-Click Backup & Passwordless Sign-In)

Because your identity is a standard cryptographic keypair, you can back it up and carry it as a self-contained **`.json` Keyfile** (`omd-did-<name>-<tag>.json`).

### Saving Your Keyfile in One Click
* **Right After Creating Your ID**: On the completion screen, click **Save Keyfile (`.json`)** to download your full cryptographic bundle (`did`, `pair`, `#tag`, and `omd_key`) in a single click.
* **Anytime in Settings**:
  * Open **Settings → Profile & Identity** and click **Save Keyfile (`.json`)** right next to your DID, **or**
  * Open **Settings → Security & Keys → DID Keypair Backup (`.json`)** and click **Save Keyfile (`.json`)**.

### Signing In with a Keyfile (No Password Needed)
On any new browser, desktop, or mobile device:
1. On the welcome screen, click **Sign in with Keyfile (`.json`)** *(also available inside the **Sign in with ID** form and **Settings → Security & Keys**)*.
2. Select your saved `omd-did-*.json` file.
3. Your cryptographic identity unlocks immediately, pulls your profile and paired **On My Disk** nodes from the GunDB graph, and connects over WebRTC P2P — with zero passwords or emails required.

> Treat your `.json` keyfile like an SSH private key or hardware wallet seed: store it in a password manager (Bitwarden, 1Password, KeePassXC) or encrypted drive. Anyone holding the `.json` file has full cryptographic control of your DID.
{: .important }

---

## 4. How Storage & AI Nodes Trust Your DID

Your `did:gun:<pub>` acts as the cryptographic root of trust across all your hardware nodes:
1. **One-Time Node Pairing**: The first time you connect to an **On My Disk Node** (`PairModal`), you verify ownership once using the host's system credentials (`Login { user, pass, didPub }`).
2. **Local Node Allowlist**: Your node records your public key in `[P2P] PairedDIDs` inside its local configuration — no cloud database owns this relationship.
3. **Automatic P2P Elevation**: On every subsequent connection from any browser or phone signed into your DID, the client proves possession of your DID over the encrypted WebRTC DataChannel (`PairAuth`), immediately elevating to `Admin` access without prompting for node passwords again.
4. **Encrypted Secrets Vault**: Optional **SSH credentials** (for the in-browser WebSSH terminal) and **MCP API keys** (CivitAI, Club/Muse Pass, Plane, etc.) are encrypted with your DID keys so only your authenticated sessions can decrypt them.

---

## 5. Privacy & Account Deletion (GDPR)

You retain full lifecycle control over your identity in **Settings → Profile & Identity → Danger Zone → Delete Account**:
* Signed profile, device, sharing, and credential nodes under `~<pub>` and `~@alias` are purged from the GunDB graph.
* A cryptographic tombstone blocks any future session restoration or certificate issuance for the deleted DID (`HTTP 410 Gone`), freeing your email or OAuth link if you ever wish to create a fresh identity later.
