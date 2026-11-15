"""
AI Horde Image Generation Mapper Layer (`reGen` / SDXL / SD 1.5).

Maps Direct UI image selections (styles, Creative Studio Express `base_model`,
character archetypes, verbal persona passports, sliders, and aspect ratios)
onto AI Horde's `/api/v2/generate/async` image network:

- **Owner / Paid Tiers (`Modal Krea 2`)**: Handled upstream by Modal Cloud GPU;
  uses this mapper only if Express SDXL is explicitly invoked while local ComfyUI is offline.
- **Guests / Free Users**:
  1. Hardcoded SFW enforcement (`nsfw=False`, `censor_nsfw=True`, `replacement_filter=True`,
     SFW negative prompt, and post-generation `censored` verification).
  2. Priority queue via `HORDE_WORKER_API_KEY` up to configurable daily Kudos caps
     (`HORDE_GUEST_DAILY_KUDOS_LIMIT` global + `HORDE_PER_USER_DAILY_KUDOS_LIMIT` per user).
  3. Automatic zero-cost fallback to anonymous (`0000000000`) SD 1.5 (`<= 512x640`)
     whenever the daily Kudos cap is reached or `KudosUpfront` is returned.
"""

from __future__ import annotations

import asyncio
import base64
import datetime
import io
import json
import logging
import os
import re
import threading
import time
from typing import Any, Awaitable, Callable, Dict, List, Optional, Tuple

import aiohttp
from PIL import Image

from config import SETTINGS
import horde_client

logger = logging.getLogger("horde_image_mapper")

DEFAULT_CLIENT_AGENT = horde_client.DEFAULT_CLIENT_AGENT
ANONYMOUS_API_KEY = horde_client.ANONYMOUS_API_KEY

# Curated AI Horde (`horde-worker-reGen`) model pools by visual family.
# Ordered by visual quality and typical worker availability on AI Horde.
STYLE_FAMILY_SDXL_MODELS: Dict[str, List[str]] = {
    "realistic": [
        "AlbedoBase XL (SDXL)",
        "Juggernaut XL",
        "AlbedoBase XL 3.1",
        "CyberRealistic Pony",
        "Pony Realism",
        "Quiet Goodnight XL",
        "SDXL 1.0",
    ],
    "semireal": [
        "AlbedoBase XL (SDXL)",
        "CyberRealistic Pony",
        "Juggernaut XL",
        "AlbedoBase XL 3.1",
        "Prefect Pony",
        "AMPonyXL",
    ],
    "anime": [
        "Nova Anime XL",
        "WAI-CUTE Pony",
        "TUNIX Pony",
        "Prefect Pony",
        "AlbedoBase XL (SDXL)",
    ],
    "fantasy": [
        "AlbedoBase XL (SDXL)",
        "Juggernaut XL",
        "AlbedoBase XL 3.1",
        "AMPonyXL",
        "CyberRealistic Pony",
    ],
}

STYLE_FAMILY_SD15_MODELS: Dict[str, List[str]] = {
    "realistic": [
        "ICBINP - I Can't Believe It's Not Photography",
        "AbsoluteReality",
        "Deliberate",
        "majicMIX realistic",
        "Realistic Vision",
        "stable_diffusion",
    ],
    "semireal": [
        "Deliberate",
        "Dreamshaper",
        "Rev Animated",
        "AbsoluteReality",
        "stable_diffusion",
    ],
    "anime": [
        "Flat-2D Animerge",
        "Rev Animated",
        "Anything v5",
        "Dreamshaper",
        "stable_diffusion",
    ],
    "fantasy": [
        "Dreamshaper",
        "Deliberate",
        "Rev Animated",
        "AbsoluteReality",
        "stable_diffusion",
    ],
}

