"""
Asynchronous client for AI Horde (https://aihorde.net) text & chat generation.

Primary transport:
  - OpenAI-compatible SSE Streaming API (`https://oai.aihorde.net/v1/chat/completions`,
    `GET https://oai.aihorde.net/v1/models`) supporting real-time token streaming,
    `reasoning_content` (thinking) deltas, `tool_calls`, multimodal image inputs,
    multi-model fallback (`models` up to 5), and `x_horde` worker/kudos metadata.

Fallback transport:
  - Legacy AI Horde async text polling (`/api/v2/generate/text/async`) for raw prompt
    requests or models served exclusively by non-OAI legacy workers.
"""

import aiohttp
import asyncio
import codecs
import contextlib
import json
import logging
import re
import time
from typing import AsyncGenerator, Optional, List, Dict, Any, Tuple

HORDE_BASE_URL = "https://aihorde.net/api/v2"
HORDE_OAI_BASE_URL = "https://oai.aihorde.net/v1"
_DEFAULT_HORDE_BASE_URL = HORDE_BASE_URL
_DEFAULT_HORDE_OAI_BASE_URL = HORDE_OAI_BASE_URL

DEFAULT_CLIENT_AGENT = "Direct:1.0.0:https://github.com/alexeyvolkoff/direct"
ANONYMOUS_API_KEY = "0000000000"

STOP_TOKENS_RE = re.compile(
    r'(<\|im_end\|>|<\|im_start\|>|<\|end_of_text\|>|<\|endoftext\|>|<\|eot_id\|>|<\|eom_id\|>|<\|start_header_id\|>|<\|end_header_id\|>|<\/s>|<s>|<end_of_turn>|<start_of_turn>|\[DONE\])',
    re.IGNORECASE
)

_OAI_STOP_TOKENS_RE = re.compile(
    r'(<\|im_end\|>|<\|im_start\|>|<\|end_of_text\|>|<\|endoftext\|>|<\|eot_id\|>|<\|eom_id\|>|<\|start_header_id\|>|<\|end_header_id\|>|<\/s>|<s>|<end_of_turn>|<start_of_turn>)',
    re.IGNORECASE
)

_OLLAMA_THINK_PREFIX_RE = re.compile(r'^\s*<\|(?:no)?think\|>\s*\n?', re.IGNORECASE)

# Strong chat / RP models frequently online on AI Horde OAI & KoboldCpp/Aphrodite
DEFAULT_HORDE_MODELS = [
    "koboldcpp/Orion-26B-A4B-v1.1",
    "koboldcpp/G4-MeroMero-26B-A4B-it-uncensored-heretic",
    "koboldcpp/Cydonia-24B-v4zg-Q4_K_M",
    "koboldcpp/mini-magnum-12b-v1.1",
    "koboldcpp/L3-8B-Stheno-v3.2",
]

# Short-lived model directory caches keyed by endpoint URL
_MODEL_CACHE_TTL = 30.0
_FAULT_COOLDOWN_SECONDS = 300.0
_oai_models_cache: Dict[str, Tuple[float, List[Dict[str, Any]]]] = {}
_legacy_models_cache: Dict[str, Tuple[float, List[Dict[str, Any]]]] = {}
_faulted_oai_models: Dict[str, float] = {}


def _is_model_quarantined(model_id: str) -> bool:
    exp = _faulted_oai_models.get(model_id)
    if not exp:
        return False
    if time.monotonic() >= exp:
        _faulted_oai_models.pop(model_id, None)
        return False
    return True


def _quarantine_faulted_model(model_id: str) -> None:
    if model_id:
        _faulted_oai_models[model_id] = time.monotonic() + _FAULT_COOLDOWN_SECONDS


def _is_tiny_or_debug_model(model_id: str) -> bool:
    low = model_id.lower()
    if "debug" in low:
        return True
    if any(tag in low for tag in ("0.5b", "0.6b", "270m", "135m", "1.5b", "1.7b")):
        return True
    return False


def _rank_oai_card(card: Dict[str, Any]) -> Tuple[int, int, int, float]:
    model_id = str(card.get("id") or "")
    xh = card.get("x_horde") or {}
    workers = int(xh.get("workers") or 0)
    ctx_len = int(card.get("context_length") or 0)
    queued = float(xh.get("queued") or 0.0)
    not_tiny = 0 if _is_tiny_or_debug_model(model_id) else 1
    # Prefer non-tiny models with online workers, larger context, and smaller queue
    return (not_tiny, workers, ctx_len, -queued)


async def _fetch_oai_model_cards(
    session: aiohttp.ClientSession,
    client_agent: str = DEFAULT_CLIENT_AGENT,
    force_refresh: bool = False,
) -> List[Dict[str, Any]]:
    base = HORDE_OAI_BASE_URL.rstrip("/")
    now = time.monotonic()
    cached = _oai_models_cache.get(base)
    if not force_refresh and cached and (now - cached[0]) < _MODEL_CACHE_TTL:
        return cached[1]

    try:
        async with session.get(
            f"{base}/models",
            headers={"Client-Agent": client_agent},
            timeout=aiohttp.ClientTimeout(total=8),
        ) as resp:
            if resp.status == 200:
                body = await resp.json()
                cards = body.get("data") if isinstance(body, dict) else []
                if isinstance(cards, list):
                    cards_sorted = sorted(cards, key=_rank_oai_card, reverse=True)
                    _oai_models_cache[base] = (now, cards_sorted)
                    return cards_sorted
    except Exception as e:
        logging.debug(f"[Horde OAI] Could not fetch /v1/models from {base}: {e}")

    return cached[1] if cached else []


