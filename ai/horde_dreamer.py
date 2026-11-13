"""
AI Horde Dreamer Worker & Image Client for Direct (`horde_dreamer.py`).

Features:
1. Built-in AI Horde Image Worker ("Dreamer"):
   - Unlimited 24/7 Local SDXL generation via local ComfyUI (`hordelib`-compatible
     `stable_diffusion` pipeline honoring requested `steps`, `cfg_scale`, `sampler_name`,
     `karras`/`scheduler`, and `clip_skip`) when local ComfyUI queue is idle.
   - Strict `AI-Horde-image-model-reference` SHA256 & byte-size verification for all
     advertised checkpoints (with zero cross-model fallback).
   - Optional daily-capped Modal Krea 2 (`Krea2-Turbo_fp8`) generation when reference
     weights (`qwen_image_fp8_e4m3fn.safetensors` / `qwen3vl_4b_fp8_scaled.safetensors` /
     `qwen_image_vae.safetensors`) are verified.
2. AI Horde Image Generation Client (`horde_generate_image`) & `ai_mode` normalization hook.
3. Owner-only management console at `/horde` and JSON endpoints under `/horde/api/*`
   and `/assistant/horde_worker`.
"""

import asyncio
import base64
import datetime
import hashlib
import io
import json
import logging
import os
import random
import re
import threading
import time
import uuid
from typing import Any, Optional

import aiohttp
from PIL import Image
from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse

import horde_client
import user_context
from config import SETTINGS

try:
    from horde_safety.deep_danbooru_model import DeepDanbooruModel, get_deep_danbooru_model
    from horde_safety.interrogate import Interrogator, get_interrogator_no_blip
    from horde_safety.nsfw_checker_class import NSFWChecker, NSFWResult
    HORDE_SAFETY_AVAILABLE = True
except Exception as _safety_import_err:
    HORDE_SAFETY_AVAILABLE = False
    NSFWChecker = Any  # type: ignore
    NSFWResult = Any  # type: ignore

logger = logging.getLogger("horde_dreamer")

ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_PATH = os.path.join(ROOT_DIR, "horde_dreamer_data.json")
HORDE_ASSETS_DIR = os.path.join(ROOT_DIR, "assets", "horde")


def is_modal_enabled() -> bool:
    return bool(str(SETTINGS.get("MODAL_COMFY_URL", "")).strip())

HORDE_API_BASE = "https://aihorde.net/api/v2"
BRIDGE_AGENT = "Direct:1.0.0:https://github.com/alexeyvolkoff/direct"
CLIENT_AGENT = "Direct:1.0.0:https://github.com/alexeyvolkoff/direct"
PROMPT_SEPARATOR = "###"

# Strict 1-to-1 mapping from official AI Horde model names (`AI-Horde-image-model-reference`)
# to their exact reference checkpoint filenames, SHA256 digests, and byte sizes.
# A model is only advertised or rendered if its local `.safetensors` file matches the reference SHA256.
LOCAL_SDXL_MODEL_CATALOG: dict[str, dict[str, Any]] = {
    "CyberRealistic Pony": {
        "ckpt": "cyberrealisticPony_v8.safetensors",
        "sha256": "9f90c59f3aa99d9160b5652c4c68215fdc27399e5d1c692429abf2407bb909d1",
        "size_bytes": 6938040682,
        "description": "Official CyberRealistic Pony v8.0 SDXL reference checkpoint",
    },
    "Pony Realism": {
        "ckpt": "ponyRealism_v21MainVAE.safetensors",
        "sha256": "059934ff58033c5abb606b6b98f969ffdf7e7312d352c3be47a0440ba28d303b",
        "size_bytes": 7105348824,
        "description": "Official Pony Realism v2.1 SDXL reference checkpoint",
    },
    "AlbedoBase XL (SDXL)": {
        "ckpt": "albedo_base_xl.safetensors",
        "sha256": "1718b5bb2da1ef4815fee8af8a7fc2fa8ab8f467b279eded4d991ea0cce59a6d",
        "size_bytes": 6776583201,
        "description": "Official AlbedoBase XL v2.1 SDXL reference checkpoint",
    },
    "SDXL 1.0": {
        "ckpt": "sd_xl_base_1.0.safetensors",
        "sha256": "31e35c80fc4829d14f90153f4c74cd59c90b779f6afe05a74cd6120b893f7e5b",
        "size_bytes": 6938078334,
        "description": "Official SDXL 1.0 Base reference checkpoint",
    },
    "WAI-NSFW-illustrious-SDXL": {
        "ckpt": "waiNSFWIllustrious_v140.safetensors",
        "sha256": "bdb59bac77d94ae7a55ff893170f9554c3f349e48a1b73c0c17c0b7c6f4d41a2",
        "size_bytes": 6938040416,
        "description": "Official WAI-NSFW-illustrious-SDXL v14.0 reference checkpoint",
    },
}

KREA_REFERENCE_FILES: dict[str, str] = {
    "unet": "qwen_image_fp8_e4m3fn.safetensors",
    "clip": "qwen3vl_4b_fp8_scaled.safetensors",
    "vae": "qwen_image_vae.safetensors",
}

_checkpoint_sha256_cache: dict[tuple[str, int, int], str] = {}

# Canonical AI Horde -> ComfyUI sampler mapping (`Haidra-Org/hordelib/hordelib/pipeline/constants.py`).
# Samplers not supported by ComfyUI (such as `plms`) are intentionally excluded rather than substituted.
HORDE_TO_COMFY_SAMPLERS: dict[str, str] = {
    "k_euler": "euler",
    "k_euler_a": "euler_ancestral",
    "k_heun": "heun",
    "k_dpm_2": "dpm_2",
    "k_dpm_2_a": "dpm_2_ancestral",
    "k_lms": "lms",
    "k_dpm_fast": "dpm_fast",
    "k_dpm_adaptive": "dpm_adaptive",
    "k_dpmpp_2s_a": "dpmpp_2s_ancestral",
    "k_dpmpp_sde": "dpmpp_sde",
    "k_dpmpp_2m": "dpmpp_2m",
    "ddim": "ddim",
    "dpmsolver": "dpmpp_2m",
    "lcm": "lcm",
    "uni_pc": "uni_pc",
    "uni_pc_bh2": "uni_pc_bh2",
    "dpmpp_2m_sde": "dpmpp_2m_sde",
    "dpmpp_3m_sde": "dpmpp_3m_sde",
    "ddpm": "ddpm",
    "deis": "deis",
    "ipndm": "ipndm",
    "res_multistep": "res_multistep",
    "gradient_estimation": "gradient_estimation",
    "heunpp2": "heunpp2",
    "er_sde": "er_sde",
    "sa_solver": "sa_solver",
    "ipndm_v": "ipndm_v",
    "dpmpp_2m_sde_heun": "dpmpp_2m_sde_heun",
    "sa_solver_pece": "sa_solver_pece",
    "seeds_2": "seeds_2",
    "seeds_3": "seeds_3",
    "res_multistep_ancestral": "res_multistep_ancestral",
    "euler_cfg_pp": "euler_cfg_pp",
    "euler_ancestral_cfg_pp": "euler_ancestral_cfg_pp",
    "dpmpp_2s_ancestral_cfg_pp": "dpmpp_2s_ancestral_cfg_pp",
    "dpmpp_2m_cfg_pp": "dpmpp_2m_cfg_pp",
    "res_multistep_cfg_pp": "res_multistep_cfg_pp",
    "res_multistep_ancestral_cfg_pp": "res_multistep_ancestral_cfg_pp",
    "gradient_estimation_cfg_pp": "gradient_estimation_cfg_pp",
}

SUPPORTED_HORDE_SAMPLERS: tuple[str, ...] = tuple(HORDE_TO_COMFY_SAMPLERS.keys())
# Baseline samplers dispatched by AI Horde (`horde/bridge_reference.py`) to non-reGen bridges:
BRIDGE_DECLARED_HORDE_SAMPLERS: tuple[str, ...] = (
    "k_lms",
    "k_heun",
    "k_euler",
    "k_euler_a",
    "k_dpm_2",
    "k_dpm_2_a",
    "k_dpm_fast",
    "k_dpm_adaptive",
    "k_dpmpp_2s_a",
    "k_dpmpp_2m",
    "k_dpmpp_sde",
    "dpmsolver",
    "ddim",
    "lcm",
)
MAX_HORDE_SEED: int = 0xFFFFFFFFFFFFFFFF  # 2**64 - 1 (ComfyUI KSampler full uint64 range)
MIN_HORDE_DIMENSION: int = 64
MAX_HORDE_DIMENSION: int = 8192
HORDE_DIMENSION_MULTIPLE: int = 64
DEFAULT_JOB_TTL_SECONDS: float = 120.0
MIN_JOB_TTL_SECONDS: float = 5.0
MAX_JOB_TTL_SECONDS: float = 600.0

COMFY_SUPPORTED_SCHEDULERS: frozenset[str] = frozenset(
    {
        "normal",
        "karras",
        "simple",
        "sgm_uniform",
        "exponential",
        "ddim_uniform",
        "beta",
        "linear_quadratic",
        "kl_optimal",
    }
)

KREA_HORDE_MODEL_NAME = "Krea2-Turbo_fp8"
_DYNAMIC_PROMPT_RE = re.compile(r"\{([^{}]+)\}")

_data_lock = threading.RLock()
_worker_lock = asyncio.Lock()
_comfy_reservation_lock = asyncio.Lock()
_worker_task: Optional[asyncio.Task] = None
_wake_event = asyncio.Event()

_safety_init_lock = threading.Lock()
_nsfw_checker: Optional[NSFWChecker] = None
_censor_png_cache: dict[str, bytes] = {}
_pending_deliveries: dict[str, dict[str, Any]] = {}
_last_known_good_data: Optional[dict[str, Any]] = None
_last_known_good_path: Optional[str] = None

# Live in-memory telemetry
_runtime_status: dict[str, Any] = {
    "state": "stopped",  # stopped | idle_polling | waiting_local_busy | rendering | evaluating_safety | awaiting_delivery | error | paused
    "current_job_id": None,
    "current_job_stage": None,
    "current_model": None,
    "current_backend": None,
    "last_pop_ts": 0.0,
    "last_submit_ts": 0.0,
    "last_error": "",
    "last_skipped": {},
    "active_models_offered": [],
    "user_info": None,
    "user_info_ts": 0.0,
    "safety_available": HORDE_SAFETY_AVAILABLE,
    "safety_loaded": False,
}


def _get_censor_png_bytes(reason: str) -> bytes:
    """
    Returns official AI Horde replacement censor image PNG bytes (`nsfw_censor_*.png` from `horde-worker-reGen`).
    Falls back to a solid black 512x512 PNG if the asset file is missing.
    """
    if reason in _censor_png_cache:
        return _censor_png_cache[reason]
    filename_map = {
        "csam": "nsfw_censor_csam.png",
        "censorlist": "nsfw_censor_censorlist.png",
        "sfw_request": "nsfw_censor_sfw_request.png",
        "sfw_worker": "nsfw_censor_sfw_worker.png",
    }
    fname = filename_map.get(reason, "nsfw_censor_sfw_request.png")
    fpath = os.path.join(HORDE_ASSETS_DIR, fname)
    try:
        if os.path.isfile(fpath):
            with open(fpath, "rb") as f:
                raw = f.read()
            _censor_png_cache[reason] = raw
            return raw
    except Exception as e:
        logger.warning(f"[HordeDreamer] Failed loading censor asset {fpath}: {e}")
    fallback_buf = io.BytesIO()
    Image.new("RGB", (512, 512), color=(0, 0, 0)).save(fallback_buf, format="PNG")
    raw = fallback_buf.getvalue()
    _censor_png_cache[reason] = raw
    return raw


def _ensure_safety_checker() -> NSFWChecker:
    """
    Initializes `horde_safety` (`NSFWChecker` with `get_interrogator_no_blip` and `get_deep_danbooru_model`)
    exactly as required by `Haidra-Org/horde-worker-reGen`.
    Raises RuntimeError if `horde_safety` cannot be initialized (fail-closed).
    """
    global _nsfw_checker
    if _nsfw_checker is not None:
        return _nsfw_checker
    with _safety_init_lock:
        if _nsfw_checker is not None:
            return _nsfw_checker
        if not HORDE_SAFETY_AVAILABLE:
            raise RuntimeError("horde_safety package is not installed; refusing to run AI Horde worker without safety checks")
        safety_device = str(SETTINGS.get("HORDE_SAFETY_DEVICE", "cpu")).strip() or "cpu"
        logger.info(f"[HordeDreamer] Initializing horde_safety (CLIP + DeepDanbooru) on device='{safety_device}'...")
        deep_danbooru_model = get_deep_danbooru_model(device=safety_device)
        interrogator = get_interrogator_no_blip(device=safety_device)
        _nsfw_checker = NSFWChecker(
            interrogator,
            deep_danbooru_model,
        )
        _runtime_status["safety_loaded"] = True
        logger.info("[HordeDreamer] horde_safety NSFWChecker initialized and ready.")
        return _nsfw_checker


def _evaluate_image_safety(
    png_bytes: bytes,
    raw_prompt: str,
    censor_nsfw: bool,
    sfw_worker: bool,
    model_info: Optional[dict[str, Any]] = None,
) -> dict[str, Any]:
    """
    Runs `horde_safety` (`NSFWChecker.check_for_nsfw`) on a generated image, matching
    `Haidra-Org/horde-worker-reGen/horde_worker_regen/process_management/safety_process.py`:
    - If `is_csam` is True: discards original image, replaces with `nsfw_censor_csam.png`,
      sets `state='csam'`, `censored=True`, and `gen_metadata=[{"type": "censorship", "value": "csam"}]`.
    - If `sfw_worker` or `censor_nsfw` and `is_nsfw` is True: replaces with official SFW censor image,
      sets `state='censored'`, `censored=True`, and `gen_metadata=[{"type": "censorship", "value": "nsfw"}]`.
    - If `is_nsfw` is True on an NSFW request/worker: keeps image and attaches
      `gen_metadata=[{"type": "information", "value": "nsfw"}]`.
    """
    checker = _ensure_safety_checker()
    with Image.open(io.BytesIO(png_bytes)) as im:
        image_as_pil = im.convert("RGB")
    nsfw_result: Optional[NSFWResult] = checker.check_for_nsfw(
        image=image_as_pil,
        prompt=raw_prompt,
        model_info=model_info or {},
    )
    if nsfw_result is None:
        raise RuntimeError("horde_safety check_for_nsfw returned None")

    final_png_bytes = png_bytes
    submit_state = "ok"
    is_censored = False
    gen_metadata: list[dict[str, str]] = []

    if nsfw_result.is_csam:
        final_png_bytes = _get_censor_png_bytes("csam")
        submit_state = "csam"
        is_censored = True
        gen_metadata.append({"type": "censorship", "value": "csam"})
    elif sfw_worker and nsfw_result.is_nsfw:
        final_png_bytes = _get_censor_png_bytes("sfw_worker")
        submit_state = "censored"
        is_censored = True
        gen_metadata.append({"type": "censorship", "value": "nsfw"})
    elif censor_nsfw and nsfw_result.is_nsfw:
        final_png_bytes = _get_censor_png_bytes("sfw_request")
        submit_state = "censored"
        is_censored = True
        gen_metadata.append({"type": "censorship", "value": "nsfw"})
    elif nsfw_result.is_nsfw:
        gen_metadata.append({"type": "information", "value": "nsfw"})

    return {
        "final_png_bytes": final_png_bytes,
        "submit_state": submit_state,
        "censored": is_censored,
        "is_csam": bool(nsfw_result.is_csam),
        "is_nsfw": bool(nsfw_result.is_nsfw),
        "gen_metadata": gen_metadata,
    }


def _utc_today_str() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")


def _default_data() -> dict[str, Any]:
    raw_enabled = str(SETTINGS.get("HORDE_WORKER_ENABLED", "false")).strip().lower() in ("1", "true", "yes", "on")
    raw_nsfw = str(SETTINGS.get("HORDE_WORKER_NSFW", "true")).strip().lower() in ("1", "true", "yes", "on")
    raw_max_pixels = 1024 * 1024
    try:
        raw_max_pixels = max(262144, int(SETTINGS.get("HORDE_WORKER_MAX_PIXELS", 1048576)))
    except Exception:
        raw_max_pixels = 1024 * 1024
    raw_limit = 10
    try:
        raw_limit = int(SETTINGS.get("HORDE_KREA_DAILY_LIMIT", 10))
    except Exception:
        raw_limit = 10
    raw_key = str(
        SETTINGS.get("HORDE_WORKER_API_KEY")
        or (SETTINGS.get("HORDE_API_KEY") if SETTINGS.get("HORDE_API_KEY") != "0000000000" else "")
        or ""
    ).strip()
    raw_name = str(SETTINGS.get("HORDE_WORKER_NAME") or "Direct-Dreamer").strip() or "Direct-Dreamer"
    raw_models_str = str(SETTINGS.get("HORDE_WORKER_MODELS") or "").strip()
    if raw_models_str:
        default_models = [
            m.strip() for m in raw_models_str.split(",")
            if m.strip() in LOCAL_SDXL_MODEL_CATALOG
        ] or ["CyberRealistic Pony"]
    else:
        default_models = ["CyberRealistic Pony"]

    return {
        "enabled": raw_enabled,
        "worker_name": raw_name,
        "api_key": raw_key,
        "nsfw": raw_nsfw,
        "max_pixels": raw_max_pixels,
        "poll_interval_idle": 10.0,
        "poll_interval_busy": 2.0,
        "sdxl_enabled": True,
        "sdxl_models": default_models,
        "krea_enabled": False,
        "krea_daily_limit": raw_limit,
        "krea_daily_date": _utc_today_str(),
        "krea_daily_count": 0,
        "total_jobs_completed": 0,
        "sdxl_jobs_completed": 0,
        "krea_jobs_completed": 0,
        "total_jobs_faulted": 0,
        "total_jobs_submit_failed": 0,
        "total_csam_filtered": 0,
        "total_nsfw_censored": 0,
        "total_kudos_earned": 0.0,
        "total_megapixelsteps": 0.0,
        "recent_jobs": [],
    }


