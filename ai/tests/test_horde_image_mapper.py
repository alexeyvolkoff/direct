import asyncio
import base64
import io
from types import SimpleNamespace

from aiohttp import web
from PIL import Image
import pytest

import horde_client
import horde_image_mapper


def _make_webp_bytes(color=(40, 120, 220)) -> bytes:
    img = Image.new("RGB", (64, 64), color=color)
    buf = io.BytesIO()
    img.save(buf, format="WEBP")
    return buf.getvalue()


def test_map_ui_guest_enforces_sfw_and_attaches_civitai_loras():
    payload = horde_image_mapper.map_ui_to_horde_request(
        prompt="Title: Sunset Cafe\nImage: <anime> <domi> <lora:DomiK4:0.8> DomiK woman drinking coffee ### secret",
        persona_prompt="warm brown eyes, cozy sweater",
        sliders={
            "detail": 0.85,
            "negative_prompt": "glasses, hat",
            "archetype_weights": {"domi": 0.9},
        },
        width=896,
        height=1152,
        seed=42,
        is_uncensored=False,
        use_sdxl=True,
        online_models={
            "Nova Anime XL": {"name": "Nova Anime XL", "count": 6, "lora_workers": 5, "fast_lora_workers": 5, "eta": 10},
            "WAI-NSFW-illustrious-SDXL": {"name": "WAI-NSFW-illustrious-SDXL", "count": 9, "lora_workers": 8, "eta": 5},
        },
    )
    assert payload["nsfw"] is False
    assert payload["censor_nsfw"] is True
    assert payload["replacement_filter"] is True
    assert payload["slow_workers"] is False
    assert payload["params"]["width"] == 832
    assert payload["params"]["height"] == 1024
    assert payload["params"]["steps"] == 18
    assert payload["params"]["sampler_name"] == "k_dpmpp_2m"
    assert payload["params"]["karras"] is True
    assert payload["params"]["clip_skip"] == 2
    assert payload["params"]["seed"] == "42"
    assert "Nova Anime XL" in payload["models"] or "WAI-NSFW-illustrious-SDXL" in payload["models"]

    lora_ids = [l["name"] for l in payload["params"]["loras"]]
    assert "1377183" in lora_ids  # SLDomi (XLDomiv2)
    assert "135867" in lora_ids   # Detail Tweaker XL (detail=0.85)

    pos, neg = payload["prompt"].split(" ### ", 1)
    assert "<anime>" not in pos
    assert "<domi>" not in pos
    assert "DomiK" not in pos
    assert "nsfw, nude, naked" in neg
    assert "glasses, hat" in neg


def test_map_ui_anon_sd15_keeps_pixels_under_upfront_kudos_limit():
    for w, h in [(896, 1152), (1344, 768), (1024, 1024)]:
        payload = horde_image_mapper.map_ui_to_horde_request(
            prompt="mountain lake at sunrise",
            sliders={"style": "photorealistic"},
            width=w,
            height=h,
            is_uncensored=False,
            use_sdxl=False,
        )
        out_w = payload["params"]["width"]
        out_h = payload["params"]["height"]
        assert out_w % 64 == 0
        assert out_h % 64 == 0
        assert (out_w * out_h) <= (576 * 576)


def test_daily_guest_kudos_budget_tracking(tmp_path, monkeypatch):
    budget_file = str(tmp_path / "guest_budget.json")
    monkeypatch.setitem(horde_image_mapper.SETTINGS, "HORDE_GUEST_DAILY_KUDOS_LIMIT", "100")
    monkeypatch.setitem(horde_image_mapper.SETTINGS, "HORDE_PER_USER_DAILY_KUDOS_LIMIT", "60")

    ok, rem_g, rem_u = horde_image_mapper.check_guest_priority_budget("guest_1", 30.0, budget_path=budget_file)
    assert ok is True
    assert rem_g == 100.0
    assert rem_u == 60.0

    horde_image_mapper.record_guest_kudos_spent("guest_1", 40.0, budget_path=budget_file)
    ok_after, rem_g2, rem_u2 = horde_image_mapper.check_guest_priority_budget("guest_1", 30.0, budget_path=budget_file)
    assert ok_after is False  # guest_1 only has 20 left (< 30)
    assert rem_u2 == 20.0
    assert rem_g2 == 60.0

    # Different guest still has priority budget until global cap is hit
    ok_other, _, _ = horde_image_mapper.check_guest_priority_budget("guest_2", 30.0, budget_path=budget_file)
    assert ok_other is True


