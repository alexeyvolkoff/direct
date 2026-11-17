"""
Embedded ComfyUI HTTP/WS bridge for `horde-worker-reGen` (`HordeInferenceProcess`).

Exposes `hordelib/_comfyui`'s native `PromptServer` + `PromptQueue` on `127.0.0.1:8188`
and executes Direct Studio / RP Chat workflows (`flow.json`, Cyberdelia SDXL + DMD2,
CivitAI LoRAs, IP-Adapter, ReActor FaceSwap, and Impact Auto-Inpaint) cooperatively inside
`HordeInferenceProcess.worker_cycle()` on the single CUDA main thread without modifying
any `horde-worker-reGen` or `hordelib` source files.
"""

from __future__ import annotations

import asyncio
import copy
import gc
import glob
import logging
import os
import sys
import threading
import time
import traceback
from typing import Any

BRIDGE_HOST = os.environ.get("DIRECT_REGEN_COMFY_HOST", "127.0.0.1")
BRIDGE_PORT = int(os.environ.get("DIRECT_REGEN_COMFY_PORT", "8188"))
MODELS_BASE_DIR = os.environ.get("DIRECT_COMFY_MODELS_DIR", "/home/alexey/models/comfyui_models")
CUSTOM_NODES_DIR = os.environ.get("DIRECT_COMFY_CUSTOM_NODES_DIR", "/home/alexey/projects/ComfyUI/custom_nodes")
WEB_ROOT_DIR = os.environ.get("DIRECT_COMFY_WEB_DIR", "/home/alexey/projects/ComfyUI/web")
OVERLAY_SITE_DIR = os.environ.get("DIRECT_REGEN_OVERLAY_DIR", "/home/alexey/.cache/regen_comfy_overlay")
IDLE_VRAM_UNLOAD_SEC = float(os.environ.get("DIRECT_REGEN_IDLE_UNLOAD_SEC", "120"))

CUSTOM_NODE_PACKAGES = (
    "comfyui_essentials",
    "comfyui_ipadapter_plus_fork",
    "comfyui-reactor-node",
    "comfyui-impact-pack",
    "comfyui-impact-subpack",
    "rgthree-comfy",
)

_installed_hook = False


def _resolve_frontend_dirs() -> tuple[str | None, str | None, str | None]:
    """Locate modern comfyui-frontend-package static/templates/docs from standalone ComfyUI venv."""
    comfy_base = os.path.dirname(os.path.abspath(CUSTOM_NODES_DIR))
    candidates = [WEB_ROOT_DIR]
    candidates.extend(
        sorted(
            glob.glob(os.path.join(comfy_base, "venv", "lib", "python*", "site-packages", "comfyui_frontend_package", "static")),
            reverse=True,
        )
    )
    web_dir = next((p for p in candidates if p and os.path.isfile(os.path.join(p, "index.html"))), None)
    tpl_candidates = sorted(
        glob.glob(os.path.join(comfy_base, "venv", "lib", "python*", "site-packages", "comfyui_workflow_templates", "templates")),
        reverse=True,
    )
    doc_candidates = sorted(
        glob.glob(os.path.join(comfy_base, "venv", "lib", "python*", "site-packages", "comfyui_embedded_docs", "docs")),
        reverse=True,
    )
    tpl_dir = next((p for p in tpl_candidates if os.path.isdir(p)), None)
    doc_dir = next((p for p in doc_candidates if os.path.isdir(p)), None)
    return web_dir, tpl_dir, doc_dir


