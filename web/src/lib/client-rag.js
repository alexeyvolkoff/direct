/**
 * Client-side RAG primitives:
 * - Pure JS/WebAPI text extraction for TXT/MD/HTML/PDF/DOCX/ODT/EPUB
 * - Sliding-window text chunking
 * - Batch embedding via AI node /embed (with single-item fallback for older nodes)
 * - Chunk-level cosine similarity search over GunDB knowledge cards + chunks
 */

const PLAIN_TEXT_EXTS = new Set([
  "txt", "md", "markdown", "csv", "tsv", "json", "log", "rst", "ini", "conf",
  "yaml", "yml", "toml", "xml", "sql", "sh", "py", "js", "ts", "svelte", "css",
]);
const HTML_EXTS = new Set(["html", "htm", "xhtml"]);
const ZIP_DOC_EXTS = new Set(["docx", "odt", "epub"]);
const SUPPORTED_DOC_EXTS = new Set([
  ...PLAIN_TEXT_EXTS,
  ...HTML_EXTS,
  ...ZIP_DOC_EXTS,
  "pdf",
]);

export function getFileExt(filename = "") {
  const clean = String(filename || "").split("?")[0].split("#")[0];
  const dot = clean.lastIndexOf(".");
  return dot >= 0 ? clean.slice(dot + 1).toLowerCase() : "";
}

export function isSupportedDocumentExt(filename = "") {
  return SUPPORTED_DOC_EXTS.has(getFileExt(filename));
}

function stripHtmlToText(html) {
  if (!html) return "";
  try {
    const doc = new DOMParser().parseFromString(html, "text/html");
    doc.querySelectorAll("script, style, noscript").forEach((el) => el.remove());
    return (doc.body?.textContent || "").replace(/\s+\n/g, "\n").replace(/\n{3,}/g, "\n\n").trim();
  } catch (_) {
    return html.replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim();
  }
}

async function inflateRaw(compressedBytes) {
  if (typeof DecompressionStream === "undefined") {
    throw new Error("DecompressionStream unavailable");
  }
  const ds = new DecompressionStream("deflate-raw");
  const writer = ds.writable.getWriter();
  writer.write(compressedBytes);
  writer.close();
  const out = await new Response(ds.readable).arrayBuffer();
  return new Uint8Array(out);
}

/**
 * Minimal ZIP reader (Central Directory) supporting Stored (0) and Deflate (8).
 * Returns Map<entryName, () => Promise<Uint8Array>>.
 */
function parseZipEntries(u8) {
  const view = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  // Locate End of Central Directory (0x06054b50) scanning backwards
  let eocdOffset = -1;
  const minOffset = Math.max(0, u8.length - 65557);
  for (let i = u8.length - 22; i >= minOffset; i--) {
    if (view.getUint32(i, true) === 0x06054b50) {
      eocdOffset = i;
      break;
    }
  }
  if (eocdOffset < 0) throw new Error("Invalid ZIP archive");

  const totalEntries = view.getUint16(eocdOffset + 10, true);
  const cdOffset = view.getUint32(eocdOffset + 16, true);
  const decoder = new TextDecoder("utf-8");
  const entries = new Map();

  let ptr = cdOffset;
  for (let i = 0; i < totalEntries && ptr + 46 <= u8.length; i++) {
    if (view.getUint32(ptr, true) !== 0x02014b50) break;
    const method = view.getUint16(ptr + 10, true);
    const compSize = view.getUint32(ptr + 20, true);
    const nameLen = view.getUint16(ptr + 28, true);
    const extraLen = view.getUint16(ptr + 30, true);
    const commentLen = view.getUint16(ptr + 32, true);
    const localHeaderOffset = view.getUint32(ptr + 42, true);
    const name = decoder.decode(u8.subarray(ptr + 46, ptr + 46 + nameLen));
    ptr += 46 + nameLen + extraLen + commentLen;

    entries.set(name, async () => {
      if (localHeaderOffset + 30 > u8.length) return new Uint8Array(0);
      const lNameLen = view.getUint16(localHeaderOffset + 26, true);
      const lExtraLen = view.getUint16(localHeaderOffset + 28, true);
      const dataStart = localHeaderOffset + 30 + lNameLen + lExtraLen;
      const slice = u8.subarray(dataStart, dataStart + compSize);
      if (method === 0) return slice;
      if (method === 8) return await inflateRaw(slice);
      throw new Error(`Unsupported ZIP compression method ${method}`);
    });
  }
  return entries;
}

