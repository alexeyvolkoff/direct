"""
Regression test suite for AI Horde Worker & Client compliance (`horde_dreamer.py` & `horde_client.py`).

Covers all 8 requirements from AI Horde maintainer review:
1. Advertise and execute only reference-verified models (SHA256 + byte size), with zero cross-model fallback.
2. Honor requested generation parameters faithfully and reject unsupported samplers/schedulers/features.
3. Preserve requested dimensions and seeds (including seed 0, uint64 range, portrait/landscape extremes) without silent clamping.
4. Require server acknowledgement before recording completion (separate render success from delivery success; retry transient errors).
5. Give every popped job a terminal handling path (`acknowledged`, `faulted`, or `submit_failed`), never faulting an already accepted result.
6. Enforce an actual wall-clock rendering deadline on silent sockets and clean up hung ComfyUI prompts.
7. Remove original renderer artifacts (local ComfyUI temp/output files and remote Modal artifacts) after read/safety evaluation.
8. Explicitly cancel unfinished client image and text requests on timeout, `is_possible=False`, cancellation, or stream closure.
"""

import asyncio
import base64
import hashlib
import io
import json
import os
import sys
from typing import Any
from unittest.mock import AsyncMock, patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import aiohttp
from aiohttp import web
from PIL import Image
import pytest

import horde_client
import horde_dreamer


def _make_png_bytes(color: tuple[int, int, int] = (32, 64, 128), size: tuple[int, int] = (64, 64)) -> bytes:
    buf = io.BytesIO()
    Image.new("RGB", size, color=color).save(buf, format="PNG")
    return buf.getvalue()


@pytest.fixture(autouse=True)
def _isolated_dreamer_env(tmp_path, monkeypatch):
    """Isolates `DATA_PATH`, `COMFY_CHECKPOINTS_DIR`, `COMFY_TEMP_DIR`, and caches per test."""
    data_file = tmp_path / "horde_dreamer_data.json"
    ckpts_dir = tmp_path / "models" / "checkpoints"
    temp_dir = tmp_path / "temp"
    output_dir = tmp_path / "output"
    ckpts_dir.mkdir(parents=True, exist_ok=True)
    temp_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    monkeypatch.setattr(horde_dreamer, "DATA_PATH", str(data_file))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_CHECKPOINTS_DIR", str(ckpts_dir))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_TEMP_DIR", str(temp_dir))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_OUTPUT_DIR", str(output_dir))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_DIR", str(tmp_path))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_WORKER_API_KEY", "test-worker-key")
    monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_KREA_REFERENCE_VERIFIED", "false")
    horde_dreamer._checkpoint_sha256_cache.clear()
    horde_dreamer._pending_deliveries.clear()
    yield {
        "tmp_path": tmp_path,
        "ckpts_dir": ckpts_dir,
        "temp_dir": temp_dir,
        "output_dir": output_dir,
        "data_file": data_file,
    }


def _install_fake_verified_model(env, model_name: str = "CyberRealistic Pony", content: bytes = b"reference-weights-v8") -> dict[str, Any]:
    """Creates a checkpoint file in `ckpts_dir` and updates catalog entry to match its exact size and SHA256."""
    spec = dict(horde_dreamer.LOCAL_SDXL_MODEL_CATALOG[model_name])
    ckpt_path = env["ckpts_dir"] / spec["ckpt"]
    ckpt_path.write_bytes(content)
    digest = hashlib.sha256(content).hexdigest()
    spec["sha256"] = digest
    spec["size_bytes"] = len(content)
    horde_dreamer.LOCAL_SDXL_MODEL_CATALOG[model_name] = spec
    return spec


# ==============================================================================
# 1. Advertise only the models actually loaded (SHA256 + byte size, no fallback)
# ==============================================================================

