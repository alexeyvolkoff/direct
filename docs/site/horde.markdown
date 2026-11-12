---
layout: page
title: AI Horde & Dreamer Worker
permalink: /horde/
nav_order: 8.5
---
# ⚡ AI Horde Client & Built-in ComfyUI Dreamer Worker
<hr>

**AI Assistant Direct** integrates natively with [**AI Horde**](https://aihorde.net) — a crowdsourced, distributed cluster of community GPU workers for text and image generation — both as a **Hybrid Client** and as a built-in, idle-aware **ComfyUI Image Worker ("Dreamer")**.

* **As a Client (`hybrid` / `horde` modes)**: Keep your private documents, spreadsheets, and code strictly local on **Ollama** in **Work Mode**, while seamlessly tapping into massive **70B–123B community models** on **AI Horde** for creative storytelling and roleplay in **Fun Mode** — complete with **1-click SillyTavern Character Card (`.png` / `.json`) import**.
* **As a Worker (`/horde`)**: Whenever your local **ComfyUI** queue is idle, your node can automatically pick up image generation jobs from AI Horde (`hordelib`-compatible SDXL text-to-image pipeline and optional daily-capped `Krea 2`), render them in the background, and earn **Kudos** for your account.

![AI Horde Dreamer Worker Console](/images/horde_worker.png)

---

## 1. Hybrid Mode: Local Work RAG + AI Horde Roleplay

In **Settings → AI Assistant → AI Node** (or in `/opt/direct/ai/config.ini`), you can select how **AI Assistant** routes your requests:

| Runtime Mode | Work Mode (RAG, Docs, Code, MCP) | Fun Mode (Creative Writing & Roleplay) |
| :--- | :--- | :--- |
| **`Local`** | Local **Ollama** (`:11434`) | Local **Ollama** (`FUN_MODEL`) |
| **`Hybrid (AI Horde for fun mode)`** | **Local Ollama** (100% private local RAG) | **AI Horde** (`70B–123B` community RP models) |
| **`AI Horde only`** | **AI Horde** | **AI Horde** |
| **`Agentic P2P`** | Remote GPU node over WebRTC P2P | Remote GPU node over WebRTC P2P |

![AI Horde Hybrid Mode & SillyTavern Character Import](/images/horde_assistant.png)

### SillyTavern Character Card Import
Under **Settings → AI Assistant → Assistant Profile**, click **Import Character** to load any standard **SillyTavern V2 Character Card** (`.png` with embedded metadata or `.json`). The app automatically populates the assistant's name, persona lore, greeting, avatar, and roleplay prompt while preserving your serious **Work Mode** prompt for day-to-day productivity.

---

## 2. Built-in ComfyUI Dreamer Worker (`/horde`)

Instead of installing and maintaining a separate Docker container or standalone bridge, the **AI Assistant** node includes a native **AI Horde Image Worker ("Dreamer")** (`horde_dreamer.py`) that talks directly to your existing local **ComfyUI** instance (`http://localhost:8188`).

### Key Capabilities
1. **Strict Idle Detection (Zero Interruption)**:
   * Before polling `/api/v2/generate/pop`, the worker checks your local ComfyUI `/queue` (`queue_running` and `queue_pending`) as well as active Studio/Autopilot locks.
   * If you or your node are using ComfyUI, the Dreamer worker immediately yields and waits until the GPU is completely idle.
2. **`hordelib`-Compatible SDXL ComfyUI Pipeline & Strict Reference Verification**:
   * Builds a clean, unmodified ComfyUI SDXL text-to-image graph (`CheckpointLoaderSimple -> CLIPSetLastLayer -> CLIPTextEncode -> EmptyLatentImage -> KSampler -> VAEDecode`) honoring the requester's exact `steps`, `cfg_scale`, `sampler_name`, `karras` / `scheduler`, `clip_skip`, `seed`, `width`, `height`, and unmodified prompts.
   * Strictly offers only official AI Horde models (`AI-Horde-image-model-reference`) whose exact `.safetensors` checkpoints are installed in your local `ComfyUI/models/checkpoints/` directory and pass **exact byte-size and SHA256 digest verification** against `AI-Horde-image-model-reference` (e.g., **`CyberRealistic Pony`**, **`Pony Realism`**, **`AlbedoBase XL (SDXL)`**, **`SDXL 1.0`**, **`WAI-NSFW-illustrious-SDXL`**). Unexpected or unverified models are never substituted with another checkpoint.
3. **Optional Daily-Capped Modal `Krea2-Turbo_fp8`**:
   * If a remote Modal GPU endpoint is configured (`MODAL_COMFY_URL`) and the official `Krea2-Turbo_fp8` reference weights pass SHA256 verification, you can optionally contribute `Krea2-Turbo_fp8` with a strict daily generation cap (e.g. `HORDE_KREA_DAILY_LIMIT = 10` per UTC day). Once the daily quota is reached, `Krea2-Turbo_fp8` is automatically removed from `/v2/generate/pop` while local SDXL continues 24/7 at zero cost.
4. **Mandatory `horde-safety` (NSFW & CSAM) Scanning & Maintenance Mode Compliance**:
   * Every generated image is scanned via [`horde-safety`](https://github.com/Haidra-Org/horde-safety) (`NSFWChecker` with `CLIP ViT-L/14` + `DeepDanbooru` on CPU, using 0 MB of GPU VRAM) prior to R2 upload and `/v2/generate/submit`.
   * Fail-closed safety: the worker refuses to pop jobs if `horde-safety` is unavailable, replaces CSAM or SFW-censored outputs with official AI Horde censor images (`nsfw_censor_*.png`), and reports `state: "csam"` / `"censored"` and `gen_metadata`.
   * If the worker is placed into **Maintenance Mode** on AI Horde, it automatically pauses polling (`enabled = false`) and never auto-clears the maintenance flag.
5. **Built-in Web Console & P2P Node App**:
   * Accessible locally at **`http://localhost:4000/horde`** or remotely from anywhere as the **AI Horde** app inside **Disk Direct** via the **[P2P App Proxy](/apps/)**.
   * Displays live status (`RENDERING` / `IDLE_POLLING`), Kudos balance, session Megapixelsteps (MPS), model toggles, daily quota controls, and a real-time log of fulfilled jobs.

---

## 3. Configuration (`config.ini`)

1. Register an account and generate an API key at **[aihorde.net/register](https://aihorde.net/register)**.
2. Add or uncomment the AI Horde settings in `/opt/direct/ai/config.ini` (or configure them directly in the **`/horde`** web UI):

```ini
[settings]
# Client API key (0000000000 = anonymous client; use your key for priority queue)
HORDE_API_KEY = your_aihorde_api_key

# Built-in ComfyUI Dreamer Worker
HORDE_WORKER_ENABLED = true
HORDE_WORKER_API_KEY = your_aihorde_api_key
HORDE_WORKER_NAME = Direct-Dreamer
HORDE_WORKER_NSFW = true
HORDE_WORKER_MAX_PIXELS = 1048576
HORDE_WORKER_MODELS = CyberRealistic Pony
HORDE_KREA_DAILY_LIMIT = 10

[apps]
horde = AI Horde | 4000 | /horde | gpu-card | AI Horde Dreamer Worker Console
```

> **Residential ISP & CGNAT (`UnsafeIP` Notice)**: AI Horde uses automated IP reputation filters to block abusive VPN workers. If your residential fiber or CGNAT connection triggers a false-positive `UnsafeIP` message in the `/horde` console, join the **[Official AI Horde Discord](https://discord.gg/3DxrhksKzn)**, link your API key via `/userinfo`, and ask a moderator in `#worker-vpn-access-request` to enable the VPN exception flag on your account.
{: .note }
