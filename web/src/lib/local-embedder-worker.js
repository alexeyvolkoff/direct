/**
 * Web Worker for client-side multilingual embeddings via @huggingface/transformers.
 * Uses Xenova/paraphrase-multilingual-MiniLM-L12-v2 (384-dim), matching the backend
 * SentenceTransformer vector space, cached in the browser's Cache API.
 */
import { pipeline, env } from "@huggingface/transformers";
import { configureLocalOrtWasm } from "./ort-wasm-env.js";

configureLocalOrtWasm(env);

const MODEL_ID = "Xenova/paraphrase-multilingual-MiniLM-L12-v2";
let extractorPromise = null;

async function getExtractor() {
  if (!extractorPromise) {
    const fileProgress = new Map();
    const emitProgress = (info) => {
      if (!info) return;
      if (info.status === "progress" && info.file) {
        const pct = typeof info.progress === "number" ? info.progress : 0;
        fileProgress.set(info.file, pct);
        const vals = Array.from(fileProgress.values());
        const avg = vals.length ? vals.reduce((a, b) => a + b, 0) / vals.length : pct;
        self.postMessage({
          type: "progress",
          file: info.file.split("/").pop() || info.file,
          progress: Math.min(99, Math.round(avg)),
        });
      } else if (info.status === "done" && info.file) {
        fileProgress.set(info.file, 100);
      }
    };

    extractorPromise = pipeline("feature-extraction", MODEL_ID, {
      dtype: "q8",
      progress_callback: emitProgress,
    }).catch(() =>
      pipeline("feature-extraction", MODEL_ID, {
        quantized: true,
        progress_callback: emitProgress,
      })
    );
  }
  return extractorPromise;
}

self.onmessage = async (event) => {
  const msg = event.data || {};
  if (msg.type === "init") {
    try {
      await getExtractor();
      self.postMessage({ type: "ready" });
    } catch (e) {
      extractorPromise = null;
      self.postMessage({ type: "error", error: e?.message || String(e) });
    }
    return;
  }

  if (msg.type === "embed") {
    const { id, texts } = msg;
    try {
      const extractor = await getExtractor();
      const input = Array.isArray(texts) ? texts : [String(texts || "")];
      const output = await extractor(input, { pooling: "mean", normalize: true });
      const dim = output.dims?.[output.dims.length - 1] || 384;
      const data = output.data;
      const embeddings = [];
      for (let i = 0; i < input.length; i++) {
        const start = i * dim;
        embeddings.push(Array.from(data.slice(start, start + dim)));
      }
      self.postMessage({ type: "embed-result", id, embeddings });
    } catch (e) {
      self.postMessage({ type: "embed-error", id, error: e?.message || String(e) });
    }
  }
};

