/**
 * Autonomous client-side indexer for Mobile (Capacitor NativeStorageDrive)
 * and Web PWA (LocalDrive).
 *
 * - Documents: extracts text in-browser/WebView, chunks, embeds via AI node /embed,
 *   and stores cards + chunk embeddings in GunDB (ChatStore).
 * - Photos: filters by timeline depth (newest first), extracts EXIF GPS/date,
 *   downscales to <=1024px JPEG on-device, calls stateless /rag/recognize/raw,
 *   and stores cards + local 320px thumbnail in GunDB with live ETA & pause/resume.
 */

import { writable } from "svelte/store";
import {
  isSupportedDocumentExt,
  extractTextFromBytes,
  chunkText,
  embedTexts,
  computeContentHash,
} from "./client-rag.js";
import { isLocalVisionEnabled, recognizeImageLocal, recognizeImageNative } from "./local-vision.js";

const IMAGE_EXTS = new Set(["jpg", "jpeg", "png", "webp"]);
const MANIFEST_KEY = "omd_indexed_manifest_v1";

const initialState = {
  status: "idle", // idle | scanning | running | paused | done | error
  total: 0,
  processed: 0,
  indexed: 0,
  skipped: 0,
  failed: 0,
  docsFound: 0,
  photosFound: 0,
  currentFile: "",
  etaSeconds: 0,
  avgMsPerItem: 0,
  error: "",
};

export const indexerStore = writable({ ...initialState });

function isImageFile(name = "") {
  const clean = String(name || "").split("?")[0].split("#")[0];
  const dot = clean.lastIndexOf(".");
  if (dot < 0) return false;
  return IMAGE_EXTS.has(clean.slice(dot + 1).toLowerCase());
}

function loadManifest() {
  if (typeof localStorage === "undefined") return {};
  try {
    return JSON.parse(localStorage.getItem(MANIFEST_KEY) || "{}") || {};
  } catch (_) {
    return {};
  }
}

function saveManifest(manifest) {
  if (typeof localStorage === "undefined") return;
  try {
    localStorage.setItem(MANIFEST_KEY, JSON.stringify(manifest));
  } catch (_) {}
}

/**
 * Minimal pure-JS JPEG EXIF parser (reads DateTimeOriginal & GPS lat/lon).
 */