# Maps UI style / base_model identifiers to (family, natural prompt modifier)
UI_STYLE_MAP: Dict[str, Tuple[str, str]] = {
    "none": ("realistic", ""),
    "free": ("realistic", ""),
    "realistic": (
        "realistic",
        "photorealistic, candid raw photography, natural skin texture, realistic lighting, sharp focus",
    ),
    "photorealistic": (
        "realistic",
        "8k uhd photorealistic, authentic raw candid photography, natural skin texture, realistic daylight",
    ),
    "sdxl_realistic": (
        "realistic",
        "photorealistic, natural lighting, detailed skin texture, sharp focus",
    ),
    "sdxl_lora_real_humans": (
        "realistic",
        "authentic natural skin texture, candid human proportions, raw daylight photography, realistic pores",
    ),
    "real_humans": (
        "realistic",
        "authentic natural skin texture, candid human proportions, raw daylight photography, realistic pores",
    ),
    "real humans": (
        "realistic",
        "authentic natural skin texture, candid human proportions, raw daylight photography, realistic pores",
    ),
    "sdxl": (
        "realistic",
        "high detail, crisp focus, natural lighting",
    ),
    "cinematic": (
        "realistic",
        "cinematic movie still, 35mm film photography, dramatic lighting, anamorphic bokeh, subtle film grain",
    ),
    "semireal": (
        "semireal",
        "2.5d digital art, semi-realistic illustration, soft cinematic subsurface scattering, refined stylized features",
    ),
    "perfect": (
        "semireal",
        "masterpiece portrait, refined aesthetics, studio rim lighting, ultra detailed",
    ),
    "sdxl_perfect": (
        "semireal",
        "masterpiece portrait, refined aesthetics, studio rim lighting, ultra detailed",
    ),
    "art": (
        "semireal",
        "artistic digital illustration, painterly lighting, expressive composition",
    ),
    "pinup": (
        "semireal",
        "vintage 1950s technicolor pin-up art style, retro glamour, warm painterly studio lighting, playful composition",
    ),
    "retro": (
        "semireal",
        "vintage retro technicolor illustration, warm nostalgic tones, painterly lighting",
    ),
    "pixar": (
        "semireal",
        "3d animated movie style, expressive stylized character, vibrant Pixar-like 3d digital render, soft subsurface scattering",
    ),
    "3d": (
        "semireal",
        "3d stylized digital render, expressive character design, soft studio lighting",
    ),
    "anime": (
        "anime",
        "vibrant masterwork anime illustration, clean detailed linework, expressive eyes, cel-shaded aesthetics",
    ),
    "tooned": (
        "anime",
        "vibrant stylized anime illustration, clean lineart, expressive features, rich colors",
    ),
    "sdxl_tooned": (
        "anime",
        "vibrant stylized anime illustration, clean lineart, expressive features, rich colors",
    ),
    "comic": (
        "anime",
        "modern comic book illustration, dynamic bold ink linework, graphic novel cel shading, vibrant colors",
    ),
    "nix_comic": (
        "anime",
        "expressive western comic book art, clean ink linework, vibrant cel-shaded colors",
    ),
    "midjourney": (
        "fantasy",
        "artistic concept art, ethereal atmospheric lighting, dramatic composition, masterpiece",
    ),
    "fantasy": (
        "fantasy",
        "epic fantasy concept art, ethereal magical lighting, intricate atmospheric details, masterpiece",
    ),
    "sdxl_fantasy": (
        "fantasy",
        "epic fantasy concept art, ethereal magical lighting, intricate atmospheric details, masterpiece",
    ),
    "neon": (
        "fantasy",
        "neon noir cyberpunk aesthetic, vibrant cyan and magenta rim light, glowing neon bokeh, night city reflections",
    ),
    "cyberpunk": (
        "fantasy",
        "cyberpunk night aesthetic, neon reflections, atmospheric rain and volumetric glow",
    ),
}

# Pure visual descriptions for Direct UI archetypes (free of private LoRA trigger tokens)
ARCHETYPE_VISUAL_TRAITS: Dict[str, Tuple[str, Optional[str]]] = {
    # Standard (SDXL) character archetypes
    "judith": ("natural expressive young woman with warm wavy auburn-brown hair, bright emerald-green eyes, subtle freckles, charming smile", None),
    "sdxl_judith": ("natural expressive young woman with warm wavy auburn-brown hair, bright emerald-green eyes, subtle freckles, charming smile", None),
    "june": ("charismatic stylish young woman, warm expressive gaze, modern look", None),
    "domi": ("charismatic stylish brunette woman with piercing hazel gaze, modern chic look", None),
    "sdxl_domi": ("charismatic stylish brunette woman with piercing hazel gaze, modern chic look", None),
    "lana": ("radiant Slavic young woman with honey-blonde hair, expressive light eyes, warm natural beauty", None),
    "sdxl_lana": ("radiant Slavic young woman with honey-blonde hair, expressive light eyes, warm natural beauty", None),
    "oriana": ("glamorous confident woman with sculpted features, luminous dark eyes, magnetic poised presence", None),
    "sdxl_oriana": ("glamorous confident woman with sculpted features, luminous dark eyes, magnetic poised presence", None),
    "vera": ("elegant dark-haired woman with striking deep gaze, classic refined features", None),
    "sdxl_vera": ("elegant dark-haired woman with striking deep gaze, classic refined features", None),
    "bianca": ("elegant poised blonde woman with refined classic features, bright eyes, graceful demeanor", None),
    "sdxl_bianca": ("elegant poised blonde woman with refined classic features, bright eyes, graceful demeanor", None),
    "beatrice": ("classic European beauty with chestnut hair, refined cheekbones, warm intelligent gaze", None),
    "sdxl_beatrice": ("classic European beauty with chestnut hair, refined cheekbones, warm intelligent gaze", None),
    "minerva": ("striking woman with dark raven hair, sculpted cheekbones, mystical piercing gaze", None),
    "sdxl_minerva": ("striking woman with dark raven hair, sculpted cheekbones, mystical piercing gaze", None),
    # Studio (Krea 2 / Anima) archetypes (kept for private fallback if needed)
    "ada": ("confident woman with short black bob hair, elegant crimson dress, poised gaze", None),
    "ada_wong": ("confident woman with short black bob hair, elegant crimson dress, poised gaze", None),
    "brie": ("cheerful blonde woman, bright blue eyes, warm radiant smile", None),
    "kxbrie": ("cheerful blonde woman, bright blue eyes, warm radiant smile", None),
    "tifa": ("athletic young woman with long dark brown hair and ruby-red eyes, confident expression", None),
    "tifa_lockhart": ("athletic young woman with long dark brown hair and ruby-red eyes, confident expression", None),
    "alt": ("alternative goth aesthetic young woman, dark eyeliner, choker, subtle colored hair highlights, moody lighting", "fantasy"),
    "goth": ("alternative goth aesthetic young woman, dark eyeliner, choker, moody atmospheric shadows", "fantasy"),
    "influencer": ("candid lifestyle portrait, golden hour natural sunlight, authentic relaxed expression", "realistic"),
    "luna": ("charming young woman, warm expressive eyes, gentle smile, soft natural lighting", None),
    "maddie": ("natural girl-next-door look, warm authentic features, soft daylight", "realistic"),
    "kate": ("striking woman with platinum blonde hair and bright blue eyes", None),
    "frieren": ("elven mage woman with silver-white twin-tail hair, green eyes, pointed ears, white capelet", "anime"),
    "2b": ("android heroine with short silver-white bob hair, black blindfold visor, gothic black dress", "anime"),
    "makima": ("young woman with braided crimson-red hair, golden-yellow eyes, white collared shirt and black tie", "anime"),
    "lucy": ("cyberpunk young woman with short pastel-streaked white hair, red eyeliner, futuristic jacket", "anime"),
    "asuka": ("spirited young woman with long reddish-brown twin-tails, blue eyes, futuristic red suit", "anime"),
}

