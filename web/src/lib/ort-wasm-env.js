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