def test_point1_advertise_only_verified_reference_weights(monkeypatch, _isolated_dreamer_env):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        # Distinct reference filenames for every catalog entry (no cross-model aliasing)
        ckpts = [spec["ckpt"] for spec in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.values()]
        assert len(ckpts) == len(set(ckpts)), "Catalog must not alias multiple Horde models to the same checkpoint"

        # With no verified files on disk, zero models are advertised
        data = horde_dreamer.load_dreamer_data()
        data["sdxl_enabled"] = True
        data["sdxl_models"] = ["CyberRealistic Pony", "Pony Realism", "SDXL 1.0"]
        data["krea_enabled"] = True
        assert horde_dreamer.get_active_models_for_pop(data) == []

        # Create a file with wrong content/hash -> still not advertised
        bad_path = env["ckpts_dir"] / orig_catalog["CyberRealistic Pony"]["ckpt"]
        bad_path.write_bytes(b"wrong-weights")
        assert horde_dreamer.get_active_models_for_pop(data) == []

        # Install exact matching reference weights for CyberRealistic Pony -> only that model is advertised
        _install_fake_verified_model(env, "CyberRealistic Pony")
        assert horde_dreamer.get_active_models_for_pop(data) == ["CyberRealistic Pony"]
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


@pytest.mark.asyncio
async def test_point1_unexpected_model_faults_without_fallback(monkeypatch, _isolated_dreamer_env):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())

        render_called = False
        fault_submissions: list[dict[str, Any]] = []

        async def fake_render(*args, **kwargs):
            nonlocal render_called
            render_called = True
            return _make_png_bytes()

        monkeypatch.setattr(horde_dreamer, "_run_local_comfy_silent", fake_render)

        app = web.Application()

        async def handle_pop(request):
            return web.json_response({
                "ids": ["job-unexpected-model"],
                "model": "Pony Realism",  # Not verified locally! Must NOT fallback to CyberRealistic Pony
                "payload": {
                    "prompt": "test prompt",
                    "width": 512,
                    "height": 512,
                    "steps": 20,
                    "cfg_scale": 7.0,
                    "sampler_name": "k_euler",
                    "seed": "12345",
                },
            })

        async def handle_submit(request):
            body = await request.json()
            fault_submissions.append(body)
            return web.json_response({"reward": 0.0})

        app.router.add_post("/generate/pop", handle_pop)
        app.router.add_post("/generate/submit", handle_submit)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")

        try:
            res = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
            assert res["status"] == "faulted"
            assert not render_called, "Renderer must never be invoked with a fallback checkpoint"
            assert len(fault_submissions) == 1
            assert fault_submissions[0]["state"] == "faulted"
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# 2. Honor requested generation parameters & reject unsupported combinations
# ==============================================================================

def test_point2_faithful_sdxl_and_krea_workflow_parameters():
    wf = horde_dreamer._build_sdxl_horde_workflow(
        positive_prompt="exact positive prompt",
        negative_prompt="exact negative prompt",
        ckpt_name="cyberrealisticPony_v8.safetensors",
        width=768,
        height=1024,
        seed=42,
        steps=37,
        cfg=6.5,
        sampler_name="k_dpmpp_2m",
        karras=True,
        scheduler="sgm_uniform",
        clip_skip=2,
    )
    assert wf["1"]["inputs"]["ckpt_name"] == "cyberrealisticPony_v8.safetensors"
    assert wf["2"]["inputs"]["stop_at_clip_layer"] == -2
    assert wf["3"]["inputs"]["text"] == "exact positive prompt"
    assert wf["4"]["inputs"]["text"] == "exact negative prompt"
    assert wf["5"]["inputs"] == {"width": 768, "height": 1024, "batch_size": 1}
    assert wf["6"]["inputs"]["seed"] == 42
    assert wf["6"]["inputs"]["steps"] == 37
    assert wf["6"]["inputs"]["cfg"] == 6.5
    assert wf["6"]["inputs"]["sampler_name"] == "dpmpp_2m"
    assert wf["6"]["inputs"]["scheduler"] == "sgm_uniform"
    # Verify no unrequested LoRA or ImageSharpen nodes exist
    class_types = {node["class_type"] for node in wf.values()}
    assert "LoraLoader" not in class_types
    assert "ImageSharpen" not in class_types

    krea_wf = horde_dreamer._build_krea_horde_workflow(
        positive_prompt="krea positive",
        negative_prompt="krea negative",
        width=832,
        height=1216,
        seed=999,
        steps=28,
        cfg=4.2,
        sampler_name="k_euler_a",
        karras=False,
        scheduler="beta",
    )
    assert krea_wf["4"]["inputs"]["text"] == "krea positive"
    assert krea_wf["5"]["inputs"]["text"] == "krea negative"
    assert krea_wf["6"]["inputs"] == {"width": 832, "height": 1216, "batch_size": 1}
    assert krea_wf["7"]["inputs"]["seed"] == 999
    assert krea_wf["7"]["inputs"]["steps"] == 28
    assert krea_wf["7"]["inputs"]["cfg"] == 4.2
    assert krea_wf["7"]["inputs"]["sampler_name"] == "euler_ancestral"
    assert krea_wf["7"]["inputs"]["scheduler"] == "beta"
    assert krea_wf["9"]["class_type"] == "PreviewImage"


