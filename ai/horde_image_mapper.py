"""
AI Horde Image Generation Mapper Layer (`reGen` / SDXL / CivitAI LoRAs / SD 1.5).

Maps Direct UI image selections (4 base model pills, predefined & user-added CivitAI
character/style LoRAs, verbal persona passports, reGen-native sampling sliders,
custom negative prompts, and aspect ratios) onto AI Horde's `/api/v2/generate/async`
network following official Haidra / `horde-worker-reGen` best practices:

- **Owner / Paid Tiers (`Modal Krea 2`)**: Handled upstream by Modal Cloud GPU;
  uses this mapper only if Express SDXL is explicitly invoked while local ComfyUI is offline.
- **Guests / Free Users**:
  1. Hardcoded SFW enforcement (`nsfw=False`, `censor_nsfw=True`, `replacement_filter=True`,
     SFW negative prompt armor, and post-generation `censored` verification).
  2. Priority queue via `HORDE_WORKER_API_KEY` up to configurable daily Kudos caps
     (`HORDE_GUEST_DAILY_KUDOS_LIMIT` global + `HORDE_PER_USER_DAILY_KUDOS_LIMIT` per user).
  3. Worker-aware SDXL + CivitAI LoRA routing (`slow_workers=False`, live `lora=True`
     worker filtering, Haidra `247778` 8-step TurboMix or 18-step `k_dpmpp_2m Karras` + CAS).
  4. Automatic zero-cost fallback to anonymous (`0000000000`) SD 1.5 (`<= 512x640`)
     whenever the daily Kudos cap is reached or `KudosUpfront` is returned.
"""

from __future__ import annotations

import asyncio
import base64
import datetime
import hashlib
import io
import json
import logging
import os
import re
import threading
import time
import urllib.parse
from typing import Any, Awaitable, Callable, Dict, List, Optional, Tuple

import aiohttp
from PIL import Image, ImageFilter

from config import SETTINGS
import horde_client

logger = logging.getLogger("horde_image_mapper")

DEFAULT_CLIENT_AGENT = horde_client.DEFAULT_CLIENT_AGENT
ANONYMOUS_API_KEY = horde_client.ANONYMOUS_API_KEY

# Official Haidra pre-cached 12.4 MB SDXL 8-step accelerator (`AI-Horde-Styles`: `sdxl-photo`)
HORDE_TURBOMIX_LORA_VERSION_ID = "247778"
# Official CivitAI `Detail Tweaker XL` (`add-detail-xl.safetensors`, 217.9 MB)
HORDE_DETAIL_TWEAKER_XL_VERSION_ID = "135867"
MAX_HORDE_LORAS = 5

# Verified predefined SDXL CivitAI LoRAs (all <= 400 MB, primary .safetensors with SHA256)
SDXL_CIVITAI_LORA_REGISTRY: Dict[str, Dict[str, Any]] = {
    # --- Predefined Companion Character LoRAs ---
    "sdxl_judith": {
        "id": "sdxl_judith",
        "name": "Judith",
        "civitai_name": "Judith B",
        "civitai_model_id": "2081266",
        "civitai_version_id": "2360657",
        "local_filename": "JudithCB_r2-000011.safetensors",
        "base_model": "Pony",
        "category": "character",
        "strength": 1.0,
        "trained_words": ["JudithCB"],
        "signature_traits": "sleek blonde hair, emerald green eyes, subtle freckles",
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/ac70cb91-ccd3-4601-8754-f1d5ea5b77d3/width=256/108221436.jpeg",
        "description": "Sleek blonde companion LoRA with green eyes & freckles (Judith B)",
    },
    "sdxl_domi": {
        "id": "sdxl_domi",
        "name": "Domi",
        "civitai_name": "SLDomi",
        "civitai_model_id": "589180",
        "civitai_version_id": "1377183",
        "local_filename": "XLDomiv2.safetensors",
        "base_model": "SDXL 1.0",
        "category": "character",
        "strength": 1.0,
        "trained_words": ["Domi"],
        "signature_traits": "blonde hair, blue eyes",
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/2dd69f45-69f3-4b25-9f02-67c0610765c5/width=256/56075413.jpeg",
        "description": "Charismatic blonde companion LoRA with blue eyes (SLDomi)",
    },
    "sdxl_lana": {
        "id": "sdxl_lana",
        "name": "Lana",
        "civitai_name": "Svetlana XL",
        "civitai_model_id": "2310427",
        "civitai_version_id": "2599427",
        "local_filename": "svetlana.safetensors",
        "base_model": "SDXL 1.0",
        "category": "character",
        "strength": 1.0,
        "trained_words": ["Svetlana"],
        "signature_traits": "copper auburn hair, light green-blue eyes",
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/c060b0ae-6f46-4968-a61a-5565df48d27d/width=256/117559750.jpeg",
        "description": "Radiant Slavic auburn-haired companion LoRA (Svetlana)",
    },
    "sdxl_oriana": {
        "id": "sdxl_oriana",
        "name": "Oriana",
        "civitai_name": "ORIANA-XSTAR-XL",
        "civitai_model_id": "2584778",
        "civitai_version_id": "2903793",
        "local_filename": "ORIANA-XSTAR-XL-FINAL.safetensors",
        "base_model": "SDXL 1.0",
        "category": "character",
        "strength": 1.0,
        "trained_words": ["OR14N4"],
        "signature_traits": "long honey blonde hair, blue eyes",
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/e9f4420d-87a8-4314-937a-063324438379/width=256/129028954.jpeg",
        "description": "Glamorous honey-blonde companion LoRA (ORIANA-XSTAR-XL)",
    },
    "sdxl_vera": {
        "id": "sdxl_vera",
        "name": "Vera",
        "civitai_name": "VEROTIKA-XSTAR-XL",
        "civitai_model_id": "1009099",
        "civitai_version_id": "1130986",
        "local_filename": "VEROTIKA-XSTAR-XL-FINAL.safetensors",
        "base_model": "SDXL 1.0",
        "category": "character",
        "strength": 1.0,
        "trained_words": ["V3r0t1k4"],
        "signature_traits": "vibrant fiery red hair, blue eyes",
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/b98edfe9-1294-4c4a-8814-2d3ffb1f068a/width=256/43775214.jpeg",
        "description": "Striking fiery redhead companion LoRA (VEROTIKA-XSTAR-XL)",
    },
    "sdxl_bianca": {
        "id": "sdxl_bianca",
        "name": "Bianca",
        "civitai_name": "BIANCA-XSTAR-XL",
        "civitai_model_id": "1068237",
        "civitai_version_id": "1198981",
        "local_filename": "BIANCA-XSTAR-XL.safetensors",
        "base_model": "SDXL 1.0",
        "category": "character",
        "strength": 1.0,
        "trained_words": ["B1anc4"],
        "signature_traits": "short platinum blonde bob hair, blue eyes",
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/5981dc82-5c87-494d-8bca-a12822a9117a/width=256/47386274.jpeg",
        "description": "Edgy short platinum-blonde companion LoRA (BIANCA-XSTAR-XL)",
    },
    "sdxl_beatrice": {
        "id": "sdxl_beatrice",
        "name": "Beatrice",
        "civitai_name": "BEATRICE-XSTAR-XL",
        "civitai_model_id": "2322656",
        "civitai_version_id": "2612904",
        "local_filename": "BEATRICE-XSTAR-XL-FINAL.safetensors",
        "base_model": "SDXL 1.0",
        "category": "character",
        "strength": 1.0,
        "trained_words": ["B3ATR1C3"],
        "signature_traits": "warm golden blonde wavy hair, arched eyebrows, blue eyes",
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/8cce104f-1c65-465d-b609-b4abebcc778b/width=256/118088057.jpeg",
        "description": "Glamorous golden-blonde European companion LoRA (BEATRICE-XSTAR-XL)",
    },
    "sdxl_minerva": {
        "id": "sdxl_minerva",
        "name": "Minerva",
        "civitai_name": "MINERVA-XSTAR-XL",
        "civitai_model_id": "2497628",
        "civitai_version_id": "2807620",
        "local_filename": "MINERVA-XSTAR-XL-FINAL.safetensors",
        "base_model": "SDXL 1.0",
        "category": "character",
        "strength": 1.0,
        "trained_words": ["MINERVA"],
        "signature_traits": "icy platinum blonde hair, blue eyes, subtle freckles",
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/c0161468-55af-4384-8cbb-10305f57e099/width=256/125529679.jpeg",
        "description": "Icy platinum-blonde freckled companion LoRA (MINERVA-XSTAR-XL)",
    },
    # --- Predefined Style / Concept LoRAs ---
    "sdxl_lora_real_humans": {
        "id": "sdxl_lora_real_humans",
        "name": "Real Humans",
        "civitai_name": "Real Humans",
        "civitai_model_id": "232746",
        "civitai_version_id": "262705",
        "local_filename": "real-humans-PublicPrompts.safetensors",
        "base_model": "SDXL 1.0",
        "category": "style",
        "strength": 0.7,
        "trained_words": ["photo", "portrait photo"],
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/e9fdda22-7af8-4e91-84ca-641caff8d41c/width=256/4587751.jpeg",
        "description": "Authentic natural skin texture & candid photography",
    },
    "sdxl_lora_cinematic_shot": {
        "id": "sdxl_lora_cinematic_shot",
        "name": "Cinematic Shot",
        "civitai_name": "Juggernaut Cinematic XL LoRA",
        "civitai_model_id": "120663",
        "civitai_version_id": "131991",
        "base_model": "SDXL 1.0",
        "category": "style",
        "strength": 0.75,
        "trained_words": ["Movie Still", "Film Still"],
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/60a7446c-62ee-4238-8687-dffa7324f844/width=256/1844260.jpeg",
        "description": "Atmospheric 35mm movie lighting, depth & color grading",
    },
    "sdxl_lora_anime_screencap": {
        "id": "sdxl_lora_anime_screencap",
        "name": "Anime Aesthetic XL",
        "civitai_name": "Aesthetic Anime LoRA",
        "civitai_model_id": "295100",
        "civitai_version_id": "331598",
        "base_model": "SDXL 1.0",
        "category": "style",
        "strength": 0.8,
        "trained_words": [],
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/7cd96ef0-a9dc-4906-b258-f389ff640a1a/width=256/6328373.jpeg",
        "description": "Crisp modern anime broadcast aesthetic & cel shading",
    },
    "sdxl_lora_eldritch_comics": {
        "id": "sdxl_lora_eldritch_comics",
        "name": "Comic Illustration XL",
        "civitai_name": "Eldritch Comics",
        "civitai_model_id": "271002",
        "civitai_version_id": "305491",
        "base_model": "SDXL 1.0",
        "category": "style",
        "strength": 0.8,
        "trained_words": ["comic book"],
        "image_url": "https://image.civitai.com/xG1nkqKTMzGDvpLrqFT7WA/09e5cda5-31bb-4601-8e71-462b1ebc7ba6/width=256/5678307.jpeg",
        "description": "Expressive graphic novel ink linework & vibrant colors",
    },
}