def _configure_comfy_paths_and_nodes() -> None:
    if os.path.isdir(OVERLAY_SITE_DIR) and OVERLAY_SITE_DIR not in sys.path:
        sys.path.append(OVERLAY_SITE_DIR)

    import comfy.cli_args
    import comfy.samplers
    import folder_paths
    import nodes

    if not hasattr(comfy.samplers, "SCHEDULER_HANDLERS"):
        comfy.samplers.SCHEDULER_HANDLERS = {name: None for name in comfy.samplers.SCHEDULER_NAMES}

    resolved_web_dir, _, _ = _resolve_frontend_dirs()
    if resolved_web_dir:
        comfy.cli_args.args.front_end_root = resolved_web_dir

    # Ensure trusted local YOLOv8 .pt detectors are whitelisted for PyTorch 2.6+ safe loading in Impact Subpack
    try:
        user_dir = folder_paths.get_user_directory()
        wl_dir = os.path.join(user_dir, "default", "ComfyUI-Impact-Subpack")
        os.makedirs(wl_dir, exist_ok=True)
        wl_file = os.path.join(wl_dir, "model-whitelist.txt")
        trusted_pt = ["person_yolov8m-seg.pt", "face_yolov8m.pt", "hand_yolov8s.pt"]
        existing_lines: list[str] = []
        if os.path.isfile(wl_file):
            with open(wl_file, "r", encoding="utf-8") as f:
                existing_lines = [ln.strip() for ln in f.readlines()]
        missing = [m for m in trusted_pt if m not in existing_lines]
        if missing:
            with open(wl_file, "a", encoding="utf-8") as f:
                for m in missing:
                    f.write(f"\n{m}\n")
    except Exception as exc:
        logging.debug(f"[DirectComfyBridge] Whitelist setup warning: {exc}")

    if os.path.isdir(MODELS_BASE_DIR):
        folder_paths.models_dir = MODELS_BASE_DIR
        model_subdirs = [
            ("checkpoints", "checkpoints"),
            ("loras", "loras"),
            ("vae", "vae"),
            ("clip_vision", "clip_vision"),
            ("ipadapter", "ipadapter"),
            ("insightface", "insightface"),
            ("hyperswap", "hyperswap"),
            ("facerestore_models", "facerestore_models"),
            ("sams", "sams"),
            ("ultralytics", "ultralytics"),
            ("ultralytics_bbox", "ultralytics/bbox"),
            ("ultralytics_segm", "ultralytics/segm"),
        ]
        extra_exts = set(folder_paths.supported_pt_extensions) | {".onnx", ".bin", ".safetensors", ".pth", ".pt"}
        for key, rel in model_subdirs:
            full_dir = os.path.join(MODELS_BASE_DIR, rel)
            if os.path.isdir(full_dir):
                existing_Entry = folder_paths.folder_names_and_paths.get(key)
                cur_dirs = list(existing_Entry[0]) if existing_Entry else []
                cur_exts = (set(existing_Entry[1]) | extra_exts) if existing_Entry else set(extra_exts)
                if full_dir not in cur_dirs:
                    cur_dirs.insert(0, full_dir)
                folder_paths.folder_names_and_paths[key] = (cur_dirs, cur_exts)

    if "DifferentialDiffusion" not in nodes.NODE_CLASS_MAPPINGS:
        nodes.init_extra_nodes(init_custom_nodes=False)

    if os.path.isdir(CUSTOM_NODES_DIR):
        for pkg_name in CUSTOM_NODE_PACKAGES:
            pkg_path = os.path.join(CUSTOM_NODES_DIR, pkg_name)
            if os.path.isdir(pkg_path):
                ok = nodes.load_custom_node(pkg_path)
                logging.info(f"[DirectComfyBridge] Custom node '{pkg_name}' loaded={ok}")


def _start_prompt_server_background() -> tuple[Any, Any]:
    import execution
    import folder_paths
    import server

    ready_evt = threading.Event()
    holder: dict[str, Any] = {}

    def _server_thread_main() -> None:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        srv = server.PromptServer(loop)
        q = execution.PromptQueue(srv)

        def _refresh_file_caches_on_prompt(json_data: dict) -> dict:
            try:
                folder_paths.filename_list_cache.clear()
            except Exception:
                pass
            return json_data

        srv.add_on_prompt_handler(_refresh_file_caches_on_prompt)

        async def _boot_and_serve() -> None:
            await srv.setup()
            srv.add_routes()
            await srv.start_multi_address([(BRIDGE_HOST, BRIDGE_PORT)])
            holder["server"] = srv
            holder["queue"] = q
            ready_evt.set()
            await srv.publish_loop()

        try:
            loop.run_until_complete(_boot_and_serve())
        except Exception as exc:
            holder["error"] = exc
            ready_evt.set()
            logging.error(f"[DirectComfyBridge] PromptServer loop failed: {exc}\n{traceback.format_exc()}")

    t = threading.Thread(target=_server_thread_main, name="DirectComfyPromptServer", daemon=True)
    t.start()
    ready_evt.wait(timeout=15.0)
    if "error" in holder:
        raise RuntimeError(f"Failed to start DirectComfyBridge on {BRIDGE_HOST}:{BRIDGE_PORT}: {holder['error']}")
    return holder["server"], holder["queue"]