BASE_NEGATIVE_PROMPT = (
    "worst quality, low quality, blurry, out of focus, bad anatomy, bad hands, "
    "missing fingers, extra digits, deformed, disfigured, watermark, signature, text, logo, jpeg artifacts"
)

GUEST_SFW_NEGATIVE_PROMPT = (
    "nsfw, nude, naked, explicit, topless, nipples, genitalia, areola, "
    "underwear, lingerie, sexual, erotic, suggestive, uncensored, fetishes, gore"
)

# Synthetic private LoRA tokens to strip if they appear in raw prompts
_PRIVATE_TRIGGER_RE = re.compile(
    r"\b(?:DomiK4?|kxbrie|KR2Pix|Lun4|sysdeep_2b|CPELucy|sysdeep_asuka|Bradhamel\s+art\s+style|famegrid\s+style)\b,?\s*",
    flags=re.IGNORECASE,
)
_ANGLE_TAG_RE = re.compile(r"<[^>]+>")

# Cache for AI Horde online image models
_IMAGE_MODELS_CACHE_TTL = 60.0
_image_models_cache: Tuple[float, Dict[str, Dict[str, Any]]] = (0.0, {})

# Global rate-limiter for AI Horde `/generate/async` (limit is 2 req/sec; we space by >=0.65s)
_submit_rate_lock: Optional[asyncio.Lock] = None
_last_submit_monotonic: float = 0.0

# Daily guest Kudos budget state
_budget_lock = threading.Lock()
_BUDGET_FILE_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "extensions",
    "horde_guest_budget.json",
)


class HordeImageError(RuntimeError):
    """Raised when AI Horde image generation fails or is blocked."""


class HordeCensoredError(HordeImageError):
    """Raised when AI Horde worker censored a guest generation due to NSFW safety."""


def _truthy(val: Any, default: bool = True) -> bool:
    if val is None:
        return default
    if isinstance(val, bool):
        return val
    s = str(val).strip().lower()
    if not s:
        return default
    return s in ("1", "true", "yes", "on")


def is_horde_image_enabled() -> bool:
    """Returns True when AI Horde image generation mapper is enabled in config."""
    return _truthy(SETTINGS.get("HORDE_IMAGE_ENABLED", "true"), default=True)


def is_horde_guest_routing_enabled() -> bool:
    """Returns True when guest / free-tier image requests route directly to AI Horde."""
    return is_horde_image_enabled() and _truthy(
        SETTINGS.get("HORDE_IMAGE_FOR_GUESTS", "true"), default=True
    )


def _is_privileged_caller(ctx: Any) -> bool:
    if ctx is None:
        return False
    if getattr(ctx, "trusted_owner", False) or getattr(ctx, "private_mode", False) or getattr(ctx, "is_unlimited", False):
        return True
    try:
        from extensions.whop_verifier import is_node_owner, is_muse_pass_user, is_club_member_sync
        if is_node_owner(ctx) or is_muse_pass_user(ctx) or is_club_member_sync(ctx):
            return True
    except Exception:
        pass
    return False


def should_use_horde_for_sdxl(ctx: Any, comfy_available: bool = False) -> bool:
    """
    Determines whether an Express / SDXL / non-Krea image request should be fulfilled
    via AI Horde (`reGen`) instead of local ComfyUI:
    - Always False if `HORDE_IMAGE_ENABLED = false`.
    - True for all Guests / Free users when `HORDE_IMAGE_FOR_GUESTS = true` (default),
      keeping local GPU free for Ollama + standalone `horde-worker-reGen`.
    - True for ANY user (including Owner) when local ComfyUI (`localhost:8188`) is offline.
    """
    if not is_horde_image_enabled():
        return False
    if not comfy_available:
        return True
    if not _is_privileged_caller(ctx) and is_horde_guest_routing_enabled():
        return True
    provider = str(SETTINGS.get("AI_PROVIDER", "local") or "local").strip().lower()
    if provider == "horde":
        return True
    return False


def _utc_today() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")


