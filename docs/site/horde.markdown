---
layout: page
title: AI Horde & Worker Bridge
permalink: /horde/
nav_order: 8.5
---
# ⚡ AI Horde Client, Dreamer Worker & Cooperative `reGen` Bridge
<hr>

**AI Assistant Direct** integrates natively with [**AI Horde**](https://aihorde.net) - a crowdsourced, distributed cluster of community GPU workers for text and image generation - both as a **Hybrid Client** and as a local **AI Horde Image Worker** managed from the owner-only **`/horde`** console.

* **As a Client (`hybrid` / `horde` modes & Guest Fallback)**: Keep your private documents, spreadsheets, and code strictly local on **Ollama** in **Work Mode**, while tapping into **70B-123B community text models** and **AI Horde SDXL image generation** in **Fun Mode** and **Creations** (including automatic CivitAI LoRA mapping, negative prompts, and 1-click SillyTavern `.png` / `.json` Character Card import).
* **As a Worker (`/horde`)**: Contribute idle GPU cycles to AI Horde to earn **Kudos** automatically using either:
  1. **Standard Setup**: the built-in idle-aware **ComfyUI SDXL Dreamer Worker** (`ai/horde_dreamer.py`) connected to a classic standalone **ComfyUI** instance (`127.0.0.1:8188`).
  2. **Advanced Single-GPU Setup**: the official **`horde-worker-reGen`** runner paired with Direct's zero-copy **Cooperative ComfyUI Bridge** (`ai/regen_comfy_bridge`), sharing one CUDA context on `127.0.0.1:8188` between AI Horde jobs and **Creations** Studio workflows.

![AI Horde Worker & ComfyUI Bridge Console](/images/horde_worker.png)

---

## 1. AI Horde Client Modes (`Settings -> AI Node`)

In **Settings -> AI Node** (or in `/opt/direct/ai/config.ini`), you can use **AI Horde** either as a dedicated standalone **AI Node** choice (available to all users, even without a personal hardware node) or as a **Runtime mode** on your own node:

* **Standalone `Horde AI` Node (`AI Node: Horde AI`)**: Select **`Horde AI`** directly in the top **AI Node** dropdown, enter your optional **Horde API Key** (`0000000000` = anonymous, or personal key for priority Kudos), pick **Auto** or a preferred **Horde Model**, and click **Save AI Node Settings**. Both **Work** and **Fun** chats plus **Creations** (`Express`) run entirely through the AI Horde community cluster.
* **Personal Node Runtime Modes (`Runtime mode`)**:

| Runtime Mode | Work Mode (RAG, Docs, Code, MCP) | Fun Mode & Creations Studio |
| :--- | :--- | :--- |
| **`Local`** | Local **Ollama** (`:11434`) | Local **Ollama** (`FUN_MODEL`) & local **ComfyUI** (`:8188`) |
| **`Hybrid (AI Horde for fun mode)`** | **Local Ollama** (100% private local RAG) | **AI Horde** (`26B-128B+` community RP models) + local **ComfyUI** / **AI Horde** image fallback |
| **`Horde (all inference via AI Horde)`** | **AI Horde** | **AI Horde** (text & SDXL image generation) |
| **`Agentic P2P`** | Remote GPU node over WebRTC P2P | Remote GPU node over WebRTC P2P |

* **Quality-Filtered Multi-Model Fallback & Live Worker Badge**: Automatic model selection and offline-model fallback strictly filter out tiny `< 8B` and debug workers (retaining capable `8B-128B+` dense and MoE models), and every AI Horde chat reply displays a compact badge (`⚡` when answered by your primary model, or `↪` when answered by an online fallback worker) showing the exact model that generated the response.

![AI Horde Hybrid Mode & SillyTavern Character Import](/images/horde_assistant.png)

### Client Image Mapping & CivitAI Token Support
* **Studio-to-Horde Mapper (`ai/horde_image_mapper.py`)**: Translates Creative Studio prompts, negative prompts (`###`), aspect ratios, steps, samplers, and open-download SDXL CivitAI LoRAs (`civitai_<versionId>`) into compliant `/api/v2/generate/async` payloads, with configurable daily Kudos budgets (`HORDE_GUEST_DAILY_KUDOS_LIMIT`, `HORDE_PER_USER_DAILY_KUDOS_LIMIT`) when no local `:8188` GPU is available.
* **Optional CivitAI API Key (`mcp_keys`)**: If a user configures a personal CivitAI API key in **Settings -> Integrations (`mcp_keys`)**, Direct automatically authenticates CivitAI catalog searches and creator-gated LoRA downloads on their behalf without storing shared credentials in `config.ini`.
* **Deterministic Request Lifecycle**: Completed generations (`done: true`) always take precedence over current worker availability (`is_possible: false`), and unfinished tasks that time out or are aborted by the caller are explicitly cancelled via `DELETE /api/v2/generate/status/{id}` (or `/generate/text/status/{id}`).

---

## 2. Standard Worker Setup: Built-in ComfyUI SDXL Dreamer (`ai/horde_dreamer.py`)

For standard installations running a classic standalone **ComfyUI** server on `http://127.0.0.1:8188`, **AI Assistant Direct** includes a built-in, idle-aware **SDXL Image Worker ("Dreamer")** that requires no separate Docker container or second CUDA process.

### Key Capabilities & Compliance Guarantees

1. **Coordinated Idle & Dispatch Reservation**:
   * Before polling `/api/v2/generate/pop` - and again under an internal reservation lock (`_comfy_reservation_lock`) immediately prior to submitting `/prompt` - the worker verifies that local ComfyUI's running and pending queues (`GET /queue`) and Autopilot locks are completely idle.
   * Each popped job runs under a strict wall-clock deadline bounded by the assignment's `ttl` lease across rendering, safety scanning, WebP conversion, R2 upload, and `/v2/generate/submit`. Hung or cancelled prompts are cleaned up from ComfyUI (`/queue`, `/interrupt`, `/history`).

2. **Reference-Verified Local SDXL Checkpoints Only (Zero Substitution)**:
   * Advertises only official local SDXL checkpoints listed in [`models.aihorde.net`](https://models.aihorde.net/) (`AI-Horde-image-model-reference`) that pass **exact byte-size and SHA256 digest verification** on disk (`CyberRealistic Pony`, `Pony Realism`, `AlbedoBase XL (SDXL)`, `SDXL 1.0`, `WAI-NSFW-illustrious-SDXL`) **and** are confirmed registered in ComfyUI `/object_info/CheckpointLoaderSimple` prior to polling.
   * Unexpected or unverified model names are immediately faulted without cross-model fallback. Cloud/proprietary models are never advertised on AI Horde.

3. **Faithful SDXL Execution Semantics**:
   * Executes a clean ComfyUI SDXL text-to-image graph (`CheckpointLoaderSimple -> CLIPSetLastLayer -> CLIPTextEncode -> EmptyLatentImage -> KSampler -> VAEDecode -> SaveImageWebsocket`) without unrequested LoRAs, sharpening, negative-prompt injections, dimension clamping, or seed modulo reduction.
   * Splits prompts on `###`, applies canonical seeded `{choice1|choice2}` dynamic-prompt expansion (`random.Random(seed)`, matching `hordelib`) to the positive prompt, and binds requested `width`, `height`, `steps`, `cfg_scale`, `denoising_strength` (`denoise`), `clip_skip`, `sampler_name`, `karras`/`scheduler`, and full `uint64` `seed` (including `0`).
   * Validates authoritative ComfyUI completion (`executing: node=null` with zero `execution_error` events and exact uncensored pixel dimensions matching `width x height`) before safety evaluation.

4. **Mandatory `horde-safety` Scanning & Zero Default Retention**:
   * Refuses to pop jobs if [`horde-safety`](https://github.com/Haidra-Org/horde-safety) (`NSFWChecker` on CPU) is unavailable; replaces CSAM or SFW-censored outputs with official AI Horde censor placeholders and reports `state: "csam"` / `"censored"` plus `gen_metadata`.
   * **In-Memory Output (`SaveImageWebsocket`)**: Disables live latent previews (`preview_method: "none"`), streams final PNG bytes directly over the local ComfyUI WebSocket into process RAM rather than writing files to `temp/` or `output/`, and deletes the prompt entry from ComfyUI `/history`.
   * **Metadata & Prompt Minimization**: Outgoing WebP encoding strips all PNG text chunks (`prompt`/`workflow`) and EXIF metadata. Raw user prompts are never written to logs or `horde_dreamer_data.json`.
   * **Bounded In-Memory Reconciliation**: While a rendered, safety-cleared WebP awaits `/v2/generate/submit` acknowledgement (or retries across transient HTTP `429`/`5xx` drops), its stripped WebP bytes exist solely in RAM (`_pending_deliveries`) until acknowledgement, terminal rejection, or `ttl` expiry.

5. **Owner-Only Management Boundary & Fail-Closed Policy**:
   * `/horde` and `/horde/api/*` enforce a strict owner trust boundary: non-owner/guest callers receive `404`, and remote access requires a cryptographically paired owner DID (such as opening the **AI Horde** Node App over Disk Direct P2P).
   * If AI Horde places the worker in `WorkerMaintenance`, polling pauses automatically (`enabled = false`) and never auto-clears the maintenance flag without explicit owner action.

### Supported Built-in SDXL Worker Scope (`Direct:1.0.0:https://github.com/alexeyvolkoff/direct`)

| Capability | Supported Contract |
| :--- | :--- |
| **`bridge_agent`** | `Direct:1.0.0:https://github.com/alexeyvolkoff/direct` |
| **Offered Models** | Subset of `{CyberRealistic Pony, Pony Realism, AlbedoBase XL (SDXL), SDXL 1.0, WAI-NSFW-illustrious-SDXL}` verified by SHA256 + byte size on disk and registered in `/object_info/CheckpointLoaderSimple`. |
| **Samplers** | `k_euler`, `k_euler_a`, `k_heun`, `k_dpm_2`, `k_dpm_2_a`, `k_lms`, `ddim`, plus ComfyUI KSampler mappings for `k_dpm_fast`, `k_dpm_adaptive`, `k_dpmpp_2s_a`, `k_dpmpp_sde`, `k_dpmpp_2m`, `dpmsolver`, `lcm`, `uni_pc`, `uni_pc_bh2`, `dpmpp_2m_sde`, `dpmpp_3m_sde`, `ddpm`, `deis` (`plms` and unknown samplers are rejected). |
| **Schedulers** | `normal`, `karras`, `simple`, `sgm_uniform`, `exponential`, `ddim_uniform`, `beta`, `linear_quadratic`, `kl_optimal` (`dpmpp_3m_sde` + `normal` normalizes to `karras` per `hordelib`). |
| **Dimensions (`width`, `height`)** | `64..8192` in multiples of `64`, bounded by worker `max_pixels` (default `1048576`), validated on admission and output without clamping. |
| **Seeds (`seed`)** | `0 .. 18446744073709551615` (`0 .. 2^64 - 1`), preserved verbatim through `KSampler` and `/v2/generate/submit`. |
| **Steps / CFG / Denoise / CLIP Skip** | `steps`: `1..500`, `cfg_scale`: `1.0..100.0`, `denoising_strength`: `0.01..1.0` (default `1.0`), `clip_skip`: `1..20`. |
| **Prompt Syntax** | `positive ### negative` splitting; seeded `{a|b}` (including nested) dynamic-prompt expansion on positive prompt. |
| **Single-Image `txt2img` Scope** | `amount: 1` (`n: 1`) `txt2img` only in the built-in Dreamer worker. For full multi-feature AI Horde worker capabilities (`LoRA`, `ControlNet`, `img2img`, `Flux.1-Schnell`), see the advanced `horde-worker-reGen` setup below. |

---

## 3. Advanced Single-GPU Setup: `horde-worker-reGen` + Cooperative ComfyUI Bridge

Power users who want to run the full official [**`horde-worker-reGen`**](https://github.com/Haidra-Org/horde-worker-reGen) worker (`SDXL`, `Pony`, `Flux.1-Schnell`, LoRAs, ControlNet, img2img, upscalers) on a single 12-24 GB GPU normally face a dilemma: `hordelib` already embeds its own internal ComfyUI runtime inside `HordeInferenceProcess`, leaving insufficient VRAM for a second standalone ComfyUI server.

Direct solves this cleanly via **`ai/regen_comfy_bridge`** (`sitecustomize.py` + `bridge.py`), without modifying a single file inside `horde-worker-reGen` or `hordelib`:

1. **Single CUDA Context & Cooperative Turn-Taking**:
   * Injecting `PYTHONPATH=/opt/direct/ai/regen_comfy_bridge` into `horde-worker-reGen`'s startup environment hooks `HordeInferenceProcess` at import time and starts `hordelib`'s native `PromptServer` + `PromptQueue` on `127.0.0.1:8188`.
   * Inside `worker_cycle()`, any queued Direct Studio / RP Chat workflow on `:8188` executes **between** AI Horde jobs on the exact same CUDA main thread: `hordelib` models are gracefully evicted from VRAM (`comfy.model_management.unload_all_models()`), Direct's `flow.json` graph runs at full GPU speed (~3.5s for 8-step SDXL + DMD2, or ~7s with IP-Adapter Plus Face + ReActor HyperSwap + GFPGAN + Impact Auto-Inpaint), and `horde-worker-reGen` resumes Horde jobs seamlessly afterward.
2. **Full `/comfyui` & `/horde` P2P App Integration**:
   * If a standalone ComfyUI `venv` (`comfyui-frontend-package`) and `custom_nodes` directory exist on the host, `bridge.py` automatically mounts custom nodes (`IPAdapter`, `ReActor`, `Impact-Pack`, `Essentials`, `rgthree`) and serves the modern ComfyUI web editor on `127.0.0.1:8188` (including HTTP WebSocket polling `/api/omd_ws_poll` for P2P iframe access via the **ComfyUI** Node App).
   * The **`/horde`** console automatically detects `horde-regen.service` (`bridgeData.yaml`), displaying live AI Horde telemetry, earned Kudos, Megapixelsteps, `:8188` GPU VRAM availability, advertised Horde models, Studio checkpoints, and a 1-click **Maintenance Mode** toggle (`PUT /api/v2/workers/{id}`).

### Launching `horde-worker-reGen` with `DirectComfyBridge`

```bash
# Inside your horde-worker-reGen startup script or systemd unit:
export PYTHONPATH="/opt/direct/ai/regen_comfy_bridge:${PYTHONPATH:-}"
export DIRECT_COMFY_MODELS_DIR="/path/to/comfyui_models"
export DIRECT_COMFY_CUSTOM_NODES_DIR="/path/to/ComfyUI/custom_nodes"

# Keep built-in Dreamer disabled when running dedicated horde-worker-reGen:
# HORDE_WORKER_ENABLED = false in /opt/direct/ai/config.ini
exec ./conda/envs/linux/bin/python -s run_worker.py
```

---

## 4. Configuration (`config.ini`)

1. Register an account and generate an API key at **[aihorde.net/register](https://aihorde.net/register)**.
2. Configure `/opt/direct/ai/config.ini` (and monitor live status via the owner **`/horde`** console or **AI Horde** Node App):

```ini
[settings]
# Client & fallback API key (0000000000 = anonymous client; use your key for priority queue)
HORDE_API_KEY = your_aihorde_api_key
HORDE_IMAGE_ENABLED = true
HORDE_IMAGE_FOR_GUESTS = true
HORDE_GUEST_DAILY_KUDOS_LIMIT = 2000
HORDE_PER_USER_DAILY_KUDOS_LIMIT = 350

# Built-in ComfyUI SDXL Dreamer Worker (for classic standalone ComfyUI on :8188;
# leave false if you use the advanced horde-worker-reGen + regen_comfy_bridge setup)
HORDE_WORKER_ENABLED = false
HORDE_WORKER_API_KEY = your_aihorde_api_key
HORDE_WORKER_NAME = Direct-Dreamer
HORDE_WORKER_NSFW = false
HORDE_WORKER_MAX_PIXELS = 1048576
HORDE_WORKER_MODELS = CyberRealistic Pony

[apps]
horde = AI Horde | 4000 | /horde | gpu-card | AI Horde Worker & ComfyUI Bridge Console
comfyui = ComfyUI | 8188 | / | diagram-3-fill | Local AI Workflow Studio
```

> **Residential ISP & CGNAT (`UnsafeIP` Notice)**: AI Horde uses automated IP reputation filters to block abusive VPN workers. If your residential fiber or CGNAT connection triggers a false-positive `UnsafeIP` message, join the **[Official AI Horde Discord](https://discord.gg/3DxrhksKzn)**, link your API key via `/userinfo`, and ask a moderator to enable the VPN exception flag on your account.
{: .note }
