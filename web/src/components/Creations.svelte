<script>
  import { createEventDispatcher, onMount, onDestroy, tick } from "svelte";
  import { fly, fade } from "svelte/transition";
  import { _, locale } from "../lib/i18n/index.js";
  import lightGallery from "lightgallery";
  import { setupGalleryPagination, isJustFileName } from "../lib/gallery-pagination.js";
  import { OnmyChatTransport, SseLineBuffer } from "../lib/onmychat-transport.js";
  import FolderPicker from "./FolderPicker.svelte";
  import { deviceRegistry } from "../lib/device-registry.js";
  import { profileStore, resolveChatUserName } from "../lib/profile-store.js";
  import { getViewerSettings } from "../lib/viewer-settings.js";
  import { parseStoragePath, formatStoragePath, persistStoragePath, DEFAULT_STORAGE_PATH, getLocalDevice, getDefaultStoragePath } from "../lib/storage-path.js";
  import { issueConsent, getClientDeviceServer } from "../lib/client-device-server.js";
  import { getOrCreateLocalLinkId } from "../lib/local-client.js";
  import { isIosNative, getClientPlatform, isExternalCheckoutUrl, sanitizeExternalCheckoutText } from "../lib/platform.js";

  let innerWidth = typeof window !== "undefined" ? window.innerWidth : 1024;
  $: isMobile = innerWidth < 768;

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
  export let kreaSupported = false;

  const dispatch = createEventDispatcher();

  let showFolderPicker = false;
  let showStorageSourceChooser = false;
  let storageSources = [];
  let storagePickerDevice = null;
  let foundDevice = null;

  function resolveDevice() {
    if (foundDevice) return foundDevice;
    const raw = (
      assistantSettingsData?.defaultStorage ||
      defaultStorage ||
      (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
      (typeof window !== "undefined" && window.__omdProfile?.defaultStorage) ||
      ""
    ).trim();
    const { device } = parseStoragePath(raw);
    if (device && !device.isAiNode) return device;
    if (deviceDrive) {
      const d = deviceRegistry.list().find((x) => x.drive === deviceDrive && !x.isAiNode);
      if (d) return d;
    }
    if (targetDevice && !targetDevice.isAiNode) return targetDevice;
    if (targetLinkId) {
      const d = deviceRegistry.get(targetLinkId) || deviceRegistry.getByPrefix(targetLinkId) || deviceRegistry.getByName(targetLinkId);
      if (d && !d.isAiNode) return d;
    }
    return deviceRegistry.list().find((d) => !d.isAiNode && d.drive && d.drive.isConnected) || deviceRegistry.list()[0] || null;
  }

  function buildStorageSources() {
    const out = [];
    const seen = new Set();
    const push = (d) => {
      if (!d) return;
      const id = d.linkId || d.deviceId || (d.local ? "local" : d.name);
      if (!id || seen.has(id)) return;
      seen.add(id);
      out.push(d);
    };
    const allDevs = [...(deviceRegistry.list() || [])];
    for (const d of allDevs) {
      if (d.local === true || d.isAiNode === true) continue;
      if (d.type === "ai" || d.type === "ai-node") continue;
      const pubName = $_("public_ai_node_name", {}, "AI Assistant Public Node");
      if (d.name === pubName || d.name === "AI Assistant Public Node" || d.name === "AI Assistant Public Node") continue;
      push(d);
    }
    const local = getLocalDevice() || deviceRegistry.list().find((d) => d.local);
    if (local) push(local);
    return out;
  }

  function handleBrowseStorage() {
    const sources = buildStorageSources();
    storageSources = sources;
    if (sources.length > 1) {
      showStorageSourceChooser = true;
    } else {
      storagePickerDevice = sources[0] || resolveDevice() || getLocalDevice();
      showFolderPicker = true;
    }
  }

  function selectStorageSource(src) {
    storagePickerDevice = src;
    showStorageSourceChooser = false;
    showFolderPicker = true;
  }

  $: isStorageConfigured = !!(
    (typeof localStorage !== "undefined" && localStorage.getItem("omd_default_storage")) ||
    (assistantSettingsData && assistantSettingsData.defaultStorage && assistantSettingsData.defaultStorage.trim()) ||
    (defaultStorage && defaultStorage.trim() && defaultStorage !== "/alexey")
  );

  $: storageDevice = resolveDevice();
  $: isStorageAvailable = Boolean(
    isStorageConfigured &&
    (
      isPrivateMode ||
      (storageDevice && (storageDevice.local || storageDevice.status === "connected" || storageDevice.drive?.isConnected)) ||
      foundDevice
    )
  );

  let items = [];
  let generationNotice = "";
  $: rawNoticeUrl = generationNotice ? ((generationNotice.match(/https?:\/\/[^\s)\]]+/i) || [])[0] || "").replace(/[.,;!?]+$/, "") : "";
  $: mappedNoticeUrl = rawNoticeUrl.replace("https://whop.com/checkout/prod_wuSw81wT1wivN", "https://whop.com/onmychat/on-my-chat-creative-studio");
  $: noticeUrl = (isIosNative() && isExternalCheckoutUrl(mappedNoticeUrl)) ? "" : mappedNoticeUrl;
  $: noticeText = generationNotice
    ? (isIosNative()
        ? sanitizeExternalCheckoutText(generationNotice)
        : generationNotice.replace(/https?:\/\/[^\s]+/i, "").trim())
    : "";
  let isLoadingFiles = true;
  let viewMode = typeof localStorage !== "undefined"
    ? localStorage.getItem("omd_creations_view") || "roll"
    : "roll"; // 'roll' | 'grid'

  let sortOrder = typeof localStorage !== "undefined"
    ? localStorage.getItem("omd_creations_sort") || "desc"
    : "desc"; // 'desc' (newest first, head) | 'asc' (oldest first, tail)

  function sortItemsList(list, order = sortOrder) {
    return [...list].sort((a, b) => {
      if (a.isPlaceholder) return order === "desc" ? -1 : 1;
      if (b.isPlaceholder) return order === "desc" ? 1 : -1;

      const timeA = a.date instanceof Date ? a.date.getTime() : new Date(a.date || 0).getTime();
      const timeB = b.date instanceof Date ? b.date.getTime() : new Date(b.date || 0).getTime();
      if (!isNaN(timeA) && !isNaN(timeB) && timeA !== timeB) {
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
      localStorage.setItem("omd_creations_sort", sortOrder);
    }
    items = sortItemsList(items, sortOrder);
    await tick();
    if (sortOrder === "desc") {
      scrollToTop(true);
    } else {
      scrollToBottom(true);
    }
  }

  let imageBlobCache = {};
  let imgLoadingState = {};
  let inputPrompt = "";
  let textareaEl = null;
  let isGenerating = false;
  let activePhaseStatus = "";
  let currentPlaceholder = null;
  let abortController = null;
  let scrollContainerEl = null;
  let showScrollTop = false;
  let showScrollBottom = false;
  let composerHeight = 90;
  let scrollContainerHeight = 600;

  $: dynamicMaxImageHeight = Math.max(
    260,
    (scrollContainerHeight || 600) - (composerHeight || 90) - 130
  );

  // Image Generation Controls & Prompt Library
  let showImageToolpanel = true;
  let paintingStyles = ["Realistic", "Fantasy", "Perfect", "Tooned"];
  let selectedStyle = "realistic";
  let withCharacters = typeof localStorage !== "undefined"
    ? localStorage.getItem("omc_gen_characters") !== "false"
    : true;
  let continueGenerating = typeof localStorage !== "undefined"
    ? localStorage.getItem("omc_continue_generating") === "true"
    : false;
  let availableLoras = [];
  let lorasLoaded = false;
  let showPromptsPopup = false;
  let promptsSearchQuery = "";
  let recentPrompts = [];
  let unsubPrompts = null;
  let copiedPromptId = null;
  let itemToDelete = null;
  let lgInstance = null;
  let lgPagination = null;

  function toggleContinueGenerating() {
    continueGenerating = !continueGenerating;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omc_continue_generating", String(continueGenerating));
    }
  }

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

  const starterSuggestions = [
    { text: "A cozy glass cabin in the snowy mountains under northern lights", style: "cinematic" },
    { text: "Futuristic neon cyberpunk street with reflections in the rain", style: "cyberpunk" },
    { text: "Impressionist watercolor painting of a sunlit Mediterranean harbor", style: "watercolor" },
    { text: "Ethereal crystal castle floating above a glowing bioluminescent sea", style: "fantasy" }
  ];

  function setViewMode(mode) {
    viewMode = mode;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_creations_view", mode);
    }
  }

  let assistantSettingsData = null;
  let unsubAssistant = null;

  async function loadAssistantSettings() {
    if (chatStore && typeof chatStore.getAssistantSettings === "function") {
      try {
        const settings = await chatStore.getAssistantSettings();
        if (settings) {
          assistantSettingsData = { ...settings, name: resolveChatUserName($profileStore, settings.name) };
        }
      } catch (e) {
        console.warn("[Creations] error loading assistant settings from DID:", e);
      }
      if (typeof chatStore.subscribeAssistantSettings === "function") {
        unsubAssistant = chatStore.subscribeAssistantSettings((st) => {
          if (st) {
            assistantSettingsData = { ...st, name: resolveChatUserName($profileStore, st.name) };
            loadCreations();
          }
        });
      }
    }
  }

  function getBaseStorage() {
    let raw = (
      assistantSettingsData?.defaultStorage ||
      defaultStorage ||
      (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
      (typeof window !== "undefined" && window.__omdProfile?.defaultStorage) ||
      DEFAULT_STORAGE_PATH ||
      "/Data"
    ).trim();
    const { folder } = parseStoragePath(raw);
    let clean = (folder || "").replace(/^\/+|\/+$/g, "");

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

    if (!clean || clean.toLowerCase() === "alexey") {
      clean = (DEFAULT_STORAGE_PATH || "/Data").replace(/^\/+/, "");
    }
    return clean || "Data";
  }

  function getTargetFolder() {
    const base = getBaseStorage();
    return `/${base}/generated`;
  }

  $: displayFolder = getTargetFolder();

  async function ensureFolderRecursive(drive, folder) {
    if (!drive) return;
    try {
      if (typeof drive.issueConsent === "function") {
        await drive.issueConsent(folder, { writable: true }).catch(() => {});
      }
      const parts = (folder || "").split("/").filter(Boolean);
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
      console.warn("[Creations] buildDevicePush error:", e);
      return null;
    }
  }

  // Watch for device drive connectivity and storage changes reactively
  let lastDriveInstance = null;
  let lastDriveConnected = false;
  let lastStoragePath = null;
  let driveCleanup = null;

  $: if (deviceDrive) {
    if (driveCleanup) {
      driveCleanup();
      driveCleanup = null;
    }
    const onStatus = (st) => {
      if (st === "connected") {
        loadCreations();
      }
    };
    if (typeof deviceDrive.addStatusListener === "function") {
      deviceDrive.addStatusListener(onStatus);
      driveCleanup = () => {
        if (typeof deviceDrive.removeStatusListener === "function") {
          deviceDrive.removeStatusListener(onStatus);
        }
      };
    } else if (typeof deviceDrive.onStatusChange === "function") {
      const prev = deviceDrive.onStatusChange;
      deviceDrive.onStatusChange = (st) => {
        try { prev(st); } catch (_) {}
        onStatus(st);
      };
      driveCleanup = () => {
        deviceDrive.onStatusChange = prev;
      };
    }
  }

  $: {
    const isConn = !!(deviceDrive && (deviceDrive.isConnected || deviceDrive.status === "connected"));
    const curStorage = defaultStorage || "";
    if (
      deviceDrive !== lastDriveInstance ||
      isConn !== lastDriveConnected ||
      curStorage !== lastStoragePath
    ) {
      lastDriveInstance = deviceDrive;
      lastDriveConnected = isConn;
      lastStoragePath = curStorage;
      if (isConn || isPrivateMode || curStorage) {
        loadCreations();
      }
    }
  }

  onMount(async () => {
    await loadAssistantSettings();
    if (chatStore) {
      if (typeof chatStore.onPromptsChanged === "function") {
        unsubPrompts = chatStore.onPromptsChanged((list) => {
          recentPrompts = list || [];
        });
      }
    }
    await loadCreations();
    loadAvailableLoras();
  });

  onDestroy(() => {
    if (driveCleanup) {
      driveCleanup();
      driveCleanup = null;
    }
    if (unsubAssistant) unsubAssistant();
    if (unsubPrompts) unsubPrompts();
    if (abortController) {
      try { abortController.abort(); } catch (_) {}
    }
    if (lgPagination) {
      try { lgPagination.cleanup(); } catch (_) {}
      lgPagination = null;
    }
    if (lgInstance) {
      try { lgInstance.destroy(); } catch (_) {}
      lgInstance = null;
    }
    Object.values(imageBlobCache).forEach((url) => {
      if (url && url.startsWith("blob:")) {
        try { URL.revokeObjectURL(url); } catch (_) {}
      }
    });
  });

  async function loadCreations() {
    isLoadingFiles = true;
    const targetFolder = getTargetFolder();
    let rawList = [];
    let effectiveFolder = targetFolder;

    // Resolve active drive to scan files from
    let activeDrive = deviceDrive;
    if (!activeDrive || !activeDrive.isConnected) {
      const resolvedDev = resolveDevice();
      if (resolvedDev?.drive && (resolvedDev.drive.isConnected || resolvedDev.drive.status === "connected")) {
        activeDrive = resolvedDev.drive;
      }
    }

    if (activeDrive && !activeDrive.isConnected && typeof activeDrive.waitUntilConnected === "function") {
      await activeDrive.waitUntilConnected(2500).catch(() => {});
    }

    const parsedStorage = parseStoragePath(defaultStorage || "");
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
          } catch (e) {
            // cand folder does not exist or failed on device, try next candidate
          }
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
        for (const cand of candidateFolders.slice(0, 3)) {
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
          } catch (e) {
            // cand folder failed on httpDrive
          }
        }
      }
    } catch (err) {
      console.error("[Creations] Error scanning folder:", err);
    }

    const imageExtRegex = /\.(jpe?g|png|webp|gif)$/i;
    const imageFiles = (rawList || []).filter((f) => {
      const name = f.name || f.value || "";
      if (!name) return false;
      if (name.startsWith(".") || name === "folder.jpg" || name.endsWith(".cover.jpg") || name.endsWith(".cover.png")) return false;
      return imageExtRegex.test(name) && !f.isDir;
    });

    const parsedItems = await Promise.all(
      imageFiles.map(async (f) => {
        const name = f.name || f.value;
        const cleanPath = `${effectiveFolder}/${name}`.replace(/\/+/g, "/");
        let promptText = "";
        let titleText = f.customAttributes?.title || f.customAttributes?.metaTitle || "";

        if (chatStore && typeof chatStore.getPromptForFile === "function") {
          try {
            promptText = await chatStore.getPromptForFile(name);
          } catch (_) {}
        }

        if (!promptText && f.customAttributes?.description) {
          promptText = f.customAttributes.description;
        }

        if (promptText) {
          const titleMatch = promptText.match(/^#\s*([^\n]+)/);
          if (titleMatch) {
            if (!titleText) titleText = titleMatch[1].trim();
            promptText = promptText.replace(/^#\s*[^\n]+\n+/, "").trim();
          }
        }

        let mtime = f.lastModified || f.date || (f.mtime ? f.mtime * 1000 : null);
        if (!mtime) {
          const matchTs = name.match(/(\d{13})/);
          if (matchTs) {
            mtime = parseInt(matchTs[1], 10);
          } else {
            mtime = Date.now();
          }
        }
        const dateObj = new Date(mtime);

        return {
          id: name,
          name,
          filename: name,
          title: titleText || "",
          path: cleanPath,
          url: `/p2p/${getDevSegment()}${encodeURI(cleanPath)}`,
          prompt: promptText || "",
          date: isNaN(dateObj.getTime()) ? new Date() : dateObj,
          size: f.size || 0,
          promptExpanded: false,
          isDeleting: false,
          isPlaceholder: false
        };
      })
    );

    items = sortItemsList(parsedItems, sortOrder);
    isLoadingFiles = false;
    dispatch("count-change", items.length);

    await tick();
    if (sortOrder === "asc") {
      scrollToBottom(false);
    } else {
      scrollToTop(false);
    }

    // Background fetch prompt text for items without one via WebRTC
    fetchMissingPrompts();
  }

  async function ensureItemPromptAndTitle(item) {
    if (!item) return;
    if (item.title && item.prompt) return;
    const readmePath = item.path.replace(/\.[^/.]+$/, "") + ".Readme.md";
    try {
      const drive = (foundDevice && foundDevice.drive) || deviceDrive || (typeof window !== "undefined" && window.currentP2PDrive);
      let txt = "";
      if (drive && typeof drive.homeRead === "function") {
        const b64 = await drive.homeRead(readmePath).catch(() => null);
        if (b64) txt = decodeURIComponent(escape(atob(b64)));
      }
      if (!txt && httpDrive && typeof httpDrive.fetch === "function") {
        const resp = await httpDrive.fetch(readmePath).catch(() => null);
        if (resp && resp.ok) {
          txt = await resp.text().catch(() => "");
        }
      }
      if (txt) {
        const titleMatch = txt.match(/^#\s*([^\n]+)/);
        if (titleMatch && !item.title) {
          item.title = titleMatch[1].trim();
        }
        const cleaned = txt.replace(/^#\s*[^\n]+\n+/, "").trim();
        if (!item.prompt) {
          item.prompt = cleaned || txt.trim();
        }
      }
    } catch (_) {}
  }

  async function fetchMissingPrompts() {
    for (const item of items) {
      if (!item.prompt || !item.title) {
        const readmePath = item.path.replace(/\.[^/.]+$/, "") + ".Readme.md";
        try {
          const drive = (foundDevice && foundDevice.drive) || deviceDrive || (typeof window !== "undefined" && window.currentP2PDrive);
          if (drive && typeof drive.homeRead === "function") {
            const b64 = await drive.homeRead(readmePath);
            if (b64) {
              const txt = decodeURIComponent(escape(atob(b64)));
              if (txt) {
                const titleMatch = txt.match(/^#\s*([^\n]+)/);
                if (titleMatch && !item.title) {
                  item.title = titleMatch[1].trim();
                }
                const cleaned = txt.replace(/^#\s*[^\n]+\n+/, "").trim();
                if (!item.prompt) {
                  item.prompt = cleaned || txt.trim();
                }
                items = [...items];
                continue;
              }
            }
          }
        } catch (_) {}
      }
    }
  }

  function getDevSegment() {
    const dev = resolveDevice();
    let seg = (dev?.prefix || dev?.linkId || targetLinkId || targetDevice?.linkId || targetDevice?.name || "").trim();
    if (!seg) {
      const first = deviceRegistry.list().find((d) => d.drive && d.drive.isConnected) || deviceRegistry.list()[0];
      if (first) {
        seg = first.prefix || first.linkId || "";
      }
    }
    seg = seg.replace(/^\/+|\/+$/g, "");
    return seg || "local";
  }

  function getThumbnailUrl(item) {
    if (!item) return "";
    if (item.data) return item.data;
    if (item.thumbBlobUrl) return item.thumbBlobUrl;
    if (item.blobUrl) return item.blobUrl;
    const dev = getDevSegment();
    const cleanPath = (item.path || "").startsWith("/") ? item.path : `/${item.path || ""}`;
    return `/p2p/${dev}${encodeURI(cleanPath)}?resize=true&width=480&height=480`;
  }

  function getFullImageUrl(item) {
    if (!item) return "";
    if (item.data) return item.data;
    if (item.blobUrl) return item.blobUrl;
    const dev = getDevSegment();
    const cleanPath = (item.path || "").startsWith("/") ? item.path : `/${item.path || ""}`;
    return `/p2p/${dev}${encodeURI(cleanPath)}`;
  }

  // Просмотр полноразмерной версии с ресайзом по настройкам вьювера (scaleTo).
  // Для скачивания используется getFullImageUrl — там нужен оригинал.
  function getViewImageUrl(item) {
    if (!item) return "";
    if (item.data) return item.data;
    if (item.blobUrl) return item.blobUrl;
    const dev = getDevSegment();
    const cleanPath = (item.path || "").startsWith("/") ? item.path : `/${item.path || ""}`;
    const vs = getViewerSettings();
    const base = `/p2p/${dev}${encodeURI(cleanPath)}`;
    return vs.downscaleImages ? `${base}?resize=true&width=${vs.scaleTo}&height=${vs.scaleTo}` : base;
  }

  async function fetchImageBlobViaWebRTC(filePath, query = "") {
    const drive = (foundDevice && foundDevice.drive) || deviceDrive || (typeof window !== "undefined" && window.currentP2PDrive);
    if (!drive || !drive.isConnected) return null;
    try {
      const openRes = await drive.sendRPC("Open", { path: filePath, query });
      if (openRes && openRes.status === "ok" && openRes.result) {
        const totalSize = openRes.result.size || 0;
        const mimeType = openRes.result.mimeType || (filePath.endsWith(".jpg") || filePath.endsWith(".jpeg") ? "image/jpeg" : "image/png");
        const rawBlob = await drive.getFileContent(filePath, totalSize, null, 0, query);
        if (rawBlob && rawBlob.size > 0) {
          const typedBlob = new Blob([rawBlob], { type: mimeType });
          return URL.createObjectURL(typedBlob);
        }
      }
    } catch (e) {
      console.warn("[Creations] WebRTC fetch blob failed:", filePath, e);
    }
    return null;
  }

  async function fetchLargeImageBlob(fetchFn, url) {
    const CHUNK_SIZE = 128 * 1024; // 128 KB safe chunk size for WebRTC SCTP
    let offset = 0;
    let totalLength = null;
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

    return new Blob(chunks, { type: "image/png" });
  }

  async function handleImageError(item, event) {
    if (!item || item._fallbackTried) return;
    item._fallbackTried = true;
    try {
      // 1. Try reading via WebRTC drive Open + getFileContent (keep thumbnail separate from full blobUrl)
      const thumbBlob = await fetchImageBlobViaWebRTC(item.path, "resize=true&width=480&height=480");
      if (thumbBlob) {
        item.thumbBlobUrl = thumbBlob;
        items = [...items];
        return;
      }
      const fullBlob = await fetchImageBlobViaWebRTC(item.path, "");
      if (fullBlob) {
        item.blobUrl = fullBlob;
        const key = item.path || item.name;
        imageBlobCache[key] = fullBlob;
        imageBlobCache = { ...imageBlobCache };
        items = [...items];
        return;
      }

      // 2. Try deviceDrive homeRead / readFileChunked if available
      const drive = (foundDevice && foundDevice.drive) || deviceDrive || (typeof window !== "undefined" && window.currentP2PDrive);
      if (drive) {
        let b64 = "";
        if (typeof drive.homeRead === "function") {
          b64 = await drive.homeRead(item.path);
        } else if (typeof drive.readFileChunked === "function") {
          b64 = await drive.readFileChunked(item.path);
        }
        if (b64) {
          const mime = item.name.endsWith(".jpg") || item.name.endsWith(".jpeg") ? "image/jpeg" : "image/png";
          item.blobUrl = `data:${mime};base64,${b64}`;
          const key = item.path || item.name;
          imageBlobCache[key] = item.blobUrl;
          imageBlobCache = { ...imageBlobCache };
          items = [...items];
          return;
        }
      }
    } catch (err) {
      console.warn("[Creations] WebRTC fallback image load failed for", item.name, err);
    }
  }

  async function loadAvailableLoras() {
    if (lorasLoaded) return;
    try {
      const ep = "/assistant/loras";
      const res = httpDrive ? await httpDrive.fetch(ep) : await fetch(ep);
      if (res && res.ok) {
        const data = await res.json();
        const list = Array.isArray(data) ? data : (data.loras || data.data || []);
        availableLoras = list.map((l) => ({
          name: l.name || l,
          type: l.type || "character",
          mode: l.mode || (l.nsfw ? "fun" : "work") || "work",
        }));
        lorasLoaded = true;
      }
    } catch (_) {}
  }

  function formatCreationTime(dateObj) {
    if (!dateObj) return "";
    try {
      const now = new Date();
      const diffMs = now - dateObj;
      const diffMin = Math.floor(diffMs / 60000);
      if (diffMin < 1) return $_("just_now", {}, "just now");
      if (diffMin < 60) return `${diffMin}m ago`;
      const diffHours = Math.floor(diffMin / 60);
      if (diffHours < 24) return `${diffHours}h ago`;
      const diffDays = Math.floor(diffHours / 24);
      if (diffDays < 7) return `${diffDays}d ago`;
      return dateObj.toLocaleDateString(undefined, { month: "short", day: "numeric" });
    } catch (_) {
      return "";
    }
  }

  function resizeTextarea() {
    if (!textareaEl) return;
    textareaEl.style.height = "auto";
    const minH = 36;
    const maxH = 180;
    const scrollH = textareaEl.scrollHeight;
    if (!inputPrompt || !inputPrompt.trim()) {
      textareaEl.style.height = minH + "px";
      textareaEl.style.overflowY = "hidden";
      return;
    }
    const newH = Math.min(Math.max(scrollH, minH), maxH);
    textareaEl.style.height = `${newH}px`;
    textareaEl.style.overflowY = scrollH > maxH ? "auto" : "hidden";
  }

  function formatPromptTime(iso) {
    if (!iso) return "";
    try {
      const d = new Date(iso);
      if (isNaN(d.getTime())) return "";
      const now = new Date();
      const diffMs = now - d;
      const diffSec = Math.floor(diffMs / 1000);
      if (diffSec < 60) return "just now";
      const diffMin = Math.floor(diffSec / 60);
      if (diffMin < 60) return `${diffMin}m ago`;
      const diffHours = Math.floor(diffMin / 60);
      if (diffHours < 24) return `${diffHours}h ago`;
      const diffDays = Math.floor(diffHours / 24);
      if (diffDays < 7) return `${diffDays}d ago`;
      return d.toLocaleDateString(undefined, { month: "short", day: "numeric" });
    } catch (_) {
      return "";
    }
  }

  function isLoraActive(loraName) {
    if (!inputPrompt) return false;
    return inputPrompt.includes("<" + loraName + ">") || inputPrompt.toLowerCase().includes("<" + loraName.toLowerCase() + ">");
  }

  function isStyleActive(styleName) {
    const target = styleName.toLowerCase();
    const text = (inputPrompt || "").toLowerCase();
    const hasAnyStyleTag = paintingStyles.some((s) => text.includes("<" + s.toLowerCase() + ">"));
    if (hasAnyStyleTag) {
      return text.includes("<" + target + ">");
    }
    return (selectedStyle || "realistic").toLowerCase() === target;
  }

  function selectPaintingStyle(styleName) {
    const target = styleName.toLowerCase();
    selectedStyle = target;
    let text = inputPrompt;
    let replaced = false;
    for (const s of paintingStyles) {
      const tagRegex = new RegExp("<" + s + ">", "gi");
      if (tagRegex.test(text)) {
        text = text.replace(tagRegex, "<" + target + ">");
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      text = "<" + target + "> " + text.trim();
    }
    inputPrompt = text.trim() + " ";
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function toggleLoraChip(loraName) {
    const tag = "<" + loraName + ">";
    let text = inputPrompt;
    const regex = new RegExp("<" + loraName + ">", "gi");
    if (regex.test(text)) {
      text = text.replace(regex, "").replace(/\s{2,}/g, " ").trim() + " ";
    } else {
      const st = selectedStyle || "realistic";
      const hasStyle = paintingStyles.some((s) => text.toLowerCase().includes("<" + s.toLowerCase() + ">"));
      if (!hasStyle) {
        text = `<${st}> ` + text.trim();
      }
      text = text.trim() + " " + tag + " ";
    }
    inputPrompt = text;
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function toggleWithCharacters() {
    withCharacters = !withCharacters;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omc_gen_characters", String(withCharacters));
    }
    const defaultChar = assistantSettingsData?.assistant_model || "Domi";
    if (withCharacters) {
      const hasAnyChar = availableLoras.filter((l) => l.type === "character").some((l) => isLoraActive(l.name));
      if (!hasAnyChar) {
        toggleLoraChip(defaultChar);
      }
    } else {
      let text = inputPrompt;
      availableLoras.filter((l) => l.type === "character").forEach((l) => {
        text = text.replace(new RegExp("<" + l.name + ">", "gi"), "");
      });
      inputPrompt = text.replace(/\s{2,}/g, " ").trim() + " ";
      if (textareaEl) setTimeout(resizeTextarea, 0);
    }
  }

  function selectPrompt(p) {
    const txt = typeof p === "string" ? p : p.text || p.prompt || "";
    let full = txt.replace(/^\/generate\s*/i, "").trim();
    if (p.style && !full.toLowerCase().includes("<" + p.style.toLowerCase() + ">")) {
      full = full.trim() + " <" + p.style.toLowerCase() + ">";
      selectedStyle = p.style.toLowerCase();
    }
    inputPrompt = full.trim() + " ";
    showPromptsPopup = false;
    if (!showImageToolpanel) {
      showImageToolpanel = true;
      loadAvailableLoras();
    }
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function handleDeletePrompt(e, p) {
    e.stopPropagation();
    if (chatStore && typeof chatStore.deletePrompt === "function") {
      chatStore.deletePrompt(p._id || p.id);
    }
  }

  function handleWindowClick(e) {
    if (showPromptsPopup && !e.target.closest(".prompts-popup") && !e.target.closest(".prompts-history-btn")) {
      showPromptsPopup = false;
    }
  }

  function handleScroll() {
    if (!scrollContainerEl) return;
    const { scrollTop, scrollHeight, clientHeight } = scrollContainerEl;
    const distanceToBottom = scrollHeight - (scrollTop + clientHeight);
    showScrollTop = scrollTop > 160;
    showScrollBottom = distanceToBottom > 160;
  }

  function scrollToTop(smooth = true) {
    if (scrollContainerEl) {
      scrollContainerEl.scrollTo({ top: 0, behavior: smooth ? "smooth" : "auto" });
      setTimeout(() => handleScroll(), 300);
    }
  }

  function scrollToBottom(smooth = true) {
    if (scrollContainerEl) {
      scrollContainerEl.scrollTo({ top: scrollContainerEl.scrollHeight, behavior: smooth ? "smooth" : "auto" });
      setTimeout(() => handleScroll(), 300);
    }
  }

  function useSuggestion(s) {
    inputPrompt = s.text;
    if (s.style) selectedStyle = s.style.toLowerCase();
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function copyPrompt(text, id) {
    if (!navigator.clipboard || !text) return;
    navigator.clipboard.writeText(text).then(() => {
      copiedPromptId = id;
      setTimeout(() => { copiedPromptId = null; }, 2000);
    });
  }

  function repeatPrompt(text, style = "") {
    inputPrompt = text;
    if (style) selectedStyle = style.toLowerCase();
    if (textareaEl) {
      textareaEl.focus();
      setTimeout(resizeTextarea, 0);
    }
  }

  function downloadImage(item) {
    const src = item.blobUrl || `${getFullImageUrl(item)}?download=true`;
    const a = document.createElement("a");
    a.href = src;
    a.download = item.name || "creation.png";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  }

  function shareImage(item) {
    dispatch("open-file", {
      source: {
        path: item.path,
        fullPath: item.path,
        name: item.name,
        isDir: false,
        type: "image"
      }
    });
  }

  async function openGallery(activeItem) {
    const validItems = items.filter((it) => !it.isPlaceholder);
    if (!validItems.length) return;

    if (activeItem) {
      await ensureItemPromptAndTitle(activeItem);
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
      // Показ — ресайз по настройкам вьювера (не оригинал); скачивание — полный файл.
      const srcUrl = it.blobUrl || getViewImageUrl(it);
      const thumbUrl = it.thumbBlobUrl || it.blobUrl || getThumbnailUrl(it);
      const rawTitle = (it.title || "").trim();
      const rawDesc = (it.prompt || "").trim();
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

  function promptDelete(item) {
    itemToDelete = item;
  }

  async function confirmDelete() {
    if (!itemToDelete) return;
    const target = itemToDelete;
    itemToDelete = null;

    target.isDeleting = true;
    items = [...items];

    try {
      const folder = getTargetFolder();
      const fullPath = target.path;
      const readmePath = fullPath.replace(/\.[^/.]+$/, "") + ".Readme.md";

      if (deviceDrive && typeof deviceDrive.removeFile === "function" && deviceDrive.isConnected) {
        await deviceDrive.removeFile(fullPath, true).catch(() => {});
        await deviceDrive.removeFile(readmePath, true).catch(() => {});
      } else if (httpDrive && typeof httpDrive.fetch === "function") {
        await httpDrive.fetch(fullPath, { method: "DELETE" }).catch(() => {});
        await httpDrive.fetch(readmePath, { method: "DELETE" }).catch(() => {});
      }

      // Clear from local list
      items = items.filter((it) => it.id !== target.id);
      dispatch("count-change", items.length);

      // Clean cached blob
      const key = target.path || target.name;
      if (imageBlobCache[key]) {
        try { URL.revokeObjectURL(imageBlobCache[key]); } catch (_) {}
        delete imageBlobCache[key];
      }
    } catch (err) {
      console.error("[Creations] Failed to delete creation:", err);
      target.isDeleting = false;
      items = [...items];
    }
  }

  async function handleSendPrompt() {
    const raw = inputPrompt.trim();
    if (!raw || isGenerating) return;

    let fullPrompt = raw;
    if (!fullPrompt.startsWith("/generate")) {
      fullPrompt = "/generate " + fullPrompt;
    }

    if (selectedStyle && !fullPrompt.toLowerCase().includes("<" + selectedStyle.toLowerCase() + ">")) {
      fullPrompt += ` <${selectedStyle.toLowerCase()}>`;
    }

    if (withCharacters && availableLoras.length > 0) {
      const charLoras = availableLoras.filter((l) => l.type === "character");
      const hasAnyChar = charLoras.some((l) =>
        fullPrompt.toLowerCase().includes("<" + l.name.toLowerCase() + ">")
      );
      if (!hasAnyChar) {
        const defaultModel = assistantSettingsData?.assistant_model || "Domi";
        fullPrompt += ` <${defaultModel}>`;
      }
    }

    const cleanDisplayPrompt = fullPrompt.replace(/^\/generate\s*/i, "").trim();

    // If continuation toggle is OFF, clear prompt and shrink textarea;
    // if ON, keep the prompt text in input for subsequent editing!
    if (!continueGenerating) {
      inputPrompt = "";
      if (textareaEl) {
        textareaEl.value = "";
        textareaEl.style.height = "36px";
        textareaEl.style.overflowY = "hidden";
      }
    } else {
      if (textareaEl) {
        setTimeout(resizeTextarea, 0);
      }
    }

    // Save prompt to recent list
    if (chatStore && typeof chatStore.savePrompt === "function") {
      chatStore.savePrompt(cleanDisplayPrompt, selectedStyle).catch(() => {});
    }

    generationNotice = "";
    isGenerating = true;
    activePhaseStatus = "🎨 Generating image...";

    // Add optimistic placeholder at top (desc) or bottom (asc)
    currentPlaceholder = {
      id: "placeholder_" + Date.now(),
      isPlaceholder: true,
      name: "New Creation",
      filename: "generating.png",
      prompt: cleanDisplayPrompt,
      date: new Date(),
      progressStatus: "🎨 Generating image...",
      partialText: ""
    };

    if (sortOrder === "desc") {
      items = [currentPlaceholder, ...items];
      await tick();
      scrollToTop(true);
    } else {
      items = [...items, currentPlaceholder];
      await tick();
      scrollToBottom(true);
    }

    abortController = new AbortController();

    try {
      const folder = getTargetFolder();
      const baseStorage = getBaseStorage();
      const effectiveKey = omdKey || (typeof window !== "undefined" && window.__omdKey) || "";
      const pushDesc = buildDevicePush();
      const mcpPayload = (typeof window !== "undefined" && typeof window.__omdGetMcpPayload === "function")
        ? window.__omdGetMcpPayload()
        : {};
      const payload = {
        omd_key: effectiveKey,
        prompt: fullPrompt,
        chat: "creations_session",
        history: [],
        client: "svar",
        image_delivery: "inline",
        device_push: pushDesc || undefined,
        mcp_keys: mcpPayload,
        settings: Object.assign(
          {},
          assistantSettingsData || {},
          {
            platform: getClientPlatform(),
            content_mode: contentMode,
            defaultStorage: baseStorage,
            language: $locale || "en",
            mcp_keys: mcpPayload
          }
        )
      };

      const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
        ? (p, opts) => httpDrive.fetch(p, opts)
        : (p, opts) => fetch(p, opts);

      const res = await fetchFn("/chat/stream", {
        method: "POST",
        headers: {
          Accept: "text/event-stream",
          "Content-Type": "application/json",
          "X-Client": "svar",
          "X-Image-Delivery": "inline"
        },
        body: JSON.stringify(payload),
        signal: abortController.signal
      });

      if (!res.ok) {
        let errText = `HTTP ${res.status}`;
        try {
          const t = await res.text();
          if (t) errText = t;
        } catch (_) {}
        throw new Error(errText);
      }

      let generatedImage = null;
      let streamedDescription = "";

      const sse = new SseLineBuffer((ev) => {
        if (!ev) return;
        if (ev.status) {
          currentPlaceholder.progressStatus = ev.status === "generating" ? "🎨 Generating image..." : ev.status;
          activePhaseStatus = currentPlaceholder.progressStatus;
          items = [...items];
        }
        if (ev.delta) {
          streamedDescription += ev.delta;
          currentPlaceholder.partialText = streamedDescription;
          items = [...items];
          if (sortOrder === "asc") {
            tick().then(() => scrollToBottom(false));
          }
        }
        if (ev.image && typeof ev.image === "object") {
          generatedImage = ev.image;
        }
      });

      const reader = res.body.getReader();
      while (true) {
        const { done, value } = await reader.read();
        if (done) {
          sse.end();
          break;
        }
        sse.push(value);
      }

      if (generatedImage) {
        const rawFileName = (generatedImage.path || "").split("/").pop() || `creation_${Date.now()}.png`;
        const fileName = rawFileName;
        const fullImagePath = `${folder}/${fileName}`.replace(/\/+/g, "/");
        const p2pUrl = `/p2p/${getDevSegment()}${encodeURI(fullImagePath)}`;

        const newItem = {
          id: fileName,
          name: fileName,
          filename: fileName,
          title: generatedImage.title || cleanDisplayPrompt || "",
          path: fullImagePath,
          url: p2pUrl,
          data: generatedImage.data || null,
          blobUrl: generatedImage.data || null,
          prompt: generatedImage.description || generatedImage.prompt || cleanDisplayPrompt,
          date: new Date(),
          size: 0,
          promptExpanded: false,
          isDeleting: false,
          isPlaceholder: false
        };

        if (generatedImage.data) {
          imageBlobCache[fullImagePath] = generatedImage.data;
          imageBlobCache[fileName] = generatedImage.data;
          imageBlobCache[p2pUrl] = generatedImage.data;
        }

        // Save generated image and Readme directly to deviceDrive so they persist in user's folder
        (async () => {
          try {
            let blob = null;
            let b64 = "";
            if (generatedImage.data) {
              const commaIdx = generatedImage.data.indexOf(",");
              b64 = commaIdx !== -1 ? generatedImage.data.slice(commaIdx + 1) : generatedImage.data;
            } else if (generatedImage.url || generatedImage.path) {
              const cleanP = (generatedImage.path || "").replace(/^\/+/, "");
              const ep = generatedImage.url && generatedImage.url.startsWith("/chat/image/")
                ? generatedImage.url
                : (cleanP ? `/chat/image/${cleanP}` : "");
              if (ep) {
                const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
                  ? (p, opts) => httpDrive.fetch(p, opts)
                  : (p, opts) => fetch(p, opts);
                blob = await fetchLargeImageBlob(fetchFn, ep);
                if (blob) {
                  const blobUrl = URL.createObjectURL(blob);
                  newItem.blobUrl = blobUrl;
                  imageBlobCache[fullImagePath] = blobUrl;
                  imageBlobCache[p2pUrl] = blobUrl;
                  imageBlobCache = { ...imageBlobCache };
                  items = [...items];
                  b64 = await new Promise((resolve) => {
                    const reader = new FileReader();
                    reader.onloadend = () => {
                      const res = reader.result;
                      const c = res ? res.indexOf(",") : -1;
                      resolve(c !== -1 ? res.slice(c + 1) : (res || ""));
                    };
                    reader.readAsDataURL(blob);
                  });
                }
              }
            }
            const targetDrive = deviceDrive
              || targetDevice?.drive
              || (deviceRegistry.list().find((x) => x && x.local && x.drive)?.drive)
              || (deviceRegistry.list().find((x) => x && !x.isAiNode && x.drive)?.drive);

            if (b64 && targetDrive) {
              await ensureFolderRecursive(targetDrive, folder);
              const title = generatedImage.title || cleanDisplayPrompt || "";
              const desc = generatedImage.description || generatedImage.prompt || cleanDisplayPrompt || "";
              const readmeContent = `# ${title}\n\n${desc}\n`;
              const readmeB64 = btoa(unescape(encodeURIComponent(readmeContent)));
              const readmeFilename = fileName.replace(/\.[^/.]+$/, "") + ".Readme.md";
              const fullReadmePath = `${folder}/${readmeFilename}`;

              if (typeof targetDrive.homeWrite === "function") {
                await targetDrive.homeWrite(fullImagePath, b64);
                await targetDrive.homeWrite(fullReadmePath, readmeB64);
              } else if (typeof targetDrive.writeFileChunked === "function") {
                await targetDrive.writeFileChunked(fullImagePath, b64);
                await targetDrive.writeFileChunked(fullReadmePath, readmeB64);
              }
              console.log("[Creations] Saved generated image and Readme to targetDrive:", fullImagePath);
            }
          } catch (err) {
            console.warn("[Creations] Failed saving generated image to deviceDrive:", err);
          }
        })();

        if (!generatedImage.data && !newItem.blobUrl) {
          fetchImageBlobViaWebRTC(fullImagePath, "resize=true&width=480&height=480").then((blobUrl) => {
            if (blobUrl) {
              newItem.blobUrl = blobUrl;
              imageBlobCache[fullImagePath] = blobUrl;
              imageBlobCache[p2pUrl] = blobUrl;
              imageBlobCache = { ...imageBlobCache };
              items = [...items];
            }
          });
        }

        // Replace placeholder with final item
        items = items.map((it) => (it.id === currentPlaceholder.id ? newItem : it));
        dispatch("count-change", items.length);
        await tick();
        if (sortOrder === "desc") {
          scrollToTop(true);
        } else {
          scrollToBottom(true);
        }
      } else {
        // Remove placeholder if no image was returned
        items = items.filter((it) => it.id !== currentPlaceholder.id);
        if (streamedDescription) {
          generationNotice = streamedDescription;
        }
      }
    } catch (err) {
      if (err.name !== "AbortError") {
        console.error("[Creations] Generation error:", err);
      }
      items = items.filter((it) => it.id !== currentPlaceholder?.id);
    } finally {
      isGenerating = false;
      activePhaseStatus = "";
      currentPlaceholder = null;
      abortController = null;
    }
  }

  function stopGenerating() {
    if (abortController) {
      try { abortController.abort(); } catch (_) {}
    }
    isGenerating = false;
    activePhaseStatus = "";
    if (currentPlaceholder) {
      items = items.filter((it) => it.id !== currentPlaceholder.id);
      currentPlaceholder = null;
    }
  }

  function handleKeydown(e) {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      handleSendPrompt();
    }
  }

  let isCheckingKrea = false;

  async function handleSwitchToKrea() {
    if (isCheckingKrea) return;
    isCheckingKrea = true;
    generationNotice = "";
    try {
      const fetchFn = (httpDrive && typeof httpDrive.fetch === "function")
        ? (p, opts) => httpDrive.fetch(p, opts)
        : (p, opts) => fetch(p, opts);
      const mcpPayload = (typeof window !== "undefined" && typeof window.__omdGetMcpPayload === "function")
        ? window.__omdGetMcpPayload()
        : {};
      const res = await fetchFn("/krea/status", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-Client": "svar", "X-OMD-Platform": getClientPlatform() },
        body: JSON.stringify({
          omd_key: omdKey || (typeof window !== "undefined" && window.__omdKey) || "",
          mcp_keys: mcpPayload,
          settings: Object.assign({}, assistantSettingsData || {}, {
            platform: getClientPlatform(),
            language: $locale || "en",
            mcp_keys: mcpPayload
          })
        })
      });
      const data = await res.json().catch(() => ({}));
      if (data && data.allowed) {
        dispatch("switch-krea");
      } else {
        generationNotice = data.error || $_("krea_unavailable", {}, "Creative Studio is currently unavailable.");
      }
    } catch (e) {
      generationNotice = String(e.message || e);
    } finally {
      isCheckingKrea = false;
    }
  }
</script>

<svelte:window bind:innerWidth={innerWidth} on:click={handleWindowClick} />

<div class="creations-app skin-chat {isDarkTheme ? 'dark-theme' : 'light-theme'}" class:overlay-header={isOverlayHeader}>
  <!-- Top Navigation Header -->
  <header class="creations-header">
    <div class="header-left">
      {#if !sidebarOpen}
        <button
          class="header-icon-btn sidebar-toggle-btn"
          title={$_("unfold", {}, "Expand sidebar")}
          on:click={() => dispatch("toggle-sidebar")}
        >
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
            <line x1="9" y1="3" x2="9" y2="21" />
          </svg>
        </button>
      {/if}

      {#if kreaSupported}
        <div class="header-mode-toggle-group">
          <div class="header-mode-toggle" role="tablist">
            <button
              type="button"
              class="mode-toggle-btn active"
              title="CyberCraft (SDXL)"
            >
              <span class="mode-icon">✨</span>
              <span class="mode-text mode-text-full">CyberCraft</span>
              <span class="mode-text mode-text-short">Cyber</span>
            </button>
            <button
              type="button"
              class="mode-toggle-btn"
              class:is-checking={isCheckingKrea}
              title={$_("krea_studio", {}, "Creative Studio")}
              disabled={isCheckingKrea}
              on:click={handleSwitchToKrea}
            >
              <span class="mode-icon">⚡</span>
              <span class="mode-text mode-text-full">
                {#if isCheckingKrea}
                  {$_("connecting", {}, "Connecting...")}
                {:else}
                  {$_("krea_studio", {}, "Creative Studio")}
                {/if}
              </span>
              <span class="mode-text mode-text-short">
                {#if isCheckingKrea}
                  ...
                {:else}
                  Creative
                {/if}
              </span>
            </button>
          </div>
          {#if items.filter(i => !i.isPlaceholder).length > 0}
            <span class="header-count-pill" title={$_("generated", {}, "Creations")}>
              {items.filter(i => !i.isPlaceholder).length}
            </span>
          {/if}
        </div>
      {:else}
        <div class="header-title-box">
          <div class="header-title-row">
            <span class="sparkle-icon">✨</span>
            <h1 class="header-title">{$_("generated", {}, "Creations")}</h1>
            <span class="count-pill">{items.filter(i => !i.isPlaceholder).length}</span>
          </div>
        </div>
      {/if}
    </div>

    <div class="header-right">
      <!-- Sort Order Toggle -->
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

      <!-- Roll / Grid View Switcher (matching Files style) — desktop only -->
      {#if !isMobile}
      <div class="wx-modes">
        <div class="wx-segmented">
          <button
            type="button"
            class:wx-selected={viewMode === "roll"}
            title={$_("view_roll", {}, "Feed View")}
            on:click={() => setViewMode("roll")}
          >
            <i class="wx-icon wxi-view-sequential wx-only"></i>
          </button>
          <button
            type="button"
            class:wx-selected={viewMode === "grid"}
            title={$_("view_grid", {}, "Grid View")}
            on:click={() => setViewMode("grid")}
          >
          <i class="wx-icon wxi-view-grid wx-only"></i>
          </button>
        </div>
      </div>
      {/if}

      <!-- Refresh Button -->
      <button
        class="header-icon-btn"
        class:spinning={isLoadingFiles}
        title="Refresh creations"
        on:click={loadCreations}
      >
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
          <polyline points="23 4 23 10 17 10" />
          <polyline points="1 20 1 14 7 14" />
          <path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15" />
        </svg>
      </button>

      <!-- Close Button -->
      <button class="header-icon-btn close-btn" title={$_("close", {}, "Close")} on:click={() => dispatch("close")}>
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
          <line x1="18" y1="6" x2="6" y2="18" />
          <line x1="6" y1="6" x2="18" y2="18" />
        </svg>
      </button>
    </div>
  </header>

  <!-- Scrollable Gallery Area -->
  <div
    class="creations-feed"
    bind:this={scrollContainerEl}
    bind:clientHeight={scrollContainerHeight}
    on:scroll={handleScroll}
    style="padding-bottom: {(composerHeight || 90) + 24}px;"
  >
    {#if !isStorageConfigured || !isStorageAvailable}
      <div class="storage-warning-banner" in:fade={{ duration: 200 }}>
        <div class="storage-warning-icon">
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z" />
            <line x1="12" y1="9" x2="12" y2="13" />
            <line x1="12" y1="17" x2="12.01" y2="17" />
          </svg>
        </div>
        <div class="storage-warning-content">
          <div class="storage-warning-title">
            {!isStorageConfigured
              ? $_("storage_not_configured_title", {}, "Storage folder is not configured")
              : $_("storage_unavailable_title", {}, "Storage device is offline")}
          </div>
          <div class="storage-warning-desc">
            {!isStorageConfigured
              ? $_("storage_not_configured_desc", {}, "Select a storage folder on your device to save and view your creations.")
              : $_("storage_unavailable_desc", {}, "The selected storage device is unreachable. Choose another folder to continue.")}
          </div>
        </div>
        <button
          type="button"
          class="storage-warning-btn"
          on:click={handleBrowseStorage}
        >
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
          </svg>
          <span>{$_("choose_folder", {}, "Choose folder")}</span>
        </button>
      </div>
    {/if}

    {#if isLoadingFiles && items.length === 0}
      <div class="skeleton-feed" in:fade>
        {#each [1, 2] as _}
          <div class="creation-card-skeleton">
            <div class="skeleton-img"></div>
            <div class="skeleton-meta">
              <div class="skeleton-line w-40"></div>
              <div class="skeleton-line w-80"></div>
            </div>
          </div>
        {/each}
      </div>
    {:else if items.length === 0 && !isGenerating}
      {#if !isStorageConfigured || !isStorageAvailable}
        <!-- Need Storage Folder Configuration State -->
        <div class="empty-creations-state" in:fade={{ duration: 250 }}>
          <div class="empty-spark-glow folder-prompt-glow">
            <svg viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="currentColor" stroke-width="1.6">
              <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
              <line x1="12" y1="11" x2="12" y2="17" />
              <line x1="9" y1="14" x2="15" y2="14" />
            </svg>
          </div>
          <h2 class="empty-title">
            {!isStorageConfigured
              ? $_("choose_storage_folder_title", {}, "Choose storage folder")
              : $_("storage_unavailable_title", {}, "Storage device is offline")}
          </h2>
          <p class="empty-desc">
            {!isStorageConfigured
              ? $_("choose_storage_folder_desc", {}, "To generate and save creations, select a storage folder on your device")
              : $_("storage_unavailable_desc", {}, "The selected storage device is unreachable. Choose another folder to continue.")}
          </p>
          <button class="primary-select-folder-btn" on:click={handleBrowseStorage}>
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
            </svg>
            <span>{$_("choose_folder", {}, "Choose folder")}</span>
          </button>
        </div>
      {:else}
        <!-- Empty State with Starter Inspiration -->
        <div class="empty-creations-state" in:fade={{ duration: 250 }}>
          <div class="empty-spark-glow">
            <svg viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="currentColor" stroke-width="1.5">
              <path d="M12 2l2.4 7.2L22 12l-7.6 2.8L12 22l-2.4-7.2L2 12l7.6-2.8z" />
            </svg>
          </div>
          <h2 class="empty-title">{$_("no_creations_yet", {}, "No creations yet")}</h2>
          <p class="empty-desc">
            {$_("creations_empty_subtitle", {}, "Generate stunning AI images using the prompt below. All creations are stored privately on your device.")}
          </p>

          <!-- Inspiration Starter Pills -->
          <div class="starter-prompts">
            <span class="starter-label">{$_("inspiration", {}, "Try an idea")}:</span>
            <div class="starter-chips">
              {#each starterSuggestions as s}
                <button class="starter-chip" on:click={() => useSuggestion(s)}>
                  <span class="chip-sparkle">✨</span>
                  <span>{s.text}</span>
                </button>
              {/each}
            </div>
          </div>
        </div>
      {/if}
    {:else}
      <!-- Gallery Container (Feed / Grid) -->
      <div class="creations-container {viewMode}">
        {#each items as item (item.id)}
          {#if item.isPlaceholder}
            <!-- Generating Placeholder Card -->
            <div class="creation-card placeholder-card" in:fly={{ y: 20, duration: 300 }}>
              <div class="placeholder-img-wrap" style={viewMode === "roll" ? `max-height: ${dynamicMaxImageHeight}px;` : ""}>
                <div class="magic-glow-border"></div>
                <div class="placeholder-inner">
                  <div class="pulse-loader">
                    <span class="spinner-ring"></span>
                    <span class="spinner-core">🎨</span>
                  </div>
                  <span class="placeholder-status">{item.progressStatus}</span>
                  {#if item.partialText}
                    <p class="placeholder-subtext">{item.partialText}</p>
                  {/if}
                </div>
                <button
                  class="cancel-btn"
                  title="Cancel generation"
                  on:click={stopGenerating}
                >
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                    <line x1="18" y1="6" x2="6" y2="18" />
                    <line x1="6" y1="6" x2="18" y2="18" />
                  </svg>
                </button>
              </div>
              <div class="card-footer">
                <div class="card-prompt-line">
                  <span class="prompt-icon">📷</span>
                  <span class="prompt-preview">{item.prompt}</span>
                </div>
              </div>
            </div>
          {:else}
            <!-- Regular Image Card -->
            <div
              class="creation-card"
              class:deleting={item.isDeleting}
              in:fly={{ y: 15, duration: 250 }}
            >
              <!-- Card Top Overlay / Date -->
              <div class="card-header-bar">
                <span class="card-time">{formatCreationTime(item.date)}</span>
                <div class="card-top-actions">
                  <button
                    class="card-top-btn delete-btn"
                    title={$_("delete_creation", {}, "Delete creation")}
                    on:click={() => promptDelete(item)}
                  >
                    <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                      <polyline points="3 6 5 6 21 6" />
                      <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                    </svg>
                  </button>
                </div>
              </div>

              <!-- Main Image Preview Box -->
              <div
                class="card-image-wrap"
                role="button"
                tabindex="0"
                style={viewMode === "roll" ? `max-height: ${dynamicMaxImageHeight}px;` : ""}
                on:click={() => openGallery(item)}
                on:keydown={(e) => e.key === "Enter" && openGallery(item)}
              >
                <img
                  src={getThumbnailUrl(item)}
                  alt={item.prompt || item.name}
                  loading="lazy"
                  class="creation-img"
                  style={viewMode === "roll" ? `max-height: ${dynamicMaxImageHeight}px;` : ""}
                  on:error={(e) => handleImageError(item, e)}
                />
              </div>

              <!-- Card Details & Actions -->
              <div class="card-footer">
                <div class="card-meta-row">
                  <button
                    class="prompt-accordion-toggle"
                    class:expanded={item.promptExpanded}
                    on:click={() => { item.promptExpanded = !item.promptExpanded; items = [...items]; }}
                  >
                    <span class="camera-icon">📷</span>
                    <span class="prompt-label">{$_("description", {}, "Description")}</span>
                    <svg class="chevron-icon" viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                      <polyline points="6 9 12 15 18 9" />
                    </svg>
                  </button>

                  <div class="card-toolbar">
                    {#if item.prompt}
                      <button
                        class="toolbar-btn"
                        class:copied={copiedPromptId === item.id}
                        title={$_("copy_prompt", {}, "Copy prompt")}
                        on:click={() => copyPrompt(item.prompt, item.id)}
                      >
                        {#if copiedPromptId === item.id}
                          <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="#10b981" stroke-width="2.5">
                            <polyline points="20 6 9 17 4 12" />
                          </svg>
                        {:else}
                          <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                            <rect x="9" y="9" width="13" height="13" rx="2" ry="2" />
                            <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1" />
                          </svg>
                        {/if}
                      </button>

                      <button
                        class="toolbar-btn"
                        title={$_("repeat_prompt", {}, "Repeat prompt")}
                        on:click={() => repeatPrompt(item.prompt, selectedStyle)}
                      >
                        <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                          <polyline points="1 4 1 10 7 10" />
                          <path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10" />
                        </svg>
                      </button>
                    {/if}

                    <button
                      class="toolbar-btn"
                      title={$_("download", {}, "Download image")}
                      on:click={() => downloadImage(item)}
                    >
                      <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                        <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                        <polyline points="7 10 12 15 17 10" />
                        <line x1="12" y1="15" x2="12" y2="3" />
                      </svg>
                    </button>

                    <button
                      class="toolbar-btn"
                      title={$_("share", {}, "Share")}
                      on:click={() => shareImage(item)}
                    >
                      <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                        <circle cx="18" cy="5" r="3" />
                        <circle cx="6" cy="12" r="3" />
                        <circle cx="18" cy="19" r="3" />
                        <line x1="8.59" y1="13.51" x2="15.42" y2="17.49" />
                        <line x1="15.41" y1="6.51" x2="8.59" y2="10.49" />
                      </svg>
                    </button>
                  </div>
                </div>
                {#if item.promptExpanded && item.prompt}
                  <div class="prompt-expanded-content" transition:fly={{ y: -6, duration: 150 }}>
                    <p>{item.prompt}</p>
                  </div>
                {/if}
              </div>
            </div>
          {/if}
        {/each}
      </div>
    {/if}
  </div>

  <!-- Bottom Pinned Image Generation Prompt Bar (only when storage configured) -->
  {#if isStorageConfigured}
    <!-- Floating Quick Scroll Dock (Bottom Right, Telegram/WhatsApp style) -->
    <div class="scroll-fab-dock" style="bottom: {(composerHeight || 90) + 14}px;">
      {#if showScrollTop}
        <button
          type="button"
          class="scroll-fab-btn"
          title={$_("scroll_top", {}, "Scroll to top")}
          on:click={() => scrollToTop(true)}
          transition:fly={{ y: 8, duration: 180 }}
        >
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
            <polyline points="18 15 12 9 6 15" />
          </svg>
        </button>
      {/if}

      {#if showScrollBottom}
        <button
          type="button"
          class="scroll-fab-btn"
          title={$_("scroll_bottom", {}, "Scroll to bottom")}
          on:click={() => scrollToBottom(true)}
          transition:fly={{ y: 8, duration: 180 }}
        >
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
            <polyline points="6 9 12 15 18 9" />
          </svg>
        </button>
      {/if}
    </div>

    <footer class="creations-footer">
      <div class="composer-container" bind:clientHeight={composerHeight}>
        <!-- Prompts History Popup (matches regular chat) -->
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
                <span>{$_("image_prompt_history", {}, "Prompt History")}</span>
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
                  placeholder={$_("search", {}, "Search prompts...")}
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
                    title={$_("delete", {}, "Delete")}
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
                  <span>{$_("no_recent_prompts", {}, "No recent prompts")}</span>
                </div>
              {/each}
            </div>
          </div>
        {/if}

        <!-- Image Generator Toolpanel (matches regular chat) -->
        {#if showImageToolpanel}
          <div class="image-generator-panel">
            <!-- Row 1: Painting styles & Toggles -->
            <div class="gen-panel-row gen-panel-main-row">
              <div class="gen-styles-group">
                <span class="gen-section-label">{$_("painting_style", {}, "Painting style")}:</span>
                <div class="gen-styles-pills">
                  {#each paintingStyles as style}
                    <button
                      type="button"
                      class="gen-style-pill"
                      class:active={isStyleActive(style)}
                      on:click={() => selectPaintingStyle(style)}
                    >
                      {$_(style, {}, style)}
                    </button>
                  {/each}
                </div>
              </div>

              <div class="gen-panel-controls">
                <!-- With characters toggle -->
                <label class="gen-toggle-btn" class:active={withCharacters}>
                  <input
                    type="checkbox"
                    checked={withCharacters}
                    on:change={toggleWithCharacters}
                    class="gen-checkbox-hidden"
                  />
                  <span class="gen-toggle-check">
                    {#if withCharacters}
                      <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="3">
                        <polyline points="20 6 9 17 4 12" />
                      </svg>
                    {/if}
                  </span>
                  <span class="gen-toggle-text">{$_("with_characters", {}, "With characters")}</span>
                </label>

                <!-- Continue generating toggle -->
                <label class="gen-toggle-btn" class:active={continueGenerating}>
                  <input
                    type="checkbox"
                    checked={continueGenerating}
                    on:change={toggleContinueGenerating}
                    class="gen-checkbox-hidden"
                  />
                  <span class="gen-toggle-check">
                    {#if continueGenerating}
                      <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="3">
                        <polyline points="20 6 9 17 4 12" />
                      </svg>
                    {/if}
                  </span>
                  <span class="gen-toggle-text">{$_("continue_generating", {}, "Continue")}</span>
                </label>

                <!-- Prompts History Button -->
                <button
                  type="button"
                  class="gen-icon-btn prompts-history-btn"
                  class:active={showPromptsPopup}
                  title={$_("image_prompt_history", {}, "Prompt history")}
                  on:click|stopPropagation={() => (showPromptsPopup = !showPromptsPopup)}
                >
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                    <circle cx="12" cy="12" r="10" />
                    <polyline points="12 6 12 12 16 14" />
                  </svg>
                </button>

                <!-- Close Generator Button -->
                <button
                  type="button"
                  class="gen-icon-btn gen-close-btn"
                  title={$_("close", {}, "Close")}
                  on:click={() => (showImageToolpanel = false)}
                >
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.5">
                    <line x1="18" y1="6" x2="6" y2="18" />
                    <line x1="6" y1="6" x2="18" y2="18" />
                  </svg>
                </button>
              </div>
            </div>

            <!-- Row 2: Characters & Character Styles (Subpanel) -->
            {#if withCharacters}
              <div class="gen-panel-row gen-panel-loras-row">
                <div class="gen-loras-group">
                  <span class="gen-section-label">{$_("characters", {}, "Characters")}:</span>
                  <div class="gen-lora-chips">
                    {#each availableLoras.filter((l) => l.type === "character" && (contentMode === "fun" || l.mode !== "fun")) as lora}
                      <button
                        type="button"
                        class="gen-lora-chip"
                        class:active={isLoraActive(lora.name)}
                        on:click={() => toggleLoraChip(lora.name)}
                      >
                        {lora.name}
                      </button>
                    {/each}
                  </div>
                </div>

                {#if availableLoras.some((l) => l.type === "style" && (contentMode === "fun" || l.mode !== "fun"))}
                  <span class="gen-divider"></span>
                  <div class="gen-loras-group">
                    <span class="gen-section-label">{$_("character_styles", {}, "Styles")}:</span>
                    <div class="gen-lora-chips">
                      {#each availableLoras.filter((l) => l.type === "style" && (contentMode === "fun" || l.mode !== "fun")) as lora}
                        <button
                          type="button"
                          class="gen-lora-chip"
                          class:active={isLoraActive(lora.name)}
                          on:click={() => toggleLoraChip(lora.name)}
                        >
                          {lora.name}
                        </button>
                      {/each}
                    </div>
                  </div>
                {/if}
              </div>
            {/if}
          </div>
        {/if}

        {#if generationNotice}
          <div class="creations-notice-banner">
            <span class="notice-banner-icon">✨</span>
            <div class="notice-banner-content">
              <span class="notice-banner-text">{noticeText}</span>
              {#if noticeUrl}
                <a href={noticeUrl} target="_blank" rel="noopener noreferrer" class="notice-banner-btn">
                  {$_("open_link", {}, "Open")} →
                </a>
              {/if}
            </div>
            <button type="button" class="notice-banner-close" aria-label="Close" on:click={() => (generationNotice = "")}>&times;</button>
          </div>
        {/if}

        <!-- Composer Pill Input Line -->
        <div class="composer-pill">
          <!-- Plus Action Button (opens/toggles settings) -->
          <div class="composer-action-wrap">
            <button
              type="button"
              class="composer-action-btn"
              class:active={showImageToolpanel}
              title={$_("settings", {}, "Settings")}
              on:click|stopPropagation={() => (showImageToolpanel = !showImageToolpanel)}
            >
              <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.5" class="plus-icon" class:rotated={showImageToolpanel}>
                <line x1="12" y1="5" x2="12" y2="19" />
                <line x1="5" y1="12" x2="19" y2="12" />
              </svg>
            </button>
          </div>

          <!-- Auto-expanding Textarea -->
          <textarea
            bind:this={textareaEl}
            bind:value={inputPrompt}
            class="composer-textarea"
            rows="1"
            placeholder={$_("image_prompt_placeholder", {}, "Describe an image...")}
            on:input={resizeTextarea}
            on:keydown={handleKeydown}
          ></textarea>

          <!-- Right Action Button: Send or Stop -->
          {#if isGenerating}
            <button
              type="button"
              class="composer-stop-btn"
              title={$_("stop", {}, "Stop generating")}
              on:click={stopGenerating}
            >
              <svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor">
                <rect x="6" y="6" width="12" height="12" rx="2" />
              </svg>
            </button>
          {:else}
            <button
              type="button"
              class="composer-send-btn"
              class:active={!!inputPrompt.trim()}
              disabled={!inputPrompt.trim()}
              title={$_("generate_image", {}, "Generate Image")}
              on:click={handleSendPrompt}
            >
              <svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor">
                <path d="M2.01 21L23 12 2.01 3 2 10l15 2-15 2z" />
              </svg>
            </button>
          {/if}
        </div>
      </div>
    </footer>
  {/if}

  <!-- Delete Confirmation Modal -->
  {#if itemToDelete}
    <div class="confirm-modal-overlay" transition:fade={{ duration: 150 }}>
      <div class="confirm-modal-box" transition:fly={{ y: 20, duration: 200 }}>
        <div class="modal-icon-wrap">🗑️</div>
        <h3 class="modal-title">{$_("delete_creation", {}, "Delete creation")}</h3>
        <p class="modal-desc">
          {$_("delete_creation_confirm", {}, "Are you sure you want to delete this creation from your device?")}
        </p>
        <div class="modal-actions">
          <button class="modal-btn cancel" on:click={() => (itemToDelete = null)}>
            {$_("cancel", {}, "Cancel")}
          </button>
          <button class="modal-btn delete" on:click={confirmDelete}>
            {$_("delete", {}, "Delete")}
          </button>
        </div>
      </div>
    </div>
  {/if}

  {#if showStorageSourceChooser}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="source-chooser-backdrop" on:click|self={() => (showStorageSourceChooser = false)}>
      <div class="source-chooser">
        <div class="source-chooser-header">
          <h3>{$_("storage_source_choose", {}, "Where to save files?")}</h3>
          <p>{$_("storage_source_hint", {}, "Select a device to store generated images, memory, and chat artifacts. The node will send files directly via P2P.")}</p>
        </div>
        <div class="source-list">
          {#each storageSources as src (src.linkId || src.deviceId || (src.local ? "local" : src.name))}
            <button type="button" class="source-item" on:click={() => selectStorageSource(src)}>
              <span class="source-icon">
                {#if src.local === true}
                  <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="4" width="20" height="14" rx="2"/><path d="M2 9h20"/></svg>
                {:else}
                  <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="8" rx="2"/><rect x="2" y="13" width="20" height="8" rx="2"/><path d="M6 7h.01M6 17h.01"/></svg>
                {/if}
              </span>
              <span class="source-name">{src.name || (src.local ? $_("this_device", {}, "This device") : src.linkId)}</span>
              <span class="source-badge">{src.local === true ? $_("this_device", {}, "This device") : $_("my_device", {}, "My device")}</span>
            </button>
          {/each}
        </div>
        <div class="source-chooser-actions">
          <button type="button" class="source-cancel" on:click={() => (showStorageSourceChooser = false)}>
            {$_("cancel", {}, "Cancel")}
          </button>
        </div>
      </div>
    </div>
  {/if}

  <!-- Destination Folder Picker Modal -->
  {#if showFolderPicker}
    {@const targetDev = storagePickerDevice || resolveDevice() || getLocalDevice()}
    {#if targetDev}
      <FolderPicker
        device={targetDev}
        mode="folder"
        title={$_("select_destination_folder", {}, "Select Destination Folder")}
        on:select={async (e) => {
          if (e.detail && (e.detail.path || e.detail.virtualPath)) {
            let chosen = e.detail.virtualPath || e.detail.path || "";
            let formatted = formatStoragePath(targetDev, chosen);
            if (chatStore && typeof chatStore.updateAssistantSettings === "function") {
              await chatStore.updateAssistantSettings({ defaultStorage: formatted });
            }
            if (!assistantSettingsData) assistantSettingsData = {};
            assistantSettingsData.defaultStorage = formatted;
            defaultStorage = formatted;
            await persistStoragePath(formatted, null, omdKey);
            await loadCreations();
          }
          showFolderPicker = false;
        }}
        on:cancel={() => (showFolderPicker = false)}
      />
    {/if}
  {/if}
</div>

<style>
  .creations-app {
    display: flex;
    flex-direction: column;
    height: 100%;
    width: 100%;
    overflow: hidden;
    position: relative;
    background-color: var(--bg-secondary, #1A121D);
    color: var(--text-primary, #FCF7FA);
  }

  /* 1. Header */
  .creations-header {
    height: calc(56px + env(safe-area-inset-top, 0px));
    min-height: calc(56px + env(safe-area-inset-top, 0px));
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: env(safe-area-inset-top, 0px) 14px 0 14px;
    background-color: var(--bg-secondary, rgba(26, 18, 29, 0.85));
    border-bottom: none;
    z-index: 10;
    box-sizing: border-box;
  }

  .header-left {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .header-title-box {
    display: flex;
    flex-direction: column;
    justify-content: center;
  }

  .header-title-row {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .sparkle-icon {
    font-size: 16px;
  }

  .header-title {
    font-size: 16px;
    font-weight: 700;
    margin: 0;
    line-height: 1.2;
    color: var(--text-primary, #ffffff);
  }

  .count-pill {
    background: rgba(236, 72, 153, 0.2);
    color: #F472B6;
    border: 1px solid rgba(236, 72, 153, 0.3);
    border-radius: 999px;
    font-size: 11px;
    font-weight: 600;
    padding: 1px 7px;
  }

  .header-folder-badge {
    background: transparent;
    border: none;
    padding: 0;
    margin-top: 2px;
    display: inline-flex;
    align-items: center;
    gap: 4px;
    font-size: 11px;
    color: var(--text-muted, #998094);
    cursor: pointer;
    transition: color 0.15s ease;
  }

  .header-folder-badge:hover {
    color: #F472B6;
  }

  .folder-path {
    max-width: 220px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .header-right {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .wx-modes {
    display: flex;
    align-items: center;
  }

  .wx-modes .wx-segmented {
    display: inline-flex;
    background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.05));
    border: 1px solid var(--border-color, var(--wx-border, rgba(255, 255, 255, 0.1)));
    border-radius: 6px;
    padding: 2px;
    gap: 2px;
    height: 36px;
    box-sizing: border-box;
  }

  .wx-modes .wx-segmented button {
    width: 32px;
    height: 30px;
    min-width: 32px;
    padding: 0;
    margin: 0;
    border-radius: 4px;
    border: none;
    outline: none;
    background: transparent;
    color: var(--wx-color-font-alt, #998094);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
    box-sizing: border-box;
  }

  .wx-modes .wx-segmented button:hover {
    background-color: var(--wx-background-hover, rgba(255, 255, 255, 0.08));
    color: var(--wx-color-primary, #F472B6);
  }

  .wx-modes .wx-segmented button.wx-selected {
    background-color: var(--wx-color-primary, #F472B6);
    color: #ffffff;
    box-shadow: none;
  }

  .wx-modes .wx-segmented button .wx-icon {
    font-size: 16px;
    line-height: 1;
    height: 16px;
    color: inherit;
    display: inline-block;
  }

  .header-icon-btn {
    width: 34px;
    height: 34px;
    border-radius: 8px;
    background: transparent;
    border: 1px solid transparent;
    color: var(--text-muted, #998094);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .header-icon-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary, #ffffff);
  }

  .sort-order-btn {
    position: relative;
  }

  .sort-order-btn:hover {
    color: var(--accent-blue, #F472B6);
  }

  .sort-order-btn.is-active {
    color: var(--accent-blue, #F472B6);
  }

  .header-icon-btn.spinning svg {
    animation: spin 1s linear infinite;
  }

  @keyframes spin {
    from { transform: rotate(0deg); }
    to { transform: rotate(360deg); }
  }

  /* 2. Feed & Scroll Container */
  .creations-feed {
    flex: 1;
    overflow-y: auto;
    overflow-x: hidden;
    padding: 24px 16px 140px;
    display: flex;
    flex-direction: column;
    align-items: center;
    scroll-behavior: smooth;
    background-color: var(--bg-primary, #0F0A10);
    border-top-left-radius: 12px;
    border-top-right-radius: 12px;
  }

  .creations-container {
    width: 100%;
    display: flex;
    flex-direction: column;
    gap: 24px;
  }

  .creations-container.roll {
    max-width: 580px;
    margin: 0 auto;
  }

  .creations-container.grid {
    max-width: 1200px;
    margin: 0 auto;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
    gap: 20px;
  }

  /* 3. Cards */
  .creation-card {
    background: var(--bg-secondary, #1A121D);
    border-radius: 16px;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.08));
    overflow: hidden;
    display: flex;
    flex-direction: column;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.25);
    transition: transform 0.2s ease, box-shadow 0.2s ease, opacity 0.2s ease;
  }

  .creation-card:hover {
    box-shadow: 0 8px 30px rgba(0, 0, 0, 0.35);
  }

  .creation-card.deleting {
    opacity: 0.3;
    pointer-events: none;
    transform: scale(0.97);
  }

  .card-header-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 14px;
    font-size: 11.5px;
    color: var(--text-muted, #998094);
    background: rgba(0, 0, 0, 0.15);
    border-bottom: 1px solid var(--border-color, rgba(255, 255, 255, 0.04));
  }

  .card-top-actions {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .card-top-btn {
    background: transparent;
    border: none;
    color: var(--text-muted, #998094);
    width: 26px;
    height: 26px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .card-top-btn:hover {
    color: #ef4444;
    background: rgba(239, 68, 68, 0.15);
  }

  .card-image-wrap {
    position: relative;
    width: 100%;
    min-height: 240px;
    background: #0F0A10;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    overflow: hidden;
  }

  .creation-img {
    width: 100%;
    height: auto;
    max-height: 680px;
    object-fit: contain;
    display: block;
    transition: transform 0.3s ease;
  }

  .card-image-wrap:hover .creation-img {
    transform: scale(1.015);
  }

  .card-img-placeholder {
    width: 100%;
    min-height: 320px;
    display: flex;
    align-items: center;
    justify-content: center;
    position: relative;
    background: rgba(255, 255, 255, 0.03);
  }

  .load-btn {
    background: rgba(236, 72, 153, 0.2);
    color: #F472B6;
    border: 1px solid rgba(236, 72, 153, 0.4);
    padding: 8px 16px;
    border-radius: 20px;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .load-btn:hover {
    background: rgba(236, 72, 153, 0.35);
  }

  .card-footer {
    padding: 10px 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
    background: var(--bg-secondary, #1A121D);
  }

  .card-meta-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }

  .prompt-accordion-toggle {
    background: transparent;
    border: none;
    padding: 2px 0;
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 12.5px;
    font-weight: 600;
    color: var(--text-muted, #998094);
    cursor: pointer;
    transition: color 0.15s ease;
  }

  .prompt-accordion-toggle:hover {
    color: var(--text-primary, #ffffff);
  }

  .chevron-icon {
    transition: transform 0.2s ease;
  }

  .prompt-accordion-toggle.expanded .chevron-icon {
    transform: rotate(180deg);
  }

  .card-toolbar {
    display: flex;
    align-items: center;
    gap: 4px;
  }

  .toolbar-btn {
    background: transparent;
    border: none;
    color: var(--text-muted, #998094);
    width: 30px;
    height: 30px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .toolbar-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary, #ffffff);
  }

  .toolbar-btn.copied {
    color: #10b981;
  }

  .prompt-expanded-content {
    padding: 8px 12px;
    background: rgba(0, 0, 0, 0.25);
    border-radius: 8px;
    font-size: 12.5px;
    line-height: 1.5;
    color: var(--text-primary, #F2E5ED);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.05));
    word-break: break-word;
  }

  /* 4. In-flight Generating Placeholder Card */
  .placeholder-card {
    border: 1px solid rgba(236, 72, 153, 0.4);
    box-shadow: 0 0 25px rgba(236, 72, 153, 0.2);
  }

  .placeholder-img-wrap {
    position: relative;
    width: 100%;
    min-height: 360px;
    background: radial-gradient(circle at center, #1E0B36 0%, #0F0A10 100%);
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: hidden;
  }

  .magic-glow-border {
    position: absolute;
    inset: -2px;
    border-radius: 16px;
    background: linear-gradient(90deg, #EC4899, #8b5cf6, #ec4899, #EC4899);
    background-size: 300% 300%;
    animation: magicGlow 4s linear infinite;
    opacity: 0.6;
    z-index: 1;
    pointer-events: none;
  }

  @keyframes magicGlow {
    0% { background-position: 0% 50%; }
    50% { background-position: 100% 50%; }
    100% { background-position: 0% 50%; }
  }

  .placeholder-inner {
    position: relative;
    z-index: 2;
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    padding: 24px;
    gap: 12px;
  }

  .pulse-loader {
    position: relative;
    width: 64px;
    height: 64px;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .spinner-ring {
    position: absolute;
    inset: 0;
    border-radius: 50%;
    border: 3px solid rgba(236, 72, 153, 0.2);
    border-top-color: #F472B6;
    animation: spin 1.2s cubic-bezier(0.5, 0, 0.5, 1) infinite;
  }

  .spinner-core {
    font-size: 26px;
    animation: pulse 1.5s ease-in-out infinite;
  }

  @keyframes pulse {
    0%, 100% { transform: scale(1); opacity: 1; }
    50% { transform: scale(1.15); opacity: 0.8; }
  }

  .placeholder-status {
    font-size: 14px;
    font-weight: 600;
    color: #F9A8D4;
    letter-spacing: 0.02em;
  }

  .placeholder-subtext {
    font-size: 12px;
    color: #998094;
    max-width: 380px;
    margin: 0;
    line-height: 1.4;
  }

  .cancel-btn {
    position: absolute;
    top: 12px;
    right: 12px;
    z-index: 3;
    width: 32px;
    height: 32px;
    border-radius: 50%;
    background: rgba(0, 0, 0, 0.5);
    backdrop-filter: blur(6px);
    border: 1px solid rgba(255, 255, 255, 0.15);
    color: #ffffff;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .cancel-btn:hover {
    background: #ef4444;
    border-color: #ef4444;
  }

  .card-prompt-line {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 12px;
    color: var(--text-muted, #998094);
  }

  .prompt-preview {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* Storage Warning Banner */
  .storage-warning-banner {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 12px 16px;
    border-radius: 12px;
    background: rgba(245, 158, 11, 0.12);
    border: 1px solid rgba(245, 158, 11, 0.3);
    margin: 8px 12px 14px;
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
  }

  .storage-warning-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    color: #f59e0b;
    flex-shrink: 0;
  }

  .storage-warning-content {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .storage-warning-title {
    font-size: 13.5px;
    font-weight: 600;
    color: #fbbf24;
    line-height: 1.3;
  }

  .storage-warning-desc {
    font-size: 12px;
    color: var(--text-muted, #998094);
    line-height: 1.4;
  }

  .storage-warning-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 7px 14px;
    border-radius: 8px;
    background: rgba(245, 158, 11, 0.2);
    border: 1px solid rgba(245, 158, 11, 0.4);
    color: #fbbf24;
    font-size: 12.5px;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
    transition: all 0.15s ease;
    flex-shrink: 0;
  }

  .storage-warning-btn:hover {
    background: rgba(245, 158, 11, 0.32);
    border-color: #f59e0b;
    color: #ffffff;
    transform: translateY(-1px);
  }

  .source-chooser-backdrop {
    position: fixed;
    top: 0; left: 0; right: 0; bottom: 0;
    background: rgba(15, 10, 16, 0.8);
    display: flex;
    justify-content: center;
    align-items: center;
    padding: 20px;
    z-index: 1100;
    backdrop-filter: blur(4px);
  }

  .source-chooser {
    background: var(--bg-card, #1A121D);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    border-radius: 14px;
    width: min(460px, 100%);
    max-height: 80vh;
    overflow: auto;
    padding: 20px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    box-shadow: 0 10px 30px rgba(0, 0, 0, 0.5);
  }

  .source-chooser-header h3 {
    margin: 0 0 6px;
    font-size: 16px;
    color: var(--text-primary, #ffffff);
  }

  .source-chooser-header p {
    margin: 0;
    font-size: 12px;
    line-height: 1.5;
    color: var(--text-muted, #998094);
  }

  .source-list {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .source-item {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
    padding: 10px 12px;
    border-radius: 10px;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    background: var(--bg-subtle, rgba(255, 255, 255, 0.03));
    color: inherit;
    cursor: pointer;
    text-align: left;
    transition: all 0.15s ease;
  }

  .source-item:hover {
    border-color: var(--accent-blue, #EC4899);
    background: var(--bg-hover, rgba(236, 72, 153, 0.1));
  }

  .source-icon {
    display: inline-flex;
    opacity: 0.8;
  }

  .source-name {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 14px;
  }

  .source-badge {
    font-size: 11px;
    opacity: 0.65;
    white-space: nowrap;
  }

  .source-chooser-actions {
    display: flex;
    gap: 8px;
    justify-content: flex-end;
    flex-wrap: wrap;
  }

  .source-cancel {
    padding: 8px 14px;
    border-radius: 8px;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.15));
    background: transparent;
    color: inherit;
    cursor: pointer;
    font-size: 13px;
  }

  /* 5. Empty State */
  .empty-creations-state {
    margin: auto 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    max-width: 480px;
    padding: 32px 16px;
  }

  .empty-spark-glow {
    width: 80px;
    height: 80px;
    border-radius: 50%;
    background: radial-gradient(circle, rgba(236, 72, 153, 0.25) 0%, rgba(236, 72, 153, 0) 70%);
    display: flex;
    align-items: center;
    justify-content: center;
    color: #F472B6;
    margin-bottom: 16px;
  }

  .empty-title {
    font-size: 20px;
    font-weight: 700;
    margin: 0 0 8px;
    color: var(--text-primary, #ffffff);
  }

  .empty-desc {
    font-size: 13.5px;
    color: var(--text-muted, #998094);
    line-height: 1.5;
    margin: 0 0 24px;
  }

  .folder-prompt-glow {
    background: radial-gradient(circle, rgba(236, 72, 153, 0.3) 0%, rgba(236, 72, 153, 0) 70%);
    color: var(--accent-blue, #F472B6);
  }

  .primary-select-folder-btn {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 10px 22px;
    border-radius: 10px;
    background: var(--accent-blue, #F472B6);
    color: #ffffff;
    font-size: 14px;
    font-weight: 600;
    border: none;
    cursor: pointer;
    transition: all 0.2s ease;
    box-shadow: 0 4px 14px rgba(236, 72, 153, 0.35);
  }

  .primary-select-folder-btn:hover {
    background: #DB2777;
    transform: translateY(-1px);
    box-shadow: 0 6px 18px rgba(236, 72, 153, 0.5);
  }

  .starter-prompts {
    width: 100%;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .starter-label {
    font-size: 11.5px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    font-weight: 600;
    color: var(--text-muted, #7D6376);
  }

  .starter-chips {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .starter-chip {
    background: var(--bg-secondary, #1A121D);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.08));
    border-radius: 10px;
    padding: 8px 12px;
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 12.5px;
    color: var(--text-primary, #E6D3DF);
    text-align: left;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .starter-chip:hover {
    background: rgba(236, 72, 153, 0.12);
    border-color: rgba(236, 72, 153, 0.4);
    color: #F472B6;
    transform: translateY(-1px);
  }

  /* Floating Quick Scroll Controls (matching Chat.svelte) */
  .scroll-fab-dock {
    position: absolute;
    right: 22px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 8px;
    z-index: 35;
    pointer-events: none;
    transition: bottom 0.2s ease;
  }

  .scroll-fab-btn {
    width: 38px;
    height: 38px;
    border-radius: 50%;
    background: var(--bg-card, #1A121D);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.15));
    color: var(--text-secondary, #E6D3DF);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    pointer-events: auto;
    box-shadow: 0 4px 14px rgba(0, 0, 0, 0.25);
    transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
    backdrop-filter: blur(8px);
    -webkit-backdrop-filter: blur(8px);
  }

  .scroll-fab-btn:hover {
    background: var(--bg-card-hover, rgba(255, 255, 255, 0.1));
    color: var(--accent-blue, #EC4899);
    transform: translateY(-2px);
    box-shadow: 0 6px 18px rgba(0, 0, 0, 0.35);
  }

  .scroll-fab-btn:active {
    transform: translateY(0);
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2);
  }

  /* 6. Footer & Composer Pill (matching Chat.svelte) */
  .creations-footer {
    position: absolute;
    bottom: 0;
    left: 0;
    right: 0;
    padding: 12px 16px 16px;
    background: linear-gradient(180deg, rgba(15, 10, 16, 0) 0%, var(--bg-primary, #0F0A10) 25%);
    display: flex;
    flex-direction: column;
    align-items: center;
    z-index: 20;
    pointer-events: none;
  }

  .composer-container {
    width: 100%;
    max-width: 820px;
    display: flex;
    flex-direction: column;
    align-items: center;
    position: relative;
    pointer-events: auto;
  }

  .composer-pill {
    width: 100%;
    display: flex;
    align-items: center;
    gap: 8px;
    background: var(--pill-bg, rgba(255, 255, 255, 0.05));
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    border-radius: 24px;
    padding: 6px 10px;
    box-shadow: 0 4px 24px rgba(0, 0, 0, 0.28);
    backdrop-filter: blur(16px);
    transition: border-color 0.2s, box-shadow 0.2s;
    box-sizing: border-box;
    position: relative;
    min-height: 48px;
  }

  .composer-pill:focus-within {
    border-color: var(--accent-blue, #EC4899);
    box-shadow: 0 4px 28px rgba(236, 72, 153, 0.2);
  }

  .composer-action-wrap {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    align-self: center;
  }

  .composer-action-btn {
    background: transparent;
    border: none;
    color: var(--text-secondary, #998094);
    width: 36px;
    height: 36px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: background 0.2s, color 0.2s;
    flex-shrink: 0;
  }

  .composer-action-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary, #ffffff);
  }

  .composer-action-btn.active {
    color: var(--accent-blue, #EC4899);
    background: rgba(236, 72, 153, 0.15);
  }

  .plus-icon {
    transition: transform 0.2s ease;
  }

  .plus-icon.rotated {
    transform: rotate(45deg);
  }

  .composer-textarea {
    flex: 1;
    background: transparent;
    border: none;
    outline: none;
    color: var(--text-primary, #ffffff);
    font-size: 0.95rem;
    line-height: 24px;
    resize: none;
    padding: 6px 8px;
    font-family: inherit;
    box-sizing: border-box;
    height: 36px;
    min-height: 36px;
    max-height: 180px;
    overflow-y: hidden;
    align-self: center;
  }

  .composer-textarea::placeholder {
    color: var(--text-muted, #7D6376);
  }

  .composer-textarea::-webkit-scrollbar {
    width: 4px;
  }

  .composer-textarea::-webkit-scrollbar-thumb {
    background: rgba(255, 255, 255, 0.2);
    border-radius: 4px;
  }

  .composer-textarea::-webkit-scrollbar-track {
    background: transparent;
  }

  .composer-send-btn,
  .composer-stop-btn {
    width: 36px;
    height: 36px;
    border-radius: 50%;
    border: none;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    flex-shrink: 0;
    transition: all 0.2s ease;
    align-self: center;
  }

  .composer-send-btn {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-muted, #7D6376);
  }

  .composer-send-btn.active {
    background: var(--accent-blue, #EC4899);
    color: #ffffff;
    box-shadow: 0 2px 10px rgba(236, 72, 153, 0.4);
  }

  .composer-send-btn:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }

  .composer-stop-btn {
    background: #ef4444;
    color: #ffffff;
  }

  /* 7. Image Generator Toolpanel (matching Chat.svelte) */
  .image-generator-panel {
    width: 100%;
    margin-bottom: 8px;
    background: var(--pill-bg, rgba(255, 255, 255, 0.05));
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    border-radius: 20px;
    padding: 10px 14px;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.2);
    backdrop-filter: blur(16px);
    display: flex;
    flex-direction: column;
    gap: 10px;
    animation: slideUpFade 0.2s cubic-bezier(0.16, 1, 0.3, 1);
    box-sizing: border-box;
  }

  @keyframes slideUpFade {
    from {
      opacity: 0;
      transform: translateY(8px);
    }
    to {
      opacity: 1;
      transform: translateY(0);
    }
  }

  .gen-panel-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    flex-wrap: wrap;
  }

  .gen-styles-group {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }

  .gen-section-label {
    font-size: 0.76rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--text-muted, #998094);
  }

  .gen-styles-pills {
    display: flex;
    align-items: center;
    gap: 4px;
    background: rgba(255, 255, 255, 0.04);
    padding: 3px;
    border-radius: 12px;
    border: 1px solid var(--border-subtle, rgba(255, 255, 255, 0.06));
  }

  .gen-style-pill {
    background: transparent;
    border: none;
    color: var(--text-secondary, #E6D3DF);
    font-size: 0.78rem;
    font-weight: 500;
    padding: 4px 10px;
    border-radius: 9px;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .gen-style-pill:hover {
    color: var(--text-primary, #ffffff);
    background: rgba(255, 255, 255, 0.08);
  }

  .gen-style-pill.active,
  .gen-style-pill.active:hover,
  .gen-style-pill.active:focus {
    background: var(--accent-blue, #EC4899);
    color: #ffffff !important;
    box-shadow: 0 2px 8px rgba(236, 72, 153, 0.35);
  }

  .gen-panel-controls {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-left: auto;
  }

  .gen-toggle-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    cursor: pointer;
    padding: 4px 8px;
    border-radius: 10px;
    border: 1px solid transparent;
    transition: all 0.15s ease;
    user-select: none;
  }

  .gen-toggle-btn:hover {
    background: rgba(255, 255, 255, 0.05);
  }

  .gen-toggle-btn.active {
    background: rgba(236, 72, 153, 0.12);
    border-color: rgba(236, 72, 153, 0.3);
  }

  .gen-checkbox-hidden {
    position: absolute;
    opacity: 0;
    pointer-events: none;
    margin: 0;
  }

  .gen-toggle-check {
    width: 14px;
    height: 14px;
    border-radius: 4px;
    border: 1.5px solid var(--border-color, rgba(255, 255, 255, 0.15));
    display: inline-flex;
    align-items: center;
    justify-content: center;
    transition: all 0.15s ease;
  }

  .gen-toggle-btn.active .gen-toggle-check {
    background: var(--accent-blue, #EC4899);
    border-color: var(--accent-blue, #EC4899);
    color: #ffffff;
  }

  .gen-toggle-text {
    font-size: 0.78rem;
    color: var(--text-secondary, #E6D3DF);
    font-weight: 500;
  }

  .gen-toggle-btn.active .gen-toggle-text {
    color: var(--text-primary, #ffffff);
  }

  .gen-icon-btn {
    background: transparent;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.15));
    color: var(--text-secondary, #E6D3DF);
    width: 28px;
    height: 28px;
    border-radius: 8px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .gen-icon-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary, #ffffff);
    border-color: var(--text-muted, #998094);
  }

  .gen-icon-btn.active {
    background: rgba(236, 72, 153, 0.15);
    color: var(--accent-blue, #EC4899);
    border-color: var(--accent-blue, #EC4899);
  }

  .gen-close-btn:hover {
    color: #ef4444;
    border-color: #ef4444;
  }

  .gen-panel-loras-row {
    padding-top: 8px;
    border-top: 1px solid var(--border-subtle, rgba(255, 255, 255, 0.06));
    display: flex;
    align-items: center;
    gap: 12px;
    flex-wrap: wrap;
  }

  .gen-loras-group {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }

  .gen-lora-chips {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-wrap: wrap;
  }

  .gen-lora-chip {
    box-sizing: border-box;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.15));
    color: var(--text-secondary, #E6D3DF);
    font-size: 0.74rem;
    font-weight: 500;
    padding: 3px 9px;
    border-radius: 12px;
    cursor: pointer;
    outline: none;
    transition: background 0.15s ease, border-color 0.15s ease, color 0.15s ease;
  }

  .gen-lora-chip:hover {
    color: var(--text-primary, #ffffff);
    background: rgba(255, 255, 255, 0.1);
    border-color: var(--text-muted, #998094);
  }

  .gen-lora-chip.active {
    background: rgba(236, 72, 153, 0.18);
    border-color: var(--accent-blue, #EC4899);
    color: var(--accent-blue, #EC4899);
    font-weight: 500;
    box-shadow: 0 0 0 1px rgba(236, 72, 153, 0.3) inset;
  }

  .gen-divider {
    width: 1px;
    height: 18px;
    background: var(--border-subtle, rgba(255, 255, 255, 0.06));
  }

  /* 8. Prompts History Popup (matching Chat.svelte) */
  .prompts-popup {
    position: absolute;
    bottom: calc(100% + 10px);
    left: 0;
    right: 0;
    max-width: 820px;
    margin: 0 auto;
    background: var(--bg-card, #1A121D);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.15));
    border-radius: 18px;
    box-shadow: 0 16px 40px rgba(0, 0, 0, 0.4);
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
    border-bottom: 1px solid var(--border-subtle, rgba(255, 255, 255, 0.06));
  }

  .prompts-header-title {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.82rem;
    font-weight: 600;
    color: var(--text-primary, #ffffff);
  }

  .prompts-popup-close {
    background: transparent;
    border: none;
    color: var(--text-muted, #998094);
    cursor: pointer;
    padding: 4px;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 6px;
    transition: color 0.15s ease;
  }

  .prompts-popup-close:hover {
    color: var(--text-primary, #ffffff);
  }

  .prompts-search-wrap {
    padding: 8px 14px;
    border-bottom: 1px solid var(--border-subtle, rgba(255, 255, 255, 0.06));
  }

  .prompts-search-input {
    width: 100%;
    background: rgba(255, 255, 255, 0.04);
    border: 1px solid var(--border-subtle, rgba(255, 255, 255, 0.06));
    border-radius: 8px;
    padding: 6px 10px;
    font-size: 0.8rem;
    color: var(--text-primary, #ffffff);
    outline: none;
    box-sizing: border-box;
  }

  .prompts-search-input:focus {
    border-color: var(--accent-blue, #EC4899);
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
    background: var(--bg-card-hover, rgba(255, 255, 255, 0.06));
  }

  .prompt-item-content {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .prompt-item-text {
    font-size: 0.84rem;
    color: var(--text-primary, #ffffff);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .prompt-item-meta {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.72rem;
    color: var(--text-muted, #998094);
  }

  .prompt-style-badge {
    background: rgba(236, 72, 153, 0.12);
    color: var(--accent-blue, #EC4899);
    padding: 1px 6px;
    border-radius: 4px;
    font-weight: 500;
    text-transform: capitalize;
  }

  .prompt-time {
    font-size: 0.72rem;
    color: var(--text-muted, #998094);
  }

  .prompt-delete-btn {
    background: transparent;
    border: none;
    color: var(--text-muted, #998094);
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
    background: rgba(239, 68, 68, 0.12);
  }

  .prompts-empty-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 32px 16px;
    color: var(--text-muted, #998094);
    font-size: 0.82rem;
  }

  /* 8. Delete Confirmation Modal */
  .confirm-modal-overlay {
    position: absolute;
    inset: 0;
    background: rgba(0, 0, 0, 0.6);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100;
    padding: 16px;
  }

  .confirm-modal-box {
    background: var(--bg-secondary, #1A121D);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    border-radius: 16px;
    padding: 24px;
    max-width: 360px;
    width: 100%;
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    box-shadow: 0 20px 50px rgba(0, 0, 0, 0.5);
  }

  .modal-icon-wrap {
    font-size: 32px;
    margin-bottom: 12px;
  }

  .modal-title {
    font-size: 16px;
    font-weight: 700;
    margin: 0 0 6px;
    color: var(--text-primary, #ffffff);
  }

  .modal-desc {
    font-size: 13px;
    color: var(--text-muted, #998094);
    line-height: 1.4;
    margin: 0 0 20px;
  }

  .modal-actions {
    display: flex;
    width: 100%;
    gap: 10px;
  }

  .modal-btn {
    flex: 1;
    padding: 10px;
    border-radius: 10px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    border: none;
    transition: all 0.15s ease;
  }

  .modal-btn.cancel {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary, #E6D3DF);
  }

  .modal-btn.cancel:hover {
    background: rgba(255, 255, 255, 0.14);
  }

  .modal-btn.delete {
    background: #ef4444;
    color: #ffffff;
  }

  .modal-btn.delete:hover {
    background: #dc2626;
  }

  /* Skeleton Loading */
  .skeleton-feed {
    width: 100%;
    max-width: 580px;
    display: flex;
    flex-direction: column;
    gap: 24px;
  }

  .creation-card-skeleton {
    background: var(--bg-secondary, #1A121D);
    border-radius: 16px;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.05));
    overflow: hidden;
  }

  .skeleton-img {
    width: 100%;
    height: 320px;
    background: rgba(255, 255, 255, 0.04);
    position: relative;
    overflow: hidden;
  }

  .skeleton-shimmer,
  .skeleton-img::after {
    content: "";
    position: absolute;
    inset: 0;
    background: linear-gradient(90deg, transparent, rgba(255, 255, 255, 0.05), transparent);
    animation: shimmer 1.5s infinite;
  }

  @keyframes shimmer {
    0% { transform: translateX(-100%); }
    100% { transform: translateX(100%); }
  }

  .skeleton-meta {
    padding: 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .skeleton-line {
    height: 12px;
    border-radius: 4px;
    background: rgba(255, 255, 255, 0.05);
  }

  .w-40 { width: 40%; }
  .w-80 { width: 80%; }

  /* Light Theme Overrides */
  .creations-app.light-theme {
    background-color: #FAF9FB;
    color: #1D1823;
  }

  .creations-app.light-theme .creations-header {
    background-color: #FFFFFF;
    border-bottom: none;
  }

  .creations-app.light-theme .creations-feed {
    background-color: #FAF9FB;
  }

  .creations-app.light-theme .header-title {
    color: #1D1823;
  }

  .creations-app.light-theme .creation-card {
    background: #ffffff;
    border-color: #E6E2ED;
    box-shadow: 0 2px 12px rgba(24, 18, 32, 0.05);
  }

  .creations-app.light-theme .card-header-bar {
    background: #FAFAFC;
    border-bottom-color: #F0EDF5;
  }

  .creations-app.light-theme .card-footer {
    background: #ffffff;
  }

  .creations-app.light-theme .prompt-expanded-content {
    background: #F4F3F7;
    color: #1D1823;
    border-color: #E6E2ED;
  }

  .creations-app.light-theme .creations-footer {
    background: linear-gradient(180deg, rgba(250, 249, 251, 0) 0%, #FAF9FB 25%);
  }

  .creations-app.light-theme .composer-pill {
    background: #ffffff;
    border-color: rgba(0, 0, 0, 0.12);
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.08);
  }

  .creations-app.light-theme .scroll-fab-btn {
    background: #ffffff;
    border-color: rgba(0, 0, 0, 0.12);
    box-shadow: 0 4px 14px rgba(0, 0, 0, 0.08);
  }

  .creations-app.light-theme .composer-textarea {
    color: #0F0A10;
  }

  .creations-app.light-theme .image-generator-panel {
    background: #ffffff;
    border-color: rgba(0, 0, 0, 0.1);
  }

  .creations-app.light-theme .gen-styles-pills {
    background: #F3F1F6;
    border-color: #E6E2ED;
  }

  .creations-app.light-theme .gen-style-pill {
    color: #5E5468;
  }

  .creations-app.light-theme .gen-style-pill:hover {
    background: rgba(0, 0, 0, 0.04);
    color: #1D1823;
  }

  .creations-app.light-theme .gen-style-pill.active,
  .creations-app.light-theme .gen-style-pill.active:hover,
  .creations-app.light-theme .gen-style-pill.active:focus,
  .creations-app.light-theme .gen-style-pill.active:focus-visible {
    background: var(--accent-blue, #DB2777);
    color: #ffffff !important;
    box-shadow: 0 2px 8px rgba(219, 39, 119, 0.30);
  }

  .creations-app.light-theme .gen-lora-chip {
    background: #FAF9FB;
    border-color: #E6E2ED;
    color: #5E5468;
  }

  .creations-app.light-theme .gen-lora-chip:hover {
    background: #F3F1F6;
    color: #1D1823;
  }

  .creations-app.light-theme .gen-lora-chip.active,
  .creations-app.light-theme .gen-lora-chip.active:hover,
  .creations-app.light-theme .gen-lora-chip.active:focus {
    background: rgba(219, 39, 119, 0.09);
    border-color: #DB2777;
    color: #BE185D !important;
    font-weight: 600;
  }

  .creations-app.light-theme .prompts-popup {
    background: #ffffff;
    border-color: rgba(0, 0, 0, 0.12);
  }

  .creations-app.light-theme .wx-modes .wx-segmented {
    background-color: #F3F1F6;
    border-color: #E6E2ED;
  }

  .creations-app.light-theme .wx-modes .wx-segmented button {
    color: #664D5F;
  }

  .creations-app.light-theme .wx-modes .wx-segmented button:hover {
    background-color: rgba(0, 0, 0, 0.05);
    color: #9D174D;
  }

  .creations-app.light-theme .wx-modes .wx-segmented button.wx-selected {
    background-color: var(--wx-color-primary, #9D174D);
    color: #ffffff;
  }

  @media (max-width: 768px) {
    .header-folder-badge {
      display: none;
    }
    .wx-modes {
      display: none;
    }
    .scroll-fab-dock {
      right: 14px;
      gap: 6px;
    }
    .scroll-fab-btn {
      width: 36px;
      height: 36px;
    }
    .creations-feed {
      padding: 16px 12px 140px;
    }
    .creations-footer {
      padding: 8px 10px 12px;
    }
  }

  /* Notice banner above composer */
  .creations-notice-banner {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 10px 14px;
    margin-bottom: 10px;
    border-radius: 12px;
    background: linear-gradient(135deg, rgba(168, 85, 247, 0.12), rgba(168, 85, 247, 0.14));
    border: 1px solid rgba(168, 85, 247, 0.35);
    backdrop-filter: blur(8px);
    animation: bannerFadeIn 0.2s ease-out;
  }
  @keyframes bannerFadeIn {
    from { opacity: 0; transform: translateY(4px); }
    to { opacity: 1; transform: translateY(0); }
  }
  .notice-banner-icon {
    font-size: 1.25rem;
    flex-shrink: 0;
  }
  .notice-banner-content {
    flex: 1;
    display: flex;
    align-items: center;
    gap: 10px;
    flex-wrap: wrap;
    font-size: 0.88rem;
    line-height: 1.4;
  }
  :global(.dark-theme) .notice-banner-text {
    color: #F7ECF2;
  }
  :global(.light-theme) .notice-banner-text {
    color: #1A121D;
  }
  .notice-banner-btn {
    display: inline-flex;
    align-items: center;
    padding: 4px 12px;
    font-size: 0.82rem;
    font-weight: 600;
    border-radius: 8px;
    background: linear-gradient(135deg, #A855F7, #a855f7);
    color: #fff !important;
    text-decoration: none !important;
    transition: opacity 0.15s, transform 0.15s;
    white-space: nowrap;
  }
  .notice-banner-btn:hover {
    opacity: 0.92;
    transform: translateY(-1px);
  }
  .notice-banner-close {
    background: none;
    border: none;
    font-size: 1.25rem;
    line-height: 1;
    cursor: pointer;
    opacity: 0.6;
    padding: 0 4px;
    color: inherit;
  }
  .notice-banner-close:hover {
    opacity: 1;
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
  .mode-toggle-btn.is-checking {
    opacity: 0.75;
    cursor: wait;
  }

  .mode-text-short {
    display: none;
  }

  @media (max-width: 640px) {
    .creations-header {
      padding: 0 8px;
      height: 48px;
      min-height: 48px;
      gap: 6px;
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
      min-width: 20px;
      height: 20px;
      padding: 0 6px;
      font-size: 0.72rem;
    }

    .header-right {
      gap: 4px;
      flex-shrink: 0;
    }

    .header-icon-btn {
      width: 34px;
      height: 34px;
      padding: 4px;
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

  @media (max-width: 360px) {
    .header-count-pill {
      display: none;
    }
  }
</style>
