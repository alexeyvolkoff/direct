/**
 * Shared Timeline, Scrubber, Back-Stack, and Smart Auto-Albums engine
 * powering Media Library, Creations Studio Gallery, and File Manager CameraRoll.
 */

import { hasSensitiveReadmeContent, hasSensitiveTag } from "./content-mode.js";
import { parseStoragePath, DEFAULT_STORAGE_PATH } from "./storage-path.js";
import { deviceRegistry } from "./device-registry.js";

const IMAGE_EXTS = new Set(["jpg", "jpeg", "png", "webp", "gif", "avif", "heic", "heif", "bmp", "tiff"]);
const VIDEO_EXTS = new Set(["mp4", "mov", "webm", "m4v", "mkv", "avi"]);

export const NOISE_MEDIA_TAGS = new Set([
  "photo", "photos", "image", "images", "picture", "video", "videos",
  "gps", "ocr", "person", "people", "portrait_raw", "group", "omd",
  "sensitive", "nsfw", "document", "note", "file", "local", "mobile",
  "camera", "dcim", "img", "vid", "pxl", "ai_generated"
]);

// ---------------------------------------------------------------------------
// 1. Sub-View History Back-Stack (prevents Android/Browser Back from exiting app)
// ---------------------------------------------------------------------------
const _subViewStack = [];
let _subViewListenerInstalled = false;
let _subViewSeq = 0;

function _ensureSubViewPopListener() {
  if (_subViewListenerInstalled || typeof window === "undefined") return;
  _subViewListenerInstalled = true;

  window.addEventListener(
    "popstate",
    (ev) => {
      if (_subViewStack.length === 0) return;
      const top = _subViewStack[_subViewStack.length - 1];
      const curStateId = ev?.state?.omdSubView || "";
      // If current history state still has our top token, nothing popped from us
      if (curStateId === top.id) return;

      _subViewStack.pop();
      window.__omdSubViewJustPopped = true;
      setTimeout(() => {
        window.__omdSubViewJustPopped = false;
      }, 150);

      try {
        top.onPop();
      } catch (err) {
        console.warn("[media-albums] subView onPop error:", err);
      }
    },
    true
  );
}

/**
 * Push an in-applet sub-view (opened Auto-Album, Story subfolder, or Scoped Viewer)
 * onto browser/WebView history so pressing hardware Back returns to parent view.
 */
export function pushSubViewHistory(label, onPop) {
  if (typeof window === "undefined" || typeof onPop !== "function") return "";
  _ensureSubViewPopListener();
  const id = `sv_${++_subViewSeq}_${Date.now()}`;
  _subViewStack.push({ id, label, onPop });
  try {
    const prev = window.history.state && typeof window.history.state === "object" ? window.history.state : {};
    window.history.pushState({ ...prev, omdSubView: id }, "", window.location.href);
  } catch (_) {}
  return id;
}

/**
 * Dismiss a sub-view initiated by an explicit UI close/back button (or component destroy).
 * If `popHistory` is true and this entry is currently on top of window.history.state,
 * pops the browser history entry cleanly without re-running `onPop`.
 */
export function dismissSubViewHistory(token, { popHistory = true } = {}) {
  if (!token || typeof window === "undefined") return;
  const idx = _subViewStack.findIndex((x) => x.id === token);
  if (idx !== -1) {
    _subViewStack.splice(idx, 1);
  }
  if (popHistory && window.history.state?.omdSubView === token) {
    window.__omdSubViewJustPopped = true;
    setTimeout(() => {
      window.__omdSubViewJustPopped = false;
    }, 150);
    try {
      window.history.back();
    } catch (_) {}
  }
}

// ---------------------------------------------------------------------------
// 2. Capture Date & Filename Timestamp Extraction
// ---------------------------------------------------------------------------
export function parseExifOrIsoTimestamp(rawStr) {
  if (!rawStr) return 0;
  if (rawStr instanceof Date) {
    const t = rawStr.getTime();
    return !isNaN(t) && t > 0 ? t : 0;
  }
  if (typeof rawStr === "number") {
    const ms = rawStr < 1e11 ? rawStr * 1000 : rawStr;
    return !isNaN(ms) && ms > 0 ? ms : 0;
  }
  const s = String(rawStr).trim();
  if (!s) return 0;
  const mExif = s.match(/^(\d{4})[:\-](\d{2})[:\-](\d{2})(?:[ T](\d{2}):(\d{2})(?::(\d{2}))?)?/);
  if (mExif) {
    const y = parseInt(mExif[1], 10);
    const mo = parseInt(mExif[2], 10) - 1;
    const d = parseInt(mExif[3], 10);
    const hh = parseInt(mExif[4] || "12", 10);
    const mm = parseInt(mExif[5] || "0", 10);
    const ss = parseInt(mExif[6] || "0", 10);
    if (y >= 1970 && y <= 2100 && mo >= 0 && mo <= 11 && d >= 1 && d <= 31) {
      const dt = new Date(y, mo, d, hh, mm, ss).getTime();
      if (!isNaN(dt) && dt > 0) return dt;
    }
  }
  const num = Number(s);
  if (!isNaN(num) && num > 0) {
    return num < 1e11 ? num * 1000 : num;
  }
  const parsed = new Date(s).getTime();
  return !isNaN(parsed) && parsed > 0 ? parsed : 0;
}