def _load_budget_state(budget_path: Optional[str] = None) -> Dict[str, Any]:
    path = budget_path or _BUDGET_FILE_PATH
    today = _utc_today()
    default_state = {"date": today, "total_kudos": 0.0, "per_user": {}}
    if not os.path.exists(path):
        return default_state
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict) or data.get("date") != today:
            return default_state
        return {
            "date": today,
            "total_kudos": float(data.get("total_kudos") or 0.0),
            "per_user": dict(data.get("per_user") or {}) if isinstance(data.get("per_user"), dict) else {},
        }
    except Exception:
        return default_state


def _save_budget_state(state: Dict[str, Any], budget_path: Optional[str] = None) -> None:
    path = budget_path or _BUDGET_FILE_PATH
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp_path = f"{path}.tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(state, f, ensure_ascii=False, indent=2)
        os.replace(tmp_path, path)
    except Exception as e:
        logger.debug(f"[HordeMapper] Could not persist guest kudos budget: {e}")


def check_guest_priority_budget(
    user_id: Optional[str],
    estimated_kudos: float = 32.0,
    budget_path: Optional[str] = None,
) -> Tuple[bool, float, float]:
    """
    Checks whether `user_id` is within both the per-user and global daily Kudos caps
    for priority AI Horde image generation.
    Returns `(within_budget, remaining_global, remaining_user)`.
    """
    try:
        global_cap = max(0.0, float(SETTINGS.get("HORDE_GUEST_DAILY_KUDOS_LIMIT", 2000.0)))
    except Exception:
        global_cap = 2000.0
    try:
        user_cap = max(0.0, float(SETTINGS.get("HORDE_PER_USER_DAILY_KUDOS_LIMIT", 350.0)))
    except Exception:
        user_cap = 350.0

    uid = str(user_id or "anonymous").strip() or "anonymous"
    with _budget_lock:
        st = _load_budget_state(budget_path)
        used_total = float(st.get("total_kudos") or 0.0)
        used_user = float((st.get("per_user") or {}).get(uid) or 0.0)
        rem_global = max(0.0, global_cap - used_total)
        rem_user = max(0.0, user_cap - used_user)
        allowed = (rem_global >= estimated_kudos) and (rem_user >= estimated_kudos)
        return allowed, rem_global, rem_user


def record_guest_kudos_spent(
    user_id: Optional[str],
    kudos: float,
    budget_path: Optional[str] = None,
) -> None:
    if kudos <= 0:
        return
    uid = str(user_id or "anonymous").strip() or "anonymous"
    with _budget_lock:
        st = _load_budget_state(budget_path)
        st["total_kudos"] = round(float(st.get("total_kudos") or 0.0) + float(kudos), 2)
        per_user = st.setdefault("per_user", {})
        per_user[uid] = round(float(per_user.get(uid) or 0.0) + float(kudos), 2)
        _save_budget_state(st, budget_path)


def _resolve_priority_api_key() -> str:
    worker_key = str(SETTINGS.get("HORDE_WORKER_API_KEY") or "").strip()
    if worker_key and worker_key != ANONYMOUS_API_KEY:
        return worker_key
    horde_key = str(SETTINGS.get("HORDE_API_KEY") or "").strip()
    if horde_key and horde_key != ANONYMOUS_API_KEY:
        return horde_key
    return ""


async def fetch_online_image_models(
    session: aiohttp.ClientSession,
    client_agent: str = DEFAULT_CLIENT_AGENT,
    force_refresh: bool = False,
) -> Dict[str, Dict[str, Any]]:
    """Fetches and caches online AI Horde image models (`count >= 1`) for 60s."""
    global _image_models_cache
    now = time.monotonic()
    cached_ts, cached_map = _image_models_cache
    if not force_refresh and cached_map and (now - cached_ts) < _IMAGE_MODELS_CACHE_TTL:
        return cached_map

    base = horde_client.HORDE_BASE_URL.rstrip("/")
    try:
        async with session.get(
            f"{base}/status/models?type=image",
            headers={"Client-Agent": client_agent, "User-Agent": "Direct/1.0"},
            timeout=aiohttp.ClientTimeout(total=8.0),
        ) as resp:
            if resp.status == 200:
                items = await resp.json()
                if isinstance(items, list):
                    online: Dict[str, Dict[str, Any]] = {}
                    for m in items:
                        if not isinstance(m, dict):
                            continue
                        name = str(m.get("name") or "").strip()
                        cnt = int(m.get("count") or 0)
                        if name and cnt > 0:
                            online[name] = m
                    if online:
                        _image_models_cache = (now, online)
                        return online
    except Exception as e:
        logger.debug(f"[HordeMapper] Could not refresh online image models: {e}")

    return cached_map


def select_online_models_for_family(
    family: str,
    use_sdxl: bool,
    online_models: Dict[str, Dict[str, Any]],
    max_models: int = 4,
) -> List[str]:
    """
    Picks up to `max_models` live AI Horde models matching `family` (`realistic`,
    `semireal`, `anime`, `fantasy`) and generation tier (`SDXL` vs `SD 1.5`).
    """
    pool = (
        STYLE_FAMILY_SDXL_MODELS.get(family) or STYLE_FAMILY_SDXL_MODELS["realistic"]
        if use_sdxl
        else STYLE_FAMILY_SD15_MODELS.get(family) or STYLE_FAMILY_SD15_MODELS["realistic"]
    )
    if not online_models:
        return list(pool[:max_models])

    available = [m for m in pool if m in online_models]
    if not available and use_sdxl:
        # Fallback across general SDXL realistic models if specialized family is offline
        available = [m for m in STYLE_FAMILY_SDXL_MODELS["realistic"] if m in online_models]
    if not available:
        available = [m for m in STYLE_FAMILY_SD15_MODELS["realistic"] if m in online_models]
    if not available:
        return list(pool[:max_models])

    def _score(model_name: str) -> Tuple[int, float, int]:
        info = online_models.get(model_name) or {}
        cnt = int(info.get("count") or 0)
        eta = float(info.get("eta") or 0.0)
        # Prefer models with >= 2 workers and low ETA while keeping curated top choices strong
        curated_bonus = max(0, 10 - pool.index(model_name)) if model_name in pool else 0
        return (1 if cnt >= 2 else 0, -eta, cnt + curated_bonus)

    ranked = sorted(available, key=_score, reverse=True)
    return ranked[:max_models]


