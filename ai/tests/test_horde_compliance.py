"""
Comprehensive offline regression test suite for AI Horde Worker & Client compliance
(`horde_dreamer.py` & `horde_client.py`), mapped 1-to-1 to Sections 1–9 of the
AI Horde maintainer remediation review (`direct-remediation-request.md`):

1. Zero default retention of users' generated images, metadata, or prompt-bearing artifacts.
2. Faithful execution of accepted generation semantics (seeds, denoise, dynamic prompts,
   dimensions, samplers/schedulers, and connected-renderer checkpoint registration).
3. Separation of rendering, R2 upload, submission, acknowledgement, and reconciliation.
4. Owned, bounded job lifecycle (`ttl` assignment lease, silent-socket deadline, ComfyUI
   dispatch reservation, and acknowledged fault handling).
5. Validation of authoritative renderer completion, pixel dimensions, and model/payload identity.
6. Hardened management authorization boundary (`_check_owner` & `/horde/api/*`).
7. Preservation of operator policy across concurrent maintenance updates, empty model selections,
   and fail-closed state recovery.
8. Explicit exclusion of `Krea2-Turbo_fp8`.
9. Client result handling (`done=True` precedence over `is_possible=False`, cancellation cleanup,
   and end-to-end Creative Studio `core_service.generate_image` routing).
"""

import asyncio
import base64
import hashlib
import io
import json
import logging
import os
import struct
import sys
import types
from typing import Any
from unittest.mock import AsyncMock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import aiohttp
from aiohttp import web
from fastapi import FastAPI
from fastapi.testclient import TestClient
from PIL import Image, PngImagePlugin
import pytest

import horde_client
import horde_dreamer


def _make_png_bytes(
    color: tuple[int, int, int] = (32, 64, 128),
    size: tuple[int, int] = (512, 512),
    embedded_prompt: str = "",
) -> bytes:
    buf = io.BytesIO()
    im = Image.new("RGB", size, color=color)
    pnginfo = None
    if embedded_prompt:
        pnginfo = PngImagePlugin.PngInfo()
        pnginfo.add_text("prompt", embedded_prompt)
        pnginfo.add_text("workflow", json.dumps({"secret": embedded_prompt}))
    im.save(buf, format="PNG", pnginfo=pnginfo)
    return buf.getvalue()


@pytest.fixture(autouse=True)
def _isolated_dreamer_env(tmp_path, monkeypatch):
    """Isolates `DATA_PATH`, ComfyUI directories, renderer URLs, and in-memory state per test."""
    data_file = tmp_path / "horde_dreamer_data.json"
    ckpts_dir = tmp_path / "models" / "checkpoints"
    temp_dir = tmp_path / "temp"
    output_dir = tmp_path / "output"
    ckpts_dir.mkdir(parents=True, exist_ok=True)
    temp_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    monkeypatch.setattr(horde_dreamer, "DATA_PATH", str(data_file))
    monkeypatch.setattr(horde_dreamer, "_last_known_good_data", None)
    monkeypatch.setattr(horde_dreamer, "_last_known_good_path", None)
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_CHECKPOINTS_DIR", str(ckpts_dir))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_TEMP_DIR", str(temp_dir))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_OUTPUT_DIR", str(output_dir))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_DIR", str(tmp_path))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_API_URL", "http://127.0.0.1:9")
    monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_WORKER_API_KEY", "test-worker-key")
    monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_KREA_REFERENCE_VERIFIED", "false")
    monkeypatch.setitem(horde_dreamer.SETTINGS, "MODAL_COMFY_URL", "")
    monkeypatch.setattr(horde_dreamer, "verify_comfy_checkpoint_registered", AsyncMock(return_value=True))
    horde_dreamer._checkpoint_sha256_cache.clear()
    horde_dreamer._pending_deliveries.clear()
    yield {
        "tmp_path": tmp_path,
        "ckpts_dir": ckpts_dir,
        "temp_dir": temp_dir,
        "output_dir": output_dir,
        "data_file": data_file,
    }


def _install_fake_verified_model(
    env,
    model_name: str = "CyberRealistic Pony",
    content: bytes = b"reference-weights-v8",
) -> dict[str, Any]:
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
# Section 1: Remove default retention of users' results & prompt-bearing traces
# ==============================================================================