async def _fetch_legacy_text_models(
    session: aiohttp.ClientSession,
    client_agent: str = DEFAULT_CLIENT_AGENT,
    force_refresh: bool = False,
) -> List[Dict[str, Any]]:
    base = HORDE_BASE_URL.rstrip("/")
    now = time.monotonic()
    cached = _legacy_models_cache.get(base)
    if not force_refresh and cached and (now - cached[0]) < _MODEL_CACHE_TTL:
        return cached[1]

    try:
        async with session.get(
            f"{base}/status/models?type=text",
            headers={"Client-Agent": client_agent},
            timeout=aiohttp.ClientTimeout(total=8),
        ) as resp:
            if resp.status == 200:
                models = await resp.json()
                if isinstance(models, list):
                    _legacy_models_cache[base] = (now, models)
                    return models
    except Exception as e:
        logging.debug(f"[Horde] Could not fetch legacy text models from {base}: {e}")

    return cached[1] if cached else []


async def get_online_text_models(limit: int = 40) -> List[Dict[str, Any]]:
    """
    Fetch currently active text models on AI Horde, prioritizing OAI-streaming
    chat workers (`https://oai.aihorde.net/v1/models`) and enriching / merging
    with `/api/v2/status/models?type=text` so the UI receives worker counts, ETA,
    context limits, and OAI capability flags in a backward-compatible shape.
    """
    try:
        async with aiohttp.ClientSession() as session:
            oai_res, leg_res = await asyncio.gather(
                _fetch_oai_model_cards(session, force_refresh=True),
                _fetch_legacy_text_models(session, force_refresh=True),
                return_exceptions=True,
            )
            oai_cards = oai_res if isinstance(oai_res, list) else []
            leg_models = leg_res if isinstance(leg_res, list) else []

            leg_by_name: Dict[str, Dict[str, Any]] = {
                str(m.get("name")): m for m in leg_models if isinstance(m, dict) and m.get("name")
            }

            merged: List[Dict[str, Any]] = []
            seen: set[str] = set()

            for card in oai_cards:
                m_id = str(card.get("id") or "").strip()
                if not m_id or m_id in seen:
                    continue
                seen.add(m_id)
                xh = card.get("x_horde") or {}
                top_p = card.get("top_provider") or {}
                arch = card.get("architecture") or {}
                leg = leg_by_name.get(m_id) or {}

                oai_workers = int(xh.get("workers") or 0)
                leg_count = int(leg.get("count") or 0)
                worker_count = max(oai_workers, leg_count, 1) if oai_workers > 0 else leg_count
                supported_params = card.get("supported_parameters") or []
                input_mods = arch.get("input_modalities") or ["text"]

                merged.append({
                    "name": m_id,
                    "type": "text",
                    "count": worker_count,
                    "oai_workers": oai_workers,
                    "queued": float(xh.get("queued") if xh.get("queued") is not None else (leg.get("queued") or 0.0)),
                    "eta": int(leg.get("eta") or 0),
                    "performance": leg.get("performance", 0),
                    "context_length": card.get("context_length") or top_p.get("context_length"),
                    "max_completion_tokens": top_p.get("max_completion_tokens"),
                    "oai_supported": True,
                    "supports_tools": "tools" in supported_params,
                    "supports_reasoning": "reasoning_effort" in supported_params,
                    "supports_vision": "image" in input_mods,
                    "supported_parameters": supported_params,
                })

            # Append remaining active legacy-only text models
            leg_only = [
                m for m in leg_models
                if isinstance(m, dict) and m.get("name") and str(m.get("name")) not in seen
            ]
            leg_only.sort(key=lambda m: (m.get("count", 0), m.get("performance", 0)), reverse=True)
            for m in leg_only:
                entry = dict(m)
                entry.setdefault("type", "text")
                entry["oai_supported"] = False
                merged.append(entry)

            return merged[:limit]
    except Exception as e:
        logging.warning(f"[Horde] Failed to fetch models: {e}")
    return []


def format_messages_for_horde(messages: List[Dict[str, Any]]) -> str:
    """
    Format a list of chat messages [{'role': ..., 'content': ...}]
    into a ChatML string widely supported by legacy KoboldCpp / Aphrodite workers.
    """
    lines = []
    for msg in messages:
        role = msg.get("role", "user")
        content = msg.get("content", "")
        if role == "system" and isinstance(content, str):
            content = _OLLAMA_THINK_PREFIX_RE.sub("", content)
        lines.append(f"<|im_start|>{role}\n{content}<|im_end|>")
    lines.append("<|im_start|>assistant\n")
    return "\n".join(lines)


