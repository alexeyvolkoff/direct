---
layout: page
title: Getting Started
permalink: /getstarted/
nav_order: 1
---
# 🚀 Getting Started with **On My Disk Direct**

Welcome to **On My Disk Direct** — your decentralized personal cloud and private AI workspace. Unlike traditional cloud providers, **On My Disk 3.0** never stores your files on central servers: your browser, phone, and computer nodes communicate peer-to-peer over encrypted **WebRTC DataChannels**, backed by self-sovereign cryptographic identity (**W3C DID** + **GunDB SEA**).

---

## 1. Sign In or Create Your Decentralized ID (DID)

Open [**disk.direct**](https://disk.direct) in any modern browser, or launch the native **On My Disk** Android / iOS app.

![Sign in or connect](/images/login.png)

Choose how you want to sign in or connect (see **[Decentralized Identity (DID)](/did/)** for full details):
* **Sign in with ID / Create your ID**: Generate or unlock a self-sovereign cryptographic identity (`did:gun:<pub>`) directly in your browser without relying on third-party OAuth providers:
  * **Any Display Nickname (No "Username Taken")**: Pick any display name you like (e.g., `Alice` or `Max`) — nicknames do not have to be unique, and your profile automatically receives a 4-character `#tag` fingerprint derived from your public key.
  * **One-Click `.json` Keyfile Backup**: Right after creating your ID (or anytime in **Settings → Security & Keys**), save your portable `omd-did-*.json` keyfile in one click.
* **Sign in with Keyfile (`.json`)**: Instantly sign in or recover your DID on any browser or mobile device by selecting your saved `.json` keyfile — no password entry required.
* **Sign in with Google / Sign in with Apple**: Instant multi-device login. Your root identity unlocks automatically and issues a signed device certificate (`SEA.certify`) so all your nodes and browser peers appear in the sidebar and workspace.
* **Connect a device**: Attach or pair a storage device right from the welcome screen — no persistent identity is created.

---

## 2. Workspace Onboarding Wizard

Right after signing in (or creating your DID) for the first time, a quick setup wizard configures your default storage and AI compute in three steps:

### Step 1: Choose Where to Save Your Materials (`2. Storage`)
Pick the default workspace folder used for collaborating on documents and saving AI-generated files:
* **Browser internal storage (OPFS)** (`/Private/OnMyChat`): Fast, private Origin Private File System built into modern browsers — works on any OS immediately with zero file-permission prompts.
* **Local folder on your computer**: Mount a real folder from your disk via the browser's File System Access API.
* **Connect an On My Disk device** *(Recommended)*: Pair an always-on Linux/Windows **On My Disk Node** so your files stay accessible 24/7 across all devices.

![Onboarding Step 2 — Choose Storage](/images/onboarding_storage.png)

### Step 2: Choose Your AI Node (`3. AI Node`)
Select which compute backend powers **On My Chat**, RAG search, and Creations:
* **Automatic (Auto)** *(Recommended)*: Uses your personal AI node whenever it is connected and online, and automatically falls back to the secure **On My Chat Public Cloud AI Node** when no personal node is available.
* **On My Chat Public Cloud AI Node**: Start chatting immediately with zero local hardware setup.
* **Custom node by LinkID**: Connect to a shared team or friend GPU node by its `LinkID`.

![Onboarding Step 3 — Choose AI Node](/images/onboarding_ainode.png)

### Step 3: All Set (`4. Done`)
Review your configured **Storage folder**, **AI Node**, default **AI Assistant** (`June`), and **Privacy** mode (`E2EE + WebRTC P2P`), then jump straight into **Start chatting in On My Chat** or **Open On My Disk files**.

![Onboarding Step 4 — All Set](/images/onboarding.png)

---

## 3. Connecting Additional Folders & Hardware Nodes

You can attach storage in two ways:
* **Directly from a Browser or Mobile App**: Share a local folder right from your browser tab (acting as a live WebRTC peer while open) or enable **Camera Auto-Sync** / SD-card folders in the mobile app.
* **Always-On Storage & AI Node**: Install the lightweight **On My Disk Node** daemon on **Linux** or **Windows** (see **[Node Deployment & Modes](/advanced/)**) so your files, SSH terminal, and **On My Chat** assistant stay online 24/7 in the background.

To add a folder on any connected device:
1. Navigate to your device in the sidebar or workspace (or click **Connect a device** in the top-left corner to pair a new one).
2. Click **Add folder** inside an empty device view, or click the **+** button in the top-right toolbar.
3. Choose the directory to publish. It immediately becomes accessible across all your authorized devices over P2P.

![Add Folder Screen](/images/add_folder.png)

---

## 4. Working with Files

### Main Sidebar & Workspace
* **Connect a device & Device Tree**: Pair a new node or browser folder with **Connect a device**, and browse all your online Linux/Windows nodes, mobile devices, and browser peers in the sidebar or workspace cards — including direct P2P cross-device copy/move without cloud staging.
* **Shared with me / Shared by me**: Direct DID user-to-user shares and public links you created.
* **Chats**: **On My Chat** AI assistant (SSE streaming, RAG source chips, Creations mode, and MCP document tools).
* **Settings**: Manage your account, recovery credentials, storage preferences, and **On My Chat** AI assistant configuration.
* **Terminal**: Built-in browser **WebSSH** terminal (`xterm.js` + `libssh2` WebAssembly over P2P DataChannel) — available only for **Always-On Nodes** with the advanced desktop/server client installed (paired Linux or Windows nodes running an SSH server).

### File Operations & Context Menu
Right-click any file or folder for quick actions:
* **Open / View**: Built-in viewer and Craft preview for PDFs, images, video/audio, Markdown, code, and Office documents (`.docx`, `.xlsx`, `.pptx`, `.odt`).
* **Add New / Upload**: Click **Add New** (or **+**) or drag-and-drop files and folders directly into the browser.
* **Download (`Ctrl+D`)**: Stream single files or multi-file ZIP archives directly from the peer node.
* **Copy / Cut / Paste / Rename / Delete**: Manage files locally or directly across two different P2P devices.
* **Share Link...**: Create a public P2P link or share directly with another On My Disk user.

![File Manager and Context Menu](/images/file_manager.png)

---

## 5. Sharing Files & Folders

### Public P2P Links
1. Right-click a file or folder and select **Share Link...**.
2. A live **Public P2P Link** (`https://disk.direct/shared/...`) is generated — click **Copy link** to share it immediately, or configure optional protections and click **Update Share**:
   * **Password Protection**: Require a password to decrypt and view the share.
   * **Link Expiration**: Choose **Never Expires** or automatically disable access after a time limit.
   * **Folder Write Access**: When sharing a folder, optionally allow visitors to upload/edit files inside that shared directory (`ScopedGuest` sandbox).
3. Visitors connect directly to your node over WebRTC P2P (with transparent relay fallback) without gaining access to anything outside the shared item.

![Sharing Modal](/images/sharing.png)

### Managing & Revoking Access
* View all active public links and user-to-user shares in **Shared by me**.
* Adjust settings with **Update Share** or click **Revoke Share** at any time — access is cut off immediately on your node.
