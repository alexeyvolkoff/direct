"""
Unit & protocol tests for AI Horde OpenAI-compatible SSE Streaming (`horde_client.py`).
"""

import json
import os
import sys
import pytest
from aiohttp import web

AI_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if AI_DIR not in sys.path:
    sys.path.insert(0, AI_DIR)

import horde_client


def test_format_messages_for_oai_sanitizes_and_supports_vision():
    raw_messages = [
        {"role": "system", "content": "<|think|>\nYou are a helpful assistant.", "facts": ["f1"]},
        {"role": "assistant", "content": "Previous reply", "sources": ["s1"], "thinking": "old cot"},
        {"role": "user", "content": "What is in this photo?", "images": ["aGVsbG8="], "mem_id": "m1"},
    ]

    # Without vision enabled -> plain string content
    msgs_text = horde_client.format_messages_for_oai(raw_messages, allow_images=False)
    assert msgs_text[0] == {"role": "system", "content": "You are a helpful assistant."}
    assert msgs_text[1] == {"role": "assistant", "content": "Previous reply"}
    assert msgs_text[2] == {"role": "user", "content": "What is in this photo?"}

    # With vision enabled -> OpenAI multimodal content array
    msgs_vis = horde_client.format_messages_for_oai(raw_messages, allow_images=True)
    assert msgs_vis[2]["role"] == "user"
    assert msgs_vis[2]["content"] == [
        {"type": "text", "text": "What is in this photo?"},
        {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,aGVsbG8="}},
    ]


@pytest.mark.asyncio
async def test_oai_sse_streaming_reasoning_content_tools_and_priority_auth(monkeypatch):
    received_headers = {}
    received_body = {}
    deleted_ids = []

    app = web.Application()

    async def handle_chat_completions(request: web.Request):
        received_headers.update(dict(request.headers))
        received_body.update(await request.json())

        resp = web.StreamResponse(
            status=200,
            reason="OK",
            headers={
                "Content-Type": "text/event-stream",
                "Cache-Control": "no-cache",
                "x-should-retry": "false",
            },
        )
        await resp.prepare(request)

        sse_frames = [
            ": keepalive waiting in queue\n\n",
            "data: " + json.dumps({
                "id": "chatcmpl-req-uuid-1234",
                "object": "chat.completion.chunk",
                "created": 1794600000,
                "model": "Nimbz/Gemma-4-Dark-Orthia-31B",
                "choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}],
            }) + "\n\n",
            "data: " + json.dumps({
                "id": "chatcmpl-req-uuid-1234",
                "object": "chat.completion.chunk",
                "created": 1794600001,
                "model": "Nimbz/Gemma-4-Dark-Orthia-31B",
                "choices": [{"index": 0, "delta": {"reasoning_content": "Analyzing user request..."}, "finish_reason": None}],
            }) + "\n\n",
            "data: " + json.dumps({
                "id": "chatcmpl-req-uuid-1234",
                "object": "chat.completion.chunk",
                "created": 1794600002,
                "model": "Nimbz/Gemma-4-Dark-Orthia-31B",
                "choices": [{"index": 0, "delta": {"content": "Привет, "}, "finish_reason": None}],
            }) + "\n\n",
            "data: " + json.dumps({
                "id": "chatcmpl-req-uuid-1234",
                "object": "chat.completion.chunk",
                "created": 1794600003,
                "model": "Nimbz/Gemma-4-Dark-Orthia-31B",
                "choices": [{"index": 0, "delta": {"content": "мир!<|im_end|>"}, "finish_reason": None}],
            }) + "\n\n",
            "data: " + json.dumps({
                "id": "chatcmpl-req-uuid-1234",
                "object": "chat.completion.chunk",
                "created": 1794600004,
                "model": "Nimbz/Gemma-4-Dark-Orthia-31B",
                "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                "x_horde": {
                    "request_id": "req-uuid-1234",
                    "worker_name": "P100-Home-Rig",
                    "worker_id": "w-99",
                    "kudos": 4.5,
                    "estimated_usage": False,
                },
            }) + "\n\n",
            "data: " + json.dumps({
                "id": "chatcmpl-req-uuid-1234",
                "object": "chat.completion.chunk",
                "created": 1794600004,
                "model": "Nimbz/Gemma-4-Dark-Orthia-31B",
                "choices": [],
                "usage": {"prompt_tokens": 28, "completion_tokens": 12, "total_tokens": 40},
            }) + "\n\n",
            "data: [DONE]\n\n",
        ]
        for frame in sse_frames:
            await resp.write(frame.encode("utf-8"))
        await resp.write_eof()
        return resp

    async def handle_delete(request: web.Request):
        deleted_ids.append(request.match_info["task_id"])
        return web.json_response({"ok": True})

    app.router.add_post("/v1/chat/completions", handle_chat_completions)
    app.router.add_delete("/api/v2/generate/text/status/{task_id}", handle_delete)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]

    monkeypatch.setattr(horde_client, "HORDE_OAI_BASE_URL", f"http://127.0.0.1:{port}/v1")
    monkeypatch.setattr(horde_client, "HORDE_BASE_URL", f"http://127.0.0.1:{port}/api/v2")

    try:
        events = []
        async for ev in horde_client.horde_generate_stream(
            {
                "messages": [{"role": "user", "content": "Say hello"}],
                "think": True,
                "options": {"num_predict": 1024, "temperature": 0.7},
            },
            api_key="test-priority-token-123",
            preferred_model="Nimbz/Gemma-4-Dark-Orthia-31B",
        ):
            events.append(ev)

        assert received_headers.get("Authorization") == "Bearer test-priority-token-123"
        assert received_body["stream"] is True
        assert received_body["stream_options"] == {"include_usage": True}
        assert received_body["model"] == "Nimbz/Gemma-4-Dark-Orthia-31B"
        assert "models" not in received_body
        assert received_body["horde"]["include_reasoning"] is True

        thoughts = "".join(
            e["message"]["thinking"] for e in events if e.get("message", {}).get("thinking")
        )
        content = "".join(
            e["message"]["content"] for e in events if e.get("message", {}).get("content")
        )
        done_ev = next(e for e in events if e.get("done") is True)

        assert thoughts == "Analyzing user request..."
        assert content == "Привет, мир!"
        assert done_ev["eval_count"] == 12
        assert done_ev["worker_info"] == "P100-Home-Rig [Nimbz/Gemma-4-Dark-Orthia-31B]"
        assert done_ev["horde_meta"]["kudos"] == 4.5
        assert deleted_ids == [], "Completed OAI SSE stream must NOT cancel request"
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
async def test_oai_sse_early_close_cancels_active_horde_request(monkeypatch):
    deleted_ids = []
    app = web.Application()

    async def handle_chat_completions(request: web.Request):
        resp = web.StreamResponse(
            status=200,
            headers={"Content-Type": "text/event-stream"},
        )
        await resp.prepare(request)
        await resp.write((
            "data: " + json.dumps({
                "id": "chatcmpl-abort-me-777",
                "object": "chat.completion.chunk",
                "created": 1794600000,
                "model": "koboldcpp/L3-8B-Stheno-v3.2",
                "choices": [{"index": 0, "delta": {"content": "Partial "}, "finish_reason": None}],
            }) + "\n\n"
        ).encode("utf-8"))
        # Hold open until client disconnects
        try:
            await request.app["hold_event"].wait()
        except Exception:
            pass
        return resp

    async def handle_delete(request: web.Request):
        deleted_ids.append(request.match_info["task_id"])
        return web.json_response({"ok": True})

    import asyncio
    app["hold_event"] = asyncio.Event()
    app.router.add_post("/v1/chat/completions", handle_chat_completions)
    app.router.add_delete("/api/v2/generate/text/status/{task_id}", handle_delete)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]

    monkeypatch.setattr(horde_client, "HORDE_OAI_BASE_URL", f"http://127.0.0.1:{port}/v1")
    monkeypatch.setattr(horde_client, "HORDE_BASE_URL", f"http://127.0.0.1:{port}/api/v2")

    try:
        gen = horde_client.horde_generate_stream(
            {"messages": [{"role": "user", "content": "Long story"}]},
            preferred_model="koboldcpp/L3-8B-Stheno-v3.2",
        )
        seen_content = False
        async for ev in gen:
            if ev.get("message", {}).get("content") == "Partial ":
                seen_content = True
                break
        await gen.aclose()
        app["hold_event"].set()

        assert seen_content is True
        assert deleted_ids == ["abort-me-777"]
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
async def test_oai_midstream_error_envelope_surfaces_cleanly(monkeypatch):
    app = web.Application()

    async def handle_chat_completions(request: web.Request):
        resp = web.StreamResponse(status=200, headers={"Content-Type": "text/event-stream"})
        await resp.prepare(request)
        await resp.write((
            "data: " + json.dumps({
                "error": {
                    "message": "Worker stream diverged mid-reply",
                    "type": "server_error",
                    "code": "ChatStreamDiverged",
                }
            }) + "\n\ndata: [DONE]\n\n"
        ).encode("utf-8"))
        await resp.write_eof()
        return resp

    app.router.add_post("/v1/chat/completions", handle_chat_completions)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]

    monkeypatch.setattr(horde_client, "HORDE_OAI_BASE_URL", f"http://127.0.0.1:{port}/v1")

    try:
        events = []
        async for ev in horde_client.horde_generate_stream(
            {"messages": [{"role": "user", "content": "Test"}]},
            preferred_model="koboldcpp/test",
        ):
            events.append(ev)

        err_ev = next(e for e in events if "error" in e)
        assert "diverged" in err_ev["error"].lower()
        assert err_ev["done"] is True
    finally:
        await runner.cleanup()