def format_messages_for_oai(
    messages: List[Dict[str, Any]],
    allow_images: bool = False,
) -> List[Dict[str, Any]]:
    """
    Sanitize internal Direct/Ollama message dicts into strict OpenAI ChatMessage objects
    accepted by `https://oai.aihorde.net/v1/chat/completions`.
    """
    valid_roles = {"system", "developer", "user", "assistant", "tool"}
    cleaned: List[Dict[str, Any]] = []

    for msg in messages:
        if not isinstance(msg, dict):
            continue
        role = str(msg.get("role") or "user").lower()
        if role not in valid_roles:
            role = "user"

        raw_content = msg.get("content")
        text_content = "" if raw_content is None else (
            raw_content if isinstance(raw_content, str) else str(raw_content)
        )
        if role in ("system", "developer"):
            text_content = _OLLAMA_THINK_PREFIX_RE.sub("", text_content)

        out: Dict[str, Any] = {"role": role}

        if msg.get("name"):
            out["name"] = str(msg["name"])

        if role == "assistant" and msg.get("tool_calls"):
            out["tool_calls"] = msg["tool_calls"]
            out["content"] = text_content if text_content else None
        elif role == "tool":
            out["tool_call_id"] = str(msg.get("tool_call_id") or "call_0")
            out["content"] = text_content
        elif role == "user" and allow_images and msg.get("images"):
            parts: List[Dict[str, Any]] = []
            if text_content:
                parts.append({"type": "text", "text": text_content})
            for img_item in msg["images"]:
                if not img_item:
                    continue
                img_str = str(img_item).strip()
                if img_str.startswith(("data:", "http://", "https://")):
                    url = img_str
                else:
                    url = f"data:image/jpeg;base64,{img_str}"
                parts.append({"type": "image_url", "image_url": {"url": url}})
            out["content"] = parts if parts else text_content
        else:
            # Allow already-structured OpenAI content arrays on user messages if passed directly
            if role == "user" and isinstance(raw_content, list):
                out["content"] = raw_content
            else:
                out["content"] = text_content

        cleaned.append(out)

    return cleaned


async def cancel_horde_task(
    task_id: str,
    api_key: str = ANONYMOUS_API_KEY,
    client_agent: str = DEFAULT_CLIENT_AGENT,
):
    """Cancel an active text generation task on AI Horde."""
    if not task_id:
        return
    clean_id = task_id[len("chatcmpl-"):] if task_id.startswith("chatcmpl-") else task_id
    try:
        async with aiohttp.ClientSession() as session:
            async with session.delete(
                f"{HORDE_BASE_URL.rstrip('/')}/generate/text/status/{clean_id}",
                headers={
                    "apikey": api_key or ANONYMOUS_API_KEY,
                    "Client-Agent": client_agent,
                },
                timeout=aiohttp.ClientTimeout(total=5),
            ) as resp:
                logging.info(f"[Horde] Cancelled task {clean_id}: status={resp.status}")
    except Exception as e:
        logging.warning(f"[Horde] Error cancelling task {clean_id}: {e}")


async def _iter_sse_events(resp: aiohttp.ClientResponse) -> AsyncGenerator[Dict[str, Any], None]:
    """
    Incremental SSE parser over `resp.content.iter_any()`.
    Avoids aiohttp's 64KB readline limit and yields decoded JSON events immediately
    as TCP packets arrive.
    """
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    buf = ""
    data_lines: List[str] = []

    def _parse_data_block(lines: List[str]) -> Optional[Dict[str, Any]]:
        payload_str = "\n".join(lines).strip()
        if not payload_str:
            return None
        if payload_str == "[DONE]":
            return {"_done": True}
        try:
            obj = json.loads(payload_str)
            if isinstance(obj, dict):
                return obj
        except Exception as parse_err:
            logging.warning(f"[Horde OAI] Malformed SSE JSON frame: {parse_err} | raw={payload_str[:200]}")
        return None

    async for raw_bytes in resp.content.iter_any():
        if not raw_bytes:
            continue
        buf += decoder.decode(raw_bytes, final=False)
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.rstrip("\r")
            if not line:
                if data_lines:
                    ev = _parse_data_block(data_lines)
                    data_lines.clear()
                    if ev is not None:
                        yield ev
                        if ev.get("_done"):
                            return
                continue
            if line.startswith(":"):
                yield {"_keepalive": line[1:].strip()}
                continue
            if line.startswith("data:"):
                val = line[5:]
                if val.startswith(" "):
                    val = val[1:]
                data_lines.append(val)

    tail = decoder.decode(b"", final=True)
    if tail:
        buf += tail
    if buf:
        for rem in buf.splitlines():
            rem = rem.rstrip("\r")
            if rem.startswith("data:"):
                val = rem[5:]
                if val.startswith(" "):
                    val = val[1:]
                data_lines.append(val)
    if data_lines:
        ev = _parse_data_block(data_lines)
        if ev is not None:
            yield ev


def _extract_oai_error_message(status: int, raw_text: str) -> Tuple[str, str]:
    """Returns `(human_message, error_code)` from an OAI error response body."""
    code = ""
    try:
        data = json.loads(raw_text)
        if isinstance(data, dict) and isinstance(data.get("error"), dict):
            err = data["error"]
            code = str(err.get("code") or err.get("type") or "")
            msg = str(err.get("message") or "").strip()
            if msg:
                return msg, code
    except Exception:
        pass
    return f"AI Horde OAI error ({status}): {raw_text[:160]}", code