def test_point2_reject_unsupported_samplers_schedulers_and_features():
    with pytest.raises(ValueError, match="Unsupported sampler_name"):
        horde_dreamer._resolve_comfy_sampler("plms")
    with pytest.raises(ValueError, match="Unsupported sampler_name"):
        horde_dreamer._resolve_comfy_sampler("nonexistent_sampler")
    with pytest.raises(ValueError, match="Unsupported scheduler"):
        horde_dreamer._resolve_comfy_scheduler("euler", "nonexistent_schedule", karras=True)

    for bad_feature in (
        {"loras": [{"name": "some_lora"}]},
        {"control_type": "canny"},
        {"source_image": "base64..."},
        {"post_processing": ["GFPGAN"]},
        {"tiling": True},
        {"hires_fix": True},
    ):
        payload = {"width": 512, "height": 512, "steps": 20, "cfg_scale": 7.0, **bad_feature}
        with pytest.raises(ValueError):
            horde_dreamer._validate_supported_job_payload(payload, max_pixels=1024 * 1024)


# ==============================================================================
# 3. Preserve requested dimensions and seeds
# ==============================================================================

def test_point3_seed_zero_and_uint64_range_preserved():
    assert horde_dreamer._normalize_horde_seed(0) == 0
    assert horde_dreamer._normalize_horde_seed("0") == 0
    assert horde_dreamer._normalize_horde_seed(1) == 1
    assert horde_dreamer._normalize_horde_seed(2**31) == 2**31
    assert horde_dreamer._normalize_horde_seed(2**32 - 1) == 2**32 - 1
    assert horde_dreamer._normalize_horde_seed(str(2**53 + 17)) == 2**53 + 17
    assert horde_dreamer._normalize_horde_seed(0xFFFFFFFFFFFFFFFF) == 0xFFFFFFFFFFFFFFFF

    for invalid_seed in (-1, "-5", 0xFFFFFFFFFFFFFFFF + 1, "not-a-seed", True):
        with pytest.raises(ValueError):
            horde_dreamer._normalize_horde_seed(invalid_seed)


def test_point3_dimensions_preserved_and_invalid_rejected():
    # Boundary and extreme portrait/landscape resolutions
    assert horde_dreamer._validate_horde_dimensions(64, 64) == (64, 64)
    assert horde_dreamer._validate_horde_dimensions(64, 8192) == (64, 8192)
    assert horde_dreamer._validate_horde_dimensions(8192, 64) == (8192, 64)
    assert horde_dreamer._validate_horde_dimensions(512, 2048, max_pixels=1048576) == (512, 2048)
    assert horde_dreamer._validate_horde_dimensions(2048, 512, max_pixels=1048576) == (2048, 512)

    # Out-of-bounds, non-multiple of 64, or exceeding max_pixels must raise ValueError (never clamp/resize)
    for bad_w, bad_h, max_px in (
        (0, 512, None),
        (32, 512, None),
        (500, 512, None),
        (512, 550, None),
        (8256, 512, None),
        (1024, 1088, 1024 * 1024),
    ):
        with pytest.raises(ValueError):
            horde_dreamer._validate_horde_dimensions(bad_w, bad_h, max_pixels=max_px)


