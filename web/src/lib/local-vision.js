/**
 * Client-side photo captioning & OCR:
 * - On native mobile (Capacitor Android/iOS), uses NativeStorage.analyzeImage
 *   (ML Kit Image Labeling + Text Recognition / Apple Vision) in ~50-150ms.
 * - On desktop web/PWA, falls back to Florence-2-base-ft in a Web Worker.
 */
import { writable } from "svelte/store";
import { isNativeStorageAvailable } from "./native-drive.js";
import { filterAndCanonicalizePhotoTags, parseExifOrIsoTimestamp } from "./media-albums.js";

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

// ---------------------------------------------------------------------------
// GPS -> Famous Landmark Discovery (cached per ~110m grid cell, zero image upload)
// ---------------------------------------------------------------------------
const GEO_LANDMARK_CACHE_KEY = "omd_geo_landmark_cache_v1";
let _landmarkCache = null;

function _loadLandmarkCache() {
  if (_landmarkCache) return _landmarkCache;
  _landmarkCache = {};
  if (typeof localStorage === "undefined") return _landmarkCache;
  try {
    const raw = localStorage.getItem(GEO_LANDMARK_CACHE_KEY);
    if (raw) _landmarkCache = JSON.parse(raw) || {};
  } catch (_) {
    _landmarkCache = {};
  }
  return _landmarkCache;
}

function _saveLandmarkCache() {
  if (!_landmarkCache || typeof localStorage === "undefined") return;
  try {
    const keys = Object.keys(_landmarkCache);
    if (keys.length > 500) {
      const trimmed = {};
      for (const k of keys.slice(keys.length - 400)) {
        trimmed[k] = _landmarkCache[k];
      }
      _landmarkCache = trimmed;
    }
    localStorage.setItem(GEO_LANDMARK_CACHE_KEY, JSON.stringify(_landmarkCache));
  } catch (_) {}
}

function _haversineMeters(lat1, lon1, lat2, lon2) {
  const toRad = (d) => (d * Math.PI) / 180;
  const dLat = toRad(lat2 - lat1);
  const dLon = toRad(lon2 - lon1);
  const a =
    Math.sin(dLat / 2) * Math.sin(dLat / 2) +
    Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) * Math.sin(dLon / 2);
  return 6371000 * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

const WIKI_LANDMARK_ACCEPT_RE = /\b(castle|citadel|fortress|palace|ch[aâ]teau|manor|cathedral|basilica|church|chapel|monastery|abbey|temple|mosque|synagogue|shrine|pagoda|museum|gallery|monument|memorial|statue|sculpture|obelisk|triumphal arch|tower|clock tower|bell tower|lighthouse|windmill|bridge|viaduct|aqueduct|fountain|square|plaza|piazza|national park|botanical garden|waterfall|falls|lake|island|mountain|peak|volcano|glacier|canyon|gorge|cave|beach|dune|theatre|theater|opera|amphitheatre|stadium|arena|ruins|archaeological|unesco|historic|heritage)\b/i;

const WIKI_LANDMARK_REJECT_RE = /\b(city in|town in|village in|municipality|administrative|oblast|raion|district|county|commune|borough|suburb|railway station|train station|underground station|metro station|tube station|bus station|bus stop|tram stop|highway|motorway|street in|road in|school|high school|hospital|clinic|bank|company|corporation|supermarket|shopping mall|election|battle of|massacre|list of|football club|politician|writer|painter|actor)\b/i;

function _inferLandmarkCategoryTag(title = "", desc = "") {
  const s = `${title} ${desc}`.toLowerCase();
  if (/\b(castle|citadel|fortress|ch[aâ]teau|grad)\b/.test(s)) return "castle";
  if (/\b(palace)\b/.test(s)) return "palace";
  if (/\b(cathedral|basilica)\b/.test(s)) return "cathedral";
  if (/\b(church|chapel)\b/.test(s)) return "church";
  if (/\b(monastery|abbey)\b/.test(s)) return "monastery";
  if (/\b(museum|gallery)\b/.test(s)) return "museum";
  if (/\b(monument|memorial|obelisk|triumphal arch|statue|sculpture)\b/.test(s)) return "monument";
  if (/\b(clock tower|bell tower|tower|big ben)\b/.test(s)) return "tower";
  if (/\b(bridge|viaduct|aqueduct)\b/.test(s)) return "bridge";
  if (/\b(fountain)\b/.test(s)) return "fountain";
  if (/\b(waterfall|falls)\b/.test(s)) return "waterfall";
  if (/\b(lake)\b/.test(s)) return "lake";
  if (/\b(mountain|peak|volcano|glacier)\b/.test(s)) return "mountains";
  if (/\b(stadium|arena)\b/.test(s)) return "stadium";
  if (/\b(theatre|theater|opera|amphitheatre)\b/.test(s)) return "theater";
  return "";
}