@pytest.mark.asyncio
async def test_get_online_text_models_merges_oai_and_legacy(monkeypatch):
    app = web.Application()

    async def handle_oai_models(request: web.Request):
        return web.json_response({
            "object": "list",
            "data": [
                {
                    "id": "Nimbz/Gemma-4-Dark-Orthia-31B",
                    "object": "model",
                    "context_length": 262144,
                    "architecture": {"input_modalities": ["text", "image"], "output_modalities": ["text"]},
                    "top_provider": {"context_length": 262144, "max_completion_tokens": 16384},
                    "supported_parameters": ["max_tokens", "temperature", "tools", "reasoning_effort"],
                    "x_horde": {"workers": 2, "queued": 0.0},
                }
            ],
        })

    async def handle_legacy_models(request: web.Request):
        return web.json_response([
            {"name": "Nimbz/Gemma-4-Dark-Orthia-31B", "type": "text", "count": 6, "eta": 0, "performance": 120},
            {"name": "aphrodite/TheDrummer/Skyfall-31B-v4.2", "type": "text", "count": 5, "eta": 12, "performance": 95},
        ])

    app.router.add_get("/v1/models", handle_oai_models)
    app.router.add_get("/api/v2/status/models", handle_legacy_models)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]

    monkeypatch.setattr(horde_client, "HORDE_OAI_BASE_URL", f"http://127.0.0.1:{port}/v1")
    monkeypatch.setattr(horde_client, "HORDE_BASE_URL", f"http://127.0.0.1:{port}/api/v2")

    try:
        models = await horde_client.get_online_text_models(limit=10)
        assert len(models) == 2
        assert models[0]["name"] == "Nimbz/Gemma-4-Dark-Orthia-31B"
        assert models[0]["oai_supported"] is True
        assert models[0]["supports_tools"] is True
        assert models[0]["supports_vision"] is True
        assert models[0]["count"] == 6
        assert models[1]["name"] == "aphrodite/TheDrummer/Skyfall-31B-v4.2"
        assert models[1]["oai_supported"] is False
    finally:
        await runner.cleanup()


