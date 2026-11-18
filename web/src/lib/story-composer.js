/**
 * Shared "Share a Story" & Illustrated Comic Folder Engine.
 *
 * - Detects story / long Readme.md folders across File Manager, Creations, and Public Shares.
 * - Copies selected media (+ companion sidecars) into a story subfolder.
 * - Gathers all per-photo facts (people, landmarks, events, places, dates, companion Readme.md).
 * - Calls the current AI node LLM to compose an illustrated Markdown mini-comic (Readme.md).
 */

import { get } from "svelte/store";
import { deviceRegistry } from "./device-registry.js";
import { aiNodeStore, aiOnmychatHeaders, getClientHordeSettings } from "./ai-node.js";
import { getStoredOmdKey } from "./omd-key.js";
import { hasSensitiveReadmeContent, hasSensitiveTag } from "./content-mode.js";
import {
  extractFilenameTimestamp,
  parseExifOrIsoTimestamp,
  parseReadmeSidecarText,
  extractCardLandmarkAndEvent,
} from "./media-albums.js";

const VIDEO_EXT_RE = /\.(mp4|webm|mov|m4v|mkv|avi)$/i;
const IMAGE_EXT_RE = /\.(jpe?g|png|webp|gif|avif|bmp)$/i;

// ---------------------------------------------------------------------------
// 1. Story / Long Readme Detection & Metadata Parsing
// ---------------------------------------------------------------------------

/**
 * Returns true if a folder's Readme.md should render as an illustrated Comic / Story header:
 * - Any Readme.md with embedded Markdown media `![caption](file.jpg)` or local media links, OR
 * - Any "long Readme.md" (>= 180 chars of prose or >= 2 headings / >= 3 paragraphs).
 */