async function extractZipDocumentText(u8, ext) {
  const entries = parseZipEntries(u8);
  const decoder = new TextDecoder("utf-8");

  if (ext === "docx") {
    const reader = entries.get("word/document.xml");
    if (!reader) return "";
    const xml = decoder.decode(await reader());
    const doc = new DOMParser().parseFromString(xml, "application/xml");
    const paras = doc.getElementsByTagNameNS("*", "p");
    const lines = [];
    for (const p of paras) {
      const texts = p.getElementsByTagNameNS("*", "t");
      let line = "";
      for (const t of texts) line += t.textContent || "";
      if (line.trim()) lines.push(line.trim());
    }
    return lines.join("\n");
  }

  if (ext === "odt") {
    const reader = entries.get("content.xml");
    if (!reader) return "";
    const xml = decoder.decode(await reader());
    const doc = new DOMParser().parseFromString(xml, "application/xml");
    const nodes = doc.querySelectorAll("p, h");
    const lines = [];
    for (const n of nodes) {
      const t = (n.textContent || "").trim();
      if (t) lines.push(t);
    }
    return lines.join("\n");
  }

  if (ext === "epub") {
    const htmlKeys = Array.from(entries.keys())
      .filter((k) => /\.(xhtml|html|htm)$/i.test(k))
      .sort();
    const parts = [];
    for (const key of htmlKeys.slice(0, 120)) {
      const raw = decoder.decode(await entries.get(key)());
      const txt = stripHtmlToText(raw);
      if (txt) parts.push(txt);
    }
    return parts.join("\n\n");
  }

  return "";
}

let _pdfjsPromise = null;
async function getPdfJs() {
  if (!_pdfjsPromise) {
    _pdfjsPromise = (async () => {
      const [pdfjsLib, workerMod] = await Promise.all([
        import("pdfjs-dist"),
        import("pdfjs-dist/build/pdf.worker.min.mjs?url"),
      ]);
      if (pdfjsLib.GlobalWorkerOptions && workerMod?.default) {
        pdfjsLib.GlobalWorkerOptions.workerSrc = workerMod.default;
      }
      return pdfjsLib;
    })();
  }
  return _pdfjsPromise;
}

async function extractPdfText(u8, maxPages = 200) {
  const pdfjsLib = await getPdfJs();
  const loadingTask = pdfjsLib.getDocument({ data: u8, verbosity: 0 });
  const pdf = await loadingTask.promise;
  const pageCount = Math.min(pdf.numPages || 0, maxPages);
  const pages = [];
  for (let p = 1; p <= pageCount; p++) {
    const page = await pdf.getPage(p);
    const content = await page.getTextContent();
    const str = (content.items || []).map((it) => it.str || "").join(" ").replace(/\s+/g, " ").trim();
    if (str) pages.push(str);
  }
  try { await pdf.destroy(); } catch (_) {}
  return pages.join("\n\n");
}

/**
 * Extract plain text from file bytes purely on the client.
 */
export async function extractTextFromBytes(bytes, filename = "") {
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes || 0);
  if (!u8.length) return "";
  const ext = getFileExt(filename);

  if (PLAIN_TEXT_EXTS.has(ext)) {
    return new TextDecoder("utf-8", { fatal: false }).decode(u8).trim();
  }
  if (HTML_EXTS.has(ext)) {
    const raw = new TextDecoder("utf-8", { fatal: false }).decode(u8);
    return stripHtmlToText(raw);
  }
  if (ext === "pdf") {
    return await extractPdfText(u8);
  }
  if (ZIP_DOC_EXTS.has(ext)) {
    return await extractZipDocumentText(u8, ext);
  }
  return "";
}