def _extract_clean_prompt_and_tags(raw_prompt: str) -> Tuple[str, List[str]]:
    text = str(raw_prompt or "").strip()
    # Strip "Title: ...\nImage: ..." wrapper if passed from /chat/stream
    if "Image:" in text:
        text = text.split("Image:", 1)[1].strip()
    elif text.lower().startswith("title:") and "\n" in text:
        text = text.split("\n", 1)[1].strip()

    # Prevent `###` prompt-splitting injection from user text
    text = text.replace("###", ", ")

    tags = [t.strip().lower() for t in re.findall(r"<([^>]+)>", text) if t.strip()]
    clean = _ANGLE_TAG_RE.sub(" ", text)
    clean = _PRIVATE_TRIGGER_RE.sub("", clean)
    clean = re.sub(r"\s+", " ", clean).strip(" ,")
    return clean, tags


def _resolve_dimensions(width: int, height: int, use_sdxl: bool) -> Tuple[int, int]:
    """
    Maps requested UI canvas dimensions to AI Horde multiples of 64:
    - Priority SDXL (`use_sdxl=True`): `832x1024` (portrait), `1024x832` (landscape), `896x896` (square).
    - Anonymous SD 1.5 (`use_sdxl=False`): `512x640` (portrait), `640x512` (landscape), `512x512` (square)
      — strictly `<= 327,680` pixels so AI Horde never rejects anonymous `0000000000` with `KudosUpfront`.
    """
    w = max(256, int(width or 896))
    h = max(256, int(height or 1152))
    ratio = w / float(h)
    if use_sdxl:
        if ratio < 0.90:
            return 832, 1024
        if ratio > 1.11:
            return 1024, 832
        return 896, 896
    else:
        if ratio < 0.90:
            return 512, 640
        if ratio > 1.11:
            return 640, 512
        return 512, 512


