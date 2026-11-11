---
layout: page
title: Node Deployment & Modes
permalink: /advanced/
nav_order: 2
---
# ⚙️ Node Deployment & Operating Modes
<hr>

## ⚡ TL;DR — Quick Install (Recommended Agentic AI Setup)

For most laptops, desktops, and mini-PCs, run **one command** to install the **Disk Direct Node** together with the **AI Assistant** service in **Agentic AI mode**. Your files and private RAG knowledge index stay **100% on your device**, while heavy LLM & image generation runs automatically over encrypted P2P on a remote GPU:

**🐧 Linux (`Ubuntu / Debian / Armbian`)**
```bash
curl -fsSL https://forge.disk.direct/repo/Linux/install-direct.sh | bash -s -- --agentic
```

**🪟 Windows 10 / 11 (`PowerShell` — per-user, no Administrator rights needed)**
```powershell
& ([scriptblock]::Create((irm https://forge.disk.direct/repo/Windows/install-direct.ps1))) -Agentic
```

**🍎 macOS (`Terminal`, Apple Silicon — per-user, no `sudo` needed)**
```bash
curl -fsSL https://forge.disk.direct/repo/macOS/install-direct.sh | bash -s -- --agentic
```

> **Have a GPU? Run Full Local AI**: Replace `--agentic` with **`--gpu`** (Linux / macOS Apple Silicon) or `-Agentic` with **`-Gpu`** (Windows NVIDIA GPU) to install **Ollama** locally and run all LLM inference directly on your own hardware (`AI_PROVIDER = local`). See the [mode details below](#1-supported-node-installation--operating-modes).
{: .tip }

> **Storage-Only Node (No AI)** — Need only P2P file sharing and remote SSH access without local RAG/AI? Run the basic installer instead:
>
> - **Linux:** `curl -fsSL https://forge.disk.direct/repo/Linux/install-direct.sh | bash`
> - **Windows (`PowerShell`):** `irm https://forge.disk.direct/repo/Windows/install-direct.ps1 | iex`
> - **macOS:** `curl -fsSL https://forge.disk.direct/repo/macOS/install-direct.sh | bash`
{: .note }

**After installation:** Sign in at [**disk.direct**](https://disk.direct) and claim your device from **Settings → Devices** (or open `http://localhost:8081` on Windows/macOS, `http://localhost/` on Linux).

---

## Overview & Headless P2P Architecture

The **Disk Direct Node** is a lightweight, native C++/Qt P2P daemon with statically linked **WebRTC DataChannels** (`libdatachannel`). It turns any Linux server, single-board computer (SBC), NAS, OpenWrt Wi-Fi router, Windows PC, or Mac into an always-on personal cloud storage node, SSH/TCP tunnel endpoint, and private **AI Assistant** host.

> **Headless P2P Architecture**: The node does not bundle a heavy local web UI. Locally it serves only a lightweight **Claim Page** (`:80` on Linux, `:8081` on OpenWrt routers, Windows, and macOS per-user installs) to pair the device with your decentralized DID account (`PairedDIDs`). All file management, Knowledge Base cards, AI chats, Creations, device settings, and WebSSH terminal sessions run from [**disk.direct**](https://disk.direct) (or the mobile app) directly over encrypted P2P DataChannels.
{: .important }

---

## 1. Supported Node Installation & Operating Modes

You can deploy a **Disk Direct** node in several combinations depending on your hardware — from a Wi-Fi router or fanless laptop up to a multi-GPU workstation:

### Node & AI Mode Matrix

| Mode & Installer Flag | Storage & RAG | LLM & ComfyUI | Target Hardware |
|-----------------------|---------------|---------------|-----------------|
| **1. Storage-Only**<br>`localAI=false` *(no flags)* | ✅ Local files<br>*(no local RAG)* | *(Optional Public AI Node in UI)* | NAS, router, SBC, VPS, PC/Mac (1–2 GB RAM) |
| **2. Agentic P2P** *(Recommended)*<br>`--agentic` / `-Agentic`<br>`AI_PROVIDER=agentic` | ✅ **100% Local**<br>files + CPU RAG | 🌐 **Remote over P2P** (Public Node or your GPU Node) | **Laptops, ultrabooks & mini-PCs without GPU** (8+ GB RAM) |
| **3. Full Local AI**<br>`--gpu` / `-Gpu`<br>`AI_PROVIDER=local` | ✅ **100% Local** | ✅ **100% Local** (`Ollama` CUDA/Metal or `RKLLAMA` NPU + `ComfyUI`) | NVIDIA GPU PC/server, Apple Silicon Mac, or RK3588 SBC |
| **4. Hybrid Mode**<br>`AI_PROVIDER=hybrid` | ✅ **100% Local** | 🔒 **Local** for Work/RAG<br>🌍 **AI Horde** for Fun/RP | Mid-range nodes wanting diverse RP models |
| **5. Public / Team Host**<br>`GuestPairing=true` | ✅ Owner Chroma + Guest E2EE GunDB | ✅ Shared over P2P (`:4000` / `:11434` / `:8188`) | Dedicated GPU server hosting guests or a team |

### How Each Mode Works in Detail

1. **Storage-Only P2P Node**:
   * Runs only the native P2P storage daemon (`~5–28 MB` binary).
   * Provides P2P file shares, public `/shared/...` links, DID user-to-user sharing, browser **WebSSH** (`xterm.js` + `libssh2.wasm` to local `:22`), external USB `automount` + Samba publishing (Linux), and inbound **`omd-tunnel`** TCP port forwarding.
   * When no local AI service is installed on the node, `localAI=false` lets the web/mobile client use the built-in **Public Demo AI Node** seamlessly.

2. **Agentic P2P Mode (`AI_PROVIDER = agentic`) — Private Local RAG + Remote GPU**:
   * **Best choice for Windows laptops, MacBooks, and GPU-less Linux mini-PCs.**
   * Runs the local **AI Assistant** service (`127.0.0.1:4000`) with CPU-friendly `SentenceTransformers` (`paraphrase-multilingual-MiniLM-L12-v2`) and `ChromaDB`. **Your private files and vector index never leave your machine.**
   * Heavy inference calls (`Ollama :11434` and `ComfyUI :8188`) are transparently forwarded by `agentic_tunnel.py` (local TCP listeners `127.0.0.1:21434` and `127.0.0.1:28188`) through outbound **WebRTC DataChannels** to a remote GPU node:
     * **Auto-discovery**: if `INFERENCE_NODE = auto` (or empty), the service automatically queries `{GATEWAY_URL}/api/ai-node` and connects to the **Public AI Node**.
     * **Pinned or Dynamic Switching**: point `INFERENCE_NODE = <linkID>` to your own home GPU server, or switch **Runtime mode → Agentic** and pick any online inference node live in **Settings → AI Assistant → AI Node**.

3. **Full Local AI Node (`AI_PROVIDER = local`)**:
   * Runs the **AI Assistant** service (`127.0.0.1:4000`), **Ollama** (`127.0.0.1:11434`) or **RKLLAMA** (`127.0.0.1:8080` on Rockchip RK3588/RK3576 NPU), and optional **ComfyUI** (`127.0.0.1:8188`) on the same host. Zero external inference traffic.

4. **Hybrid & Horde Modes (`AI_PROVIDER = hybrid` / `horde`)**:
   * In `hybrid` mode, **Work** mode, document analysis, and **RAG** always stay on your local Ollama model, while **Fun** / roleplay chats are routed to distributed **AI Horde** community GPUs (`AI_PROVIDER = horde` routes all chat generation to AI Horde).

5. **Public / Shared Guest AI Mode (`GuestPairing=true`)**:
   * Setting `GuestPairing=true` under `[Settings]` in the node config lets external users connect as `ScopedGuest` (`GuestDIDs`) without a password. Guests can chat with the AI service (`:4000`) or tunnel inference, using `public`-tagged knowledge and their own E2EE GunDB memory cards, while file RPCs and private shares remain strictly blocked.

---

## 2. Linux Installation (`Ubuntu / Debian / Armbian`)

Supported architectures: **`x86_64` (`amd64`)**, **`ARM64` (`aarch64` — Raspberry Pi 4/5, Orange Pi 5, Firefly RK3588)**, and **`ARMHF` (`armv7`)**.

### Hardware Recommendations (Linux)

| Deployment Profile | CPU | RAM | Storage | Accelerator |
|--------------------|-----|-----|---------|-------------|
| **Storage-Only Node** | 2+ cores | 2 GB | 8+ GB | None |
| **Agentic AI Node (CPU RAG)** | 4+ cores | 8 GB | 20+ GB SSD | None (uses remote P2P GPU) |
| **SBC Local AI Node** | RK3588 / RK3576 (8 cores) | 8–16 GB | 64+ GB NVMe/eMMC | Built-in 6 TOPS NPU (`RKLLAMA`) |
| **Workstation Local AI Node** | 8+ cores | 16–32+ GB | 100+ GB NVMe SSD | NVIDIA GPU (8–24+ GB VRAM) |

### Automated One-Line Installer (Recommended)

```bash
# 1) Agentic AI Node (CPU RAG + remote P2P GPU — no local GPU required):
curl -fsSL https://forge.disk.direct/repo/Linux/install-direct.sh | bash -s -- --agentic

# 2) Full Local GPU AI Node (installs Node + AI Assistant + local Ollama):
curl -fsSL https://forge.disk.direct/repo/Linux/install-direct.sh | bash -s -- --gpu

# 3) Rockchip RK3588 / RK3576 NPU Node (points AI Assistant to RKLLAMA on :8080):
curl -fsSL https://forge.disk.direct/repo/Linux/install-direct.sh | bash -s -- --rkllama

# 4) Storage-only P2P Node:
curl -fsSL https://forge.disk.direct/repo/Linux/install-direct.sh | bash

# 5) Uninstall services and packages:
curl -fsSL https://forge.disk.direct/repo/Linux/install-direct.sh | bash -s -- --uninstall
```

### Manual `.deb` Installation

#### Step 1: Install Node `.deb` Package

Pre-built `.deb` packages work out of the box on **Ubuntu 22.04 LTS** and **Ubuntu 24.04 LTS** (`apt` resolves Qt5 libraries, `samba`, `udisks2`, `ffmpeg`, `poppler-utils`, and `pandoc` automatically):

```bash
sudo apt update

# x86_64 (amd64)
wget https://forge.disk.direct/repo/OnMyDisk/Connector/onmydisk-connector-amd64.deb
sudo apt install ./onmydisk-connector-amd64.deb

# ARM64 (Raspberry Pi 4/5, Orange Pi 5, Firefly RK3588, etc.)
# wget https://forge.disk.direct/repo/OnMyDisk/Connector/onmydisk-connector-arm64.deb
# sudo apt install ./onmydisk-connector-arm64.deb

# ARMHF (32-bit ARMv7)
# wget https://forge.disk.direct/repo/Ubuntu/focal/onmydisk-connector-armhf.deb
# sudo apt install ./onmydisk-connector-armhf.deb
```

#### Step 2: Configure Service User & Gateway

Edit `/etc/onmydisk/onmydisk.conf` so the node accesses your home directory with your local Linux user permissions and connects to `disk.direct`:

```ini
[FileNode]
Port=80
User=your_username
Group=your_groupname

[Network]
host=disk.direct
port=9010
```

Enable and restart the systemd service:

```bash
sudo systemctl enable --now onmydisk
sudo systemctl restart onmydisk
systemctl status onmydisk
```

#### Step 3: Claim the Linux Node into Your Account

1. Sign in to [**disk.direct**](https://disk.direct) in your browser.
2. Open the node's local **Claim Page** at `http://<device-lan-ip>/` (or `http://localhost/`, port `80`).
3. Authorize your DID keypair (`PairedDIDs`).
4. Your Linux machine immediately appears in the sidebar and workspace with P2P file access, settings, and WebSSH terminal support.

### Linux NAS Capabilities
Configure directly from **Device Settings** in the Web App:
* 🎛️ **Plug-and-Play USB Automount (`automount=true`)**: Automatically mounts external USB/NVMe drives via `UDisks2` and exposes them as shares.
* 📂 **Automatic RAG Indexing (`autoreindex=true`)**: Filesystem watcher (`2s` debounce + periodic `6h` scan) automatically indexes modified documents and photos into your local **ChromaDB** knowledge base.
* 🌐 **Local Network Shares (`updateSamba=true`)**: Publishes mounted drives via **Samba (SMB)** on your LAN alongside global P2P access.

---

## 3. OpenWrt Routers & Embedded Gateways (`.ipk` — [Full Guide](/openwrt/))

The exact same native C++ P2P Linux node (`DEFINES+=ROUTER`, compiled against `musl` libc) runs directly on **OpenWrt** Wi-Fi routers, travel routers, and CPE gateways — turning any router with a USB port into a silent `< 5 W` always-on personal cloud and P2P SSH/TCP gateway behind CGNAT.

```sh
# 1. Check router CPU architecture (mips, mipsel, armv7, or arm64):
opkg print-architecture

# 2. Download and install the matching .ipk package:
wget -O /tmp/direct-node.ipk https://forge.disk.direct/repo/OpenWrt/onmydisk-latest-arm64.ipk
opkg update && opkg install /tmp/direct-node.ipk && rm -f /tmp/direct-node.ipk

# 3. Enable and start the daemon (Claim Page listens on LuCI-safe port 8081):
/etc/init.d/onmydisk enable
/etc/init.d/onmydisk start
```

Open **`http://192.168.1.1:8081`** while signed in to [**disk.direct**](https://disk.direct) to pair your router. See the **[OpenWrt Routers Guide](/openwrt/)** for architecture tables and LuCI tunneling.

---

## 4. Windows 10 / 11 Installation (`x64`, Per-User, No Admin)

The Windows node ships as a **self-contained static executable** (no Qt DLLs or Visual C++ Redistributable required) and installs **100% per-user** inside `%LOCALAPPDATA%` — no Administrator prompt, no `Program Files`, and no `HKLM` changes.

### Step 1: Run the PowerShell Installer

Open **Windows PowerShell** as your normal user:

```powershell
# 1) Recommended for laptops & PCs without NVIDIA GPU (Agentic AI + local CPU RAG):
& ([scriptblock]::Create((irm https://forge.disk.direct/repo/Windows/install-direct.ps1))) -Agentic

# 2) Full Local GPU AI Node (PCs with NVIDIA GPU — installs AI Assistant + CUDA PyTorch + local Ollama):
& ([scriptblock]::Create((irm https://forge.disk.direct/repo/Windows/install-direct.ps1))) -Gpu

# 3) Storage-Only P2P Node (fastest, ~30 MB):
irm https://forge.disk.direct/repo/Windows/install-direct.ps1 | iex

# 4) Pin a specific remote GPU node LinkID in Agentic mode:
& ([scriptblock]::Create((irm https://forge.disk.direct/repo/Windows/install-direct.ps1))) -Agentic -InferenceNode <remote-node-linkID>

# 5) Cleanly stop services, remove autostart tasks/shortcuts, and uninstall:
& ([scriptblock]::Create((irm https://forge.disk.direct/repo/Windows/install-direct.ps1))) -Uninstall
```

> **Windows Firewall Prompt**: On first launch, if Windows Firewall asks to allow the node binary, click **Allow** so the local Claim page (`:8081`) is reachable.
{: .note }

### Step 2: Claim the Windows Node & Verify Status

1. Open **`http://localhost:8081`** in your browser while signed in at [**disk.direct**](https://disk.direct) and click **Connect** to pair the Windows PC with your DID.
2. Both the P2P storage daemon (`:8081`) and local AI service (`:4000`) start automatically in the background whenever you sign into Windows — even when running on laptop battery.
3. Verify status anytime in PowerShell:
   ```powershell
   curl.exe http://127.0.0.1:8081/api/pair/info
   curl.exe http://127.0.0.1:4000/api/agentic/status
   ```

---

## 5. macOS Installation (`Apple Silicon`, Per-User, No `sudo`)

The native P2P node for **macOS (`arm64` Apple Silicon — M1 / M2 / M3 / M4)** installs per-user inside `~/Library/Application Support/` with automatic login startup via `launchd` (`~/Library/LaunchAgents`) — no `sudo` password and no system extension prompts required.

### Step 1: Run the Terminal Installer

Open **Terminal** on your Mac:

```bash
# 1) Recommended ultrabook / zero-load setup (Agentic AI + local RAG, remote P2P GPU):
curl -fsSL https://forge.disk.direct/repo/macOS/install-direct.sh | bash -s -- --agentic

# 2) Apple Silicon Unified Memory GPU mode (local Metal Ollama + AI Assistant):
curl -fsSL https://forge.disk.direct/repo/macOS/install-direct.sh | bash -s -- --gpu

# 3) Storage-Only P2P Node:
curl -fsSL https://forge.disk.direct/repo/macOS/install-direct.sh | bash

# 4) Uninstall background agents and files:
curl -fsSL https://forge.disk.direct/repo/macOS/install-direct.sh | bash -s -- --uninstall
```

### Step 2: Claim Your Mac

1. Sign in at [**disk.direct**](https://disk.direct) in Safari, Chrome, Brave, or Edge.
2. Open **`http://localhost:8081`** (or click **Connect** next to your Mac under **Settings → Devices → Detected in Network**).
3. Verify status anytime:
   ```bash
   curl http://127.0.0.1:8081/api/pair/info
   curl http://127.0.0.1:4000/api/agentic/status
   ```

> **Intel Macs & Zero-Install Browser Mode**: On any Mac (`x86_64` or `arm64`), you can also open [**disk.direct**](https://disk.direct) directly in the browser (or install it as a Desktop PWA) to share local folders over WebRTC P2P, manage remote nodes, open **WebSSH** terminals, and chat via the **Public Demo AI Node**.
{: .note }

---

## 6. Remote Terminal (WebSSH) & TCP Port Forwarding (`omd-tunnel`)

Every **Disk Direct** node acts as a secure tunnel endpoint behind NAT — **no public IP, no router port forwarding, and no DDNS required**.

### Browser WebSSH Terminal (Zero Client Install)
If an SSH server is running on your node (`openssh-server` on Linux, **Remote Login** on macOS, or `Win32-OpenSSH` `sshd` on Windows port `22`), open the **Terminal** workspace in [**disk.direct**](https://disk.direct):
* Connects directly from your browser over a **WebRTC DataChannel** using **xterm.js** + **libssh2 (WebAssembly)** straight to `127.0.0.1:22` on the node.

### CLI TCP Forwarding with `omd-tunnel` (SSH, SFTP, IDE Remote, RDP)
For native `ssh`, VS Code / Cursor / Antigravity Remote-SSH, `scp`, or Windows Remote Desktop (`RDP`), use the standalone **`omd-tunnel`** binary installed automatically by `install-direct`:

```bash
# Forward local port 22222 to SSH (:22) on a remote Linux, macOS, or Windows node by its LinkID:
omd-tunnel --listen 22222 <target-node-linkID> 22
ssh -p 22222 user@127.0.0.1

# Or forward local port 13389 to Windows RDP (:3389):
omd-tunnel --listen 13389 <windows-node-linkID> 3389
```

`--listen` mode multiplexes multiple concurrent local TCP connections (SSH `ControlMaster`, SFTP, IDE language servers) over a single gateway link. On Linux/macOS you can also wire `omd-tunnel` directly as an SSH `ProxyCommand` in `~/.ssh/config`:

```sshconfig
Host my-node
    HostName 127.0.0.1
    User alex
    ProxyCommand omd-tunnel <target-node-linkID> 22
```