export function extractJpegExif(u8) {
  const result = { takenAt: "", gpsLat: null, gpsLon: null, gpsString: "" };
  if (!u8 || u8.length < 12 || u8[0] !== 0xff || u8[1] !== 0xd8) return result;

  const view = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  let offset = 2;
  let tiffStart = -1;

  while (offset + 4 < u8.length) {
    if (u8[offset] !== 0xff) break;
    const marker = u8[offset + 1];
    if (marker === 0xda || marker === 0xd9) break;
    const segLen = view.getUint16(offset + 2, false);
    if (marker === 0xe1 && offset + 10 < u8.length) {
      if (
        u8[offset + 4] === 0x45 && u8[offset + 5] === 0x78 &&
        u8[offset + 6] === 0x69 && u8[offset + 7] === 0x66 &&
        u8[offset + 8] === 0x00 && u8[offset + 9] === 0x00
      ) {
        tiffStart = offset + 10;
        break;
      }
    }
    offset += 2 + segLen;
  }

  if (tiffStart < 0 || tiffStart + 8 > u8.length) return result;
  const le = u8[tiffStart] === 0x49 && u8[tiffStart + 1] === 0x49;
  const getU16 = (p) => (p + 2 <= u8.length ? view.getUint16(p, le) : 0);
  const getU32 = (p) => (p + 4 <= u8.length ? view.getUint32(p, le) : 0);

  const readAscii = (valOffset, count) => {
    const abs = count <= 4 ? valOffset : tiffStart + getU32(valOffset);
    if (abs < 0 || abs + count > u8.length) return "";
    let s = "";
    for (let i = 0; i < count - 1; i++) {
      const c = u8[abs + i];
      if (c === 0) break;
      s += String.fromCharCode(c);
    }
    return s.trim();
  };

  const readRationals = (valOffset, count) => {
    const abs = tiffStart + getU32(valOffset);
    if (abs < 0 || abs + count * 8 > u8.length) return null;
    const out = [];
    for (let i = 0; i < count; i++) {
      const num = getU32(abs + i * 8);
      const den = getU32(abs + i * 8 + 4);
      out.push(den ? num / den : 0);
    }
    return out;
  };

  const ifd0Offset = getU32(tiffStart + 4);
  let exifIfdOffset = 0;
  let gpsIfdOffset = 0;

  const parseIfd = (relOffset, cb) => {
    const base = tiffStart + relOffset;
    if (base + 2 > u8.length) return;
    const count = getU16(base);
    for (let i = 0; i < count; i++) {
      const entry = base + 2 + i * 12;
      if (entry + 12 > u8.length) break;
      const tag = getU16(entry);
      const valCount = getU32(entry + 4);
      cb(tag, valCount, entry + 8);
    }
  };

  parseIfd(ifd0Offset, (tag, count, valOff) => {
    if (tag === 0x0132 && !result.takenAt) {
      result.takenAt = readAscii(valOff, count);
    } else if (tag === 0x8769) {
      exifIfdOffset = getU32(valOff);
    } else if (tag === 0x8825) {
      gpsIfdOffset = getU32(valOff);
    }
  });

  if (exifIfdOffset) {
    parseIfd(exifIfdOffset, (tag, count, valOff) => {
      if (tag === 0x9003) {
        const dt = readAscii(valOff, count);
        if (dt) result.takenAt = dt;
      }
    });
  }

  if (gpsIfdOffset) {
    let latRef = "N", lonRef = "E", latDms = null, lonDms = null;
    parseIfd(gpsIfdOffset, (tag, count, valOff) => {
      if (tag === 1) latRef = readAscii(valOff, count) || "N";
      else if (tag === 2) latDms = readRationals(valOff, 3);
      else if (tag === 3) lonRef = readAscii(valOff, count) || "E";
      else if (tag === 4) lonDms = readRationals(valOff, 3);
    });
    if (latDms && lonDms && latDms.length === 3 && lonDms.length === 3) {
      let lat = latDms[0] + latDms[1] / 60 + latDms[2] / 3600;
      let lon = lonDms[0] + lonDms[1] / 60 + lonDms[2] / 3600;
      if (latRef.toUpperCase() === "S") lat = -lat;
      if (lonRef.toUpperCase() === "W") lon = -lon;
      if (Number.isFinite(lat) && Number.isFinite(lon) && (lat !== 0 || lon !== 0)) {
        result.gpsLat = lat;
        result.gpsLon = lon;
        result.gpsString = `${lat.toFixed(5)}, ${lon.toFixed(5)}`;
      }
    }
  }

  return result;
}

async function resizeBlobToJpeg(blob, maxSide = 1024, quality = 0.78) {
  const bitmap = await createImageBitmap(blob);
  try {
    const scale = Math.min(1, maxSide / Math.max(bitmap.width, bitmap.height));
    const w = Math.max(1, Math.round(bitmap.width * scale));
    const h = Math.max(1, Math.round(bitmap.height * scale));
    const canvas = document.createElement("canvas");
    canvas.width = w;
    canvas.height = h;
    const ctx = canvas.getContext("2d");
    ctx.drawImage(bitmap, 0, 0, w, h);
    const outBlob = await new Promise((resolve) => canvas.toBlob(resolve, "image/jpeg", quality));
    const dataUrl = canvas.toDataURL("image/jpeg", 0.72);
    return { blob: outBlob, dataUrl };
  } finally {
    bitmap.close();
  }
}

async function dataUrlToBytes(dataUrl) {
  const res = await fetch(dataUrl);
  return new Uint8Array(await res.arrayBuffer());
}

async function readDriveFileBytes(drive, path, size = 0) {
  if (typeof drive.readFile === "function") {
    try {
      const res = await drive.readFile(path);
      const b64 = res && res.data ? res.data : (typeof res === "string" ? res : null);
      if (b64) {
        const bin = atob(b64.replace(/\s+/g, ""));
        const u8 = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
        return u8;
      }
    } catch (_) {}
  }
  if (typeof drive.getFileContent === "function") {
    const blob = await drive.getFileContent(path, size);
    if (blob && blob.arrayBuffer) {
      return new Uint8Array(await blob.arrayBuffer());
    }
  }
  if (typeof drive.readRange === "function") {
    const { bytes } = await drive.readRange(path, 0, size || 10 * 1024 * 1024);
    return bytes;
  }
  throw new Error("Drive does not support reading file bytes");
}