# Short aliases -> canonical registry keys (works for both RP chat `<domi>` and Studio `sdxl_domi`)
LORA_ALIAS_MAP: Dict[str, str] = {
    "judith": "sdxl_judith",
    "sdxl_judith": "sdxl_judith",
    "judithcb_r2-000011": "sdxl_judith",
    "judithcb_r2-000011.safetensors": "sdxl_judith",
    "domi": "sdxl_domi",
    "sdxl_domi": "sdxl_domi",
    "xldomiv2": "sdxl_domi",
    "xldomiv2.safetensors": "sdxl_domi",
    "lana": "sdxl_lana",
    "sdxl_lana": "sdxl_lana",
    "svetlana": "sdxl_lana",
    "svetlana.safetensors": "sdxl_lana",
    "oriana": "sdxl_oriana",
    "sdxl_oriana": "sdxl_oriana",
    "oriana-xstar-xl-final": "sdxl_oriana",
    "oriana-xstar-xl-final.safetensors": "sdxl_oriana",
    "vera": "sdxl_vera",
    "sdxl_vera": "sdxl_vera",
    "verotika": "sdxl_vera",
    "verotika-xstar-xl-final": "sdxl_vera",
    "verotika-xstar-xl-final.safetensors": "sdxl_vera",
    "bianca": "sdxl_bianca",
    "sdxl_bianca": "sdxl_bianca",
    "bianca-xstar-xl": "sdxl_bianca",
    "bianca-xstar-xl.safetensors": "sdxl_bianca",
    "beatrice": "sdxl_beatrice",
    "sdxl_beatrice": "sdxl_beatrice",
    "beatrice-xstar-xl-final": "sdxl_beatrice",
    "beatrice-xstar-xl-final.safetensors": "sdxl_beatrice",
    "minerva": "sdxl_minerva",
    "sdxl_minerva": "sdxl_minerva",
    "minerva-xstar-xl-final": "sdxl_minerva",
    "minerva-xstar-xl-final.safetensors": "sdxl_minerva",
    "real_humans": "sdxl_lora_real_humans",
    "real humans": "sdxl_lora_real_humans",
    "sdxl_lora_real_humans": "sdxl_lora_real_humans",
    "real-humans-publicprompts": "sdxl_lora_real_humans",
    "real-humans-publicprompts.safetensors": "sdxl_lora_real_humans",
    "sdxl_lora_cinematic_shot": "sdxl_lora_cinematic_shot",
    "cinematic_shot": "sdxl_lora_cinematic_shot",
    "sdxl_lora_anime_screencap": "sdxl_lora_anime_screencap",
    "anime_screencap": "sdxl_lora_anime_screencap",
    "sdxl_lora_eldritch_comics": "sdxl_lora_eldritch_comics",
    "eldritch_comics": "sdxl_lora_eldritch_comics",
}

# 4 Main UI Base Model Pills -> Official AI Horde SDXL Checkpoints
STYLE_FAMILY_SDXL_MODELS: Dict[str, List[str]] = {
    "realistic": [
        "Juggernaut XL",
        "AlbedoBase XL (SDXL)",
        "AlbedoBase XL 3.1",
        "ICBINP XL",
        "CyberRealistic Pony",
    ],
    "perfect": [
        "CyberRealistic Pony",
        "Pony Realism",
        "Juggernaut XL",
        "AlbedoBase XL (SDXL)",
    ],
    "fantasy": [
        "WAI-NSFW-illustrious-SDXL",
        "Juggernaut XL",
        "AlbedoBase XL (SDXL)",
        "DreamShaper XL",
    ],
    "anime": [
        "Nova Anime XL",
        "WAI-NSFW-illustrious-SDXL",
        "Mistoon Anime",
        "Animagine XL",
        "AlbedoBase XL (SDXL)",
    ],
    "semireal": [
        "CyberRealistic Pony",
        "Juggernaut XL",
        "AlbedoBase XL (SDXL)",
        "WAI-NSFW-illustrious-SDXL",
    ],
}