def map_ui_to_horde_request(
    prompt: str,
    persona_prompt: str = "",
    sliders: Optional[Dict[str, Any]] = None,
    style_setting: str = "",
    width: int = 896,
    height: int = 1152,
    seed: Optional[int] = None,
    is_uncensored: bool = False,
    use_sdxl: bool = True,
    online_models: Optional[Dict[str, Dict[str, Any]]] = None,
) -> Dict[str, Any]:
    """
    Translates Direct UI parameters into a validated AI Horde `/api/v2/generate/async` JSON payload.
    """
    sliders = sliders or {}
    clean_scene, prompt_tags = _extract_clean_prompt_and_tags(prompt)
    clean_persona, _ = _extract_clean_prompt_and_tags(persona_prompt)

    # 1. Resolve UI style & model family
    style_candidates: List[str] = []
    for t in prompt_tags:
        if t in UI_STYLE_MAP and t not in ("none", "free"):
            style_candidates.append(t)
    if isinstance(sliders.get("styles"), list):
        for s in sliders["styles"]:
            s_low = str(s or "").strip().lower()
            if s_low in UI_STYLE_MAP and s_low not in ("none", "free"):
                style_candidates.append(s_low)
    for raw_s in (sliders.get("style"), sliders.get("base_model"), style_setting):
        s_low = str(raw_s or "").strip().lower().strip("<> ")
        if s_low in UI_STYLE_MAP and s_low not in ("none", "free"):
            style_candidates.append(s_low)

    chosen_style_key = style_candidates[0] if style_candidates else "realistic"
    family, style_modifier = UI_STYLE_MAP.get(chosen_style_key, UI_STYLE_MAP["realistic"])

    # 2. Resolve character archetypes -> visual traits
    raw_archs: List[str] = []
    if isinstance(sliders.get("archetypes"), (list, tuple)):
        raw_archs.extend(str(a).strip().lower() for a in sliders["archetypes"] if a)
    elif isinstance(sliders.get("archetype"), str) and sliders["archetype"].strip():
        raw_archs.extend(a.strip().lower() for a in sliders["archetype"].split(",") if a.strip())
    for t in prompt_tags:
        if t in ARCHETYPE_VISUAL_TRAITS and t not in raw_archs:
            raw_archs.append(t)

    arch_traits: List[str] = []
    for arch in raw_archs:
        trait_entry = ARCHETYPE_VISUAL_TRAITS.get(arch)
        if trait_entry:
            trait_text, arch_family = trait_entry
            if trait_text and trait_text not in arch_traits:
                arch_traits.append(trait_text)
            # If user didn't pick an explicit non-default style, let anime archetypes steer family
            if arch_family and chosen_style_key in ("realistic", "sdxl_realistic", "sdxl", "none", "free"):
                family = arch_family

    # 3. Compose natural positive prompt (scene first, then persona/archetype, then sliders & style)
    prompt_parts: List[str] = []
    if clean_scene:
        prompt_parts.append(clean_scene)
    if clean_persona and clean_persona.lower() not in clean_scene.lower():
        prompt_parts.append(clean_persona)
    for tr in arch_traits:
        if tr.lower() not in clean_scene.lower():
            prompt_parts.append(tr)

    # Micro-sliders from Creative Studio
    try:
        detail_val = float(sliders.get("detail", 0.5))
        if detail_val >= 0.7:
            prompt_parts.append("ultra-detailed, fine textures, crisp focus")
        elif detail_val <= 0.3:
            prompt_parts.append("soft cinematic focus, gentle atmospheric glow")
    except Exception:
        pass

    try:
        age_val = float(sliders.get("age", 0.0))
        if age_val > 0.3:
            prompt_parts.append("mature distinguished appearance")
        elif age_val < -0.3:
            prompt_parts.append("youthful vibrant appearance")
    except Exception:
        pass

    try:
        weight_val = float(sliders.get("weight", 0.0))
        if weight_val > 0.3:
            prompt_parts.append("curvy athletic build")
        elif weight_val < -0.3:
            prompt_parts.append("slender delicate silhouette")
    except Exception:
        pass

    if style_modifier:
        prompt_parts.append(style_modifier)

    positive_prompt = ", ".join(p.strip(" ,") for p in prompt_parts if p and p.strip(" ,"))
    if not positive_prompt:
        positive_prompt = "beautiful scenic illustration, high quality, detailed"
    # Keep prompt well within AI Horde max length
    positive_prompt = positive_prompt[:1400]

    # 4. Compose negative prompt (hardcoded SFW negative armor for all non-uncensored/guest callers)
    neg_parts = [BASE_NEGATIVE_PROMPT]
    if not is_uncensored:
        neg_parts.insert(0, GUEST_SFW_NEGATIVE_PROMPT)
    if family == "realistic":
        neg_parts.append("3d render, cgi, anime, cartoon, plastic skin")
    negative_prompt = ", ".join(neg_parts)

    horde_prompt = f"{positive_prompt} ### {negative_prompt}"

    # 5. Dimensions, sampler, steps, and live models
    target_w, target_h = _resolve_dimensions(width, height, use_sdxl=use_sdxl)
    steps = 22 if use_sdxl else 20
    cfg_scale = 5.5 if use_sdxl else 6.5

    models = select_online_models_for_family(
        family=family,
        use_sdxl=use_sdxl,
        online_models=online_models or {},
        max_models=4,
    )

    params: Dict[str, Any] = {
        "sampler_name": "k_euler_a",
        "cfg_scale": cfg_scale,
        "width": target_w,
        "height": target_h,
        "steps": steps,
        "karras": True,
        "n": 1,
    }
    if seed is not None and int(seed) > 0:
        params["seed"] = str(int(seed))

    # Hardcoded guest safety guarantees:
    # - Guests (`is_uncensored=False`): nsfw=False, censor_nsfw=True, replacement_filter=True
    # - Verified Owner (`is_uncensored=True`): nsfw=True, censor_nsfw=False
    allow_nsfw = bool(is_uncensored)
    payload: Dict[str, Any] = {
        "prompt": horde_prompt,
        "params": params,
        "nsfw": allow_nsfw,
        "censor_nsfw": not allow_nsfw,
        "replacement_filter": not allow_nsfw,
        "trusted_workers": False,
        "slow_workers": True,
        "models": models,
        "r2": True,
    }
    return payload


def _normalize_image_to_png(raw_bytes: bytes) -> bytes:
    """Converts WebP/JPEG/PNG bytes returned by AI Horde into standard RGB PNG bytes."""
    with Image.open(io.BytesIO(raw_bytes)) as im:
        if im.mode not in ("RGB", "RGBA"):
            im = im.convert("RGB")
        out = io.BytesIO()
        im.save(out, format="PNG")
        return out.getvalue()


async def _throttled_submit(
    session: aiohttp.ClientSession,
    url: str,
    payload: Dict[str, Any],
    headers: Dict[str, str],
) -> Tuple[int, Dict[str, Any], str]:
    """Submits `POST /generate/async` respecting AI Horde's 2 req/sec limit with 429 retry."""
    global _submit_rate_lock, _last_submit_monotonic
    if _submit_rate_lock is None:
        _submit_rate_lock = asyncio.Lock()

    for attempt in range(3):
        async with _submit_rate_lock:
            elapsed = time.monotonic() - _last_submit_monotonic
            if elapsed < 0.65:
                await asyncio.sleep(0.65 - elapsed)
            _last_submit_monotonic = time.monotonic()
            async with session.post(url, json=payload, headers=headers) as resp:
                status = resp.status
                raw_text = await resp.text()

        if status == 429 and attempt < 2:
            await asyncio.sleep(1.2 * (attempt + 1))
            continue

        parsed: Dict[str, Any] = {}
        try:
            parsed = json.loads(raw_text) if raw_text else {}
        except Exception:
            parsed = {}
        return status, parsed, raw_text

    return 429, {}, "Rate limited by AI Horde"