async function resolveLandmarkFromGps(lat, lon, { city = "", nativePoi = "", signal = null } = {}) {
  if (!Number.isFinite(lat) || !Number.isFinite(lon) || (lat === 0 && lon === 0)) {
    return null;
  }

  const cache = _loadLandmarkCache();
  const gridKey = `${lat.toFixed(3)},${lon.toFixed(3)}`;
  if (Object.prototype.hasOwnProperty.call(cache, gridKey)) {
    return cache[gridKey]; // null or { name, description, categoryTag }
  }

  const cityLow = String(city || "").split(",")[0].trim().toLowerCase();
  let best = null;

  if (typeof fetch === "function") {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), 2200);
    const onAbort = () => ctrl.abort();
    if (signal) signal.addEventListener("abort", onAbort, { once: true });

    try {
      const url =
        `https://en.wikipedia.org/w/api.php?action=query&generator=geosearch` +
        `&ggscoord=${lat.toFixed(4)}|${lon.toFixed(4)}&ggsradius=220&ggslimit=6` +
        `&prop=coordinates|description&colimit=6&format=json&origin=*`;
      const resp = await fetch(url, { signal: ctrl.signal });
      if (resp.ok) {
        const json = await resp.json();
        const pages = Object.values(json?.query?.pages || {});
        let bestScore = -Infinity;

        for (const p of pages) {
          const title = String(p?.title || "").trim();
          const desc = String(p?.description || "").trim();
          if (!title) continue;
          const titleLow = title.toLowerCase();
          if (cityLow && titleLow === cityLow) continue;
          const combined = `${title} - ${desc}`;
          if (WIKI_LANDMARK_REJECT_RE.test(combined)) continue;
          if (!WIKI_LANDMARK_ACCEPT_RE.test(combined)) continue;

          const coord = Array.isArray(p?.coordinates) ? p.coordinates[0] : null;
          const dist = coord && Number.isFinite(coord.lat) && Number.isFinite(coord.lon)
            ? _haversineMeters(lat, lon, coord.lat, coord.lon)
            : 180;
          if (dist > 230) continue;

          // Prefer closer landmarks and iconic architectural/natural monuments
          let score = 250 - dist;
          if (/\b(castle|citadel|palace|cathedral|museum|monument|clock tower|big ben|waterfall)\b/i.test(combined)) {
            score += 40;
          }
          if (score > bestScore) {
            bestScore = score;
            const cleanTitle = title.replace(/\s*\([^)]*\)\s*$/, "").trim();
            best = {
              name: cleanTitle,
              description: desc,
              categoryTag: _inferLandmarkCategoryTag(cleanTitle, desc),
            };
          }
        }
      }
    } catch (_) {
      // Offline or blocked: fall through silently to nativePoi
    } finally {
      clearTimeout(timer);
      if (signal) signal.removeEventListener("abort", onAbort);
    }
  }

  // Fallback to Android/iOS Geocoder POI if it clearly names a landmark or cultural venue
  if (!best && nativePoi) {
    const cleanPoi = String(nativePoi).trim();
    if (
      cleanPoi.length >= 4 &&
      (!cityLow || cleanPoi.toLowerCase() !== cityLow) &&
      (WIKI_LANDMARK_ACCEPT_RE.test(cleanPoi) ||
        /\b(замок|фортеця|палац|собор|монастир|музей|галерея|театр|опера|водоспад|озеро|парк|площа|grad|dvorec|muzej)\b/i.test(cleanPoi))
    ) {
      best = {
        name: cleanPoi,
        description: "",
        categoryTag: _inferLandmarkCategoryTag(cleanPoi, ""),
      };
    }
  }

  cache[gridKey] = best;
  _saveLandmarkCache();
  return best;
}