STYLE_FAMILY_SD15_MODELS: Dict[str, List[str]] = {
    "realistic": [
        "ICBINP - I Can't Believe It's Not Photography",
        "AbsoluteReality",
        "Deliberate",
        "Realistic Vision",
        "stable_diffusion",
    ],
    "perfect": [
        "Deliberate",
        "AbsoluteReality",
        "ICBINP - I Can't Believe It's Not Photography",
        "Dreamshaper",
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

# Maps UI base_model / style identifiers to (family, natural prompt modifier)
UI_STYLE_MAP: Dict[str, Tuple[str, str]] = {
    "none": ("realistic", ""),
    "free": ("realistic", ""),
    "realistic": (
        "realistic",
        "photorealistic, candid natural photography, detailed skin texture, natural lighting, sharp focus",
    ),
    "photorealistic": (
        "realistic",
        "photorealistic, candid natural photography, detailed skin texture, natural daylight, sharp focus",
    ),
    "sdxl_realistic": (
        "realistic",
        "photorealistic, natural lighting, detailed skin texture, sharp focus",
    ),
    "sdxl": (
        "realistic",
        "high detail, crisp focus, natural lighting",
    ),
    "perfect": (
        "perfect",
        "masterpiece portrait, expressive features, dynamic studio lighting, ultra detailed",
    ),
    "sdxl_perfect": (
        "perfect",
        "masterpiece portrait, expressive features, dynamic studio lighting, ultra detailed",
    ),
    "fantasy": (
        "fantasy",
        "masterpiece illustration, atmospheric magical lighting, rich intricate details",
    ),
    "sdxl_fantasy": (
        "fantasy",
        "masterpiece illustration, atmospheric magical lighting, rich intricate details",
    ),
    "midjourney": (
        "fantasy",
        "artistic concept art, ethereal atmospheric lighting, dramatic composition, masterpiece",
    ),
    "tooned": (
        "anime",
        "vibrant stylized anime illustration, clean detailed linework, expressive eyes, rich colors",
    ),
    "sdxl_tooned": (
        "anime",
        "vibrant stylized anime illustration, clean detailed linework, expressive eyes, rich colors",
    ),
    "anime": (
        "anime",
        "vibrant masterwork anime illustration, clean detailed linework, expressive eyes, cel-shaded aesthetics",
    ),
    "comic": (
        "anime",
        "modern comic book illustration, dynamic bold ink linework, graphic novel cel shading, vibrant colors",
    ),
    "nix_comic": (
        "anime",
        "expressive western comic book art, clean ink linework, vibrant cel-shaded colors",
    ),
    "cinematic": (
        "realistic",
        "cinematic movie still, 35mm film photography, dramatic lighting, anamorphic bokeh",
    ),
    "semireal": (
        "semireal",
        "2.5d semi-realistic digital art, soft cinematic lighting, refined stylized features",
    ),
    "art": (
        "semireal",
        "artistic digital illustration, painterly lighting, expressive composition",
    ),
    "pinup": (
        "semireal",
        "vintage 1950s technicolor pin-up art style, retro glamour, warm painterly studio lighting",
    ),
    "retro": (
        "semireal",
        "vintage retro technicolor illustration, warm nostalgic tones, painterly lighting",
    ),
    "pixar": (
        "semireal",
        "3d animated movie style, expressive stylized character, soft subsurface scattering",
    ),
    "3d": (
        "semireal",
        "3d stylized digital render, expressive character design, soft studio lighting",
    ),
    "neon": (
        "fantasy",
        "neon noir cyberpunk aesthetic, vibrant cyan and magenta rim light, night city reflections",
    ),
    "cyberpunk": (
        "fantasy",
        "cyberpunk night aesthetic, neon reflections, atmospheric volumetric glow",
    ),
    # Predefined CivitAI style LoRAs
    "sdxl_lora_real_humans": (
        "realistic",
        "photo, portrait photo, authentic natural skin texture, raw daylight photography",
    ),
    "real_humans": (
        "realistic",
        "photo, portrait photo, authentic natural skin texture, raw daylight photography",
    ),
    "real humans": (
        "realistic",
        "photo, portrait photo, authentic natural skin texture, raw daylight photography",
    ),
    "sdxl_lora_cinematic_shot": (
        "realistic",
        "cinematic shot, 35mm film grain, dramatic atmospheric lighting, shallow depth of field",
    ),
    "sdxl_lora_anime_screencap": (
        "anime",
        "anime screencap, anime coloring, crisp cel shading",
    ),
    "sdxl_lora_eldritch_comics": (
        "anime",
        "comic book illustration, expressive ink linework",
    ),
}

# Verbal fallback descriptions (used on anonymous SD 1.5 fallback or for non-SDXL archetypes)
ARCHETYPE_VISUAL_TRAITS: Dict[str, Tuple[str, Optional[str]]] = {
    "judith": ("expressive young woman with sleek blonde hair, emerald green eyes, subtle freckles, charming smile", None),
    "sdxl_judith": ("expressive young woman with sleek blonde hair, emerald green eyes, subtle freckles, charming smile", None),
    "june": ("pretty young woman with long wavy auburn hair, bright emerald-green eyes, subtle freckles across nose", None),
    "domi": ("charismatic stylish woman with layered blonde hair, expressive blue eyes, modern chic look", None),
    "sdxl_domi": ("charismatic stylish woman with layered blonde hair, expressive blue eyes, modern chic look", None),
    "lana": ("radiant Slavic young woman with copper auburn hair, expressive green-blue eyes, warm natural beauty", None),
    "sdxl_lana": ("radiant Slavic young woman with copper auburn hair, expressive green-blue eyes, warm natural beauty", None),
    "oriana": ("glamorous confident woman with long honey blonde hair, bright blue eyes, sculpted features", None),
    "sdxl_oriana": ("glamorous confident woman with long honey blonde hair, bright blue eyes, sculpted features", None),
    "vera": ("striking elegant woman with vibrant fiery red hair, piercing blue eyes, refined features", None),
    "sdxl_vera": ("striking elegant woman with vibrant fiery red hair, piercing blue eyes, refined features", None),
    "bianca": ("edgy poised woman with short platinum blonde bob hair, striking blue eyes, refined features", None),
    "sdxl_bianca": ("edgy poised woman with short platinum blonde bob hair, striking blue eyes, refined features", None),
    "beatrice": ("glamorous European woman with warm golden blonde wavy hair, arched eyebrows, blue eyes", None),
    "sdxl_beatrice": ("glamorous European woman with warm golden blonde wavy hair, arched eyebrows, blue eyes", None),
    "minerva": ("striking young woman with icy platinum blonde hair, blue eyes, subtle freckles, full sculpted lips", None),
    "sdxl_minerva": ("striking young woman with icy platinum blonde hair, blue eyes, subtle freckles, full sculpted lips", None),
    "ada": ("confident woman with short black bob hair, elegant crimson dress, poised gaze", None),
    "ada_wong": ("confident woman with short black bob hair, elegant crimson dress, poised gaze", None),
    "brie": ("cheerful blonde woman, bright blue eyes, warm radiant smile", None),
    "kxbrie": ("cheerful blonde woman, bright blue eyes, warm radiant smile", None),
    "tifa": ("athletic young woman with long dark brown hair and ruby-red eyes, confident expression", None),
    "tifa_lockhart": ("athletic young woman with long dark brown hair and ruby-red eyes, confident expression", None),
    "alt": ("alternative goth aesthetic young woman, dark eyeliner, choker, subtle colored hair highlights", "fantasy"),
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
    "rating_explicit, rating_questionable, nsfw, nude, naked, explicit, questionable, topless, "
    "nipples, pokies, erect nipples, nipples through clothes, visible nipples, areola, no bra, braless, "
    "genitalia, underboob, sideboob, deep cleavage, plunging neckline, open shirt, unbuttoned shirt, "
    "sheer clothing, see-through, transparent clothes, wet clothes, microskirt, upskirt, "
    "underwear, lingerie, bikini slip, sexual, erotic, lewd, suggestive, uncensored, fetishes, gore"
)

PONY_MODEL_NAMES = {
    "CyberRealistic Pony",
    "Pony Realism",
    "Pony Diffusion XL",
    "Prefect Pony",
    "AMPonyXL",
    "WAI-CUTE Pony",
    "WAI-ANI-NSFW-PONYXL",
}

SDXL_COMPATIBLE_BASE_MODELS = {
    "SDXL 1.0",
    "SDXL Turbo",
    "SDXL Lightning",
    "SDXL Hyper",
    "SDXL Distilled",
    "Pony",
    "Illustrious",
    "NoobAI",
}

# Synthetic private Krea-only LoRA tokens to strip if they appear in raw prompts
_PRIVATE_TRIGGER_RE = re.compile(
    r"\b(?:DomiK4?|kxbrie|KR2Pix|Lun4|sysdeep_2b|CPELucy|sysdeep_asuka|Bradhamel\s+art\s+style|famegrid\s+style)\b,?\s*",
    flags=re.IGNORECASE,
)
_ANGLE_TAG_RE = re.compile(r"<[^>]+>")
_CIVITAI_CUSTOM_ID_RE = re.compile(r"^(?:civitai[_:])?(\d{4,12})$", flags=re.IGNORECASE)
_EXPLICIT_HAIR_CUE_RE = re.compile(
    r"\b(?:"
    r"blond|blonde|brunette|redhead|ginger|auburn|raven[- ]hair|jet[- ]black hair|"
    r"(?:black|brown|dark|red|pink|blue|purple|green|silver|white|grey|gray|platinum|chestnut|golden|copper)[- ]hair(?:ed)?|"
    r"hair\s+(?:is\s+)?(?:black|brown|dark|blonde?|red|pink|blue|silver|white|short|long)|"
    r"short\s+hair|pixie\s+cut|bob\s+cut|buzz\s*cut|bald|"
    r"блондин\w*|брюнет\w*|шатен\w*|рыж\w*|черноволос\w*|темноволос\w*|светловолос\w*|русы\w*|каштанов\w*|пепельн\w*|седы\w*|седовлас\w*"
    r")\b",
    flags=re.IGNORECASE,
)


def has_explicit_hair_cue(text: str) -> bool:
    """Returns True if the user/persona prompt already specifies a hair color or haircut."""
    return bool(_EXPLICIT_HAIR_CUE_RE.search(str(text or "")))

# Caches for AI Horde online image models, live worker capabilities, and CivitAI searches
_IMAGE_MODELS_CACHE_TTL = 60.0
_image_models_cache: Tuple[float, Dict[str, Dict[str, Any]]] = (0.0, {})
_worker_caps_cache: Tuple[float, Dict[str, Dict[str, Any]]] = (0.0, {})
_CIVITAI_SEARCH_CACHE_TTL = 300.0
_civitai_search_cache: Dict[str, Tuple[float, List[Dict[str, Any]]]] = {}

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


def get_predefined_sdxl_presets() -> Dict[str, List[Dict[str, Any]]]:
    """Returns enriched SDXL style & character LoRA definitions with CivitAI metadata and thumbnails."""
    art_styles: List[Dict[str, Any]] = [
        {
            "id": "none",
            "name": "Free",
            "description": "Pure base checkpoint guided by your prompt",
            "icon": "",
            "filename": "",
            "url": "",
            "image_url": "",
            "civitai_version_id": "",
            "civitai_name": "",
            "trained_words": [],
            "trigger": "",
            "strength": 0.0,
            "models": ["all"],
        }
    ]
    archetypes: List[Dict[str, Any]] = []

    for key, item in SDXL_CIVITAI_LORA_REGISTRY.items():
        entry = {
            "id": item["id"],
            "name": item["name"],
            "civitai_name": item["civitai_name"],
            "civitai_model_id": item["civitai_model_id"],
            "civitai_version_id": item["civitai_version_id"],
            "base_model": item["base_model"],
            "category": item["category"],
            "description": item["description"],
            "icon": "",
            "filename": item.get("local_filename") or f"civitai_{item['civitai_version_id']}.safetensors",
            "url": f"https://civitai.com/models/{item['civitai_model_id']}?modelVersionId={item['civitai_version_id']}",
            "image_url": item["image_url"],
            "trained_words": list(item.get("trained_words") or []),
            "trigger": f"<{item['name']}>",
            "strength": float(item["strength"]),
            "models": ["sdxl"],
        }
        if item["category"] == "style":
            art_styles.append(entry)
        else:
            archetypes.append(entry)

    return {"art_styles": art_styles, "archetypes": archetypes}


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
    """Returns True only when HORDE_IMAGE_FORCE_FOR_GUESTS is explicitly enabled to bypass local ComfyUI."""
    return is_horde_image_enabled() and _truthy(
        SETTINGS.get("HORDE_IMAGE_FORCE_FOR_GUESTS", "false"), default=False
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


def _parse_mps(perf_str: Any) -> float:
    m = re.search(r"([0-9]+(?:\.[0-9]+)?)", str(perf_str or ""))
    return float(m.group(1)) if m else 0.0


async def fetch_online_image_models(
    session: aiohttp.ClientSession,
    client_agent: str = DEFAULT_CLIENT_AGENT,
    force_refresh: bool = False,
) -> Dict[str, Dict[str, Any]]:
    """
    Fetches and caches live AI Horde image models enriched with worker-level LoRA
    and speed (`fast_lora_workers`) capabilities for 60 seconds.
    """
    global _image_models_cache, _worker_caps_cache
    now = time.monotonic()
    cached_ts, cached_map = _image_models_cache
    if not force_refresh and cached_map and (now - cached_ts) < _IMAGE_MODELS_CACHE_TTL:
        return cached_map

    base = horde_client.HORDE_BASE_URL.rstrip("/")
    headers = {"Client-Agent": client_agent, "User-Agent": "Direct/1.0"}
    online: Dict[str, Dict[str, Any]] = {}

    try:
        async with session.get(
            f"{base}/status/models?type=image",
            headers=headers,
            timeout=aiohttp.ClientTimeout(total=8.0),
        ) as resp:
            if resp.status == 200:
                items = await resp.json()
                if isinstance(items, list):
                    for m in items:
                        if not isinstance(m, dict):
                            continue
                        name = str(m.get("name") or "").strip()
                        cnt = int(m.get("count") or 0)
                        if name and cnt > 0:
                            online[name] = dict(m)
    except Exception as e:
        logger.debug(f"[HordeMapper] Could not refresh online image models: {e}")

    # Enrich with live worker LoRA & speed capabilities
    try:
        async with session.get(
            f"{base}/workers?type=image",
            headers=headers,
            timeout=aiohttp.ClientTimeout(total=8.0),
        ) as w_resp:
            if w_resp.status == 200:
                workers = await w_resp.json()
                if isinstance(workers, list):
                    model_caps: Dict[str, Dict[str, Any]] = {}
                    for w in workers:
                        if not isinstance(w, dict) or not w.get("online") or w.get("maintenance_mode"):
                            continue
                        has_lora = bool(w.get("lora"))
                        mps = _parse_mps(w.get("performance"))
                        is_fast = mps >= 0.45 and not bool(w.get("extra_slow_worker"))
                        for mname in (w.get("models") or []):
                            st = model_caps.setdefault(
                                mname,
                                {"lora_workers": 0, "fast_lora_workers": 0, "fast_workers": 0, "max_mps": 0.0},
                            )
                            if is_fast:
                                st["fast_workers"] += 1
                            if has_lora:
                                st["lora_workers"] += 1
                                if is_fast:
                                    st["fast_lora_workers"] += 1
                            if mps > st["max_mps"]:
                                st["max_mps"] = mps
                    _worker_caps_cache = (now, model_caps)
                    for mname, caps in model_caps.items():
                        if mname in online:
                            online[mname].update(caps)
    except Exception as e:
        logger.debug(f"[HordeMapper] Could not enrich worker LoRA capabilities: {e}")

    if online:
        _image_models_cache = (now, online)
        return online
    return cached_map


def select_online_models_for_family(
    family: str,
    use_sdxl: bool,
    online_models: Dict[str, Dict[str, Any]],
    require_lora: bool = False,
    max_models: int = 3,
) -> List[str]:
    """
    Picks up to `max_models` live AI Horde models matching `family` (`realistic`,
    `perfect`, `fantasy`, `anime`, `semireal`), preferring curated priority order,
    low queue ETA, and active fast LoRA workers when `require_lora=True`.
    """
    pool = (
        STYLE_FAMILY_SDXL_MODELS.get(family) or STYLE_FAMILY_SDXL_MODELS["realistic"]
        if use_sdxl
        else STYLE_FAMILY_SD15_MODELS.get(family) or STYLE_FAMILY_SD15_MODELS["realistic"]
    )
    if not online_models:
        return list(pool[:max_models])

    def _has_worker(mname: str) -> bool:
        info = online_models.get(mname)
        if not info:
            return False
        if require_lora and "lora_workers" in info:
            return int(info.get("lora_workers") or 0) > 0
        return int(info.get("count") or 0) > 0

    available = [m for m in pool if _has_worker(m)]
    if not available and use_sdxl and require_lora:
        # Relax across all SDXL realistic/fantasy models that currently have LoRA workers
        fallback_sdxl = []
        for fam_list in STYLE_FAMILY_SDXL_MODELS.values():
            for cand in fam_list:
                if cand not in fallback_sdxl and _has_worker(cand):
                    fallback_sdxl.append(cand)
        available = fallback_sdxl
    if not available and use_sdxl:
        available = [m for m in pool if m in online_models] or [
            m for m in STYLE_FAMILY_SDXL_MODELS["realistic"] if m in online_models
        ]
    if not available:
        available = [m for m in STYLE_FAMILY_SD15_MODELS["realistic"] if m in online_models]
    if not available:
        return list(pool[:max_models])

    def _score(model_name: str) -> Tuple[int, int, float]:
        info = online_models.get(model_name) or {}
        fast_lora = int(info.get("fast_lora_workers") or info.get("lora_workers") or info.get("count") or 0)
        eta = float(info.get("eta") or 0.0)
        curated_rank = (len(pool) - pool.index(model_name)) if model_name in pool else 0
        # Penalize heavily queued models (>90s ETA) while preserving our curated checkpoint fidelity
        not_congested = 1 if eta <= 75.0 else 0
        has_fast = 1 if fast_lora >= 1 else 0
        return (has_fast + not_congested, curated_rank, -eta)

    ranked = sorted(available, key=_score, reverse=True)
    return ranked[:max_models]


def _extract_clean_prompt_and_tags(raw_prompt: str) -> Tuple[str, List[str]]:
    text = str(raw_prompt or "").strip()
    if "Image:" in text:
        text = text.split("Image:", 1)[1].strip()
    elif text.lower().startswith("title:") and "\n" in text:
        text = text.split("\n", 1)[1].strip()

    text = text.replace("###", ", ")
    tags = [t.strip().lower() for t in re.findall(r"<([^>]+)>", text) if t.strip()]
    clean = _ANGLE_TAG_RE.sub(" ", text)
    clean = _PRIVATE_TRIGGER_RE.sub("", clean)
    clean = re.sub(r"\s+", " ", clean).strip(" ,")
    return clean, tags


def _resolve_dimensions(width: int, height: int, use_sdxl: bool) -> Tuple[int, int]:
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


def _resolve_lora_spec(
    raw_id: str,
    custom_lookup: Dict[str, Dict[str, Any]],
) -> Optional[Dict[str, Any]]:
    """
    Resolves an archetype/style identifier (`sdxl_domi`, `domi`, `XLDomiv2.safetensors`,
    `civitai_1377183`, or user-added custom CivitAI LoRA) into a normalized CivitAI LoRA spec.
    """
    cand = str(raw_id or "").strip().strip("<> ").lower()
    if not cand or cand in ("none", "free", "prompt"):
        return None

    if cand in custom_lookup:
        return custom_lookup[cand]

    canonical = LORA_ALIAS_MAP.get(cand)
    if canonical and canonical in SDXL_CIVITAI_LORA_REGISTRY:
        return SDXL_CIVITAI_LORA_REGISTRY[canonical]

    m = _CIVITAI_CUSTOM_ID_RE.match(cand.replace(".safetensors", ""))
    if m:
        vid = m.group(1)
        if f"civitai_{vid}" in custom_lookup:
            return custom_lookup[f"civitai_{vid}"]
        return {
            "id": f"civitai_{vid}",
            "name": f"LoRA {vid}",
            "civitai_version_id": vid,
            "strength": 0.85,
            "trained_words": [],
            "inject_any": True,
        }
    return None


def map_ui_to_horde_request(
    prompt: str,
    persona_prompt: str = "",
    sliders: Optional[Dict[str, Any]] = None,
    style_setting: str = "",
    width: int = 896,
    height: int = 1152,
    steps: Optional[int] = None,
    cfg: Optional[float] = None,
    seed: Optional[int] = None,
    persona_lora: Optional[str] = None,
    persona_name: Optional[str] = None,
    is_uncensored: bool = False,
    use_sdxl: bool = True,
    online_models: Optional[Dict[str, Dict[str, Any]]] = None,
) -> Dict[str, Any]:
    """
    Translates Direct UI parameters (Base Model pill, predefined & custom CivitAI
    Character/Style LoRAs, steps/cfg/detail sliders, custom negative prompt) into a
    validated AI Horde `/api/v2/generate/async` JSON payload.
    """
    sliders = sliders or {}
    clean_scene, prompt_tags = _extract_clean_prompt_and_tags(prompt)
    clean_persona, persona_tags = _extract_clean_prompt_and_tags(persona_prompt)
    all_inline_tags = prompt_tags + [t for t in persona_tags if t not in prompt_tags]

    # Index any user-supplied custom CivitAI LoRAs from UI / Persona render_preset
    custom_lookup: Dict[str, Dict[str, Any]] = {}
    raw_custom_list = sliders.get("custom_loras")
    if isinstance(raw_custom_list, list):
        for item in raw_custom_list:
            if not isinstance(item, dict):
                continue
            vid = str(item.get("civitai_version_id") or item.get("version_id") or "").strip()
            cid = str(item.get("id") or (f"civitai_{vid}" if vid else "")).strip().lower()
            if cid and vid.isdigit():
                spec = {
                    "id": cid,
                    "name": str(item.get("name") or f"LoRA {vid}"),
                    "civitai_version_id": vid,
                    "strength": float(item.get("strength") or 0.85),
                    "trained_words": [str(w).strip() for w in (item.get("trained_words") or []) if str(w).strip()],
                    "category": str(item.get("category") or "character"),
                }
                custom_lookup[cid] = spec
                custom_lookup[f"civitai_{vid}"] = spec
                custom_lookup[vid] = spec

    # 1. Resolve Base Model family (`realistic`, `perfect`, `fantasy`, `anime`) and style modifier
    base_candidates: List[str] = []
    for raw_b in (sliders.get("base_model"), style_setting):
        b_low = str(raw_b or "").strip().lower().strip("<> ")
        if b_low in UI_STYLE_MAP and b_low not in ("none", "free"):
            base_candidates.append(b_low)

    style_ids: List[str] = []
    if isinstance(sliders.get("styles"), list):
        for s in sliders["styles"]:
            s_clean = str(s or "").strip().lower()
            if s_clean and s_clean not in ("none", "free") and s_clean not in style_ids:
                style_ids.append(s_clean)
    elif sliders.get("style"):
        s_clean = str(sliders.get("style") or "").strip().lower()
        if s_clean and s_clean not in ("none", "free"):
            style_ids.append(s_clean)

    for t in all_inline_tags:
        if t in UI_STYLE_MAP and t not in ("none", "free") and t not in style_ids:
            style_ids.append(t)

    primary_base_key = base_candidates[0] if base_candidates else (
        next((s for s in style_ids if s in UI_STYLE_MAP), "realistic")
    )
    family, base_modifier = UI_STYLE_MAP.get(primary_base_key, UI_STYLE_MAP["realistic"])

    extra_style_modifiers: List[str] = []
    for sid in style_ids:
        if sid in UI_STYLE_MAP and sid != primary_base_key:
            s_fam, s_mod = UI_STYLE_MAP[sid]
            if s_mod and s_mod not in extra_style_modifiers and s_mod != base_modifier:
                extra_style_modifiers.append(s_mod)
            if not base_candidates and s_fam:
                family = s_fam

    # 2. Collect Character & Style LoRAs (for SDXL `reGen` workers) + verbal traits
    raw_archs: List[str] = []
    if isinstance(sliders.get("archetypes"), (list, tuple)):
        for a in sliders["archetypes"]:
            a_low = str(a or "").strip().lower()
            if a_low and a_low != "none" and a_low not in raw_archs:
                raw_archs.append(a_low)
    elif isinstance(sliders.get("archetype"), str) and sliders["archetype"].strip():
        for a in sliders["archetype"].split(","):
            a_low = a.strip().lower()
            if a_low and a_low != "none" and a_low not in raw_archs:
                raw_archs.append(a_low)

    for extra_cand in (persona_lora, sliders.get("lora")):
        if extra_cand:
            ec = str(extra_cand).strip().lower()
            if ec and ec not in raw_archs:
                raw_archs.append(ec)

    if not raw_archs and persona_name:
        pn_low = str(persona_name).strip().lower()
        if pn_low in LORA_ALIAS_MAP:
            raw_archs.append(LORA_ALIAS_MAP[pn_low])

    for t in all_inline_tags:
        t_clean = t.split(":", 1)[1].strip() if t.startswith("lora:") else t
        if (t_clean in LORA_ALIAS_MAP or t_clean in ARCHETYPE_VISUAL_TRAITS or _CIVITAI_CUSTOM_ID_RE.match(t_clean)) and t_clean not in raw_archs:
            raw_archs.append(t_clean)

    arch_weights = sliders.get("archetype_weights") if isinstance(sliders.get("archetype_weights"), dict) else {}
    style_weights = sliders.get("style_weights") if isinstance(sliders.get("style_weights"), dict) else {}

    horde_loras: List[Dict[str, Any]] = []
    seen_version_ids: set[str] = set()
    lora_trigger_words: List[str] = []
    arch_traits: List[str] = []

    def _lookup_weight(weight_dict: Dict[str, Any], raw_key: str, canonical_id: str, default_w: float) -> float:
        for k in (raw_key, canonical_id, raw_key.upper(), canonical_id.replace("sdxl_", "")):
            if k in weight_dict and weight_dict[k] is not None:
                try:
                    return round(max(0.1, min(1.5, float(weight_dict[k]))), 2)
                except Exception:
                    pass
        return round(float(default_w), 2)

    # Resolve Character LoRAs first (highest companion priority)
    for arch in raw_archs:
        spec = _resolve_lora_spec(arch, custom_lookup) if use_sdxl else None
        if spec and len(horde_loras) < MAX_HORDE_LORAS:
            vid = str(spec["civitai_version_id"])
            if vid not in seen_version_ids:
                w = _lookup_weight(arch_weights, arch, spec["id"], spec.get("strength", 0.9))
                lora_entry: Dict[str, Any] = {
                    "name": vid,
                    "model": w,
                    "clip": w,
                    "is_version": True,
                }
                t_words = spec.get("trained_words") or []
                if t_words:
                    for tw in t_words[:2]:
                        if tw and tw not in lora_trigger_words:
                            lora_trigger_words.append(tw)
                elif spec.get("inject_any"):
                    lora_entry["inject_trigger"] = "any"
                sig_traits = str(spec.get("signature_traits") or "").strip()
                if sig_traits and not has_explicit_hair_cue(f"{clean_scene} {clean_persona}"):
                    if sig_traits.lower() not in f"{clean_scene} {clean_persona}".lower() and sig_traits not in lora_trigger_words:
                        lora_trigger_words.append(sig_traits)
                horde_loras.append(lora_entry)
                seen_version_ids.add(vid)
        else:
            # Verbal trait fallback (when on SD 1.5 anonymous fallback or non-CivitAI preset)
            trait_entry = ARCHETYPE_VISUAL_TRAITS.get(arch)
            if trait_entry:
                trait_text, arch_family = trait_entry
                if trait_text and trait_text not in arch_traits:
                    arch_traits.append(trait_text)
                if arch_family and not base_candidates:
                    family = arch_family

    # Resolve Style / Concept LoRAs second
    for sid in style_ids:
        spec = _resolve_lora_spec(sid, custom_lookup) if use_sdxl else None
        if spec and len(horde_loras) < MAX_HORDE_LORAS:
            vid = str(spec["civitai_version_id"])
            if vid not in seen_version_ids:
                w = _lookup_weight(style_weights, sid, spec["id"], spec.get("strength", 0.75))
                lora_entry = {
                    "name": vid,
                    "model": w,
                    "clip": w,
                    "is_version": True,
                }
                t_words = spec.get("trained_words") or []
                if t_words:
                    for tw in t_words[:2]:
                        if tw and tw not in lora_trigger_words:
                            lora_trigger_words.append(tw)
                elif spec.get("inject_any"):
                    lora_entry["inject_trigger"] = "any"
                sig_traits = str(spec.get("signature_traits") or "").strip()
                if sig_traits and not has_explicit_hair_cue(f"{clean_scene} {clean_persona}"):
                    if sig_traits.lower() not in f"{clean_scene} {clean_persona}".lower() and sig_traits not in lora_trigger_words:
                        lora_trigger_words.append(sig_traits)
                horde_loras.append(lora_entry)
                seen_version_ids.add(vid)

    # 3. Resolve Steps, CFG, Sampler & TurboMix / Detail Tweaker XL LoRAs
    raw_steps = steps if steps is not None else sliders.get("steps")
    raw_cfg = cfg if cfg is not None else sliders.get("cfg")
    horde_native = bool(sliders.get("horde_native"))

    if not use_sdxl:
        eff_steps = 20
        eff_cfg = 6.5
        sampler_name = "k_euler_a"
        karras_flag = True
    else:
        parsed_steps = 18
        if raw_steps is not None:
            try:
                s_int = int(raw_steps)
                # Legacy DMD2 default (`6` or `7` with cfg <= 2.0 and without horde_native flag) -> map to 18 crisp Karras steps
                if not horde_native and s_int in (6, 7) and (raw_cfg is None or float(raw_cfg) <= 2.2):
                    parsed_steps = 18
                else:
                    parsed_steps = max(6, min(30, s_int))
            except Exception:
                parsed_steps = 18

        is_turbo_8step = parsed_steps <= 10
        if is_turbo_8step:
            eff_steps = max(6, min(10, parsed_steps))
            eff_cfg = 2.0
            sampler_name = "k_dpmpp_sde"
            karras_flag = True
            if (
                HORDE_TURBOMIX_LORA_VERSION_ID not in seen_version_ids
                and len(horde_loras) < MAX_HORDE_LORAS
            ):
                horde_loras.insert(
                    0,
                    {
                        "name": HORDE_TURBOMIX_LORA_VERSION_ID,
                        "model": 1.0,
                        "clip": 1.0,
                        "is_version": True,
                    },
                )
                seen_version_ids.add(HORDE_TURBOMIX_LORA_VERSION_ID)
        else:
            eff_steps = max(12, min(28, parsed_steps))
            sampler_name = "k_dpmpp_2m"
            karras_flag = True
            eff_cfg = 5.0
            if raw_cfg is not None:
                try:
                    c_flt = float(raw_cfg)
                    if not horde_native and c_flt <= 2.2:
                        eff_cfg = 5.0
                    else:
                        eff_cfg = round(max(2.5, min(8.0, c_flt)), 1)
                except Exception:
                    eff_cfg = 5.0

        # Detail Tweaker XL (`135867`) when user moves Detail slider away from neutral 0.5
        try:
            detail_val = float(sliders.get("detail", 0.5))
            if (
                abs(detail_val - 0.5) >= 0.15
                and HORDE_DETAIL_TWEAKER_XL_VERSION_ID not in seen_version_ids
                and len(horde_loras) < MAX_HORDE_LORAS
            ):
                # Map 0.0..1.0 -> 0.15..0.85 strength
                det_w = round(max(0.15, min(0.9, detail_val * 0.85)), 2)
                horde_loras.append(
                    {
                        "name": HORDE_DETAIL_TWEAKER_XL_VERSION_ID,
                        "model": det_w,
                        "clip": det_w,
                        "is_version": True,
                    }
                )
                seen_version_ids.add(HORDE_DETAIL_TWEAKER_XL_VERSION_ID)
        except Exception:
            pass

    # 4. Select online models (filtering to active LoRA-capable workers when `horde_loras` is non-empty)
    models = select_online_models_for_family(
        family=family,
        use_sdxl=use_sdxl,
        online_models=online_models or {},
        require_lora=bool(horde_loras),
        max_models=3 if horde_loras else 4,
    )

    primary_model = models[0] if models else ""
    uses_pony = use_sdxl and (primary_model in PONY_MODEL_NAMES)

    # 5. Compose Positive Prompt
    prompt_parts: List[str] = []
    if uses_pony:
        prompt_parts.append("score_9, score_8_up, score_7_up")
    for tw in lora_trigger_words:
        if tw.lower() not in clean_scene.lower():
            prompt_parts.append(tw)
    if clean_scene:
        prompt_parts.append(clean_scene)
    # Only inject long verbal persona appearance when no dedicated CivitAI character LoRA is active,
    # or keep short non-conflicting context so hair/eye color from verbal passport doesn't fight the LoRA
    has_char_civitai_lora = any(
        vid not in (HORDE_TURBOMIX_LORA_VERSION_ID, HORDE_DETAIL_TWEAKER_XL_VERSION_ID)
        for vid in seen_version_ids
    )
    if clean_persona and not has_char_civitai_lora and clean_persona.lower() not in clean_scene.lower():
        prompt_parts.append(clean_persona)
    for tr in arch_traits:
        if tr.lower() not in clean_scene.lower():
            prompt_parts.append(tr)

    try:
        detail_val = float(sliders.get("detail", 0.5))
        if detail_val >= 0.7:
            prompt_parts.append("ultra-detailed, fine skin texture, crisp sharp focus")
        elif detail_val <= 0.3:
            prompt_parts.append("soft cinematic focus, gentle atmospheric glow")
    except Exception:
        pass

    if base_modifier:
        prompt_parts.append(base_modifier)
    for em in extra_style_modifiers:
        prompt_parts.append(em)

    # Prevent random gibberish shirt text unless user explicitly asked for text/logos
    if use_sdxl and not re.search(r"\b(text|logo|sign|print|printed|banner|надпис|принт|лого)\b", clean_scene, re.I):
        prompt_parts.append("plain unprinted clothing")

    # SFW clothing guard when running in SFW mode (Guest or Work mode) with Pony or character subjects
    if not is_uncensored and use_sdxl:
        is_pure_scenery = bool(
            re.search(r"\b(landscape|mountain|ocean|forest|architecture|building|interior|empty room|no humans)\b", clean_scene, re.I)
            and not has_char_civitai_lora
            and not persona_prompt
            and not re.search(r"\b(woman|girl|lady|female|man|person|portrait|she|her)\b", clean_scene, re.I)
        )
        if not is_pure_scenery:
            prompt_parts.append("modest opaque buttoned outfit, covered chest, sfw")

    positive_prompt = ", ".join(p.strip(" ,") for p in prompt_parts if p and p.strip(" ,"))
    if not positive_prompt:
        positive_prompt = "portrait, natural lighting, high quality, detailed"
    positive_prompt = positive_prompt[:1400]

    # 6. Compose Negative Prompt (preserving mandatory SFW armor for guests + user's custom negative prompt)
    neg_parts: List[str] = []
    if not is_uncensored:
        neg_parts.append(GUEST_SFW_NEGATIVE_PROMPT)
    user_neg = str(sliders.get("negative_prompt") or "").replace("###", ", ").strip(" ,")
    if user_neg:
        neg_parts.append(user_neg[:500])
    neg_parts.append(BASE_NEGATIVE_PROMPT)
    if family in ("realistic", "perfect"):
        neg_parts.append("3d render, cgi, cartoon, plastic skin")
    negative_prompt = ", ".join(neg_parts)

    horde_prompt = f"{positive_prompt} ### {negative_prompt}"

    target_w, target_h = _resolve_dimensions(width, height, use_sdxl=use_sdxl)
    params: Dict[str, Any] = {
        "sampler_name": sampler_name,
        "cfg_scale": eff_cfg,
        "width": target_w,
        "height": target_h,
        "steps": eff_steps,
        "karras": karras_flag,
        "n": 1,
    }
    if use_sdxl:
        uses_clip_skip_2 = (
            uses_pony
            or family in ("fantasy", "tooned")
            or ("illustrious" in primary_model.lower())
            or ("anime" in primary_model.lower())
        )
        params["clip_skip"] = 2 if uses_clip_skip_2 else 1
    if horde_loras:
        params["loras"] = horde_loras
    if seed is not None and int(seed) > 0:
        params["seed"] = str(int(seed))

    allow_nsfw = bool(is_uncensored)
    payload: Dict[str, Any] = {
        "prompt": horde_prompt,
        "params": params,
        "nsfw": allow_nsfw,
        "censor_nsfw": not allow_nsfw,
        "replacement_filter": not allow_nsfw,
        "trusted_workers": False,
        "slow_workers": not use_sdxl,
        "models": models,
        "r2": True,
    }
    return payload


def _normalize_image_to_png(raw_bytes: bytes, apply_cas_sharpen: bool = True) -> bytes:
    """
    Converts WebP/JPEG/PNG bytes returned by AI Horde into standard RGB PNG bytes,
    applying subtle Contrast-Adaptive Unsharp (`flow.json` node `123` equivalent).
    """
    with Image.open(io.BytesIO(raw_bytes)) as im:
        if im.mode not in ("RGB", "RGBA"):
            im = im.convert("RGB")
        if apply_cas_sharpen and min(im.size) >= 640:
            im = im.filter(ImageFilter.UnsharpMask(radius=1.1, percent=52, threshold=2))
        out = io.BytesIO()
        im.save(out, format="PNG")
        return out.getvalue()


async def _throttled_submit(
    session: aiohttp.ClientSession,
    url: str,
    payload: Dict[str, Any],
    headers: Dict[str, str],
) -> Tuple[int, Dict[str, Any], str]:
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
    steps: Optional[int] = None,
    cfg: Optional[float] = None,
    seed: Optional[int] = None,
    persona_lora: Optional[str] = None,
    persona_name: Optional[str] = None,
    is_uncensored: bool = False,
    progress_cb: Optional[Callable[..., Awaitable[None]]] = None,
    client_agent: str = DEFAULT_CLIENT_AGENT,
    poll_interval: float = 2.0,
    max_wait_seconds: Optional[int] = None,
    budget_path: Optional[str] = None,
) -> Dict[str, Any]:
    if max_wait_seconds is None:
        try:
            max_wait_seconds = max(30, int(SETTINGS.get("HORDE_IMAGE_MAX_WAIT_SECONDS", 150)))
        except Exception:
            max_wait_seconds = 150

    is_owner_or_priv = _is_privileged_caller(ctx)
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
                user_id, estimated_kudos=28.0, budget_path=budget_path
            )
            use_priority = within_cap
            if not within_cap:
                logger.info(
                    f"[HordeMapper] Guest '{user_id}' priority Kudos cap reached "
                    f"(rem_global={rem_g:.1f}, rem_user={rem_u:.1f}), falling back to anonymous SD 1.5"
                )

    base = horde_client.HORDE_BASE_URL.rstrip("/")
    timeout = aiohttp.ClientTimeout(total=max_wait_seconds + 20, connect=15)

    async with aiohttp.ClientSession(timeout=timeout) as session:
        online_models = await fetch_online_image_models(session, client_agent=client_agent)

        if progress_cb:
            await progress_cb("warming", phase="queueing")

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
            tier_uncensored = effective_uncensored and is_sdxl_tier
            req_payload = map_ui_to_horde_request(
                prompt=prompt,
                persona_prompt=persona_prompt,
                sliders=sliders,
                style_setting=style_setting,
                width=width,
                height=height,
                steps=steps,
                cfg=cfg,
                seed=seed,
                persona_lora=persona_lora,
                persona_name=persona_name,
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
            rc = str(parsed.get("rc") or "")
            # If fast workers only (`slow_workers=False`) had zero worker match (not an auth/kudos error), retry once allowing slow_workers
            if (
                status_code not in (200, 202)
                and is_sdxl_tier
                and not req_payload.get("slow_workers")
                and status_code not in (401, 403)
                and rc not in ("KudosUpfront", "InvalidCredentials")
            ):
                req_payload_slow = dict(req_payload, slow_workers=True)
                status_code, parsed, raw_err = await _throttled_submit(
                    session, f"{base}/generate/async", req_payload_slow, headers
                )
                rc = str(parsed.get("rc") or "")
                if status_code in (200, 202) and parsed.get("id"):
                    req_payload = req_payload_slow

            if status_code in (200, 202) and parsed.get("id"):
                submit_res = parsed
                tier_used = candidate_tier
                break
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
        loras_requested = [l.get("name") for l in (req_payload.get("params", {}).get("loras") or [])]
        logger.info(
            f"[HordeMapper] Submitted job {task_id} (tier={tier_used}, models={models_requested}, "
            f"loras={loras_requested}, steps={req_payload['params']['steps']}, cfg={req_payload['params']['cfg_scale']}, "
            f"size={req_payload['params']['width']}x{req_payload['params']['height']}, kudos={kudos_cost})"
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
                    raise HordeImageError("No online AI Horde workers can fulfill this model/LoRA combination right now.")

                q_pos = int(chk.get("queue_position") or 0)
                wait_s = int(chk.get("wait_time") or 0)
                processing = int(chk.get("processing") or 0)
                if progress_cb:
                    if processing > 0 or q_pos == 0:
                        elapsed_ratio = min(0.92, (time.monotonic() - start_mono) / max(10.0, float(wait_s + 6)))
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
                    "Generated image was blocked by the AI Horde SFW safety filter. Please try a more modest outfit or prompt."
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

            png_bytes = _normalize_image_to_png(raw_img_bytes, apply_cas_sharpen=(tier_used == "priority_sdxl"))
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


def _format_civitai_thumb(url: str) -> str:
    if not url:
        return ""
    u = re.sub(r"/original=true/", "/width=256/", url)
    u = re.sub(r"/width=\d+/", "/width=256/", u)
    return u


ECOSYSTEM_BASE_MODELS: Dict[str, set] = {
    "sdxl": set(SDXL_COMPATIBLE_BASE_MODELS),
    "krea2": {"Krea 2"},
    "anima": {"Anima"},
}

ECOSYSTEM_QUERY_BASE_MODELS: Dict[str, List[str]] = {
    "sdxl": ["SDXL 1.0", "Pony", "Illustrious"],
    "krea2": ["Krea 2"],
    "anima": ["Anima"],
}

ECOSYSTEM_MAX_LORA_MB: Dict[str, float] = {
    "sdxl": 400.0,
    "krea2": 650.0,
    "anima": 650.0,
}


def _normalize_ecosystem(ecosystem: str = "sdxl") -> str:
    raw = str(ecosystem or "sdxl").strip().lower()
    if raw in ("krea", "krea2", "krea 2", "realism", "semi_realism", "muse", "dit"):
        return "krea2"
    if raw in ("anima",):
        return "anima"
    return "sdxl"


def _extract_civitai_lora_card(
    model_obj: Dict[str, Any],
    version_obj: Dict[str, Any],
    category: str,
    allow_nsfw_thumbs: bool = False,
    ecosystem: str = "sdxl",
) -> Optional[Dict[str, Any]]:
    """
    Validates a CivitAI model + version against the target engine ecosystem (`sdxl` | `krea2` | `anima`):
    - Baseline matches `ECOSYSTEM_BASE_MODELS[ecosystem]`
    - Primary `.safetensors` file with SHA256 and `size_mb <= max_mb`
    - Not a real-person POI (`poi == False`)
    - Static preview image (`nsfwLevel <= 2` unless `allow_nsfw_thumbs=True`)
    """
    if bool(model_obj.get("poi")):
        return None
    norm_eco = _normalize_ecosystem(ecosystem)
    allowed_bases = ECOSYSTEM_BASE_MODELS[norm_eco]
    max_mb = ECOSYSTEM_MAX_LORA_MB[norm_eco]

    base_model = str(version_obj.get("baseModel") or "").strip()
    if base_model not in allowed_bases:
        return None

    chosen_file = None
    for f in (version_obj.get("files") or []):
        if not isinstance(f, dict):
            continue
        fname = str(f.get("name") or "")
        hashes = f.get("hashes") or {}
        if fname.endswith(".safetensors") and hashes.get("SHA256") and f.get("downloadUrl"):
            if f.get("primary") or chosen_file is None:
                chosen_file = f
                if f.get("primary"):
                    break
    if not chosen_file:
        return None

    size_mb = round(float(chosen_file.get("sizeKB") or 0.0) / 1024.0, 1)
    if size_mb <= 0 or size_mb > max_mb:
        return None

    max_nsfw_lvl = 16 if allow_nsfw_thumbs else 2
    preview_url = ""
    for im in (version_obj.get("images") or []):
        if not isinstance(im, dict) or im.get("type") == "video":
            continue
        lvl = int(im.get("nsfwLevel") or 1)
        if lvl <= max_nsfw_lvl and im.get("url"):
            preview_url = _format_civitai_thumb(str(im["url"]))
            break
    if not preview_url:
        return None

    vid = str(version_obj.get("id") or "").strip()
    mid = str(model_obj.get("id") or version_obj.get("modelId") or "").strip()
    if not vid.isdigit():
        return None

    raw_name = str(model_obj.get("name") or f"LoRA {vid}").strip()
    if not allow_nsfw_thumbs and re.search(
        r"\b(?:nsfw|hentai|porn|nude|naked|fetish|explicit|sex|xxx|erotic|furry\s+babes|boobs|breasts)\b",
        raw_name,
        flags=re.IGNORECASE,
    ):
        return None
    trained = [
        re.sub(r"<lora:[^>]+>", "", str(w)).strip(" ,")
        for w in (version_obj.get("trainedWords") or [])
        if str(w).strip()
    ]
    trained = [w for w in trained if w][:4]
    tags = [str(t).strip().lower() for t in (model_obj.get("tags") or []) if str(t).strip()]

    return {
        "id": f"civitai_{vid}",
        "name": raw_name,
        "civitai_name": raw_name,
        "version_name": str(version_obj.get("name") or ""),
        "civitai_model_id": mid,
        "civitai_version_id": vid,
        "base_model": base_model,
        "size_mb": size_mb,
        "category": category,
        "image_url": preview_url,
        "trained_words": trained,
        "tags": tags[:6],
        "strength": 0.85 if norm_eco in ("krea2", "anima") else (0.9 if category == "character" else 0.75),
        "filename": f"civitai_{vid}.safetensors",
        "trigger": f"<civitai_{vid}>",
        "models": [norm_eco],
        "url": f"https://civitai.com/models/{mid}?modelVersionId={vid}" if mid else "",
        "description": f"{base_model} · {size_mb:.0f} MB" + (f" · trigger: {trained[0]}" if trained else ""),
    }


_civitai_open_dl_cache: Dict[str, bool] = {}


def _get_civitai_api_token(explicit_token: str = "") -> str:
    if explicit_token and str(explicit_token).strip():
        return str(explicit_token).strip()
    return str(
        SETTINGS.get("CIVITAI_API_KEY")
        or SETTINGS.get("CIVITAI_API_TOKEN")
        or os.environ.get("CIVITAI_API_KEY")
        or os.environ.get("CIVITAI_API_TOKEN")
        or ""
    ).strip()


async def _filter_open_civitai_cards(
    session: aiohttp.ClientSession,
    cards: List[Dict[str, Any]],
    limit: int,
    civitai_token: str = "",
) -> List[Dict[str, Any]]:
    if not cards:
        return []
    token = _get_civitai_api_token(civitai_token)
    if token:
        return cards[:limit]

    async def _is_open(card: Dict[str, Any]) -> bool:
        vid = str(card.get("civitai_version_id") or "").strip()
        if not vid:
            return False
        if vid in _civitai_open_dl_cache:
            return _civitai_open_dl_cache[vid]
        try:
            async with session.get(
                f"https://civitai.com/api/download/models/{vid}",
                allow_redirects=False,
                timeout=aiohttp.ClientTimeout(total=4.0),
            ) as dr:
                loc = str(dr.headers.get("Location") or "")
                ok = dr.status in (200, 301, 302, 303, 307, 308) and "/login" not in loc
        except Exception:
            ok = True
        _civitai_open_dl_cache[vid] = ok
        return ok

    checks = await asyncio.gather(*(_is_open(c) for c in cards), return_exceptions=True)
    filtered = [c for c, ok in zip(cards, checks) if ok is True]
    return filtered[:limit]


async def search_civitai_loras(
    mode: str = "character",
    query: str = "",
    sort: str = "Most Downloaded",
    limit: int = 24,
    allow_nsfw: bool = False,
    ecosystem: str = "sdxl",
    civitai_token: str = "",
) -> List[Dict[str, Any]]:
    """
    Searches CivitAI for engine-compatible (`sdxl` | `krea2` | `anima`) LoRAs in two modes:
    - `mode="character"`: Companion / fictional characters (`tag=character`, excluding sliders/poses)
    - `mode="style"`: Art styles, aesthetics, lighting, concepts (everything that is NOT a character)
    Also supports pasting a direct CivitAI model/version URL or numeric ID into `query`.
    """
    norm_mode = "character" if str(mode or "").strip().lower().startswith("char") else "style"
    norm_eco = _normalize_ecosystem(ecosystem)
    q_clean = str(query or "").strip()
    norm_sort = sort if sort in ("Most Downloaded", "Highest Rated", "Newest") else "Most Downloaded"
    lim = max(4, min(36, int(limit or 24)))
    eff_token = _get_civitai_api_token(civitai_token)

    cache_key = f"{norm_eco}|{norm_mode}|{q_clean.lower()}|{norm_sort}|{lim}|{int(bool(allow_nsfw))}|{int(bool(eff_token))}"
    now = time.monotonic()
    cached = _civitai_search_cache.get(cache_key)
    if cached and (now - cached[0]) < _CIVITAI_SEARCH_CACHE_TTL:
        return cached[1]

    headers: Dict[str, str] = {"User-Agent": "Direct/1.0"}
    if eff_token:
        headers["Authorization"] = f"Bearer {eff_token}"
    timeout = aiohttp.ClientTimeout(total=12.0, connect=6.0)
    results: List[Dict[str, Any]] = []

    async with aiohttp.ClientSession(timeout=timeout, headers=headers) as session:
        # Case A: User pasted a CivitAI URL (`civitai.com/models/12345?modelVersionId=67890`) or raw numeric ID
        url_vid_match = re.search(r"modelVersionId=(\d+)", q_clean)
        url_mid_match = re.search(r"civitai\.(?:com|red)/models/(\d+)", q_clean)
        raw_id_match = re.match(r"^(\d{4,12})$", q_clean)

        if url_vid_match or url_mid_match or raw_id_match:
            try:
                if url_vid_match:
                    vid = url_vid_match.group(1)
                    async with session.get(f"https://civitai.com/api/v1/model-versions/{vid}") as vr:
                        if vr.status == 200:
                            v_data = await vr.json()
                            mid = v_data.get("modelId")
                            async with session.get(f"https://civitai.com/api/v1/models/{mid}") as mr:
                                if mr.status == 200:
                                    m_data = await mr.json()
                                    card = _extract_civitai_lora_card(m_data, v_data, norm_mode, allow_nsfw_thumbs=allow_nsfw, ecosystem=norm_eco)
                                    if card:
                                        results.append(card)
                else:
                    num_id = url_mid_match.group(1) if url_mid_match else raw_id_match.group(1)
                    # Try as model ID first, then as version ID
                    async with session.get(f"https://civitai.com/api/v1/models/{num_id}") as mr:
                        if mr.status == 200:
                            m_data = await mr.json()
                            if str(m_data.get("type") or "").upper() == "LORA":
                                for v_data in (m_data.get("modelVersions") or []):
                                    card = _extract_civitai_lora_card(m_data, v_data, norm_mode, allow_nsfw_thumbs=allow_nsfw, ecosystem=norm_eco)
                                    if card:
                                        results.append(card)
                                        break
                    if not results:
                        async with session.get(f"https://civitai.com/api/v1/model-versions/{num_id}") as vr:
                            if vr.status == 200:
                                v_data = await vr.json()
                                mid = v_data.get("modelId")
                                async with session.get(f"https://civitai.com/api/v1/models/{mid}") as mr2:
                                    if mr2.status == 200:
                                        m_data = await mr2.json()
                                        card = _extract_civitai_lora_card(m_data, v_data, norm_mode, allow_nsfw_thumbs=allow_nsfw, ecosystem=norm_eco)
                                        if card:
                                            results.append(card)
            except Exception as e:
                logger.debug(f"[CivitaiSearch] Direct ID/URL lookup failed for '{q_clean}': {e}")

            if results:
                _civitai_search_cache[cache_key] = (now, results)
                return results

        # Case B: Keyword / tag catalog search on CivitAI
        params: List[Tuple[str, str]] = [
            ("types", "LORA"),
            ("sort", norm_sort),
            ("limit", str(min(60, lim * 2 + 12))),
            ("nsfw", "true" if allow_nsfw else "false"),
        ]
        for bm in ECOSYSTEM_QUERY_BASE_MODELS[norm_eco]:
            params.append(("baseModels", bm))
        if norm_mode == "character":
            params.append(("tag", "character"))
        elif not q_clean:
            params.append(("tag", "style"))

        if q_clean:
            params.append(("query", q_clean))

        qs = urllib.parse.urlencode(params)
        try:
            async with session.get(f"https://civitai.com/api/v1/models?{qs}") as resp:
                if resp.status == 200:
                    payload = await resp.json()
                    for item in (payload.get("items") or []):
                        if not isinstance(item, dict):
                            continue
                        tags_low = {str(t).strip().lower() for t in (item.get("tags") or [])}
                        if not allow_nsfw and (item.get("nsfw") or (tags_low & {"nsfw", "hentai", "explicit", "fetish"})):
                            continue
                        if norm_mode == "character":
                            if tags_low & {"slider", "poses", "background", "architecture", "vehicle"}:
                                continue
                        else:
                            # Styles / Concepts: exclude character & celebrity LoRAs
                            if tags_low & {"character", "celebrity", "vtuber"}:
                                continue

                        # Pick first ecosystem-compatible version within size limit
                        for ver in (item.get("modelVersions") or []):
                            card = _extract_civitai_lora_card(item, ver, norm_mode, allow_nsfw_thumbs=allow_nsfw, ecosystem=norm_eco)
                            if card:
                                results.append(card)
                                break
                        if len(results) >= (lim + 8):
                            break
                    results = await _filter_open_civitai_cards(session, results, lim, civitai_token=eff_token)
        except Exception as e:
            logger.warning(f"[CivitaiSearch] Search failed ({norm_eco}/{norm_mode}, q={q_clean!r}): {e}")

    _civitai_search_cache[cache_key] = (now, results)
    return results


_LOCAL_LORA_DL_LOCK: Optional[asyncio.Lock] = None
_MAX_CIVITAI_CACHE_BYTES = int(float(os.environ.get("DIRECT_CIVITAI_LORA_CACHE_GB", "6.0")) * (1024 ** 3))


def _prune_civitai_lora_cache(loras_dir: str, max_bytes: int = _MAX_CIVITAI_CACHE_BYTES) -> None:
    try:
        entries = []
        total = 0
        for fn in os.listdir(loras_dir):
            if fn.startswith("civitai_") and fn.endswith(".safetensors"):
                fp = os.path.join(loras_dir, fn)
                if os.path.isfile(fp):
                    st = os.stat(fp)
                    entries.append((st.st_atime or st.st_mtime, st.st_size, fp))
                    total += st.st_size
        if total <= max_bytes:
            return
        entries.sort(key=lambda x: x[0])
        for _, sz, fp in entries:
            if total <= max_bytes:
                break
            try:
                os.remove(fp)
                meta_fp = fp[:-len(".safetensors")] + ".meta.json"
                if os.path.isfile(meta_fp):
                    os.remove(meta_fp)
                total -= sz
                logger.info(f"[CivitaiCache] Pruned old cached LoRA {os.path.basename(fp)}")
            except Exception:
                pass
    except Exception as e:
        logger.debug(f"[CivitaiCache] Cache prune skipped: {e}")


async def resolve_civitai_lora_metadata(
    lora_key: str,
    custom_loras: Optional[List[Dict[str, Any]]] = None,
    civitai_token: str = "",
) -> Optional[Dict[str, Any]]:
    """
    Resolves `{filename, strength, trained_words, category, civitai_version_id}` for a `civitai_<vid>`
    LoRA (using `custom_loras` payload first, falling back to CivitAI API metadata without downloading
    model weights locally - ideal when dispatching Krea 2 / Anima workflows to Modal Cloud GPU).
    """
    custom_lookup: Dict[str, Dict[str, Any]] = {}
    for cl in (custom_loras or []):
        if isinstance(cl, dict) and cl.get("civitai_version_id"):
            vid_s = str(cl["civitai_version_id"]).strip()
            cid_s = str(cl.get("id") or f"civitai_{vid_s}").strip().lower()
            entry = {
                "id": cid_s,
                "name": str(cl.get("name") or cid_s),
                "civitai_version_id": vid_s,
                "category": str(cl.get("category") or "character"),
                "strength": float(cl.get("strength") or 0.85),
                "trained_words": list(cl.get("trained_words") or []),
            }
            custom_lookup[cid_s] = entry
            custom_lookup[vid_s] = entry

    spec = _resolve_lora_spec(lora_key, custom_lookup)
    if not spec:
        return None
    vid = str(spec.get("civitai_version_id") or "").strip()
    if not vid.isdigit():
        return None
    trained = list(spec.get("trained_words") or [])
    if not trained:
        eff_tok = _get_civitai_api_token(civitai_token)
        headers: Dict[str, str] = {"User-Agent": "Direct/1.0"}
        if eff_tok:
            headers["Authorization"] = f"Bearer {eff_tok}"
        try:
            async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=8.0), headers=headers) as session:
                async with session.get(f"https://civitai.com/api/v1/model-versions/{vid}") as vr:
                    if vr.status == 200:
                        vd = await vr.json()
                        trained_api = [
                            re.sub(r"<lora:[^>]+>", "", str(w)).strip(" ,")
                            for w in (vd.get("trainedWords") or [])
                            if str(w).strip()
                        ]
                        trained = [w for w in trained_api if w][:4]
        except Exception:
            pass
    return {
        "filename": f"civitai_{vid}.safetensors",
        "civitai_version_id": vid,
        "strength": float(spec.get("strength", 0.85)),
        "trained_words": trained,
        "category": str(spec.get("category") or "character"),
    }


async def ensure_local_civitai_lora(
    lora_key: str,
    custom_loras: Optional[List[Dict[str, Any]]] = None,
    loras_dir: Optional[str] = None,
    allow_nsfw: bool = False,
    ecosystem: str = "sdxl",
    civitai_token: str = "",
) -> Optional[Dict[str, Any]]:
    """
    Ensures a predefined or user-selected CivitAI LoRA `.safetensors` file
    exists in the local ComfyUI `loras/` directory so the local ComfyUI runner
    can load it directly. Returns `{filename, strength, trained_words, category}`.
    """
    global _LOCAL_LORA_DL_LOCK
    if _LOCAL_LORA_DL_LOCK is None:
        _LOCAL_LORA_DL_LOCK = asyncio.Lock()

    norm_eco = _normalize_ecosystem(ecosystem)
    allowed_bases = ECOSYSTEM_BASE_MODELS[norm_eco]
    max_mb = ECOSYSTEM_MAX_LORA_MB[norm_eco]
    eff_tok = _get_civitai_api_token(civitai_token)

    target_dir = loras_dir or SETTINGS.get("COMFY_LORAS_DIR") or "/home/alexey/models/comfyui_models/loras"
    if not os.path.isdir(target_dir):
        return None

    custom_lookup: Dict[str, Dict[str, Any]] = {}
    for cl in (custom_loras or []):
        if isinstance(cl, dict) and cl.get("civitai_version_id"):
            vid_s = str(cl["civitai_version_id"]).strip()
            cid_s = str(cl.get("id") or f"civitai_{vid_s}").strip().lower()
            entry = {
                "id": cid_s,
                "name": str(cl.get("name") or cid_s),
                "civitai_version_id": vid_s,
                "category": str(cl.get("category") or "character"),
                "strength": float(cl.get("strength") or (0.75 if cl.get("category") == "style" else 0.9)),
                "trained_words": list(cl.get("trained_words") or []),
            }
            custom_lookup[cid_s] = entry
            custom_lookup[vid_s] = entry

    spec = _resolve_lora_spec(lora_key, custom_lookup)
    if not spec:
        return None

    local_fn = spec.get("local_filename")
    if local_fn and os.path.isfile(os.path.join(target_dir, local_fn)):
        return {
            "filename": local_fn,
            "strength": float(spec.get("strength", 0.9)),
            "trained_words": list(spec.get("trained_words") or []),
            "category": str(spec.get("category") or "character"),
        }

    vid = str(spec.get("civitai_version_id") or "").strip()
    if not vid.isdigit():
        return None

    cached_fn = f"civitai_{vid}.safetensors"
    cached_fp = os.path.join(target_dir, cached_fn)
    meta_fp = os.path.join(target_dir, f"civitai_{vid}.meta.json")

    if os.path.isfile(cached_fp) and os.path.getsize(cached_fp) > 65536:
        try:
            os.utime(cached_fp, None)
        except Exception:
            pass
        trained = list(spec.get("trained_words") or [])
        category = str(spec.get("category") or "character")
        if os.path.isfile(meta_fp):
            try:
                with open(meta_fp, "r", encoding="utf-8") as mf:
                    m_saved = json.load(mf)
                if not trained and m_saved.get("trained_words"):
                    trained = list(m_saved["trained_words"])
                if m_saved.get("category"):
                    category = str(m_saved["category"])
            except Exception:
                pass
        return {
            "filename": cached_fn,
            "strength": float(spec.get("strength", 0.9)),
            "trained_words": trained,
            "category": category,
        }

    async with _LOCAL_LORA_DL_LOCK:
        if os.path.isfile(cached_fp) and os.path.getsize(cached_fp) > 65536:
            return {
                "filename": cached_fn,
                "strength": float(spec.get("strength", 0.9)),
                "trained_words": list(spec.get("trained_words") or []),
                "category": str(spec.get("category") or "character"),
            }

        headers: Dict[str, str] = {"User-Agent": "Direct/1.0"}
        if eff_tok:
            headers["Authorization"] = f"Bearer {eff_tok}"
        timeout = aiohttp.ClientTimeout(total=180.0, connect=10.0)
        try:
            async with aiohttp.ClientSession(timeout=timeout, headers=headers) as session:
                async with session.get(f"https://civitai.com/api/v1/model-versions/{vid}") as vr:
                    if vr.status != 200:
                        logger.warning(f"[CivitaiCache] Version {vid} metadata HTTP {vr.status}")
                        return None
                    v_data = await vr.json()

                mid = v_data.get("modelId")
                m_data: Dict[str, Any] = {}
                if mid:
                    async with session.get(f"https://civitai.com/api/v1/models/{mid}") as mr:
                        if mr.status == 200:
                            m_data = await mr.json()

                if m_data.get("poi"):
                    logger.warning(f"[CivitaiCache] Rejected POI LoRA version {vid}")
                    return None
                if not allow_nsfw and m_data.get("nsfw") and lora_key not in SDXL_CIVITAI_LORA_REGISTRY:
                    logger.warning(f"[CivitaiCache] Rejected NSFW LoRA version {vid} for guest")
                    return None

                base_model = str(v_data.get("baseModel") or "").strip()
                if base_model and base_model not in allowed_bases:
                    logger.warning(f"[CivitaiCache] Rejected incompatible baseline '{base_model}' for {norm_eco} ({vid})")
                    return None

                chosen_file = None
                for f_obj in (v_data.get("files") or []):
                    if not isinstance(f_obj, dict):
                        continue
                    fn_low = str(f_obj.get("name") or "").lower()
                    hashes = f_obj.get("hashes") or {}
                    if fn_low.endswith(".safetensors") and hashes.get("SHA256") and f_obj.get("downloadUrl"):
                        if f_obj.get("primary") or chosen_file is None:
                            chosen_file = f_obj
                            if f_obj.get("primary"):
                                break
                if not chosen_file:
                    return None

                size_kb = float(chosen_file.get("sizeKB") or 0.0)
                if size_kb <= 0 or (size_kb / 1024.0) > max_mb:
                    logger.warning(f"[CivitaiCache] Rejected LoRA {vid} exceeding {max_mb} MB ({size_kb / 1024.0:.1f} MB)")
                    return None

                expected_sha = str((chosen_file.get("hashes") or {}).get("SHA256") or "").strip().lower()
                dl_url = str(chosen_file["downloadUrl"])
                if eff_tok and "civitai.com" in dl_url and "token=" not in dl_url:
                    sep = "&" if "?" in dl_url else "?"
                    dl_url = f"{dl_url}{sep}token={eff_tok}"
                tmp_fp = f"{cached_fp}.part.{os.getpid()}"
                hasher = hashlib.sha256()
                total_read = 0
                max_dl_bytes = int((max_mb + 20) * 1024 * 1024)

                logger.info(f"[CivitaiCache] Downloading CivitAI LoRA {vid} ({size_kb / 1024.0:.1f} MB) -> {cached_fn}")
                async with session.get(dl_url, allow_redirects=True) as dl_resp:
                    if dl_resp.status != 200:
                        logger.warning(f"[CivitaiCache] Download failed HTTP {dl_resp.status} for {vid}")
                        return None
                    with open(tmp_fp, "wb") as out_f:
                        async for chunk in dl_resp.content.iter_chunked(262144):
                            total_read += len(chunk)
                            if total_read > max_dl_bytes:
                                raise RuntimeError(f"LoRA download exceeded {max_mb} MB limit")
                            hasher.update(chunk)
                            out_f.write(chunk)

                actual_sha = hasher.hexdigest().lower()
                if expected_sha and actual_sha != expected_sha:
                    try:
                        os.remove(tmp_fp)
                    except Exception:
                        pass
                    logger.error(f"[CivitaiCache] SHA256 mismatch for {vid}: expected {expected_sha}, got {actual_sha}")
                    return None

                os.replace(tmp_fp, cached_fp)
                trained_api = [
                    re.sub(r"<lora:[^>]+>", "", str(w)).strip(" ,")
                    for w in (v_data.get("trainedWords") or [])
                    if str(w).strip()
                ]
                trained_final = list(spec.get("trained_words") or [w for w in trained_api if w][:4])
                cat_final = str(spec.get("category") or "character")
                try:
                    with open(meta_fp, "w", encoding="utf-8") as mf:
                        json.dump(
                            {
                                "civitai_version_id": vid,
                                "name": m_data.get("name") or spec.get("name") or cached_fn,
                                "base_model": base_model,
                                "trained_words": trained_final,
                                "category": cat_final,
                            },
                            mf,
                            ensure_ascii=False,
                            indent=2,
                        )
                except Exception:
                    pass

                _prune_civitai_lora_cache(target_dir)
                logger.info(f"[CivitaiCache] Ready local CivitAI LoRA {cached_fn} ({total_read / (1024 * 1024):.1f} MB)")
                return {
                    "filename": cached_fn,
                    "strength": float(spec.get("strength", 0.9)),
                    "trained_words": trained_final,
                    "category": cat_final,
                }
        except Exception as e:
            logger.warning(f"[CivitaiCache] Failed to download CivitAI LoRA {vid}: {e}")
            try:
                tmp_cand = f"{cached_fp}.part.{os.getpid()}"
                if os.path.exists(tmp_cand):
                    os.remove(tmp_cand)
            except Exception:
                pass
            return None