# ==============================================================================
# 4. Require server acknowledgement before recording completion
# ==============================================================================

@pytest.mark.asyncio
@pytest.mark.parametrize("submit_mode", ["http_400", "http_500", "conn_drop", "retry_then_200", "ambiguous_duplicate"])
async def test_point4_server_acknowledgement_and_retry(monkeypatch, _isolated_dreamer_env, submit_mode):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())
        monkeypatch.setattr(
            horde_dreamer,
            "_evaluate_image_safety",
            lambda png, prompt, censor, sfw, info: {
                "final_png_bytes": png,
                "submit_state": "ok",
                "censored": False,
                "is_csam": False,
                "is_nsfw": False,
                "gen_metadata": [],
            },
        )
        monkeypatch.setattr(horde_dreamer, "_run_local_comfy_silent", AsyncMock(return_value=_make_png_bytes()))

        submit_calls = 0
        app = web.Application()

        async def handle_pop(request):
            return web.json_response({
                "ids": [f"job-{submit_mode}"],
                "model": "CyberRealistic Pony",
                "payload": {
                    "prompt": "a scenic mountain lake",
                    "width": 512,
                    "height": 512,
                    "steps": 20,
                    "cfg_scale": 7.0,
                    "sampler_name": "k_euler",
                    "seed": "0",
                },
            })

        async def handle_submit(request):
            nonlocal submit_calls
            submit_calls += 1
            if submit_mode == "http_400":
                return web.Response(status=400, text='{"message": "Invalid generation payload"}')
            if submit_mode == "http_500":
                return web.Response(status=500, text="Internal Server Error")
            if submit_mode == "conn_drop":
                request.transport.close()
                return web.Response(status=500)
            if submit_mode == "retry_then_200":
                if submit_calls == 1:
                    return web.Response(status=502, text="Bad Gateway")
                return web.json_response({"reward": 19.5})
            if submit_mode == "ambiguous_duplicate":
                if submit_calls == 1:
                    return web.Response(status=504, text="Gateway Timeout")
                return web.Response(status=400, text='{"message": "Generation already submitted"}')
            return web.json_response({"reward": 10.0})

        app.router.add_post("/generate/pop", handle_pop)
        app.router.add_post("/generate/submit", handle_submit)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")

        # Speed up retry delay for test
        orig_submit_fn = horde_dreamer._submit_generation_with_retry
        async def fast_submit(session, body, headers, **kw):
            return await orig_submit_fn(session, body, headers, max_attempts=2, retry_delay=0.02)
        monkeypatch.setattr(horde_dreamer, "_submit_generation_with_retry", fast_submit)

        try:
            res = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
            saved = horde_dreamer.load_dreamer_data()
            if submit_mode in ("http_400", "http_500", "conn_drop"):
                assert res["status"] == "submit_failed"
                assert saved["total_jobs_completed"] == 0
                assert saved["sdxl_jobs_completed"] == 0
                assert saved["total_kudos_earned"] == 0.0
                assert saved["total_jobs_submit_failed"] == 1
                assert horde_dreamer._runtime_status["state"] == "error"
                assert horde_dreamer._runtime_status["last_error"] != ""
            else:
                assert res["status"] == "ok"
                assert saved["total_jobs_completed"] == 1
                assert saved["sdxl_jobs_completed"] == 1
                assert saved["total_jobs_submit_failed"] == 0
                assert horde_dreamer._runtime_status["last_error"] == ""
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# 5. Give every popped job a terminal handling path
# ==============================================================================