/**
 * Scan drive roots for indexable documents & photos.
 */
export async function scanDeviceForIndexable(drive, {
  deviceName = "Mobile",
  includeDocs = true,
  includePhotos = true,
  timelineDays = 30,
  chatStore = null,
  extraCards = [],
  maxScanFiles = 1500,
  onProgress = null,
} = {}) {
  if (!drive || typeof drive.getFiles !== "function") {
    return { docs: [], photos: [], skippedCount: 0 };
  }

  const manifest = loadManifest();
  const knownCards = [
    ...(chatStore ? chatStore.listKnowledge() : []),
    ...(Array.isArray(extraCards) ? extraCards : []),
  ];
  const existingDocs = new Set(knownCards.map((c) => c?.document_id).filter(Boolean));
  const existingHashes = new Set(knownCards.map((c) => c?.content_hash).filter(Boolean));

  const cutoffMs = timelineDays > 0 ? Date.now() - timelineDays * 86400 * 1000 : 0;
  const docs = [];
  const photos = [];
  let skippedCount = 0;
  let visited = 0;
  let totalVisited = 0;
  let lastProgressAt = 0;

  const emitProgress = (folder, force = false) => {
    if (typeof onProgress !== "function") return;
    const now = performance.now();
    if (!force && now - lastProgressAt < 80) return;
    lastProgressAt = now;
    try {
      onProgress({
        folder: folder || "/",
        scanned: totalVisited,
        docs: docs.length,
        photos: photos.length,
        skipped: skippedCount,
      });
    } catch (_) {}
  };

  const walk = async (folderPath, depth = 0, bucket = "") => {
    if (depth > 5 || visited >= maxScanFiles) return;
    emitProgress(folderPath, true);
    let entries = [];
    try {
      entries = await drive.getFiles(folderPath);
    } catch (_) {
      return;
    }
    if (!Array.isArray(entries)) return;

    // Sort directories first, then files newest-first so large camera folders
    // (10k+ old files) never hit maxScanFiles before reaching recent photos.
    const sorted = [...entries].sort((a, b) => {
      const aDir = !!(a.isDir || a.type === "folder");
      const bDir = !!(b.isDir || b.type === "folder");
      if (aDir !== bDir) return aDir ? -1 : 1;
      const aMs = a.date ? new Date(a.date).getTime() : 0;
      const bMs = b.date ? new Date(b.date).getTime() : 0;
      return bMs - aMs;
    });

    for (const item of sorted) {
      if (visited >= maxScanFiles) break;
      const name = item.name || item.value || "";
      if (!name || name.startsWith(".")) continue;
      const itemPath = item.realPath || item.id || `${folderPath.replace(/\/+$/, "")}/${name}`;
      const isDir = !!(item.isDir || item.type === "folder");

      if (isDir) {
        const lower = name.toLowerCase();
        let nextBucket = bucket;
        if (depth === 0) {
          visited = 0; // Give each root (DCIM, Pictures, Documents, Downloads) its own budget
          if (lower === "documents" || lower === "downloads") nextBucket = "docs";
          else if (lower === "camera" || lower === "pictures" || lower === "dcim") nextBucket = "photos";
          else if (drive.isNative) continue; // On mobile, focus on Documents/Downloads/Camera/Pictures
        }
        await walk(itemPath, depth + 1, nextBucket);
        continue;
      }

      visited++;
      totalVisited++;
      if (totalVisited % 25 === 0) {
        emitProgress(folderPath, false);
        await new Promise((r) => setTimeout(r, 0));
      }
      const docId = `/${deviceName}/${itemPath.replace(/^\/+/, "")}`;
      const mtimeMs = item.date ? new Date(item.date).getTime() : 0;
      if (bucket === "photos" && cutoffMs > 0 && mtimeMs > 0 && mtimeMs < cutoffMs) {
        break; // Remaining files in this directory are older than timelineDays
      }
      const size = Number(item.size || 0);
      const prev = manifest[docId];
      const manifestStillExists =
        prev &&
        prev.size === size &&
        prev.mtime === mtimeMs &&
        (prev.empty || existingDocs.has(docId) || (prev.hash && existingHashes.has(prev.hash)));
      if (manifestStillExists || existingDocs.has(docId)) {
        skippedCount++;
        continue;
      }

      if (includeDocs && bucket !== "photos" && isSupportedDocumentExt(name)) {
        if (size <= 15 * 1024 * 1024) {
          docs.push({ kind: "doc", name, path: itemPath, docId, size, mtimeMs });
        }
      } else if (includePhotos && bucket !== "docs" && isImageFile(name)) {
        if (!cutoffMs || !mtimeMs || mtimeMs >= cutoffMs) {
          photos.push({ kind: "photo", name, path: itemPath, docId, size, mtimeMs });
        }
      }
    }
  };

  await walk("/", 0, "");
  // Newest photos first
  photos.sort((a, b) => (b.mtimeMs || 0) - (a.mtimeMs || 0));
  emitProgress("", true);

  return { docs, photos, skippedCount };
}

