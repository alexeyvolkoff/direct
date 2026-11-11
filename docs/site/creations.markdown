---
layout: page
title: Image Generation
permalink: /creations/
nav_order: 8
---
# 🎨 Image Generation (**Creations** — *Optional*)
<hr>

Turn your node into a private generative art studio right inside the **Creations** workspace — accessible with one click from the left sidebar in both **Disk Direct** and **AI Assistant Direct**.

> **Optional Component**: Local image generation is completely **optional** — **AI Assistant** runs chat, semantic search, Memory Cards, and MCP document tools without it. Whenever **ComfyUI** is configured on your GPU node (or reached remotely via **Agentic P2P mode** / the **Public Demo AI Node**), the **Creations** studio activates automatically.
{: .note }

![Creations Studio Workspace](/images/creations.png)

---

## 1. Studio Modes Scaled to Your GPU (`Express`, `Studio` & `Video`)

When **ComfyUI** is connected, **Creations** provides dedicated generation modes at the top of the workspace so you can match model weight to available VRAM:

* **⚡ `Express` (SDXL Mode — Modest VRAM GPUs)**:
  * Built for fast, small-footprint generation on everyday GPUs with **low-to-medium VRAM** (e.g. 8–12 GB).
  * Includes one-click **Art Styles** (`Realistic`, `Perfect`, `Fantasy`, `Tooned`), consistent **Personas** (`+ Pick Persona` + character/style LoRA selectors), and **`+ Add photo`** reference conditioning.
* **✨ `Studio` (`Krea 2` Mode — High-VRAM GPUs)**:
  * Built for high-memory GPUs (**16–24+ GB VRAM** workstations or shared P2P GPU nodes).
  * Runs flagship high-parameter diffusion pipelines powered by **CyberRealistic Krea 2** (alongside curated studio checkpoints), delivering cinematic lighting, lifelike detail, and high-resolution renders saved straight to your private storage.

---

## 2. Two Ways to Connect ComfyUI

### Option A: Agentic P2P Mode (Zero Local GPU)
If your node runs in **Agentic mode** (`AI_PROVIDER = agentic`, e.g., installed with `--agentic` / `-Agentic`), **no local ComfyUI installation is needed**:
* `agentic_tunnel.py` exposes a local listener (`127.0.0.1:28188`) that tunnels ComfyUI HTTP API calls (`/prompt`, `/view`, `/object_info`) and real-time WebSocket progress (`/ws`) over an encrypted **WebRTC DataChannel** to your remote GPU node or the **Public AI Node** (`port 8188`).

### Option B: Local ComfyUI on Your Own GPU Host
To run **Express (SDXL)** and/or **Studio (`Krea 2`)** locally on your own NVIDIA GPU machine:

1. **Install ComfyUI**:
   ```bash
   git clone https://github.com/comfyanonymous/ComfyUI /opt/ComfyUI
   cd /opt/ComfyUI
   python3 -m venv venv && source venv/bin/activate
   pip install -r requirements.txt
   ```
2. **Place Models & LoRAs**:
   Drop your SDXL and/or **Krea 2** checkpoints and character `.safetensors` LoRA weights into `/opt/ComfyUI/models/checkpoints/` and `/opt/ComfyUI/models/loras/`.
3. **Start ComfyUI on port `8188`**:
   ```bash
   python3 main.py --listen 127.0.0.1 --port 8188
   ```
4. **Configure `/opt/direct/ai/config.ini` & Restart**:
   ```ini
   COMFY_API_URL = http://localhost:8188
   WORKFLOW_PATH = flow.json
   COMFY_OUTPUT_DIR = /opt/ComfyUI/output
   COMFY_INPUT_DIR = /opt/ComfyUI/input
   ```
   ```bash
   sudo systemctl restart direct-ai
   ```

---

## 3. Generating Images in Creations & Chat

1. **Creations Workspace**: Click **Creations** in the left sidebar, switch between **⚡ Express** and **✨ Studio**, pick your style or persona presets, and type your prompt in *Describe an image...*.
2. **In-Chat `/generate` Command**: You can also trigger image generation directly inside any conversation:
   ```text
   /generate Golden afternoon light across Provence lavender hills, cinematic wide landscape
   ```

All generated artworks are saved directly into your configured **Disk Direct** storage folder (**Settings → AI Assistant**) and can be browsed, downloaded, or shared via P2P links from any device.