@pytest.mark.asyncio
async def test_sec1_save_image_websocket_in_memory_and_history_deleted(monkeypatch, _isolated_dreamer_env):
    env = _isolated_dreamer_env
    unrelated_user_file = env["temp_dir"] / "ComfyUI_interactive_user_0001_.png"
    unrelated_user_file.write_bytes(b"owner-interactive-preview")

    wf = horde_dreamer._build_sdxl_horde_workflow(
        positive_prompt="harmless landscape",
        negative_prompt="",
        ckpt_name="cyberrealisticPony_v8.safetensors",
        width=512,
        height=512,
        seed=42,
    )
    assert wf["8"]["class_type"] == "SaveImageWebsocket"
    assert "PreviewImage" not in {n["class_type"] for n in wf.values()}
    assert "SaveImage" not in {n["class_type"] for n in wf.values()}

    history_deletes: list[Any] = []
    png_payload = _make_png_bytes(size=(512, 512), embedded_prompt="SUPER_SECRET_USER_PROMPT_991")

    app = web.Application()

    async def handle_ws(request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        await asyncio.sleep(0.03)
        await ws.send_json({"type": "executing", "data": {"prompt_id": "ws-prompt-1", "node": "8"}})
        # ComfyUI SaveImageWebsocket binary frame: >II (event_type=1, format=2 PNG) + raw PNG bytes
        await ws.send_bytes(struct.pack(">II", 1, 2) + png_payload)
        await ws.send_json({"type": "executing", "data": {"prompt_id": "ws-prompt-1", "node": None}})
        await ws.close()
        return ws

    async def handle_prompt(request):
        body = await request.json()
        assert body["extra_data"]["preview_method"] == "none"
        return web.json_response({"prompt_id": "ws-prompt-1"})

    async def handle_history(request):
        history_deletes.append(await request.json())
        return web.json_response({"ok": True})

    app.router.add_get("/ws", handle_ws)
    app.router.add_post("/prompt", handle_prompt)
    app.router.add_post("/history", handle_history)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_API_URL", f"http://127.0.0.1:{port}")

    try:
        out_bytes = await horde_dreamer._run_local_comfy_silent(wf, timeout=5.0)
        assert out_bytes == png_payload
        assert history_deletes == [{"delete": ["ws-prompt-1"]}]
        # Unrelated interactive user's temp file must remain untouched
        assert unrelated_user_file.exists()
        assert list(env["output_dir"].iterdir()) == []

        # Outgoing WebP conversion must strip embedded PNG prompt/workflow metadata and EXIF
        webp_out = horde_dreamer._convert_png_to_webp(out_bytes)
        assert b"SUPER_SECRET_USER_PROMPT_991" not in webp_out
        with Image.open(io.BytesIO(webp_out)) as wim:
            assert not wim.info.get("exif")
            assert "prompt" not in wim.info
            assert "workflow" not in wim.info
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "outcome_case",
    [
        "success",
        "censored_nsfw",
        "csam_verdict",
        "safety_error",
        "webp_error",
        "r2_failure_fallback",
        "cancelled",
        "renderer_failure",
    ],
)
async def test_sec1_privacy_and_artifact_lifecycle_across_all_outcomes(
    monkeypatch, caplog, _isolated_dreamer_env, outcome_case
):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    secret_prompt = "PRIVATE_USER_PROMPT_DO_NOT_LOG_OR_PERSIST_7731"
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())

        # Unrelated interactive ComfyUI file owned by local user — must NEVER be deleted by Horde worker
        interactive_file = env["temp_dir"] / "ComfyUI_owner_session_0001_.png"
        interactive_file.write_bytes(b"interactive-owner-image")

        # Worker-owned fallback artifact — MUST be deleted on every single path
        worker_artifact = env["temp_dir"] / f"ComfyUI_worker_{outcome_case}_0001_.png"
        harmless_png = _make_png_bytes(color=(20, 120, 220), size=(512, 512), embedded_prompt=secret_prompt)
        worker_artifact.write_bytes(harmless_png)

        async def fake_render(wf, timeout=180.0, tracked_artifacts=None):
            if tracked_artifacts is not None:
                tracked_artifacts.append(str(worker_artifact))
            if outcome_case == "renderer_failure":
                return None
            if outcome_case == "cancelled":
                raise asyncio.CancelledError()
            return harmless_png

        monkeypatch.setattr(horde_dreamer, "_run_local_comfy_silent", fake_render)

        censor_nsfw_png = horde_dreamer._get_censor_png_bytes("nsfw")
        censor_csam_png = horde_dreamer._get_censor_png_bytes("csam")

        def fake_safety(png, prompt, censor_nsfw, sfw_worker, info):
            if outcome_case == "safety_error":
                raise RuntimeError("Simulated classifier failure")
            if outcome_case == "censored_nsfw":
                return {
                    "final_png_bytes": censor_nsfw_png,
                    "submit_state": "censored",
                    "censored": True,
                    "is_csam": False,
                    "is_nsfw": True,
                    "gen_metadata": [{"type": "censorship", "value": "nsfw"}],
                }
            if outcome_case == "csam_verdict":
                return {
                    "final_png_bytes": censor_csam_png,
                    "submit_state": "csam",
                    "censored": True,
                    "is_csam": True,
                    "is_nsfw": True,
                    "gen_metadata": [{"type": "censorship", "value": "csam"}],
                }
            return {
                "final_png_bytes": png,
                "submit_state": "ok",
                "censored": False,
                "is_csam": False,
                "is_nsfw": False,
                "gen_metadata": [],
            }

        monkeypatch.setattr(horde_dreamer, "_evaluate_image_safety", fake_safety)
        if outcome_case == "webp_error":
            monkeypatch.setattr(
                horde_dreamer,
                "_convert_png_to_webp",
                lambda *a, **kw: (_ for _ in ()).throw(OSError("Simulated WebP encode error")),
            )

        submitted_bodies: list[dict[str, Any]] = []
        app = web.Application()

        async def handle_pop(request):
            port_now = request.host.split(":")[-1]
            r2_field = f"http://127.0.0.1:{port_now}/r2_upload" if outcome_case == "r2_failure_fallback" else ""
            return web.json_response({
                "ids": [f"job-sec1-{outcome_case}"],
                "model": "CyberRealistic Pony",
                "r2_uploads": [r2_field] if r2_field else [],
                "payload": {
                    "prompt": f"{secret_prompt} ### lowres",
                    "width": 512,
                    "height": 512,
                    "steps": 20,
                    "cfg_scale": 7.0,
                    "sampler_name": "k_euler",
                    "seed": "101",
                    "use_nsfw_censor": outcome_case == "censored_nsfw",
                },
            })

        async def handle_r2(request):
            return web.Response(status=503, text="R2 Unavailable")

        async def handle_submit(request):
            submitted_bodies.append(await request.json())
            return web.json_response({"reward": 8.0})

        app.router.add_post("/generate/pop", handle_pop)
        app.router.add_put("/r2_upload", handle_r2)
        app.router.add_post("/generate/submit", handle_submit)

        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")

        caplog.set_level(logging.INFO)
        try:
            if outcome_case == "cancelled":
                with pytest.raises(asyncio.CancelledError):
                    await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
            else:
                res = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
                if outcome_case in ("success", "censored_nsfw", "csam_verdict", "r2_failure_fallback"):
                    assert res["status"] == "ok"
                else:
                    assert res["status"] == "faulted"

            # 1. Worker-owned artifact must be removed; unrelated interactive user's file must remain untouched
            assert not worker_artifact.exists(), f"Worker artifact retained on {outcome_case}"
            assert interactive_file.exists(), "Unrelated user file in temp_dir must remain untouched"
            assert horde_dreamer._pending_deliveries == {}

            # 2. Verify outgoing payload/metadata on delivery vs fault
            assert len(submitted_bodies) == 1
            sub = submitted_bodies[0]
            if outcome_case == "censored_nsfw":
                assert sub["state"] == "censored"
                assert sub["censored"] is True
                assert base64.b64decode(sub["generation"]) == horde_dreamer._convert_png_to_webp(censor_nsfw_png)
            elif outcome_case == "csam_verdict":
                assert sub["state"] == "csam"
                assert sub["censored"] is True
                assert base64.b64decode(sub["generation"]) == horde_dreamer._convert_png_to_webp(censor_csam_png)
            elif outcome_case in ("success", "r2_failure_fallback"):
                assert sub["state"] == "ok"
                assert sub["censored"] is False
                raw_webp = base64.b64decode(sub["generation"])
                assert secret_prompt.encode("utf-8") not in raw_webp
            else:
                assert sub["state"] == "faulted"

            # 3. Verify zero prompt retention in logs and persisted state
            assert secret_prompt not in caplog.text
            saved_raw = env["data_file"].read_text(encoding="utf-8")
            assert secret_prompt not in saved_raw
            saved_json = json.loads(saved_raw)
            for rj in saved_json.get("recent_jobs", []):
                assert "prompt_preview" not in rj
                assert "prompt" not in rj
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# Section 2: Preserve accepted generation semantics & renderer asset registration
# ==============================================================================