// ---------------------------------------------------------------------------
// OCR & Calendar Event / Landmark Extraction (banners, slides, stage screens, plaques)
// ---------------------------------------------------------------------------
const NAMED_EVENT_LINE_RE = /^([A-ZА-ЯІЇЄҐČŠŽ0-9][A-Za-zА-Яа-яІіЇїЄєҐґČčŠšŽž0-9'’&.,:\- ]{2,52}?\b(?:Conference|Summit|Forum|Congress|Symposium|Expo|Exhibition|Festival|Fest|Hackathon|Meetup|Biennale|Carnival|Cup|Championship|Marathon|Grand\s+Prix|Конференція|Конференция|Саміт|Саммит|Форум|Фестиваль|Виставка|Выставка|Чемпіонат|Чемпионат|Кубок|Марафон)(?:\s+20\d{2})?)$/i;

const NAMED_LANDMARK_SIGN_RE = /^([A-ZА-ЯІЇЄҐČŠŽ][A-Za-zА-Яа-яІіЇїЄєҐґČčŠšŽž'’.\- ]{2,48}?\b(?:Castle|Citadel|Palace|Cathedral|Monastery|Museum|National\s+Park|Botanical\s+Garden|Opera\s+House|Замок|Фортеця|Палац|Собор|Монастир|Музей|Національний\s+парк|Grad|Muzej))$/i;

const OCR_JUNK_HEADLINE_RE = /^(https?:\/\/|www\.|[a-z0-9._%+-]+@|\+?\d[\d\s\-()]{5,}$|exit|вхід|вихід|вход|выход|wc|toilet|wifi|password|menu|open|closed|push|pull|parking)$/i;

function extractInsightsFromOcrAndDate({
  ocr = "",
  headlines = [],
  semanticTags = [],
  takenAt = "",
} = {}) {
  let namedEvent = "";
  let signLandmark = "";
  const extraEventTags = [];

  const candidateLines = [];
  for (const h of headlines || []) {
    const txt = typeof h === "string" ? h.trim() : String(h?.text || "").trim();
    const relH = typeof h === "object" && h !== null ? Number(h.relHeight || 0) : 0.04;
    if (txt) candidateLines.push({ text: txt, relHeight: relH });
  }
  if (ocr) {
    for (const rawLn of ocr.split(/\r?\n/).slice(0, 14)) {
      const ln = rawLn.trim().replace(/\s+/g, " ");
      if (ln.length >= 4 && ln.length <= 64 && !candidateLines.some((c) => c.text === ln)) {
        candidateLines.push({ text: ln, relHeight: 0.02 });
      }
    }
  }

  for (const cand of candidateLines) {
    const clean = cand.text.replace(/^[•*#\-\u2013\u2014\s]+|[•*#\-\u2013\u2014\s]+$/g, "").trim();
    if (clean.length < 4 || clean.length > 60 || OCR_JUNK_HEADLINE_RE.test(clean)) continue;

    if (!namedEvent) {
      const mEv = clean.match(NAMED_EVENT_LINE_RE);
      if (mEv) {
        namedEvent = mEv[1].trim();
        if (/conference|summit|forum|congress|symposium|конференц|саміт|саммит|форум/i.test(namedEvent)) {
          extraEventTags.push("conference");
        } else if (/festival|fest|carnival|фестиваль/i.test(namedEvent)) {
          extraEventTags.push("festival");
        } else if (/expo|exhibition|biennale|виставка|выставка/i.test(namedEvent)) {
          extraEventTags.push("exhibition");
        } else if (/marathon|cup|championship|марафон|чемпіонат|чемпионат|кубок/i.test(namedEvent)) {
          extraEventTags.push("marathon");
        }
      }
    }

    if (!signLandmark) {
      const mLm = clean.match(NAMED_LANDMARK_SIGN_RE);
      if (mLm) {
        signLandmark = mLm[1].trim();
      }
    }
  }

  // If the scene is clearly a stage / conference / concert / exhibition and has a prominent banner/slide title
  const isEventScene = semanticTags.some((t) =>
    ["conference", "concert", "festival", "theater", "opera", "exhibition"].includes(t)
  );
  if (!namedEvent && isEventScene && candidateLines.length > 0) {
    const top = candidateLines[0];
    const cleanTop = top.text.replace(/\s+/g, " ").trim();
    if (
      top.relHeight >= 0.032 &&
      cleanTop.length >= 4 &&
      cleanTop.length <= 46 &&
      !OCR_JUNK_HEADLINE_RE.test(cleanTop) &&
      /[A-Za-zА-Яа-яІіЇїЄєҐґ]{3,}/.test(cleanTop)
    ) {
      namedEvent = cleanTop;
    }
  }

  // Calendar holiday enrichment from EXIF capture timestamp
  const ts = parseExifOrIsoTimestamp(takenAt);
  if (ts > 0) {
    const dt = new Date(ts);
    const m = dt.getMonth() + 1;
    const d = dt.getDate();
    const hr = dt.getHours();
    if ((m === 12 && d === 31 && hr >= 18) || (m === 1 && d === 1 && hr <= 6)) {
      extraEventTags.push("new year");
    } else if (
      ((m === 12 && (d === 24 || d === 25 || d === 26)) || (m === 1 && (d === 6 || d === 7))) &&
      (semanticTags.includes("christmas") || semanticTags.includes("fireworks") || semanticTags.includes("party"))
    ) {
      extraEventTags.push("christmas");
    }
  }

  return { namedEvent, signLandmark, extraEventTags };
}

function buildPhotoResult({
  labels = [],
  labelDetails = [],
  caption = "",
  ocr = "",
  headlines = [],
  faceCount = 0,
  people = [],
  location = "",
  landmarkInfo = null,
  filename = "",
  exif = null,
}) {
  const loc = String(location || "").trim();
  const takenAt = exif?.takenAt || "";

  // 1. Canonicalize & filter visual labels (cats, dogs, landmarks, specific events, nature; zero clothing/furniture/sky)
  const rawVisualCandidates = labelDetails && labelDetails.length > 0 ? labelDetails : labels;
  const baseSemanticTags = filterAndCanonicalizePhotoTags(rawVisualCandidates, {
    peopleLower: new Set((people || []).map((p) => String(p).toLowerCase())),
    isCreation: false,
  });

  // 2. Extract named events & plaque landmarks from OCR headlines + capture date
  const ocrInsights = extractInsightsFromOcrAndDate({
    ocr,
    headlines,
    semanticTags: baseSemanticTags,
    takenAt,
  });

  // Do not attach outdoor GPS landmark to close-up indoor pet/food/document shots unless visually plausible
  const isPureCloseUpSubject =
    faceCount === 0 &&
    baseSemanticTags.length > 0 &&
    baseSemanticTags.every((t) => ["cat", "dog", "sushi", "pizza", "dessert"].includes(t));

  const resolvedLandmark = (!isPureCloseUpSubject && landmarkInfo?.name) || ocrInsights.signLandmark || "";
  const landmarkDesc = (!isPureCloseUpSubject && landmarkInfo?.description) || "";
  const landmarkCategory = (!isPureCloseUpSubject && landmarkInfo?.categoryTag) || "";
  const resolvedEvent = ocrInsights.namedEvent || "";

  // 3. Merge high-signal tags
  const mergedCandidates = [
    ...baseSemanticTags,
    ...(landmarkCategory ? [landmarkCategory] : []),
    ...ocrInsights.extraEventTags,
    ...(resolvedLandmark ? [resolvedLandmark.toLowerCase()] : []),
  ];
  const cleanSemanticTags = filterAndCanonicalizePhotoTags(mergedCandidates, {
    peopleLower: new Set((people || []).map((p) => String(p).toLowerCase())),
    landmarkLower: resolvedLandmark ? resolvedLandmark.toLowerCase() : "",
    eventLower: resolvedEvent ? resolvedEvent.toLowerCase() : "",
    isCreation: false,
  });

  // 4. Build concise, factual annotation for RAG & gallery captions
  const parts = [];
  if (resolvedLandmark) {
    parts.push(landmarkDesc ? `Landmark: ${resolvedLandmark} (${landmarkDesc})` : `Landmark: ${resolvedLandmark}`);
  }
  if (resolvedEvent) {
    parts.push(`Event: ${resolvedEvent}`);
  }
  if (caption) {
    parts.push(caption);
  } else if (cleanSemanticTags.length > 0) {
    const displayThemes = cleanSemanticTags
      .filter((t) => t !== resolvedLandmark.toLowerCase())
      .map((t) => t.charAt(0).toUpperCase() + t.slice(1));
    if (displayThemes.length > 0) {
      parts.push(displayThemes.join(", "));
    }
  }
  if (people.length > 0) {
    parts.push(`People: ${people.join(", ")}`);
  } else if (faceCount > 0) {
    parts.push(faceCount === 1 ? "Portrait" : `Group (${faceCount} people)`);
  }
  if (ocr) {
    parts.push(`Text: ${ocr.slice(0, 1200)}`);
  }
  if (takenAt) {
    parts.push(`Date: ${takenAt}`);
  }
  if (loc) {
    parts.push(`Location: ${loc}`);
  } else if (exif?.gpsString) {
    parts.push(`GPS: ${exif.gpsString}`);
  }

  // Final card tags: structural markers + clean semantic tags + named people (no city/oblast/clothing clutter)
  const tags = ["photo"];
  const addTag = (t) => {
    const clean = String(t || "").trim().toLowerCase();
    if (clean && !tags.includes(clean)) tags.push(clean);
  };
  for (const p of people) addTag(p);
  for (const st of cleanSemanticTags.slice(0, 6)) addTag(st);

  const smartTitle = resolvedLandmark || resolvedEvent || filename || "Photo";

  return {
    title: smartTitle,
    annotation: parts.join("\n").trim() || smartTitle,
    tags,
    location: loc,
    landmark: resolvedLandmark,
    event: resolvedEvent,
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
  const labelDetails = Array.isArray(res?.labelDetails) ? res.labelDetails : [];
  const headlines = Array.isArray(res?.headlines) ? res.headlines : [];
  const ocr = typeof res?.text === "string" ? res.text.trim() : "";
  const faceCount = Number(res?.faceCount || 0);
  const location = typeof res?.location === "string" ? res.location.trim() : "";
  const nativePoi = typeof res?.poi === "string" ? res.poi.trim() : "";

  const mergedExif = {
    ...(exif || {}),
    takenAt: exif?.takenAt || (typeof res?.takenAt === "string" ? res.takenAt.trim() : ""),
    gpsLat: Number.isFinite(exif?.gpsLat) ? exif.gpsLat : (Number.isFinite(res?.gpsLat) ? res.gpsLat : null),
    gpsLon: Number.isFinite(exif?.gpsLon) ? exif.gpsLon : (Number.isFinite(res?.gpsLon) ? res.gpsLon : null),
  };
  if (!mergedExif.gpsString && Number.isFinite(mergedExif.gpsLat) && Number.isFinite(mergedExif.gpsLon)) {
    mergedExif.gpsString = `${mergedExif.gpsLat.toFixed(5)}, ${mergedExif.gpsLon.toFixed(5)}`;
  }

  const [people, landmarkInfo] = await Promise.all([
    faceCount > 0 && httpDrive ? matchPeopleOnNode(httpDrive, drive, path, signal) : Promise.resolve([]),
    Number.isFinite(mergedExif.gpsLat) && Number.isFinite(mergedExif.gpsLon)
      ? resolveLandmarkFromGps(mergedExif.gpsLat, mergedExif.gpsLon, {
          city: location,
          nativePoi,
          signal,
        })
      : Promise.resolve(null),
  ]);

  console.log(
    `[local-vision] native analyzed ${filename} in ${res?.elapsedMs ?? "?"}ms: labels=${labels.length}, faces=${faceCount}, people=${people.length}, landmark=${landmarkInfo?.name || "-"}, loc=${location || "-"}`
  );
  return buildPhotoResult({
    labels,
    labelDetails,
    ocr,
    headlines,
    faceCount,
    people,
    location,
    landmarkInfo,
    filename,
    exif: mergedExif,
  });
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
  const [{ caption, ocr }, landmarkInfo] = await Promise.all([
    new Promise((resolve, reject) => {
      pending.set(id, { resolve, reject });
      w.postMessage({ type: "recognize", id, jpegBytes: copy }, [copy.buffer]);
    }),
    Number.isFinite(exif?.gpsLat) && Number.isFinite(exif?.gpsLon)
      ? resolveLandmarkFromGps(exif.gpsLat, exif.gpsLon, { signal })
      : Promise.resolve(null),
  ]);

  return buildPhotoResult({ caption, ocr, landmarkInfo, filename, exif });
}