@pytest.mark.asyncio
@pytest.mark.parametrize("failure_stage", ["workflow_build", "render_exc", "safety_exc", "webp_exc", "cancelled_render"])
async def test_point5_every_popped_job_has_terminal_fault_path(monkeypatch, _isolated_dreamer_env, failure_stage):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())

        if failure_stage == "workflow_build":
            monkeypatch.setattr(
                horde_dreamer,
                "_build_sdxl_horde_workflow",
                lambda *a, **kw: (_ for _ in ()).throw(RuntimeError("Workflow graph failure")),
            )
        elif failure_stage == "render_exc":
            monkeypatch.setattr(
                horde_dreamer,
                "_run_local_comfy_silent",
                AsyncMock(side_effect=RuntimeError("Unexpected ComfyUI crash")),
            )
        elif failure_stage == "safety_exc":
            monkeypatch.setattr(horde_dreamer, "_run_local_comfy_silent", AsyncMock(return_value=_make_png_bytes()))
            monkeypatch.setattr(
                horde_dreamer,
                "_evaluate_image_safety",
                lambda *a, **kw: (_ for _ in ()).throw(RuntimeError("CLIP model error")),
            )
        elif failure_stage == "webp_exc":
            monkeypatch.setattr(horde_dreamer, "_run_local_comfy_silent", AsyncMock(return_value=_make_png_bytes()))
            monkeypatch.setattr(
                horde_dreamer,
                "_evaluate_image_safety",
                lambda png, *a, **kw: {
                    "final_png_bytes": png,
                    "submit_state": "ok",
                    "censored": False,
                    "is_csam": False,
                    "is_nsfw": False,
                    "gen_metadata": [],
                },
            )
            monkeypatch.setattr(
                horde_dreamer,
                "_convert_png_to_webp",
                lambda *a, **kw: (_ for _ in ()).throw(OSError("WebP encoder failure")),
            )
        elif failure_stage == "cancelled_render":
            monkeypatch.setattr(
                horde_dreamer,
                "_run_local_comfy_silent",
                AsyncMock(side_effect=asyncio.CancelledError()),
            )

        submitted_payloads: list[dict[str, Any]] = []
        app = web.Application()

        async def handle_pop(request):
            return web.json_response({
                "ids": [f"job-{failure_stage}"],
                "model": "CyberRealistic Pony",
                "payload": {
                    "prompt": "terminal path test",
                    "width": 512,
                    "height": 512,
                    "steps": 20,
                    "cfg_scale": 7.0,
                    "sampler_name": "k_euler",
                    "seed": "77",
                },
            })

        async def handle_submit(request):
            submitted_payloads.append(await request.json())
            return web.json_response({"reward": 0.0})

        app.router.add_post("/generate/pop", handle_pop)
        app.router.add_post("/generate/submit", handle_submit)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")

        try:
            if failure_stage == "cancelled_render":
                with pytest.raises(asyncio.CancelledError):
                    await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
            else:
                res = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
                assert res["status"] == "faulted"

            assert len(submitted_payloads) == 1
            assert submitted_payloads[0]["id"] == f"job-{failure_stage}"
            assert submitted_payloads[0]["state"] == "faulted"
            saved = horde_dreamer.load_dreamer_data()
            assert saved["total_jobs_faulted"] == 1
            assert saved["total_jobs_completed"] == 0
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# 6. Enforce an actual rendering deadline on a silent WebSocket
# ==============================================================================