@pytest.mark.asyncio
async def test_generate_horde_image_r2_webp_to_png_and_budget(tmp_path, monkeypatch):
    webp_bytes = _make_webp_bytes()
    submitted_payloads = []

    async def handle_models(request):
        return web.json_response([
            {"name": "AlbedoBase XL (SDXL)", "count": 7, "eta": 5},
            {"name": "Deliberate", "count": 10, "eta": 2},
        ])

    async def handle_submit(request):
        body = await request.json()
        submitted_payloads.append((request.headers.get("apikey"), body))
        return web.json_response({"id": "job-123", "kudos": 30.0}, status=202)

    async def handle_check(request):
        return web.json_response({"done": True, "faulted": False, "queue_position": 0, "wait_time": 0})

    async def handle_status(request):
        base_url = str(request.url.origin())
        return web.json_response({
            "done": True,
            "kudos": 30.0,
            "generations": [
                {
                    "img": f"{base_url}/r2/out.webp",
                    "model": "AlbedoBase XL (SDXL)",
                    "worker_name": "reGen-Node-1",
                    "censored": False,
                }
            ],
        })

    async def handle_r2(request):
        return web.Response(body=webp_bytes, content_type="image/webp")

    app = web.Application()
    app.router.add_get("/api/v2/status/models", handle_models)
    app.router.add_post("/api/v2/generate/async", handle_submit)
    app.router.add_get("/api/v2/generate/check/job-123", handle_check)
    app.router.add_get("/api/v2/generate/status/job-123", handle_status)
    app.router.add_get("/r2/out.webp", handle_r2)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]

    try:
        monkeypatch.setattr(horde_client, "HORDE_BASE_URL", f"http://127.0.0.1:{port}/api/v2")
        monkeypatch.setattr(horde_image_mapper, "_image_models_cache", (0.0, {}))
        monkeypatch.setattr(horde_image_mapper, "_last_submit_monotonic", 0.0)
        monkeypatch.setitem(horde_image_mapper.SETTINGS, "HORDE_WORKER_API_KEY", "priority-secret-key")
        monkeypatch.setitem(horde_image_mapper.SETTINGS, "HORDE_GUEST_DAILY_KUDOS_LIMIT", "500")
        monkeypatch.setitem(horde_image_mapper.SETTINGS, "HORDE_PER_USER_DAILY_KUDOS_LIMIT", "100")

        budget_file = str(tmp_path / "budget.json")
        guest_ctx = SimpleNamespace(user_id="guest_abc", private_mode=False, trusted_owner=False, is_unlimited=False, settings={"style": "realistic"})
        stages = []

        async def on_prog(stage, **info):
            stages.append(stage)

        res = await horde_image_mapper.generate_horde_image(
            ctx=guest_ctx,
            prompt="cozy cabin in snowy forest",
            width=896,
            height=1152,
            is_uncensored=True,  # must be ignored for guest!
            progress_cb=on_prog,
            poll_interval=0.05,
            budget_path=budget_file,
        )

        assert res["tier_used"] == "priority_sdxl"
        assert res["model"] == "AlbedoBase XL (SDXL)"
        assert res["worker_name"] == "reGen-Node-1"
        assert res["image_bytes"][:8] == b"\x89PNG\r\n\x1a\n"
        assert "warming" in stages and "saving" in stages

        api_key_sent, sent_body = submitted_payloads[0]
        assert api_key_sent == "priority-secret-key"
        assert sent_body["nsfw"] is False
        assert sent_body["censor_nsfw"] is True

        _, _, rem_u = horde_image_mapper.check_guest_priority_budget("guest_abc", 10.0, budget_path=budget_file)
        assert rem_u == 70.0
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
async def test_generate_horde_image_downgrades_on_kudos_upfront_and_blocks_censored(tmp_path, monkeypatch):
    png_img = Image.new("RGB", (32, 32), color=(0, 0, 0))
    buf = io.BytesIO()
    png_img.save(buf, format="PNG")
    b64_png = base64.b64encode(buf.getvalue()).decode("ascii")

    calls = []

    async def handle_models(request):
        return web.json_response([{"name": "Deliberate", "count": 8, "eta": 1}])

    async def handle_submit(request):
        key = request.headers.get("apikey")
        body = await request.json()
        calls.append((key, body))
        if key != "0000000000":
            return web.json_response({"message": "Need upfront kudos", "rc": "KudosUpfront"}, status=403)
        return web.json_response({"id": "job-anon", "kudos": 8.0}, status=202)

    async def handle_check(request):
        return web.json_response({"done": True, "faulted": False})

    async def handle_status(request):
        return web.json_response({
            "done": True,
            "kudos": 8.0,
            "generations": [
                {
                    "img": b64_png,
                    "model": "Deliberate",
                    "worker_name": "reGen-Node-2",
                    "censored": True,
                }
            ],
        })

    app = web.Application()
    app.router.add_get("/api/v2/status/models", handle_models)
    app.router.add_post("/api/v2/generate/async", handle_submit)
    app.router.add_get("/api/v2/generate/check/job-anon", handle_check)
    app.router.add_get("/api/v2/generate/status/job-anon", handle_status)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]

    try:
        monkeypatch.setattr(horde_client, "HORDE_BASE_URL", f"http://127.0.0.1:{port}/api/v2")
        monkeypatch.setattr(horde_image_mapper, "_image_models_cache", (0.0, {}))
        monkeypatch.setattr(horde_image_mapper, "_last_submit_monotonic", 0.0)
        monkeypatch.setitem(horde_image_mapper.SETTINGS, "HORDE_WORKER_API_KEY", "depleted-key")

        guest_ctx = SimpleNamespace(user_id="guest_xyz", private_mode=False, trusted_owner=False, is_unlimited=False, settings={})
        with pytest.raises(horde_image_mapper.HordeCensoredError):
            await horde_image_mapper.generate_horde_image(
                ctx=guest_ctx,
                prompt="borderline prompt",
                poll_interval=0.05,
                budget_path=str(tmp_path / "budget2.json"),
            )

        assert len(calls) == 2
        assert calls[0][0] == "depleted-key"
        assert calls[1][0] == "0000000000"
        assert calls[1][1]["params"]["width"] == 512
        assert calls[1][1]["params"]["height"] == 640
    finally:
        await runner.cleanup()