class ClientIndexerRunner {
  constructor() {
    this._abort = null;
    this._paused = false;
    this._pauseResolvers = [];
  }

  reset() {
    if (this._abort) {
      try { this._abort.abort(); } catch (_) {}
    }
    this._paused = false;
    this._pauseResolvers.splice(0);
    indexerStore.set({ ...initialState });
  }

  pause() {
    this._paused = true;
    indexerStore.update((s) => (s.status === "running" ? { ...s, status: "paused" } : s));
  }

  resume() {
    this._paused = false;
    indexerStore.update((s) => (s.status === "paused" ? { ...s, status: "running" } : s));
    const resolvers = this._pauseResolvers.splice(0);
    resolvers.forEach((r) => r());
  }

  stop() {
    this._paused = false;
    const resolvers = this._pauseResolvers.splice(0);
    resolvers.forEach((r) => r());
    if (this._abort) {
      try { this._abort.abort(); } catch (_) {}
    }
    indexerStore.update((s) => ({ ...s, status: "idle", currentFile: "", etaSeconds: 0 }));
  }

  async _waitIfPaused(signal) {
    while (this._paused) {
      if (signal.aborted) throw new DOMException("Aborted", "AbortError");
      await new Promise((resolve) => this._pauseResolvers.push(resolve));
    }
  }

