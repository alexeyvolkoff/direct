---
layout: page
title: AI Horde & reGen Bridge
permalink: /horde/
nav_order: 8.5
---
# ⚡ AI Horde Client & Cooperative `reGen` Bridge
<hr>

**AI Assistant Direct** integrates natively with [**AI Horde**](https://aihorde.net) - a crowdsourced, distributed cluster of community GPU workers for text and image generation - as a first-class **Text & Image Client**, plus an optional zero-copy **Cooperative ComfyUI Bridge** (`ai/regen_comfy_bridge`) and owner **`/horde`** telemetry console for operators running the official [**`horde-worker-reGen`**](https://github.com/Haidra-Org/horde-worker-reGen) worker.

* **As a Client (`Horde AI` node, `hybrid` / `horde` modes & Studio Fallback)**: Keep your private documents, spreadsheets, and code strictly local on **Ollama** in **Work Mode**, or run 100% through **AI Horde** (`8B-128B+` community text models via `https://oai.aihorde.net/v1` SSE streaming + **SDXL** image generation via `/api/v2/generate/async` with CivitAI LoRA mapping, SFW guardrails, and 1-click SillyTavern `.png` / `.json` V2 Character Card import).
* **Official Worker Policy (No Custom Standalone Worker)**: Direct **does not** bundle or run a custom third-party AI Horde worker. Node operators who want to volunteer idle GPU cycles to earn **Kudos** run the official upstream [**`horde-worker-reGen`**](https://github.com/Haidra-Org/horde-worker-reGen) (for image generation) or [**`KoboldCpp`**](https://github.com/LostRuins/koboldcpp) (for text generation), keeping 100% of AI Horde protocol handling, reference model verification, and `horde-safety` pipelines on official Haidra-Org worker software.
* **Single-GPU Cooperative Bridge (`/horde` & `:8188`)**: When your GPU runs official **`horde-worker-reGen`**, Direct's non-invasive **`ai/regen_comfy_bridge`** lets **Creations** Studio share `hordelib`'s embedded ComfyUI CUDA context on `127.0.0.1:8188` **between** AI Horde jobs without starting a second CUDA process or modifying a single file inside `horde-worker-reGen`.

![AI Horde reGen Telemetry & ComfyUI Bridge Console](/images/horde_worker.png)

---

## 1. AI Horde Client Modes (`Settings -> AI Node`)

In **Settings -> AI Node** (or in `/opt/direct/ai/config.ini`), you can use **AI Horde** either as a standalone **AI Node** choice (available to all users, even without a personal hardware node) or as a **Runtime mode** on your own node:

* **Standalone `Horde AI` Node (`AI Node: Horde AI`)**: Select **`Horde AI`** directly in the top **AI Node** dropdown, enter your optional **Horde API Key** (`0000000000` = anonymous, or personal key for priority Kudos), pick **Auto** or a preferred **Horde Model**, and click **Save AI Node Settings**. Both **Work** and **Fun** chats plus **Creations** (`Express`) run directly through the AI Horde community cluster.
* **Personal Node Runtime Modes (`Runtime mode`)**:

| Runtime Mode | Work Mode (RAG, Docs, Code, MCP) | Fun Mode & Creations Studio |
| :--- | :--- | :--- |
| **`Local`** | Local **Ollama** (`:11434`) | Local **Ollama** (`FUN_MODEL`) & local **ComfyUI** (`:8188`) |
| **`Hybrid (AI Horde for fun mode)`** | **Local Ollama** (100% private local RAG) | **AI Horde** (`8B-128B+` community RP models) + local **ComfyUI** / **AI Horde** image fallback |
| **`Horde (all inference via AI Horde)`** | **AI Horde** | **AI Horde** (text & SDXL image generation) |
| **`Agentic P2P`** | Remote GPU node over WebRTC P2P | Remote GPU node over WebRTC P2P |

### Real-Time OAI SSE Text Streaming (`ai/horde_client.py`)
* **Primary OpenAI-Compatible SSE Transport (`https://oai.aihorde.net/v1/chat/completions`)**: Supports real-time token streaming, `reasoning_content` thinking deltas, `tool_calls`, multimodal vision inputs, multi-model fallback (`models` up to 5), and `x_horde` worker/Kudos metadata, with automatic fallback to legacy `/api/v2/generate/text/async` polling for models served only by non-OAI workers.
* **Quality-Filtered `>= 8B` Catalog & Live Worker Badge**: Automatic model selection and offline-model fallback strictly filter out tiny `< 8B` and debug workers (retaining capable `8B-128B+` dense and MoE models, with 5-minute fault quarantine). Every AI Horde chat reply displays a compact badge (`⚡` when answered by your primary model, or `↪` when answered by an online fallback worker) showing the exact model that generated the response.

![AI Horde Hybrid Mode & SillyTavern Character Import](/images/horde_assistant.png)

### Client Image Mapping, SFW Guardrails & CivitAI LoRAs (`ai/horde_image_mapper.py`)
* **Studio-to-Horde Mapper**: Translates **Creations** (`Express`) and in-chat `/generate` prompts, negative prompts (`###`), aspect ratios, steps, samplers, verbal Persona passports, and open-download SDXL CivitAI LoRAs (`<= 400 MB`, up to 5 LoRAs, plus Haidra's pre-cached `247778` 8-step TurboMix accelerator or 18-step `k_dpmpp_2m Karras`) into compliant `/api/v2/generate/async` requests (`Client-Agent: Direct:1.0.0:https://github.com/alexeyvolkoff/direct`).
* **Strict SFW Defaults & Kudos Budgets**: Enforces hardcoded SFW mode (`nsfw: false`, `censor_nsfw: true`, `replacement_filter: true`, and SFW negative prompt guardrails) in **Work** mode and for all guests. Node owners can optionally sponsor priority guest generations up to configurable daily Kudos caps (`HORDE_GUEST_DAILY_KUDOS_LIMIT`, `HORDE_PER_USER_DAILY_KUDOS_LIMIT`), automatically falling back to zero-cost anonymous (`0000000000`) SD 1.5 (`<= 512x640`) once the daily budget is reached.
* **Optional Personal CivitAI API Key (`mcp_keys`)**: If a user configures a personal CivitAI API key in **Settings -> Integrations (`mcp_keys`)**, Direct authenticates CivitAI catalog searches and creator-gated LoRA metadata lookups on their behalf without storing shared tokens in `config.ini`.
* **Deterministic Request Lifecycle**: Completed generations (`done: true`) always take precedence over current worker availability (`is_possible: false`), and unfinished tasks that time out or are cancelled by the caller are explicitly aborted via `DELETE /api/v2/generate/status/{id}` (and `/generate/text/status/{id}`).

---

## 2. Official `horde-worker-reGen` + Cooperative ComfyUI Bridge

Node operators who want to earn **Kudos** on AI Horde run the official [**`horde-worker-reGen`**](https://github.com/Haidra-Org/horde-worker-reGen) worker (`SDXL`, `Pony`, `Flux.1-Schnell`, LoRAs, ControlNet, img2img, upscalers). On a single 12-24 GB GPU, `hordelib` already runs its own internal ComfyUI runtime inside `HordeInferenceProcess`, leaving insufficient VRAM for a second standalone ComfyUI server.

Direct solves this cleanly via **`ai/regen_comfy_bridge`** (`sitecustomize.py` + `bridge.py`), **without modifying a single file inside `horde-worker-reGen` or `hordelib`**:

1. **100% Official AI Horde Execution**:
   * `horde-worker-reGen` owns all AI Horde job popping, reference checkpoint verification, `horde-safety` (`CLIP` + `DeepDanbooru`) checks, and result submission untouched.
2. **Single CUDA Context & Cooperative Turn-Taking (`127.0.0.1:8188`)**:
   * Prepending `PYTHONPATH=/opt/direct/ai/regen_comfy_bridge` in `horde-worker-reGen`'s startup environment hooks `HordeInferenceProcess` at import time and starts `hordelib`'s native `PromptServer` + `PromptQueue` on `127.0.0.1:8188`.
   * Inside `worker_cycle()`, any queued local **Creations** Studio / RP Chat workflow on `:8188` executes **between** AI Horde jobs on the exact same CUDA main thread: `hordelib` models are gracefully unloaded from VRAM (`comfy.model_management.unload_all_models()`), Direct's `flow.json` graph runs at full GPU speed (~3.5s for 8-step SDXL + DMD2, or ~7s with IP-Adapter Plus Face + ReActor HyperSwap + GFPGAN + Impact Auto-Inpaint), and `horde-worker-reGen` resumes AI Horde jobs seamlessly afterward.
3. **Full `/comfyui` Web Editor & Owner-Only `/horde` Telemetry Console**:
   * If a local ComfyUI `venv` (`comfyui-frontend-package`) and `custom_nodes` directory exist on the host, `bridge.py` mounts custom nodes (`IPAdapter`, `ReActor`, `Impact-Pack`, `Essentials`, `rgthree`) and serves the modern ComfyUI web UI on `127.0.0.1:8188` (accessible remotely via the **ComfyUI** P2P Node App).
   * The owner-only **`/horde`** console (`http://127.0.0.1:4000/horde` or the **AI Horde** Node App; returns `404` to non-owners/guests) inspects `horde-regen.service` (`bridgeData.yaml`) and live AI Horde worker status to display earned Kudos, Megapixelsteps, fulfilled requests, `:8188` GPU VRAM availability, advertised Horde models, Studio checkpoints, and a 1-click **Maintenance Mode** toggle (`PUT /api/v2/workers/{id}`).

### Launching `horde-worker-reGen` with `DirectComfyBridge` & Recommended Tuning

For smooth 24/7 operation on a 12-24 GB single-GPU workstation without CPU saturation:

1. **In `bridgeData.yaml`** (inside your `horde-worker-reGen` folder):
   ```yaml
   # Run CLIP + DeepDanbooru safety checks on GPU (avoids 100% multi-core CPU spikes)
   safety_on_gpu: true
   # Cache active checkpoints in host RAM to avoid repeated disk reloads
   high_memory_mode: true
   ram_to_leave_free: "40%"
   ```
2. **In your `horde-worker-reGen` startup script or `horde-regen.service`**:
   ```bash
   export PYTHONPATH="/opt/direct/ai/regen_comfy_bridge:${PYTHONPATH:-}"
   export DIRECT_COMFY_MODELS_DIR="/path/to/comfyui_models"
   export DIRECT_COMFY_CUSTOM_NODES_DIR="/path/to/ComfyUI/custom_nodes"
   export OMP_NUM_THREADS=2
   export MKL_NUM_THREADS=2

   exec ./conda/envs/linux/bin/python -s run_worker.py
   ```

---

## 3. Configuration (`config.ini`)

1. Optional: register an account and generate an API key at **[aihorde.net/register](https://aihorde.net/register)** for priority Kudos queue access (or use `0000000000` for anonymous client requests).
2. Configure `/opt/direct/ai/config.ini` (and monitor live `horde-worker-reGen` + `:8188` bridge health via the owner **`/horde`** console or **AI Horde** Node App):

```ini
[settings]
# AI Horde client key (0000000000 = anonymous client; use your key for priority queue)
HORDE_API_KEY = your_aihorde_api_key
HORDE_IMAGE_ENABLED = true
HORDE_IMAGE_FOR_GUESTS = true
HORDE_GUEST_DAILY_KUDOS_LIMIT = 2000
HORDE_PER_USER_DAILY_KUDOS_LIMIT = 350

# Optional priority key used for sponsored Studio/guest Horde image fallback
# and for querying / toggling maintenance on your local horde-worker-reGen instance:
HORDE_WORKER_API_KEY = your_aihorde_api_key

[apps]
horde = AI Horde | 4000 | /horde | gpu-card | AI Horde reGen Telemetry & ComfyUI Bridge Console
comfyui = ComfyUI | 8188 | / | diagram-3-fill | Local AI Workflow Studio
```

> **Residential ISP & CGNAT (`UnsafeIP` Notice)**: AI Horde uses automated IP reputation filters to block abusive VPN workers. If your residential fiber or CGNAT connection triggers a false-positive `UnsafeIP` message when starting `horde-worker-reGen`, join the **[Official AI Horde Discord](https://discord.gg/3DxrhksKzn)**, link your API key via `/userinfo`, and ask a moderator to enable the VPN exception flag on your account.
{: .note }