@pytest.mark.asyncio
async def test_point6_silent_websocket_enforces_deadline_and_interrupts_comfy(monkeypatch, _isolated_dreamer_env):
    queue_deletes: list[Any] = []
    interrupts: list[Any] = []

    app = web.Application()

    async def handle_ws(request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        # Connected but completely silent forever
        try:
            await asyncio.sleep(30.0)
        except asyncio.CancelledError:
            pass
        return ws

    async def handle_prompt(request):
        return web.json_response({"prompt_id": "silent-prompt-999"})

    async def handle_queue(request):
        queue_deletes.append(await request.json())
        return web.json_response({"ok": True})

    async def handle_interrupt(request):
        interrupts.append(await request.json())
        return web.json_response({"ok": True})

    async def handle_history(request):
        return web.json_response({"ok": True})

    app.router.add_get("/ws", handle_ws)
    app.router.add_post("/prompt", handle_prompt)
    app.router.add_post("/queue", handle_queue)
    app.router.add_post("/interrupt", handle_interrupt)
    app.router.add_post("/history", handle_history)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_API_URL", f"http://127.0.0.1:{port}")

    try:
        t0 = asyncio.get_event_loop().time()
        res = await horde_dreamer._run_local_comfy_silent({"1": {}}, timeout=0.35)
        elapsed = asyncio.get_event_loop().time() - t0

        assert res is None
        assert elapsed < 1.5, f"Silent socket must terminate near the 0.35s deadline, took {elapsed:.2f}s"
        assert queue_deletes == [{"delete": ["silent-prompt-999"]}]
        assert interrupts == [{"prompt_id": "silent-prompt-999"}]
    finally:
        await runner.cleanup()


# ==============================================================================
# 7. Handle original renderer artifacts after safety evaluation
# ==============================================================================

@pytest.mark.asyncio
async def test_point7_safety_replacement_and_artifact_removal(monkeypatch, _isolated_dreamer_env):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())

        # Create a simulated ComfyUI preview artifact in the configured temp_dir
        flagged_preview_file = env["temp_dir"] / "ComfyUI_temp_flagged_00001_.png"
        original_secret_png = _make_png_bytes(color=(255, 0, 0))
        flagged_preview_file.write_bytes(original_secret_png)

        async def fake_comfy_render(wf, timeout=180.0, tracked_artifacts=None):
            if tracked_artifacts is not None:
                tracked_artifacts.append(str(flagged_preview_file))
            return original_secret_png

        monkeypatch.setattr(horde_dreamer, "_run_local_comfy_silent", fake_comfy_render)

        censor_png = horde_dreamer._get_censor_png_bytes("csam")
        monkeypatch.setattr(
            horde_dreamer,
            "_evaluate_image_safety",
            lambda png, prompt, censor, sfw, info: {
                "final_png_bytes": censor_png,
                "submit_state": "csam",
                "censored": True,
                "is_csam": True,
                "is_nsfw": True,
                "gen_metadata": [{"type": "censorship", "value": "csam"}],
            },
        )

        submitted_bodies: list[dict[str, Any]] = []
        app = web.Application()

        async def handle_pop(request):
            return web.json_response({
                "ids": ["job-csam-artifact"],
                "model": "CyberRealistic Pony",
                "payload": {
                    "prompt": "flagged prompt",
                    "width": 512,
                    "height": 512,
                    "steps": 20,
                    "cfg_scale": 7.0,
                    "sampler_name": "k_euler",
                    "seed": "101",
                },
            })

        async def handle_submit(request):
            submitted_bodies.append(await request.json())
            return web.json_response({"reward": 5.0})

        app.router.add_post("/generate/pop", handle_pop)
        app.router.add_post("/generate/submit", handle_submit)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")

        try:
            res = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
            assert res["status"] == "ok"
            # 1. Original local artifact must be deleted from disk
            assert not flagged_preview_file.exists(), "Flagged ComfyUI preview file must be deleted from disk"
            # 2. Submitted image must be the CSAM censor placeholder, not the original red image
            assert len(submitted_bodies) == 1
            sub = submitted_bodies[0]
            assert sub["state"] == "csam"
            assert sub["censored"] is True
            decoded_webp = base64.b64decode(sub["generation"])
            expected_censor_webp = horde_dreamer._convert_png_to_webp(censor_png, quality=92)
            assert decoded_webp == expected_censor_webp
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# 8. Cancel client requests when abandoning them
# ==============================================================================