def test_guest_krea_presets_excludes_all_krea_and_anima_loras():
    import core_service

    guest_presets = core_service.get_krea_presets(include_fun=False, include_krea=False)
    style_ids = [s["id"] for s in guest_presets["art_styles"]]
    arch_ids = [a["id"] for a in guest_presets["archetypes"]]

    # Zero Krea 2 or Anima presets leaked to guests
    for s in guest_presets["art_styles"]:
        assert "krea2" not in (s.get("models") or [])
        assert "anima" not in (s.get("models") or [])
    for a in guest_presets["archetypes"]:
        assert a.get("models") == ["sdxl"]
        assert a["id"].startswith("sdxl_")

    assert "none" in style_ids
    assert "sdxl_lora_real_humans" in style_ids
    assert "sdxl_lora_busty" not in style_ids
    assert "sdxl_judith" in arch_ids
    assert "domi" not in arch_ids
    assert "kate" not in arch_ids
    assert "frieren" not in arch_ids

    # Standard SDXL LoRA IDs + custom CivitAI LoRAs + 8-step TurboMix map cleanly onto AI Horde params.loras
    mapped = horde_image_mapper.map_ui_to_horde_request(
        prompt="reading a book in a sunlit library",
        sliders={
            "base_model": "sdxl_fantasy",
            "styles": ["none", "sdxl_lora_real_humans"],
            "archetypes": ["sdxl_oriana", "civitai_777888"],
            "custom_loras": [
                {
                    "id": "civitai_777888",
                    "name": "Custom Companion",
                    "civitai_version_id": "777888",
                    "strength": 0.8,
                    "trained_words": ["cust_trig"],
                    "category": "character",
                }
            ],
            "steps": 8,
            "horde_native": True,
        },
        is_uncensored=False,
        use_sdxl=True,
    )
    pos, _ = mapped["prompt"].split(" ### ", 1)
    assert "<Oriana>" not in pos
    assert "OR14N4" in pos
    assert "cust_trig" in pos
    assert mapped["params"]["steps"] == 8
    assert mapped["params"]["sampler_name"] == "k_dpmpp_sde"
    lora_ids = [l["name"] for l in mapped["params"]["loras"]]
    assert lora_ids[0] == "247778"  # Haidra pre-cached 12MB TurboMix
    assert "2903793" in lora_ids    # Oriana
    assert "777888" in lora_ids     # Custom CivitAI LoRA
    assert "262705" in lora_ids     # Real Humans