def _sanitize_recent_jobs(jobs: Any) -> list[dict[str, Any]]:
    """Removes any prompt-bearing fields from persisted recent job telemetry records."""
    if not isinstance(jobs, list):
        return []
    cleaned: list[dict[str, Any]] = []
    for item in jobs[:40]:
        if isinstance(item, dict):
            rec = {k: v for k, v in item.items() if k not in ("prompt", "prompt_preview")}
            cleaned.append(rec)
    return cleaned


def _fail_closed_data(defaults: dict[str, Any]) -> dict[str, Any]:
    """
    Returns a fail-closed worker configuration when an existing `DATA_PATH` file is unreadable,
    corrupted, or has an invalid schema. Preserves last-known-good restrictive operator policies
    and never re-enables intake (`enabled=False, nsfw=False, sdxl_models=[], krea_enabled=False`).
    """
    base = dict(defaults)
    if _last_known_good_data is not None and _last_known_good_path == DATA_PATH:
        base.update(_last_known_good_data)
        base["enabled"] = False
        base["nsfw"] = bool(_last_known_good_data.get("nsfw", False))
        raw_lkg_models = _last_known_good_data.get("sdxl_models")
        base["sdxl_models"] = list(raw_lkg_models) if isinstance(raw_lkg_models, list) else []
        base["krea_enabled"] = False
    else:
        base["enabled"] = False
        base["nsfw"] = False
        base["sdxl_models"] = []
        base["krea_enabled"] = False
    base["recent_jobs"] = _sanitize_recent_jobs(base.get("recent_jobs"))
    return base


def load_dreamer_data() -> dict[str, Any]:
    global _last_known_good_data, _last_known_good_path
    with _data_lock:
        defaults = _default_data()
        if not os.path.exists(DATA_PATH):
            return defaults
        try:
            with open(DATA_PATH, "r", encoding="utf-8") as f:
                loaded = json.load(f)
            if not isinstance(loaded, dict):
                logger.error(f"[HordeDreamer] Invalid top-level JSON type in {DATA_PATH}: {type(loaded).__name__}; failing closed.")
                return _fail_closed_data(defaults)
            merged = dict(defaults)
            merged.update(loaded)
            # Fail-closed validation on security/policy fields if corrupted in state file
            if "enabled" in loaded and not isinstance(loaded["enabled"], bool):
                merged["enabled"] = False
            else:
                merged["enabled"] = bool(merged.get("enabled", False))
            if "nsfw" in loaded and not isinstance(loaded["nsfw"], bool):
                merged["nsfw"] = False
            else:
                merged["nsfw"] = bool(merged.get("nsfw", False))
            if "sdxl_enabled" in loaded and not isinstance(loaded["sdxl_enabled"], bool):
                merged["sdxl_enabled"] = False
            if "sdxl_models" in loaded:
                if isinstance(loaded["sdxl_models"], list):
                    merged["sdxl_models"] = [
                        str(m).strip()
                        for m in loaded["sdxl_models"]
                        if isinstance(m, str) and str(m).strip() in LOCAL_SDXL_MODEL_CATALOG
                    ]
                else:
                    merged["sdxl_models"] = []
            merged["krea_enabled"] = False
            merged["recent_jobs"] = _sanitize_recent_jobs(merged.get("recent_jobs"))
            if not merged.get("api_key") and defaults.get("api_key"):
                merged["api_key"] = defaults["api_key"]
            # Roll over daily counter if UTC date changed
            today = _utc_today_str()
            if merged.get("krea_daily_date") != today:
                merged["krea_daily_date"] = today
                merged["krea_daily_count"] = 0
            _last_known_good_data = dict(merged)
            _last_known_good_path = DATA_PATH
            return merged
        except Exception as e:
            logger.error(f"[HordeDreamer] Failed reading {DATA_PATH}: {e}; failing closed.")
            return _fail_closed_data(defaults)


def save_dreamer_data(data: dict[str, Any]) -> None:
    global _last_known_good_data, _last_known_good_path
    with _data_lock:
        today = _utc_today_str()
        if data.get("krea_daily_date") != today:
            data["krea_daily_date"] = today
            data["krea_daily_count"] = 0
        data["recent_jobs"] = _sanitize_recent_jobs(data.get("recent_jobs"))
        tmp = DATA_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        try:
            os.chmod(tmp, 0o600)
        except Exception:
            pass
        os.replace(tmp, DATA_PATH)
        _last_known_good_data = dict(data)
        _last_known_good_path = DATA_PATH


def update_dreamer_data(mutator: Any) -> dict[str, Any]:
    """
    Atomically loads the latest dreamer state under `_data_lock`, applies `mutator(state)` to update
    only the intended fields without clobbering concurrent operator edits, saves to disk, and returns
    the updated dictionary.
    """
    with _data_lock:
        current = load_dreamer_data()
        mutator(current)
        save_dreamer_data(current)
        return current


def get_effective_api_key(data: Optional[dict[str, Any]] = None) -> str:
    if data is None:
        data = load_dreamer_data()
    k = str(data.get("api_key") or "").strip()
    if k and k != "0000000000":
        return k
    cfg_k = str(SETTINGS.get("HORDE_WORKER_API_KEY") or "").strip()
    if cfg_k and cfg_k != "0000000000":
        return cfg_k
    fallback_k = str(SETTINGS.get("HORDE_API_KEY") or "").strip()
    if fallback_k and fallback_k != "0000000000":
        return fallback_k
    return ""


def _verify_reference_checkpoint(
    ckpt_name: Any,
    expected_sha256: Optional[str] = None,
    expected_size_bytes: Optional[int] = None,
) -> bool:
    """
    Verifies that `ckpt_name` (or a `LOCAL_SDXL_MODEL_CATALOG` spec dict) exists in
    `COMFY_CHECKPOINTS_DIR` and matches the exact `AI-Horde-image-model-reference`
    byte size and SHA256 digest (cached by `(path, mtime_ns, size)`).
    """
    if isinstance(ckpt_name, dict):
        expected_sha256 = ckpt_name.get("sha256", expected_sha256)
        expected_size_bytes = ckpt_name.get("size_bytes", expected_size_bytes)
        ckpt_name = ckpt_name.get("ckpt", "")
    if not ckpt_name:
        return False
    ckpts_dir = os.path.expanduser(str(SETTINGS.get("COMFY_CHECKPOINTS_DIR", "~/projects/ComfyUI/models/checkpoints")))
    full_path = os.path.join(ckpts_dir, ckpt_name)
    try:
        st = os.stat(full_path)
    except OSError:
        return False
    if expected_size_bytes is not None and st.st_size != int(expected_size_bytes):
        return False
    if not expected_sha256:
        return True
    cache_key = (full_path, int(getattr(st, "st_mtime_ns", int(st.st_mtime * 1e9))), int(st.st_size))
    digest = _checkpoint_sha256_cache.get(cache_key)
    if digest is None:
        h = hashlib.sha256()
        try:
            with open(full_path, "rb") as f:
                while True:
                    chunk = f.read(8 * 1024 * 1024)
                    if not chunk:
                        break
                    h.update(chunk)
            digest = h.hexdigest().lower()
            _checkpoint_sha256_cache[cache_key] = digest
        except OSError:
            return False
    return digest == expected_sha256.strip().lower()


async def verify_comfy_checkpoint_registered(
    ckpt_name: str,
    session: Optional[aiohttp.ClientSession] = None,
) -> bool:
    """
    Queries the connected ComfyUI `/object_info/CheckpointLoaderSimple` endpoint to confirm
    that the running ComfyUI instance resolves `ckpt_name` in its checkpoint list (fail-closed).
    """
    if not ckpt_name:
        return False
    comfy_url = str(SETTINGS.get("COMFY_API_URL", "http://localhost:8188")).rstrip("/")
    if not comfy_url:
        return False

    async def _check(sess: aiohttp.ClientSession) -> bool:
        async with sess.get(
            f"{comfy_url}/object_info/CheckpointLoaderSimple",
            timeout=aiohttp.ClientTimeout(total=3.0),
        ) as resp:
            if resp.status != 200:
                return False
            info = await resp.json()
            if not isinstance(info, dict):
                return False
            node_spec = info.get("CheckpointLoaderSimple")
            if not isinstance(node_spec, dict):
                return False
            req_inputs = (node_spec.get("input") or {}).get("required") or {}
            ckpt_entry = req_inputs.get("ckpt_name")
            if isinstance(ckpt_entry, list) and ckpt_entry and isinstance(ckpt_entry[0], list):
                available = [str(x) for x in ckpt_entry[0]]
                return ckpt_name in available
            return False

    try:
        if session is not None and not session.closed:
            return await _check(session)
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=3.0)) as own_sess:
            return await _check(own_sess)
    except Exception:
        return False


def is_krea_reference_verified() -> bool:
    """
    Krea (`Krea2-Turbo_fp8`) is permanently excluded until registered in the live
    `models.aihorde.net` reference and remote cryptographic asset + cleanup verification are implemented.
    """
    return False


def get_active_models_for_pop(data: Optional[dict[str, Any]] = None) -> list[str]:
    """
    Returns the list of AI Horde models this worker currently offers for `/v2/generate/pop`:
    - Local SDXL models when `sdxl_enabled` is True AND their `.safetensors` file passes
      `AI-Horde-image-model-reference` SHA256 + byte-size verification.
    - Distinguishes an absent setting (`None`) from an explicitly empty selection (`[]`).
    - Krea (`Krea2-Turbo_fp8`) is excluded.
    """
    if data is None:
        data = load_dreamer_data()

    models: list[str] = []
    if data.get("sdxl_enabled", True):
        raw_selected = data.get("sdxl_models")
        if raw_selected is None:
            selected_models = ["CyberRealistic Pony"]
        elif isinstance(raw_selected, list):
            selected_models = raw_selected
        else:
            selected_models = []
        for m in selected_models:
            m_str = str(m).strip()
            cat_entry = LOCAL_SDXL_MODEL_CATALOG.get(m_str)
            if (
                cat_entry
                and m_str not in models
                and _verify_reference_checkpoint(
                    cat_entry.get("ckpt", ""),
                    cat_entry.get("sha256"),
                    cat_entry.get("size_bytes"),
                )
            ):
                models.append(m_str)

    return models


async def is_local_comfy_idle() -> bool:
    """
    Returns True if local ComfyUI is reachable AND its running + pending queues are completely empty,
    and Autopilot is not currently generating a post.
    """
    try:
        from extensions import autopilot
        if getattr(autopilot, "_gen_lock", None) is not None and autopilot._gen_lock.locked():
            return False
    except Exception:
        pass

    comfy_url = str(SETTINGS.get("COMFY_API_URL", "http://localhost:8188")).rstrip("/")
    if not comfy_url:
        return False

    try:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=2.5)) as session:
            async with session.get(f"{comfy_url}/queue") as resp:
                if resp.status != 200:
                    return False
                qdata = await resp.json()
                running = qdata.get("queue_running") or []
                pending = qdata.get("queue_pending") or []
                return len(running) == 0 and len(pending) == 0
    except Exception:
        return False


def _split_horde_prompt(raw_prompt: str) -> tuple[str, str]:
    """Splits AI Horde prompt string on '###' into (positive_prompt, negative_prompt)."""
    if not raw_prompt:
        return "", ""
    if PROMPT_SEPARATOR in raw_prompt:
        pos, _, neg = raw_prompt.partition(PROMPT_SEPARATOR)
        return pos.strip(), neg.strip()
    return raw_prompt.strip(), ""


def _expand_dynamic_prompt(prompt: str, seed: int) -> str:
    """
    Expands `{option1|option2}` dynamic prompt syntax (including nested `{a|{b|c}}` choices)
    using a deterministic PRNG seeded with the generation `seed`, matching
    `Haidra-Org/hordelib/hordelib/utils/dynamicprompt.py` (`DynamicPromptParser`).
    """
    if not prompt or "{" not in prompt:
        return prompt
    rng = random.Random(int(seed))
    text = prompt
    for _ in range(64):
        match = _DYNAMIC_PROMPT_RE.search(text)
        if not match:
            break
        options = match.group(1).split("|")
        choice = rng.choice(options)
        text = text[: match.start()] + choice + text[match.end() :]
    return text.strip()


def _resolve_comfy_sampler(sampler_name: Optional[str]) -> str:
    """
    Maps an AI Horde sampler name to its canonical ComfyUI KSampler `sampler_name`.
    Raises `ValueError` for unsupported/unknown samplers instead of silently substituting `euler`.
    """
    if sampler_name is None or str(sampler_name).strip() == "":
        return HORDE_TO_COMFY_SAMPLERS["k_euler"]
    raw = str(sampler_name).strip().lower()
    if raw not in HORDE_TO_COMFY_SAMPLERS:
        raise ValueError(f"Unsupported sampler_name for ComfyUI worker: {sampler_name!r}")
    return HORDE_TO_COMFY_SAMPLERS[raw]


def _resolve_comfy_scheduler(comfy_sampler: str, scheduler: Optional[str], karras: bool = True) -> str:
    """
    Resolves the ComfyUI KSampler `scheduler` from the AI Horde request's `scheduler` and `karras` fields,
    matching `Haidra-Org/AI-Horde/horde/consts.py` (`scheduler_for_request`) and `Haidra-Org/hordelib`
    (`dpmpp_3m_sde` schedule guard). Raises `ValueError` if an explicit `scheduler` is unsupported.
    """
    if scheduler is not None and str(scheduler).strip() != "":
        cand = str(scheduler).strip().lower()
        if cand not in COMFY_SUPPORTED_SCHEDULERS:
            raise ValueError(f"Unsupported scheduler for ComfyUI worker: {scheduler!r}")
        eff_sched = cand
    else:
        eff_sched = "karras" if karras else "normal"

    # `dpmpp_3m_sde` diverges to color noise under `normal`; `hordelib` substitutes `karras`.
    if comfy_sampler == "dpmpp_3m_sde" and eff_sched == "normal":
        eff_sched = "karras"
    return eff_sched


def _normalize_horde_seed(raw_seed: Any, *, allow_random: bool = True) -> int:
    """
    Preserves server-normalized seeds across the full supported ComfyUI uint64 range (`0 .. 2^64-1`),
    including seed `0`. When `allow_random=False` (used for popped worker assignments), a missing
    or empty seed raises `ValueError` instead of conflating with `0` or randomizing.
    """
    if raw_seed is None:
        if not allow_random:
            raise ValueError("Missing required seed in popped AI Horde job payload")
        return random.randint(0, 0xFFFFFFFF)
    if isinstance(raw_seed, bool):
        raise ValueError(f"Invalid boolean seed: {raw_seed!r}")
    if isinstance(raw_seed, str):
        s = raw_seed.strip()
        if s == "":
            if not allow_random:
                raise ValueError("Empty seed string in popped AI Horde job payload")
            return random.randint(0, 0xFFFFFFFF)
        try:
            seed_int = int(s, 10)
        except ValueError as exc:
            raise ValueError(f"Invalid non-integer seed: {raw_seed!r}") from exc
    elif isinstance(raw_seed, int):
        seed_int = int(raw_seed)
    else:
        raise ValueError(f"Invalid seed type: {type(raw_seed).__name__}")

    if seed_int < 0 or seed_int > MAX_HORDE_SEED:
        raise ValueError(f"Seed {seed_int} is outside supported range [0, {MAX_HORDE_SEED}]")
    return seed_int


def _validate_horde_dimensions(
    width: Any,
    height: Any,
    *,
    max_pixels: Optional[int] = None,
) -> tuple[int, int]:
    """
    Validates and preserves exact server-normalized dimensions without silent clamping or resizing.
    Raises `ValueError` if `width` or `height` is outside `[64, 8192]`, not divisible by `64`,
    or if `width * height > max_pixels`.
    """
    def _parse_dim(val: Any, name: str) -> int:
        if val is None:
            return 512
        if isinstance(val, bool):
            raise ValueError(f"Invalid boolean {name}: {val!r}")
        if isinstance(val, float) and not val.is_integer():
            raise ValueError(f"Non-integer {name}: {val!r}")
        try:
            d = int(val)
        except (ValueError, TypeError) as exc:
            raise ValueError(f"Invalid {name}: {val!r}") from exc
        if d < MIN_HORDE_DIMENSION or d > MAX_HORDE_DIMENSION:
            raise ValueError(f"{name}={d} is outside supported range [{MIN_HORDE_DIMENSION}, {MAX_HORDE_DIMENSION}]")
        if d % HORDE_DIMENSION_MULTIPLE != 0:
            raise ValueError(f"{name}={d} must be divisible by {HORDE_DIMENSION_MULTIPLE}")
        return d

    w = _parse_dim(width, "width")
    h = _parse_dim(height, "height")
    if max_pixels is not None and (w * h) > int(max_pixels):
        raise ValueError(f"Requested resolution {w}x{h} ({w * h} px) exceeds worker max_pixels ({max_pixels})")
    return w, h