@pytest.mark.asyncio
@pytest.mark.parametrize("seed_val", [0, 1, 2147483647, 2147483648, 4294967295])
async def test_sec2_seeds_preserved_end_to_end_to_graph_and_submit(monkeypatch, _isolated_dreamer_env, seed_val):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())
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

        captured_workflows: list[dict[str, Any]] = []

        async def fake_render(wf, timeout=180.0, tracked_artifacts=None):
            captured_workflows.append(wf)
            return _make_png_bytes(size=(512, 512))

        monkeypatch.setattr(horde_dreamer, "_run_local_comfy_silent", fake_render)

        submitted: list[dict[str, Any]] = []
        app = web.Application()

        async def handle_pop(request):
            return web.json_response({
                "ids": [f"job-seed-{seed_val}"],
                "model": "CyberRealistic Pony",
                "payload": {
                    "prompt": "a {red|blue} bird ### blurry",
                    "width": 512,
                    "height": 512,
                    "steps": 20,
                    "cfg_scale": 7.0,
                    "denoising_strength": 0.5,
                    "sampler_name": "k_euler",
                    "seed": str(seed_val),
                },
            })

        async def handle_submit(request):
            submitted.append(await request.json())
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
            assert len(captured_workflows) == 1
            assert captured_workflows[0]["6"]["inputs"]["seed"] == seed_val
            assert captured_workflows[0]["6"]["inputs"]["denoise"] == 0.5
            assert len(submitted) == 1
            assert submitted[0]["seed"] == seed_val
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


def test_sec2_missing_and_malformed_seeds_rejected_on_worker_jobs():
    assert horde_dreamer._normalize_horde_seed(0, allow_random=False) == 0
    assert horde_dreamer._normalize_horde_seed("0", allow_random=False) == 0
    assert horde_dreamer._normalize_horde_seed(0xFFFFFFFFFFFFFFFF, allow_random=False) == 0xFFFFFFFFFFFFFFFF

    for bad_seed in (None, "", "   ", -1, "-5", 0xFFFFFFFFFFFFFFFF + 1, "not-a-seed", True, False):
        with pytest.raises(ValueError):
            horde_dreamer._normalize_horde_seed(bad_seed, allow_random=False)


def test_sec2_denoise_and_dynamic_prompts_and_workflow_bindings():
    # 1. Dynamic prompt canonical seeded expansion (`seed=123`: `a {red|blue} bird` -> `a red bird`)
    assert horde_dreamer._expand_dynamic_prompt("a {red|blue} bird", 123) == "a red bird"
    nested = horde_dreamer._expand_dynamic_prompt("a {warm|{cool|icy}} landscape", 123)
    assert nested in ("a warm landscape", "a cool landscape", "a icy landscape")
    assert "{" not in nested and "}" not in nested
    assert horde_dreamer._expand_dynamic_prompt("ordinary prompt text", 123) == "ordinary prompt text"

    # 2. Prompt splitting + negative prompt preserved literally without dynamic-prompt mutation or extra words
    pos, neg = horde_dreamer._split_horde_prompt("a {red|blue} bird ### {keep_literal} negative")
    wf_default_denoise = horde_dreamer._build_sdxl_horde_workflow(
        positive_prompt=pos,
        negative_prompt=neg,
        ckpt_name="cyberrealisticPony_v8.safetensors",
        width=768,
        height=1024,
        seed=123,
        steps=37,
        cfg=6.5,
        sampler_name="k_dpmpp_2m",
        karras=True,
        scheduler="sgm_uniform",
        clip_skip=2,
    )
    assert wf_default_denoise["3"]["inputs"]["text"] == "a red bird"
    assert wf_default_denoise["4"]["inputs"]["text"] == "{keep_literal} negative"
    assert wf_default_denoise["5"]["inputs"] == {"width": 768, "height": 1024, "batch_size": 1}
    assert wf_default_denoise["6"]["inputs"]["denoise"] == 1.0
    assert wf_default_denoise["6"]["inputs"]["steps"] == 37
    assert wf_default_denoise["6"]["inputs"]["cfg"] == 6.5
    assert wf_default_denoise["6"]["inputs"]["sampler_name"] == "dpmpp_2m"
    assert wf_default_denoise["6"]["inputs"]["scheduler"] == "sgm_uniform"
    assert wf_default_denoise["2"]["inputs"]["stop_at_clip_layer"] == -2
    assert "LoraLoader" not in {n["class_type"] for n in wf_default_denoise.values()}
    assert "ImageSharpen" not in {n["class_type"] for n in wf_default_denoise.values()}

    # Explicit denoise 0.5 and 1.0 in payload validation & graph
    v_omitted = horde_dreamer._validate_supported_job_payload({"width": 512, "height": 512, "seed": "0"})
    assert v_omitted["denoise"] == 1.0
    v_half = horde_dreamer._validate_supported_job_payload(
        {"width": 512, "height": 512, "seed": "1", "denoising_strength": 0.5}
    )
    assert v_half["denoise"] == 0.5
    wf_half = horde_dreamer._build_sdxl_horde_workflow(
        positive_prompt="test",
        negative_prompt="",
        ckpt_name="cyberrealisticPony_v8.safetensors",
        width=512,
        height=512,
        seed=1,
        denoise=v_half["denoise"],
    )
    assert wf_half["6"]["inputs"]["denoise"] == 0.5

    # `dpmpp_3m_sde` under karras=False normalizes scheduler to `karras` matching hordelib
    assert horde_dreamer._resolve_comfy_scheduler("dpmpp_3m_sde", None, karras=False) == "karras"

    # Unsupported samplers, schedulers, denoise bounds, dimensions, and modes rejected
    with pytest.raises(ValueError, match="Unsupported sampler_name"):
        horde_dreamer._resolve_comfy_sampler("plms")
    with pytest.raises(ValueError, match="Unsupported scheduler"):
        horde_dreamer._resolve_comfy_scheduler("euler", "bad_sched", karras=True)
    for bad_payload in (
        {"denoising_strength": 0.0},
        {"denoising_strength": 1.5},
        {"loras": [{"name": "x"}]},
        {"tis": [{"name": "ti"}]},
        {"control_type": "canny"},
        {"source_image": "b64"},
        {"post_processing": ["GFPGAN"]},
        {"tiling": True},
        {"hires_fix": True},
        {"transparent": True},
        {"workflow": "qr_code"},
        {"n": 2},
        {"width": 500, "height": 512},
        {"width": 1024, "height": 1088},
    ):
        base = {"width": 512, "height": 512, "steps": 20, "cfg_scale": 7.0, "seed": "42", **bad_payload}
        with pytest.raises(ValueError):
            horde_dreamer._validate_supported_job_payload(base, max_pixels=1024 * 1024)