def test_civitai_lora_card_validator_enforces_400mb_and_sdxl():
    base_model_obj = {"id": 100, "name": "Test LoRA", "poi": False, "tags": ["character"]}
    valid_ver = {
        "id": 555666,
        "baseModel": "Illustrious",
        "trainedWords": ["hero_char"],
        "files": [{"name": "hero.safetensors", "primary": True, "sizeKB": 220 * 1024, "downloadUrl": "https://x/y", "hashes": {"SHA256": "ABC"}}],
        "images": [{"type": "image", "nsfwLevel": 1, "url": "https://image.civitai.com/x/y/original=true/1.jpeg"}],
    }
    card = horde_image_mapper._extract_civitai_lora_card(base_model_obj, valid_ver, "character")
    assert card is not None
    assert card["id"] == "civitai_555666"
    assert "/width=256/" in card["image_url"]

    # Over 400 MB -> rejected
    too_big = dict(valid_ver, files=[{"name": "big.safetensors", "primary": True, "sizeKB": 450 * 1024, "downloadUrl": "https://x/y", "hashes": {"SHA256": "ABC"}}])
    assert horde_image_mapper._extract_civitai_lora_card(base_model_obj, too_big, "character") is None

    # SD 1.5 -> rejected for SDXL picker
    sd15_ver = dict(valid_ver, baseModel="SD 1.5")
    assert horde_image_mapper._extract_civitai_lora_card(base_model_obj, sd15_ver, "character") is None


@pytest.mark.asyncio
async def test_local_comfy_preferred_when_available_and_local_lora_resolved(tmp_path, monkeypatch):
    guest_ctx = SimpleNamespace(user_id="guest_local", private_mode=False, trusted_owner=False, is_unlimited=False, settings={})
    owner_ctx = SimpleNamespace(user_id="alexey", private_mode=True, trusted_owner=True, is_unlimited=True, settings={})

    monkeypatch.setitem(horde_image_mapper.SETTINGS, "HORDE_IMAGE_FORCE_FOR_GUESTS", "false")
    assert horde_image_mapper.should_use_horde_for_sdxl(guest_ctx, comfy_available=True) is False
    assert horde_image_mapper.should_use_horde_for_sdxl(guest_ctx, comfy_available=False) is True
    assert horde_image_mapper.should_use_horde_for_sdxl(owner_ctx, comfy_available=True) is False

    monkeypatch.setitem(horde_image_mapper.SETTINGS, "HORDE_IMAGE_FORCE_FOR_GUESTS", "true")
    assert horde_image_mapper.should_use_horde_for_sdxl(guest_ctx, comfy_available=True) is True

    # Local pre-installed LoRA file resolution (e.g. XLDomiv2.safetensors)
    fake_domi = tmp_path / "XLDomiv2.safetensors"
    fake_domi.write_bytes(b"0" * 70000)
    resolved = await horde_image_mapper.ensure_local_civitai_lora(
        "sdxl_domi",
        loras_dir=str(tmp_path),
    )
    assert resolved is not None
    assert resolved["filename"] == "XLDomiv2.safetensors"
    assert "Domi" in resolved["trained_words"]
    assert resolved["strength"] == 1.0