def _validate_horde_int_param(value: Any, *, name: str, min_val: int, max_val: int, default: int) -> int:
    """Validates an integer generation parameter without silent clamping of out-of-range values."""
    if value is None:
        return default
    if isinstance(value, bool):
        raise ValueError(f"Invalid boolean {name}: {value!r}")
    if isinstance(value, float) and not value.is_integer():
        raise ValueError(f"Non-integer {name}: {value!r}")
    try:
        val = int(value)
    except (ValueError, TypeError) as exc:
        raise ValueError(f"Invalid {name}: {value!r}") from exc
    if val < min_val or val > max_val:
        raise ValueError(f"{name}={val} is outside supported range [{min_val}, {max_val}]")
    return val


def _validate_horde_float_param(value: Any, *, name: str, min_val: float, max_val: float, default: float) -> float:
    """Validates a float generation parameter without silent clamping of out-of-range values."""
    if value is None:
        return default
    if isinstance(value, bool):
        raise ValueError(f"Invalid boolean {name}: {value!r}")
    try:
        val = float(value)
    except (ValueError, TypeError) as exc:
        raise ValueError(f"Invalid {name}: {value!r}") from exc
    if val < min_val or val > max_val:
        raise ValueError(f"{name}={val} is outside supported range [{min_val}, {max_val}]")
    return val


def _validate_supported_job_payload(gen_payload: dict[str, Any], *, max_pixels: Optional[int] = None) -> dict[str, Any]:
    """
    Validates that an incoming AI Horde payload uses only supported features and valid parameter
    bounds, preserving exact dimensions, seeds, steps, CFG, denoise, clip_skip, sampler, and scheduler.
    Raises `ValueError` if any unsupported feature (LoRAs, TI, ControlNet, img2img, post-processing,
    unsupported sampler/scheduler, or missing/out-of-bounds dimension/seed/parameter) is present.
    """
    if not isinstance(gen_payload, dict):
        raise ValueError("Invalid job payload: expected dict")
    if gen_payload.get("loras"):
        raise ValueError("Unsupported payload feature: loras")
    if gen_payload.get("tis"):
        raise ValueError("Unsupported payload feature: textual_inversion (tis)")
    if gen_payload.get("control_type") or gen_payload.get("control_net"):
        raise ValueError("Unsupported payload feature: controlnet")
    if gen_payload.get("source_image") or gen_payload.get("source_processing") not in (None, "", "txt2img"):
        raise ValueError("Unsupported payload feature: img2img / source_image")
    if gen_payload.get("post_processing"):
        raise ValueError("Unsupported payload feature: post_processing")
    if gen_payload.get("tiling"):
        raise ValueError("Unsupported payload feature: tiling")
    if gen_payload.get("hires_fix"):
        raise ValueError("Unsupported payload feature: hires_fix")
    if gen_payload.get("transparent"):
        raise ValueError("Unsupported payload feature: transparent")
    if gen_payload.get("workflow") not in (None, ""):
        raise ValueError("Unsupported payload feature: custom workflow")
    if gen_payload.get("n") not in (None, 1):
        raise ValueError(f"Unsupported batch size n={gen_payload.get('n')!r}; single-image worker only")

    width, height = _validate_horde_dimensions(
        gen_payload.get("width"),
        gen_payload.get("height"),
        max_pixels=max_pixels,
    )
    raw_steps = gen_payload.get("ddim_steps") if gen_payload.get("ddim_steps") is not None else gen_payload.get("steps")
    steps = _validate_horde_int_param(raw_steps, name="steps", min_val=1, max_val=500, default=30)
    cfg_scale = _validate_horde_float_param(gen_payload.get("cfg_scale"), name="cfg_scale", min_val=1.0, max_val=100.0, default=8.0)
    clip_skip = _validate_horde_int_param(gen_payload.get("clip_skip"), name="clip_skip", min_val=1, max_val=20, default=1)
    raw_denoise = (
        gen_payload.get("denoising_strength")
        if gen_payload.get("denoising_strength") is not None
        else gen_payload.get("denoise")
    )
    denoise = _validate_horde_float_param(raw_denoise, name="denoising_strength", min_val=0.01, max_val=1.0, default=1.0)
    sampler_name = str(gen_payload.get("sampler_name") or "k_euler")
    karras = bool(gen_payload.get("karras", True))
    scheduler = gen_payload.get("scheduler")
    comfy_sampler = _resolve_comfy_sampler(sampler_name)
    comfy_scheduler = _resolve_comfy_scheduler(comfy_sampler, scheduler, karras)
    seed_int = _normalize_horde_seed(gen_payload.get("seed"), allow_random=False)

    return {
        "width": width,
        "height": height,
        "steps": steps,
        "cfg_scale": cfg_scale,
        "clip_skip": clip_skip,
        "denoise": denoise,
        "sampler_name": sampler_name,
        "karras": karras,
        "scheduler": scheduler,
        "comfy_sampler": comfy_sampler,
        "comfy_scheduler": comfy_scheduler,
        "seed": seed_int,
    }


def _build_sdxl_horde_workflow(
    positive_prompt: str,
    negative_prompt: str,
    ckpt_name: str,
    width: int,
    height: int,
    seed: int,
    steps: int = 30,
    cfg: float = 8.0,
    sampler_name: str = "k_euler",
    karras: bool = True,
    scheduler: Optional[str] = None,
    clip_skip: int = 1,
    denoise: float = 1.0,
    max_pixels: Optional[int] = None,
) -> dict[str, Any]:
    """
    Builds a pure, unmodified ComfyUI SDXL text-to-image workflow matching `Haidra-Org/hordelib`'s
    `stable_diffusion` pipeline (`CheckpointLoaderSimple -> CLIPSetLastLayer -> CLIPTextEncode ->
    EmptyLatentImage -> KSampler -> VAEDecode -> SaveImageWebsocket`), honoring the exact request
    parameters (`steps`, `cfg_scale`, `denoise`, `sampler_name`, `karras`/`scheduler`, `clip_skip`,
    `seed`, `width`, `height`, and canonical dynamic-prompt expansion on `positive_prompt`)
    while streaming output directly over the WebSocket without writing preview files to disk.
    """
    w_exact, h_exact = _validate_horde_dimensions(width, height, max_pixels=max_pixels)
    eff_steps = _validate_horde_int_param(steps, name="steps", min_val=1, max_val=500, default=30)
    eff_cfg = _validate_horde_float_param(cfg, name="cfg_scale", min_val=1.0, max_val=100.0, default=8.0)
    eff_clip_skip = _validate_horde_int_param(clip_skip, name="clip_skip", min_val=1, max_val=20, default=1)
    eff_denoise = _validate_horde_float_param(denoise, name="denoising_strength", min_val=0.01, max_val=1.0, default=1.0)
    eff_seed = _normalize_horde_seed(seed, allow_random=False)
    comfy_sampler = _resolve_comfy_sampler(sampler_name)
    comfy_scheduler = _resolve_comfy_scheduler(comfy_sampler, scheduler, karras)
    expanded_positive = _expand_dynamic_prompt(positive_prompt, eff_seed)

    return {
        "1": {
            "class_type": "CheckpointLoaderSimple",
            "inputs": {"ckpt_name": ckpt_name},
        },
        "2": {
            "class_type": "CLIPSetLastLayer",
            "inputs": {"stop_at_clip_layer": -eff_clip_skip, "clip": ["1", 1]},
        },
        "3": {
            "class_type": "CLIPTextEncode",
            "inputs": {"text": expanded_positive, "clip": ["2", 0]},
        },
        "4": {
            "class_type": "CLIPTextEncode",
            "inputs": {"text": negative_prompt, "clip": ["2", 0]},
        },
        "5": {
            "class_type": "EmptyLatentImage",
            "inputs": {"width": w_exact, "height": h_exact, "batch_size": 1},
        },
        "6": {
            "class_type": "KSampler",
            "inputs": {
                "seed": eff_seed,
                "steps": eff_steps,
                "cfg": eff_cfg,
                "sampler_name": comfy_sampler,
                "scheduler": comfy_scheduler,
                "denoise": eff_denoise,
                "model": ["1", 0],
                "positive": ["3", 0],
                "negative": ["4", 0],
                "latent_image": ["5", 0],
            },
        },
        "7": {
            "class_type": "VAEDecode",
            "inputs": {"samples": ["6", 0], "vae": ["1", 2]},
        },
        "8": {
            "class_type": "SaveImageWebsocket",
            "inputs": {"images": ["7", 0]},
        },
    }


def _build_krea_horde_workflow(
    positive_prompt: str,
    negative_prompt: str,
    width: int,
    height: int,
    seed: int,
    steps: int = 30,
    cfg: float = 8.0,
    sampler_name: str = "k_euler",
    karras: bool = False,
    scheduler: Optional[str] = None,
    denoise: float = 1.0,
    max_pixels: Optional[int] = None,
) -> dict[str, Any]:
    """
    Builds the Krea 2 UNET workflow (currently disabled/excluded for AI Horde dispatch).
    """
    unet_model = KREA_REFERENCE_FILES["unet"]
    clip_model = KREA_REFERENCE_FILES["clip"]
    vae_model = KREA_REFERENCE_FILES["vae"]

    w_exact, h_exact = _validate_horde_dimensions(width, height, max_pixels=max_pixels)
    eff_steps = _validate_horde_int_param(steps, name="steps", min_val=1, max_val=500, default=30)
    eff_cfg = _validate_horde_float_param(cfg, name="cfg_scale", min_val=1.0, max_val=100.0, default=8.0)
    eff_denoise = _validate_horde_float_param(denoise, name="denoising_strength", min_val=0.01, max_val=1.0, default=1.0)
    eff_seed = _normalize_horde_seed(seed, allow_random=False)
    comfy_sampler = _resolve_comfy_sampler(sampler_name)
    comfy_scheduler = _resolve_comfy_scheduler(comfy_sampler, scheduler, karras)
    expanded_positive = _expand_dynamic_prompt(positive_prompt, eff_seed)

    return {
        "1": {
            "class_type": "UNETLoader",
            "inputs": {"unet_name": unet_model, "weight_dtype": "default"},
        },
        "2": {
            "class_type": "CLIPLoader",
            "inputs": {"clip_name": clip_model, "type": "krea2"},
        },
        "3": {
            "class_type": "VAELoader",
            "inputs": {"vae_name": vae_model},
        },
        "4": {
            "class_type": "CLIPTextEncode",
            "inputs": {"text": expanded_positive, "clip": ["2", 0]},
        },
        "5": {
            "class_type": "CLIPTextEncode",
            "inputs": {"text": negative_prompt, "clip": ["2", 0]},
        },
        "6": {
            "class_type": "EmptySD3LatentImage",
            "inputs": {"width": w_exact, "height": h_exact, "batch_size": 1},
        },
        "7": {
            "class_type": "KSampler",
            "inputs": {
                "seed": eff_seed,
                "steps": eff_steps,
                "cfg": eff_cfg,
                "sampler_name": comfy_sampler,
                "scheduler": comfy_scheduler,
                "denoise": eff_denoise,
                "model": ["1", 0],
                "positive": ["4", 0],
                "negative": ["5", 0],
                "latent_image": ["6", 0],
            },
        },
        "8": {
            "class_type": "VAEDecode",
            "inputs": {"samples": ["7", 0], "vae": ["3", 0]},
        },
        "9": {
            "class_type": "SaveImageWebsocket",
            "inputs": {"images": ["8", 0]},
        },
    }


def _get_comfy_base_dir() -> str:
    """Derives the configured ComfyUI root directory from `SETTINGS` without hardcoding paths."""
    explicit_base = str(SETTINGS.get("COMFY_DIR", "")).strip()
    if explicit_base:
        return os.path.expanduser(explicit_base)
    ckpts_dir = os.path.expanduser(str(SETTINGS.get("COMFY_CHECKPOINTS_DIR", "~/projects/ComfyUI/models/checkpoints")))
    # Typical layout: <comfy_root>/models/checkpoints
    parent = os.path.dirname(ckpts_dir.rstrip("/\\"))
    if os.path.basename(parent) == "models":
        return os.path.dirname(parent)
    return parent


def _resolve_comfy_artifact_path(filename: str, subfolder: str = "", img_type: str = "temp") -> Optional[str]:
    """Resolves the local filesystem path of a ComfyUI output/temp artifact from configured directories."""
    if not filename:
        return None
    safe_name = os.path.basename(filename)
    if img_type == "output":
        base_dir = str(SETTINGS.get("COMFY_OUTPUT_DIR", "")).strip()
        if not base_dir:
            base_dir = os.path.join(_get_comfy_base_dir(), "output")
    else:
        base_dir = str(SETTINGS.get("COMFY_TEMP_DIR", "")).strip()
        if not base_dir:
            base_dir = os.path.join(_get_comfy_base_dir(), "temp")
    base_dir = os.path.expanduser(base_dir)
    if subfolder:
        norm_sub = os.path.normpath(subfolder).lstrip("/\\")
        if not norm_sub.startswith(".."):
            base_dir = os.path.join(base_dir, norm_sub)
    return os.path.join(base_dir, safe_name)


def _remove_renderer_artifacts(artifact_paths: list[str]) -> int:
    """Immediately deletes tracked worker-owned renderer artifact files from disk and returns the count removed."""
    removed = 0
    for path in artifact_paths:
        if not path:
            continue
        try:
            if os.path.isfile(path) or os.path.islink(path):
                os.remove(path)
                removed += 1
        except OSError as e:
            logger.warning(f"[HordeDreamer] Failed removing renderer artifact {path}: {e}")
    return removed


async def _cancel_comfy_prompt(comfy_url: str, prompt_id: str) -> None:
    """Cancels a queued or running ComfyUI prompt and deletes its history entry."""
    if not comfy_url or not prompt_id:
        return
    try:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=5.0)) as session:
            try:
                await session.post(f"{comfy_url}/queue", json={"delete": [prompt_id]})
            except Exception:
                pass
            try:
                await session.post(f"{comfy_url}/interrupt", json={"prompt_id": prompt_id})
            except Exception:
                pass
            try:
                await session.post(f"{comfy_url}/history", json={"delete": [prompt_id]})
            except Exception:
                pass
    except Exception as e:
        logger.debug(f"[HordeDreamer] Cleanup for ComfyUI prompt {prompt_id} raised: {e}")


async def _delete_comfy_history(session: aiohttp.ClientSession, comfy_url: str, prompt_id: str) -> None:
    """Removes a completed prompt from ComfyUI's in-memory `/history` so previews/prompts are not retained."""
    if not comfy_url or not prompt_id:
        return
    try:
        async with session.post(
            f"{comfy_url}/history",
            json={"delete": [prompt_id]},
            timeout=aiohttp.ClientTimeout(total=5.0),
        ):
            pass
    except Exception:
        pass


async def _run_local_comfy_silent(
    workflow: dict[str, Any],
    timeout: float = 180.0,
    tracked_artifacts: Optional[list[str]] = None,
) -> Optional[bytes]:
    """
    Executes a workflow on local ComfyUI (`COMFY_API_URL`) with a strict wall-clock deadline (`timeout`),
    streams output in-memory via `SaveImageWebsocket` (or deletes any fallback worker-owned file artifact
    and `/history` entry immediately), requires an authoritative `executing: node=None` completion
    event with zero `execution_error` events and exactly one output image, and cancels the ComfyUI
    prompt on timeout, EOF, error, or cancellation.
    """
    comfy_url = str(SETTINGS.get("COMFY_API_URL", "http://localhost:8188")).rstrip("/")
    client_id = str(uuid.uuid4())
    ws_url = f"{comfy_url.replace('http', 'ws')}/ws?clientId={client_id}"
    prompt_id: Optional[str] = None
    local_artifacts: list[str] = tracked_artifacts if tracked_artifacts is not None else []
    confirmed_completion = False
    had_execution_error = False

    async def _execute() -> Optional[bytes]:
        nonlocal prompt_id, confirmed_completion, had_execution_error
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, timeout=15.0, receive_timeout=float(timeout)) as ws:
                payload = {
                    "prompt": workflow,
                    "client_id": client_id,
                    "extra_data": {"preview_method": "none"},
                }
                async with session.post(
                    f"{comfy_url}/prompt",
                    json=payload,
                    timeout=aiohttp.ClientTimeout(total=15.0),
                ) as resp:
                    if resp.status != 200:
                        err_txt = await resp.text()
                        logger.error(f"[HordeDreamer] ComfyUI /prompt HTTP {resp.status}: {err_txt[:200]}")
                        return None
                    resp_data = await resp.json()
                    prompt_id = resp_data.get("prompt_id")
                    if not prompt_id:
                        logger.error(f"[HordeDreamer] ComfyUI /prompt missing prompt_id: {resp_data}")
                        return None

                received_images: list[bytes] = []
                current_node: Optional[str] = None

                async for msg in ws:
                    if msg.type == aiohttp.WSMsgType.TEXT:
                        data = json.loads(msg.data)
                        msg_type = data.get("type")
                        data_content = data.get("data") or {}
                        msg_prompt_id = data_content.get("prompt_id")

                        if msg_type == "executing" and msg_prompt_id in (None, prompt_id):
                            node_id = data_content.get("node")
                            if node_id is None and msg_prompt_id == prompt_id:
                                confirmed_completion = True
                                break
                            current_node = str(node_id) if node_id is not None else None
                        elif msg_type in ("execution_error", "execution_interrupted") and msg_prompt_id in (None, prompt_id):
                            had_execution_error = True
                            logger.error(f"[HordeDreamer] Local ComfyUI {msg_type} for prompt {prompt_id}")
                            break
                        elif msg_type == "executed" and msg_prompt_id == prompt_id:
                            outputs = data_content.get("output") or {}
                            if isinstance(outputs, dict) and "images" in outputs:
                                for image in outputs.get("images") or []:
                                    filename = str(image.get("filename") or "")
                                    subfolder = str(image.get("subfolder") or "")
                                    img_type = str(image.get("type") or "temp")
                                    art_path = _resolve_comfy_artifact_path(filename, subfolder, img_type)
                                    if art_path and art_path not in local_artifacts:
                                        local_artifacts.append(art_path)
                                    params = {"filename": filename, "subfolder": subfolder, "type": img_type}
                                    async with session.get(f"{comfy_url}/view", params=params) as img_resp:
                                        if img_resp.status == 200:
                                            img_bytes = await img_resp.read()
                                            if img_bytes:
                                                received_images.append(img_bytes)
                    elif msg.type == aiohttp.WSMsgType.BINARY:
                        raw_bin = bytes(msg.data)
                        if len(raw_bin) > 8 and current_node in (None, "8", "9", "save_image_websocket_node"):
                            received_images.append(raw_bin[8:])
                    elif msg.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                        break

                if prompt_id:
                    await _delete_comfy_history(session, comfy_url, prompt_id)
                _remove_renderer_artifacts(local_artifacts)

                if had_execution_error or not confirmed_completion or len(received_images) != 1:
                    if prompt_id and not confirmed_completion:
                        await _cancel_comfy_prompt(comfy_url, prompt_id)
                    return None
                return received_images[0]

    try:
        return await asyncio.wait_for(_execute(), timeout=float(timeout))
    except asyncio.TimeoutError:
        logger.error(f"[HordeDreamer] Hard deadline ({timeout}s) exceeded waiting for local ComfyUI prompt {prompt_id}")
        if prompt_id:
            await asyncio.shield(_cancel_comfy_prompt(comfy_url, prompt_id))
        _remove_renderer_artifacts(local_artifacts)
        return None
    except asyncio.CancelledError:
        if prompt_id and not confirmed_completion:
            await asyncio.shield(_cancel_comfy_prompt(comfy_url, prompt_id))
        _remove_renderer_artifacts(local_artifacts)
        raise
    except Exception as e:
        logger.error(f"[HordeDreamer] Error in _run_local_comfy_silent: {e}")
        if prompt_id and not confirmed_completion:
            await asyncio.shield(_cancel_comfy_prompt(comfy_url, prompt_id))
        _remove_renderer_artifacts(local_artifacts)
        return None


