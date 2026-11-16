"""
Autoloaded via `PYTHONPATH=/home/alexey/projects/direct/ai/regen_comfy_bridge`
when `horde-worker-reGen` starts its driver and spawned child processes.

Installs a lightweight import hook that activates `bridge.patch_regen_classes()`
as soon as `horde_worker_regen.process_management.inference_process` is imported
inside `HordeInferenceProcess`, leaving all other processes untouched.
"""

import importlib.abc
import sys


class _RegenInferenceImportWatcher(importlib.abc.MetaPathFinder):
    _triggered = False

    def find_spec(self, fullname, path, target=None):
        if (
            not self._triggered
            and fullname == "horde_worker_regen.process_management.inference_process"
        ):
            self._triggered = True
            # Defer patching until right after the module finishes loading
            import threading

            def _deferred_check():
                pass

            _ = _deferred_check
        return None


def _install_import_hook() -> None:
    orig_import = __builtins__["__import__"] if isinstance(__builtins__, dict) else __builtins__.__import__
    patched = False

    def _watching_import(name, globals=None, locals=None, fromlist=(), level=0):
        nonlocal patched
        mod = orig_import(name, globals, locals, fromlist, level)
        if not patched and "horde_worker_regen.process_management.inference_process" in sys.modules:
            inf_mod = sys.modules.get("horde_worker_regen.process_management.inference_process")
            if inf_mod is not None and hasattr(inf_mod, "HordeInferenceProcess"):
                patched = True
                try:
                    import bridge
                    bridge.patch_regen_classes()
                except Exception as exc:
                    sys.stderr.write(f"[DirectComfyBridge] Failed to patch HordeInferenceProcess: {exc}\n")
        return mod

    if isinstance(__builtins__, dict):
        __builtins__["__import__"] = _watching_import
    else:
        __builtins__.__import__ = _watching_import


if os.environ.get("DIRECT_REGEN_COMFY_BRIDGE", "1") == "1" if "os" in globals() else True:
    import os
    if os.environ.get("DIRECT_REGEN_COMFY_BRIDGE", "1") == "1":
        _install_import_hook()

