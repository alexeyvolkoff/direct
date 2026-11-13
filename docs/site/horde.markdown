---
layout: page
title: AI Horde & Dreamer Worker
permalink: /horde/
nav_order: 8.5
---
# ⚡ AI Horde Client & Built-in ComfyUI Dreamer Worker
<hr>

**AI Assistant Direct** integrates natively with [**AI Horde**](https://aihorde.net) — a crowdsourced, distributed cluster of community GPU workers for text and image generation — both as a **Hybrid Client** and as a built-in, idle-aware **ComfyUI SDXL Image Worker ("Dreamer")**.

* **As a Client (`hybrid` / `horde` modes)**: Keep your private documents, spreadsheets, and code strictly local on **Ollama** in **Work Mode**, while seamlessly tapping into **70B–123B community text models** and **AI Horde image generation** in **Fun Mode** & **Creative Studio** — complete with **1-click SillyTavern Character Card (`.png` / `.json`) import**.
* **As a Worker (`/horde`)**: Whenever your local **ComfyUI** queue is idle, your node can pick up single-image SDXL text-to-image jobs from AI Horde for reference-verified local checkpoints, execute them faithfully in memory, and earn **Kudos** for your account.

![AI Horde Dreamer Worker Console](/images/horde_worker.png)

---

## 1. Hybrid Mode: Local Work RAG + AI Horde Roleplay & Studio

In **Settings → AI Assistant → AI Node** (or in `/opt/direct/ai/config.ini`), you can select how **AI Assistant** routes your requests:

| Runtime Mode | Work Mode (RAG, Docs, Code, MCP) | Fun Mode & Creative Studio |
| :--- | :--- | :--- |
| **`Local`** | Local **Ollama** (`:11434`) | Local **Ollama** (`FUN_MODEL`) & local **ComfyUI** |
| **`Hybrid (AI Horde for fun mode)`** | **Local Ollama** (100% private local RAG) | **AI Horde** (`70B–123B` community RP models) |
| **`AI Horde only`** | **AI Horde** | **AI Horde** (text & `core_service.generate_image` image requests) |
| **`Agentic P2P`** | Remote GPU node over WebRTC P2P | Remote GPU node over WebRTC P2P |

![AI Horde Hybrid Mode & SillyTavern Character Import](/images/horde_assistant.png)

### Client Request Lifecycle
* Completed generations (`done: true`) always take precedence over current worker availability (`is_possible: false`), ensuring already-settled results are delivered without redundant retries.
* Unfinished image or text tasks that time out, become impossible before completion, or are cancelled by the caller are explicitly cancelled via `DELETE /api/v2/generate/status/{id}` (or `/generate/text/status/{id}`); successfully consumed results are never cancelled afterward.

---

## 2. Built-in ComfyUI SDXL Dreamer Worker (`/horde`)

Instead of running a separate inference container with a duplicate CUDA context on single-GPU desktop hardware, **AI Assistant Direct** includes a tightly scoped **AI Horde SDXL Image Worker ("Dreamer")** (`ai/horde_dreamer.py`) coordinated with your local **ComfyUI** instance (`http://localhost:8188`).

### Key Capabilities & Guarantees

1. **Coordinated Idle & Dispatch Reservation**:
   * Before polling `/api/v2/generate/pop` — and again under an internal reservation lock (`_comfy_reservation_lock`) immediately prior to submitting `/prompt` — the worker verifies that local ComfyUI's running and pending queues (`GET /queue`) and Autopilot locks are idle.
   * Each popped job runs under a strict wall-clock deadline bounded by the assignment's `ttl` lease across rendering, safety scanning, WebP conversion, R2 upload, and `/v2/generate/submit`. Hung or cancelled prompts are removed from ComfyUI (`/queue`, `/interrupt`, `/history`).

2. **Reference-Verified SDXL Checkpoints Only (Zero Substitution)**:
   * Offers only official SDXL checkpoints present in [`models.aihorde.net`](https://models.aihorde.net/) (`AI-Horde-image-model-reference`) that pass **exact byte-size and SHA256 digest verification** on disk (`CyberRealistic Pony`, `Pony Realism`, `AlbedoBase XL (SDXL)`, `SDXL 1.0`, `WAI-NSFW-illustrious-SDXL`) **and** are confirmed registered in the connected ComfyUI `/object_info/CheckpointLoaderSimple` endpoint prior to polling.
   * Unexpected or unverified model names are immediately faulted without cross-model fallback.
   * **Explicit Krea Exclusion**: The experimental `Krea2-Turbo_fp8` / remote Modal path is **disabled and excluded** (`krea_enabled = false`) until registered in the live AI Horde model reference with cryptographic remote-asset verification and enforced remote cleanup.

3. **Faithful SDXL Execution Semantics**:
   * Executes a clean ComfyUI SDXL text-to-image graph (`CheckpointLoaderSimple -> CLIPSetLastLayer -> CLIPTextEncode -> EmptyLatentImage -> KSampler -> VAEDecode -> SaveImageWebsocket`) without unrequested LoRAs, sharpening, negative-prompt injections, dimension clamping, or seed modulo reduction.
   * Splits prompts on `###`, applies canonical seeded `{choice1|choice2}` dynamic-prompt expansion (`random.Random(seed)`, matching `hordelib`) to the positive prompt, and binds requested `width`, `height`, `steps`, `cfg_scale`, `denoising_strength` (`denoise`), `clip_skip`, `sampler_name`, `karras`/`scheduler`, and full `uint64` `seed` (including `0`).
   * Validates authoritative ComfyUI completion (`executing: node=null` with zero `execution_error` events and exact uncensored pixel dimensions matching `width × height`) before safety evaluation.

4. **Mandatory `horde-safety` Scanning & Zero Default Retention (Transient-Data Lifecycle)**:
   * Refuses to pop jobs if [`horde-safety`](https://github.com/Haidra-Org/horde-safety) (`NSFWChecker` on CPU) is unavailable; replaces CSAM or SFW-censored outputs with official AI Horde censor placeholders and reports `state: "csam"` / `"censored"` plus `gen_metadata`.
   * **In-Memory Output (`SaveImageWebsocket`)**: Disables live latent previews (`preview_method: "none"`), streams final PNG bytes directly over the local ComfyUI WebSocket into process memory rather than writing ComfyUI sidebar/gallery files to `temp/` or `output/`, immediately deletes the prompt entry from ComfyUI `/history`, and deletes only tracked worker-owned fallback files if any occur (never touching interactive users' files).
   * **Metadata & Prompt Minimization**: Outgoing WebP encoding strips all PNG text chunks (`prompt`/`workflow`) and EXIF metadata. Raw user prompts are never written to application logs or `horde_dreamer_data.json` (`recent_jobs` records only job ID, model, resolution, steps, duration, reward, and status/error category).
   * **Bounded In-Memory Reconciliation**: While a rendered, safety-cleared WebP is awaiting `/v2/generate/submit` acknowledgement — or retained across a transient HTTP `429`/`5xx`/network drop for next-cycle duplicate/completion reconciliation — its metadata-stripped WebP bytes exist **solely in process RAM** (`_pending_deliveries`) until server acknowledgement, terminal rejection, or assignment `ttl` expiry, after which the buffer is cleared.

5. **Owner-Only Management Boundary & Fail-Closed Policy**:
   * `/horde` and `/horde/api/*` enforce a strict owner trust boundary: direct non-loopback peers and explicit `guest`/`shared`/non-owner caller roles are rejected (`404`), a `False` verdict from the host owner guard (`extensions.autopilot._check_owner_request`) never falls through to loopback defaults, and any proxied or role-header request requires a server-verified owner credential (`HORDE_OWNER_SECRET`, paired DID via `api.is_did_paired`, or `request.state.trusted_owner`).
   * Concurrent operator configuration edits are preserved via atomic partial state updates when AI Horde places a worker in `WorkerMaintenance` (which automatically sets `enabled = false`), explicit empty model selections (`sdxl_models = []`) are preserved, and corrupted or unreadable `horde_dreamer_data.json` files fail closed (`enabled = false`, `nsfw = false`, `sdxl_models = []`).

---

## 3. Supported SDXL Worker Scope (`Direct:1.0.0:https://github.com/alexeyvolkoff/direct`)

| Capability | Supported Contract |
| :--- | :--- |
| **`bridge_agent`** | `Direct:1.0.0:https://github.com/alexeyvolkoff/direct` |
| **Offered Models** | Subset of `{CyberRealistic Pony, Pony Realism, AlbedoBase XL (SDXL), SDXL 1.0, WAI-NSFW-illustrious-SDXL}` selected by operator, verified by SHA256 + byte size on disk, and registered in connected ComfyUI `/object_info/CheckpointLoaderSimple`. (`Krea2-Turbo_fp8` excluded.) |
| **Samplers** | Baseline AI Horde bridge dispatch set (`k_euler`, `k_euler_a`, `k_heun`, `k_dpm_2`, `k_dpm_2_a`, `k_lms`, `ddim`), plus ComfyUI KSampler mappings for `k_dpm_fast`, `k_dpm_adaptive`, `k_dpmpp_2s_a`, `k_dpmpp_sde`, `k_dpmpp_2m`, `dpmsolver`, `lcm`, `uni_pc`, `uni_pc_bh2`, `dpmpp_2m_sde`, `dpmpp_3m_sde`, `ddpm`, and `deis` once declared in AI Horde's `bridge_reference.py` (`plms` and unknown samplers are rejected). |
| **Schedulers** | `normal`, `karras`, `simple`, `sgm_uniform`, `exponential`, `ddim_uniform`, `beta`, `linear_quadratic`, `kl_optimal` (`dpmpp_3m_sde` + `normal` normalizes to `karras` per `hordelib`). |
| **Dimensions (`width`, `height`)** | `64..8192` in multiples of `64`, bounded by worker `max_pixels` (default `1048576`), validated on both request admission and rendered image output. Never clamped or resized. |
| **Seeds (`seed`)** | `0 .. 18446744073709551615` (`0 .. 2^64 - 1`), preserved verbatim through `KSampler` and `/v2/generate/submit`. Missing or malformed worker seeds are rejected. |
| **Steps / CFG / Denoise / CLIP Skip** | `steps`: `1..500`, `cfg_scale`: `1.0..100.0`, `denoising_strength`: `0.01..1.0` (default `1.0`), `clip_skip`: `1..20`. |
| **Prompt Syntax** | `positive ### negative` splitting; seeded `{a|b}` (including nested) dynamic-prompt expansion on positive prompt. |
| **Excluded Modes** | Single-image (`amount: 1`, `n: 1`) `txt2img` only. `img2img`, `painting`, `post_processing`, `lora`, `tis`, `controlnet`, `tiling`, `hires_fix`, `transparent`, custom `workflow`, and `Krea2-Turbo_fp8` are excluded and rejected. |

---

## 4. Configuration (`config.ini`)

1. Register an account and generate an API key at **[aihorde.net/register](https://aihorde.net/register)**.
2. Configure the AI Horde settings in `/opt/direct/ai/config.ini` (or via the local owner **`/horde`** console):

```ini
[settings]
# Client API key (0000000000 = anonymous client; use your key for priority queue)
HORDE_API_KEY = your_aihorde_api_key

# Built-in ComfyUI SDXL Dreamer Worker (opt-in; keep false until ready to contribute)
HORDE_WORKER_ENABLED = false
HORDE_WORKER_API_KEY = your_aihorde_api_key
HORDE_WORKER_NAME = Direct-Dreamer
HORDE_WORKER_NSFW = false
HORDE_WORKER_MAX_PIXELS = 1048576
HORDE_WORKER_MODELS = CyberRealistic Pony

[apps]
horde = AI Horde | 4000 | /horde | gpu-card | AI Horde Dreamer Worker Console
```

> **Residential ISP & CGNAT (`UnsafeIP` Notice)**: AI Horde uses automated IP reputation filters to block abusive VPN workers. If your residential fiber or CGNAT connection triggers a false-positive `UnsafeIP` message in the `/horde` console, join the **[Official AI Horde Discord](https://discord.gg/3DxrhksKzn)**, link your API key via `/userinfo`, and ask a moderator to enable the VPN exception flag on your account.
{: .note }