@pytest.mark.asyncio
async def test_sec2_connected_comfy_checkpoint_verification_gates_offer(monkeypatch, _isolated_dreamer_env):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    # Restore real `verify_comfy_checkpoint_registered`
    real_verify = horde_dreamer.verify_comfy_checkpoint_registered
    # Note: fixture replaced it on module, grab original via reload-safe reference from __wrapped__ or reimport
    try:
        spec = _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.undo()  # undo fixture's AsyncMock? No, only restore verify_comfy_checkpoint_registered!
    finally:
        pass
    # Re-apply isolated paths cleanly and test real HTTP `/object_info/CheckpointLoaderSimple`
    monkeypatch.setattr(horde_dreamer, "DATA_PATH", str(env["data_file"]))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_CHECKPOINTS_DIR", str(env["ckpts_dir"]))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_WORKER_API_KEY", "test-worker-key")

    try:
        spec = _install_fake_verified_model(env, "CyberRealistic Pony")
        registered_list: list[str] = ["other_model.safetensors"]
        pop_called = False

        app = web.Application()

        async def handle_queue(request):
            return web.json_response({"queue_running": [], "queue_pending": []})

        async def handle_object_info(request):
            return web.json_response({
                "CheckpointLoaderSimple": {
                    "input": {"required": {"ckpt_name": [registered_list]}}
                }
            })

        async def handle_pop(request):
            nonlocal pop_called
            pop_called = True
            return web.json_response({"ids": []})

        app.router.add_get("/queue", handle_queue)
        app.router.add_get("/object_info/CheckpointLoaderSimple", handle_object_info)
        app.router.add_post("/generate/pop", handle_pop)

        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_API_URL", f"http://127.0.0.1:{port}")
        monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())

        try:
            # 1. Local file hash matches, but connected ComfyUI does NOT list the checkpoint -> excluded BEFORE pop
            assert await horde_dreamer.verify_comfy_checkpoint_registered(spec["ckpt"]) is False
            res_unreg = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
            assert res_unreg["status"] == "no_models"
            assert not pop_called, "Worker must not pop jobs when connected ComfyUI lacks the checkpoint"

            # 2. Connected ComfyUI lists the verified checkpoint -> proceeds to pop cleanly
            registered_list.append(spec["ckpt"])
            assert await horde_dreamer.verify_comfy_checkpoint_registered(spec["ckpt"]) is True
            res_reg = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
            assert res_reg["status"] == "empty_queue"
            assert pop_called
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# Section 3: Separate rendering, uploading, submission, and acknowledgement
# ==============================================================================

@pytest.mark.asyncio
@pytest.mark.parametrize(
    "submit_mode",
    [
        "http_400",
        "auth_401",
        "missing_404",
        "aborted_400",
        "unprompted_duplicate_409",
        "rate_limit_429_then_200",
        "http_500",
        "conn_drop",
        "lost_response_reconciled_next_cycle",
    ],
)
async def test_sec3_delivery_acknowledgement_and_reconciliation(monkeypatch, _isolated_dreamer_env, submit_mode):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())
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
            "_run_local_comfy_silent",
            AsyncMock(return_value=_make_png_bytes(size=(512, 512))),
        )

        r2_put_calls = 0
        submit_calls = 0
        fault_calls = 0
        app = web.Application()

        async def handle_pop(request):
            port_now = request.host.split(":")[-1]
            return web.json_response({
                "ids": [f"job-{submit_mode}"],
                "model": "CyberRealistic Pony",
                "r2_uploads": [f"http://127.0.0.1:{port_now}/r2_put"],
                "payload": {
                    "prompt": "a calm ocean horizon",
                    "width": 512,
                    "height": 512,
                    "steps": 20,
                    "cfg_scale": 7.0,
                    "sampler_name": "k_euler",
                    "seed": "0",
                },
            })

        async def handle_r2_put(request):
            nonlocal r2_put_calls
            r2_put_calls += 1
            await request.read()
            return web.Response(status=200)

        async def handle_submit(request):
            nonlocal submit_calls, fault_calls
            body = await request.json()
            if body.get("state") == "faulted":
                fault_calls += 1
                return web.json_response({"reward": 0.0})
            submit_calls += 1

            if submit_mode == "http_400":
                return web.Response(status=400, text='{"message": "Invalid generation payload"}')
            if submit_mode == "auth_401":
                return web.Response(status=401, text='{"message": "Invalid API key"}')
            if submit_mode == "missing_404":
                return web.Response(status=404, text='{"message": "RequestNotFound"}')
            if submit_mode == "aborted_400":
                return web.Response(status=400, text='{"message": "Request aborted by user"}')
            if submit_mode == "unprompted_duplicate_409":
                return web.Response(status=409, text='{"message": "Generation already submitted"}')
            if submit_mode == "rate_limit_429_then_200":
                if submit_calls == 1:
                    return web.Response(status=429, text="Too Many Requests")
                return web.json_response({"reward": 14.0})
            if submit_mode == "http_500":
                return web.Response(status=500, text="Internal Server Error")
            if submit_mode == "conn_drop":
                request.transport.close()
                return web.Response(status=500)
            if submit_mode == "lost_response_reconciled_next_cycle":
                # Cycle 1 exhausts 2 attempts with 504; Cycle 2 reconciles against backend duplicate message
                if submit_calls <= 2:
                    return web.Response(status=504, text="Gateway Timeout")
                return web.Response(status=400, text='{"message": "Generation already submitted"}')
            return web.json_response({"reward": 10.0})

        app.router.add_post("/generate/pop", handle_pop)
        app.router.add_put("/r2_put", handle_r2_put)
        app.router.add_post("/generate/submit", handle_submit)

        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")

        orig_submit_fn = horde_dreamer._submit_generation_with_retry

        async def fast_submit(session, body, headers, **kw):
            kw["max_attempts"] = 2
            kw["retry_delay"] = 0.02
            return await orig_submit_fn(session, body, headers, **kw)

        monkeypatch.setattr(horde_dreamer, "_submit_generation_with_retry", fast_submit)

        try:
            res = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
            saved = horde_dreamer.load_dreamer_data()
            assert fault_calls == 0, "Completed render awaiting delivery must never send a compensating fault"

            if submit_mode in ("http_400", "auth_401", "missing_404", "aborted_400", "unprompted_duplicate_409"):
                assert res["status"] == "submit_failed"
                assert saved["total_jobs_completed"] == 0
                assert saved["total_jobs_submit_failed"] == 1
                assert horde_dreamer._pending_deliveries == {}
            elif submit_mode in ("http_500", "conn_drop"):
                assert res["status"] == "submit_unresolved"
                assert saved["total_jobs_completed"] == 0
                assert f"job-{submit_mode}" in horde_dreamer._pending_deliveries
                assert r2_put_calls == 1
            elif submit_mode == "rate_limit_429_then_200":
                assert res["status"] == "ok"
                assert saved["total_jobs_completed"] == 1
                assert r2_put_calls == 1, "Submit retry must not re-upload unchanged image to R2"
                assert horde_dreamer._pending_deliveries == {}
            elif submit_mode == "lost_response_reconciled_next_cycle":
                assert res["status"] == "submit_unresolved"
                assert saved["total_jobs_completed"] == 0
                assert f"job-{submit_mode}" in horde_dreamer._pending_deliveries
                assert r2_put_calls == 1

                # Next cycle reconciles pending delivery without popping a new job or re-uploading to R2
                res2 = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=True)
                saved2 = horde_dreamer.load_dreamer_data()
                assert res2["status"] == "ok"
                assert res2["outcome"] == "reconciled"
                assert saved2["total_jobs_completed"] == 1
                assert r2_put_calls == 1, "Reconciliation cycle must reuse r2_uploaded=True without re-uploading"
                assert horde_dreamer._pending_deliveries == {}
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# Section 4: Give each accepted job an owned, bounded lifecycle
# ==============================================================================

