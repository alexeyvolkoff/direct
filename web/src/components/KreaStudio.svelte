<script>
  import { createEventDispatcher, onMount, onDestroy, tick } from "svelte";
  import { fly, fade } from "svelte/transition";
  import { _, locale } from "../lib/i18n/index.js";
  import lightGallery from "lightgallery";
  import { setupGalleryPagination, isJustFileName } from "../lib/gallery-pagination.js";
  import FolderPicker from "./FolderPicker.svelte";
  import { deviceRegistry } from "../lib/device-registry.js";
  import { profileStore } from "../lib/profile-store.js";
  import { getViewerSettings } from "../lib/viewer-settings.js";
  import {
    parseStoragePath,
    DEFAULT_STORAGE_PATH,
    getLocalDevice
  } from "../lib/storage-path.js";
  import { issueConsent, getClientDeviceServer } from "../lib/client-device-server.js";
  import { getOrCreateLocalLinkId } from "../lib/local-client.js";
  import { mcpKeysStore, getMcpPayload, saveMcpAppKey } from "../lib/mcp-keys.js";
  import { setDidStudioEnabled } from "../lib/identity/persistent.js";
  import { getClientPlatform, isNativeMobile, isPrimaryWebGateway, sanitizeExternalCheckoutText } from "../lib/platform.js";
  import defaultKreaPresets from "../lib/krea_presets.json";

  let civitaiAccounts = [];
  $: userCivitaiMcpApps = Object.values(($mcpKeysStore && $mcpKeysStore.byApp) || {}).filter((app) => {
    if (!app || !app.key) return false;
    const id = String(app.appId || "").toLowerCase();
    const name = String(app.name || "").toLowerCase();
    const url = String(app.url || "").toLowerCase();
    return id === "civitai" || id.startsWith("civitai_") || id.startsWith("civitai-") || url.includes("civitai.") || name.includes("civitai");
  });
  $: hasCivitaiToken = userCivitaiMcpApps.length > 0;
  $: canPublishCivitai = true;
  $: effectiveCivitaiAccounts = userCivitaiMcpApps.map((app) => {
    const resolved = (civitaiAccounts || []).find((a) => String(a.id || "").toLowerCase() === String(app.appId || "").toLowerCase());
    if (resolved) return resolved;
    return {
      id: app.appId,
      username: "",
      label: app.name || app.appId
    };
  });

  export let httpDrive = null;
  export let deviceDrive = null;
  export let targetDevice = null;
  export let targetLinkId = "";
  export let isPrivateMode = false;
  export let omdKey = "";
  export let chatStore = null;
  export let isDarkTheme = true;
  export let contentMode = "work";
  export let isOverlayHeader = false;
  export let sidebarOpen = true;
  export let defaultStorage = "";
  export let userName = "";
  export let sdxlSupported = true;
  export let kreaSupported = true;

  const isMobileClientApp = isNativeMobile();

  // Studio capability state (gated silently by DID flag + /krea/status)
  let studioTier = "standard";
  let kreaAllowed = false;
  let videoAllowed = false;
  let directorAllowed = false;
  let kreaDailyRemaining = null;
  let kreaDailyLimit = null;
  let memberCredits = null;
  let kreaLockNotice = "";
  let generationError = "";
  let statusLoadedOnce = false;
  let pendingSavedDirectorOpen = false;
  let activeVideoModalItem = null;

  $: hasDidStudio = Boolean($profileStore && $profileStore.studio) || Boolean(isPrivateMode);
  $: canShowStudioTab = Boolean(hasDidStudio && kreaSupported && kreaAllowed);
  $: canShowVideoTab = Boolean(hasDidStudio && kreaSupported && kreaAllowed && videoAllowed);
  $: canShowDirector = Boolean(hasDidStudio && directorAllowed);

  $: if (!canShowDirector && showDirectorPanel) {
    showDirectorPanel = false;
  }
  $: if (canShowDirector && pendingSavedDirectorOpen) {
    pendingSavedDirectorOpen = false;
    showDirectorPanel = true;
  }

  function extractUrlFromText(txt) {
    if (!txt || isMobileClientApp || !isPrimaryWebGateway()) return "";
    const m = String(txt).match(/https?:\/\/[^\s)>]+/);
    return m ? m[0].replace(/[.,;!?]+$/, "") : "";
  }

  function stripUrlFromText(txt) {
    if (!txt) return "";
    return String(txt).replace(/https?:\/\/[^\s)>]+/g, "").replace(/[:—–\-]\s*$/, "").trim();
  }

  const dispatch = createEventDispatcher();

  let innerWidth = typeof window !== "undefined" ? window.innerWidth : 1024;
  $: isMobile = innerWidth < 768;

  // Active top tab: 'gallery' | 'personas'
  let activeTab = "gallery";

  // Gallery items & Personas
  let items = [];

  let sortOrder = typeof localStorage !== "undefined"
    ? localStorage.getItem("omd_krea_sort") || localStorage.getItem("omd_creations_sort") || "desc"
    : "desc"; // 'desc' (newest first) | 'asc' (oldest first)

  function getItemTimestamp(item) {
    if (!item) return 0;
    const name = item.name || item.id || "";
    // 1. Try YYYYMMDD_HHMMSS (e.g. KREA_00001_20261015_145535.png, IMG_00001_20261013_081139.png)
    const matchYmdHms = name.match(/_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})/);
    if (matchYmdHms) {
      const [_, y, m, d, h, min, s] = matchYmdHms;
      const parsedTime = new Date(parseInt(y, 10), parseInt(m, 10) - 1, parseInt(d, 10), parseInt(h, 10), parseInt(min, 10), parseInt(s, 10)).getTime();
      if (!isNaN(parsedTime)) return parsedTime;
    }
    // 2. Try 13-digit Unix ms timestamp (e.g. krea_1729000000000.png)
    const match13 = name.match(/_(\d{13})/);
    if (match13) {
      const parsedTime = parseInt(match13[1], 10);
      if (!isNaN(parsedTime)) return parsedTime;
    }
    // 3. Try item.date
    if (item.date instanceof Date) {
      const t = item.date.getTime();
      if (!isNaN(t) && t > 0) return t;
    } else if (typeof item.date === "number" && !isNaN(item.date) && item.date > 0) {
      return item.date;
    } else if (item.date) {
      const t = new Date(item.date).getTime();
      if (!isNaN(t) && t > 0) return t;
    }
    return 0;
  }

  function sortItemsList(list, order = sortOrder) {
    return [...list].sort((a, b) => {
      const timeA = getItemTimestamp(a);
      const timeB = getItemTimestamp(b);
      if (timeA !== timeB) {
        return order === "desc" ? timeB - timeA : timeA - timeB;
      }
      return order === "desc"
        ? (b.name || "").localeCompare(a.name || "")
        : (a.name || "").localeCompare(b.name || "");
    });
  }

  async function toggleSortOrder() {
    sortOrder = sortOrder === "desc" ? "asc" : "desc";
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_krea_sort", sortOrder);
      localStorage.setItem("omd_creations_sort", sortOrder);
    }
    items = sortItemsList(items, sortOrder);
    await tick();
    initLightGallery();
  }

  const SELECTED_PERSONAS_KEY = "krea_selected_personas";

  function getLocalSelectedPersonas(currentPersonas) {
    if (typeof localStorage === "undefined") return [];
    try {
      const raw = localStorage.getItem(SELECTED_PERSONAS_KEY);
      if (raw) {
        const ids = JSON.parse(raw);
        if (Array.isArray(ids)) {
          return (currentPersonas || []).filter((p) => ids.includes(p.id));
        }
      }
    } catch (_) {}
    return [];
  }

  function saveLocalSelectedPersonas(selectedList) {
    if (typeof localStorage === "undefined") return;
    try {
      localStorage.setItem(
        SELECTED_PERSONAS_KEY,
        JSON.stringify((selectedList || []).map((p) => p.id))
      );
    } catch (_) {}
  }

  let personas = [];
  let selectedPersonas = [];
  $: selectedPersona = selectedPersonas[0] || null;

  $: if (typeof localStorage !== "undefined" && selectedPersonas) {
    saveLocalSelectedPersonas(selectedPersonas);
  }
  let isLoadingFiles = true;
  let isGenerating = false;
  let generationElapsed = 0;
  let generationTimer = null;
  let generatingPrompt = "";
  let generatingPersona = "";
  let generatingStage = "warming"; // "warming" | "generating" | "saving"
  let warmingPhase = "preparing"; // "preparing" | "booting" | "loading_models"
  let generatingStep = 0;
  let generatingTotalSteps = 0;
  let isGpuWarm = false;
  let gpuWarmUntil = 0;
  let lastGenFinishedAt = 0;
  let isRandomizingPrompt = false;

  // --- AI Director's Chair State ---
  let showDirectorPanel = false;
  let directorTab = "prompt"; // "prompt" | "storyboard"
  let directorInput = "";
  let isDirectorThinking = false;
  let directorMessages = [];
  let promptUndoStack = [];
  let directorInputEl = null;
  let directorFeedEl = null;

  // Storyboard & Multi-scene Series State
  let storyTitle = "";
  let storySlug = "";
  let storySetting = "";
  let storyIdea = "";
  let storySceneCount = 4;
  let storyScenes = [];
  let savedStoriesList = [];
  let savedSettingPresets = [];
  let isPlanningStory = false;
  let refiningSceneIdx = -1;
  let isShootingSeries = false;
  let stopSeriesRequested = false;
  let shootingSceneIdx = -1;
  let isSavingStory = false;
  let isExportingStory = false;
  let isSetupCollapsed = false;

  // Gallery Story Subfolder Filter
  let storyFolders = [];
  let activeStorySubfolder = "";
  let hoveredVideoId = null;
  let playingVideoId = null;

  function getKreaFetch() {
    return (httpDrive && typeof httpDrive.fetch === "function")
      ? (p, opts) => httpDrive.fetch(p, opts)
      : (p, opts) => fetch(p, opts);
  }

  function buildAuthHeaders() {
    const h = {
      "Content-Type": "application/json",
      "X-Client": "svar"
    };
    const activeToken = omdKey || (typeof window !== "undefined" && (window.__omdKey || window.identity?.token)) || "";
    if (activeToken) {
      h["Token"] = activeToken;
      h["X-OMD-Token"] = activeToken;
    }
    const effectiveDid = (httpDrive && httpDrive.didPub) || (typeof window !== "undefined" && (window.identity?.mainPub || window.identity?.pair?.pub)) || "";
    if (effectiveDid) {
      h["X-OMD-Caller-DID"] = effectiveDid;
    }
    if (httpDrive && httpDrive.isPrivateMode !== undefined) {
      h["X-OMD-Private-Mode"] = httpDrive.isPrivateMode ? "1" : "0";
    } else {
      h["X-OMD-Private-Mode"] = "1";
    }
    return h;
  }

  const KREA_SLIDERS_KEY = "krea_studio_sliders_v1";

  function getLocalSliders() {
    try {
      let raw = null;
      if (typeof localStorage !== "undefined") {
        raw = localStorage.getItem(KREA_SLIDERS_KEY);
      }
      if (!raw && typeof sessionStorage !== "undefined") {
        raw = sessionStorage.getItem(KREA_SLIDERS_KEY);
      }
      if (raw) {
        const parsed = JSON.parse(raw);
        console.log("[KreaStudio] Restored sliders from storage:", parsed);
        return parsed;
      }
    } catch (err) {
      console.warn("[KreaStudio] Failed to read sliders from storage:", err);
    }
    return null;
  }

  function saveLocalSliders(s) {
    if (!s) return;
    try {
      const activeStyles = Array.isArray(s.styles) ? s.styles.filter((x) => x && x !== "none") : [];
      const primaryStyle = activeStyles.length > 0 ? activeStyles[0] : "none";
      const activeArchetypes = Array.isArray(s.archetypes) ? s.archetypes.filter(Boolean) : [];
      const primaryArchetype = activeArchetypes.length > 0 ? activeArchetypes[0] : null;

      const cleanedStyleWeights = {};
      for (const stId of activeStyles) {
        if (s.style_weights?.[stId] !== undefined) {
          cleanedStyleWeights[stId] = s.style_weights[stId];
        }
      }
      const cleanedArchetypeWeights = {};
      for (const aId of activeArchetypes) {
        if (s.archetype_weights?.[aId] !== undefined) {
          cleanedArchetypeWeights[aId] = s.archetype_weights[aId];
        }
      }

      const payload = {
        base_model: s.base_model || "anima",
        detail: typeof s.detail === "number" ? s.detail : 0.5,
        styles: activeStyles,
        style_weights: cleanedStyleWeights,
        style: primaryStyle,
        style_strength: typeof s.style_strength === "number" ? s.style_strength : 0.8,
        archetypes: activeArchetypes,
        archetype_weights: cleanedArchetypeWeights,
        archetype: primaryArchetype,
        archetype_strength: typeof s.archetype_strength === "number" ? s.archetype_strength : 0.4,
        aspect: s.aspect || "896x1152",
        ipadapter_weight: typeof s.ipadapter_weight === "number" ? s.ipadapter_weight : 0.65,
        face_restore_visibility: typeof s.face_restore_visibility === "number" ? s.face_restore_visibility : 0.30,
        inpaint_denoise: typeof s.inpaint_denoise === "number" ? s.inpaint_denoise : 0.94,
        steps: typeof s.steps === "number" ? s.steps : (isSdxlModel(s.base_model) ? 6 : (s.base_model === "anima" ? 25 : 12)),
        cfg: typeof s.cfg === "number" ? s.cfg : (isSdxlModel(s.base_model) ? 1.0 : (s.base_model === "realism" ? 1.5 : (s.base_model === "semi_realism" ? 2.0 : 3.0)))
      };
      const jsonStr = JSON.stringify(payload);

      let saved = false;
      if (typeof localStorage !== "undefined") {
        try {
          localStorage.removeItem(KREA_SLIDERS_KEY);
          localStorage.setItem(KREA_SLIDERS_KEY, jsonStr);
          saved = true;
        } catch (lsErr) {
          console.warn("[KreaStudio] localStorage.setItem failed (quota exceeded?), falling back to sessionStorage:", lsErr);
        }
      }

      if (typeof sessionStorage !== "undefined") {
        try {
          sessionStorage.setItem(KREA_SLIDERS_KEY, jsonStr);
          saved = true;
        } catch (_) {}
      }

      if (saved) {
        console.log("[KreaStudio] Saved sliders to storage:", payload);
      }
    } catch (err) {
      console.warn("[KreaStudio] Failed to save sliders to storage:", err);
    }
  }

  function closeStyleDropdown() {
    showStyleDropdown = false;
    sliders.styles = Array.isArray(sliders.styles) ? sliders.styles.filter((x) => x && x !== "none") : [];
    sliders.style = sliders.styles[0] || "none";
    if (sliders.styles.length > 0 && sliders.style_weights?.[sliders.style]) {
      sliders.style_strength = sliders.style_weights[sliders.style];
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function closeArchetypeDropdown() {
    showArchetypeDropdown = false;
    sliders.archetypes = Array.isArray(sliders.archetypes) ? sliders.archetypes.filter(Boolean) : [];
    sliders.archetype = sliders.archetypes[0] || null;
    if (sliders.archetypes.length > 0 && sliders.archetype_weights?.[sliders.archetype]) {
      sliders.archetype_strength = sliders.archetype_weights[sliders.archetype];
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  const SDXL_BASE_MODELS = ["sdxl_realistic", "sdxl_perfect", "sdxl_fantasy", "sdxl_tooned"];
  const VIDEO_BASE_MODELS = ["minimax_h3", "video", "h3"];
  const videoExtRegex = /\.(mp4|webm|mov)$/i;

  function isSdxlModel(m) {
    const v = String(m || "").trim().toLowerCase();
    return v === "sdxl" || SDXL_BASE_MODELS.includes(v);
  }

  function isVideoModel(m) {
    const v = String(m || "").trim().toLowerCase();
    return VIDEO_BASE_MODELS.includes(v);
  }

  function normalizeSdxlModel(m) {
    const v = String(m || "").trim().toLowerCase();
    if (SDXL_BASE_MODELS.includes(v)) return v;
    return "sdxl_realistic";
  }

  function isStyleCompatibleWithCat(s, cat) {
    if (!s) return false;
    if (s.id === "none") return true;
    if (cat === "sdxl") {
      return Array.isArray(s.models) && s.models.includes("sdxl");
    }
    return !s.models || s.models.includes("all") || s.models.includes(cat);
  }

  const initialSavedSliders = getLocalSliders();
  const rawSavedStyles = (initialSavedSliders && Array.isArray(initialSavedSliders.styles))
    ? initialSavedSliders.styles.filter((x) => x && x !== "none")
    : ((initialSavedSliders && initialSavedSliders.style && initialSavedSliders.style !== "none") ? [initialSavedSliders.style] : []);
  // Migrate legacy sdxl_* checkpoint IDs out of styles into base_model if present
  const legacyCkptFromStyles = rawSavedStyles.find((id) => SDXL_BASE_MODELS.includes(id));
  const initStyles = rawSavedStyles.filter((id) => !SDXL_BASE_MODELS.includes(id));
  const initPrimaryStyle = initStyles.length > 0 ? initStyles[0] : "none";

  const initArchetypes = (initialSavedSliders && Array.isArray(initialSavedSliders.archetypes))
    ? initialSavedSliders.archetypes.filter(Boolean)
    : ((initialSavedSliders && initialSavedSliders.archetype) ? [initialSavedSliders.archetype] : []);
  const initPrimaryArchetype = initArchetypes.length > 0 ? initArchetypes[0] : null;

  const rawSavedBaseModel = (initialSavedSliders && initialSavedSliders.base_model) || "";
  const initialHasStudioFlag = Boolean(
    isPrivateMode || (typeof window !== "undefined" && window.__omdProfile && window.__omdProfile.studio)
  );
  const initBaseModel = (!kreaSupported || !initialHasStudioFlag)
    ? normalizeSdxlModel(legacyCkptFromStyles || rawSavedBaseModel)
    : (isSdxlModel(rawSavedBaseModel)
        ? normalizeSdxlModel(legacyCkptFromStyles || rawSavedBaseModel)
        : (rawSavedBaseModel || "realism"));

  let lastSdxlModel = isSdxlModel(initBaseModel)
    ? normalizeSdxlModel(initBaseModel)
    : (legacyCkptFromStyles || "sdxl_realistic");
  let lastDitModel = (!isSdxlModel(initBaseModel) && !isVideoModel(initBaseModel)) ? initBaseModel : "realism";

  let videoQuality = (initialSavedSliders && initialSavedSliders.video_quality === "fast") ? "fast" : "hd";
  const VALID_VIDEO_DURATIONS = [5, 8, 10, 15];
  let videoDuration = (initialSavedSliders && VALID_VIDEO_DURATIONS.includes(Number(initialSavedSliders.video_duration)))
    ? Number(initialSavedSliders.video_duration)
    : 5;
  let videoRefMode = (initialSavedSliders && (initialSavedSliders.video_ref_mode === "subject" || initialSavedSliders.video_ref_mode === "keyframe"))
    ? initialSavedSliders.video_ref_mode
    : "keyframe";
  let extendingVideoItem = null;
  let videoAspect = (initialSavedSliders && initialSavedSliders.video_aspect) || "896x1152";

  const initRawSteps = (initialSavedSliders && typeof initialSavedSliders.steps === "number") ? initialSavedSliders.steps : null;
  const initRawCfg = (initialSavedSliders && typeof initialSavedSliders.cfg === "number") ? initialSavedSliders.cfg : null;

  // Sliders state (restored from localStorage if available, with DMD2 steps/cfg normalized in SDXL mode)
  let sliders = {
    base_model: initBaseModel,
    detail: (initialSavedSliders && typeof initialSavedSliders.detail === "number") ? initialSavedSliders.detail : 0.5,
    styles: initStyles,
    style_weights: (initialSavedSliders && typeof initialSavedSliders.style_weights === "object" && initialSavedSliders.style_weights) ? initialSavedSliders.style_weights : {},
    style: initPrimaryStyle,
    style_strength: (initialSavedSliders && typeof initialSavedSliders.style_strength === "number") ? initialSavedSliders.style_strength : 0.8,
    archetypes: initArchetypes,
    archetype_weights: (initialSavedSliders && typeof initialSavedSliders.archetype_weights === "object" && initialSavedSliders.archetype_weights) ? initialSavedSliders.archetype_weights : {},
    archetype: initPrimaryArchetype,
    archetype_strength: (initialSavedSliders && typeof initialSavedSliders.archetype_strength === "number") ? initialSavedSliders.archetype_strength : 0.4,
    aspect: (initialSavedSliders && initialSavedSliders.aspect) || "896x1152",
    ipadapter_weight: (initialSavedSliders && typeof initialSavedSliders.ipadapter_weight === "number") ? initialSavedSliders.ipadapter_weight : 0.65,
    face_restore_visibility: (initialSavedSliders && typeof initialSavedSliders.face_restore_visibility === "number") ? initialSavedSliders.face_restore_visibility : 0.30,
    inpaint_denoise: (initialSavedSliders && typeof initialSavedSliders.inpaint_denoise === "number") ? initialSavedSliders.inpaint_denoise : 0.94,
    video_quality: videoQuality,
    video_duration: videoDuration,
    video_aspect: videoAspect,
    steps: isVideoModel(initBaseModel)
      ? (videoQuality === "fast" ? 4 : 8)
      : (isSdxlModel(initBaseModel)
          ? ((initRawSteps !== null && initRawSteps >= 4 && initRawSteps <= 12) ? initRawSteps : 6)
          : (initRawSteps !== null ? initRawSteps : (initBaseModel === "anima" ? 25 : 12))),
    cfg: isVideoModel(initBaseModel)
      ? 1.0
      : (isSdxlModel(initBaseModel)
          ? ((initRawCfg !== null && initRawCfg >= 1.0 && initRawCfg <= 2.0) ? initRawCfg : 1.0)
          : (initRawCfg !== null ? initRawCfg : (initBaseModel === "realism" ? 1.5 : (initBaseModel === "semi_realism" ? 2.0 : 3.0))))
  };

  $: studioEngineMode = isVideoModel(sliders.base_model) ? "video" : (isSdxlModel(sliders.base_model) ? "sdxl" : "dit");

  function switchEngineMode(mode) {
    if (mode === "sdxl") {
      setBaseModel(lastSdxlModel || "sdxl_realistic");
    } else if (mode === "video") {
      setBaseModel("minimax_h3");
    } else {
      setBaseModel(lastDitModel || "realism");
    }
  }

  function setVideoQuality(q) {
    videoQuality = q === "fast" ? "fast" : "hd";
    sliders.video_quality = videoQuality;
    if (isVideoModel(sliders.base_model)) {
      sliders.steps = videoQuality === "fast" ? 4 : 8;
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function setVideoDuration(sec) {
    const n = Number(sec);
    videoDuration = VALID_VIDEO_DURATIONS.includes(n) ? n : 5;
    sliders.video_duration = videoDuration;
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function setVideoRefMode(mode) {
    videoRefMode = mode === "subject" ? "subject" : "keyframe";
    sliders.video_ref_mode = videoRefMode;
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function setVideoAspect(asp) {
    videoAspect = asp;
    sliders.video_aspect = asp;
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function setBaseModel(rawModel, force = false) {
    const model = rawModel === "sdxl" ? (lastSdxlModel || "sdxl_realistic") : rawModel;
    const requiresCloud = !isSdxlModel(model);
    const cloudPermitted = isVideoModel(model) ? canShowVideoTab : canShowStudioTab;
    if (requiresCloud && !force && (!kreaSupported || (statusLoadedOnce && !cloudPermitted))) {
      generationError = "";
      if (!isSdxlModel(sliders.base_model)) {
        sliders.base_model = lastSdxlModel || "sdxl_realistic";
        sliders.cfg = 1.0;
        sliders.steps = 6;
        sliders = { ...sliders };
        saveLocalSliders(sliders);
      }
      return;
    }
    if (isSdxlModel(model)) {
      lastSdxlModel = normalizeSdxlModel(model);
    } else if (!isVideoModel(model)) {
      lastDitModel = model;
    }
    if (sliders.base_model === model) {
      if (isSdxlModel(model)) {
        generationError = "";
        if (sliders.steps > 12 || sliders.cfg > 2.0) {
          sliders.steps = 6;
          sliders.cfg = 1.0;
          sliders = { ...sliders };
          saveLocalSliders(sliders);
        }
      } else if (isVideoModel(model)) {
        generationError = "";
        sliders.steps = videoQuality === "fast" ? 4 : 8;
        sliders.cfg = 1.0;
        sliders = { ...sliders };
        saveLocalSliders(sliders);
      }
      return;
    }
    sliders.base_model = model;
    if (isSdxlModel(model)) {
      sliders.cfg = 1.0;
      sliders.steps = 6;
      generationError = "";
    } else if (isVideoModel(model)) {
      sliders.cfg = 1.0;
      sliders.steps = videoQuality === "fast" ? 4 : 8;
      generationError = "";
    } else if (model === "realism") {
      sliders.cfg = 1.5;
      sliders.steps = 12;
    } else if (model === "semi_realism") {
      sliders.cfg = 2.0;
      sliders.steps = 12;
    } else {
      sliders.cfg = 3.0;
      sliders.steps = 25;
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function setAspect(aspect) {
    sliders.aspect = aspect;
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  $: if (typeof localStorage !== "undefined" && sliders) {
    saveLocalSliders(sliders);
  }

  let showSlidersPanel = true;
  let showPromptTokensPreview = false;
  let showStyleDropdown = false;
  let showArchetypeDropdown = false;
  let showAdvancedSettings = false;

  function closeAdvancedSettings() {
    showAdvancedSettings = false;
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function resetAdvancedToModelDefaults() {
    if (isSdxlModel(sliders.base_model)) {
      sliders.cfg = 1.0;
      sliders.steps = 6;
    } else if (sliders.base_model === "realism") {
      sliders.cfg = 1.5;
      sliders.steps = 12;
    } else if (sliders.base_model === "semi_realism") {
      sliders.cfg = 2.0;
      sliders.steps = 12;
    } else {
      sliders.cfg = 3.0;
      sliders.steps = 25;
    }
    sliders.detail = 0.5;
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  // 1. Art Styles & Archetypes from unified registry krea_presets.json + SDXL flow.json
  let kreaPresets = defaultKreaPresets;

  $: currentBaseCategory = isSdxlModel(sliders.base_model) ? "sdxl" : (sliders.base_model === "anima" ? "anima" : "krea2");

  $: filteredArtStyles = (kreaPresets.art_styles || []).filter((s) => isStyleCompatibleWithCat(s, currentBaseCategory));

  $: stylePresets = filteredArtStyles.map((s) => ({
    id: s.id,
    label: (s.icon ? s.icon + " " : "") + (s.id === "none" ? $_("krea_style_none", {}, s.name) : s.name),
    name: s.id === "none" ? $_("krea_style_none", {}, s.name) : s.name,
    desc: s.id === "none" ? $_("krea_style_none_desc", {}, s.description) : $_(`krea_preset_desc_${s.id}`, {}, s.description),
    trigger: s.trigger,
    lora: s.filename,
    strength: s.strength
  }));

  $: filteredArchetypes = (kreaPresets.archetypes || []).filter((a) => {
    if (!a.models || a.models.includes("all")) return true;
    return a.models.includes(currentBaseCategory);
  });

  $: archetypePresets = filteredArchetypes.map((a) => ({
    id: a.id,
    label: (a.icon ? a.icon + " " : "") + a.name,
    name: a.name,
    desc: $_(`krea_preset_desc_${a.id}`, {}, a.description),
    trigger: a.trigger,
    lora: a.filename,
    strength: a.strength
  }));

  const _studioRuntime = {
    prevBaseModel: sliders.base_model,
    driveCleanup: null,
    subscribedDrive: null,
    lastDriveInstance: null,
    lastDriveConnected: false,
    lastStoragePath: "",
    lastPassKeySignature: null,
    creationsLoadSeq: 0,
    initialMounted: false,
    isDestroyed: false
  };

  function syncBaseModelPresets(curModel, presetsObj) {
    if (curModel === _studioRuntime.prevBaseModel) return;
    _studioRuntime.prevBaseModel = curModel;
    const cat = isSdxlModel(curModel) ? "sdxl" : (curModel === "anima" ? "anima" : "krea2");
    const availableStyles = ((presetsObj && presetsObj.art_styles) || [])
      .filter((s) => isStyleCompatibleWithCat(s, cat))
      .map((s) => s.id);
    const availableArchetypes = ((presetsObj && presetsObj.archetypes) || [])
      .filter((a) => !a.models || a.models.includes("all") || a.models.includes(cat))
      .map((a) => a.id);

    let changed = false;
    if (sliders.styles && sliders.styles.length > 0) {
      const filtered = sliders.styles.filter((id) => availableStyles.includes(id));
      if (filtered.length !== sliders.styles.length) {
        sliders.styles = filtered;
        sliders.style = sliders.styles[0] || "none";
        changed = true;
      }
    }
    if (sliders.archetypes && sliders.archetypes.length > 0) {
      const filtered = sliders.archetypes.filter((id) => availableArchetypes.includes(id));
      if (filtered.length !== sliders.archetypes.length) {
        sliders.archetypes = filtered;
        sliders.archetype = sliders.archetypes[0] || null;
        changed = true;
      }
    }
    if (changed) {
      sliders = { ...sliders };
      saveLocalSliders(sliders);
    }
  }

  $: syncBaseModelPresets(sliders.base_model, kreaPresets);

  $: activeStyleObjs = stylePresets.filter((s) => (sliders.styles || []).includes(s.id));
  $: activeStyleObj = activeStyleObjs[0] || stylePresets.find((s) => s.id === sliders.style) || stylePresets[0] || { label: "✨ " + $_("krea_free_style", {}, "Свободный"), lora: "" };
  $: activeStyleLabel = (() => {
    const list = (sliders.styles || []).filter((id) => id !== "none");
    if (list.length === 0) return "✨ " + $_("krea_free_style", {}, "Свободный");
    if (list.length === 1) {
      const s = activeStyleObjs[0];
      const w = sliders.style_weights?.[s?.id] ?? s?.strength;
      return s ? `${s.name}${w !== undefined ? ` (${w})` : ""}` : $_("krea_1_style", {}, "1 стиль");
    }
    if (list.length === 2) {
      const s1 = activeStyleObjs[0];
      const s2 = activeStyleObjs[1];
      const w1 = sliders.style_weights?.[s1?.id] ?? s1?.strength;
      const w2 = sliders.style_weights?.[s2?.id] ?? s2?.strength;
      return `${s1?.name || "1"}${w1 !== undefined ? ` (${w1})` : ""}, ${s2?.name || "2"}${w2 !== undefined ? ` (${w2})` : ""}`;
    }
    return `${activeStyleObjs[0]?.name || $_("krea_style_label", {}, "Стиль")} +${list.length - 1} (${list.length})`;
  })();

  $: activeArchetypeObjs = archetypePresets.filter((a) => (sliders.archetypes || []).includes(a.id));
  $: activeArchetypeObj = activeArchetypeObjs[0] || null;
  $: activeArchetypeLabel = (() => {
    const list = sliders.archetypes || [];
    if (list.length === 0) return "✨ " + $_("krea_free_style", {}, "Свободный");
    if (list.length === 1) {
      const a = activeArchetypeObjs[0];
      const w = sliders.archetype_weights?.[a?.id] ?? a?.strength;
      return a ? `${a.name}${w !== undefined ? ` (${w})` : ""}` : $_("krea_1_archetype", {}, "1 типаж");
    }
    if (list.length === 2) {
      const a1 = activeArchetypeObjs[0];
      const a2 = activeArchetypeObjs[1];
      const w1 = sliders.archetype_weights?.[a1?.id] ?? a1?.strength;
      const w2 = sliders.archetype_weights?.[a2?.id] ?? a2?.strength;
      return `${a1?.name || "1"}${w1 !== undefined ? ` (${w1})` : ""}, ${a2?.name || "2"}${w2 !== undefined ? ` (${w2})` : ""}`;
    }
    return `${activeArchetypeObjs[0]?.name || $_("krea_archetype_label", {}, "Типаж")} +${list.length - 1} (${list.length})`;
  })();

  function getArchetypeToken(id) {
    const ap = archetypePresets.find((a) => a.id === id);
    if (!ap) return "";
    const weight = sliders.archetype_weights?.[id] ?? ap.strength ?? 0.8;
    const loraPart = ap.lora ? `LoRA: ${ap.lora.replace('.safetensors', '')} (${weight}) + ` : "";
    return `${loraPart}${ap.trigger || ""}`;
  }

  function getStyleToken(id) {
    const sp = stylePresets.find((s) => s.id === id);
    if (!sp || !sp.trigger) return "";
    const weight = sliders.style_weights?.[id] ?? sp.strength ?? 0.8;
    const loraPart = sp.lora ? `LoRA: ${sp.lora.replace('.safetensors', '')} (${weight}) + ` : "";
    return `${loraPart}${sp.trigger || ""}`;
  }

  $: activeStyleTokens = activeStyleObjs
    .map((sp) => getStyleToken(sp.id))
    .filter(Boolean)
    .join(" | ");

  $: activeArchetypeTokens = activeArchetypeObjs
    .map((ap) => getArchetypeToken(ap.id))
    .filter(Boolean)
    .join(" | ");

  // Prompt & Auto-resize
  let promptText = "";
  let textareaEl = null;

  // Reference Images (max 2)
  let attachedReferences = []; // [{ id, name, path, preview, dataUrl }]
  let refFileInputEl = null;
  let isUploadingRef = false;
  let sdxlRefMode = "faceswap"; // "faceswap" | "inpaint"
  let sdxlFaceTarget = "auto"; // "auto" | "left" | "right" | "second"
  let sdxlInpaintTarget = "outfit"; // "outfit" | "background" | "person"
  let activeRefPopoverIdx = null;

  // Prompt History
  let showPromptsPopup = false;
  let promptsSearchQuery = "";
  let recentPrompts = [];
  let unsubPrompts = null;

  $: filteredPrompts = recentPrompts
    .filter((p) => {
      if (!promptsSearchQuery.trim()) return true;
      const q = promptsSearchQuery.toLowerCase();
      const txt = (typeof p === "string" ? p : p.text || p.prompt || "").toLowerCase();
      const st = (p.style || "").toLowerCase();
      return txt.includes(q) || st.includes(q);
    })
    .sort((a, b) => {
      const ta = new Date(a.lastUsed || a.created || a.date || 0).getTime();
      const tb = new Date(b.lastUsed || b.created || b.date || 0).getTime();
      return tb - ta;
    });

  function resizeTextarea() {
    if (!textareaEl) return;
    const minH = 42;
    const maxH = 126;
    if (!promptText || !promptText.trim()) {
      textareaEl.style.height = `${minH}px`;
      textareaEl.style.overflowY = "hidden";
      return;
    }
    textareaEl.style.height = "auto";
    const scrollH = textareaEl.scrollHeight;
    const newH = Math.min(Math.max(scrollH, minH), maxH);
    textareaEl.style.height = `${newH}px`;
    textareaEl.style.overflowY = scrollH > maxH ? "auto" : "hidden";
  }

  function resizeDirectorTextarea() {
    if (!directorInputEl) return;
    const minH = 42;
    const maxH = 140;
    if (!directorInput || !directorInput.trim()) {
      directorInputEl.style.height = `${minH}px`;
      directorInputEl.style.overflowY = "hidden";
      return;
    }
    directorInputEl.style.height = "auto";
    const scrollH = directorInputEl.scrollHeight;
    const newH = Math.min(Math.max(scrollH, minH), maxH);
    directorInputEl.style.height = `${newH}px`;
    directorInputEl.style.overflowY = scrollH > maxH ? "auto" : "hidden";
  }

  // Storage resolution
  let showFolderPicker = false;
  let showStorageSourceChooser = false;
  let storageSources = [];
  let storagePickerDevice = null;
  let foundDevice = null;

  // Persona Modal (create / edit manually) — Persona 2.0 (Character/Lore + Native Render Preset)
  let showPersonaModal = false;
  let editingPersonaId = null;
  let personaFormName = "";
  let personaFormTitle = "";
  let personaFormPrompt = "";
  let personaFormPersonality = "";
  let personaFormAvatarData = "";
  let personaFormAvatarPreview = "";
  let personaFormCivitaiAccount = "";
  let personaFormPresetEnabled = false;
  let personaFormBaseModel = "realism";
  let personaFormArchetypes = [];
  let personaFormArchetypeWeights = {};
  let personaFormStyles = [];
  let personaFormStyleWeights = {};
  let personaFormUseFaceRef = true;
  let isSavingPersona = false;
  let isGeneratingAvatar = false;
  let isAnalyzingFace = false;
  let avatarGenArchetype = "prompt";

  $: personaModalEngineCat = isSdxlModel(personaFormBaseModel) ? "sdxl" : (personaFormBaseModel === "anima" ? "anima" : "krea2");
  $: personaModalArchetypes = (kreaPresets.archetypes || []).filter((a) => !a.models || a.models.includes("all") || a.models.includes(personaModalEngineCat));
  $: personaModalStyles = (kreaPresets.art_styles || []).filter((s) => s.id !== "none" && isStyleCompatibleWithCat(s, personaModalEngineCat));

  // Direct CivitAI API Publish Modal state
  let showCivitaiPublishModal = false;
  let civitaiPubMode = "single"; // "single" | "story"
  let civitaiPubItems = [];
  let civitaiPubTitle = "";
  let civitaiPubDetail = "";
  let civitaiPubTags = "";
  let civitaiPubAccountId = "";
  let civitaiPubCustomToken = "";
  let civitaiPubSaveKey = true;
  let civitaiPubImmediate = true;
  let civitaiPubIncludeMeta = true;
  let civitaiPubPersonaId = "";
  let isPublishingCivitai = false;
  let civitaiPubError = "";
  let civitaiPubResult = null;

  // AI Persona Factory Modal (Scenario A + Scenario B combined)
  let showAiFactoryModal = false;
  let aiFactoryArchetype = "alt";
  let isGeneratingAiPersona = false;
  let generatedAiPersona = null;
  // Confirmation Modal state
  let deleteConfirmTarget = null;
  let isDeletingItem = false;
  $: aiArchetypes = (kreaPresets.archetypes || []).map((a) => {
    const locDesc = $_(`krea_preset_desc_${a.id}`, {}, a.description);
    return {
      id: a.id,
      name: (a.icon ? a.icon + " " : "") + a.name,
      rawName: a.name,
      title: a.title || `${a.name} / ${locDesc}`,
      hint: locDesc + (a.filename ? " (LoRA)" : ""),
      trigger: a.trigger || "",
      icon: a.icon || "👤",
      lora: a.filename || ""
    };
  });
  $: if (!aiFactoryArchetype && aiArchetypes.length > 0) {
    aiFactoryArchetype = aiArchetypes[0].id;
  }

  // Dynamic Scene & Action Generator (combines vivid action, emotion, lighting, and camera angle)
  const dynamicActionScenes = [
    "running dynamically through a neon-lit rain-soaked alleyway, coat billowing, wet asphalt reflections",
    "adjusting dark sunglasses while leaning against a vintage sports car, wind blowing hair, golden sunset light",
    "sipping steaming espresso on a foggy morning Parisian balcony at sunrise, intimate mood, soft bokeh",
    "confidently leaping across city rooftops under a glowing moon, mid-air freeze frame, dynamic action pose",
    "laughing candidly at an outdoor summer music festival, warm sun flare, colorful confetti falling around",
    "crouching behind a concrete column in a high-tech data center, holding a tactical scanner, suspenseful atmosphere",
    "vibing with vintage over-ear headphones in an atmospheric vinyl record shop, warm retro tungsten lighting",
    "walking briskly through a crowded Tokyo Shibuya crossing at twilight, transparent umbrella, city motion blur",
    "drawing a ceremonial katana in a blossom-filled garden, sakura petals swirling in the gust of wind, dynamic stance",
    "relaxing on a deep emerald velvet lounge couch under warm amber lamps, looking over shoulder with an enigmatic smirk",
    "riding a sleek cafe racer motorcycle down an empty coastal highway at golden hour, hair flying in the sea breeze",
    "stepping out of a vintage yellow cab into rainy downtown Manhattan, steam rising from underground subway grates",
    "dancing under moody laser lights in an underground warehouse venue, subtle motion trails, electric atmosphere",
    "inspecting an antique camera at a sun-drenched wooden workbench, dust motes floating in sunbeams",
    "standing on an open rooftop helipad overlooking a vast futuristic metropolis skyline, strong wind blowing scarf"
  ];

  const dynamicCameraAngles = [
    "cinematic Dutch angle shot, intense dynamic composition, shallow depth of field",
    "low-angle heroic perspective, wide-angle 24mm lens, dramatic cinematic scale",
    "over-the-shoulder tracking perspective, anamorphic lens flare, motion blur",
    "cinematic eye-level candid medium shot, 50mm f/1.4 lens, creamy soft bokeh",
    "dynamic three-quarter action angle, sharp foreground focus, diagonal cinematic lines",
    "atmospheric high-angle establishing shot, cinematic framing, moody cinematic lighting"
  ];

  // LightGallery & Blob Cache
  let scrollContainerEl;
  let lgInstance = null;
  let lgPagination = null;
  let copiedPromptId = null;
  let imageBlobCache = {};
  let thumbBlobCache = {};

  // Prompt suggestions for empty gallery state
  const promptSuggestions = [
    "cinematic street portrait in rainy Tokyo, neon bokeh reflections, 35mm film",
    "candid cafe portrait, soft morning window light, delicate details, 8k raw photo",
    "dramatic moody editorial in vintage suit, chiaroscuro lighting, studio photography",
    "cyberpunk nighttime skyline, misty atmospheric lighting, ultra-high resolution"
  ];

  function resolveDevice() {
    if (foundDevice) return foundDevice;
    const raw = (
      defaultStorage ||
      (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
      (typeof window !== "undefined" && window.__omdProfile?.defaultStorage) ||
      ""
    ).trim();
    const parsed = parseStoragePath(raw);
    if (parsed.device && !parsed.device.isAiNode) return parsed.device;
    if (parsed.deviceName && parsed.deviceName.toLowerCase() !== "local") {
      const match = deviceRegistry.list().find(
        (d) => d && (
          (d.name && d.name.toLowerCase() === parsed.deviceName.toLowerCase()) ||
          (d.linkId && d.linkId.toLowerCase() === parsed.deviceName.toLowerCase())
        )
      );
      if (match && !match.isAiNode) return match;
    }
    if (deviceDrive) {
      const d = deviceRegistry.list().find((x) => x.drive === deviceDrive && !x.isAiNode);
      if (d) return d;
    }
    if (targetDevice && !targetDevice.isAiNode) return targetDevice;
    if (targetLinkId) {
      const d = deviceRegistry.get(targetLinkId) || deviceRegistry.getByPrefix(targetLinkId) || deviceRegistry.getByName(targetLinkId);
      if (d && !d.isAiNode) return d;
    }
    const connectedDev = deviceRegistry.list().find((d) => !d.isAiNode && d.drive && (d.drive.isConnected || d.drive.status === "connected"));
    if (connectedDev) return connectedDev;
    const local = getLocalDevice();
    if (local) return local;
    return (
      deviceRegistry.list().find((d) => !d.isAiNode) ||
      null
    );
  }

  function getTargetDrive() {
    const resDev = resolveDevice();
    return (
      resDev?.drive ||
      deviceDrive ||
      targetDevice?.drive ||
      (typeof window !== "undefined" && window.currentP2PDrive) ||
      null
    );
  }

  function syncDeviceDriveState(curDrive, curStorageRaw, privMode) {
    if (curDrive !== _studioRuntime.subscribedDrive) {
      if (_studioRuntime.driveCleanup) {
        try { _studioRuntime.driveCleanup(); } catch (_) {}
        _studioRuntime.driveCleanup = null;
      }
      _studioRuntime.subscribedDrive = curDrive;
      if (curDrive) {
        const onStatus = (st) => {
          if (st === "connected" && !_studioRuntime.isDestroyed) {
            loadCreations();
            loadPersonas();
          }
        };
        if (typeof curDrive.addStatusListener === "function") {
          curDrive.addStatusListener(onStatus);
          _studioRuntime.driveCleanup = () => {
            if (typeof curDrive.removeStatusListener === "function") {
              curDrive.removeStatusListener(onStatus);
            }
          };
        } else if (typeof curDrive.onStatusChange === "function") {
          const prev = curDrive.onStatusChange;
          curDrive.onStatusChange = (st) => {
            try { prev(st); } catch (_) {}
            onStatus(st);
          };
          _studioRuntime.driveCleanup = () => {
            curDrive.onStatusChange = prev;
          };
        }
      }
    }

    const isConn = !!(curDrive && (curDrive.isConnected || curDrive.status === "connected"));
    const curStorage = curStorageRaw || "";
    if (
      curDrive !== _studioRuntime.lastDriveInstance ||
      isConn !== _studioRuntime.lastDriveConnected ||
      curStorage !== _studioRuntime.lastStoragePath
    ) {
      const wasInitialized = _studioRuntime.initialMounted;
      _studioRuntime.lastDriveInstance = curDrive;
      _studioRuntime.lastDriveConnected = isConn;
      _studioRuntime.lastStoragePath = curStorage;
      if (wasInitialized) {
        foundDevice = null;
        items = [];
        imageBlobCache = {};
        thumbBlobCache = {};
        if (isConn || privMode || curStorage) {
          loadCreations();
          loadPersonas();
        }
      }
    }
  }

  $: syncDeviceDriveState(deviceDrive, defaultStorage, isPrivateMode);

  async function ensureFolderRecursive(drive, folder) {
    if (!drive) return;
    try {
      if (typeof drive.issueConsent === "function") {
        await drive.issueConsent(folder, { writable: true }).catch(() => {});
      }
      const parts = folder.split("/").filter(Boolean);
      let cur = "";
      for (const seg of parts) {
        cur += "/" + seg;
        if (typeof drive.homeMkDir === "function") {
          await drive.homeMkDir(cur).catch(() => {});
        } else if (typeof drive.mkDir === "function") {
          await drive.mkDir(cur).catch(() => {});
        } else if (typeof drive.mkdir === "function") {
          await drive.mkdir(cur).catch(() => {});
        }
      }
    } catch (_) {}
  }

  const ensureFolder = ensureFolderRecursive;

  async function writeDriveFile(drive, filePath, b64Data) {
    if (!drive || !filePath) return;
    try {
      if (typeof drive.writeFileChunked === "function") {
        await drive.writeFileChunked(filePath, b64Data, 32768);
      } else if (typeof drive.homeWrite === "function") {
        await drive.homeWrite(filePath, b64Data);
      } else if (typeof drive.putFile === "function") {
        await drive.putFile(filePath, b64Data, { encoding: "base64" });
      }
    } catch (e) {
      console.warn("[KreaStudio] writeDriveFile failed:", filePath, e);
    }
  }

  async function writeDriveText(drive, filePath, text) {
    if (!drive || !filePath) return;
    try {
      const b64 = btoa(unescape(encodeURIComponent(text)));
      await writeDriveFile(drive, filePath, b64);
    } catch (e) {
      console.warn("[KreaStudio] writeDriveText failed:", filePath, e);
    }
  }

  async function readDriveText(drive, filePath) {
    if (!drive || !filePath) return null;
    try {
      if (typeof drive.getFileContent === "function") {
        const blob = await drive.getFileContent(filePath);
        if (blob && typeof blob.text === "function") {
          return await blob.text();
        }
      } else if (typeof drive.getFile === "function") {
        const c = await drive.getFile(filePath);
        return typeof c === "string" ? c : JSON.stringify(c);
      }
    } catch (_) {}
    return null;
  }

  function guessImageMime(filePath, currentType = "") {
    if (currentType && currentType.startsWith("image/")) return currentType;
    const lower = String(filePath || "").toLowerCase();
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
    if (lower.endsWith(".webp")) return "image/webp";
    if (lower.endsWith(".gif")) return "image/gif";
    return "image/png";
  }

  async function readDriveBlobAsDataUrl(drive, filePath) {
    if (!drive || !filePath) return null;
    try {
      if (typeof drive.getFileContent === "function") {
        const rawBlob = await drive.getFileContent(filePath);
        if (rawBlob && rawBlob.size > 0) {
          const mime = guessImageMime(filePath, rawBlob.type);
          const blob = rawBlob.type === mime ? rawBlob : new Blob([rawBlob], { type: mime });
          return await new Promise((resolve) => {
            const reader = new FileReader();
            reader.onloadend = () => resolve(reader.result || null);
            reader.onerror = () => resolve(null);
            reader.readAsDataURL(blob);
          });
        }
      }
    } catch (_) {}
    return null;
  }

  function getBaseStorage() {
    let raw = (
      defaultStorage ||
      (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
      (typeof window !== "undefined" && window.__omdProfile?.defaultStorage) ||
      DEFAULT_STORAGE_PATH ||
      "/Data"
    ).trim();

    const parsed = (parseStoragePath && parseStoragePath(raw)) || {};
    let clean = (parsed.folder || parsed.subpath || raw || "").replace(/^\/+|\/+$/g, "");

    const knownUsers = [
      userName,
      typeof window !== "undefined" && window.__omdProfile?.user,
      typeof window !== "undefined" && window.__omdProfile?.name
    ].filter(Boolean);

    for (const u of knownUsers) {
      const uLower = u.toLowerCase();
      if (clean.toLowerCase() === uLower) {
        clean = (DEFAULT_STORAGE_PATH || "/Data").replace(/^\/+/, "");
        break;
      } else if (clean.toLowerCase().startsWith(uLower + "/")) {
        clean = clean.slice(u.length).replace(/^\/+/, "");
      }
    }

    clean = clean.replace(/^(home|Users)\/[^\/]+(\/|$)/i, "").replace(/^\/+|\/+$/g, "");
    clean = clean.replace(/^private\//i, "");
    if (clean.endsWith("/generated")) {
      clean = clean.slice(0, -"/generated".length);
    } else if (clean.endsWith("generated")) {
      clean = clean.slice(0, -"generated".length);
    }
    clean = clean.replace(/^\/+|\/+$/g, "");

    if (!clean) {
      clean = (DEFAULT_STORAGE_PATH || "/Data").replace(/^\/+/, "");
    }
    return clean || "Data";
  }

  function getTargetFolder() {
    const base = getBaseStorage();
    return `/${base}/generated`;
  }

  function getPersonasFolder() {
    const base = getBaseStorage();
    return `/${base}/personas`;
  }

  function getUploadFolder() {
    const base = getBaseStorage();
    return `/${base}/upload`;
  }

  async function handleRefFileSelected(e) {
    const file = e.target.files?.[0];
    if (!file) return;
    if (attachedReferences.length >= 2) {
      alert($_("krea_err_max_refs", {}, "Максимум 2 референса (Картинка 1 и Картинка 2)."));
      return;
    }
    isUploadingRef = true;
    try {
      const reader = new FileReader();
      const rawDataUrl = await new Promise((resolve, reject) => {
        reader.onload = () => resolve(reader.result);
        reader.onerror = reject;
        reader.readAsDataURL(file);
      });
      const dataUrl = await downscaleImage(rawDataUrl, 1024);
      const cleanName = `ref_${Date.now()}_${file.name.replace(/[^a-zA-Z0-9._-]/g, "_")}`;
      const uploadFolder = getUploadFolder();
      const drive = getTargetDrive();
      if (drive && (drive.isConnected || drive.status === "connected")) {
        try {
          await ensureFolderRecursive(drive, uploadFolder);
          const b64 = dataUrl.includes(",") ? dataUrl.split(",")[1] : dataUrl;
          await writeDriveFile(drive, `${uploadFolder}/${cleanName}`, b64);
        } catch (err) {
          console.warn("[KreaStudio] Error saving uploaded ref to drive:", err);
        }
      }
      attachedReferences = [
        ...attachedReferences,
        {
          id: `ref_${Date.now()}_${Math.random().toString(36).slice(2, 6)}`,
          name: file.name,
          path: `${uploadFolder}/${cleanName}`,
          preview: dataUrl,
          dataUrl: dataUrl
        }
      ];
    } catch (err) {
      console.error("[KreaStudio] Error reading reference image:", err);
      alert($_("krea_err_load_ref", { error: err.message }, `Ошибка загрузки референса: ${err.message}`));
    } finally {
      isUploadingRef = false;
      if (e.target) e.target.value = "";
    }
  }

  function removeReference(idx) {
    attachedReferences = attachedReferences.filter((_, i) => i !== idx);
    activeRefPopoverIdx = null;
  }

  function toggleRefPopover(idx) {
    if (studioEngineMode !== "sdxl") return;
    if (sdxlRefMode === "inpaint" && idx === 1 && sdxlInpaintTarget !== "person") return;
    activeRefPopoverIdx = activeRefPopoverIdx === idx ? null : idx;
  }

  function resetRefStrengths(mode = "face") {
    if (mode === "inpaint") {
      sliders = { ...sliders, inpaint_denoise: 0.94 };
    } else {
      sliders = { ...sliders, ipadapter_weight: 0.65, face_restore_visibility: 0.30 };
    }
  }

  function swapReferences() {
    if (attachedReferences.length >= 2) {
      attachedReferences = [attachedReferences[1], attachedReferences[0]];
    }
  }

  function getGalleryItemRefIndex(item, refs) {
    if (!item || item.isVideo || !Array.isArray(refs) || refs.length === 0) return -1;
    const itemPath = item.path || "";
    const itemName = item.title || item.name || "";
    return refs.findIndex((r) => {
      if (!r) return false;
      if (itemPath && r.path) return r.path === itemPath;
      return Boolean(itemName && r.name === itemName);
    });
  }

  async function resolveGalleryItemDataUrl(item) {
    let dataUrl = "";
    const drive = getTargetDrive();
    if (item.path && drive && (drive.isConnected || drive.status === "connected")) {
      try {
        dataUrl = await readDriveBlobAsDataUrl(drive, item.path);
      } catch (_) {}
    }
    if (!dataUrl) {
      const fullUrl = getFullImageUrl(item);
      try {
        const fetchFn = (fullUrl.startsWith("/p2p/") || fullUrl.startsWith("blob:"))
          ? (p, opts) => fetch(p, opts)
          : ((httpDrive && typeof httpDrive.fetch === "function")
              ? (p, opts) => httpDrive.fetch(p, opts)
              : (p, opts) => fetch(p, opts));
        const res = await fetchFn(fullUrl);
        if (res.ok) {
          const rawBlob = await res.blob();
          const mime = guessImageMime(item.path || item.name, rawBlob.type);
          const blob = rawBlob.type === mime ? rawBlob : new Blob([rawBlob], { type: mime });
          const reader = new FileReader();
          dataUrl = await new Promise((resolve, reject) => {
            reader.onload = () => resolve(reader.result);
            reader.onerror = reject;
            reader.readAsDataURL(blob);
          });
        }
      } catch (_) {}
    }
    return dataUrl || getFullImageUrl(item);
  }

  async function addGalleryItemAsReference(item) {
    if (!item || item.isVideo) return;
    const existingIdx = getGalleryItemRefIndex(item, attachedReferences);
    if (existingIdx >= 0) {
      removeReference(existingIdx);
      return;
    }
    const name = item.title || item.name || "Gallery image";
    const dataUrl = await resolveGalleryItemDataUrl(item);
    const newRef = {
      id: `ref_gallery_${Date.now()}_${Math.random().toString(36).slice(2, 6)}`,
      name,
      path: item.path || "",
      preview: dataUrl,
      dataUrl
    };
    if (studioEngineMode === "video") {
      setVideoRefMode("subject");
    }
    if (attachedReferences.length >= 2) {
      attachedReferences = [attachedReferences[0], newRef];
    } else {
      attachedReferences = [...attachedReferences, newRef];
    }
  }

  async function resolveGalleryVideoSource(item) {
    if (!item) return "";
    let dataUrl = "";
    const drive = getTargetDrive();
    if (item.path && drive && (drive.isConnected || drive.status === "connected")) {
      try {
        dataUrl = await readDriveBlobAsDataUrl(drive, item.path);
      } catch (_) {}
    }
    if (!dataUrl) {
      const fullUrl = getFullImageUrl(item);
      try {
        const fetchFn = (fullUrl.startsWith("/p2p/") || fullUrl.startsWith("blob:"))
          ? (p, opts) => fetch(p, opts)
          : ((httpDrive && typeof httpDrive.fetch === "function")
              ? (p, opts) => httpDrive.fetch(p, opts)
              : (p, opts) => fetch(p, opts));
        const res = await fetchFn(fullUrl);
        if (res.ok) {
          const rawBlob = await res.blob();
          const blob = rawBlob.type && rawBlob.type.startsWith("video/")
            ? rawBlob
            : new Blob([rawBlob], { type: "video/mp4" });
          const reader = new FileReader();
          dataUrl = await new Promise((resolve, reject) => {
            reader.onload = () => resolve(reader.result);
            reader.onerror = reject;
            reader.readAsDataURL(blob);
          });
        }
      } catch (_) {}
    }
    return dataUrl || item.path || item.url || getFullImageUrl(item);
  }

  async function extendGalleryVideo(item) {
    if (!item || !item.isVideo) return;
    if (!canShowVideoTab) {
      return;
    }
    if (extendingVideoItem && (extendingVideoItem.path === item.path || extendingVideoItem.name === item.name)) {
      extendingVideoItem = null;
      return;
    }
    const videoSource = await resolveGalleryVideoSource(item);
    const posterPreview = item.coverUrl || item.poster || "";
    extendingVideoItem = {
      id: `ext_vid_${Date.now()}`,
      name: item.title || item.name || "H3 Video",
      path: item.path || "",
      duration: item.duration || 5,
      preview: posterPreview,
      videoSource
    };
    switchEngineMode("video");
    if (attachedReferences.length > 0) {
      setVideoRefMode("subject");
    }
    activeTab = "gallery";
    await tick();
    if (textareaEl) {
      textareaEl.focus();
      resizeTextarea();
    }
  }

  function clearExtendingVideo() {
    extendingVideoItem = null;
  }

  async function animateGalleryItem(item) {
    if (!item || item.isVideo) return;
    if (!canShowVideoTab) {
      return;
    }

    const existingIdx = getGalleryItemRefIndex(item, attachedReferences);
    if (studioEngineMode === "video" && existingIdx >= 0 && videoRefMode === "keyframe") {
      removeReference(existingIdx);
      return;
    }

    extendingVideoItem = null;
    setVideoRefMode("keyframe");

    const appendSecondKeyframe = studioEngineMode === "video" && attachedReferences.length >= 1;
    const name = item.title || item.name || (appendSecondKeyframe ? "End frame" : "First frame");
    const dataUrl = await resolveGalleryItemDataUrl(item);

    // Auto-detect image aspect ratio to snap videoAspect when picking the primary #1 frame
    if (!appendSecondKeyframe) {
      try {
        const dims = await new Promise((resolve) => {
          const probe = new Image();
          probe.onload = () => resolve({ w: probe.naturalWidth || 0, h: probe.naturalHeight || 0 });
          probe.onerror = () => resolve(null);
          probe.src = dataUrl;
        });
        if (dims && dims.w > 0 && dims.h > 0) {
          const ratio = dims.w / dims.h;
          if (ratio < 0.66) setVideoAspect("768x1344");
          else if (ratio < 0.90) setVideoAspect("896x1152");
          else if (ratio <= 1.12) setVideoAspect("1024x1024");
          else if (ratio <= 1.55) setVideoAspect("1152x896");
          else setVideoAspect("1344x768");
        }
      } catch (_) {}
    }

    const newRef = {
      id: `ref_anim_${Date.now()}_${Math.random().toString(36).slice(2, 6)}`,
      name,
      path: item.path || "",
      preview: dataUrl,
      dataUrl
    };

    if (appendSecondKeyframe) {
      attachedReferences = [attachedReferences[0], newRef];
    } else {
      attachedReferences = [newRef];
      switchEngineMode("video");
    }

    activeTab = "gallery";
    await tick();
    if (textareaEl) {
      textareaEl.focus();
      resizeTextarea();
    }
  }

  $: displayFolder = getTargetFolder();

  function applyPersonaRenderPreset(p) {
    if (!p || !p.render_preset || p.render_preset.enabled === false) return;
    const rp = p.render_preset;
    const nextModel = rp.base_model || sliders.base_model || "realism";
    if (sliders.base_model !== nextModel) {
      setBaseModel(nextModel);
    }
    if (Array.isArray(rp.archetypes)) {
      sliders.archetypes = [...rp.archetypes];
      sliders.archetype = rp.archetypes[0] || null;
      sliders.archetype_weights = { ...(rp.archetype_weights || {}) };
      if (sliders.archetype && sliders.archetype_weights[sliders.archetype] !== undefined) {
        sliders.archetype_strength = sliders.archetype_weights[sliders.archetype];
      }
    }
    if (Array.isArray(rp.styles)) {
      sliders.styles = [...rp.styles];
      sliders.style = rp.styles[0] || "none";
      sliders.style_weights = { ...(rp.style_weights || {}) };
      if (sliders.style !== "none" && sliders.style_weights[sliders.style] !== undefined) {
        sliders.style_strength = sliders.style_weights[sliders.style];
      }
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function selectPersona(p) {
    if (selectedPersonas.some((x) => x.id === p.id)) {
      selectedPersonas = selectedPersonas.filter((x) => x.id !== p.id);
    } else {
      selectedPersonas = [...selectedPersonas, p];
      if (selectedPersonas.length === 1) {
        applyPersonaRenderPreset(p);
      }
    }
  }

  function removeSelectedPersona(pId) {
    selectedPersonas = selectedPersonas.filter((x) => x.id !== pId);
  }

  function toggleStyle(id, multi = false) {
    if (!sliders.styles) sliders.styles = [];
    if (!sliders.style_weights) sliders.style_weights = {};

    if (!multi) {
      if (sliders.styles.length === 1 && sliders.styles[0] === id) {
        sliders.styles = [];
        sliders.style = "none";
        sliders.style_weights = {};
      } else {
        sliders.styles = [id];
        sliders.style = id;
        const sp = stylePresets.find((s) => s.id === id);
        const str = sp?.strength ?? 0.8;
        sliders.style_weights = { [id]: str };
        sliders.style_strength = str;
      }
    } else {
      if (sliders.styles.includes(id)) {
        sliders.styles = sliders.styles.filter((x) => x !== id);
        delete sliders.style_weights[id];
      } else {
        sliders.styles = [...sliders.styles, id];
        const sp = stylePresets.find((s) => s.id === id);
        if (sp && sp.strength !== undefined && sp.strength !== null) {
          sliders.style_weights[id] = sp.strength;
        }
      }
      sliders.style = sliders.styles[0] || "none";
      if (sliders.styles.length > 0 && sliders.style_weights[sliders.style]) {
        sliders.style_strength = sliders.style_weights[sliders.style];
      }
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function updateStyleWeight(id, val) {
    if (!sliders.style_weights) sliders.style_weights = {};
    sliders.style_weights[id] = val;
    if (sliders.style === id) {
      sliders.style_strength = val;
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function toggleArchetype(id, multi = false) {
    if (!sliders.archetypes) sliders.archetypes = [];
    if (!sliders.archetype_weights) sliders.archetype_weights = {};

    if (!multi) {
      if (sliders.archetypes.length === 1 && sliders.archetypes[0] === id) {
        sliders.archetypes = [];
        sliders.archetype = null;
        sliders.archetype_weights = {};
      } else {
        sliders.archetypes = [id];
        sliders.archetype = id;
        const foundAp = archetypePresets.find((a) => a.id === id);
        const str = foundAp?.strength ?? 0.4;
        sliders.archetype_weights = { [id]: str };
        sliders.archetype_strength = str;
      }
    } else {
      if (sliders.archetypes.includes(id)) {
        sliders.archetypes = sliders.archetypes.filter((x) => x !== id);
        delete sliders.archetype_weights[id];
      } else {
        sliders.archetypes = [...sliders.archetypes, id];
        const foundAp = archetypePresets.find((a) => a.id === id);
        if (foundAp && foundAp.strength !== undefined && foundAp.strength !== null) {
          sliders.archetype_weights[id] = foundAp.strength;
        }
      }
      sliders.archetype = sliders.archetypes[0] || null;
      if (sliders.archetypes.length > 0 && sliders.archetype_weights[sliders.archetype]) {
        sliders.archetype_strength = sliders.archetype_weights[sliders.archetype];
      }
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function updateArchetypeWeight(id, val) {
    if (!sliders.archetype_weights) sliders.archetype_weights = {};
    sliders.archetype_weights[id] = val;
    if (sliders.archetype === id) {
      sliders.archetype_strength = val;
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  function captureStudioSettingsIntoPersonaForm() {
    personaFormPresetEnabled = true;
    personaFormBaseModel = sliders.base_model || "realism";
    personaFormArchetypes = Array.isArray(sliders.archetypes) ? [...sliders.archetypes.filter(Boolean)] : [];
    personaFormArchetypeWeights = { ...(sliders.archetype_weights || {}) };
    for (const aid of personaFormArchetypes) {
      if (personaFormArchetypeWeights[aid] === undefined) {
        const preset = (kreaPresets.archetypes || []).find((a) => a.id === aid);
        personaFormArchetypeWeights[aid] = preset?.strength ?? 0.8;
      }
    }
    personaFormStyles = Array.isArray(sliders.styles) ? [...sliders.styles.filter((s) => s && s !== "none")] : [];
    personaFormStyleWeights = { ...(sliders.style_weights || {}) };
    for (const sid of personaFormStyles) {
      if (personaFormStyleWeights[sid] === undefined) {
        const preset = (kreaPresets.art_styles || []).find((s) => s.id === sid);
        personaFormStyleWeights[sid] = preset?.strength ?? 0.8;
      }
    }
    personaFormUseFaceRef = personaFormArchetypes.length === 0;
  }

  function togglePersonaFormArchetype(archId) {
    if (personaFormArchetypes.includes(archId)) {
      personaFormArchetypes = personaFormArchetypes.filter((x) => x !== archId);
      const copy = { ...personaFormArchetypeWeights };
      delete copy[archId];
      personaFormArchetypeWeights = copy;
    } else {
      personaFormArchetypes = [...personaFormArchetypes, archId];
      const preset = (kreaPresets.archetypes || []).find((a) => a.id === archId);
      personaFormArchetypeWeights = { ...personaFormArchetypeWeights, [archId]: preset?.strength ?? 0.8 };
      personaFormUseFaceRef = false;
    }
  }

  function togglePersonaFormStyle(styleId) {
    if (personaFormStyles.includes(styleId)) {
      personaFormStyles = personaFormStyles.filter((x) => x !== styleId);
      const copy = { ...personaFormStyleWeights };
      delete copy[styleId];
      personaFormStyleWeights = copy;
    } else {
      personaFormStyles = [...personaFormStyles, styleId];
      const preset = (kreaPresets.art_styles || []).find((s) => s.id === styleId);
      personaFormStyleWeights = { ...personaFormStyleWeights, [styleId]: preset?.strength ?? 0.8 };
    }
  }

  function openCreatePersonaModal() {
    if (userCivitaiMcpApps.length > 0 && civitaiAccounts.length !== userCivitaiMcpApps.length) {
      refreshCivitaiAccounts();
    }
    editingPersonaId = null;
    personaFormName = "";
    personaFormTitle = "";
    personaFormPrompt = "";
    personaFormPersonality = "";
    personaFormAvatarData = "";
    personaFormAvatarPreview = "";
    personaFormCivitaiAccount = "";
    personaFormPresetEnabled = false;
    personaFormBaseModel = sliders.base_model || "realism";
    personaFormArchetypes = [];
    personaFormArchetypeWeights = {};
    personaFormStyles = [];
    personaFormStyleWeights = {};
    personaFormUseFaceRef = true;
    showPersonaModal = true;
  }

  function openEditPersonaModal(p) {
    if (userCivitaiMcpApps.length > 0 && civitaiAccounts.length !== userCivitaiMcpApps.length) {
      refreshCivitaiAccounts();
    }
    editingPersonaId = p.id;
    personaFormName = p.name || "";
    personaFormTitle = p.title || "";
    personaFormPrompt = p.prompt || "";
    personaFormPersonality = p.personality || "";
    personaFormAvatarData = p.avatarData || "";
    personaFormAvatarPreview = p.avatar || "";
    personaFormCivitaiAccount = p.civitai_account || "";
    const rp = p.render_preset;
    if (rp && typeof rp === "object") {
      personaFormPresetEnabled = rp.enabled !== false;
      personaFormBaseModel = rp.base_model || "realism";
      personaFormArchetypes = Array.isArray(rp.archetypes) ? [...rp.archetypes] : (p.archetype ? [p.archetype] : []);
      personaFormArchetypeWeights = { ...(rp.archetype_weights || {}) };
      personaFormStyles = Array.isArray(rp.styles) ? [...rp.styles] : [];
      personaFormStyleWeights = { ...(rp.style_weights || {}) };
      personaFormUseFaceRef = rp.use_face_ref !== undefined ? Boolean(rp.use_face_ref) : (personaFormArchetypes.length === 0);
    } else if (p.archetype) {
      personaFormPresetEnabled = true;
      personaFormBaseModel = "realism";
      personaFormArchetypes = [p.archetype];
      personaFormArchetypeWeights = { [p.archetype]: 0.8 };
      personaFormStyles = [];
      personaFormStyleWeights = {};
      personaFormUseFaceRef = false;
    } else {
      personaFormPresetEnabled = false;
      personaFormBaseModel = sliders.base_model || "realism";
      personaFormArchetypes = [];
      personaFormArchetypeWeights = {};
      personaFormStyles = [];
      personaFormStyleWeights = {};
      personaFormUseFaceRef = true;
    }
    showPersonaModal = true;
  }

  function openAiPersonaModal() {
    aiFactoryArchetype = "alt";
    generatedAiPersona = null;
    showAiFactoryModal = true;
  }

  function downscaleImage(dataUrl, maxDim = 768) {
    return new Promise((resolve) => {
      if (typeof Image === "undefined") {
        resolve(dataUrl);
        return;
      }
      const img = new Image();
      img.onload = () => {
        let w = img.width;
        let h = img.height;
        if (w <= maxDim && h <= maxDim) {
          resolve(dataUrl);
          return;
        }
        if (w > h) {
          h = Math.round((h * maxDim) / w);
          w = maxDim;
        } else {
          w = Math.round((w * maxDim) / h);
          h = maxDim;
        }
        const canvas = document.createElement("canvas");
        canvas.width = w;
        canvas.height = h;
        const ctx = canvas.getContext("2d");
        ctx.drawImage(img, 0, 0, w, h);
        resolve(canvas.toDataURL("image/jpeg", 0.9));
      };
      img.onerror = () => resolve(dataUrl);
      img.src = dataUrl;
    });
  }

  async function handleAvatarUpload(e) {
    const file = e.target.files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = async (ev) => {
      const rawDataUrl = ev.target.result;
      const dataUrl = await downscaleImage(rawDataUrl, 768);
      personaFormAvatarData = dataUrl;
      personaFormAvatarPreview = dataUrl;
      if (!personaFormPrompt.trim()) {
        await analyzeFaceForPassport(dataUrl);
      }
    };
    reader.readAsDataURL(file);
  }

  async function analyzeFaceForPassport(imgInput) {
    const imgData = imgInput || personaFormAvatarData || personaFormAvatarPreview;
    if (!imgData) {
      alert($_("krea_err_upload_face_first", {}, "Сначала загрузите фотографию лица."));
      return;
    }
    isAnalyzingFace = true;
    try {
      const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
        ? (p, opts) => httpDrive.fetch(p, opts)
        : (p, opts) => fetch(p, opts);

      const res = await fetchFn("/personas/describe-face", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          image: imgData,
          omd_key: omdKey || ""
        })
      });

      if (!res.ok) {
        throw new Error(`HTTP ${res.status}`);
      }

      const data = await res.json();
      if (data.prompt) {
        personaFormPrompt = data.prompt;
      }
      if (data.title && !personaFormTitle.trim()) {
        personaFormTitle = data.title;
      }
    } catch (err) {
      console.warn("[KreaStudio] Face vision error:", err);
      alert($_("krea_err_vision_failed", { error: err.message }, `Не удалось распознать черты лица через Vision: ${err.message}`));
    } finally {
      isAnalyzingFace = false;
    }
  }

  const archetypeDefaultPassports = {
    domi: "DomiK, attractive woman with piercing hazel gaze, shoulder-length stylish layered haircut, confident radiant smile, modern chic outfit, magnetic presence",
    ada: "ada_wong, short sleek black bob hair, elegant crimson silk dress, captivating dark eyes, confident enigmatic expression, cinematic espionage aesthetics",
    brie: "kxbrie woman, blonde wavy hair, sparkling blue eyes, radiant playful smile, sun-kissed natural complexion, stylish casual summer attire",
    tifa: "Stunning athletic martial artist with long waist-length glossy black hair tied in dolphin-tail tip, captivating ruby-red eyes, toned athletic build, iconic white top with suspenders",
    alt: "Young alternative woman with chin-length jet-black hair with vibrant purple streaks, dark winged eyeliner, small silver nose ring, velvet choker, expressive moody gaze",
    influencer: "Stunning 22-year-old lifestyle influencer with long wavy honey-blonde hair, warm hazel eyes, glowing tanned skin, soft natural makeup, charming candid smile",
    business: "Sophisticated modern woman with sleek dark brunette bob, confident warm brown eyes, sharp tailored blazer, intelligent poised expression",
    cyberpunk: "Futuristic young cyber-operative with asymmetrical platinum hair and teal neon tips, glowing optic cyberware around temple, dark techwear jacket",
    anime: "Artistic girl with voluminous pastel pink wavy hair, large luminous violet eyes, playful gentle expression, delicate porcelain skin",
    luna: "Lun4, attractive young woman, sweet and flirtatious gaze, charming smile, alluring presence"
  };

  async function generateAvatarForPersona() {
    if (isGeneratingAvatar) return;
    let cleanPrompt = personaFormPrompt.trim();
    if (!cleanPrompt && avatarGenArchetype && avatarGenArchetype !== "prompt") {
      const archPreset = (kreaPresets.archetypes || []).find(a => a.id === avatarGenArchetype);
      cleanPrompt = (archPreset && archPreset.trigger) || archetypeDefaultPassports[avatarGenArchetype] || "";
      if (cleanPrompt) {
        personaFormPrompt = cleanPrompt;
      }
    }

    if (!cleanPrompt && avatarGenArchetype === "prompt") {
      alert($_("krea_err_fill_passport", {}, "Заполните словесный паспорт персоны или выберите типаж для генерации аватара."));
      return;
    }

    isGeneratingAvatar = true;
    try {
      const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
        ? (p, opts) => httpDrive.fetch(p, opts)
        : (p, opts) => fetch(p, opts);

      let chosenSliders = { style: "photorealistic", detail: 0.5 };
      if (avatarGenArchetype && avatarGenArchetype !== "prompt") {
        chosenSliders.archetypes = [avatarGenArchetype];
      }

      const mcpPayload = getMcpPayload();
      const res = await fetchFn("/generate/krea2", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          prompt: "close-up headshot avatar portrait, looking directly at camera, soft studio rim light, natural skin detail, crisp sharp focus",
          persona_prompt: cleanPrompt || "",
          sliders: chosenSliders,
          width: 768,
          height: 768,
          steps: 6,
          cfg: 1.0,
          omd_key: omdKey || "",
          mcp_keys: mcpPayload,
          settings: { mcp_keys: mcpPayload }
        })
      });

      if (!res.ok) {
        throw new Error(`HTTP ${res.status}`);
      }

      const data = await res.json();
      isGpuWarm = true;
      lastGenFinishedAt = Date.now();
      gpuWarmUntil = lastGenFinishedAt + 290000;
      if (data.url) {
        const imgResp = await fetchFn(data.url);
        if (imgResp.ok) {
          const blob = await imgResp.blob();
          const base64 = await new Promise((resolve) => {
            const reader = new FileReader();
            reader.onloadend = () => resolve(reader.result);
            reader.onerror = () => resolve(null);
            reader.readAsDataURL(blob);
          });
          if (base64) {
            personaFormAvatarPreview = base64;
            personaFormAvatarData = base64;
          }
        }
      }
    } catch (err) {
      console.error("[KreaStudio] Avatar generation error:", err);
      alert($_("krea_err_avatar_gen", { error: err.message }, `Ошибка генерации аватара: ${err.message}`));
    } finally {
      isGeneratingAvatar = false;
    }
  }

  async function savePersona() {
    if (!personaFormName.trim() || !personaFormPrompt.trim()) return;
    isSavingPersona = true;
    try {
      const id = editingPersonaId || ("persona_" + Date.now());
      const existingPersona = personas.find((x) => x.id === id) || {};
      const renderPresetObj = personaFormPresetEnabled
        ? {
            enabled: true,
            base_model: personaFormBaseModel || "realism",
            archetypes: [...personaFormArchetypes],
            archetype_weights: { ...personaFormArchetypeWeights },
            styles: [...personaFormStyles],
            style_weights: { ...personaFormStyleWeights },
            use_face_ref: Boolean(personaFormUseFaceRef)
          }
        : null;
      const primaryArchId = personaFormPresetEnabled && personaFormArchetypes.length > 0
        ? personaFormArchetypes[0]
        : (existingPersona.archetype || "");
      const primaryArchPreset = primaryArchId
        ? (kreaPresets.archetypes || []).find((a) => a.id === primaryArchId)
        : null;

      const pObj = {
        ...existingPersona,
        id,
        name: personaFormName.trim(),
        title: personaFormTitle.trim() || "Character",
        prompt: personaFormPrompt.trim(),
        personality: personaFormPersonality.trim(),
        archetype: primaryArchId,
        lora: primaryArchPreset?.filename || existingPersona.lora || "",
        render_preset: renderPresetObj,
        avatar: personaFormAvatarPreview || "",
        avatarData: personaFormAvatarData || "",
        civitai_account: personaFormCivitaiAccount || "",
        createdAt: existingPersona.createdAt || Date.now(),
        updatedAt: Date.now()
      };

      // 1. Первичное и авторитетное хранилище — целевой накопитель пользователя (P2P Drive)
      const drive = getTargetDrive();
      if (drive && (drive.isConnected || drive.status === "connected")) {
        const pFolder = getPersonasFolder();
        await ensureFolderRecursive(drive, pFolder);
        try {
          await writeDriveText(drive, `${pFolder}/${id}.json`, JSON.stringify(pObj, null, 2));
          if (personaFormAvatarData) {
            const b64 = personaFormAvatarData.includes(",") ? personaFormAvatarData.split(",")[1] : personaFormAvatarData;
            await writeDriveFile(drive, `${pFolder}/${id}.png`, b64);
          }
        } catch (err) {
          console.warn("[KreaStudio] Failed saving persona to drive:", err);
        }
      }

      // 2. На AI-ноду сохраняем ТОЛЬКО если пользователь явно выбрал саму ноду как целевой накопитель
      const targetDev = resolveDevice();
      const isAiNodeTarget = Boolean(targetDev && targetDev.isAiNode);
      if (isAiNodeTarget) {
        try {
          const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
            ? (p, opts) => httpDrive.fetch(p, opts)
            : (p, opts) => fetch(p, opts);
          const postPayload = JSON.stringify({ ...pObj, omd_key: omdKey || "", save_local: true });
          const postHeaders = { "Content-Type": "application/json" };
          const res = await fetchFn("/personas", {
            method: "POST",
            headers: postHeaders,
            body: postPayload
          });
          if (res.ok) {
            const resData = await res.json();
            if (resData.persona) {
              if (resData.persona.avatar_url) pObj.avatar_url = resData.persona.avatar_url;
              if (!pObj.avatar && resData.persona.avatar) pObj.avatar = resData.persona.avatar;
            }
          }
        } catch (err) {
          console.warn("[KreaStudio] Error saving persona to AI node:", err);
        }
      }

      const existingIdx = personas.findIndex((x) => x.id === id);
      if (existingIdx !== -1) {
        personas[existingIdx] = pObj;
        personas = [...personas];
      } else {
        personas = [pObj, ...personas];
      }

      if (selectedPersonas.some((x) => x.id === id)) {
        selectedPersonas = selectedPersonas.map((x) => (x.id === id ? pObj : x));
      }

      showPersonaModal = false;
    } catch (err) {
      console.error("[KreaStudio] Error saving persona:", err);
    } finally {
      isSavingPersona = false;
    }
  }

  function deletePersona(p, e) {
    if (e) {
      e.preventDefault();
      e.stopPropagation();
    }
    deleteConfirmTarget = {
      type: "persona",
      item: p,
      title: $_("delete_persona_title", {}, "Удалить персону"),
      name: p.name || $_("krea_persona", {}, "Персона"),
      subtext: $_("delete_persona_subtext", {}, "Словесный паспорт и аватар персоны будут безвозвратно удалены.")
    };
  }

  async function executeDeletePersona(p) {
    // 1. Удаляем из целевого накопителя (P2P Drive)
    const drive = getTargetDrive();
    if (drive && (drive.isConnected || drive.status === "connected")) {
      const pFolder = getPersonasFolder();
      try {
        if (typeof drive.unlink === "function") {
          await drive.unlink(`${pFolder}/${p.id}.json`).catch(() => {});
          await drive.unlink(`${pFolder}/${p.id}.png`).catch(() => {});
        } else if (typeof drive.removeFile === "function") {
          await drive.removeFile(`${pFolder}/${p.id}.json`).catch(() => {});
          await drive.removeFile(`${pFolder}/${p.id}.png`).catch(() => {});
        } else if (typeof drive.deleteFile === "function") {
          await drive.deleteFile(`${pFolder}/${p.id}.json`).catch(() => {});
          await drive.deleteFile(`${pFolder}/${p.id}.png`).catch(() => {});
        }
      } catch (_) {}
    }

    // 2. С AI-ноды удаляем ТОЛЬКО если она была выбрана как целевой накопитель
    const targetDev = resolveDevice();
    if (targetDev && targetDev.isAiNode) {
      try {
        const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
          ? (p, opts) => httpDrive.fetch(p, opts)
          : (p, opts) => fetch(p, opts);
        await fetchFn(`/personas/${p.id}`, { method: "DELETE" });
      } catch (err) {
        console.warn("[KreaStudio] Error deleting persona via API:", err);
      }
    }

    personas = personas.filter((x) => x.id !== p.id);
    selectedPersonas = selectedPersonas.filter((x) => x.id !== p.id);
  }

  async function generateAiPersona() {
    isGeneratingAiPersona = true;
    generatedAiPersona = null;
    try {
      const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
        ? (p, opts) => httpDrive.fetch(p, opts)
        : (p, opts) => fetch(p, opts);

      const mcpPayload = getMcpPayload();
      const res = await fetchFn("/personas/generate", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          archetype: aiFactoryArchetype,
          omd_key: omdKey || "",
          mcp_keys: mcpPayload,
          settings: { mcp_keys: mcpPayload }
        })
      });

      if (!res.ok) {
        throw new Error(`HTTP ${res.status}`);
      }

      const data = await res.json();
      if (data && data.avatar) {
        isGpuWarm = true;
        lastGenFinishedAt = Date.now();
        gpuWarmUntil = lastGenFinishedAt + 290000;
      }
      generatedAiPersona = data;
    } catch (err) {
      console.error("[KreaStudio] AI Persona generation error:", err);
      alert($_("krea_err_persona_gen", { error: err.message }, `Ошибка генерации персоны: ${err.message}`));
    } finally {
      isGeneratingAiPersona = false;
    }
  }

  async function saveAiGeneratedPersona() {
    if (!generatedAiPersona) return;
    const pObj = {
      id: generatedAiPersona.id || ("persona_" + Date.now()),
      name: generatedAiPersona.name,
      title: generatedAiPersona.title,
      prompt: generatedAiPersona.prompt,
      avatar: generatedAiPersona.avatar || "",
      avatarData: generatedAiPersona.avatarData || generatedAiPersona.avatar || "",
      createdAt: Date.now()
    };

    // 1. Первичное сохранение — в целевой накопитель (P2P Drive)
    const drive = getTargetDrive();
    if (drive && (drive.isConnected || drive.status === "connected")) {
      const pFolder = getPersonasFolder();
      await ensureFolderRecursive(drive, pFolder);
      try {
        await writeDriveText(drive, `${pFolder}/${pObj.id}.json`, JSON.stringify(pObj, null, 2));
        if (pObj.avatarData) {
          const b64 = pObj.avatarData.includes(",") ? pObj.avatarData.split(",")[1] : pObj.avatarData;
          await writeDriveFile(drive, `${pFolder}/${pObj.id}.png`, b64);
        }
      } catch (err) {
        console.warn("[KreaStudio] Error saving generated AI persona to drive:", err);
      }
    }

    // 2. На AI-ноду сохраняем ТОЛЬКО если нода выбрана как целевой накопитель
    const targetDev = resolveDevice();
    const isAiNodeTarget = Boolean(targetDev && targetDev.isAiNode);
    if (isAiNodeTarget) {
      try {
        const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
          ? (p, opts) => httpDrive.fetch(p, opts)
          : (p, opts) => fetch(p, opts);
        const postPayload = JSON.stringify({ ...pObj, omd_key: omdKey || "", save_local: true });
        const postHeaders = { "Content-Type": "application/json" };
        const res = await fetchFn("/personas", {
          method: "POST",
          headers: postHeaders,
          body: postPayload
        });
        if (res.ok) {
          const resData = await res.json();
          if (resData.persona) {
            if (resData.persona.avatar_url) pObj.avatar_url = resData.persona.avatar_url;
            if (!pObj.avatar && resData.persona.avatar) pObj.avatar = resData.persona.avatar;
          }
        }
      } catch (err) {
        console.warn("[KreaStudio] Error saving generated persona to AI node:", err);
      }
    }

    personas = [pObj, ...personas];
    selectedPersonas = [pObj, ...selectedPersonas.filter((x) => x.id !== pObj.id)];
    showAiFactoryModal = false;
    generatedAiPersona = null;
  }

  async function loadPersonas() {
    let loaded = [];
    const drive = getTargetDrive();
    const pFolder = getPersonasFolder();

    // 1. Сначала загружаем персоны с диска через бэкенд API (/personas)
    try {
      const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
        ? (p, opts) => httpDrive.fetch(p, opts)
        : (p, opts) => fetch(p, opts);
      const res = await fetchFn("/personas");
      if (res.ok) {
        const data = await res.json();
        if (Array.isArray(data.personas) && data.personas.length > 0) {
          loaded = data.personas;
        }
      }
    } catch (err) {
      console.warn("[KreaStudio] Error loading personas from API:", err);
    }

    // 2. Дополняем и синхронизируем из P2P Drive (/${base}/personas)
    if (drive && !drive.isConnected && typeof drive.waitUntilConnected === "function") {
      await drive.waitUntilConnected(2000).catch(() => {});
    }

    if (drive && (drive.isConnected || drive.status === "connected")) {
      await ensureFolderRecursive(drive, pFolder);
      try {
        let files = [];
        if (typeof drive.getFiles === "function") {
          files = await drive.getFiles(pFolder);
        } else if (typeof drive.list === "function") {
          files = await drive.list(pFolder);
        }
        if (Array.isArray(files)) {
          const jsonFiles = files.filter((f) => {
            const name = f.name || f.value || f;
            return typeof name === "string" && name.endsWith(".json") && !name.startsWith(".");
          });
          const drivePersonas = [];
          for (const jf of jsonFiles) {
            const fname = jf.name || jf.value || jf;
            try {
              const text = await readDriveText(drive, `${pFolder}/${fname}`);
              if (text) {
                const pData = JSON.parse(text);
                const baseName = fname.replace(/\.json$/, "");
                const avatarData = await readDriveBlobAsDataUrl(drive, `${pFolder}/${baseName}.png`);
                if (avatarData) {
                  pData.avatar = avatarData;
                  pData.avatarData = avatarData;
                }
                drivePersonas.push(pData);
              }
            } catch (_) {}
          }
          if (drivePersonas.length > 0) {
            const driveIds = new Set(drivePersonas.map((p) => p.id));
            loaded = [...drivePersonas, ...loaded.filter((p) => !driveIds.has(p.id))];
          }
        }
      } catch (_) {}
    }

    personas = loaded;
    // Подгружаем аватары через fetchFn и конвертируем в Data URL (base64) для автономной передачи
    const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
      ? (path, opts) => httpDrive.fetch(path, opts)
      : (path, opts) => fetch(path, opts);
    for (const p of personas) {
      if (p.avatar && (p.avatar.startsWith("/personas/") || p.avatar.startsWith("/assistant/avatar")) && !p.avatar.startsWith("data:")) {
        try {
          const avRes = await fetchFn(p.avatar);
          if (avRes.ok) {
            const blob = await avRes.blob();
            if (blob && blob.size > 0) {
              const reader = new FileReader();
              const b64 = await new Promise((res) => {
                reader.onload = () => res(reader.result);
                reader.onerror = () => res(null);
                reader.readAsDataURL(blob);
              });
              if (b64) {
                p.avatar = b64;
                p.avatarData = b64;
              }
            }
          }
        } catch (_) {}
      }
    }
    personas = [...personas];
    selectedPersonas = getLocalSelectedPersonas(personas);
  }

  async function loadCreations() {
    const loadSeq = ++_studioRuntime.creationsLoadSeq;
    isLoadingFiles = true;
    const targetFolder = getTargetFolder();
    let rawList = [];
    let effectiveFolder = targetFolder;

    const resDev = resolveDevice();
    let activeDrive = resDev?.drive || deviceDrive;

    if (activeDrive && !activeDrive.isConnected && typeof activeDrive.waitUntilConnected === "function") {
      await activeDrive.waitUntilConnected(2000).catch(() => {});
    }

    const rawStorage = (defaultStorage || "").trim();
    const parsedStorage = parseStoragePath(rawStorage);
    const hasSpecificDevice = !!(parsedStorage.deviceName && parsedStorage.deviceName.toLowerCase() !== "local");

    // When a specific target device is configured (e.g. /Linux-desktop/Private/OnMyChat),
    // strictly scan ONLY that target folder on that device!
    const candidateFolders = [targetFolder];
    if (!hasSpecificDevice) {
      const cleanBase = getBaseStorage();
      if (cleanBase && !candidateFolders.includes(`/${cleanBase}/generated`)) {
        candidateFolders.push(`/${cleanBase}/generated`);
      }
      if (!candidateFolders.includes("/Private/Data/generated")) {
        candidateFolders.push("/Private/Data/generated");
      }
      if (!candidateFolders.includes("/Data/generated")) {
        candidateFolders.push("/Data/generated");
      }
      if (!candidateFolders.includes("/Private/OnMyChat/generated")) {
        candidateFolders.push("/Private/OnMyChat/generated");
      }
      if (!candidateFolders.includes("/OnMyChat/generated")) {
        candidateFolders.push("/OnMyChat/generated");
      }
      if (!candidateFolders.includes("/Data/generated")) {
        candidateFolders.push("/Data/generated");
      }
      if (!candidateFolders.includes("/generated")) {
        candidateFolders.push("/generated");
      }
    }

    try {
      if (activeDrive && typeof activeDrive.getFiles === "function" && (activeDrive.isConnected || activeDrive.status === "connected")) {
        for (const cand of candidateFolders) {
          try {
            const devFiles = await activeDrive.getFiles(cand);
            if (Array.isArray(devFiles) && devFiles.length > 0) {
              rawList = devFiles;
              effectiveFolder = cand;
              break;
            }
          } catch (e) {}
        }
      }

      // ONLY if NO specific device was chosen and no files were found, check other local devices or httpDrive
      if (!hasSpecificDevice && (!rawList || rawList.length === 0)) {
        const otherConnected = deviceRegistry.list().filter(
          (d) => d && !d.isAiNode && d.drive && (d.drive.isConnected || d.drive.status === "connected") && d.drive !== activeDrive
        );
        for (const dev of otherConnected) {
          for (const cand of candidateFolders) {
            try {
              const devFiles = await dev.drive.getFiles(cand);
              if (Array.isArray(devFiles) && devFiles.length > 0) {
                rawList = devFiles;
                effectiveFolder = cand;
                activeDrive = dev.drive;
                foundDevice = dev;
                break;
              }
            } catch (_) {}
          }
          if (rawList && rawList.length > 0) break;
        }
      }

      if (!hasSpecificDevice && (!rawList || rawList.length === 0) && httpDrive && typeof httpDrive.fetch === "function") {
        for (const cand of candidateFolders.slice(0, 2)) {
          try {
            const res = await httpDrive.fetch(`/api/v1/files?path=${encodeURIComponent(cand)}`);
            if (res && res.ok) {
              const data = await res.json();
              const list = data.list || data.files || [];
              if (Array.isArray(list) && list.length > 0) {
                rawList = list;
                effectiveFolder = cand;
                break;
              }
            }
          } catch (e) {}
        }
      }
      const dirItems = (rawList || []).filter((f) => {
        const name = f.name || f.value || "";
        return name && !name.startsWith(".") && (f.isDir || f.type === "folder");
      });
      if (!activeStorySubfolder) {
        storyFolders = dirItems.map((d) => d.name || d.value).sort();
      } else if (activeDrive && typeof activeDrive.getFiles === "function") {
        try {
          const subPath = `${effectiveFolder}/${activeStorySubfolder}`.replace(/\/+/g, "/");
          const subFiles = await activeDrive.getFiles(subPath);
          if (Array.isArray(subFiles)) {
            rawList = subFiles;
            effectiveFolder = subPath;
          }
        } catch (_) {
          rawList = [];
        }
      }
    } catch (err) {
      console.error("[KreaStudio] Error scanning folder:", err);
    }

    const imageExtRegex = /\.(jpe?g|png|webp|gif|mp4|webm|mov)$/i;
    const imageFiles = (rawList || []).filter((f) => {
      const name = f.name || f.value || "";
      if (!name) return false;
      if (name.startsWith(".") || name === "folder.jpg" || name.endsWith(".cover.jpg") || name.endsWith(".cover.png")) return false;
      return imageExtRegex.test(name) && !f.isDir;
    });

    let localPromptCache = {};
    try {
      localPromptCache = JSON.parse(localStorage.getItem("krea_file_prompts") || "{}");
    } catch (_) {}

    const parsedItems = imageFiles.map((f) => {
      const name = f.name || f.value;
      const cleanPath = `${effectiveFolder}/${name}`.replace(/\/+/g, "/");
      const isVid = videoExtRegex.test(name);
      let mtime = getItemTimestamp({ name, date: f.lastModified || f.date || (f.mtime ? f.mtime * 1000 : null) }) || Date.now();
      return {
        id: name,
        name,
        path: cleanPath,
        isVideo: isVid,
        title: name.replace(/\.[^.]+$/, ""),
        description: "",
        prompt: localPromptCache[name] || "",
        blobUrl: imageBlobCache[cleanPath] || imageBlobCache[name] || null,
        thumbBlobUrl: thumbBlobCache[cleanPath] || thumbBlobCache[name] || null,
        coverBlobUrl: isVid ? (thumbBlobCache[cleanPath + ":cover"] || null) : null,
        persona: "",
        date: mtime
      };
    });

    if (loadSeq !== _studioRuntime.creationsLoadSeq || _studioRuntime.isDestroyed) return;

    items = sortItemsList(parsedItems, sortOrder);
    isLoadingFiles = false;
    dispatch("count-change", items.length);

    await tick();
    if (loadSeq !== _studioRuntime.creationsLoadSeq || _studioRuntime.isDestroyed) return;
    initLightGallery();

    fetchMissingPrompts(effectiveFolder, activeDrive, loadSeq);
  }

  async function fetchMissingPrompts(effectiveFolder, activeDrive, loadSeq) {
    const drive = activeDrive || getTargetDrive();
    let dirty = false;
    let lastFlushTs = Date.now();
    const maybeFlush = (force = false) => {
      if (!dirty) return;
      const now = Date.now();
      if (force || now - lastFlushTs >= 500) {
        items = [...items];
        dirty = false;
        lastFlushTs = now;
      }
    };

    for (const item of items) {
      if (loadSeq !== _studioRuntime.creationsLoadSeq || _studioRuntime.isDestroyed) return;
      if (item.prompt && item.description && item.title !== item.name.replace(/\.[^.]+$/, "")) continue;

      if (!item.prompt && chatStore && typeof chatStore.getPromptForFile === "function") {
        try {
          const cachedPrompt = await chatStore.getPromptForFile(item.name).catch(() => "");
          if (cachedPrompt) {
            item.prompt = cachedPrompt;
            setBoundedPromptCache(item.name, cachedPrompt);
            dirty = true;
            maybeFlush(false);
            if (item.description && item.title !== item.name.replace(/\.[^.]+$/, "")) continue;
          }
        } catch (_) {}
      }

      const readmePath = item.path.replace(/\.[^/.]+$/, "") + ".Readme.md";
      const baseName = item.name.replace(/\.[^/.]+$/, "") + ".Readme.md";
      try {
        let txt = "";
        // 1. Читаем из целевого P2P Drive через readDriveText
        if (drive && (drive.isConnected || drive.status === "connected")) {
          txt = await readDriveText(drive, readmePath).catch(() => null);
        }
        // 2. Если на P2P drive нет, запрашиваем через httpDrive (с ноды)
        if (!txt && httpDrive && typeof httpDrive.fetch === "function") {
          const resp = await httpDrive.fetch(readmePath).catch(() => null);
          if (resp && resp.ok) {
            txt = await resp.text().catch(() => "");
          }
        }
        // 3. Fallback: прямой запрос к /generated/{baseName}
        if (!txt && typeof window !== "undefined") {
          const resp = await window.fetch(`/generated/${baseName}`).catch(() => null);
          if (resp && resp.ok) {
            txt = await resp.text().catch(() => "");
          }
        }
        if (loadSeq !== _studioRuntime.creationsLoadSeq || _studioRuntime.isDestroyed) return;
        if (txt) {
          const titleMatch = txt.match(/^#\s*([^\n]+)/);
          if (titleMatch) {
            item.title = titleMatch[1].trim();
          }
          const promptMatch = txt.match(/<!--\s*prompt:\s*([\s\S]*?)-->/i);
          if (promptMatch) {
            item.prompt = promptMatch[1].trim();
          }
          const descText = txt.replace(/<!--\s*prompt:[\s\S]*?-->/gi, "").replace(/^#\s*[^\n]+\n+/, "").trim();
          item.description = descText;
          if (!item.prompt) {
            item.prompt = descText;
          }
          if (item.prompt) {
            setBoundedPromptCache(item.name, item.prompt);
          }
          dirty = true;
          maybeFlush(false);
        }
      } catch (_) {}
    }
    if (loadSeq === _studioRuntime.creationsLoadSeq && !_studioRuntime.isDestroyed) {
      maybeFlush(true);
    }
  }

  function deleteCreation(item, e) {
    if (e) {
      e.preventDefault();
      e.stopPropagation();
    }
    deleteConfirmTarget = {
      type: "image",
      item: item,
      title: item?.isVideo ? $_("delete_video", {}, "Удалить видео") : $_("delete_creation", {}, "Удалить изображение"),
      name: item.title || item.name || (item?.isVideo ? $_("krea_video", {}, "Видео") : $_("krea_image", {}, "Изображение")),
      subtext: $_("delete_creation_subtext", {}, "Файл будет безвозвратно удалён из подключенного хранилища.")
    };
  }

  async function executeDeleteCreation(item) {
    try {
      const fullPath = item.path;
      const readmePath = fullPath.replace(/\.[^/.]+$/, "") + ".Readme.md";
      const coverPath = item.isVideo ? fullPath.replace(/\.[^/.]+$/, "") + ".cover.jpg" : "";
      const drive = getTargetDrive();
      if (drive && typeof drive.removeFile === "function") {
        await drive.removeFile(fullPath, true).catch(() => {});
        await drive.removeFile(readmePath, true).catch(() => {});
        if (coverPath) await drive.removeFile(coverPath, true).catch(() => {});
      } else if (httpDrive && typeof httpDrive.fetch === "function") {
        await httpDrive.fetch(fullPath, { method: "DELETE" }).catch(() => {});
        await httpDrive.fetch(readmePath, { method: "DELETE" }).catch(() => {});
        if (coverPath) await httpDrive.fetch(coverPath, { method: "DELETE" }).catch(() => {});
      }
      if (hoveredVideoId === item.id) {
        hoveredVideoId = null;
        playingVideoId = null;
      }
      items = items.filter((x) => x.id !== item.id);
      dispatch("count-change", items.length);
      const key = item.path || item.name;
      if (imageBlobCache[key]) {
        try { URL.revokeObjectURL(imageBlobCache[key]); } catch (_) {}
        delete imageBlobCache[key];
      }
      if (thumbBlobCache[key]) {
        try { URL.revokeObjectURL(thumbBlobCache[key]); } catch (_) {}
        delete thumbBlobCache[key];
      }
      if (thumbBlobCache[key + ":cover"]) {
        try { URL.revokeObjectURL(thumbBlobCache[key + ":cover"]); } catch (_) {}
        delete thumbBlobCache[key + ":cover"];
      }
    } catch (err) {
      console.warn("[KreaStudio] Failed to delete creation:", err);
    }
  }

  async function executeConfirmDelete() {
    if (!deleteConfirmTarget || isDeletingItem) return;
    const target = deleteConfirmTarget;
    deleteConfirmTarget = null;
    isDeletingItem = true;
    try {
      if (target.type === "persona") {
        await executeDeletePersona(target.item);
      } else if (target.type === "image") {
        await executeDeleteCreation(target.item);
      }
    } catch (err) {
      console.error("[KreaStudio] Deletion error:", err);
    } finally {
      isDeletingItem = false;
    }
  }

  function getDevSegment() {
    const dev = resolveDevice();
    let seg = (dev?.prefix || dev?.linkId || targetLinkId || targetDevice?.linkId || dev?.name || targetDevice?.name || "").trim();
    if (!seg) {
      const first = deviceRegistry.list().find((d) => !d.isAiNode && d.drive && d.drive.isConnected) || deviceRegistry.list().find((d) => !d.isAiNode);
      if (first) {
        seg = first.prefix || first.linkId || "";
      }
    }
    seg = seg.replace(/^\/+|\/+$/g, "");
    return seg || "local";
  }

  $: activeDevSegment = (deviceDrive, targetLinkId, targetDevice, defaultStorage, getDevSegment());

  function getThumbnailUrl(item) {
    if (!item) return "";
    if (item.thumbBlobUrl) return item.thumbBlobUrl;
    if (thumbBlobCache[item.path]) return thumbBlobCache[item.path];
    if (thumbBlobCache[item.name]) return thumbBlobCache[item.name];
    if (item.blobUrl) return item.blobUrl;
    if (imageBlobCache[item.path]) return imageBlobCache[item.path];
    if (imageBlobCache[item.name]) return imageBlobCache[item.name];
    const dev = activeDevSegment || getDevSegment();
    const cleanPath = (item.path || "").startsWith("/") ? item.path : `/${item.path || ""}`;
    return `/p2p/${dev}${encodeURI(cleanPath)}?resize=true&width=480&height=480`;
  }

  function lazyLoad(node, src) {
    let currentSrc = src || "";
    let observer = null;
    const markLoaded = () => {
      node.classList.add("thumb-loaded");
      if (node.parentElement) {
        node.parentElement.classList.remove("thumb-loading");
        node.parentElement.classList.add("thumb-ready");
      }
    };
    const markLoading = () => {
      node.classList.remove("thumb-loaded");
      if (node.parentElement) {
        node.parentElement.classList.remove("thumb-ready");
        node.parentElement.classList.add("thumb-loading");
      }
    };
    const onLoad = () => markLoaded();
    node.addEventListener("load", onLoad);

    const show = () => {
      if (currentSrc && node.getAttribute("src") !== currentSrc) {
        markLoading();
        node.src = currentSrc;
        if (node.complete && node.naturalWidth > 0) {
          markLoaded();
        }
      } else if (node.complete && node.naturalWidth > 0 && node.getAttribute("src")) {
        markLoaded();
      }
    };
    const root = null; // viewport-based lazy loading
    if (typeof IntersectionObserver === "undefined") {
      show();
      return {
        update(newSrc) {
          const next = newSrc || "";
          if (next !== currentSrc) {
            currentSrc = next;
            if (node.getAttribute("src") && currentSrc) {
              markLoading();
              node.src = currentSrc;
              if (node.complete && node.naturalWidth > 0) markLoaded();
            }
          }
        },
        destroy() {
          node.removeEventListener("load", onLoad);
        }
      };
    }
    observer = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (entry.isIntersecting) {
          show();
          try { observer.unobserve(node); } catch (_) {}
        }
      }
    }, { root, rootMargin: "400px 0px", threshold: 0.01 });
    observer.observe(node);
    return {
      update(newSrc) {
        const next = newSrc || "";
        if (next !== currentSrc) {
          currentSrc = next;
          if (node.getAttribute("src") && currentSrc) {
            markLoading();
            node.src = currentSrc;
            if (node.complete && node.naturalWidth > 0) markLoaded();
          }
        }
      },
      destroy() {
        node.removeEventListener("load", onLoad);
        try { observer && observer.disconnect(); } catch (_) {}
      }
    };
  }

  function getFullImageUrl(item) {
    if (!item) return "";
    if (item.blobUrl) return item.blobUrl;
    if (imageBlobCache[item.path]) return imageBlobCache[item.path];
    if (imageBlobCache[item.name]) return imageBlobCache[item.name];
    const dev = activeDevSegment || getDevSegment();
    const cleanPath = (item.path || "").startsWith("/") ? item.path : `/${item.path || ""}`;
    return `/p2p/${dev}${encodeURI(cleanPath)}`;
  }

  function getVideoPosterUrl(item) {
    if (!item) return "";
    const key = (item.path || item.name || "") + ":cover";
    if (item.coverBlobUrl) return item.coverBlobUrl;
    if (thumbBlobCache[key]) return thumbBlobCache[key];
    if (item.thumbBlobUrl) return item.thumbBlobUrl;
    const dev = activeDevSegment || getDevSegment();
    const cleanPath = (item.path || "").startsWith("/") ? item.path : `/${item.path || ""}`;
    const coverPath = cleanPath.replace(/\.[^/.]+$/, "") + ".cover.jpg";
    return `/p2p/${dev}${encodeURI(coverPath)}?resize=true&width=480&height=480`;
  }

  function getViewImageUrl(item) {
    if (!item) return "";
    if (item.blobUrl) return item.blobUrl;
    if (imageBlobCache[item.path]) return imageBlobCache[item.path];
    if (imageBlobCache[item.name]) return imageBlobCache[item.name];
    const dev = activeDevSegment || getDevSegment();
    const cleanPath = (item.path || "").startsWith("/") ? item.path : `/${item.path || ""}`;
    if (item.isVideo) return `/p2p/${dev}${encodeURI(cleanPath)}`;
    const vs = getViewerSettings();
    const base = `/p2p/${dev}${encodeURI(cleanPath)}`;
    return vs.downscaleImages ? `${base}?resize=true&width=${vs.scaleTo}&height=${vs.scaleTo}` : base;
  }

  async function fetchImageBlobViaWebRTC(filePath, query = "") {
    const drive = getTargetDrive();
    if (!drive || !drive.isConnected) return null;
    try {
      if (typeof drive.sendRPC === "function" && typeof drive.getFileContent === "function") {
        const openRes = await drive.sendRPC("Open", { path: filePath, query });
        if (openRes && openRes.status === "ok" && openRes.result) {
          const totalSize = openRes.result.size || 0;
          const lowerFp = String(filePath || "").toLowerCase();
          const fallbackMime = lowerFp.endsWith(".mp4")
            ? "video/mp4"
            : (lowerFp.endsWith(".webm")
                ? "video/webm"
                : (lowerFp.endsWith(".mov")
                    ? "video/quicktime"
                    : (lowerFp.endsWith(".jpg") || lowerFp.endsWith(".jpeg") ? "image/jpeg" : "image/png")));
          const mimeType = openRes.result.mimeType || fallbackMime;
          const rawBlob = await drive.getFileContent(filePath, totalSize, null, 0, query);
          if (rawBlob && rawBlob.size > 0) {
            const typedBlob = new Blob([rawBlob], { type: mimeType });
            return URL.createObjectURL(typedBlob);
          }
        }
      }
    } catch (e) {
      console.warn("[KreaStudio] WebRTC fetch blob failed:", filePath, e);
    }
    return null;
  }

  async function fetchLargeImageBlob(fetchFn, url, defaultMime = "") {
    const CHUNK_SIZE = 128 * 1024; // 128 KB safe chunk size for WebRTC SCTP
    let offset = 0;
    let totalLength = null;
    let detectedMime = defaultMime;
    const chunks = [];

    while (totalLength === null || offset < totalLength) {
      const end = totalLength !== null ? Math.min(offset + CHUNK_SIZE - 1, totalLength - 1) : offset + CHUNK_SIZE - 1;
      const resp = await fetchFn(url, {
        headers: {
          Range: `bytes=${offset}-${end}`
        }
      });
      if (!resp.ok && resp.status !== 206) {
        if (offset === 0) {
          return await resp.blob();
        }
        throw new Error(`Range fetch failed: HTTP ${resp.status}`);
      }

      if (!detectedMime) {
        const ct = (resp.headers.get("Content-Type") || resp.headers.get("content-type") || "").split(";")[0].trim();
        if (ct && ct !== "application/octet-stream") detectedMime = ct;
      }

      const cr = resp.headers.get("Content-Range") || resp.headers.get("content-range");
      if (cr) {
        const match = /\/(\d+)$/.exec(cr);
        if (match) totalLength = parseInt(match[1], 10);
      }
      const blob = await resp.blob();
      chunks.push(blob);
      offset += blob.size;
      if (blob.size === 0 || (totalLength !== null && offset >= totalLength)) {
        break;
      }
    }

    const lowerUrl = String(url || "").toLowerCase();
    const finalMime = detectedMime || (lowerUrl.includes(".mp4") ? "video/mp4" : (lowerUrl.includes(".jpg") || lowerUrl.includes(".jpeg") ? "image/jpeg" : "image/png"));
    return new Blob(chunks, { type: finalMime });
  }

  async function handleImageError(item, e) {
    if (!item || item._fallbackTried) {
      e?.target?.parentElement?.classList.remove("thumb-loading");
      return;
    }
    if (!e || !e.target || !e.target.src || e.target.src === window.location.href) return;
    item._fallbackTried = true;

    // 1. Try reading thumbnail via WebRTC getFileContent (keep separate from full-res blobUrl!)
    const webrtcThumbUrl = await fetchImageBlobViaWebRTC(item.path, "resize=true&width=480&height=480");
    if (webrtcThumbUrl) {
      item.thumbBlobUrl = webrtcThumbUrl;
      thumbBlobCache[item.path] = webrtcThumbUrl;
      thumbBlobCache[item.name] = webrtcThumbUrl;
      if (e && e.target) e.target.src = webrtcThumbUrl;
      return;
    }

    // 1b. Full-res WebRTC fallback if thumbnail query wasn't supported
    const webrtcFullUrl = await fetchImageBlobViaWebRTC(item.path, "");
    if (webrtcFullUrl) {
      item.blobUrl = webrtcFullUrl;
      imageBlobCache[item.path] = webrtcFullUrl;
      imageBlobCache[item.name] = webrtcFullUrl;
      if (e && e.target) e.target.src = webrtcFullUrl;
      return;
    }

    // 2. Try reading via drive homeRead
    const drive = getTargetDrive();
    if (drive) {
      try {
        if (typeof drive.homeRead === "function") {
          const b64 = await drive.homeRead(item.path);
          if (b64) {
            item.blobUrl = `data:image/png;base64,${b64}`;
            imageBlobCache[item.path] = item.blobUrl;
            imageBlobCache[item.name] = item.blobUrl;
            if (e && e.target) e.target.src = item.blobUrl;
            return;
          }
        }
      } catch (_) {}
    }

    // 3. Try fetching from ephemeral /chat/image/{filename} via safe chunks
    try {
      const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
        ? (p, opts) => httpDrive.fetch(p, opts)
        : (p, opts) => fetch(p, opts);
      const blob = await fetchLargeImageBlob(fetchFn, `/chat/image/${item.name}`);
      if (blob && blob.size > 0) {
        item.blobUrl = URL.createObjectURL(blob);
        imageBlobCache[item.path] = item.blobUrl;
        imageBlobCache[item.name] = item.blobUrl;
        if (e && e.target) e.target.src = item.blobUrl;
        return;
      }
    } catch (_) {}

    // All fallbacks failed — stop spinner so broken/missing cards stay idle at 0% CPU
    e?.target?.parentElement?.classList.remove("thumb-loading");
  }

  async function handleVideoPosterError(item, e) {
    if (!item || item._posterFallbackTried) {
      e?.target?.parentElement?.classList.remove("thumb-loading");
      return;
    }
    if (!e || !e.target || !e.target.src || e.target.src === window.location.href) return;
    item._posterFallbackTried = true;

    const coverPath = (item.path || "").replace(/\.[^/.]+$/, "") + ".cover.jpg";
    const coverName = (item.name || "").replace(/\.[^/.]+$/, "") + ".cover.jpg";
    const coverKey = (item.path || item.name || "") + ":cover";
    const applyPoster = (url) => {
      item.coverBlobUrl = url;
      thumbBlobCache[coverKey] = url;
      if (e && e.target) e.target.src = url;
    };

    // 1. Try resized .cover.jpg via WebRTC getFileContent, then full .cover.jpg
    const webrtcCoverThumb = await fetchImageBlobViaWebRTC(coverPath, "resize=true&width=480&height=480");
    if (webrtcCoverThumb) {
      applyPoster(webrtcCoverThumb);
      return;
    }
    const webrtcCoverFull = await fetchImageBlobViaWebRTC(coverPath, "");
    if (webrtcCoverFull) {
      applyPoster(webrtcCoverFull);
      return;
    }

    // 2. Fallback to OnMyDisk's native P2P video frame extractor (?poster) for external .mp4 files
    const webrtcNativePoster = await fetchImageBlobViaWebRTC(item.path, "poster");
    if (webrtcNativePoster) {
      applyPoster(webrtcNativePoster);
      return;
    }

    // 3. Try reading .cover.jpg via drive.homeRead
    const drive = getTargetDrive();
    if (drive && typeof drive.homeRead === "function") {
      try {
        const b64 = await drive.homeRead(coverPath);
        if (b64) {
          applyPoster(`data:image/jpeg;base64,${b64}`);
          return;
        }
      } catch (_) {}
    }

    // 4. Ephemeral /chat/image/{coverName} fallback
    try {
      const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
        ? (p, opts) => httpDrive.fetch(p, opts)
        : (p, opts) => fetch(p, opts);
      const blob = await fetchLargeImageBlob(fetchFn, `/chat/image/${coverName}`, "image/jpeg");
      if (blob && blob.size > 0 && String(blob.type || "").startsWith("image/")) {
        applyPoster(URL.createObjectURL(blob));
        return;
      }
    } catch (_) {}

    e?.target?.parentElement?.classList.remove("thumb-loading");
  }

  async function handleVideoPreviewError(item, e) {
    if (!item || item._videoPreviewFallbackTried) return;
    item._videoPreviewFallbackTried = true;
    const webrtcVideoUrl = await fetchImageBlobViaWebRTC(item.path, "");
    if (webrtcVideoUrl) {
      item.blobUrl = webrtcVideoUrl;
      imageBlobCache[item.path] = webrtcVideoUrl;
      imageBlobCache[item.name] = webrtcVideoUrl;
      if (e && e.target && hoveredVideoId === item.id) {
        e.target.src = webrtcVideoUrl;
        e.target.play().catch(() => {});
      }
    }
  }

  function buildDevicePush() {
    try {
      const storage = (defaultStorage || "").trim();
      const parsed = parseStoragePath(storage);
      let targetDev = parsed.device || resolveDevice() || targetDevice || null;
      if ((!targetDev || targetDev.local) && parsed.deviceName && parsed.deviceName.toLowerCase() !== "local") {
        const found = deviceRegistry.getByName(parsed.deviceName) ||
                      deviceRegistry.getByPrefix(parsed.deviceName) ||
                      deviceRegistry.get(parsed.deviceName);
        if (found) targetDev = found;
      }

      const aiNodeId = targetLinkId || (targetDevice && targetDevice.isAiNode ? targetDevice.linkId : "") || httpDrive?.targetLinkId || "";
      const server = getClientDeviceServer();
      let linkId = targetDev?.linkId || server?.linkId || getOrCreateLocalLinkId();
      if (linkId === parsed.deviceName || linkId === "local" || linkId === "This Device") {
        linkId = server?.linkId || getOrCreateLocalLinkId();
      }
      if (!linkId || (aiNodeId && linkId.toLowerCase() === aiNodeId.toLowerCase())) {
        return null;
      }
      const folder = getTargetFolder().replace(/^\/+/, "");
      if (!folder) return null;
      const grant = issueConsent(folder, { writable: true, openWindow: false });
      if (!grant?.token) return null;
      const didPub = (typeof window !== "undefined" && (window.identity?.mainPub || window.identity?.pair?.pub)) || "";
      return { linkId, consentToken: grant.token, folder, didPub };
    } catch (e) {
      console.warn("[KreaStudio] buildDevicePush error:", e);
      return null;
    }
  }

  const CIVITAI_URL_REGEX = /https?:\/\/(?:www\.)?civitai\.(?:com|red)\/(?:images|posts)\/\d+/i;

  function applyImportedSettings(applied) {
    if (!applied || typeof applied !== "object") return;
    if (applied.base_model) {
      const normModel = isSdxlModel(applied.base_model) ? normalizeSdxlModel(applied.base_model) : applied.base_model;
      if (isSdxlModel(normModel)) lastSdxlModel = normModel;
      else lastDitModel = normModel;
      _studioRuntime.prevBaseModel = normModel;
      sliders.base_model = normModel;
    }
    if (Array.isArray(applied.styles)) {
      sliders.styles = applied.styles.filter((x) => x && x !== "none" && !SDXL_BASE_MODELS.includes(x));
      sliders.style = sliders.styles[0] || "none";
      sliders.style_weights = (applied.style_weights && typeof applied.style_weights === "object")
        ? { ...applied.style_weights }
        : {};
      if (sliders.styles.length > 0 && sliders.style_weights[sliders.style] !== undefined) {
        sliders.style_strength = sliders.style_weights[sliders.style];
      }
    }
    if (Array.isArray(applied.archetypes)) {
      sliders.archetypes = applied.archetypes.filter(Boolean);
      sliders.archetype = sliders.archetypes[0] || null;
      sliders.archetype_weights = (applied.archetype_weights && typeof applied.archetype_weights === "object")
        ? { ...applied.archetype_weights }
        : {};
      if (sliders.archetypes.length > 0 && sliders.archetype_weights[sliders.archetype] !== undefined) {
        sliders.archetype_strength = sliders.archetype_weights[sliders.archetype];
      }
    }
    if (typeof applied.steps === "number") {
      sliders.steps = isSdxlModel(sliders.base_model) ? 6 : applied.steps;
    }
    if (typeof applied.cfg === "number") {
      sliders.cfg = isSdxlModel(sliders.base_model) ? 1.0 : applied.cfg;
    }
    if (applied.aspect) {
      sliders.aspect = applied.aspect;
    }
    sliders = { ...sliders };
    saveLocalSliders(sliders);
  }

  async function runGeneration(overrideOpts = null) {
    const isOverride = overrideOpts && typeof overrideOpts === "object" && !(overrideOpts instanceof Event);
    const isVideoGen = isVideoModel(sliders.base_model);
    const isExtendingVideo = isVideoGen && Boolean(extendingVideoItem && extendingVideoItem.videoSource);
    let cleanPrompt = isOverride && typeof overrideOpts.prompt === "string"
      ? overrideOpts.prompt.trim()
      : promptText.trim();
    if (!cleanPrompt && isVideoGen && !isExtendingVideo && attachedReferences.length > 0) {
      cleanPrompt = attachedReferences.length >= 2 && videoRefMode === "keyframe"
        ? "<Picture 1> smoothly transitions into <Picture 2> with natural cinematic movement, fluid camera motion, and realistic lighting.\nAudio: Natural ambient environmental atmosphere and subtle realistic foley in crisp stereo."
        : "<Picture 1> comes alive with natural subtle movement, gentle breathing, realistic camera micro-motion, and cinematic lighting.\nAudio: Natural ambient room tone and subtle realistic environmental foley in crisp stereo.";
    }
    if (!cleanPrompt && selectedPersonas.length === 0 && !isExtendingVideo) return null;
    if (isGenerating) return null;

    generationError = "";
    const cloudPermitted = isVideoGen ? canShowVideoTab : canShowStudioTab;
    if (!isSdxlModel(sliders.base_model) && (!kreaSupported || (statusLoadedOnce && !cloudPermitted))) {
      if (sdxlSupported !== false) {
        setBaseModel(lastSdxlModel || "sdxl_realistic", true);
      }
      return null;
    }

    // Auto-extract if user pasted a CivitAI image/post URL directly into the main composer
    if (!isOverride && canShowDirector && CIVITAI_URL_REGEX.test(cleanPrompt)) {
      isDirectorThinking = true;
      try {
        const fetchFnPre = getKreaFetch();
        const mcpPayloadPre = getMcpPayload();
        const preRes = await fetchFnPre("/krea/director/prompt", {
          method: "POST",
          headers: buildAuthHeaders(),
          body: JSON.stringify({
            omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
            instruction: cleanPrompt,
            current_prompt: "",
            base_model: sliders.base_model || "realism",
            persona_name: selectedPersonas.map((p) => p.name).join(", "),
            persona_prompt: selectedPersonas.map((p) => p.prompt || p.passport || "").join("; "),
            warmup_gpu: !isSdxlModel(sliders.base_model),
            mcp_keys: mcpPayloadPre,
            settings: { platform: getClientPlatform(), language: $locale || "ru", mcp_keys: mcpPayloadPre }
          })
        });
        if (!preRes || !preRes.ok) {
          let errMsg = preRes ? `HTTP ${preRes.status}` : $_("krea_err_no_response", {}, "No response from server");
          if (preRes) {
            try {
              const ed = await preRes.json();
              errMsg = ed.detail || ed.error || errMsg;
            } catch (_) {}
          }
          generationError = sanitizeExternalCheckoutText(errMsg);
          return null;
        }
        const preData = await preRes.json();
        if (preData && preData.applied_settings) {
          applyImportedSettings(preData.applied_settings);
        }
        if (preData && preData.prompt) {
          pushPromptUndo(promptText);
          promptText = preData.prompt.trim();
          cleanPrompt = promptText;
          await tick();
          resizeTextarea();
        }
      } catch (preErr) {
        generationError = sanitizeExternalCheckoutText(preErr.message || String(preErr));
        return null;
      } finally {
        isDirectorThinking = false;
      }
    }

    const targetSubfolder = isOverride && overrideOpts.subfolder ? String(overrideOpts.subfolder).trim() : "";
    const targetPrefix = isOverride && overrideOpts.filenamePrefix ? String(overrideOpts.filenamePrefix).trim() : "";
    const customTitle = isOverride && overrideOpts.customTitle ? String(overrideOpts.customTitle).trim() : "";
    const customDescription = isOverride && overrideOpts.customDescription ? String(overrideOpts.customDescription).trim() : "";
    const chosenAspect = (isOverride && overrideOpts.aspect)
      ? overrideOpts.aspect
      : (isVideoGen ? videoAspect : sliders.aspect);

    // Save prompt to history
    if (cleanPrompt) {
      if (chatStore && typeof chatStore.savePrompt === "function") {
        chatStore.savePrompt(cleanPrompt, sliders.style || "krea").catch(() => {});
      }
      const newEntry = {
        id: "p_" + Date.now(),
        text: cleanPrompt,
        style: sliders.style || "krea",
        lastUsed: new Date().toISOString()
      };
      recentPrompts = [newEntry, ...recentPrompts.filter((x) => (x.text || x.prompt || x) !== cleanPrompt)].slice(0, 50);
      try {
        localStorage.setItem("krea_recent_prompts", JSON.stringify(recentPrompts));
      } catch (_) {}
    }

    isGenerating = true;
    generatingPrompt = cleanPrompt || selectedPersonas.map((p) => p.name).join(", ");
    generatingPersona = selectedPersonas.map((p) => p.name).join(", ");
    
    const isLocalSdxlGen = isSdxlModel(sliders.base_model);
    const now = Date.now();
    const warm = gpuWarmUntil > now;
    isGpuWarm = warm;
    // Local SDXL has no cloud GPU wakeup — start directly in "generating"
    generatingStage = isLocalSdxlGen ? "generating" : "warming";
    warmingPhase = isLocalSdxlGen ? "loading_models" : (warm ? "loading_models" : "booting");
    generatingStep = 0;
    generatingTotalSteps = 0;

    activeTab = "gallery";
    generationElapsed = 0;
    const t0 = Date.now();
    generationTimer = setInterval(() => {
      generationElapsed = ((Date.now() - t0) / 1000).toFixed(1);
    }, 100);

    const [wStr, hStr] = String(chosenAspect || "896x1152").split("x");
    const width = parseInt(wStr, 10) || 896;
    const height = parseInt(hStr, 10) || 1152;

    // Собираем референсные изображения (вручную прикрепленные или Face Lock персоны, если у персоны включён use_face_ref)
    const resolvedRefImages = [];
    if (attachedReferences.length > 0) {
      for (const r of attachedReferences) {
        const d = r.dataUrl || r.preview;
        if (d && typeof d === "string") resolvedRefImages.push(d);
      }
    } else if (selectedPersonas.length > 0) {
      for (const sp of selectedPersonas) {
        if (sp.render_preset && sp.render_preset.enabled !== false && sp.render_preset.use_face_ref === true) {
          const avRef = sp.avatarData || (typeof sp.avatar === "string" && sp.avatar.startsWith("data:") ? sp.avatar : "");
          if (avRef && resolvedRefImages.length < 2) {
            resolvedRefImages.push(avRef);
          }
        }
      }
    }

    const targetDev = resolveDevice();
    const isAiNodeTarget = Boolean(targetDev && targetDev.isAiNode);
    const baseStorage = getBaseStorage();
    const fetchFn = getKreaFetch();

    const effectiveSteps = isVideoGen
      ? (videoQuality === "fast" ? 4 : 8)
      : (sliders.steps || (sliders.base_model === "anima" ? 25 : 12));
    const effectiveCfg = isVideoGen
      ? 1.0
      : (sliders.cfg || (sliders.base_model === "realism" ? 1.5 : (sliders.base_model === "semi_realism" ? 2.0 : 3.0)));
    const endpointPath = isVideoGen ? "/generate/krea/video" : "/generate/krea2";
    const reqTimeoutMs = isVideoGen ? 540000 : 180000;

    const payload = {
      omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
      prompt: cleanPrompt,
      persona_id: selectedPersonas.length > 0 ? selectedPersonas.map((p) => p.id).join(",") : null,
      persona_name: selectedPersonas.length > 0 ? selectedPersonas.map((p) => p.name).join(", ") : null,
      persona_prompt: selectedPersonas.length > 0 ? selectedPersonas.map((p) => p.prompt || p.passport || "").join("; ") : "",
      reference_images: resolvedRefImages,
      reference_image: resolvedRefImages[0] || null,
      sliders: {
        base_model: sliders.base_model || "realism",
        detail: sliders.detail,
        styles: Array.isArray(sliders.styles) ? sliders.styles.filter((x) => x && x !== "none") : [],
        style: (sliders.styles && sliders.styles.length > 0) ? sliders.styles[0] : "none",
        style_weights: sliders.style_weights || {},
        style_strength: sliders.style_strength,
        archetypes: Array.isArray(sliders.archetypes) ? sliders.archetypes.filter(Boolean) : [],
        archetype: (sliders.archetypes && sliders.archetypes.length > 0) ? sliders.archetypes[0] : null,
        archetype_weights: sliders.archetype_weights || {},
        archetype_strength: sliders.archetype_strength,
        steps: effectiveSteps,
        cfg: effectiveCfg,
        ...(isVideoGen
          ? {
              video_quality: videoQuality,
              video_duration: videoDuration,
              video_ref_mode: videoRefMode,
              video_aspect: chosenAspect
            }
          : {}),
        ...(isLocalSdxlGen && resolvedRefImages.length > 0
          ? {
              sdxl_ref_mode: sdxlRefMode || "faceswap",
              sdxl_face_target: sdxlFaceTarget || "auto",
              sdxl_inpaint_target: sdxlInpaintTarget || "outfit",
              ipadapter_weight: typeof sliders.ipadapter_weight === "number" ? sliders.ipadapter_weight : 0.65,
              face_restore_visibility: typeof sliders.face_restore_visibility === "number" ? sliders.face_restore_visibility : 0.30,
              inpaint_denoise: typeof sliders.inpaint_denoise === "number" ? sliders.inpaint_denoise : 0.94
            }
          : {})
      },
      width,
      height,
      steps: effectiveSteps,
      cfg: effectiveCfg,
      ...(isVideoGen
        ? {
            duration: videoDuration,
            quality: videoQuality,
            fps: 24,
            video_ref_mode: attachedReferences.length > 0 ? videoRefMode : "auto",
            extend_video: isExtendingVideo ? extendingVideoItem.videoSource : null
          }
        : {}),
      subfolder: targetSubfolder || null,
      filename_prefix: targetPrefix || null,
      custom_title: customTitle || null,
      custom_description: customDescription || null,
      stream: true,
      client: "svar",
      image_delivery: "inline",
      save_local: true,
      mcp_keys: getMcpPayload(),
      settings: {
        defaultStorage: baseStorage,
        client: "svar",
        image_delivery: "inline",
        mcp_keys: getMcpPayload()
      }
    };

    const abortCtrl = new AbortController();
    const timeoutHandle = setTimeout(() => {
      try { abortCtrl.abort(); } catch (_) {}
    }, reqTimeoutMs);

    try {
      let res = null;
      const requestHeaders = {
        "Content-Type": "application/json",
        "X-Client": "svar"
      };
      const activeToken = omdKey || (typeof window !== "undefined" && (window.__omdKey || window.identity?.token)) || "";
      if (activeToken) {
        requestHeaders["Token"] = activeToken;
        requestHeaders["X-OMD-Token"] = activeToken;
      }
      const effectiveDid = (httpDrive && httpDrive.didPub) || (typeof window !== "undefined" && (window.identity?.mainPub || window.identity?.pair?.pub)) || "";
      if (effectiveDid) {
        requestHeaders["X-OMD-Caller-DID"] = effectiveDid;
      }
      if (httpDrive && httpDrive.isPrivateMode !== undefined) {
        requestHeaders["X-OMD-Private-Mode"] = httpDrive.isPrivateMode ? "1" : "0";
      } else {
        requestHeaders["X-OMD-Private-Mode"] = "1";
      }

      // 1. Via P2P tunnel to node (httpDrive)
      if (httpDrive && typeof httpDrive.fetch === "function") {
        try {
          res = await httpDrive.fetch(endpointPath, {
            method: "POST",
            headers: requestHeaders,
            body: JSON.stringify(payload),
            signal: abortCtrl.signal,
            timeoutMs: reqTimeoutMs
          });
        } catch (driveErr) {
          throw new Error($_("krea_err_p2p_node", { err: driveErr.message || driveErr }, `P2P node connection error: ${driveErr.message || driveErr}. Please reload the page (F5).`));
        }
      } else {
        // 2. Direct HTTP request (fallback when running without P2P drive)
        res = await fetch(endpointPath, {
          method: "POST",
          headers: requestHeaders,
          body: JSON.stringify(payload),
          signal: abortCtrl.signal
        });
      }

      if (!res || !res.ok) {
        let errDetail = res ? `HTTP ${res.status}` : $_("krea_err_no_response", {}, "No response from server");
        if (res) {
          try {
            const errData = await res.json();
            if (errData && errData.detail) errDetail = errData.detail;
            else if (errData && errData.error) errDetail = errData.error;
          } catch (_) {}
        }
        throw new Error(errDetail);
      }

      const ctype = (res.headers && res.headers.get && (res.headers.get("content-type") || res.headers.get("Content-Type"))) || "";
      let data = null;

      if (ctype.includes("ndjson") && res.body && typeof res.body.getReader === "function") {
        const reader = res.body.getReader();
        const decoder = new TextDecoder("utf-8");
        let streamBuf = "";

        const handleStreamLine = (rawLine) => {
          const trimmed = rawLine.trim();
          if (!trimmed) return;
          let evt = null;
          try {
            evt = JSON.parse(trimmed);
          } catch (_) {
            return;
          }
          if (!evt || typeof evt !== "object") return;
          if (evt.type === "progress") {
            if (evt.stage) {
              const nextStage = (isLocalSdxlGen && evt.stage === "warming") ? "generating" : evt.stage;
              const stageRank = { warming: 0, generating: 1, saving: 2 };
              if ((stageRank[nextStage] ?? 0) >= (stageRank[generatingStage] ?? 0)) {
                generatingStage = nextStage;
              }
            }
            if (evt.phase) warmingPhase = evt.phase;
            if (typeof evt.step === "number") generatingStep = evt.step;
            if (typeof evt.total_steps === "number" && evt.total_steps > 0) generatingTotalSteps = evt.total_steps;
          } else if (evt.type === "result" && evt.data) {
            data = evt.data;
          } else if (evt.type === "error") {
            throw new Error(evt.detail || $_("krea_err_generation", {}, "Generation failed"));
          }
        };

        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          streamBuf += decoder.decode(value, { stream: true });
          let nlIdx;
          while ((nlIdx = streamBuf.indexOf("\n")) !== -1) {
            const line = streamBuf.slice(0, nlIdx);
            streamBuf = streamBuf.slice(nlIdx + 1);
            handleStreamLine(line);
          }
        }
        streamBuf += decoder.decode();
        if (streamBuf.trim()) {
          handleStreamLine(streamBuf);
        }
        if (!data) {
          throw new Error($_("krea_err_empty_stream", {}, "Generation stream ended without a result"));
        }
      } else {
        data = await res.json();
      }

      const filename = data.video || data.image || (isVideoGen ? `H3_${Date.now()}.mp4` : `krea_${Date.now()}.png`);
      const itemIsVideo = isVideoGen || Boolean(data.is_video) || videoExtRegex.test(filename);

      // Update live quota / entitlement state from server response
      if (data.tier) studioTier = data.tier;
      if (typeof data.krea_allowed === "boolean") kreaAllowed = data.krea_allowed;
      if (typeof data.video_allowed === "boolean") videoAllowed = data.video_allowed;
      if (typeof data.credits === "number") memberCredits = data.credits;
      if (typeof data.krea_daily_remaining === "number") kreaDailyRemaining = data.krea_daily_remaining;
      if (typeof data.krea_daily_limit === "number") kreaDailyLimit = data.krea_daily_limit;
      if (typeof data.krea_notice === "string") kreaLockNotice = data.krea_notice;

      // GPU generation completed — now downloading chunk stream and saving!
      generatingStage = "saving";
      if (!isSdxlModel(sliders.base_model)) {
        isGpuWarm = true;
        lastGenFinishedAt = Date.now();
        gpuWarmUntil = lastGenFinishedAt + 290000;
      }

      const folder = getTargetFolder();
      const effectiveSub = data.subfolder || targetSubfolder || "";
      const destFolder = effectiveSub ? `${folder}/${effectiveSub}` : folder;
      const fullImagePath = `${destFolder}/${filename}`;
      if (effectiveSub && !storyFolders.includes(effectiveSub)) {
        storyFolders = [...storyFolders, effectiveSub].sort();
      }

      let localBlobUrl = null;
      let imageBlob = null;
      if (data.url) {
        try {
          imageBlob = await fetchLargeImageBlob(fetchFn, data.url, itemIsVideo ? "video/mp4" : "image/png");
          if (imageBlob && imageBlob.size > 0) {
            localBlobUrl = URL.createObjectURL(imageBlob);
            imageBlobCache[fullImagePath] = localBlobUrl;
            imageBlobCache[filename] = localBlobUrl;
            imageBlobCache = { ...imageBlobCache };
          }
        } catch (fetchErr) {
          console.warn("[KreaStudio] Preload/save error:", fetchErr);
        }
      }

      let localCoverBlobUrl = null;
      let coverBlob = null;
      if (itemIsVideo && data.cover_url) {
        try {
          coverBlob = await fetchLargeImageBlob(fetchFn, data.cover_url, "image/jpeg");
          if (coverBlob && coverBlob.size > 0) {
            localCoverBlobUrl = URL.createObjectURL(coverBlob);
            thumbBlobCache[fullImagePath + ":cover"] = localCoverBlobUrl;
            thumbBlobCache[filename + ":cover"] = localCoverBlobUrl;
            thumbBlobCache = { ...thumbBlobCache };
          }
        } catch (coverErr) {
          console.warn("[KreaStudio] Video cover preload warning:", coverErr);
        }
      }

      if (chatStore && typeof chatStore.associateFileWithPrompt === "function") {
        chatStore.associateFileWithPrompt("", filename, cleanPrompt).catch(() => {});
      }
      setBoundedPromptCache(filename, cleanPrompt);

      const resolvedTitle = (data.title && !isJustFileName(data.title, filename, fullImagePath))
        ? data.title.trim()
        : (customTitle || "");
      const resolvedDesc = (data.description && !isJustFileName(data.description, filename, fullImagePath))
        ? data.description.trim()
        : (customDescription || "");

      const newItem = {
        id: filename,
        name: filename,
        path: fullImagePath,
        isVideo: itemIsVideo,
        duration: data.duration || (itemIsVideo ? videoDuration : null),
        subfolder: effectiveSub,
        title: resolvedTitle || resolvedDesc || cleanPrompt.slice(0, 48),
        description: resolvedDesc,
        prompt: cleanPrompt,
        blobUrl: localBlobUrl,
        coverBlobUrl: localCoverBlobUrl,
        imageBlob,
        persona: selectedPersonas.map((p) => p.name).join(", ") || "",
        date: Date.now(),
        gpuTime: data.gpu_time || null,
        totalTime: data.total_time || null
      };

      if (!activeStorySubfolder || activeStorySubfolder === effectiveSub) {
        items = sortItemsList([newItem, ...items.filter((x) => x.id !== newItem.id)], sortOrder);
        dispatch("count-change", items.length);
      }

      if (!resolvedTitle || !resolvedDesc) {
        ensureItemPromptAndTitle(newItem).then(() => {
          items = [...items];
        }).catch(() => {});
      }

      isGenerating = false;
      if (isExtendingVideo) {
        extendingVideoItem = null;
      }
      clearInterval(generationTimer);
      clearTimeout(timeoutHandle);

      activeTab = "gallery";
      await tick();
      initLightGallery();

      // Случай 2 и 3: Сохранение на целевое веб/remote устройство через фронтенд (targetDrive)
      if (!data.saved_locally && imageBlob) {
        const targetDrive = getTargetDrive();
        if (targetDrive) {
          if (!targetDrive.isConnected && typeof targetDrive.connect === "function") {
            try {
              await targetDrive.connect();
            } catch (connErr) {
              console.warn("[KreaStudio] Connect targetDrive error:", connErr);
            }
          }
          const blobToBase64 = (bl) => new Promise((resolve) => {
            const reader = new FileReader();
            reader.onloadend = () => resolve((reader.result || "").split(",")[1] || "");
            reader.onerror = () => resolve(null);
            reader.readAsDataURL(bl);
          });
          const imageBase64 = await blobToBase64(imageBlob);

          if (imageBase64) {
            try {
              await ensureFolderRecursive(targetDrive, destFolder);
              const title = data.title || customTitle || cleanPrompt.slice(0, 30) || "";
              const desc = data.description || customDescription || "";
              let readmeContent = `# ${title}\n\n${desc}\n`;
              if (cleanPrompt) {
                readmeContent += `\n<!-- prompt: ${cleanPrompt} -->\n`;
              }
              const readmeFilename = filename.replace(/\.[^/.]+$/, "") + ".Readme.md";
              const fullReadmePath = `${destFolder}/${readmeFilename}`;

              await writeDriveFile(targetDrive, fullImagePath, imageBase64);
              await writeDriveText(targetDrive, fullReadmePath, readmeContent);
              if (itemIsVideo && coverBlob) {
                const coverB64 = await blobToBase64(coverBlob);
                if (coverB64) {
                  const coverFilename = data.cover || (filename.replace(/\.[^/.]+$/, "") + ".cover.jpg");
                  await writeDriveFile(targetDrive, `${destFolder}/${coverFilename}`, coverB64).catch(() => {});
                }
              }
              console.log("[KreaStudio] Successfully saved creation and companion Readme to drive:", fullImagePath);
            } catch (driveErr) {
              console.warn("[KreaStudio] Direct save to targetDrive warning:", driveErr);
            }
          }
        }
      }
      return newItem;
    } catch (err) {
      console.error("[KreaStudio] Generation error:", err);
      const rawErrMsg = err.name === "AbortError"
        ? (isVideoGen
            ? $_("krea_err_video_timeout", {}, "Таймаут генерации видео (9 мин): сетевой туннель или GPU-воркер не ответили вовремя.")
            : $_("krea_err_timeout", {}, "Таймаут генерации (3 мин): сетевой туннель или нода не ответили вовремя."))
        : (err.message || String(err));
      generationError = sanitizeExternalCheckoutText(rawErrMsg);
      return null;
    } finally {
      clearTimeout(timeoutHandle);
      clearInterval(generationTimer);
      isGenerating = false;
    }
  }

  async function pickRandomSuggestion() {
    if (isRandomizingPrompt) return;
    isRandomizingPrompt = true;

    const fallbackGenerate = () => {
      const personaName = selectedPersonas.length > 0 ? selectedPersonas.map((p) => p.name).join(", ") : "";
      const hasChar = Boolean(personaName || (activeArchetypeObjs && activeArchetypeObjs.length > 0));
      const stylePart = (sliders.styles && sliders.styles.length > 0)
        ? `, ${sliders.styles.join(", ")} style`
        : ((sliders.style && sliders.style !== "none") ? `, ${sliders.style} style` : "");
      const lightings = [
        "bathed in warm golden hour rim lighting with long dramatic shadows",
        "illuminated by cool moody neon hues reflecting off slick wet surfaces",
        "soft diffuse morning skylight highlighting natural textures",
        "dramatic chiaroscuro lighting with deep rich blacks and vibrant warm highlights",
        "dappled sunlight filtering through lush tree canopy with subtle lens flare",
        "dim atmospheric twilight lighting creating an intimate serene warmth"
      ];
      const framings = [
        "sweeping panoramic establishing shot, monumental scale, crystal clear horizon",
        "dramatic low-angle ground perspective looking across sculpted geology toward the sky",
        "high-angle crane shot looking down across deep environmental layers",
        "cinematic wide-angle view, deep atmospheric backdrop, immersive depth"
      ];

      if (!hasChar) {
        const landscapes = [
          "Towering jagged alpine mountain peaks rising above a mirror-like turquoise glacial lake, morning mist weaving through pine forests",
          "A dramatic wind-swept coastal cliff overlooking churning ocean waves crashing against dark volcanic sea stacks",
          "Vast rolling desert sand dunes rippled by the wind, casting deep golden shadows under a twilight sky with a crescent moon",
          "An ancient mystical moss-covered redwood forest, ethereal god rays piercing through dense canopy illuminating winding streams",
          "Panoramic view of a sprawling neon-lit metropolis in twilight rain, towering glass spires reflecting amber sunset glow",
          "Majestic northern lights dancing in vibrant green and violet ribbons over a tranquil snow-covered Arctic fjord",
          "A deep sandstone slot canyon with sculpted undulating orange rock walls, warm ambient light glowing from above",
          "A tranquil secluded tropical cove with turquoise water gently lapping against powdery white sand, framed by wild lush cliffs"
        ];
        const ls = landscapes[Math.floor(Math.random() * landscapes.length)];
        const l = lightings[Math.floor(Math.random() * lightings.length)];
        const f = framings[Math.floor(Math.random() * framings.length)];
        return `${ls}, ${l}, ${f}${stylePart}.`;
      }

      const lead = personaName ? `${personaName}, ` : "";
      const actions = [
        "running swiftly across rain-slicked neon cobblestones, trench coat billowing in the wind",
        "stepping purposefully down a monumental concrete spiral staircase in a brutalist art gallery",
        "kneeling on one knee to inspect intricate antique clockwork gears on a subterranean vault floor",
        "leaning against the fender of a vintage roadster parked on an open coastal cliff overlooking crashing waves",
        "walking full-stride through an overgrown sunlit botanical atrium surrounded by towering palms and ferns",
        "working intently at a cluttered wooden workbench, soldering glowing vacuum tubes in a dimly lit workshop",
        "leaping gracefully across an industrial rooftop gap against a panoramic twilight skyline",
        "sitting cross-legged on a patterned kilim rug on an open desert terrace examining ancient star charts",
        "perched casually on the stone balustrade of a bridge spanning a misty river in early morning light",
        "hiking with dynamic energy along a wind-swept rocky mountain ridge as low clouds drift through the peaks",
        "striding purposefully through a crowded neon-lit market alley under heavy rain with a transparent umbrella",
        "skating fluidly down an oceanfront boulevard between swaying palm trees in warm afternoon light"
      ];
      const a = actions[Math.floor(Math.random() * actions.length)];
      const l = lightings[Math.floor(Math.random() * lightings.length)];
      const f = framings[Math.floor(Math.random() * framings.length)];
      return `${lead}${a}, ${l}, ${f}${stylePart}.`;
    };

    try {
      // Only pass promptText as keywords if it is a short user-typed topic (<= 4 words, no sentence punctuation)
      // Never pass an already generated full prompt as keywords!
      let userKeywords = "";
      const trimmed = (promptText || "").trim();
      const words = trimmed.split(/\s+/).filter(Boolean);
      if (words.length > 0 && words.length <= 4 && !/[.!?\n]/.test(trimmed)) {
        userKeywords = trimmed;
      }

      const archetypesStr = (activeArchetypeObjs || []).map(a => `${a.label} (${a.desc})`).join("; ");
      const stylesStr = (activeStyleObjs || []).map(s => s.name).join(", ");
      const payload = {
        omd_key: omdKey || "",
        persona_name: selectedPersonas.length > 0 ? selectedPersonas.map((p) => p.name).join(", ") : "",
        persona_prompt: selectedPersonas.length > 0 ? selectedPersonas.map((p) => p.prompt || p.passport || "").join("; ") : "",
        style: stylesStr || ((sliders.style && sliders.style !== "none") ? sliders.style : ""),
        archetypes: archetypesStr,
        keywords: userKeywords
      };

      let resp = null;

      // 1. Try P2P WebRTC data channel directly to OnMyChat API (binds this to httpDrive)
      if (httpDrive && typeof httpDrive.fetch === "function") {
        try {
          resp = await httpDrive.fetch("/generate/prompt/random", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(payload)
          });
        } catch (driveErr) {
          console.warn("[KreaStudio] httpDrive.fetch failed:", driveErr);
        }
      }

      // 2. If httpDrive was not available or failed, try standard HTTP endpoints
      if (!resp || !resp.ok) {
        for (const ep of ["/generate/prompt/random", "/ai/generate/prompt/random"]) {
          try {
            const r = await fetch(ep, {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify(payload)
            });
            if (r.ok) {
              resp = r;
              break;
            }
          } catch (_) {}
        }
      }

      if (resp && resp.ok) {
        const data = await resp.json().catch(() => null);
        if (data && data.prompt && data.prompt.trim()) {
          promptText = data.prompt.trim();
        } else {
          promptText = fallbackGenerate();
        }
      } else {
        promptText = fallbackGenerate();
      }
    } catch (err) {
      console.warn("[KreaStudio] pickRandomSuggestion error:", err);
      promptText = fallbackGenerate();
    } finally {
      isRandomizingPrompt = false;
      await tick();
      resizeTextarea();
      if (textareaEl) textareaEl.focus();
    }
  }

  function selectPrompt(p) {
    promptText = typeof p === "string" ? p : p.text || p.prompt || "";
    showPromptsPopup = false;
    tick().then(() => {
      resizeTextarea();
      if (textareaEl) textareaEl.focus();
    });
  }

  function handleDeletePrompt(e, p) {
    e.stopPropagation();
    const pId = p._id || p.id;
    if (chatStore && typeof chatStore.deletePrompt === "function" && pId) {
      chatStore.deletePrompt(pId).catch(() => {});
    }
    recentPrompts = recentPrompts.filter((x) => (x._id || x.id || x) !== (pId || p));
    try {
      localStorage.setItem("krea_recent_prompts", JSON.stringify(recentPrompts));
    } catch (_) {}
  }

  function formatPromptTime(iso) {
    if (!iso) return "";
    try {
      const d = new Date(iso);
      if (isNaN(d.getTime())) return "";
      const now = new Date();
      const diffMs = now.getTime() - d.getTime();
      const diffMins = Math.floor(diffMs / 60000);
      const diffHours = Math.floor(diffMs / 3600000);
      const diffDays = Math.floor(diffMs / 86400000);
      if (diffMins < 1) return $_("krea_time_just_now", {}, "только что");
      if (diffMins < 60) return $_("krea_time_mins_ago", { count: diffMins }, `${diffMins} мин назад`);
      if (diffHours < 24) return $_("krea_time_hours_ago", { count: diffHours }, `${diffHours} ч назад`);
      if (diffDays < 7) return $_("krea_time_days_ago", { count: diffDays }, `${diffDays} дн назад`);
      return d.toLocaleDateString();
    } catch (_) {
      return "";
    }
  }

  function handleDocumentClick(e) {
    if (showPromptsPopup && !e.target.closest(".prompts-popup") && !e.target.closest(".toggle-history-pill")) {
      showPromptsPopup = false;
    }
    if (showStyleDropdown && !e.target.closest(".style-dropdown-control")) {
      closeStyleDropdown();
    }
    if (showArchetypeDropdown && !e.target.closest(".archetype-dropdown-control")) {
      closeArchetypeDropdown();
    }
    if (showAdvancedSettings && !e.target.closest(".advanced-settings-control")) {
      closeAdvancedSettings();
    }
    if (activeRefPopoverIdx !== null && !e.target.closest(".ref-chip-wrap")) {
      activeRefPopoverIdx = null;
    }
  }

  function handleKeydown(e) {
    if (e.key === "Escape") {
      if (activeRefPopoverIdx !== null) {
        activeRefPopoverIdx = null;
      }
      if (activeVideoModalItem) {
        activeVideoModalItem = null;
      }
      if (deleteConfirmTarget) {
        deleteConfirmTarget = null;
      }
      if (showStyleDropdown) {
        closeStyleDropdown();
      }
      if (showArchetypeDropdown) {
        closeArchetypeDropdown();
      }
      if (showAdvancedSettings) {
        closeAdvancedSettings();
      }
      if (showPromptsPopup) {
        showPromptsPopup = false;
      }
    }
  }

  function downloadImage(item) {
    if (!item) return;
    const src = item.blobUrl || `${getFullImageUrl(item)}?download=true`;
    const a = document.createElement("a");
    a.href = src;
    a.download = item.name || (item.isVideo ? "creation.mp4" : "creation.png");
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  }

  function shareImage(item) {
    if (!item) return;
    const dev = resolveDevice();
    dispatch("open-file", {
      source: {
        path: item.path,
        fullPath: item.path,
        name: item.name,
        title: item.title || item.name,
        description: item.description || item.prompt || "",
        customAttributes: {
          title: item.title || item.name,
          description: item.description || item.prompt || ""
        },
        isDir: false,
        type: item.isVideo ? "video" : "image",
        mimeType: item.isVideo ? "video/mp4" : "image/png",
        linkId: dev?.linkId || targetLinkId || "",
        targetDevice: dev || targetDevice || null
      }
    });
  }

  function setBoundedPromptCache(fileName, promptStr) {
    if (!fileName || !promptStr || typeof localStorage === "undefined") return;
    try {
      const cache = JSON.parse(localStorage.getItem("krea_file_prompts") || "{}");
      delete cache[fileName];
      cache[fileName] = promptStr;
      const keys = Object.keys(cache);
      while (keys.length > 80) {
        delete cache[keys.shift()];
      }
      localStorage.setItem("krea_file_prompts", JSON.stringify(cache));
    } catch (_) {
      try {
        localStorage.removeItem("gun/");
        localStorage.removeItem("krea_file_prompts");
      } catch (__) {}
    }
  }

  async function refreshCivitaiAccounts() {
    try {
      const fetchFn = getKreaFetch();
      const mcpPayload = getMcpPayload();
      const res = await fetchFn("/krea/civitai/accounts", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
          mcp_keys: mcpPayload,
          settings: { mcp_keys: mcpPayload }
        })
      });
      if (res && res.ok) {
        const data = await res.json();
        if (data && Array.isArray(data.accounts)) {
          civitaiAccounts = data.accounts;
        }
      }
    } catch (_) {}
  }

  function resolveDefaultCivitaiAccount(personaHint = "") {
    const avail = effectiveCivitaiAccounts || [];
    if (personaHint) {
      const foundP = (personas || []).find((p) => p.id === personaHint || p.name === personaHint);
      if (foundP && foundP.civitai_account && avail.some((a) => a.id === foundP.civitai_account)) {
        return foundP.civitai_account;
      }
    }
    if (typeof window !== "undefined") {
      try {
        const savedAcc = window.localStorage.getItem("omc_civitai_pub_account") || window.sessionStorage.getItem("omc_civitai_pub_account") || "";
        if (savedAcc && (savedAcc === "custom" || avail.some((a) => a.id === savedAcc))) {
          return savedAcc;
        }
      } catch (_) {}
    }
    if (avail.length > 0) return avail[0].id;
    return "custom";
  }

  async function blobToDataUrl(blobOrUrl) {
    if (!blobOrUrl) return "";
    let actualBlob = blobOrUrl;
    if (typeof actualBlob === "string") {
      if (actualBlob.startsWith("data:")) return actualBlob;
      try {
        const r = await fetch(actualBlob);
        if (!r.ok) return "";
        actualBlob = await r.blob();
      } catch (_) {
        return "";
      }
    }
    if (typeof Blob !== "undefined" && !(actualBlob instanceof Blob)) return "";
    return await new Promise((resolve) => {
      const reader = new FileReader();
      reader.onloadend = () => resolve(typeof reader.result === "string" ? reader.result : "");
      reader.onerror = () => resolve("");
      reader.readAsDataURL(actualBlob);
    });
  }

  let autopilotToastMsg = "";
  let autopilotToastTimer = null;

  function showAutopilotToast(msg) {
    autopilotToastMsg = msg;
    if (autopilotToastTimer) clearTimeout(autopilotToastTimer);
    autopilotToastTimer = setTimeout(() => {
      autopilotToastMsg = "";
    }, 3500);
  }

  async function sendItemsToAutopilot(itemsToSend, titleText = "", detailText = "", tagsText = "", personaIdText = "") {
    if (isPublishingCivitai || !itemsToSend || !itemsToSend.length) return;
    isPublishingCivitai = true;
    try {
      const targetDev = resolveDevice();
      const isAiNodeTarget = Boolean(targetDev && targetDev.isAiNode);
      const drive = getTargetDrive();
      const fetchFn = getKreaFetch();

      const payloadImages = [];
      for (const it of itemsToSend) {
        const entry = {
          image_name: it.name || it.image_name || "creation.png",
          image_path: it.path || it.image_path || "",
          story_slug: it.story_slug || it.subfolder || activeStorySubfolder || "",
          prompt: it.prompt || ""
        };

        if (!isAiNodeTarget) {
          if (it.blob instanceof Blob) {
            entry.data_b64 = await blobToDataUrl(it.blob);
          }
          if (!entry.data_b64 && it.blobUrl) {
            entry.data_b64 = await blobToDataUrl(it.blobUrl);
          }
          if (!entry.data_b64 && entry.image_path) {
            const tmpBlobUrl = await fetchImageBlobViaWebRTC(entry.image_path, "").catch(() => null);
            if (tmpBlobUrl) {
              entry.data_b64 = await blobToDataUrl(tmpBlobUrl);
              try {
                if (typeof tmpBlobUrl === "string" && tmpBlobUrl.startsWith("blob:")) {
                  URL.revokeObjectURL(tmpBlobUrl);
                }
              } catch (_) {}
            }
          }
          if (!entry.data_b64 && entry.image_path && httpDrive && typeof httpDrive.fetch === "function") {
            try {
              const r = await httpDrive.fetch(entry.image_path);
              if (r && r.ok) entry.data_b64 = await blobToDataUrl(await r.blob());
            } catch (_) {}
          }
          if (!entry.data_b64 && entry.image_path && drive && typeof drive.homeRead === "function") {
            try {
              const b64 = await drive.homeRead(entry.image_path);
              if (b64) entry.data_b64 = b64;
            } catch (_) {}
          }
        }
        payloadImages.push(entry);
      }

      const res = await fetchFn("/krea/autopilot/intake", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
          images: payloadImages,
          title: titleText,
          detail: detailText,
          tags: tagsText,
          persona_id: personaIdText || ""
        })
      });

      const data = await res.json().catch(() => ({}));
      if (!res.ok) {
        throw new Error(data.detail || data.error || `HTTP ${res.status}`);
      }
      for (const it of itemsToSend) {
        it.civitaiPostUrl = data.autopilot_url || "/autopilot";
      }
      items = [...items];
      showAutopilotToast($_("autopilot_sent_toast", {}, "🚀 Sent to Autopilot for review"));
    } catch (err) {
      generationError = err.message || String(err);
    } finally {
      isPublishingCivitai = false;
    }
  }

  async function publishToCivitai(item) {
    if (!item) return;
    await ensureItemPromptAndTitle(item);
    const cleanStem = (item.name || "").replace(/\.[^.]+$/, "");
    const title = (item.title && item.title !== cleanStem) ? item.title : "";
    const detail = (item.description && item.description !== item.prompt) ? item.description : "";
    const tags = item.isVideo ? "video, minimax h3, comfyui" : "";
    const personaId = item.persona || (selectedPersona && selectedPersona.id !== "none" ? selectedPersona.id : "") || "";
    await sendItemsToAutopilot([item], title, detail, tags, personaId);
  }

  async function openPublishStoryToCivitai() {
    const renderedScenes = (storyScenes || []).filter((s) => s.image_name);
    if (renderedScenes.length === 0) {
      alert($_("civitai_err_no_scenes", {}, "Please shoot at least one story frame before publishing."));
      return;
    }
    await persistCurrentStory(true).catch(() => {});
    const storyItems = renderedScenes.map((sc) => ({
      name: sc.image_name,
      path: sc.image_path || (storySlug ? `${storySlug}/${sc.image_name}` : sc.image_name),
      story_slug: storySlug || "",
      prompt: sc.prompt || "",
      blob: sc.imageBlob || null,
      blobUrl: sc.blobUrl || ""
    }));
    const title = (storyTitle || "").trim();
    const detail = renderedScenes
      .map((s) => (s.narrative ? `${s.title ? `**${s.scene_number}. ${s.title}**\n` : ""}${s.narrative}` : ""))
      .filter(Boolean)
      .join("\n\n");
    const personaId = (selectedPersona && selectedPersona.id !== "none") ? selectedPersona.id : "";
    await sendItemsToAutopilot(storyItems, title, detail, "", personaId);
  }

  async function submitCivitaiPublish() {
    if (isPublishingCivitai || !civitaiPubItems.length) return;
    const rawCustomTok = civitaiPubCustomToken.trim();
    if (civitaiPubAccountId === "custom" && !rawCustomTok) {
      civitaiPubError = $_("civitai_err_missing_token", {}, "Enter a CivitAI API key (or add one in Settings → MCP API Keys).");
      return;
    }
    isPublishingCivitai = true;
    civitaiPubError = "";
    civitaiPubResult = null;

    try {
      if (typeof window !== "undefined" && civitaiPubAccountId && civitaiPubAccountId !== "custom") {
        try {
          window.localStorage.setItem("omc_civitai_pub_account", civitaiPubAccountId);
        } catch (_) {
          try { window.sessionStorage.setItem("omc_civitai_pub_account", civitaiPubAccountId); } catch (__) {}
        }
      }

      const targetDev = resolveDevice();
      const isAiNodeTarget = Boolean(targetDev && targetDev.isAiNode);
      const drive = getTargetDrive();
      const fetchFn = getKreaFetch();

      const payloadImages = [];
      for (const it of civitaiPubItems) {
        const entry = {
          image_name: it.name || it.image_name || "creation.png",
          image_path: it.path || it.image_path || "",
          story_slug: it.story_slug || it.subfolder || activeStorySubfolder || "",
          prompt: it.prompt || ""
        };

        if (!isAiNodeTarget) {
          if (it.blob instanceof Blob) {
            entry.data_b64 = await blobToDataUrl(it.blob);
          }
          if (!entry.data_b64 && it.blobUrl) {
            entry.data_b64 = await blobToDataUrl(it.blobUrl);
          }
          if (!entry.data_b64 && entry.image_path) {
            const tmpBlobUrl = await fetchImageBlobViaWebRTC(entry.image_path, "").catch(() => null);
            if (tmpBlobUrl) {
              entry.data_b64 = await blobToDataUrl(tmpBlobUrl);
              try {
                if (typeof tmpBlobUrl === "string" && tmpBlobUrl.startsWith("blob:")) {
                  URL.revokeObjectURL(tmpBlobUrl);
                }
              } catch (_) {}
            }
          }
          if (!entry.data_b64 && entry.image_path && httpDrive && typeof httpDrive.fetch === "function") {
            try {
              const r = await httpDrive.fetch(entry.image_path);
              if (r && r.ok) entry.data_b64 = await blobToDataUrl(await r.blob());
            } catch (_) {}
          }
          if (!entry.data_b64 && entry.image_path && drive && typeof drive.homeRead === "function") {
            try {
              const b64 = await drive.homeRead(entry.image_path);
              if (b64) entry.data_b64 = b64;
            } catch (_) {}
          }
        }
        payloadImages.push(entry);
      }

      const mcpPayload = getMcpPayload();
      const res = await fetchFn("/krea/civitai/publish", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
          images: payloadImages,
          title: civitaiPubTitle.trim(),
          detail: civitaiPubDetail.trim(),
          tags: civitaiPubTags.trim(),
          publish: Boolean(civitaiPubImmediate),
          include_meta: Boolean(civitaiPubIncludeMeta),
          account_id: civitaiPubAccountId === "custom" ? "" : civitaiPubAccountId,
          custom_token: civitaiPubAccountId === "custom" ? rawCustomTok : "",
          persona_id: civitaiPubPersonaId || "",
          mcp_keys: mcpPayload,
          settings: { mcp_keys: mcpPayload }
        })
      });

      const data = await res.json().catch(() => ({}));
      if (!res.ok) {
        throw new Error(data.detail || data.error || `HTTP ${res.status}`);
      }
      civitaiPubResult = data;
      if (civitaiPubAccountId === "custom" && rawCustomTok && civitaiPubSaveKey) {
        try {
          const byApp = ($mcpKeysStore && $mcpKeysStore.byApp) || {};
          const existingMatch = Object.values(byApp).find((a) => a && a.key === rawCustomTok);
          let savedAppId = existingMatch ? existingMatch.appId : "";
          if (!savedAppId) {
            const uname = String(data.username || "").trim();
            const slug = uname.toLowerCase().replace(/[^a-z0-9_-]/g, "_");
            if (!byApp["civitai"]) {
              savedAppId = "civitai";
            } else if (slug) {
              savedAppId = `civitai_${slug}`;
            } else {
              savedAppId = `civitai_${Date.now().toString(36)}`;
            }
            const accLabel = uname ? `CivitAI (@${uname})` : "CivitAI";
            await saveMcpAppKey(savedAppId, {
              name: accLabel,
              url: "https://civitai.com/user/account",
              key: rawCustomTok
            });
          }
          await refreshCivitaiAccounts();
          civitaiPubAccountId = savedAppId;
          civitaiPubCustomToken = "";
          if (typeof window !== "undefined") {
            try { window.localStorage.setItem("omc_civitai_pub_account", savedAppId); } catch (_) {
              try { window.sessionStorage.setItem("omc_civitai_pub_account", savedAppId); } catch (__) {}
            }
          }
        } catch (_) {}
      }
      if (civitaiPubMode === "single" && civitaiPubItems[0]) {
        civitaiPubItems[0].civitaiPostUrl = data.post_url;
        items = [...items];
      }
    } catch (err) {
      civitaiPubError = err.message || String(err);
    } finally {
      isPublishingCivitai = false;
    }
  }

  async function ensureItemPromptAndTitle(item) {
    if (!item) return;
    if (item.title && item.title !== item.name.replace(/\.[^.]+$/, "") && item.prompt && item.description) return;
    const readmePath = item.path.replace(/\.[^/.]+$/, "") + ".Readme.md";
    const baseName = item.name.replace(/\.[^/.]+$/, "") + ".Readme.md";
    try {
      const drive = getTargetDrive();
      let txt = "";
      if (drive && (drive.isConnected || drive.status === "connected")) {
        txt = await readDriveText(drive, readmePath).catch(() => null);
      }
      if (!txt && httpDrive && typeof httpDrive.fetch === "function") {
        const resp = await httpDrive.fetch(readmePath).catch(() => null);
        if (resp && resp.ok) txt = await resp.text().catch(() => "");
      }
      if (!txt && typeof window !== "undefined") {
        const resp = await window.fetch(`/generated/${baseName}`).catch(() => null);
        if (resp && resp.ok) txt = await resp.text().catch(() => "");
      }
      if (txt) {
        const titleMatch = txt.match(/^#\s*([^\n]+)/);
        if (titleMatch) item.title = titleMatch[1].trim();
        const promptMatch = txt.match(/<!--\s*prompt:\s*([\s\S]*?)-->/i);
        if (promptMatch) item.prompt = promptMatch[1].trim();
        const descText = txt.replace(/<!--\s*prompt:[\s\S]*?-->/gi, "").replace(/^#\s*[^\n]+\n+/, "").trim();
        item.description = descText;
        if (!item.prompt) item.prompt = descText;
      }
    } catch (_) {}
  }

  function copyPrompt(text, id) {
    if (!navigator.clipboard || !text) return;
    navigator.clipboard.writeText(text).then(() => {
      copiedPromptId = id;
      setTimeout(() => { copiedPromptId = null; }, 2000);
    });
  }

  function repeatPrompt(text, item = null) {
    if (!text) return;
    promptText = text;
    if (item?.persona) {
      const match = personas.find(
        (p) => p.name && (p.name.toLowerCase() === item.persona.toLowerCase() || item.persona.toLowerCase().includes(p.name.toLowerCase()))
      );
      if (match && !selectedPersonas.some((sp) => sp.id === match.id)) {
        selectedPersonas = [match];
      }
    }
    activeTab = "studio";
    tick().then(() => {
      if (textareaEl) {
        textareaEl.focus();
        resizeTextarea();
      }
    });
  }

  async function openGallery(activeItem) {
    if (activeItem && activeItem.isVideo) {
      await ensureItemPromptAndTitle(activeItem);
      shareImage(activeItem);
      return;
    }

    const validItems = items.filter((it) => it && !it.isVideo && (it.blobUrl || it.url || it.path || it.id));
    if (!validItems.length) return;

    if (activeItem) {
      await ensureItemPromptAndTitle(activeItem);
      const swReady = typeof navigator !== "undefined" && navigator.serviceWorker && navigator.serviceWorker.controller;
      if (!activeItem.blobUrl && (!swReady || activeItem.thumbBlobUrl)) {
        const fullBlob = await fetchImageBlobViaWebRTC(activeItem.path, "");
        if (fullBlob) {
          activeItem.blobUrl = fullBlob;
          imageBlobCache[activeItem.path] = fullBlob;
          imageBlobCache[activeItem.name] = fullBlob;
        }
      }
    }

    let startIndex = 0;
    if (activeItem) {
      const idx = validItems.findIndex((it) => it.id === activeItem.id);
      if (idx !== -1) startIndex = idx;
    }

    if (lgPagination) {
      try { lgPagination.cleanup(); } catch (_) {}
      lgPagination = null;
    }
    if (lgInstance) {
      try { lgInstance.destroy(); } catch (_) {}
      lgInstance = null;
    }

    const escapeCaption = (s) => String(s || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
    const dynamicEl = validItems.map((it) => {
      const srcUrl = it.blobUrl || getViewImageUrl(it);
      const thumbUrl = it.thumbBlobUrl || it.blobUrl || getThumbnailUrl(it);
      const rawTitle = (it.title || "").trim();
      const rawDesc = (it.description || it.prompt || "").trim();
      const title = isJustFileName(rawTitle, it.name, it.path) ? "" : rawTitle;
      const desc = isJustFileName(rawDesc, it.name, it.path) ? "" : rawDesc;
      let descHtml = "";
      if (title && desc) {
        descHtml = `<div class="omd-gallery-caption"><h4>${escapeCaption(title)}</h4><p>${escapeCaption(desc)}</p></div>`;
      } else if (title) {
        descHtml = `<div class="omd-gallery-caption"><h4>${escapeCaption(title)}</h4></div>`;
      } else if (desc) {
        descHtml = `<div class="omd-gallery-caption"><p>${escapeCaption(desc)}</p></div>`;
      }
      return {
        src: srcUrl,
        thumb: thumbUrl,
        downloadUrl: it.blobUrl || `${getFullImageUrl(it)}?download=true`,
        subHtml: descHtml,
        title: title,
        description: desc,
        name: it.name,
        path: it.path
      };
    });

    const galleryDiv = document.createElement("div");
    document.body.appendChild(galleryDiv);

    lgPagination = setupGalleryPagination(galleryDiv, dynamicEl, startIndex);

    lgInstance = lightGallery(galleryDiv, {
      licenseKey: "GPLv3",
      dynamic: true,
      dynamicEl,
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
    lgPagination.setInstance(lgInstance);

    const ensureSlideFullBlob = async (idx) => {
      const it = validItems[idx];
      if (!it || it.blobUrl) return;
      const swReady = typeof navigator !== "undefined" && navigator.serviceWorker && navigator.serviceWorker.controller;
      if (swReady && !it.thumbBlobUrl) return;
      const fullBlob = await fetchImageBlobViaWebRTC(it.path, "");
      if (fullBlob) {
        it.blobUrl = fullBlob;
        imageBlobCache[it.path] = fullBlob;
        imageBlobCache[it.name] = fullBlob;
        if (dynamicEl[idx]) {
          dynamicEl[idx].src = fullBlob;
          dynamicEl[idx].downloadUrl = fullBlob;
        }
        const imgEl = document.querySelector(`.lg-current .lg-image, .lg-item[data-lg-item-id="${idx}"] .lg-image`);
        if (imgEl && imgEl.src !== fullBlob) {
          imgEl.src = fullBlob;
        }
      }
    };
    galleryDiv.addEventListener("lgAfterSlide", (ev) => {
      const idx = ev?.detail?.index;
      if (typeof idx === "number") ensureSlideFullBlob(idx).catch(() => {});
    });

    galleryDiv.addEventListener("onCloseAfter", () => {
      try {
        if (lgPagination) lgPagination.cleanup();
        if (lgInstance) lgInstance.destroy();
      } catch (_) {}
      lgPagination = null;
      lgInstance = null;
      if (galleryDiv.parentNode) galleryDiv.parentNode.removeChild(galleryDiv);
    });

    lgInstance.openGallery(startIndex);
  }

  function initLightGallery() {
    // Gallery uses dynamic openGallery(item) modal viewer
  }

  function handleSwitchToCreations() {
    dispatch("switch-creations");
  }

  // =========================================================================
  // 🎬 AI DIRECTOR'S CHAIR FUNCTIONS (Prompt Co-Pilot & Multi-Scene Storyboard)
  // =========================================================================
  let directorWidth = typeof localStorage !== "undefined"
    ? Math.max(320, Math.min(780, parseInt(localStorage.getItem("krea_director_width_v1") || "390", 10) || 390))
    : 390;
  let isResizingDirector = false;

  function startDirectorResize(e) {
    if (innerWidth <= 960) return;
    e.preventDefault();
    isResizingDirector = true;
    const startX = e.clientX;
    const startW = directorWidth;
    const prevUserSelect = document.body.style.userSelect;
    const prevCursor = document.body.style.cursor;
    document.body.style.userSelect = "none";
    document.body.style.cursor = "col-resize";

    const onMove = (ev) => {
      if (typeof ev.clientX !== "number") return;
      const delta = startX - ev.clientX;
      const maxW = Math.max(360, Math.min(820, Math.floor(window.innerWidth * 0.58)));
      directorWidth = Math.max(320, Math.min(maxW, startW + delta));
      resizeDirectorTextarea();
    };

    const onUp = () => {
      isResizingDirector = false;
      document.body.style.userSelect = prevUserSelect;
      document.body.style.cursor = prevCursor;
      window.removeEventListener("pointermove", onMove);
      window.removeEventListener("pointerup", onUp);
      window.removeEventListener("pointercancel", onUp);
      try {
        localStorage.setItem("krea_director_width_v1", String(Math.round(directorWidth)));
      } catch (_) {}
    };

    window.addEventListener("pointermove", onMove);
    window.addEventListener("pointerup", onUp);
    window.addEventListener("pointercancel", onUp);
  }

  function resetDirectorWidth() {
    directorWidth = 390;
    try {
      localStorage.setItem("krea_director_width_v1", "390");
    } catch (_) {}
    tick().then(resizeDirectorTextarea);
  }

  function scrollDirectorFeedToBottom() {
    tick().then(() => {
      if (directorFeedEl) {
        directorFeedEl.scrollTop = directorFeedEl.scrollHeight;
      }
      if (typeof requestAnimationFrame === "function") {
        requestAnimationFrame(() => {
          if (directorFeedEl) {
            directorFeedEl.scrollTop = directorFeedEl.scrollHeight;
          }
        });
      }
    });
  }

  function toggleDirectorPanel() {
    if (!canShowDirector) {
      showDirectorPanel = false;
      return;
    }
    showDirectorPanel = !showDirectorPanel;
    try {
      localStorage.setItem("krea_director_open_v1", showDirectorPanel ? "1" : "0");
    } catch (_) {}
    if (showDirectorPanel) {
      loadSavedStories().catch(() => {});
      scrollDirectorFeedToBottom();
      tick().then(() => {
        resizeDirectorTextarea();
        if (directorTab === "prompt" && directorInputEl) directorInputEl.focus();
      });
    }
  }

  function pushPromptUndo(prevText) {
    const val = (prevText || "").trim();
    if (!val) return;
    if (promptUndoStack[promptUndoStack.length - 1] === val) return;
    promptUndoStack = [...promptUndoStack.slice(-14), val];
  }

  function undoDirectorPrompt() {
    if (promptUndoStack.length === 0) return;
    const prev = promptUndoStack[promptUndoStack.length - 1];
    promptUndoStack = promptUndoStack.slice(0, -1);
    promptText = prev;
    tick().then(resizeTextarea);
  }

  function applyDirectorQuickHint(template) {
    directorInput = template;
    tick().then(() => {
      resizeDirectorTextarea();
      if (directorInputEl) {
        directorInputEl.focus();
        directorInputEl.selectionStart = directorInputEl.selectionEnd = directorInput.length;
      }
    });
  }

  function applyDirectorSnapshot(snapText, snapSettings = null) {
    if (!snapText) return;
    if (snapSettings) {
      applyImportedSettings(snapSettings);
    }
    pushPromptUndo(promptText);
    promptText = snapText;
    tick().then(resizeTextarea);
  }

  let isDirectorImportingCivitai = false;

  async function askDirectorPrompt(andGenerate = false) {
    const instr = (directorInput || "").trim();
    if (!instr || isDirectorThinking) return;
    isDirectorThinking = true;
    isDirectorImportingCivitai = CIVITAI_URL_REGEX.test(instr) || CIVITAI_URL_REGEX.test(promptText || "");

    const userMsg = {
      id: "dm_" + Date.now(),
      role: "user",
      text: instr
    };
    directorMessages = [...directorMessages, userMsg];
    directorInput = "";
    await tick();
    resizeDirectorTextarea();
    if (directorFeedEl) directorFeedEl.scrollTop = directorFeedEl.scrollHeight;

    try {
      const fetchFn = getKreaFetch();
      const archetypesStr = (activeArchetypeObjs || []).map((a) => `${a.label} (${a.id})`).join(", ");
      const stylesStr = (activeStyleObjs || []).filter((s) => s.id !== "none").map((s) => s.name || s.label).join(", ");
      const mcpPayload = getMcpPayload();
      const chatHist = directorMessages
        .slice(-7, -1)
        .map((m) => ({
          role: m.role,
          content: m.role === "assistant" && m.promptSnapshot ? `${m.text}\nPrompt: ${m.promptSnapshot}` : m.text
        }));

      const res = await fetchFn("/krea/director/prompt", {
        method: "POST",
        headers: buildAuthHeaders(),
        body: JSON.stringify({
          omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
          instruction: instr,
          current_prompt: (promptText || "").trim(),
          base_model: sliders.base_model || "realism",
          persona_name: selectedPersonas.map((p) => p.name).join(", "),
          persona_prompt: selectedPersonas.map((p) => p.prompt || p.passport || "").join("; "),
          persona_personality: selectedPersonas.map((p) => p.personality || "").filter(Boolean).join("; "),
          styles: stylesStr,
          archetypes: archetypesStr,
          history: chatHist,
          warmup_gpu: Boolean(andGenerate),
          mcp_keys: mcpPayload,
          settings: {
            language: $locale || "ru",
            mcp_keys: mcpPayload
          }
        })
      });

      if (!res || !res.ok) {
        let errTxt = res ? `HTTP ${res.status}` : $_("krea_err_no_response", {}, "No response from server");
        if (res) {
          try {
            const ed = await res.json();
            errTxt = ed.detail || ed.error || errTxt;
          } catch (_) {}
        }
        throw new Error(errTxt);
      }

      const data = await res.json();
      if (data && data.applied_settings) {
        applyImportedSettings(data.applied_settings);
      }
      if (data && data.prompt) {
        pushPromptUndo(promptText);
        promptText = data.prompt.trim();
        await tick();
        resizeTextarea();
      }
      directorMessages = [
        ...directorMessages,
        {
          id: "da_" + Date.now(),
          role: "assistant",
          text: data.reply || $_("krea_dir_prompt_updated", {}, "Prompt updated!"),
          promptSnapshot: (data.prompt || promptText || "").trim(),
          appliedSettings: data.applied_settings || null
        }
      ];
      try {
        sessionStorage.setItem("krea_director_msgs_v1", JSON.stringify(directorMessages.slice(-20)));
      } catch (_) {}

      await tick();
      if (directorFeedEl) directorFeedEl.scrollTop = directorFeedEl.scrollHeight;

      if (andGenerate && promptText.trim()) {
        await runGeneration();
      }
    } catch (err) {
      directorMessages = [
        ...directorMessages,
        {
          id: "de_" + Date.now(),
          role: "assistant",
          isError: true,
          text: $_("krea_dir_error_prefix", { err: err.message || err }, `Error: ${err.message || err}`)
        }
      ];
    } finally {
      isDirectorThinking = false;
      isDirectorImportingCivitai = false;
    }
  }

  function saveSettingPreset() {
    const clean = (storySetting || "").trim();
    if (!clean) return;
    const label = (storyTitle || clean.slice(0, 32)).trim();
    const entry = { id: "sp_" + Date.now(), label, setting: clean };
    savedSettingPresets = [entry, ...savedSettingPresets.filter((x) => x.setting !== clean)].slice(0, 15);
    try {
      localStorage.setItem("krea_saved_settings_v1", JSON.stringify(savedSettingPresets));
    } catch (_) {}
  }

  function deleteSettingPreset(id) {
    savedSettingPresets = savedSettingPresets.filter((x) => x.id !== id);
    try {
      localStorage.setItem("krea_saved_settings_v1", JSON.stringify(savedSettingPresets));
    } catch (_) {}
  }

  function saveActiveDraftStoryToLocal() {
    try {
      const draft = {
        storyTitle,
        storySlug,
        storySetting,
        storyIdea,
        storySceneCount,
        storyScenes: storyScenes.map((s) => ({
          scene_number: s.scene_number,
          title: s.title,
          narrative: s.narrative,
          visual_prompt: s.visual_prompt,
          aspect: s.aspect,
          image_name: s.image_name || "",
          image_path: s.image_path || "",
          status: s.image_name ? "done" : "draft"
        }))
      };
      localStorage.setItem("krea_active_story_draft_v1", JSON.stringify(draft));
    } catch (_) {}
  }

  async function planOrContinueStory(mode = "plan") {
    if (isPlanningStory) return;
    if (mode === "plan" && !storySetting.trim() && !storyIdea.trim()) {
      alert($_("krea_story_err_empty_idea", {}, "Briefly describe the setting or story idea."));
      return;
    }
    isPlanningStory = true;
    try {
      const fetchFn = getKreaFetch();
      const mcpPayload = getMcpPayload();
      const archetypesStr = (activeArchetypeObjs || []).map((a) => `${a.label} (${a.id})`).join(", ");
      const stylesStr = (activeStyleObjs || []).filter((s) => s.id !== "none").map((s) => s.name || s.label).join(", ");
      const personasPayload = selectedPersonas.map((p) => ({
        id: p.id,
        name: p.name,
        title: p.title || "",
        prompt: p.prompt || p.passport || "",
        personality: p.personality || ""
      }));

      const res = await fetchFn("/krea/director/storyboard", {
        method: "POST",
        headers: buildAuthHeaders(),
        body: JSON.stringify({
          omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
          action: mode === "continue" && storyScenes.length > 0 ? "continue" : "plan",
          story_title: storyTitle.trim(),
          setting: storySetting.trim(),
          idea: storyIdea.trim(),
          scene_count: Number(storySceneCount) || 4,
          base_model: sliders.base_model || "realism",
          personas: personasPayload,
          styles: stylesStr,
          archetypes: archetypesStr,
          existing_scenes: mode === "continue" ? storyScenes : null,
          warmup_gpu: false,
          mcp_keys: mcpPayload,
          settings: {
            language: $locale || "ru",
            mcp_keys: mcpPayload
          }
        })
      });

      if (!res || !res.ok) {
        let errTxt = res ? `HTTP ${res.status}` : $_("krea_err_no_response", {}, "No response from server");
        if (res) {
          try {
            const ed = await res.json();
            errTxt = ed.detail || ed.error || errTxt;
          } catch (_) {}
        }
        throw new Error(errTxt);
      }

      const data = await res.json();
      if (!storyTitle.trim() && data.story_title) storyTitle = data.story_title;
      if (!storySlug.trim() && data.story_slug) storySlug = data.story_slug;
      if (!storySetting.trim() && data.setting) storySetting = data.setting;

      const mappedNew = (data.scenes || []).map((sc, i) => ({
        id: `sc_${Date.now()}_${i}`,
        scene_number: sc.scene_number || ((mode === "continue" ? storyScenes.length : 0) + i + 1),
        title: sc.title || $_("krea_story_scene_num", { num: i + 1 }, `Scene ${i + 1}`),
        narrative: sc.narrative || "",
        visual_prompt: sc.visual_prompt || "",
        aspect: sc.aspect || sliders.aspect || "896x1152",
        status: "draft",
        image_name: "",
        image_path: "",
        blobUrl: null,
        showPrompt: false,
        showRefine: false,
        refineText: ""
      }));

      if (mode === "continue" && storyScenes.length > 0) {
        storyScenes = [...storyScenes, ...mappedNew];
        storyIdea = "";
      } else {
        storyScenes = mappedNew;
        if (data.story_slug) storySlug = data.story_slug;
        if (data.story_title) storyTitle = data.story_title;
      }
      isSetupCollapsed = true;
      saveActiveDraftStoryToLocal();
      await persistCurrentStory(true);
    } catch (err) {
      alert($_("krea_story_err_plan", { err: err.message || err }, `Failed to plan storyboard: ${err.message || err}`));
    } finally {
      isPlanningStory = false;
    }
  }

  async function refineSingleScene(idx) {
    const sc = storyScenes[idx];
    if (!sc || refiningSceneIdx !== -1) return;
    const instr = (sc.refineText || "").trim();
    if (!instr) return;

    refiningSceneIdx = idx;
    try {
      const fetchFn = getKreaFetch();
      const mcpPayload = getMcpPayload();
      const res = await fetchFn("/krea/director/storyboard", {
        method: "POST",
        headers: buildAuthHeaders(),
        body: JSON.stringify({
          omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
          action: "refine_scene",
          story_title: storyTitle.trim(),
          setting: storySetting.trim(),
          idea: instr,
          base_model: sliders.base_model || "realism",
          personas: selectedPersonas.map((p) => ({ name: p.name, title: p.title || "", prompt: p.prompt || "", personality: p.personality || "" })),
          existing_scenes: storyScenes,
          target_scene_index: idx,
          warmup_gpu: false,
          mcp_keys: mcpPayload,
          settings: { language: $locale || "ru", mcp_keys: mcpPayload }
        })
      });
      if (!res || !res.ok) throw new Error(res ? `HTTP ${res.status}` : $_("krea_err_network", {}, "Network error"));
      const data = await res.json();
      if (data && data.scene) {
        storyScenes[idx] = {
          ...storyScenes[idx],
          title: data.scene.title || sc.title,
          narrative: data.scene.narrative || sc.narrative,
          visual_prompt: data.scene.visual_prompt || sc.visual_prompt,
          aspect: data.scene.aspect || sc.aspect,
          refineText: "",
          showRefine: false
        };
        storyScenes = [...storyScenes];
        saveActiveDraftStoryToLocal();
      }
    } catch (err) {
      alert($_("krea_story_err_refine", { err: err.message || err }, `Failed to update scene: ${err.message || err}`));
    } finally {
      refiningSceneIdx = -1;
    }
  }

  function removeStoryScene(idx) {
    storyScenes = storyScenes
      .filter((_, i) => i !== idx)
      .map((s, i) => ({ ...s, scene_number: i + 1 }));
    saveActiveDraftStoryToLocal();
  }

  function addManualStoryScene() {
    const nextNum = storyScenes.length + 1;
    storyScenes = [
      ...storyScenes,
      {
        id: `sc_${Date.now()}_${nextNum}`,
        scene_number: nextNum,
        title: $_("krea_story_scene_num", { num: nextNum }, `Scene ${nextNum}`),
        narrative: "",
        visual_prompt: "",
        aspect: sliders.aspect || "896x1152",
        status: "draft",
        image_name: "",
        image_path: "",
        blobUrl: null,
        showPrompt: true,
        showRefine: false,
        refineText: ""
      }
    ];
    saveActiveDraftStoryToLocal();
  }

  async function persistCurrentStory(silent = false) {
    if (storyScenes.length === 0 && !storyTitle.trim()) return null;
    isSavingStory = true;
    try {
      const fetchFn = getKreaFetch();
      const mcpPayload = getMcpPayload();
      const baseStorage = getBaseStorage();
      const cleanScenes = storyScenes.map((s, i) => ({
        scene_number: s.scene_number || i + 1,
        title: s.title || $_("krea_story_scene_num", { num: i + 1 }, `Scene ${i + 1}`),
        narrative: s.narrative || "",
        visual_prompt: s.visual_prompt || "",
        aspect: s.aspect || "896x1152",
        image_name: s.image_name || "",
        image_path: s.image_path || ""
      }));

      const res = await fetchFn("/krea/story/save", {
        method: "POST",
        headers: buildAuthHeaders(),
        body: JSON.stringify({
          omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
          story_title: storyTitle.trim() || $_("krea_story_default_title", {}, "New Story"),
          story_slug: storySlug.trim(),
          setting: storySetting.trim(),
          personas: selectedPersonas.map((p) => ({ id: p.id, name: p.name, title: p.title || "", prompt: p.prompt || "" })),
          scenes: cleanScenes,
          save_local: true,
          mcp_keys: mcpPayload,
          settings: { defaultStorage: baseStorage, mcp_keys: mcpPayload }
        })
      });

      if (res && res.ok) {
        const data = await res.json();
        if (data.story_slug) {
          storySlug = data.story_slug;
          if (!storyFolders.includes(storySlug)) {
            storyFolders = [...storyFolders, storySlug].sort();
          }
        }
        // Save to remote targetDrive if not saved locally on AI node
        if (!data.saved_locally && data.readme_markdown && data.story_slug) {
          const targetDrive = getTargetDrive();
          if (targetDrive) {
            try {
              const subFolder = `${getTargetFolder()}/${data.story_slug}`;
              await ensureFolderRecursive(targetDrive, subFolder);
              await writeDriveText(targetDrive, `${subFolder}/Readme.md`, data.readme_markdown);
              await writeDriveText(targetDrive, `${subFolder}/story.json`, JSON.stringify(data.story_json, null, 2));
            } catch (e) {
              console.warn("[Director] Remote story save warning:", e);
            }
          }
        }
        // Also mirror in localStorage index for instant access
        try {
          const list = JSON.parse(localStorage.getItem("krea_saved_stories_v1") || "[]");
          const entry = {
            story_slug: data.story_slug,
            story_title: storyTitle.trim() || $_("krea_story_default_title", {}, "New Story"),
            setting: storySetting.trim(),
            scene_count: cleanScenes.length,
            updated_at: new Date().toISOString(),
            story: data.story_json
          };
          const merged = [entry, ...list.filter((x) => x.story_slug !== entry.story_slug)].slice(0, 30);
          localStorage.setItem("krea_saved_stories_v1", JSON.stringify(merged));
          savedStoriesList = merged;
        } catch (_) {}
        saveActiveDraftStoryToLocal();
        return data;
      }
    } catch (err) {
      if (!silent) alert($_("krea_story_err_save", { err: err.message || err }, `Failed to save story: ${err.message || err}`));
    } finally {
      isSavingStory = false;
    }
    return null;
  }

  async function loadSavedStories() {
    let localList = [];
    try {
      localList = JSON.parse(localStorage.getItem("krea_saved_stories_v1") || "[]");
    } catch (_) {}

    try {
      const fetchFn = getKreaFetch();
      const res = await fetchFn("/krea/stories", { headers: buildAuthHeaders() });
      if (res && res.ok) {
        const data = await res.json();
        if (Array.isArray(data.stories) && data.stories.length > 0) {
          const bySlug = new Map();
          for (const st of [...data.stories, ...localList]) {
            if (st && st.story_slug && !bySlug.has(st.story_slug)) {
              bySlug.set(st.story_slug, st);
            }
          }
          savedStoriesList = Array.from(bySlug.values());
          return;
        }
      }
    } catch (_) {}
    savedStoriesList = localList;
  }

  function openSavedStory(entry) {
    if (!entry) return;
    const st = entry.story || entry;
    storyTitle = st.story_title || entry.story_title || "";
    storySlug = st.story_slug || entry.story_slug || "";
    storySetting = st.setting || entry.setting || "";
    storyIdea = "";
    const folder = getTargetFolder();
    storyScenes = (st.scenes || []).map((sc, i) => {
      const imgName = sc.image_name || "";
      const fullP = imgName && storySlug ? `${folder}/${storySlug}/${imgName}` : (sc.image_path || "");
      return {
        id: `sc_${Date.now()}_${i}`,
        scene_number: sc.scene_number || i + 1,
        title: sc.title || $_("krea_story_scene_num", { num: i + 1 }, `Scene ${i + 1}`),
        narrative: sc.narrative || "",
        visual_prompt: sc.visual_prompt || "",
        aspect: sc.aspect || "896x1152",
        status: imgName ? "done" : "draft",
        image_name: imgName,
        image_path: fullP,
        blobUrl: (fullP && imageBlobCache[fullP]) || (imgName && imageBlobCache[imgName]) || null,
        showPrompt: false,
        showRefine: false,
        refineText: ""
      };
    });
    isSetupCollapsed = storyScenes.length > 0;
    saveActiveDraftStoryToLocal();
  }

  function startFreshStory(keepSetting = false) {
    storyTitle = "";
    storySlug = "";
    if (!keepSetting) storySetting = "";
    storyIdea = "";
    storyScenes = [];
    isSetupCollapsed = false;
    saveActiveDraftStoryToLocal();
  }

  async function selectStorySubfolder(sub) {
    activeStorySubfolder = sub || "";
    if (sub) {
      sortOrder = "asc"; // Show story frames in 01, 02, 03 narrative order!
    }
    await loadCreations();
    if (sub) {
      const match = savedStoriesList.find((s) => s.story_slug === sub);
      if (match) {
        openSavedStory(match);
      } else {
        // Try reading story.json from targetDrive
        try {
          const drive = getTargetDrive();
          if (drive) {
            const raw = await readDriveText(drive, `${getTargetFolder()}/${sub}/story.json`).catch(() => null);
            if (raw) {
              const parsed = JSON.parse(raw);
              openSavedStory({ story_slug: sub, story: parsed });
            }
          }
        } catch (_) {}
      }
    }
  }

  async function shootSingleScene(idx) {
    const sc = storyScenes[idx];
    if (!sc || isGenerating) return false;
    if (!sc.visual_prompt || !sc.visual_prompt.trim()) {
      alert($_("krea_story_err_empty_vprompt", {}, "This scene has an empty visual prompt."));
      return false;
    }

    // Ensure we have a valid storySlug before shooting so all frames land in the same subfolder!
    if (!storySlug.trim()) {
      const saved = await persistCurrentStory(true);
      if (!storySlug.trim()) {
        storySlug = (saved && saved.story_slug) || `story_${Date.now()}`;
      }
    }

    shootingSceneIdx = idx;
    storyScenes[idx] = { ...storyScenes[idx], status: "shooting" };
    storyScenes = [...storyScenes];

    const padNum = String(sc.scene_number || idx + 1).padStart(2, "0");
    const sceneHeader = `${sc.scene_number || idx + 1}. ${sc.title || $_("krea_story_scene_num", { num: idx + 1 }, `Scene ${idx + 1}`)}`;
    const item = await runGeneration({
      prompt: sc.visual_prompt,
      aspect: sc.aspect || sliders.aspect || "896x1152",
      subfolder: storySlug,
      filenamePrefix: `Scene_${padNum}`,
      customTitle: sceneHeader,
      customDescription: sc.narrative || ""
    });

    shootingSceneIdx = -1;
    if (item && item.name) {
      storyScenes[idx] = {
        ...storyScenes[idx],
        status: "done",
        image_name: item.name,
        image_path: item.path,
        blobUrl: item.blobUrl || null,
        imageBlob: item.imageBlob || null
      };
      storyScenes = [...storyScenes];
      await persistCurrentStory(true);
      return true;
    } else {
      storyScenes[idx] = { ...storyScenes[idx], status: sc.image_name ? "done" : "draft" };
      storyScenes = [...storyScenes];
      return false;
    }
  }

  async function shootAllRemainingScenes() {
    if (isShootingSeries || isGenerating) return;
    const pendingIndices = storyScenes
      .map((s, idx) => (!s.image_name ? idx : -1))
      .filter((idx) => idx >= 0);
    if (pendingIndices.length === 0) {
      alert($_("krea_story_all_shot", {}, "All scenes are already shot! You can reshoot any card with 🔄 or continue the story."));
      return;
    }

    isShootingSeries = true;
    stopSeriesRequested = false;
    try {
      for (const idx of pendingIndices) {
        if (stopSeriesRequested) break;
        const ok = await shootSingleScene(idx);
        if (!ok || stopSeriesRequested) break;
      }
    } finally {
      isShootingSeries = false;
      stopSeriesRequested = false;
    }
  }

  async function exportStoryPack(openCivitai = false) {
    if (isExportingStory || storyScenes.length === 0) return;
    isExportingStory = true;
    try {
      const saved = await persistCurrentStory(true);
      const mdContent = (saved && saved.readme_markdown) || `# ${storyTitle}\n\n${storyScenes.map((s) => `## ${s.scene_number}. ${s.title}\n\n${s.narrative}\n`).join("\n")}`;

      // Copy formatted story + prompts to clipboard for easy CivitAI post description pasting
      if (navigator.clipboard) {
        const postSummary = `${storyTitle ? storyTitle + "\n\n" : ""}${storySetting ? "Setting: " + storySetting + "\n\n" : ""}` +
          storyScenes.map((s) => `[${s.scene_number}. ${s.title}]\n${s.narrative}`).join("\n\n");
        await navigator.clipboard.writeText(postSummary).catch(() => {});
      }

      const renderedScenes = storyScenes.filter((s) => s.image_name);
      if (renderedScenes.length > 0) {
        const { default: JSZip } = await import("jszip");
        const zip = new JSZip();
        zip.file("Story.md", mdContent);

        const fetchFn = getKreaFetch();
        const drive = getTargetDrive();

        for (const sc of renderedScenes) {
          const padNum = String(sc.scene_number || 1).padStart(2, "0");
          const ext = (sc.image_name.split(".").pop() || "png").toLowerCase();
          const zipImgName = `${padNum}_${(storySlug || "scene")}.${ext}`;
          let blob = sc.imageBlob || null;

          if (!blob && sc.blobUrl && sc.blobUrl.startsWith("blob:")) {
            try {
              const r = await fetch(sc.blobUrl);
              if (r.ok) blob = await r.blob();
            } catch (_) {}
          }
          if (!blob && sc.image_path && drive && typeof drive.homeRead === "function") {
            try {
              const b64 = await drive.homeRead(sc.image_path);
              if (b64) {
                const bin = atob(b64);
                const arr = new Uint8Array(bin.length);
                for (let k = 0; k < bin.length; k++) arr[k] = bin.charCodeAt(k);
                blob = new Blob([arr], { type: "image/png" });
              }
            } catch (_) {}
          }
          if (!blob && sc.image_name) {
            try {
              blob = await fetchLargeImageBlob(fetchFn, `/chat/image/${sc.image_name}`);
            } catch (_) {}
          }
          if (blob && blob.size > 0) {
            zip.file(zipImgName, blob);
          }
        }

        const zipBlob = await zip.generateAsync({ type: "blob" });
        const dlUrl = URL.createObjectURL(zipBlob);
        const a = document.createElement("a");
        a.href = dlUrl;
        a.download = `${storySlug || "story"}_civitai_pack.zip`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        setTimeout(() => URL.revokeObjectURL(dlUrl), 5000);
      }
    } catch (err) {
      alert($_("krea_story_err_export", { err: err.message || err }, `Failed to export story pack: ${err.message || err}`));
    } finally {
      isExportingStory = false;
    }
  }

  onMount(async () => {
    if (chatStore && typeof chatStore.onPromptsChanged === "function") {
      unsubPrompts = chatStore.onPromptsChanged((list) => {
        if (Array.isArray(list) && list.length > 0) {
          const same = list.length === recentPrompts.length && list.every((v, idx) => v === recentPrompts[idx]);
          if (!same) recentPrompts = list;
        }
      });
    }
    try {
      const local = JSON.parse(localStorage.getItem("krea_recent_prompts") || "[]");
      if (Array.isArray(local) && local.length > 0 && recentPrompts.length === 0) {
        recentPrompts = local;
      }
      if (localStorage.getItem("krea_director_open_v1") === "1" && (typeof window === "undefined" || window.innerWidth > 960)) {
        if (canShowDirector) {
          showDirectorPanel = true;
        } else {
          pendingSavedDirectorOpen = true;
        }
      }
      const savedMsgs = JSON.parse(sessionStorage.getItem("krea_director_msgs_v1") || "[]");
      if (Array.isArray(savedMsgs)) directorMessages = savedMsgs;
      if (showDirectorPanel && directorMessages.length > 0) {
        scrollDirectorFeedToBottom();
      }
      const savedPresets = JSON.parse(localStorage.getItem("krea_saved_settings_v1") || "[]");
      if (Array.isArray(savedPresets)) savedSettingPresets = savedPresets;
      const savedDraft = JSON.parse(localStorage.getItem("krea_active_story_draft_v1") || "null");
      if (savedDraft && typeof savedDraft === "object") {
        storyTitle = savedDraft.storyTitle || "";
        storySlug = savedDraft.storySlug || "";
        storySetting = savedDraft.storySetting || "";
        storyIdea = savedDraft.storyIdea || "";
        storySceneCount = savedDraft.storySceneCount || 4;
        if (Array.isArray(savedDraft.storyScenes) && savedDraft.storyScenes.length > 0) {
          storyScenes = savedDraft.storyScenes.map((s, i) => ({
            ...s,
            id: `sc_init_${i}`,
            showPrompt: false,
            showRefine: false,
            refineText: ""
          }));
          isSetupCollapsed = true;
        }
      }
    } catch (_) {}

    await refreshStudioStatusAndPresets();

    _studioRuntime.initialMounted = true;
    await loadPersonas();
    await loadCreations();
    loadSavedStories().catch(() => {});
    scrollDirectorFeedToBottom();
    tick().then(() => {
      resizeTextarea();
      resizeDirectorTextarea();
    });
  });

  async function refreshStudioStatusAndPresets() {
    try {
      const fetchFn = getKreaFetch();
      const mcpPayload = getMcpPayload();
      const reqBody = JSON.stringify({
        omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
        mcp_keys: mcpPayload,
        settings: { platform: getClientPlatform(), language: $locale || "ru", mcp_keys: mcpPayload }
      });
      const reqHeaders = {
        ...buildAuthHeaders(),
        "Content-Type": "application/json",
        "X-OMD-Platform": getClientPlatform()
      };

      fetchFn("/krea/presets", {
        method: "POST",
        headers: reqHeaders,
        body: reqBody
      }).then(async (res) => {
        if (res && res.ok) {
          const data = await res.json();
          if (data && data.art_styles && data.archetypes) {
            kreaPresets = data;
          }
        }
      }).catch(() => {});

      const res = await fetchFn("/krea/status", {
        method: "POST",
        headers: reqHeaders,
        body: reqBody
      });
      if (res && res.ok) {
        const data = await res.json();
        statusLoadedOnce = true;
        if (data && data.tier) studioTier = data.tier;
        kreaAllowed = Boolean(data && data.krea_allowed === true);
        videoAllowed = (data && typeof data.video_allowed === "boolean") ? data.video_allowed : kreaAllowed;
        const serverStudioEnabled = Boolean(
          data && (
            data.studio_enabled === true ||
            (kreaAllowed && data.tier && data.tier !== "guest" && data.tier !== "standard")
          )
        );
        directorAllowed = (data && typeof data.director_allowed === "boolean")
          ? data.director_allowed
          : Boolean(serverStudioEnabled || isPrivateMode);
        if (!isPrivateMode) {
          setDidStudioEnabled(serverStudioEnabled);
        }
        if (data && typeof data.credits === "number") memberCredits = data.credits;
        else memberCredits = null;
        if (data && typeof data.krea_daily_remaining === "number") kreaDailyRemaining = data.krea_daily_remaining;
        else kreaDailyRemaining = null;
        if (data && typeof data.krea_daily_limit === "number") kreaDailyLimit = data.krea_daily_limit;
        else kreaDailyLimit = null;
        kreaLockNotice = sanitizeExternalCheckoutText((data && data.krea_notice) || "");

        if (data && Array.isArray(data.civitai_accounts)) {
          civitaiAccounts = data.civitai_accounts;
        }
        if (data && data.gpu_warm) {
          const remSec = typeof data.gpu_warm_remaining === "number" ? data.gpu_warm_remaining : 290;
          gpuWarmUntil = Date.now() + remSec * 1000;
          isGpuWarm = remSec > 0;
        } else {
          gpuWarmUntil = 0;
          isGpuWarm = false;
        }

        const effectiveStudio = Boolean((serverStudioEnabled || isPrivateMode) && kreaSupported && kreaAllowed);
        const effectiveVideo = Boolean(effectiveStudio && videoAllowed);
        if (!effectiveStudio) {
          if (!isSdxlModel(sliders.base_model) && sdxlSupported !== false) {
            setBaseModel(lastSdxlModel || "sdxl_realistic", true);
          }
        } else {
          generationError = "";
          if (!effectiveVideo && isVideoModel(sliders.base_model)) {
            setBaseModel(lastDitModel || "realism", true);
          } else if (rawSavedBaseModel && !isSdxlModel(rawSavedBaseModel) && sliders.base_model !== rawSavedBaseModel) {
            if (!isVideoModel(rawSavedBaseModel) || effectiveVideo) {
              setBaseModel(rawSavedBaseModel, true);
            }
          }
        }
      }
    } catch (_) {}
  }

  function syncPassKeySignature(byAppObj) {
    const byApp = byAppObj || {};
    const sig = Object.keys(byApp)
      .sort()
      .map((k) => `${k}:${(byApp[k] && byApp[k].key) || ""}`)
      .join("|");
    if (_studioRuntime.lastPassKeySignature === null) {
      _studioRuntime.lastPassKeySignature = sig;
    } else if (sig !== _studioRuntime.lastPassKeySignature) {
      _studioRuntime.lastPassKeySignature = sig;
      refreshStudioStatusAndPresets();
    }
  }

  $: syncPassKeySignature($mcpKeysStore && $mcpKeysStore.byApp);

  onDestroy(() => {
    _studioRuntime.isDestroyed = true;
    if (_studioRuntime.driveCleanup) {
      try { _studioRuntime.driveCleanup(); } catch (_) {}
      _studioRuntime.driveCleanup = null;
    }
    if (unsubPrompts) unsubPrompts();
    if (lgPagination) {
      try { lgPagination.cleanup(); } catch (_) {}
      lgPagination = null;
    }
    if (lgInstance) {
      try { lgInstance.destroy(true); } catch (_) {}
      lgInstance = null;
    }
    if (generationTimer) {
      clearInterval(generationTimer);
    }
  });
</script>

<svelte:window bind:innerWidth={innerWidth} on:click={handleDocumentClick} on:keydown={handleKeydown} />

<div class="krea-studio-root skin-chat {isDarkTheme ? 'dark-theme' : 'light-theme'}" class:overlay-header={isOverlayHeader}>
  <!-- Header / Top Bar -->
  <header class="studio-header">
    <div class="header-left">
      {#if !sidebarOpen}
        <button
          type="button"
          class="icon-btn sidebar-toggle-btn"
          title={$_("unfold", {}, "Развернуть сайдбар")}
          aria-label={$_("unfold", {}, "Развернуть сайдбар")}
          on:click={() => dispatch("toggle-sidebar")}
        >
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
            <line x1="9" y1="3" x2="9" y2="21" />
          </svg>
        </button>
      {/if}

      <div class="header-mode-toggle-group">
        {#if sdxlSupported !== false && (canShowStudioTab || canShowVideoTab)}
          <div class="header-mode-toggle" role="tablist" aria-label={$_("studio_mode_aria", {}, "Generation mode")}>
            <button
              type="button"
              role="tab"
              aria-selected={studioEngineMode === "sdxl"}
              class="mode-toggle-btn"
              class:active={studioEngineMode === "sdxl"}
              title={$_("studio_mode_express_hint", {}, "Express (SDXL Turbo) — instant 6-step local generation (~3s)")}
              on:click={() => switchEngineMode("sdxl")}
            >
              <span class="mode-icon">⚡</span>
              <span>{$_("studio_mode_express", {}, "Express")}</span>
            </button>
            {#if canShowStudioTab}
              <button
                type="button"
                role="tab"
                aria-selected={studioEngineMode === "dit"}
                class="mode-toggle-btn"
                class:active={studioEngineMode === "dit"}
                title={$_("studio_mode_studio_hint", {}, "Studio (DiT: Krea 2 / Muse / Anima) — next-gen cinematic rendering")}
                on:click={() => switchEngineMode("dit")}
              >
                <span class="mode-icon">✨</span>
                <span>{$_("studio_mode_studio", {}, "Studio")}</span>
              </button>
            {/if}
            {#if canShowVideoTab}
              <button
                type="button"
                role="tab"
                aria-selected={studioEngineMode === "video"}
                class="mode-toggle-btn"
                class:active={studioEngineMode === "video"}
                title={$_("studio_mode_video_hint", {}, "Video + Stereo Audio (MiniMax-H3) — 24fps omni-modal video & sound generation")}
                on:click={() => switchEngineMode("video")}
              >
                <span class="mode-icon">🎬</span>
                <span>{$_("studio_mode_video", {}, "Video")}</span>
              </button>
            {/if}
          </div>
        {:else}
          <div class="studio-brand-pill" title={$_("krea_studio", {}, "Creative Studio")}>
            <span class="mode-icon">⚡</span>
            <span class="mode-text mode-text-full">{$_("krea_studio", {}, "Creative Studio")}</span>
            <span class="mode-text mode-text-short">Studio</span>
          </div>
        {/if}
        {#if items.length > 0}
          <span class="header-count-pill" title={$_("generated", {}, "Creations")}>
            {items.length}
          </span>
        {/if}
        {#if canShowStudioTab && kreaDailyRemaining !== null && kreaDailyLimit !== null}
          <span
            class="studio-tier-badge muse"
            title={`${kreaDailyRemaining}/${kreaDailyLimit}`}
          >
            ✨ {kreaDailyRemaining}/{kreaDailyLimit}
          </span>
        {:else if canShowStudioTab && memberCredits !== null}
          <span
            class="studio-tier-badge member"
            title={`${memberCredits}`}
          >
            💎 {memberCredits}
          </span>
        {/if}
      </div>
    </div>

    <!-- Centered Mode Switcher: Gallery vs Personas -->
    <div class="header-tabs">
      <button
        class="tab-btn"
        class:active={activeTab === "gallery"}
        on:click={() => (activeTab = "gallery")}
      >
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
          <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
          <circle cx="8.5" cy="8.5" r="1.5" />
          <polyline points="21 15 16 10 5 21" />
        </svg>
        <span>{$_("krea_tab_gallery", {}, "Галерея")}</span>
        {#if items.length > 0}
          <span class="badge">{items.length}</span>
        {/if}
      </button>

      <button
        class="tab-btn"
        class:active={activeTab === "personas"}
        on:click={() => (activeTab = "personas")}
      >
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
          <path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2" />
          <circle cx="9" cy="7" r="4" />
          <path d="M23 21v-2a4 4 0 0 0-3-3.87" />
          <path d="M16 3.13a4 4 0 0 1 0 7.75" />
        </svg>
        <span>{$_("krea_tab_personas", {}, "Персоны")}</span>
        {#if personas.length > 0}
          <span class="badge">{personas.length}</span>
        {/if}
      </button>
    </div>

    <div class="header-right">
      <!-- AI Director's Chair Toggle -->
      {#if canShowDirector}
        <button
          type="button"
          class="icon-btn director-icon-btn"
          class:is-active={showDirectorPanel}
          title={$_("krea_director_tooltip", {}, "Режиссёрское кресло — AI-помощник по промптам и сценариям")}
          aria-label={$_("krea_director_tooltip", {}, "Режиссёрское кресло")}
          on:click={toggleDirectorPanel}
        >
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <rect x="2" y="7" width="20" height="14" rx="2" ry="2" />
            <path d="M2 11h20" />
            <path d="M7 7l3 4" />
            <path d="M14 7l3 4" />
            <path d="M4 3l16 4" />
          </svg>
        </button>
      {/if}

      <!-- Sort Order Toggle -->
      <button
        type="button"
        class="icon-btn sort-order-btn"
        class:is-active={sortOrder === "asc"}
        title={sortOrder === "desc" ? $_("krea_sort_desc", {}, "Дата (сначала новые)") : $_("krea_sort_asc", {}, "Дата (сначала старые)")}
        aria-label={sortOrder === "desc" ? $_("krea_sort_desc", {}, "Дата (сначала новые)") : $_("krea_sort_asc", {}, "Дата (сначала старые)")}
        on:click={toggleSortOrder}
      >
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
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

      <button
        class="icon-btn"
        class:spinning={isLoadingFiles}
        title={$_("krea_refresh", {}, "Обновить")}
        on:click={() => { loadPersonas(); loadCreations(); }}
      >
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
          <polyline points="23 4 23 10 17 10" />
          <polyline points="1 20 1 14 7 14" />
          <path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15" />
        </svg>
      </button>

      <button
        class="icon-btn close-btn"
        title={$_("krea_close", {}, "Закрыть")}
        on:click={() => dispatch("close")}
      >
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
          <line x1="18" y1="6" x2="6" y2="18" />
          <line x1="6" y1="6" x2="18" y2="18" />
        </svg>
      </button>
    </div>
  </header>

  <!-- Main Workspace + AI Director Split -->
  <div class="studio-body-split">
  <div class="studio-canvas-col">
  <main class="studio-main">
    {#if activeTab === "personas"}
      <!-- PERSONAS VIEW -->
      <section class="personas-section" in:fade={{ duration: 150 }}>
        <div class="section-toolbar">
          <div class="section-desc">
            <h3>{$_("krea_personas_title", {}, "Библиотека персон")}</h3>
            <p>{$_("krea_personas_subtitle", {}, "Словесные паспорта и внешности персонажей, закрепляемые за генерациями")}</p>
          </div>
          <div class="toolbar-actions">
            {#if selectedPersonas.length > 0}
              <button class="go-gallery-btn" on:click={() => (activeTab = "gallery")}>
                <span>🎨 {$_("krea_to_studio", {}, "В студию")} ({selectedPersonas.length}) ➔</span>
              </button>
            {/if}
            <button class="ai-factory-btn" on:click={openAiPersonaModal}>
              <span class="btn-sparkle">✨</span> {$_("krea_ai_factory", {}, "AI Фабрика персон")}
            </button>
            <button class="create-persona-btn" on:click={openCreatePersonaModal}>
              <span>+</span> {$_("krea_create_manual", {}, "Создать вручную")}
            </button>
          </div>
        </div>

        <div class="personas-grid">
          {#each personas as p (p.id)}
            <!-- svelte-ignore a11y-click-events-have-key-events -->
            <!-- svelte-ignore a11y-no-static-element-interactions -->
            <div
              class="persona-card"
              class:selected={selectedPersonas.some((x) => x.id === p.id)}
              on:click={() => selectPersona(p)}
            >
              <div class="persona-avatar-wrap">
                {#if p.avatar}
                  <img src={p.avatar} alt={p.name} class="persona-avatar" />
                {:else}
                  <div class="persona-avatar-placeholder">
                    {p.name.slice(0, 1).toUpperCase()}
                  </div>
                {/if}
                {#if selectedPersonas.some((x) => x.id === p.id)}
                  <div class="active-check-badge">✓</div>
                {/if}
              </div>

              <div class="persona-info">
                <div class="persona-header-row">
                  <span class="persona-name">{p.name}</span>
                  {#if p.render_preset && p.render_preset.enabled !== false && p.render_preset.archetypes && p.render_preset.archetypes.length > 0}
                    <span class="persona-ref-chip" title={$_("krea_persona_native_chip_hint", {}, "Native character LoRA & render preset")}>🎛️ {p.render_preset.archetypes.join(", ")}</span>
                  {:else if p.avatarData || p.avatar}
                    <span class="persona-ref-chip" title="One-Shot Face Likeness: Krea 2 Identity Edit dual conditioning">📸 One-Shot Ref</span>
                  {/if}
                  {#if p.personality}
                    <span class="persona-ref-chip" title={p.personality}>💬 {$_("krea_persona_lore_chip", {}, "Lore")}</span>
                  {/if}
                  {#if p.title}
                    <span class="persona-title-chip">{p.title}</span>
                  {/if}
                </div>
                <p class="persona-passport" title={p.prompt}>{p.prompt}</p>
              </div>

              <div class="persona-actions">
                <button
                  class="card-action-btn select-btn"
                  class:active={selectedPersonas.some((x) => x.id === p.id)}
                  on:click={(e) => { e.stopPropagation(); selectPersona(p); }}
                >
                  {selectedPersonas.some((x) => x.id === p.id) ? "✓ " + $_("krea_persona_selected", {}, "Выбрана") : "+ " + $_("krea_persona_select", {}, "Выбрать")}
                </button>
                <button
                  class="card-action-btn icon-only"
                  title={$_("krea_persona_edit", {}, "Редактировать")}
                  on:click={(e) => { e.stopPropagation(); openEditPersonaModal(p); }}
                >
                  ✎
                </button>
                <button
                  class="card-action-btn icon-only danger"
                  title={$_("krea_persona_delete", {}, "Удалить")}
                  on:click={(e) => deletePersona(p, e)}
                >
                  ✕
                </button>
              </div>
            </div>
          {/each}

          <!-- New Persona Card Slot -->
          <!-- svelte-ignore a11y-click-events-have-key-events -->
          <!-- svelte-ignore a11y-no-static-element-interactions -->
          <div class="persona-card add-new-slot" on:click={openAiPersonaModal}>
            <div class="add-slot-circle">✨</div>
            <span class="add-slot-label">{$_("krea_ai_factory", {}, "AI Фабрика персон")}</span>
            <span class="add-slot-hint">{$_("krea_factory_hint", {}, "Создать по типажу в 1 клик")}</span>
          </div>
        </div>
      </section>
    {:else}
      <!-- GALLERY VIEW -->
      <section
        class="gallery-section"
        bind:this={scrollContainerEl}
        in:fade={{ duration: 150 }}
      >
        {#if storyFolders.length > 0 || activeStorySubfolder}
          <div class="story-folders-bar">
            <button
              type="button"
              class="story-folder-pill"
              class:active={!activeStorySubfolder}
              on:click={() => selectStorySubfolder("")}
            >
              📁 {$_("krea_all_creations", {}, "Все работы")}
            </button>
            {#each storyFolders as sf}
              <button
                type="button"
                class="story-folder-pill"
                class:active={activeStorySubfolder === sf}
                on:click={() => selectStorySubfolder(sf)}
                title={`generated/${sf}`}
              >
                🎞️ {sf}
              </button>
            {/each}
            {#if activeStorySubfolder && canShowDirector}
              <button
                type="button"
                class="story-open-director-chip"
                on:click={() => { showDirectorPanel = true; directorTab = "storyboard"; }}
              >
                🎬 {$_("krea_storyboard_tab", {}, "Storyboard")}
              </button>
            {/if}
          </div>
        {/if}

        {#if isLoadingFiles && items.length === 0 && !isGenerating}
          <div class="empty-gallery loading-gallery">
            <div class="gallery-load-spinner"></div>
            <p class="loading-gallery-text">{$_("krea_loading_creations", {}, "Загрузка творений...")}</p>
          </div>
        {:else if items.length === 0 && !isGenerating}
          <div class="empty-gallery">
            <div class="empty-icon">🎨</div>
            <h3>{$_("krea_no_creations_title", {}, "Пока нет генераций в студии")}</h3>
            <p>{$_("krea_no_creations_desc", {}, "Введите промпт внизу или выберите персону и нажмите \"Сгенерировать\". Скорость рендера ~4 сек!")}</p>
            <div class="empty-suggestions">
              {#each promptSuggestions as sug}
                <button class="sug-pill" on:click={() => { promptText = sug; }}>
                  "{sug}"
                </button>
              {/each}
            </div>
          </div>
        {:else}
          <div class="gallery-grid">
            {#if isGenerating}
              <div class="gallery-card placeholder-card">
                <div
                  class="placeholder-preview"
                  style={`aspect-ratio: ${(studioEngineMode === "video" ? videoAspect : sliders.aspect).replace('x', ' / ')};`}
                >
                  <div class="shimmer-mesh"></div>
                  <div class="glow-orb"></div>
                  <div class="placeholder-content">
                    <div class="status-pill">
                      <span class="pulse-dot"></span>
                      {#if generatingStage === "warming"}
                        <span class="status-text">⚡ {$_("krea_stage_warming", {}, "Прогрев студии")} {generationElapsed}s</span>
                      {:else if generatingStage === "generating"}
                        <span class="status-text">
                          🎨 {$_("krea_stage_generating", {}, "Генерация")}
                          {#if generatingTotalSteps > 0 && generatingStep > 0}
                            ({generatingStep}/{generatingTotalSteps})
                          {/if}
                          {generationElapsed}s
                        </span>
                      {:else}
                        <span class="status-text">📥 {$_("krea_stage_saving", {}, "Сохранение")} {generationElapsed}s</span>
                      {/if}
                    </div>
                    <p class="placeholder-prompt" title={generatingPrompt}>{generatingPrompt}</p>
                    <div class="placeholder-progress-bar">
                      <div
                        class="progress-bar-fill"
                        class:downloading={generatingStage === "saving"}
                        class:determinate={generatingStage === "generating" && generatingTotalSteps > 0 && generatingStep > 0}
                        style={generatingStage === "generating" && generatingTotalSteps > 0 && generatingStep > 0
                          ? `width: ${Math.max(6, Math.min(100, Math.round((generatingStep / generatingTotalSteps) * 100)))}%;`
                          : ""}
                      ></div>
                    </div>
                  </div>
                </div>
                <div class="card-footer-info placeholder-footer">
                  <span class="card-title">
                    {#if generatingStage === "warming"}
                      {warmingPhase === "loading_models"
                        ? $_("krea_stage_loading_models_hint", {}, "Загрузка модели и художественных стилей...")
                        : $_("krea_stage_warming_hint", {}, "Подготовка студии и загрузка моделей...")}
                    {:else if generatingStage === "generating"}
                      {generatingTotalSteps > 0 && generatingStep > 0
                        ? $_("krea_stage_generating_steps_hint", { step: generatingStep, total: generatingTotalSteps }, `Отрисовка кадра (шаг ${generatingStep} из ${generatingTotalSteps})...`)
                        : (warmingPhase === "loading_models"
                          ? $_("krea_stage_loading_models_hint", {}, "Загрузка модели и художественных стилей...")
                          : $_("krea_stage_generating_hint", {}, "Отрисовка кадра нейросетью..."))}
                    {:else}
                      {$_("krea_stage_saving_hint", {}, "Запись файла и подготовка кадра...")}
                    {/if}
                  </span>
                  {#if generatingPersona}
                    <span class="persona-tag">👤 {generatingPersona}</span>
                  {/if}
                </div>
              </div>
            {/if}
            {#each items as item (item.id)}
              {@const itemRefIdx = getGalleryItemRefIndex(item, attachedReferences)}
              <div class="gallery-card" class:is-attached-ref={itemRefIdx >= 0}>
                <!-- svelte-ignore a11y-click-events-have-key-events -->
                <!-- svelte-ignore a11y-no-static-element-interactions -->
                <div
                  class="gallery-card-thumb-wrap"
                  role="button"
                  tabindex="0"
                  on:click={() => openGallery(item)}
                  on:keydown={(e) => e.key === "Enter" && openGallery(item)}
                  on:mouseenter={() => {
                    if (item.isVideo) hoveredVideoId = item.id;
                  }}
                  on:mouseleave={() => {
                    if (item.isVideo && hoveredVideoId === item.id) {
                      hoveredVideoId = null;
                      playingVideoId = null;
                    }
                  }}
                >
                  {#if item.isVideo}
                    <img
                      src=""
                      alt={item.title || item.prompt}
                      use:lazyLoad={(activeDevSegment, getVideoPosterUrl(item))}
                      class="gallery-thumb"
                      loading="lazy"
                      on:error={(e) => handleVideoPosterError(item, e)}
                    />
                    {#if hoveredVideoId === item.id}
                      <video
                        src={(activeDevSegment, getFullImageUrl(item))}
                        autoplay
                        muted
                        loop
                        playsinline
                        preload="auto"
                        class="gallery-video-hover-layer"
                        class:is-playing={playingVideoId === item.id}
                        on:playing={() => {
                          if (hoveredVideoId === item.id) playingVideoId = item.id;
                        }}
                        on:error={(e) => handleVideoPreviewError(item, e)}
                      ></video>
                    {/if}
                    <div class="video-card-badge" title={$_("krea_video_stereo_badge", {}, "Видео со стереозвуком MiniMax-H3 (клик для просмотра со звуком)")}>
                      <span>🎬 MP4 · 🔊</span>
                      {#if item.duration}
                        <span>{item.duration}s</span>
                      {/if}
                    </div>
                  {:else}
                    <img
                      src=""
                      alt={item.title || item.prompt}
                      use:lazyLoad={(activeDevSegment, getThumbnailUrl(item))}
                      class="gallery-thumb"
                      loading="lazy"
                      on:error={(e) => handleImageError(item, e)}
                    />
                    {#if itemRefIdx >= 0}
                      <div class="attached-ref-card-badge">
                        {studioEngineMode === "video"
                          ? (itemRefIdx === 0
                              ? $_("krea_card_keyframe_start", {}, "🎬 #1 Старт")
                              : $_("krea_card_keyframe_end", {}, "🎬 #2 Финал"))
                          : `📎 #${itemRefIdx + 1}`}
                      </div>
                    {/if}
                  {/if}
                  {#if item.persona || item.totalTime}
                    <div class="card-thumb-badges">
                      {#if item.persona}
                        <span class="persona-tag">👤 {item.persona}</span>
                      {/if}
                      {#if item.totalTime}
                        <span class="card-time-tag" title={$_("krea_total_time", { total: item.totalTime, gpu: item.gpuTime ? `, GPU: ${item.gpuTime}s` : "" }, `Полное время: ${item.totalTime}s${item.gpuTime ? `, генерация GPU: ${item.gpuTime}s` : ""}`)}>
                          ⚡ {item.totalTime}s{#if item.gpuTime}<span class="gpu-sub">({item.gpuTime}s)</span>{/if}
                        </span>
                      {/if}
                    </div>
                  {/if}
                </div>

                <div class="card-footer-info">
                  <div class="card-meta">
                    <span
                      class="card-title"
                      title={item.description && item.title && item.description !== item.title ? `${item.title} — ${item.description}` : (item.description || item.title || item.prompt)}
                    >
                      {item.title && !isJustFileName(item.title, item.name, item.path) ? item.title : (item.description || item.prompt || item.title)}
                    </span>
                  </div>

                  <!-- svelte-ignore a11y-click-events-have-key-events -->
                  <!-- svelte-ignore a11y-no-static-element-interactions -->
                  <div class="card-toolbar" on:click|stopPropagation>
                    {#if item.prompt || item.title}
                      <button
                        type="button"
                        class="toolbar-btn"
                        class:copied={copiedPromptId === item.id}
                        title={$_("copy_prompt", {}, "Копировать промпт")}
                        on:click|stopPropagation={() => copyPrompt(item.prompt || item.title, item.id)}
                      >
                        {#if copiedPromptId === item.id}
                          <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="#10b981" stroke-width="2.5">
                            <polyline points="20 6 9 17 4 12" />
                          </svg>
                        {:else}
                          <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                            <rect x="9" y="9" width="13" height="13" rx="2" ry="2" />
                            <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1" />
                          </svg>
                        {/if}
                      </button>

                      <button
                        type="button"
                        class="toolbar-btn"
                        title={$_("repeat_prompt", {}, "Повторить промпт")}
                        on:click|stopPropagation={() => repeatPrompt(item.prompt || item.title, item)}
                      >
                        <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                          <polyline points="1 4 1 10 7 10" />
                          <path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10" />
                        </svg>
                      </button>
                    {/if}

                    <button
                      type="button"
                      class="toolbar-btn"
                      title={$_("download", {}, "Скачать")}
                      on:click|stopPropagation={() => downloadImage(item)}
                    >
                      <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                        <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                        <polyline points="7 10 12 15 17 10" />
                        <line x1="12" y1="15" x2="12" y2="3" />
                      </svg>
                    </button>

                    <button
                      type="button"
                      class="toolbar-btn"
                      title={$_("share", {}, "Поделиться")}
                      on:click|stopPropagation={() => shareImage(item)}
                    >
                      <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                        <circle cx="18" cy="5" r="3" />
                        <circle cx="6" cy="12" r="3" />
                        <circle cx="18" cy="19" r="3" />
                        <line x1="8.59" y1="13.51" x2="15.42" y2="17.49" />
                        <line x1="15.41" y1="6.51" x2="8.59" y2="10.49" />
                      </svg>
                    </button>

                    {#if canPublishCivitai}
                      <button
                        type="button"
                        class="toolbar-btn civitai-publish-btn {item.civitaiPostUrl ? 'published' : ''}"
                        disabled={isPublishingCivitai}
                        title={item.civitaiPostUrl ? $_("civitai_already_published_hint", {}, "In Autopilot (click to resend)") : $_("publish_civitai", {}, "Publish")}
                        on:click|stopPropagation={() => publishToCivitai(item)}
                      >
                        <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                          <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                          <polyline points="17 8 12 3 7 8" />
                          <line x1="12" y1="3" x2="12" y2="15" />
                        </svg>
                      </button>
                    {/if}

                    {#if !item.isVideo}
                      {#if canShowVideoTab}
                        <button
                          type="button"
                          class="toolbar-btn card-animate-btn"
                          class:active={studioEngineMode === "video" && itemRefIdx >= 0 && videoRefMode === "keyframe"}
                          title={studioEngineMode === "video" && itemRefIdx >= 0 && videoRefMode === "keyframe"
                            ? $_("krea_remove_video_keyframe", { idx: itemRefIdx + 1 }, `Убрать опорный кадр #${itemRefIdx + 1}`)
                            : (studioEngineMode === "video" && attachedReferences.length === 1 && videoRefMode === "keyframe"
                                ? $_("krea_add_second_keyframe", {}, "Добавить как #2 финальный кадр перехода (<Picture 2>)")
                                : (studioEngineMode === "video" && attachedReferences.length >= 2 && videoRefMode === "keyframe"
                                    ? $_("krea_replace_second_keyframe", {}, "Заменить #2 финальный кадр перехода (<Picture 2>)")
                                    : $_("krea_animate_with_sound", {}, "Оживить кадр со звуком — задать #1 старт (MiniMax-H3 FL2VA)")))}
                          on:click|stopPropagation={() => animateGalleryItem(item)}
                        >
                          <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                            <rect x="2" y="4" width="20" height="16" rx="2" ry="2" />
                            <polygon points="10 9 15 12 10 15 10 9" fill="currentColor" stroke="none" />
                          </svg>
                        </button>
                      {/if}

                      <button
                        type="button"
                        class="toolbar-btn card-ref-btn"
                        class:active={itemRefIdx >= 0}
                        title={itemRefIdx >= 0
                          ? $_("krea_remove_ref_idx", { idx: itemRefIdx + 1 }, `Убрать референс #${itemRefIdx + 1}`)
                          : (studioEngineMode === "video"
                              ? $_("krea_use_as_subject_ref", {}, "Референс персонажа / образа для свободной видеосцены (Ref2VA)")
                              : (attachedReferences.length === 1
                                  ? $_("krea_use_as_second_ref", {}, "Добавить как референс #2")
                                  : (attachedReferences.length >= 2
                                      ? $_("krea_replace_second_ref", {}, "Заменить референс #2")
                                      : $_("krea_use_as_ref", {}, "Использовать как референс"))))}
                        on:click|stopPropagation={() => addGalleryItemAsReference(item)}
                      >
                        <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                          <path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z" />
                          <circle cx="12" cy="13" r="4" />
                        </svg>
                      </button>
                    {:else if canShowVideoTab}
                      <button
                        type="button"
                        class="toolbar-btn card-extend-btn"
                        class:active={Boolean(extendingVideoItem && ((extendingVideoItem.path && extendingVideoItem.path === item.path) || extendingVideoItem.name === item.name))}
                        title={extendingVideoItem && ((extendingVideoItem.path && extendingVideoItem.path === item.path) || extendingVideoItem.name === item.name)
                          ? $_("krea_cancel_extend_video", {}, "Отменить продолжение этого видео")
                          : $_("krea_extend_video_btn", {}, "Продлить видео (+5 сек со звуком, MiniMax-H3 Continuity)")}
                        on:click|stopPropagation={() => extendGalleryVideo(item)}
                      >
                        <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                          <polygon points="5 4 15 12 5 20 5 4" />
                          <line x1="19" y1="7" x2="19" y2="17" />
                          <line x1="15" y1="12" x2="23" y2="12" />
                        </svg>
                      </button>
                    {/if}
                    <button
                      type="button"
                      class="toolbar-btn card-delete-btn"
                      title={$_("krea_persona_delete", {}, "Удалить")}
                      on:click|stopPropagation={(e) => deleteCreation(item, e)}
                    >
                      <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                        <line x1="18" y1="6" x2="6" y2="18" />
                        <line x1="6" y1="6" x2="18" y2="18" />
                      </svg>
                    </button>
                  </div>
                </div>
              </div>
            {/each}
          </div>
        {/if}
      </section>
    {/if}
  </main>

  <!-- Sticky Bottom Generation Composer with Sliders -->
  <footer class="studio-composer">
    {#if autopilotToastMsg}
      <div class="studio-notice-banner" style="background: rgba(16, 185, 129, 0.14); border-color: rgba(16, 185, 129, 0.35);" transition:fade={{ duration: 120 }}>
        <div class="studio-notice-text" style="color: #6ee7b7;">{autopilotToastMsg}</div>
        <div class="studio-notice-actions">
          <button type="button" class="studio-notice-close" on:click={() => (autopilotToastMsg = "")} aria-label="Close">
            ✕
          </button>
        </div>
      </div>
    {/if}
    {#if generationError}
      {@const sanitizedErr = sanitizeExternalCheckoutText(generationError)}
      {@const errLink = extractUrlFromText(sanitizedErr)}
      {@const errCleanText = stripUrlFromText(sanitizedErr)}
      <div class="studio-notice-banner" transition:fade={{ duration: 120 }}>
        <div class="studio-notice-text">{errCleanText || sanitizedErr}</div>
        <div class="studio-notice-actions">
          {#if errLink && !isMobileClientApp}
            <a href={errLink} target="_blank" rel="noopener noreferrer" class="studio-notice-cta">
              ⚡ Creative Studio →
            </a>
          {/if}
          <button type="button" class="studio-notice-close" on:click={() => (generationError = "")} aria-label="Close">
            ✕
          </button>
        </div>
      </div>
    {/if}

    <!-- Selected Persona, Photo Ref & Controls Row -->
    <div class="composer-controls-row">
      <div class="composer-controls-left">
        <!-- Active Personas Group -->
        {#if selectedPersonas.length > 0}
          <div class="active-personas-wrap">
            {#each selectedPersonas as p (p.id)}
              <div class="active-persona-pill" in:fade={{ duration: 100 }}>
                <span class="pill-avatar-icon">👤</span>
                <span class="pill-name">{p.name}</span>
                <button class="pill-clear-btn" title={$_("krea_clear_persona", { name: p.name }, `Сбросить ${p.name}`)} on:click={() => removeSelectedPersona(p.id)}>
                  ✕
                </button>
              </div>
            {/each}
            <button class="add-more-personas-btn" title={$_("krea_add_more_personas", {}, "Добавить еще персону")} on:click={() => (activeTab = "personas")}>
              <span class="pill-label-desktop">{$_("krea_add_more", {}, "+ Еще")}</span>
              <span class="pill-label-mobile">+👤</span>
            </button>
          </div>
        {:else}
          <button class="pick-persona-btn" title={$_("krea_pick_persona", {}, "+ Выбрать персону")} on:click={() => (activeTab = "personas")}>
            <span class="pill-label-desktop">{$_("krea_pick_persona", {}, "+ Выбрать персону")}</span>
            <span class="pill-label-mobile">+👤</span>
          </button>
        {/if}

        <!-- Hidden file input for references -->
        <input
          type="file"
          bind:this={refFileInputEl}
          accept="image/png,image/jpeg,image/webp"
          on:change={handleRefFileSelected}
          style="display: none;"
        />

        <!-- Add Reference Photo Pill (max 2) -->
        <button
          type="button"
          class="add-photo-pill"
          class:active={attachedReferences.length > 0}
          disabled={attachedReferences.length >= 2 || isUploadingRef}
          title={attachedReferences.length >= 2 ? $_("krea_attach_ref_max", {}, "Прикреплено 2 референса (максимум)") : $_("krea_attach_ref_title", {}, "Добавить картинку-референс (до 2 штук)")}
          on:click={() => refFileInputEl?.click()}
        >
          <span class="pill-label-desktop">
            {#if isUploadingRef}
              {$_("krea_uploading_ref", {}, "⏳ Загрузка...")}
            {:else if attachedReferences.length >= 2}
              {$_("krea_add_photo_max", {}, "📷 Фото (2/2)")}
            {:else if attachedReferences.length === 1}
              {$_("krea_add_photo_1", {}, "+ Добавить фото (1/2)")}
            {:else}
              {$_("krea_add_photo_pill", {}, "+ Добавить фото")}
            {/if}
          </span>
          <span class="pill-label-mobile">
            {#if isUploadingRef}
              ⏳
            {:else if attachedReferences.length > 0}
              📷{attachedReferences.length}
            {:else}
              +📷
            {/if}
          </span>
        </button>
      </div>

      <div class="composer-controls-right">
        <!-- Quick AI Director Toggle Pill -->
        {#if canShowDirector}
          <button
            type="button"
            class="toggle-director-pill"
            class:open={showDirectorPanel}
            class:active={showDirectorPanel}
            title={$_("krea_director_open_hint", {}, "Open AI Director's Chair (prompt co-pilot & storyboards)")}
            on:click={toggleDirectorPanel}
          >
            <span class="pill-label-desktop">{$_("krea_director_btn", {}, "Режиссёр")}</span>
            <span class="pill-label-mobile">🎬</span>
            <span class="pill-chevron">{showDirectorPanel ? "▶" : "◀"}</span>
          </button>
        {/if}

        <!-- Prompt History Toggle Pill -->
        <button
          type="button"
          class="toggle-history-pill"
          class:open={showPromptsPopup}
          class:active={showPromptsPopup}
          title={$_("krea_prompt_history", {}, "История промптов")}
          on:click|stopPropagation={() => (showPromptsPopup = !showPromptsPopup)}
        >
          <span class="pill-label-desktop">{$_("krea_history_pill", {}, "История")}</span>
          <span class="pill-label-mobile">🕒</span>
          <span class="pill-chevron">{showPromptsPopup ? "▼" : "▲"}</span>
        </button>

        <!-- Tags Preview Toggle Button -->
        <button
          type="button"
          class="toggle-tokens-btn"
          class:open={showPromptTokensPreview}
          class:active={showPromptTokensPreview}
          title={$_("krea_toggle_tags_title", {}, "Показать/скрыть теги")}
          on:click={() => (showPromptTokensPreview = !showPromptTokensPreview)}
        >
          <span class="pill-label-desktop">{$_("krea_show_tags", {}, "Теги")}</span>
          <span class="pill-label-mobile">🏷️</span>
          <span class="pill-chevron">{showPromptTokensPreview ? "▼" : "▲"}</span>
        </button>

        <!-- Parameters Toggle Button -->
        <button
          class="toggle-sliders-btn"
          class:open={showSlidersPanel}
          title={$_("krea_params_sliders", {}, "Параметры")}
          on:click={() => (showSlidersPanel = !showSlidersPanel)}
        >
          <span class="pill-label-desktop">{$_("krea_params_sliders", {}, "Параметры")}</span>
          <span class="pill-label-mobile">🎛️</span>
          <span class="pill-chevron">{showSlidersPanel ? "▼" : "▲"}</span>
        </button>

        <!-- Gear Settings Control (Extreme Right) -->
        <div class="advanced-settings-control" class:active={showAdvancedSettings}>
          <button
            type="button"
            class="advanced-gear-btn"
            class:active={showAdvancedSettings}
            title={$_("krea_advanced_gear_title", {}, "Параметры генерации: Шаги, CFG, Детализация")}
            on:click={(e) => {
              e.stopPropagation();
              showAdvancedSettings = !showAdvancedSettings;
              if (showAdvancedSettings) {
                showStyleDropdown = false;
                showArchetypeDropdown = false;
              }
            }}
          >
            <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/>
              <circle cx="12" cy="12" r="3"/>
            </svg>
            <span class="gear-summary-text pill-label-desktop">{sliders.steps} {$_("krea_steps_short", {}, "ш.")} / {sliders.cfg} cfg</span>
            <span class="pill-chevron">{showAdvancedSettings ? "▼" : "▲"}</span>
          </button>

          {#if showAdvancedSettings}
            <!-- svelte-ignore a11y-click-events-have-key-events -->
            <!-- svelte-ignore a11y-no-static-element-interactions -->
            <div class="dropdown-popup-menu advanced-popup" transition:fade={{ duration: 100 }} on:click|stopPropagation>
              <div class="popup-menu-header">
                <span>{$_("krea_advanced_settings", {}, "Параметры генерации")}</span>
                <button
                  type="button"
                  class="popup-reset-link"
                  on:click={resetAdvancedToModelDefaults}
                  title={$_("krea_advanced_reset_title", {}, "Сбросить к рекомендованным значениям для выбранной модели")}
                >
                  {$_("krea_reset", {}, "Сбросить")}
                </button>
              </div>

              <div class="advanced-popup-body">
                <!-- Setting 1: Detail Slider -->
                <div class="advanced-setting-row">
                  <div class="setting-title-row">
                    <span class="setting-name">{$_("krea_detail_label", {}, "Детализация (Detail):")}</span>
                    <span class="setting-val-badge">{sliders.detail > 0 ? `+${sliders.detail}` : sliders.detail}</span>
                  </div>
                  <input
                    type="range"
                    min="0.0"
                    max="1.0"
                    step="0.1"
                    bind:value={sliders.detail}
                    on:input={() => { sliders = { ...sliders }; saveLocalSliders(sliders); }}
                    class="slider-input advanced-slider"
                  />
                  <div class="setting-subtext">
                    <span>{$_("krea_detail_soft", {}, "Мягко (0.0)")}</span>
                    <span>{$_("krea_detail_normal", {}, "Норма (0.5)")}</span>
                    <span>{$_("krea_detail_ultra", {}, "Чётко (1.0)")}</span>
                  </div>
                </div>

                <!-- Setting 2: Steps Slider -->
                <div class="advanced-setting-row">
                  <div class="setting-title-row">
                    <span class="setting-name">{$_("krea_steps", {}, "Шаги (Steps):")}</span>
                    <span class="setting-val-badge">{sliders.steps} {studioEngineMode === "sdxl" ? "(DMD2)" : (sliders.steps <= 8 ? "(Turbo)" : "")}</span>
                  </div>
                  {#if studioEngineMode === "sdxl"}
                    <input
                      type="range"
                      min="4"
                      max="12"
                      step="1"
                      bind:value={sliders.steps}
                      on:input={() => { sliders = { ...sliders }; saveLocalSliders(sliders); }}
                      class="slider-input advanced-slider"
                    />
                    <div class="setting-subtext">
                      <span>4 ({$_("krea_steps_fast", {}, "Быстро")})</span>
                      <span>6 (DMD2)</span>
                      <span>8</span>
                      <span>12 (Max)</span>
                    </div>
                  {:else}
                    <input
                      type="range"
                      min="4"
                      max="50"
                      step="1"
                      bind:value={sliders.steps}
                      on:input={() => { sliders = { ...sliders }; saveLocalSliders(sliders); }}
                      class="slider-input advanced-slider"
                    />
                    <div class="setting-subtext">
                      <span>4 ({$_("krea_steps_fast", {}, "Быстро")})</span>
                      <span>14 ({$_("krea_steps_quality", {}, "Качество")})</span>
                      <span>25 (Anima)</span>
                      <span>40 (Max)</span>
                    </div>
                  {/if}
                </div>

                <!-- Setting 3: CFG Scale -->
                <div class="advanced-setting-row">
                  <div class="setting-title-row">
                    <span class="setting-name">{$_("krea_cfg", {}, "Сила промпта (CFG Scale):")}</span>
                    <span class="setting-val-badge">{sliders.cfg}</span>
                  </div>
                  {#if studioEngineMode === "sdxl"}
                    <input
                      type="range"
                      min="1.0"
                      max="2.2"
                      step="0.1"
                      bind:value={sliders.cfg}
                      on:input={() => { sliders = { ...sliders }; saveLocalSliders(sliders); }}
                      class="slider-input advanced-slider"
                    />
                    <div class="setting-subtext">
                      <span>1.0 (DMD2)</span>
                      <span>1.5</span>
                      <span>2.0 (Max)</span>
                    </div>
                  {:else}
                    <input
                      type="range"
                      min="1.0"
                      max="7.0"
                      step="0.1"
                      bind:value={sliders.cfg}
                      on:input={() => { sliders = { ...sliders }; saveLocalSliders(sliders); }}
                      class="slider-input advanced-slider"
                    />
                    <div class="setting-subtext">
                      <span>1.0 ({$_("krea_cfg_soft", {}, "Мягко")})</span>
                      <span>1.5 ({$_("krea_realism", {}, "Реализм")})</span>
                      <span>2.0 ({$_("krea_semi_realism", {}, "Полуреализм")})</span>
                      <span>3.0 ({$_("krea_anima", {}, "Анима")})</span>
                    </div>
                  {/if}
                </div>
              </div>

              <div class="popup-menu-footer">
                <span class="popup-count-hint">
                  {$_("krea_model_label", {}, "Модель:")}
                  {#if sliders.base_model === "sdxl_realistic"}
                    Realistic (DMD2)
                  {:else if sliders.base_model === "sdxl_perfect"}
                    Perfect (DMD2)
                  {:else if sliders.base_model === "sdxl_fantasy"}
                    Fantasy (DMD2)
                  {:else if sliders.base_model === "sdxl_tooned"}
                    Tooned (DMD2)
                  {:else if sliders.base_model === "anima"}
                    {$_("krea_anima", {}, "Анима")}
                  {:else if sliders.base_model === "semi_realism"}
                    {$_("krea_semi_realism", {}, "Полуреализм")}
                  {:else}
                    {$_("krea_realism", {}, "Реализм")}
                  {/if}
                </span>
                <button
                  type="button"
                  class="popup-done-btn"
                  on:click={closeAdvancedSettings}
                >
                  {$_("krea_done", {}, "Готово")}
                </button>
              </div>
            </div>
          {/if}
        </div>
      </div>
    </div>

    <!-- Sliders and Style Presets Panel -->
    {#if showSlidersPanel}
      <div class="sliders-panel" class:video-sliders-panel={studioEngineMode === "video"} in:fade={{ duration: 120 }}>
        {#if studioEngineMode === "video"}
          <!-- Video Card 1: Quality & Turbo Steps -->
          <div class="param-card model-card">
            <div class="model-pill-control">
              <div class="model-pill-header">
                <span>{$_("krea_video_engine", {}, "Движок и качество")}</span>
                <span class="model-pill-hint">MiniMax-H3 · 🔊 Stereo</span>
              </div>
              <div class="pill-toggle-wrapper">
                <button
                  type="button"
                  class="pill-btn"
                  class:active={videoQuality === "fast"}
                  on:click={() => setVideoQuality("fast")}
                  title={$_("krea_video_fast_hint", {}, "Fast 480p (4 шага LightX2V Turbo) — быстрый черновой рендер со стереозвуком")}
                >
                  <span class="pill-icon">⚡</span>
                  <span>Fast 480p</span>
                </button>
                <button
                  type="button"
                  class="pill-btn"
                  class:active={videoQuality === "hd"}
                  on:click={() => setVideoQuality("hd")}
                  title={$_("krea_video_hd_hint", {}, "HD 768p (8 шагов LightX2V) — кинематографичная чёткость и синхронный стереозвук")}
                >
                  <span class="pill-icon">🎬</span>
                  <span>HD 768p</span>
                </button>
              </div>
            </div>
          </div>

          <!-- Video Card 2: Duration (5s / 8s / 10s / 15s @ 24fps + Continuity) -->
          <div class="param-card video-duration-card">
            <div class="model-pill-control">
              <div class="model-pill-header">
                <span>{$_("krea_video_duration", {}, "Длительность")}</span>
                <span class="model-pill-hint">
                  {#if videoDuration === 15}
                    362 к. · 3 сегм.
                  {:else if videoDuration === 10}
                    243 к. · 2 сегм.
                  {:else if videoDuration === 8}
                    192 к. · 24fps
                  {:else}
                    124 к. · 24fps
                  {/if}
                </span>
              </div>
              <div class="pill-toggle-wrapper">
                <button
                  type="button"
                  class="pill-btn"
                  class:active={videoDuration === 5}
                  on:click={() => setVideoDuration(5)}
                  title={$_("krea_video_5s_hint", {}, "5 секунд (124 кадра @ 24fps + стереозвук)")}
                >
                  <span>5 {$_("krea_sec_short", {}, "с")}</span>
                </button>
                <button
                  type="button"
                  class="pill-btn"
                  class:active={videoDuration === 8}
                  on:click={() => setVideoDuration(8)}
                  title={$_("krea_video_8s_hint", {}, "8 секунд (192 кадра @ 24fps + расширенная стереосцена)")}
                >
                  <span>8 {$_("krea_sec_short", {}, "с")}</span>
                </button>
                <button
                  type="button"
                  class="pill-btn"
                  class:active={videoDuration === 10}
                  on:click={() => setVideoDuration(10)}
                  title={$_("krea_video_10s_hint", {}, "10 секунд (243 кадра — бесшовная склейка 2 сегментов в AV-латентах)")}
                >
                  <span>10 {$_("krea_sec_short", {}, "с")}</span>
                </button>
                <button
                  type="button"
                  class="pill-btn"
                  class:active={videoDuration === 15}
                  on:click={() => setVideoDuration(15)}
                  title={$_("krea_video_15s_hint", {}, "15 секунд (362 кадра — бесшовная склейка 3 сегментов в AV-латентах с удержанием лица)")}
                >
                  <span>15 {$_("krea_sec_short", {}, "с")}</span>
                </button>
              </div>
            </div>
          </div>

          <!-- Video Card 3: Video Aspect Ratio -->
          <div class="param-card aspect-settings-card">
            <div class="card-header-row">
              <span class="select-label">{$_("krea_video_aspect", {}, "Формат видео:")}</span>
            </div>
            <div class="aspect-chips">
              <button
                type="button"
                class="aspect-chip"
                class:active={videoAspect === "768x1344"}
                on:click={() => setVideoAspect("768x1344")}
                title={$_("krea_video_aspect_9_16", {}, "9:16 Вертикальное / Reels / Shorts")}
              >
                📱 9:16
              </button>
              <button
                type="button"
                class="aspect-chip"
                class:active={videoAspect === "896x1152"}
                on:click={() => setVideoAspect("896x1152")}
                title={$_("krea_video_aspect_3_4", {}, "3:4 Портрет (родной формат Studio)")}
              >
                🖼 3:4
              </button>
              <button
                type="button"
                class="aspect-chip"
                class:active={videoAspect === "1024x1024"}
                on:click={() => setVideoAspect("1024x1024")}
                title={$_("krea_video_aspect_1_1", {}, "1:1 Квадратное видео")}
              >
                ⏹ 1:1
              </button>
              <button
                type="button"
                class="aspect-chip"
                class:active={videoAspect === "1152x896"}
                on:click={() => setVideoAspect("1152x896")}
                title={$_("krea_video_aspect_4_3", {}, "4:3 Альбомное (родной формат Studio)")}
              >
                🖥 4:3
              </button>
              <button
                type="button"
                class="aspect-chip"
                class:active={videoAspect === "1344x768"}
                on:click={() => setVideoAspect("1344x768")}
                title={$_("krea_video_aspect_16_9", {}, "16:9 Киноширокоформатное")}
              >
                🎬 16:9
              </button>
            </div>
          </div>
        {:else}
        <!-- Card 1: Base Model (Mode-dependent: SDXL Express vs DiT Studio) -->
        <div class="param-card model-card">
          <div class="model-pill-control">
            <div class="model-pill-header">
              <span>{$_("krea_base_model", {}, "Базовая модель")}</span>
              <span class="model-pill-hint">
                {#if sliders.base_model === "sdxl_realistic"}
                  CyberRealistic XL
                {:else if sliders.base_model === "sdxl_perfect"}
                  CyberPony XL
                {:else if sliders.base_model === "sdxl_fantasy"}
                  Illustrious XL
                {:else if sliders.base_model === "sdxl_tooned"}
                  Pony Semi-Real
                {:else if sliders.base_model === "anima"}
                  Anima V1.1
                {:else if sliders.base_model === "semi_realism"}
                  Muse Yogi
                {:else}
                  CyberReal Krea 2
                {/if}
              </span>
            </div>
            <div class="pill-toggle-wrapper">
              {#if studioEngineMode === "sdxl"}
                <button
                  type="button"
                  class="pill-btn"
                  class:active={sliders.base_model === "sdxl_realistic" || sliders.base_model === "sdxl"}
                  on:click={() => setBaseModel("sdxl_realistic")}
                  title={$_("sdxl_realistic_title", {}, "Realistic (CyberRealistic XL) — candid photography, natural light & skin texture")}
                >
                  <span class="pill-icon">📸</span>
                  <span>Realistic</span>
                </button>
                <button
                  type="button"
                  class="pill-btn"
                  class:active={sliders.base_model === "sdxl_perfect"}
                  on:click={() => setBaseModel("sdxl_perfect")}
                  title={$_("sdxl_perfect_title", {}, "Perfect (CyberPony XL) — expressive anatomy, vivid looks & dynamic angles")}
                >
                  <span class="pill-icon">💎</span>
                  <span>Perfect</span>
                </button>
                <button
                  type="button"
                  class="pill-btn"
                  class:active={sliders.base_model === "sdxl_fantasy"}
                  on:click={() => setBaseModel("sdxl_fantasy")}
                  title={$_("sdxl_fantasy_title", {}, "Fantasy (CyberIllustrious XL) — fantasy, anime & painterly illustration")}
                >
                  <span class="pill-icon">🔮</span>
                  <span>Fantasy</span>
                </button>
                <button
                  type="button"
                  class="pill-btn"
                  class:active={sliders.base_model === "sdxl_tooned"}
                  on:click={() => setBaseModel("sdxl_tooned")}
                  title={$_("sdxl_tooned_title", {}, "Tooned (CyberPony Semi-Real) — stylized 2.5D semi-realism")}
                >
                  <span class="pill-icon">🎨</span>
                  <span>Tooned</span>
                </button>
              {:else}
                <button
                  type="button"
                  class="pill-btn"
                  class:active={sliders.base_model === "realism"}
                  class:locked-pill={statusLoadedOnce && !kreaAllowed}
                  on:click={() => setBaseModel("realism")}
                  title={$_("krea_realism_title", {}, "CyberRealistic Krea 2 — бескомпромиссный фотореализм")}
                >
                  <span class="pill-icon">{statusLoadedOnce && !kreaAllowed ? "🔒" : "📸"}</span>
                  <span>{$_("krea_realism", {}, "Реализм")}</span>
                </button>
                <button
                  type="button"
                  class="pill-btn"
                  class:active={sliders.base_model === "semi_realism"}
                  class:locked-pill={statusLoadedOnce && !kreaAllowed}
                  on:click={() => setBaseModel("semi_realism")}
                  title={$_("krea_semi_realism_title", {}, "Muse by Stable Yogi — 2.5D / Semi-realism, мягкий киношный свет и комиксы")}
                >
                  <span class="pill-icon">{statusLoadedOnce && !kreaAllowed ? "🔒" : "🎨"}</span>
                  <span>{$_("krea_semi_realism", {}, "Полуреализм")}</span>
                </button>
                <button
                  type="button"
                  class="pill-btn"
                  class:active={sliders.base_model === "anima"}
                  class:locked-pill={statusLoadedOnce && !kreaAllowed}
                  on:click={() => setBaseModel("anima")}
                  title={$_("krea_anima_title", {}, "Anima Aesthetic V1.1 — аутентичный аниме и традиционный 2D арт от CircleStone Labs & Comfy Org")}
                >
                  <span class="pill-icon">{statusLoadedOnce && !kreaAllowed ? "🔒" : "🌸"}</span>
                  <span>{$_("krea_anima", {}, "Анима")}</span>
                </button>
              {/if}
            </div>
          </div>
        </div>

        <!-- Card 2: Art Style (Multi-select with inline LoRA sliders) -->
        <div class="param-card style-card">
          <div class="dropdown-control style-dropdown-control" class:active={showStyleDropdown}>
            <span class="select-label">{$_("krea_art_style", {}, "Стиль арта:")}</span>
            <button
              type="button"
              class="dropdown-trigger-btn"
              class:has-style={(sliders.styles || []).length > 0}
              class:active={showStyleDropdown}
              on:click={(e) => {
                e.stopPropagation();
                showStyleDropdown = !showStyleDropdown;
                if (showStyleDropdown) showArchetypeDropdown = false;
              }}
            >
              <span class="trigger-label">{activeStyleLabel}</span>
              <svg class="chevron" class:open={showStyleDropdown} viewBox="0 0 20 20" fill="currentColor" width="13" height="13">
                <path fill-rule="evenodd" d="M5.293 7.293a1 1 0 011.414 0L10 10.586l3.293-3.293a1 1 0 111.414 1.414l-4 4a1 1 0 01-1.414 0l-4-4a1 1 0 010-1.414z" clip-rule="evenodd"/>
              </svg>
            </button>

            {#if showStyleDropdown}
              <!-- svelte-ignore a11y-click-events-have-key-events -->
              <!-- svelte-ignore a11y-no-static-element-interactions -->
              <div class="dropdown-popup-menu style-popup" transition:fade={{ duration: 100 }} on:click|stopPropagation>
                <div class="popup-menu-header">
                  <span>{$_("krea_image_style", {}, "Стили изображения / LoRA")}</span>
                  {#if (sliders.styles || []).length > 0}
                    <button
                      type="button"
                      class="popup-reset-link"
                      on:click={() => { sliders.styles = []; sliders.style = "none"; sliders = { ...sliders }; saveLocalSliders(sliders); }}
                    >
                      {$_("krea_reset", {}, "Сбросить")}
                    </button>
                  {/if}
                </div>
                <div class="popup-options-list">
                  <button
                    type="button"
                    class="popup-option-item"
                    class:selected={(sliders.styles || []).length === 0}
                    on:click={() => { sliders.styles = []; sliders.style = "none"; sliders = { ...sliders }; saveLocalSliders(sliders); }}
                  >
                    <div class="option-check-icon radio" class:checked={(sliders.styles || []).length === 0}>
                      {#if (sliders.styles || []).length === 0}
                        <svg class="check-svg" viewBox="0 0 24 24" width="11" height="11">
                          <path fill="currentColor" d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/>
                        </svg>
                      {/if}
                    </div>
                    <div class="option-main">
                      <span class="option-label">✨ {$_("krea_free_style", {}, "Свободный")}</span>
                      <span class="option-desc">{$_("krea_free_style_desc", {}, "Без LoRA (управляется текстом промпта)")}</span>
                    </div>
                  </button>

                  {#each stylePresets as sp}
                    {#if sp.id !== "none"}
                      {@const isChecked = (sliders.styles || []).includes(sp.id)}
                      <div class="popup-option-card" class:selected={isChecked}>
                        <button
                          type="button"
                          class="popup-option-row"
                          on:click={() => toggleStyle(sp.id, false)}
                        >
                          <!-- svelte-ignore a11y-click-events-have-key-events -->
                          <!-- svelte-ignore a11y-no-static-element-interactions -->
                          <div
                            class="option-check-icon checkbox"
                            class:checked={isChecked}
                            title={$_("krea_multiselect_tooltip", {}, "Мультивыбор (добавить/убрать)")}
                            on:click|stopPropagation={() => toggleStyle(sp.id, true)}
                          >
                            {#if isChecked}
                              <svg class="check-svg" viewBox="0 0 24 24" width="11" height="11">
                                <path fill="currentColor" d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/>
                              </svg>
                            {/if}
                          </div>
                          <div class="option-main">
                            <span class="option-label">{sp.label}</span>
                            <span class="option-desc">{sp.desc}</span>
                          </div>
                        </button>
                        {#if isChecked && sp.lora}
                          <!-- svelte-ignore a11y-click-events-have-key-events -->
                          <!-- svelte-ignore a11y-no-static-element-interactions -->
                          <div class="option-inline-slider-row" on:click|stopPropagation on:mousedown|stopPropagation>
                            <input
                              type="range"
                              min="0.1"
                              max="1.0"
                              step="0.05"
                              value={sliders.style_weights?.[sp.id] ?? sp.strength ?? 0.8}
                              on:input={(e) => updateStyleWeight(sp.id, parseFloat(e.target.value))}
                              class="inline-weight-slider"
                            />
                            <span class="inline-weight-val">
                              {sliders.style_weights?.[sp.id] ?? sp.strength ?? 0.8}
                            </span>
                          </div>
                        {/if}
                      </div>
                    {/if}
                  {/each}
                </div>
                <div class="popup-menu-footer">
                  <span class="popup-count-hint">{$_("krea_selected_count", { count: (sliders.styles || []).length }, `Выбрано: ${(sliders.styles || []).length}`)}</span>
                  <button
                    type="button"
                    class="popup-done-btn"
                    on:click={closeStyleDropdown}
                  >
                    {$_("krea_done", {}, "Готово")}
                  </button>
                </div>
              </div>
            {/if}
          </div>
        </div>

        <!-- Card 3: Archetype / Character LoRA (Multi-select with inline LoRA sliders) -->
        <div class="param-card archetype-card">
          <div class="dropdown-control archetype-dropdown-control" class:active={showArchetypeDropdown}>
            <span class="select-label">{$_("krea_archetype_lora", {}, "Типаж / LoRA:")}</span>
            <button
              type="button"
              class="dropdown-trigger-btn"
              class:has-archetype={(sliders.archetypes || []).length > 0}
              class:active={showArchetypeDropdown}
              on:click={(e) => {
                e.stopPropagation();
                showArchetypeDropdown = !showArchetypeDropdown;
                if (showArchetypeDropdown) showStyleDropdown = false;
              }}
            >
              <span class="trigger-label">{activeArchetypeLabel}</span>
              <svg class="chevron" class:open={showArchetypeDropdown} viewBox="0 0 20 20" fill="currentColor" width="13" height="13">
                <path fill-rule="evenodd" d="M5.293 7.293a1 1 0 011.414 0L10 10.586l3.293-3.293a1 1 0 111.414 1.414l-4 4a1 1 0 01-1.414 0l-4-4a1 1 0 010-1.414z" clip-rule="evenodd"/>
              </svg>
            </button>

            {#if showArchetypeDropdown}
              <!-- svelte-ignore a11y-click-events-have-key-events -->
              <!-- svelte-ignore a11y-no-static-element-interactions -->
              <div class="dropdown-popup-menu archetype-popup" transition:fade={{ duration: 100 }} on:click|stopPropagation>
                <div class="popup-menu-header">
                  <span>{$_("krea_appearance_lora", {}, "Типажи внешности / LoRA")}</span>
                  {#if (sliders.archetypes || []).length > 0}
                    <button
                      type="button"
                      class="popup-reset-link"
                      on:click={() => { sliders.archetypes = []; sliders.archetype = null; sliders = { ...sliders }; saveLocalSliders(sliders); }}
                    >
                      {$_("krea_reset", {}, "Сбросить")}
                    </button>
                  {/if}
                </div>
                <div class="popup-options-list">
                  <button
                    type="button"
                    class="popup-option-item"
                    class:selected={(sliders.archetypes || []).length === 0}
                    on:click={() => { sliders.archetypes = []; sliders.archetype = null; sliders = { ...sliders }; saveLocalSliders(sliders); }}
                  >
                    <div class="option-check-icon radio" class:checked={(sliders.archetypes || []).length === 0}>
                      {#if (sliders.archetypes || []).length === 0}
                        <svg class="check-svg" viewBox="0 0 24 24" width="11" height="11">
                          <path fill="currentColor" d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/>
                        </svg>
                      {/if}
                    </div>
                    <div class="option-main">
                      <span class="option-label">✨ {$_("krea_free_style", {}, "Свободный")}</span>
                      <span class="option-desc">{$_("krea_free_style_desc", {}, "Без LoRA (управляется текстом промпта)")}</span>
                    </div>
                  </button>
                  {#each archetypePresets as ap}
                    {@const isChecked = (sliders.archetypes || []).includes(ap.id)}
                    <div class="popup-option-card archetype-item" class:selected={isChecked}>
                      <button
                        type="button"
                        class="popup-option-row"
                        on:click={() => toggleArchetype(ap.id, false)}
                      >
                        <!-- svelte-ignore a11y-click-events-have-key-events -->
                        <!-- svelte-ignore a11y-no-static-element-interactions -->
                        <div
                          class="option-check-icon checkbox"
                          class:checked={isChecked}
                          title={$_("krea_multiselect_tooltip", {}, "Мультивыбор (добавить/убрать)")}
                          on:click|stopPropagation={() => toggleArchetype(ap.id, true)}
                        >
                          {#if isChecked}
                            <svg class="check-svg" viewBox="0 0 24 24" width="11" height="11">
                              <path fill="currentColor" d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/>
                            </svg>
                          {/if}
                        </div>
                        <div class="option-main">
                          <span class="option-label">{ap.label}</span>
                          <span class="option-desc">{ap.desc}</span>
                        </div>
                      </button>
                      {#if isChecked && ap.lora}
                        <!-- svelte-ignore a11y-click-events-have-key-events -->
                        <!-- svelte-ignore a11y-no-static-element-interactions -->
                        <div class="option-inline-slider-row" on:click|stopPropagation on:mousedown|stopPropagation>
                          <input
                            type="range"
                            min="0.1"
                            max="1.0"
                            step="0.05"
                            value={sliders.archetype_weights?.[ap.id] ?? ap.strength ?? 0.8}
                            on:input={(e) => updateArchetypeWeight(ap.id, parseFloat(e.target.value))}
                            class="inline-weight-slider"
                          />
                          <span class="inline-weight-val">
                            {sliders.archetype_weights?.[ap.id] ?? ap.strength ?? 0.8}
                          </span>
                        </div>
                      {/if}
                    </div>
                  {/each}
                </div>
                <div class="popup-menu-footer">
                  <span class="popup-count-hint">{$_("krea_selected_count", { count: (sliders.archetypes || []).length }, `Выбрано: ${(sliders.archetypes || []).length}`)}</span>
                  <button
                    type="button"
                    class="popup-done-btn"
                    on:click={closeArchetypeDropdown}
                  >
                    {$_("krea_done", {}, "Готово")}
                  </button>
                </div>
              </div>
            {/if}
          </div>
        </div>

        <!-- Card 4: Aspect Ratio -->
        <div class="param-card aspect-settings-card">
          <div class="card-header-row">
            <span class="select-label">{$_("krea_aspect_ratio", {}, "Формат:")}</span>
          </div>

          <div class="aspect-chips">
            <button
              type="button"
              class="aspect-chip"
              class:active={sliders.aspect === "896x1152"}
              on:click={() => setAspect("896x1152")}
              title={$_("krea_aspect_portrait", {}, "3:4 Портрет (896x1152)")}
            >
              📱 3:4
            </button>
            <button
              type="button"
              class="aspect-chip"
              class:active={sliders.aspect === "1024x1024"}
              on:click={() => setAspect("1024x1024")}
              title={$_("krea_aspect_square", {}, "1:1 Квадрат (1024x1024)")}
            >
              ⏹ 1:1
            </button>
            <button
              type="button"
              class="aspect-chip"
              class:active={sliders.aspect === "1152x896"}
              on:click={() => setAspect("1152x896")}
              title={$_("krea_aspect_landscape", {}, "4:3 Альбомный (1152x896)")}
            >
              🖥 4:3
            </button>
          </div>
        </div>
        {/if}
      </div>
    {/if}

    <!-- Prompts History Popup -->
    {#if showPromptsPopup}
      <!-- svelte-ignore a11y-click-events-have-key-events -->
      <!-- svelte-ignore a11y-no-noninteractive-element-interactions -->
      <div class="prompts-popup" role="dialog" tabindex="-1">
        <div class="prompts-popup-header">
          <div class="prompts-header-title">
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
              <circle cx="12" cy="12" r="10" />
              <polyline points="12 6 12 12 16 14" />
            </svg>
            <span>{$_("krea_prompt_history", {}, "История промптов")}</span>
          </div>
          <button
            type="button"
            class="prompts-popup-close"
            on:click={() => (showPromptsPopup = false)}
          >
            <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2.5">
              <line x1="18" y1="6" x2="6" y2="18" />
              <line x1="6" y1="6" x2="18" y2="18" />
            </svg>
          </button>
        </div>

        {#if recentPrompts.length > 5}
          <div class="prompts-search-wrap">
            <input
              type="text"
              class="prompts-search-input"
              placeholder={$_("krea_search_history", {}, "Поиск в истории...")}
              bind:value={promptsSearchQuery}
            />
          </div>
        {/if}

        <div class="prompts-popup-list">
          {#each filteredPrompts as p}
            <div class="prompt-item" on:click={() => selectPrompt(p)}>
              <div class="prompt-item-content">
                <div class="prompt-item-text">{p.text || p.prompt || p}</div>
                <div class="prompt-item-meta">
                  {#if p.style}
                    <span class="prompt-style-badge">{p.style}</span>
                  {/if}
                  <span class="prompt-time">{formatPromptTime(p.lastUsed || p.created || p.date)}</span>
                </div>
              </div>
              <button
                type="button"
                class="prompt-delete-btn"
                title={$_("krea_persona_delete", {}, "Удалить")}
                on:click={(e) => handleDeletePrompt(e, p)}
              >
                <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                  <polyline points="3 6 5 6 21 6" />
                  <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                </svg>
              </button>
            </div>
          {:else}
            <div class="prompts-empty-state">
              <svg viewBox="0 0 24 24" width="28" height="28" fill="none" stroke="currentColor" stroke-width="1.5">
                <circle cx="12" cy="12" r="10" />
                <polyline points="12 6 12 12 16 14" />
              </svg>
              <span>{$_("krea_no_recent_prompts", {}, "Нет недавних промптов")}</span>
            </div>
          {/each}
        </div>
      </div>
    {/if}

    <!-- Active Modifiers & Prompt Preview Bar -->
    {#if selectedPersonas.length > 0 || activeStyleObjs.some(s => s.id !== "none") || activeArchetypeObjs.length > 0}
      <div class="active-modifiers-bar">
        {#each selectedPersonas as p (p.id)}
          <div class="active-pill persona-pill" in:fade={{ duration: 100 }} title={$_("krea_active_passport", {}, "Активный словесный паспорт")}>
            <span class="pill-icon">👤</span>
            <span class="pill-text">{p.name}</span>
            <button
              type="button"
              class="pill-remove-btn"
              title={$_("krea_clear_persona", { name: p.name }, `Сбросить ${p.name}`)}
              on:click|stopPropagation={() => removeSelectedPersona(p.id)}
            >✕</button>
          </div>
        {/each}

        {#each activeStyleObjs as sp (sp.id)}
          {#if sp.id !== "none"}
            <div class="active-pill style-pill" in:fade={{ duration: 100 }} title={sp.desc}>
              <span class="pill-text">{sp.label} {sliders.style_weights?.[sp.id] !== undefined ? `(${sliders.style_weights[sp.id]})` : (sp.strength ? `(${sp.strength})` : "")}</span>
              <button
                type="button"
                class="pill-remove-btn"
                title={$_("krea_remove_style", { name: sp.name || sp.label }, `Убрать стиль ${sp.label}`)}
                on:click|stopPropagation={() => toggleStyle(sp.id)}
              >✕</button>
            </div>
          {/if}
        {/each}

        {#each activeArchetypeObjs as ap (ap.id)}
          <div class="active-pill archetype-pill" in:fade={{ duration: 100 }} title={ap.desc}>
            <span class="pill-text">{ap.label} {sliders.archetype_weights?.[ap.id] !== undefined ? `(${sliders.archetype_weights[ap.id]})` : (ap.strength ? `(${ap.strength})` : "")}</span>
            <button
              type="button"
              class="pill-remove-btn"
              title={$_("krea_remove_archetype", { name: ap.name || ap.label }, `Сбросить типаж LoRA ${ap.label}`)}
              on:click|stopPropagation={() => toggleArchetype(ap.id)}
            >✕</button>
          </div>
        {/each}
      </div>
    {/if}

    {#if showPromptTokensPreview}
      <div class="tokens-preview-panel" transition:fly={{ y: 6, duration: 150 }}>
        <div class="tokens-preview-header">
          <span class="preview-title">⚡ {$_("krea_final_pipeline", {}, "Итоговый пайплайн:")}</span>
        </div>
        <div class="tokens-preview-body">
          {#if selectedPersonas.length > 0}
            {#each selectedPersonas as p}
              <div class="token-row">
                <span class="token-badge token-badge-persona">👤 {$_("krea_passport_badge", {}, "Паспорт")} ({p.name})</span>
                <span class="token-text">{p.prompt}</span>
              </div>
              {#if p.avatarData || p.avatar}
                <div class="token-row">
                  <span class="token-badge token-badge-ref">📸 One-Shot Face Ref ({p.name})</span>
                  <span class="token-text">Krea 2 Identity Edit (native dual-conditioning likeness active)</span>
                </div>
              {/if}
            {/each}
          {/if}
          {#if activeStyleTokens}
            <div class="token-row">
              <span class="token-badge token-badge-style">🎨 {$_("krea_style_badge", {}, "Стиль")} ({activeStyleLabel})</span>
              <span class="token-text">{activeStyleTokens}</span>
            </div>
          {/if}
          {#if activeArchetypeObjs.length > 0}
            {#each activeArchetypeObjs as ap}
              <div class="token-row">
                <span class="token-badge token-badge-archetype">🧬 LoRA ({ap.label})</span>
                <span class="token-text">{getArchetypeToken(ap.id)}</span>
              </div>
            {/each}
          {/if}
          <div class="token-row">
            <span class="token-badge token-badge-user">✍️ {$_("krea_prompt_badge", {}, "Сюжет")}</span>
            <span class="token-text">{promptText.trim() || $_("krea_prompt_empty_hint", {}, "(пусто — введите желаемое действие или локацию)")}</span>
          </div>
        </div>
      </div>
    {/if}

    {#if studioEngineMode === "video" && extendingVideoItem}
      <div class="attached-refs-bar extend-video-bar">
        <span class="refs-label">🎞 {$_("krea_extend_video_label", {}, "Продолжение (+5 сек):")}</span>
        <div class="refs-list">
          <div class="ref-chip extend-video-chip">
            <span class="ref-chip-idx">+5с</span>
            {#if extendingVideoItem.preview}
              <img src={extendingVideoItem.preview} alt="" class="ref-chip-img" />
            {/if}
            <span class="ref-chip-title">{extendingVideoItem.name}</span>
            <button
              type="button"
              class="ref-chip-del"
              title={$_("krea_cancel_extend_video", {}, "Отменить продолжение видео")}
              on:click|stopPropagation={clearExtendingVideo}
            >✕</button>
          </div>
        </div>
        <div class="sdxl-ref-controls">
          <span class="sdxl-duo-hint">
            {$_("krea_extend_video_hint", {}, "Хвост видео и звука бесшовно стыкуется со следующим действием")}
          </span>
        </div>
      </div>
    {/if}

    <!-- Attached Reference Images Bar -->
    {#if attachedReferences.length > 0}
      <div class="attached-refs-bar">
        <span class="refs-label">
          {studioEngineMode === "video"
            ? (videoRefMode === "subject"
                ? $_("krea_video_subjects_count", { count: attachedReferences.length }, `Персонажи Ref2VA (${attachedReferences.length}/2):`)
                : $_("krea_video_keyframes_count", { count: attachedReferences.length }, `Опорные кадры (${attachedReferences.length}/2):`))
            : $_("krea_refs_count", { count: attachedReferences.length }, `Референсы (${attachedReferences.length}/2):`)}
        </span>
        <div class="refs-list">
          {#each attachedReferences as ref, idx}
            {@const isInpaintSceneChip = studioEngineMode === "sdxl" && sdxlRefMode === "inpaint" && idx === 0}
            {@const isInactiveInpaintSecond = studioEngineMode === "sdxl" && sdxlRefMode === "inpaint" && idx === 1 && sdxlInpaintTarget !== "person"}
            {@const canTuneRef = studioEngineMode === "sdxl" && !isInactiveInpaintSecond}
            <div class="ref-chip-wrap">
              <!-- svelte-ignore a11y-click-events-have-key-events -->
              <!-- svelte-ignore a11y-no-static-element-interactions -->
              <div
                class="ref-chip"
                class:interactive={canTuneRef}
                class:open={activeRefPopoverIdx === idx && canTuneRef}
                title={canTuneRef
                  ? $_("krea_ref_tune_hint", {}, "Нажмите, чтобы настроить силу сходства или перерисовки")
                  : (ref.name || $_("krea_ref_img", { idx: idx + 1 }, `Картинка ${idx + 1}`))}
                on:click|stopPropagation={() => { if (canTuneRef) toggleRefPopover(idx); }}
              >
                <span class="ref-chip-idx">
                  {studioEngineMode === "video"
                    ? (videoRefMode === "subject"
                        ? `#${idx + 1} ${$_("krea_subject_idx_short", {}, "Образ")}`
                        : (idx === 0
                            ? $_("krea_keyframe_idx_start", {}, "#1 Старт")
                            : $_("krea_keyframe_idx_end", {}, "#2 Финал")))
                    : `#${idx + 1}`}
                </span>
                <img src={ref.preview} alt={$_("krea_ref_img", { idx: idx + 1 }, `Картинка ${idx + 1}`)} class="ref-chip-img" />
                <span class="ref-chip-title">{ref.name || $_("krea_ref_img", { idx: idx + 1 }, `Картинка ${idx + 1}`)}</span>
                {#if canTuneRef}
                  <span class="ref-chip-weight">
                    {#if isInpaintSceneChip}
                      {(sliders.inpaint_denoise ?? 0.94).toFixed(2)}
                    {:else if attachedReferences.length >= 2 && sdxlRefMode === "faceswap"}
                      {(sliders.face_restore_visibility ?? 0.30) <= 0.02
                        ? $_("krea_ref_raw_short", {}, "raw")
                        : (sliders.face_restore_visibility ?? 0.30).toFixed(2)}
                    {:else}
                      {(sliders.ipadapter_weight ?? 0.65) <= 0.02
                        ? $_("krea_ref_face_only_short", {}, "лицо")
                        : (sliders.ipadapter_weight ?? 0.65).toFixed(2)}
                    {/if}
                  </span>
                {/if}
                <button
                  type="button"
                  class="ref-chip-del"
                  title={$_("krea_delete_ref", {}, "Удалить референс")}
                  on:click|stopPropagation={() => removeReference(idx)}
                >✕</button>
              </div>

              {#if activeRefPopoverIdx === idx && canTuneRef}
                <!-- svelte-ignore a11y-click-events-have-key-events -->
                <!-- svelte-ignore a11y-no-static-element-interactions -->
                <div class="ref-weight-popover" on:click|stopPropagation>
                  <div class="ref-popover-header">
                    <span class="ref-popover-title">
                      {isInpaintSceneChip
                        ? $_("krea_ref_popover_inpaint_title", {}, "Сила ретуши")
                        : $_("krea_ref_popover_face_title", {}, "Перенос образа")}
                    </span>
                    <div class="ref-popover-actions">
                      {#if isInpaintSceneChip ? Math.abs((sliders.inpaint_denoise ?? 0.94) - 0.94) > 0.005 : (Math.abs((sliders.ipadapter_weight ?? 0.65) - 0.65) > 0.005 || Math.abs((sliders.face_restore_visibility ?? 0.30) - 0.30) > 0.005)}
                        <button
                          type="button"
                          class="ref-popover-reset"
                          on:click={() => resetRefStrengths(isInpaintSceneChip ? "inpaint" : "face")}
                        >{$_("krea_ref_reset_btn", {}, "Сброс")}</button>
                      {/if}
                      <button
                        type="button"
                        class="ref-popover-close"
                        aria-label="Close"
                        on:click={() => (activeRefPopoverIdx = null)}
                      >✕</button>
                    </div>
                  </div>

                  {#if isInpaintSceneChip}
                    <div class="ref-popover-row">
                      <div class="ref-popover-label-row">
                        <span class="setting-name">{$_("krea_ref_inpaint_denoise_label", {}, "Сила перерисовки")}</span>
                        <span class="setting-val-badge">{(sliders.inpaint_denoise ?? 0.94).toFixed(2)}</span>
                      </div>
                      <input
                        type="range"
                        min="0.50"
                        max="0.98"
                        step="0.02"
                        class="advanced-slider"
                        value={sliders.inpaint_denoise ?? 0.94}
                        on:input={(e) => { sliders = { ...sliders, inpaint_denoise: parseFloat(e.currentTarget.value) }; }}
                      />
                      <div class="setting-subtext">
                        {$_("krea_ref_inpaint_denoise_hint", {}, "0.55 — деликатная правка ткани и света · 0.94 — полная замена одежды или фона")}
                      </div>
                    </div>
                  {:else}
                    {#if !(attachedReferences.length >= 2 && sdxlRefMode === "faceswap")}
                      <div class="ref-popover-row">
                        <div class="ref-popover-label-row">
                          <span class="setting-name">{$_("krea_ref_ipa_weight_label", {}, "Образ и причёска")}</span>
                          <span class="setting-val-badge">
                            {(sliders.ipadapter_weight ?? 0.65) <= 0.02
                              ? $_("krea_ref_face_only_badge", {}, "Только лицо")
                              : (sliders.ipadapter_weight ?? 0.65).toFixed(2)}
                          </span>
                        </div>
                        <input
                          type="range"
                          min="0"
                          max="0.95"
                          step="0.05"
                          class="advanced-slider"
                          value={sliders.ipadapter_weight ?? 0.65}
                          on:input={(e) => { sliders = { ...sliders, ipadapter_weight: parseFloat(e.currentTarget.value) }; }}
                        />
                        <div class="setting-subtext">
                          {$_("krea_ref_ipa_weight_hint", {}, "0 — свободная смена волос по промпту · 0.65 — баланс · 0.90 — макс. сходство с фото")}
                        </div>
                      </div>
                    {/if}

                    <div class="ref-popover-row">
                      <div class="ref-popover-label-row">
                        <span class="setting-name">{$_("krea_ref_restore_label", {}, "Сглаживание кожи")}</span>
                        <span class="setting-val-badge">
                          {(sliders.face_restore_visibility ?? 0.30) <= 0.02
                            ? $_("krea_ref_off_badge", {}, "Выкл")
                            : (sliders.face_restore_visibility ?? 0.30).toFixed(2)}
                        </span>
                      </div>
                      <input
                        type="range"
                        min="0"
                        max="0.85"
                        step="0.05"
                        class="advanced-slider"
                        value={sliders.face_restore_visibility ?? 0.30}
                        on:input={(e) => { sliders = { ...sliders, face_restore_visibility: parseFloat(e.currentTarget.value) }; }}
                      />
                      <div class="setting-subtext">
                        {$_("krea_ref_restore_hint", {}, "Ниже (0.15–0.30) — точнее живые черты · Выше — бьюти-фильтр для размытых исходников")}
                      </div>
                    </div>
                  {/if}
                </div>
              {/if}
            </div>
            {#if idx === 0 && attachedReferences.length >= 2}
              <button
                type="button"
                class="swap-refs-btn"
                title={studioEngineMode === "video"
                  ? $_("krea_swap_keyframes_hint", {}, "Поменять местами #1 Старт ↔ #2 Финал")
                  : $_("krea_swap_refs_hint", {}, "Поменять местами референсы #1 ↔ #2")}
                on:click={swapReferences}
              >⇄</button>
            {/if}
          {/each}
          {#if attachedReferences.length < 2}
            <button
              type="button"
              class="add-ref-mini-btn"
              title={studioEngineMode === "video" ? $_("krea_add_end_keyframe", {}, "Загрузить файл или кликните 🎬 на второй карточке в галерее") : $_("krea_add_more_refs", {}, "Добавить ещё референс")}
              on:click={() => refFileInputEl?.click()}
            >
              {studioEngineMode === "video" ? $_("krea_add_end_frame_btn", {}, "+ Финальный кадр") : $_("krea_add_ref_btn", {}, "+ Ещё картинка")}
            </button>
          {/if}
        </div>
        {#if studioEngineMode === "video"}
          <div class="sdxl-ref-controls">
            <div class="sdxl-ref-mode-group" role="tablist" aria-label="Video reference mode">
              <button
                type="button"
                role="tab"
                aria-selected={videoRefMode === "keyframe"}
                class="sdxl-mode-pill"
                class:active={videoRefMode === "keyframe"}
                title={$_("krea_video_mode_keyframe_hint", {}, "FL2VA: #1 становится точным стартовым кадром видео (и #2 — финальным кадром перехода)")}
                on:click={() => setVideoRefMode("keyframe")}
              >
                <span>🎬 {$_("krea_video_mode_keyframe", {}, "Кадр")}</span>
              </button>
              <button
                type="button"
                role="tab"
                aria-selected={videoRefMode === "subject"}
                class="sdxl-mode-pill"
                class:active={videoRefMode === "subject"}
                title={$_("krea_video_mode_subject_hint", {}, "Ref2VA: переносит точное лицо и внешность персонажа в любую новую позу, ракурс и сцену")}
                on:click={() => setVideoRefMode("subject")}
              >
                <span>👤 {$_("krea_video_mode_subject", {}, "Персонаж")}</span>
              </button>
            </div>
            <span class="sdxl-duo-hint">
              {videoRefMode === "subject"
                ? $_("krea_video_ref2va_badge", {}, "👤 Свободная сцена с сохранением лица (<Picture 1>)")
                : (attachedReferences.length >= 2
                    ? $_("krea_video_fl2va_badge", {}, "🎬 #1 Старт (<Picture 1>) → #2 Финал (<Picture 2>)")
                    : $_("krea_video_i2va_badge_pick2", {}, "🎬 #1 Старт · кликните 🎬 на 2-й карточке для #2 Финал"))}
            </span>
          </div>
        {:else if studioEngineMode === "sdxl"}
          <div class="sdxl-ref-controls">
            <div class="sdxl-ref-mode-group" role="tablist" aria-label={$_("krea_sdxl_ref_mode_aria", {}, "Reference mode")}>
              <button
                type="button"
                role="tab"
                aria-selected={sdxlRefMode === "faceswap"}
                class="sdxl-mode-pill"
                class:active={sdxlRefMode === "faceswap"}
                title={$_("krea_sdxl_mode_face_hint", {}, "Transfer face from reference into a brand-new scene (ReActor HyperSwap 256)")}
                on:click={() => (sdxlRefMode = "faceswap")}
              >
                <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
                  <circle cx="12" cy="8" r="4" />
                  <path d="M5 20a7 7 0 0 1 14 0" />
                </svg>
                <span>{$_("krea_sdxl_mode_face", {}, "Face")}</span>
              </button>
              <button
                type="button"
                role="tab"
                aria-selected={sdxlRefMode === "inpaint"}
                class="sdxl-mode-pill"
                class:active={sdxlRefMode === "inpaint"}
                title={$_("krea_sdxl_mode_inpaint_hint", {}, "Auto-Inpaint: replace outfit, background, or person on photo #1 while preserving unmasked pixels")}
                on:click={() => (sdxlRefMode = "inpaint")}
              >
                <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
                  <path d="M15 4V2" /><path d="M15 16v-2" /><path d="M8 9h2" /><path d="M20 9h2" />
                  <path d="M17.8 11.8 19 13" /><path d="M15 9h.01" /><path d="M17.8 6.2 19 5" />
                  <path d="m3 21 9-9" /><path d="M12.2 6.2 11 5" />
                </svg>
                <span>{$_("krea_sdxl_mode_inpaint", {}, "Retouch")}</span>
              </button>
            </div>
            {#if sdxlRefMode === "inpaint"}
              <div class="sdxl-face-target-group" role="group" aria-label={$_("krea_sdxl_inpaint_target_aria", {}, "Retouch target")}>
                <button
                  type="button"
                  class="sdxl-target-pill"
                  class:active={sdxlInpaintTarget === "outfit"}
                  title={$_("krea_sdxl_inpaint_outfit_hint", {}, "Replace clothing on photo #1 — face, hair, hands, and background stay untouched")}
                  on:click={() => (sdxlInpaintTarget = "outfit")}
                >{$_("krea_sdxl_inpaint_outfit", {}, "Outfit")}</button>
                <button
                  type="button"
                  class="sdxl-target-pill"
                  class:active={sdxlInpaintTarget === "background"}
                  title={$_("krea_sdxl_inpaint_bg_hint", {}, "Replace background & environment around the person on photo #1")}
                  on:click={() => (sdxlInpaintTarget = "background")}
                >{$_("krea_sdxl_inpaint_bg", {}, "Background")}</button>
                <button
                  type="button"
                  class="sdxl-target-pill"
                  class:active={sdxlInpaintTarget === "person"}
                  title={$_("krea_sdxl_inpaint_person_hint", {}, "Replace the foreground person on photo #1 while keeping the background (photo #2 swaps face)")}
                  on:click={() => (sdxlInpaintTarget = "person")}
                >{$_("krea_sdxl_inpaint_person", {}, "Person")}</button>
              </div>
              {#if sdxlInpaintTarget === "person" && attachedReferences.length >= 2}
                <span class="sdxl-duo-hint" title={$_("krea_sdxl_inpaint_duo_hint_title", {}, "Photo #1 provides the scene & background, photo #2 transfers the face")}>
                  {$_("krea_sdxl_inpaint_duo_hint", {}, "#1 scene · #2 face")}
                </span>
              {/if}
            {:else if attachedReferences.length === 1}
              <div class="sdxl-face-target-group" role="group" aria-label={$_("krea_sdxl_face_target_aria", {}, "Target face in frame")}>
                <button
                  type="button"
                  class="sdxl-target-pill"
                  class:active={sdxlFaceTarget === "auto"}
                  title={$_("krea_sdxl_face_auto_hint", {}, "Primary (largest) face in frame")}
                  on:click={() => (sdxlFaceTarget = "auto")}
                >{$_("krea_sdxl_face_auto", {}, "Auto")}</button>
                <button
                  type="button"
                  class="sdxl-target-pill"
                  class:active={sdxlFaceTarget === "left"}
                  title={$_("krea_sdxl_face_left_hint", {}, "Left person (handy for duo scenes with a character)")}
                  on:click={() => (sdxlFaceTarget = "left")}
                >{$_("krea_sdxl_face_left", {}, "Left")}</button>
                <button
                  type="button"
                  class="sdxl-target-pill"
                  class:active={sdxlFaceTarget === "right"}
                  title={$_("krea_sdxl_face_right_hint", {}, "Right person (handy for duo scenes with a character)")}
                  on:click={() => (sdxlFaceTarget = "right")}
                >{$_("krea_sdxl_face_right", {}, "Right")}</button>
                <button
                  type="button"
                  class="sdxl-target-pill"
                  class:active={sdxlFaceTarget === "second"}
                  title={$_("krea_sdxl_face_second_hint", {}, "Second largest face in frame")}
                  on:click={() => (sdxlFaceTarget = "second")}
                >{$_("krea_sdxl_face_second", {}, "2nd")}</button>
              </div>
            {:else if attachedReferences.length >= 2}
              <span class="sdxl-duo-hint" title={$_("krea_sdxl_duo_hint_title", {}, "In a duo scene #1 maps to the person on the left and #2 to the person on the right")}>
                {$_("krea_sdxl_duo_hint", {}, "#1 → left · #2 → right")}
              </span>
            {/if}
          </div>
        {/if}
      </div>
    {/if}

    <!-- Prompt Textarea and Actions -->
    <div class="composer-input-row">
      <textarea
        bind:this={textareaEl}
        bind:value={promptText}
        placeholder={studioEngineMode === "video"
          ? (extendingVideoItem
              ? $_("krea_video_extend_placeholder", {}, "Опишите следующее действие (+5 сек) — или сразу нажмите Продлить для естественного продолжения...")
              : (attachedReferences.length > 0
                  ? (videoRefMode === "subject"
                      ? $_("krea_video_ref2va_placeholder", {}, "Опишите любую новую сцену, ракурс, диалог (<d>[Russian] ...</d>) и стереозвук для персонажа...")
                      : $_("krea_video_i2v_placeholder", {}, "Опишите движение <Picture 1>, камеру и стереозвук (Audio: ...) — или сразу нажмите Снять видео..."))
                  : $_("krea_video_t2v_placeholder", {}, "Опишите видеосцену, движение камеры и звук (Audio: ...) — или попросите Режиссёра 🎬...")))
          : (studioEngineMode === "sdxl" && attachedReferences.length > 0 && sdxlRefMode === "inpaint"
              ? (sdxlInpaintTarget === "background"
                ? $_("krea_sdxl_inpaint_bg_placeholder", {}, "Describe the new background & lighting (person and outfit stay intact)...")
                : (sdxlInpaintTarget === "person"
                  ? $_("krea_sdxl_inpaint_person_placeholder", {}, "Describe the new character in this scene...")
                  : $_("krea_sdxl_inpaint_outfit_placeholder", {}, "Describe the new outfit / look (face, hands & background stay intact)...")))
              : $_("krea_prompt_placeholder", {}, "Describe the image..."))}
        rows="1"
        class="prompt-textarea"
        on:input={resizeTextarea}
        on:keydown={(e) => {
          if (e.key === "Enter" && !e.shiftKey) {
            e.preventDefault();
            runGeneration();
          }
        }}
      ></textarea>

      <div class="composer-actions">
        <button
          class="generate-submit-btn"
          class:busy={isGenerating}
          disabled={isGenerating || (!promptText.trim() && selectedPersonas.length === 0 && !(studioEngineMode === "video" && (attachedReferences.length > 0 || extendingVideoItem)))}
          on:click={() => runGeneration()}
        >
          {#if isGenerating}
            <div class="spinner"></div>
            {#if generatingStage === "warming"}
              <span>{$_("krea_warming", {}, "Прогрев")} {generationElapsed}s</span>
            {:else if generatingStage === "saving"}
              <span>{$_("krea_saving", {}, "Сохранение")} {generationElapsed}s</span>
            {:else}
              <span>
                {$_("krea_generating", {}, "Генерация")}{#if generatingTotalSteps > 0 && generatingStep > 0}&nbsp;{generatingStep}/{generatingTotalSteps}{/if} {generationElapsed}s
              </span>
            {/if}
          {:else}
            <span class="btn-sparkle">{studioEngineMode === "video" ? (extendingVideoItem ? "➕" : "🎬") : "✨"}</span>
            <span>
              {studioEngineMode === "video"
                ? (extendingVideoItem
                    ? $_("krea_extend_submit_btn", {}, "Продлить +5 сек")
                    : $_("krea_generate_video_btn", {}, "Снять видео"))
                : $_("krea_generate_btn", {}, "Сгенерировать")}
            </span>
          {/if}
        </button>
      </div>
    </div>
  </footer>
  </div>

  <!-- 🎬 AI DIRECTOR'S CHAIR RIGHT PANEL -->
  {#if showDirectorPanel && canShowDirector}
    <aside
      class="director-panel"
      class:resizing={isResizingDirector}
      style={innerWidth > 960 ? `width: ${directorWidth}px; min-width: ${directorWidth}px; max-width: ${directorWidth}px;` : ""}
      transition:fly={{ x: 40, duration: 180 }}
    >
      <!-- svelte-ignore a11y-no-static-element-interactions -->
      <div
        class="director-resize-handle"
        class:active={isResizingDirector}
        title={$_("krea_dir_resize_hint", {}, "Drag edge to resize (double-click to reset)")}
        on:pointerdown={startDirectorResize}
        on:dblclick={resetDirectorWidth}
      ></div>
      <div class="director-header">
        <div class="director-title-group">
          <span class="director-icon">🎬</span>
          <div>
            <div class="director-title">{$_("krea_dir_title", {}, "Director's Chair")}</div>
            <div class="director-subtitle">{$_("krea_dir_subtitle", {}, "Scene staging & smart prompts")}</div>
          </div>
        </div>
        <button type="button" class="director-close-btn" title={$_("krea_dir_hide_panel", {}, "Hide panel")} on:click={toggleDirectorPanel}>✕</button>
      </div>

      <div class="director-mode-tabs">
        <button
          type="button"
          class="director-tab-btn"
          class:active={directorTab === "prompt"}
          on:click={() => { directorTab = "prompt"; scrollDirectorFeedToBottom(); }}
        >
          ✍️ {$_("krea_dir_tab_prompt", {}, "Prompt Co-Pilot")}
        </button>
        <button
          type="button"
          class="director-tab-btn"
          class:active={directorTab === "storyboard"}
          on:click={() => { directorTab = "storyboard"; loadSavedStories().catch(() => {}); }}
        >
          🎞️ {$_("krea_storyboard_tab", {}, "Storyboard")}
          {#if storyScenes.length > 0}
            <span class="director-tab-badge">{storyScenes.length}</span>
          {/if}
        </button>
      </div>

      {#if directorTab === "prompt"}
        <!-- TAB 1: PROMPT CO-PILOT -->
        <div class="director-context-bar">
          {#if selectedPersonas.length > 0}
            <span class="ctx-chip persona">👤 {selectedPersonas.map((p) => p.name).join(", ")}</span>
          {/if}
          <span class="ctx-chip muted">
            {promptText.trim() ? $_("krea_dir_char_count", { count: promptText.trim().length }, `${promptText.trim().length} chars`) : $_("krea_dir_empty_prompt", {}, "Empty prompt")}
          </span>
          {#if promptUndoStack.length > 0}
            <button type="button" class="ctx-undo-btn" title={$_("krea_dir_undo_hint", {}, "Restore previous prompt in composer")} on:click={undoDirectorPrompt}>
              ↶ {$_("krea_dir_undo", {}, "Undo")}
            </button>
          {/if}
        </div>

        <div class="director-quick-chips">
          <button type="button" class="quick-chip" on:click={() => applyDirectorQuickHint($_("krea_dir_hint_plot", {}, "Invent an atmospheric storyline and detailed scene tailored to the active model, style, and persona: "))}>✨ {$_("krea_dir_chip_plot", {}, "Invent plot")}</button>
          <button type="button" class="quick-chip" title={$_("krea_dir_chip_civitai_hint", {}, "Paste a CivitAI image link (.com / .red) + optional edit — model, LoRAs, and prompt will be imported automatically!")} on:click={() => applyDirectorQuickHint("https://civitai.red/images/ — ")}>🔗 {$_("krea_dir_chip_civitai", {}, "CivitAI link")}</button>
          <button type="button" class="quick-chip" on:click={() => applyDirectorQuickHint($_("krea_dir_hint_pose", {}, "Change pose and camera angle: make "))}>🧍‍♀️ {$_("krea_dir_chip_pose", {}, "Pose / angle")}</button>
          <button type="button" class="quick-chip" on:click={() => applyDirectorQuickHint($_("krea_dir_hint_outfit", {}, "Change the outfit to: "))}>👗 {$_("krea_dir_chip_outfit", {}, "Outfit")}</button>
          <button type="button" class="quick-chip" on:click={() => applyDirectorQuickHint($_("krea_dir_hint_setting", {}, "Move the scene to this location and lighting: "))}>🌆 {$_("krea_dir_chip_setting", {}, "Setting & light")}</button>
          <button type="button" class="quick-chip" on:click={() => applyDirectorQuickHint($_("krea_dir_hint_emotion", {}, "Add vivid emotion, playful gaze, and sensual charm while keeping the composition"))}>🔥 {$_("krea_dir_chip_emotion", {}, "Emotion / charm")}</button>
          <button type="button" class="quick-chip" on:click={() => applyDirectorQuickHint($_("krea_dir_hint_adapt", {}, "Translate into English and adapt the prompt structure ideally for the selected base model"))}>🎯 {$_("krea_dir_chip_adapt", {}, "Fit model")}</button>
        </div>

        <div class="director-feed" bind:this={directorFeedEl}>
          {#if directorMessages.length === 0}
            <div class="director-empty-hint">
              <div class="empty-clapper">🎬</div>
              <p><strong>{$_("krea_dir_empty_p1_bold", {}, "Drop a CivitAI link")}</strong> ({$_("krea_dir_empty_p1_eg", {}, "e.g.")} <code>civitai.red/images/...</code> + <em>{$_("krea_dir_empty_p1_ex", {}, "\"holding a sword\"")}</em>) — {$_("krea_dir_empty_p1_rest", {}, "the model, LoRA weights, and prompt will be pulled in automatically!")}</p>
              <p>{$_("krea_dir_empty_p2", {}, "Or describe an idea from scratch or tweak the composer prompt in plain words.")}</p>
            </div>
          {:else}
            {#each directorMessages as msg (msg.id)}
              <div class="director-msg {msg.role}" class:error={msg.isError}>
                <div class="msg-bubble">{msg.text}</div>
                {#if msg.role === "assistant" && msg.promptSnapshot}
                  <div class="msg-prompt-preview">
                    <div class="msg-prompt-text">{msg.promptSnapshot}</div>
                    <div class="msg-prompt-actions">
                      <button type="button" class="mini-act-btn" on:click={() => applyDirectorSnapshot(msg.promptSnapshot, msg.appliedSettings)}>
                        ✓ {$_("krea_dir_to_composer", {}, "To composer")}
                      </button>
                      <button type="button" class="mini-act-btn" on:click={() => copyPrompt(msg.promptSnapshot, msg.id)}>
                        {copiedPromptId === msg.id ? $_("krea_copied", {}, "Copied!") : $_("krea_copy", {}, "Copy")}
                      </button>
                    </div>
                  </div>
                {/if}
              </div>
            {/each}
          {/if}
          {#if isDirectorThinking}
            <div class="director-msg assistant">
              <div class="msg-bubble thinking-bubble">
                <span class="dot-pulse"></span> {isDirectorImportingCivitai ? $_("krea_dir_thinking_civitai", {}, "Extracting CivitAI preset and staging the scene...") : $_("krea_dir_thinking_rewrite", {}, "Director is rewriting the scene...")}
              </div>
            </div>
          {/if}
        </div>

        <div class="director-composer">
          <textarea
            bind:this={directorInputEl}
            bind:value={directorInput}
            rows="1"
            class="director-textarea"
            placeholder={$_("krea_dir_input_placeholder", {}, "CivitAI link + tweak or any scene idea...")}
            on:input={resizeDirectorTextarea}
            on:keydown={(e) => {
              if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
                e.preventDefault();
                askDirectorPrompt(true);
              } else if (e.key === "Enter" && !e.shiftKey) {
                e.preventDefault();
                askDirectorPrompt(false);
              }
            }}
          ></textarea>
          <div class="director-composer-btns">
            {#if directorMessages.length > 0}
              <button
                type="button"
                class="dir-clear-btn"
                title={$_("krea_dir_clear_chat_hint", {}, "Clear chat with Director")}
                on:click={() => { directorMessages = []; try { sessionStorage.removeItem("krea_director_msgs_v1"); } catch (_) {} }}
              >
                {$_("krea_clear", {}, "Clear")}
              </button>
            {/if}
            <button
              type="button"
              class="dir-rewrite-btn"
              disabled={isDirectorThinking || !directorInput.trim()}
              on:click={() => askDirectorPrompt(false)}
              title={$_("krea_dir_update_prompt_hint", {}, "Build or update prompt in Studio (Enter)")}
            >
              ✍️ {$_("krea_dir_update_prompt", {}, "Update prompt")}
            </button>
            <button
              type="button"
              class="dir-shoot-now-btn"
              disabled={isDirectorThinking || isGenerating || !directorInput.trim()}
              on:click={() => askDirectorPrompt(true)}
              title={$_("krea_dir_shoot_now_hint", {}, "Update prompt and shoot immediately (Ctrl+Enter)")}
            >
              ⚡ {$_("krea_dir_shoot_now", {}, "Shoot")}
            </button>
          </div>
        </div>
      {:else}
        <!-- TAB 2: MULTI-SCENE STORYBOARD & COMICS -->
        <div class="storyboard-body">
          <!-- Saved Stories & New Story Bar -->
          <div class="story-projects-bar">
            <select
              class="story-select"
              value={storySlug}
              on:change={(e) => {
                const val = e.target.value;
                if (!val) return;
                const found = savedStoriesList.find((x) => x.story_slug === val);
                if (found) openSavedStory(found);
              }}
            >
              <option value="">📂 {$_("krea_story_saved_list", { count: savedStoriesList.length }, `Saved stories (${savedStoriesList.length})`)}</option>
              {#each savedStoriesList as st}
                <option value={st.story_slug}>{st.story_title} ({$_("krea_story_scenes_short", { count: st.scene_count || (st.story?.scenes?.length || 0) }, `${st.scene_count || (st.story?.scenes?.length || 0)} sc.`)})</option>
              {/each}
            </select>
            <button type="button" class="story-new-btn" title={$_("krea_story_new_hint", {}, "Start a brand-new story")} on:click={() => startFreshStory(false)}>
              {$_("krea_story_new_btn", {}, "+ New")}
            </button>
            {#if storySetting.trim() && storyScenes.length > 0}
              <button type="button" class="story-new-btn subtle" title={$_("krea_story_same_world_hint", {}, "New story in the same world/setting")} on:click={() => startFreshStory(true)}>
                {$_("krea_story_same_world_btn", {}, "Same world")}
              </button>
            {/if}
          </div>

          <!-- Collapsible Setting & Plot Card -->
          <div class="story-setup-card">
            <button type="button" class="setup-card-toggle" on:click={() => (isSetupCollapsed = !isSetupCollapsed)}>
              <span class="setup-toggle-title">
                🌍 {storyTitle.trim() || $_("krea_story_setup_header", {}, "Setting & Premise")}
                {#if storySlug}
                  <span class="slug-pill">/{storySlug}</span>
                {/if}
              </span>
              <span>{isSetupCollapsed ? "▼" : "▲"}</span>
            </button>

            {#if !isSetupCollapsed}
              <div class="setup-fields">
                <input
                  type="text"
                  class="dir-input"
                  placeholder={$_("krea_story_title_placeholder", {}, "Story title (or let the Director invent one)")}
                  bind:value={storyTitle}
                  on:input={saveActiveDraftStoryToLocal}
                />

                <div class="field-label-row">
                  <span>{$_("krea_story_setting_label", {}, "Setting / world & atmosphere:")}</span>
                  {#if storySetting.trim()}
                    <button type="button" class="text-link-btn" on:click={saveSettingPreset}>📌 {$_("krea_story_save_preset", {}, "Save preset")}</button>
                  {/if}
                </div>
                {#if savedSettingPresets.length > 0}
                  <div class="setting-presets-row">
                    {#each savedSettingPresets as sp (sp.id)}
                      <div class="preset-chip-wrap">
                        <button type="button" class="preset-chip" on:click={() => { storySetting = sp.setting; saveActiveDraftStoryToLocal(); }} title={sp.setting}>
                          {sp.label}
                        </button>
                        <button type="button" class="preset-chip-x" on:click={() => deleteSettingPreset(sp.id)}>✕</button>
                      </div>
                    {/each}
                  </div>
                {/if}
                <textarea
                  rows="2"
                  class="dir-textarea"
                  placeholder={$_("krea_story_setting_placeholder", {}, "E.g.: Rainy evening in a penthouse above neon Tokyo, warm fireplace glow...")}
                  bind:value={storySetting}
                  on:input={saveActiveDraftStoryToLocal}
                ></textarea>

                <div class="field-label-row">
                  <span>{storyScenes.length > 0 ? $_("krea_story_next_label", {}, "What happens next (or new premise):") : $_("krea_story_premise_label", {}, "Plot premise in a few words:")}</span>
                </div>
                <textarea
                  rows="2"
                  class="dir-textarea"
                  placeholder={$_("krea_story_idea_placeholder", {}, "Describe the idea or plot twist in plain words...")}
                  bind:value={storyIdea}
                  on:input={saveActiveDraftStoryToLocal}
                ></textarea>

                <div class="plan-controls-row">
                  <label class="scene-count-label">
                    {$_("krea_story_frames_label", {}, "Frames:")}
                    <select bind:value={storySceneCount} class="count-select">
                      <option value={2}>2</option>
                      <option value={3}>3</option>
                      <option value={4}>4</option>
                      <option value={5}>5</option>
                      <option value={6}>6</option>
                    </select>
                  </label>

                  {#if storyScenes.length > 0}
                    <button
                      type="button"
                      class="plan-btn secondary"
                      disabled={isPlanningStory}
                      on:click={() => planOrContinueStory("plan")}
                    >
                      🔄 {$_("krea_story_replan_btn", {}, "Replan")}
                    </button>
                    <button
                      type="button"
                      class="plan-btn primary"
                      disabled={isPlanningStory}
                      on:click={() => planOrContinueStory("continue")}
                    >
                      {isPlanningStory ? $_("krea_story_writing", {}, "Writing...") : $_("krea_story_continue_count", { count: storySceneCount }, `➕ Continue (+${storySceneCount})`)}
                    </button>
                  {:else}
                    <button
                      type="button"
                      class="plan-btn primary full"
                      disabled={isPlanningStory}
                      on:click={() => planOrContinueStory("plan")}
                    >
                      {isPlanningStory ? $_("krea_story_planning", {}, "🎬 Director is scripting scenes...") : $_("krea_story_plan_count", { count: storySceneCount }, `🎬 Plan ${storySceneCount} scenes`)}
                    </button>
                  {/if}
                </div>
              </div>
            {/if}
          </div>

          <!-- Scenes List -->
          <div class="story-scenes-list">
            {#if storyScenes.length === 0 && !isPlanningStory}
              <div class="director-empty-hint compact">
                <p>{$_("krea_story_empty_hint", {}, "Enter a Setting and a brief Premise above (and select a Persona if starring in the story) — the Director will write literary narrative, dialogue, and visual frame prompts.")}</p>
              </div>
            {:else}
              {#each storyScenes as sc, idx (sc.id || idx)}
                <div class="scene-card" class:shooting={shootingSceneIdx === idx} class:done={!!sc.image_name}>
                  <div class="scene-card-head">
                    <span class="scene-num">#{sc.scene_number || idx + 1}</span>
                    <input
                      type="text"
                      class="scene-title-input"
                      bind:value={sc.title}
                      on:change={saveActiveDraftStoryToLocal}
                    />
                    <select
                      class="scene-aspect-select"
                      bind:value={sc.aspect}
                      on:change={saveActiveDraftStoryToLocal}
                      title={$_("krea_story_frame_aspect_hint", {}, "Frame aspect ratio")}
                    >
                      <option value="896x1152">3:4</option>
                      <option value="1024x1024">1:1</option>
                      <option value="1152x896">4:3</option>
                    </select>
                    <button type="button" class="scene-del-btn" title={$_("krea_story_del_scene_hint", {}, "Delete scene")} on:click={() => removeStoryScene(idx)}>✕</button>
                  </div>

                  {#if sc.blobUrl || sc.image_path}
                    <div class="scene-thumb-box">
                      <img
                        src={sc.blobUrl || getThumbnailUrl({ path: sc.image_path, name: sc.image_name })}
                        alt={sc.title}
                        class="scene-thumb-img"
                      />
                      <span class="scene-done-badge">✓ {$_("krea_story_shot_done", {}, "Shot")}</span>
                    </div>
                  {/if}

                  <textarea
                    rows="3"
                    class="scene-narrative-box"
                    placeholder={$_("krea_story_narrative_placeholder", {}, "Scene literary prose & dialogue...")}
                    bind:value={sc.narrative}
                    on:change={saveActiveDraftStoryToLocal}
                  ></textarea>

                  <div class="scene-sub-toggles">
                    <button type="button" class="scene-sub-link" on:click={() => { sc.showPrompt = !sc.showPrompt; storyScenes = [...storyScenes]; }}>
                      🎥 {$_("krea_story_frame_prompt_btn", {}, "Frame prompt")} {sc.showPrompt ? "▲" : "▼"}
                    </button>
                    <button type="button" class="scene-sub-link" on:click={() => { sc.showRefine = !sc.showRefine; storyScenes = [...storyScenes]; }}>
                      🪄 {$_("krea_story_ai_refine_btn", {}, "AI tweak")}
                    </button>
                    <button
                      type="button"
                      class="scene-sub-link"
                      title={$_("krea_story_send_composer_hint", {}, "Send this scene's prompt to main Studio composer")}
                      on:click={() => { pushPromptUndo(promptText); promptText = sc.visual_prompt; tick().then(resizeTextarea); }}
                    >
                      ↗ {$_("krea_dir_to_composer", {}, "To composer")}
                    </button>
                  </div>

                  {#if sc.showPrompt}
                    <textarea
                      rows="3"
                      class="scene-vprompt-box"
                      placeholder={$_("krea_story_vprompt_placeholder", {}, "English diffusion visual prompt...")}
                      bind:value={sc.visual_prompt}
                      on:change={saveActiveDraftStoryToLocal}
                    ></textarea>
                  {/if}

                  {#if sc.showRefine}
                    <div class="scene-refine-row">
                      <input
                        type="text"
                        class="dir-input small"
                        placeholder={$_("krea_story_refine_placeholder", {}, "What should change in this scene?")}
                        bind:value={sc.refineText}
                        on:keydown={(e) => e.key === "Enter" && refineSingleScene(idx)}
                      />
                      <button
                        type="button"
                        class="mini-act-btn primary"
                        disabled={refiningSceneIdx !== -1 || !(sc.refineText || "").trim()}
                        on:click={() => refineSingleScene(idx)}
                      >
                        {refiningSceneIdx === idx ? "..." : $_("krea_ok", {}, "OK")}
                      </button>
                    </div>
                  {/if}

                  <div class="scene-actions-row">
                    <button
                      type="button"
                      class="shoot-scene-btn"
                      disabled={isGenerating}
                      on:click={() => shootSingleScene(idx)}
                    >
                      {#if shootingSceneIdx === idx}
                        ⏳ {$_("krea_story_shooting_elapsed", { sec: generationElapsed }, `Shooting (${generationElapsed}s)...`)}
                      {:else if sc.image_name}
                        🔄 {$_("krea_story_reshoot_btn", {}, "Reshoot frame")}
                      {:else}
                        🎬 {$_("krea_story_shoot_frame_btn", {}, "Shoot frame")}
                      {/if}
                    </button>
                  </div>
                </div>
              {/each}

              <div class="story-continue-inline">
                <button type="button" class="add-scene-inline-btn" on:click={addManualStoryScene}>{$_("krea_story_add_empty_scene", {}, "+ Empty scene")}</button>
                <button
                  type="button"
                  class="add-scene-inline-btn highlight"
                  disabled={isPlanningStory}
                  on:click={() => { isSetupCollapsed = false; }}
                >
                  ➕ {$_("krea_story_continue_plot_btn", {}, "Continue story...")}
                </button>
              </div>
            {/if}
          </div>

          <!-- Sticky Storyboard Footer -->
          {#if storyScenes.length > 0}
            <div class="storyboard-footer">
              {#if isShootingSeries}
                <button type="button" class="series-stop-btn" on:click={() => (stopSeriesRequested = true)}>
                  ⏹ {$_("krea_story_stop_after_frame", {}, "Stop after current frame")}
                </button>
              {:else}
                <button
                  type="button"
                  class="series-shoot-all-btn"
                  disabled={isGenerating}
                  on:click={shootAllRemainingScenes}
                >
                  ▶️ {$_("krea_story_shoot_series", { count: storyScenes.filter((s) => !s.image_name).length || storyScenes.length }, `Shoot series (${storyScenes.filter((s) => !s.image_name).length || storyScenes.length})`)}
                </button>
              {/if}
              <div class="storyboard-footer-sub">
                <button
                  type="button"
                  class="story-util-btn"
                  disabled={isSavingStory}
                  on:click={() => persistCurrentStory(false)}
                  title={$_("krea_story_save_md_hint", {}, "Save illustrated Readme.md and story.json into story folder")}
                >
                  {isSavingStory ? "💾..." : "💾 Story.md"}
                </button>
                {#if storySlug}
                  <button
                    type="button"
                    class="story-util-btn"
                    on:click={() => selectStorySubfolder(storySlug)}
                    title={$_("krea_story_open_gallery_hint", {}, "Show this story folder in Gallery")}
                  >
                    📁 {$_("krea_story_in_gallery", {}, "In gallery")}
                  </button>
                {/if}
                <button
                  type="button"
                  class="story-util-btn"
                  disabled={isExportingStory}
                  on:click={() => exportStoryPack(false)}
                  title={$_("krea_story_zip_hint", {}, "Download ZIP archive with all frames and Story.md")}
                >
                  {isExportingStory ? "📦..." : "📦 ZIP"}
                </button>
                {#if canPublishCivitai}
                  <button
                    type="button"
                    class="story-util-btn civitai"
                    disabled={isPublishingCivitai}
                    on:click={openPublishStoryToCivitai}
                    title={$_("krea_story_civitai_hint", {}, "Send shot frames and story to Autopilot for review")}
                  >
                    🚀 {$_("krea_story_to_civitai", {}, "Publish")}
                  </button>
                {/if}
              </div>
            </div>
          {/if}
        </div>
      {/if}
    </aside>
  {/if}
  </div>

  <!-- Modal: Manual Create/Edit Persona -->
  {#if showPersonaModal}
    <div class="modal-backdrop" on:click={() => (showPersonaModal = false)}>
      <!-- svelte-ignore a11y-click-events-have-key-events -->
      <!-- svelte-ignore a11y-no-static-element-interactions -->
      <div class="persona-modal-card" on:click|stopPropagation>
        <div class="modal-header">
          <h3>{editingPersonaId ? $_("krea_edit_persona", {}, "Редактировать персону") : $_("krea_new_persona", {}, "Новая персона")}</h3>
          <button class="modal-close-btn" on:click={() => (showPersonaModal = false)}>✕</button>
        </div>

        <div class="modal-body">
          <div class="modal-field">
            <label for="p-name">{$_("krea_persona_name", {}, "Имя персоны:")}</label>
            <input
              id="p-name"
              type="text"
              placeholder={$_("krea_persona_name_placeholder", {}, "например: Кира, Alex, June...")}
              bind:value={personaFormName}
              class="modal-input"
            />
          </div>

          <div class="modal-field">
            <label for="p-title">{$_("krea_persona_role", {}, "Амплуа / Роль:")}</label>
            <input
              id="p-title"
              type="text"
              placeholder={$_("krea_persona_role_placeholder", {}, "например: Alt Girl, Tech Lead, Фотомодель...")}
              bind:value={personaFormTitle}
              class="modal-input"
            />
          </div>

          <div class="modal-field">
            <div class="modal-label-row">
              <label for="p-prompt">{$_("krea_verbal_passport", {}, "Словесный паспорт (черты лица, прическа, особенности):")}</label>
              {#if personaFormAvatarPreview}
                <button
                  type="button"
                  class="vision-inline-link"
                  disabled={isAnalyzingFace}
                  on:click={() => analyzeFaceForPassport(personaFormAvatarData || personaFormAvatarPreview)}
                >
                  {#if isAnalyzingFace}
                    ⏳ {$_("krea_analyzing_face", {}, "Анализируем лицо...")}
                  {:else}
                    🔍 {$_("krea_autofill_photo", {}, "Заполнить по фото")}
                  {/if}
                </button>
              {/if}
            </div>
            <textarea
              id="p-prompt"
              rows="4"
              placeholder={$_("krea_persona_prompt_placeholder", {}, "детальное словесное описание внешности для фиксации лица во всех генерациях...")}
              bind:value={personaFormPrompt}
              class="modal-textarea"
            ></textarea>
            <span class="field-hint">{$_("krea_passport_hint", {}, "Krea 2 фиксирует лицо строго по этому паспорту. Фотография лица анализируется Vision-нейросетью для извлечения черт лица.")}</span>
          </div>

          <div class="modal-field">
            <label for="p-personality">{$_("krea_persona_personality", {}, "Характер, биография и манера речи (Lore & Voice):")}</label>
            <textarea
              id="p-personality"
              rows="3"
              placeholder={$_("krea_persona_personality_placeholder", {}, "например: 23 года, графический дизайнер, самоирония, любит утренний чёрный кофе, винтаж и плёночное зерно, пишет просто и тепло от первого лица...")}
              bind:value={personaFormPersonality}
              class="modal-textarea"
            ></textarea>
            <span class="field-hint">{$_("krea_personality_hint", {}, "Инжектится в контекст LLM для Режиссёра, реплик в раскадровке, постов от лица персоны и будущих диалогов в чате.")}</span>
          </div>

          <div class="modal-field" style="background: rgba(120, 140, 255, 0.05); border: 1px solid rgba(120, 140, 255, 0.18); border-radius: 10px; padding: 10px 12px;">
            <div class="modal-label-row" style="align-items: center; justify-content: space-between; display: flex; gap: 8px; flex-wrap: wrap;">
              <label style="display: inline-flex; align-items: center; gap: 8px; margin: 0; cursor: pointer;">
                <input type="checkbox" bind:checked={personaFormPresetEnabled} />
                <span>🎛️ {$_("krea_persona_native_preset", {}, "Закрепить родную модель и веса LoRA за персоной")}</span>
              </label>
              <button
                type="button"
                class="vision-inline-link"
                on:click={captureStudioSettingsIntoPersonaForm}
              >
                📥 {$_("krea_capture_studio_settings", {}, "Взять текущие из Студии")}
              </button>
            </div>
            {#if personaFormPresetEnabled}
              <div style="margin-top: 10px; display: flex; flex-direction: column; gap: 9px;">
                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 10px; align-items: center;">
                  <div>
                    <label for="p-base-model" style="font-size: 11.5px; opacity: 0.85;">{$_("krea_base_engine", {}, "Базовая модель:")}</label>
                    <select id="p-base-model" bind:value={personaFormBaseModel} class="select-styled modal-input">
                      <option value="sdxl_realistic">⚡ Express · Realistic</option>
                      <option value="sdxl_perfect">⚡ Express · Perfect</option>
                      <option value="sdxl_fantasy">⚡ Express · Fantasy</option>
                      <option value="sdxl_tooned">⚡ Express · Tooned</option>
                      <option value="realism">🎬 Studio · Krea 2 Realism</option>
                      <option value="semi_realism">🎬 Studio · Muse Semi-Realism</option>
                      <option value="anima">🎬 Studio · Anima Aesthetic 2D</option>
                    </select>
                  </div>
                  <div style="padding-top: 14px;">
                    <label style="display: inline-flex; align-items: center; gap: 7px; font-size: 12px; cursor: pointer;">
                      <input type="checkbox" bind:checked={personaFormUseFaceRef} />
                      <span>📸 {$_("krea_persona_face_lock", {}, "Face Lock from avatar")}</span>
                    </label>
                  </div>
                </div>

                <div>
                  <span style="font-size: 11.5px; opacity: 0.85;">{$_("krea_persona_archetypes", {}, "Character LoRAs / Archetypes:")}</span>
                  <div style="display: flex; flex-wrap: wrap; gap: 6px; margin-top: 5px;">
                    {#each personaModalArchetypes as arch}
                      <button
                        type="button"
                        on:click={() => togglePersonaFormArchetype(arch.id)}
                        style="font-size: 12px; padding: 4px 9px; border-radius: 7px; cursor: pointer; border: 1px solid {personaFormArchetypes.includes(arch.id) ? '#6c8cff' : 'rgba(255,255,255,0.14)'}; background: {personaFormArchetypes.includes(arch.id) ? 'rgba(108,140,255,0.22)' : 'rgba(0,0,0,0.2)'}; color: inherit;"
                      >
                        {arch.icon || ""} {arch.name}
                      </button>
                    {/each}
                  </div>
                  {#each personaFormArchetypes as aid}
                    <div style="display: flex; align-items: center; gap: 8px; margin-top: 6px; font-size: 12px;">
                      <span style="min-width: 90px; opacity: 0.85;">{$_("krea_persona_lora_weight", { id: aid }, `Weight ${aid}:`)}</span>
                      <input
                        type="range"
                        min="0.1"
                        max="1.4"
                        step="0.05"
                        value={personaFormArchetypeWeights[aid] ?? 0.8}
                        on:input={(ev) => { personaFormArchetypeWeights = { ...personaFormArchetypeWeights, [aid]: parseFloat(ev.target.value) }; }}
                        style="flex: 1;"
                      />
                      <span style="min-width: 34px; text-align: right;">{(personaFormArchetypeWeights[aid] ?? 0.8).toFixed(2)}</span>
                    </div>
                  {/each}
                </div>

                <div>
                  <span style="font-size: 11.5px; opacity: 0.85;">{$_("krea_persona_styles", {}, "Style LoRAs (optional):")}</span>
                  <div style="display: flex; flex-wrap: wrap; gap: 6px; margin-top: 5px;">
                    {#each personaModalStyles as st}
                      <button
                        type="button"
                        on:click={() => togglePersonaFormStyle(st.id)}
                        style="font-size: 12px; padding: 4px 9px; border-radius: 7px; cursor: pointer; border: 1px solid {personaFormStyles.includes(st.id) ? '#6c8cff' : 'rgba(255,255,255,0.14)'}; background: {personaFormStyles.includes(st.id) ? 'rgba(108,140,255,0.22)' : 'rgba(0,0,0,0.2)'}; color: inherit;"
                      >
                        {st.icon || ""} {st.name}
                      </button>
                    {/each}
                  </div>
                </div>
                <span class="field-hint">{$_("krea_persona_native_preset_hint", {}, "When this persona is selected, Studio automatically applies her model and LoRA weights. For a dedicated character LoRA (e.g. Kate), disable Face Lock.")}</span>
              </div>
            {/if}
          </div>

          <div class="modal-field">
            <label for="p-avatar">{$_("krea_avatar_face_ref", {}, "Аватар / Референс лица:")}</label>
            <div class="avatar-generator-card">
              <div class="avatar-preview-container">
                {#if isGeneratingAvatar}
                  <div class="avatar-loading-box">
                    <span class="avatar-spinner">⏳</span>
                    <span>Krea 2...</span>
                  </div>
                {:else if personaFormAvatarPreview}
                  <img src={personaFormAvatarPreview} alt="Avatar" class="avatar-preview-box" />
                {:else}
                  <div class="avatar-empty-box">👤</div>
                {/if}
              </div>

              <div class="avatar-actions-col">
                <div class="avatar-controls-row">
                  <select bind:value={avatarGenArchetype} class="select-styled archetype-select" disabled={isGeneratingAvatar}>
                    <option value="prompt">{$_("krea_from_passport", {}, "По словесному паспорту")}</option>
                    {#each aiArchetypes as arch}
                      <option value={arch.id}>{arch.name}</option>
                    {/each}
                  </select>

                  <button
                    type="button"
                    class="generate-avatar-btn"
                    disabled={isGeneratingAvatar || (!personaFormPrompt.trim() && avatarGenArchetype === "prompt")}
                    on:click={generateAvatarForPersona}
                  >
                    {#if isGeneratingAvatar}
                      🎨 {$_("krea_creating", {}, "Создание...")}
                    {:else}
                      ✨ {$_("krea_generate_btn", {}, "Сгенерировать")}
                    {/if}
                  </button>
                </div>

                <div class="avatar-manual-upload">
                  <span class="upload-hint">{$_("krea_upload_photo_ref", {}, "Или загрузите фото лица (референс):")}</span>
                  <input
                    id="p-avatar"
                    type="file"
                    accept="image/png,image/jpeg,image/webp"
                    on:change={handleAvatarUpload}
                    class="file-input"
                    disabled={isGeneratingAvatar || isAnalyzingFace}
                  />
                  {#if personaFormAvatarPreview}
                    <button
                      type="button"
                      class="vision-analyze-btn"
                      disabled={isAnalyzingFace}
                      on:click={() => analyzeFaceForPassport(personaFormAvatarData || personaFormAvatarPreview)}
                    >
                      {#if isAnalyzingFace}
                        ⏳ {$_("krea_analyzing_face", {}, "Распознавание лица через Vision...")}
                      {:else}
                        ✨ {$_("krea_recognize_features", {}, "Распознать черты лица с фото (Vision)")}
                      {/if}
                    </button>
                  {/if}
                </div>
              </div>
            </div>
          </div>
          {#if effectiveCivitaiAccounts.length > 0}
            <div class="modal-field">
              <label for="p-civitai-acc">{$_("civitai_persona_acc_label", {}, "CivitAI publishing account (MCP):")}</label>
              <select id="p-civitai-acc" bind:value={personaFormCivitaiAccount} class="select-styled modal-input">
                <option value="">{$_("civitai_default_acc", {}, "Default")}</option>
                {#each effectiveCivitaiAccounts as acc}
                  <option value={acc.id}>{acc.label}</option>
                {/each}
              </select>
            </div>
          {/if}
        </div>

        <div class="modal-footer">
          <button class="cancel-btn" on:click={() => (showPersonaModal = false)}>{$_("krea_cancel", {}, "Cancel")}</button>
          <button
            class="save-btn"
            disabled={isSavingPersona || !personaFormName.trim() || !personaFormPrompt.trim()}
            on:click={savePersona}
          >
            {isSavingPersona ? $_("krea_saving", {}, "Saving...") : $_("krea_save", {}, "Save")}
          </button>
        </div>
      </div>
    </div>
  {/if}

  <!-- Modal: Direct CivitAI API Publish -->
  {#if showCivitaiPublishModal}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="modal-backdrop" on:click={() => { if (!isPublishingCivitai) showCivitaiPublishModal = false; }}>
      <!-- svelte-ignore a11y-click-events-have-key-events -->
      <!-- svelte-ignore a11y-no-static-element-interactions -->
      <div class="persona-modal-card civitai-publish-modal" on:click|stopPropagation>
        <div class="modal-header">
          <h3>🚀 {$_("civitai_pub_modal_title", {}, "Publish to CivitAI")} {civitaiPubMode === "story" ? $_("civitai_pub_frames_count", { count: civitaiPubItems.length }, `(${civitaiPubItems.length} frames)`) : ""}</h3>
          <button class="modal-close-btn" disabled={isPublishingCivitai} on:click={() => (showCivitaiPublishModal = false)}>✕</button>
        </div>

        <div class="modal-body">
          <div class="modal-field">
            <label for="civ-pub-acc">{$_("civitai_pub_acc_label", {}, "Publishing account (from your MCP keys):")}</label>
            <select id="civ-pub-acc" bind:value={civitaiPubAccountId} class="select-styled modal-input" disabled={isPublishingCivitai}>
              {#each effectiveCivitaiAccounts as acc}
                <option value={acc.id}>{acc.label}</option>
              {/each}
              <option value="custom">{$_("civitai_pub_custom_acc_opt", {}, "➕ Add / use another CivitAI API key...")}</option>
            </select>
          </div>

          {#if civitaiPubAccountId === "custom"}
            <div class="modal-field">
              <label for="civ-pub-tok">{$_("civitai_pub_token_label", {}, "CivitAI API Key (Bearer):")}</label>
              <input
                id="civ-pub-tok"
                type="password"
                placeholder={$_("civitai_pub_token_placeholder", {}, "Paste the API key for your CivitAI account...")}
                bind:value={civitaiPubCustomToken}
                class="modal-input"
                disabled={isPublishingCivitai}
              />
              <label class="civitai-check-row" style="margin-top: 6px;">
                <input type="checkbox" bind:checked={civitaiPubSaveKey} disabled={isPublishingCivitai} />
                <span>{$_("civitai_pub_save_key_check", {}, "Save this account in my MCP keys (encrypted with DID)")}</span>
              </label>
            </div>
          {/if}

          <div class="modal-field">
            <label for="civ-pub-title">{$_("civitai_pub_title_label", {}, "Post title:")}</label>
            <input
              id="civ-pub-title"
              type="text"
              placeholder={$_("civitai_pub_title_placeholder", {}, "Artwork or story title...")}
              bind:value={civitaiPubTitle}
              class="modal-input"
              disabled={isPublishingCivitai}
            />
          </div>

          <div class="modal-field">
            <label for="civ-pub-detail">{$_("civitai_pub_detail_label", {}, "Description / story:")}</label>
            <textarea
              id="civ-pub-detail"
              rows={civitaiPubMode === "story" ? 5 : 3}
              placeholder={$_("civitai_pub_detail_placeholder", {}, "Post caption on CivitAI (optional)...")}
              bind:value={civitaiPubDetail}
              class="modal-textarea"
              disabled={isPublishingCivitai}
            ></textarea>
          </div>

          <div class="modal-field">
            <label for="civ-pub-tags">{$_("civitai_pub_tags_label", {}, "Tags (comma-separated, optional):")}</label>
            <input
              id="civ-pub-tags"
              type="text"
              placeholder={$_("civitai_pub_tags_placeholder", {}, "e.g.: anime, portrait, cyberpunk")}
              bind:value={civitaiPubTags}
              class="modal-input"
              disabled={isPublishingCivitai}
            />
          </div>

          <div class="civitai-pub-toggles">
            <label class="civitai-check-row">
              <input type="checkbox" bind:checked={civitaiPubIncludeMeta} disabled={isPublishingCivitai} />
              <span>{$_("civitai_pub_include_meta", {}, "Attach prompt, checkpoint & LoRAs (for model gallery indexing)")}</span>
            </label>
            <label class="civitai-check-row">
              <input type="checkbox" bind:checked={civitaiPubImmediate} disabled={isPublishingCivitai} />
              <span>{$_("civitai_pub_immediate", {}, "Publish immediately (uncheck to save as draft on your account)")}</span>
            </label>
          </div>

          {#if civitaiPubError}
            <div class="civitai-pub-alert error">⚠️ {civitaiPubError}</div>
          {/if}

          {#if civitaiPubResult}
            <div class="civitai-pub-alert success">
              <div class="civitai-pub-success-head">
                <strong>
                  {civitaiPubResult.published ? $_("civitai_pub_done_live", {}, "✅ Post published!") : $_("civitai_pub_done_draft", {}, "📝 Draft saved!")}
                </strong>
                {#if civitaiPubResult.username}
                  <span class="civitai-user-pill">@{civitaiPubResult.username}</span>
                {/if}
              </div>
              {#if civitaiPubResult.linked_resources && civitaiPubResult.linked_resources.length > 0}
                <div class="civitai-linked-res">
                  {$_("civitai_pub_linked_res", { list: civitaiPubResult.linked_resources.join(" • ") }, `Linked resources: ${civitaiPubResult.linked_resources.join(" • ")}`)}
                </div>
              {/if}
              <div class="civitai-pub-links">
                <a href={civitaiPubResult.post_url} target="_blank" rel="noopener noreferrer" class="civitai-open-post-btn">
                  {$_("civitai_pub_open_btn", {}, "Open on CivitAI ↗")}
                </a>
                {#if civitaiPubResult.published && civitaiPubResult.edit_url}
                  <a href={civitaiPubResult.edit_url} target="_blank" rel="noopener noreferrer" class="civitai-edit-post-link">
                    {$_("krea_persona_edit", {}, "Edit")}
                  </a>
                {/if}
              </div>
            </div>
          {/if}
        </div>

        <div class="modal-footer">
          <button class="cancel-btn" disabled={isPublishingCivitai} on:click={() => (showCivitaiPublishModal = false)}>
            {civitaiPubResult ? $_("krea_done", {}, "Done") : $_("krea_cancel", {}, "Cancel")}
          </button>
          {#if !civitaiPubResult}
            <button
              class="save-btn civitai-submit-btn"
              disabled={isPublishingCivitai}
              on:click={submitCivitaiPublish}
            >
              {#if isPublishingCivitai}
                ⏳ {$_("civitai_pub_uploading", {}, "Uploading to CivitAI...")}
              {:else}
                {civitaiPubImmediate ? $_("civitai_pub_submit_live", {}, "🚀 Publish") : $_("civitai_pub_submit_draft", {}, "📝 Save draft")}
              {/if}
            </button>
          {/if}
        </div>
      </div>
    </div>
  {/if}

  <!-- Modal: AI Persona Factory (Scenario A + B combined) -->
  {#if showAiFactoryModal}
    <div class="modal-backdrop" on:click={() => (showAiFactoryModal = false)}>
      <!-- svelte-ignore a11y-click-events-have-key-events -->
      <!-- svelte-ignore a11y-no-static-element-interactions -->
      <div class="persona-modal-card ai-factory-modal" on:click|stopPropagation>
        <div class="modal-header">
          <div class="ai-factory-title">
            <span class="ai-icon">✨</span>
            <h3>{$_("krea_factory_title", {}, "AI Фабрика персон")}</h3>
          </div>
          <button class="modal-close-btn" on:click={() => (showAiFactoryModal = false)}>✕</button>
        </div>

        <div class="modal-body">
          <div class="modal-field">
            <label>{$_("krea_select_archetype", {}, "Выберите типаж / архетип:")}</label>
            <div class="archetype-grid">
              {#each aiArchetypes as arch}
                <button
                  class="archetype-card"
                  class:active={aiFactoryArchetype === arch.id}
                  on:click={() => (aiFactoryArchetype = arch.id)}
                >
                  <span class="arch-name">{arch.name}</span>
                  <span class="arch-hint">{arch.hint}</span>
                </button>
              {/each}
            </div>
          </div>

          <div class="ai-factory-action-row">
            <button
              class="generate-ai-persona-btn"
              class:busy={isGeneratingAiPersona}
              disabled={isGeneratingAiPersona}
              on:click={generateAiPersona}
            >
              {#if isGeneratingAiPersona}
                <div class="spinner"></div>
                <span>{$_("krea_generating_face", {}, "Генерирую внешность (~4s)...")}</span>
              {:else}
                <span>🎲 {$_("krea_factory_generate_btn", {}, "Сгенерировать паспорт и портрет")}</span>
              {/if}
            </button>
          </div>

          <!-- Generated Persona Preview -->
          {#if generatedAiPersona}
            <div class="generated-persona-card" in:fade={{ duration: 150 }}>
              <div class="gen-avatar-col">
                {#if generatedAiPersona.avatar}
                  <img src={generatedAiPersona.avatar} alt="Avatar" class="gen-avatar-img" />
                {:else}
                  <div class="gen-avatar-empty">👤</div>
                {/if}
              </div>
              <div class="gen-info-col">
                <div class="gen-field">
                  <span class="gen-label">{$_("krea_name_label", {}, "Имя:")}</span>
                  <input type="text" bind:value={generatedAiPersona.name} class="modal-input small" />
                </div>
                <div class="gen-field">
                  <span class="gen-label">{$_("krea_role_label", {}, "Роль:")}</span>
                  <input type="text" bind:value={generatedAiPersona.title} class="modal-input small" />
                </div>
                <div class="gen-field">
                  <span class="gen-label">{$_("krea_verbal_passport", {}, "Словесный паспорт:")}</span>
                  <textarea rows="3" bind:value={generatedAiPersona.prompt} class="modal-textarea small"></textarea>
                </div>
              </div>
            </div>
          {/if}
        </div>

        <div class="modal-footer">
          <button class="cancel-btn" on:click={() => (showAiFactoryModal = false)}>{$_("krea_close", {}, "Закрыть")}</button>
          {#if generatedAiPersona}
            <button class="save-btn" on:click={saveAiGeneratedPersona}>
              💾 {$_("krea_save_to_personas", {}, "Сохранить в мои персоны")}
            </button>
          {/if}
        </div>
      </div>
    </div>
  {/if}

  <!-- Delete Confirmation Modal (Native OMD Style) -->
  {#if deleteConfirmTarget}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div
      class="confirm-modal-overlay"
      transition:fade={{ duration: 150 }}
      on:click|self={() => (deleteConfirmTarget = null)}
    >
      <div class="confirm-modal-box" transition:fly={{ y: 20, duration: 200 }}>
        <div class="confirm-modal-header">
          <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="#f85149" stroke-width="2" class="confirm-danger-icon">
            <path stroke-linecap="round" stroke-linejoin="round" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
          </svg>
          <h3>{deleteConfirmTarget.title}</h3>
        </div>
        <div class="confirm-modal-body">
          <p>
            {$_("krea_delete_confirm_text", { name: deleteConfirmTarget.name }, `Вы уверены, что хотите удалить «${deleteConfirmTarget.name}»?`)}
          </p>
          {#if deleteConfirmTarget.subtext}
            <p class="confirm-modal-subtext">{deleteConfirmTarget.subtext}</p>
          {/if}
        </div>
        <div class="confirm-modal-actions">
          <button
            class="confirm-btn cancel"
            on:click={() => (deleteConfirmTarget = null)}
            disabled={isDeletingItem}
          >
            {$_("cancel", {}, "Отмена")}
          </button>
          <button
            class="confirm-btn danger"
            on:click={executeConfirmDelete}
            disabled={isDeletingItem}
          >
            {isDeletingItem ? $_("krea_deleting", {}, "Удаление...") : $_("delete", {}, "Удалить")}
          </button>
        </div>
      </div>
    </div>
  {/if}
</div>

<style>
  .krea-studio-root {
    display: flex;
    flex-direction: column;
    height: 100%;
    width: 100%;
    background: #0F0A10;
    color: #F7ECF2;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    position: relative;
    overflow: hidden;
  }

  /* Header */
  .studio-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    min-height: calc(56px + env(safe-area-inset-top, 0px));
    padding: env(safe-area-inset-top, 0px) 14px 0 14px;
    background: rgba(15, 10, 16, 0.95);
    border-bottom: 1px solid rgba(255, 255, 255, 0.08);
    backdrop-filter: blur(12px);
    z-index: 20;
    gap: 12px;
    box-sizing: border-box;
  }

  .header-left {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .sidebar-toggle-btn {
    background: transparent;
    border: none;
    color: #998094;
    cursor: pointer;
    width: 34px;
    height: 34px;
    padding: 0;
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .sidebar-toggle-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: #fff;
  }

  .header-title-row {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .studio-badge {
    font-size: 16px;
    font-weight: 700;
    background: linear-gradient(135deg, #a78bfa 0%, #ec4899 100%);
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
  }

  .turbo-pill {
    font-size: 11px;
    font-weight: 600;
    background: rgba(167, 139, 250, 0.15);
    color: #c4b5fd;
    border: 1px solid rgba(167, 139, 250, 0.3);
    padding: 2px 7px;
    border-radius: 10px;
  }

  .header-subtitle {
    font-size: 11px;
    color: #998094;
    margin-top: 1px;
  }

  /* Header Tabs */
  .header-tabs {
    display: flex;
    align-items: center;
    background: rgba(255, 255, 255, 0.05);
    padding: 3px;
    border-radius: 20px;
    border: 1px solid rgba(255, 255, 255, 0.08);
    gap: 4px;
  }

  .tab-btn {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 6px 14px;
    border-radius: 16px;
    background: transparent;
    border: none;
    color: #998094;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.2s ease;
  }

  .tab-btn:hover {
    color: #fff;
  }

  .tab-btn.active {
    background: linear-gradient(135deg, #9333EA 0%, #7c3aed 100%);
    color: #ffffff;
    box-shadow: 0 2px 10px rgba(124, 58, 237, 0.35);
  }

  .tab-btn .badge {
    font-size: 10px;
    background: rgba(0, 0, 0, 0.3);
    padding: 1px 6px;
    border-radius: 8px;
  }

  .header-right {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .icon-btn {
    background: transparent;
    border: 1px solid rgba(255, 255, 255, 0.08);
    color: #998094;
    border-radius: 8px;
    width: 32px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.2s;
  }

  .icon-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: #fff;
  }

  .sort-order-btn {
    position: relative;
  }

  .sort-order-btn:hover {
    color: var(--accent-blue, #F472B6);
  }

  .sort-order-btn.is-active {
    color: var(--accent-blue, #F472B6);
    background: rgba(236, 72, 153, 0.12);
    border-color: rgba(236, 72, 153, 0.3);
  }

  .icon-btn.spinning svg {
    animation: spin 1s linear infinite;
  }

  @keyframes spin {
    100% { transform: rotate(360deg); }
  }

  /* Main Area */
  .studio-main {
    flex: 1;
    overflow-y: auto;
    padding: 20px 24px 220px 24px;
  }

  /* Personas View */
  .personas-section {
    max-width: 1200px;
    margin: 0 auto;
  }

  .section-toolbar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 20px;
    gap: 16px;
  }

  .toolbar-actions {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .section-desc h3 {
    margin: 0 0 4px 0;
    font-size: 18px;
    font-weight: 600;
  }

  .section-desc p {
    margin: 0;
    font-size: 13px;
    color: #998094;
  }

  .ai-factory-btn {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 16px;
    border-radius: 8px;
    background: linear-gradient(135deg, #ec4899 0%, #8b5cf6 100%);
    color: #fff;
    border: none;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    box-shadow: 0 4px 12px rgba(236, 72, 153, 0.35);
    transition: all 0.2s;
  }

  .ai-factory-btn:hover {
    transform: translateY(-1px);
    box-shadow: 0 6px 18px rgba(236, 72, 153, 0.5);
  }

  .create-persona-btn {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 8px 16px;
    border-radius: 8px;
    background: linear-gradient(135deg, #A855F7 0%, #8b5cf6 100%);
    color: #fff;
    border: none;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    box-shadow: 0 4px 12px rgba(168, 85, 247, 0.3);
    transition: all 0.2s;
  }

  .create-persona-btn:hover {
    opacity: 0.95;
    transform: translateY(-1px);
  }

  .personas-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
    gap: 18px;
  }

  .persona-card {
    background: rgba(255, 255, 255, 0.03);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 14px;
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 12px;
    cursor: pointer;
    transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
    position: relative;
  }

  .persona-card:hover {
    background: rgba(255, 255, 255, 0.06);
    border-color: rgba(167, 139, 250, 0.3);
    transform: translateY(-2px);
  }

  .persona-card.selected {
    border-color: #8b5cf6;
    background: rgba(139, 92, 246, 0.08);
    box-shadow: 0 0 20px rgba(139, 92, 246, 0.25);
  }

  .persona-avatar-wrap {
    position: relative;
    width: 64px;
    height: 64px;
    border-radius: 50%;
    overflow: hidden;
    background: rgba(255, 255, 255, 0.08);
  }

  .persona-avatar {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }

  .persona-avatar-placeholder {
    width: 100%;
    height: 100%;
    display: flex;
    align-items: center;
    justify-content: center;
    font-weight: 700;
    font-size: 20px;
    color: #c4b5fd;
    background: linear-gradient(135deg, #581C87 0%, #4c1d95 100%);
  }

  .active-check-badge {
    position: absolute;
    bottom: 0;
    right: 0;
    background: #10b981;
    color: #fff;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 11px;
    font-weight: bold;
    border: 2px solid #0F0A10;
  }

  .persona-header-row {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 4px;
  }

  .persona-name {
    font-weight: 700;
    font-size: 16px;
  }

  .persona-title-chip {
    font-size: 11px;
    color: #a78bfa;
    background: rgba(167, 139, 250, 0.12);
    padding: 2px 6px;
    border-radius: 6px;
  }

  .persona-ref-chip {
    font-size: 10px;
    font-weight: 700;
    color: #F472B6;
    background: rgba(236, 72, 153, 0.15);
    border: 1px solid rgba(236, 72, 153, 0.35);
    padding: 2px 6px;
    border-radius: 6px;
    display: inline-flex;
    align-items: center;
    gap: 3px;
  }

  .persona-passport {
    font-size: 12px;
    color: #998094;
    line-height: 1.4;
    display: -webkit-box;
    -webkit-line-clamp: 3;
    -webkit-box-orient: vertical;
    overflow: hidden;
    margin: 0;
  }

  .persona-actions {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-top: auto;
    padding-top: 8px;
    border-top: 1px solid rgba(255, 255, 255, 0.05);
  }

  .card-action-btn {
    border: none;
    border-radius: 6px;
    padding: 5px 12px;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
    background: rgba(255, 255, 255, 0.08);
    color: #F2E5ED;
    transition: all 0.15s;
  }

  .card-action-btn:hover {
    background: rgba(255, 255, 255, 0.15);
  }

  .card-action-btn.select-btn.active {
    background: #8b5cf6;
    color: #fff;
  }

  .card-action-btn.icon-only {
    padding: 5px 8px;
  }

  .card-action-btn.danger:hover {
    background: rgba(239, 68, 68, 0.2);
    color: #f87171;
  }

  .add-new-slot {
    align-items: center;
    justify-content: center;
    border: 2px dashed rgba(255, 255, 255, 0.12);
    background: transparent;
    min-height: 180px;
  }

  .add-slot-circle {
    width: 44px;
    height: 44px;
    border-radius: 50%;
    background: rgba(255, 255, 255, 0.06);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 24px;
    color: #a78bfa;
    margin-bottom: 6px;
  }

  .add-slot-label {
    font-size: 14px;
    font-weight: 600;
    color: #F2E5ED;
  }

  .add-slot-hint {
    font-size: 11px;
    color: #7D6376;
  }

  /* Gallery View */
  .gallery-section {
    max-width: 1400px;
    margin: 0 auto;
  }

  .empty-gallery {
    text-align: center;
    padding: 60px 20px;
    max-width: 600px;
    margin: 0 auto;
  }

  .loading-gallery {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 14px;
    padding: 80px 20px;
  }

  .gallery-load-spinner {
    width: 34px;
    height: 34px;
    border-radius: 50%;
    border: 3px solid rgba(139, 92, 246, 0.2);
    border-top-color: #8b5cf6;
    animation: gallerySpin 0.8s linear infinite;
  }

  .krea-studio-root.light-theme .gallery-load-spinner {
    border-color: rgba(168, 85, 247, 0.18);
    border-top-color: #A855F7;
  }

  .loading-gallery-text {
    margin: 0 !important;
    font-size: 13.5px;
    color: #998094;
  }

  @keyframes gallerySpin {
    0% { transform: rotate(0deg); }
    100% { transform: rotate(360deg); }
  }

  .empty-icon {
    font-size: 48px;
    margin-bottom: 12px;
  }

  .empty-gallery h3 {
    margin: 0 0 8px 0;
    font-size: 20px;
  }

  .empty-gallery p {
    color: #998094;
    font-size: 14px;
    margin-bottom: 24px;
  }

  .empty-suggestions {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    justify-content: center;
  }

  .sug-pill {
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.08);
    color: #c4b5fd;
    padding: 6px 12px;
    border-radius: 16px;
    font-size: 12px;
    cursor: pointer;
    transition: all 0.2s;
  }

  .sug-pill:hover {
    background: rgba(167, 139, 250, 0.15);
    border-color: rgba(167, 139, 250, 0.4);
  }

  .gallery-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
    gap: 16px;
  }

  .gallery-card {
    background: rgba(255, 255, 255, 0.03);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 12px;
    overflow: hidden;
    display: flex;
    flex-direction: column;
    transition: border-color 0.2s ease, transform 0.2s ease;
    content-visibility: auto;
    contain-intrinsic-size: 240px 360px;
  }

  .gallery-card.placeholder-card {
    content-visibility: visible;
    contain-intrinsic-size: none;
  }

  /* Dynamic Generating Placeholder Card */
  .placeholder-card {
    position: relative;
    border: 1px solid rgba(139, 92, 246, 0.45);
    box-shadow: 0 0 25px rgba(139, 92, 246, 0.2);
    animation: placeholderBorderGlow 3s ease-in-out infinite alternate;
  }

  @keyframes placeholderBorderGlow {
    0% {
      border-color: rgba(139, 92, 246, 0.35);
      box-shadow: 0 0 15px rgba(139, 92, 246, 0.15);
    }
    100% {
      border-color: rgba(236, 72, 153, 0.65);
      box-shadow: 0 0 30px rgba(236, 72, 153, 0.3);
    }
  }

  .placeholder-preview {
    position: relative;
    aspect-ratio: 3 / 4;
    overflow: hidden;
    background: radial-gradient(circle at 50% 40%, #1a102f 0%, #0F0A10 70%, #0F0A10 100%);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px;
    box-sizing: border-box;
  }

  .shimmer-mesh {
    position: absolute;
    inset: -60%;
    background: radial-gradient(circle at 50% 50%, rgba(139, 92, 246, 0.25), transparent 55%),
                radial-gradient(circle at 80% 20%, rgba(236, 72, 153, 0.2), transparent 45%),
                radial-gradient(circle at 20% 80%, rgba(236, 72, 153, 0.2), transparent 45%);
    animation: rotateMesh 10s linear infinite;
  }

  @keyframes rotateMesh {
    0% { transform: rotate(0deg); }
    100% { transform: rotate(360deg); }
  }

  .glow-orb {
    position: absolute;
    width: 140px;
    height: 140px;
    border-radius: 50%;
    background: radial-gradient(circle, rgba(168, 85, 247, 0.35) 0%, rgba(168, 85, 247, 0) 70%);
    filter: blur(20px);
    animation: floatOrb 4s ease-in-out infinite alternate;
  }

  @keyframes floatOrb {
    0% { transform: translate(-20px, -15px) scale(0.9); }
    100% { transform: translate(20px, 15px) scale(1.15); }
  }

  .placeholder-content {
    position: relative;
    z-index: 2;
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    gap: 12px;
    width: 100%;
  }

  .status-pill {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 6px 14px;
    border-radius: 9999px;
    background: rgba(15, 10, 16, 0.75);
    backdrop-filter: blur(12px);
    border: 1px solid rgba(255, 255, 255, 0.15);
    font-size: 12px;
    font-weight: 600;
    color: #FCF7FA;
    box-shadow: 0 4px 15px rgba(0, 0, 0, 0.4);
  }

  .pulse-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #10b981;
    box-shadow: 0 0 10px #10b981;
    animation: pulseDotAnim 1.2s ease-in-out infinite;
  }

  @keyframes pulseDotAnim {
    0%, 100% { transform: scale(0.85); opacity: 0.6; }
    50% { transform: scale(1.35); opacity: 1; }
  }

  .placeholder-prompt {
    font-size: 13px;
    line-height: 1.45;
    color: rgba(255, 255, 255, 0.9);
    display: -webkit-box;
    -webkit-line-clamp: 3;
    -webkit-box-orient: vertical;
    overflow: hidden;
    text-overflow: ellipsis;
    margin: 0;
    font-weight: 500;
    text-shadow: 0 1px 4px rgba(0, 0, 0, 0.8);
    max-width: 90%;
  }

  .placeholder-progress-bar {
    width: 75%;
    height: 4px;
    background: rgba(255, 255, 255, 0.12);
    border-radius: 9999px;
    overflow: hidden;
    position: relative;
  }

  .progress-bar-fill {
    position: absolute;
    top: 0;
    bottom: 0;
    left: 0;
    width: 35%;
    border-radius: 9999px;
    background: linear-gradient(90deg, #8b5cf6, #ec4899);
    animation: indeterminateProgress 1.6s cubic-bezier(0.4, 0, 0.2, 1) infinite;
  }

  .progress-bar-fill.downloading {
    background: linear-gradient(90deg, #EC4899, #10b981);
    animation: indeterminateProgress 0.9s cubic-bezier(0.4, 0, 0.2, 1) infinite;
  }

  .progress-bar-fill.determinate {
    left: 0;
    animation: none;
    transition: width 0.22s ease;
  }

  @keyframes indeterminateProgress {
    0% { left: -35%; width: 35%; }
    50% { left: 40%; width: 45%; }
    100% { left: 100%; width: 35%; }
  }

  .placeholder-footer {
    opacity: 0.85;
  }

  .krea-studio-root.light-theme .placeholder-preview {
    background: radial-gradient(circle at 50% 40%, #ede9fe 0%, #F2E5ED 70%, #F7ECF2 100%);
  }

  .krea-studio-root.light-theme .status-pill {
    background: rgba(255, 255, 255, 0.9);
    color: #0F0A10;
    border-color: rgba(0, 0, 0, 0.12);
    box-shadow: 0 4px 15px rgba(0, 0, 0, 0.08);
  }

  .krea-studio-root.light-theme .placeholder-prompt {
    color: #1A121D;
    text-shadow: none;
  }

  .gallery-card:hover {
    border-color: rgba(255, 255, 255, 0.2);
    transform: translateY(-2px);
  }

  .gallery-card-thumb-wrap {
    display: block;
    aspect-ratio: 3 / 4;
    overflow: hidden;
    background: linear-gradient(145deg, #1A121D 0%, #221727 100%);
    cursor: pointer;
    position: relative;
  }

  .krea-studio-root.light-theme .gallery-card-thumb-wrap {
    background: linear-gradient(145deg, #F2E5ED 0%, #F7ECF2 100%);
  }

  .gallery-card-thumb-wrap::before {
    content: "";
    position: absolute;
    inset: 0;
    margin: auto;
    width: 28px;
    height: 28px;
    border-radius: 50%;
    border: 2px solid rgba(153, 128, 148, 0.18);
    border-top-color: rgba(167, 139, 250, 0.6);
    opacity: 0;
    animation: none;
    pointer-events: none;
    will-change: transform;
  }

  .krea-studio-root.light-theme .gallery-card-thumb-wrap::before {
    border-color: rgba(153, 128, 148, 0.16);
    border-top-color: rgba(168, 85, 247, 0.6);
  }

  .gallery-card-thumb-wrap.thumb-loading::before {
    opacity: 1;
    animation: gallerySpin 0.9s linear infinite;
  }

  .gallery-card-thumb-wrap.thumb-ready {
    background: rgba(15, 10, 16, 0.4);
  }

  .krea-studio-root.light-theme .gallery-card-thumb-wrap.thumb-ready {
    background: #F7ECF2;
  }

  .gallery-card-thumb-wrap.thumb-ready::before {
    opacity: 0;
    animation: none;
  }

  .gallery-thumb {
    width: 100%;
    height: 100%;
    object-fit: cover;
    opacity: 0;
    transition: opacity 0.24s ease, transform 0.3s ease;
    position: relative;
    z-index: 1;
  }

  .gallery-thumb.thumb-loaded {
    opacity: 1;
  }

  .gallery-thumb[src=""] {
    opacity: 0;
  }

  .gallery-card:hover .gallery-thumb.thumb-loaded {
    transform: scale(1.03);
  }

  .card-footer-info {
    padding: 8px 8px 8px 10px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    font-size: 12px;
    gap: 0;
    min-height: 40px;
    position: relative;
    overflow: hidden;
  }

  .card-meta {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
    flex: 1;
    overflow: hidden;
  }

  .card-title {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    color: #E6D3DF;
    font-weight: 500;
    font-size: 11.5px;
    flex: 1;
    min-width: 0;
  }

  .card-thumb-badges {
    position: absolute;
    left: 7px;
    right: 7px;
    bottom: 7px;
    display: flex;
    align-items: center;
    gap: 5px;
    flex-wrap: wrap;
    pointer-events: none;
    z-index: 3;
  }

  .persona-tag {
    font-size: 10px;
    background: rgba(30, 27, 75, 0.78);
    backdrop-filter: blur(6px);
    border: 1px solid rgba(167, 139, 250, 0.35);
    color: #ede9fe;
    padding: 2px 6px;
    border-radius: 6px;
    white-space: nowrap;
    flex-shrink: 0;
  }

  .card-time-tag {
    font-size: 10px;
    background: rgba(15, 10, 16, 0.78);
    backdrop-filter: blur(6px);
    border: 1px solid rgba(255, 255, 255, 0.16);
    color: #FCF7FA;
    padding: 2px 6px;
    border-radius: 6px;
    white-space: nowrap;
    flex-shrink: 0;
    display: inline-flex;
    align-items: center;
    gap: 3px;
  }
  .card-time-tag .gpu-sub {
    color: #c4b5fd;
    font-size: 9.5px;
    font-weight: 600;
  }

  .card-toolbar {
    display: flex;
    align-items: center;
    gap: 1px;
    flex-shrink: 0;
    margin-left: 5px;
  }

  @media (hover: hover) {
    .gallery-card .card-toolbar {
      max-width: 0;
      margin-left: 0;
      overflow: hidden;
      opacity: 0;
      pointer-events: none;
      transition: opacity 0.16s ease;
    }
    .gallery-card:hover .card-toolbar,
    .gallery-card:focus-within .card-toolbar {
      max-width: 240px;
      margin-left: 4px;
      opacity: 1;
      pointer-events: auto;
    }
    .gallery-card:hover .card-title,
    .gallery-card:focus-within .card-title {
      text-overflow: clip;
      -webkit-mask-image: linear-gradient(90deg, #000 0%, #000 calc(100% - 16px), transparent 100%);
      mask-image: linear-gradient(90deg, #000 0%, #000 calc(100% - 16px), transparent 100%);
    }
  }

  @media (hover: none) {
    .gallery-card .card-title {
      text-overflow: clip;
      -webkit-mask-image: linear-gradient(90deg, #000 0%, #000 calc(100% - 16px), transparent 100%);
      mask-image: linear-gradient(90deg, #000 0%, #000 calc(100% - 16px), transparent 100%);
    }
  }

  .card-toolbar .toolbar-btn {
    background: transparent;
    border: none;
    color: #998094;
    width: 21px;
    height: 23px;
    border-radius: 5px;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
    padding: 0;
    font-size: 11px;
    opacity: 0.78;
    flex-shrink: 0;
  }

  .gallery-card:hover .card-toolbar .toolbar-btn {
    opacity: 0.92;
  }

  .card-toolbar .toolbar-btn:hover {
    background: rgba(255, 255, 255, 0.12);
    color: #ffffff;
    opacity: 1;
  }

  .card-toolbar .toolbar-btn.copied {
    color: #10b981;
    opacity: 1;
  }

  .card-toolbar .card-delete-btn:hover {
    color: #f87171;
    background: rgba(239, 68, 68, 0.18);
  }

  /* Sticky Composer */
  .studio-composer {
    position: absolute;
    bottom: 0;
    left: 0;
    right: 0;
    background: rgba(15, 10, 16, 0.96);
    border-top: 1px solid rgba(255, 255, 255, 0.1);
    backdrop-filter: blur(16px);
    padding: 14px 20px;
    z-index: 30;
    box-shadow: 0 -10px 30px rgba(0, 0, 0, 0.5);
  }

  .composer-controls-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 8px;
    gap: 6px;
    width: 100%;
    box-sizing: border-box;
  }

  .composer-controls-left {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
    flex-shrink: 1;
  }

  .pill-label-mobile {
    display: none;
  }

  .active-persona-pill {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    background: rgba(139, 92, 246, 0.18);
    border: 1px solid rgba(139, 92, 246, 0.4);
    padding: 0 10px;
    border-radius: 12px;
    height: 26px;
    font-size: 11px;
    font-weight: 500;
    font-family: inherit;
    box-sizing: border-box;
    white-space: nowrap;
  }

  .pill-name {
    font-weight: 500;
    font-size: 11px;
    font-family: inherit;
    color: #e9d5ff;
  }

  .pill-hint {
    color: #a78bfa;
    font-size: 11px;
    font-weight: 500;
  }

  .pill-clear-btn {
    background: transparent;
    border: none;
    color: #c4b5fd;
    cursor: pointer;
    font-size: 11px;
    padding: 0 2px;
  }

  .pick-persona-btn,
  .add-photo-pill {
    background: transparent;
    border: 1px solid rgba(167, 139, 250, 0.3);
    color: #c4b5fd;
    padding: 0 9px;
    height: 26px;
    border-radius: 12px;
    font-size: 11px;
    font-weight: 500;
    font-family: inherit;
    display: inline-flex;
    align-items: center;
    gap: 6px;
    cursor: pointer;
    box-sizing: border-box;
    white-space: nowrap;
    transition: all 0.15s ease;
    flex-shrink: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .pick-persona-btn:hover,
  .add-photo-pill:hover:not(:disabled) {
    background: rgba(139, 92, 246, 0.15);
    border-color: rgba(167, 139, 250, 0.5);
    color: #ede9fe;
  }

  .add-photo-pill.active {
    background: rgba(139, 92, 246, 0.2);
    border-color: rgba(167, 139, 250, 0.5);
    color: #ede9fe;
  }

  .add-photo-pill:disabled {
    opacity: 0.6;
    cursor: not-allowed;
  }

  .composer-controls-right {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    margin-left: auto;
    gap: 4px;
    flex-shrink: 0;
  }

  .toggle-history-pill,
  .toggle-tokens-btn,
  .toggle-sliders-btn {
    background: transparent;
    border: 1px solid rgba(167, 139, 250, 0.3);
    color: #c4b5fd;
    padding: 0 10px;
    border-radius: 12px;
    font-size: 11px;
    font-weight: 500;
    font-family: inherit;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    gap: 6px;
    height: 26px;
    box-sizing: border-box;
    white-space: nowrap;
    transition: all 0.15s ease;
  }

  .toggle-history-pill:hover,
  .toggle-tokens-btn:hover,
  .toggle-sliders-btn:hover {
    background: rgba(139, 92, 246, 0.15);
    border-color: rgba(167, 139, 250, 0.5);
    color: #ede9fe;
  }

  .toggle-history-pill.open,
  .toggle-history-pill.active,
  .toggle-tokens-btn.open,
  .toggle-tokens-btn.active,
  .toggle-sliders-btn.open {
    color: #c4b5fd;
    border-color: rgba(167, 139, 250, 0.3);
    background: rgba(139, 92, 246, 0.15);
  }

  .pill-chevron {
    font-size: 10px;
    line-height: 1;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    margin-left: 2px;
    color: currentColor;
    font-weight: 600;
  }

  /* Sliders Panel & Param Cards */
  .sliders-panel {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
    gap: 12px;
    padding: 12px 14px;
    background: rgba(0, 0, 0, 0.35);
    border-radius: 12px;
    border: 1px solid rgba(255, 255, 255, 0.05);
    margin-bottom: 12px;
  }

  .param-card {
    display: flex;
    flex-direction: column;
    justify-content: center;
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid rgba(255, 255, 255, 0.06);
    border-radius: 10px;
    padding: 10px 12px;
    min-height: 56px;
    min-width: 0;
    box-sizing: border-box;
  }

  .param-card.aspect-settings-card {
    justify-content: flex-start;
  }

  .card-header-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    width: 100%;
    margin-bottom: 6px;
    min-height: 18px;
  }

  .card-header-row .select-label {
    margin: 0;
    line-height: 1;
    font-size: 11px;
    color: #998094;
  }

  .advanced-settings-control {
    position: relative;
    display: inline-flex !important;
    flex-direction: row !important;
    align-items: center;
    min-width: 0 !important;
    width: auto !important;
    flex-shrink: 0;
    margin: 0 !important;
    padding: 0 !important;
    gap: 0 !important;
  }

  .composer-controls-right :global(.dropdown-control) {
    min-width: 0 !important;
    width: auto !important;
    display: inline-flex !important;
    flex-direction: row !important;
    gap: 0 !important;
    margin: 0 !important;
    padding: 0 !important;
  }

  .advanced-gear-btn {
    height: 26px;
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 0 9px;
    background: transparent;
    border: 1px solid rgba(167, 139, 250, 0.3);
    border-radius: 12px;
    color: #c4b5fd;
    font-size: 11px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
    white-space: nowrap;
    max-width: 100%;
    box-sizing: border-box;
  }

  .advanced-gear-btn svg {
    color: #a78bfa;
    flex-shrink: 0;
    transition: transform 0.3s cubic-bezier(0.34, 1.56, 0.64, 1);
  }

  .advanced-gear-btn:hover {
    background: rgba(255, 255, 255, 0.06);
    border-color: rgba(167, 139, 250, 0.3);
    color: #F7ECF2;
  }

  .advanced-gear-btn:hover svg {
    transform: rotate(45deg);
  }

  .advanced-gear-btn.active {
    background: rgba(139, 92, 246, 0.15);
    border-color: #8b5cf6;
    color: #ede9fe;
    box-shadow: 0 0 10px rgba(139, 92, 246, 0.25);
  }

  .advanced-gear-btn.active svg {
    transform: rotate(90deg);
    color: #c4b5fd;
  }

  .gear-summary-text {
    font-family: inherit;
    font-size: 11px;
    font-weight: 500;
  }


  .dropdown-popup-menu.advanced-popup {
    left: auto !important;
    right: 0 !important;
    bottom: calc(100% + 8px);
    width: 320px;
    max-width: calc(100vw - 32px);
    padding: 10px 0 6px 0;
    box-sizing: border-box;
  }

  .advanced-popup-body {
    display: flex;
    flex-direction: column;
    gap: 14px;
    padding: 8px 14px 10px;
  }

  .advanced-setting-row {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .setting-title-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-size: 12px;
  }

  .setting-name {
    font-weight: 500;
    color: #F2E5ED;
  }

  .setting-val-badge {
    font-size: 11px;
    font-weight: 700;
    color: #c4b5fd;
    background: rgba(139, 92, 246, 0.2);
    border: 1px solid rgba(139, 92, 246, 0.3);
    padding: 1px 7px;
    border-radius: 6px;
    font-variant-numeric: tabular-nums;
  }

  .advanced-slider {
    width: 100%;
    height: 5px;
    margin: 2px 0 0;
    accent-color: #8b5cf6;
    cursor: pointer;
  }

  .setting-subtext {
    display: flex;
    justify-content: space-between;
    font-size: 9px;
    color: #7D6376;
    line-height: 1;
    margin-top: 2px;
  }

  .slider-control {
    display: flex;
    flex-direction: column;
    gap: 4px;
    transition: opacity 0.2s ease;
  }

  .slider-control.disabled {
    opacity: 0.35;
    pointer-events: none;
  }

  .slider-header {
    display: flex;
    justify-content: space-between;
    font-size: 11px;
    color: #998094;
  }

  .slider-val {
    font-weight: 600;
    color: #c4b5fd;
  }

  .slider-input {
    width: 100%;
    accent-color: #8b5cf6;
    height: 4px;
    cursor: pointer;
  }

  .slider-ticks {
    display: flex;
    justify-content: space-between;
    font-size: 9px;
    color: #7D6376;
  }

  /* Model Pill Toggle Control */
  .model-pill-control {
    display: flex;
    flex-direction: column;
    gap: 4px;
    width: 100%;
    min-width: 0;
  }

  .model-pill-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-size: 11px;
    color: #998094;
    min-height: 18px;
    margin-bottom: 2px;
  }

  .model-pill-hint {
    font-size: 10px;
    font-weight: 600;
    color: #a78bfa;
    background: rgba(167, 139, 250, 0.12);
    padding: 1px 6px;
    border-radius: 6px;
    white-space: nowrap;
  }

  .pill-toggle-wrapper {
    display: flex;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.1);
    border-radius: 8px;
    padding: 2px;
    gap: 2px;
    width: 100%;
    min-width: 0;
    box-sizing: border-box;
  }

  .pill-btn {
    flex: 1 1 0;
    min-width: 0;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 3px;
    padding: 5px 3px;
    border-radius: 6px;
    border: none;
    background: transparent;
    color: #998094;
    font-size: 11px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
    box-sizing: border-box;
    overflow: hidden;
  }

  .pill-btn span:not(.pill-icon) {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    min-width: 0;
  }

  .pill-btn:hover {
    color: #F2E5ED;
    background: rgba(255, 255, 255, 0.05);
  }

  .pill-btn.active {
    background: linear-gradient(135deg, #7c3aed, #A855F7);
    color: #ffffff;
    font-weight: 600;
    box-shadow: 0 2px 6px rgba(124, 58, 237, 0.35);
  }

  .pill-icon {
    font-size: 12px;
    line-height: 1;
    flex-shrink: 0;
  }

  .select-control {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .select-label {
    font-size: 11px;
    color: #998094;
  }

  /* Dropdown Controls */
  .dropdown-control {
    position: relative;
    display: flex;
    flex-direction: column;
    gap: 6px;
    min-width: 135px;
  }

  .dropdown-trigger-btn {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.1);
    color: #E6D3DF;
    font-size: 12px;
    font-weight: 500;
    padding: 6px 10px;
    border-radius: 8px;
    cursor: pointer;
    transition: all 0.15s ease;
    height: 32px;
    width: 100%;
    box-sizing: border-box;
  }

  .dropdown-trigger-btn:hover {
    background: rgba(255, 255, 255, 0.1);
    color: #ffffff;
    border-color: rgba(255, 255, 255, 0.2);
  }

  .dropdown-trigger-btn.active {
    background: rgba(139, 92, 246, 0.2);
    border-color: #8b5cf6;
    color: #f3e8ff;
    box-shadow: 0 0 0 2px rgba(139, 92, 246, 0.25);
  }

  .dropdown-trigger-btn.has-style {
    background: rgba(139, 92, 246, 0.15);
    border-color: rgba(139, 92, 246, 0.45);
    color: #f3e8ff;
    font-weight: 600;
  }

  .dropdown-trigger-btn.has-style.active {
    border-color: #8b5cf6;
    box-shadow: 0 0 0 2px rgba(139, 92, 246, 0.25);
  }

  .dropdown-trigger-btn.has-archetype {
    background: rgba(236, 72, 153, 0.15);
    border-color: rgba(236, 72, 153, 0.5);
    color: #fdf2f8;
    font-weight: 600;
  }

  .dropdown-trigger-btn.has-archetype.active {
    border-color: #ec4899;
    box-shadow: 0 0 0 2px rgba(236, 72, 153, 0.25);
  }

  .trigger-label {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    font-size: 12px;
  }

  .chevron {
    flex-shrink: 0;
    color: #998094;
    transition: transform 0.2s ease;
  }

  .chevron.open {
    transform: rotate(180deg);
    color: inherit;
  }

  /* Floating Dropdown Popup Menu */
  .dropdown-popup-menu {
    position: absolute;
    bottom: calc(100% + 8px);
    left: 0;
    width: 300px;
    max-width: calc(100vw - 32px);
    background: #1A121D;
    border: 1px solid rgba(255, 255, 255, 0.15);
    border-radius: 12px;
    box-shadow: 0 16px 36px rgba(0, 0, 0, 0.6), 0 0 0 1px rgba(255, 255, 255, 0.05);
    backdrop-filter: blur(20px);
    z-index: 100;
    overflow: hidden;
    padding: 6px 0;
  }

  .archetype-popup {
    left: auto;
    right: 0;
  }

  @media (max-width: 600px) {
    .dropdown-popup-menu {
      left: 0;
      right: auto;
      width: calc(100vw - 40px);
    }
  }

  .popup-menu-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 6px 12px 6px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.08);
    font-size: 10px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: #998094;
    margin-bottom: 4px;
  }

  .popup-reset-link {
    background: transparent;
    border: none;
    color: #f472b6;
    font-size: 10px;
    cursor: pointer;
    padding: 0;
    text-transform: none;
    font-weight: 500;
  }

  .popup-reset-link:hover {
    text-decoration: underline;
  }

  .popup-options-list {
    max-height: 350px;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 2px;
    padding: 0 4px;
  }

  .popup-option-card {
    display: flex;
    flex-direction: column;
    border-radius: 8px;
    background: transparent;
    transition: background 0.12s ease;
    border: 1px solid transparent;
  }

  .popup-option-card:hover {
    background: rgba(255, 255, 255, 0.05);
  }

  .popup-option-card.selected {
    background: rgba(139, 92, 246, 0.18);
    border-color: rgba(139, 92, 246, 0.35);
  }

  .archetype-popup .popup-option-card.selected {
    background: rgba(236, 72, 153, 0.18);
    border-color: rgba(236, 72, 153, 0.35);
  }

  .popup-option-row {
    display: flex;
    align-items: center;
    justify-content: flex-start;
    gap: 10px;
    padding: 8px 10px 4px;
    background: transparent;
    border: none;
    color: #F7ECF2;
    cursor: pointer;
    text-align: left;
    width: 100%;
    box-sizing: border-box;
  }

  .option-inline-slider-row {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 2px 12px 10px 34px;
    box-sizing: border-box;
    min-height: 26px;
  }

  .inline-weight-slider {
    flex: 1;
    height: 6px;
    -webkit-appearance: none;
    appearance: none;
    background: rgba(255, 255, 255, 0.2);
    border-radius: 6px;
    outline: none;
    cursor: pointer;
    accent-color: #8b5cf6;
    margin: 0;
  }

  .inline-weight-slider::-webkit-slider-thumb {
    -webkit-appearance: none;
    appearance: none;
    width: 16px;
    height: 16px;
    border-radius: 50%;
    background: #8b5cf6;
    border: 2px solid #ffffff;
    cursor: pointer;
    box-shadow: 0 1px 4px rgba(0, 0, 0, 0.4);
  }

  .inline-weight-slider::-moz-range-thumb {
    width: 16px;
    height: 16px;
    border-radius: 50%;
    background: #8b5cf6;
    border: 2px solid #ffffff;
    cursor: pointer;
    box-shadow: 0 1px 4px rgba(0, 0, 0, 0.4);
  }

  .archetype-popup .inline-weight-slider {
    accent-color: #ec4899;
  }

  .archetype-popup .inline-weight-slider::-webkit-slider-thumb {
    background: #ec4899;
  }

  .archetype-popup .inline-weight-slider::-moz-range-thumb {
    background: #ec4899;
  }

  .inline-weight-val {
    font-size: 12px;
    font-weight: 700;
    color: #c4b5fd;
    min-width: 32px;
    text-align: right;
    font-variant-numeric: tabular-nums;
  }

  .archetype-popup .inline-weight-val {
    color: #f472b6;
  }

  .popup-option-item {
    display: flex;
    align-items: center;
    justify-content: flex-start;
    gap: 10px;
    padding: 7px 10px;
    border-radius: 8px;
    background: transparent;
    border: none;
    color: #F7ECF2;
    cursor: pointer;
    text-align: left;
    transition: background 0.12s ease;
    width: 100%;
    box-sizing: border-box;
  }

  .popup-option-item:hover {
    background: rgba(255, 255, 255, 0.08);
  }

  .popup-option-item.selected {
    background: rgba(139, 92, 246, 0.18);
    color: #f3e8ff;
  }

  .archetype-popup .popup-option-item.selected {
    background: rgba(236, 72, 153, 0.18);
    color: #fdf2f8;
  }

  .option-main {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
    text-align: left;
  }

  .option-label {
    font-size: 12px;
    font-weight: 600;
    line-height: 1.2;
  }

  .option-desc {
    font-size: 10px;
    color: #998094;
    line-height: 1.25;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .popup-option-item.selected .option-desc {
    color: #E6D3DF;
  }

  .option-check {
    font-size: 13px;
    font-weight: 700;
    color: #a78bfa;
    flex-shrink: 0;
    margin-left: auto;
  }

  .active-personas-wrap {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 6px;
  }

  .add-more-personas-btn {
    background: rgba(255, 255, 255, 0.05);
    border: 1px dashed rgba(255, 255, 255, 0.2);
    color: #998094;
    font-size: 11px;
    padding: 3px 8px;
    border-radius: 12px;
    cursor: pointer;
    transition: all 0.15s;
  }

  .add-more-personas-btn:hover {
    background: rgba(255, 255, 255, 0.12);
    color: #fff;
  }

  .go-gallery-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    background: rgba(139, 92, 246, 0.2);
    border: 1.5px solid #8b5cf6;
    color: #f3e8ff;
    padding: 8px 16px;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    box-shadow: 0 2px 8px rgba(139, 92, 246, 0.25);
    transition: all 0.2s;
  }

  .go-gallery-btn:hover {
    background: #8b5cf6;
    color: #ffffff;
    box-shadow: 0 4px 14px rgba(139, 92, 246, 0.4);
    transform: translateY(-1px);
  }

  .option-check-icon {
    width: 18px;
    height: 18px;
    border-radius: 4px;
    border: 1.5px solid rgba(255, 255, 255, 0.25);
    background: rgba(255, 255, 255, 0.04);
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    box-sizing: border-box;
    transition: all 0.15s ease;
    color: #ffffff;
  }

  .option-check-icon.radio {
    border-radius: 50%;
  }

  .option-check-icon.checked {
    background: #ec4899;
    border-color: #ec4899;
    color: #ffffff;
    box-shadow: 0 0 8px rgba(236, 72, 153, 0.35);
  }

  .option-check-icon .check-svg {
    display: block;
  }

  .popup-menu-footer {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 6px 12px;
    border-top: 1px solid rgba(255, 255, 255, 0.08);
    margin-top: 4px;
    background: rgba(0, 0, 0, 0.2);
  }

  .popup-count-hint {
    font-size: 11px;
    color: #998094;
  }

  .popup-done-btn {
    background: #8b5cf6;
    color: #fff;
    border: none;
    border-radius: 6px;
    font-size: 11px;
    font-weight: 600;
    padding: 4px 12px;
    cursor: pointer;
    transition: background 0.15s;
  }

  .popup-done-btn:hover {
    background: #7c3aed;
  }

  .aspect-chips {
    display: flex;
    align-items: center;
    gap: 4px;
    height: 28px;
    width: 100%;
    box-sizing: border-box;
  }

  .aspect-chip {
    flex: 1 1 0;
    min-width: 0;
    height: 28px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    box-sizing: border-box;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.08);
    color: #E6D3DF;
    font-size: 11px;
    padding: 0 4px;
    border-radius: 8px;
    cursor: pointer;
    transition: all 0.15s;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .aspect-chip.active {
    background: rgba(139, 92, 246, 0.25);
    border-color: #8b5cf6;
    color: #f3e8ff;
    font-weight: 600;
  }

  /* Active Modifiers Bar */
  .active-modifiers-bar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
    margin-bottom: 8px;
    padding: 0 2px;
  }

  .active-pill {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 11px;
    font-weight: 500;
    padding: 3px 8px;
    border-radius: 12px;
    line-height: 1.2;
    transition: all 0.2s ease;
  }

  .persona-pill {
    background: rgba(139, 92, 246, 0.2);
    border: 1px solid rgba(139, 92, 246, 0.4);
    color: #ddd6fe;
  }

  .style-pill {
    background: rgba(236, 72, 153, 0.2);
    border: 1px solid rgba(236, 72, 153, 0.35);
    color: #FBCFE8;
  }

  .archetype-pill {
    background: rgba(236, 72, 153, 0.2);
    border: 1px solid rgba(236, 72, 153, 0.4);
    color: #fbcfe8;
  }

  .pill-remove-btn {
    background: transparent;
    border: none;
    color: inherit;
    opacity: 0.7;
    cursor: pointer;
    font-size: 10px;
    padding: 0 2px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
  }

  .pill-remove-btn:hover {
    opacity: 1;
  }


  /* Tokens Preview Panel */
  .tokens-preview-panel {
    background: rgba(15, 10, 16, 0.95);
    border: 1px solid rgba(153, 128, 148, 0.2);
    border-radius: 10px;
    padding: 10px 12px;
    margin-bottom: 8px;
    font-size: 11px;
    line-height: 1.5;
    box-shadow: 0 4px 16px rgba(0, 0, 0, 0.4);
  }

  .tokens-preview-header {
    margin-bottom: 6px;
  }

  .preview-title {
    font-weight: 600;
    color: #c4b5fd;
    font-size: 11px;
    letter-spacing: 0.2px;
  }

  .tokens-preview-body {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .token-row {
    display: flex;
    align-items: baseline;
    gap: 6px;
    word-break: break-word;
  }

  .token-badge {
    display: inline-block;
    flex-shrink: 0;
    font-size: 10px;
    font-weight: 600;
    padding: 1px 6px;
    border-radius: 6px;
  }

  .token-badge-persona {
    background: rgba(139, 92, 246, 0.25);
    color: #c4b5fd;
  }

  .token-badge-ref {
    background: rgba(236, 72, 153, 0.25);
    color: #F9A8D4;
    border: 1px solid rgba(236, 72, 153, 0.4);
  }

  .token-badge-style {
    background: rgba(236, 72, 153, 0.25);
    color: #F9A8D4;
  }

  .token-badge-archetype {
    background: rgba(236, 72, 153, 0.25);
    color: #f472b6;
  }

  .token-badge-user {
    background: rgba(34, 197, 94, 0.25);
    color: #86efac;
  }

  .token-text {
    color: #E6D3DF;
    font-size: 11px;
  }

  /* Composer Input */
  .composer-input-row {
    display: flex;
    gap: 12px;
    align-items: flex-end;
  }

  .prompt-textarea {
    flex: 1;
    display: block;
    margin: 0;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 10px;
    color: #fff;
    padding: 10px 14px;
    font-size: 14px;
    font-family: inherit;
    resize: none;
    line-height: 20px;
    height: 42px;
    min-height: 42px;
    max-height: 126px;
    box-sizing: border-box;
    outline: none;
    overflow-y: hidden;
    transition: border-color 0.2s;
  }

  .prompt-textarea:focus {
    border-color: #8b5cf6;
  }

  .composer-actions {
    display: flex;
    gap: 8px;
    align-items: center;
  }

  /* Prompts Popup */
  .prompts-popup {
    position: absolute;
    bottom: calc(100% + 10px);
    left: 20px;
    right: 20px;
    max-width: 820px;
    margin: 0 auto;
    background: #1A121D;
    border: 1px solid rgba(255, 255, 255, 0.15);
    border-radius: 16px;
    box-shadow: 0 16px 40px rgba(0, 0, 0, 0.5);
    backdrop-filter: blur(24px);
    z-index: 60;
    display: flex;
    flex-direction: column;
    overflow: hidden;
    animation: popIn 0.2s cubic-bezier(0.16, 1, 0.3, 1);
  }

  @keyframes popIn {
    from {
      opacity: 0;
      transform: translateY(8px) scale(0.96);
    }
    to {
      opacity: 1;
      transform: translateY(0) scale(1);
    }
  }

  .prompts-popup-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 16px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.08);
  }

  .prompts-header-title {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.85rem;
    font-weight: 600;
    color: #ffffff;
  }

  .prompts-popup-close {
    background: transparent;
    border: none;
    color: #998094;
    cursor: pointer;
    padding: 4px;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 6px;
    transition: color 0.15s ease;
  }

  .prompts-popup-close:hover {
    color: #ffffff;
  }

  .prompts-search-wrap {
    padding: 8px 14px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.08);
  }

  .prompts-search-input {
    width: 100%;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.1);
    border-radius: 8px;
    padding: 6px 12px;
    font-size: 0.82rem;
    color: #ffffff;
    outline: none;
    box-sizing: border-box;
  }

  .prompts-search-input:focus {
    border-color: #8b5cf6;
  }

  .prompts-popup-list {
    max-height: 280px;
    overflow-y: auto;
    padding: 6px;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .prompt-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 8px 12px;
    border-radius: 10px;
    cursor: pointer;
    transition: background 0.15s ease;
  }

  .prompt-item:hover {
    background: rgba(255, 255, 255, 0.08);
  }

  .prompt-item-content {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .prompt-item-text {
    font-size: 0.86rem;
    color: #ffffff;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .prompt-item-meta {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.72rem;
    color: #998094;
  }

  .prompt-style-badge {
    background: rgba(139, 92, 246, 0.2);
    color: #c084fc;
    padding: 1px 6px;
    border-radius: 4px;
    font-weight: 500;
    text-transform: capitalize;
  }

  .prompt-time {
    font-size: 0.72rem;
    color: #998094;
  }

  .prompt-delete-btn {
    background: transparent;
    border: none;
    color: #998094;
    cursor: pointer;
    padding: 6px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    opacity: 0;
    transition: all 0.15s ease;
  }

  .prompt-item:hover .prompt-delete-btn {
    opacity: 1;
  }

  .prompt-delete-btn:hover {
    color: #ef4444;
    background: rgba(239, 68, 68, 0.15);
  }

  .prompts-empty-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 32px 16px;
    color: #998094;
    font-size: 0.85rem;
  }


  .generate-submit-btn {
    height: 42px;
    padding: 0 20px;
    border-radius: 10px;
    background: linear-gradient(135deg, #7c3aed 0%, #ec4899 100%);
    border: none;
    color: #fff;
    font-weight: 600;
    font-size: 14px;
    display: flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
    box-shadow: 0 4px 16px rgba(124, 58, 237, 0.4);
    transition: all 0.2s;
  }

  .generate-submit-btn:hover:not(:disabled) {
    transform: translateY(-1px);
    box-shadow: 0 6px 20px rgba(124, 58, 237, 0.5);
  }

  .generate-submit-btn.busy {
    background: #352339;
    cursor: not-allowed;
  }

  .spinner {
    width: 16px;
    height: 16px;
    border: 2px solid rgba(255, 255, 255, 0.3);
    border-top-color: #fff;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  /* Modal */
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.7);
    backdrop-filter: blur(8px);
    z-index: 100;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 20px;
  }

  .persona-modal-card {
    background: #1A121D;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 16px;
    width: 100%;
    max-width: 540px;
    max-height: 90vh;
    overflow-y: auto;
    box-shadow: 0 20px 50px rgba(0, 0, 0, 0.6);
  }

  .modal-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 16px 20px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.08);
  }

  .modal-header h3 {
    margin: 0;
    font-size: 17px;
  }

  .ai-factory-title {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .ai-icon {
    font-size: 20px;
  }

  .modal-close-btn {
    background: transparent;
    border: none;
    color: #998094;
    cursor: pointer;
    font-size: 16px;
  }

  .modal-body {
    padding: 20px;
    display: flex;
    flex-direction: column;
    gap: 16px;
  }

  .modal-field {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .modal-field label {
    font-size: 12px;
    font-weight: 600;
    color: #E6D3DF;
  }

  .modal-input, .modal-textarea {
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    color: #fff;
    padding: 8px 12px;
    font-size: 13px;
    font-family: inherit;
    outline: none;
  }

  .modal-input.small, .modal-textarea.small {
    padding: 6px 10px;
    font-size: 12px;
  }

  .modal-textarea {
    resize: vertical;
  }

  .modal-input:focus, .modal-textarea:focus {
    border-color: #8b5cf6;
  }

  .field-hint {
    font-size: 11px;
    color: #7D6376;
  }

  .avatar-upload-row {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .avatar-generator-card {
    display: flex;
    align-items: flex-start;
    gap: 14px;
    padding: 12px;
    background: rgba(255, 255, 255, 0.03);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 12px;
  }

  .avatar-preview-container {
    flex-shrink: 0;
  }

  .avatar-preview-box {
    width: 68px;
    height: 68px;
    border-radius: 12px;
    object-fit: cover;
    border: 1.5px solid rgba(139, 92, 246, 0.5);
    display: block;
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
  }

  .avatar-loading-box {
    width: 68px;
    height: 68px;
    border-radius: 12px;
    background: rgba(139, 92, 246, 0.1);
    border: 1.5px dashed rgba(139, 92, 246, 0.6);
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    font-size: 10px;
    color: #c084fc;
    gap: 4px;
  }

  .avatar-spinner {
    font-size: 18px;
    animation: avatarSpin 1.5s linear infinite;
  }

  @keyframes avatarSpin {
    0% { transform: rotate(0deg); }
    100% { transform: rotate(360deg); }
  }

  .avatar-empty-box {
    width: 68px;
    height: 68px;
    border-radius: 12px;
    background: rgba(255, 255, 255, 0.05);
    border: 1px dashed rgba(255, 255, 255, 0.15);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 26px;
    color: #7D6376;
  }

  .avatar-actions-col {
    display: flex;
    flex-direction: column;
    gap: 10px;
    flex: 1;
    min-width: 0;
  }

  .avatar-controls-row {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }

  .archetype-select {
    flex: 1;
    min-width: 140px;
    background: rgba(255, 255, 255, 0.06);
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 8px;
    color: #F7ECF2;
    padding: 7px 10px;
    font-size: 12px;
    outline: none;
    cursor: pointer;
  }

  .archetype-select:focus {
    border-color: #8b5cf6;
  }

  .archetype-select option {
    background: #1A121D;
    color: #fff;
  }

  .generate-avatar-btn {
    background: linear-gradient(135deg, #7c3aed, #A855F7);
    color: #ffffff;
    border: none;
    border-radius: 8px;
    padding: 7px 14px;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
    transition: all 0.15s ease;
    box-shadow: 0 2px 8px rgba(124, 58, 237, 0.3);
  }

  .generate-avatar-btn:hover:not(:disabled) {
    background: linear-gradient(135deg, #8b5cf6, #9333EA);
    transform: translateY(-1px);
    box-shadow: 0 4px 12px rgba(124, 58, 237, 0.4);
  }

  .generate-avatar-btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
    transform: none;
  }

  .avatar-manual-upload {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .upload-hint {
    font-size: 11px;
    color: #998094;
  }

  .modal-label-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }

  .vision-inline-link {
    background: transparent;
    border: none;
    color: #a78bfa;
    font-size: 11px;
    font-weight: 500;
    cursor: pointer;
    padding: 0;
    transition: color 0.15s;
    text-decoration: underline;
    text-underline-offset: 2px;
  }

  .vision-inline-link:hover:not(:disabled) {
    color: #c084fc;
  }

  .vision-inline-link:disabled {
    opacity: 0.6;
    cursor: wait;
    text-decoration: none;
  }

  .vision-analyze-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    background: rgba(139, 92, 246, 0.15);
    border: 1px solid rgba(139, 92, 246, 0.4);
    color: #c084fc;
    padding: 6px 12px;
    border-radius: 8px;
    font-size: 11px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
    margin-top: 4px;
    width: fit-content;
  }

  .vision-analyze-btn:hover:not(:disabled) {
    background: rgba(139, 92, 246, 0.28);
    border-color: #a855f7;
    color: #ffffff;
  }

  .vision-analyze-btn:disabled {
    opacity: 0.6;
    cursor: wait;
  }

  .file-input {
    font-size: 12px;
    color: #998094;
  }

  /* AI Factory Archetypes */
  .archetype-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 8px;
  }

  .archetype-card {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    padding: 10px 12px;
    background: rgba(255, 255, 255, 0.04);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 10px;
    cursor: pointer;
    text-align: left;
    transition: all 0.15s;
  }

  .archetype-card:hover {
    background: rgba(255, 255, 255, 0.08);
    border-color: rgba(167, 139, 250, 0.3);
  }

  .archetype-card.active {
    background: rgba(139, 92, 246, 0.15);
    border-color: #8b5cf6;
    box-shadow: 0 0 12px rgba(139, 92, 246, 0.25);
  }

  .arch-name {
    font-size: 13px;
    font-weight: 600;
    color: #fff;
    margin-bottom: 2px;
  }

  .arch-hint {
    font-size: 11px;
    color: #998094;
    line-height: 1.3;
  }

  .ai-factory-action-row {
    margin-top: 4px;
  }

  .generate-ai-persona-btn {
    width: 100%;
    padding: 10px 16px;
    border-radius: 10px;
    background: linear-gradient(135deg, #7c3aed 0%, #ec4899 100%);
    border: none;
    color: #fff;
    font-weight: 600;
    font-size: 13px;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    box-shadow: 0 4px 14px rgba(124, 58, 237, 0.35);
  }

  .generate-ai-persona-btn:hover:not(:disabled) {
    opacity: 0.95;
    transform: translateY(-1px);
  }

  .generated-persona-card {
    display: flex;
    gap: 14px;
    padding: 12px;
    background: rgba(0, 0, 0, 0.3);
    border: 1px solid rgba(255, 255, 255, 0.1);
    border-radius: 12px;
    margin-top: 6px;
  }

  .gen-avatar-col {
    width: 90px;
    flex-shrink: 0;
  }

  .gen-avatar-img {
    width: 90px;
    height: 115px;
    object-fit: cover;
    border-radius: 8px;
    border: 1px solid rgba(255, 255, 255, 0.15);
  }

  .gen-avatar-empty {
    width: 90px;
    height: 115px;
    display: flex;
    align-items: center;
    justify-content: center;
    background: rgba(255, 255, 255, 0.05);
    border-radius: 8px;
    font-size: 32px;
  }

  .gen-info-col {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .gen-field {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .gen-label {
    font-size: 11px;
    color: #998094;
    font-weight: 600;
  }

  .modal-footer {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 10px;
    padding: 14px 20px;
    background: rgba(0, 0, 0, 0.2);
    border-top: 1px solid rgba(255, 255, 255, 0.08);
  }

  .cancel-btn {
    background: transparent;
    border: 1px solid rgba(255, 255, 255, 0.1);
    color: #998094;
    padding: 7px 14px;
    border-radius: 8px;
    font-size: 13px;
    cursor: pointer;
  }

  .save-btn {
    background: linear-gradient(135deg, #A855F7 0%, #8b5cf6 100%);
    border: none;
    color: #fff;
    padding: 7px 18px;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
  }

  .save-btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  /* =========================================================================
     LIGHT THEME OVERRIDES (matches rest of SVAR / OnMyChat)
     ========================================================================= */
  .krea-studio-root.light-theme {
    background: #F7ECF2;
    color: #0F0A10;
  }

  /* Header */
  .krea-studio-root.light-theme .studio-header {
    background: #FCF7FA;
    border-bottom: 1px solid #F2E5ED;
  }

  .krea-studio-root.light-theme .sidebar-toggle-btn {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .sidebar-toggle-btn:hover {
    background: #F2E5ED;
    color: #0F0A10;
  }

  .krea-studio-root.light-theme .turbo-pill {
    background: #ede9fe;
    color: #6d28d9;
    border-color: #ddd6fe;
  }

  .krea-studio-root.light-theme .header-subtitle {
    color: #7D6376;
  }

  .krea-studio-root.light-theme .header-tabs {
    background: #F2E5ED;
    border-color: #E6D3DF;
  }

  .krea-studio-root.light-theme .tab-btn {
    color: #664D5F;
  }
  .krea-studio-root.light-theme .tab-btn:hover {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .tab-btn.active {
    background: #ffffff;
    color: #0F0A10;
    box-shadow: 0 1px 4px rgba(0, 0, 0, 0.12);
  }
  .krea-studio-root.light-theme .tab-btn .badge {
    background: #F2E5ED;
    color: #664D5F;
  }
  .krea-studio-root.light-theme .tab-btn.active .badge {
    background: #F7ECF2;
    color: #0F0A10;
  }

  .krea-studio-root.light-theme .icon-btn {
    border-color: #E6D3DF;
    color: #7D6376;
  }
  .krea-studio-root.light-theme .icon-btn:hover {
    background: #F2E5ED;
    color: #0F0A10;
  }

  /* Main & Sections */
  .krea-studio-root.light-theme .section-desc h3 {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .section-desc p {
    color: #7D6376;
  }

  /* Personas */
  .krea-studio-root.light-theme .persona-card {
    background: #ffffff;
    border-color: rgba(0, 0, 0, 0.08);
    box-shadow: 0 2px 10px rgba(0, 0, 0, 0.04);
  }
  .krea-studio-root.light-theme .persona-card:hover {
    background: #ffffff;
    border-color: #8b5cf6;
    box-shadow: 0 6px 18px rgba(139, 92, 246, 0.15);
  }
  .krea-studio-root.light-theme .persona-card.selected {
    border-color: #8b5cf6;
    background: #f5f3ff;
    box-shadow: 0 0 0 2px rgba(139, 92, 246, 0.3), 0 6px 18px rgba(139, 92, 246, 0.12);
  }
  .krea-studio-root.light-theme .persona-name {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .persona-title-chip {
    color: #7c3aed;
    background: #ede9fe;
  }
  .krea-studio-root.light-theme .persona-passport {
    color: #664D5F;
  }
  .krea-studio-root.light-theme .persona-actions {
    border-top-color: #F7ECF2;
  }
  .krea-studio-root.light-theme .card-action-btn {
    background: #F7ECF2;
    color: #352339;
  }
  .krea-studio-root.light-theme .card-action-btn:hover {
    background: #F2E5ED;
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .card-action-btn.select-btn.active {
    background: #8b5cf6;
    color: #ffffff;
  }
  .krea-studio-root.light-theme .card-action-btn.danger:hover {
    background: #fee2e2;
    color: #dc2626;
  }
  .krea-studio-root.light-theme .go-gallery-btn {
    background: #ede9fe;
    border: 1.5px solid #8b5cf6;
    color: #5b21b6;
    box-shadow: 0 1px 4px rgba(139, 92, 246, 0.15);
  }
  .krea-studio-root.light-theme .go-gallery-btn:hover {
    background: #7c3aed;
    border-color: #6d28d9;
    color: #ffffff;
    box-shadow: 0 4px 14px rgba(109, 40, 217, 0.3);
  }
  .krea-studio-root.light-theme .add-more-personas-btn {
    background: #FCF7FA;
    border-color: #E6D3DF;
    color: #7D6376;
  }
  .krea-studio-root.light-theme .add-more-personas-btn:hover {
    background: #ede9fe;
    border-color: #8b5cf6;
    color: #6d28d9;
  }
  .krea-studio-root.light-theme .add-new-slot {
    border-color: #E6D3DF;
    background: #ffffff;
  }
  .krea-studio-root.light-theme .add-new-slot:hover {
    border-color: #8b5cf6;
    background: #faf5ff;
  }
  .krea-studio-root.light-theme .add-slot-circle {
    background: #ede9fe;
    color: #7c3aed;
  }
  .krea-studio-root.light-theme .add-slot-label {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .add-slot-hint {
    color: #7D6376;
  }

  /* Gallery */
  .krea-studio-root.light-theme .empty-gallery h3 {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .empty-gallery p {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .sug-pill {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #6d28d9;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.05);
  }
  .krea-studio-root.light-theme .sug-pill:hover {
    background: #ede9fe;
    border-color: #a78bfa;
    color: #5b21b6;
  }
  .krea-studio-root.light-theme .gallery-card {
    background: #ffffff;
    border-color: rgba(0, 0, 0, 0.08);
    box-shadow: 0 3px 12px rgba(0, 0, 0, 0.05);
  }
  .krea-studio-root.light-theme .gallery-card:hover {
    border-color: #8b5cf6;
    box-shadow: 0 6px 20px rgba(0, 0, 0, 0.1);
  }
  .krea-studio-root.light-theme .card-footer-info {
    background: #ffffff;
    border-top: 1px solid #F7ECF2;
  }
  .krea-studio-root.light-theme .card-toolbar .toolbar-btn {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .card-toolbar .toolbar-btn:hover {
    background: rgba(168, 85, 247, 0.1);
    color: #9333EA;
  }
  .krea-studio-root.light-theme .card-toolbar .card-delete-btn:hover {
    background: rgba(239, 68, 68, 0.12);
    color: #dc2626;
  }
  .krea-studio-root.light-theme .card-title {
    color: #1A121D;
  }
  .krea-studio-root.light-theme .placeholder-footer .persona-tag {
    background: #ede9fe;
    border-color: rgba(109, 40, 217, 0.2);
    color: #6d28d9;
  }

  /* Composer & Bottom Panel */
  .krea-studio-root.light-theme .studio-composer {
    background: linear-gradient(180deg, rgba(247, 236, 242, 0) 0%, rgba(247, 236, 242, 0.98) 25%);
    border-top: none;
    box-shadow: none;
  }
  .krea-studio-root.light-theme .active-persona-pill {
    background: #ede9fe;
    border-color: #c4b5fd;
  }
  .krea-studio-root.light-theme .pill-name {
    color: #5b21b6;
  }
  .krea-studio-root.light-theme .pill-hint {
    color: #6d28d9;
  }
  .krea-studio-root.light-theme .pill-clear-btn {
    color: #7c3aed;
  }
  .krea-studio-root.light-theme .pick-persona-btn,
  .krea-studio-root.light-theme .add-photo-pill,
  .krea-studio-root.light-theme .toggle-history-pill,
  .krea-studio-root.light-theme .toggle-tokens-btn,
  .krea-studio-root.light-theme .toggle-sliders-btn,
  .krea-studio-root.light-theme .advanced-gear-btn {
    background: #ffffff;
    border: 1px solid #c4b5fd;
    color: #6d28d9;
    box-shadow: 0 1px 3px rgba(109, 40, 217, 0.06);
  }
  .krea-studio-root.light-theme .pick-persona-btn:hover,
  .krea-studio-root.light-theme .add-photo-pill:hover:not(:disabled),
  .krea-studio-root.light-theme .toggle-history-pill:hover,
  .krea-studio-root.light-theme .toggle-tokens-btn:hover,
  .krea-studio-root.light-theme .toggle-sliders-btn:hover,
  .krea-studio-root.light-theme .advanced-gear-btn:hover {
    background: #f5f3ff;
    color: #5b21b6;
    border-color: #a78bfa;
  }
  .krea-studio-root.light-theme .add-photo-pill.active,
  .krea-studio-root.light-theme .toggle-history-pill.open,
  .krea-studio-root.light-theme .toggle-history-pill.active,
  .krea-studio-root.light-theme .toggle-tokens-btn.open,
  .krea-studio-root.light-theme .toggle-tokens-btn.active,
  .krea-studio-root.light-theme .toggle-sliders-btn.open,
  .krea-studio-root.light-theme .advanced-gear-btn.active {
    color: #6d28d9;
    border-color: #c4b5fd;
    background: #f5f3ff;
  }
  .krea-studio-root.light-theme .sliders-panel {
    background: #ffffff;
    border-color: #E6D3DF;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.06);
  }
  .krea-studio-root.light-theme .slider-header,
  .krea-studio-root.light-theme .select-label {
    color: #664D5F;
  }
  .krea-studio-root.light-theme .slider-val {
    color: #7c3aed;
  }
  .krea-studio-root.light-theme .slider-ticks {
    color: #998094;
  }
  .krea-studio-root.light-theme .dropdown-trigger-btn {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #1A121D;
  }

  .krea-studio-root.light-theme .dropdown-trigger-btn:hover {
    background: #FCF7FA;
    border-color: #998094;
  }

  .krea-studio-root.light-theme .dropdown-trigger-btn.active {
    background: #ede9fe;
    border-color: #8b5cf6;
    color: #5b21b6;
    box-shadow: 0 0 0 2px rgba(139, 92, 246, 0.15);
  }

  .krea-studio-root.light-theme .dropdown-trigger-btn.has-style {
    background: #ede9fe;
    border-color: #a78bfa;
    color: #5b21b6;
  }

  .krea-studio-root.light-theme .dropdown-trigger-btn.has-archetype {
    background: #fdf2f8;
    border-color: #f472b6;
    color: #9d174d;
  }

  .krea-studio-root.light-theme .dropdown-popup-menu {
    background: #ffffff;
    border-color: #F2E5ED;
    box-shadow: 0 16px 36px rgba(0, 0, 0, 0.12), 0 0 0 1px rgba(0, 0, 0, 0.05);
  }

  .krea-studio-root.light-theme .popup-menu-header {
    border-bottom-color: #F7ECF2;
    color: #664D5F;
  }

  .krea-studio-root.light-theme .popup-option-card {
    background: transparent;
    border-color: transparent;
  }

  .krea-studio-root.light-theme .popup-option-card:hover {
    background: #FCF7FA;
  }

  .krea-studio-root.light-theme .popup-option-card.selected {
    background: #f5f3ff;
    border-color: #ddd6fe;
  }

  .krea-studio-root.light-theme .archetype-popup .popup-option-card.selected {
    background: #fdf2f8;
    border-color: #fbcfe8;
  }

  .krea-studio-root.light-theme .popup-option-row {
    color: #0F0A10;
  }

  .krea-studio-root.light-theme .popup-option-item {
    color: #0F0A10;
  }

  .krea-studio-root.light-theme .popup-option-item:hover {
    background: #FCF7FA;
  }

  .krea-studio-root.light-theme .popup-option-item.selected {
    background: #ede9fe;
    color: #5b21b6;
  }

  .krea-studio-root.light-theme .archetype-popup .popup-option-item.selected {
    background: #fdf2f8;
    color: #9d174d;
  }

  .krea-studio-root.light-theme .option-label {
    color: #0F0A10;
    font-weight: 600;
  }

  .krea-studio-root.light-theme .popup-option-card.selected .option-label {
    color: #4c1d95;
    font-weight: 700;
  }

  .krea-studio-root.light-theme .archetype-popup .popup-option-card.selected .option-label {
    color: #831843;
    font-weight: 700;
  }

  .krea-studio-root.light-theme .option-desc {
    color: #7D6376;
  }

  .krea-studio-root.light-theme .popup-option-card.selected .option-desc {
    color: #6d28d9;
  }

  .krea-studio-root.light-theme .archetype-popup .popup-option-card.selected .option-desc {
    color: #be185d;
  }

  .krea-studio-root.light-theme .inline-weight-slider {
    background: #E6D3DF;
    accent-color: #7c3aed;
  }

  .krea-studio-root.light-theme .inline-weight-slider::-webkit-slider-thumb {
    background: #7c3aed;
    border: 2px solid #ffffff;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.25);
  }

  .krea-studio-root.light-theme .inline-weight-slider::-moz-range-thumb {
    background: #7c3aed;
    border: 2px solid #ffffff;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.25);
  }

  .krea-studio-root.light-theme .archetype-popup .inline-weight-slider {
    accent-color: #db2777;
  }

  .krea-studio-root.light-theme .archetype-popup .inline-weight-slider::-webkit-slider-thumb {
    background: #db2777;
    border: 2px solid #ffffff;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.25);
  }

  .krea-studio-root.light-theme .archetype-popup .inline-weight-slider::-moz-range-thumb {
    background: #db2777;
    border: 2px solid #ffffff;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.25);
  }

  .krea-studio-root.light-theme .inline-weight-val {
    color: #5b21b6;
    font-weight: 700;
  }

  .krea-studio-root.light-theme .archetype-popup .inline-weight-val {
    color: #9d174d;
    font-weight: 700;
  }

  .krea-studio-root.light-theme .popup-menu-footer {
    border-top-color: #F7ECF2;
    background: #ffffff;
  }

  .krea-studio-root.light-theme .popup-count-hint {
    color: #664D5F;
  }

  .krea-studio-root.light-theme .detail-val,
  .krea-studio-root.light-theme .slider-val {
    color: #6d28d9;
    font-weight: 700;
  }

  .krea-studio-root.light-theme .detail-header,
  .krea-studio-root.light-theme .slider-header {
    color: #664D5F;
  }

  .krea-studio-root.light-theme .detail-ticks,
  .krea-studio-root.light-theme .slider-ticks {
    color: #7D6376;
  }

  .krea-studio-root.light-theme .slider-input {
    background: #E6D3DF;
  }

  .krea-studio-root.light-theme .option-check {
    color: #7c3aed;
  }

  .krea-studio-root.light-theme .archetype-popup .option-check {
    color: #db2777;
  }

  .krea-studio-root.light-theme .option-check-icon {
    border-color: #E6D3DF;
    background: #ffffff;
  }

  .krea-studio-root.light-theme .option-check-icon.checked {
    background: #7c3aed;
    border-color: #7c3aed;
    color: #ffffff;
    box-shadow: 0 1px 3px rgba(124, 58, 237, 0.25);
  }

  .krea-studio-root.light-theme .archetype-popup .option-check-icon.checked {
    background: #ec4899;
    border-color: #ec4899;
    color: #ffffff;
    box-shadow: 0 1px 3px rgba(236, 72, 153, 0.25);
  }

  .krea-studio-root.light-theme .aspect-chip {
    background: #F7ECF2;
    border-color: #F2E5ED;
    color: #664D5F;
  }
  .krea-studio-root.light-theme .aspect-chip:hover {
    background: #F2E5ED;
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .aspect-chip.active {
    background: #ede9fe;
    border-color: #8b5cf6;
    color: #6d28d9;
  }
  .krea-studio-root.light-theme .pill-toggle-wrapper {
    background: #F7ECF2;
    border-color: #E6D3DF;
  }
  .krea-studio-root.light-theme .pill-btn {
    color: #664D5F;
  }
  .krea-studio-root.light-theme .pill-btn:hover {
    color: #0F0A10;
    background: #F2E5ED;
  }
  .krea-studio-root.light-theme .pill-btn.active {
    background: linear-gradient(135deg, #7c3aed, #A855F7);
    color: #ffffff;
    box-shadow: 0 2px 6px rgba(124, 58, 237, 0.25);
  }
  .krea-studio-root.light-theme .model-pill-hint {
    color: #6d28d9;
  }

  .krea-studio-root.light-theme .advanced-gear-btn svg {
    color: #7c3aed;
  }
  .krea-studio-root.light-theme .advanced-gear-btn:hover {
    background: #ede9fe;
    border-color: #8b5cf6;
    color: #4c1d95;
  }
  .krea-studio-root.light-theme .advanced-gear-btn.active {
    background: #ede9fe;
    border-color: #7c3aed;
    color: #5b21b6;
    box-shadow: 0 0 0 2px rgba(124, 58, 237, 0.15);
  }
  .krea-studio-root.light-theme .advanced-popup {
    background: #ffffff;
    border-color: #F2E5ED;
    box-shadow: 0 16px 36px rgba(0, 0, 0, 0.12), 0 0 0 1px rgba(0, 0, 0, 0.05);
  }
  .krea-studio-root.light-theme .advanced-popup .setting-name {
    color: #1A121D;
  }
  .krea-studio-root.light-theme .advanced-popup .setting-val-badge {
    color: #6d28d9;
    background: #ede9fe;
    border-color: #c4b5fd;
  }
  .krea-studio-root.light-theme .advanced-popup .setting-subtext {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .advanced-popup .advanced-slider {
    background: #E6D3DF;
    accent-color: #7c3aed;
  }
  .krea-studio-root.light-theme .param-card {
    background: #FCF7FA;
    border-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .persona-pill {
    background: #ede9fe;
    border-color: #c4b5fd;
    color: #5b21b6;
  }
  .krea-studio-root.light-theme .style-pill {
    background: #FCE7F3;
    border-color: #F9A8D4;
    color: #9D174D;
  }
  .krea-studio-root.light-theme .archetype-pill {
    background: #fce7f3;
    border-color: #f472b6;
    color: #9d174d;
  }
  .krea-studio-root.light-theme .tokens-preview-panel {
    background: #ffffff;
    border-color: #E6D3DF;
    box-shadow: 0 4px 16px rgba(0, 0, 0, 0.08);
  }
  .krea-studio-root.light-theme .preview-title {
    color: #6d28d9;
  }
  .krea-studio-root.light-theme .token-text {
    color: #352339;
  }
  .krea-studio-root.light-theme .prompt-textarea {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #0F0A10;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.04);
  }
  .krea-studio-root.light-theme .prompt-textarea::placeholder {
    color: #998094;
  }
  .krea-studio-root.light-theme .prompt-textarea:focus {
    border-color: #8b5cf6;
    box-shadow: 0 0 0 3px rgba(139, 92, 246, 0.15);
  }
  .krea-studio-root.light-theme .attached-refs-bar {
    background: #ffffff;
    border-color: #F2E5ED;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.04);
  }
  .krea-studio-root.light-theme .refs-label {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .ref-chip {
    background: #F7ECF2;
    border-color: #E6D3DF;
    color: #1A121D;
  }
  .krea-studio-root.light-theme .ref-chip.interactive:hover,
  .krea-studio-root.light-theme .ref-chip.open {
    background: #f5f3ff;
    border-color: #8b5cf6;
  }
  .krea-studio-root.light-theme .ref-chip-weight {
    background: #ede9fe;
    color: #6d28d9;
    border-color: #c4b5fd;
  }
  .krea-studio-root.light-theme .ref-weight-popover {
    background: #ffffff;
    border-color: #F2E5ED;
    box-shadow: 0 14px 34px rgba(15, 10, 16, 0.14), 0 0 0 1px rgba(15, 10, 16, 0.04);
  }
  .krea-studio-root.light-theme .ref-popover-header {
    border-bottom-color: #F7ECF2;
  }
  .krea-studio-root.light-theme .ref-popover-title {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .ref-popover-close {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .ref-popover-close:hover {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .ref-weight-popover .setting-name {
    color: #1A121D;
  }
  .krea-studio-root.light-theme .ref-weight-popover .setting-val-badge {
    color: #6d28d9;
    background: #ede9fe;
    border-color: #c4b5fd;
  }
  .krea-studio-root.light-theme .ref-weight-popover .setting-subtext {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .ref-weight-popover .advanced-slider {
    background: #E6D3DF;
    accent-color: #7c3aed;
  }
  .krea-studio-root.light-theme .add-ref-mini-btn {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #664D5F;
  }
  .krea-studio-root.light-theme .add-ref-mini-btn:hover {
    background: #f5f3ff;
    border-color: #7c3aed;
    color: #7c3aed;
  }
  .krea-studio-root.light-theme .sdxl-ref-mode-badge {
    background: #f5f3ff;
    border-color: #ddd6fe;
    color: #6d28d9;
  }
  .krea-studio-root.light-theme .sdxl-ref-mode-group {
    background: #F7ECF2;
    border-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .sdxl-mode-pill {
    color: #664D5F;
  }
  .krea-studio-root.light-theme .sdxl-mode-pill:hover {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .sdxl-mode-pill.active {
    background: #ffffff;
    color: #6d28d9;
    box-shadow: 0 1px 3px rgba(15, 10, 16, 0.08);
  }
  .krea-studio-root.light-theme .sdxl-face-target-group {
    background: #F7ECF2;
    border-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .sdxl-target-pill {
    color: #664D5F;
  }
  .krea-studio-root.light-theme .sdxl-target-pill:hover {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .sdxl-target-pill.active {
    background: #ffffff;
    color: #6d28d9;
    box-shadow: 0 1px 3px rgba(15, 10, 16, 0.08);
  }
  .krea-studio-root.light-theme .sdxl-duo-hint {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .prompts-popup {
    background: #ffffff;
    border-color: #F2E5ED;
    box-shadow: 0 16px 40px rgba(0, 0, 0, 0.12);
  }
  .krea-studio-root.light-theme .prompts-popup-header {
    border-bottom-color: #F7ECF2;
  }
  .krea-studio-root.light-theme .prompts-header-title {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .prompts-popup-close {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .prompts-popup-close:hover {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .prompts-search-wrap {
    border-bottom-color: #F7ECF2;
  }
  .krea-studio-root.light-theme .prompts-search-input {
    background: #FCF7FA;
    border-color: #E6D3DF;
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .prompt-item:hover {
    background: #FCF7FA;
  }
  .krea-studio-root.light-theme .prompt-item-text {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .prompt-item-meta {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .prompt-style-badge {
    background: #ede9fe;
    color: #7c3aed;
  }
  .krea-studio-root.light-theme .prompt-delete-btn {
    color: #998094;
  }
  .krea-studio-root.light-theme .prompts-empty-state {
    color: #7D6376;
  }

  /* Modal */
  .krea-studio-root.light-theme .persona-modal-card {
    background: #ffffff;
    border-color: #F2E5ED;
    color: #0F0A10;
    box-shadow: 0 20px 50px rgba(0, 0, 0, 0.15);
  }
  .krea-studio-root.light-theme .modal-header {
    border-bottom-color: #F7ECF2;
  }
  .krea-studio-root.light-theme .modal-header h3 {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .modal-close-btn {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .modal-close-btn:hover {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .modal-field label {
    color: #352339;
  }
  .krea-studio-root.light-theme .modal-input,
  .krea-studio-root.light-theme .modal-textarea {
    background: #FCF7FA;
    border-color: #E6D3DF;
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .field-hint {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .archetype-card {
    background: #FCF7FA;
    border-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .archetype-card:hover {
    background: #F7ECF2;
    border-color: #E6D3DF;
  }
  .krea-studio-root.light-theme .archetype-card.active {
    background: #ede9fe;
    border-color: #8b5cf6;
  }
  .krea-studio-root.light-theme .arch-name {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .arch-hint {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .generated-persona-card {
    background: #FCF7FA;
    border-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .avatar-generator-card {
    background: #FCF7FA;
    border-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .archetype-select {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .archetype-select option {
    background: #ffffff;
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .avatar-empty-box {
    background: #F7ECF2;
    border-color: #E6D3DF;
    color: #998094;
  }
  .krea-studio-root.light-theme .modal-footer {
    background: #FCF7FA;
    border-top-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .cancel-btn {
    border-color: #E6D3DF;
    color: #664D5F;
  }
  .krea-studio-root.light-theme .cancel-btn:hover {
    background: #F2E5ED;
    color: #0F0A10;
  }

  /* ── Custom Confirmation Modal ─────────────────── */
  .confirm-modal-overlay {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(0, 0, 0, 0.75);
    backdrop-filter: blur(6px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 10000;
    padding: 16px;
  }

  .confirm-modal-box {
    background: #1A121D;
    border: 1px solid #352339;
    border-radius: 14px;
    width: 440px;
    max-width: 92vw;
    padding: 24px;
    box-shadow: 0 20px 45px rgba(0, 0, 0, 0.6);
    color: #F7ECF2;
    font-family: inherit;
    display: flex;
    flex-direction: column;
  }

  .confirm-modal-header {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-bottom: 14px;
  }

  .confirm-modal-header h3 {
    margin: 0;
    font-size: 1.15rem;
    font-weight: 600;
    color: #F7ECF2;
  }

  .confirm-danger-icon {
    color: #f85149;
    flex-shrink: 0;
  }

  .confirm-modal-body p {
    margin: 0 0 8px 0;
    font-size: 0.95rem;
    line-height: 1.5;
    color: #D4BECF;
    word-break: break-word;
    overflow-wrap: anywhere;
  }

  .confirm-modal-body strong {
    word-break: break-word;
    overflow-wrap: anywhere;
    color: #ffffff;
    font-weight: 600;
  }

  .confirm-modal-subtext {
    font-size: 0.85rem !important;
    color: #998094 !important;
    margin-bottom: 0 !important;
  }

  .confirm-modal-actions {
    display: flex;
    justify-content: flex-end;
    gap: 10px;
    margin-top: 22px;
  }

  .confirm-btn {
    padding: 9px 18px;
    border-radius: 8px;
    font-size: 0.9rem;
    font-weight: 500;
    cursor: pointer;
    border: 1px solid transparent;
    transition: background 0.15s, border-color 0.15s, transform 0.1s;
  }

  .confirm-btn:active {
    transform: scale(0.98);
  }

  .confirm-btn.cancel {
    background: #221727;
    border-color: #352339;
    color: #D4BECF;
  }

  .confirm-btn.cancel:hover {
    background: #352339;
  }

  .confirm-btn.danger {
    background: #da3633;
    color: #ffffff;
    border-color: rgba(255, 255, 255, 0.1);
  }

  .confirm-btn.danger:hover {
    background: #b82a28;
  }

  .confirm-btn:disabled {
    opacity: 0.6;
    cursor: not-allowed;
  }

  /* Confirmation Modal (Light Theme) */
  .krea-studio-root.light-theme .confirm-modal-box {
    background: #ffffff;
    border-color: #F2E5ED;
    box-shadow: 0 20px 45px rgba(0, 0, 0, 0.15);
    color: #0F0A10;
  }

  .krea-studio-root.light-theme .confirm-modal-header h3 {
    color: #0F0A10;
  }

  .krea-studio-root.light-theme .confirm-modal-body p {
    color: #352339;
  }

  .krea-studio-root.light-theme .confirm-modal-body strong {
    color: #0F0A10;
  }

  .krea-studio-root.light-theme .confirm-modal-subtext {
    color: #7D6376 !important;
  }

  .krea-studio-root.light-theme .confirm-btn.cancel {
    background: #F7ECF2;
    border-color: #E6D3DF;
    color: #352339;
  }

  .krea-studio-root.light-theme .confirm-btn.cancel:hover {
    background: #F2E5ED;
  }

  .krea-studio-root.light-theme .confirm-btn.danger {
    background: #ef4444;
    color: #ffffff;
  }

  .krea-studio-root.light-theme .confirm-btn.danger:hover {
    background: #dc2626;
  }

  /* Attached References Bar & Chips */
  .attached-refs-bar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 10px;
    padding: 6px 14px;
    background: rgba(15, 10, 16, 0.7);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 12px;
    margin-bottom: 8px;
    backdrop-filter: blur(12px);
  }

  .refs-label {
    font-size: 0.75rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: #998094;
  }

  .refs-list {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }

  .ref-chip-wrap {
    position: relative;
    display: inline-flex;
    align-items: center;
  }

  .ref-chip {
    display: flex;
    align-items: center;
    gap: 6px;
    background: rgba(26, 18, 29, 0.85);
    border: 1px solid rgba(139, 92, 246, 0.4);
    padding: 3px 8px 3px 4px;
    border-radius: 20px;
    font-size: 0.78rem;
    color: #F7ECF2;
    transition: border-color 0.15s ease, background 0.15s ease;
  }

  .ref-chip.interactive {
    cursor: pointer;
    user-select: none;
  }

  .ref-chip.interactive:hover,
  .ref-chip.open {
    border-color: #8b5cf6;
    background: rgba(46, 16, 101, 0.45);
  }

  .ref-chip-weight {
    font-size: 0.68rem;
    font-weight: 600;
    padding: 1px 6px;
    border-radius: 9px;
    background: rgba(139, 92, 246, 0.2);
    border: 1px solid rgba(139, 92, 246, 0.35);
    color: #ddd6fe;
    line-height: 1.25;
  }

  .ref-weight-popover {
    position: absolute;
    bottom: calc(100% + 10px);
    left: 0;
    width: 280px;
    padding: 12px 14px;
    border-radius: 14px;
    background: rgba(15, 10, 16, 0.96);
    border: 1px solid rgba(139, 92, 246, 0.32);
    box-shadow: 0 16px 38px rgba(0, 0, 0, 0.45);
    backdrop-filter: blur(16px);
    z-index: 95;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .ref-popover-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding-bottom: 6px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.08);
  }

  .ref-popover-title {
    font-size: 0.78rem;
    font-weight: 700;
    color: #FCF7FA;
  }

  .ref-popover-actions {
    display: inline-flex;
    align-items: center;
    gap: 8px;
  }

  .ref-popover-reset {
    border: none;
    background: transparent;
    color: #8b5cf6;
    font-size: 0.72rem;
    font-weight: 600;
    cursor: pointer;
    padding: 0;
  }

  .ref-popover-reset:hover {
    text-decoration: underline;
  }

  .ref-popover-close {
    border: none;
    background: transparent;
    color: #998094;
    font-size: 0.82rem;
    cursor: pointer;
    padding: 0 2px;
    line-height: 1;
  }

  .ref-popover-close:hover {
    color: #FCF7FA;
  }

  .ref-popover-row {
    display: flex;
    flex-direction: column;
    gap: 5px;
  }

  .ref-popover-label-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }

  .ref-chip-idx {
    font-size: 0.7rem;
    font-weight: 700;
    background: #8b5cf6;
    color: #ffffff;
    padding: 1px 6px;
    border-radius: 10px;
  }

  .ref-chip-img {
    width: 24px;
    height: 24px;
    border-radius: 50%;
    object-fit: cover;
    border: 1px solid rgba(255, 255, 255, 0.2);
  }

  .ref-chip-title {
    max-width: 120px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .ref-chip-del {
    background: transparent;
    border: none;
    color: #998094;
    cursor: pointer;
    font-size: 0.85rem;
    padding: 0 2px;
    line-height: 1;
    transition: color 0.15s;
  }

  .ref-chip-del:hover {
    color: #ef4444;
  }

  .add-ref-mini-btn {
    background: rgba(255, 255, 255, 0.06);
    border: 1px dashed rgba(255, 255, 255, 0.2);
    color: #E6D3DF;
    font-size: 0.75rem;
    padding: 3px 10px;
    border-radius: 14px;
    cursor: pointer;
    transition: all 0.2s ease;
  }

  .add-ref-mini-btn:hover {
    background: rgba(139, 92, 246, 0.15);
    border-color: #8b5cf6;
    color: #c084fc;
  }

  .sdxl-ref-controls {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-left: auto;
    flex-wrap: wrap;
  }

  .sdxl-ref-mode-group {
    display: inline-flex;
    align-items: center;
    gap: 2px;
    padding: 2px;
    border-radius: 14px;
    background: rgba(26, 18, 29, 0.8);
    border: 1px solid rgba(255, 255, 255, 0.1);
  }

  .sdxl-mode-pill {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    border: none;
    background: transparent;
    color: #998094;
    font-size: 0.72rem;
    font-weight: 600;
    padding: 3px 9px;
    border-radius: 11px;
    cursor: pointer;
    transition: background 0.15s ease, color 0.15s ease, box-shadow 0.15s ease;
    white-space: nowrap;
  }

  .sdxl-mode-pill:hover {
    color: #F2E5ED;
  }

  .sdxl-mode-pill.active {
    background: rgba(139, 92, 246, 0.35);
    color: #ffffff;
  }

  .sdxl-face-target-group {
    display: inline-flex;
    align-items: center;
    gap: 2px;
    padding: 2px;
    border-radius: 14px;
    background: rgba(26, 18, 29, 0.8);
    border: 1px solid rgba(255, 255, 255, 0.1);
  }

  .sdxl-target-pill {
    border: none;
    background: transparent;
    color: #998094;
    font-size: 0.72rem;
    font-weight: 600;
    padding: 2px 8px;
    border-radius: 11px;
    cursor: pointer;
    transition: background 0.15s ease, color 0.15s ease;
    white-space: nowrap;
  }

  .sdxl-target-pill:hover {
    color: #F2E5ED;
  }

  .sdxl-target-pill.active {
    background: rgba(139, 92, 246, 0.35);
    color: #ffffff;
  }

  .sdxl-duo-hint {
    font-size: 0.72rem;
    color: #998094;
    white-space: nowrap;
  }


  .card-toolbar .card-ref-btn:hover {
    background: rgba(139, 92, 246, 0.28);
    color: #ede9fe;
  }

  /* Header Mode Switcher (CyberCraft / Krea Studio) */
  .header-mode-toggle-group {
    display: inline-flex;
    align-items: center;
    gap: 8px;
  }
  .header-count-pill {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    min-width: 24px;
    height: 24px;
    padding: 0 8px;
    font-size: 0.78rem;
    font-weight: 600;
    border-radius: 9999px;
    background: rgba(255, 255, 255, 0.12);
    color: rgba(255, 255, 255, 0.9);
    border: 1px solid rgba(255, 255, 255, 0.14);
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.15);
  }
  :global(.light-theme) .header-count-pill {
    background: rgba(0, 0, 0, 0.06);
    color: #352339;
    border-color: rgba(0, 0, 0, 0.08);
  }
  .header-mode-toggle {
    display: inline-flex;
    align-items: center;
    background: rgba(15, 10, 16, 0.55);
    backdrop-filter: blur(12px);
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 9999px;
    padding: 3px;
    gap: 2px;
  }
  :global(.light-theme) .header-mode-toggle {
    background: rgba(247, 236, 242, 0.9);
    border-color: rgba(0, 0, 0, 0.1);
  }
  .mode-toggle-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 13px;
    border-radius: 9999px;
    border: 1px solid transparent;
    background: transparent;
    color: rgba(255, 255, 255, 0.65);
    font-size: 0.88rem;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.18s cubic-bezier(0.16, 1, 0.3, 1);
    user-select: none;
  }
  :global(.light-theme) .mode-toggle-btn {
    color: rgba(26, 18, 29, 0.7);
  }
  .mode-toggle-btn:hover:not(.active) {
    color: rgba(255, 255, 255, 0.95);
    background: rgba(255, 255, 255, 0.08);
  }
  :global(.light-theme) .mode-toggle-btn:hover:not(.active) {
    color: #0F0A10;
    background: rgba(0, 0, 0, 0.05);
  }
  .mode-toggle-btn.active {
    background: linear-gradient(135deg, rgba(168, 85, 247, 0.4), rgba(168, 85, 247, 0.4));
    border-color: rgba(168, 85, 247, 0.5);
    color: #fff;
    font-weight: 600;
    box-shadow: 0 2px 10px rgba(168, 85, 247, 0.25);
  }
  :global(.light-theme) .mode-toggle-btn.active {
    background: linear-gradient(135deg, rgba(168, 85, 247, 0.18), rgba(168, 85, 247, 0.18));
    border-color: rgba(168, 85, 247, 0.4);
    color: #7E22CE;
  }
  .mode-toggle-btn .mode-icon {
    font-size: 0.95rem;
  }
  .mode-toggle-btn.locked {
    opacity: 0.72;
  }

  .mode-text-short {
    display: none;
  }

  /* Responsive Mobile Layout (max-width: 640px) */
  @media (max-width: 640px) {
    .studio-header {
      flex-wrap: wrap;
      padding: 8px 10px;
      gap: 6px 8px;
    }

    .header-left {
      gap: 6px;
      flex: 1;
      min-width: 0;
    }

    .header-mode-toggle-group {
      gap: 4px;
    }

    .header-mode-toggle {
      padding: 2px;
      gap: 2px;
    }

    .mode-toggle-btn {
      padding: 4px 8px;
      font-size: 0.8rem;
      gap: 4px;
    }

    .header-count-pill {
      display: none;
    }

    .header-right {
      gap: 4px;
      flex-shrink: 0;
    }

    .header-right .icon-btn {
      width: 34px;
      height: 34px;
      padding: 4px;
    }

    /* Tabs (Gallery vs Personas) become a sleek full-width segmented control on row 2 */
    .header-tabs {
      order: 3;
      width: 100%;
      margin-top: 2px;
      padding: 3px;
      gap: 4px;
      border-radius: 12px;
      box-sizing: border-box;
    }

    .header-tabs .tab-btn {
      flex: 1;
      justify-content: center;
      padding: 6px 8px;
      font-size: 12px;
      gap: 6px;
    }

    /* Sticky Bottom Composer */
    .studio-composer {
      padding: 10px 10px 12px 10px;
    }

    .composer-controls-row {
      margin-bottom: 6px;
      gap: 4px;
      flex-wrap: nowrap;
      align-items: center;
      width: 100%;
      justify-content: space-between;
    }

    .composer-controls-left {
      gap: 4px;
      flex-wrap: nowrap;
      min-width: 0;
      flex-shrink: 1;
    }

    .active-personas-wrap {
      flex-wrap: nowrap;
      gap: 4px;
      min-width: 0;
    }

    .active-persona-pill .pill-name {
      max-width: 58px;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .composer-controls-right {
      margin-left: auto;
      justify-content: flex-end;
      gap: 3px;
      flex-wrap: nowrap;
      flex-shrink: 0;
    }

    .pill-label-desktop {
      display: none !important;
    }

    .pill-label-mobile {
      display: inline-flex;
      align-items: center;
      font-size: 12px;
      line-height: 1;
    }

    .pick-persona-btn,
    .add-more-personas-btn,
    .add-photo-pill,
    .active-persona-pill,
    .toggle-director-pill,
    .toggle-history-pill,
    .toggle-tokens-btn,
    .toggle-sliders-btn,
    .advanced-gear-btn {
      font-size: 11px;
      padding: 0 6px;
      gap: 3px;
      height: 26px;
      white-space: nowrap;
      flex-shrink: 0;
    }

    .active-persona-pill {
      flex-shrink: 1;
      min-width: 0;
    }

    /* Input Row: prompt full-width on top, action button row below */
    .composer-input-row {
      flex-direction: column;
      align-items: stretch;
      gap: 8px;
    }

    .prompt-textarea {
      width: 100%;
      min-height: 46px;
      max-height: 120px;
      font-size: 15px;
      line-height: 20px;
      padding: 9px 12px;
      box-sizing: border-box;
    }

    .composer-actions {
      width: 100%;
      display: flex;
      align-items: center;
      gap: 6px;
    }

    .generate-submit-btn {
      flex: 1;
      min-width: 0;
      height: 40px;
      padding: 0 10px;
      font-size: 14px;
      justify-content: center;
    }

    /* Prompts popup */
    .prompts-popup {
      left: 10px;
      right: 10px;
      max-height: 60vh;
    }

    /* Sliders panel on mobile */
    .sliders-panel {
      grid-template-columns: 1fr;
      padding: 10px;
      gap: 10px;
      max-height: 45vh;
      overflow-y: auto;
    }

    .studio-main {
      padding: 10px 10px 240px 10px;
    }

    .gallery-grid {
      gap: 12px;
      grid-template-columns: 1fr;
    }
  }

  @media (max-width: 640px) {
    .mode-text-full {
      display: none;
    }
    .mode-text-short {
      display: inline;
    }
  }

  /* =======================================================================
     🎬 AI DIRECTOR'S CHAIR & SPLIT WORKSPACE STYLES
     ======================================================================= */
  .studio-body-split {
    display: flex;
    flex: 1;
    min-height: 0;
    width: 100%;
    position: relative;
    overflow: hidden;
  }

  .studio-canvas-col {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-width: 0;
    min-height: 0;
    position: relative;
    overflow: hidden;
  }

  .director-icon-btn.is-active {
    color: #c4b5fd;
    background: rgba(139, 92, 246, 0.16);
    border-color: rgba(167, 139, 250, 0.45);
  }

  .toggle-director-pill {
    background: transparent;
    border: 1px solid rgba(167, 139, 250, 0.3);
    color: #c4b5fd;
    padding: 0 10px;
    border-radius: 12px;
    height: 26px;
    font-size: 11px;
    font-weight: 500;
    font-family: inherit;
    display: inline-flex;
    align-items: center;
    gap: 6px;
    box-sizing: border-box;
    cursor: pointer;
    transition: all 0.15s ease;
    white-space: nowrap;
  }

  .toggle-director-pill:hover {
    background: rgba(139, 92, 246, 0.15);
    border-color: rgba(167, 139, 250, 0.5);
    color: #ede9fe;
  }

  .toggle-director-pill.open,
  .toggle-director-pill.active {
    background: rgba(139, 92, 246, 0.22);
    border-color: rgba(167, 139, 250, 0.55);
    color: #ede9fe;
    font-weight: 600;
  }

  /* Story Subfolders Filter Bar in Gallery */
  .story-folders-bar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 8px;
    margin-bottom: 16px;
    padding-bottom: 10px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.07);
  }

  .story-folder-pill {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 5px 12px;
    border-radius: 14px;
    font-size: 12px;
    font-weight: 500;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.1);
    color: #E6D3DF;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .story-folder-pill:hover {
    background: rgba(139, 92, 246, 0.16);
    border-color: rgba(139, 92, 246, 0.4);
    color: #fff;
  }

  .story-folder-pill.active {
    background: linear-gradient(135deg, #A855F7 0%, #8b5cf6 100%);
    border-color: transparent;
    color: #fff;
    font-weight: 600;
  }

  .story-open-director-chip {
    margin-left: auto;
    padding: 5px 11px;
    border-radius: 14px;
    font-size: 12px;
    font-weight: 600;
    background: rgba(236, 72, 153, 0.18);
    border: 1px solid rgba(236, 72, 153, 0.45);
    color: #fbcfe8;
    cursor: pointer;
  }

  /* Director Panel */
  .director-panel {
    position: relative;
    flex-shrink: 0;
    width: 390px;
    min-width: 320px;
    max-width: 820px;
    height: 100%;
    background: rgba(15, 10, 16, 0.98);
    border-left: 1px solid rgba(255, 255, 255, 0.1);
    display: flex;
    flex-direction: column;
    z-index: 35;
    box-shadow: -8px 0 28px rgba(0, 0, 0, 0.45);
  }

  .director-resize-handle {
    position: absolute;
    top: 0;
    bottom: 0;
    left: -4px;
    width: 8px;
    cursor: col-resize;
    z-index: 45;
    display: flex;
    align-items: center;
    justify-content: center;
    touch-action: none;
  }

  .director-resize-handle::after {
    content: "";
    width: 2px;
    height: 100%;
    background: transparent;
    transition: background 0.15s ease;
  }

  .director-resize-handle:hover::after,
  .director-resize-handle.active::after {
    background: #8b5cf6;
  }

  .director-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 14px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.08);
    background: rgba(26, 18, 29, 0.8);
  }

  .director-title-group {
    display: flex;
    align-items: center;
    gap: 9px;
  }

  .director-icon {
    font-size: 20px;
  }

  .director-title {
    font-size: 14px;
    font-weight: 700;
    color: #FCF7FA;
  }

  .director-subtitle {
    font-size: 11px;
    color: #998094;
  }

  .director-close-btn {
    background: transparent;
    border: none;
    color: #998094;
    font-size: 15px;
    width: 28px;
    height: 28px;
    border-radius: 6px;
    cursor: pointer;
  }

  .director-close-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: #fff;
  }

  .director-mode-tabs {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 4px;
    margin: 8px 12px;
    padding: 3px;
    border-radius: 12px;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.1);
  }

  .director-tab-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 5px;
    padding: 6px 10px;
    border-radius: 9px;
    border: none;
    background: transparent;
    color: #E6D3DF;
    font-size: 12px;
    font-weight: 600;
    font-family: inherit;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .director-tab-btn:hover {
    color: #ffffff;
    background: rgba(255, 255, 255, 0.06);
  }

  .director-tab-btn.active {
    background: linear-gradient(135deg, #9333EA 0%, #7c3aed 100%);
    color: #ffffff;
    box-shadow: 0 2px 8px rgba(124, 58, 237, 0.35);
  }

  .director-tab-badge {
    font-size: 10px;
    background: rgba(0, 0, 0, 0.3);
    padding: 1px 6px;
    border-radius: 8px;
  }

  .director-context-bar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 5px;
    padding: 8px 12px;
    background: rgba(255, 255, 255, 0.025);
    border-top: 1px solid rgba(255, 255, 255, 0.06);
    border-bottom: 1px solid rgba(255, 255, 255, 0.06);
  }

  .ctx-chip {
    font-size: 11px;
    font-weight: 500;
    padding: 2px 8px;
    border-radius: 8px;
    border: 1px solid rgba(167, 139, 250, 0.3);
    background: rgba(139, 92, 246, 0.18);
    color: #ede9fe;
  }

  .ctx-chip.persona {
    border-color: rgba(244, 114, 182, 0.35);
    background: rgba(236, 72, 153, 0.18);
    color: #fce7f3;
  }

  .ctx-chip.muted {
    border-color: rgba(255, 255, 255, 0.12);
    background: rgba(255, 255, 255, 0.06);
    color: #E6D3DF;
  }

  .ctx-undo-btn {
    margin-left: auto;
    font-size: 11px;
    font-weight: 600;
    padding: 2px 8px;
    border-radius: 7px;
    border: 1px solid rgba(250, 204, 21, 0.45);
    background: rgba(250, 204, 21, 0.15);
    color: #fef08a;
    cursor: pointer;
  }

  .director-quick-chips {
    display: flex;
    flex-wrap: wrap;
    gap: 5px;
    padding: 8px 12px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.06);
  }

  .quick-chip {
    font-size: 11px;
    font-weight: 500;
    font-family: inherit;
    padding: 4px 9px;
    border-radius: 10px;
    border: 1px solid rgba(255, 255, 255, 0.16);
    background: rgba(255, 255, 255, 0.06);
    color: #F2E5ED;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .quick-chip:hover {
    background: rgba(139, 92, 246, 0.2);
    border-color: rgba(167, 139, 250, 0.45);
    color: #fff;
  }

  .director-feed {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    padding: 12px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .director-empty-hint {
    margin: auto 0;
    text-align: center;
    color: #998094;
    font-size: 12.5px;
    line-height: 1.5;
    padding: 16px 10px;
  }

  .director-empty-hint.compact {
    margin: 12px 0;
    padding: 14px 10px;
    background: rgba(255, 255, 255, 0.02);
    border: 1px dashed rgba(255, 255, 255, 0.1);
    border-radius: 10px;
  }

  .empty-clapper {
    font-size: 28px;
    margin-bottom: 8px;
  }

  .director-msg {
    display: flex;
    flex-direction: column;
    gap: 6px;
    max-width: 94%;
  }

  .director-msg.user {
    align-self: flex-end;
  }

  .director-msg.assistant {
    align-self: flex-start;
  }

  .msg-bubble {
    padding: 8px 11px;
    border-radius: 11px;
    font-size: 12.5px;
    line-height: 1.45;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }

  .director-msg.user .msg-bubble {
    background: linear-gradient(135deg, #9333EA 0%, #7c3aed 100%);
    color: #fff;
    border-bottom-right-radius: 3px;
  }

  .director-msg.assistant .msg-bubble {
    background: rgba(255, 255, 255, 0.06);
    border: 1px solid rgba(255, 255, 255, 0.1);
    color: #F2E5ED;
    border-bottom-left-radius: 3px;
  }

  .director-msg.error .msg-bubble {
    background: rgba(239, 68, 68, 0.16);
    border-color: rgba(239, 68, 68, 0.4);
    color: #fca5a5;
  }

  .thinking-bubble {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    color: #c4b5fd !important;
  }

  .dot-pulse {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #ec4899;
    animation: pulse 1s infinite ease-in-out;
  }

  @keyframes pulse {
    0%, 100% { transform: scale(0.85); opacity: 0.5; }
    50% { transform: scale(1.2); opacity: 1; }
  }

  .msg-prompt-preview {
    background: rgba(0, 0, 0, 0.35);
    border: 1px solid rgba(139, 92, 246, 0.3);
    border-radius: 9px;
    padding: 8px 10px;
  }

  .msg-prompt-text {
    font-size: 11.5px;
    color: #E6D3DF;
    line-height: 1.4;
    max-height: 110px;
    overflow-y: auto;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
  }

  .msg-prompt-actions {
    display: flex;
    gap: 6px;
    margin-top: 6px;
  }

  .mini-act-btn {
    font-size: 11px;
    padding: 3px 8px;
    border-radius: 6px;
    border: 1px solid rgba(255, 255, 255, 0.12);
    background: rgba(255, 255, 255, 0.06);
    color: #F2E5ED;
    cursor: pointer;
  }

  .mini-act-btn.primary {
    background: #8b5cf6;
    border-color: transparent;
    color: #fff;
    font-weight: 600;
  }

  .director-composer {
    padding: 10px 12px;
    border-top: 1px solid rgba(255, 255, 255, 0.08);
    background: rgba(15, 10, 16, 0.95);
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .director-textarea {
    width: 100%;
    display: block;
    margin: 0;
    box-sizing: border-box;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 10px;
    color: #F7ECF2;
    padding: 10px 12px;
    font-size: 13.5px;
    line-height: 20px;
    height: 42px;
    min-height: 42px;
    max-height: 140px;
    font-family: inherit;
    resize: none;
    overflow-y: hidden;
    transition: border-color 0.2s;
  }

  .dir-textarea,
  .dir-input {
    width: 100%;
    box-sizing: border-box;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 9px;
    color: #F7ECF2;
    padding: 8px 10px;
    font-size: 12.5px;
    font-family: inherit;
    resize: vertical;
  }

  .director-textarea:focus,
  .dir-textarea:focus,
  .dir-input:focus {
    outline: none;
    border-color: #8b5cf6;
  }

  .dir-input.small {
    padding: 5px 8px;
    font-size: 12px;
  }

  .director-composer-btns {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .dir-clear-btn {
    background: transparent;
    border: 1px solid rgba(255, 255, 255, 0.12);
    color: #998094;
    border-radius: 8px;
    padding: 6px 9px;
    font-size: 11.5px;
    cursor: pointer;
  }

  .dir-rewrite-btn {
    flex: 1;
    background: linear-gradient(135deg, #9333EA 0%, #7c3aed 100%);
    border: none;
    color: #fff;
    border-radius: 8px;
    padding: 7px 10px;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
  }

  .dir-shoot-now-btn {
    background: linear-gradient(135deg, #ec4899 0%, #f43f5e 100%);
    border: none;
    color: #fff;
    border-radius: 8px;
    padding: 7px 11px;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
  }

  .dir-rewrite-btn:disabled,
  .dir-shoot-now-btn:disabled {
    opacity: 0.45;
    cursor: not-allowed;
  }

  /* Storyboard Tab */
  .storyboard-body {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }

  .story-projects-bar {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 12px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.07);
  }

  .story-select {
    flex: 1;
    min-width: 0;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 7px;
    color: #F2E5ED;
    padding: 5px 8px;
    font-size: 11.5px;
  }

  .story-new-btn {
    background: rgba(139, 92, 246, 0.18);
    border: 1px solid rgba(139, 92, 246, 0.4);
    color: #ddd6fe;
    border-radius: 7px;
    padding: 5px 8px;
    font-size: 11px;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
  }

  .story-new-btn.subtle {
    background: rgba(236, 72, 153, 0.14);
    border-color: rgba(236, 72, 153, 0.35);
    color: #fbcfe8;
  }

  .story-setup-card {
    margin: 8px 12px;
    border: 1px solid rgba(255, 255, 255, 0.09);
    border-radius: 10px;
    background: rgba(255, 255, 255, 0.025);
    flex-shrink: 0;
  }

  .setup-card-toggle {
    width: 100%;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 11px;
    background: transparent;
    border: none;
    color: #F2E5ED;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
    text-align: left;
  }

  .setup-toggle-title {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .slug-pill {
    font-size: 10.5px;
    color: #a78bfa;
    margin-left: 4px;
    font-weight: 500;
  }

  .setup-fields {
    padding: 4px 10px 10px 10px;
    display: flex;
    flex-direction: column;
    gap: 7px;
  }

  .field-label-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    font-size: 11px;
    color: #998094;
  }

  .text-link-btn {
    background: transparent;
    border: none;
    color: #c4b5fd;
    font-size: 11px;
    cursor: pointer;
    padding: 0;
  }

  .setting-presets-row {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
  }

  .preset-chip-wrap {
    display: inline-flex;
    align-items: center;
    background: rgba(139, 92, 246, 0.14);
    border: 1px solid rgba(139, 92, 246, 0.3);
    border-radius: 8px;
    overflow: hidden;
  }

  .preset-chip {
    background: transparent;
    border: none;
    color: #ddd6fe;
    font-size: 10.5px;
    padding: 2px 6px;
    cursor: pointer;
    max-width: 130px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .preset-chip-x {
    background: transparent;
    border: none;
    color: #998094;
    font-size: 10px;
    padding: 2px 5px;
    cursor: pointer;
  }

  .plan-controls-row {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-top: 2px;
  }

  .scene-count-label {
    font-size: 11.5px;
    color: #998094;
    display: inline-flex;
    align-items: center;
    gap: 4px;
  }

  .count-select {
    background: rgba(255, 255, 255, 0.08);
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 6px;
    color: #fff;
    padding: 3px 5px;
    font-size: 11.5px;
  }

  .plan-btn {
    border: none;
    border-radius: 8px;
    padding: 7px 10px;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
    color: #fff;
  }

  .plan-btn.primary {
    flex: 1;
    background: linear-gradient(135deg, #ec4899 0%, #8b5cf6 100%);
  }

  .plan-btn.secondary {
    background: rgba(255, 255, 255, 0.08);
    color: #E6D3DF;
  }

  .plan-btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .story-scenes-list {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    padding: 4px 12px 12px 12px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .scene-card {
    background: rgba(255, 255, 255, 0.03);
    border: 1px solid rgba(255, 255, 255, 0.09);
    border-radius: 11px;
    padding: 9px 10px;
    display: flex;
    flex-direction: column;
    gap: 7px;
  }

  .scene-card.shooting {
    border-color: #ec4899;
    box-shadow: 0 0 14px rgba(236, 72, 153, 0.25);
  }

  .scene-card.done {
    border-color: rgba(16, 185, 129, 0.35);
  }

  .scene-card-head {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .scene-num {
    font-size: 11px;
    font-weight: 700;
    color: #c4b5fd;
    background: rgba(139, 92, 246, 0.2);
    padding: 2px 6px;
    border-radius: 6px;
  }

  .scene-title-input {
    flex: 1;
    min-width: 0;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    color: #F7ECF2;
    font-size: 12.5px;
    font-weight: 600;
    padding: 2px 5px;
  }

  .scene-title-input:focus {
    outline: none;
    border-color: rgba(139, 92, 246, 0.5);
    background: rgba(255, 255, 255, 0.04);
  }

  .scene-aspect-select {
    background: rgba(255, 255, 255, 0.06);
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 5px;
    color: #E6D3DF;
    font-size: 11px;
    padding: 2px 4px;
  }

  .scene-del-btn {
    background: transparent;
    border: none;
    color: #7D6376;
    cursor: pointer;
    font-size: 12px;
  }

  .scene-del-btn:hover {
    color: #f87171;
  }

  .scene-thumb-box {
    position: relative;
    border-radius: 8px;
    overflow: hidden;
    max-height: 180px;
    background: #0F0A10;
  }

  .scene-thumb-img {
    width: 100%;
    max-height: 180px;
    object-fit: cover;
    display: block;
  }

  .scene-done-badge {
    position: absolute;
    top: 6px;
    right: 6px;
    background: rgba(16, 185, 129, 0.9);
    color: #fff;
    font-size: 10px;
    font-weight: 700;
    padding: 2px 6px;
    border-radius: 6px;
  }

  .scene-narrative-box,
  .scene-vprompt-box {
    width: 100%;
    box-sizing: border-box;
    background: rgba(0, 0, 0, 0.25);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 7px;
    color: #F2E5ED;
    padding: 6px 8px;
    font-size: 12px;
    line-height: 1.42;
    font-family: inherit;
    resize: vertical;
  }

  .scene-vprompt-box {
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 11px;
    color: #E6D3DF;
    border-color: rgba(139, 92, 246, 0.3);
  }

  .scene-sub-toggles {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .scene-sub-link {
    background: transparent;
    border: none;
    color: #a78bfa;
    font-size: 11px;
    padding: 0;
    cursor: pointer;
  }

  .scene-sub-link:hover {
    text-decoration: underline;
  }

  .scene-refine-row {
    display: flex;
    gap: 6px;
  }

  .scene-actions-row {
    display: flex;
    gap: 6px;
  }

  .shoot-scene-btn {
    width: 100%;
    border: 1px solid rgba(139, 92, 246, 0.4);
    background: rgba(139, 92, 246, 0.16);
    color: #ede9fe;
    border-radius: 7px;
    padding: 5px 10px;
    font-size: 11.5px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .shoot-scene-btn:hover:not(:disabled) {
    background: #8b5cf6;
    color: #fff;
  }

  .shoot-scene-btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .story-continue-inline {
    display: flex;
    gap: 6px;
    padding-top: 2px;
  }

  .add-scene-inline-btn {
    flex: 1;
    padding: 6px 8px;
    border-radius: 8px;
    border: 1px dashed rgba(255, 255, 255, 0.16);
    background: transparent;
    color: #998094;
    font-size: 11.5px;
    cursor: pointer;
  }

  .add-scene-inline-btn.highlight {
    border-color: rgba(236, 72, 153, 0.45);
    color: #f9a8d4;
  }

  .storyboard-footer {
    padding: 10px 12px;
    border-top: 1px solid rgba(255, 255, 255, 0.08);
    background: rgba(15, 10, 16, 0.96);
    display: flex;
    flex-direction: column;
    gap: 7px;
  }

  .series-shoot-all-btn,
  .series-stop-btn {
    width: 100%;
    border: none;
    border-radius: 8px;
    padding: 8px 12px;
    font-size: 12.5px;
    font-weight: 700;
    color: #fff;
    cursor: pointer;
  }

  .series-shoot-all-btn {
    background: linear-gradient(135deg, #ec4899 0%, #8b5cf6 100%);
  }

  .series-stop-btn {
    background: #ef4444;
  }

  .storyboard-footer-sub {
    display: flex;
    gap: 6px;
  }

  .story-util-btn {
    flex: 1;
    padding: 5px 8px;
    border-radius: 7px;
    border: 1px solid rgba(255, 255, 255, 0.12);
    background: rgba(255, 255, 255, 0.05);
    color: #E6D3DF;
    font-size: 11px;
    font-weight: 600;
    cursor: pointer;
  }

  .story-util-btn.civitai {
    border-color: rgba(236, 72, 153, 0.4);
    background: rgba(236, 72, 153, 0.12);
    color: #F9A8D4;
  }

  /* Light Theme Overrides for Director Panel & Buttons */
  .krea-studio-root.light-theme .director-icon-btn.is-active {
    background: #ede9fe;
    border-color: #8b5cf6;
    color: #5b21b6;
  }

  .krea-studio-root.light-theme .toggle-director-pill {
    background: #ffffff;
    border: 1px solid #c4b5fd;
    color: #6d28d9;
    box-shadow: 0 1px 3px rgba(109, 40, 217, 0.06);
  }
  .krea-studio-root.light-theme .toggle-director-pill:hover {
    background: #f5f3ff;
    color: #5b21b6;
    border-color: #a78bfa;
  }
  .krea-studio-root.light-theme .toggle-director-pill.open,
  .krea-studio-root.light-theme .toggle-director-pill.active {
    background: #ede9fe;
    color: #5b21b6;
    border-color: #8b5cf6;
    font-weight: 600;
  }

  .krea-studio-root.light-theme .story-folders-bar {
    border-bottom-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .story-folder-pill:not(.active) {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #352339;
  }
  .krea-studio-root.light-theme .story-folder-pill:not(.active):hover {
    background: #ede9fe;
    border-color: #a78bfa;
    color: #5b21b6;
  }
  .krea-studio-root.light-theme .story-open-director-chip {
    background: #ede9fe;
    border-color: #8b5cf6;
    color: #5b21b6;
  }

  .krea-studio-root.light-theme .director-panel {
    background: #FCF7FA;
    border-left-color: #E6D3DF;
    color: #0F0A10;
    box-shadow: -6px 0 22px rgba(15, 10, 16, 0.08);
  }
  .krea-studio-root.light-theme .director-header,
  .krea-studio-root.light-theme .director-composer,
  .krea-studio-root.light-theme .storyboard-footer {
    background: #ffffff;
    border-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .director-title,
  .krea-studio-root.light-theme .scene-title-input,
  .krea-studio-root.light-theme .setup-card-toggle {
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .director-subtitle {
    color: #664D5F;
  }
  .krea-studio-root.light-theme .director-close-btn {
    color: #664D5F;
  }
  .krea-studio-root.light-theme .director-close-btn:hover {
    background: #F2E5ED;
    color: #0F0A10;
  }

  .krea-studio-root.light-theme .director-mode-tabs {
    background: #F2E5ED;
    border-color: #E6D3DF;
  }
  .krea-studio-root.light-theme .director-tab-btn {
    color: #352339;
    background: transparent;
  }
  .krea-studio-root.light-theme .director-tab-btn:hover {
    color: #0F0A10;
    background: rgba(255, 255, 255, 0.55);
  }
  .krea-studio-root.light-theme .director-tab-btn.active {
    background: #ffffff;
    color: #5b21b6;
    box-shadow: 0 1px 4px rgba(15, 10, 16, 0.12);
  }
  .krea-studio-root.light-theme .director-tab-badge {
    background: #E6D3DF;
    color: #1A121D;
  }
  .krea-studio-root.light-theme .director-tab-btn.active .director-tab-badge {
    background: #ede9fe;
    color: #5b21b6;
  }

  .krea-studio-root.light-theme .director-context-bar {
    background: #ffffff;
    border-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .ctx-chip {
    background: #ede9fe;
    border-color: #c4b5fd;
    color: #5b21b6;
    font-weight: 600;
  }
  .krea-studio-root.light-theme .ctx-chip.persona {
    background: #fce7f3;
    border-color: #f9a8d4;
    color: #9d174d;
    font-weight: 600;
  }
  .krea-studio-root.light-theme .ctx-chip.muted {
    background: #F7ECF2;
    border-color: #E6D3DF;
    color: #352339;
    font-weight: 500;
  }
  .krea-studio-root.light-theme .ctx-undo-btn {
    background: #fef9c3;
    border-color: #eab308;
    color: #854d0e;
  }

  .krea-studio-root.light-theme .director-quick-chips {
    background: #FCF7FA;
    border-bottom-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .quick-chip {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #1A121D;
    box-shadow: 0 1px 2px rgba(15, 10, 16, 0.04);
  }
  .krea-studio-root.light-theme .quick-chip:hover {
    background: #ede9fe;
    border-color: #8b5cf6;
    color: #5b21b6;
  }

  .krea-studio-root.light-theme .director-empty-hint {
    color: #352339;
  }
  .krea-studio-root.light-theme .director-empty-hint.compact {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #352339;
  }

  .krea-studio-root.light-theme .director-msg.assistant .msg-bubble,
  .krea-studio-root.light-theme .scene-card,
  .krea-studio-root.light-theme .story-setup-card {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #0F0A10;
    box-shadow: 0 1px 3px rgba(15, 10, 16, 0.04);
  }
  .krea-studio-root.light-theme .director-msg.error .msg-bubble {
    background: #fef2f2;
    border-color: #f87171;
    color: #b91c1c;
    font-weight: 500;
  }
  .krea-studio-root.light-theme .thinking-bubble {
    color: #5b21b6 !important;
  }
  .krea-studio-root.light-theme .msg-prompt-preview {
    background: #F7ECF2;
    border-color: #E6D3DF;
  }
  .krea-studio-root.light-theme .msg-prompt-text {
    color: #1A121D;
  }
  .krea-studio-root.light-theme .mini-act-btn {
    background: #ffffff;
    border-color: #E6D3DF;
    color: #1A121D;
    font-weight: 500;
  }
  .krea-studio-root.light-theme .mini-act-btn:hover {
    background: #ede9fe;
    border-color: #8b5cf6;
    color: #5b21b6;
  }
  .krea-studio-root.light-theme .mini-act-btn.primary {
    background: #6d28d9;
    border-color: #6d28d9;
    color: #ffffff;
  }

  .krea-studio-root.light-theme .director-textarea,
  .krea-studio-root.light-theme .dir-textarea,
  .krea-studio-root.light-theme .dir-input,
  .krea-studio-root.light-theme .scene-narrative-box,
  .krea-studio-root.light-theme .story-select,
  .krea-studio-root.light-theme .count-select,
  .krea-studio-root.light-theme .scene-aspect-select {
    background: #ffffff;
    border-color: #998094;
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .director-textarea::placeholder,
  .krea-studio-root.light-theme .dir-textarea::placeholder,
  .krea-studio-root.light-theme .dir-input::placeholder {
    color: #7D6376;
  }
  .krea-studio-root.light-theme .scene-vprompt-box {
    background: #f5f3ff;
    border-color: #a78bfa;
    color: #1E0B36;
  }
  .krea-studio-root.light-theme .dir-clear-btn {
    background: #F7ECF2;
    border-color: #E6D3DF;
    color: #352339;
    font-weight: 500;
  }
  .krea-studio-root.light-theme .dir-clear-btn:hover {
    background: #F2E5ED;
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .dir-rewrite-btn:disabled,
  .krea-studio-root.light-theme .dir-shoot-now-btn:disabled,
  .krea-studio-root.light-theme .plan-btn:disabled {
    background: #F2E5ED;
    color: #7D6376;
    border: 1px solid #E6D3DF;
    opacity: 1;
  }

  .krea-studio-root.light-theme .story-projects-bar {
    background: #ffffff;
    border-bottom-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .story-new-btn {
    background: #ede9fe;
    border-color: #a78bfa;
    color: #5b21b6;
  }
  .krea-studio-root.light-theme .story-new-btn.subtle {
    background: #fce7f3;
    border-color: #f472b6;
    color: #9d174d;
  }
  .krea-studio-root.light-theme .slug-pill,
  .krea-studio-root.light-theme .text-link-btn,
  .krea-studio-root.light-theme .scene-sub-link {
    color: #6d28d9;
    font-weight: 600;
  }
  .krea-studio-root.light-theme .field-label-row,
  .krea-studio-root.light-theme .scene-count-label {
    color: #352339;
    font-weight: 500;
  }
  .krea-studio-root.light-theme .preset-chip-wrap {
    background: #ede9fe;
    border-color: #c4b5fd;
  }
  .krea-studio-root.light-theme .preset-chip {
    color: #5b21b6;
    font-weight: 500;
  }
  .krea-studio-root.light-theme .preset-chip-x {
    color: #664D5F;
  }
  .krea-studio-root.light-theme .plan-btn.secondary {
    background: #F7ECF2;
    border: 1px solid #E6D3DF;
    color: #1A121D;
  }
  .krea-studio-root.light-theme .scene-num {
    background: #ede9fe;
    border: 1px solid #c4b5fd;
    color: #5b21b6;
  }
  .krea-studio-root.light-theme .shoot-scene-btn {
    background: #ede9fe;
    border-color: #8b5cf6;
    color: #5b21b6;
  }
  .krea-studio-root.light-theme .shoot-scene-btn:hover:not(:disabled) {
    background: #6d28d9;
    border-color: #6d28d9;
    color: #ffffff;
  }
  .krea-studio-root.light-theme .add-scene-inline-btn {
    background: #ffffff;
    border-color: #998094;
    color: #352339;
    font-weight: 500;
  }
  .krea-studio-root.light-theme .add-scene-inline-btn.highlight {
    background: #fdf2f8;
    border-color: #db2777;
    color: #9d174d;
  }
  .krea-studio-root.light-theme .story-util-btn {
    background: #FCF7FA;
    border-color: #E6D3DF;
    color: #1A121D;
  }
  .krea-studio-root.light-theme .story-util-btn:hover {
    background: #F2E5ED;
    color: #0F0A10;
  }
  .krea-studio-root.light-theme .story-util-btn.civitai {
    background: #FCE7F3;
    border-color: #F472B6;
    color: #BE185D;
  }

  .civitai-publish-btn.published {
    color: #34d399 !important;
  }
  .civitai-publish-modal {
    max-width: 520px;
    width: 94%;
  }
  .civitai-pub-toggles {
    display: flex;
    flex-direction: column;
    gap: 8px;
    margin-top: 4px;
    padding: 10px 12px;
    border-radius: 10px;
    background: rgba(255, 255, 255, 0.04);
    border: 1px solid rgba(255, 255, 255, 0.08);
  }
  .civitai-check-row {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 12.5px;
    color: #E6D3DF;
    cursor: pointer;
    user-select: none;
  }
  .civitai-check-row input[type="checkbox"] {
    accent-color: #8b5cf6;
    width: 15px;
    height: 15px;
    flex-shrink: 0;
  }
  .civitai-pub-alert {
    margin-top: 10px;
    padding: 11px 13px;
    border-radius: 10px;
    font-size: 12.5px;
    line-height: 1.45;
  }
  .civitai-pub-alert.error {
    background: rgba(239, 68, 68, 0.14);
    border: 1px solid rgba(239, 68, 68, 0.35);
    color: #fca5a5;
  }
  .civitai-pub-alert.success {
    background: rgba(16, 185, 129, 0.13);
    border: 1px solid rgba(16, 185, 129, 0.35);
    color: #d1fae5;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .civitai-pub-success-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }
  .civitai-user-pill {
    font-size: 11px;
    font-weight: 600;
    padding: 2px 8px;
    border-radius: 999px;
    background: rgba(139, 92, 246, 0.25);
    color: #ddd6fe;
  }
  .civitai-linked-res {
    font-size: 11.5px;
    opacity: 0.9;
  }
  .civitai-pub-links {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-top: 2px;
  }
  .civitai-open-post-btn {
    display: inline-flex;
    align-items: center;
    padding: 6px 12px;
    border-radius: 8px;
    background: #10b981;
    color: #ffffff !important;
    font-weight: 600;
    font-size: 12px;
    text-decoration: none;
  }
  .civitai-open-post-btn:hover {
    background: #059669;
  }
  .civitai-edit-post-link {
    font-size: 12px;
    color: #F9A8D4;
    text-decoration: underline;
  }
  .krea-studio-root.light-theme .civitai-pub-toggles {
    background: #FCF7FA;
    border-color: #F2E5ED;
  }
  .krea-studio-root.light-theme .civitai-check-row {
    color: #352339;
  }
  .krea-studio-root.light-theme .civitai-pub-alert.error {
    background: #fef2f2;
    border-color: #fecaca;
    color: #b91c1c;
  }
  .krea-studio-root.light-theme .civitai-pub-alert.success {
    background: #ecfdf5;
    border-color: #a7f3d0;
    color: #065f46;
  }
  .krea-studio-root.light-theme .civitai-user-pill {
    background: #ede9fe;
    color: #5b21b6;
  }
  .krea-studio-root.light-theme .civitai-edit-post-link {
    color: #DB2777;
  }

  @media (max-width: 960px) {
    .director-panel {
      position: absolute;
      top: 0;
      right: 0;
      bottom: 0;
      width: min(100%, 390px);
      min-width: 0;
      max-width: 100%;
    }

    .director-resize-handle {
      display: none;
    }
  }

  .studio-brand-pill {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 12px;
    border-radius: 999px;
    font-size: 13px;
    font-weight: 600;
    background: rgba(139, 92, 246, 0.14);
    border: 1px solid rgba(139, 92, 246, 0.32);
    color: inherit;
  }

  .studio-tier-badge {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 4px 10px;
    border-radius: 999px;
    font-size: 11.5px;
    font-weight: 600;
    white-space: nowrap;
  }

  .studio-tier-badge.muse {
    background: linear-gradient(135deg, rgba(236, 72, 153, 0.18), rgba(139, 92, 246, 0.22));
    border: 1px solid rgba(236, 72, 153, 0.42);
    color: #fbcfe8;
  }

  .krea-studio-root.light-theme .studio-tier-badge.muse {
    background: #fdf2f8;
    border-color: #f9a8d4;
    color: #9d174d;
  }

  .studio-tier-badge.member {
    background: rgba(236, 72, 153, 0.16);
    border: 1px solid rgba(236, 72, 153, 0.38);
    color: #FBCFE8;
  }

  .krea-studio-root.light-theme .studio-tier-badge.member {
    background: #FDF2F8;
    border-color: #F9A8D4;
    color: #BE185D;
  }

  .studio-tier-badge.guest-pass-btn {
    background: rgba(255, 255, 255, 0.06);
    border: 1px dashed rgba(139, 92, 246, 0.5);
    color: inherit;
    cursor: pointer;
    transition: background 0.15s ease, border-color 0.15s ease;
  }

  .studio-tier-badge.guest-pass-btn:hover {
    background: rgba(139, 92, 246, 0.18);
    border-style: solid;
  }

  .pill-btn.locked-pill {
    opacity: 0.68;
  }

  .studio-notice-banner {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    padding: 9px 12px;
    margin-bottom: 8px;
    border-radius: 10px;
    background: rgba(139, 92, 246, 0.14);
    border: 1px solid rgba(139, 92, 246, 0.38);
    font-size: 12.5px;
    line-height: 1.45;
  }

  .krea-studio-root.light-theme .studio-notice-banner {
    background: #f5f3ff;
    border-color: #c4b5fd;
    color: #3b0764;
  }

  .studio-notice-text {
    flex: 1;
    white-space: pre-line;
  }

  .studio-notice-actions {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    flex-shrink: 0;
  }

  .studio-notice-cta {
    display: inline-flex;
    align-items: center;
    padding: 5px 11px;
    border-radius: 7px;
    background: linear-gradient(135deg, #8b5cf6, #A855F7);
    color: #fff !important;
    font-weight: 600;
    font-size: 12px;
    text-decoration: none;
    white-space: nowrap;
  }

  .studio-notice-pass-btn {
    padding: 5px 9px;
    border-radius: 7px;
    border: 1px solid rgba(255, 255, 255, 0.22);
    background: rgba(255, 255, 255, 0.08);
    color: inherit;
    font-size: 12px;
    font-weight: 500;
    cursor: pointer;
    white-space: nowrap;
  }

  .krea-studio-root.light-theme .studio-notice-pass-btn {
    border-color: #c4b5fd;
    background: #fff;
  }

  .studio-notice-close {
    background: transparent;
    border: none;
    color: inherit;
    opacity: 0.7;
    cursor: pointer;
    font-size: 14px;
    padding: 2px 5px;
  }

  /* Video Mode Card & Controls Styles */
  .sliders-panel.video-sliders-panel {
    grid-template-columns: minmax(0, 1.15fr) minmax(0, 0.9fr) minmax(0, 1.35fr);
  }

  @media (max-width: 900px) {
    .sliders-panel.video-sliders-panel {
      grid-template-columns: 1fr;
    }
  }

  .gallery-video-hover-layer {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    display: block;
    object-fit: cover;
    opacity: 0;
    transition: opacity 0.22s ease, transform 0.3s ease;
    pointer-events: none;
    z-index: 2;
  }

  .gallery-video-hover-layer.is-playing {
    opacity: 1;
  }

  .gallery-card:hover .gallery-video-hover-layer {
    transform: scale(1.03);
  }

  .video-card-badge {
    position: absolute;
    top: 8px;
    left: 8px;
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 3px 8px;
    border-radius: 999px;
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 0.01em;
    color: #FCF7FA;
    background: rgba(15, 10, 16, 0.76);
    border: 1px solid rgba(139, 92, 246, 0.45);
    backdrop-filter: blur(8px);
    pointer-events: none;
    z-index: 3;
  }

  .toolbar-btn.card-animate-btn:hover,
  .toolbar-btn.card-animate-btn.active,
  .toolbar-btn.card-extend-btn:hover,
  .toolbar-btn.card-extend-btn.active {
    color: #c4b5fd;
    background: rgba(139, 92, 246, 0.24);
    opacity: 1;
  }

  .extend-video-chip {
    border-color: rgba(139, 92, 246, 0.55) !important;
    background: rgba(139, 92, 246, 0.14) !important;
  }

  .card-toolbar .card-ref-btn.active {
    color: #ede9fe;
    background: rgba(139, 92, 246, 0.28);
    opacity: 1;
  }

  .krea-studio-root.light-theme .toolbar-btn.card-animate-btn.active,
  .krea-studio-root.light-theme .toolbar-btn.card-extend-btn.active,
  .krea-studio-root.light-theme .card-toolbar .card-ref-btn.active {
    color: #6d28d9;
    background: rgba(124, 58, 237, 0.16);
    opacity: 1;
  }

  .gallery-card.is-attached-ref {
    border-color: rgba(139, 92, 246, 0.75);
    box-shadow: 0 0 0 1px rgba(139, 92, 246, 0.45), 0 8px 22px rgba(139, 92, 246, 0.18);
  }

  .krea-studio-root.light-theme .gallery-card.is-attached-ref {
    border-color: #7c3aed;
    box-shadow: 0 0 0 2px rgba(124, 58, 237, 0.22), 0 6px 18px rgba(124, 58, 237, 0.14);
  }

  .attached-ref-card-badge {
    position: absolute;
    top: 8px;
    left: 8px;
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 3px 9px;
    border-radius: 999px;
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 0.01em;
    color: #ffffff;
    background: linear-gradient(135deg, rgba(124, 58, 237, 0.92), rgba(168, 85, 247, 0.92));
    border: 1px solid rgba(221, 214, 254, 0.55);
    box-shadow: 0 3px 10px rgba(15, 10, 16, 0.45);
    backdrop-filter: blur(8px);
    pointer-events: none;
    z-index: 3;
  }

  .swap-refs-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 26px;
    height: 26px;
    border-radius: 999px;
    border: 1px solid rgba(139, 92, 246, 0.42);
    background: rgba(139, 92, 246, 0.16);
    color: #ddd6fe;
    font-size: 13px;
    font-weight: 700;
    cursor: pointer;
    transition: all 0.16s ease;
    padding: 0;
  }

  .swap-refs-btn:hover {
    background: rgba(139, 92, 246, 0.34);
    border-color: #a78bfa;
    color: #ffffff;
    transform: scale(1.06);
  }

  .krea-studio-root.light-theme .swap-refs-btn {
    background: #f5f3ff;
    border-color: #c4b5fd;
    color: #6d28d9;
  }

  .krea-studio-root.light-theme .swap-refs-btn:hover {
    background: #ede9fe;
    border-color: #7c3aed;
    color: #5b21b6;
  }
</style>