/**
 * Split document text into overlapping chunks suitable for embeddings.
 */
export function chunkText(text, maxChars = 900, overlap = 150) {
  const clean = String(text || "").replace(/\r\n/g, "\n").replace(/\n{3,}/g, "\n\n").trim();
  if (!clean) return [];
  if (clean.length <= maxChars) return [clean];

  const chunks = [];
  let start = 0;
  while (start < clean.length) {
    let end = Math.min(clean.length, start + maxChars);
    if (end < clean.length) {
      // Prefer breaking on paragraph or sentence boundary
      const paraBreak = clean.lastIndexOf("\n\n", end);
      const lineBreak = clean.lastIndexOf("\n", end);
      const sentBreak = clean.lastIndexOf(". ", end);
      const minAcceptable = start + Math.floor(maxChars * 0.45);
      if (paraBreak >= minAcceptable) end = paraBreak;
      else if (lineBreak >= minAcceptable) end = lineBreak;
      else if (sentBreak >= minAcceptable) end = sentBreak + 1;
    }
    const piece = clean.slice(start, end).trim();
    if (piece) chunks.push(piece);
    if (end >= clean.length) break;
    start = Math.max(start + 1, end - overlap);
  }
  return chunks;
}

/**
 * Embed an array of texts via AI node /embed (batched, with single-text fallback).
 */
export async function embedTexts(httpDrive, texts, batchSize = 24, signal = null) {
  if (!httpDrive || !Array.isArray(texts) || texts.length === 0) return [];
  const results = new Array(texts.length).fill(null);

  for (let i = 0; i < texts.length; i += batchSize) {
    if (signal?.aborted) throw new DOMException("Aborted", "AbortError");
    const batch = texts.slice(i, i + batchSize).map((t) => String(t || "").trim() || " ");

    let batchOk = false;
    try {
      const res = await httpDrive.fetch("/embed", {
        method: "POST",
        headers: { "Content-Type": "application/json", Accept: "application/json" },
        body: JSON.stringify({ texts: batch }),
        ...(signal ? { signal } : {}),
      });
      if (res.ok) {
        const data = await res.json();
        if (Array.isArray(data?.embeddings) && data.embeddings.length === batch.length) {
          for (let j = 0; j < batch.length; j++) {
            results[i + j] = data.embeddings[j];
          }
          batchOk = true;
        }
      }
    } catch (e) {
      if (e?.name === "AbortError") throw e;
    }

    if (!batchOk) {
      // Fallback for older AI nodes supporting only { text }
      for (let j = 0; j < batch.length; j++) {
        if (signal?.aborted) throw new DOMException("Aborted", "AbortError");
        const res = await httpDrive.fetch("/embed", {
          method: "POST",
          headers: { "Content-Type": "application/json", Accept: "application/json" },
          body: JSON.stringify({ text: batch[j] }),
          ...(signal ? { signal } : {}),
        });
        if (res.ok) {
          const data = await res.json();
          if (Array.isArray(data?.embedding)) {
            results[i + j] = data.embedding;
          }
        }
      }
    }
  }
  return results;
}

export function cosineSimilarity(a, b) {
  if (!a || !b || a.length !== b.length || a.length === 0) return 0;
  let dot = 0, normA = 0, normB = 0;
  for (let i = 0; i < a.length; i++) {
    dot += a[i] * b[i];
    normA += a[i] * a[i];
    normB += b[i] * b[i];
  }
  const denom = Math.sqrt(normA) * Math.sqrt(normB);
  return denom === 0 ? 0 : dot / denom;
}

