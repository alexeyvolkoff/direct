---
layout: page
title: Node Deployment & Modes
permalink: /advanced/
nav_order: 2
---
# ⚙️ Node Deployment & Operating Modes
<hr>

## ⚡ TL;DR — Quick Install (Recommended Agentic AI Setup)

For most laptops, desktops, and mini-PCs, run **one command** to install the **On My Disk 3.0 Node** together with **On My Chat** in **Agentic AI mode**. Your files and private RAG knowledge index stay **100% on your device**, while heavy LLM & image generation runs automatically over encrypted P2P on a remote GPU:

**🐧 Linux (`Ubuntu / Debian / Armbian`)**
```bash
curl -fsSL https://forge.onmydisk.net/repo/Linux/install-onmydisk.sh | bash -s -- --agentic
```

**🪟 Windows 10 / 11 (`PowerShell` — per-user, no Administrator rights needed)**
```powershell
& ([scriptblock]::Create((irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1))) -Agentic
```

**🍎 macOS (`Terminal`, Apple Silicon — per-user, no `sudo` needed)**
```bash
curl -fsSL https://forge.onmydisk.net/repo/macOS/install-onmydisk.sh | bash -s -- --agentic
```

> **Have a GPU? Run Full Local AI**: Replace `--agentic` with **`--gpu`** (Linux / macOS Apple Silicon) or `-Agentic` with **`-Gpu`** (Windows NVIDIA GPU) to install **Ollama** locally and run all LLM inference directly on your own hardware (`AI_PROVIDER = local`). See the [mode details below](#1-supported-node-installation--operating-modes).
{: .tip }

> **Storage-Only Node (No AI)** — Need only P2P file sharing and remote SSH access without **On My Chat**? Run the basic installer instead:
>
> - **Linux:** `curl -fsSL https://forge.onmydisk.net/repo/Linux/install-onmydisk.sh | bash`
> - **Windows (`PowerShell`):** `irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1 | iex`
> - **macOS:** `curl -fsSL https://forge.onmydisk.net/repo/macOS/install-onmydisk.sh | bash`
{: .note }

**After installation:** Sign in at [**direct.onmydisk.net**](https://direct.onmydisk.net) and connect your device from **Settings → Devices** (or open `http://localhost:8081` on Windows/macOS, `http://localhost/` on Linux).

---

## Overview & Headless P2P Architecture

The **On My Disk 3.0 Node** (packaged as **OnMyDisk Connector** on Linux and **On My Disk Node** on Windows and macOS) is a lightweight, native C++/Qt P2P daemon with statically linked **WebRTC DataChannels** (`libdatachannel`). It turns any Linux server, single-board computer (SBC), NAS, OpenWrt Wi-Fi router, Windows PC, or Mac into an always-on personal cloud storage node, SSH/TCP tunnel endpoint, and **On My Chat** AI host.

> **OMD 3.0 Headless Architecture**: Unlike legacy 2.x builds, the 3.0 P2P node does not bundle a heavy local web interface. Locally it serves only a lightweight **Claim Page** (`:80` on Linux Connector, `:8081` on OpenWrt routers, Windows, and macOS per-user installs) to pair the device with your decentralized DID account (`PairedDIDs`). All file management, AI chats, Creations, device settings, and WebSSH terminal sessions run from the [**On My Disk Web App**](https://direct.onmydisk.net) or mobile app directly over encrypted P2P DataChannels.
{: .important }

---

## 1. Supported Node Installation & Operating Modes

You can deploy an **On My Disk** node in several combinations depending on your hardware — from a fanless laptop or Raspberry Pi up to a multi-GPU workstation:

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
   * Runs only the native `onmydisk` daemon (`~5–28 MB` binary).
   * Provides P2P file shares, public `/shared/...` links, DID user-to-user sharing, browser **WebSSH** (`xterm.js` + `libssh2.wasm` to local `:22`), external USB `automount` + Samba publishing (Linux), and inbound **`omd-tunnel`** TCP port forwarding.
   * If **On My Chat** is not installed on the node, set `localAI=false` in `onmydisk.conf.user` (or select **On My Chat Public Node** in **Settings → AI Assistant → AI Node**).

2. **Agentic P2P Mode (`AI_PROVIDER = agentic`) — Private Local RAG + Remote GPU**:
   * **Best choice for Windows laptops, MacBooks, and GPU-less Linux mini-PCs.**
   * Runs **On My Chat** (`127.0.0.1:4000`) locally with CPU-friendly `SentenceTransformers` (`paraphrase-multilingual-MiniLM-L12-v2`) and `ChromaDB` (`omd_unified`). **Your private files and vector index never leave your machine.**
   * Heavy inference calls (`Ollama :11434` and `ComfyUI :8188`) are transparently forwarded by `agentic_tunnel.py` (local TCP listeners `127.0.0.1:21434` and `127.0.0.1:28188`) through outbound **WebRTC DataChannels** to a remote GPU node:
     * **Auto-discovery**: if `INFERENCE_NODE = auto` (or empty), On My Chat automatically queries `{GATEWAY_URL}/api/ai-node` and connects to the **On My Chat Public Node**.
     * **Pinned or Dynamic Switching**: point `INFERENCE_NODE = <linkID>` to your own home GPU server, or switch **Runtime mode → Agentic** and pick any online inference node live in **Settings → AI Assistant → AI Node** (propagated per request via `X-OMD-Ai-Mode` / `X-OMD-Ai-Node` headers).

3. **Full Local AI Node (`AI_PROVIDER = local`)**:
   * Runs **On My Chat** (`127.0.0.1:4000`), **Ollama** (`127.0.0.1:11434`) or **RKLLAMA** (`127.0.0.1:8080` on Rockchip RK3588/RK3576 NPU), and optional **ComfyUI** (`127.0.0.1:8188`) on the same host. Zero external inference traffic.

4. **Hybrid & Horde Modes (`AI_PROVIDER = hybrid` / `horde`)**:
   * In `hybrid` mode, **Work** mode, document analysis, and **RAG** always stay on your local Ollama model, while **Fun** / roleplay chats are routed to distributed **AI Horde** community GPUs (`AI_PROVIDER = horde` routes all chat generation to AI Horde).

5. **Public / Shared Guest AI Mode (`GuestPairing=true`)**:
   * Setting `GuestPairing=true` under `[Settings]` in `onmydisk.conf` lets external users pair as `ScopedGuest` (`GuestDIDs`) without a password. Guests can chat with `OnMyChat` (`:4000`) or tunnel inference, using `public`-tagged knowledge and their own E2EE GunDB memory cards, while file RPCs and private shares remain strictly blocked.

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
curl -fsSL https://forge.onmydisk.net/repo/Linux/install-onmydisk.sh | bash -s -- --agentic

# 2) Full Local GPU AI Node (installs Connector + OnMyChat + local Ollama):
curl -fsSL https://forge.onmydisk.net/repo/Linux/install-onmydisk.sh | bash -s -- --gpu

# 3) Rockchip RK3588 / RK3576 NPU Node (points OnMyChat to RKLLAMA on :8080):
curl -fsSL https://forge.onmydisk.net/repo/Linux/install-onmydisk.sh | bash -s -- --rkllama

# 4) Storage-only P2P Node:
curl -fsSL https://forge.onmydisk.net/repo/Linux/install-onmydisk.sh | bash

# 5) Uninstall services and packages:
curl -fsSL https://forge.onmydisk.net/repo/Linux/install-onmydisk.sh | bash -s -- --uninstall
```

### Manual `.deb` Installation

#### Step 1: Install `onmydisk-connector` Package

Pre-built `.deb` packages work out of the box on **Ubuntu 22.04 LTS** and **Ubuntu 24.04 LTS** (`apt` resolves `t64` Qt5 libraries, `samba`, `udisks2`, `ffmpeg`, `poppler-utils`, and `pandoc` automatically):

| Architecture | Direct `.deb` Download |
|--------------|------------------------|
| **x86_64 (`amd64`)** | [onmydisk-connector-amd64.deb](https://forge.onmydisk.net/repo/OnMyDisk/Connector/onmydisk-connector-amd64.deb) |
| **ARM64 (`aarch64`)** | [onmydisk-connector-arm64.deb](https://forge.onmydisk.net/repo/OnMyDisk/Connector/onmydisk-connector-arm64.deb) |
| **ARMHF (`armv7`)** | [onmydisk-connector-armhf.deb](https://forge.onmydisk.net/repo/Ubuntu/focal/onmydisk-connector-armhf.deb) |

```bash
sudo apt update

# x86_64 (amd64)
wget https://forge.onmydisk.net/repo/OnMyDisk/Connector/onmydisk-connector-amd64.deb
sudo apt install ./onmydisk-connector-amd64.deb

# ARM64 (Raspberry Pi 4/5, Orange Pi 5, Firefly RK3588, etc.)
# wget https://forge.onmydisk.net/repo/OnMyDisk/Connector/onmydisk-connector-arm64.deb
# sudo apt install ./onmydisk-connector-arm64.deb

# ARMHF (32-bit ARMv7)
# wget https://forge.onmydisk.net/repo/Ubuntu/focal/onmydisk-connector-armhf.deb
# sudo apt install ./onmydisk-connector-armhf.deb
```

#### Step 2: Configure Service User & Start Daemon

Edit `/etc/onmydisk/onmydisk.conf` so the node accesses your home directory with your local Linux user permissions:

```ini
[FileNode]
Port=80
User=your_username
Group=your_groupname
```

Enable and restart the systemd service:

```bash
sudo systemctl enable --now onmydisk
sudo systemctl restart onmydisk
systemctl status onmydisk
```

#### Step 3: Claim the Linux Node into Your Account

1. Sign in to [**direct.onmydisk.net**](https://direct.onmydisk.net) in your browser.
2. Open the node's local **Claim Page** at `http://<device-lan-ip>/` (or `http://localhost/`, port `80`).
3. Click **Connect in On My Disk** to authorize your DID keypair (`PairedDIDs`).
4. Your Linux machine immediately appears in the sidebar and workspace with P2P file access, settings, and WebSSH terminal support.

#### Step 4 (Optional): Manual AI (`OnMyChat`) Setup on Linux

See **[Personal AI Node](/ainode/)** for the full manual setup. Make sure `uvicorn` runs on **port `4000`** (`--port 4000`), which matches `AiServer=http://localhost:4000` in `/etc/onmydisk/onmydisk.conf` and the P2P chat tunnel:
* **Local GPU / CPU**: `AI_PROVIDER = local` + `Ollama` (`:11434`) or `RKLLAMA` (`:8080`).
* **Agentic Mode (no local GPU)**: install CPU-only PyTorch (`pip install torch --index-url https://download.pytorch.org/whl/cpu`), skip Ollama, and set in `/opt/onmychat/config.ini`:
  ```ini
  GATEWAY_URL = https://direct.onmydisk.net
  AI_PROVIDER = agentic
  INFERENCE_NODE = auto
  INFERENCE_BIND_OLLAMA = 21434
  INFERENCE_BIND_COMFY = 28188
  ```

### Linux NAS Capabilities
Configure directly from **Device Settings** in the Web App:
* 🎛️ **Plug-and-Play USB Automount (`automount=true`)**: Automatically mounts external USB/NVMe drives via `UDisks2` and exposes them as shares.
* 📂 **Automatic RAG Indexing (`autoreindex=true`)**: Filesystem watcher (`2s` debounce + periodic `6h` scan) automatically indexes modified documents into **On My Chat** (`omd_unified` ChromaDB).
* 🌐 **Local Network Shares (`updateSamba=true`)**: Publishes mounted drives via **Samba (SMB)** on your LAN alongside global P2P access.

---

## 3. OpenWrt Routers & Embedded Gateways (`.ipk` — [Full Guide](/openwrt/))

The exact same native C++ P2P Linux node (`DEFINES+=ROUTER`, compiled against `musl` libc) runs directly on **OpenWrt** Wi-Fi routers, travel routers, and ISP/CPE gateways — turning any router with a USB port into a silent `< 5 W` always-on personal cloud and P2P SSH/TCP gateway behind CGNAT.

### Supported OpenWrt Architectures

| Architecture | Typical Router SoCs / Targets | Package (`.ipk`) |
|--------------|-------------------------------|------------------|
| **`mips`** *(big-endian)* | Qualcomm Atheros (`ath79`), Lantiq, Realtek, InnBox (`mips_24kc`) | [onmydisk-latest-mips.ipk](https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-mips.ipk) |
| **`mipsel`** *(little-endian)* | MediaTek `MT7621`, `MT7628` (`ramips` / `mipsel_24kc`) | [onmydisk-latest-mipsel.ipk](https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-mipsel.ipk) |
| **`armv7`** *(32-bit ARM)* | Cortex-A7 / Cortex-A9 (`ipq40xx`, `mvebu`, `bcm53xx`) | [onmydisk-latest-armv7.ipk](https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-armv7.ipk) |
| **`arm64`** *(`aarch64`)* | MediaTek Filogic (`mt7981`/`mt7986`), Qualcomm `ipq807x`, Rockchip, RPi | [onmydisk-latest-arm64.ipk](https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-arm64.ipk) |

> **Ensure Sufficient Storage Space**: Because the `.ipk` bundles a self-contained P2P binary (`/opt/bin/onmydisk`), routers with small internal NOR flash (`16 MB`) should have **OpenWrt `extroot`** (or `/opt` mounted on an external USB flash/SSD drive) before installing. Routers with `128 MB+` NAND/eMMC install directly to internal flash.
{: .note }

### Step 1: Install the `.ipk` Package via SSH

SSH into your OpenWrt router (`ssh root@192.168.1.1`), check the target architecture, and install the matching package:

```sh
# 1. Check router CPU architecture:
opkg print-architecture

# 2. Download the matching package (replace arm64 with mips, mipsel, or armv7):
wget -O /tmp/onmydisk.ipk https://forge.onmydisk.net/repo/OpenWrt/onmydisk-latest-arm64.ipk

# 3. Install package and start the service:
opkg update
opkg install /tmp/onmydisk.ipk
rm -f /tmp/onmydisk.ipk

/etc/init.d/onmydisk enable
/etc/init.d/onmydisk start
```

### Step 2: Claim the Router on Port `8081` (LuCI-Safe)

To avoid conflicting with OpenWrt's **LuCI** web interface on port `80`, the router configuration (`/etc/onmydisk/onmydisk.conf`) binds its local Claim Page on **port `8081`** and automatically ignores internal flash partitions (`/dev/mmcblk0`, `/dev/zram`):

1. Plug a USB flash drive or external USB SSD/HDD into your router.
2. While signed in to [**direct.onmydisk.net**](https://direct.onmydisk.net), open **`http://192.168.1.1:8081`** (or your router's LAN IP on `:8081`).
3. Click **Connect in On My Disk** to pair the router with your DID identity.
4. Your router's USB storage is now accessible worldwide over encrypted **WebRTC DataChannels**, with in-browser **WebSSH** access to `dropbear` (`:22`) and `omd-tunnel` TCP forwarding (`omd-tunnel --listen 18080 <router-linkID> 80` to reach LuCI remotely without exposing ports to the internet).

---

## 4. Windows 10 / 11 Installation (`x64`, Per-User, No Admin)

The Windows node ships as a **self-contained static executable** (`onmydisk.exe` + `omd-tunnel.exe`, no Qt DLLs or Visual C++ Redistributable required) and installs **100% per-user** inside `%LOCALAPPDATA%` — no Administrator prompt, no `Program Files`, and no `HKLM` changes.

### What Gets Installed on Windows

| Component | Path | Purpose |
|-----------|------|---------|
| **`onmydisk.exe`** | `%LOCALAPPDATA%\OnMyDisk\bin` | Headless P2P storage node, Claim page (`:8081`), TCP/WebRTC tunnel bridge |
| **`omd-tunnel.exe`** | `%LOCALAPPDATA%\OnMyDisk\bin` | Outbound CLI port forwarder for SSH, RDP, or HTTP |
| **Launchers & Config** | `%LOCALAPPDATA%\OnMyDisk\etc`, `%LOCALAPPDATA%\OnMyDisk\bin\*.vbs` | Default config (`onmydisk.conf`) + silent VBScript start/open/stop wrappers |
| **Shortcuts & Autostart** | Desktop, Start Menu, Task Scheduler `OnMyDiskNode` | Runs silently at logon (including on laptop battery power) + `HKCU\...\Run` fallback |
| **Portable Python 3.12** *(`-Agentic` / `-Gpu`)* | `%LOCALAPPDATA%\OnMyDisk\python` | Isolated embeddable Python runtime with pre-built binary wheels (no MSVC needed) |
| **On My Chat** *(`-Agentic` / `-Gpu`)* | `%LOCALAPPDATA%\OnMyChat` | Local FastAPI AI & RAG service on `127.0.0.1:4000` (Task Scheduler `OnMyChatAI`) |
| **Knowledge & Embeddings** *(`-Agentic` / `-Gpu`)* | `%LOCALAPPDATA%\OnMyChat\memory_index`, `%USERPROFILE%\.cache\huggingface` | Pre-indexed OMD ChromaDB + cached `paraphrase-multilingual-MiniLM-L12-v2` |
| **Ollama** *(`-Gpu`)* | `%LOCALAPPDATA%\Programs\Ollama` | Local LLM engine (skipped automatically in `-Agentic` mode) |

### Step 1: Run the PowerShell Installer

Open **Windows PowerShell** as your normal user:

```powershell
# 1) Recommended for laptops & PCs without NVIDIA GPU (Agentic AI + local CPU RAG):
& ([scriptblock]::Create((irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1))) -Agentic

# 2) Full Local GPU AI Node (PCs with NVIDIA GPU — installs OnMyChat + CUDA PyTorch + local Ollama):
& ([scriptblock]::Create((irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1))) -Gpu

# 3) Storage-Only P2P Node (fastest, ~30 MB):
irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1 | iex

# 4) Pin a specific remote GPU node LinkID in Agentic mode:
& ([scriptblock]::Create((irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1))) -Agentic -InferenceNode <remote-node-linkID>

# 5) Cleanly stop services, remove autostart tasks/shortcuts, and uninstall:
& ([scriptblock]::Create((irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1))) -Uninstall
```

> **Windows Firewall Prompt**: On first launch, if Windows Firewall asks to allow `onmydisk.exe`, click **Allow** so the local Claim page (`:8081`) is reachable.
{: .note }

### Step 2: Claim the Windows Node & Manage It Without PowerShell

1. Double-click the **On My Disk Node** shortcut on your **Desktop** (or **Start Menu → On My Disk → On My Disk Node**), or open **`http://localhost:8081`** in your browser.
2. Click **Connect in On My Disk** on the Claim page to link the Windows PC to your account.
3. Both `OnMyDiskNode` (`:8081`) and `OnMyChatAI` (`:4000`) start automatically in the background whenever Windows boots and you sign in — **even when running on laptop battery**.
4. To stop the background node from the GUI at any time, use **Start Menu → On My Disk → Stop On My Disk Node**.

### Step 3: Verify Windows Node & Agentic P2P Status

```powershell
# Verify node pairing metadata:
curl.exe http://127.0.0.1:8081/api/pair/info

# Verify OnMyChat & P2P Agentic tunnel listeners (:21434 -> Ollama, :28188 -> ComfyUI):
curl.exe http://127.0.0.1:4000/api/agentic/status

# View logs:
Get-Content "$env:LOCALAPPDATA\OnMyChat\onmychat.log" -Tail 40
explorer "$env:LOCALAPPDATA\Bineon d.o.o.\onmydisk\log"
```

---

## 5. macOS Installation (`Apple Silicon`, Per-User, No `sudo`)

The native **On My Disk 3.0 P2P Node** for **macOS (`arm64` Apple Silicon — M1 / M2 / M3 / M4)** installs completely per-user inside `~/Library/Application Support/` with automatic login startup via `launchd` (`~/Library/LaunchAgents`) — no `sudo` password and no system extension prompts required.

### What Gets Installed on macOS

| Component | Path | Purpose |
|-----------|------|---------|
| **`onmydisk` & Qt5 bundle** | `~/Library/Application Support/OnMyDisk` | Self-contained native `arm64` P2P node, Claim page (`:8081`), WebRTC bridge |
| **`omd-tunnel`** | `~/Library/Application Support/OnMyDisk/bin` (`~/.local/bin/omd-tunnel`) | Outbound CLI TCP port forwarder for SSH, RDP, or HTTP |
| **`launchd` Agents** | `~/Library/LaunchAgents/net.onmydisk.node.plist`, `com.onmychat.ai.plist` | Automatic background startup at user login |
| **On My Chat** *(`--agentic` / `--gpu`)* | `~/Library/Application Support/OnMyChat` | Local FastAPI AI & RAG service on `127.0.0.1:4000` (with portable Python 3.12 fallback if needed) |
| **Ollama** *(`--gpu`)* | `/Applications/Ollama.app` | Apple Silicon Metal-accelerated local LLM engine (skipped in `--agentic` mode) |

### Step 1: Run the Terminal Installer

Open **Terminal** on your Mac:

```bash
# 1) Recommended ultrabook / zero-load setup (Agentic AI + local RAG, remote P2P GPU):
curl -fsSL https://forge.onmydisk.net/repo/macOS/install-onmydisk.sh | bash -s -- --agentic

# 2) Apple Silicon Unified Memory GPU mode (local Metal Ollama + OnMyChat):
curl -fsSL https://forge.onmydisk.net/repo/macOS/install-onmydisk.sh | bash -s -- --gpu

# 3) Storage-Only P2P Node:
curl -fsSL https://forge.onmydisk.net/repo/macOS/install-onmydisk.sh | bash

# 4) Uninstall background agents and files:
curl -fsSL https://forge.onmydisk.net/repo/macOS/install-onmydisk.sh | bash -s -- --uninstall
```

### Step 2: Claim Your Mac

1. Sign in at [**direct.onmydisk.net**](https://direct.onmydisk.net) in Safari, Chrome, Brave, or Edge.
2. Open **`http://localhost:8081`** (or click **Connect** next to your Mac under **Settings → Devices → Detected in Network**).
3. Verify status or inspect logs anytime:
   ```bash
   curl http://127.0.0.1:8081/api/pair/info
   curl http://127.0.0.1:4000/api/agentic/status
   tail -n 40 ~/Library/Application\ Support/OnMyChat/onmychat.log
   ```

> **Intel Macs & Zero-Install Browser Mode**: On any Mac (`x86_64` or `arm64`), you can also open [**direct.onmydisk.net**](https://direct.onmydisk.net) directly in the browser (or install it as a Desktop PWA) to share local folders over WebRTC P2P, manage remote nodes, open **WebSSH** terminals, and chat via the **On My Chat Public Node**.
{: .note }

---

## 6. Remote Terminal (WebSSH) & TCP Port Forwarding (`omd-tunnel`)

Every **On My Disk 3.0** node acts as a secure tunnel endpoint behind NAT — **no public IP, no router port forwarding, and no DDNS required**.

### Browser WebSSH Terminal (Zero Client Install)
If an SSH server is running on your node (`openssh-server` on Linux, **Remote Login** on macOS, or `Win32-OpenSSH` `sshd` on Windows port `22`), open the **Terminal** applet in the On My Disk Web App:
* Connects directly from your browser over a **WebRTC DataChannel** using **xterm.js** + **libssh2 (WebAssembly)** straight to `127.0.0.1:22` on the node.

### CLI TCP Forwarding with `omd-tunnel` (SSH, SFTP, IDE Remote, RDP)
For native `ssh`, VS Code / Cursor / Antigravity Remote-SSH, `scp`, or Windows Remote Desktop (`RDP`), use the standalone **`omd-tunnel`** binary:
* **Linux (`amd64`)**: installed automatically by `install-onmydisk.sh` (or standalone `https://forge.onmydisk.net/repo/Ubuntu/focal/omd-tunnel-amd64.deb`)
* **Windows (`x64`)**: installed automatically at `%LOCALAPPDATA%\OnMyDisk\bin\omd-tunnel.exe` (or standalone `https://forge.onmydisk.net/repo/Windows/omd-tunnel-x64.exe`)
* **macOS (`arm64`)**: installed automatically at `~/.local/bin/omd-tunnel` (`~/Library/Application Support/OnMyDisk/bin/omd-tunnel`)

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
    User alexey
    ProxyCommand omd-tunnel <target-node-linkID> 22
```
