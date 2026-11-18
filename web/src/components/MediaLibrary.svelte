<script>
  import { createEventDispatcher, onMount, onDestroy, tick } from "svelte";
  import { fade, fly } from "svelte/transition";
  import lightGallery from "lightgallery";
  import { _, locale } from "../lib/i18n/index.js";
  import { setupGalleryPagination, isJustFileName } from "../lib/gallery-pagination.js";
  import { getViewerSettings } from "../lib/viewer-settings.js";
  import { deviceRegistry } from "../lib/device-registry.js";
  import { getDefaultWebDeviceName, LocalDrive } from "../lib/local-client.js";
  import { isNativeStorageAvailable, NativeStorageDrive } from "../lib/native-drive.js";
  import { indexerStore, clientIndexer } from "../lib/client-indexer.js";
  import { contentModeStore, setGlobalContentMode, hasSensitiveTag, hasSensitiveReadmeContent } from "../lib/content-mode.js";
  import {
    pushSubViewHistory,
    dismissSubViewHistory,
    enrichCreationAttributes,
    parseReadmeSidecarText,
    scanDirectCreationsFolder,
  } from "../lib/media-albums.js";
  import DeviceIndexModal from "./DeviceIndexModal.svelte";
  import VideoViewer from "./VideoViewer.svelte";

  export let knowledgeList = [];
  export let isDarkTheme = true;
  export let contentMode = "";
  export let isOverlayHeader = false;
  export let sidebarOpen = true;
  export let isOwner = false;
  export let hasOwnHardwareNode = false;
  export let loading = false;
  export let thumbFetcher = null;
  export let httpDrive = null;
  export let deviceDrive = null;
  export let storageDevice = null;
  export let chatStore = null;
  export let targetDevice = null;
  export let targetLinkId = "";
  export let defaultStorage = "";

  const dispatch = createEventDispatcher();

  let innerWidth = typeof window !== "undefined" ? window.innerWidth : 1024;
  $: isMobile = innerWidth < 768;
  $: effectiveContentMode = contentMode || $contentModeStore || "work";

  // Top mode: 'timeline' | 'albums'
  let activeTab = typeof localStorage !== "undefined"
    ? localStorage.getItem("omd_media_tab") || "timeline"
    : "timeline";

  let sortOrder = typeof localStorage !== "undefined"
    ? localStorage.getItem("omd_media_sort") || "desc"
    : "desc";

  let gridFit = typeof localStorage !== "undefined"
    ? localStorage.getItem("omd_media_fit") || "cover"
    : "cover"; // 'cover' | 'contain'

  let includeCreations = typeof localStorage !== "undefined"
    ? localStorage.getItem("omd_media_include_creations") === "true"
    : false;

  let mediaTypeFilter = "all"; // 'all' | 'image' | 'video'
  let activeAlbum = null; // { kind: 'person'|'place'|'moment'|'story'|'model'|'tag', id, title, subtitle, icon, coverUrl, itemIds: Set }
  let albumHistoryToken = "";
  let viewerHistoryToken = "";
  let mobileScrubberOpen = false;

  // Direct Creations scanner state (captures unindexed generated/ files, subfolders & personas)
  let directCreationCards = [];
  let directPersonas = [];
  let creationsScanAbort = null;
  let lastCreationsScanKey = "";

  async function triggerDirectCreationsScan(force = false) {
    const scanKey = `${Boolean(deviceDrive?.isConnected)}|${storageDevice?.linkId || ""}|${targetDevice?.linkId || ""}|${Boolean(httpDrive)}|${defaultStorage || ""}`;
    if (!force && scanKey === lastCreationsScanKey && directCreationCards.length > 0) return;
    lastCreationsScanKey = scanKey;
    if (creationsScanAbort) {
      try { creationsScanAbort.abort(); } catch (_) {}
    }
    const ctl = new AbortController();
    creationsScanAbort = ctl;
    try {
      await scanDirectCreationsFolder({
        deviceDrive,
        storageDevice,
        targetDevice,
        httpDrive,
        defaultStorage,
        onBatchUpdate: ({ items: cItems, personas: cPersonas }) => {
          if (ctl.signal.aborted) return;
          directCreationCards = cItems || [];
          directPersonas = cPersonas || [];
        },
        signal: ctl.signal,
      });
    } catch (_) {}
  }

  $: if (httpDrive || deviceDrive || storageDevice || targetDevice) {
    triggerDirectCreationsScan(false);
  }

  let searchInput = "";
  let searchQuery = "";
  let searchTimer = null;

  function onSearchInput(e) {
    searchInput = e?.target?.value ?? "";
    if (searchTimer) clearTimeout(searchTimer);
    searchTimer = setTimeout(() => {
      searchQuery = searchInput.trim();
      searchTimer = null;
    }, 220);
  }

  function clearSearch() {
    searchInput = "";
    searchQuery = "";
  }

  function setTab(tab) {
    activeTab = tab;
    if (tab === "timeline" && activeAlbum) {
      closeAlbum();
    }
    mobileScrubberOpen = false;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_media_tab", tab);
    }
  }

  let registryTick = 0;
  const unsubRegistry = deviceRegistry.subscribe((v) => {
    registryTick = v;
  });

  async function toggleSortOrder() {
    sortOrder = sortOrder === "desc" ? "asc" : "desc";
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_media_sort", sortOrder);
    }
    await tick();
    if (scrollContainerEl) {
      scrollContainerEl.scrollTo({ top: 0, behavior: "smooth" });
    }
  }

  function toggleGridFit() {
    gridFit = gridFit === "cover" ? "contain" : "cover";
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_media_fit", gridFit);
    }
  }

  function toggleIncludeCreations() {
    includeCreations = !includeCreations;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_media_include_creations", includeCreations ? "true" : "false");
    }
  }

  function toggleContentMode() {
    const next = effectiveContentMode === "fun" ? "work" : "fun";
    contentMode = setGlobalContentMode(next);
  }

  // Device indexing modal
  let showDeviceIndexModal = false;
  let localDeviceDrive = null;
  $: if (showDeviceIndexModal && !localDeviceDrive && typeof window !== "undefined") {
    localDeviceDrive = isNativeStorageAvailable() ? new NativeStorageDrive() : new LocalDrive();
  }

  // Known people from face recognition service
  let knownPeopleList = [];
  let lastPeopleDrive = null;

  async function loadKnownPeople() {
    if (!httpDrive || typeof httpDrive.fetch !== "function") return;
    try {
      const res = await httpDrive.fetch("/api/faces/people");
      if (res.ok) {
        const data = await res.json();
        knownPeopleList = Array.isArray(data.people) ? data.people : [];
      }
    } catch (_) {}
  }

  $: if (httpDrive && httpDrive !== lastPeopleDrive) {
    lastPeopleDrive = httpDrive;
    loadKnownPeople();
  }

  const IMAGE_EXTS = new Set(["jpg", "jpeg", "png", "webp", "gif", "avif", "heic", "heif", "bmp", "tiff"]);
  const VIDEO_EXTS = new Set(["mp4", "mov", "webm", "m4v", "mkv", "avi"]);
  const NOISE_TAGS = new Set([
    "photo", "photos", "image", "images", "picture", "video", "videos",
    "gps", "ocr", "person", "people", "portrait", "group", "omd",
    "sensitive", "nsfw", "document", "note", "file", "local", "mobile",
    "camera", "dcim", "img", "vid", "pxl", "screenshot"
  ]);

  function isFolderCard(k) {
    if (!k) return false;
    return k.is_folder === true || k.isFolder === true || k.contentType === "folder" || k.sType === "folder";
  }

  function detectMediaKind(k) {
    if (!k || isFolderCard(k)) return "";
    const docId = String(k.document_id || k.path || "");
    if (!docId || docId.startsWith("user:note:") || docId.startsWith("http://") || docId.startsWith("https://")) {
      return "";
    }
    const ct = String(k.contentType || "").toLowerCase();
    const mime = String(k.mimeType || "").toLowerCase();
    const ext = docId.split("?")[0].split("#")[0].split(".").pop().toLowerCase();
    const tagsLow = Array.isArray(k.tags) ? k.tags.map((t) => String(t).toLowerCase()) : [];

    if (ct === "video" || mime.startsWith("video/") || VIDEO_EXTS.has(ext) || tagsLow.includes("video")) {
      return "video";
    }
    if (ct === "image" || mime.startsWith("image/") || IMAGE_EXTS.has(ext)) {
      return "image";
    }
    if (k.imagePreview || k.image_preview) {
      return "image";
    }
    return "";
  }

  function isCreationItem(k, docId) {
    const cleanPath = String(docId || "").toLowerCase();
    const fname = cleanPath.split("/").pop() || "";
    const txt = String(k.text || k.content || "");
    if (/^(krea_|anima_|horde_|gen_)/i.test(fname)) return true;
    if (/^img_\d{5}_\d{8}_\d{6}/i.test(fname)) return true;
    if (cleanPath.includes("/onmychat/") || cleanPath.includes("/creations/") || cleanPath.includes("/generated/")) return true;
    if (defaultStorage) {
      const cleanDef = String(defaultStorage).replace(/^\/+|\/+$/g, "").toLowerCase();
      if (cleanDef && cleanDef.includes("onmychat") && cleanPath.includes(cleanDef)) return true;
    }
    if (/<!--\s*prompt:/i.test(txt)) return true;
    const tagsLow = Array.isArray(k.tags) ? k.tags.map((t) => String(t).toLowerCase()) : [];
    if (tagsLow.includes("ai_generated") || tagsLow.includes("krea") || tagsLow.includes("sdxl")) return true;
    return false;
  }

  function parseExifOrIsoDate(rawStr) {
    if (!rawStr) return 0;
    const s = String(rawStr).trim();
    // EXIF format: YYYY:MM:DD HH:MM:SS or YYYY-MM-DD HH:MM:SS
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
    const parsed = new Date(isNaN(Number(s)) ? s : Number(s)).getTime();
    return !isNaN(parsed) && parsed > 0 ? parsed : 0;
  }

  function extractCaptureTimestamp(k, docId) {
    if (k.taken_at) {
      const t = parseExifOrIsoDate(k.taken_at);
      if (t > 0) return t;
    }
    const txt = String(k.text || k.content || "");
    if (txt) {
      const mDate = txt.match(/(?:^|\n)(?:Date|Taken at|Дата съёмки|Дата):\s*([^\n.]+)/i);
      if (mDate) {
        const t = parseExifOrIsoDate(mDate[1]);
        if (t > 0) return t;
      }
    }
    const fname = String(docId || k.title || "").split("/").pop() || "";
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
    const mDashDate = fname.match(/(?:^|\D)(20\d{2})-(\d{2})-(\d{2})(?:[_-](\d{2})[-.](\d{2})(?:[-.](\d{2}))?)?/);
    if (mDashDate) {
      const dt = new Date(
        parseInt(mDashDate[1], 10),
        parseInt(mDashDate[2], 10) - 1,
        parseInt(mDashDate[3], 10),
        parseInt(mDashDate[4] || "12", 10),
        parseInt(mDashDate[5] || "0", 10),
        parseInt(mDashDate[6] || "0", 10)
      ).getTime();
      if (!isNaN(dt) && dt > 0) return dt;
    }
    const fallback = parseExifOrIsoDate(k.created || k.timestamp);
    return fallback > 0 ? fallback : 0;
  }

  function extractLocationInfo(k) {
    let rawLoc = String(k.location || "").trim();
    const txt = String(k.text || k.content || "");
    if (!rawLoc && txt) {
      const mLoc = txt.match(/(?:^|\n)Location(?:\s*\([^)]*\))?:\s*([^\n]+)/i);
      if (mLoc) rawLoc = mLoc[1].trim().replace(/\.$/, "");
    }
    if (!rawLoc) return { place: "", city: "", country: "", partsLower: [] };
    // Ignore raw numeric GPS coordinates masquerading as place names
    if (/^[-+]?\d+\.\d+\s*,\s*[-+]?\d+\.\d+$/.test(rawLoc)) {
      return { place: "", city: "", country: "", partsLower: [] };
    }
    const segs = rawLoc.split(",").map((s) => s.trim()).filter(Boolean);
    const city = segs[0] || rawLoc;
    const country = segs.length > 1 ? segs[segs.length - 1] : "";
    const label = country && country.toLowerCase() !== city.toLowerCase() ? `${city}, ${country}` : city;
    return {
      place: label,
      city,
      country,
      partsLower: segs.map((s) => s.toLowerCase()),
    };
  }

  function extractCardPeople(k, docId, peopleByPhotoDoc, knownByNameLower) {
    const names = [];
    const seen = new Set();
    const addName = (nm) => {
      const clean = String(nm || "").trim();
      if (!clean) return;
      const low = clean.toLowerCase();
      if (seen.has(low) || /^\d+$/.test(clean)) return;
      seen.add(low);
      const canonical = knownByNameLower.get(low)?.name || clean;
      names.push(canonical);
    };

    const byPhoto = peopleByPhotoDoc.get(docId);
    if (byPhoto) {
      for (const pName of byPhoto) addName(pName);
    }

    const txt = String(k.text || k.content || "");
    if (txt) {
      const mPeople = txt.match(/(?:^|\n)People:\s*([^\n]+)/i);
      if (mPeople) {
        for (const part of mPeople[1].split(",")) {
          const trimmed = part.trim().replace(/\.$/, "");
          if (trimmed && !/^\d+$/.test(trimmed)) addName(trimmed);
        }
      }
    }

    if (Array.isArray(k.tags)) {
      for (const t of k.tags) {
        const low = String(t || "").trim().toLowerCase();
        if (knownByNameLower.has(low)) {
          addName(knownByNameLower.get(low).name);
        }
      }
    }
    return names;
  }

  function cleanCardSummary(k, docId) {
    const txt = String(k.text || k.content || "").trim();
    if (!txt) return "";
    const lines = txt
      .replace(/<!--[\s\S]*?-->/g, "")
      .split("\n")
      .map((l) => l.trim())
      .filter((l) =>
        l &&
        !l.startsWith("#") &&
        !/^(Date|Taken at|Location|GPS|People|Text):/i.test(l)
      );
    const joined = lines.join(" ").trim();
    if (isJustFileName(joined, docId)) return "";
    return joined;
  }

  // Lookup maps from knownPeopleList
  $: knownPeopleIndexes = (() => {
    const byPhoto = new Map();
    const byName = new Map();
    for (const p of knownPeopleList || []) {
      if (!p || !p.name) continue;
      const low = p.name.trim().toLowerCase();
      byName.set(low, p);
      if (Array.isArray(p.photos)) {
        for (const docId of p.photos) {
          if (!docId) continue;
          if (!byPhoto.has(docId)) byPhoto.set(docId, []);
          byPhoto.get(docId).push(p.name);
        }
      }
    }
    return { byPhoto, byName };
  })();

  function isDevConnected(d) {
    if (!d) return false;
    return Boolean(
      d.local === true ||
      d.status === "connected" ||
      d.status === "online" ||
      d.online === true ||
      d.drive?.isConnected
    );
  }

  function matchesPropDevice(dev, seg) {
    if (!dev || !seg) return false;
    const low = String(seg).replace(/^\/+/, "").toLowerCase();
    if (!low) return false;
    const cands = [
      dev.prefix ? String(dev.prefix).replace(/^\/+/, "") : "",
      dev.name || "",
      dev.linkId || "",
      dev.peerId || "",
      dev.shortId || "",
    ];
    return cands.some((c) => c && String(c).toLowerCase() === low);
  }

  function resolveMediaDeviceStatus(k, docId) {
    const isNodeCard = Boolean(k?.onDevice || k?.source === "device");
    const cleanDoc = String(docId || "");
    const parts = cleanDoc.split("/").filter(Boolean);
    const firstSeg = parts.length > 1 ? parts[0] : "";

    let matchedDev = firstSeg
      ? (deviceRegistry.getByPrefix(firstSeg) || deviceRegistry.get(firstSeg) || deviceRegistry.getByName(firstSeg))
      : null;

    if (!matchedDev && firstSeg) {
      if (matchesPropDevice(targetDevice, firstSeg)) matchedDev = targetDevice;
      else if (matchesPropDevice(storageDevice, firstSeg)) matchedDev = storageDevice;
    }

    if (matchedDev) {
      const onlineViaNodeHttp = Boolean(isNodeCard && httpDrive);
      const onlineViaOwnerNode = Boolean(matchedDev === targetDevice && httpDrive && isOwner);
      const isOnline = isDevConnected(matchedDev) || onlineViaNodeHttp || onlineViaOwnerNode;
      const seg = (matchedDev.prefix || matchedDev.linkId || matchedDev.name || firstSeg).replace(/^\/+|\/+$/g, "");
      const path = parts.length > 1 ? "/" + parts.slice(1).join("/") : (cleanDoc.startsWith("/") ? cleanDoc : "/" + cleanDoc);
      return {
        isNodeCard,
        isOnline,
        device: matchedDev,
        deviceLabel: matchedDev.name || firstSeg,
        seg: isOnline ? seg : "",
        path: isOnline ? path : "",
      };
    }

    if (isNodeCard) {
      const isOnline = Boolean(httpDrive || isDevConnected(targetDevice));
      const fallbackSeg = (
        targetDevice?.prefix ||
        targetDevice?.linkId ||
        targetLinkId ||
        targetDevice?.name ||
        ""
      ).replace(/^\/+|\/+$/g, "") || "local";
      return {
        isNodeCard: true,
        isOnline,
        device: targetDevice || null,
        deviceLabel: targetDevice?.name || firstSeg || "",
        seg: isOnline ? fallbackSeg : "",
        path: isOnline ? (cleanDoc.startsWith("/") ? cleanDoc : "/" + cleanDoc) : "",
      };
    }

    // Personal GunDB card from an unpaired or currently offline device (e.g. /Mobile/DCIM/...)
    return {
      isNodeCard: false,
      isOnline: false,
      device: null,
      deviceLabel: firstSeg || "",
      seg: "",
      path: "",
    };
  }

  // Deduplicate and parse all media items from Knowledge Base
  $: parsedAllMedia = (() => {
    const _regVer = registryTick;
    const _td = targetDevice;
    const _sd = storageDevice;
    const _hd = httpDrive;
    void _regVer;
    void _td;
    void _sd;
    void _hd;

    const byHashOrDoc = new Map();
    const rawOut = [];
    const deviceNamesLower = new Set(
      (deviceRegistry.list() || [])
        .flatMap((d) => [d?.name, d?.prefix?.replace(/^\/+/, ""), d?.linkId])
        .filter(Boolean)
        .map((s) => String(s).toLowerCase())
    );

    const combinedPersonas = [...(knownPeopleList || []), ...(directPersonas || [])];
    const normDocSuffix = (p) => {
      const clean = String(p || "").replace(/^\/+/, "");
      const slash = clean.indexOf("/");
      return (slash > 0 ? clean.slice(slash + 1) : clean).toLowerCase();
    };
    const seenCreationSuffix = new Set();

    for (const k of knowledgeList || []) {
      const kind = detectMediaKind(k);
      if (!kind) continue;
      const docId = String(k.document_id || k.path || "");
      const dedupKey = k.content_hash || docId;
      if (!dedupKey) continue;
      seenCreationSuffix.add(normDocSuffix(docId));
      const prev = byHashOrDoc.get(dedupKey);
      const isDev = k.source === "device" || !!k.onDevice;
      if (!prev) {
        byHashOrDoc.set(dedupKey, { card: k, idx: rawOut.length });
        rawOut.push(k);
      } else if (isDev && !(prev.card.source === "device" || prev.card.onDevice)) {
        rawOut[prev.idx] = k;
        byHashOrDoc.set(dedupKey, { card: k, idx: prev.idx });
      }
    }

    // Merge unindexed files directly from generated/ (and its story subfolders)
    for (const ck of directCreationCards || []) {
      const docId = String(ck.document_id || ck.path || "");
      if (!docId) continue;
      const suf = normDocSuffix(docId);
      if (seenCreationSuffix.has(suf) || byHashOrDoc.has(docId)) continue;
      seenCreationSuffix.add(suf);
      byHashOrDoc.set(docId, { card: ck, idx: rawOut.length });
      rawOut.push(ck);
    }

    return rawOut.map((k) => {
      const docId = String(k.document_id || k.path || "");
      const mediaKind = detectMediaKind(k);
      const isCreation = Boolean(k.isDirectCreation || isCreationItem(k, docId));
      const takenMs = extractCaptureTimestamp(k, docId);
      const locInfo = extractLocationInfo(k);
      const people = extractCardPeople(k, docId, knownPeopleIndexes.byPhoto, knownPeopleIndexes.byName);
      const fname = docId.split("/").pop() || k.title || "Media";

      let storyFolder = k.storyFolder || "";
      let modelKey = k.modelKey || "";
      let modelLabel = k.modelLabel || "";

      if (isCreation) {
        const parsedSidecar = parseReadmeSidecarText(k.text || k.content || "");
        const enriched = enrichCreationAttributes({
          filename: fname,
          path: docId,
          title: k.title || parsedSidecar.title || "",
          description: parsedSidecar.description || "",
          prompt: parsedSidecar.prompt || "",
          storyFolder,
          isVideo: mediaKind === "video",
          explicitTags: parsedSidecar.explicitTags,
          knownPersonas: combinedPersonas,
        });
        storyFolder = storyFolder || enriched.storyFolder;
        modelKey = modelKey || enriched.modelKey;
        modelLabel = modelLabel || enriched.modelLabel;
        for (const pName of k.matchedPersonas || enriched.personas) {
          if (!people.some((x) => x.toLowerCase() === pName.toLowerCase())) {
            people.push(pName);
          }
        }
        if (enriched.semanticTags.length > 0) {
          k = { ...k, tags: [...(k.tags || []), ...enriched.semanticTags] };
        }
      }

      const peopleLower = new Set(people.map((p) => p.toLowerCase()));
      const locPartsLower = new Set(locInfo.partsLower);
      const docFirstSeg = docId.split("/").filter(Boolean)[0]?.toLowerCase() || "";
      const devStatus = resolveMediaDeviceStatus(k, docId);

      const semanticTags = [];
      const seenTags = new Set();
      for (const rawT of k.tags || []) {
        const cleanT = String(rawT || "").trim().replace(/^#+/, "");
        const lowT = cleanT.toLowerCase();
        if (!lowT || lowT.length < 2 || lowT.length > 32) continue;
        if (NOISE_TAGS.has(lowT)) continue;
        if (peopleLower.has(lowT) || locPartsLower.has(lowT)) continue;
        if (lowT === docFirstSeg || deviceNamesLower.has(lowT)) continue;
        if (/^\d+$/.test(lowT)) continue;
        if (!seenTags.has(lowT)) {
          seenTags.add(lowT);
          semanticTags.push(lowT);
        }
      }

      const isSensitive = Boolean(
        k.sensitive ||
        hasSensitiveTag(k) ||
        hasSensitiveTag(k.tags) ||
        hasSensitiveReadmeContent(k.text || k.content || "")
      );

      const displayTitle = k.title && !isJustFileName(k.title, docId) ? k.title : fname;
      const summary = cleanCardSummary(k, docId);

      return {
        id: k.id || k._id || k.memory_id || docId,
        docId,
        card: k,
        mediaKind,
        isVideo: mediaKind === "video",
        isCreation,
        isSensitive,
        isOffline: !devStatus.isOnline,
        isNodeCard: devStatus.isNodeCard,
        deviceLabel: devStatus.deviceLabel,
        resolvedSeg: devStatus.seg,
        resolvedPath: devStatus.path,
        device: devStatus.device,
        takenMs,
        place: locInfo.place,
        city: locInfo.city,
        country: locInfo.country,
        people,
        storyFolder,
        modelKey,
        modelLabel,
        semanticTags,
        title: displayTitle,
        filename: fname,
        summary,
        duration: Number(k.duration || 0),
        inlinePreview: k.imagePreview || k.image_preview || "",
      };
    });
  })();

  $: creationsTotalCount = parsedAllMedia.filter((m) => m.isCreation).length;
  $: baseMediaPool = includeCreations
    ? parsedAllMedia
    : parsedAllMedia.filter((m) => !m.isCreation);

  $: dispatch("count-change", baseMediaPool.length);

  $: videosInPoolCount = baseMediaPool.filter((m) => m.isVideo).length;
  $: photosInPoolCount = baseMediaPool.length - videosInPoolCount;

  // Smart Auto-Albums computed from baseMediaPool
  $: autoAlbums = (() => {
    const sortedDesc = [...baseMediaPool].sort((a, b) => (b.takenMs || 0) - (a.takenMs || 0));
    const hasAvailThumb = (m) => Boolean(!m.isOffline || m.inlinePreview);
    const pickCoverItem = (arr) => {
      return (
        arr.find((m) => (!m.isSensitive || effectiveContentMode === "fun") && hasAvailThumb(m)) ||
        arr.find((m) => hasAvailThumb(m)) ||
        arr.find((m) => !m.isSensitive || effectiveContentMode === "fun") ||
        arr[0] ||
        null
      );
    };
    const pickCollageItems = (arr, n = 3) => {
      const withThumb = arr.filter((m) => hasAvailThumb(m));
      if (withThumb.length >= n) return withThumb.slice(0, n);
      const rest = arr.filter((m) => !hasAvailThumb(m));
      return [...withThumb, ...rest].slice(0, n);
    };

    // 1. People & Personas albums
    const personBuckets = new Map();
    for (const p of [...(knownPeopleList || []), ...(directPersonas || [])]) {
      if (p && p.name) {
        const key = p.name.trim().toLowerCase();
        const prev = personBuckets.get(key);
        if (!prev) {
          personBuckets.set(key, {
            key,
            title: p.name.trim(),
            avatarUrl: p.avatar_b64 || "",
            items: [],
          });
        } else if (!prev.avatarUrl && p.avatar_b64) {
          prev.avatarUrl = p.avatar_b64;
        }
      }
    }
    for (const m of sortedDesc) {
      for (const pName of m.people) {
        const key = pName.toLowerCase();
        if (!personBuckets.has(key)) {
          personBuckets.set(key, { key, title: pName, avatarUrl: "", items: [] });
        }
        personBuckets.get(key).items.push(m);
      }
    }
    const peopleAlbums = Array.from(personBuckets.values())
      .filter((b) => b.items.length > 0)
      .sort((a, b) => b.items.length - a.items.length || a.title.localeCompare(b.title))
      .map((b) => ({
        kind: "person",
        id: `person:${b.key}`,
        title: b.title,
        count: b.items.length,
        avatarUrl: b.avatarUrl,
        coverItems: pickCollageItems(b.items, 3),
        coverItem: pickCoverItem(b.items),
        itemIds: new Set(b.items.map((x) => x.id)),
      }));

    // 2. Places albums
    const placeBuckets = new Map();
    for (const m of sortedDesc) {
      if (!m.place) continue;
      const key = (m.city || m.place).toLowerCase();
      if (!placeBuckets.has(key)) {
        placeBuckets.set(key, {
          key,
          title: m.city || m.place,
          subtitle: m.country || "",
          items: [],
        });
      }
      placeBuckets.get(key).items.push(m);
    }
    const placeAlbums = Array.from(placeBuckets.values())
      .filter((b) => b.items.length > 0)
      .sort((a, b) => b.items.length - a.items.length || a.title.localeCompare(b.title))
      .map((b) => ({
        kind: "place",
        id: `place:${b.key}`,
        title: b.title,
        subtitle: b.subtitle,
        count: b.items.length,
        coverItems: pickCollageItems(b.items, 3),
        coverItem: pickCoverItem(b.items),
        itemIds: new Set(b.items.map((x) => x.id)),
      }));

    // 3. Moments & Trips (contiguous chronological clusters with >= 3 items within <= 36h gap)
    const datedAsc = sortedDesc.filter((m) => m.takenMs > 0).slice().reverse();
    const rawClusters = [];
    let curCluster = [];
    for (const m of datedAsc) {
      if (!curCluster.length) {
        curCluster = [m];
      } else {
        const prev = curCluster[curCluster.length - 1];
        const gapHours = (m.takenMs - prev.takenMs) / 3600000;
        const sameCity = m.city && prev.city && m.city.toLowerCase() === prev.city.toLowerCase();
        const maxGap = sameCity ? 72 : 30;
        if (gapHours <= maxGap) {
          curCluster.push(m);
        } else {
          rawClusters.push(curCluster);
          curCluster = [m];
        }
      }
    }
    if (curCluster.length) rawClusters.push(curCluster);

    const momentAlbums = rawClusters
      .filter((cl) => {
        const hasPlace = cl.some((x) => Boolean(x.city || x.place));
        return (hasPlace && cl.length >= 3) || cl.length >= 5;
      })
      .map((cl) => {
        const descItems = [...cl].reverse();
        const startDt = new Date(cl[0].takenMs);
        const endDt = new Date(cl[cl.length - 1].takenMs);
        const cityCounts = new Map();
        for (const x of cl) {
          if (x.city) cityCounts.set(x.city, (cityCounts.get(x.city) || 0) + 1);
        }
        let topCity = "";
        let topCnt = 0;
        for (const [c, cnt] of cityCounts.entries()) {
          if (cnt > topCnt) {
            topCity = c;
            topCnt = cnt;
          }
        }
        const locLocale = $locale || undefined;
        const sameDay =
          startDt.getFullYear() === endDt.getFullYear() &&
          startDt.getMonth() === endDt.getMonth() &&
          startDt.getDate() === endDt.getDate();
        const dateLabel = sameDay
          ? startDt.toLocaleDateString(locLocale, { day: "numeric", month: "short", year: "numeric" })
          : `${startDt.toLocaleDateString(locLocale, { day: "numeric", month: "short" })} - ${endDt.toLocaleDateString(locLocale, { day: "numeric", month: "short", year: "numeric" })}`;
        const title = topCity ? `${topCity} · ${dateLabel}` : dateLabel;
        return {
          kind: "moment",
          id: `moment:${cl[0].takenMs}_${cl.length}`,
          title,
          subtitle: topCity ? dateLabel : "",
          endMs: cl[cl.length - 1].takenMs,
          count: descItems.length,
          coverItems: pickCollageItems(descItems, 3),
          coverItem: pickCoverItem(descItems),
          itemIds: new Set(descItems.map((x) => x.id)),
        };
      })
      .sort((a, b) => b.endMs - a.endMs)
      .slice(0, 18);

    // 4. Stories & Series (subfolders inside generated/)
    const storyBuckets = new Map();
    for (const m of sortedDesc) {
      if (!m.storyFolder) continue;
      const key = m.storyFolder.toLowerCase();
      if (!storyBuckets.has(key)) {
        storyBuckets.set(key, {
          key,
          title: m.storyFolder.replace(/_/g, " "),
          rawSlug: m.storyFolder,
          items: [],
        });
      }
      storyBuckets.get(key).items.push(m);
    }
    const storyAlbums = Array.from(storyBuckets.values())
      .filter((b) => b.items.length > 0)
      .sort((a, b) => b.items.length - a.items.length || a.title.localeCompare(b.title))
      .map((b) => ({
        kind: "story",
        id: `story:${b.key}`,
        title: b.title,
        subtitle: `generated/${b.rawSlug}`,
        count: b.items.length,
        coverItems: pickCollageItems(b.items, 3),
        coverItem: pickCoverItem(b.items),
        itemIds: new Set(b.items.map((x) => x.id)),
      }));

    // 5. Creation Models & Engines (when Creations are included)
    const modelBuckets = new Map();
    for (const m of sortedDesc) {
      if (!m.isCreation || !m.modelKey) continue;
      if (!modelBuckets.has(m.modelKey)) {
        modelBuckets.set(m.modelKey, {
          key: m.modelKey,
          title: m.modelLabel || m.modelKey,
          items: [],
        });
      }
      modelBuckets.get(m.modelKey).items.push(m);
    }
    const modelAlbums = Array.from(modelBuckets.values())
      .filter((b) => b.items.length > 0)
      .sort((a, b) => b.items.length - a.items.length)
      .map((b) => ({
        kind: "model",
        id: `model:${b.key}`,
        title: b.title,
        count: b.items.length,
        coverItems: pickCollageItems(b.items, 3),
        coverItem: pickCoverItem(b.items),
        itemIds: new Set(b.items.map((x) => x.id)),
      }));

    // 6. Meaningful Semantic Tags & Scene Categories
    const tagBuckets = new Map();
    for (const m of sortedDesc) {
      for (const t of m.semanticTags) {
        if (!tagBuckets.has(t)) tagBuckets.set(t, []);
        tagBuckets.get(t).push(m);
      }
    }
    const minTagItems = sortedDesc.length >= 12 ? 2 : 1;
    const tagAlbums = Array.from(tagBuckets.entries())
      .filter(([, arr]) => arr.length >= minTagItems)
      .sort((a, b) => b[1].length - a[1].length || a[0].localeCompare(b[0]))
      .slice(0, 28)
      .map(([tag, arr]) => ({
        kind: "tag",
        id: `tag:${tag}`,
        title: `#${tag}`,
        rawTag: tag,
        count: arr.length,
        coverItems: pickCollageItems(arr, 3),
        coverItem: pickCoverItem(arr),
        itemIds: new Set(arr.map((x) => x.id)),
      }));

    return {
      people: peopleAlbums,
      stories: storyAlbums,
      places: placeAlbums,
      moments: momentAlbums,
      models: modelAlbums,
      tags: tagAlbums,
      totalCount:
        peopleAlbums.length +
        storyAlbums.length +
        placeAlbums.length +
        momentAlbums.length +
        modelAlbums.length +
        tagAlbums.length,
    };
  })();

  // Filtered & sorted items for Timeline or Opened Album
  $: filteredMedia = (() => {
    const q = searchQuery.toLowerCase();
    return baseMediaPool
      .filter((m) => {
        if (activeAlbum && activeAlbum.itemIds && !activeAlbum.itemIds.has(m.id)) {
          return false;
        }
        if (mediaTypeFilter === "image" && m.isVideo) return false;
        if (mediaTypeFilter === "video" && !m.isVideo) return false;
        if (!q) return true;
        return (
          m.title.toLowerCase().includes(q) ||
          m.filename.toLowerCase().includes(q) ||
          m.summary.toLowerCase().includes(q) ||
          m.place.toLowerCase().includes(q) ||
          m.people.some((p) => p.toLowerCase().includes(q)) ||
          m.semanticTags.some((t) => t.includes(q))
        );
      })
      .sort((a, b) => {
        if (a.takenMs !== b.takenMs) {
          return sortOrder === "desc"
            ? (b.takenMs || 0) - (a.takenMs || 0)
            : (a.takenMs || 0) - (b.takenMs || 0);
        }
        return sortOrder === "desc"
          ? b.filename.localeCompare(a.filename)
          : a.filename.localeCompare(b.filename);
      });
  })();

  // Progressive rendering limit for smooth 60fps scrolling on thousands of photos
  let renderLimit = 120;
  let scrollContainerEl = null;
  let activeScrubberKey = "";

  $: if (searchQuery !== undefined || activeAlbum !== undefined || mediaTypeFilter !== undefined || sortOrder !== undefined || includeCreations !== undefined) {
    renderLimit = 120;
  }

  $: visibleMediaSlice = filteredMedia.slice(0, renderLimit);

  // Group visible timeline items by Day (with dominant place in header)
  $: timelineGroups = (() => {
    const groups = [];
    const locLocale = $locale || undefined;
    let cur = null;

    for (const m of visibleMediaSlice) {
      let dayKey = "undated";
      let monthKey = "undated";
      let dayLabel = $_("media_undated", {}, "Undated");
      let monthShort = "•";

      if (m.takenMs > 0) {
        const dt = new Date(m.takenMs);
        const y = dt.getFullYear();
        const mo = String(dt.getMonth() + 1).padStart(2, "0");
        const d = String(dt.getDate()).padStart(2, "0");
        dayKey = `${y}-${mo}-${d}`;
        monthKey = `${y}-${mo}`;
        dayLabel = dt.toLocaleDateString(locLocale, {
          day: "numeric",
          month: "long",
          year: "numeric",
          weekday: "short",
        });
        monthShort = dt.toLocaleDateString(locLocale, { month: "short", year: "2-digit" });
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
      if (m.city || m.place) {
        const p = m.city || m.place;
        cur.placesCount.set(p, (cur.placesCount.get(p) || 0) + 1);
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
  })();

  // Full list of Year-Month markers across ALL filteredMedia for the right-side Date Scrubber
  $: monthScrubberItems = (() => {
    const seen = new Map();
    const locLocale = $locale || undefined;
    for (let i = 0; i < filteredMedia.length; i++) {
      const m = filteredMedia[i];
      if (!m.takenMs) continue;
      const dt = new Date(m.takenMs);
      const y = dt.getFullYear();
      const mo = String(dt.getMonth() + 1).padStart(2, "0");
      const key = `${y}-${mo}`;
      if (!seen.has(key)) {
        seen.set(key, {
          key,
          year: y,
          shortLabel: dt.toLocaleDateString(locLocale, { month: "short" }),
          fullLabel: dt.toLocaleDateString(locLocale, { month: "long", year: "numeric" }),
          firstIndex: i,
        });
      }
    }
    return Array.from(seen.values());
  })();

  async function jumpToMonth(scrubberItem) {
    if (!scrubberItem) return;
    if (scrubberItem.firstIndex >= renderLimit) {
      renderLimit = Math.min(filteredMedia.length, scrubberItem.firstIndex + 90);
      await tick();
    }
    activeScrubberKey = scrubberItem.key;
    mobileScrubberOpen = false;
    if (scrollContainerEl) {
      const targetEl = scrollContainerEl.querySelector(`[data-month-anchor="${scrubberItem.key}"]`);
      if (targetEl) {
        targetEl.scrollIntoView({ behavior: "smooth", block: "start" });
      }
    }
  }

  function handleScroll() {
    if (!scrollContainerEl) return;
    const { scrollTop, scrollHeight, clientHeight } = scrollContainerEl;
    if (scrollHeight - (scrollTop + clientHeight) < 900 && renderLimit < filteredMedia.length) {
      renderLimit = Math.min(filteredMedia.length, renderLimit + 90);
    }
    if (monthScrubberItems.length > 1) {
      const anchors = scrollContainerEl.querySelectorAll("[data-month-anchor]");
      const containerTop = scrollContainerEl.getBoundingClientRect().top;
      let currentKey = monthScrubberItems[0]?.key || "";
      for (const el of anchors) {
        const rect = el.getBoundingClientRect();
        if (rect.top - containerTop <= 110) {
          currentKey = el.getAttribute("data-month-anchor") || currentKey;
        } else {
          break;
        }
      }
      activeScrubberKey = currentKey;
    }
  }

  // Thumbnail & P2P URL resolution with concurrency-capped lazy queue
  let thumbUrls = {};
  let imgFailed = {};
  let inlineImgFailed = {};
  let thumbBlobFailed = {};
  let personAvatarFailed = {};
  const thumbInFlight = new Set();
  const thumbQueue = [];
  let activeThumbWorkers = 0;
  const MAX_THUMB_CONCURRENCY = 4;

  function getDevSegment() {
    const dev = targetDevice && targetDevice.drive ? targetDevice : null;
    let seg = (dev?.prefix || dev?.linkId || targetLinkId || targetDevice?.linkId || targetDevice?.name || "").trim();
    if (!seg) {
      try {
        const first = deviceRegistry.list().find((d) => d.drive && d.drive.isConnected) || deviceRegistry.list()[0];
        if (first) seg = first.prefix || first.linkId || "";
      } catch (_) {}
    }
    return seg.replace(/^\/+|\/+$/g, "") || "local";
  }

  function previewSegAndPath(docId) {
    const parts = String(docId || "").split("/").filter(Boolean);
    if (parts.length > 1) {
      const dev = deviceRegistry.getByPrefix(parts[0]) || deviceRegistry.get(parts[0]) || deviceRegistry.getByName(parts[0]);
      if (dev) {
        const seg = (dev.prefix || dev.linkId || dev.name || "").replace(/^\/+|\/+$/g, "");
        if (seg) return { seg, path: "/" + parts.slice(1).join("/"), device: dev };
      }
    }
    return {
      seg: getDevSegment(),
      path: String(docId || "").startsWith("/") ? docId : "/" + docId,
      device: null,
    };
  }

  function enqueueThumbFetch(docId, size = 480) {
    if (!docId || !thumbFetcher || thumbUrls[docId] !== undefined || thumbInFlight.has(docId)) return;
    thumbInFlight.add(docId);
    thumbQueue.push({ docId, size });
    pumpThumbQueue();
  }

  function pumpThumbQueue() {
    while (activeThumbWorkers < MAX_THUMB_CONCURRENCY && thumbQueue.length > 0) {
      const next = thumbQueue.shift();
      if (!next) break;
      activeThumbWorkers++;
      thumbFetcher(next.docId, next.size)
        .then((url) => {
          if (url) {
            thumbUrls = { ...thumbUrls, [next.docId]: url };
          } else {
            thumbUrls = { ...thumbUrls, [next.docId]: "" };
          }
        })
        .catch(() => {
          thumbUrls = { ...thumbUrls, [next.docId]: "" };
        })
        .finally(() => {
          thumbInFlight.delete(next.docId);
          activeThumbWorkers--;
          pumpThumbQueue();
        });
    }
  }

  function getItemThumbUrl(
    item,
    size = 480,
    thumbsMap = thumbUrls,
    failedMap = imgFailed,
    inlineFailedMap = inlineImgFailed,
    blobFailedMap = thumbBlobFailed
  ) {
    if (!item) return "";
    const validInline = item.inlinePreview && !inlineFailedMap[item.id] ? item.inlinePreview : "";

    // Offline device: never attempt to load full/p2p photo over network.
    // Show embedded base64 preview from DB if available, otherwise offline placeholder.
    if (item.isOffline) {
      return validInline;
    }

    const validBlob = thumbsMap[item.docId] && !blobFailedMap[item.id] ? thumbsMap[item.docId] : "";
    if (validBlob) return validBlob;
    if (failedMap[item.id]) return validInline;

    if (item.isVideo) {
      if (validInline) return validInline;
      if (item.isCreation && item.resolvedSeg && item.resolvedPath && !failedMap[item.id]) {
        const coverPath = item.resolvedPath.replace(/\.[^/.]+$/, ".cover.jpg");
        return `/p2p/${item.resolvedSeg}${encodeURI(coverPath)}?resize=true&width=${size}&height=${size}`;
      }
      enqueueThumbFetch(item.docId, size);
      return "";
    }

    if (item.device && item.device.local) {
      if (validInline) return validInline;
      enqueueThumbFetch(item.docId, size);
      return "";
    }

    if (item.resolvedSeg && item.resolvedPath) {
      return `/p2p/${item.resolvedSeg}${encodeURI(item.resolvedPath)}?resize=true&width=${size}&height=${size}`;
    }

    if (item.isNodeCard && item.docId) {
      enqueueThumbFetch(item.docId, size);
    }
    return validInline;
  }

  function handleThumbError(item, currentUrl = "") {
    if (!item) return;
    if (currentUrl && item.inlinePreview && currentUrl === item.inlinePreview) {
      if (!inlineImgFailed[item.id]) {
        inlineImgFailed = { ...inlineImgFailed, [item.id]: true };
      }
      return;
    }
    if (currentUrl && thumbUrls[item.docId] && currentUrl === thumbUrls[item.docId]) {
      if (!thumbBlobFailed[item.id]) {
        thumbBlobFailed = { ...thumbBlobFailed, [item.id]: true };
      }
      return;
    }
    if (!imgFailed[item.id]) {
      imgFailed = { ...imgFailed, [item.id]: true };
      if (!item.isOffline) {
        enqueueThumbFetch(item.docId, 480);
      }
    }
  }

  function lazyObserveTile(node, item) {
    let obs = null;
    if (typeof IntersectionObserver !== "undefined" && item && !item.isOffline) {
      obs = new IntersectionObserver(
        (entries) => {
          for (const ent of entries) {
            if (ent.isIntersecting) {
              if (
                item &&
                !item.isOffline &&
                (item.isNodeCard || item.isVideo || (item.device && item.device.local && !item.inlinePreview))
              ) {
                enqueueThumbFetch(item.docId, 480);
              }
              if (obs) {
                obs.disconnect();
                obs = null;
              }
            }
          }
        },
        { rootMargin: "280px" }
      );
      obs.observe(node);
    }
    return {
      destroy() {
        if (obs) obs.disconnect();
      },
    };
  }

  function isBlurredInCurrentMode(item) {
    return Boolean(item && item.isSensitive && effectiveContentMode !== "fun");
  }

  // Toast message for Work-mode sensitive lock
  let sensitiveNotice = "";
  let sensitiveNoticeTimer = null;
  function showSensitiveWorkNotice() {
    sensitiveNotice = $_("sensitive_work_mode_blur", {}, "Sensitive preview hidden in Work mode. Switch to Fun mode to view.");
    if (sensitiveNoticeTimer) clearTimeout(sensitiveNoticeTimer);
    sensitiveNoticeTimer = setTimeout(() => {
      sensitiveNotice = "";
    }, 3200);
  }

  function escapeHtml(str) {
    return String(str || "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function buildSlideCaptionHtml(item) {
    const metaParts = [];
    if (item.place) metaParts.push(`📍 ${escapeHtml(item.place)}`);
    if (item.people && item.people.length) metaParts.push(`👤 ${escapeHtml(item.people.join(", "))}`);
    if (item.takenMs > 0) {
      metaParts.push(escapeHtml(new Date(item.takenMs).toLocaleDateString($locale || undefined, { day: "numeric", month: "short", year: "numeric" })));
    }
    const metaLine = metaParts.length ? `<div style="font-size:12px;opacity:0.78;margin-bottom:4px;">${metaParts.join(" · ")}</div>` : "";
    const titleHtml = item.title && item.title !== item.filename ? `<h4>${escapeHtml(item.title)}</h4>` : "";
    const descHtml = item.summary ? `<p>${escapeHtml(item.summary)}</p>` : "";
    if (!metaLine && !titleHtml && !descHtml) return "";
    return `<div class="omd-gallery-caption">${metaLine}${titleHtml}${descHtml}</div>`;
  }

  // Scoped Lightbox (ONLY items within the clicked group or active auto-album!)
  let lgMediaInstance = null;
  let lgMediaPagination = null;

  function buildPhotoSlide(item) {
    const vs = getViewerSettings();
    const { seg, path, device } = previewSegAndPath(item.docId);
    const isLocalNoSw = Boolean(device && device.local);
    const p2pBase = item.docId.startsWith("/") && !isLocalNoSw ? `/p2p/${seg}${encodeURI(path)}` : "";
    const cachedHiRes = thumbUrls[`${item.docId}_full`] || "";
    const thumb = thumbUrls[item.docId] || item.inlinePreview || (p2pBase ? `${p2pBase}?resize=true&width=360&height=360` : "");
    let src = cachedHiRes;
    if (!src) {
      if (p2pBase) {
        src = vs.downscaleImages
          ? `${p2pBase}?resize=true&width=${vs.scaleTo}&height=${vs.scaleTo}`
          : p2pBase;
      } else {
        src = thumb || `/api/device_memory_thumb?path=${encodeURIComponent(item.docId)}&size=1600`;
      }
    }
    const downloadUrl = p2pBase ? `${p2pBase}?download=true` : src;
    return {
      docId: item.docId,
      src,
      thumb,
      downloadUrl,
      subHtml: buildSlideCaptionHtml(item),
    };
  }

  function openScopedPhotoGallery(clickedItem, scopeList) {
    if (!clickedItem || clickedItem.isOffline) return;
    if (isBlurredInCurrentMode(clickedItem)) {
      showSensitiveWorkNotice();
      return;
    }
    const photosInScope = (scopeList || []).filter(
      (x) => !x.isVideo && !x.isOffline && !isBlurredInCurrentMode(x)
    );
    if (!photosInScope.length) return;

    const slides = photosInScope.map(buildPhotoSlide);
    let startIndex = photosInScope.findIndex((x) => x.id === clickedItem.id);
    if (startIndex < 0) startIndex = 0;

    if (lgMediaPagination) {
      try { lgMediaPagination.cleanup(); } catch (_) {}
      lgMediaPagination = null;
    }
    if (lgMediaInstance) {
      try { lgMediaInstance.destroy(); } catch (_) {}
      lgMediaInstance = null;
    }

    const container = document.createElement("div");
    document.body.appendChild(container);
    lgMediaPagination = setupGalleryPagination(container, slides, startIndex);

    lgMediaInstance = lightGallery(container, {
      licenseKey: "GPLv3",
      dynamic: true,
      dynamicEl: slides,
      download: true,
      counter: true,
      closable: true,
      showCloseIcon: true,
      escKey: true,
      swipeToClose: true,
      slideEndAnimation: true,
      mobileSettings: {
        controls: false,
        showCloseIcon: true,
        download: true,
      },
    });
    lgMediaPagination.setInstance(lgMediaInstance);

    let closedViaBack = false;
    if (viewerHistoryToken) {
      dismissSubViewHistory(viewerHistoryToken, { popHistory: false });
      viewerHistoryToken = "";
    }
    viewerHistoryToken = pushSubViewHistory("media-lightbox", () => {
      closedViaBack = true;
      viewerHistoryToken = "";
      if (lgMediaInstance) {
        try { lgMediaInstance.closeGallery(); } catch (_) {}
      }
    });

    container.addEventListener("lgAfterClose", () => {
      try {
        if (lgMediaPagination) lgMediaPagination.cleanup();
        if (lgMediaInstance) lgMediaInstance.destroy();
      } catch (_) {}
      lgMediaPagination = null;
      lgMediaInstance = null;
      if (container.parentNode) container.parentNode.removeChild(container);
      if (viewerHistoryToken && !closedViaBack) {
        const tok = viewerHistoryToken;
        viewerHistoryToken = "";
        dismissSubViewHistory(tok, { popHistory: true });
      }
    });

    lgMediaInstance.openGallery(startIndex);

    // Upgrade local/remote slides via thumbFetcher(docId, 1600) in background if needed
    if (thumbFetcher) {
      (async () => {
        const order = [startIndex, ...slides.map((_, idx) => idx).filter((idx) => idx !== startIndex)];
        for (const idx of order) {
          if (!document.querySelector(".lg-container.lg-show")) return;
          const pItem = photosInScope[idx];
          if (!pItem) continue;
          const { device } = previewSegAndPath(pItem.docId);
          if (device && device.local && !thumbUrls[`${pItem.docId}_full`]) {
            try {
              const hiUrl = await thumbFetcher(pItem.docId, 1600);
              if (hiUrl) {
                thumbUrls[`${pItem.docId}_full`] = hiUrl;
                slides[idx].src = hiUrl;
                slides[idx].downloadUrl = hiUrl;
                if (lgMediaPagination) lgMediaPagination.updateItems(slides);
              }
            } catch (_) {}
          }
        }
      })();
    }
  }

  // Scoped Fullscreen Video Player (ONLY videos within the clicked group or active auto-album!)
  let activeVideoModal = null; // { playlist: [], startIndex: 0 }
  let localVideoBlobUrls = [];

  function closeVideoModal({ fromBack = false } = {}) {
    activeVideoModal = null;
    for (const u of localVideoBlobUrls) {
      try { URL.revokeObjectURL(u); } catch (_) {}
    }
    localVideoBlobUrls = [];
    if (viewerHistoryToken) {
      const tok = viewerHistoryToken;
      viewerHistoryToken = "";
      dismissSubViewHistory(tok, { popHistory: !fromBack });
    }
  }

  async function resolvePlayableVideoUrl(item) {
    const { seg, path, device } = previewSegAndPath(item.docId);
    if (device && device.local && device.drive) {
      try {
        if (typeof device.drive.getFileContent === "function") {
          const blob = await device.drive.getFileContent(path);
          if (blob) {
            const u = URL.createObjectURL(blob);
            localVideoBlobUrls.push(u);
            return u;
          }
        }
        if (typeof device.drive.readFile === "function") {
          const res = await device.drive.readFile(path);
          const b64 = res && res.data ? res.data : (typeof res === "string" ? res : null);
          if (b64) {
            const bin = atob(b64.replace(/\s+/g, ""));
            const u8 = new Uint8Array(bin.length);
            for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
            const u = URL.createObjectURL(new Blob([u8], { type: item.card?.mimeType || "video/mp4" }));
            localVideoBlobUrls.push(u);
            return u;
          }
        }
      } catch (_) {}
    }
    if (item.docId.startsWith("/")) {
      return `/p2p/${seg}${encodeURI(path)}`;
    }
    return `/api/device_memory_file?path=${encodeURIComponent(item.docId)}`;
  }

  async function openScopedVideoPlayer(clickedItem, scopeList) {
    if (!clickedItem || clickedItem.isOffline) return;
    if (isBlurredInCurrentMode(clickedItem)) {
      showSensitiveWorkNotice();
      return;
    }
    const videosInScope = (scopeList || []).filter(
      (x) => x.isVideo && !x.isOffline && !isBlurredInCurrentMode(x)
    );
    if (!videosInScope.length) return;

    const clickedIdx = Math.max(0, videosInScope.findIndex((x) => x.id === clickedItem.id));
    const firstUrl = await resolvePlayableVideoUrl(videosInScope[clickedIdx]);
    const playlist = videosInScope.map((v, idx) => {
      const { seg, path } = previewSegAndPath(v.docId);
      const defaultUrl = v.docId.startsWith("/") ? `/p2p/${seg}${encodeURI(path)}` : "";
      return {
        url: idx === clickedIdx ? firstUrl : defaultUrl,
        poster: getItemThumbUrl(v, 640),
        name: v.filename,
        title: v.title,
        description: [v.place ? `📍 ${v.place}` : "", v.summary].filter(Boolean).join(" · "),
      };
    });

    if (viewerHistoryToken) {
      dismissSubViewHistory(viewerHistoryToken, { popHistory: false });
      viewerHistoryToken = "";
    }
    viewerHistoryToken = pushSubViewHistory("media-video", () => {
      viewerHistoryToken = "";
      closeVideoModal({ fromBack: true });
    });

    activeVideoModal = {
      playlist,
      startIndex: clickedIdx,
    };
  }

  function handleTileClick(item, scopeList) {
    if (!item || item.isOffline) return;
    if (isBlurredInCurrentMode(item)) {
      showSensitiveWorkNotice();
      return;
    }
    if (item.isVideo) {
      openScopedVideoPlayer(item, scopeList);
    } else {
      openScopedPhotoGallery(item, scopeList);
    }
  }

  function openAlbum(album) {
    const hadAlbum = Boolean(activeAlbum);
    activeAlbum = album;
    mobileScrubberOpen = false;
    if (!hadAlbum) {
      if (albumHistoryToken) {
        dismissSubViewHistory(albumHistoryToken, { popHistory: false });
      }
      albumHistoryToken = pushSubViewHistory("media-album", () => {
        albumHistoryToken = "";
        activeAlbum = null;
        mobileScrubberOpen = false;
      });
    }
    if (scrollContainerEl) {
      scrollContainerEl.scrollTo({ top: 0, behavior: "auto" });
    }
  }

  function closeAlbum() {
    activeAlbum = null;
    mobileScrubberOpen = false;
    if (albumHistoryToken) {
      const tok = albumHistoryToken;
      albumHistoryToken = "";
      dismissSubViewHistory(tok, { popHistory: true });
    }
  }

  function formatDuration(sec) {
    if (!sec || sec <= 0) return "";
    const m = Math.floor(sec / 60);
    const s = Math.round(sec % 60);
    return `${m}:${String(s).padStart(2, "0")}`;
  }

  onDestroy(() => {
    if (creationsScanAbort) {
      try { creationsScanAbort.abort(); } catch (_) {}
    }
    if (viewerHistoryToken) {
      dismissSubViewHistory(viewerHistoryToken, { popHistory: false });
      viewerHistoryToken = "";
    }
    if (albumHistoryToken) {
      dismissSubViewHistory(albumHistoryToken, { popHistory: false });
      albumHistoryToken = "";
    }
    if (unsubRegistry) {
      try { unsubRegistry(); } catch (_) {}
    }
    if (searchTimer) clearTimeout(searchTimer);
    if (sensitiveNoticeTimer) clearTimeout(sensitiveNoticeTimer);
    if (lgMediaPagination) {
      try { lgMediaPagination.cleanup(); } catch (_) {}
    }
    if (lgMediaInstance) {
      try { lgMediaInstance.destroy(); } catch (_) {}
    }
    closeVideoModal({ fromBack: true });
  });
</script>

<svelte:window bind:innerWidth on:keydown={(e) => {
  if (e.key === "Escape" && activeVideoModal) {
    closeVideoModal();
  }
}} />

<div class="media-library-view" class:dark-theme={isDarkTheme} class:light-theme={!isDarkTheme}>
  <!-- Top Header -->
  <header class="media-header" class:overlay-header={isOverlayHeader}>
    <div class="header-left">
      {#if !sidebarOpen || isOverlayHeader}
        <button
          type="button"
          class="header-icon-btn"
          title={$_("expand", {}, "Open sidebar")}
          on:click={() => dispatch("toggle-sidebar")}
        >
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
            <line x1="9" y1="3" x2="9" y2="21" />
          </svg>
        </button>
      {/if}

      <!-- Segmented Switcher: Timeline | Albums -->
      <div class="view-seg-pill" role="tablist">
        <button
          type="button"
          role="tab"
          class="seg-btn"
          class:active={activeTab === "timeline" && !activeAlbum}
          on:click={() => setTab("timeline")}
        >
          <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="3" y="3" width="7" height="7" rx="1" />
            <rect x="14" y="3" width="7" height="7" rx="1" />
            <rect x="3" y="14" width="7" height="7" rx="1" />
            <rect x="14" y="14" width="7" height="7" rx="1" />
          </svg>
          <span>{$_("media_timeline", {}, "Timeline")}</span>
          <span class="seg-count">{baseMediaPool.length}</span>
        </button>
        <button
          type="button"
          role="tab"
          class="seg-btn"
          class:active={activeTab === "albums" || Boolean(activeAlbum)}
          on:click={() => setTab("albums")}
        >
          <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M12 2l2.4 7.4H22l-6.2 4.5 2.4 7.4L12 16.8l-6.2 4.5 2.4-7.4L2 9.4h7.6z" />
          </svg>
          <span>{$_("media_auto_albums", {}, "Auto-Albums")}</span>
          {#if autoAlbums.totalCount > 0}
            <span class="seg-count">{autoAlbums.totalCount}</span>
          {/if}
        </button>
      </div>
    </div>

    <!-- Center Search -->
    <div class="header-search">
      <svg class="search-icon" viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
        <circle cx="11" cy="11" r="8" />
        <line x1="21" y1="21" x2="16.65" y2="16.65" />
      </svg>
      <input
        type="text"
        class="search-input"
        value={searchInput}
        on:input={onSearchInput}
        placeholder={$_("media_search_placeholder", {}, "Search places, people, scenes, text...")}
      />
      {#if searchInput}
        <button type="button" class="search-clear" on:click={clearSearch} title={$_("clear", {}, "Clear")}>
          ✕
        </button>
      {/if}
    </div>

    <!-- Right Controls -->
    <div class="header-right">
      {#if creationsTotalCount > 0 || includeCreations}
        <button
          type="button"
          class="pill-toggle-btn"
          class:active={includeCreations}
          title={$_("media_include_creations_hint", {}, "Include AI-generated images and videos from Creations")}
          on:click={toggleIncludeCreations}
        >
          <span>+ {$_("generated", {}, "Creations")}</span>
        </button>
      {/if}

      {#if includeCreations}
        <button
          type="button"
          class="pill-toggle-btn mode-pill"
          class:fun={effectiveContentMode === "fun"}
          title="Work / Fun"
          on:click={toggleContentMode}
        >
          <span>{effectiveContentMode === "fun" ? "✨ Fun" : "💼 Work"}</span>
        </button>
      {/if}

      <button
        type="button"
        class="header-icon-btn sort-order-btn"
        class:is-active={sortOrder === "asc"}
        title={sortOrder === "desc" ? $_("sort_date_desc", {}, "Date (newest first)") : $_("sort_date_asc", {}, "Date (oldest first)")}
        aria-label={sortOrder === "desc" ? $_("sort_date_desc", {}, "Date (newest first)") : $_("sort_date_asc", {}, "Date (oldest first)")}
        on:click={toggleSortOrder}
      >
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          {#if sortOrder === "desc"}
            <path d="M4 6h9M4 12h7M4 18h5" />
            <path d="M16 15l3 3 3-3" />
            <path d="M19 6v12" />
          {:else}
            <path d="M4 6h5M4 12h7M4 18h9" />
            <path d="M16 9l3-3 3 3" />
            <path d="M19 18V6" />
          {/if}
        </svg>
      </button>

      {#if isMobile && (activeTab === "timeline" || activeAlbum) && monthScrubberItems.length >= 2}
        <button
          type="button"
          class="header-icon-btn scrubber-toggle-btn"
          class:is-active={mobileScrubberOpen}
          title={$_("media_jump_date", {}, "Jump to date")}
          aria-label={$_("media_jump_date", {}, "Jump to date")}
          on:click={() => (mobileScrubberOpen = !mobileScrubberOpen)}
        >
          <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <rect x="3" y="4" width="18" height="18" rx="2" ry="2" />
            <line x1="16" y1="2" x2="16" y2="6" />
            <line x1="8" y1="2" x2="8" y2="6" />
            <line x1="3" y1="10" x2="21" y2="10" />
          </svg>
        </button>
      {/if}

      {#if !isMobile}
        <button
          type="button"
          class="header-icon-btn"
          title={gridFit === "cover" ? $_("media_fit_natural", {}, "Show full aspect ratio") : $_("media_fit_square", {}, "Square grid")}
          on:click={toggleGridFit}
        >
          <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2">
            {#if gridFit === "cover"}
              <rect x="3" y="5" width="18" height="14" rx="2" />
            {:else}
              <rect x="4" y="4" width="16" height="16" rx="2" />
            {/if}
          </svg>
        </button>
      {/if}

      <button
        type="button"
        class="header-icon-btn"
        title={$_("index_this_device", {}, "Index this device")}
        on:click={() => (showDeviceIndexModal = true)}
      >
        <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2">
          <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
          <polyline points="7 10 12 15 17 10" />
          <line x1="12" y1="15" x2="12" y2="3" />
        </svg>
      </button>

      {#if !isMobile}
        <button
          type="button"
          class="header-icon-btn"
          title={$_("refresh", {}, "Refresh")}
          on:click={() => {
            loadKnownPeople();
            triggerDirectCreationsScan(true);
            dispatch("refresh");
          }}
        >
          <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2">
            <polyline points="23 4 23 10 17 10" />
            <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10" />
          </svg>
        </button>
      {/if}

      <button
        type="button"
        class="header-icon-btn close-btn"
        title={$_("close", {}, "Close")}
        on:click={() => dispatch("close")}
      >
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
          <line x1="18" y1="6" x2="6" y2="18" />
          <line x1="6" y1="6" x2="18" y2="18" />
        </svg>
      </button>
    </div>
  </header>

  <!-- Live On-Device Indexing Progress Bar -->
  {#if ($indexerStore.status === "running" || $indexerStore.status === "paused") && !showDeviceIndexModal}
    {@const bgPct = $indexerStore.total > 0 ? Math.min(100, Math.round(($indexerStore.processed / $indexerStore.total) * 100)) : 0}
    <div class="bg-index-bar" role="status">
      <button type="button" class="bg-index-info" on:click={() => (showDeviceIndexModal = true)}>
        <span class="bg-index-dot" class:paused={$indexerStore.status === "paused"}></span>
        <span>
          {$_("device_index_bg_status", { processed: $indexerStore.processed, total: $indexerStore.total }, `Indexing: ${$indexerStore.processed}/${$indexerStore.total}`)} ({bgPct}%)
        </span>
      </button>
      <div class="bg-index-actions">
        {#if $indexerStore.status === "running"}
          <button type="button" class="bg-index-btn" on:click={() => clientIndexer.pause()}>{$_("pause", {}, "Pause")}</button>
        {:else}
          <button type="button" class="bg-index-btn" on:click={() => clientIndexer.resume()}>{$_("resume", {}, "Resume")}</button>
        {/if}
      </div>
    </div>
  {/if}

  {#if sensitiveNotice}
    <div class="sensitive-toast" transition:fade={{ duration: 160 }}>
      <span>🔒 {sensitiveNotice}</span>
    </div>
  {/if}

  <!-- Secondary Filter Strip (Videos / Photos filter or Active Album Banner) -->
  {#if activeAlbum || videosInPoolCount > 0}
    <div class="sub-filter-bar">
      {#if activeAlbum}
        <div class="album-crumb">
          <button type="button" class="back-album-btn" on:click={closeAlbum}>
            <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2.2">
              <polyline points="15 18 9 12 15 6" />
            </svg>
            <span>{$_("media_all_albums", {}, "All albums")}</span>
          </button>
          <span class="crumb-sep">/</span>
          <span class="crumb-title">{activeAlbum.title}</span>
          <span class="crumb-count">({filteredMedia.length})</span>
        </div>
      {/if}

      {#if videosInPoolCount > 0}
        <div class="type-pills">
          <button type="button" class="type-pill" class:active={mediaTypeFilter === "all"} on:click={() => (mediaTypeFilter = "all")}>
            {$_("filter_all", {}, "All")} ({baseMediaPool.length})
          </button>
          <button type="button" class="type-pill" class:active={mediaTypeFilter === "image"} on:click={() => (mediaTypeFilter = "image")}>
            🖼️ {$_("filter_images", {}, "Photos")} ({photosInPoolCount})
          </button>
          <button type="button" class="type-pill" class:active={mediaTypeFilter === "video"} on:click={() => (mediaTypeFilter = "video")}>
            🎬 {$_("media_videos", {}, "Videos")} ({videosInPoolCount})
          </button>
        </div>
      {/if}
    </div>
  {/if}

  <!-- Main Scroll Body -->
  <div class="media-body-wrap">
    <div class="media-scroll" bind:this={scrollContainerEl} on:scroll={handleScroll}>
      {#if loading && parsedAllMedia.length === 0}
        <div class="empty-state">
          <div class="spinner-ring"></div>
          <p>{$_("loading", {}, "Loading...")}</p>
        </div>
      {:else if baseMediaPool.length === 0}
        <div class="empty-state" in:fade={{ duration: 180 }}>
          <div class="empty-icon">🖼️</div>
          <h3>{$_("media_empty_title", {}, "No indexed photos or videos yet")}</h3>
          <p>
            {$_("media_empty_desc", {}, "Index photos and videos on your phone or folder to automatically organize them by timeline, places, people, and themes.")}
          </p>
          <div class="empty-actions">
            <button type="button" class="primary-action-btn" on:click={() => (showDeviceIndexModal = true)}>
              {$_("index_this_device", {}, "Index this device")}
            </button>
            {#if isOwner}
              <button type="button" class="secondary-action-btn" on:click={() => dispatch("index-folder")}>
                {$_("index_folder", {}, "Index folder")}
              </button>
            {/if}
            {#if !includeCreations && creationsTotalCount > 0}
              <button type="button" class="secondary-action-btn" on:click={toggleIncludeCreations}>
                + {$_("generated", {}, "Creations")} ({creationsTotalCount})
              </button>
            {/if}
          </div>
        </div>

      {:else if activeTab === "albums" && !activeAlbum && !searchQuery}
        <!-- AUTO-ALBUMS SHOWCASE -->
        <div class="albums-showcase" in:fade={{ duration: 150 }}>
          <!-- 1. People Shelf -->
          {#if autoAlbums.people.length > 0}
            <section class="album-section">
              <div class="section-head">
                <h3>👥 {$_("media_people", {}, "People")}</h3>
                <span class="section-badge">{autoAlbums.people.length}</span>
              </div>
              <div class="people-shelf">
                {#each autoAlbums.people as pAlbum (pAlbum.id)}
                  {@const fallbackUrl = pAlbum.coverItem ? getItemThumbUrl(pAlbum.coverItem, 360, thumbUrls, imgFailed, inlineImgFailed, thumbBlobFailed) : ""}
                  {@const blurredCover = pAlbum.coverItem ? isBlurredInCurrentMode(pAlbum.coverItem) : false}
                  <button type="button" class="person-tile" on:click={() => openAlbum(pAlbum)}>
                    <div class="person-ring">
                      {#if pAlbum.avatarUrl && !personAvatarFailed[pAlbum.id]}
                        <img
                          src={pAlbum.avatarUrl}
                          alt={pAlbum.title}
                          loading="lazy"
                          on:error={() => { personAvatarFailed = { ...personAvatarFailed, [pAlbum.id]: true }; }}
                        />
                      {:else if fallbackUrl}
                        <img
                          src={fallbackUrl}
                          alt={pAlbum.title}
                          loading="lazy"
                          class:blurred-img={blurredCover}
                          on:error={() => handleThumbError(pAlbum.coverItem, fallbackUrl)}
                        />
                      {:else}
                        <span class="person-placeholder">{pAlbum.title.slice(0, 1).toUpperCase()}</span>
                      {/if}
                    </div>
                    <span class="person-tile-name">{pAlbum.title}</span>
                    <span class="person-tile-count">{pAlbum.count}</span>
                  </button>
                {/each}
              </div>
            </section>
          {/if}

          <!-- 1.5 Stories & Series (subfolders inside generated/) -->
          {#if autoAlbums.stories && autoAlbums.stories.length > 0}
            <section class="album-section">
              <div class="section-head">
                <h3>🎬 {$_("media_stories", {}, "Stories & Series")}</h3>
                <span class="section-badge">{autoAlbums.stories.length}</span>
              </div>
              <div class="album-cards-grid moments-grid">
                {#each autoAlbums.stories as sAlbum (sAlbum.id)}
                  <button type="button" class="album-collage-card" on:click={() => openAlbum(sAlbum)}>
                    <div class="collage-cover count-{Math.min(3, sAlbum.coverItems.length)}">
                      {#each sAlbum.coverItems.slice(0, 3) as cItem, idx (cItem.id)}
                        {@const cUrl = getItemThumbUrl(cItem, idx === 0 ? 480 : 320, thumbUrls, imgFailed, inlineImgFailed, thumbBlobFailed)}
                        <div class="collage-cell cell-{idx}" use:lazyObserveTile={cItem}>
                          {#if cUrl}
                            <img
                              src={cUrl}
                              alt=""
                              loading="lazy"
                              class:blurred-img={isBlurredInCurrentMode(cItem)}
                              on:error={() => handleThumbError(cItem, cUrl)}
                            />
                          {:else}
                            <div class="tile-fallback">🎬</div>
                          {/if}
                        </div>
                      {/each}
                    </div>
                    <div class="album-card-meta">
                      <span class="album-card-title">{sAlbum.title}</span>
                      <span class="album-card-count">{sAlbum.count}</span>
                    </div>
                  </button>
                {/each}
              </div>
            </section>
          {/if}

          <!-- 2. Moments & Trips -->
          {#if autoAlbums.moments.length > 0}
            <section class="album-section">
              <div class="section-head">
                <h3>🗓️ {$_("media_moments", {}, "Moments & Trips")}</h3>
                <span class="section-badge">{autoAlbums.moments.length}</span>
              </div>
              <div class="album-cards-grid moments-grid">
                {#each autoAlbums.moments as mAlbum (mAlbum.id)}
                  <button type="button" class="album-collage-card" on:click={() => openAlbum(mAlbum)}>
                    <div class="collage-cover count-{Math.min(3, mAlbum.coverItems.length)}">
                      {#each mAlbum.coverItems.slice(0, 3) as cItem, idx (cItem.id)}
                        {@const cUrl = getItemThumbUrl(cItem, idx === 0 ? 480 : 320, thumbUrls, imgFailed, inlineImgFailed, thumbBlobFailed)}
                        <div class="collage-cell cell-{idx}" use:lazyObserveTile={cItem}>
                          {#if cUrl}
                            <img
                              src={cUrl}
                              alt=""
                              loading="lazy"
                              class:blurred-img={isBlurredInCurrentMode(cItem)}
                              on:error={() => handleThumbError(cItem, cUrl)}
                            />
                          {:else if cItem.isOffline}
                            <div class="tile-fallback offline-placeholder compact" title={$_("media_device_offline", {}, "Device offline")}>
                              <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.8">
                                <line x1="2" y1="2" x2="22" y2="22" />
                                <path d="M17 17H7a2 2 0 0 1-2-2V5c0-.6.26-1.14.68-1.5" />
                                <path d="M9 2h8a2 2 0 0 1 2 2v9" />
                              </svg>
                            </div>
                          {:else}
                            <div class="tile-fallback">🖼️</div>
                          {/if}
                        </div>
                      {/each}
                    </div>
                    <div class="album-card-meta">
                      <span class="album-card-title">{mAlbum.title}</span>
                      <span class="album-card-count">{mAlbum.count}</span>
                    </div>
                  </button>
                {/each}
              </div>
            </section>
          {/if}

          <!-- 3. Places -->
          {#if autoAlbums.places.length > 0}
            <section class="album-section">
              <div class="section-head">
                <h3>📍 {$_("media_places", {}, "Places")}</h3>
                <span class="section-badge">{autoAlbums.places.length}</span>
              </div>
              <div class="album-cards-grid">
                {#each autoAlbums.places as plAlbum (plAlbum.id)}
                  {@const cItem = plAlbum.coverItem}
                  {@const cUrl = cItem ? getItemThumbUrl(cItem, 480, thumbUrls, imgFailed, inlineImgFailed, thumbBlobFailed) : ""}
                  <button type="button" class="album-single-card" on:click={() => openAlbum(plAlbum)}>
                    <div class="single-cover" use:lazyObserveTile={cItem}>
                      {#if cUrl}
                        <img
                          src={cUrl}
                          alt={plAlbum.title}
                          loading="lazy"
                          class:blurred-img={isBlurredInCurrentMode(cItem)}
                          on:error={() => handleThumbError(cItem, cUrl)}
                        />
                      {:else if cItem?.isOffline}
                        <div class="tile-fallback offline-placeholder cover-offline">
                          <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="1.8">
                            <line x1="2" y1="2" x2="22" y2="22" />
                            <path d="M17 17H7a2 2 0 0 1-2-2V5c0-.6.26-1.14.68-1.5" />
                            <path d="M9 2h8a2 2 0 0 1 2 2v9" />
                          </svg>
                          <span class="offline-cover-hint">{$_("media_device_offline", {}, "Device offline")}</span>
                        </div>
                      {:else}
                        <div class="tile-fallback">📍</div>
                      {/if}
                      <div class="cover-gradient">
                        <span class="cover-title">{plAlbum.title}</span>
                        {#if plAlbum.subtitle}
                          <span class="cover-sub">{plAlbum.subtitle}</span>
                        {/if}
                      </div>
                      <span class="cover-count-pill">{plAlbum.count}</span>
                    </div>
                  </button>
                {/each}
              </div>
            </section>
          {/if}

          <!-- 3.5 AI Models & Formats -->
          {#if autoAlbums.models && autoAlbums.models.length > 0}
            <section class="album-section">
              <div class="section-head">
                <h3>🎛️ {$_("media_models", {}, "Models & Formats")}</h3>
                <span class="section-badge">{autoAlbums.models.length}</span>
              </div>
              <div class="album-cards-grid tags-cards-grid">
                {#each autoAlbums.models as mdAlbum (mdAlbum.id)}
                  {@const cItem = mdAlbum.coverItem}
                  {@const cUrl = cItem ? getItemThumbUrl(cItem, 360, thumbUrls, imgFailed, inlineImgFailed, thumbBlobFailed) : ""}
                  <button type="button" class="album-single-card compact" on:click={() => openAlbum(mdAlbum)}>
                    <div class="single-cover" use:lazyObserveTile={cItem}>
                      {#if cUrl}
                        <img
                          src={cUrl}
                          alt={mdAlbum.title}
                          loading="lazy"
                          class:blurred-img={isBlurredInCurrentMode(cItem)}
                          on:error={() => handleThumbError(cItem, cUrl)}
                        />
                      {:else}
                        <div class="tile-fallback">🎛️</div>
                      {/if}
                      <div class="cover-gradient">
                        <span class="cover-title">{mdAlbum.title}</span>
                      </div>
                      <span class="cover-count-pill">{mdAlbum.count}</span>
                    </div>
                  </button>
                {/each}
              </div>
            </section>
          {/if}

          <!-- 4. Meaningful Semantic Tags -->
          {#if autoAlbums.tags.length > 0}
            <section class="album-section">
              <div class="section-head">
                <h3>🏷️ {$_("media_themes", {}, "Themes & Tags")}</h3>
                <span class="section-badge">{autoAlbums.tags.length}</span>
              </div>
              <div class="album-cards-grid tags-cards-grid">
                {#each autoAlbums.tags as tAlbum (tAlbum.id)}
                  {@const cItem = tAlbum.coverItem}
                  {@const cUrl = cItem ? getItemThumbUrl(cItem, 360, thumbUrls, imgFailed, inlineImgFailed, thumbBlobFailed) : ""}
                  <button type="button" class="album-single-card compact" on:click={() => openAlbum(tAlbum)}>
                    <div class="single-cover" use:lazyObserveTile={cItem}>
                      {#if cUrl}
                        <img
                          src={cUrl}
                          alt={tAlbum.title}
                          loading="lazy"
                          class:blurred-img={isBlurredInCurrentMode(cItem)}
                          on:error={() => handleThumbError(cItem, cUrl)}
                        />
                      {:else if cItem?.isOffline}
                        <div class="tile-fallback offline-placeholder cover-offline">
                          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.8">
                            <line x1="2" y1="2" x2="22" y2="22" />
                            <path d="M17 17H7a2 2 0 0 1-2-2V5c0-.6.26-1.14.68-1.5" />
                            <path d="M9 2h8a2 2 0 0 1 2 2v9" />
                          </svg>
                          <span class="offline-cover-hint">{$_("media_device_offline", {}, "Device offline")}</span>
                        </div>
                      {:else}
                        <div class="tile-fallback">#</div>
                      {/if}
                      <div class="cover-gradient">
                        <span class="cover-title">{tAlbum.title}</span>
                      </div>
                      <span class="cover-count-pill">{tAlbum.count}</span>
                    </div>
                  </button>
                {/each}
              </div>
            </section>
          {/if}

          {#if autoAlbums.totalCount === 0}
            <div class="empty-state">
              <p>{$_("media_no_albums_yet", {}, "More metadata is needed to form automatic albums. Switch to Timeline to browse all media.")}</p>
              <button type="button" class="secondary-action-btn" on:click={() => setTab("timeline")}>
                {$_("media_timeline", {}, "Timeline")}
              </button>
            </div>
          {/if}
        </div>

      {:else}
        <!-- CHRONOLOGICAL TIMELINE OR OPENED AUTO-ALBUM -->
        {#if filteredMedia.length === 0}
          <div class="empty-state">
            <p>{$_("no_results_found", {}, "Nothing found for your query.")}</p>
            {#if searchQuery}
              <button type="button" class="secondary-action-btn" on:click={clearSearch}>
                {$_("clear", {}, "Clear")}
              </button>
            {/if}
          </div>
        {:else}
          <div class="timeline-stream" class:natural-fit={gridFit === "contain"}>
            {#each timelineGroups as group, gIdx (group.dayKey)}
              {@const isNewMonthAnchor = gIdx === 0 || timelineGroups[gIdx - 1].monthKey !== group.monthKey}
              <section
                class="day-group"
                data-month-anchor={isNewMonthAnchor ? group.monthKey : undefined}
              >
                <div class="day-sticky-header">
                  <div class="day-title-row">
                    <h4 class="day-label">{group.dayLabel}</h4>
                    {#if group.topPlace}
                      <span class="day-place-chip">📍 {group.topPlace}</span>
                    {/if}
                  </div>
                  <span class="day-count">{group.items.length}</span>
                </div>

                <div class="media-grid" class:contain-mode={gridFit === "contain"}>
                  {#each group.items as item (item.id)}
                    {@const tUrl = getItemThumbUrl(item, 480, thumbUrls, imgFailed, inlineImgFailed, thumbBlobFailed)}
                    {@const isBlurred = isBlurredInCurrentMode(item)}
                    {@const viewerScope = activeAlbum ? filteredMedia : group.items}
                    {@const offlineTooltip = item.isOffline ? $_("media_offline_preview_only", { device: item.deviceLabel || "" }, "Original file is on an offline device") : ""}
                    <div
                      class="media-tile"
                      class:is-offline={item.isOffline}
                      role={item.isOffline ? "group" : "button"}
                      tabindex={item.isOffline ? -1 : 0}
                      title={offlineTooltip}
                      use:lazyObserveTile={item}
                      on:click={() => handleTileClick(item, viewerScope)}
                      on:keydown={(e) => {
                        if (!item.isOffline && (e.key === "Enter" || e.key === " ")) {
                          e.preventDefault();
                          handleTileClick(item, viewerScope);
                        }
                      }}
                    >
                      {#if tUrl}
                        <img
                          class="tile-img"
                          class:blurred-img={isBlurred}
                          src={tUrl}
                          alt={item.title}
                          loading="lazy"
                          on:error={() => handleThumbError(item, tUrl)}
                        />
                      {:else if item.isOffline}
                        <div class="tile-fallback offline-placeholder">
                          <svg viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="1.8">
                            <line x1="2" y1="2" x2="22" y2="22" />
                            <path d="M17 17H7a2 2 0 0 1-2-2V5c0-.6.26-1.14.68-1.5" />
                            <path d="M9 2h8a2 2 0 0 1 2 2v9" />
                          </svg>
                          <span class="offline-title">{$_("media_device_offline", {}, "Device offline")}</span>
                          {#if item.deviceLabel}
                            <span class="offline-dev">{item.deviceLabel}</span>
                          {/if}
                        </div>
                      {:else}
                        <div class="tile-fallback">
                          <span>{item.isVideo ? "🎬" : "🖼️"}</span>
                        </div>
                      {/if}

                      {#if isBlurred}
                        <div class="sensitive-overlay">
                          <span class="sensitive-pill">🔒 Sensitive</span>
                        </div>
                      {/if}

                      {#if item.isVideo}
                        <div class="video-badge">
                          <svg viewBox="0 0 24 24" width="12" height="12" fill="currentColor">
                            <polygon points="6 4 20 12 6 20 6 4" />
                          </svg>
                          {#if item.duration > 0}
                            <span>{formatDuration(item.duration)}</span>
                          {/if}
                        </div>
                      {:else if item.isOffline && tUrl}
                        <div class="offline-badge" title={offlineTooltip}>
                          <svg viewBox="0 0 24 24" width="11" height="11" fill="none" stroke="currentColor" stroke-width="2.2">
                            <line x1="2" y1="2" x2="22" y2="22" />
                            <path d="M17 17H7a2 2 0 0 1-2-2V5c0-.6.26-1.14.68-1.5" />
                            <path d="M9 2h8a2 2 0 0 1 2 2v9" />
                          </svg>
                          <span>{item.deviceLabel || $_("media_device_offline", {}, "Device offline")}</span>
                        </div>
                      {/if}

                      {#if item.people.length > 0 && !isBlurred}
                        <div class="tile-bottom-meta">
                          <span class="tile-person-tag">{item.people[0]}{item.people.length > 1 ? ` +${item.people.length - 1}` : ""}</span>
                        </div>
                      {:else if item.city && !group.topPlace && !isBlurred}
                        <div class="tile-bottom-meta">
                          <span class="tile-person-tag">📍 {item.city}</span>
                        </div>
                      {/if}

                      <!-- Info / Mark People Button -->
                      <button
                        type="button"
                        class="tile-info-btn"
                        title={$_("details", {}, "Details & Mark people")}
                        on:click|stopPropagation={() => dispatch("edit", { card: item.card?.isDirectCreation ? { ...item.card, readOnly: true } : item.card })}
                      >
                        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2.2">
                          <circle cx="12" cy="12" r="10" />
                          <line x1="12" y1="16" x2="12" y2="12" />
                          <line x1="12" y1="8" x2="12.01" y2="8" />
                        </svg>
                      </button>
                    </div>
                  {/each}
                </div>
              </section>
            {/each}
          </div>
        {/if}
      {/if}
    </div>

    <!-- Interactive Year-Month Date Scrubber (Desktop rail + Mobile drawer) -->
    {#if (activeTab === "timeline" || activeAlbum) && monthScrubberItems.length >= 2 && (!isMobile || mobileScrubberOpen)}
      {#if isMobile && mobileScrubberOpen}
        <!-- svelte-ignore a11y_click_events_have_key_events -->
        <!-- svelte-ignore a11y_no_static_element_interactions -->
        <div class="mobile-scrubber-backdrop" on:click={() => (mobileScrubberOpen = false)}></div>
      {/if}
      <nav class="date-scrubber" class:mobile-open={isMobile && mobileScrubberOpen} aria-label="Timeline jump bar">
        {#each monthScrubberItems.slice(0, 36) as sc (sc.key)}
          <button
            type="button"
            class="scrubber-tick"
            class:active={activeScrubberKey === sc.key}
            title={sc.fullLabel}
            on:click={() => jumpToMonth(sc)}
          >
            <span class="tick-label">{sc.shortLabel} '{String(sc.year).slice(-2)}</span>
            <span class="tick-dot"></span>
          </button>
        {/each}
      </nav>
    {/if}
  </div>

  <!-- Scoped Fullscreen Video Viewer Overlay -->
  {#if activeVideoModal}
    <div class="video-modal-overlay" transition:fade={{ duration: 150 }}>
      <VideoViewer
        playlist={activeVideoModal.playlist}
        currentIndex={activeVideoModal.startIndex}
        showClose={true}
        autoplay={true}
        on:close={closeVideoModal}
      />
    </div>
  {/if}

  {#if showDeviceIndexModal}
    <DeviceIndexModal
      show={showDeviceIndexModal}
      {isDarkTheme}
      deviceDrive={localDeviceDrive}
      {httpDrive}
      {chatStore}
      extraCards={knowledgeList}
      deviceName={getDefaultWebDeviceName()}
      on:close={() => {
        showDeviceIndexModal = false;
      }}
      on:indexed={() => dispatch("refresh")}
    />
  {/if}
</div>

<style>
  .media-library-view {
    display: flex;
    flex-direction: column;
    height: 100%;
    width: 100%;
    overflow: hidden;
    background: var(--bg-primary, #0f1117);
    color: var(--text-primary, #f1f5f9);
    position: relative;
  }

  .media-library-view.light-theme {
    --ml-bg: #f8fafc;
    --ml-surface: #ffffff;
    --ml-surface-hover: #f1f5f9;
    --ml-border: rgba(15, 23, 42, 0.1);
    --ml-text: #0f172a;
    --ml-muted: #64748b;
    --ml-accent: #7c3aed;
    --ml-accent-soft: rgba(124, 58, 237, 0.12);
    background: var(--ml-bg);
    color: var(--ml-text);
  }

  .media-library-view.dark-theme {
    --ml-bg: #0d0f15;
    --ml-surface: #161923;
    --ml-surface-hover: #1e2330;
    --ml-border: rgba(255, 255, 255, 0.09);
    --ml-text: #f1f5f9;
    --ml-muted: #94a3b8;
    --ml-accent: #8b5cf6;
    --ml-accent-soft: rgba(139, 92, 246, 0.18);
    background: var(--ml-bg);
    color: var(--ml-text);
  }

  .media-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    padding: 10px 14px;
    border-bottom: 1px solid var(--ml-border);
    background: var(--ml-surface);
    flex-shrink: 0;
    flex-wrap: wrap;
    z-index: 20;
  }

  .header-left,
  .header-right {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .view-seg-pill {
    display: inline-flex;
    align-items: center;
    background: var(--ml-bg);
    border: 1px solid var(--ml-border);
    border-radius: 999px;
    padding: 3px;
    gap: 2px;
  }

  .seg-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    border: none;
    background: transparent;
    color: var(--ml-muted);
    font-size: 0.83rem;
    font-weight: 600;
    padding: 5px 12px;
    border-radius: 999px;
    cursor: pointer;
    transition: all 0.16s ease;
  }

  .seg-btn:hover {
    color: var(--ml-text);
  }

  .seg-btn.active {
    background: var(--ml-accent);
    color: #ffffff;
  }

  .seg-count {
    font-size: 0.72rem;
    padding: 1px 6px;
    border-radius: 999px;
    background: rgba(255, 255, 255, 0.16);
  }

  .seg-btn:not(.active) .seg-count {
    background: var(--ml-surface-hover);
    color: var(--ml-muted);
  }

  .header-search {
    flex: 1;
    min-width: 180px;
    max-width: 380px;
    position: relative;
    display: flex;
    align-items: center;
  }

  .search-icon {
    position: absolute;
    left: 11px;
    color: var(--ml-muted);
    pointer-events: none;
  }

  .search-input {
    width: 100%;
    height: 34px;
    padding: 0 30px 0 34px;
    border-radius: 999px;
    border: 1px solid var(--ml-border);
    background: var(--ml-bg);
    color: var(--ml-text);
    font-size: 0.84rem;
    outline: none;
    transition: border-color 0.15s;
  }

  .search-input:focus {
    border-color: var(--ml-accent);
  }

  .search-clear {
    position: absolute;
    right: 8px;
    background: transparent;
    border: none;
    color: var(--ml-muted);
    cursor: pointer;
    font-size: 0.8rem;
    padding: 2px 5px;
  }

  .pill-toggle-btn {
    height: 32px;
    padding: 0 11px;
    border-radius: 999px;
    border: 1px solid var(--ml-border);
    background: var(--ml-bg);
    color: var(--ml-muted);
    font-size: 0.78rem;
    font-weight: 600;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    gap: 5px;
    transition: all 0.15s;
    white-space: nowrap;
  }

  .pill-toggle-btn:hover {
    color: var(--ml-text);
    border-color: var(--ml-accent);
  }

  .pill-toggle-btn.active {
    background: var(--ml-accent-soft);
    color: var(--ml-accent);
    border-color: var(--ml-accent);
  }

  .pill-toggle-btn.mode-pill.fun {
    background: rgba(236, 72, 153, 0.15);
    color: #ec4899;
    border-color: rgba(236, 72, 153, 0.4);
  }

  .header-icon-btn {
    width: 34px;
    height: 34px;
    border-radius: 9px;
    border: 1px solid var(--ml-border);
    background: var(--ml-bg);
    color: var(--ml-muted);
    display: inline-flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s;
  }

  .header-icon-btn:hover {
    color: var(--ml-text);
    background: var(--ml-surface-hover);
  }

  .sort-order-btn:hover {
    color: #ec4899;
  }

  .sort-order-btn.is-active,
  .scrubber-toggle-btn.is-active {
    color: #ec4899;
    border-color: rgba(236, 72, 153, 0.45);
    background: rgba(236, 72, 153, 0.1);
  }

  .mobile-scrubber-backdrop {
    position: absolute;
    inset: 0;
    z-index: 24;
    background: rgba(0, 0, 0, 0.18);
  }

  .date-scrubber.mobile-open {
    z-index: 28;
    right: 8px;
    padding: 10px 8px;
    gap: 6px;
    max-height: 82%;
    box-shadow: 0 12px 30px rgba(0, 0, 0, 0.4);
  }

  .date-scrubber.mobile-open .scrubber-tick {
    font-size: 0.78rem;
    padding: 5px 8px;
  }

  .bg-index-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 6px 14px;
    background: var(--ml-accent-soft);
    border-bottom: 1px solid var(--ml-border);
    font-size: 0.8rem;
  }

  .bg-index-info {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    background: transparent;
    border: none;
    color: var(--ml-text);
    cursor: pointer;
  }

  .bg-index-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #10b981;
  }

  .bg-index-dot.paused {
    background: #f59e0b;
  }

  .bg-index-btn {
    padding: 3px 9px;
    border-radius: 6px;
    border: 1px solid var(--ml-border);
    background: var(--ml-surface);
    color: var(--ml-text);
    font-size: 0.76rem;
    cursor: pointer;
  }

  .sensitive-toast {
    position: absolute;
    top: 64px;
    left: 50%;
    transform: translateX(-50%);
    z-index: 60;
    background: rgba(15, 23, 42, 0.92);
    color: #f8fafc;
    border: 1px solid rgba(255, 255, 255, 0.15);
    padding: 8px 16px;
    border-radius: 999px;
    font-size: 0.82rem;
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.35);
    pointer-events: none;
  }

  .sub-filter-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    padding: 8px 16px;
    border-bottom: 1px solid var(--ml-border);
    background: var(--ml-surface);
    flex-wrap: wrap;
    flex-shrink: 0;
  }

  .album-crumb {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.88rem;
  }

  .back-album-btn {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 4px 10px;
    border-radius: 8px;
    border: 1px solid var(--ml-border);
    background: var(--ml-bg);
    color: var(--ml-text);
    font-size: 0.82rem;
    font-weight: 600;
    cursor: pointer;
  }

  .back-album-btn:hover {
    border-color: var(--ml-accent);
  }

  .crumb-sep {
    color: var(--ml-muted);
  }

  .crumb-title {
    font-weight: 700;
  }

  .crumb-count {
    color: var(--ml-muted);
    font-size: 0.8rem;
  }

  .type-pills {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .type-pill {
    padding: 4px 11px;
    border-radius: 999px;
    border: 1px solid var(--ml-border);
    background: var(--ml-bg);
    color: var(--ml-muted);
    font-size: 0.78rem;
    font-weight: 600;
    cursor: pointer;
  }

  .type-pill.active {
    background: var(--ml-accent-soft);
    color: var(--ml-accent);
    border-color: var(--ml-accent);
  }

  .media-body-wrap {
    flex: 1;
    position: relative;
    overflow: hidden;
    display: flex;
  }

  .media-scroll {
    flex: 1;
    height: 100%;
    overflow-y: auto;
    padding: 16px 20px 48px;
  }

  /* Auto-Albums Showcase */
  .albums-showcase {
    display: flex;
    flex-direction: column;
    gap: 28px;
    max-width: 1440px;
    margin: 0 auto;
  }

  .album-section {
    display: flex;
    flex-direction: column;
    gap: 12px;
  }

  .section-head {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .section-head h3 {
    margin: 0;
    font-size: 1.02rem;
    font-weight: 700;
  }

  .section-badge {
    font-size: 0.75rem;
    padding: 2px 8px;
    border-radius: 999px;
    background: var(--ml-surface);
    border: 1px solid var(--ml-border);
    color: var(--ml-muted);
  }

  .people-shelf {
    display: flex;
    gap: 14px;
    overflow-x: auto;
    padding-bottom: 6px;
  }

  .person-tile {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    min-width: 92px;
    max-width: 104px;
    background: transparent;
    border: none;
    color: var(--ml-text);
    cursor: pointer;
    padding: 6px 4px;
    border-radius: 12px;
    transition: background 0.15s;
  }

  .person-tile:hover {
    background: var(--ml-surface-hover);
  }

  .person-ring {
    width: 76px;
    height: 76px;
    border-radius: 50%;
    overflow: hidden;
    border: 2px solid var(--ml-border);
    background: var(--ml-surface);
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.16);
  }

  .person-ring img {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }

  .person-placeholder {
    font-size: 1.5rem;
    font-weight: 700;
    color: var(--ml-accent);
  }

  .person-tile-name {
    font-size: 0.82rem;
    font-weight: 600;
    max-width: 96px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .person-tile-count {
    font-size: 0.72rem;
    color: var(--ml-muted);
  }

  .album-cards-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(210px, 1fr));
    gap: 14px;
  }

  .moments-grid {
    grid-template-columns: repeat(auto-fill, minmax(250px, 1fr));
  }

  .tags-cards-grid {
    grid-template-columns: repeat(auto-fill, minmax(165px, 1fr));
  }

  .album-collage-card,
  .album-single-card {
    background: var(--ml-surface);
    border: 1px solid var(--ml-border);
    border-radius: 14px;
    overflow: hidden;
    padding: 0;
    cursor: pointer;
    text-align: left;
    color: var(--ml-text);
    transition: transform 0.16s ease, border-color 0.16s ease;
  }

  .album-collage-card:hover,
  .album-single-card:hover {
    transform: translateY(-2px);
    border-color: var(--ml-accent);
  }

  .collage-cover {
    display: grid;
    height: 156px;
    gap: 2px;
    background: var(--ml-bg);
  }

  .collage-cover.count-1 {
    grid-template-columns: 1fr;
  }

  .collage-cover.count-2 {
    grid-template-columns: 1fr 1fr;
  }

  .collage-cover.count-3 {
    grid-template-columns: 1.4fr 1fr;
    grid-template-rows: 1fr 1fr;
  }

  .collage-cover.count-3 .cell-0 {
    grid-row: 1 / span 2;
  }

  .collage-cell {
    overflow: hidden;
    background: var(--ml-surface-hover);
  }

  .collage-cell img {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
  }

  .album-card-meta {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 10px 12px;
  }

  .album-card-title {
    font-size: 0.85rem;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .album-card-count {
    font-size: 0.75rem;
    color: var(--ml-muted);
    flex-shrink: 0;
  }

  .single-cover {
    position: relative;
    height: 150px;
    overflow: hidden;
    background: var(--ml-surface-hover);
  }

  .album-single-card.compact .single-cover {
    height: 118px;
  }

  .single-cover img {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
  }

  .cover-gradient {
    position: absolute;
    inset: auto 0 0 0;
    padding: 24px 12px 10px;
    background: linear-gradient(to top, rgba(0, 0, 0, 0.78), transparent);
    color: #ffffff;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .cover-title {
    font-size: 0.88rem;
    font-weight: 700;
    text-shadow: 0 1px 3px rgba(0, 0, 0, 0.5);
  }

  .cover-sub {
    font-size: 0.74rem;
    opacity: 0.85;
  }

  .cover-count-pill {
    position: absolute;
    top: 8px;
    right: 8px;
    padding: 2px 8px;
    border-radius: 999px;
    background: rgba(0, 0, 0, 0.58);
    color: #ffffff;
    font-size: 0.72rem;
    font-weight: 600;
    backdrop-filter: blur(4px);
  }

  /* Timeline Stream */
  .timeline-stream {
    display: flex;
    flex-direction: column;
    gap: 22px;
    max-width: 1520px;
    margin: 0 auto;
    padding-right: 36px;
  }

  .day-group {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .day-sticky-header {
    position: sticky;
    top: -16px;
    z-index: 10;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 4px;
    background: color-mix(in srgb, var(--ml-bg) 88%, transparent);
    backdrop-filter: blur(8px);
  }

  .day-title-row {
    display: flex;
    align-items: center;
    gap: 10px;
    flex-wrap: wrap;
  }

  .day-label {
    margin: 0;
    font-size: 0.95rem;
    font-weight: 700;
    text-transform: capitalize;
  }

  .day-place-chip {
    font-size: 0.78rem;
    color: var(--ml-muted);
    background: var(--ml-surface);
    border: 1px solid var(--ml-border);
    padding: 2px 9px;
    border-radius: 999px;
  }

  .day-count {
    font-size: 0.76rem;
    color: var(--ml-muted);
  }

  .media-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(175px, 1fr));
    gap: 6px;
  }

  .media-tile {
    position: relative;
    aspect-ratio: 1 / 1;
    border-radius: 8px;
    overflow: hidden;
    background: var(--ml-surface);
    border: 1px solid var(--ml-border);
    cursor: pointer;
    user-select: none;
  }

  .media-grid.contain-mode .media-tile {
    aspect-ratio: 4 / 3;
    background: var(--ml-surface);
  }

  .tile-img {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
    transition: transform 0.2s ease;
  }

  .media-grid.contain-mode .tile-img {
    object-fit: contain;
  }

  .media-tile.is-offline {
    cursor: default;
  }

  .media-tile:not(.is-offline):hover .tile-img:not(.blurred-img) {
    transform: scale(1.03);
  }

  .blurred-img {
    filter: blur(22px) saturate(0.65);
    transform: scale(1.12);
  }

  .sensitive-overlay {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: rgba(0, 0, 0, 0.24);
  }

  .sensitive-pill {
    padding: 3px 9px;
    border-radius: 999px;
    background: rgba(15, 23, 42, 0.72);
    color: #ffffff;
    font-size: 0.72rem;
    font-weight: 600;
  }

  .tile-fallback {
    width: 100%;
    height: 100%;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1.6rem;
    color: var(--ml-muted);
  }

  .tile-fallback.offline-placeholder {
    flex-direction: column;
    gap: 5px;
    padding: 10px;
    text-align: center;
    font-size: 0.75rem;
    background: repeating-linear-gradient(
      -45deg,
      var(--ml-surface),
      var(--ml-surface) 8px,
      var(--ml-surface-hover) 8px,
      var(--ml-surface-hover) 16px
    );
    color: var(--ml-muted);
  }

  .tile-fallback.offline-placeholder.cover-offline {
    padding-bottom: 34px;
    gap: 4px;
  }

  .offline-title,
  .offline-cover-hint {
    font-size: 0.73rem;
    font-weight: 600;
    line-height: 1.2;
  }

  .offline-dev {
    font-size: 0.67rem;
    opacity: 0.78;
    max-width: 100%;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .offline-badge {
    position: absolute;
    top: 6px;
    left: 6px;
    display: inline-flex;
    align-items: center;
    gap: 4px;
    max-width: calc(100% - 40px);
    padding: 2px 7px;
    border-radius: 999px;
    background: rgba(15, 23, 42, 0.68);
    color: rgba(248, 250, 252, 0.92);
    font-size: 0.67rem;
    font-weight: 600;
    backdrop-filter: blur(4px);
    pointer-events: none;
  }

  .offline-badge span {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .video-badge {
    position: absolute;
    top: 7px;
    left: 7px;
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 3px 7px;
    border-radius: 999px;
    background: rgba(0, 0, 0, 0.65);
    color: #ffffff;
    font-size: 0.72rem;
    font-weight: 600;
    backdrop-filter: blur(4px);
  }

  .tile-bottom-meta {
    position: absolute;
    inset: auto 0 0 0;
    padding: 16px 8px 6px;
    background: linear-gradient(to top, rgba(0, 0, 0, 0.72), transparent);
    pointer-events: none;
  }

  .tile-person-tag {
    color: #ffffff;
    font-size: 0.73rem;
    font-weight: 600;
  }

  .tile-info-btn {
    position: absolute;
    top: 6px;
    right: 6px;
    width: 26px;
    height: 26px;
    border-radius: 50%;
    border: none;
    background: rgba(0, 0, 0, 0.55);
    color: #ffffff;
    display: flex;
    align-items: center;
    justify-content: center;
    opacity: 0;
    cursor: pointer;
    transition: opacity 0.15s, background 0.15s;
  }

  .media-tile:hover .tile-info-btn,
  .tile-info-btn:focus-visible {
    opacity: 1;
  }

  .tile-info-btn:hover {
    background: var(--ml-accent);
  }

  /* Date Scrubber */
  .date-scrubber {
    position: absolute;
    right: 6px;
    top: 50%;
    transform: translateY(-50%);
    z-index: 18;
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 4px;
    padding: 8px 4px;
    border-radius: 12px;
    background: color-mix(in srgb, var(--ml-surface) 82%, transparent);
    backdrop-filter: blur(6px);
    border: 1px solid var(--ml-border);
    max-height: 78%;
    overflow-y: auto;
  }

  .scrubber-tick {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    background: transparent;
    border: none;
    color: var(--ml-muted);
    font-size: 0.68rem;
    padding: 2px 5px;
    border-radius: 6px;
    cursor: pointer;
    white-space: nowrap;
  }

  .scrubber-tick:hover,
  .scrubber-tick.active {
    color: var(--ml-accent);
    font-weight: 700;
  }

  .tick-dot {
    width: 5px;
    height: 5px;
    border-radius: 50%;
    background: currentColor;
  }

  /* Empty States */
  .empty-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    text-align: center;
    padding: 64px 20px;
    max-width: 460px;
    margin: 0 auto;
    gap: 12px;
  }

  .empty-icon {
    font-size: 3rem;
  }

  .empty-state h3 {
    margin: 0;
    font-size: 1.15rem;
  }

  .empty-state p {
    margin: 0;
    color: var(--ml-muted);
    font-size: 0.9rem;
    line-height: 1.5;
  }

  .empty-actions {
    display: flex;
    flex-wrap: wrap;
    justify-content: center;
    gap: 10px;
    margin-top: 8px;
  }

  .primary-action-btn {
    padding: 9px 18px;
    border-radius: 10px;
    border: none;
    background: var(--ml-accent);
    color: #ffffff;
    font-weight: 600;
    font-size: 0.86rem;
    cursor: pointer;
  }

  .secondary-action-btn {
    padding: 9px 16px;
    border-radius: 10px;
    border: 1px solid var(--ml-border);
    background: var(--ml-surface);
    color: var(--ml-text);
    font-weight: 600;
    font-size: 0.86rem;
    cursor: pointer;
  }

  .spinner-ring {
    width: 30px;
    height: 30px;
    border: 3px solid var(--ml-border);
    border-top-color: var(--ml-accent);
    border-radius: 50%;
    animation: ml-spin 0.8s linear infinite;
  }

  @keyframes ml-spin {
    to { transform: rotate(360deg); }
  }

  .video-modal-overlay {
    position: fixed;
    inset: 0;
    z-index: 9500;
    background: #000000;
  }

  @media (max-width: 768px) {
    .media-scroll {
      padding: 10px 10px 36px;
    }

    .timeline-stream {
      padding-right: 0;
    }

    .media-grid {
      grid-template-columns: repeat(3, 1fr);
      gap: 3px;
    }

    .media-tile {
      border-radius: 4px;
    }

    .tile-info-btn {
      opacity: 0.85;
    }

    .header-search {
      order: 3;
      max-width: none;
      flex-basis: 100%;
    }
  }
</style>