// In-memory cache of GunDB chunks per memoryId so repeat queries don't re-read GunDB
const _chunksCache = new Map(); // memoryId -> { updated, chunks }

export function invalidateClientChunksCache(memoryId = null) {
  if (memoryId) _chunksCache.delete(memoryId);
  else _chunksCache.clear();
}

async function getCachedCardChunks(chatStore, card) {
  const mid = card._id || card.memory_id || card.id;
  if (!mid || !card.chunk_count) return [];
  const cached = _chunksCache.get(mid);
  if (cached && cached.updated === (card.updated || "")) {
    return cached.chunks;
  }
  try {
    const chunks = await chatStore.getChunks(mid);
    if (Array.isArray(chunks)) {
      _chunksCache.set(mid, { updated: card.updated || "", chunks });
      return chunks;
    }
  } catch (_) {}
  return [];
}

/**
 * Two-stage client-side similarity search:
 * 1) Rank cards by annotation_embedding (always in memory)
 * 2) Load chunks for top candidate cards (and any cards with chunk_count > 0)
 *    and return the best matching chunks + card metadata for /chat/stream.
 */
export async function searchClientKnowledgeChunks(chatStore, queryEmb, opts = {}) {
  if (!chatStore || !Array.isArray(queryEmb) || queryEmb.length === 0) return [];

  const threshold = opts.threshold ?? 0.25;
  const relativeRatio = opts.relativeRatio ?? 0.55;
  const maxCards = opts.maxCards ?? 4;
  const maxChunksPerCard = opts.maxChunksPerCard ?? 2;
  const maxTotal = opts.maxTotal ?? 5;

  const allCards = chatStore.listKnowledge();
  const withEmb = allCards.filter(
    (m) => Array.isArray(m.annotation_embedding) && m.annotation_embedding.length > 0
  );
  if (withEmb.length === 0) return [];

  const cardScored = withEmb
    .map((card) => ({
      card,
      score: cosineSimilarity(queryEmb, card.annotation_embedding),
    }))
    .sort((a, b) => b.score - a.score);

  const bestCardScore = cardScored[0]?.score || 0;
  // Use slightly looser pre-filter for candidate cards so deep chunks inside a doc aren't missed
  const preCutoff = Math.max(threshold * 0.8, bestCardScore * 0.45);
  const candidates = cardScored
    .filter((x) => x.score >= preCutoff)
    .slice(0, maxCards * 2);

  const chunkHits = [];
  for (const { card, score: cardScore } of candidates) {
    const mid = card._id || card.memory_id || card.id || "";
    const baseMeta = {
      id: mid,
      title: card.title || "",
      document_id: card.document_id || "",
      tags: Array.isArray(card.tags) ? card.tags : [],
    };

    const chunks = await getCachedCardChunks(chatStore, card);
    const validChunks = chunks
      .filter((c) => c && c.text && Array.isArray(c.embedding) && c.embedding.length > 0)
      .map((c) => ({
        ...baseMeta,
        text: c.text,
        score: cosineSimilarity(queryEmb, c.embedding),
      }))
      .sort((a, b) => b.score - a.score)
      .slice(0, maxChunksPerCard);

    if (validChunks.length > 0) {
      // Also include card annotation if its score beats chunks or if it's an image/short card
      const topChunkScore = validChunks[0].score;
      if (cardScore > topChunkScore && (card.text || "").trim()) {
        chunkHits.push({ ...baseMeta, text: card.text, score: cardScore });
      }
      chunkHits.push(...validChunks);
    } else if ((card.text || "").trim()) {
      chunkHits.push({ ...baseMeta, text: card.text, score: cardScore });
    }
  }

  chunkHits.sort((a, b) => b.score - a.score);
  const best = chunkHits[0]?.score || 0;
  const cutoff = Math.max(threshold, best * relativeRatio);
  return chunkHits.filter((h) => h.score >= cutoff).slice(0, maxTotal);
}