def patch_regen_classes() -> None:
    global _installed_hook
    if _installed_hook:
        return
    _installed_hook = True

    import horde_worker_regen.process_management.worker_entry_points as wep
    from horde_worker_regen.process_management.inference_process import HordeInferenceProcess
    from horde_worker_regen.process_management.messages import HordeHeartbeatType

    orig_hordelib_init_wrapper = wep.start_inference_process
    orig_inf_init = HordeInferenceProcess.__init__
    orig_worker_cycle = HordeInferenceProcess.worker_cycle
    orig_start_inference = HordeInferenceProcess.start_inference
    orig_cleanup = HordeInferenceProcess.cleanup_for_exit

    def _patched_inf_init(self: Any, *args: Any, **kwargs: Any) -> None:
        orig_inf_init(self, *args, **kwargs)

        try:
            import hordelib.comfy_horde as ch
            for m_tag in ("sdxl", "flux", "cascade"):
                if m_tag not in ch.models_not_to_force_load:
                    ch.models_not_to_force_load.append(m_tag)
        except Exception as exc:
            logging.warning(f"[DirectComfyBridge] Could not update models_not_to_force_load: {exc}")

        self._direct_server = None
        self._direct_queue = None
        self._direct_executor = None
        self._direct_engine_owner = "horde"
        self._direct_last_job_ts = 0.0

        if getattr(self, "process_id", 1) != 1:
            return

        try:
            import execution

            # Dummy PromptServer placeholder while custom nodes register routes during import,
            # replaced immediately by the live PromptServer instance or started first!
            srv_loop_ready = _start_existing_or_new_server()
            srv, q = srv_loop_ready
            self._direct_server = srv
            self._direct_queue = q
            self._direct_executor = execution.PromptExecutor(srv, lru_size=2)
            logging.info(
                f"[DirectComfyBridge] Active inside HordeInferenceProcess (pid={os.getpid()}) "
                f"on http://{BRIDGE_HOST}:{BRIDGE_PORT}"
            )
        except Exception as exc:
            logging.error(f"[DirectComfyBridge] Initialization failed: {exc}\n{traceback.format_exc()}")

    def _start_existing_or_new_server() -> tuple[Any, Any]:
        import asyncio as _aio
        import execution
        import folder_paths
        import server

        ready_evt = threading.Event()
        holder: dict[str, Any] = {}

        def _thread_entry() -> None:
            loop = _aio.new_event_loop()
            _aio.set_event_loop(loop)
            try:
                from collections import deque
                import uuid as _uuid
                from aiohttp import web as _aweb
                import comfy.cli_args
                resolved_web_dir, resolved_tpl_dir, resolved_doc_dir = _resolve_frontend_dirs()
                if resolved_web_dir:
                    comfy.cli_args.args.front_end_root = resolved_web_dir
                srv = server.PromptServer(loop)
                q = execution.PromptQueue(srv)

                @srv.routes.get("/workflow_templates")
                async def _handle_workflow_templates(_request: Any) -> Any:
                    return _aweb.json_response({})

                @srv.routes.get("/i18n")
                async def _handle_comfy_i18n(_request: Any) -> Any:
                    return _aweb.json_response({})

                if resolved_tpl_dir:
                    srv.app.add_routes([_aweb.static("/templates", resolved_tpl_dir)])
                if resolved_doc_dir:
                    srv.app.add_routes([_aweb.static("/docs", resolved_doc_dir)])

                # Ring buffer so P2P Service-Worker iframe clients (/app/<node>/comfyui) can poll WS events over HTTP
                poll_events: deque[tuple[int, float, str | None, dict]] = deque(maxlen=400)
                poll_seq = 0
                orig_send_json = srv.send_json

                async def _patched_send_json(event: str, data: Any, sid: str | None = None) -> None:
                    nonlocal poll_seq
                    poll_seq += 1
                    poll_events.append((poll_seq, time.time(), sid, {"type": event, "data": data}))
                    await orig_send_json(event, data, sid)

                srv.send_json = _patched_send_json

                @srv.routes.get("/omd_ws_poll")
                async def _handle_omd_ws_poll(request: Any) -> Any:
                    sid = request.rel_url.query.get("clientId", "") or _uuid.uuid4().hex
                    try:
                        since = int(request.rel_url.query.get("since", "0") or 0)
                    except Exception:
                        since = 0
                    out_evts: list[dict] = []
                    if since <= 0:
                        out_evts.append({"type": "status", "data": {"status": srv.get_queue_info(), "sid": sid}})
                        if srv.client_id == sid and srv.last_node_id is not None:
                            out_evts.append({"type": "executing", "data": {"node": srv.last_node_id}})
                    cutoff = time.time() - 120.0
                    for seq_i, ts_i, ev_sid, payload_i in list(poll_events):
                        if seq_i > since and ts_i >= cutoff and (ev_sid is None or ev_sid == sid):
                            out_evts.append(payload_i)
                    return _aweb.json_response({"seq": poll_seq, "sid": sid, "events": out_evts})

                def _on_prompt(json_data: dict) -> dict:
                    try:
                        folder_paths.filename_list_cache.clear()
                    except Exception:
                        pass
                    return json_data

                srv.add_on_prompt_handler(_on_prompt)
                holder["server"] = srv
                holder["queue"] = q
                ready_evt.set()

                # Wait until custom nodes finish registering routes on main thread
                holder["routes_ready"].wait(timeout=30.0)

                async def _serve() -> None:
                    await srv.setup()
                    srv.add_routes()
                    await srv.start_multi_address([(BRIDGE_HOST, BRIDGE_PORT)])
                    holder["listening"].set()
                    await srv.publish_loop()

                loop.run_until_complete(_serve())
            except Exception as exc:
                holder["error"] = exc
                ready_evt.set()
                if "listening" in holder:
                    holder["listening"].set()
                logging.error(f"[DirectComfyBridge] Server thread error: {exc}\n{traceback.format_exc()}")

        holder["routes_ready"] = threading.Event()
        holder["listening"] = threading.Event()
        t = threading.Thread(target=_thread_entry, name="DirectComfyPromptServer", daemon=True)
        t.start()
        ready_evt.wait(timeout=10.0)
        if "error" in holder:
            raise holder["error"]

        try:
            _configure_comfy_paths_and_nodes()
        finally:
            holder["routes_ready"].set()

        holder["listening"].wait(timeout=15.0)
        if "error" in holder:
            raise holder["error"]
        return holder["server"], holder["queue"]

    def _install_direct_progress_hook(proc: Any, srv: Any) -> None:
        import comfy.model_management
        import comfy.utils
        from server import BinaryEventTypes

        def _hook(value: int, total: int, preview_image: Any) -> None:
            comfy.model_management.throw_exception_if_processing_interrupted()
            progress = {
                "value": value,
                "max": total,
                "prompt_id": getattr(srv, "last_prompt_id", None),
                "node": getattr(srv, "last_node_id", None),
            }
            srv.send_sync("progress", progress, srv.client_id)
            if preview_image is not None:
                srv.send_sync(BinaryEventTypes.UNENCODED_PREVIEW_IMAGE, preview_image, srv.client_id)
            try:
                proc.send_heartbeat_message(heartbeat_type=HordeHeartbeatType.PIPELINE_STATE_CHANGE)
            except Exception:
                pass

        comfy.utils.set_progress_bar_global_hook(_hook)

    def _patched_worker_cycle(self: Any) -> None:
        orig_worker_cycle(self)
        q = getattr(self, "_direct_queue", None)
        srv = getattr(self, "_direct_server", None)
        executor = getattr(self, "_direct_executor", None)
        if q is None or srv is None or executor is None:
            return

        import comfy.model_management
        import comfy.utils
        import execution
        import folder_paths
        import torch
        from hordelib.comfy_horde import unload_all_models_vram

        # Handle /free endpoint flags
        flags = q.get_flags()
        free_mem = flags.get("free_memory", False)
        if flags.get("unload_models", free_mem) or free_mem:
            try:
                executor.reset()
                unload_all_models_vram()
                gc.collect()
                torch.cuda.empty_cache()
                self._direct_engine_owner = "none"
            except Exception as exc:
                logging.warning(f"[DirectComfyBridge] /free cleanup warning: {exc}")

        # Drain all queued Direct Studio / RP Chat jobs with top priority before returning to Horde loop
        while len(q.queue) > 0 and not self._end_process:
            queue_item = q.get(timeout=0.0)
            if queue_item is None:
                break

            item, item_id = queue_item
            prompt_id = item[1]
            prompt_graph = item[2]
            extra_data = item[3] or {}
            outputs_to_execute = item[4]

            self._inference_semaphore.acquire()
            self._is_busy = True
            t0 = time.perf_counter()
            try:
                self.send_heartbeat_message(heartbeat_type=HordeHeartbeatType.PIPELINE_STATE_CHANGE)
                if self._direct_engine_owner == "horde":
                    logging.info("[DirectComfyBridge] Evicting Horde VRAM models before Direct SDXL workflow")
                    unload_all_models_vram()
                    gc.collect()
                    torch.cuda.empty_cache()

                self._direct_engine_owner = "direct"
                folder_paths.filename_list_cache.clear()
                srv.last_prompt_id = prompt_id
                _install_direct_progress_hook(self, srv)

                executor.execute(prompt_graph, prompt_id, extra_data, outputs_to_execute)

                q.task_done(
                    item_id,
                    executor.history_result,
                    status=execution.PromptQueue.ExecutionStatus(
                        status_str="success" if executor.success else "error",
                        completed=executor.success,
                        messages=executor.status_messages,
                    ),
                )
                if srv.client_id is not None:
                    srv.send_sync("executing", {"node": None, "prompt_id": prompt_id}, srv.client_id)

                elapsed = time.perf_counter() - t0
                self._direct_last_job_ts = time.time()
                logging.info(f"[DirectComfyBridge] Prompt {prompt_id} executed in {elapsed:.2f}s (success={executor.success})")
            except Exception as exc:
                logging.error(f"[DirectComfyBridge] Execution error for {prompt_id}: {exc}\n{traceback.format_exc()}")
                try:
                    q.task_done(
                        item_id,
                        {},
                        status=execution.PromptQueue.ExecutionStatus(
                            status_str="error",
                            completed=False,
                            messages=[("execution_error", {"prompt_id": prompt_id, "exception_message": str(exc)})],
                        ),
                    )
                except Exception:
                    pass
            finally:
                comfy.utils.set_progress_bar_global_hook(None)
                self._is_busy = False
                try:
                    self._inference_semaphore.release()
                except Exception:
                    pass
                self.send_heartbeat_message(heartbeat_type=HordeHeartbeatType.PIPELINE_STATE_CHANGE)

        # Auto-unload idle Direct SDXL VRAM after IDLE_VRAM_UNLOAD_SEC so Ollama and Horde have headroom
        if (
            self._direct_engine_owner == "direct"
            and self._direct_last_job_ts > 0
            and (time.time() - self._direct_last_job_ts) > IDLE_VRAM_UNLOAD_SEC
        ):
            try:
                logging.info("[DirectComfyBridge] Unloading idle Direct SDXL models from VRAM")
                executor.reset()
                comfy.model_management.unload_all_models()
                comfy.model_management.soft_empty_cache()
                gc.collect()
                torch.cuda.empty_cache()
                self._direct_engine_owner = "none"
            except Exception as exc:
                logging.debug(f"[DirectComfyBridge] Idle unload warning: {exc}")

    def _patched_start_inference(self: Any, job_info: Any) -> Any:
        executor = getattr(self, "_direct_executor", None)
        if getattr(self, "_direct_engine_owner", "horde") == "direct":
            try:
                import comfy.model_management
                import comfy.utils
                import torch
                from hordelib.comfy_horde import unload_all_models_vram

                logging.info("[DirectComfyBridge] Releasing Direct SDXL VRAM before AI Horde job")
                comfy.utils.set_progress_bar_global_hook(None)
                if executor is not None:
                    executor.reset()
                comfy.model_management.unload_all_models()
                unload_all_models_vram()
                gc.collect()
                torch.cuda.empty_cache()
            except Exception as exc:
                logging.warning(f"[DirectComfyBridge] Pre-Horde VRAM release warning: {exc}")
        self._direct_engine_owner = "horde"
        return orig_start_inference(self, job_info)

    def _patched_cleanup(self: Any) -> None:
        executor = getattr(self, "_direct_executor", None)
        if executor is not None:
            try:
                executor.reset()
            except Exception:
                pass
        return orig_cleanup(self)

    HordeInferenceProcess.__init__ = _patched_inf_init
    HordeInferenceProcess.worker_cycle = _patched_worker_cycle
    HordeInferenceProcess.start_inference = _patched_start_inference
    HordeInferenceProcess.cleanup_for_exit = _patched_cleanup
    _ = orig_hordelib_init_wrapper