@pytest.mark.asyncio
@pytest.mark.parametrize(
    "failure_stage",
    [
        "workflow_build",
        "render_exc",
        "decode_exc",
        "safety_exc",
        "safety_ttl_timeout",
        "webp_exc",
        "cancelled_render",
        "local_busy_during_dispatch",
    ],
)
async def test_sec4_owned_bounded_lifecycle_and_acknowledged_faults(monkeypatch, _isolated_dreamer_env, failure_stage):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())
        monkeypatch.setattr(horde_dreamer, "MIN_JOB_TTL_SECONDS", 0.2)

        idle_calls = 0

        async def dynamic_idle():
            nonlocal idle_calls
            idle_calls += 1
            if failure_stage == "local_busy_during_dispatch" and idle_calls >= 2:
                return False
            return True

        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", dynamic_idle)

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
        elif failure_stage == "decode_exc":
            monkeypatch.setattr(
                horde_dreamer,
                "_run_local_comfy_silent",
                AsyncMock(return_value=b"corrupt-not-a-png"),
            )
        elif failure_stage == "safety_exc":
            monkeypatch.setattr(
                horde_dreamer,
                "_run_local_comfy_silent",
                AsyncMock(return_value=_make_png_bytes(size=(512, 512))),
            )
            monkeypatch.setattr(
                horde_dreamer,
                "_evaluate_image_safety",
                lambda *a, **kw: (_ for _ in ()).throw(RuntimeError("CLIP model error")),
            )
        elif failure_stage == "safety_ttl_timeout":
            monkeypatch.setattr(
                horde_dreamer,
                "_run_local_comfy_silent",
                AsyncMock(return_value=_make_png_bytes(size=(512, 512))),
            )

            def slow_safety(*a, **kw):
                import time as _t
                _t.sleep(0.8)
                return {
                    "final_png_bytes": _make_png_bytes(size=(512, 512)),
                    "submit_state": "ok",
                    "censored": False,
                    "is_csam": False,
                    "is_nsfw": False,
                    "gen_metadata": [],
                }

            monkeypatch.setattr(horde_dreamer, "_evaluate_image_safety", slow_safety)
        elif failure_stage == "webp_exc":
            monkeypatch.setattr(
                horde_dreamer,
                "_run_local_comfy_silent",
                AsyncMock(return_value=_make_png_bytes(size=(512, 512))),
            )
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
        elif failure_stage == "local_busy_during_dispatch":
            render_mock = AsyncMock(return_value=_make_png_bytes(size=(512, 512)))
            monkeypatch.setattr(horde_dreamer, "_run_local_comfy_silent", render_mock)

        submitted_payloads: list[dict[str, Any]] = []
        fault_attempt = 0
        app = web.Application()

        async def handle_pop(request):
            return web.json_response({
                "ids": [f"job-{failure_stage}"],
                "model": "CyberRealistic Pony",
                "ttl": 0.25 if failure_stage == "safety_ttl_timeout" else 60,
                "payload": {
                    "prompt": "lifecycle test",
                    "width": 512,
                    "height": 512,
                    "steps": 20,
                    "cfg_scale": 7.0,
                    "sampler_name": "k_euler",
                    "seed": "77",
                },
            })

        async def handle_submit(request):
            nonlocal fault_attempt
            fault_attempt += 1
            body = await request.json()
            submitted_payloads.append(body)
            # First fault report gets transient 502 to prove `_fault_popped_job` retries until acknowledged
            if fault_attempt == 1:
                return web.Response(status=502, text="Transient Bad Gateway")
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
                assert res["fault_acknowledged"] is True

            assert not horde_dreamer._worker_lock.locked(), "_worker_lock must be released"
            assert not horde_dreamer._comfy_reservation_lock.locked(), "_comfy_reservation_lock must be released"
            assert horde_dreamer._runtime_status["current_job_id"] is None
            assert len(submitted_payloads) == 2
            assert submitted_payloads[-1]["id"] == f"job-{failure_stage}"
            assert submitted_payloads[-1]["state"] == "faulted"
            saved = horde_dreamer.load_dreamer_data()
            assert saved["total_jobs_faulted"] == 1
            assert saved["recent_jobs"][0]["fault_acknowledged"] is True
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