def _build_oai_request_body(
    payload: Dict[str, Any],
    target_models: List[str],
    is_anon: bool,
    single_card: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    options = payload.get("options") or {}
    raw_messages = payload.get("messages") or []

    has_user_images = any(
        isinstance(m, dict) and m.get("role") == "user" and bool(m.get("images"))
        for m in raw_messages
    )
    card_supports_images = False
    supported_params: set[str] = set()
    if single_card:
        arch = single_card.get("architecture") or {}
        card_supports_images = "image" in (arch.get("input_modalities") or [])
        supported_params = set(single_card.get("supported_parameters") or [])

    oai_messages = format_messages_for_oai(
        raw_messages,
        allow_images=has_user_images and (card_supports_images or single_card is None),
    )

    default_max_tok = 512 if is_anon else 1024
    req_max_tok = int(
        payload.get("max_tokens")
        or payload.get("max_completion_tokens")
        or options.get("num_predict")
        or default_max_tok
    )
    if is_anon and req_max_tok > 512:
        req_max_tok = 512
    req_max_tok = max(16, min(req_max_tok, 16384))

    temperature = float(payload.get("temperature", options.get("temperature", 0.85)))
    temperature = max(0.0, min(temperature, 2.0))

    top_p = float(payload.get("top_p", options.get("top_p", 0.9)))
    top_p = max(0.0, min(top_p, 1.0))

    body: Dict[str, Any] = {
        "messages": oai_messages,
        "stream": True,
        "stream_options": {"include_usage": True},
        "max_tokens": req_max_tok,
        "temperature": temperature,
        "top_p": top_p,
    }

    # Exactly one of `model` or `models` (1..5 unique items)
    dedup_models: List[str] = []
    for m in target_models:
        m_clean = str(m or "").strip()
        if m_clean and m_clean not in dedup_models:
            dedup_models.append(m_clean)
        if len(dedup_models) >= 5:
            break

    if len(dedup_models) == 1:
        body["model"] = dedup_models[0]
    else:
        body["models"] = dedup_models or list(DEFAULT_HORDE_MODELS[:5])

    # Only include penalties if supported (non-zero penalties filter out workers lacking the capability)
    freq_pen = payload.get("frequency_penalty", options.get("frequency_penalty"))
    if freq_pen is not None and float(freq_pen) != 0.0 and single_card and "frequency_penalty" in supported_params:
        body["frequency_penalty"] = max(-2.0, min(float(freq_pen), 2.0))

    pres_pen = payload.get("presence_penalty", options.get("presence_penalty"))
    if pres_pen is not None and float(pres_pen) != 0.0 and single_card and "presence_penalty" in supported_params:
        body["presence_penalty"] = max(-2.0, min(float(pres_pen), 2.0))

    if payload.get("stop"):
        stops = payload["stop"]
        if isinstance(stops, str):
            body["stop"] = stops
        elif isinstance(stops, list) and stops:
            body["stop"] = [str(s) for s in stops[:4]]

    if payload.get("tools"):
        body["tools"] = payload["tools"]
        if payload.get("tool_choice") is not None:
            body["tool_choice"] = payload["tool_choice"]
        if payload.get("parallel_tool_calls") is not None:
            body["parallel_tool_calls"] = bool(payload["parallel_tool_calls"])

    if payload.get("response_format"):
        body["response_format"] = payload["response_format"]

    want_reasoning = bool(payload.get("think", False))
    user_horde_opts = payload.get("horde") if isinstance(payload.get("horde"), dict) else {}
    horde_opts: Dict[str, Any] = {
        "conformance": "best_effort",
        "context_fit": "truncate_oldest",
        "availability_mode": "wait",
        "include_reasoning": want_reasoning,
        "trusted_workers": False,
        "slow_workers": True,
    }
    horde_opts.update(user_horde_opts)
    body["horde"] = horde_opts

    return body


async def _horde_generate_oai_stream(
    payload: Dict[str, Any],
    api_key: str = ANONYMOUS_API_KEY,
    preferred_model: Optional[str] = None,
    client_agent: str = DEFAULT_CLIENT_AGENT,
    check_interval: float = 1.5,
    max_wait_seconds: int = 180,
) -> AsyncGenerator[Dict[str, Any], None]:
    """
    Real-time SSE streaming via AI Horde's OpenAI-compatible Chat Completions endpoint
    (`POST https://oai.aihorde.net/v1/chat/completions`).
    """
    clean_key = (api_key or "").strip()
    is_anon = (not clean_key) or (clean_key == ANONYMOUS_API_KEY)
    req_model = str(preferred_model or payload.get("model") or "").strip()
    if req_model.lower() == "auto":
        req_model = ""

    oai_base = HORDE_OAI_BASE_URL.rstrip("/")
    is_live_oai = (oai_base == _DEFAULT_HORDE_OAI_BASE_URL.rstrip("/"))

    timeout = aiohttp.ClientTimeout(
        total=max_wait_seconds,
        connect=15,
        sock_read=max_wait_seconds,
    )

    request_id: Optional[str] = None
    task_terminal = False

    try:
        async with aiohttp.ClientSession(timeout=timeout) as session:
            oai_cards: List[Dict[str, Any]] = []
            leg_models: List[Dict[str, Any]] = []

            if is_live_oai:
                oai_res, leg_res = await asyncio.gather(
                    _fetch_oai_model_cards(session, client_agent=client_agent),
                    _fetch_legacy_text_models(session, client_agent=client_agent),
                    return_exceptions=True,
                )
                oai_cards = oai_res if isinstance(oai_res, list) else []
                leg_models = leg_res if isinstance(leg_res, list) else []

            oai_by_id = {str(c.get("id")): c for c in oai_cards if isinstance(c, dict) and c.get("id")}
            healthy_oai_ids = [
                str(c["id"]) for c in oai_cards
                if isinstance(c, dict) and c.get("id") and not _is_model_quarantined(str(c["id"]))
            ]
            ranked_oai_ids = healthy_oai_ids or [
                str(c["id"]) for c in oai_cards if isinstance(c, dict) and c.get("id")
            ]
            leg_online = {
                str(m.get("name")): int(m.get("count") or 0)
                for m in leg_models
                if isinstance(m, dict) and m.get("name") and int(m.get("count") or 0) > 0
            }

            # If user explicitly picked a model that is online ONLY on legacy non-OAI workers,
            # transparently delegate to legacy polling so their exact chosen model serves it.
            if is_live_oai and req_model and (req_model not in oai_by_id) and (req_model in leg_online):
                logging.info(
                    f"[Horde OAI] Model '{req_model}' is online on legacy workers only "
                    f"(count={leg_online[req_model]}); falling back to legacy async endpoint."
                )
                async with contextlib.aclosing(
                    _horde_generate_legacy_stream(
                        payload=payload,
                        api_key=api_key,
                        preferred_model=req_model,
                        client_agent=client_agent,
                        check_interval=check_interval,
                        max_wait_seconds=max_wait_seconds,
                    )
                ) as leg_gen:
                    async for ev in leg_gen:
                        yield ev
                task_terminal = True
                return

            has_images = any(
                isinstance(m, dict) and m.get("role") == "user" and bool(m.get("images"))
                for m in (payload.get("messages") or [])
            )
            if has_images and ranked_oai_ids:
                vision_ids = [
                    cid for cid in ranked_oai_ids
                    if "image" in ((oai_by_id.get(cid, {}).get("architecture") or {}).get("input_modalities") or [])
                ]
            else:
                vision_ids = []

            fallback_pool = vision_ids or ranked_oai_ids or list(DEFAULT_HORDE_MODELS)

            single_card: Optional[Dict[str, Any]] = None
            if req_model:
                if not is_live_oai or req_model in oai_by_id:
                    target_models = [req_model]
                    single_card = oai_by_id.get(req_model)
                else:
                    # Requested model is currently offline everywhere (e.g. stale default in localStorage):
                    # include it first plus up to 4 online OAI models so Horde serves immediately.
                    target_models = [req_model] + [m for m in fallback_pool if m != req_model][:4]
                    logging.info(
                        f"[Horde OAI] Requested model '{req_model}' is offline; submitting multi-model list: {target_models}"
                    )
            else:
                target_models = fallback_pool[:5]

            headers: Dict[str, str] = {
                "Content-Type": "application/json",
                "Accept": "text/event-stream",
                "Client-Agent": client_agent,
            }
            if not is_anon:
                headers["Authorization"] = f"Bearer {clean_key}"

            yield {"status": "Орда: подключение к потоковому воркеру..."}

            for attempt in range(2):
                req_body = _build_oai_request_body(
                    payload=payload,
                    target_models=target_models,
                    is_anon=is_anon,
                    single_card=single_card if len(target_models) == 1 else None,
                )
                logging.info(
                    f"[Horde OAI] Submitting SSE chat request (attempt {attempt + 1}): "
                    f"model={req_body.get('model') or req_body.get('models')}, "
                    f"max_tokens={req_body.get('max_tokens')}, auth={'priority' if not is_anon else 'anon'}"
                )

                started_at = time.monotonic()
                last_wait_status_at = started_at

                async with session.post(
                    f"{oai_base}/chat/completions",
                    json=req_body,
                    headers=headers,
                ) as resp:
                    if resp.status != 200:
                        err_raw = await resp.text()
                        err_msg, err_code = _extract_oai_error_message(resp.status, err_raw)
                        should_retry_hdr = (resp.headers.get("x-should-retry") or "").strip().lower()
                        can_retry_safely = (should_retry_hdr != "false")

                        logging.warning(
                            f"[Horde OAI] HTTP {resp.status} (code={err_code}, x-should-retry={should_retry_hdr}): {err_msg}"
                        )

                        if (
                            attempt == 0
                            and can_retry_safely
                            and resp.status in (404, 422, 503)
                            and len(target_models) == 1
                            and fallback_pool
                        ):
                            alt_models = [m for m in fallback_pool if m != target_models[0]][:5]
                            if alt_models:
                                logging.info(f"[Horde OAI] Retrying with online fallback pool: {alt_models}")
                                target_models = alt_models
                                single_card = None
                                yield {"status": "Орда: выбранная модель занята, переключение на доступную ноду..."}
                                continue

                        task_terminal = True
                        if resp.status == 503 or err_code == "no_eligible_worker":
                            yield {
                                "error": f"В Орде сейчас нет свободных воркеров для выбранной модели ({err_msg})",
                                "done": True,
                            }
                        else:
                            yield {"error": err_msg, "done": True}
                        return

                    actual_model = target_models[0] if target_models else "unknown"
                    worker_name = "AI Horde"
                    worker_id: Optional[str] = None
                    kudos_used: Optional[float] = None
                    finish_reason: Optional[str] = None
                    usage_info: Optional[Dict[str, Any]] = None
                    collected_tool_calls: List[Dict[str, Any]] = []
                    received_any_delta = False
                    announced_generation = False
                    approx_chunks = 0

                    stream_retry_needed = False
                    async for event in _iter_sse_events(resp):
                        if event.get("_done"):
                            task_terminal = True
                            break

                        if "_keepalive" in event:
                            now = time.monotonic()
                            if not received_any_delta and (now - last_wait_status_at) >= 4.0:
                                last_wait_status_at = now
                                waited = int(now - started_at)
                                yield {"status": f"Орда: ожидание свободной ноды ({waited}с)..."}
                            continue

                        # Mid-stream error envelope sent as SSE event
                        if isinstance(event.get("error"), dict):
                            task_terminal = True
                            err_obj = event["error"]
                            e_code = str(err_obj.get("code") or "")
                            e_msg = str(err_obj.get("message") or "Ошибка потока AI Horde")
                            logging.warning(f"[Horde OAI] Mid-stream error event: {err_obj}")
                            if e_code == "ChatRequestFaulted":
                                _quarantine_faulted_model(actual_model)

                            if (
                                attempt == 0
                                and not received_any_delta
                                and e_code in ("ChatRequestFaulted", "ChatRequestNoGeneration", "HordeUnavailable")
                                and fallback_pool
                            ):
                                alt_models = [
                                    m for m in fallback_pool
                                    if m != actual_model and not _is_model_quarantined(m)
                                ][:5] or [m for m in fallback_pool if m != actual_model][:5]
                                if alt_models:
                                    logging.info(
                                        f"[Horde OAI] Worker faulted before first token on '{actual_model}'; "
                                        f"failing over to {alt_models}"
                                    )
                                    target_models = alt_models
                                    single_card = None
                                    stream_retry_needed = True
                                    break

                            yield {"error": e_msg, "done": True}
                            return

                        chunk_id = str(event.get("id") or "")
                        if chunk_id and not request_id:
                            request_id = chunk_id[len("chatcmpl-"):] if chunk_id.startswith("chatcmpl-") else chunk_id

                        if event.get("model"):
                            actual_model = str(event["model"])

                        if isinstance(event.get("x_horde"), dict):
                            xh = event["x_horde"]
                            if xh.get("request_id"):
                                request_id = str(xh["request_id"])
                            if xh.get("worker_name"):
                                worker_name = str(xh["worker_name"])
                            if xh.get("worker_id"):
                                worker_id = str(xh["worker_id"])
                            if xh.get("kudos") is not None:
                                try:
                                    kudos_used = float(xh["kudos"])
                                except (TypeError, ValueError):
                                    pass

                        if isinstance(event.get("usage"), dict):
                            usage_info = event["usage"]

                        choices = event.get("choices")
                        if not isinstance(choices, list):
                            continue

                        for choice in choices:
                            if not isinstance(choice, dict):
                                continue
                            if choice.get("finish_reason"):
                                finish_reason = str(choice["finish_reason"])
                                task_terminal = True

                            delta = choice.get("delta")
                            if not isinstance(delta, dict):
                                continue

                            if not announced_generation and (
                                delta.get("role") or delta.get("content") or delta.get("reasoning_content") or delta.get("tool_calls")
                            ):
                                announced_generation = True
                                short_m = actual_model.split("/")[-1]
                                yield {"status": f"Орда: генерация ответа ({short_m})..."}

                            r_delta = delta.get("reasoning_content")
                            if isinstance(r_delta, str) and r_delta:
                                received_any_delta = True
                                approx_chunks += 1
                                yield {
                                    "message": {
                                        "role": "assistant",
                                        "thinking": r_delta,
                                    }
                                }

                            c_delta = delta.get("content")
                            if isinstance(c_delta, str) and c_delta:
                                c_clean = _OAI_STOP_TOKENS_RE.sub("", c_delta)
                                if c_clean:
                                    received_any_delta = True
                                    approx_chunks += 1
                                    yield {
                                        "message": {
                                            "role": "assistant",
                                            "content": c_clean,
                                        }
                                    }

                            tc_delta = delta.get("tool_calls")
                            if isinstance(tc_delta, list) and tc_delta:
                                received_any_delta = True
                                collected_tool_calls.extend(tc_delta)
                                yield {
                                    "message": {
                                        "role": "assistant",
                                        "tool_calls": tc_delta,
                                    }
                                }

                    if stream_retry_needed:
                        task_terminal = False
                        request_id = None
                        yield {"status": "Орда: воркер недоступен, переключение на резервную ноду..."}
                        continue

                    task_terminal = True
                    eval_count = (
                        int(usage_info["completion_tokens"])
                        if isinstance(usage_info, dict) and usage_info.get("completion_tokens") is not None
                        else approx_chunks
                    )
                    worker_label = f"{worker_name} [{actual_model}]"
                    logging.info(
                        f"[Horde OAI] Stream finished: {worker_label}, reason={finish_reason or 'stop'}, "
                        f"tokens={eval_count}, kudos={kudos_used}"
                    )

                    done_payload: Dict[str, Any] = {
                        "done": True,
                        "done_reason": finish_reason or "stop",
                        "eval_count": eval_count,
                        "worker_info": worker_label,
                        "horde_meta": {
                            "request_id": request_id,
                            "worker_name": worker_name,
                            "worker_id": worker_id,
                            "model": actual_model,
                            "kudos": kudos_used,
                            "usage": usage_info,
                        },
                    }
                    if collected_tool_calls:
                        done_payload["tool_calls"] = collected_tool_calls
                    yield done_payload
                    return

    except asyncio.CancelledError:
        logging.info(f"[Horde OAI] Stream cancelled by caller, aborting request {request_id}")
        raise
    except asyncio.TimeoutError:
        logging.warning(f"[Horde OAI] Stream timed out after {max_wait_seconds}s (request_id={request_id})")
        yield {"error": f"Превышено время ожидания ответа от Орды ({max_wait_seconds}с)", "done": True}
    except Exception as e:
        logging.error(f"[Horde OAI] Unexpected stream exception: {e}")
        yield {"error": f"AI Horde connection error: {str(e)}", "done": True}
    finally:
        if request_id and not task_terminal:
            try:
                await asyncio.shield(cancel_horde_task(request_id, api_key, client_agent))
            except Exception as cancel_err:
                logging.warning(f"[Horde OAI] Failed to cancel abandoned request {request_id}: {cancel_err}")


async def _horde_generate_legacy_stream(
    payload: Dict[str, Any],
    api_key: str = ANONYMOUS_API_KEY,
    preferred_model: Optional[str] = None,
    client_agent: str = DEFAULT_CLIENT_AGENT,
    check_interval: float = 1.5,
    max_wait_seconds: int = 180,
) -> AsyncGenerator[Dict[str, Any], None]:
    """
    Legacy AI Horde text generation (`/api/v2/generate/text/async`) with status polling
    and smooth chunk delivery. Used for raw prompt completions and non-OAI legacy workers.
    """
    messages = payload.get("messages", [])
    if messages:
        prompt = format_messages_for_horde(messages)
    else:
        prompt = payload.get("prompt", "")

    options = payload.get("options", {})
    is_anon = (not api_key) or (api_key == ANONYMOUS_API_KEY)

    requested_length = options.get("num_predict", 500 if is_anon else 1024)
    if is_anon and requested_length > 500:
        requested_length = 500

    requested_ctx = options.get("num_ctx", 2048 if is_anon else 8192)
    if is_anon and requested_ctx > 2048:
        requested_ctx = 2048

    gen_params = {
        "n": 1,
        "max_context_length": requested_ctx,
        "max_length": requested_length,
        "temperature": options.get("temperature", 0.85),
        "top_p": options.get("top_p", 0.9),
        "rep_pen": options.get("repeat_penalty", 1.1),
        "singleline": False,
        "stop_sequence": [
            "<|im_end|>", "<|im_start|>", "<|end_of_text|>", "<|endoftext|>",
            "<|eot_id|>", "<|eom_id|>", "</s>", "<end_of_turn>"
        ],
    }

    models = []
    if preferred_model:
        models.append(preferred_model)
    elif payload.get("model"):
        models.append(payload.get("model"))
    else:
        models.extend(DEFAULT_HORDE_MODELS)

    req_body = {
        "prompt": prompt,
        "params": gen_params,
        "models": models,
        "trusted_workers": False,
    }

    headers = {
        "Content-Type": "application/json",
        "apikey": api_key or ANONYMOUS_API_KEY,
        "Client-Agent": client_agent,
    }

    task_id = None
    task_terminal = False
    try:
        timeout = aiohttp.ClientTimeout(total=max_wait_seconds, connect=15)
        async with aiohttp.ClientSession(timeout=timeout) as session:
            async with session.post(f"{HORDE_BASE_URL}/generate/text/async", json=req_body, headers=headers) as resp:
                if resp.status not in (200, 202):
                    err_text = await resp.text()
                    logging.error(f"[Horde] Submit failed ({resp.status}): {err_text}")
                    yield {"error": f"AI Horde error ({resp.status}): {err_text[:120]}", "done": True}
                    return

                res_json = await resp.json()
                task_id = res_json.get("id")

            if not task_id:
                yield {"error": "AI Horde did not return a task ID", "done": True}
                return

            logging.info(f"[Horde] Legacy task submitted: {task_id}, requested models: {models}")
            yield {"status": "Орда: поиск свободной ноды..."}

            elapsed = 0.0
            last_queue = None
            while elapsed < max_wait_seconds:
                await asyncio.sleep(check_interval)
                elapsed += check_interval

                status_url = f"{HORDE_BASE_URL}/generate/text/status/{task_id}"
                async with session.get(status_url, headers={"Client-Agent": client_agent}) as s_resp:
                    if s_resp.status != 200:
                        continue
                    st = await s_resp.json()

                if st.get("done") and st.get("generations"):
                    task_terminal = True
                    gen = st["generations"][0]
                    full_text = gen.get("text", "")
                    full_text = STOP_TOKENS_RE.sub("", full_text).strip()
                    worker_name = gen.get("worker_name", "unknown")
                    actual_model = gen.get("model", models[0] if models else "unknown")
                    logging.info(f"[Horde] Legacy generation completed by '{worker_name}' [{actual_model}], length: {len(full_text)}")

                    chunk_size = 4
                    words = full_text.split(" ")
                    for i in range(0, len(words), chunk_size):
                        chunk = " ".join(words[i:i + chunk_size])
                        if i + chunk_size < len(words):
                            chunk += " "
                        yield {
                            "message": {
                                "role": "assistant",
                                "content": chunk,
                            }
                        }
                        await asyncio.sleep(0.02)

                    yield {
                        "done": True,
                        "done_reason": "stop",
                        "eval_count": len(words),
                        "worker_info": f"{worker_name} [{actual_model}]",
                    }
                    return

                if st.get("faulted"):
                    task_terminal = True
                    logging.warning(f"[Horde] Task {task_id} faulted")
                    yield {"error": "Генерация в Орде прервана с ошибкой воркера", "done": True}
                    return

                if st.get("is_possible") is False:
                    logging.warning(f"[Horde] Task {task_id} unsatisfiable: no workers available")
                    yield {"error": "В Орде сейчас нет свободных воркеров для выбранной модели", "done": True}
                    return

                q_pos = st.get("queue_position", 0)
                wait_time = st.get("wait_time", 0)
                if q_pos != last_queue or wait_time > 0:
                    last_queue = q_pos
                    if q_pos > 0:
                        yield {"status": f"Орда: позиция в очереди {q_pos} (~{wait_time}с)"}
                    else:
                        yield {"status": "Орда: воркер генерирует ответ..."}

            yield {"error": f"Превышено время ожидания ответа от Орды ({max_wait_seconds}с)", "done": True}

    except asyncio.CancelledError:
        logging.info(f"[Horde] Stream cancelled by client, aborting task {task_id}")
        raise
    except Exception as e:
        logging.error(f"[Horde] Unexpected stream exception: {e}")
        yield {"error": f"AI Horde connection error: {str(e)}", "done": True}
    finally:
        if task_id and not task_terminal:
            try:
                await asyncio.shield(cancel_horde_task(task_id, api_key, client_agent))
            except Exception as cancel_err:
                logging.warning(f"[Horde] Failed to cancel abandoned task {task_id}: {cancel_err}")


async def horde_generate_stream(
    payload: Dict[str, Any],
    api_key: str = ANONYMOUS_API_KEY,
    preferred_model: Optional[str] = None,
    client_agent: str = DEFAULT_CLIENT_AGENT,
    check_interval: float = 1.5,
    max_wait_seconds: int = 180,
    use_oai: Optional[bool] = None,
) -> AsyncGenerator[Dict[str, Any], None]:
    """
    Submits a chat/text generation request to AI Horde and yields:
    - Status updates: {"status": "..."}
    - Real-time reasoning stream: {"message": {"role": "assistant", "thinking": delta}}
    - Real-time token stream: {"message": {"role": "assistant", "content": delta}}
    - Tool call chunks: {"message": {"role": "assistant", "tool_calls": [...]}}
    - Final completion: {"done": True, "done_reason": ..., "eval_count": ..., "worker_info": ...}

    Uses AI Horde's OpenAI-compatible SSE endpoint (`https://oai.aihorde.net/v1/chat/completions`)
    whenever chat `messages` are provided, falling back to legacy `/generate/text/async`
    for raw `prompt`-only requests or legacy-only workers.
    """
    messages = payload.get("messages")
    legacy_mock_only = (
        HORDE_BASE_URL != _DEFAULT_HORDE_BASE_URL
        and HORDE_OAI_BASE_URL == _DEFAULT_HORDE_OAI_BASE_URL
    )

    if use_oai is None:
        should_use_oai = bool(messages) and not legacy_mock_only
    else:
        should_use_oai = bool(use_oai)

    sub_stream = (
        _horde_generate_oai_stream(
            payload=payload,
            api_key=api_key,
            preferred_model=preferred_model,
            client_agent=client_agent,
            check_interval=check_interval,
            max_wait_seconds=max_wait_seconds,
        )
        if should_use_oai
        else _horde_generate_legacy_stream(
            payload=payload,
            api_key=api_key,
            preferred_model=preferred_model,
            client_agent=client_agent,
            check_interval=check_interval,
            max_wait_seconds=max_wait_seconds,
        )
    )
    async with contextlib.aclosing(sub_stream) as gen:
        async for ev in gen:
            yield ev