def test_character_signature_traits_and_flow_clip_wiring():
    import json
    import os

    flow_path = os.path.join(os.path.dirname(__file__), "..", "flow.json")
    with open(flow_path, "r", encoding="utf-8") as f:
        flow = json.load(f)

    # Positive CLIPTextEncode (node 85) must receive CLIP from Power Lora Loader (node 103)
    assert flow["85"]["inputs"]["clip"] == ["103", 1]
    assert flow["103"]["inputs"]["lora_11"]["on"] is False
    assert flow["126"]["inputs"]["strength_clip"] == 0

    # Without explicit hair color, signature_traits differentiate each companion LoRA
    mapped_vera = horde_image_mapper.map_ui_to_horde_request(
        prompt="Girl wearing denim jeans and white blouse walks down the hallway",
        sliders={"base_model": "sdxl_perfect", "archetypes": ["sdxl_vera"]},
        is_uncensored=False,
        use_sdxl=True,
    )
    pos_vera, _ = mapped_vera["prompt"].split(" ### ", 1)
    assert "V3r0t1k4" in pos_vera
    assert "vibrant fiery red hair" in pos_vera

    # Explicit hair color from user overrides default signature hair trait
    mapped_custom_hair = horde_image_mapper.map_ui_to_horde_request(
        prompt="brunette girl wearing denim jeans and white blouse walks down the hallway",
        sliders={"base_model": "sdxl_perfect", "archetypes": ["sdxl_vera"]},
        is_uncensored=False,
        use_sdxl=True,
    )
    pos_custom, _ = mapped_custom_hair["prompt"].split(" ### ", 1)
    assert "V3r0t1k4" in pos_custom
    assert "vibrant fiery red hair" not in pos_custom


