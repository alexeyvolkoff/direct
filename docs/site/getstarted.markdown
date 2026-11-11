---
layout: page
title: Getting Started
permalink: /getstarted/
nav_order: 1
---
# 🚀 Getting Started with **Disk Direct**

Welcome to **Disk Direct** — your decentralized personal cloud and private AI workspace. Unlike traditional cloud providers, **Disk Direct** never stores your files on central servers: your browser, phone, tablet, and computer nodes communicate peer-to-peer over encrypted **WebRTC DataChannels**, backed by self-sovereign cryptographic identity (**W3C DID** + **GunDB SEA**).

---

## 1. Sign In or Create Your Decentralized ID (DID)

Open [**disk.direct**](https://disk.direct) (or [**aiassistant.direct**](https://aiassistant.direct) to jump straight into chat) in any modern browser, or launch the native **Disk Direct** Android / iOS app.

![Sign in or connect](/images/login.png)

Choose how you want to sign in or connect (see **[Decentralized Identity (DID)](/did/)** for full details):
* **Sign in with ID / Create your ID**: Generate or unlock a self-sovereign cryptographic identity (`did:gun:<pub>`) directly on your device without relying on third-party OAuth providers:
  * **Any Display Nickname (No "Username Taken")**: Pick any display name you like (e.g., `Alex` or `Studio`) — nicknames do not have to be unique, and your profile automatically receives a 4-character `#tag` fingerprint derived from your public key.
  * **One-Click `.json` Keyfile Backup**: Right after creating your ID (or anytime in **Settings → Security & Keys**), save your portable `.json` keyfile in one click.
* **Sign in with Keyfile (`.json`)**: Instantly sign in or recover your DID on any browser or mobile device by selecting your saved `.json` keyfile — no password entry required.
* **Sign in with Google / Sign in with Apple**: Instant multi-device login. Your root identity unlocks automatically and issues a signed device certificate (`SEA.certify`) so all your paired nodes and browser peers appear in the sidebar and workspace.
* **Connect a device**: Attach or pair a storage node right from the welcome screen via Link ID or QR code without creating a persistent identity.

---

## 2. Workspace Onboarding Wizard

Right after signing in for the first time, a quick setup wizard configures your default storage and AI compute:

### Step 1: Choose Where to Save Your Materials (`Storage`)
Pick the default workspace folder used for collaborating on documents and saving AI-generated files:
* **Browser internal storage (OPFS)** (`/Private`): Fast, private Origin Private File System built into modern browsers and mobile shells — works immediately on any OS with zero file-permission prompts.
* **Local folder on your computer**: Mount a real folder from your disk via the browser's File System Access API.
* **Connect a Disk Direct node** *(Recommended)*: Pair an always-on Linux, Windows, macOS, or OpenWrt **Disk Direct Node** so your files stay accessible 24/7 across all devices.

![Onboarding Step — Choose Storage](/images/onboarding_storage.png)

### Step 2: Choose Your AI Node (`AI Node`)
Select which compute backend powers **Chats**, **Knowledge** (RAG search), and **Creations**:
* **Automatic (Auto)** *(Recommended)*: Uses your personal AI node whenever it is connected and online, and automatically falls back to the built-in **Public Demo AI Node** when no personal node is available.
* **Public Demo AI Node**: Start chatting, exploring public knowledge cards, and creating studio illustrations immediately with zero local hardware setup.
* **Custom node by LinkID**: Connect to a shared team or friend GPU node by its `LinkID`.

![Onboarding Step — Choose AI Node](/images/onboarding_ainode.png)

### Step 3: All Set (`Done`)
Review your configured **Storage folder**, **AI Node**, default **AI Assistant** (`June`), and **Privacy** mode (`E2EE + WebRTC P2P`), then jump straight into your files or AI chat.

![Onboarding Step — All Set](/images/onboarding.png)

---

## 3. Connecting Additional Folders & Hardware Nodes

You can attach storage in two ways:
* **Directly from a Browser or Mobile App**: Share a local folder right from your browser tab (acting as a live WebRTC peer while open) or enable **Camera Auto-Sync** / media folders in the mobile app.
* **Always-On Storage & AI Node**: Install the lightweight **Disk Direct Node** daemon on **Linux**, **Windows**, **macOS**, or **OpenWrt** (see **[Node Deployment & Modes](/advanced/)**) so your files, SSH terminal, Knowledge Base index, and AI assistant stay online 24/7 in the background.

To add a folder on any connected device:
1. Select your device in the left sidebar (or click **Connect a device** / **Settings → Devices** to pair a new one).
2. Click **Add New → New folder** or attach an existing directory on your node.
3. The folder immediately becomes accessible across all your authorized devices over encrypted WebRTC P2P.

![Device Workspace & Add Folder](/images/add_folder.png)

---

## 4. Navigating the Unified Workspace

### Left Sidebar Shortcuts
Whether you open **Disk Direct** in Files mode (`disk.direct`) or AI mode (`aiassistant.direct`), every core workspace is one click away in the left sidebar:
* **Device Tree & Shares**: Browse all your online Linux/Windows/macOS/OpenWrt nodes, mobile devices, and local browser storage — including direct P2P cross-device copy and move without cloud staging.
* **Shared with me / Shared by me**: Direct DID user-to-user shares and public P2P links you created.
* **Chats**: Private AI assistant (`June`) with SSE streaming, RAG source chips, and autonomous MCP document tools.
* **Creations**: Generative image & studio workspace (`Express`, `Studio`, and `Video` galleries).
* **Knowledge**: Visual & semantic **Memory Cards** across your documents, notes, photos, and people *(if you haven't paired a self-hosted node yet, a banner at the bottom guides you to set up your own node for automatic indexing)*.
* **Settings**: Manage your DID profile, paired devices, camera/folder backup, network gateway (BYOS), theme appearance, and AI assistant persona.
* **Terminal**: Built-in browser **WebSSH** terminal (`xterm.js` + `libssh2` WebAssembly over P2P DataChannel) for paired nodes running an SSH server.

### File Operations & Context Menu
Right-click (or long-press on mobile) any file or folder for quick actions:
* **Open / View**: Built-in media gallery and **WordCraft / GridCraft / DeckCraft** editors for PDFs, photos, video/audio, Markdown, code, and Office documents (`.docx`, `.xlsx`, `.pptx`, `.odt`).
* **Add New / Upload**: Click **Add New** (or **+**) or drag-and-drop files and folders directly into the workspace.
* **Download (`Ctrl+D`)**: Stream single files or multi-file archives directly from the peer node.
* **Copy / Cut / Paste / Rename / Delete**: Manage files locally or directly across two different P2P devices.
* **Share**: Create a public P2P link or share directly with another DID user.

![File Manager and Context Menu](/images/file_manager.png)

---

## 5. Sharing Files & Folders

### Public P2P Links
1. Right-click any file or folder and select **Share**.
2. A live **Public P2P Link** (`https://disk.direct/shared/...`) is generated — copy the link to share it immediately, or configure optional protections:
   * **Password Protection**: Require a password to access and view the share.
   * **Link Expiration**: Choose **Never Expires** or automatically disable access after a set period.
   * **Folder Write Access**: When sharing a folder, optionally allow recipients to upload or edit files inside that shared directory (`ScopedGuest` sandbox).
3. Visitors stream directly from your node over WebRTC P2P (with transparent TURN relay fallback) without gaining access to anything outside the shared item.

![Sharing Modal](/images/sharing.png)

### Managing & Revoking Access
* View all active public links and user-to-user shares under **Shared by me**.
* Update permissions or revoke a share at any time — access is cut off immediately on your node.
