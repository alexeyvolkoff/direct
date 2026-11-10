# OnMyDisk 3.0 Architecture

This document describes the (3.0) architecture: a **gateway that is a pure signaling relay** plus **local nodes** that serve the PWA and files directly to browsers over WebRTC P2P, with Jekyll sites served through a ServiceWorker page tunnel.

## 1. Topology

- **Gateway** — signaling relay only. Knows no users. Keeps `OnMyDiskLinkServer` (one command socket per connected node) and WebRTC signaling (`sendWebSignaling`, tunnel queues, site registry). Proxies `/signin` and the user APIs to the legacy gateway (`Network/legacyServer`, default `onmydisk.net`); legacy users log in with username + password.
- **Node** — local node. Runs the HTTP server (serves the PWA bootstrap, `/api/*`, webhooks), the P2P DataChannel layer (libdatachannel), the WebChannel RPCs (node-local search/ai/chat), and the local file system. Connects to the gateway over a single command `LinkSocket` for signaling/OTA/presence.
- **Browser/PWA** — talks to the node directly over WebRTC DataChannels (with a ServiceWorker that rewrites `/sites/**` to `/p2p/sites/**`); the gateway is only used for signaling and the initial bootstrap.

## 2. Project layout

- Root projects: `Gateway.pro` (gateway) and `Node.pro` (node).
- Single client entry: `Node/embedded.pro` with a flavour flag — `qmake DEFINES+=ROUTER Node.pro` selects the router flavour.
- Gateway Qt modules: `QT += core network sql webchannel websockets`.
- `OnMyDiskLink` shared library — link socket + message framing + P2P tunnel + resource tree.

## 3. Identity and pairing (DID auth by SEA signature)

- Identity is a cryptographic DID (`did:gun:<mainPub>`), with a deterministic P-256 SEA keypair derived in the PWA (`hub.js`: PBKDF2 → P-256, `deriveDeterministicPair`) plus a `credCheck` verifier in GunDB.
- Node pairing is **signature-based, no per-device secrets**:
  - At first login the client signs `did:<didPub>,device:<devicePub>,session:<deviceId>` with the DID **master key** (in memory right after a password login) — `seaVerifySignature` (OpenSSL ECDSA P-256 over SHA-256, the Gun SEA wire format `SEA{m,s}`) verifies it against `didPub`, and the node writes `PairedDIDs/<didPub>` (marker) + `PairedDevice/<devicePub>=didPub`.
  - Subsequent sessions sign the same message with the **device key** (certified at first login) — the node verifies against `devicePub` and checks the binding.
  - The node-issued per-device `pairingSecret` is not used anymore; old entries still count as "registered" (the marker is the group itself), so one re-login upgrades them.
- `PairAuth`/`AuthInfo`/`Login` carry `{didPub, deviceId, devicePub, proof}`. `GuestPair` (ToS-accept → `ScopedGuest`) is unchanged and only allowed under `sites/`.

## 4. HTTP server (gateway and node share `httpnode`)

- `HttpServerBackend`: no-DB backend, memory-only sessions. Authentication is DID-based (SEA signatures); legacy username/password login is proxied to the legacy gateway. `signCSR` (PKI) issues certificates via `POST /api/signcsr` (inputs: `csr`, `user`; returns the signed certificate).
- `/signin` and user APIs proxy to the legacy gateway via `HttpHandler::callUpstream()` (default `Network/legacyServer` = `onmydisk.net`).
- Guest sessions are created locally; guests get anonymous sessions.
- `/api/site-meta` — in-memory `m_sites` lookup (also by Host header) for the Jekyll bootstrap.
- Sites bootstrap: static `site-view.html` served from the node; nginx stays the domain-binding mechanism.
- Webhooks `/sync-rights` and `/provision-group` (HMAC via `m_crawlerToken`) are kept.

## 5. P2P layer

- `P2PTunnel` (libdatachannel) — WebRTC DataChannels between browser/PWA and the node; ICE/relay negotiation goes through the gateway's single command socket.
- `OnMyDiskLink` is a **single-command-socket** class. `LinkSocket` framing + `LinkMessage` survive for signaling.
- `p2ptunnel.cpp` chunks messages with UTF-8-aware boundaries; pending RPC requests are rejected on channel/ICE failure.

## 6. Jekyll sites over P2P

- `svar/public/site-view.html` — static bootstrap/loader (MiniDrive, ToS, prefetch).
- `svar/public/service-worker.js` — SW with `/sites/**` → `/p2p/sites/**` cache keys, `omd-sites-v1` cache.
- One-time SW injection via static bootstrap; the SW intercepts `/sites/**`.
- Guest `SetScope` allowed only under `sites/`.

## 7. DID auth verification (C++)

- `seaVerifySignature(sigStr, msg, pub)` in `onmydisklinkclient.cpp`:
  - parses the Gun SEA wire format `SEA` + `JSON {m, s}` (base64 raw r||s),
  - reconstructs the P-256 public key from the `x.y` base64url halves via `EVP_PKEY_fromdata` (OSSL_PARAM group `prime256v1` + the 65-byte X9.63 point),
  - verifies ECDSA over the **single** SHA-256(msg) digest — EVP hashes it again, matching gun/WebCrypto double-hash semantics (TBS = first digest, not the double hash).

## 8. Frontend (svar)

- `persistent.js`: DID = deterministic SEA pair; `signProof(msg)` (master key when in memory, else device key); `devicePub` getter; `publishDevice`/`removeDevice` to the hub's GunDB graph.
- `webrtc-drive.js`: `pairAuth`/`login` carry `{didPub, devicePub, deviceId, proof}`; pending requests rejected on channel/ICE failure.
- `site-view.html` + SW for sites; guest `ScopedGuest` only under `sites/`.

## 9. Build flavours

- `Gateway.pro` — gateway (signaling relay).
- `Node.pro` — node (embedded client); `qmake DEFINES+=ROUTER Node.pro` selects the router flavour.
- `Node/embedded.pro` — single client entry for both flavours.
- `libdatachannel` is built locally; node P2P builds auto-detect it.

## 10. Deployment

- Gateway image rebuild needed after the gateway/docu/signing/sites work.
- New `svar/dist` (with `site-view.html`, updated SW) ships with the next deploy.
- Legacy users log in with username + password.
