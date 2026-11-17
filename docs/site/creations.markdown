---
layout: page
title: Image Generation
permalink: /creations/
nav_order: 8
---
# 🎨 Image Generation (**Creations** - *Optional*)
<hr>

Turn your node into a private generative art studio right inside the **Creations** workspace - accessible with one click from the left sidebar in both **Disk Direct** and **AI Assistant Direct**.

> **Optional Component**: Local image generation is completely **optional** - **AI Assistant** runs chat, semantic search, Memory Cards, and MCP document tools without it. Whenever **ComfyUI** is reachable on `:8188` (locally, via the cooperative `reGen` bridge, or over **Agentic P2P**) - or when **AI Horde** image fallback is enabled - the **Creations** studio activates automatically.
{: .note }

![Creations Studio Workspace](/images/creations.png)

---

## 1. Studio Modes & Creative Controls

* **⚡ `Express` (Fast SDXL + DMD2 Studio - Everyday 8-12+ GB GPUs)**:
  * Powered by an 8-step **SDXL + DMD2 (`lcm` / `exponential`)** ComfyUI workflow (`flow.json`) with curated **Base Models** (`Realistic`, `Perfect`, `Fantasy`, `Tooned`), **Art Styles**, **Character** LoRAs with live thumbnails, and a collapsible **Negative Prompt** editor.
  * **CivitAI LoRA Browser**: Search and attach compatible **Characters** and **Styles / Concepts** directly from CivitAI inside the composer, AI Persona Factory, or Manual Persona Editor. Weights are downloaded and cached automatically on first render (using your personal CivitAI API key from **Settings -> Integrations (`mcp_keys`)** when configured).
  * **`+ Add photo` One-Shot Reference Conditioning**: Attach a portrait reference photo (or select a Persona with a reference avatar) to activate local **IP-Adapter Plus Face** + **ReActor HyperSwap 256** + **GFPGAN** + **Impact Auto-Inpaint** face & hand refinement.
  * **Zero-GPU AI Horde Fallback**: When no local `:8188` engine is available (or when **`Horde AI`** is selected in **Settings -> AI Node**), **Express** automatically maps prompts, negative prompts, dimensions, and open SDXL CivitAI LoRAs to **[AI Horde](/horde/)**.
  * **`Work / Fun` Safety Filter & `#sensitive` Blur**: In **Work** mode (and for guests), safety negative prompts prevent unintended nudity on expressive SDXL checkpoints. NSFW generations are automatically tagged `#sensitive` in companion `.Readme.md` sidecars and Memory Cards, blurred in **Work** mode across **Creations**, **Memory Cards**, and **[Media Library](/media/)**, and permanently blurred on public shared links (requiring local download to view).
* **✨ `Studio` (`Krea 2` & `Anima` - High-VRAM Workstation / Serverless Cloud)**:
  * Optional high-parameter mode for **16-24+ GB VRAM** setups or self-hosted serverless cloud GPUs (`MODAL_COMFY_URL`), supporting **Krea 2** and **Anima** base models, visual Character & Style presets, and ecosystem-filtered **CivitAI LoRA** imports (`Krea 2` / `Anima`).

---

## 2. Connecting ComfyUI (Official & Advanced Options)

### Option A: Official Classic Setup - Standalone Local ComfyUI (`127.0.0.1:8188`)
Recommended for dedicated desktop or home-server NVIDIA GPUs:

1. **Install ComfyUI**:
   ```bash
   git clone https://github.com/comfyanonymous/ComfyUI /opt/ComfyUI
   cd /opt/ComfyUI
   python3 -m venv venv && source venv/bin/activate
   pip install -r requirements.txt
   ```
2. **Place Models & Optional Custom Nodes**:
   * Place your SDXL checkpoints and `dmd2_sdxl_4step_lora.safetensors` into `/opt/ComfyUI/models/checkpoints/` and `/opt/ComfyUI/models/loras/` (additional CivitAI LoRAs selected in the UI are downloaded automatically into `loras/civitai_cache/`).
   * For `+ Add photo` face/reference workflows, install `ComfyUI_IPAdapter_plus`, `comfyui-reactor-node`, `ComfyUI-Impact-Pack`, `ComfyUI-Impact-Subpack`, and `ComfyUI_essentials` under `custom_nodes/`.
3. **Start ComfyUI on loopback port `8188`**:
   ```bash
   python3 main.py --listen 127.0.0.1 --port 8188
   ```
4. **Configure `/opt/direct/ai/config.ini` & Restart**:
   ```ini
   COMFY_API_URL = http://127.0.0.1:8188
   WORKFLOW_PATH = flow.json
   COMFY_OUTPUT_DIR = /opt/ComfyUI/output
   COMFY_INPUT_DIR = /opt/ComfyUI/input
   ```
   ```bash
   sudo systemctl restart direct-ai
   ```

### Option B: Agentic P2P Mode (Zero Local GPU)
If your node runs in **Agentic mode** (`AI_PROVIDER = agentic`, installed with `--agentic` / `-Agentic`), no local ComfyUI installation is needed:
* `agentic_tunnel.py` exposes a local listener (`127.0.0.1:28188`) that tunnels ComfyUI API calls and WebSocket progress over an encrypted **WebRTC DataChannel** to your paired remote GPU node or the **Public AI Node**.

### Option C: Advanced Single-GPU Sharing with `horde-worker-reGen`
If your GPU already runs **`horde-worker-reGen`** 24/7 to earn Kudos on AI Horde, you can reuse its internal ComfyUI engine cooperatively on `127.0.0.1:8188` via `ai/regen_comfy_bridge` without running a second CUDA process - see **[AI Horde & Cooperative `reGen` Bridge](/horde/#3-advanced-single-gpu-setup-horde-worker-regen--cooperative-comfyui-bridge)**.

### Option D: Optional Serverless Cloud Backend for `Studio` (`Krea 2` / `Anima`)
Advanced self-hosters who want on-demand **24-40 GB VRAM** cloud bursts for `Krea 2`, `Anima`, and video pipelines without keeping heavy weights on local hardware can point `MODAL_COMFY_URL` (and `MODAL_AUTH_TOKEN`) in `config.ini` to their own serverless ComfyUI endpoint (for example on [Modal](https://modal.com) backed by a persistent models volume). Dynamic CivitAI LoRAs (`civitai_<versionId>`) selected in `Studio` are cached directly onto the remote volume on demand.

---

## 3. Generating Images in Creations & Chat

1. **Creations Workspace**: Click **Creations** in the left sidebar, pick your **Base Model**, **Art Style**, **Character**, or **Persona**, optionally attach a reference photo (`+ Add photo`) or open **Director's Chair** for AI-assisted prompt staging, and click **Generate**.
2. **In-Chat `/generate` & RP Selfies**: Trigger image generation directly inside any chat (including automatic Persona appearance/LoRA preservation):
   ```text
   /generate Golden afternoon light across Provence lavender hills, cinematic wide landscape
   ```

All generated artworks are saved straight into your own **Disk Direct** storage folder (**Settings -> AI Assistant**) and can be browsed, downloaded, or shared via P2P links from any device.