async def _cleanup_modal_artifact(artifact_ref: Optional[str]) -> None:
    """Requests immediate deletion of a remote Modal renderer artifact if one was reported."""
    if not artifact_ref:
        return
    url = str(SETTINGS.get("MODAL_COMFY_URL", "")).strip()
    if not url:
        return
    headers = {"Content-Type": "application/json"}
    token = str(SETTINGS.get("MODAL_AUTH_TOKEN", "")).strip()
    if token:
        headers["Authorization"] = f"Bearer {token}"
    cleanup_endpoint = url.rstrip("/") + "/cleanup"
    try:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=5.0)) as session:
            async with session.post(cleanup_endpoint, json={"artifact": artifact_ref}, headers=headers):
                pass
    except Exception:
        pass


async def _run_modal_krea_silent(
    workflow: dict[str, Any],
    timeout: float = 180.0,
    tracked_remote_artifacts: Optional[list[str]] = None,
) -> Optional[bytes]:
    """
    Dispatches a workflow to remote Modal GPU with an enforced overall deadline (`timeout`),
    tracks any remote artifact identifiers for cleanup, and rejects any stream that emits an
    `error` event or closes without a confirmed `done`/`result` completion and single image.
    """
    url = str(SETTINGS.get("MODAL_COMFY_URL", "")).strip()
    if not url:
        return None

    headers = {"Content-Type": "application/json"}
    token = str(SETTINGS.get("MODAL_AUTH_TOKEN", "")).strip()
    if token:
        headers["Authorization"] = f"Bearer {token}"

    stream_endpoint = url.rstrip("/") + "/generate_stream"
    remote_artifacts = tracked_remote_artifacts if tracked_remote_artifacts is not None else []

    async def _execute_modal() -> Optional[bytes]:
        async with aiohttp.ClientSession() as session:
            async with session.post(
                stream_endpoint,
                json={"workflow": workflow, "ephemeral": True},
                headers=headers,
                timeout=aiohttp.ClientTimeout(total=float(timeout)),
            ) as resp:
                if resp.status != 200:
                    err_txt = await resp.text()
                    logger.error(f"[HordeDreamer] Modal HTTP {resp.status}: {err_txt[:200]}")
                    return None
                buf = bytearray()
                received_images: list[bytes] = []
                had_error = False
                confirmed_done = False

                def _process_event(ev: dict[str, Any]) -> None:
                    nonlocal had_error, confirmed_done
                    art_id = ev.get("artifact") or ev.get("filename")
                    if art_id and str(art_id) not in remote_artifacts:
                        remote_artifacts.append(str(art_id))
                    ev_type = ev.get("event")
                    if ev_type == "error" or ev.get("error"):
                        had_error = True
                        logger.error(f"[HordeDreamer] Modal stream error: {ev.get('error') or ev.get('detail')}")
                    elif ev_type in ("done", "result"):
                        if ev.get("image_b64"):
                            received_images.append(base64.b64decode(ev["image_b64"]))
                        confirmed_done = True

                async for chunk in resp.content.iter_any():
                    if not chunk:
                        continue
                    buf.extend(chunk)
                    while True:
                        nl = buf.find(b"\n")
                        if nl < 0:
                            break
                        line = bytes(buf[:nl]).strip()
                        del buf[: nl + 1]
                        if not line:
                            continue
                        try:
                            ev = json.loads(line.decode("utf-8"))
                        except Exception:
                            had_error = True
                            continue
                        if isinstance(ev, dict):
                            _process_event(ev)
                if buf.strip():
                    try:
                        ev = json.loads(bytes(buf).strip().decode("utf-8"))
                        if isinstance(ev, dict):
                            _process_event(ev)
                    except Exception:
                        had_error = True

                if had_error or not confirmed_done or len(received_images) != 1:
                    for art in remote_artifacts:
                        await _cleanup_modal_artifact(art)
                    return None
                return received_images[0]

    try:
        return await asyncio.wait_for(_execute_modal(), timeout=float(timeout))
    except asyncio.TimeoutError:
        logger.error(f"[HordeDreamer] Hard deadline ({timeout}s) exceeded waiting for Modal Krea stream")
        for art in remote_artifacts:
            await asyncio.shield(_cleanup_modal_artifact(art))
        return None
    except asyncio.CancelledError:
        for art in remote_artifacts:
            await asyncio.shield(_cleanup_modal_artifact(art))
        raise
    except Exception as e:
        logger.error(f"[HordeDreamer] Error in _run_modal_krea_silent: {e}")
        for art in remote_artifacts:
            await asyncio.shield(_cleanup_modal_artifact(art))
        return None


def _validate_rendered_image_bytes(png_bytes: bytes, expected_width: int, expected_height: int) -> None:
    """
    Validates that the uncensored rendered image bytes decode cleanly as an image whose exact pixel
    dimensions match the accepted `(expected_width, expected_height)` before safety evaluation.
    """
    if not png_bytes:
        raise ValueError("Renderer returned empty image bytes")
    with Image.open(io.BytesIO(png_bytes)) as im:
        im.load()
        actual_w, actual_h = im.size
    if (actual_w, actual_h) != (int(expected_width), int(expected_height)):
        raise ValueError(
            f"Rendered image dimensions {actual_w}x{actual_h} do not match requested {expected_width}x{expected_height}"
        )


def _convert_png_to_webp(img_bytes: bytes, quality: int = 92) -> bytes:
    """
    Converts PNG/JPEG bytes into AI Horde standard WebP bytes while stripping all embedded
    PNG text chunks, workflow JSON, and EXIF metadata.
    """
    with Image.open(io.BytesIO(img_bytes)) as im:
        mode = im.mode if im.mode in ("RGB", "RGBA") else "RGB"
        converted = im.convert(mode)
        clean_im = Image.new(mode, converted.size)
        clean_im.putdata(list(converted.getdata()))
        out = io.BytesIO()
        clean_im.save(out, format="WEBP", quality=quality, method=4, exif=b"")
        return out.getvalue()


async def _upload_webp_to_r2(
    session: aiohttp.ClientSession,
    r2_url: str,
    webp_bytes: bytes,
    *,
    timeout: float = 45.0,
) -> bool:
    """
    Uploads WebP image bytes to AI Horde's presigned Cloudflare R2 URL.
    Critically uses `skip_auto_headers=["Content-Type"]` so S3 signature verification succeeds.
    """
    eff_timeout = max(2.0, float(timeout))
    for attempt in range(2):
        try:
            async with session.put(
                r2_url,
                data=webp_bytes,
                skip_auto_headers=["Content-Type", "content-type"],
                timeout=aiohttp.ClientTimeout(total=eff_timeout),
            ) as resp:
                if resp.status in (200, 201, 204):
                    return True
                body = await resp.text()
                logger.warning(f"[HordeDreamer] R2 PUT attempt {attempt + 1} returned HTTP {resp.status}: {body[:200]}")
        except Exception as e:
            logger.warning(f"[HordeDreamer] R2 PUT attempt {attempt + 1} error: {e}")
        await asyncio.sleep(0.5)
    return False


async def fetch_horde_user_info(api_key: Optional[str] = None, force: bool = False) -> Optional[dict[str, Any]]:
    """Fetches `/api/v2/find_user` for the configured worker API key (cached for 120s unless forced)."""
    now = time.time()
    if not force and _runtime_status.get("user_info") and (now - float(_runtime_status.get("user_info_ts") or 0)) < 120.0:
        return _runtime_status["user_info"]

    eff_key = api_key or get_effective_api_key()
    if not eff_key:
        return None

    try:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=10.0)) as session:
            async with session.get(
                f"{HORDE_API_BASE}/find_user",
                headers={"apikey": eff_key, "Client-Agent": CLIENT_AGENT},
            ) as resp:
                if resp.status == 200:
                    uinfo = await resp.json()
                    _runtime_status["user_info"] = uinfo
                    _runtime_status["user_info_ts"] = now
                    return uinfo
                else:
                    txt = await resp.text()
                    logger.warning(f"[HordeDreamer] /find_user returned HTTP {resp.status}: {txt[:160]}")
    except Exception as e:
        logger.warning(f"[HordeDreamer] /find_user error: {e}")
    return _runtime_status.get("user_info")


async def _fault_popped_job(
    session: Optional[aiohttp.ClientSession],
    job_id: str,
    seed_int: int,
    headers: dict[str, str],
    *,
    reason: str,
    job_model: str = "unknown",
    backend_label: str = "Local SDXL",
    width: int = 512,
    height: int = 512,
    steps: int = 30,
    duration_sec: float = 0.0,
    pos_prompt: str = "",
) -> dict[str, Any]:
    """
    Terminal fault handler for any popped job that failed during validation, workflow construction,
    rendering, image verification, safety evaluation, or conversion. Submits `state='faulted'` with
    bounded retry, verifies backend acknowledgement, and records telemetry without retaining prompt text.
    """
    del pos_prompt  # Never persist or log user prompt text
    fault_body = {
        "id": job_id,
        "generation": "faulted",
        "state": "faulted",
        "seed": int(seed_int) if isinstance(seed_int, int) and seed_int >= 0 else 0,
        "censored": False,
    }
    fault_acked = False
    last_fault_status = 0

    async def _post_fault(sess: aiohttp.ClientSession) -> bool:
        nonlocal last_fault_status
        for attempt in range(2):
            try:
                async with sess.post(
                    f"{HORDE_API_BASE}/generate/submit",
                    json=fault_body,
                    headers=headers,
                    timeout=aiohttp.ClientTimeout(total=8.0),
                ) as resp:
                    last_fault_status = resp.status
                    txt = await resp.text()
                    if resp.status in (200, 201, 202):
                        return True
                    if resp.status in (400, 404, 409) and any(
                        m in txt.lower() for m in ("aborted", "not found", "already", "duplicate")
                    ):
                        return True
                    if resp.status in (408, 425, 429, 500, 502, 503, 504) and attempt == 0:
                        await asyncio.sleep(0.5)
                        continue
                    return False
            except Exception as e:
                logger.warning(f"[HordeDreamer] Fault submit attempt {attempt + 1} error for {job_id}: {e}")
                if attempt == 0:
                    await asyncio.sleep(0.5)
        return False

    if session is not None and not session.closed:
        fault_acked = await _post_fault(session)
    if not fault_acked and last_fault_status not in (400, 401, 403, 422):
        try:
            async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=8.0)) as fallback_session:
                fault_acked = await _post_fault(fallback_session)
        except Exception as e:
            logger.warning(f"[HordeDreamer] Fallback fault notification failed for job {job_id}: {e}")

    def _record_fault(upd: dict[str, Any]) -> None:
        upd["total_jobs_faulted"] = int(upd.get("total_jobs_faulted", 0)) + 1
        upd.setdefault("recent_jobs", []).insert(0, {
            "id": job_id,
            "ts": int(time.time()),
            "model": job_model,
            "backend": backend_label,
            "resolution": f"{width}x{height}",
            "steps": steps,
            "duration_sec": duration_sec,
            "kudos": 0.0,
            "status": "faulted",
            "fault_acknowledged": fault_acked,
            "error": reason,
        })

    update_dreamer_data(_record_fault)
    _runtime_status["state"] = "error"
    _runtime_status["last_error"] = reason
    _runtime_status["current_job_id"] = None
    _runtime_status["current_job_stage"] = None
    _runtime_status["current_model"] = None
    _runtime_status["current_backend"] = None
    return {
        "status": "faulted",
        "job_id": job_id,
        "fault_acknowledged": fault_acked,
        "detail": reason,
    }


async def _submit_generation_with_retry(
    session: aiohttp.ClientSession,
    submit_body: dict[str, Any],
    headers: dict[str, str],
    *,
    max_attempts: int = 3,
    retry_delay: float = 1.0,
    had_prior_ambiguous: bool = False,
    request_timeout: float = 30.0,
) -> dict[str, Any]:
    """
    Submits a completed generation to `/v2/generate/submit` with bounded retry for transient failures
    (HTTP 408/425/429/5xx and connection/timeout errors) and explicit outcome classification:
    - `acknowledged`: HTTP 200/201/202
    - `reconciled`: duplicate completion response after a prior ambiguous/lost-response attempt
    - `aborted`: HTTP 404 or aborted/expired assignment
    - `auth_rejected`: HTTP 401/403 credential/ownership rejection
    - `duplicate_rejected` / `rejected`: permanent 4xx rejection
    - `unresolved`: transient failure or lost response where server state remains uncertain
    """
    job_id = str(submit_body.get("id") or "")
    last_error = ""
    last_status = 0
    had_ambiguous_attempt = bool(had_prior_ambiguous)
    eff_timeout = max(2.0, float(request_timeout))

    for attempt in range(max_attempts):
        try:
            async with session.post(
                f"{HORDE_API_BASE}/generate/submit",
                json=submit_body,
                headers=headers,
                timeout=aiohttp.ClientTimeout(total=eff_timeout),
            ) as sub_resp:
                last_status = sub_resp.status
                sub_txt = await sub_resp.text()

                if sub_resp.status in (200, 201, 202):
                    reward = 0.0
                    try:
                        sub_json = json.loads(sub_txt) if sub_txt.strip() else {}
                        reward = float(sub_json.get("reward") or 0.0)
                    except Exception:
                        reward = 0.0
                    return {
                        "acknowledged": True,
                        "reconciled": False,
                        "terminal": True,
                        "outcome": "acknowledged",
                        "reward": reward,
                        "status_code": sub_resp.status,
                        "had_ambiguous_attempt": had_ambiguous_attempt,
                        "error": "",
                    }

                lower_txt = sub_txt.lower()
                is_duplicate_msg = any(
                    marker in lower_txt
                    for marker in ("already submitted", "already completed", "duplicate")
                )
                if is_duplicate_msg and sub_resp.status in (400, 404, 409):
                    if had_ambiguous_attempt:
                        logger.info(f"[HordeDreamer] Reconciled duplicate submit for job {job_id} after prior ambiguous attempt.")
                        return {
                            "acknowledged": True,
                            "reconciled": True,
                            "terminal": True,
                            "outcome": "reconciled",
                            "reward": 0.0,
                            "status_code": sub_resp.status,
                            "had_ambiguous_attempt": True,
                            "error": "",
                        }
                    last_error = f"Submit duplicate rejected ({sub_resp.status}): {sub_txt[:180]}"
                    return {
                        "acknowledged": False,
                        "reconciled": False,
                        "terminal": True,
                        "outcome": "duplicate_rejected",
                        "reward": 0.0,
                        "status_code": sub_resp.status,
                        "had_ambiguous_attempt": False,
                        "error": last_error,
                    }

                if sub_resp.status in (401, 403):
                    last_error = f"Submit auth/ownership rejected ({sub_resp.status}): {sub_txt[:180]}"
                    logger.error(f"[HordeDreamer] {last_error} for job {job_id}")
                    return {
                        "acknowledged": False,
                        "reconciled": False,
                        "terminal": True,
                        "outcome": "auth_rejected",
                        "reward": 0.0,
                        "status_code": sub_resp.status,
                        "had_ambiguous_attempt": had_ambiguous_attempt,
                        "error": last_error,
                    }

                if sub_resp.status == 404 or any(
                    marker in lower_txt
                    for marker in ("aborted", "requestnotfound", "not found", "expired")
                ):
                    last_error = f"Submit aborted/missing job ({sub_resp.status}): {sub_txt[:180]}"
                    logger.warning(f"[HordeDreamer] {last_error} for job {job_id}")
                    return {
                        "acknowledged": False,
                        "reconciled": False,
                        "terminal": True,
                        "outcome": "aborted",
                        "reward": 0.0,
                        "status_code": sub_resp.status,
                        "had_ambiguous_attempt": had_ambiguous_attempt,
                        "error": last_error,
                    }

                last_error = f"Submit HTTP {sub_resp.status}: {sub_txt[:180]}"
                logger.error(f"[HordeDreamer] Submit attempt {attempt + 1}/{max_attempts} failed ({sub_resp.status}) for {job_id}: {sub_txt[:200]}")

                if sub_resp.status in (408, 425, 429, 500, 502, 503, 504):
                    had_ambiguous_attempt = True
                    if attempt + 1 < max_attempts:
                        await asyncio.sleep(retry_delay * (attempt + 1))
                        continue
                    return {
                        "acknowledged": False,
                        "reconciled": False,
                        "terminal": False,
                        "outcome": "unresolved",
                        "reward": 0.0,
                        "status_code": sub_resp.status,
                        "had_ambiguous_attempt": True,
                        "error": last_error,
                    }

                return {
                    "acknowledged": False,
                    "reconciled": False,
                    "terminal": True,
                    "outcome": "rejected",
                    "reward": 0.0,
                    "status_code": sub_resp.status,
                    "had_ambiguous_attempt": had_ambiguous_attempt,
                    "error": last_error,
                }
        except asyncio.CancelledError:
            raise
        except Exception as e:
            had_ambiguous_attempt = True
            last_error = f"Submit connection error: {e}"
            logger.warning(f"[HordeDreamer] Submit attempt {attempt + 1}/{max_attempts} network error for {job_id}: {e}")
            if attempt + 1 < max_attempts:
                await asyncio.sleep(retry_delay * (attempt + 1))
                continue
            return {
                "acknowledged": False,
                "reconciled": False,
                "terminal": False,
                "outcome": "unresolved",
                "reward": 0.0,
                "status_code": last_status,
                "had_ambiguous_attempt": True,
                "error": last_error,
            }

    return {
        "acknowledged": False,
        "reconciled": False,
        "terminal": not had_ambiguous_attempt,
        "outcome": "unresolved" if had_ambiguous_attempt else "rejected",
        "reward": 0.0,
        "status_code": last_status,
        "had_ambiguous_attempt": had_ambiguous_attempt,
        "error": last_error or "Submit failed without server acknowledgement",
    }