@pytest.mark.asyncio
async def test_owner_chat_uses_sdxl_except_for_krea2_personas(monkeypatch):
    import core_service

    async def fake_krea_avail(ctx=None):
        return True

    async def fake_krea_status(ctx):
        return {"allowed": True, "krea_allowed": True, "tier": "owner"}

    monkeypatch.setattr(core_service, "is_krea_available", fake_krea_avail)
    monkeypatch.setattr(core_service, "check_krea_status", fake_krea_status)

    # 1. Owner in default assistant chat -> SDXL (not Krea 2), Domi maps to sdxl_domi, no avatar faceswap
    owner_default = SimpleNamespace(
        user_id="alexey",
        private_mode=True,
        trusted_owner=True,
        is_unlimited=True,
        settings={"style": "perfect", "assistant_name": "June", "assistant_model": "Domi"},
    )
    assert core_service.is_krea2_persona_chat(owner_default) is False
    _, _, sliders_def = core_service.extract_krea2_prompt_and_params(owner_default, "smiling in the kitchen", is_character=True)
    assert sliders_def["archetypes"] == ["sdxl_domi"]
    eff_def, can_krea_def = await core_service._resolve_chat_image_sliders(owner_default, sliders_def)
    assert can_krea_def is False
    assert eff_def["base_model"] == "sdxl_perfect"
    ref_av, _, _, _ = core_service._resolve_character_ref_avatar(owner_default, eff_def)
    assert ref_av is None

    # 2. Owner in chat with SDXL persona -> SDXL
    owner_sdxl_persona = SimpleNamespace(
        user_id="alexey",
        private_mode=True,
        trusted_owner=True,
        is_unlimited=True,
        settings={
            "persona_id": "persona_sdxl_minerva_1",
            "persona_name": "Minerva",
            "persona_archetype": "sdxl_minerva",
            "persona_lora": "MINERVA-XSTAR-XL-FINAL.safetensors",
            "persona_render_preset": {"enabled": True, "base_model": "sdxl_fantasy", "archetypes": ["sdxl_minerva"]},
        },
    )
    assert core_service.is_krea2_persona_chat(owner_sdxl_persona) is False
    _, _, sliders_sdxl_p = core_service.extract_krea2_prompt_and_params(owner_sdxl_persona, "portrait", is_character=True)
    eff_sdxl_p, can_krea_sdxl_p = await core_service._resolve_chat_image_sliders(owner_sdxl_persona, sliders_sdxl_p)
    assert can_krea_sdxl_p is False
    assert eff_sdxl_p["base_model"] == "sdxl_fantasy"

    # 3. Owner in chat with plain text-only persona (no Krea2 preset/LoRA) -> SDXL
    owner_plain_persona = SimpleNamespace(
        user_id="alexey",
        private_mode=True,
        trusted_owner=True,
        is_unlimited=True,
        settings={
            "persona_id": "persona_1793725073722",
            "persona_name": "Dana",
            "persona_personality": "Warm artist",
            "persona_render_preset": None,
        },
    )
    assert core_service.is_krea2_persona_chat(owner_plain_persona) is False
    eff_plain, can_krea_plain = await core_service._resolve_chat_image_sliders(owner_plain_persona, {})
    assert can_krea_plain is False
    assert eff_plain["base_model"] == "sdxl_realistic"

    # 4. Owner in chat with Krea 2 persona (e.g. Lark / Kate Krea2) -> Krea 2 enabled!
    owner_krea_persona = SimpleNamespace(
        user_id="alexey",
        private_mode=True,
        trusted_owner=True,
        is_unlimited=True,
        settings={
            "persona_id": "persona_shinylark",
            "persona_name": "Lark",
            "persona_archetype": "kate",
            "persona_lora": "kate_krea2.safetensors",
            "persona_render_preset": {"enabled": True, "base_model": "realism", "archetypes": ["kate"]},
        },
    )
    assert core_service.is_krea2_persona_chat(owner_krea_persona) is True
    _, _, sliders_krea_p = core_service.extract_krea2_prompt_and_params(owner_krea_persona, "walking in the rain", is_character=True)
    eff_krea_p, can_krea_p = await core_service._resolve_chat_image_sliders(owner_krea_persona, sliders_krea_p)
    assert can_krea_p is True
    assert eff_krea_p["base_model"] == "realism"

    # 5. Owner in chat with manual Krea 2 persona marked via persona_engine="krea2" -> Krea 2 enabled!
    owner_manual_krea = SimpleNamespace(
        user_id="alexey",
        private_mode=True,
        trusted_owner=True,
        is_unlimited=True,
        settings={
            "persona_id": "persona_manual_k2",
            "persona_name": "Chloe",
            "persona_engine": "krea2",
            "persona_render_preset": None,
        },
    )
    assert core_service.is_krea2_persona_chat(owner_manual_krea) is True
    _, can_krea_manual = await core_service._resolve_chat_image_sliders(owner_manual_krea, {})
    assert can_krea_manual is True