def test_tiny_model_filter_and_moe_ranking():
    # Sub-8B and debug models are filtered out
    for tiny in (
        "koboldcpp/Llama-3.2-1B-Instruct",
        "koboldcpp/Llama-3.2-3B-Instruct",
        "koboldcpp/Qwen3-0.6B",
        "koboldcpp/Qwen3-1.7B",
        "koboldcpp/Qwen3.5-4B",
        "koboldcpp/Gemma-4-E4B-it-Ultra-Uncensored-Heretic",
        "koboldcpp/debug-gemma-4-E2B-Queen-it",
    ):
        assert horde_client._is_tiny_or_debug_model(tiny) is True, f"Expected {tiny} to be marked tiny"

    # Capable >=8B and MoE models (26B-A4B, 35B-A3B, 128B) are preserved
    for good in (
        "koboldcpp/G4-MeroMero-26B-A4B-it-uncensored-heretic",
        "llamacpp/Qwen3.6-35B-A3B-Uncensored-HauhauCS-Aggressive",
        "Nimbz/Gemma-4-Dark-Orthia-31B",
        "aphrodite/TheDrummer/Behemoth-128B-v3",
        "koboldcpp/Cydonia-24B-v4zg-Q4_K_M",
        "koboldcpp/L3-8B-Stheno-v3.2",
    ):
        assert horde_client._is_tiny_or_debug_model(good) is False, f"Expected {good} NOT to be marked tiny"

    # 26B MoE and 128B rank strictly higher than 8B (even with 5 workers) and 1B (with 4 workers)
    cards = [
        {"id": "koboldcpp/Llama-3.2-1B-Instruct", "context_length": 16384, "x_horde": {"workers": 4, "queued": 0}},
        {"id": "koboldcpp/L3-8B-Stheno-v3.2", "context_length": 8192, "x_horde": {"workers": 5, "queued": 0}},
        {"id": "aphrodite/TheDrummer/Behemoth-128B-v3", "context_length": 32768, "x_horde": {"workers": 1, "queued": 0}},
        {"id": "koboldcpp/G4-MeroMero-26B-A4B-it-uncensored-heretic", "context_length": 262144, "x_horde": {"workers": 3, "queued": 0}},
    ]
    ranked = [c["id"] for c in sorted(cards, key=horde_client._rank_oai_card, reverse=True)]
    assert ranked[0] == "koboldcpp/G4-MeroMero-26B-A4B-it-uncensored-heretic"
    assert ranked[1] == "aphrodite/TheDrummer/Behemoth-128B-v3"
    assert ranked[-1] == "koboldcpp/Llama-3.2-1B-Instruct"