@pytest.mark.asyncio
async def test_sec4_silent_websocket_enforces_deadline_and_cleans_up_comfy(monkeypatch, _isolated_dreamer_env):
    queue_deletes: list[Any] = []
    interrupts: list[Any] = []
    history_deletes: list[Any] = []

    app = web.Application()

    async def handle_ws(request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
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
        history_deletes.append(await request.json())
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
        assert history_deletes == [{"delete": ["silent-prompt-999"]}]
    finally:
        await runner.cleanup()


# ==============================================================================
# Section 5: Validate renderer outcome, pixel dimensions, and model identity
# ==============================================================================

@pytest.mark.asyncio
@pytest.mark.parametrize(
    "ws_sequence",
    ["image_before_error", "error_before_image", "eof_without_completion", "zero_images", "proper_completion"],
)
async def test_sec5_local_comfy_runner_outcome_validation(monkeypatch, _isolated_dreamer_env, ws_sequence):
    img_bytes = _make_png_bytes(size=(512, 512))
    app = web.Application()

    async def handle_ws(request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        await asyncio.sleep(0.02)
        await ws.send_json({"type": "executing", "data": {"prompt_id": "p-sec5", "node": "8"}})
        if ws_sequence == "image_before_error":
            await ws.send_bytes(struct.pack(">II", 1, 2) + img_bytes)
            await ws.send_json({"type": "execution_error", "data": {"prompt_id": "p-sec5"}})
        elif ws_sequence == "error_before_image":
            await ws.send_json({"type": "execution_error", "data": {"prompt_id": "p-sec5"}})
        elif ws_sequence == "eof_without_completion":
            await ws.send_bytes(struct.pack(">II", 1, 2) + img_bytes)
            # Close socket without `executing: node=None`
        elif ws_sequence == "zero_images":
            await ws.send_json({"type": "executing", "data": {"prompt_id": "p-sec5", "node": None}})
        elif ws_sequence == "proper_completion":
            await ws.send_bytes(struct.pack(">II", 1, 2) + img_bytes)
            await ws.send_json({"type": "executing", "data": {"prompt_id": "p-sec5", "node": None}})
        await ws.close()
        return ws

    async def handle_prompt(request):
        return web.json_response({"prompt_id": "p-sec5"})

    async def handle_ok(request):
        return web.json_response({"ok": True})

    app.router.add_get("/ws", handle_ws)
    app.router.add_post("/prompt", handle_prompt)
    app.router.add_post("/queue", handle_ok)
    app.router.add_post("/interrupt", handle_ok)
    app.router.add_post("/history", handle_ok)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    monkeypatch.setitem(horde_dreamer.SETTINGS, "COMFY_API_URL", f"http://127.0.0.1:{port}")

    try:
        out = await horde_dreamer._run_local_comfy_silent({"1": {}}, timeout=3.0)
        if ws_sequence == "proper_completion":
            assert out == img_bytes
        else:
            assert out is None
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "modal_seq",
    ["image_before_error", "error_before_image", "truncated_eof", "proper_done"],
)
async def test_sec5_modal_runner_outcome_validation(monkeypatch, _isolated_dreamer_env, modal_seq):
    img_b64 = base64.b64encode(_make_png_bytes(size=(512, 512))).decode("ascii")
    cleaned_artifacts: list[str] = []
    app = web.Application()

    async def handle_stream(request):
        resp = web.StreamResponse(status=200)
        await resp.prepare(request)
        if modal_seq == "image_before_error":
            await resp.write((json.dumps({"event": "result", "image_b64": img_b64, "artifact": "a1"}) + "\n").encode())
            await resp.write((json.dumps({"event": "error", "error": "CUDA OOM"}) + "\n").encode())
        elif modal_seq == "error_before_image":
            await resp.write((json.dumps({"event": "error", "error": "Failed", "artifact": "a2"}) + "\n").encode())
        elif modal_seq == "truncated_eof":
            await resp.write(b'{"event": "progress", "artifact": "a3"}\n{"truncated_json": ')
        elif modal_seq == "proper_done":
            await resp.write((json.dumps({"event": "done", "image_b64": img_b64}) + "\n").encode())
        await resp.write_eof()
        return resp

    async def handle_cleanup(request):
        body = await request.json()
        cleaned_artifacts.append(body["artifact"])
        return web.json_response({"ok": True})

    app.router.add_post("/generate_stream", handle_stream)
    app.router.add_post("/cleanup", handle_cleanup)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    monkeypatch.setitem(horde_dreamer.SETTINGS, "MODAL_COMFY_URL", f"http://127.0.0.1:{port}")

    try:
        out = await horde_dreamer._run_modal_krea_silent({"1": {}}, timeout=3.0)
        if modal_seq == "proper_done":
            assert out is not None
        else:
            assert out is None
            assert len(cleaned_artifacts) == 1
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
@pytest.mark.parametrize("anomaly", ["wrong_dimensions_64x64", "missing_model_in_pop", "missing_payload_in_pop"])
async def test_sec5_dimension_mismatch_and_malformed_pop_fault_cleanly(monkeypatch, _isolated_dreamer_env, anomaly):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())
        # Return a 64x64 thumbnail for a 512x512 job
        render_mock = AsyncMock(return_value=_make_png_bytes(size=(64, 64)))
        monkeypatch.setattr(horde_dreamer, "_run_local_comfy_silent", render_mock)

        faults: list[dict[str, Any]] = []
        app = web.Application()

        async def handle_pop(request):
            base_payload = {
                "prompt": "test",
                "width": 512,
                "height": 512,
                "steps": 20,
                "cfg_scale": 7.0,
                "sampler_name": "k_euler",
                "seed": "9",
            }
            if anomaly == "missing_model_in_pop":
                return web.json_response({"ids": ["job-no-model"], "payload": base_payload})
            if anomaly == "missing_payload_in_pop":
                return web.json_response({"ids": ["job-no-payload"], "model": "CyberRealistic Pony"})
            return web.json_response({
                "ids": ["job-64x64"],
                "model": "CyberRealistic Pony",
                "payload": base_payload,
            })

        async def handle_submit(request):
            faults.append(await request.json())
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
            if anomaly in ("missing_model_in_pop", "missing_payload_in_pop"):
                assert render_mock.call_count == 0, "Malformed pop must never fall back to models_to_offer[0]"
            assert len(faults) == 1
            assert faults[0]["state"] == "faulted"
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# Section 6: Repair management authorization boundary
# ==============================================================================

def _make_dummy_request(
    client_host: str = "127.0.0.1",
    headers: dict[str, str] | None = None,
    state_attrs: dict[str, Any] | None = None,
):
    hdrs = {k.lower(): v for k, v in (headers or {}).items()}
    state_obj = types.SimpleNamespace(**(state_attrs or {}))
    client_obj = types.SimpleNamespace(host=client_host, port=54321)
    return types.SimpleNamespace(client=client_obj, headers=hdrs, state=state_obj)


def test_sec6_owner_authorization_boundary_matrix(monkeypatch, _isolated_dreamer_env):
    # Ensure no `extensions.autopilot` interference for baseline checks
    monkeypatch.setitem(sys.modules, "extensions.autopilot", types.SimpleNamespace(_check_owner_request=None))
    monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_OWNER_SECRET", "s3cr3t-owner-token")

    # 1. Direct remote caller — rejected even with spoofed forwarding + owner headers
    req_remote = _make_dummy_request(
        client_host="203.0.113.42",
        headers={"X-Forwarded-For": "127.0.0.1", "X-OMD-Caller-Role": "owner"},
    )
    assert horde_dreamer._check_owner(req_remote) is False

    # 2. Genuine unproxied local loopback owner -> allowed
    req_local_owner = _make_dummy_request(client_host="127.0.0.1")
    assert horde_dreamer._check_owner(req_local_owner) is True

    # 3. Loopback guest / shared callers -> rejected (no loopback fallthrough)
    for role in ("guest", "shared", "anonymous", "viewer"):
        assert horde_dreamer._check_owner(_make_dummy_request("127.0.0.1", {"X-OMD-Caller-Role": role})) is False
        assert horde_dreamer._check_owner(_make_dummy_request("127.0.0.1", state_attrs={"role": role})) is False
    assert horde_dreamer._check_owner(_make_dummy_request("127.0.0.1", state_attrs={"is_guest": True})) is False

    # 4. Proxied loopback caller with unverified `X-OMD-Caller-Role: owner` -> rejected
    req_proxied_unverified = _make_dummy_request(
        client_host="127.0.0.1",
        headers={"X-Forwarded-For": "198.51.100.25", "X-OMD-Caller-Role": "owner"},
    )
    assert horde_dreamer._check_owner(req_proxied_unverified) is False

    # 5. Proxied loopback owner with verified secret OR paired DID -> allowed
    req_proxied_secret = _make_dummy_request(
        client_host="127.0.0.1",
        headers={
            "X-Forwarded-For": "198.51.100.25",
            "X-OMD-Caller-Role": "owner",
            "X-Horde-Owner-Secret": "s3cr3t-owner-token",
        },
    )
    assert horde_dreamer._check_owner(req_proxied_secret) is True

    fake_api = types.SimpleNamespace(is_did_paired=lambda did: did == "did:key:zVerifiedOwner")
    monkeypatch.setitem(sys.modules, "api", fake_api)
    req_proxied_did = _make_dummy_request(
        client_host="127.0.0.1",
        headers={
            "X-Forwarded-For": "198.51.100.25",
            "X-OMD-Caller-Role": "owner",
            "X-OMD-Caller-DID": "did:key:zVerifiedOwner",
        },
    )
    assert horde_dreamer._check_owner(req_proxied_did) is True

    # 6. Authoritative host guard (`extensions.autopilot._check_owner_request`) returning False MUST NOT fall through
    monkeypatch.setitem(
        sys.modules,
        "extensions.autopilot",
        types.SimpleNamespace(_check_owner_request=lambda req: False),
    )
    assert horde_dreamer._check_owner(req_local_owner) is False

    # 7. FastAPI route-level HTTP check via TestClient (`testclient` non-loopback host vs verified `HORDE_OWNER_SECRET` / `trusted_owner`)
    monkeypatch.setitem(sys.modules, "extensions.autopilot", types.SimpleNamespace(_check_owner_request=None))
    monkeypatch.setattr(horde_dreamer, "fetch_horde_user_info", AsyncMock(return_value=None))
    fapp = FastAPI()
    fapp.include_router(horde_dreamer.router)
    client = TestClient(fapp)
    assert client.get("/horde/api/state", headers={"X-OMD-Caller-Role": "guest"}).status_code == 404
    assert client.post("/horde/api/config", json={"enabled": True}, headers={"X-OMD-Caller-Role": "owner"}).status_code == 404


