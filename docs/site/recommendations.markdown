---
layout: page
title: Recommendations
permalink: /recommendations/
nav_order: 98
---
# 💡 Hardware & Best-Practice Recommendations
<hr>

## 1. Recommended Node Hardware Profiles

Choose the setup that best matches your workload and power budget:

| Use Case | Recommended Hardware | Recommended Mode |
|----------|----------------------|------------------|
| **Zero-Noise USB Cloud on Wi-Fi Router** | OpenWrt router (`ARM64`, `ARMv7`, `MIPSEL`, `MIPS`, `128 MB+` flash or USB `extroot` + USB 3.0 SSD) | OpenWrt `.ipk` **Storage-Only** node (`< 5 W`) + **Public Demo AI Node** in UI |
| **Everyday Laptop / Ultrabook** | Any Windows 10/11, macOS, or Linux laptop (8–16 GB RAM, no discrete GPU needed) | **`--agentic` / `-Agentic`** (`AI_PROVIDER = agentic` — local CPU RAG + P2P remote GPU inference) |
| **Silent 24/7 Home NAS + AI Bridge** | Intel N100 / AMD Ryzen Mini-PC (e.g. Beelink, Minisforum, 16–32 GB RAM, NVMe SSD) | Linux node + **Agentic** or **Hybrid** mode |
| **Low-Power Offline AI Appliance** | Rockchip **RK3588** SBC (Orange Pi 5 / Firefly / Rock 5B, 16 GB RAM + 6 TOPS NPU) | Linux `arm64` node + **RKLLAMA** (`gemma-2-2b-it-rk3588`) |
| **Full Private AI & ComfyUI Workstation** | Linux, Windows, or Apple Silicon Mac with **NVIDIA RTX 3060 12 GB / 4070 / 4090** or **M-series Unified Memory** (32+ GB RAM, NVMe SSD) | **`AI_PROVIDER = local`** (`Ollama` `gemma4` + local `ComfyUI`) |

---

## 2. Tips for High RAG & Search Quality

* **Descriptive Filenames & Folder Hierarchy**: Clear folder structures and meaningful filenames significantly improve both hybrid filesystem lookup and semantic chunk ranking.
* **Leverage `.Readme.md` Sidecars & `#hashtags`**: Adding short descriptions or `#project` tags in notes or `<file>.Readme.md` sidecars gives the **AI Assistant** high-signal metadata during retrieval; keep `AUTO_RECOGNIZE_IMAGES = true` enabled if you want photos indexed automatically.
* **Preserve Document Metadata**: Avoid stripping PDF/Office titles, headings, and author metadata — **WordCraft** and `pandoc` use document structure to produce cleaner semantic chunks.

---

## 3. Security Best Practices

* **Back Up Your `.json` DID Keyfile Offline**: Your root `did:gun` keypair controls device certificates and E2EE GunDB chat/knowledge graphs — the server cannot reset or decrypt it for you. Save a copy in your password manager (**Settings → Security → Save Keyfile**).
* **Review Paired Devices Periodically**: In **Settings → Devices**, revoke any old browsers or lost devices with one click.
* **Use Scoped Shares for External Collaborators**: Share specific folders with expiration timers or passwords rather than pairing guest browsers as admin `PairedDIDs`.