async def _deliver_pending_entry(
    session: aiohttp.ClientSession,
    entry: dict[str, Any],
    headers: dict[str, str],
    *,
    max_attempts: int = 3,
    retry_delay: float = 1.0,
) -> dict[str, Any]:
    """
    Executes R2 upload (if not already uploaded) and `/v2/generate/submit` for a tracked `_pending_deliveries`
    entry, ensuring unchanged images are never re-uploaded on submit retries, acknowledged/reconciled jobs
    are counted exactly once, terminal rejections/aborts are removed cleanly, and transient unresolved
    submissions remain in `_pending_deliveries` until bounded lease expiry.
    """
    job_id = str(entry["job_id"])
    webp_bytes: bytes = entry["webp_bytes"]
    r2_url = str(entry.get("r2_url") or "")
    rem_lease = max(4.0, float(entry.get("expires_mono", time.monotonic() + 30.0)) - time.monotonic())

    if r2_url.startswith("http") and not entry.get("r2_uploaded", False):
        uploaded = await _upload_webp_to_r2(
            session,
            r2_url,
            webp_bytes,
            timeout=min(45.0, rem_lease),
        )
        if uploaded:
            entry["r2_uploaded"] = True
        else:
            logger.warning(f"[HordeDreamer] R2 upload failed for {job_id}, falling back to inline base64 submit")

    generation_field = "R2" if entry.get("r2_uploaded") else base64.b64encode(webp_bytes).decode("ascii")
    submit_body: dict[str, Any] = {
        "id": job_id,
        "generation": generation_field,
        "state": entry["submit_state"],
        "seed": entry["seed"],
        "censored": bool(entry["censored"]),
        "gen_metadata": entry["gen_metadata"],
    }
    entry["submit_body"] = submit_body
    entry["attempts"] = int(entry.get("attempts", 0)) + 1

    rem_submit = max(3.0, min(30.0, float(entry.get("expires_mono", time.monotonic() + 30.0)) - time.monotonic()))
    delivery = await _submit_generation_with_retry(
        session,
        submit_body,
        headers,
        max_attempts=max_attempts,
        retry_delay=retry_delay,
        had_prior_ambiguous=bool(entry.get("had_ambiguous_submit", False)),
        request_timeout=rem_submit,
    )
    outcome = str(delivery.get("outcome") or "rejected")

    if delivery.get("acknowledged"):
        _pending_deliveries.pop(job_id, None)
        entry["webp_bytes"] = b""
        if entry.get("completion_counted"):
            return {"status": "ok", "outcome": outcome, "job_id": job_id}
        entry["completion_counted"] = True
        reward = float(delivery.get("reward") or 0.0)
        width = int(entry["width"])
        height = int(entry["height"])
        steps = int(entry["steps"])
        render_sec = float(entry["render_sec"])
        job_model = str(entry["model"])
        backend_label = str(entry["backend"])
        submit_state = str(entry["submit_state"])
        is_censored = bool(entry["censored"])
        is_csam = bool(entry.get("is_csam"))
        is_krea_job = bool(entry.get("is_krea_job"))
        mps = round((width * height * steps) / 1_000_000.0, 2)

        job_record = {
            "id": job_id,
            "ts": int(time.time()),
            "model": job_model,
            "backend": backend_label,
            "resolution": f"{width}x{height}",
            "steps": steps,
            "duration_sec": render_sec,
            "kudos": round(reward, 2),
            "status": submit_state,
            "outcome": outcome,
            "censored": is_censored,
        }

        def _record_ack(upd: dict[str, Any]) -> None:
            upd["total_jobs_completed"] = int(upd.get("total_jobs_completed", 0)) + 1
            if is_krea_job:
                upd["krea_jobs_completed"] = int(upd.get("krea_jobs_completed", 0)) + 1
            else:
                upd["sdxl_jobs_completed"] = int(upd.get("sdxl_jobs_completed", 0)) + 1
            if is_csam:
                upd["total_csam_filtered"] = int(upd.get("total_csam_filtered", 0)) + 1
            elif is_censored:
                upd["total_nsfw_censored"] = int(upd.get("total_nsfw_censored", 0)) + 1
            upd["total_kudos_earned"] = round(float(upd.get("total_kudos_earned", 0.0)) + reward, 2)
            upd["total_megapixelsteps"] = round(float(upd.get("total_megapixelsteps", 0.0)) + mps, 2)
            upd.setdefault("recent_jobs", []).insert(0, job_record)

        update_dreamer_data(_record_ack)
        _runtime_status["last_submit_ts"] = time.time()
        _runtime_status["last_error"] = ""
        _runtime_status["current_job_id"] = None
        _runtime_status["current_job_stage"] = None
        _runtime_status["current_model"] = None
        _runtime_status["current_backend"] = None
        _runtime_status["state"] = "idle_polling"
        logger.info(
            f"[HordeDreamer] Acknowledged job {job_id} ({job_model}, state={submit_state}, outcome={outcome}, censored={is_censored}) in {render_sec}s -> +{reward:.1f} Kudos"
        )
        return {"status": "ok", "outcome": outcome, "job": job_record}

    err_msg = str(delivery.get("error") or "Submit rejected by AI Horde")
    if delivery.get("terminal", True):
        _pending_deliveries.pop(job_id, None)
        entry["webp_bytes"] = b""

        def _record_terminal_fail(upd: dict[str, Any]) -> None:
            if not entry.get("failure_counted"):
                upd["total_jobs_submit_failed"] = int(upd.get("total_jobs_submit_failed", 0)) + 1
                entry["failure_counted"] = True
            upd.setdefault("recent_jobs", []).insert(0, {
                "id": job_id,
                "ts": int(time.time()),
                "model": entry["model"],
                "backend": entry["backend"],
                "resolution": f"{entry['width']}x{entry['height']}",
                "steps": entry["steps"],
                "duration_sec": entry["render_sec"],
                "kudos": 0.0,
                "status": "submit_failed",
                "outcome": outcome,
                "censored": bool(entry["censored"]),
                "error": err_msg,
            })

        update_dreamer_data(_record_terminal_fail)
        _runtime_status["state"] = "error"
        _runtime_status["last_error"] = err_msg
        _runtime_status["current_job_id"] = None
        _runtime_status["current_job_stage"] = None
        _runtime_status["current_model"] = None
        _runtime_status["current_backend"] = None
        return {"status": "submit_failed", "outcome": outcome, "job_id": job_id, "detail": err_msg}

    # Unresolved transient failure: retain entry in `_pending_deliveries` for bounded reconciliation
    entry["had_ambiguous_submit"] = True
    entry["delivery_state"] = "unresolved"
    _pending_deliveries[job_id] = entry

    def _record_unresolved(upd: dict[str, Any]) -> None:
        if not entry.get("failure_counted"):
            upd["total_jobs_submit_failed"] = int(upd.get("total_jobs_submit_failed", 0)) + 1
            entry["failure_counted"] = True
        upd.setdefault("recent_jobs", []).insert(0, {
            "id": job_id,
            "ts": int(time.time()),
            "model": entry["model"],
            "backend": entry["backend"],
            "resolution": f"{entry['width']}x{entry['height']}",
            "steps": entry["steps"],
            "duration_sec": entry["render_sec"],
            "kudos": 0.0,
            "status": "submit_unresolved",
            "outcome": "unresolved",
            "censored": bool(entry["censored"]),
            "error": err_msg,
        })

    update_dreamer_data(_record_unresolved)
    _runtime_status["state"] = "error"
    _runtime_status["last_error"] = err_msg
    _runtime_status["current_job_id"] = None
    _runtime_status["current_job_stage"] = None
    _runtime_status["current_model"] = None
    _runtime_status["current_backend"] = None
    return {"status": "submit_unresolved", "outcome": "unresolved", "job_id": job_id, "detail": err_msg}


async def _reconcile_pending_deliveries(
    session: aiohttp.ClientSession,
    headers: dict[str, str],
) -> Optional[dict[str, Any]]:
    """
    Reconciles or expires any unresolved in-memory deliveries before popping new assignments.
    Ensures transient network/5xx submission outcomes are reconciled within their assignment lease
    and transient bytes are purged once resolved or expired.
    """
    if not _pending_deliveries:
        return None
    now_mono = time.monotonic()
    for jid in list(_pending_deliveries.keys()):
        entry = _pending_deliveries.get(jid)
        if not entry:
            continue
        if now_mono >= float(entry.get("expires_mono", 0.0)):
            _pending_deliveries.pop(jid, None)
            entry["webp_bytes"] = b""
            logger.warning(f"[HordeDreamer] Purged expired unresolved delivery for job {jid}")
            continue
        if entry.get("delivery_state") == "unresolved":
            return await _deliver_pending_entry(session, entry, headers, max_attempts=2, retry_delay=0.5)
    return None


def _parse_job_ttl_seconds(pop_data: dict[str, Any], gen_payload: dict[str, Any]) -> float:
    """Extracts and bounds the AI Horde pop-to-submit assignment lease (`ttl`) in seconds."""
    raw_ttl = pop_data.get("ttl")
    if raw_ttl is None and isinstance(gen_payload, dict):
        raw_ttl = gen_payload.get("ttl")
    if raw_ttl is None:
        return DEFAULT_JOB_TTL_SECONDS
    try:
        ttl_val = float(raw_ttl)
    except (TypeError, ValueError):
        return DEFAULT_JOB_TTL_SECONDS
    return max(MIN_JOB_TTL_SECONDS, min(MAX_JOB_TTL_SECONDS, ttl_val))


def _remaining_lease_seconds(lease_deadline: float, stage: str) -> float:
    """Returns remaining assignment lease time in seconds or raises `asyncio.TimeoutError` if expired."""
    rem = lease_deadline - time.monotonic()
    if rem <= 0.0:
        raise asyncio.TimeoutError(f"Assignment TTL lease expired before {stage}")
    return rem


