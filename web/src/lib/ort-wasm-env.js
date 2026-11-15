/**
 * Configures @huggingface/transformers ONNX Runtime Web backend to load
 * WASM/MJS binaries from the app's bundled /ort-wasm/ directory instead of
 * external CDNs (ensures offline capability and App Store 2.5.2 compliance).
 */
export function configureLocalOrtWasm(env) {
  env.allowLocalModels = false;
  env.useBrowserCache = true;

  if (env.backends?.onnx?.wasm) {
    const base = new URL("/ort-wasm/", self.location.origin).href;
    const current = env.backends.onnx.wasm.wasmPaths;
    if (current && typeof current === "object") {
      const mjsFile = (current.mjs || "ort-wasm-simd-threaded.mjs").split("/").pop();
      const wasmFile = (current.wasm || "ort-wasm-simd-threaded.wasm").split("/").pop();
      env.backends.onnx.wasm.wasmPaths = {
        mjs: `${base}${mjsFile}`,
        wasm: `${base}${wasmFile}`,
      };
    } else {
      env.backends.onnx.wasm.wasmPaths = base;
    }
  }
}

/**
 * Creates a monotonic, byte-weighted progress emitter for @huggingface/transformers
 * model downloads so the progress bar never jumps backwards as new files start.
 */
export function createModelProgressEmitter(postFn) {
  const files = new Map();
  let maxPct = 0;

  return (info) => {
    if (!info || !info.file) return;
    const prev = files.get(info.file) || { loaded: 0, total: 0, pct: 0 };

    if (info.status === "progress") {
      const hasBytes = typeof info.total === "number" && info.total > 0 && typeof info.loaded === "number";
      const pct = typeof info.progress === "number" ? info.progress : prev.pct;
      files.set(info.file, {
        loaded: hasBytes ? Math.max(prev.loaded, info.loaded) : prev.loaded,
        total: hasBytes ? Math.max(prev.total, info.total) : prev.total,
        pct: Math.max(prev.pct, pct),
      });
    } else if (info.status === "done") {
      files.set(info.file, {
        loaded: prev.total || prev.loaded || 1,
        total: prev.total || prev.loaded || 1,
        pct: 100,
      });
    } else {
      return;
    }

    let totalBytes = 0;
    let loadedBytes = 0;
    let pctSum = 0;
    for (const entry of files.values()) {
      totalBytes += entry.total;
      loadedBytes += entry.loaded;
      pctSum += entry.pct;
    }

    const rawPct = totalBytes > 0
      ? (loadedBytes / totalBytes) * 100
      : (files.size ? pctSum / files.size : 0);

    const nextPct = Math.min(99, Math.max(maxPct, Math.round(rawPct)));
    if (nextPct > maxPct || info.status === "progress") {
      maxPct = nextPct;
      postFn({
        type: "progress",
        file: info.file.split("/").pop() || info.file,
        progress: maxPct,
      });
    }
  };
}