# ==============================================================================
# Section 7: Preserve operator policy through configuration changes & recovery
# ==============================================================================

@pytest.mark.asyncio
async def test_sec7_concurrent_config_preserved_across_maintenance_auto_pause(monkeypatch, _isolated_dreamer_env):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())

        # Initial saved state: enabled=True, nsfw=True, worker_name="InitialName"
        horde_dreamer.save_dreamer_data({
            **horde_dreamer._default_data(),
            "enabled": True,
            "nsfw": True,
            "worker_name": "InitialName",
            "sdxl_models": ["CyberRealistic Pony"],
        })

        app = web.Application()

        async def handle_pop(request):
            # While `/generate/pop` is in flight, operator updates SFW policy and worker_name!
            horde_dreamer.update_dreamer_data(
                lambda d: d.update({"nsfw": False, "worker_name": "OperatorUpdatedName"})
            )
            return web.Response(status=403, text='{"rc": "WorkerMaintenance", "message": "Maintenance mode"}')

        app.router.add_post("/generate/pop", handle_pop)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")

        try:
            res = await horde_dreamer.run_single_dreamer_cycle(ignore_enabled=False)
            assert res["status"] == "maintenance_paused"
            after = horde_dreamer.load_dreamer_data()
            assert after["enabled"] is False, "Maintenance response must pause the worker"
            assert after["nsfw"] is False, "Concurrent SFW operator update must NOT be overwritten"
            assert after["worker_name"] == "OperatorUpdatedName", "Concurrent worker_name update must be preserved"
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


@pytest.mark.asyncio
async def test_sec7_empty_model_selection_and_fail_closed_state_recovery(monkeypatch, _isolated_dreamer_env):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setitem(sys.modules, "extensions.autopilot", types.SimpleNamespace(_check_owner_request=None))
        monkeypatch.setattr(horde_dreamer, "fetch_horde_user_info", AsyncMock(return_value=None))

        # Permissive installation defaults in SETTINGS
        monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_WORKER_ENABLED", "true")
        monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_WORKER_NSFW", "true")
        monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_WORKER_MODELS", "CyberRealistic Pony")

        # 1. Missing state file (genuine first run) -> uses documented installation defaults
        assert not env["data_file"].exists()
        first_run = horde_dreamer.load_dreamer_data()
        assert first_run["enabled"] is True
        assert first_run["nsfw"] is True
        assert horde_dreamer.get_active_models_for_pop(first_run) == ["CyberRealistic Pony"]

        # 2. Explicit empty model selection via `horde_update_config` -> persists `[]` and offers `[]`
        req_owner = _make_dummy_request("127.0.0.1")

        async def fake_json():
            return {"enabled": False, "nsfw": False, "sdxl_models": []}

        req_owner.json = fake_json
        await horde_dreamer.horde_update_config(req_owner)
        saved_empty = horde_dreamer.load_dreamer_data()
        assert saved_empty["sdxl_models"] == []
        assert horde_dreamer.get_active_models_for_pop(saved_empty) == []
        state_payload = await horde_dreamer._build_state_payload(refresh_user=False)
        assert state_payload["config"]["sdxl_models"] == []
        assert state_payload["runtime"]["active_models_offered"] == []

        # 3. Malformed JSON on existing file (even without in-memory last-known-good cache) -> fails closed!
        horde_dreamer._last_known_good_data = None
        env["data_file"].write_text("{corrupted-json", encoding="utf-8")
        recovered_malformed = horde_dreamer.load_dreamer_data()
        assert recovered_malformed["enabled"] is False
        assert recovered_malformed["nsfw"] is False
        assert recovered_malformed["sdxl_models"] == []

        # 4. Wrong-type JSON (`[1, 2, 3]`) -> fails closed!
        horde_dreamer._last_known_good_data = None
        env["data_file"].write_text("[1, 2, 3]", encoding="utf-8")
        recovered_wrong_type = horde_dreamer.load_dreamer_data()
        assert recovered_wrong_type["enabled"] is False
        assert recovered_wrong_type["nsfw"] is False
        assert recovered_wrong_type["sdxl_models"] == []

        # 5. Unreadable file permissions (`0o000`) when non-root -> fails closed!
        if os.geteuid() != 0:
            horde_dreamer._last_known_good_data = None
            env["data_file"].write_text('{"enabled": true}', encoding="utf-8")
            env["data_file"].chmod(0o000)
            try:
                recovered_unreadable = horde_dreamer.load_dreamer_data()
                assert recovered_unreadable["enabled"] is False
                assert recovered_unreadable["nsfw"] is False
                assert recovered_unreadable["sdxl_models"] == []
            finally:
                env["data_file"].chmod(0o600)
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# Section 8: Keep Krea excluded until separate prerequisites are satisfied
# ==============================================================================