@pytest.mark.asyncio
@pytest.mark.parametrize("scenario", ["is_possible_false", "timeout", "cancelled", "success"])
async def test_point8_image_client_cancels_only_abandoned_requests(monkeypatch, scenario):
    deleted_ids: list[str] = []
    app = web.Application()

    async def handle_async(request):
        return web.json_response({"id": f"img-task-{scenario}"}, status=202)

    async def handle_check(request):
        if scenario == "is_possible_false":
            return web.json_response({"done": False, "faulted": False, "is_possible": False})
        if scenario in ("timeout", "cancelled"):
            return web.json_response({"done": False, "faulted": False, "is_possible": True})
        return web.json_response({"done": True, "faulted": False, "is_possible": True})

    async def handle_status(request):
        webp_bytes = horde_dreamer._convert_png_to_webp(_make_png_bytes())
        return web.json_response({
            "done": True,
            "generations": [{
                "img": base64.b64encode(webp_bytes).decode("ascii"),
                "worker_name": "TestWorker",
                "model": "CyberRealistic Pony",
            }],
        })

    async def handle_delete(request):
        deleted_ids.append(request.match_info["task_id"])
        return web.json_response({"ok": True})

    app.router.add_post("/generate/async", handle_async)
    app.router.add_get("/generate/check/{task_id}", handle_check)
    app.router.add_get("/generate/status/{task_id}", handle_status)
    app.router.add_delete("/generate/status/{task_id}", handle_delete)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")

    try:
        if scenario == "cancelled":
            task = asyncio.create_task(
                horde_dreamer.horde_generate_image("test", max_wait_seconds=10, poll_interval=0.05)
            )
            await asyncio.sleep(0.12)
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task
            assert deleted_ids == [f"img-task-{scenario}"]
        elif scenario in ("is_possible_false", "timeout"):
            png, worker, model = await horde_dreamer.horde_generate_image(
                "test", max_wait_seconds=0.15, poll_interval=0.05
            )
            assert png is None
            assert deleted_ids == [f"img-task-{scenario}"]
        else:
            png, worker, model = await horde_dreamer.horde_generate_image(
                "test", max_wait_seconds=2, poll_interval=0.05
            )
            assert png is not None
            assert worker == "TestWorker"
            assert deleted_ids == [], "Successfully consumed image request must NOT be cancelled"
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
@pytest.mark.parametrize("scenario", ["is_possible_false", "timeout", "stream_closed_early", "success"])
async def test_point8_text_client_cancels_only_abandoned_requests(monkeypatch, scenario):
    deleted_ids: list[str] = []
    app = web.Application()

    async def handle_text_async(request):
        return web.json_response({"id": f"txt-task-{scenario}"}, status=202)

    async def handle_text_status(request):
        if scenario == "is_possible_false":
            return web.json_response({"done": False, "faulted": False, "is_possible": False})
        if scenario in ("timeout", "stream_closed_early"):
            return web.json_response({"done": False, "faulted": False, "is_possible": True, "queue_position": 2, "wait_time": 5})
        return web.json_response({
            "done": True,
            "faulted": False,
            "is_possible": True,
            "generations": [{"text": "Hello from AI Horde worker", "worker_name": "TextNode", "model": "koboldcpp/test"}],
        })

    async def handle_text_delete(request):
        deleted_ids.append(request.match_info["task_id"])
        return web.json_response({"ok": True})

    app.router.add_post("/generate/text/async", handle_text_async)
    app.router.add_get("/generate/text/status/{task_id}", handle_text_status)
    app.router.add_delete("/generate/text/status/{task_id}", handle_text_delete)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    monkeypatch.setattr(horde_client, "HORDE_BASE_URL", f"http://127.0.0.1:{port}")

    try:
        gen = horde_client.horde_generate_stream(
            {"prompt": "Hi"},
            check_interval=0.05,
            max_wait_seconds=0.18 if scenario != "success" else 2.0,
        )
        events = []
        if scenario == "stream_closed_early":
            first_ev = await gen.__anext__()
            events.append(first_ev)
            await gen.aclose()
            assert deleted_ids == [f"txt-task-{scenario}"]
        else:
            async for ev in gen:
                events.append(ev)
            if scenario in ("is_possible_false", "timeout"):
                assert any("error" in e for e in events)
                assert deleted_ids == [f"txt-task-{scenario}"]
            else:
                assert any(e.get("done") is True for e in events)
                assert deleted_ids == [], "Successfully consumed text request must NOT be cancelled"
    finally:
        await runner.cleanup()