  async run({
    drive,
    httpDrive,
    chatStore,
    extraCards = [],
    deviceName = "Mobile",
    owner = "anonymous",
    includeDocs = true,
    includePhotos = true,
    timelineDays = 30,
  }) {
    if (!drive || !httpDrive || !chatStore) {
      indexerStore.set({ ...initialState, status: "error", error: "Drive, AI node, or ChatStore unavailable" });
      return;
    }

    this.stop();
    this._abort = new AbortController();
    const signal = this._abort.signal;

    indexerStore.set({ ...initialState, status: "scanning" });
    const { docs, photos, skippedCount } = await scanDeviceForIndexable(drive, {
      deviceName,
      includeDocs,
      includePhotos,
      timelineDays,
      chatStore,
      extraCards,
    });

    const queue = [...docs, ...photos].sort((a, b) => (b.mtimeMs || 0) - (a.mtimeMs || 0));
    if (queue.length === 0) {
      indexerStore.set({
        ...initialState,
        status: "done",
        skipped: skippedCount,
        docsFound: docs.length,
        photosFound: photos.length,
      });
      return;
    }

    indexerStore.set({
      ...initialState,
      status: "running",
      total: queue.length,
      skipped: skippedCount,
      docsFound: docs.length,
      photosFound: photos.length,
    });

    const manifest = loadManifest();
    const existingHashes = new Set(
      [...(chatStore ? chatStore.listKnowledge() : []), ...(Array.isArray(extraCards) ? extraCards : [])]
        .map((c) => c?.content_hash)
        .filter(Boolean)
    );
    let processed = 0;
    let indexed = 0;
    let skipped = skippedCount;
    let failed = 0;
    let avgMs = 0;

    for (const item of queue) {
      if (signal.aborted) break;
      await this._waitIfPaused(signal);
      if (signal.aborted) break;

      const t0 = performance.now();
      indexerStore.update((s) => ({ ...s, currentFile: item.name }));

      try {
        if (item.kind === "doc") {
          const bytes = await readDriveFileBytes(drive, item.path, item.size);
          const contentHash = await computeContentHash(bytes, item.size || bytes.length);
          if (existingHashes.has(contentHash)) {
            manifest[item.docId] = { size: item.size, mtime: item.mtimeMs, hash: contentHash, at: Date.now() };
            saveManifest(manifest);
            skipped++;
          } else {
            const text = await extractTextFromBytes(bytes, item.name);
            if (text && text.length >= 20) {
              // Cap per-document text/chunks on-device so huge dumps (.sql/.log) don't stall WASM
              const chunksTexts = chunkText(text.slice(0, 60000), 900, 150).slice(0, 64);
              const annotation = text.slice(0, 600).trim();
              const textsToEmbed = [annotation, ...chunksTexts];
              const embeddings = await embedTexts(httpDrive, textsToEmbed, 24, signal);
              const annotationEmbedding = embeddings[0] || null;
              const chunks = chunksTexts
                .map((ct, idx) => ({ text: ct, embedding: embeddings[idx + 1] }))
                .filter((c) => Array.isArray(c.embedding) && c.embedding.length > 0);

              await chatStore.addKnowledge({
                document_id: item.docId,
                title: item.name,
                text: annotation,
                tags: ["document", deviceName.toLowerCase()],
                collection: "user",
                content_hash: contentHash,
                annotation_embedding: annotationEmbedding,
                chunks,
                timestamp: item.mtimeMs || Date.now(),
              });
              existingHashes.add(contentHash);
              manifest[item.docId] = { size: item.size, mtime: item.mtimeMs, hash: contentHash, at: Date.now() };
              saveManifest(manifest);
              indexed++;
            } else {
              manifest[item.docId] = { size: item.size, mtime: item.mtimeMs, empty: true, at: Date.now() };
              saveManifest(manifest);
            }
          }
        } else {
          // Photo pipeline: EXIF + contentHash + <=1024px JPEG + local 320px thumbnail + /rag/recognize/raw
          let exif = { takenAt: "", gpsString: "" };
          let headBytes = null;
          let tailBytes = null;
          let rawBytes = null;
          if (typeof drive.readRange === "function") {
            try {
              const headRes = await drive.readRange(item.path, 0, 131072);
              headBytes = headRes?.bytes || null;
              if (headBytes) exif = extractJpegExif(headBytes);
              const totalSz = Number(headRes?.totalSize || item.size || 0);
              if (totalSz > 65536) {
                const tailOff = Math.max(0, totalSz - 65536);
                const tailRes = await drive.readRange(item.path, tailOff, 65536);
                tailBytes = tailRes?.bytes || null;
              }
            } catch (_) {}
          }

          let contentHash = "";
          if (headBytes) {
            const totalSz = Number(item.size || headBytes.length);
            const h64 = headBytes.length > 65536 ? headBytes.subarray(0, 65536) : headBytes;
            const t64 = tailBytes && totalSz > 65536 ? tailBytes : new Uint8Array(0);
            const combined = new Uint8Array(h64.length + t64.length);
            combined.set(h64, 0);
            if (t64.length) combined.set(t64, h64.length);
            contentHash = await computeContentHash(combined, totalSz);
          } else {
            rawBytes = await readDriveFileBytes(drive, item.path, item.size);
            contentHash = await computeContentHash(rawBytes, item.size || rawBytes.length);
          }

          if (contentHash && existingHashes.has(contentHash)) {
            manifest[item.docId] = { size: item.size, mtime: item.mtimeMs, hash: contentHash, at: Date.now() };
            saveManifest(manifest);
            skipped++;
          } else {
            let uploadBytes = null;
            let previewDataUrl = "";
            let data = null;

            if (isLocalVisionEnabled() && typeof drive.analyzeImage === "function") {
              try {
                const localRes = await recognizeImageNative(drive, item.path, {
                  filename: item.name,
                  exif,
                });
                if (typeof drive.getThumbnail === "function") {
                  previewDataUrl = (await drive.getThumbnail(item.path, 320, 320)) || "";
                }
                const [emb] = await embedTexts(httpDrive, [localRes.annotation], 1, signal);
                data = {
                  title: localRes.title,
                  annotation: localRes.annotation,
                  tags: localRes.tags,
                  annotation_embedding: Array.isArray(emb) ? emb : null,
                  chunks: [{ text: localRes.annotation, embedding: Array.isArray(emb) ? emb : null }],
                };
              } catch (nativeErr) {
                if (nativeErr?.name === "AbortError") throw nativeErr;
                console.warn(`[client-indexer] native vision fallback for ${item.name}:`, nativeErr);
              }
            }

            if (!data) {
              if (drive.isNative && typeof drive.getThumbnail === "function") {
                const bigUrl = await drive.getThumbnail(item.path, 1024, 1024);
                if (bigUrl) {
                  uploadBytes = await dataUrlToBytes(bigUrl);
                  const smallUrl = await drive.getThumbnail(item.path, 320, 320);
                  previewDataUrl = smallUrl || bigUrl;
                }
              }
              if (!uploadBytes) {
                if (!rawBytes) rawBytes = await readDriveFileBytes(drive, item.path, item.size);
                if (!exif.gpsString && !exif.takenAt) exif = extractJpegExif(rawBytes);
                const blob = new Blob([rawBytes]);
                const resized = await resizeBlobToJpeg(blob, 1024, 0.78);
                uploadBytes = new Uint8Array(await resized.blob.arrayBuffer());
                const thumb = await resizeBlobToJpeg(blob, 320, 0.72);
                previewDataUrl = thumb.dataUrl;
              }

              if (isLocalVisionEnabled()) {
                try {
                  const localRes = await recognizeImageLocal(uploadBytes, {
                    filename: item.name,
                    exif,
                    signal,
                  });
                  const [emb] = await embedTexts(httpDrive, [localRes.annotation], 1, signal);
                  data = {
                    title: localRes.title,
                    annotation: localRes.annotation,
                    tags: localRes.tags,
                    annotation_embedding: Array.isArray(emb) ? emb : null,
                    chunks: [{ text: localRes.annotation, embedding: Array.isArray(emb) ? emb : null }],
                  };
                } catch (localErr) {
                  if (localErr?.name === "AbortError") throw localErr;
                  console.warn(`[client-indexer] local vision fallback to node for ${item.name}:`, localErr);
                }
              }
            }

            if (!data) {
              const headers = {
                Accept: "application/json",
                "X-OMD-Filename": encodeURIComponent(item.name),
                "X-OMD-Source": encodeURIComponent(item.path),
                "X-OMD-Doc-Path": encodeURIComponent(item.docId),
                "X-OMD-RAG-Scope": "private",
                "X-OMD-Owner": owner,
                "X-OMD-Skip-Preview": "1",
              };
              if (contentHash) headers["X-OMD-Content-Hash"] = contentHash;
              if (exif.gpsString) headers["X-OMD-GPS"] = exif.gpsString;
              if (exif.takenAt) headers["X-OMD-Taken-At"] = exif.takenAt;

              const res = await httpDrive.fetch("/rag/recognize/raw", {
                method: "POST",
                headers,
                body: uploadBytes,
                signal,
              });
              if (!res.ok) throw new Error(`recognize HTTP ${res.status}`);
              data = await res.json();
            }

            await chatStore.addKnowledge({
              document_id: item.docId,
              title: data.title || item.name,
              text: data.annotation || "",
              tags: Array.isArray(data.tags) ? data.tags : ["photo"],
              collection: "user",
              content_hash: contentHash || data.content_hash || "",
              imagePreview: previewDataUrl || data.imagePreview || "",
              annotation_embedding: Array.isArray(data.annotation_embedding) ? data.annotation_embedding : null,
              chunks: Array.isArray(data.chunks) ? data.chunks : [],
              timestamp: item.mtimeMs || Date.now(),
            });
            if (contentHash) existingHashes.add(contentHash);
            manifest[item.docId] = { size: item.size, mtime: item.mtimeMs, hash: contentHash, at: Date.now() };
            saveManifest(manifest);
            indexed++;
          }
        }
      } catch (e) {
        if (e?.name === "AbortError") break;
        console.warn(`[client-indexer] failed ${item.name}:`, e);
        failed++;
      }

      processed++;
      const dt = performance.now() - t0;
      if (processed > 1) {
        avgMs = avgMs ? avgMs * 0.7 + dt * 0.3 : dt;
      }
      const remaining = Math.max(0, queue.length - processed);
      const etaSeconds = avgMs > 0 ? Math.round((remaining * avgMs) / 1000) : 0;

      indexerStore.update((s) => ({
        ...s,
        processed,
        indexed,
        skipped,
        failed,
        avgMsPerItem: Math.round(avgMs),
        etaSeconds,
      }));
    }

    if (!signal.aborted) {
      indexerStore.update((s) => ({
        ...s,
        status: "done",
        currentFile: "",
        etaSeconds: 0,
      }));
    }
  }
}

export const clientIndexer = new ClientIndexerRunner();

