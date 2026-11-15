/**
 * Client-side photo captioning & OCR via Web Worker (Florence-2-base-ft).
 * Produces caption + OCR text on-device so photos can be indexed end-to-end
 * without sending pixels to the AI node.
 */
import { writable } from "svelte/store";

const STORAGE_KEY = "omd_local_vision_enabled";

export function isLocalVisionEnabled() {
  try {
    return localStorage.getItem(STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

export const localVisionStore = writable({
  enabled: isLocalVisionEnabled(),
  status: "idle", // "idle" | "downloading" | "ready" | "error"
  progress: 0,
  currentFile: "",
  error: "",
});

let worker = null;
let initPromise = null;
let reqCounter = 0;
const pending = new Map();

function ensureWorker() {
  if (worker) return worker;
  worker = new Worker(new URL("./local-vision-worker.js", import.meta.url), {
    type: "module",
  });
  worker.onmessage = (event) => {
    const msg = event.data || {};
    if (msg.type === "progress") {
      localVisionStore.update((s) => ({
        ...s,
        status: "downloading",
        progress: msg.progress ?? s.progress,
        currentFile: msg.file || s.currentFile,
      }));
    } else if (msg.type === "ready") {
      localVisionStore.update((s) => ({
        ...s,
        status: "ready",
        progress: 100,
        currentFile: "",
        error: "",
      }));
      const item = pending.get("__init__");
      if (item) {
        pending.delete("__init__");
        item.resolve(true);
      }
    } else if (msg.type === "error") {
      initPromise = null;
      localVisionStore.update((s) => ({
        ...s,
        status: "error",
        error: msg.error || "Failed to load local vision model",
      }));
      const item = pending.get("__init__");
      if (item) {
        pending.delete("__init__");
        item.reject(new Error(msg.error || "Failed to load local vision model"));
      }
    } else if (msg.type === "recognize-result") {
      const item = pending.get(msg.id);
      if (item) {
        pending.delete(msg.id);
        item.resolve({ caption: msg.caption || "", ocr: msg.ocr || "" });
      }
    } else if (msg.type === "recognize-error") {
      const item = pending.get(msg.id);
      if (item) {
        pending.delete(msg.id);
        item.reject(new Error(msg.error || "Local vision recognition error"));
      }
    }
  };
  worker.onerror = (err) => {
    const message = err?.message || "Local vision worker error";
    initPromise = null;
    localVisionStore.update((s) => ({ ...s, status: "error", error: message }));
    for (const [k, item] of pending.entries()) {
      pending.delete(k);
      item.reject(new Error(message));
    }
  };
  return worker;
}

export function setLocalVisionEnabled(enabled) {
  const flag = Boolean(enabled);
  try {
    localStorage.setItem(STORAGE_KEY, flag ? "1" : "0");
  } catch {}
  localVisionStore.update((s) => ({ ...s, enabled: flag }));
  if (flag) {
    return initLocalVision();
  }
  return Promise.resolve(false);
}

export function initLocalVision() {
  if (initPromise) return initPromise;
  localVisionStore.update((s) => ({
    ...s,
    status: s.status === "ready" ? "ready" : "downloading",
    error: "",
  }));
  const w = ensureWorker();
  initPromise = new Promise((resolve, reject) => {
    pending.set("__init__", { resolve, reject });
    w.postMessage({ type: "init" });
  });
  return initPromise;
}

export async function recognizeImageLocal(jpegBytes, { filename = "", exif = null, signal = null } = {}) {
  if (!isLocalVisionEnabled()) {
    throw new Error("Local vision disabled");
  }
  if (signal?.aborted) {
    throw new DOMException("Aborted", "AbortError");
  }
  await initLocalVision();
  if (signal?.aborted) {
    throw new DOMException("Aborted", "AbortError");
  }
  const w = ensureWorker();
  const id = ++reqCounter;
  const copy = jpegBytes instanceof Uint8Array ? new Uint8Array(jpegBytes) : new Uint8Array(jpegBytes);
  const { caption, ocr } = await new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    w.postMessage({ type: "recognize", id, jpegBytes: copy }, [copy.buffer]);
  });

  const parts = [];
  if (caption) parts.push(caption);
  if (ocr) parts.push(`Text: ${ocr}`);
  if (exif?.takenAt) parts.push(`Date: ${exif.takenAt}`);
  if (exif?.gpsString) parts.push(`GPS: ${exif.gpsString}`);

  const tags = ["photo"];
  if (ocr) tags.push("ocr");
  if (exif?.gpsString) tags.push("gps");

  return {
    title: filename || "Photo",
    annotation: parts.join("\n").trim() || filename || "Photo",
    tags,
  };
}