export function isStoryOrLongReadme(rawMd = "") {
  const txt = String(rawMd || "").replace(/<!--[\s\S]*?-->/g, "").trim();
  if (!txt) return false;

  // Embedded markdown image or video
  if (/!\[[^\]]*\]\([^)]+\)/.test(txt)) return true;
  // Markdown link pointing to a local media file in the folder
  if (/\[[^\]]+\]\(\.?\/?[^)\s]+\.(?:jpe?g|png|webp|gif|mp4|webm|mov|m4v)\)/i.test(txt)) {
    return true;
  }

  // Strip standalone #hashtag lines at bottom when measuring prose length
  const proseLines = txt
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l && !/^(?:#[a-zA-Zа-яА-ЯёЁіїєґІЇЄҐ0-9_-]+\s*)+$/.test(l));

  const headingCount = proseLines.filter((l) => /^#{1,3}\s+\S/.test(l)).length;
  const proseChars = proseLines.join(" ").replace(/^#+\s+/gm, "").trim().length;

  return proseChars >= 180 || headingCount >= 2 || proseLines.length >= 4;
}

/**
 * Parses Story Readme.md into structured title, clean prose detail (e.g. for CivitAI post),
 * and ordered list of referenced filenames.
 */
export function parseStoryReadmeMeta(rawMd = "") {
  const raw = String(rawMd || "");
  const withoutComments = raw.replace(/<!--[\s\S]*?-->/g, "").trim();
  let title = "";
  const mTitle = withoutComments.match(/^#\s+([^\n]+)/m);
  if (mTitle) {
    title = mTitle[1].trim();
  }

  const referencedFiles = [];
  const seenFiles = new Set();
  const embedRe = /!?\[[^\]]*\]\(([^)]+)\)/g;
  let m;
  while ((m = embedRe.exec(withoutComments)) !== null) {
    const rawHref = m[1].trim().split(/\s+/)[0].replace(/^\.\//, "").replace(/^\/+/, "");
    const base = decodeURIComponent(rawHref.split("?")[0].split("#")[0]).split("/").pop() || "";
    if (base && (IMAGE_EXT_RE.test(base) || VIDEO_EXT_RE.test(base))) {
      const low = base.toLowerCase();
      if (!seenFiles.has(low)) {
        seenFiles.add(low);
        referencedFiles.push(base);
      }
    }
  }

  const cleanDetail = withoutComments
    .replace(/^#\s+[^\n]+\n*/m, "")
    .replace(/!\[[^\]]*\]\([^)]+\)/g, "")
    .replace(/(?:^|\s)#(?:sensitive|nsfw)\b/gi, "")
    .replace(/\n{3,}/g, "\n\n")
    .trim();

  const isSensitive = Boolean(hasSensitiveTag(raw) || hasSensitiveReadmeContent(raw));

  return {
    title,
    cleanDetail,
    referencedFiles,
    isSensitive,
  };
}

// ---------------------------------------------------------------------------
// 2. Folder Name Sanitizer & Auto-Suggestion
// ---------------------------------------------------------------------------

export function sanitizeStoryFolderName(raw = "") {
  const cleaned = String(raw || "")
    .replace(/[\u{1F300}-\u{1FAFF}\u{2600}-\u{27BF}✨🏛️📍🎛️🎬🗓️🏷️👥🎭📖]/gu, "")
    .replace(/[<>:"/\\|?*\x00-\x1F]+/g, " ")
    .replace(/[·•.,;!?()[\]{}'"`~@#$%^&+=]+/g, " ")
    .trim()
    .replace(/\s+/g, "_")
    .replace(/_+/g, "_")
    .replace(/^[-_.]+|[-_.]+$/g, "");
  return (cleaned || `Story_${new Date().toISOString().slice(0, 10)}`).slice(0, 48);
}

export function suggestStoryFolderName({ albumTitle = "", items = [], isCreations = false } = {}) {
  if (albumTitle) {
    const fromAlbum = sanitizeStoryFolderName(albumTitle);
    if (fromAlbum && fromAlbum.length >= 2) {
      return isCreations ? fromAlbum.toLowerCase() : fromAlbum;
    }
  }

  // Synthesize from top landmark / event / city / persona + date
  let lead = "";
  let earliestMs = 0;
  for (const it of items || []) {
    if (!lead) {
      lead =
        it.landmark ||
        it.namedEvent ||
        it.event ||
        it.storyFolder ||
        it.city ||
        (Array.isArray(it.people) && it.people[0]) ||
        it.persona ||
        "";
    }
    const ts = Number(it.takenMs || it.date || 0);
    if (ts > 0 && (!earliestMs || ts < earliestMs)) {
      earliestMs = ts;
    }
  }

  const dateSlug = earliestMs > 0
    ? new Date(earliestMs).toISOString().slice(0, 10)
    : new Date().toISOString().slice(0, 10);

  const combined = lead ? `${lead}_${dateSlug}` : `Story_${dateSlug}`;
  const clean = sanitizeStoryFolderName(combined);
  return isCreations ? clean.toLowerCase() : clean;
}

// ---------------------------------------------------------------------------
// 3. Cached Folder Readme.md Loader (File Manager / Cards / Table / CameraRoll)
// ---------------------------------------------------------------------------

const _folderReadmeCache = new Map();

export function invalidateFolderReadmeCache(folderId = "") {
  if (!folderId) {
    _folderReadmeCache.clear();
    return;
  }
  const norm = String(folderId).replace(/\/+$/, "");
  for (const key of Array.from(_folderReadmeCache.keys())) {
    if (key === norm || key.startsWith(`${norm}::`)) {
      _folderReadmeCache.delete(key);
    }
  }
}

export function splitPrefixedFolderPath(fullId = "") {
  const clean = String(fullId || "").trim();
  if (!clean || clean === "/") return { prefix: "", realPath: "/" };
  const parts = clean.replace(/^\/+/, "").split("/");
  const prefix = parts[0] || "";
  const dev =
    deviceRegistry.getByPrefix(prefix) ||
    deviceRegistry.get(prefix) ||
    deviceRegistry.getByName(prefix) ||
    null;
  if (dev) {
    return {
      prefix: (dev.prefix || prefix).replace(/^\/+|\/+$/g, ""),
      device: dev,
      realPath: parts.length > 1 ? "/" + parts.slice(1).join("/") : "/",
    };
  }
  return { prefix, device: null, realPath: clean.startsWith("/") ? clean : "/" + clean };
}

export function buildMarkdownFromStoryJson(storyData = null) {
  if (!storyData || typeof storyData !== "object") return "";
  const title = String(storyData.story_title || storyData.title || storyData.story_slug || "Story").trim();
  const setting = String(storyData.setting || "").trim();
  const scenes = Array.isArray(storyData.scenes) ? storyData.scenes : [];
  if (!scenes.length) return "";

  const lines = [`# ${title}`, ""];
  if (setting) {
    lines.push(`> ${setting}`, "");
  }
  lines.push("---", "");

  let anySensitive = hasSensitiveReadmeContent(setting);
  for (let i = 0; i < scenes.length; i++) {
    const sc = scenes[i];
    if (!sc || typeof sc !== "object") continue;
    const num = sc.scene_number || i + 1;
    const scTitle = String(sc.title || `Scene ${num}`).trim();
    const narrative = String(sc.narrative || "").trim();
    const imgName = String(sc.image_name || "").trim();
    const visPrompt = String(sc.visual_prompt || sc.prompt || "").trim();
    if (hasSensitiveReadmeContent(visPrompt) || hasSensitiveReadmeContent(narrative)) {
      anySensitive = true;
    }
    lines.push(`## ${num}. ${scTitle}`, "");
    if (imgName) {
      lines.push(`![${scTitle}](./${imgName})`, "");
    }
    if (narrative) {
      lines.push(narrative, "");
    }
    if (visPrompt) {
      lines.push(`<!-- scene_${num}_prompt: ${visPrompt} -->`);
    }
    lines.push("---", "");
  }
  if (anySensitive) {
    lines.push("#sensitive");
  }
  return lines.join("\n").trim() + "\n";
}

export async function readTextFromDrive(drive, realPath, p2pFallbackUrl = "") {
  if (drive) {
    try {
      if (typeof drive.readFile === "function") {
        const res = await drive.readFile(realPath, 0, 262144).catch(() => null);
        const b64 = res && res.data ? res.data : (typeof res === "string" ? res : null);
        if (b64) {
          const bin = atob(b64.replace(/\s+/g, ""));
          const u8 = new Uint8Array(bin.length);
          for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
          return new TextDecoder("utf-8").decode(u8);
        }
      }
      if (typeof drive.getFileContent === "function") {
        const blob = await drive.getFileContent(realPath).catch(() => null);
        if (blob && blob.size > 0) {
          return await blob.text();
        }
      }
      if (typeof drive.homeRead === "function") {
        const b64 = await drive.homeRead(realPath).catch(() => null);
        if (b64) {
          const bin = atob(b64.replace(/\s+/g, ""));
          const u8 = new Uint8Array(bin.length);
          for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
          return new TextDecoder("utf-8").decode(u8);
        }
      }
    } catch (_) {}
  }
  if (p2pFallbackUrl && typeof fetch === "function") {
    try {
      const resp = await fetch(p2pFallbackUrl);
      if (resp.ok) {
        const ct = (resp.headers.get("content-type") || "").toLowerCase();
        if (!ct.includes("text/html")) {
          return await resp.text();
        }
      }
    } catch (_) {}
  }
  return "";
}

/**
 * Checks if `folderId` contains `Readme.md` / `README.md` (or `story.json`) and loads its comic text.
 * Works seamlessly even when `webrtc-drive.getFiles()` filters `Readme.md` out of the visible `files` list.
 */
export async function loadFolderStoryReadme(folderId, files = []) {
  if (!folderId || folderId === "/" || folderId.startsWith("/shared-by-me") || !Array.isArray(files)) {
    return "";
  }
  const normFolder = String(folderId).replace(/\/+$/, "");
  const trackedMeta =
    typeof window !== "undefined" && window._folderReadmeMeta
      ? window._folderReadmeMeta[normFolder] || null
      : null;

  const inlineEntry =
    trackedMeta?.readmeFile ||
    files.readmeFile ||
    files.find((f) => {
      if (!f || f.isDir || f.type === "folder") return false;
      return String(f.name || f.value || "").trim().toLowerCase() === "readme.md";
    }) ||
    null;

  const storyJsonEntry = trackedMeta?.storyJsonFile || files.storyJsonFile || null;

  // Fast skip on WebRTC folders confirmed to have neither Readme.md nor story.json
  if (trackedMeta && trackedMeta.isWebRtc && !inlineEntry && !storyJsonEntry) {
    return "";
  }

  if (files.length === 0 && !inlineEntry && !storyJsonEntry) {
    return "";
  }

  const stamp = `${files.length}_${inlineEntry?.size || 0}_${storyJsonEntry?.size || 0}`;
  const cacheKey = `${normFolder}::${stamp}`;
  if (_folderReadmeCache.has(cacheKey)) {
    return _folderReadmeCache.get(cacheKey);
  }

  const split = splitPrefixedFolderPath(normFolder);
  const drive = trackedMeta?.drive || split.device?.drive || null;
  const prefix = trackedMeta?.prefix || split.prefix || "";
  const realPath = trackedMeta?.realPath || split.realPath || "/";
  if (!drive && !prefix) return "";

  const baseDir = realPath === "/" ? "" : realPath;
  const readmeName = inlineEntry?.name || inlineEntry?.value || "Readme.md";
  const readmeRealPath = `${baseDir}/${readmeName}`;
  const p2pUrl = prefix && !split.device?.local ? `/p2p/${prefix}${encodeURI(readmeRealPath)}` : "";

  let text = await readTextFromDrive(drive, readmeRealPath, p2pUrl);
  if (!text && readmeName !== "README.md" && !inlineEntry) {
    const upperPath = `${baseDir}/README.md`;
    const upperP2p = prefix && !split.device?.local ? `/p2p/${prefix}${encodeURI(upperPath)}` : "";
    text = await readTextFromDrive(drive, upperPath, upperP2p);
  }

  if (!text && (storyJsonEntry || /\/(generated|creations|onmychat)(\/|$)/i.test(normFolder))) {
    const jsonPath = `${baseDir}/story.json`;
    const jsonP2p = prefix && !split.device?.local ? `/p2p/${prefix}${encodeURI(jsonPath)}` : "";
    const rawJson = await readTextFromDrive(drive, jsonPath, jsonP2p);
    if (rawJson) {
      try {
        text = buildMarkdownFromStoryJson(JSON.parse(rawJson));
      } catch (_) {}
    }
  }

  _folderReadmeCache.set(cacheKey, text || "");
  return text || "";
}

const _localMediaBlobUrlCache = new Map();

/**
 * Resolves a relative filename referenced in a folder's Readme.md into a renderable URL.
 * Supports P2P drives, shared folders (via SVAR `templates.preview`), and local browser/mobile drives.
 */
export function resolveFolderMediaAssetUrl(
  folderId,
  files = [],
  relOrName = "",
  { forPoster = false, templates = null, onAsyncReady = null } = {}
) {
  const cleanRel = String(relOrName || "").replace(/^\.\//, "").replace(/^\/+/, "").trim();
  if (!cleanRel) return "";
  const targetName = (cleanRel.split("/").pop() || cleanRel).toLowerCase();

  const matchedFile = (files || []).find(
    (f) => String(f?.name || f?.value || "").toLowerCase() === targetName
  );
  const isVid = VIDEO_EXT_RE.test(targetName);

  // Video poster: delegate to templates.preview which generates ?cover=...
  if (forPoster) {
    if (matchedFile && templates?.preview) {
      return templates.preview(matchedFile, 960, 720) || "";
    }
    return "";
  }

  const normFolder = String(folderId || "").replace(/\/+$/, "");
  const trackedMeta =
    typeof window !== "undefined" && window._folderReadmeMeta
      ? window._folderReadmeMeta[normFolder] || null
      : null;
  const split = splitPrefixedFolderPath(normFolder);
  const prefix = trackedMeta?.prefix || split.prefix || "";
  const device = split.device;
  const realPath = trackedMeta?.realPath || split.realPath || "/";
  const exactName = matchedFile ? (matchedFile.name || matchedFile.value || cleanRel) : cleanRel;
  const fullRealPath = `${realPath === "/" ? "" : realPath}/${exactName}`;

  // Remote P2P device (including /shared-to-me via trackedMeta): direct /p2p URL streams videos and renders crisp images
  if (prefix && !device?.local && (!normFolder.startsWith("/shared-") || Boolean(trackedMeta?.prefix))) {
    const baseP2p = `/p2p/${prefix}${encodeURI(fullRealPath)}`;
    if (isVid) return baseP2p;
    if (matchedFile && templates?.preview) {
      return templates.preview(matchedFile, 1440, 1440) || baseP2p;
    }
    return baseP2p;
  }

  // Shared-to-me / Shared-by-me or custom preview handler
  if (!isVid && matchedFile && templates?.preview) {
    const prev = templates.preview(matchedFile, 1440, 1440);
    if (prev) return prev;
  }

  // Local device drive: load Blob once and cache object URL
  if (device?.drive && typeof device.drive.getFileContent === "function") {
    const cacheKey = `${normFolder}::${exactName}`;
    if (_localMediaBlobUrlCache.has(cacheKey)) {
      return _localMediaBlobUrlCache.get(cacheKey);
    }
    _localMediaBlobUrlCache.set(cacheKey, "");
    device.drive
      .getFileContent(fullRealPath)
      .then((blob) => {
        if (blob && blob.size > 0 && typeof URL !== "undefined") {
          const u = URL.createObjectURL(blob);
          _localMediaBlobUrlCache.set(cacheKey, u);
          if (typeof onAsyncReady === "function") onAsyncReady();
        }
      })
      .catch(() => {});
  }

  return "";
}

// ---------------------------------------------------------------------------
// 4. Drive File Operations (MkDir, Copy, Read, Write)
// ---------------------------------------------------------------------------

async function ensureDirRecursiveOnDrive(drive, folderPath) {
  if (!drive || !folderPath || folderPath === "/") return;
  const parts = String(folderPath).split("/").filter(Boolean);
  let cur = "";
  for (const seg of parts) {
    cur += "/" + seg;
    try {
      if (typeof drive.homeMkDir === "function") {
        await drive.homeMkDir(cur);
      } else if (typeof drive.mkDir === "function") {
        await drive.mkDir(cur);
      } else if (typeof drive.createFolder === "function") {
        const parent = cur.slice(0, cur.lastIndexOf("/")) || "/";
        await drive.createFolder(parent, seg);
      }
    } catch (_) {
      // Already exists
    }
  }
}

function uint8ToBase64(u8) {
  let binary = "";
  const chunk = 0x8000;
  for (let i = 0; i < u8.length; i += chunk) {
    binary += String.fromCharCode.apply(null, u8.subarray(i, i + chunk));
  }
  return btoa(binary);
}

async function blobToBase64(blob) {
  const buf = await blob.arrayBuffer();
  return uint8ToBase64(new Uint8Array(buf));
}

async function writeBase64ToDrive(drive, filePath, b64Data) {
  if (!drive) throw new Error("Storage drive is not connected");
  if (typeof drive.homeWrite === "function") {
    await drive.homeWrite(filePath, b64Data);
    return;
  }
  const bin = atob(b64Data);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  if (typeof drive.writeFile === "function") {
    await drive.writeFile(filePath, bytes);
    return;
  }
  throw new Error("Drive does not support writing files");
}

async function writeUtf8ToDrive(drive, filePath, text) {
  const u8 = new TextEncoder().encode(String(text || ""));
  await writeBase64ToDrive(drive, filePath, uint8ToBase64(u8));
}

async function fetchSourceItemBlob(item) {
  if (item.blob instanceof Blob && item.blob.size > 0) return item.blob;
  if (item.imageBlob instanceof Blob && item.imageBlob.size > 0) return item.imageBlob;

  if (item.blobUrl && typeof fetch === "function") {
    try {
      const r = await fetch(item.blobUrl);
      if (r.ok) {
        const b = await r.blob();
        if (b.size > 0) return b;
      }
    } catch (_) {}
  }

  if (item.srcDrive && item.srcRealPath && typeof item.srcDrive.getFileContent === "function") {
    try {
      const b = await item.srcDrive.getFileContent(item.srcRealPath);
      if (b && b.size > 0) return b;
    } catch (_) {}
  }

  if (item.p2pUrl && typeof fetch === "function") {
    try {
      const r = await fetch(item.p2pUrl);
      if (r.ok) {
        const b = await r.blob();
        if (b.size > 0) return b;
      }
    } catch (_) {}
  }

  if (item.inlinePreview && item.inlinePreview.startsWith("data:") && typeof fetch === "function") {
    try {
      const r = await fetch(item.inlinePreview);
      if (r.ok) return await r.blob();
    } catch (_) {}
  }

  return null;
}

// ---------------------------------------------------------------------------
// 5. AI Node LLM Story Composition (Live /story/compose + /chat Fallback)
// ---------------------------------------------------------------------------

function buildStoryAuthHeaders(omdKey = "", httpDrive = null) {
  const h = {
    "Content-Type": "application/json",
    "X-Client": "svar",
  };
  const tok = omdKey || getStoredOmdKey() || (typeof window !== "undefined" && window.__omdKey) || "";
  if (tok) {
    h["Token"] = tok;
    h["X-OMD-Token"] = tok;
  }
  const didPub =
    httpDrive?.didPub ||
    (typeof window !== "undefined" && (window.identity?.mainPub || window.identity?.pair?.pub)) ||
    "";
  if (didPub) {
    h["X-OMD-Caller-DID"] = didPub;
  }
  if (httpDrive && httpDrive.isPrivateMode !== undefined) {
    h["X-OMD-Private-Mode"] = httpDrive.isPrivateMode ? "1" : "0";
  }
  try {
    Object.assign(h, aiOnmychatHeaders(get(aiNodeStore)));
  } catch (_) {}
  return h;
}

function buildStoryCompositionPrompt({ plotHint = "", albumTitle = "", frames = [], localeCode = "en" }) {
  const hint = String(plotHint || "").trim();
  const hasCyrillic = /[а-яА-ЯёЁіїєґІЇЄҐ]/.test(`${hint} ${albumTitle}`);
  const targetLang = hint
    ? "the exact same language as the user's Plot Hint"
    : hasCyrillic || ["ru", "uk", "sr"].includes(String(localeCode || "").toLowerCase())
      ? (localeCode === "uk" ? "Ukrainian" : "Russian")
      : (localeCode || "English");

  const frameLines = frames.map((f, idx) => {
    const meta = [];
    meta.push(`File: ./${f.filename} (${f.isVideo ? "VIDEO" : "IMAGE"})`);
    if (f.dateLabel) meta.push(`Date: ${f.dateLabel}`);
    if (f.people && f.people.length) meta.push(`People/Characters: ${f.people.join(", ")}`);
    if (f.landmark) meta.push(`Landmark: ${f.landmark}`);
    if (f.event) meta.push(`Event: ${f.event}`);
    if (f.place) meta.push(`Location: ${f.place}`);
    if (f.tags && f.tags.length) meta.push(`Themes: ${f.tags.join(", ")}`);
    if (f.description) meta.push(`Visual/Companion notes: ${f.description.slice(0, 420)}`);
    return `Frame ${idx + 1}: ${meta.join(" | ")}`;
  }).join("\n");

  const videoFiles = frames.filter((f) => f.isVideo).map((f) => `./${f.filename}`);

  return [
    "You are a master visual storyteller and graphic-novel / photo-story author.",
    `Write an engaging illustrated story in Markdown (Readme.md) in ${targetLang}.`,
    "",
    albumTitle ? `Collection / Album context: ${albumTitle}` : "",
    hint
      ? `User's Story Plot Hint (central theme & tone): ${hint}`
      : "User's Story Plot Hint: Create a vivid, warm, cohesive narrative connecting these moments naturally.",
    "",
    "Available Media Frames (in chronological/narrative order):",
    frameLines,
    "",
    "STRICT FORMATTING RULES:",
    "1. Start with `# <Evocative Story Title>` on the first line, followed by a short atmospheric `> <tagline>`.",
    "2. Structure the story like an illustrated mini-comic / visual essay: weave narrative chapters or beats with embedded media panels using EXACT Markdown syntax `![<Short scene caption>](./<exact_filename>)` on its own line.",
    "3. Use ONLY the exact `./<filename>` paths provided in the Available Media Frames list above. Never invent filenames or external URLs.",
    frames.length <= 14
      ? "4. Embed EVERY provided media frame at the right narrative beat so each picture illustrates its part of the story."
      : "4. Embed the most expressive 8 to 14 key frames across the story beats using exact `./<filename>` paths so the comic flows dynamically.",
    videoFiles.length > 0
      ? `5. FINALE VIDEO: Place video file(s) (${videoFiles.join(", ")}) as the dramatic culmination / finale at the end of the story using \`![Finale](${videoFiles[videoFiles.length - 1]})\`.`
      : "5. End with a memorable closing line.",
    "6. Ground the story in the real facts from the frames (recognized people/characters, landmarks, events, locations, and visual details) guided by the user's Plot Hint.",
    "7. Output ONLY the raw Markdown document itself, without ```markdown code fences and without meta-commentary.",
  ]
    .filter(Boolean)
    .join("\n");
}

function postProcessStoryMarkdown(rawOutput = "", orderedFrames = [], fallbackTitle = "Story") {
  let md = String(rawOutput || "").trim();

  // Strip outer ```markdown ... ``` fence if the LLM added one
  const fenceMatch = md.match(/^```(?:markdown|md)?\s*\n([\s\S]*?)\n```\s*$/i);
  if (fenceMatch) {
    md = fenceMatch[1].trim();
  }

  // Build case-insensitive map of actual filenames in the folder
  const validByLower = new Map();
  for (const f of orderedFrames) {
    validByLower.set(f.filename.toLowerCase(), f.filename);
  }

  const embeddedLowers = new Set();

  // Normalize ![alt](path) so relative filenames match exact copied filenames
  md = md.replace(/!\[([^\]]*)\]\(([^)]+)\)/g, (full, alt, rawHref) => {
    const cleanHref = rawHref.trim().split(/\s+/)[0].replace(/^\.\//, "").replace(/^\/+/, "");
    const base = decodeURIComponent(cleanHref.split("?")[0].split("#")[0]).split("/").pop() || "";
    const matched = validByLower.get(base.toLowerCase());
    if (matched) {
      embeddedLowers.add(matched.toLowerCase());
      return `![${alt.trim() || matched}](./${matched})`;
    }
    return full;
  });

  // Ensure a top-level '# Title' exists
  if (!/^#\s+\S/m.test(md)) {
    md = `# ${fallbackTitle}\n\n${md}`;
  }

  // Fallback: if LLM returned plain prose without embedding any images, interleave frames as comic panels!
  if (embeddedLowers.size === 0 && orderedFrames.length > 0) {
    const lines = md.split(/\r?\n/);
    const titleLine = lines.find((l) => /^#\s+/.test(l)) || `# ${fallbackTitle}`;
    const paragraphs = md
      .replace(/^#\s+[^\n]+\n*/m, "")
      .split(/\n{2,}/)
      .map((p) => p.trim())
      .filter(Boolean);

    const keyFrames = orderedFrames.slice(0, 12);
    const outBlocks = [titleLine, ""];
    const maxLen = Math.max(paragraphs.length, keyFrames.length);
    for (let i = 0; i < maxLen; i++) {
      if (keyFrames[i]) {
        const kf = keyFrames[i];
        outBlocks.push(`![${kf.landmark || kf.event || kf.title || kf.filename}](./${kf.filename})`);
        outBlocks.push("");
        embeddedLowers.add(kf.filename.toLowerCase());
      }
      if (paragraphs[i]) {
        outBlocks.push(paragraphs[i]);
        outBlocks.push("");
      }
    }
    md = outBlocks.join("\n").trim();
  } else {
    // Ensure any video finale in orderedFrames is included at the end if the LLM missed it
    const videos = orderedFrames.filter((f) => f.isVideo && !embeddedLowers.has(f.filename.toLowerCase()));
    for (const v of videos) {
      md += `\n\n---\n\n![Finale](./${v.filename})\n`;
    }
  }

  return md.trim() + "\n";
}

async function callAiNodeStoryLlm({
  httpDrive = null,
  omdKey = "",
  plotHint = "",
  albumTitle = "",
  frames = [],
  localeCode = "en",
  signal = null,
}) {
  const fetchFn =
    httpDrive && typeof httpDrive.fetch === "function"
      ? (p, opts) => httpDrive.fetch(p, opts)
      : (p, opts) => fetch(p, opts);

  const headers = buildStoryAuthHeaders(omdKey, httpDrive);
  const clientSettings = getClientHordeSettings({ language: localeCode || "en" });
  const keyVal = omdKey || getStoredOmdKey() || "";

  // 1. Try dedicated /story/compose endpoint first
  try {
    const composeResp = await fetchFn("/story/compose", {
      method: "POST",
      headers,
      body: JSON.stringify({
        omd_key: keyVal,
        album_title: albumTitle,
        plot_hint: plotHint,
        frames,
        language: localeCode || "en",
        settings: clientSettings,
      }),
      signal,
    });
    if (composeResp && composeResp.ok) {
      const data = await composeResp.json();
      if (data && data.markdown) {
        return String(data.markdown);
      }
    }
  } catch (e) {
    if (e?.name === "AbortError") throw e;
  }

  // 2. Universal live fallback via /chat (supports Local Ollama, Public AI Node, and AI Horde)
  const fullPrompt = buildStoryCompositionPrompt({
    plotHint,
    albumTitle,
    frames,
    localeCode,
  });

  const chatResp = await fetchFn("/chat", {
    method: "POST",
    headers,
    body: JSON.stringify({
      omd_key: keyVal,
      prompt: fullPrompt,
      chat: "__story_composer__",
      history: [],
      knowledge: [],
      settings: clientSettings,
    }),
    signal,
  });

  if (!chatResp || !chatResp.ok) {
    throw new Error(`AI node returned HTTP ${chatResp ? chatResp.status : "error"}`);
  }

  const chatData = await chatResp.json();
  const content =
    (typeof chatData === "string" ? chatData : "") ||
    chatData?.content ||
    chatData?.response ||
    chatData?.answer ||
    chatData?.text ||
    "";

  if (!content || String(content).startsWith("⚠️")) {
    throw new Error(content || "AI node returned an empty story response");
  }
  return String(content);
}

// ---------------------------------------------------------------------------
// 6. End-to-End Story Folder Builder
// ---------------------------------------------------------------------------

/**
 * Normalizes heterogeneous media items (from MediaLibrary, CameraRoll, FileManager, or KreaStudio),
 * copies them into `<parentFolder>/<subfolderName>`, composes `Readme.md` via LLM, and writes it.
 */
export async function composeAndCreateStoryFolder({
  rawItems = [],
  subfolderName = "",
  plotHint = "",
  albumTitle = "",
  explicitParentFolderId = "", // e.g. "/beelink/Pictures" or "/beelink/OnMyChat/generated"
  targetDevice = null,
  storageDevice = null,
  deviceDrive = null,
  httpDrive = null,
  defaultStorage = "",
  omdKey = "",
  localeCode = "en",
  isCreations = false,
  onProgress = null,
  signal = null,
} = {}) {
  const emit = (stage, detail = "") => {
    if (typeof onProgress === "function") {
      try {
        onProgress({ stage, detail });
      } catch (_) {}
    }
  };

  const cleanSub = sanitizeStoryFolderName(subfolderName);
  if (!cleanSub) throw new Error("Story folder name is required");
  if (!Array.isArray(rawItems) || rawItems.length === 0) {
    throw new Error("Select at least one photo or video");
  }

  // 1. Normalize source items
  const knowledgeLookup = new Map();
  if (typeof window !== "undefined" && Array.isArray(window.__omdKnowledgeCards)) {
    for (const k of window.__omdKnowledgeCards) {
      const id = String(k?.document_id || k?.path || "").toLowerCase();
      if (id) knowledgeLookup.set(id, k);
      const fn = id.split("/").pop();
      if (fn && !knowledgeLookup.has(fn)) knowledgeLookup.set(fn, k);
    }
  }

  const normalized = rawItems.map((it, idx) => {
    // Support MediaLibrary item, CameraRoll enriched item, raw SVAR file item, and KreaStudio item
    const rawFile = it.raw || it;
    const fullId = String(it.docId || rawFile.id || it.path || "");
    const fname = String(it.filename || it.name || rawFile.name || rawFile.value || fullId.split("/").pop() || `media_${idx + 1}.jpg`);
    const split = splitPrefixedFolderPath(fullId);
    const srcDev = it.device || split.device || targetDevice || storageDevice || null;
    const srcDrive = srcDev?.drive || deviceDrive || null;
    const srcRealPath = it.resolvedPath || (it.path && !split.device ? it.path : split.realPath) || "";
    const srcPrefix = it.resolvedSeg || split.prefix || srcDev?.prefix?.replace(/^\/+|\/+$/g, "") || "";
    const p2pUrl = srcPrefix && srcRealPath && !srcDev?.local
      ? `/p2p/${srcPrefix}${encodeURI(srcRealPath.startsWith("/") ? srcRealPath : "/" + srcRealPath)}`
      : "";

    const matchedCard =
      it.card ||
      knowledgeLookup.get(fullId.toLowerCase()) ||
      knowledgeLookup.get(fname.toLowerCase()) ||
      null;

    const cardText = String(matchedCard?.text || matchedCard?.content || it.description || "");
    const takenMs =
      Number(it.takenMs || 0) ||
      (matchedCard?.taken_at ? parseExifOrIsoTimestamp(matchedCard.taken_at) : 0) ||
      extractFilenameTimestamp(fname) ||
      parseExifOrIsoTimestamp(it.date || rawFile.date || rawFile.lastModified) ||
      0;

    const isVid = Boolean(it.isVideo || VIDEO_EXT_RE.test(fname));
    const place = String(it.place || it.city || matchedCard?.location || "").trim();
    const lmEv = extractCardLandmarkAndEvent(cardText, it.title || matchedCard?.title || "", it.city || "");
    const landmark = String(it.landmark || matchedCard?.landmark || lmEv.landmark || "").trim();
    const event = String(it.namedEvent || it.event || matchedCard?.event || lmEv.event || "").trim();

    const peopleSet = new Set();
    for (const p of it.people || []) if (p) peopleSet.add(String(p).trim());
    if (it.persona) {
      for (const p of String(it.persona).split(",")) {
        if (p.trim()) peopleSet.add(p.trim());
      }
    }
    if (cardText) {
      const mP = cardText.match(/(?:^|\n)People:\s*([^\n]+)/i);
      if (mP) {
        for (const p of mP[1].split(",")) {
          const cl = p.trim().replace(/\.$/, "");
          if (cl && !/^\d+$/.test(cl)) peopleSet.add(cl);
        }
      }
    }

    const isSens = Boolean(
      it.isSensitive ||
      it.sensitive ||
      rawFile.isSensitive ||
      matchedCard?.sensitive ||
      hasSensitiveTag(matchedCard?.tags) ||
      hasSensitiveReadmeContent(cardText)
    );

    return {
      orig: it,
      filename: fname,
      fullId,
      srcDev,
      srcDrive,
      srcPrefix,
      srcRealPath: srcRealPath.startsWith("/") ? srcRealPath : (srcRealPath ? "/" + srcRealPath : ""),
      p2pUrl,
      blob: it.imageBlob || it.blob || null,
      blobUrl: it.blobUrl || it.thumbBlobUrl || "",
      inlinePreview: it.inlinePreview || matchedCard?.imagePreview || matchedCard?.image_preview || "",
      isVideo: isVid,
      isSensitive: isSens,
      takenMs,
      place,
      landmark,
      event,
      people: Array.from(peopleSet),
      summary: String(it.summary || it.description || "").trim(),
      prompt: String(it.prompt || "").trim(),
      tags: Array.isArray(it.semanticTags)
        ? it.semanticTags
        : (Array.isArray(it.tags) ? it.tags : (Array.isArray(matchedCard?.tags) ? matchedCard.tags : [])),
      cardText,
    };
  });

  // Sort chronologically ascending so the story flows start -> finish, keeping videos at their time or finale
  const sortedItems = [...normalized].sort((a, b) => {
    if (a.isVideo !== b.isVideo && (!a.takenMs || !b.takenMs)) {
      return a.isVideo ? 1 : -1;
    }
    if (a.takenMs && b.takenMs && a.takenMs !== b.takenMs) {
      return a.takenMs - b.takenMs;
    }
    return a.filename.localeCompare(b.filename, undefined, { numeric: true, sensitivity: "base" });
  });

  // Ensure unique filenames inside the story subfolder when merging across folders
  const usedNames = new Set();
  for (const item of sortedItems) {
    let baseName = item.filename;
    let low = baseName.toLowerCase();
    if (usedNames.has(low)) {
      const dotIdx = baseName.lastIndexOf(".");
      const stem = dotIdx > 0 ? baseName.slice(0, dotIdx) : baseName;
      const ext = dotIdx > 0 ? baseName.slice(dotIdx) : "";
      let n = 2;
      while (usedNames.has(`${stem}_${n}${ext}`.toLowerCase())) n++;
      baseName = `${stem}_${n}${ext}`;
      low = baseName.toLowerCase();
      item.renamedInStory = true;
    }
    usedNames.add(low);
    item.sourceFilename = item.filename;
    item.filename = baseName;
  }

  // 2. Resolve destination parent folder & writable drive
  let destDevice = null;
  let destDrive = null;
  let destPrefix = "";
  let destParentRealPath = "";

  if (explicitParentFolderId && explicitParentFolderId !== "/") {
    const sp = splitPrefixedFolderPath(explicitParentFolderId);
    destDevice = sp.device || targetDevice || storageDevice || null;
    destDrive = destDevice?.drive || deviceDrive || null;
    destPrefix = sp.prefix || destDevice?.prefix?.replace(/^\/+|\/+$/g, "") || "";
    destParentRealPath = sp.realPath || "/";
  } else {
    // Infer dominant parent folder among online items (e.g. when called from MediaLibrary)
    const folderVotes = new Map();
    for (const item of sortedItems) {
      if (item.srcDrive && item.srcRealPath && item.srcRealPath.includes("/")) {
        const parentReal = item.srcRealPath.slice(0, item.srcRealPath.lastIndexOf("/")) || "/";
        const key = `${item.srcPrefix}::${parentReal}`;
        const prev = folderVotes.get(key) || { count: 0, dev: item.srcDev, drive: item.srcDrive, prefix: item.srcPrefix, parentReal };
        prev.count++;
        folderVotes.set(key, prev);
      }
    }
    let best = null;
    for (const cand of folderVotes.values()) {
      if (!best || cand.count > best.count) best = cand;
    }
    if (best && best.drive) {
      destDevice = best.dev;
      destDrive = best.drive;
      destPrefix = best.prefix;
      destParentRealPath = best.parentReal;
    } else {
      destDevice = storageDevice || targetDevice || deviceRegistry.list().find((d) => !d.isAiNode && d.drive) || null;
      destDrive = destDevice?.drive || deviceDrive || null;
      destPrefix = destDevice?.prefix?.replace(/^\/+|\/+$/g, "") || "local";
      const cleanDef = String(defaultStorage || "/Pictures").replace(/^\/+|\/+$/g, "");
      destParentRealPath = "/" + (cleanDef || "Pictures");
    }
  }

  if (!destDrive) {
    throw new Error("No writable storage folder is connected to save the story.");
  }

  const cleanParentReal = destParentRealPath === "/" ? "" : destParentRealPath.replace(/\/+$/, "");
  const storyRealPath = `${cleanParentReal}/${cleanSub}`;
  const storyFolderId = destPrefix ? `/${destPrefix}${storyRealPath}` : storyRealPath;

  emit("copying", `0 / ${sortedItems.length}`);
  await ensureDirRecursiveOnDrive(destDrive, storyRealPath);

  // 3. Copy media files (+ companion .Readme.md / .cover.jpg) & enrich missing sidecar facts
  let firstCoverBlob = null;
  let anySensitive = false;
  let copiedCount = 0;

  for (const item of sortedItems) {
    if (signal?.aborted) throw new DOMException("Aborted", "AbortError");
    if (item.isSensitive) anySensitive = true;

    const targetFilePath = `${storyRealPath}/${item.filename}`;
    const sameDrive = !item.renamedInStory && item.srcDrive && item.srcDrive === destDrive && item.srcRealPath;

    let copiedFast = false;
    if (sameDrive && typeof destDrive.copyFile === "function") {
      try {
        await destDrive.copyFile(item.srcRealPath, storyRealPath);
        copiedFast = true;
      } catch (_) {
        copiedFast = false;
      }
    }

    if (!copiedFast) {
      const blob = await fetchSourceItemBlob(item);
      if (blob && blob.size > 0) {
        const b64 = await blobToBase64(blob);
        await writeBase64ToDrive(destDrive, targetFilePath, b64);
        if (!firstCoverBlob && !item.isVideo && !item.isSensitive) {
          firstCoverBlob = blob;
        }
      }
    }

    // Read & copy companion <stem>.Readme.md if present on source drive
    if (item.srcRealPath) {
      const stemPath = item.srcRealPath.replace(/\.[^/.]+$/, "");
      const compReadmeReal = `${stemPath}.Readme.md`;
      const compP2p = item.srcPrefix && !item.srcDev?.local
        ? `/p2p/${item.srcPrefix}${encodeURI(compReadmeReal)}`
        : "";
      const sidecarTxt = item.cardText || (await readTextFromDrive(item.srcDrive, compReadmeReal, compP2p));
      if (sidecarTxt) {
        if (!item.cardText) {
          item.cardText = sidecarTxt;
          const parsed = parseReadmeSidecarText(sidecarTxt);
          if (!item.summary && parsed.description) item.summary = parsed.description;
          if (!item.prompt && parsed.prompt) item.prompt = parsed.prompt;
          if (parsed.isSensitive) {
            item.isSensitive = true;
            anySensitive = true;
          }
          const lmEv = extractCardLandmarkAndEvent(sidecarTxt, parsed.title, item.place);
          if (!item.landmark && lmEv.landmark) item.landmark = lmEv.landmark;
          if (!item.event && lmEv.event) item.event = lmEv.event;
        }
        if (sameDrive && typeof destDrive.copyFile === "function") {
          await destDrive.copyFile(compReadmeReal, storyRealPath).catch(() => {});
        } else {
          const stemName = item.filename.replace(/\.[^/.]+$/, "");
          await writeUtf8ToDrive(destDrive, `${storyRealPath}/${stemName}.Readme.md`, sidecarTxt).catch(() => {});
        }
      }

      if (item.isVideo) {
        const compCoverReal = `${stemPath}.cover.jpg`;
        if (sameDrive && typeof destDrive.copyFile === "function") {
          await destDrive.copyFile(compCoverReal, storyRealPath).catch(() => {});
        }
      }
    }

    copiedCount++;
    emit("copying", `${copiedCount} / ${sortedItems.length}`);
  }

  // Write folder.jpg cover for non-sensitive story folders so Auto-Albums / File Manager show a cover card immediately
  const firstNonSensImg = sortedItems.find((x) => !x.isVideo && !x.isSensitive);
  if (firstNonSensImg) {
    try {
      if (firstNonSensImg.srcDrive === destDrive && typeof destDrive.copyFile === "function" && firstNonSensImg.srcRealPath) {
        const b = await fetchSourceItemBlob(firstNonSensImg);
        if (b && b.size > 0) {
          await writeBase64ToDrive(destDrive, `${storyRealPath}/folder.jpg`, await blobToBase64(b));
        }
      } else if (firstCoverBlob) {
        await writeBase64ToDrive(destDrive, `${storyRealPath}/folder.jpg`, await blobToBase64(firstCoverBlob));
      }
    } catch (_) {}
  }

  // 4. Build chronological frame facts for LLM
  emit("composing", "");
  const framesForLlm = sortedItems.map((it) => {
    const dateLabel = it.takenMs > 0
      ? new Date(it.takenMs).toLocaleString(localeCode || undefined, {
          day: "numeric",
          month: "short",
          year: "numeric",
          hour: "2-digit",
          minute: "2-digit",
        })
      : "";
    const cleanDesc = (it.summary || it.cardText || "")
      .replace(/<!--[\s\S]*?-->/g, "")
      .replace(/(?:^|\s)#(?:sensitive|nsfw)\b/gi, "")
      .replace(/\s+/g, " ")
      .trim();

    return {
      filename: it.filename,
      isVideo: it.isVideo,
      dateLabel,
      people: it.people,
      landmark: it.landmark,
      event: it.event,
      place: it.place,
      tags: (it.tags || [])
        .map((t) => String(t || "").replace(/^#+/, "").trim())
        .filter((t) => t && !["photo", "video", "private", "public", "sensitive", "omd"].includes(t.toLowerCase()))
        .slice(0, 6),
      description: cleanDesc,
    };
  });

  const fallbackTitle =
    (albumTitle ? String(albumTitle).replace(/^[\u{1F300}-\u{1FAFF}\u{2600}-\u{27BF}✨🏛️📍🎛️🎬🗓️🏷️👥🎭📖\s#]+/gu, "").trim() : "") ||
    cleanSub.replace(/_/g, " ");

  let finalMarkdown = "";
  try {
    const rawStory = await callAiNodeStoryLlm({
      httpDrive,
      omdKey,
      plotHint,
      albumTitle: fallbackTitle,
      frames: framesForLlm,
      localeCode,
      signal,
    });
    finalMarkdown = postProcessStoryMarkdown(rawStory, sortedItems, fallbackTitle);
  } catch (llmErr) {
    if (llmErr?.name === "AbortError") throw llmErr;
    console.warn("[story-composer] LLM fallback to structured comic:", llmErr);
    // Graceful offline/fallback comic layout using real photo facts if AI node is unreachable
    const blocks = [`# ${fallbackTitle}`, ""];
    if (plotHint) {
      blocks.push(`> ${plotHint.trim()}`, "");
    }
    for (let i = 0; i < sortedItems.length; i++) {
      const f = framesForLlm[i];
      const heading = f.landmark || f.event || f.place || `Scene ${i + 1}`;
      blocks.push(`## ${i + 1}. ${heading}`, "");
      blocks.push(`![${heading}](./${f.filename})`, "");
      if (f.description) {
        blocks.push(f.description, "");
      }
      blocks.push("---", "");
    }
    finalMarkdown = blocks.join("\n").trim() + "\n";
  }

  if (anySensitive && !hasSensitiveTag(finalMarkdown)) {
    finalMarkdown = `${finalMarkdown.trim()}\n\n#sensitive\n`;
  }

  // 5. Save Readme.md (and story.json for Creations compatibility with Storyboard!)
  emit("saving", "");
  await writeUtf8ToDrive(destDrive, `${storyRealPath}/Readme.md`, finalMarkdown);

  const parsedMeta = parseStoryReadmeMeta(finalMarkdown);
  if (isCreations || /\/generated(\/|$)/i.test(storyRealPath)) {
    const storyJson = {
      story_title: parsedMeta.title || fallbackTitle,
      story_slug: cleanSub,
      setting: plotHint || "",
      personas: [],
      scenes: sortedItems
        .filter((x) => !x.isVideo)
        .map((x, idx) => ({
          scene_number: idx + 1,
          title: x.landmark || x.event || `Scene ${idx + 1}`,
          narrative: x.summary || "",
          visual_prompt: x.prompt || "",
          image_name: x.filename,
          image_path: `${cleanSub}/${x.filename}`,
        })),
    };
    await writeUtf8ToDrive(destDrive, `${storyRealPath}/story.json`, JSON.stringify(storyJson, null, 2)).catch(() => {});
  }

  invalidateFolderReadmeCache(storyFolderId);
  emit("done", storyFolderId);

  return {
    folderId: storyFolderId,
    realFolderPath: storyRealPath,
    parentFolderId: destPrefix ? `/${destPrefix}${cleanParentReal || "/"}` : (cleanParentReal || "/"),
    subfolderName: cleanSub,
    device: destDevice,
    storyTitle: parsedMeta.title || fallbackTitle,
    storyDetail: parsedMeta.cleanDetail,
    storyMarkdown: finalMarkdown,
    copiedItems: sortedItems,
  };
}
