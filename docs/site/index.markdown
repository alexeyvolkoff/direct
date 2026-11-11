---
layout: home
title: Home
nav_order: 0
---
## Welcome to **Disk Direct** & **AI Assistant Direct**
<hr>

Welcome to the official documentation for [**Disk Direct**](https://disk.direct) and [**AI Assistant Direct**](https://aiassistant.direct) — a decentralized personal cloud and private AI workspace where your files, knowledge index, and cryptographic identity stay under your control. Data travels directly between your devices over encrypted **WebRTC DataChannels**, while the signaling gateway coordinates peer discovery and fallback relay without storing your files.

![Disk Direct Workspace](/images/file_manager.png)

### 📁 Core Storage, Nodes & Connectivity
Connect browsers, mobile devices, laptops, mini-PCs, NAS boxes, and routers into a unified multi-device P2P storage fabric:
* **[Getting Started](/getstarted/)**: Set up decentralized DID sign-in, explore **Files**, **Chats**, **Creations**, and **Knowledge** from the sidebar, connect local folders or hardware nodes, and create secure public or DID shares.
* **[Decentralized Identity (DID)](/did/)**: Self-sovereign `did:gun` keypairs, flexible display nicknames with `#tag` fingerprints, one-click `.json` keyfile backup & passwordless sign-in, and P2P node trust.
* **[Node Deployment & Modes (Linux, OpenWrt, Windows, macOS)](/advanced/)**: Install the standalone P2P node on **Linux**, **OpenWrt routers** (`mips`/`mipsel`/`armv7`/`arm64`), **Windows**, or **macOS**, explore all operating modes (**Storage-only**, **Agentic P2P**, **Full Local AI**, **Hybrid**, **Public Guest AI**), and use **WebSSH** & **`omd-tunnel`**.
* **[OpenWrt Routers](/openwrt/)**: Dedicated `.ipk` installation guide (`mips`, `mipsel`, `armv7`, `arm64`) for turning Wi-Fi routers with USB storage into `< 5 W` always-on P2P cloud nodes and LuCI/SSH gateways.
* **[External Cloud Storage (rclone)](/external/)**: Mount **ProtonDrive**, **WebDAV**, and other external cloud providers directly into your node.

### 🤖 Private AI & Knowledge Workspace
Turn your storage into an intelligent, privacy-preserving workspace powered by **AI Assistant Direct**:
* **[Personal AI Node](/ainode/)**: Deploy the local AI service (`:4000`) with **Ollama** (CPU/CUDA/Metal), **Rockchip RKLLAMA** (NPU), **Hybrid**, or zero-GPU **Agentic P2P** inference — plus **MCP** tools.
* **[Semantic Search](/searchnode/)**: Search documents, photos, and people by meaning across 50+ languages using the unified multimodal vector index.
* **[Storage as Knowledge (RAG)](/rag/)**: Browse multimodal **Memory Cards** right from the sidebar, find specific people and photos with on-device facial & vision recognition, and chat with your private documents.
* **[Agentic Loop & MCP](/mcp/)**: Automate multi-step tasks and Office document creation (`WordCraft`, `GridCraft`, `DeckCraft`) in chat or connect external MCP agents.
* **[Image Generation (Creations)](/creations/)** *(Optional)*: Render artworks in **Creations** (`Express`, `Studio`, and `Video`) via local or P2P-tunneled **ComfyUI**.
* **[AI Horde & Dreamer Worker](/horde/)**: Tap into crowdsourced **AI Horde** (`aihorde.net`) 70B–123B creative models and turn your idle local **ComfyUI** GPU into a 24/7 **Dreamer Worker** (`/horde`) to earn Kudos automatically.
* **[Terminal & Coding](/coding/)**: Access your nodes via the in-browser P2P SSH terminal and run any autonomous TUI coding harness (**OpenCode**, **Claude Code**, **Codex**, **Aider**, **Hermes**) or connect desktop IDEs via `omd-tunnel` and **MCP**.
* **[P2P App Proxy](/apps/)**: Launch any localhost web application (`127.0.0.1:<port>` — **ComfyUI**, **GenVault**, **Memos**, **DocuSeal**, **Gitea**, **Home Assistant**, and more) directly inside **Disk Direct** over encrypted WebRTC DataChannels with zero open firewall ports.

### 🌐 Publishing, Hardware & Support
* **[Sites Engine](/sites/)**: Publish Jekyll websites directly from your storage node over P2P tunnels.
* **[Folder Synchronization](/sync/)**: Mobile camera auto-backup, Obsidian vault sync, and scheduled backup workflows.
* **[Recommendations](/recommendations/)**: Recommended hardware profiles (routers, SBCs, mini-PCs, GPU workstations, and CPU-only Agentic laptops) and security best practices.
* **[About](/about/)**: Core architectural principles and open-source mission behind **Disk Direct**.
* **[Support](/support/)**: Official community and support channels (Discord, GitHub Issues, Email, and account/privacy help).

<br>
*Select a guide from the left navigation sidebar or the links above to get started!*
