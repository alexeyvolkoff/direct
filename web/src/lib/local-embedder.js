/**
 * Client-side local embeddings manager (Web Worker + @huggingface/transformers).
 * Works identically in browser PWA and Capacitor Android/iOS WebView.
 */
import { writable } from "svelte/store";

const STORAGE_KEY = "omd_local_embeddings_enabled";

export function isLocalEmbeddingsEnabled() {
  if (typeof localStorage === "undefined") return false;
  return localStorage.getItem(STORAGE_KEY) === "true";
}

export const localEmbedderStore = writable({
  enabled: isLocalEmbeddingsEnabled(),
  status: "idle", // idle | downloading | ready | error
  progress: 0,
  currentFile: "",
  error: "",
});

let worker = null;
let initPromise = null;
let nextReqId = 1;
const pending = new Map();

function ensureWorker() {
  if (worker) return worker;
  if (typeof Worker === "undefined") {
    throw new Error("Web Workers are not supported in this environment");
  }
  worker = new Worker(new URL("./local-embedder-worker.js", import.meta.url), { type: "module" });
  worker.onmessage = (event) => {
    const msg = event.data || {};
    if (msg.type === "progress") {
      localEmbedderStore.update((s) => ({
        ...s,
        status: "downloading",
        progress: msg.progress ?? s.progress,
        currentFile: msg.file || s.currentFile,
        error: "",
      }));
    } else if (msg.type === "ready") {
      localEmbedderStore.update((s) => ({
        ...s,
        status: "ready",
        progress: 100,
        currentFile: "",
        error: "",
      }));
    } else if (msg.type === "error") {
      localEmbedderStore.update((s) => ({
        ...s,
        status: "error",
        error: msg.error || "Failed to load local embedding model",
      }));
    } else if (msg.type === "embed-result") {
      const cb = pending.get(msg.id);
      if (cb) {
        pending.delete(msg.id);
        cb.resolve(msg.embeddings);
      }
    } else if (msg.type === "embed-error") {
      const cb = pending.get(msg.id);
      if (cb) {
        pending.delete(msg.id);
        cb.reject(new Error(msg.error || "Local embedding failed"));
      }
    }
  };
  worker.onerror = (err) => {
    localEmbedderStore.update((s) => ({
      ...s,
      status: "error",
      error: err?.message || "Worker error",
    }));
  };
  return worker;
}

export function setLocalEmbeddingsEnabled(enabled) {
  const flag = !!enabled;
  if (typeof localStorage !== "undefined") {
    try {
      localStorage.setItem(STORAGE_KEY, flag ? "true" : "false");
    } catch (_) {}
  }
  localEmbedderStore.update((s) => ({ ...s, enabled: flag }));
  if (flag) {
    return initLocalEmbedder();
  }
  return Promise.resolve(false);
}

export function initLocalEmbedder() {
  if (initPromise) return initPromise;
  initPromise = new Promise((resolve) => {
    try {
      const w = ensureWorker();
      localEmbedderStore.update((s) => ({
        ...s,
        status: s.status === "ready" ? "ready" : "downloading",
        error: "",
      }));
      const unsub = localEmbedderStore.subscribe((s) => {
        if (s.status === "ready") {
          unsub();
          resolve(true);
        } else if (s.status === "error") {
          unsub();
          initPromise = null;
          resolve(false);
        }
      });
      w.postMessage({ type: "init" });
    } catch (e) {
      localEmbedderStore.update((s) => ({
        ...s,
        status: "error",
        error: e?.message || String(e),
      }));
      initPromise = null;
      resolve(false);
    }
  });
  return initPromise;
}

export async function embedTextsLocal(texts, signal = null) {
  if (!Array.isArray(texts) || texts.length === 0) return [];
  if (signal?.aborted) throw new DOMException("Aborted", "AbortError");
  const ok = await initLocalEmbedder();
  if (!ok) throw new Error("Local embedding model unavailable");
  const w = ensureWorker();
  const id = nextReqId++;
  return new Promise((resolve, reject) => {
    const onAbort = () => {
      pending.delete(id);
      reject(new DOMException("Aborted", "AbortError"));
    };
    if (signal) signal.addEventListener("abort", onAbort, { once: true });
    pending.set(id, {
      resolve: (res) => {
        if (signal) signal.removeEventListener("abort", onAbort);
        resolve(res);
      },
      reject: (err) => {
        if (signal) signal.removeEventListener("abort", onAbort);
        reject(err);
      },
    });
    w.postMessage({ type: "embed", id, texts });
  });
}