@pytest.mark.asyncio
async def test_sec8_krea_permanently_excluded_and_faulted_if_popped(monkeypatch, _isolated_dreamer_env):
    env = _isolated_dreamer_env
    orig_catalog = {k: dict(v) for k, v in horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.items()}
    try:
        _install_fake_verified_model(env, "CyberRealistic Pony")
        monkeypatch.setitem(horde_dreamer.SETTINGS, "MODAL_COMFY_URL", "https://example-modal.invalid")
        monkeypatch.setitem(horde_dreamer.SETTINGS, "HORDE_KREA_REFERENCE_VERIFIED", "true")
        assert horde_dreamer.is_krea_reference_verified() is False

        data = horde_dreamer.load_dreamer_data()
        data["krea_enabled"] = True
        offered = horde_dreamer.get_active_models_for_pop(data)
        assert horde_dreamer.KREA_HORDE_MODEL_NAME not in offered

        monkeypatch.setattr(horde_dreamer, "is_local_comfy_idle", AsyncMock(return_value=True))
        monkeypatch.setattr(horde_dreamer, "_ensure_safety_checker", lambda: object())
        modal_mock = AsyncMock(return_value=_make_png_bytes())
        monkeypatch.setattr(horde_dreamer, "_run_modal_krea_silent", modal_mock)

        faults: list[dict[str, Any]] = []
        app = web.Application()

        async def handle_pop(request):
            body = await request.json()
            assert horde_dreamer.KREA_HORDE_MODEL_NAME not in body["models"]
            return web.json_response({
                "ids": ["job-krea-unexpected"],
                "model": horde_dreamer.KREA_HORDE_MODEL_NAME,
                "payload": {"prompt": "test", "width": 512, "height": 512, "steps": 20, "cfg_scale": 7.0, "seed": "1"},
            })

        async def handle_submit(request):
            faults.append(await request.json())
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
            assert modal_mock.call_count == 0
            assert len(faults) == 1 and faults[0]["state"] == "faulted"
        finally:
            await runner.cleanup()
    finally:
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.clear()
        horde_dreamer.LOCAL_SDXL_MODEL_CATALOG.update(orig_catalog)


# ==============================================================================
# Section 9: Client result handling & end-to-end Creative Studio routing
# ==============================================================================

@pytest.mark.asyncio
@pytest.mark.parametrize(
    "scenario",
    ["is_possible_false", "timeout", "cancelled", "done_with_is_possible_false", "success"],
)
async def test_sec9_image_client_completion_precedence_and_cleanup(monkeypatch, scenario):
    deleted_ids: list[str] = []
    app = web.Application()

    async def handle_async(request):
        return web.json_response({"id": f"img-task-{scenario}"}, status=202)

    async def handle_check(request):
        if scenario == "is_possible_false":
            return web.json_response({"done": False, "faulted": False, "is_possible": False})
        if scenario in ("timeout", "cancelled"):
            return web.json_response({"done": False, "faulted": False, "is_possible": True})
        if scenario == "done_with_is_possible_false":
            # Backend reports done=True while no eligible workers remain (`is_possible=False`)
            return web.json_response({"done": True, "faulted": False, "is_possible": False})
        return web.json_response({"done": True, "faulted": False, "is_possible": True})

    async def handle_status(request):
        webp_bytes = horde_dreamer._convert_png_to_webp(_make_png_bytes(size=(512, 512)))
        return web.json_response({
            "done": True,
            "is_possible": scenario != "done_with_is_possible_false",
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
            assert deleted_ids == [], "Completed image result must be delivered and NOT cancelled"
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "scenario",
    ["is_possible_false", "timeout", "stream_closed_early", "done_with_is_possible_false", "success"],
)
async def test_sec9_text_client_completion_precedence_and_cleanup(monkeypatch, scenario):
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
            "is_possible": scenario != "done_with_is_possible_false",
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
            max_wait_seconds=0.18 if scenario in ("is_possible_false", "timeout", "stream_closed_early") else 2.0,
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
                assert deleted_ids == [], "Completed text result must be delivered and NOT cancelled"
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
async def test_sec9_creative_studio_end_to_end_horde_image_routing(monkeypatch, _isolated_dreamer_env):
    """Verifies the end-to-end user path from `core_service.generate_image(ctx, prompt)` through AI Horde."""
    local_called = False

    async def fake_local_generate_image(ctx, prompt, *args, **kwargs):
        nonlocal local_called
        local_called = True
        return ("IMG_LOCAL.png", "Local", "Local ComfyUI", _make_png_bytes(size=(512, 512)))

    fake_cs = types.SimpleNamespace(
        perform_prompt_stream=None,
        generate_image=fake_local_generate_image,
    )
    monkeypatch.setitem(sys.modules, "core_service", fake_cs)
    horde_dreamer.install_horde_client_hooks()

    app = web.Application()

    async def handle_async(request):
        body = await request.json()
        assert "aurora borealis over fjord" in body["prompt"]
        assert body["models"] == ["AlbedoBase XL (SDXL)"]
        return web.json_response({"id": "studio-horde-task-1"}, status=202)

    async def handle_check(request):
        return web.json_response({"done": True, "faulted": False, "is_possible": True})

    async def handle_status(request):
        webp_bytes = horde_dreamer._convert_png_to_webp(_make_png_bytes(size=(512, 512)))
        return web.json_response({
            "done": True,
            "generations": [{
                "img": base64.b64encode(webp_bytes).decode("ascii"),
                "worker_name": "CommunityNode42",
                "model": "AlbedoBase XL (SDXL)",
            }],
        })

    app.router.add_post("/generate/async", handle_async)
    app.router.add_get("/generate/check/{task_id}", handle_check)
    app.router.add_get("/generate/status/{task_id}", handle_status)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    monkeypatch.setattr(horde_dreamer, "HORDE_API_BASE", f"http://127.0.0.1:{port}")

    orig_hgi = horde_dreamer.horde_generate_image

    async def fast_hgi(*args, **kwargs):
        kwargs["poll_interval"] = 0.03
        kwargs["max_wait_seconds"] = 2
        return await orig_hgi(*args, **kwargs)

    monkeypatch.setattr(horde_dreamer, "horde_generate_image", fast_hgi)

    try:
        # 1. With `ai_mode == "horde"`, `core_service.generate_image` routes through AI Horde end-to-end
        ctx_horde = types.SimpleNamespace(
            settings={"ai_mode": "horde", "horde_image_model": "AlbedoBase XL (SDXL)"}
        )
        fname, title, desc, png_bytes = await fake_cs.generate_image(ctx_horde, "aurora borealis over fjord")
        assert not local_called
        assert fname.startswith("IMG_HORDE_") and fname.endswith(".png")
        assert "AlbedoBase XL (SDXL)" in title
        assert "CommunityNode42" in desc
        with Image.open(io.BytesIO(png_bytes)) as im:
            assert im.size == (512, 512)

        # 2. With `ai_mode == "local"`, falls through cleanly to local generator
        ctx_local = types.SimpleNamespace(settings={"ai_mode": "local"})
        fname_loc, _, _, _ = await fake_cs.generate_image(ctx_local, "local test")
        assert local_called and fname_loc == "IMG_LOCAL.png"
    finally:
        await runner.cleanup()