export function extractFilenameTimestamp(nameOrPath) {
  const fname = String(nameOrPath || "").split("/").pop() || "";
  if (!fname) return 0;

  // 1. YYYYMMDD_HHMMSS or YYYYMMDD-HHMMSS (IMG_20261015_145535, Lark_KREA_20261118_113821, PXL_...)
  const mYmdHms = fname.match(/(?:^|\D)(20\d{2})(\d{2})(\d{2})[_-](\d{2})(\d{2})(\d{2})/);
  if (mYmdHms) {
    const dt = new Date(
      parseInt(mYmdHms[1], 10),
      parseInt(mYmdHms[2], 10) - 1,
      parseInt(mYmdHms[3], 10),
      parseInt(mYmdHms[4], 10),
      parseInt(mYmdHms[5], 10),
      parseInt(mYmdHms[6], 10)
    ).getTime();
    if (!isNaN(dt) && dt > 0) return dt;
  }

  // 2. YYYY-MM-DD[ _-HH-MM-SS] (Screenshot_2026-10-15-14-22-10)
  const mDash = fname.match(/(?:^|\D)(20\d{2})-(\d{2})-(\d{2})(?:[_ T-](\d{2})[-.:](\d{2})(?:[-.:](\d{2}))?)?/);
  if (mDash) {
    const dt = new Date(
      parseInt(mDash[1], 10),
      parseInt(mDash[2], 10) - 1,
      parseInt(mDash[3], 10),
      parseInt(mDash[4] || "12", 10),
      parseInt(mDash[5] || "0", 10),
      parseInt(mDash[6] || "0", 10)
    ).getTime();
    if (!isNaN(dt) && dt > 0) return dt;
  }

  // 3. 13-digit epoch ms (e.g. krea_1794930113223.png)
  const mEpoch = fname.match(/(?:^|\D)(1[6-9]\d{11})(?:\D|$)/);
  if (mEpoch) {
    const ms = parseInt(mEpoch[1], 10);
    if (!isNaN(ms) && ms > 0) return ms;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// 3. Creations Metadata, Model, Persona & Scene Category Inference
// ---------------------------------------------------------------------------
const SCENE_CATEGORY_RULES = [
  { tag: "portrait", rx: /\b(close[- ]up portrait|medium close[- ]up|headshot|selfie|portrait of)\b/i },
  { tag: "cafe & bistro", rx: /\b(bistro|cafe|coffee shop|espresso|restaurant|bar|booth|wine glass)\b/i },
  { tag: "street & city", rx: /\b(city street|cobblestone|alley|sidewalk|crosswalk|downtown|urban|street lamps|balcony)\b/i },
  { tag: "interior", rx: /\b(bookshop|bookshelf|library|elevator|bedroom|living room|hotel|apartment|kitchen|studio loft)\b/i },
  { tag: "nightlife", rx: /\b(nightclub|gothic club|club|neon lights|party|concert|lounge)\b/i },
  { tag: "nature & outdoors", rx: /\b(beach|ocean|sea|mountain|mountains|forest|woods|garden|park|lake|lavender|meadow)\b/i },
  { tag: "golden hour", rx: /\b(golden hour|sunset|sunrise|sunlit|warm tungsten|candlelight)\b/i },
  { tag: "cinematic & film", rx: /\b(35mm|film grain|analog|cinematic|bokeh|50mm|85mm|volumetric lighting)\b/i },
];

export function humanizeCheckpointName(rawCkpt = "") {
  const stem = String(rawCkpt || "")
    .trim()
    .replace(/\.(safetensors|ckpt|pt|pth|bin)$/i, "")
    .replace(/_+\d+$/, "")
    .trim();
  if (!stem) return "";
  const low = stem.toLowerCase();

  if (low.includes("analogmadness")) return { key: "ckpt:analog_madness_sdxl", label: "AnalogMadness SDXL" };
  if (low.includes("cyberrealisticxl") || low === "realistic") return { key: "ckpt:cyberrealistic_xl", label: "CyberRealistic XL" };
  if (low.includes("ponysemi") || low === "tooned") return { key: "ckpt:pony_semi_xl", label: "Pony Semi XL" };
  if (low.includes("cyberrealisticpony") || low.includes("ponyrealism") || low === "perfect") {
    return { key: "ckpt:cyberrealistic_pony", label: "CyberRealistic Pony" };
  }
  if (low.includes("illustrious") || low === "fantasy") return { key: "ckpt:illustrious_xl", label: "Illustrious XL" };
  if (low.includes("juggernaut")) return { key: "ckpt:juggernaut_xl", label: "Juggernaut XL" };
  if (low.includes("albedobase") || low.includes("albedo_base")) return { key: "ckpt:albedobase_xl", label: "AlbedoBase XL" };
  if (low.includes("flux")) return { key: "ckpt:flux", label: "Flux.1" };
  if (low.includes("krea")) return { key: "krea2", label: "Krea 2" };
  if (low.includes("anima")) return { key: "anima", label: "Anima" };
  if (low.includes("minimax") || low === "h3") return { key: "video_h3", label: "MiniMax-H3 · Video" };
  if (low.includes("horde")) return { key: "horde", label: "AI Horde" };

  // Generic humanizer for checkpoint filenames like myModelSDXL_v20 -> MyModel SDXL
  const cleaned = stem
    .replace(/[_-]+(?:v|xl|fp16|fp8|bf16|pruned)[0-9a-z.]*$/i, "")
    .replace(/([a-z])([A-Z])/g, "$1 $2")
    .replace(/[_-]+/g, " ")
    .replace(/\s+/g, " ")
    .trim();
  const pretty = cleaned
    ? cleaned.charAt(0).toUpperCase() + cleaned.slice(1)
    : stem;
  const slug = pretty.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_+|_+$/g, "");
  return { key: `ckpt:${slug || "custom"}`, label: pretty };
}

export function isLikelyGeneratedMedia({
  filename = "",
  path = "",
  text = "",
  tags = [],
  defaultStorage = "",
} = {}) {
  const cleanPath = String(path || "").toLowerCase();
  const fname = String(filename || cleanPath.split("/").pop() || "");
  const lowName = fname.toLowerCase();
  const txt = String(text || "");

  if (cleanPath.includes("/onmychat/") || cleanPath.includes("/creations/") || cleanPath.includes("/generated/")) {
    return true;
  }
  if (defaultStorage) {
    const cleanDef = String(defaultStorage).replace(/^\/+|\/+$/g, "").toLowerCase();
    if (cleanDef && cleanDef.includes("onmychat") && cleanPath.includes(cleanDef)) return true;
  }
  if (/^(krea_|anima_|horde_|gen_|sdxl_|h3_|comfyui_|avatar_)/i.test(fname)) return true;
  if (/^[a-zа-яё]{2,20}(?:avatar)?_(?:krea|sdxl|h3|anima|horde)_/i.test(fname)) return true;
  if (/^img_\d{5}_/i.test(lowName) || /\.safetensors_\d+/i.test(lowName) || /^img-[a-z0-9_]+_\d+_/i.test(lowName)) {
    return true;
  }
  if (/<!--\s*(?:prompt|model):/i.test(txt)) return true;
  if (/\bscore_9\s*,\s*score_8_up\b/i.test(txt)) return true;
  const tagsLow = Array.isArray(tags) ? tags.map((t) => String(t || "").toLowerCase().replace(/^#+/, "")) : [];
  if (tagsLow.includes("ai_generated") || tagsLow.includes("krea") || tagsLow.includes("sdxl") || tagsLow.includes("comfyui")) {
    return true;
  }
  return false;
}

export function detectCreationModelInfo(filename, isVideo = false, extra = {}) {
  const fname = String(filename || "");
  const low = fname.toLowerCase();
  const modelHint = String(extra?.modelHint || "").trim();
  const combinedTxt = `${extra?.prompt || ""}\n${extra?.rawText || ""}`;

  if (modelHint) {
    const parsedHint = humanizeCheckpointName(modelHint);
    if (parsedHint) return parsedHint;
  }

  // 1. Embedded checkpoint filename like IMG-analogMadnessSDXL_xl5.safetensors_00644_.png
  const mCkpt = fname.match(/^(?:IMG-|GEN-|SDXL-)?([A-Za-z0-9_.-]+?)\.safetensors(?:_\d+)?/i);
  if (mCkpt && mCkpt[1]) {
    const parsedCkpt = humanizeCheckpointName(mCkpt[1]);
    if (parsedCkpt) return parsedCkpt;
  }

  // 2. Avatars
  if (/^avatar_/i.test(fname) || /avatar_(?:krea|sdxl)/i.test(low) || /^[a-z]+avatar_/i.test(fname)) {
    return { key: "avatars", label: "Avatars" };
  }
  // 3. Video / MiniMax-H3
  if (/\bh3[_-]/i.test(fname) || /_h3_/i.test(low) || isVideo) {
    return { key: "video_h3", label: "MiniMax-H3 · Video" };
  }
  // 4. Krea 2
  if (/\bkrea\b|_krea_|^krea_/i.test(fname)) {
    return { key: "krea2", label: "Krea 2" };
  }
  // 5. AI Horde
  if (/^horde_|_horde_/i.test(low)) {
    return { key: "horde", label: "AI Horde" };
  }
  // 6. Anima
  if (/^anima_|_anima_/i.test(low)) {
    return { key: "anima", label: "Anima" };
  }
  // 7. Pony / Illustrious prompt tags in raw prompt sidecars (e.g. score_9, score_8_up)
  if (/\bscore_9\s*,\s*score_8_up\b/i.test(combinedTxt)) {
    return { key: "ckpt:pony_xl", label: "Pony XL" };
  }
  // 8. ComfyUI direct workflow outputs
  if (/^comfyui_/i.test(low)) {
    return { key: "comfyui", label: "ComfyUI Studio" };
  }
  // 9. Express SDXL Studio
  if (/^sdxl_|^img_\d{5}_|^gen_/i.test(low)) {
    return { key: "sdxl", label: "SDXL Studio" };
  }
  return { key: "studio", label: "Studio" };
}

export function parseReadmeSidecarText(rawTxt = "") {
  const txt = String(rawTxt || "").trim();
  if (!txt) {
    return { title: "", description: "", prompt: "", model: "", isSensitive: false, explicitTags: [] };
  }

  let prompt = "";
  const mPrompt = txt.match(/<!--\s*prompt:\s*([\s\S]*?)-->/i);
  if (mPrompt) {
    prompt = mPrompt[1].trim();
  }

  let model = "";
  const mModel = txt.match(/<!--\s*model:\s*([\s\S]*?)-->/i);
  if (mModel) {
    model = mModel[1].trim();
  }

  const withoutComments = txt.replace(/<!--[\s\S]*?-->/g, "").trim();
  let title = "";
  const mTitle = withoutComments.match(/^#\s+([^\n]+)/);
  if (mTitle) {
    title = mTitle[1].trim();
  }

  const body = withoutComments.replace(/^#\s+[^\n]+\n*/, "").trim();
  // Legacy raw-prompt sidecars (e.g. "<Judith> score_9, score_8_up...") without markdown title
  if (!prompt && !title && (/\bscore_9\b/i.test(body) || /^<[A-Za-z0-9_-]{2,24}>/.test(body))) {
    prompt = body;
  }

  const explicitTags = [];
  const seen = new Set();
  const tagMatches = body.matchAll(/(?:^|\s)#([a-zA-Zа-яА-ЯёЁіїєґІЇЄҐ0-9_]{2,30})/g);
  for (const m of tagMatches) {
    const t = m[1].toLowerCase();
    if (t === "sensitive" || t === "nsfw") continue;
    if (!NOISE_MEDIA_TAGS.has(t) && !seen.has(t)) {
      seen.add(t);
      explicitTags.push(t);
    }
  }

  const cleanDesc = body
    .split("\n")
    .map((l) => l.replace(/(?:^|\s)#(?:sensitive|nsfw)\b/gi, "").trim())
    .filter(Boolean)
    .join(" ")
    .trim();

  const isSensitive = Boolean(hasSensitiveTag(txt) || hasSensitiveReadmeContent(txt));
  return {
    title,
    description: cleanDesc,
    prompt,
    model,
    isSensitive,
    explicitTags,
  };
}

export function enrichCreationAttributes({
  filename = "",
  path = "",
  title = "",
  description = "",
  prompt = "",
  modelHint = "",
  rawText = "",
  personaHint = "",
  storyFolder = "",
  isVideo = false,
  explicitTags = [],
  knownPersonas = [],
}) {
  const fname = String(filename || path.split("/").pop() || "");
  const modelInfo = detectCreationModelInfo(fname, isVideo, {
    modelHint,
    prompt,
    rawText: rawText || description,
  });

  // Build lookup of known personas
  const personaMap = new Map();
  for (const p of knownPersonas || []) {
    if (p && p.name) {
      personaMap.set(p.name.trim().toLowerCase(), p.name.trim());
    }
  }

  const matchedPersonas = [];
  const addPersona = (rawName) => {
    const clean = String(rawName || "").trim();
    if (!clean || clean.length < 2 || clean.length > 28) return;
    const low = clean.toLowerCase();
    if (["img", "sdxl", "krea", "h3", "clip", "avatar", "video", "photo", "gen", "horde", "anima", "scene"].includes(low)) {
      return;
    }
    if (/^\d+$/.test(low)) return;
    const canonical = personaMap.get(low) || (clean.charAt(0).toUpperCase() + clean.slice(1));
    if (!matchedPersonas.some((x) => x.toLowerCase() === canonical.toLowerCase())) {
      matchedPersonas.push(canonical);
    }
  };

  if (personaHint) addPersona(personaHint);

  // 1. Filename prefix like Lark_KREA_..., LarkAvatar_KREA_..., Dana_SDXL_...
  const mPrefix = fname.match(/^([A-Za-zА-Яа-яЁё]{2,20}?)(?:Avatar)?_(?:KREA|SDXL|H3|ANIMA|HORDE)\b/i);
  if (mPrefix) {
    addPersona(mPrefix[1]);
  }

  const combinedText = `${title}\n${description}\n${prompt}`;

  // 2. Known personas mentioned in title/description/prompt
  for (const [lowName, canonicalName] of personaMap.entries()) {
    const escaped = lowName.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    const rx = new RegExp(`(?:^|[^a-zA-Zа-яА-Я0-9_])${escaped}(?:$|[^a-zA-Zа-яА-Я0-9_])`, "i");
    if (rx.test(combinedText)) {
      addPersona(canonicalName);
    }
  }

  // 3. Explicit character patterns in prompt: "shot of Kate, a 23-year-old..." or "<Judith>" / "<lora:Name:...>"
  const mAngleTag = combinedText.match(/(?:^|\s)<([A-Z][a-zA-Z0-9]{1,18})>/);
  if (mAngleTag && !["Picture", "Image", "Video", "Photo", "Scene"].includes(mAngleTag[1])) {
    addPersona(mAngleTag[1]);
  }
  const mShotOf = prompt.match(/\b(?:shot|portrait|photo|view) of ([A-Z][a-z]{2,15})(?:\s*,|\s+pulling|\s+standing|\s+sitting|\s+leaning|\s+walking|\s+wearing|\s+in\b)/);
  if (mShotOf && !["The", "Her", "His", "Two", "One"].includes(mShotOf[1])) {
    addPersona(mShotOf[1]);
  }

  // Scene categories + explicit tags
  const tags = [];
  const seenTags = new Set();
  const pushTag = (t) => {
    const clean = String(t || "").trim().toLowerCase().replace(/^#+/, "");
    if (!clean || clean.length < 2 || NOISE_MEDIA_TAGS.has(clean) || seenTags.has(clean)) return;
    seenTags.add(clean);
    tags.push(clean);
  };

  for (const t of explicitTags || []) pushTag(t);
  for (const rule of SCENE_CATEGORY_RULES) {
    if (rule.rx.test(combinedText)) {
      pushTag(rule.tag);
    }
  }

  // Detect story folder from path if not explicitly passed
  let resolvedStory = storyFolder || "";
  if (!resolvedStory && path) {
    const mStory = String(path).match(/\/generated\/([^\/]+)\/[^\/]+$/i);
    if (mStory && mStory[1] && !mStory[1].startsWith(".")) {
      resolvedStory = mStory[1];
    }
  }

  return {
    modelKey: modelInfo.key,
    modelLabel: modelInfo.label,
    personas: matchedPersonas,
    storyFolder: resolvedStory,
    semanticTags: tags,
  };
}

// ---------------------------------------------------------------------------
// 4. Lightweight Readme Cache & Direct Creations Scanner (for MediaLibrary)
// ---------------------------------------------------------------------------
const README_META_CACHE_KEY = "omd_media_readme_meta_v2";
let _memReadmeCache = null;

function _loadReadmeCache() {
  if (_memReadmeCache) return _memReadmeCache;
  _memReadmeCache = {};
  if (typeof localStorage === "undefined") return _memReadmeCache;
  try {
    const raw = localStorage.getItem(README_META_CACHE_KEY);
    if (raw) _memReadmeCache = JSON.parse(raw) || {};
  } catch (_) {
    _memReadmeCache = {};
  }
  return _memReadmeCache;
}

function _saveReadmeCache() {
  if (!_memReadmeCache || typeof localStorage === "undefined") return;
  try {
    const keys = Object.keys(_memReadmeCache);
    if (keys.length > 1500) {
      const trimmed = {};
      for (const k of keys.slice(keys.length - 1200)) {
        trimmed[k] = _memReadmeCache[k];
      }
      _memReadmeCache = trimmed;
    }
    localStorage.setItem(README_META_CACHE_KEY, JSON.stringify(_memReadmeCache));
  } catch (_) {}
}

async function _readDriveTextSafe(drive, httpDrive, path) {
  if (drive) {
    try {
      if (typeof drive.homeRead === "function") {
        const b64 = await drive.homeRead(path).catch(() => null);
        if (b64) return decodeURIComponent(escape(atob(b64)));
      }
      if (typeof drive.readFile === "function") {
        const res = await drive.readFile(path).catch(() => null);
        const b64 = res && res.data ? res.data : (typeof res === "string" ? res : null);
        if (b64) return new TextDecoder("utf-8").decode(Uint8Array.from(atob(b64.replace(/\s+/g, "")), (c) => c.charCodeAt(0)));
      }
    } catch (_) {}
  }
  if (httpDrive && typeof httpDrive.fetch === "function") {
    try {
      const resp = await httpDrive.fetch(path).catch(() => null);
      if (resp && resp.ok) {
        const ct = (resp.headers?.get?.("content-type") || "").toLowerCase();
        if (!ct.includes("text/html")) {
          return await resp.text();
        }
      }
    } catch (_) {}
  }
  return "";
}

export function resolveCreationsBaseStorage(defaultStorage = "", userName = "") {
  let raw = (
    defaultStorage ||
    (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
    (typeof window !== "undefined" && window.__omdProfile?.defaultStorage) ||
    DEFAULT_STORAGE_PATH ||
    "/Data"
  ).trim();
  const { folder, device, deviceName } = parseStoragePath(raw);
  let clean = (folder || "").replace(/^\/+|\/+$/g, "");

  const knownUsers = [
    userName,
    typeof window !== "undefined" && window.__omdProfile?.user,
    typeof window !== "undefined" && window.__omdProfile?.name,
  ].filter(Boolean);

  for (const u of knownUsers) {
    const uLow = String(u).toLowerCase();
    if (clean.toLowerCase() === uLow) {
      clean = (DEFAULT_STORAGE_PATH || "/Data").replace(/^\/+/, "");
      break;
    } else if (clean.toLowerCase().startsWith(uLow + "/")) {
      clean = clean.slice(String(u).length).replace(/^\/+/, "");
    }
  }

  clean = clean.replace(/^(home|Users)\/[^\/]+(\/|$)/i, "").replace(/^\/+|\/+$/g, "");
  clean = clean.replace(/^private\//i, "");
  if (clean.endsWith("/generated")) clean = clean.slice(0, -"/generated".length);
  else if (clean === "generated") clean = "";
  clean = clean.replace(/^\/+|\/+$/g, "");
  if (!clean || clean.toLowerCase() === "alexey") {
    clean = (DEFAULT_STORAGE_PATH || "/Data").replace(/^\/+/, "");
  }
  return {
    baseFolder: clean || "Data",
    device,
    deviceName,
  };
}

/**
 * Directly scans the `generated/` directory (plus story subfolders and `personas/*.json`)
 * so unindexed Creations appear in Media Library with full Persona, Story, Model, and Tag albums.
 */
export async function scanDirectCreationsFolder({
  deviceDrive = null,
  storageDevice = null,
  targetDevice = null,
  httpDrive = null,
  defaultStorage = "",
  userName = "",
  onBatchUpdate = null,
  signal = null,
}) {
  const { baseFolder, device: parsedDev } = resolveCreationsBaseStorage(defaultStorage, userName);
  let activeDev =
    parsedDev ||
    storageDevice ||
    (deviceDrive && deviceRegistry.list().find((d) => d.drive === deviceDrive && !d.isAiNode)) ||
    (targetDevice && !targetDevice.isAiNode ? targetDevice : null) ||
    deviceRegistry.list().find((d) => !d.isAiNode && d.drive && d.drive.isConnected) ||
    null;

  let activeDrive = (activeDev && activeDev.drive) || deviceDrive || null;
  if (activeDrive && !activeDrive.isConnected && typeof activeDrive.waitUntilConnected === "function") {
    await activeDrive.waitUntilConnected(2000).catch(() => {});
  }

  const candidateFolders = [
    `/${baseFolder}/generated`,
    "/OnMyChat/generated",
    "/Private/OnMyChat/generated",
    "/Data/generated",
    "/Private/Data/generated",
    "/generated",
  ].filter((v, i, a) => v && a.indexOf(v) === i);

  const listDir = async (dirPath) => {
    if (activeDrive && typeof activeDrive.getFiles === "function" && (activeDrive.isConnected || activeDrive.status === "connected" || activeDev?.local)) {
      try {
        const res = await activeDrive.getFiles(dirPath);
        if (Array.isArray(res) && res.length > 0) return res;
      } catch (_) {}
    }
    if (httpDrive && typeof httpDrive.fetch === "function") {
      try {
        const res = await httpDrive.fetch(`/api/v1/files?path=${encodeURIComponent(dirPath)}`);
        if (res && res.ok) {
          const data = await res.json();
          const arr = data.list || data.files || [];
          if (Array.isArray(arr) && arr.length > 0) return arr;
        }
      } catch (_) {}
    }
    return [];
  };

  let rootEntries = [];
  let effectiveFolder = candidateFolders[0];
  for (const cand of candidateFolders) {
    if (signal?.aborted) return { items: [], personas: [] };
    const entries = await listDir(cand);
    if (entries.length > 0) {
      rootEntries = entries;
      effectiveFolder = cand;
      break;
    }
  }

  if (!rootEntries.length) {
    return { items: [], personas: [] };
  }

  const devPrefix = (
    activeDev?.prefix ||
    activeDev?.name ||
    activeDev?.linkId ||
    targetDevice?.prefix ||
    targetDevice?.name ||
    targetDevice?.linkId ||
    "local"
  ).replace(/^\/+|\/+$/g, "");

  // 1. Load personas/*.json from sibling folder for real names & avatars
  const personasFolder = effectiveFolder.replace(/\/generated\/?$/i, "/personas");
  const personas = [];
  if (personasFolder && personasFolder !== effectiveFolder) {
    const pEntries = await listDir(personasFolder);
    const jsonFiles = pEntries.filter((f) => !f.isDir && /\.json$/i.test(f.name || f.value || ""));
    for (const jf of jsonFiles.slice(0, 30)) {
      if (signal?.aborted) break;
      const jName = jf.name || jf.value;
      const rawJson = await _readDriveTextSafe(activeDrive, httpDrive, `${personasFolder}/${jName}`);
      if (rawJson) {
        try {
          const pObj = JSON.parse(rawJson);
          if (pObj && pObj.name) {
            personas.push({
              id: pObj.id || jName,
              name: pObj.name,
              avatar_b64: pObj.avatar || "",
            });
          }
        } catch (_) {}
      }
    }
  }

  // 2. Collect media files in generated/ AND immediate story subfolders
  const allFileRecords = [];
  const collectFromList = (entries, folderPath, storyFolder = "") => {
    const readmeSet = new Set();
    for (const e of entries) {
      const nm = e.name || e.value || "";
      if (!e.isDir && /\.readme\.md$/i.test(nm)) {
        readmeSet.add(nm.replace(/\.readme\.md$/i, "").toLowerCase());
      }
    }
    for (const e of entries) {
      const nm = e.name || e.value || "";
      if (!nm || nm.startsWith(".") || e.isDir || e.type === "folder") continue;
      if (nm === "folder.jpg" || /\.cover\.(jpe?g|png)$/i.test(nm)) continue;
      const ext = nm.includes(".") ? nm.split(".").pop().toLowerCase() : "";
      if (!IMAGE_EXTS.has(ext) && !VIDEO_EXTS.has(ext)) continue;
      const relPath = `${folderPath}/${nm}`.replace(/\/+/g, "/");
      const docId = `/${devPrefix}${relPath}`;
      const stem = nm.replace(/\.[^/.]+$/, "").toLowerCase();
      allFileRecords.push({
        entry: e,
        name: nm,
        ext,
        isVideo: VIDEO_EXTS.has(ext),
        relPath,
        docId,
        storyFolder,
        hasReadme: readmeSet.has(stem),
      });
    }
  };

  collectFromList(rootEntries, effectiveFolder, "");

  const subDirs = rootEntries.filter((e) => {
    const nm = e.name || e.value || "";
    return nm && !nm.startsWith(".") && (e.isDir || e.type === "folder");
  });

  for (const dir of subDirs.slice(0, 25)) {
    if (signal?.aborted) break;
    const subName = dir.name || dir.value;
    const subPath = `${effectiveFolder}/${subName}`.replace(/\/+/g, "/");
    const subEntries = await listDir(subPath);
    if (subEntries.length > 0) {
      collectFromList(subEntries, subPath, subName);
    }
  }

  const readmeCache = _loadReadmeCache();
  const buildCardFromRecord = (rec, meta) => {
    const mtimeMs =
      extractFilenameTimestamp(rec.name) ||
      parseExifOrIsoTimestamp(rec.entry.lastModified || rec.entry.date || rec.entry.mtime) ||
      0;
    const enriched = enrichCreationAttributes({
      filename: rec.name,
      path: rec.relPath,
      title: meta?.title || "",
      description: meta?.description || "",
      prompt: meta?.prompt || "",
      modelHint: meta?.model || "",
      storyFolder: rec.storyFolder,
      isVideo: rec.isVideo,
      explicitTags: meta?.explicitTags || [],
      knownPersonas: personas,
    });

    return {
      id: `creation:${rec.docId}`,
      document_id: rec.docId,
      path: rec.docId,
      relPath: rec.relPath,
      title: meta?.title || rec.name,
      text: [
        meta?.description,
        meta?.model ? `<!-- model: ${meta.model} -->` : "",
        meta?.prompt ? `<!-- prompt: ${meta.prompt} -->` : "",
      ].filter(Boolean).join("\n"),
      contentType: rec.isVideo ? "video" : "image",
      isDirectCreation: true,
      onDevice: Boolean(!activeDev?.local),
      source: "creations",
      sensitive: Boolean(meta?.isSensitive),
      tags: [
        ...(meta?.isSensitive ? ["sensitive"] : []),
        ...enriched.semanticTags,
      ],
      taken_at: mtimeMs,
      created: mtimeMs,
      storyFolder: enriched.storyFolder,
      modelKey: enriched.modelKey,
      modelLabel: enriched.modelLabel,
      matchedPersonas: enriched.personas,
    };
  };

  const builtCards = allFileRecords.map((rec) => {
    const sig = `${rec.relPath}:${rec.entry.size || 0}`;
    const cached = readmeCache[sig] || null;
    return buildCardFromRecord(rec, cached);
  });

  if (typeof onBatchUpdate === "function") {
    onBatchUpdate({ items: builtCards, personas });
  }

  // 3. Background hydrate uncached .Readme.md files in small batches (newest first)
  const uncachedIndices = [];
  for (let i = 0; i < allFileRecords.length; i++) {
    const rec = allFileRecords[i];
    if (!rec.hasReadme) continue;
    const sig = `${rec.relPath}:${rec.entry.size || 0}`;
    if (!readmeCache[sig]) {
      uncachedIndices.push(i);
    }
  }

  if (uncachedIndices.length > 0 && !signal?.aborted) {
    // Sort uncached indices newest-first so visible items hydrate immediately
    uncachedIndices.sort((ia, ib) => (builtCards[ib].taken_at || 0) - (builtCards[ia].taken_at || 0));
    let dirtyCount = 0;
    const BATCH = 6;
    for (let b = 0; b < uncachedIndices.length; b += BATCH) {
      if (signal?.aborted) break;
      const slice = uncachedIndices.slice(b, b + BATCH);
      await Promise.all(
        slice.map(async (idx) => {
          const rec = allFileRecords[idx];
          const readmePath = rec.relPath.replace(/\.[^/.]+$/, "") + ".Readme.md";
          const txt = await _readDriveTextSafe(activeDrive, httpDrive, readmePath);
          const sig = `${rec.relPath}:${rec.entry.size || 0}`;
          const parsed = parseReadmeSidecarText(txt || "");
          readmeCache[sig] = parsed;
          builtCards[idx] = buildCardFromRecord(rec, parsed);
          dirtyCount++;
        })
      );
      if (dirtyCount >= 18 && typeof onBatchUpdate === "function" && !signal?.aborted) {
        dirtyCount = 0;
        _saveReadmeCache();
        onBatchUpdate({ items: [...builtCards], personas });
      }
    }
    _saveReadmeCache();
    if (typeof onBatchUpdate === "function" && !signal?.aborted) {
      onBatchUpdate({ items: [...builtCards], personas });
    }
  }

  return { items: builtCards, personas };
}

// ---------------------------------------------------------------------------
// 5. Shared Timeline Day Groups & Year-Month Scrubber Builders
// ---------------------------------------------------------------------------
export function buildTimelineDayGroups(items, { localeStr = undefined, undatedLabel = "Undated" } = {}) {
  const groups = [];
  let cur = null;

  for (const m of items || []) {
    const ts = Number(m.takenMs || 0);
    let dayKey = "undated";
    let monthKey = "undated";
    let dayLabel = undatedLabel;
    let monthShort = "•";

    if (ts > 0) {
      const dt = new Date(ts);
      const y = dt.getFullYear();
      const mo = String(dt.getMonth() + 1).padStart(2, "0");
      const d = String(dt.getDate()).padStart(2, "0");
      dayKey = `${y}-${mo}-${d}`;
      monthKey = `${y}-${mo}`;
      dayLabel = dt.toLocaleDateString(localeStr, {
        day: "numeric",
        month: "long",
        year: "numeric",
        weekday: "short",
      });
      monthShort = dt.toLocaleDateString(localeStr, { month: "short", year: "2-digit" });
    }

    if (!cur || cur.dayKey !== dayKey) {
      cur = {
        dayKey,
        monthKey,
        monthShort,
        dayLabel,
        placesCount: new Map(),
        topPlace: "",
        items: [],
      };
      groups.push(cur);
    }
    cur.items.push(m);
    const placeName = m.city || m.place || "";
    if (placeName) {
      cur.placesCount.set(placeName, (cur.placesCount.get(placeName) || 0) + 1);
    }
  }

  for (const g of groups) {
    let bestP = "";
    let bestCnt = 0;
    for (const [p, cnt] of g.placesCount.entries()) {
      if (cnt > bestCnt) {
        bestP = p;
        bestCnt = cnt;
      }
    }
    g.topPlace = bestP;
  }

  return groups;
}

export function buildMonthScrubberMarkers(allSortedItems, localeStr = undefined) {
  const seen = new Map();
  for (let i = 0; i < (allSortedItems || []).length; i++) {
    const m = allSortedItems[i];
    const ts = Number(m.takenMs || 0);
    if (!ts) continue;
    const dt = new Date(ts);
    const y = dt.getFullYear();
    const mo = String(dt.getMonth() + 1).padStart(2, "0");
    const key = `${y}-${mo}`;
    if (!seen.has(key)) {
      seen.set(key, {
        key,
        year: y,
        shortLabel: dt.toLocaleDateString(localeStr, { month: "short" }),
        fullLabel: dt.toLocaleDateString(localeStr, { month: "long", year: "numeric" }),
        firstIndex: i,
      });
    }
  }
  return Array.from(seen.values());
}