async def _cancel_image_task(
    task_id: str,
    api_key: str,
    client_agent: str = DEFAULT_CLIENT_AGENT,
) -> None:
    if not task_id:
        return
    base = horde_client.HORDE_BASE_URL.rstrip("/")
    try:
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=6.0)) as sess:
            async with sess.delete(
                f"{base}/generate/status/{task_id}",
                headers={"apikey": api_key, "Client-Agent": client_agent, "User-Agent": "Direct/1.0"},
            ) as resp:
                logger.info(f"[HordeMapper] Cancelled unfinished image job {task_id} (HTTP {resp.status})")
    except Exception as e:
        logger.debug(f"[HordeMapper] Failed to cancel image job {task_id}: {e}")


async def generate_horde_image(
    ctx: Any,
    prompt: str,
    persona_prompt: str = "",
    sliders: Optional[Dict[str, Any]] = None,
    width: int = 896,
    height: int = 1152,
    seed: Optional[int] = None,
    is_uncensored: bool = False,
    progress_cb: Optional[Callable[..., Awaitable[None]]] = None,
    client_agent: str = DEFAULT_CLIENT_AGENT,
    poll_interval: float = 2.2,
    max_wait_seconds: Optional[int] = None,
    budget_path: Optional[str] = None,
) -> Dict[str, Any]:
    """
    End-to-end AI Horde image generation with UI mapping, SFW guest enforcement,
    daily Kudos cap management, automatic anonymous SD 1.5 fallback, and PNG conversion.

    Returns:
        {
            "image_bytes": bytes (PNG),
            "model": str,
            "worker_name": str,
            "kudos": float,
            "tier_used": "priority_sdxl" | "anon_sd15",
            "censored": bool,
        }
    """
    if max_wait_seconds is None:
        try:
            max_wait_seconds = max(30, int(SETTINGS.get("HORDE_IMAGE_MAX_WAIT_SECONDS", 150)))
        except Exception:
            max_wait_seconds = 150

    is_owner_or_priv = _is_privileged_caller(ctx)
    # Guests are NEVER allowed uncensored Horde image requests regardless of caller args
    effective_uncensored = bool(is_uncensored and is_owner_or_priv)

    user_id = getattr(ctx, "user_id", None) or "anonymous"
    style_setting = ""
    if ctx is not None and isinstance(getattr(ctx, "settings", None), dict):
        style_setting = str(ctx.settings.get("style") or "")

    priority_key = _resolve_priority_api_key()
    use_priority = False
    if priority_key:
        if is_owner_or_priv:
            use_priority = True
        else:
            within_cap, rem_g, rem_u = check_guest_priority_budget(
                user_id, estimated_kudos=32.0, budget_path=budget_path
            )
            use_priority = within_cap
            if not within_cap:
                logger.info(
                    f"[HordeMapper] Guest '{user_id}' priority Kudos cap reached "
                    f"(rem_global={rem_g:.1f}, rem_user={rem_u:.1f}) — falling back to anonymous SD 1.5"
                )

    base = horde_client.HORDE_BASE_URL.rstrip("/")
    timeout = aiohttp.ClientTimeout(total=max_wait_seconds + 20, connect=15)

    async with aiohttp.ClientSession(timeout=timeout) as session:
        online_models = await fetch_online_image_models(session, client_agent=client_agent)

        if progress_cb:
            await progress_cb("warming", phase="queueing")

        # Try priority SDXL first if eligible; auto-downgrade to anonymous SD 1.5 on KudosUpfront
        tiers_to_try = ["priority_sdxl", "anon_sd15"] if use_priority else ["anon_sd15"]
        if is_owner_or_priv and not use_priority:
            tiers_to_try = ["anon_sd15"]

        submit_res: Dict[str, Any] = {}
        active_api_key = ANONYMOUS_API_KEY
        tier_used = "anon_sd15"
        req_payload: Dict[str, Any] = {}

        for candidate_tier in tiers_to_try:
            is_sdxl_tier = candidate_tier == "priority_sdxl"
            active_api_key = priority_key if is_sdxl_tier else ANONYMOUS_API_KEY
            # Note: anonymous 0000000000 on AI Horde cannot reliably request high-res NSFW;
            # keep effective_uncensored only when authenticated with priority key.
            tier_uncensored = effective_uncensored and is_sdxl_tier
            req_payload = map_ui_to_horde_request(
                prompt=prompt,
                persona_prompt=persona_prompt,
                sliders=sliders,
                style_setting=style_setting,
                width=width,
                height=height,
                seed=seed,
                is_uncensored=tier_uncensored,
                use_sdxl=is_sdxl_tier,
                online_models=online_models,
            )
            headers = {
                "apikey": active_api_key,
                "Client-Agent": client_agent,
                "User-Agent": "Direct/1.0",
                "Content-Type": "application/json",
            }
            status_code, parsed, raw_err = await _throttled_submit(
                session, f"{base}/generate/async", req_payload, headers
            )
            if status_code in (200, 202) and parsed.get("id"):
                submit_res = parsed
                tier_used = candidate_tier
                break

            rc = str(parsed.get("rc") or "")
            if is_sdxl_tier and (status_code in (401, 403) or rc in ("KudosUpfront", "InvalidCredentials")):
                logger.warning(
                    f"[HordeMapper] Priority SDXL rejected ({status_code} {rc}: {raw_err[:120]}), "
                    "downgrading to anonymous SD 1.5"
                )
                continue

            raise HordeImageError(
                f"AI Horde image submit failed ({status_code}): {parsed.get('message') or raw_err[:160]}"
            )

        task_id = str(submit_res.get("id") or "")
        kudos_cost = float(submit_res.get("kudos") or 0.0)
        models_requested = req_payload.get("models") or []
        logger.info(
            f"[HordeMapper] Submitted job {task_id} (tier={tier_used}, models={models_requested}, "
            f"size={req_payload['params']['width']}x{req_payload['params']['height']}, "
            f"nsfw={req_payload['nsfw']}, censor={req_payload['censor_nsfw']}, kudos={kudos_cost})"
        )

        task_terminal = False
        try:
            start_mono = time.monotonic()
            while (time.monotonic() - start_mono) < max_wait_seconds:
                await asyncio.sleep(poll_interval)
                async with session.get(
                    f"{base}/generate/check/{task_id}",
                    headers={"Client-Agent": client_agent, "User-Agent": "Direct/1.0"},
                ) as c_resp:
                    if c_resp.status == 404:
                        task_terminal = True
                        raise HordeImageError("AI Horde image job expired or was not found.")
                    if c_resp.status != 200:
                        continue
                    chk = await c_resp.json()

                if chk.get("done"):
                    task_terminal = True
                    break

                if chk.get("faulted"):
                    task_terminal = True
                    raise HordeImageError("AI Horde worker faulted during image generation.")

                if chk.get("is_possible") is False:
                    raise HordeImageError("No online AI Horde workers can fulfill this image style right now.")

                q_pos = int(chk.get("queue_position") or 0)
                wait_s = int(chk.get("wait_time") or 0)
                processing = int(chk.get("processing") or 0)
                if progress_cb:
                    if processing > 0 or q_pos == 0:
                        elapsed_ratio = min(0.9, (time.monotonic() - start_mono) / max(12.0, float(wait_s + 8)))
                        est_step = max(1, int(round(elapsed_ratio * req_payload["params"]["steps"])))
                        await progress_cb(
                            "generating",
                            phase="sampling",
                            step=est_step,
                            total_steps=req_payload["params"]["steps"],
                        )
                    else:
                        await progress_cb(
                            "warming",
                            phase=f"queue:{q_pos}" if q_pos else "queueing",
                            queue_position=q_pos,
                            wait_time=wait_s,
                        )
            else:
                raise HordeImageError(f"AI Horde image generation timed out after {max_wait_seconds}s.")

            # Fetch final generation payload once `done == True`
            async with session.get(
                f"{base}/generate/status/{task_id}",
                headers={"apikey": active_api_key, "Client-Agent": client_agent, "User-Agent": "Direct/1.0"},
            ) as s_resp:
                if s_resp.status != 200:
                    err_txt = await s_resp.text()
                    raise HordeImageError(f"Failed to fetch AI Horde image status ({s_resp.status}): {err_txt[:120]}")
                st_data = await s_resp.json()

            generations = st_data.get("generations") or []
            if not generations:
                raise HordeImageError("AI Horde completed job without returning an image.")

            gen = generations[0]
            was_censored = bool(gen.get("censored"))
            if was_censored and not effective_uncensored:
                logger.warning(f"[HordeMapper] Worker censored NSFW output for guest '{user_id}' on job {task_id}")
                raise HordeCensoredError(
                    "Generated image was blocked by the AI Horde SFW safety filter. Please try a safer prompt."
                )

            img_field = str(gen.get("img") or "").strip()
            if not img_field:
                raise HordeImageError("AI Horde generation payload missing image data.")

            if progress_cb:
                await progress_cb("saving")

            if img_field.startswith("http://") or img_field.startswith("https://"):
                async with session.get(img_field, timeout=aiohttp.ClientTimeout(total=30.0)) as dl_resp:
                    if dl_resp.status != 200:
                        raise HordeImageError(f"Failed to download generated image from R2 ({dl_resp.status})")
                    raw_img_bytes = await dl_resp.read()
            else:
                if "," in img_field and img_field.startswith("data:"):
                    _, img_field = img_field.split(",", 1)
                raw_img_bytes = base64.b64decode(img_field)

            png_bytes = _normalize_image_to_png(raw_img_bytes)
            actual_model = str(gen.get("model") or (models_requested[0] if models_requested else "AI Horde"))
            worker_name = str(gen.get("worker_name") or "horde-worker")
            final_kudos = float(st_data.get("kudos") or kudos_cost or 0.0)

            if tier_used == "priority_sdxl" and not is_owner_or_priv and final_kudos > 0:
                record_guest_kudos_spent(user_id, final_kudos, budget_path=budget_path)

            logger.info(
                f"[HordeMapper] Completed job {task_id} via '{worker_name}' [{actual_model}] "
                f"({len(png_bytes)} PNG bytes, kudos={final_kudos}, tier={tier_used})"
            )

            return {
                "image_bytes": png_bytes,
                "model": actual_model,
                "worker_name": worker_name,
                "kudos": final_kudos,
                "tier_used": tier_used,
                "censored": was_censored,
            }

        finally:
            if task_id and not task_terminal:
                await asyncio.shield(_cancel_image_task(task_id, active_api_key, client_agent))