async def run_single_dreamer_cycle(ignore_enabled: bool = False) -> dict[str, Any]:
    """
    Executes a single `reconcile -> pop -> validate -> render -> verify -> safety -> upload R2 -> submit`
    cycle protected by `_worker_lock`. Enforces the assignment `ttl` lease across all stages and
    guarantees every popped job reaches acknowledged completion, acknowledged fault, or bounded
    delivery reconciliation with zero user-image or prompt retention.
    """
    async with _worker_lock:
        data = load_dreamer_data()
        if not ignore_enabled and not data.get("enabled", True):
            _runtime_status["state"] = "paused"
            return {"status": "paused", "detail": "Worker is paused"}

        api_key = get_effective_api_key(data)
        if not api_key:
            _runtime_status["state"] = "error"
            _runtime_status["last_error"] = "Missing HORDE_WORKER_API_KEY"
            return {"status": "error", "detail": "Missing HORDE_WORKER_API_KEY"}

        headers = {
            "Content-Type": "application/json",
            "apikey": api_key,
            "Client-Agent": BRIDGE_AGENT,
        }

        if _pending_deliveries:
            try:
                async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=45.0)) as rec_session:
                    rec_res = await _reconcile_pending_deliveries(rec_session, headers)
                    if rec_res is not None:
                        return rec_res
            except asyncio.CancelledError:
                raise
            except Exception as rec_err:
                logger.warning(f"[HordeDreamer] Pending delivery reconciliation error: {rec_err}")

        models_to_offer = get_active_models_for_pop(data)
        _runtime_status["active_models_offered"] = models_to_offer
        if not models_to_offer:
            _runtime_status["state"] = "paused"
            return {"status": "no_models", "detail": "No active models available to offer"}

        # Check local ComfyUI idle status and verify connected renderer resolves offered checkpoints before pop
        has_sdxl_offer = any(m != KREA_HORDE_MODEL_NAME for m in models_to_offer)
        if has_sdxl_offer:
            if _comfy_reservation_lock.locked() or not await is_local_comfy_idle():
                _runtime_status["state"] = "waiting_local_busy"
                return {"status": "local_busy", "detail": "Local ComfyUI or Autopilot is currently busy"}
            renderer_ready_models: list[str] = []
            for m_offer in models_to_offer:
                cat_spec = LOCAL_SDXL_MODEL_CATALOG.get(m_offer)
                if cat_spec and await verify_comfy_checkpoint_registered(str(cat_spec["ckpt"])):
                    renderer_ready_models.append(m_offer)
            models_to_offer = renderer_ready_models
            _runtime_status["active_models_offered"] = models_to_offer
            if not models_to_offer:
                _runtime_status["state"] = "error"
                _runtime_status["last_error"] = "Connected ComfyUI renderer does not resolve any verified checkpoints"
                return {"status": "no_models", "detail": _runtime_status["last_error"]}

        # Fail-closed safety requirement: never pop jobs if horde_safety (NSFW/CSAM scanner) is unavailable
        try:
            await asyncio.to_thread(_ensure_safety_checker)
        except Exception as safety_err:
            _runtime_status["state"] = "error"
            _runtime_status["last_error"] = f"horde_safety initialization failed: {safety_err}"
            logger.error(f"[HordeDreamer] Refusing to pop jobs without horde_safety: {safety_err}")
            return {"status": "safety_unavailable", "detail": _runtime_status["last_error"]}

        worker_name = str(data.get("worker_name") or "Direct-Dreamer").strip()[:100]
        max_pixels = int(data.get("max_pixels") or (1024 * 1024))
        nsfw_flag = bool(data.get("nsfw", True))

        pop_payload = {
            "name": worker_name,
            "models": models_to_offer,
            "max_pixels": max_pixels,
            "nsfw": nsfw_flag,
            "bridge_agent": BRIDGE_AGENT,
            "threads": 1,
            "require_upfront_kudos": False,
            "amount": 1,
            "allow_img2img": False,
            "allow_painting": False,
            "allow_unsafe_ipaddr": True,
            "allow_post_processing": False,
            "allow_controlnet": False,
            "allow_extended_controlnet": False,
            "allow_sdxl_controlnet": False,
            "allow_lora": False,
            "limit_max_steps": False,
        }

        _runtime_status["state"] = "idle_polling"
        _runtime_status["last_pop_ts"] = time.time()

        try:
            async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=60.0)) as session:
                async with session.post(f"{HORDE_API_BASE}/generate/pop", json=pop_payload, headers=headers) as pop_resp:
                    if pop_resp.status != 200:
                        err_text = await pop_resp.text()
                        _runtime_status["state"] = "error"
                        if "UnsafeIP" in err_text:
                            _runtime_status["last_error"] = (
                                "UnsafeIP (403): AI Horde flagged ISP IP as VPN. "
                                "Ask a mod in AI Horde Discord (#horde-support) to enable the 'vpn' flag on ShinyLark#605378."
                            )
                            logger.warning(f"[HordeDreamer] {_runtime_status['last_error']}")
                            return {"status": "unsafe_ip", "detail": _runtime_status["last_error"]}
                        if "Maintenance mode" in err_text or "WorkerMaintenance" in err_text:
                            update_dreamer_data(lambda d: d.__setitem__("enabled", False))
                            _runtime_status["state"] = "paused"
                            _runtime_status["last_error"] = (
                                "Worker placed in Maintenance Mode on AI Horde. "
                                "Worker has been automatically paused; resolve any issues before disabling maintenance mode."
                            )
                            logger.warning(f"[HordeDreamer] {_runtime_status['last_error']}")
                            return {"status": "maintenance_paused", "detail": _runtime_status["last_error"]}
                        _runtime_status["last_error"] = f"Pop HTTP {pop_resp.status}: {err_text[:180]}"
                        logger.warning(f"[HordeDreamer] Pop failed ({pop_resp.status}): {err_text[:200]}")
                        return {"status": "error", "detail": _runtime_status["last_error"]}

                    pop_data = await pop_resp.json()

                if not isinstance(pop_data, dict):
                    _runtime_status["state"] = "error"
                    _runtime_status["last_error"] = "Malformed non-dict pop response"
                    return {"status": "error", "detail": _runtime_status["last_error"]}

                ids = pop_data.get("ids") or ([pop_data["id"]] if pop_data.get("id") else [])
                if not ids:
                    skipped = pop_data.get("skipped") or {}
                    _runtime_status["last_skipped"] = skipped
                    _runtime_status["last_error"] = ""
                    return {"status": "empty_queue", "skipped": skipped, "models_offered": models_to_offer}

                job_id = str(ids[0]).strip()
                if not job_id:
                    _runtime_status["state"] = "error"
                    _runtime_status["last_error"] = "Pop response contained empty job ID"
                    return {"status": "error", "detail": _runtime_status["last_error"]}

                raw_model = pop_data.get("model")
                raw_payload = pop_data.get("payload")
                r2_uploads = pop_data.get("r2_uploads") or ([pop_data["r2_upload"]] if pop_data.get("r2_upload") else [])
                r2_url = str(r2_uploads[0]) if r2_uploads else ""
                job_model = str(raw_model).strip() if isinstance(raw_model, str) else ""
                is_krea_job = (job_model == KREA_HORDE_MODEL_NAME)
                backend_label = "Modal Krea 2" if is_krea_job else "Local SDXL"

                job_stage = "popped"
                seed_int = 0
                width = 512
                height = 512
                steps = 30
                t0 = time.time()
                ttl_seconds = _parse_job_ttl_seconds(pop_data, raw_payload if isinstance(raw_payload, dict) else {})
                lease_deadline = time.monotonic() + ttl_seconds
                local_artifacts: list[str] = []
                remote_artifacts: list[str] = []

                _runtime_status["state"] = "rendering"
                _runtime_status["current_job_id"] = job_id
                _runtime_status["current_job_stage"] = job_stage
                _runtime_status["current_model"] = job_model or "invalid"
                _runtime_status["current_backend"] = backend_label

                try:
                    if len(ids) != 1:
                        raise ValueError(f"Expected single job ID for amount=1, received {len(ids)} IDs")
                    if not job_model:
                        raise ValueError("Missing or malformed model identity in pop response")
                    if not isinstance(raw_payload, dict) or not raw_payload:
                        raise ValueError("Missing or malformed payload in pop response")

                    gen_payload = raw_payload
                    raw_prompt = str(gen_payload.get("prompt") or "")
                    pos_prompt, neg_prompt = _split_horde_prompt(raw_prompt)

                    validated = _validate_supported_job_payload(gen_payload, max_pixels=max_pixels)
                    width = validated["width"]
                    height = validated["height"]
                    steps = validated["steps"]
                    cfg_scale = validated["cfg_scale"]
                    clip_skip = validated["clip_skip"]
                    denoise = validated["denoise"]
                    sampler_name = validated["sampler_name"]
                    karras = validated["karras"]
                    scheduler = validated["scheduler"]
                    seed_int = validated["seed"]

                    job_stage = "rendering"
                    _runtime_status["current_job_stage"] = job_stage

                    logger.info(
                        f"[HordeDreamer] Popped job {job_id} | model='{job_model}' ({backend_label}) | "
                        f"{width}x{height} | steps={steps} | sampler={sampler_name} | cfg={cfg_scale} | "
                        f"denoise={denoise} | clip_skip={clip_skip} | seed={seed_int} | ttl={ttl_seconds:.0f}s"
                    )

                    png_bytes: Optional[bytes] = None
                    render_error_reason = f"Render failed on {backend_label} for job {job_id}"

                    if is_krea_job:
                        render_error_reason = (
                            f"Refusing Krea job {job_id}: Krea2-Turbo_fp8 is disabled until model-reference "
                            f"registration and remote asset/cleanup verification are satisfied"
                        )
                        logger.error(f"[HordeDreamer] {render_error_reason}")
                    else:
                        cat_entry = LOCAL_SDXL_MODEL_CATALOG.get(job_model)
                        if not cat_entry or not _verify_reference_checkpoint(
                            cat_entry.get("ckpt", ""),
                            cat_entry.get("sha256"),
                            cat_entry.get("size_bytes"),
                        ):
                            render_error_reason = (
                                f"Refusing job {job_id}: unexpected or unverified model '{job_model}' "
                                f"(zero cross-model fallback)"
                            )
                            logger.error(f"[HordeDreamer] {render_error_reason}")
                        else:
                            ckpt_name = str(cat_entry["ckpt"])
                            if not await verify_comfy_checkpoint_registered(ckpt_name, session=session):
                                render_error_reason = (
                                    f"Refusing job {job_id}: connected ComfyUI renderer does not list verified checkpoint '{ckpt_name}'"
                                )
                                logger.error(f"[HordeDreamer] {render_error_reason}")
                            else:
                                wf = _build_sdxl_horde_workflow(
                                    positive_prompt=pos_prompt,
                                    negative_prompt=neg_prompt,
                                    ckpt_name=ckpt_name,
                                    width=width,
                                    height=height,
                                    seed=seed_int,
                                    steps=steps,
                                    cfg=cfg_scale,
                                    sampler_name=sampler_name,
                                    karras=karras,
                                    scheduler=scheduler,
                                    clip_skip=clip_skip,
                                    denoise=denoise,
                                    max_pixels=max_pixels,
                                )
                                rem_render = _remaining_lease_seconds(lease_deadline, "rendering")
                                async with _comfy_reservation_lock:
                                    if not await is_local_comfy_idle():
                                        render_error_reason = (
                                            f"Local ComfyUI became busy with interactive work before dispatching job {job_id}"
                                        )
                                        logger.info(f"[HordeDreamer] {render_error_reason}")
                                    else:
                                        png_bytes = await _run_local_comfy_silent(
                                            wf,
                                            timeout=min(180.0, rem_render),
                                            tracked_artifacts=local_artifacts,
                                        )

                    render_sec = round(time.time() - t0, 2)
                    _remove_renderer_artifacts(local_artifacts)

                    if not png_bytes:
                        for art in remote_artifacts:
                            await _cleanup_modal_artifact(art)
                        job_stage = "faulted"
                        return await _fault_popped_job(
                            session,
                            job_id,
                            seed_int,
                            headers,
                            reason=render_error_reason,
                            job_model=job_model or "unknown",
                            backend_label=backend_label,
                            width=width,
                            height=height,
                            steps=steps,
                            duration_sec=render_sec,
                        )

                    # Validate uncensored output image dimensions before safety replacement
                    _validate_rendered_image_bytes(png_bytes, width, height)

                    # Evaluate generated image safety (NSFW & CSAM scanning via horde_safety) within remaining lease
                    job_stage = "evaluating_safety"
                    _runtime_status["state"] = "evaluating_safety"
                    _runtime_status["current_job_stage"] = job_stage
                    censor_nsfw_req = bool(
                        gen_payload.get("use_nsfw_censor")
                        or gen_payload.get("censor_nsfw")
                        or pop_data.get("censor_nsfw")
                    )
                    rem_safety = _remaining_lease_seconds(lease_deadline, "safety evaluation")
                    safety_res = await asyncio.wait_for(
                        asyncio.to_thread(
                            _evaluate_image_safety,
                            png_bytes,
                            raw_prompt,
                            censor_nsfw_req,
                            not nsfw_flag,
                            {"name": job_model},
                        ),
                        timeout=rem_safety,
                    )
                    # Drop reference to uncensored raw PNG bytes immediately if replaced
                    png_bytes = b""

                    final_png_bytes = safety_res["final_png_bytes"]
                    submit_state = safety_res["submit_state"]
                    is_censored = bool(safety_res["censored"])
                    gen_metadata = safety_res["gen_metadata"]

                    _remove_renderer_artifacts(local_artifacts)
                    if remote_artifacts:
                        for art in remote_artifacts:
                            await _cleanup_modal_artifact(art)

                    if safety_res.get("is_csam"):
                        logger.warning(f"[HordeDreamer] CSAM detected by horde_safety in job {job_id}. Image replaced with CSAM censor placeholder.")
                    elif is_censored:
                        logger.info(f"[HordeDreamer] NSFW censored by horde_safety in job {job_id} (censor_nsfw={censor_nsfw_req}, sfw_worker={not nsfw_flag}).")

                    _remaining_lease_seconds(lease_deadline, "image conversion")
                    webp_bytes = _convert_png_to_webp(final_png_bytes, quality=92)
                    final_png_bytes = b""

                    # Transition to `awaiting_delivery`: from this point on we have a valid safety-checked
                    # result and must never fault an already accepted/pending-delivery job.
                    _remaining_lease_seconds(lease_deadline, "delivery")
                    job_stage = "awaiting_delivery"
                    _runtime_status["state"] = "awaiting_delivery"
                    _runtime_status["current_job_stage"] = job_stage

                    pending_entry: dict[str, Any] = {
                        "job_id": job_id,
                        "webp_bytes": webp_bytes,
                        "r2_url": r2_url,
                        "r2_uploaded": False,
                        "submit_state": submit_state,
                        "seed": seed_int,
                        "censored": is_censored,
                        "gen_metadata": gen_metadata,
                        "is_csam": bool(safety_res.get("is_csam")),
                        "is_krea_job": is_krea_job,
                        "model": job_model,
                        "backend": backend_label,
                        "width": width,
                        "height": height,
                        "steps": steps,
                        "render_sec": render_sec,
                        "created_mono": time.monotonic(),
                        "expires_mono": lease_deadline,
                        "had_ambiguous_submit": False,
                        "attempts": 0,
                        "delivery_state": "awaiting_delivery",
                    }
                    _pending_deliveries[job_id] = pending_entry

                    delivery_res = await _deliver_pending_entry(session, pending_entry, headers)
                    if delivery_res.get("status") == "ok":
                        job_stage = "acknowledged"
                    else:
                        job_stage = str(delivery_res.get("status") or "submit_failed")
                    return delivery_res

                except BaseException as job_exc:
                    _remove_renderer_artifacts(local_artifacts)
                    for art in remote_artifacts:
                        try:
                            await asyncio.shield(_cleanup_modal_artifact(art))
                        except Exception:
                            pass

                    if job_stage in ("popped", "rendering", "evaluating_safety"):
                        reason = f"{type(job_exc).__name__} during {job_stage} for job {job_id}: {job_exc}"
                        logger.error(f"[HordeDreamer] {reason}")
                        fault_res = await asyncio.shield(
                            _fault_popped_job(
                                session,
                                job_id,
                                seed_int,
                                headers,
                                reason=reason,
                                job_model=job_model or "unknown",
                                backend_label=backend_label,
                                width=width,
                                height=height,
                                steps=steps,
                                duration_sec=round(time.time() - t0, 2),
                            )
                        )
                        job_stage = "faulted"
                        if isinstance(job_exc, asyncio.CancelledError):
                            raise
                        return fault_res

                    # If `job_stage` is `awaiting_delivery` or `acknowledged`, never fault an already-rendered
                    # or already-accepted result; preserve unresolved entry for bounded reconciliation.
                    if job_id in _pending_deliveries and job_stage == "awaiting_delivery":
                        _pending_deliveries[job_id]["delivery_state"] = "unresolved"
                        _pending_deliveries[job_id]["had_ambiguous_submit"] = True
                    _runtime_status["current_job_id"] = None
                    _runtime_status["current_job_stage"] = None
                    _runtime_status["current_model"] = None
                    _runtime_status["current_backend"] = None
                    if job_stage != "acknowledged":
                        _runtime_status["state"] = "error"
                        _runtime_status["last_error"] = f"Delivery interrupted for job {job_id}: {job_exc}"
                    if isinstance(job_exc, asyncio.CancelledError):
                        raise
                    return {"status": "submit_unresolved", "job_id": job_id, "detail": str(job_exc)}

        except asyncio.CancelledError:
            raise
        except Exception as e:
            _runtime_status["state"] = "error"
            _runtime_status["last_error"] = str(e)
            _runtime_status["current_job_id"] = None
            _runtime_status["current_job_stage"] = None
            logger.error(f"[HordeDreamer] Cycle exception: {e}", exc_info=True)
            return {"status": "error", "detail": str(e)}


async def _dreamer_worker_loop() -> None:
    logger.info("[HordeDreamer] Background AI Horde Dreamer worker loop started.")
    await asyncio.sleep(3.0)

    while True:
        try:
            data = load_dreamer_data()
            if not data.get("enabled", True):
                _runtime_status["state"] = "paused"
                _wake_event.clear()
                try:
                    await asyncio.wait_for(_wake_event.wait(), timeout=15.0)
                except asyncio.TimeoutError:
                    pass
                continue

            res = await run_single_dreamer_cycle(ignore_enabled=False)
            st = res.get("status")

            if st == "ok":
                sleep_sec = float(data.get("poll_interval_busy") or 2.0)
            elif st == "local_busy":
                sleep_sec = 6.0
            elif st == "unsafe_ip":
                sleep_sec = 300.0
            elif st in ("error", "submit_failed", "faulted"):
                sleep_sec = 20.0
            else:
                sleep_sec = float(data.get("poll_interval_idle") or 10.0)

            _wake_event.clear()
            try:
                await asyncio.wait_for(_wake_event.wait(), timeout=max(1.5, sleep_sec))
            except asyncio.TimeoutError:
                pass
        except asyncio.CancelledError:
            logger.info("[HordeDreamer] Background worker loop cancelled.")
            break
        except Exception as e:
            logger.error(f"[HordeDreamer] Unexpected loop error: {e}", exc_info=True)
            await asyncio.sleep(15.0)


async def start_horde_dreamer() -> None:
    global _worker_task
    install_horde_client_hooks()
    if _worker_task is None or _worker_task.done():
        _worker_task = asyncio.create_task(_dreamer_worker_loop())


# ============================================================================
# AI Horde Image Generation Client (for Direct users in Horde mode)
# ============================================================================

async def cancel_horde_image_task(
    task_id: str,
    api_key: str = horde_client.ANONYMOUS_API_KEY,
    client_agent: str = CLIENT_AGENT,
) -> None:
    """Cancels an unfinished image generation request on AI Horde (`DELETE /api/v2/generate/status/{id}`)."""
    if not task_id:
        return
    try:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=5.0)) as session:
            async with session.delete(
                f"{HORDE_API_BASE}/generate/status/{task_id}",
                headers={
                    "apikey": api_key or horde_client.ANONYMOUS_API_KEY,
                    "Client-Agent": client_agent,
                },
            ) as resp:
                logger.info(f"[HordeDreamer] Cancelled abandoned image task {task_id}: status={resp.status}")
    except Exception as e:
        logger.warning(f"[HordeDreamer] Error cancelling image task {task_id}: {e}")