@pytest.mark.asyncio
async def test_work_fun_toggle_and_sensitive_tagging():
    import core_service
    import unified_memory

    # 1. Node Owner in Work mode MUST get SFW guardrails (effective fun = False)
    owner_work = SimpleNamespace(
        user_id="alexey",
        private_mode=True,
        trusted_owner=True,
        is_unlimited=True,
        settings={"content_mode": "work"},
    )
    assert await core_service.resolve_effective_fun_mode(owner_work, {"content_mode": "work"}) is False
    assert await core_service.resolve_effective_fun_mode(owner_work) is False

    # 2. Node Owner in Fun mode gets uncensored mode (effective fun = True)
    owner_fun = SimpleNamespace(
        user_id="alexey",
        private_mode=True,
        trusted_owner=True,
        is_unlimited=True,
        settings={"content_mode": "fun"},
    )
    assert await core_service.resolve_effective_fun_mode(owner_fun) is True
    # Slider override to work still forces SFW even if settings had fun
    assert await core_service.resolve_effective_fun_mode(owner_fun, {"content_mode": "work"}) is False

    # 3. Guest cannot bypass SFW even if passing content_mode="fun"
    guest_spoof = SimpleNamespace(
        user_id="guest_123",
        private_mode=False,
        trusted_owner=False,
        is_unlimited=False,
        settings={"content_mode": "fun"},
    )
    assert await core_service.resolve_effective_fun_mode(guest_spoof, {"content_mode": "fun"}) is False

    # 4. Pony / shirt prompts in Work/Guest mode still get modest opaque buttoned clothing + rating_explicit/pokies negative
    mapped_shirt = horde_image_mapper.map_ui_to_horde_request(
        prompt="woman in white shirt walking in office hallway",
        sliders={"base_model": "sdxl_perfect", "archetypes": ["sdxl_oriana"]},
        is_uncensored=False,
        use_sdxl=True,
    )
    pos_s, neg_s = mapped_shirt["prompt"].split(" ### ", 1)
    assert "modest opaque buttoned outfit" in pos_s
    assert "covered chest" in pos_s
    assert "rating_explicit" in neg_s
    assert "pokies" in neg_s
    assert "unbuttoned shirt" in neg_s

    # 5. Mandatory #sensitive tagging at generation & indexing without polluting <!-- prompt -->
    nsfw_readme = core_service.build_companion_readme(
        "Midnight Silhouette",
        "An intimate artistic portrait in soft lighting.",
        prompt="1girl, topless, lace lingerie, bedroom",
        sensitive=core_service.detect_generation_sensitive("1girl, topless, lace lingerie, bedroom"),
    )
    assert "\n#sensitive\n" in nsfw_readme
    assert "<!-- prompt: 1girl, topless, lace lingerie, bedroom -->" in nsfw_readme

    # Indexing extracts 'sensitive' tag and preserves <!-- prompt --> at the very end
    legacy_nsfw_readme = "# Hallway Portrait\n\nSoft corridor lighting.\n\n<!-- prompt: woman, unbuttoned shirt, bare breasts, nsfw -->\n"
    extracted_tags = unified_memory.auto_extract_tags(legacy_nsfw_readme, "KREA_001.png")
    assert "sensitive" in extracted_tags
    updated_readme = unified_memory.append_tags_to_card_text(legacy_nsfw_readme, extracted_tags)
    assert "#sensitive" in updated_readme
    assert updated_readme.strip().endswith("<!-- prompt: woman, unbuttoned shirt, bare breasts, nsfw -->")

    # SFW prompt never false-triggers #sensitive
    sfw_readme = core_service.build_companion_readme(
        "Autumn Park Walk",
        "A woman in a warm trench coat walking among golden leaves.",
        prompt="modest opaque buttoned outfit, covered chest, woman in trench coat walking in autumn park",
        sensitive=core_service.detect_generation_sensitive("woman in trench coat walking in autumn park"),
    )
    assert "#sensitive" not in sfw_readme
    assert "sensitive" not in unified_memory.auto_extract_tags(sfw_readme, "KREA_002.png")

    # Vision recognition formatter preserves #sensitive
    formatted_vis = core_service.format_readme_description(
        "# Studio Portrait\n\nA woman posing in sheer lingerie in a dimly lit room.\n#sensitive"
    )
    assert formatted_vis.endswith("#sensitive")



