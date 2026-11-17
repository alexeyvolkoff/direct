/**
 * Client-side photo captioning & OCR:
 * - On native mobile (Capacitor Android/iOS), uses NativeStorage.analyzeImage
 *   (ML Kit Image Labeling + Text Recognition / Apple Vision) in ~50-150ms.
 * - On desktop web/PWA, falls back to Florence-2-base-ft in a Web Worker.
 */
import { writable } from "svelte/store";
import { isNativeStorageAvailable } from "./native-drive.js";

const STORAGE_KEY = "omd_local_vision_enabled";

export function isNativeVisionSupported() {
  return isNativeStorageAvailable();
}

export function isLocalVisionEnabled() {
  try {
    return localStorage.getItem(STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

export const localVisionStore = writable({
  enabled: isLocalVisionEnabled(),
  status: isNativeVisionSupported() && isLocalVisionEnabled() ? "ready" : "idle", // "idle" | "downloading" | "ready" | "error"
  progress: isNativeVisionSupported() && isLocalVisionEnabled() ? 100 : 0,
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
  if (isNativeVisionSupported()) {
    localVisionStore.update((s) => ({
      ...s,
      status: "ready",
      progress: 100,
      currentFile: "",
      error: "",
    }));
    return Promise.resolve(true);
  }
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

function buildPhotoResult({
  labels = [],
  caption = "",
  ocr = "",
  faceCount = 0,
  people = [],
  location = "",
  filename = "",
  exif = null,
}) {
  const parts = [];
  if (caption) parts.push(caption);
  else if (labels.length) parts.push(labels.join(", "));
  if (people.length > 0) {
    parts.push(`People: ${people.join(", ")}`);
  } else if (faceCount > 0) {
    parts.push(faceCount === 1 ? "Person" : `People: ${faceCount}`);
  }
  if (ocr) parts.push(`Text: ${ocr.slice(0, 1500)}`);
  if (exif?.takenAt) parts.push(`Date: ${exif.takenAt}`);
  const loc = String(location || "").trim();
  if (loc) parts.push(`Location: ${loc}`);
  else if (exif?.gpsString) parts.push(`GPS: ${exif.gpsString}`);

  const tags = ["photo"];
  const addTag = (t) => {
    const clean = String(t || "").trim().toLowerCase();
    if (clean && !tags.includes(clean)) tags.push(clean);
  };
  if (people.length > 0 || faceCount > 0) {
    addTag("person");
    if (faceCount === 1) addTag("portrait");
    else if (faceCount > 1) addTag("group");
    for (const p of people) addTag(p);
  }
  for (const lbl of labels.slice(0, 5)) addTag(lbl);
  if (loc) {
    for (const part of loc.split(",")) addTag(part);
  } else if (exif?.gpsString) {
    addTag("gps");
  }
  if (ocr) addTag("ocr");

  return {
    title: filename || "Photo",
    annotation: parts.join("\n").trim() || filename || "Photo",
    tags,
    location: loc,
    people,
  };
}

async function matchPeopleOnNode(httpDrive, drive, path, signal) {
  if (!httpDrive || typeof httpDrive.fetch !== "function" || typeof drive.getThumbnail !== "function") {
    return [];
  }
  try {
    const thumbDataUrl = await drive.getThumbnail(path, 640, 640);
    if (!thumbDataUrl) return [];
    const res = await httpDrive.fetch("/api/faces/detect", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ image_b64: thumbDataUrl }),
      signal,
    });
    if (!res.ok) return [];
    const data = await res.json();
    const names = [];
    for (const f of data?.faces || []) {
      const name = String(f?.name || "").trim();
      if (name && !names.includes(name)) names.push(name);
    }
    return names;
  } catch (e) {
    if (e?.name === "AbortError") throw e;
    return [];
  }
}

export async function recognizeImageNative(drive, path, { filename = "", exif = null, httpDrive = null, signal = null } = {}) {
  if (!isLocalVisionEnabled()) {
    throw new Error("Local vision disabled");
  }
  const res = await drive.analyzeImage(path, 768);
  const labels = Array.isArray(res?.labels) ? res.labels : [];
  const ocr = typeof res?.text === "string" ? res.text.trim() : "";
  const faceCount = Number(res?.faceCount || 0);
  const location = typeof res?.location === "string" ? res.location.trim() : "";
  let people = [];
  if (faceCount > 0 && httpDrive) {
    people = await matchPeopleOnNode(httpDrive, drive, path, signal);
  }
  console.log(
    `[local-vision] native analyzed ${filename} in ${res?.elapsedMs ?? "?"}ms: labels=${labels.length}, faces=${faceCount}, people=${people.length}, ocrLen=${ocr.length}, loc=${location || "-"}`
  );
  return buildPhotoResult({ labels, ocr, faceCount, people, location, filename, exif });
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

  return buildPhotoResult({ caption, ocr, filename, exif });
}