async def horde_generate_image(
    prompt: str,
    negative_prompt: str = "",
    width: int = 1024,
    height: int = 1024,
    steps: int = 25,
    cfg_scale: float = 7.0,
    seed: Optional[int] = None,
    model: str = "CyberRealistic Pony",
    api_key: str = "",
    max_wait_seconds: int = 180,
    poll_interval: float = 2.5,
) -> tuple[Optional[bytes], Optional[str], Optional[str]]:
    """
    Submits an asynchronous image generation request to AI Horde (`/api/v2/generate/async`),
    polls `/generate/check/{id}` and `/generate/status/{id}`, downloads the generated WebP,
    and converts it to PNG bytes for seamless display in AI Assistant Direct.
    Checks `done` before `is_possible=False` so completed results are always delivered, and
    cancels only unfinished requests on timeout, `is_possible=False`, cancellation, or error.
    Returns `(png_bytes, worker_name, actual_model)` or `(None, None, error_str)`.
    """
    eff_key = (api_key or "").strip() or get_effective_api_key() or horde_client.ANONYMOUS_API_KEY
    full_prompt = f"{prompt.strip()} {PROMPT_SEPARATOR} {negative_prompt.strip()}" if negative_prompt.strip() else prompt.strip()

    w_aligned = max(512, min(1024, (int(width or 1024) // 64) * 64))
    h_aligned = max(512, min(1024, (int(height or 1024) // 64) * 64))

    params: dict[str, Any] = {
        "sampler_name": "k_euler_a",
        "cfg_scale": float(cfg_scale or 7.0),
        "width": w_aligned,
        "height": h_aligned,
        "steps": min(30, max(15, int(steps or 25))),
        "karras": True,
        "n": 1,
    }
    if seed is not None and int(seed) >= 0:
        params["seed"] = str(int(seed))

    req_body = {
        "prompt": full_prompt,
        "params": params,
        "nsfw": True,
        "censor_nsfw": False,
        "trusted_workers": False,
        "slow_workers": True,
        "models": [model] if model else ["CyberRealistic Pony", "AlbedoBase XL (SDXL)"],
        "r2": True,
    }

    headers = {
        "Content-Type": "application/json",
        "apikey": eff_key,
        "Client-Agent": CLIENT_AGENT,
    }

    task_id: Optional[str] = None
    task_terminal = False

    try:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=max_wait_seconds)) as session:
            async with session.post(f"{HORDE_API_BASE}/generate/async", json=req_body, headers=headers) as resp:
                if resp.status not in (200, 202):
                    err = await resp.text()
                    return None, None, f"AI Horde image submit error ({resp.status}): {err[:140]}"
                res_json = await resp.json()
                task_id = res_json.get("id")

            if not task_id:
                return None, None, "AI Horde did not return an image task ID"

            elapsed = 0.0
            while elapsed < max_wait_seconds:
                await asyncio.sleep(poll_interval)
                elapsed += poll_interval

                async with session.get(f"{HORDE_API_BASE}/generate/check/{task_id}", headers={"Client-Agent": CLIENT_AGENT}) as c_resp:
                    if c_resp.status != 200:
                        continue
                    chk = await c_resp.json()

                if chk.get("done"):
                    async with session.get(f"{HORDE_API_BASE}/generate/status/{task_id}", headers={"Client-Agent": CLIENT_AGENT}) as s_resp:
                        if s_resp.status != 200:
                            break
                        st = await s_resp.json()
                    task_terminal = True
                    gens = st.get("generations") or []
                    if not gens:
                        return None, None, "AI Horde returned empty generations list"
                    gen0 = gens[0]
                    img_ref = str(gen0.get("img") or "")
                    worker_name = str(gen0.get("worker_name") or "AI Horde")
                    used_model = str(gen0.get("model") or model)

                    raw_bytes: Optional[bytes] = None
                    if img_ref.startswith("http"):
                        async with session.get(img_ref) as dl_resp:
                            if dl_resp.status == 200:
                                raw_bytes = await dl_resp.read()
                    elif img_ref:
                        raw_bytes = base64.b64decode(img_ref)

                    if not raw_bytes:
                        return None, None, "Failed to download generated image from AI Horde"

                    # Convert WebP to PNG for Direct storage & pipeline compatibility
                    with Image.open(io.BytesIO(raw_bytes)) as im:
                        out_png = io.BytesIO()
                        im.save(out_png, format="PNG")
                        return out_png.getvalue(), worker_name, used_model

                if chk.get("faulted"):
                    task_terminal = True
                    return None, None, "AI Horde worker faulted while generating image"
                if chk.get("is_possible") is False:
                    return None, None, "No AI Horde image workers available for the selected model"

            return None, None, f"Timed out waiting for AI Horde image ({max_wait_seconds}s)"
    except asyncio.CancelledError:
        logger.info(f"[HordeDreamer] horde_generate_image cancelled, aborting task {task_id}")
        raise
    except Exception as e:
        logger.error(f"[HordeDreamer] horde_generate_image error: {e}")
        return None, None, str(e)
    finally:
        if task_id and not task_terminal:
            try:
                await asyncio.shield(cancel_horde_image_task(task_id, eff_key, CLIENT_AGENT))
            except Exception as cancel_err:
                logger.warning(f"[HordeDreamer] Failed to cancel abandoned image task {task_id}: {cancel_err}")


def _is_horde_image_provider_selected(ctx: Any) -> bool:
    """Returns True when `ctx.settings` routes image generation through AI Horde."""
    settings = getattr(ctx, "settings", None)
    if not isinstance(settings, dict):
        return False
    for key in ("image_provider", "ai_provider", "ai_mode"):
        val = str(settings.get(key) or "").strip().lower()
        if val in ("horde", "aihorde", "ai_horde"):
            return True
    return False


def install_horde_client_hooks() -> None:
    """
    Attach `horde_generate_image` onto `horde_client`, normalize `ctx.settings['ai_mode']`
    to `ctx.settings['ai_provider']` in `core_service.perform_prompt_stream`, and route
    `core_service.generate_image` (Creative Studio / chat image generation) through AI Horde
    when `ctx.settings` selects the `horde` provider.
    """
    setattr(horde_client, "horde_generate_image", horde_generate_image)
    setattr(horde_client, "DEFAULT_CLIENT_AGENT", CLIENT_AGENT)
    try:
        import core_service as _cs
        orig_stream = getattr(_cs, "perform_prompt_stream", None)
        if callable(orig_stream) and not getattr(orig_stream, "_horde_hooked", False):
            async def _hooked_perform_prompt_stream(ctx, *args, **kwargs):
                if ctx is not None and isinstance(getattr(ctx, "settings", None), dict):
                    if not ctx.settings.get("ai_provider") and ctx.settings.get("ai_mode"):
                        ctx.settings["ai_provider"] = ctx.settings["ai_mode"]
                async for item in orig_stream(ctx, *args, **kwargs):
                    yield item
            _hooked_perform_prompt_stream._horde_hooked = True
            _cs.perform_prompt_stream = _hooked_perform_prompt_stream

        orig_gen_img = getattr(_cs, "generate_image", None)
        if callable(orig_gen_img) and not getattr(orig_gen_img, "_horde_hooked", False):
            async def _hooked_generate_image(ctx, prompt, *args, **kwargs):
                if not _is_horde_image_provider_selected(ctx):
                    return await orig_gen_img(ctx, prompt, *args, **kwargs)
                if not prompt or not str(prompt).strip():
                    raise ValueError("Please explain what do you want to see.")
                settings = getattr(ctx, "settings", None) or {}
                req_model = str(settings.get("horde_image_model") or "CyberRealistic Pony").strip()
                user_key = str(settings.get("horde_api_key") or "").strip()
                png_bytes, worker_name, used_model_or_err = await horde_generate_image(
                    prompt=str(prompt),
                    model=req_model,
                    api_key=user_key,
                )
                if not png_bytes:
                    raise RuntimeError(used_model_or_err or "AI Horde image generation failed")
                ts_str = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d_%H%M%S")
                fname = f"IMG_HORDE_{ts_str}.png"
                title = f"AI Horde ({used_model_or_err or req_model})"
                desc = f"Generated via AI Horde worker {worker_name or 'AI Horde'} using {used_model_or_err or req_model}"
                return fname, title, desc, png_bytes

            _hooked_generate_image._horde_hooked = True
            _cs.generate_image = _hooked_generate_image
    except Exception as e:
        logger.warning(f"[HordeDreamer] Could not install core_service Horde hooks: {e}")


# ============================================================================
# Owner Auth & HTTP Console / API Routes
# ============================================================================

_LOOPBACK_HOSTS: frozenset[str] = frozenset({"127.0.0.1", "::1", "localhost"})
_NON_OWNER_ROLES: frozenset[str] = frozenset({"guest", "shared", "anonymous", "user", "public", "viewer"})


def _is_direct_loopback_peer(request: Request) -> bool:
    """Returns True only when the immediate socket peer is loopback."""
    client = getattr(request, "client", None)
    client_host = (getattr(client, "host", "") if client is not None else "") or ""
    return client_host.strip().lower() in _LOOPBACK_HOSTS


def _has_proxy_forwarding_headers(request: Request) -> bool:
    """Returns True if any reverse-proxy / tunnel forwarding header is present on the request."""
    headers = getattr(request, "headers", None) or {}
    for hdr in ("x-forwarded-for", "x-real-ip", "forwarded", "x-forwarded-host", "via"):
        if (headers.get(hdr) or "").strip():
            return True
    return False


def _has_verified_owner_credential(request: Request) -> bool:
    """
    Checks whether a proxied/header-bearing loopback request carries a server-verified owner
    credential (such as `request.state.trusted_owner`, a configured `HORDE_OWNER_SECRET`, or a
    cryptographically paired node DID verified by `api.is_did_paired`). Client-asserted headers
    like `X-OMD-Caller-Role: owner` or `X-Forwarded-For: 127.0.0.1` are never trusted on their own.
    """
    state = getattr(request, "state", None)
    if state is not None and getattr(state, "trusted_owner", None) is True:
        return True

    headers = getattr(request, "headers", None) or {}
    configured_secret = str(SETTINGS.get("HORDE_OWNER_SECRET") or "").strip()
    if configured_secret:
        provided_secret = (headers.get("x-horde-owner-secret") or "").strip()
        if provided_secret and provided_secret == configured_secret:
            return True

    caller_did = (headers.get("x-omd-caller-did") or headers.get("x-omd-did") or "").strip()
    if caller_did:
        try:
            import api as _api_mod
            if hasattr(_api_mod, "is_did_paired") and callable(_api_mod.is_did_paired):
                if _api_mod.is_did_paired(caller_did):
                    return True
        except Exception:
            pass
    return False


def _check_owner(request: Request) -> bool:
    """
    Enforces a strict verified trust boundary for the `/horde` management console and `/horde/api/*`:
    1. Immediately rejects any explicit guest, shared, or non-owner role assertion in headers or state.
    2. If an authoritative host owner guard (`extensions.autopilot._check_owner_request`) is installed,
       its `False` verdict is final and never falls through to weaker loopback or header checks.
    3. Requires the physical socket peer to be loopback (unless explicitly verified by `request.state.trusted_owner`).
    4. Rejects proxied requests (`X-Forwarded-For`, `X-Real-IP`, `Forwarded`) and client-asserted
       `X-OMD-Caller-Role: owner` headers unless backed by a server-verified credential (paired DID,
       `HORDE_OWNER_SECRET`, or `request.state.trusted_owner`).
    """
    headers = getattr(request, "headers", None) or {}
    state = getattr(request, "state", None)

    role_hdr = (headers.get("x-omd-caller-role") or "").strip().lower()
    if role_hdr in _NON_OWNER_ROLES:
        return False

    if state is not None:
        if getattr(state, "is_guest", False) is True:
            return False
        state_role = str(getattr(state, "role", "") or "").strip().lower()
        if state_role in _NON_OWNER_ROLES:
            return False
        if getattr(state, "trusted_owner", None) is False:
            return False

    # Check authoritative host guard when present: a `False` verdict is final (no fallthrough).
    try:
        from extensions.autopilot import _check_owner_request
    except ImportError:
        _check_owner_request = None  # type: ignore
    except Exception:
        return False

    if callable(_check_owner_request):
        try:
            if not _check_owner_request(request):
                return False
        except Exception:
            return False

    if state is not None and getattr(state, "trusted_owner", None) is True:
        return True

    if not _is_direct_loopback_peer(request):
        return False

    # If the loopback connection came through a local proxy/tunnel or carries a caller-role assertion,
    # require a server-verified owner credential rather than trusting proxy IPs or header claims.
    if _has_proxy_forwarding_headers(request) or role_hdr:
        return _has_verified_owner_credential(request)

    return True


def _require_owner(request: Request) -> None:
    if not _check_owner(request):
        raise HTTPException(status_code=404, detail="Not Found")


router = APIRouter()


async def _build_state_payload(refresh_user: bool = False) -> dict[str, Any]:
    data = load_dreamer_data()
    uinfo = await fetch_horde_user_info(data.get("api_key"), force=refresh_user)
    raw_key = get_effective_api_key(data)
    masked_key = (raw_key[:4] + "••••" + raw_key[-4:]) if len(raw_key) > 8 else ("••••" if raw_key else "")
    raw_models = data.get("sdxl_models")
    configured_models = list(raw_models) if isinstance(raw_models, list) else ["CyberRealistic Pony"]

    return {
        "config": {
            "enabled": bool(data.get("enabled", False)),
            "worker_name": data.get("worker_name", "Direct-Dreamer"),
            "has_api_key": bool(raw_key),
            "masked_api_key": masked_key,
            "nsfw": bool(data.get("nsfw", False)),
            "max_pixels": int(data.get("max_pixels", 1024 * 1024)),
            "poll_interval_idle": float(data.get("poll_interval_idle", 10.0)),
            "poll_interval_busy": float(data.get("poll_interval_busy", 2.0)),
            "sdxl_enabled": bool(data.get("sdxl_enabled", True)),
            "sdxl_models": configured_models,
            "sdxl_catalog": {
                m_name: {**spec, "verified": _verify_reference_checkpoint(spec)}
                for m_name, spec in LOCAL_SDXL_MODEL_CATALOG.items()
            },
            "krea_enabled": False,
            "krea_verified": False,
            "krea_daily_limit": int(data.get("krea_daily_limit", 10)),
            "krea_daily_date": data.get("krea_daily_date", _utc_today_str()),
            "krea_daily_count": int(data.get("krea_daily_count", 0)),
            "modal_available": is_modal_enabled(),
        },
        "stats": {
            "total_jobs_completed": int(data.get("total_jobs_completed", 0)),
            "sdxl_jobs_completed": int(data.get("sdxl_jobs_completed", 0)),
            "krea_jobs_completed": int(data.get("krea_jobs_completed", 0)),
            "total_jobs_faulted": int(data.get("total_jobs_faulted", 0)),
            "total_jobs_submit_failed": int(data.get("total_jobs_submit_failed", 0)),
            "total_csam_filtered": int(data.get("total_csam_filtered", 0)),
            "total_nsfw_censored": int(data.get("total_nsfw_censored", 0)),
            "total_kudos_earned": round(float(data.get("total_kudos_earned", 0.0)), 2),
            "total_megapixelsteps": round(float(data.get("total_megapixelsteps", 0.0)), 2),
            "pending_deliveries_count": len(_pending_deliveries),
            "recent_jobs": _sanitize_recent_jobs(data.get("recent_jobs"))[:30],
        },
        "runtime": {
            "state": _runtime_status.get("state", "stopped"),
            "current_job_id": _runtime_status.get("current_job_id"),
            "current_job_stage": _runtime_status.get("current_job_stage"),
            "current_model": _runtime_status.get("current_model"),
            "current_backend": _runtime_status.get("current_backend"),
            "last_pop_ts": _runtime_status.get("last_pop_ts", 0.0),
            "last_submit_ts": _runtime_status.get("last_submit_ts", 0.0),
            "last_error": _runtime_status.get("last_error", ""),
            "last_skipped": _runtime_status.get("last_skipped") or {},
            "active_models_offered": get_active_models_for_pop(data),
            "bridge_declared_samplers": list(BRIDGE_DECLARED_HORDE_SAMPLERS),
            "supported_samplers": list(SUPPORTED_HORDE_SAMPLERS),
            "supported_schedulers": sorted(COMFY_SUPPORTED_SCHEDULERS),
            "bridge_agent": BRIDGE_AGENT,
            "safety_available": bool(_runtime_status.get("safety_available", HORDE_SAFETY_AVAILABLE)),
            "safety_loaded": bool(_runtime_status.get("safety_loaded", False)),
        },
        "account": {
            "username": (uinfo or {}).get("username", ""),
            "id": (uinfo or {}).get("id"),
            "kudos": (uinfo or {}).get("kudos", 0.0),
            "trusted": (uinfo or {}).get("trusted", False),
            "worker_count": (uinfo or {}).get("worker_count", 0),
            "worker_ids": (uinfo or {}).get("worker_ids") or [],
            "records": (uinfo or {}).get("records") or {},
        } if uinfo else None,
    }


@router.get("/horde", response_class=HTMLResponse)
async def horde_console_page(request: Request):
    _require_owner(request)
    return HTMLResponse(content=HORDE_CONSOLE_HTML)


@router.get("/horde/api/state")
@router.get("/assistant/horde_worker")
async def horde_get_state(request: Request, refresh: bool = False):
    _require_owner(request)
    return JSONResponse(await _build_state_payload(refresh_user=refresh))


@router.post("/horde/api/config")
@router.post("/assistant/horde_worker")
async def horde_update_config(request: Request):
    _require_owner(request)
    body = await request.json()
    if not isinstance(body, dict):
        raise HTTPException(status_code=400, detail="Invalid JSON object")

    def _apply_config(data: dict[str, Any]) -> None:
        if "enabled" in body:
            data["enabled"] = bool(body["enabled"])
        if "worker_name" in body and str(body["worker_name"]).strip():
            data["worker_name"] = str(body["worker_name"]).strip()[:100]
        if "api_key" in body:
            new_key = str(body["api_key"]).strip()
            if new_key and "••••" not in new_key:
                data["api_key"] = new_key
                _runtime_status["user_info"] = None
        if "nsfw" in body:
            data["nsfw"] = bool(body["nsfw"])
        if "max_pixels" in body:
            try:
                data["max_pixels"] = max(262144, min(2097152, int(body["max_pixels"])))
            except Exception:
                pass
        if "sdxl_enabled" in body:
            data["sdxl_enabled"] = bool(body["sdxl_enabled"])
        if "sdxl_models" in body:
            if not isinstance(body["sdxl_models"], list):
                raise HTTPException(status_code=400, detail="sdxl_models must be a list")
            data["sdxl_models"] = [
                str(m).strip()
                for m in body["sdxl_models"]
                if isinstance(m, str) and str(m).strip() in LOCAL_SDXL_MODEL_CATALOG
            ]
        # Krea remains hard-disabled until reference registration & remote verification exist
        data["krea_enabled"] = False
        if "krea_daily_limit" in body:
            try:
                data["krea_daily_limit"] = max(-1, min(1000, int(body["krea_daily_limit"])))
            except Exception:
                pass

    update_dreamer_data(_apply_config)
    _wake_event.set()
    return JSONResponse(await _build_state_payload(refresh_user=True))


@router.post("/horde/api/toggle")
async def horde_toggle_worker(request: Request):
    _require_owner(request)
    update_dreamer_data(lambda d: d.__setitem__("enabled", not bool(d.get("enabled", False))))
    _wake_event.set()
    return JSONResponse(await _build_state_payload(refresh_user=False))


@router.post("/horde/api/step")
async def horde_trigger_single_step(request: Request):
    _require_owner(request)
    res = await run_single_dreamer_cycle(ignore_enabled=True)
    state = await _build_state_payload(refresh_user=True)
    return JSONResponse({"step_result": res, "state": state})


@router.post("/horde/api/reset_krea_quota")
async def horde_reset_krea_quota(request: Request):
    _require_owner(request)

    def _reset(d: dict[str, Any]) -> None:
        d["krea_daily_date"] = _utc_today_str()
        d["krea_daily_count"] = 0

    update_dreamer_data(_reset)
    _wake_event.set()
    return JSONResponse(await _build_state_payload(refresh_user=False))


@router.post("/horde/api/test_local_sdxl")
async def horde_test_local_sdxl(request: Request):
    _require_owner(request)
    verified_spec = next(
        (spec for spec in LOCAL_SDXL_MODEL_CATALOG.values() if _verify_reference_checkpoint(spec)),
        None,
    )
    if not verified_spec:
        return JSONResponse(
            {"ok": False, "error": "No reference-verified SDXL checkpoint installed in ComfyUI/models/checkpoints"},
            status_code=400,
        )
    t0 = time.time()
    wf = _build_sdxl_horde_workflow(
        positive_prompt="cinematic portrait of a cyberpunk hacker girl in neon rain, masterpiece",
        negative_prompt="blurry, low quality",
        ckpt_name=verified_spec["ckpt"],
        width=896,
        height=1152,
        seed=random.randint(1, 2147483646),
    )
    png_bytes = await _run_local_comfy_silent(wf)
    dt = round(time.time() - t0, 2)
    if not png_bytes:
        return JSONResponse({"ok": False, "error": "Local ComfyUI render failed"}, status_code=500)
    webp_bytes = _convert_png_to_webp(png_bytes)
    return JSONResponse({
        "ok": True,
        "duration_sec": dt,
        "png_bytes": len(png_bytes),
        "webp_bytes": len(webp_bytes),
    })


def mount_horde_dreamer(app) -> None:
    install_horde_client_hooks()
    app.include_router(router)


HORDE_CONSOLE_HTML = """<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AI Horde Dreamer Worker</title>
<style>
  :root {
    color-scheme: dark;
    --bg: #0d1117;
    --card: #161b22;
    --border: #30363d;
    --text: #e6edf3;
    --muted: #8b949e;
    --accent: #58a6ff;
    --green: #3fb950;
    --yellow: #d29922;
    --red: #f85149;
    --purple: #a371f7;
    --scrollbar-thumb: #3b4452;
    --scrollbar-thumb-hover: #525f72;
    --scrollbar-track: transparent;
  }
  html {
    background: var(--bg);
    color-scheme: dark;
    scrollbar-width: thin;
    scrollbar-color: var(--scrollbar-thumb) var(--bg);
  }
  * {
    box-sizing: border-box;
    scrollbar-width: thin;
    scrollbar-color: var(--scrollbar-thumb) var(--scrollbar-track);
  }
  *::-webkit-scrollbar {
    width: 8px;
    height: 8px;
  }
  *::-webkit-scrollbar-track {
    background: var(--scrollbar-track);
  }
  html::-webkit-scrollbar-track,
  body::-webkit-scrollbar-track {
    background: var(--bg);
  }
  *::-webkit-scrollbar-thumb {
    background: var(--scrollbar-thumb);
    border-radius: 4px;
  }
  *::-webkit-scrollbar-thumb:hover {
    background: var(--scrollbar-thumb-hover);
  }
  *::-webkit-scrollbar-corner {
    background: transparent !important;
  }
  body {
    margin: 0; padding: 24px;
    background: var(--bg); color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  }
  .wrap { max-width: 1080px; margin: 0 auto; }
  header {
    display: flex; align-items: center; justify-content: space-between;
    flex-wrap: wrap; gap: 12px; margin-bottom: 20px;
    padding-bottom: 16px; border-bottom: 1px solid var(--border);
  }
  h1 { margin: 0; font-size: 22px; display: flex; align-items: center; gap: 10px; }
  .badge {
    font-size: 12px; padding: 4px 10px; border-radius: 999px; font-weight: 600;
    background: #21262d; border: 1px solid var(--border);
  }
  .badge.ok { color: var(--green); border-color: rgba(63,185,80,0.4); }
  .badge.warn { color: var(--yellow); border-color: rgba(210,153,34,0.4); }
  .badge.err { color: var(--red); border-color: rgba(248,81,73,0.4); }
  .grid {
    display: grid; grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
    gap: 14px; margin-bottom: 20px;
  }
  .card {
    background: var(--card); border: 1px solid var(--border);
    border-radius: 10px; padding: 16px;
  }
  .stat-label { font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: 0.04em; }
  .stat-val { font-size: 26px; font-weight: 700; margin-top: 6px; }
  .stat-sub { font-size: 12px; color: var(--muted); margin-top: 4px; }
  .row { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 20px; }
  @media (max-width: 800px) { .row { grid-template-columns: 1fr; } }
  h2 { margin: 0 0 12px 0; font-size: 16px; }
  label { display: block; font-size: 13px; color: var(--muted); margin-bottom: 4px; }
  input[type="text"], input[type="password"], input[type="number"] {
    width: 100%; padding: 8px 10px; background: #0d1117; border: 1px solid var(--border);
    color: var(--text); border-radius: 6px; font-size: 14px; margin-bottom: 12px;
  }
  .chk-row { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; font-size: 14px; }
  .btns { display: flex; gap: 10px; flex-wrap: wrap; margin-top: 10px; }
  button {
    cursor: pointer; border: 1px solid var(--border); background: #21262d;
    color: var(--text); padding: 8px 14px; border-radius: 6px; font-size: 13px; font-weight: 600;
  }
  button.primary { background: #238636; border-color: rgba(240,246,252,0.1); color: #fff; }
  button.accent { background: #1f6feb; border-color: rgba(240,246,252,0.1); color: #fff; }
  button:disabled { opacity: 0.5; cursor: not-allowed; }
  .table-wrap { width: 100%; overflow-x: auto; }
  table { width: 100%; border-collapse: collapse; font-size: 13px; }
  th, td { text-align: left; padding: 8px 10px; border-bottom: 1px solid var(--border); }
  th { color: var(--muted); font-weight: 600; }
  .pill { padding: 2px 8px; border-radius: 6px; font-size: 11px; font-weight: 600; }
  .pill.sdxl { background: rgba(63,185,80,0.15); color: var(--green); }
  .pill.krea { background: rgba(163,113,247,0.15); color: var(--purple); }
  .mono { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 12px; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div>
      <h1>
        <span>⚡ AI Horde Dreamer Worker</span>
        <span id="stateBadge" class="badge">Loading...</span>
        <span id="safetyBadge" class="badge ok">🛡️ horde-safety</span>
      </h1>
      <div class="stat-sub mono" id="bridgeAgent"></div>
    </div>
    <div class="btns">
      <button id="toggleBtn" onclick="toggleWorker()">Pause / Resume</button>
      <button class="accent" id="stepBtn" onclick="runSingleStep()">▶ Run 1 Job Now</button>
      <button onclick="loadState(true)">↻ Refresh</button>
    </div>
  </header>

  <div class="grid">
    <div class="card">
      <div class="stat-label">AI Horde Account</div>
      <div class="stat-val" id="accName">—</div>
      <div class="stat-sub" id="accKudos">Balance: 0 Kudos</div>
    </div>
    <div class="card">
      <div class="stat-label">Local SDXL (Unlimited)</div>
      <div class="stat-val" id="sdxlCount">0</div>
      <div class="stat-sub">Jobs rendered on local RTX GPU ($0 cost)</div>
    </div>
    <div class="card">
      <div class="stat-label">Modal Krea 2 Daily Quota</div>
      <div class="stat-val" id="kreaQuota">0 / 10</div>
      <div class="stat-sub" id="kreaSub">Auto-stops Krea2 at limit</div>
    </div>
    <div class="card">
      <div class="stat-label">Session Kudos Earned</div>
      <div class="stat-val" id="earnedKudos">0</div>
      <div class="stat-sub" id="mpsTotal">0 Megapixelsteps</div>
    </div>
  </div>

  <div class="row">
    <div class="card">
      <h2>Worker Settings & Modal Krea 2 Cap</h2>
      <label>Worker Name</label>
      <input type="text" id="workerName" placeholder="Direct-Dreamer">
      <label>AI Horde Worker API Key</label>
      <input type="password" id="apiKey" placeholder="Leave unchanged or paste new API key">
      <label>Krea 2 (Modal) Daily Generation Limit (0 = off, 10 = max 10/day)</label>
      <input type="number" id="kreaLimit" min="0" max="500" value="10">
      <div class="chk-row">
        <input type="checkbox" id="kreaEnabled" disabled>
        <span style="color:var(--muted);">Offer <b>Krea2-Turbo_fp8</b> (excluded pending reference registration)</span>
      </div>
      <div class="chk-row">
        <input type="checkbox" id="nsfwEnabled">
        <span>Allow NSFW generations</span>
      </div>
      <div class="btns">
        <button class="primary" onclick="saveConfig()">Save Settings</button>
        <button onclick="resetKreaQuota()">Reset Today's Krea Counter</button>
      </div>
    </div>

    <div class="card">
      <h2>Unlimited Local SDXL Models (`http://localhost:8188`)</h2>
      <div class="chk-row">
        <input type="checkbox" id="sdxlEnabled">
        <span><b>Enable Unlimited Local SDXL Worker (24/7 when idle)</b></span>
      </div>
      <div id="sdxlModelsList" style="margin-top:10px;"></div>
      <div class="stat-sub" style="margin-top:10px;">
        Active models currently advertised to <code>/v2/generate/pop</code>:
        <div class="mono" id="activeModels" style="margin-top:4px;color:var(--accent);"></div>
      </div>
      <div class="stat-sub" id="lastErrorBox" style="margin-top:8px;color:var(--red);"></div>
    </div>
  </div>

  <div class="card">
    <h2>Recent Fulfilled Jobs</h2>
    <div class="table-wrap">
      <table>
        <thead>
          <tr>
            <th>Time</th>
            <th>Job ID</th>
            <th>Model</th>
            <th>Backend</th>
            <th>Size</th>
            <th>Time</th>
            <th>Kudos</th>
            <th>Status</th>
          </tr>
        </thead>
        <tbody id="jobsTable">
          <tr><td colspan="8" style="color:var(--muted);">No jobs recorded yet.</td></tr>
        </tbody>
      </table>
    </div>
  </div>
</div>

<script>
let catalogData = {};
let formDirty = false;

async function loadState(refresh=false) {
  const r = await fetch('/horde/api/state' + (refresh ? '?refresh=true' : ''));
  if (!r.ok) return;
  const d = await r.json();
  renderState(d);
}

function renderState(d) {
  const cfg = d.config || {};
  const st = d.stats || {};
  const rt = d.runtime || {};
  const acc = d.account || {};

  const badge = document.getElementById('stateBadge');
  badge.textContent = (cfg.enabled ? rt.state : 'paused').toUpperCase();
  badge.className = 'badge ' + (cfg.enabled && rt.state !== 'error' ? 'ok' : (rt.state === 'error' ? 'err' : 'warn'));

  const sBadge = document.getElementById('safetyBadge');
  if (sBadge) {
    sBadge.textContent = rt.safety_available ? (rt.safety_loaded ? '🛡️ horde-safety: Active' : '🛡️ horde-safety: Ready') : '⚠️ horde-safety: Missing';
    sBadge.className = 'badge ' + (rt.safety_available ? 'ok' : 'err');
  }

  document.getElementById('toggleBtn').textContent = cfg.enabled ? '⏸ Pause Worker' : '▶ Resume Worker';
  document.getElementById('bridgeAgent').textContent = rt.bridge_agent || '';

  document.getElementById('accName').textContent = acc.username || 'Not fetched';
  document.getElementById('accKudos').textContent = 'Account Balance: ' + Math.round(acc.kudos || 0).toLocaleString() + ' Kudos';
  document.getElementById('sdxlCount').textContent = st.sdxl_jobs_completed || 0;
  document.getElementById('kreaQuota').textContent = (cfg.krea_daily_count || 0) + ' / ' + (cfg.krea_daily_limit ?? 10);
  document.getElementById('kreaSub').textContent = 'Total Krea jobs: ' + (st.krea_jobs_completed || 0) + ' (' + (cfg.krea_daily_date || '') + ' UTC)';
  document.getElementById('earnedKudos').textContent = '+' + (st.total_kudos_earned || 0).toLocaleString();
  document.getElementById('mpsTotal').textContent = (st.total_megapixelsteps || 0).toLocaleString() + ' MPS (' + (st.total_jobs_completed || 0) + ' total jobs)';

  if (!formDirty) {
    document.getElementById('workerName').value = cfg.worker_name || '';
    document.getElementById('apiKey').placeholder = cfg.masked_api_key || 'Enter API Key';
    document.getElementById('kreaLimit').value = cfg.krea_daily_limit ?? 10;
    document.getElementById('kreaEnabled').checked = !!cfg.krea_enabled;
    document.getElementById('nsfwEnabled').checked = !!cfg.nsfw;
    document.getElementById('sdxlEnabled').checked = !!cfg.sdxl_enabled;

    catalogData = cfg.sdxl_catalog || {};
    const selected = new Set(cfg.sdxl_models || []);
    const box = document.getElementById('sdxlModelsList');
    box.innerHTML = '';
    Object.keys(catalogData).forEach(mName => {
      const info = catalogData[mName];
      const vBadge = info.verified
        ? '<span style="color:var(--green);font-size:11px;">[✓ SHA256 verified]</span>'
        : '<span style="color:var(--yellow);font-size:11px;">[✗ missing reference weight]</span>';
      const div = document.createElement('div');
      div.className = 'chk-row';
      div.innerHTML = `<label style="display:flex;align-items:center;gap:8px;color:var(--text);margin:0;cursor:pointer;">
        <input type="checkbox" class="sdxl-model-chk" value="${mName}" ${selected.has(mName) ? 'checked' : ''}>
        <span><b>${mName}</b> <span style="color:var(--muted);font-size:12px;">(${info.ckpt})</span> ${vBadge}</span>
      </label>`;
      box.appendChild(div);
    });
  }

  document.getElementById('activeModels').textContent = (rt.active_models_offered || []).join(', ') || 'None';
  document.getElementById('lastErrorBox').textContent = rt.last_error ? ('Last Error: ' + rt.last_error) : '';

  const tbody = document.getElementById('jobsTable');
  const jobs = st.recent_jobs || [];
  if (!jobs.length) {
    tbody.innerHTML = '<tr><td colspan="8" style="color:var(--muted);">No jobs recorded yet.</td></tr>';
  } else {
    tbody.innerHTML = jobs.map(j => {
      const dt = new Date((j.ts || 0) * 1000).toLocaleTimeString();
      const pillCls = (j.backend || '').includes('Krea') ? 'krea' : 'sdxl';
      return `<tr>
        <td class="mono">${dt}</td>
        <td class="mono">${(j.id || '').slice(0, 8)}</td>
        <td><b>${j.model || ''}</b></td>
        <td><span class="pill ${pillCls}">${j.backend || ''}</span></td>
        <td class="mono">${j.resolution || ''}</td>
        <td class="mono">${j.duration_sec || 0}s</td>
        <td class="mono" style="color:var(--green);">+${j.kudos || 0}</td>
        <td style="color:var(--muted);max-width:260px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${((j.status || '') + (j.error ? ': ' + j.error : '')).replace(/</g,'&lt;')}</td>
      </tr>`;
    }).join('');
  }
}

document.addEventListener('input', () => { formDirty = true; });

async function saveConfig() {
  const models = Array.from(document.querySelectorAll('.sdxl-model-chk:checked')).map(el => el.value);
  const body = {
    worker_name: document.getElementById('workerName').value,
    krea_daily_limit: parseInt(document.getElementById('kreaLimit').value || '10', 10),
    krea_enabled: document.getElementById('kreaEnabled').checked,
    nsfw: document.getElementById('nsfwEnabled').checked,
    sdxl_enabled: document.getElementById('sdxlEnabled').checked,
    sdxl_models: models
  };
  const apiKeyVal = document.getElementById('apiKey').value.trim();
  if (apiKeyVal) body.api_key = apiKeyVal;

  const r = await fetch('/horde/api/config', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body)
  });
  if (r.ok) {
    document.getElementById('apiKey').value = '';
    formDirty = false;
    renderState(await r.json());
  }
}

async function toggleWorker() {
  const r = await fetch('/horde/api/toggle', {method: 'POST'});
  if (r.ok) renderState(await r.json());
}

async function resetKreaQuota() {
  const r = await fetch('/horde/api/reset_krea_quota', {method: 'POST'});
  if (r.ok) renderState(await r.json());
}

async function runSingleStep() {
  const btn = document.getElementById('stepBtn');
  btn.disabled = true;
  btn.textContent = '⏳ Popping & Rendering...';
  try {
    const r = await fetch('/horde/api/step', {method: 'POST'});
    if (r.ok) {
      const res = await r.json();
      renderState(res.state);
    }
  } finally {
    btn.disabled = false;
    btn.textContent = '▶ Run 1 Job Now';
  }
}

loadState(true);
setInterval(() => loadState(false), 6000);
</script>
</body>
</html>
"""
