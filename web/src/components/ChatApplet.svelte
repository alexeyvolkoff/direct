<script>
  import { createEventDispatcher, onMount, onDestroy } from "svelte";
  import { _ } from "../lib/i18n/index.js";
  import { HttpDrive } from "../lib/http-drive.js";
  import { getStoredOmdKey, omdKeyStore } from "../lib/omd-key.js";
  import { getChatStore, getChatIcon } from "../lib/chat-store.js";
  import { profileStore } from "../lib/profile-store.js";
  import Chat from "./Chat.svelte";
  import KreaStudio from "./KreaStudio.svelte";
  import MemoryCards from "./MemoryCards.svelte";
  import MemoryCardModal from "./MemoryCardModal.svelte";
  import FolderPicker from "./FolderPicker.svelte";
  import {
    issueConsent, getClientDeviceServer,
  } from "../lib/client-device-server.js";
  import { deviceRegistry } from "../lib/device-registry.js";
  import { isFileSystemAccessAvailable, addShareFromPicker, addOpfsShare } from "../lib/local-client.js";
  import { parseStoragePath } from "../lib/storage-path.js";
  import { aiNodeStore, aiOnmychatHeaders } from "../lib/ai-node.js";
  import { gatewayConfig } from "../lib/gateway-config.js";
  import { resolveAppMode, APP_MODE_CHAT } from "../lib/app-mode.js";

  export let targetLinkId = "";
  export let targetDevice = null;
  export let port = 4000;
  export let isDarkTheme = true;
  export let isPrivateMode = undefined;
  // linkId устройств, которые принадлежат пользователю (из его identity плюс
  // поделившиеся с ним). Нужен чату, чтобы /learn и /recognize предлагали выбор
  // ЛЮБОГО своего устройства: флаг isOwner выставляется лишь при admin-паре,
  // поэтому по нему одному список получался неполным.
  export let ownLinkIds = [];
  export let hasOwnHardwareNode = false;
  export let activeView = "chat"; // 'chat' | 'creations' | 'memory'
  // When the chat is the root app (aiassistant.direct), it has no close button and
  // exposes a "Files" entry in the sidebar instead.
  export let isMainApp = false;
  export let showFilesEntry = true;

  const dispatch = createEventDispatcher();

  let omdKey = getStoredOmdKey();
  let storeError = "";
  let chatStore = null;
  let chats = [];
  export let activeChatId = "";
  export let isChatLoading = false;
  let userSelectedChatManually = false;
  let creationsCount = 0;
  let drive = null;
  let chatSidebarOpen = true;
  let sidebarOpen = activeView === "chat";
  let prevActiveView = activeView;
  $: isOverlaySidebar = activeView !== "chat";
  $: if (activeView !== "chat" && isChatLoading) {
    isChatLoading = false;
  }

  $: if (activeView !== prevActiveView) {
    prevActiveView = activeView;
    if (activeView !== "chat") {
      sidebarOpen = false;
    } else {
      sidebarOpen = (typeof window !== "undefined" && window.innerWidth < 768)
        ? false
        : chatSidebarOpen;
    }
  }

  function setSidebarOpen(val) {
    sidebarOpen = val;
    if (activeView === "chat" && (typeof window === "undefined" || window.innerWidth >= 768)) {
      chatSidebarOpen = val;
    }
  }

  function toggleSidebar() {
    setSidebarOpen(!sidebarOpen);
  }

  let mainContentWidth = 0;
  $: isOverlayHeader = mainContentWidth >= 960;
  let chatComponent = null;
  let attachedCards = [];
  let attachMenuOpen = false;
  let knowledgeList = [];
  let deviceCards = [];
  let showMemoryModal = false;
  let selectedMemoryCard = null;
  let selectedKindIntent = "note";
  // Реиндекс папки: выбор устройства-источника + пикер + прогресс
  let showIndexFolderPicker = false;
  let indexSourceDevice = null;
  let showIndexSourceChooser = false;
  let indexSources = [];
  let indexSourceError = "";
  let indexFolderStatusText = "";
  let indexFolderCounts = null;
  let indexFolderAbort = false;
  let renamingChatId = null;  let renamingTitle = "";
  let chatMenuOpenId = null;

  let contentMode = typeof localStorage !== "undefined"
    ? localStorage.getItem("onmychat_mode") || "work"
    : "work";

  function toggleMode() {
    contentMode = contentMode === "work" ? "fun" : "work";
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("onmychat_mode", contentMode);
    }
  }


  $: hasKey = !!omdKey;

  function newChatId() {
    return "chat_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8);
  }

  // Приватный режим = ЯВНО выданные права владельца (isOwner === true).
  // Дефолт должен быть безопасным: устройства без isOwner (в т.ч. подключённая
  // публичная AI-нода, registerDevicePlaceholder) — гостевые, их знания живут
  // в личном GunDB, а не в ChromaDB ноды. Раньше isOwner === undefined
  // считался приватным и HttpDrive отправлял X-OMD-Private-Mode: 1 — так
  // внешний пользователь получал доступ к приватным данным владельца ноды.
  $: computedPrivateMode = isPrivateMode !== undefined
    ? isPrivateMode
    : (targetDevice?.isOwner === true);

  $: {
    const currentDidPub = (typeof window !== "undefined" && (window.identity?.mainPub || window.identity?.pair?.pub)) || "";
    if (!drive || drive.targetLinkId !== targetLinkId || drive.port !== port || drive.isPrivateMode !== computedPrivateMode) {
      if (drive) drive.close();
      drive = new HttpDrive({
        targetLinkId,
        port,
        isPrivateMode: computedPrivateMode,
        didPub: currentDidPub,
        authToken: omdKey || ""
      });
    } else if (omdKey && drive.authToken !== omdKey) {
      drive.setAuth(omdKey);
    }
  }

  // Agentic inference routing: headers go on every onmychat request.
  $: if (drive) {
    const aiHeaders = aiOnmychatHeaders($aiNodeStore);
    drive.setAiHeaders(aiHeaders && Object.keys(aiHeaders).length ? aiHeaders : null);
  }

  let subscribedStore = null;
  let unsubscribeChats = null;
  let unsubscribeKnowledge = null;

  // Image generation support (assistant capabilities)
  let imageGenSupported = false;
  let kreaSupported = false;
  let kreaActive = typeof localStorage !== "undefined" ? localStorage.getItem("omd_krea_active") === "1" : false;

  function setKreaActive(val) {
    kreaActive = !!val;
    try {
      localStorage.setItem("omd_krea_active", kreaActive ? "1" : "0");
    } catch (_) {}
  }
  let capabilitiesLoaded = false;
  let lastCapabilitiesDrive = null;

  async function loadAssistantCapabilities() {
    if (!drive) return;
    try {
      const res = await drive.fetch("/assistant");
      if (res.ok) {
        const info = await res.json();
        imageGenSupported = !!(info?.capabilities?.image_generation);
        kreaSupported = !!(info?.capabilities?.krea2);
        capabilitiesLoaded = true;
      }
    } catch (_) {}
  }

  $: if (drive !== lastCapabilitiesDrive) {
    lastCapabilitiesDrive = drive;
    capabilitiesLoaded = false;
  }

  $: if (!capabilitiesLoaded && drive) {
    loadAssistantCapabilities();
  }

  // Объединяем GunDB-карточки (личные) с on-device карточками ChromaDB
  $: allKnowledgeCards = [...deviceCards, ...knowledgeList];

  // Защита от «шторма»: реактивных вызовов может быть несколько подряд, а ответ
  // /api/device_memory — мегабайты. Грузим порциями (DEVICE_PAGE_SIZE): первая
  // страница отрисовывается сразу, остальные — последовательно в фоне.
  // AbortController: долгие тянущиеся страницы убиваются при уходе с раздела,
  // refresh или новом заходе, иначе сообщение «Loading knowledge» висит вечно.
  const DEVICE_PAGE_SIZE = 300;
  const DEVICE_FETCH_TIMEOUT_MS = 30000;
  let deviceMemoryLoading = false;   // идёт самый первый (страничный) запрос
  let deviceMemoryLoaded = false;
  let deviceMemoryError = false;
  let deviceMemoryMore = false; // на сервере остались недогруженные страницы
  let deviceMemoryTotal = 0;
  let deviceMemoryAbortCtl = null; // AbortController текущего запроса
  let deviceMemoryRestFetching = false; // идёт фоновая догрузка следующих страниц

  function _abortDeviceMemoryFetch() {
    if (deviceMemoryAbortCtl) {
      try { deviceMemoryAbortCtl.abort(); } catch { /* уже убит */ }
      deviceMemoryAbortCtl = null;
    }
  }

  async function fetchDeviceMemoryPage(offset = 0) {
    // Таймаут на весь ответ (заголовки + тело): раньше тело стримилось без лимита
    const ctrl = new AbortController();
    deviceMemoryAbortCtl = ctrl;
    const timer = setTimeout(() => ctrl.abort(), DEVICE_FETCH_TIMEOUT_MS);
    try {
      const res = await drive.fetch(`/api/device_memory?offset=${offset}&limit=${DEVICE_PAGE_SIZE}`, {
        headers: { Accept: "application/json" },
        signal: ctrl.signal,
      });
      if (!res.ok) return null;
      return await res.json();
    } finally {
      clearTimeout(timer);
      if (deviceMemoryAbortCtl === ctrl) deviceMemoryAbortCtl = null;
    }
  }

  async function _loadRestDevicePages() {
    if (deviceMemoryRestFetching || !deviceMemoryMore || !drive) return;
    deviceMemoryRestFetching = true;
    try {
      while (deviceMemoryMore) {
        const data = await fetchDeviceMemoryPage(deviceCards.length);
        if (!data || !Array.isArray(data.memories)) {
          deviceMemoryMore = false;
          break;
        }
        deviceCards = deviceCards.concat(data.memories);
        deviceMemoryMore = !!data.hasMore && !!data.memories.length;
        deviceMemoryTotal = data.total || deviceMemoryTotal;
      }
    } catch (e) {
      console.warn("[chat-applet] loadDeviceMemory rest error:", e?.name === "AbortError" ? "aborted" : e);
    } finally {
      deviceMemoryRestFetching = false;
    }
  }

  async function loadDeviceMemory(force = false) {
    if (!drive) {
      deviceCards = [];
      return;
    }
    if (deviceMemoryLoading) return;
    if (deviceMemoryLoaded && !force) return;
    _abortDeviceMemoryFetch();
    deviceMemoryLoading = true;
    deviceMemoryError = false;
    try {
      const data = await fetchDeviceMemoryPage(0);
      if (!data || !Array.isArray(data.memories)) {
        deviceCards = [];
        deviceMemoryError = true;
        return;
      }
      deviceCards = data.memories;
      deviceMemoryTotal = data.total ?? data.memories.length;
      deviceMemoryMore = !!data.hasMore;
      deviceMemoryLoaded = true;
      // Остальные страницы — фоновая догрузка, не блокируя UI
      if (deviceMemoryMore) _loadRestDevicePages();
    } catch (e) {
      console.warn("[chat-applet] loadDeviceMemory error:", e);
      deviceCards = deviceCards.length ? deviceCards : [];
      deviceMemoryError = true;
      deviceMemoryLoaded = false;
    } finally {
      deviceMemoryLoading = false;
    }
  }

  // Ленивое превью карточки on-device: /api/device_memory не тащит base64,
  // миниатюру даёт /api/device_memory_thumb?path=<document_id>.
  const thumbCache = new Map();
  async function fetchDeviceThumb(documentId, size = 480) {
    if (!documentId) return "";
    let cleanDoc = String(documentId);
    if (cleanDoc.startsWith("http://") || cleanDoc.startsWith("https://")) {
      try {
        const u = new URL(cleanDoc);
        cleanDoc = decodeURIComponent(u.pathname);
      } catch (_) {
        cleanDoc = cleanDoc.replace(/^https?:\/\/[^\/]+/, "");
      }
    }
    cleanDoc = cleanDoc.startsWith("/") ? cleanDoc : "/" + cleanDoc;
    const cacheKey = `${cleanDoc}_${size}`;
    if (thumbCache.has(cacheKey)) return thumbCache.get(cacheKey);
    try {
      let res = null;
      // 1. Пробуем прямой p2p-путь через Service Worker / WebRTC с ресайзом
      const parts = cleanDoc.split("/").filter(Boolean);
      if (parts.length > 1) {
        const d = (typeof deviceRegistry !== "undefined") ? (deviceRegistry.getByPrefix(parts[0]) || deviceRegistry.get(parts[0]) || deviceRegistry.getByName(parts[0])) : null;
        const seg = (d && ((d.prefix ? d.prefix.replace(/^\/+/, "") : "") || d.linkId || d.name)) || parts[0];
        const p2pUrl = `/p2p/${seg}${encodeURI("/" + parts.slice(1).join("/"))}?resize=true&width=${size}&height=${size}`;
        try {
          res = await fetch(p2pUrl);
        } catch (_) {}
      }
      if (!res || !res.ok) {
        if (drive && typeof drive.fetch === "function") {
          try {
            res = await drive.fetch(`/api/device_memory_thumb?path=${encodeURIComponent(cleanDoc)}&size=${size}`);
          } catch (_) {}
        }
      }
      if (!res || !res.ok) {
        const keyParam = omdKey || getStoredOmdKey();
        const url = `/api/device_memory_thumb?path=${encodeURIComponent(cleanDoc)}&size=${size}` + (keyParam ? `&omd_key=${encodeURIComponent(keyParam)}` : "");
        res = await fetch(url);
      }
      if (!res || !res.ok) {
        thumbCache.set(cacheKey, "");
        return "";
      }
      const blob = await res.blob();
      const url = URL.createObjectURL(blob);
      thumbCache.set(cacheKey, url);
      return url;
    } catch (e) {
      thumbCache.set(cacheKey, "");
      return "";
    }
  }

  let lastMemoryDrive = null;
  $: if (drive !== lastMemoryDrive) {
    lastMemoryDrive = drive;
    deviceMemoryLoaded = false;
    _abortDeviceMemoryFetch();
  }

  // Грузим знания девайса когда пользователь открыл раздел Memory Cards:
  // Для владельца приходят все знания ноды, для гостей — только публичные (#omd)
  $: if (drive && activeView === "memory") {
    loadDeviceMemory();
  }

  // Ушли из раздела пораньше — убиваем недогруженные страницы,
  // чтобы фоновая догрузка не забивала P2P-туннель во время чата.
  $: if (activeView && activeView !== "memory") {
    _abortDeviceMemoryFetch();
  }

  function initSession(key) {
    try {
      chatStore = getChatStore(key);

      if (subscribedStore !== chatStore) {
        subscribedStore = chatStore;
        if (unsubscribeChats) unsubscribeChats();
        if (unsubscribeKnowledge) unsubscribeKnowledge();

        unsubscribeChats = chatStore.onChatsChanged((list) => {
          chats = list;
          const unarchived = list.filter((c) => !c.archived && c.updated !== "1970-01-01T00:00:00Z");
          const newest = unarchived[0] || list[0];

          if (!userSelectedChatManually) {
            // After refresh / on initial load: preselect the most recently updated
            // chat in the background without overriding an explicit activeView
            // ('creations' or 'memory') passed on first mount.
            if (newest) {
              selectChat(newest._id, { switchView: false });
            } else if (!list.length) {
              startNewChat({ switchView: false });
              userSelectedChatManually = false;
            }
          } else {
            // User manually selected a chat during this session: keep it unless it was removed
            if (!list.some((c) => c._id === activeChatId)) {
              if (newest) {
                selectChat(newest._id, { switchView: false });
              } else if (!list.length) {
                startNewChat({ switchView: false });
              }
            }
          }
        });

        unsubscribeKnowledge = chatStore.onKnowledgeChanged((kList) => {
          knowledgeList = kList;
        });
      }
      storeError = "";
    } catch (e) {
      storeError = e.message || String(e);
    }
  }

  function reinitSession(key) {
    userSelectedChatManually = false;
    activeChatId = "";
    chats = [];
    deviceCards = [];
    deviceMemoryLoaded = false;
    deviceMemoryError = false;
    deviceMemoryMore = false;
    deviceMemoryTotal = 0;
    _abortDeviceMemoryFetch();
    omdKey = key;
    initSession(key);
  }

  $: liveKey = $omdKeyStore.key;
  $: if (liveKey && liveKey !== omdKey) reinitSession(liveKey);

  let showArchive = false;

  $: unarchivedChats = chats.filter((c) => !c.archived && c.updated !== "1970-01-01T00:00:00Z");
  $: recentChats = unarchivedChats.slice(0, 10);
  $: overflowChats = unarchivedChats.slice(10);
  $: explicitlyArchived = chats.filter((c) => c.archived || c.updated === "1970-01-01T00:00:00Z");
  $: allArchivedChats = [...overflowChats, ...explicitlyArchived];

  $: isOmdClassicGateway = resolveAppMode() !== APP_MODE_CHAT && $gatewayConfig?.skin?.preset === "omd-classic";
  $: isCustomGatewaySkin = resolveAppMode() !== APP_MODE_CHAT && $gatewayConfig?.skin?.preset === "custom";
  $: logoChatSrc = isOmdClassicGateway
    ? (isDarkTheme ? "/customtheme/logo-omd-chat.svg" : "/customtheme/logo-omd-chat-dark.svg")
    : isCustomGatewaySkin
      ? (isDarkTheme ? ($gatewayConfig?.skin?.logoDark || "/customtheme/logo-chat.svg") : ($gatewayConfig?.skin?.logoLight || "/customtheme/logo-chat-dark.svg"))
      : (isDarkTheme ? "/customtheme/logo-chat.svg" : "/customtheme/logo-chat-dark.svg");
  $: logoChatAlt = isOmdClassicGateway
    ? "On My Chat"
    : isCustomGatewaySkin
      ? ($gatewayConfig?.brandName || "AI Assistant")
      : "AI Assistant";
  $: sidebarLogoAlign = resolveAppMode() === APP_MODE_CHAT
    ? "center"
    : ($gatewayConfig?.skin?.logoAlign || "center");

  async function handleArchiveChat(id) {
    chatMenuOpenId = null;
    if (!chatStore) return;
    try {
      await chatStore.archiveChat(id, true);
    } catch (e) {
      console.warn("[chat-applet] archive error:", e);
    }
  }

  async function handleUnarchiveChat(id) {
    chatMenuOpenId = null;
    if (!chatStore) return;
    try {
      await chatStore.archiveChat(id, false);
    } catch (e) {
      console.warn("[chat-applet] unarchive error:", e);
    }
  }

  function selectChat(id, { switchView = true } = {}) {
    if (switchView) {
      activeView = "chat";
    }
    if (activeView === "chat" && id && id !== activeChatId) {
      isChatLoading = true;
    }
    activeChatId = id;
    if (typeof localStorage !== "undefined" && id) {
      localStorage.setItem("omc_active_chat_id", id);
    }
    chatMenuOpenId = null;
    renamingChatId = null;
    // On mobile, auto-close sidebar after selection
    if (switchView && typeof window !== "undefined" && window.innerWidth < 768) {
      sidebarOpen = false;
    }
  }

  function startNewChat({ switchView = true } = {}) {
    if (switchView) {
      activeView = "chat";
      userSelectedChatManually = true;
    }
    if (activeView === "chat") {
      isChatLoading = true;
    }
    const id = newChatId();
    activeChatId = id;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omc_active_chat_id", id);
    }
    chatMenuOpenId = null;
    renamingChatId = null;
    if (typeof window !== "undefined" && window.innerWidth < 768) {
      sidebarOpen = false;
    }
  }

  async function removeChat(id) {
    if (!chatStore) return;
    await chatStore.deleteChat(id);
    chatMenuOpenId = null;
    if (id === activeChatId) {
      const remaining = chats.filter((c) => c._id !== id);
      const unarchived = remaining.filter((c) => !c.archived && c.updated !== "1970-01-01T00:00:00Z");
      if (unarchived.length) {
        selectChat(unarchived[0]._id);
      } else if (remaining.length) {
        selectChat(remaining[0]._id);
      } else {
        startNewChat();
      }
    }
  }

  function startRename(c) {
    renamingChatId = c._id;
    renamingTitle = c.title || c.name || "";
    chatMenuOpenId = null;
  }

  async function saveRename(id) {
    if (!chatStore || !renamingTitle.trim()) {
      renamingChatId = null;
      return;
    }
    const t = renamingTitle.trim();
    renamingChatId = null;
    await chatStore.saveChat({
      _id: id,
      title: t,
      name: t.slice(0, 60),
      updated: new Date().toISOString(),
    });
  }

  function handleTitleChange(e) {
    const { chatId: cid, title } = e.detail || {};
    if (!cid || !title || !chatStore) return;
    const known = chats.find((c) => c._id === cid);
    if (!known || known.title !== title) {
      chatStore
        .saveChat({
          _id: cid,
          title,
          name: title.slice(0, 60),
          updated: new Date().toISOString(),
        })
        .catch(() => {});
    }
  }

  function openCreateMemoryCard(e) {
    selectedMemoryCard = null;
    selectedKindIntent = e?.detail?.kind || "note";
    showMemoryModal = true;
  }

  function openEditMemoryCard(card) {
    selectedMemoryCard = card;
    selectedKindIntent = "note";
    showMemoryModal = true;
  }

  function handleCardOpenFile(item) {
    const docId = item?.document_id || item?.path || item?.id;
    if (!docId) return;
    if (docId.startsWith("http://") || docId.startsWith("https://")) {
      window.open(docId, "_blank", "noopener,noreferrer");
    } else {
      dispatch("open-file", {
        path: docId,
        id: docId,
        title: item?.title || docId.split("/").pop(),
        targetLinkId,
        targetDevice
      });
    }
  }

  function forwardOpenFile(detail) {
    dispatch("open-file", { ...detail, targetLinkId, targetDevice });
  }

  function handleOpenFolder(item) {
    const docId = item?.path || item?.document_id || item?.id;
    if (!docId || !docId.startsWith("/")) return;
    // document_id уже полный логический путь /<устройство>/<путь> — свой
    // префикс в нём есть, склеивать devPrefix второй раз нельзя.
    // Префикс добавляем только если путь устройства не распознаётся.
    const firstSeg = docId.split("/").filter(Boolean)[0] || "";
    const alreadyHasDevice = !!deviceRegistry.getByPrefix(firstSeg);
    let id = docId;
    if (!alreadyHasDevice) {
      const devPrefix = targetDevice && (targetDevice.prefix || ("/" + (targetDevice.linkId || "")).replace(/\/+$/, ""));
      if (!devPrefix) {
        console.warn("[chat-applet] open-folder: no target device prefix");
        return;
      }
      id = devPrefix + docId;
    }
    dispatch("open-folder", {
      id,
      path: docId,
      title: item?.title || docId.split("/").pop(),
    });
  }

  async function handleDeleteKnowledge(id) {
    if (!id) return;
    const isDeviceCard = deviceCards.some((c) => (c.memory_id || c._id) === id);
    if (isDeviceCard) {
      if (!drive) {
        console.warn("[chat-applet] cannot delete on-device card: no drive");
        return;
      }
      try {
        const res = await drive.fetch(`/api/device_memory/${encodeURIComponent(id)}`, {
          method: "DELETE",
          headers: { Accept: "application/json" },
        });
        if (res.ok) {
          deviceCards = deviceCards.filter((c) => (c.memory_id || c._id) !== id);
        } else {
          console.warn("[chat-applet] device card delete failed:", res.status);
        }
      } catch (e) {
        console.warn("[chat-applet] device card delete error:", e);
      }
      return;
    }
    if (!chatStore) return;
    try {
      await chatStore.deleteKnowledge(id);
    } catch (e) {
      console.warn("[chat-applet] delete knowledge error:", e);
    }
  }

  $: activeChat = chats.find((c) => c._id === activeChatId);

  function closeApp() {
    // In chat mode there is no parent surface to close; a close action from
    // Creations/Studio returns to the chat view instead.
    if (isMainApp) {
      if (activeView !== "chat") activeView = "chat";
      return;
    }
    dispatch("close");
  }

  // ── Реиндекс папки (RAG 3.0 контракт — тот же, что посылает C++ нода) ────
  // Источник документов = устройство САМОГО пользователя. У публичной AI-ноды
  // внешний пользователь — гость: её файловая система закрыта (FileInfo home
  // требует admin), поэтому папки выбираются на своих устройствах, а индекс
  // строит удалённая нода через P2P-туннель (выбор папки = consent-грант).
  function localDeviceEntry() {
    return deviceRegistry.list().find((d) => d && d.local === true && d.drive) || null;
  }

  function canBrowseOwnFiles(device) {
    return !!(device && device.drive && (device.local === true || device.isOwner === true));
  }

  // Устройство хранения пользователя (куда сохраняются сгенерированные файлы,
  // аватары, и откуда читаются творения в Creations).
  // Если в пути указано конкретное устройство (напр. /MyLaptop/Private/OnMyChat) — используем его.
  // Если общаемся со своей нодой (владелец) — пишем на неё.
  // Если общаемся с удалённой AI-нодой (гость) — пишем на СВОЁ устройство.
  $: parsedStorage = parseStoragePath($profileStore?.defaultStorage || "");
  $: userStorageDevice = (parsedStorage?.device && !parsedStorage.device.isAiNode ? parsedStorage.device : null)
    || (canBrowseOwnFiles(targetDevice) ? targetDevice : null)
    || deviceRegistry.list().find((d) => d && !d.isAiNode && canBrowseOwnFiles(d))
    || (ownLinkIds && ownLinkIds.length ? deviceRegistry.list().find((d) => d && !d.isAiNode && ownLinkIds.includes(d.linkId) && d.drive) : null)
    || deviceRegistry.list().find((d) => d && !d.isAiNode && !d.local && d.drive)
    || localDeviceEntry()
    || null;

  $: effectiveDeviceDrive = userStorageDevice?.drive || (canBrowseOwnFiles(targetDevice) ? targetDevice?.drive : null);

  function buildIndexSources() {
    const out = [];
    const seen = new Set();
    const push = (d) => {
      if (!d || !d.drive) return;
      const id = d.linkId || d.deviceId;
      if (!id || seen.has(id)) return;
      seen.add(id);
      out.push(d);
    };
    push(localDeviceEntry());
    for (const d of deviceRegistry.list()) {
      if (d.local === true || d.isAiNode === true) continue;
      // shared/guest-устройства файлы не отдают — только свои (isOwner)
      if (d.isOwner !== true) continue;
      push(d);
    }
    // Целевой чат-узел, если это своя нода (владелец) — прежнее поведение.
    if (canBrowseOwnFiles(targetDevice)) push(targetDevice);
    return out;
  }

  function openIndexFolderPicker() {
    indexFolderStatusText = "";
    indexFolderCounts = null;
    indexFolderAbort = false;
    indexSourceError = "";
    const sources = buildIndexSources();
    if (!sources.length) {
      indexFolderStatusText = $_("index_no_own_device", {}, "Add a folder to your own device first, then index it from here");
      setTimeout(() => { indexFolderStatusText = ""; }, 6000);
      return;
    }
    indexSources = sources;
    if (sources.length === 1) {
      openIndexFolderForSource(sources[0]);
      return;
    }
    showIndexSourceChooser = true;
  }

  function openIndexFolderForSource(device) {
    indexSourceDevice = device || null;
    showIndexSourceChooser = false;
    indexSourceError = "";
    showIndexFolderPicker = true;
  }

  // Мобильные браузеры без File System Access: OPFS-папка, потом файлы в неё.
  // Кнопка «добавить папку на этом устройстве» есть и в пикере локального
  // устройства: шапка пикера с этой кнопкой и есть быстрый путь для мобильных,
  // где нет File System Access и шары появляются только через OPFS + загрузку.
  async function addLocalShareForIndex() {
    indexSourceError = "";
    try {
      let share = null;
      if (isFileSystemAccessAvailable()) {
        try {
          share = await addShareFromPicker();
        } catch (e) {
          if (e && (e.name === "AbortError" || e.code === 20)) return; // отмена выбора
          throw e;
        }
      } else {
        share = await addOpfsShare();
      }
      if (!share) return;
      const local = localDeviceEntry();
      if (local) openIndexFolderForSource(local);
    } catch (e) {
      console.warn("[chat-applet] add local share for index failed:", e);
      indexSourceError = (e && e.message) || String(e);
    }
  }

  async function handleIndexFolderSelected(e) {
    showIndexFolderPicker = false;
    const source = indexSourceDevice;
    indexSourceDevice = null;
    // путь выбранной папки на устройстве-источнике
    const path = e?.detail?.realPath || e?.detail?.path || "";
    if (!path || !source) return;

    if (source.local === true) {
      // Файлы лежат на этом устройстве, а индекс строит удалённая нода: нода сама
      // поднимает P2P-туннель к нам и тянет их напрямую. Выбор папки = consent-грант,
      // поэтому нода читает ровно выбранное и может записать карточку в наш GunDB.
      const server = getClientDeviceServer();
      if (!server?.linkId) {
        indexFolderStatusText = $_("index_device_not_ready", {}, "This device is not ready for direct indexing yet");
        setTimeout(() => { indexFolderStatusText = ""; }, 5000);
        return;
      }
      try {
        await reindexFromThisDevice(server.linkId, path);
      } catch (err) {
        console.warn("[chat-applet] P2P reindex failed:", err);
        indexFolderStatusText = `${$_("index_error", {}, "Index error")}: ${err.message || err}`;
        indexFolderCounts = null;
        setTimeout(() => { indexFolderStatusText = ""; }, 6000);
      }
      return;
    }

    // Своя нода: индексирует она сама, из своих шаров.
    await indexFolderOnNode(path, source);
  }

  async function reindexFromThisDevice(deviceLinkId, path) {
    indexFolderStatusText = `Indexing ${path.split("/").pop() || path}...`;
    indexFolderCounts = { indexed: 0, pending: 0, failed: 0 };

    // Одноразовый грант, открывающий окно consent на устройстве: без него нода
    // не проходит даже AuthInfo.
    const grant = issueConsent(path, { writable: true });
    const didPub = omdKeyStore?.identity?.didPub || omdKeyStore?.didPub || "";

    const res = await drive.fetch("/rag/reindex-remote", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Accept: "application/json",
        "X-OMD-RAG-Scope": "all",
      },
      body: JSON.stringify({
        linkId: deviceLinkId,
        consentToken: grant.token,
        didPub,
        path,
      }),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      const msg = (data && (data.detail || data.error?.message || data.error)) || `Reindex request failed (${res.status})`;
      throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
    }
    await pollRemoteReindexStatus(deviceLinkId, path);
    loadDeviceMemory(true);
  }

  async function pollRemoteReindexStatus(deviceLinkId, path) {
    for (;;) {
      if (indexFolderAbort) {
        indexFolderStatusText = "";
        return;
      }
      try {
        const qs = new URLSearchParams({ linkId: deviceLinkId, path });
        const res = await drive.fetch("/rag/reindex-remote/status?" + qs.toString(), {
          headers: { Accept: "application/json" },
        });
        if (res.ok) {
          const s = await res.json();
          indexFolderCounts = {
            indexed: s.indexed ?? 0,
            pending: s.pending ?? 0,
            failed: s.failed ?? 0,
          };
          if (s.done) {
            const n = s.indexed ?? 0;
            if (s.lastError && !n) {
              indexFolderStatusText = `Index failed: ${s.lastError}`;
            } else {
              indexFolderStatusText = `Indexing done: ${n} documents indexed`;
            }
            setTimeout(() => { indexFolderStatusText = ""; }, 4000);
            return;
          }
        }
      } catch (e) {
        console.warn("[chat-applet] remote index status poll failed:", e);
      }
      await new Promise((r) => setTimeout(r, 1000));
    }
  }

  async function indexFolderOnNode(path, srcDevice) {

    // Своя нода индексирует свои шары сама; публичная к гостю их не отдаёт,
    // поэтому папка всегда выбирается на устройствах самого пользователя.
    let useDrive = drive;
    let tmpDrive = null;
    try {
      if (srcDevice && srcDevice.linkId && srcDevice.linkId !== targetLinkId) {
        tmpDrive = new HttpDrive({
          targetLinkId: srcDevice.linkId,
          port,
          isPrivateMode: srcDevice.isOwner === true,
          authToken: omdKey || ""
        });
        useDrive = tmpDrive;
      }
      const res = await useDrive.fetch("/rag/index", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Accept: "application/json",
          "X-OMD-RAG-Scope": "all",
        },
        body: JSON.stringify({ path, force: true }),
      });
      const data = await res.json().catch(() => null);
      if (!res.ok) {
        const msg = (data && (data.detail || data.error?.message || data.error)) || `Index request failed (${res.status})`;
        throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
      }
      const docRoot = data.path || path;
      await pollIndexFolderStatus(docRootForStatus(docRoot, path), useDrive);
      // обнови on-device карточки после индексации
      loadDeviceMemory(true);
    } catch (err) {
      console.warn("[chat-applet] index folder error:", err);
      indexFolderStatusText = `Index error: ${err.message || err}`;
      indexFolderCounts = null;
      setTimeout(() => { indexFolderStatusText = ""; }, 5000);
    } finally {
      if (tmpDrive) {
        try { tmpDrive.close(); } catch (_) {}
      }
    }
  }

  function docRootForStatus(returnedPath, originalPath) {
    // Нормализация ответа ноды: /<share>/...
    let s = (returnedPath || originalPath || "").toString();
    const parts = s.split("/").filter(Boolean);
    return parts.length ? "/" + parts.join("/") : s;
  }

  async function pollIndexFolderStatus(docRoot, statusDrive) {
    const d = statusDrive || drive;
    indexFolderStatusText = `Indexing ${docRoot.split("/").pop() || docRoot}...`;
    for (;;) {
      if (indexFolderAbort) {
        indexFolderStatusText = "";
        return;
      }
      try {
        const qs = new URLSearchParams({ path: docRoot });
        const res = await d.fetch("/rag/status?" + qs.toString(), {
          headers: { Accept: "application/json" },
        });
        if (res.ok) {
          const s = await res.json();
          indexFolderCounts = {
            indexed: s.indexed ?? 0,
            pending: s.pending ?? 0,
            failed: s.failed ?? 0,
          };
          if (s.done) {
            const n = s.indexed ?? 0;
            if (s.lastError && !n) {
              indexFolderStatusText = `Index failed: ${s.lastError}`;
            } else {
              indexFolderStatusText = `Indexing done: ${n} documents indexed`;
            }
            setTimeout(() => { indexFolderStatusText = ""; }, 4000);
            return;
          }
        }
      } catch (e) {
        console.warn("[chat-applet] index status poll failed:", e);
      }
      await new Promise((r) => setTimeout(r, 1000));
    }
  }

  function cancelIndexFolder() {
    indexFolderAbort = true;
    showIndexFolderPicker = false;
    showIndexSourceChooser = false;
    indexSourceDevice = null;
    indexFolderStatusText = "";
  }

  onMount(() => {
    if (hasKey && !chatStore && !storeError) {
      initSession(omdKey);
    }
    if (activeView !== "chat" || (typeof window !== "undefined" && window.innerWidth < 768)) {
      sidebarOpen = false;
    }
  });

  onDestroy(() => {
    if (unsubscribeChats) {
      unsubscribeChats();
      unsubscribeChats = null;
    }
    if (unsubscribeKnowledge) {
      unsubscribeKnowledge();
      unsubscribeKnowledge = null;
    }
    if (drive) {
      drive.close();
      drive = null;
    }
    isChatLoading = false;
  });
</script>

<svelte:window
  on:click={() => (attachMenuOpen = false)}
  on:keydown={(e) => {
    if (e.key === "Escape" && sidebarOpen && isOverlaySidebar) {
      setSidebarOpen(false);
    }
  }}
/>
<div class="applet-root skin-chat {isDarkTheme ? 'dark-theme' : 'light-theme'}">
  <div class="applet-body" class:overlay-sidebar={isOverlaySidebar}>
    <!-- Sidebar Backdrop (Mobile & Applet Overlay Mode) -->
    {#if sidebarOpen}
      <div
        class="sidebar-backdrop"
        role="button"
        tabindex="0"
        on:click={() => setSidebarOpen(false)}
        on:keydown={(e) => e.key === "Escape" && setSidebarOpen(false)}
      ></div>
    {/if}

    <!-- Left Sessions Sidebar -->
    <aside class="sidebar" class:collapsed={!sidebarOpen}>
      <!-- Centered Logo Brand Header in Sidebar -->
      <div
        class="sidebar-brand"
        class:logo-align-left={sidebarLogoAlign === "left"}
        class:logo-align-right={sidebarLogoAlign === "right"}
      >
        <button
          class="icon-btn sidebar-collapse-btn"
          title={$_("fold", {}, "Collapse sidebar")}
          on:click={() => setSidebarOpen(false)}
        >
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
            <line x1="9" y1="3" x2="9" y2="21" />
          </svg>
        </button>

        <img
          src={logoChatSrc}
          alt={logoChatAlt}
          class="sidebar-logo-img"
        />

        <div class="sidebar-brand-spacer" aria-hidden="true"></div>
      </div>

      <div class="sidebar-top">
        <!-- + New Chat Primary Action Button -->
        <button
          class="new-chat-action-btn"
          on:click={() => {
            startNewChat();
            if (typeof window !== "undefined" && window.innerWidth < 768) sidebarOpen = false;
          }}
        >
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7" />
            <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z" />
          </svg>
          <span class="new-chat-label">{$_("new_chat", {}, "New chat")}</span>
        </button>
      </div>

      <!-- Chat Sessions List (Top 10 recent chats) -->
      <div class="sessions-scroll">
        {#if !recentChats.length && !allArchivedChats.length}
          <div class="sessions-empty">
            <span>{$_("no_chats_yet", {}, "No conversations yet")}</span>
          </div>
        {:else}
          <div class="sessions-list">
            {#each recentChats as c (c._id)}
              <div
                class="session-row"
                class:active={activeView === "chat" && c._id === activeChatId}
                role="button"
                tabindex="0"
                on:click={() => {
                  userSelectedChatManually = true;
                  selectChat(c._id);
                }}
                on:keydown={(e) => {
                  if (e.key === "Enter") {
                    userSelectedChatManually = true;
                    selectChat(c._id);
                  }
                }}
              >
                <!-- Title or Rename Input (no duplicate emoji span) -->
                {#if renamingChatId === c._id}
                  <input
                    class="rename-input"
                    bind:value={renamingTitle}
                    on:click|stopPropagation
                    on:keydown|stopPropagation={(e) => {
                      if (e.key === "Enter") saveRename(c._id);
                      if (e.key === "Escape") renamingChatId = null;
                    }}
                    on:blur={() => saveRename(c._id)}
                    autoFocus
                  />
                {:else}
                  <span class="session-title" title={c.title || $_("new_chat", {}, "New chat")}>
                    {c.title || $_("new_chat", {}, "New chat")}
                  </span>
                {/if}

                <!-- Session Options Button (3 dots) -->
                <div class="session-options-wrapper" on:click|stopPropagation>
                  <button
                    class="session-more-btn"
                    title={$_("more", {}, "Options")}
                    on:click={() => (chatMenuOpenId = chatMenuOpenId === c._id ? null : c._id)}
                  >
                    <svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor">
                      <circle cx="12" cy="5" r="2" />
                      <circle cx="12" cy="12" r="2" />
                      <circle cx="12" cy="19" r="2" />
                    </svg>
                  </button>

                  {#if chatMenuOpenId === c._id}
                    <div class="session-menu-dropdown">
                      <button
                        class="menu-item"
                        on:click={() => startRename(c)}
                      >
                        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                          <path d="M12 20h9" />
                          <path d="M16.5 3.5a2.121 2.121 0 0 1 3 3L7 19l-4 1 1-4L16.5 3.5z" />
                        </svg>
                        <span>{$_("rename", {}, "Rename")}</span>
                      </button>
                      <button
                        class="menu-item"
                        on:click={() => handleArchiveChat(c._id)}
                      >
                        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                          <polyline points="21 8 21 21 3 21 3 8" />
                          <rect x="1" y="3" width="22" height="5" />
                          <line x1="10" y1="12" x2="14" y2="12" />
                        </svg>
                        <span>{$_("archive", {}, "Archive")}</span>
                      </button>
                      <button
                        class="menu-item delete"
                        on:click={() => removeChat(c._id)}
                      >
                        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                          <polyline points="3 6 5 6 21 6" />
                          <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                        </svg>
                        <span>{$_("delete", {}, "Delete")}</span>
                      </button>
                    </div>
                  {/if}
                </div>
              </div>
            {/each}

            <!-- Archive Collapsible Section -->
            {#if allArchivedChats.length > 0}
              <div class="archive-widget">
                <button
                  type="button"
                  class="archive-header-btn"
                  on:click={() => (showArchive = !showArchive)}
                >
                  <span class="archive-header-left">
                    <span class="archive-folder-icon">🗄️</span>
                    <span class="archive-title">{$_("archive_folder", {}, "Archive")}</span>
                    <span class="archive-count">{allArchivedChats.length}</span>
                  </span>
                  <svg
                    class="chevron-icon"
                    class:expanded={showArchive}
                    viewBox="0 0 24 24"
                    width="14"
                    height="14"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2"
                  >
                    <polyline points="6 9 12 15 18 9" />
                  </svg>
                </button>

                {#if showArchive}
                  <div class="archived-items-list">
                    {#each allArchivedChats as c (c._id)}
                      <div
                        class="session-row archived-row"
                        class:active={activeView === "chat" && c._id === activeChatId}
                        role="button"
                        tabindex="0"
                        on:click={() => {
                          userSelectedChatManually = true;
                          selectChat(c._id);
                        }}
                        on:keydown={(e) => {
                          if (e.key === "Enter") {
                            userSelectedChatManually = true;
                            selectChat(c._id);
                          }
                        }}
                      >
                        <span class="session-title archived-title" title={c.title || $_("new_chat", {}, "New chat")}>
                          {c.title || $_("new_chat", {}, "New chat")}
                        </span>
                        <div class="archived-actions" on:click|stopPropagation>
                          <button
                            class="session-inline-btn"
                            title={$_("unarchive", {}, "Restore chat")}
                            on:click={() => handleUnarchiveChat(c._id)}
                          >
                            <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                              <polyline points="9 14 15 14 15 8" />
                              <path d="M4 20h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.93a2 2 0 0 1-1.66-.9l-.82-1.2A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13c0 1.1.9 2 2 2z" />
                            </svg>
                          </button>
                          <button
                            class="session-inline-btn delete"
                            title={$_("delete", {}, "Delete chat")}
                            on:click={() => removeChat(c._id)}
                          >
                            <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                              <polyline points="3 6 5 6 21 6" />
                              <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                            </svg>
                          </button>
                        </div>
                      </div>
                    {/each}
                  </div>
                {/if}
              </div>
            {/if}
          </div>
        {/if}
      </div>

      <!-- Navigation, Mode Switch & Profile Section -->
      <div class="sidebar-bottom">
        <!-- Work / Fun Mode Switch -->
        <div class="mode-switch-container">
          <label class="mode-switch-toggle" title="Toggle Work / Fun mode">
            <input
              type="checkbox"
              checked={contentMode === "fun"}
              on:change={toggleMode}
            />
            <span class="switch-slider"></span>
            <span class="mode-switch-label">
              {contentMode === "fun" ? "✨ Fun" : "💼 Work"}
            </span>
          </label>
        </div>

        <div class="sidebar-nav-links">
          {#if showFilesEntry}
            <button
              class="nav-link-btn"
              on:click={() => {
                if (isOverlaySidebar || (typeof window !== "undefined" && window.innerWidth < 768)) sidebarOpen = false;
                dispatch("open-files");
              }}
            >
              <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
              </svg>
              <span>{$_("files_app", {}, "Files")}</span>
            </button>
          {/if}

          <button
            class="nav-link-btn"
            class:active={activeView === "creations"}
            on:click={() => {
              activeView = "creations";
              sidebarOpen = false;
            }}
          >
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
              <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
              <circle cx="8.5" cy="8.5" r="1.5" />
              <polyline points="21 15 16 10 5 21" />
            </svg>
            <span>{$_("generated", {}, "Creations")}</span>
            {#if creationsCount > 0}
              <span class="nav-count-badge">{creationsCount}</span>
            {/if}
          </button>

          <button
            class="nav-link-btn"
            class:active={activeView === "memory"}
            on:click={() => {
              if (activeView === "memory") {
                activeView = "chat";
              } else {
                activeView = "memory";
                sidebarOpen = false;
              }
            }}
          >
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M2 3h6a4 4 0 0 1 4 4v14a3 3 0 0 0-3-3H2z" />
              <path d="M22 3h-6a4 4 0 0 0-4 4v14a3 3 0 0 1 3-3h7z" />
            </svg>
            <span>{$_("knowledge", {}, "Knowledge")}</span>
            {#if allKnowledgeCards.length}
              <span class="nav-count-badge">{allKnowledgeCards.length}</span>
            {/if}
          </button>

          <button
            class="nav-link-btn"
            on:click={() => {
              if (isOverlaySidebar || (typeof window !== "undefined" && window.innerWidth < 768)) sidebarOpen = false;
              dispatch("open-settings", { section: "assistant" });
            }}
          >
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
              <circle cx="12" cy="12" r="3" />
              <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z" />
            </svg>
            <span>{$_("settings", {}, "Settings")}</span>
          </button>
        </div>

        <div class="sidebar-divider"></div>

        <!-- User Profile Card with Sign Out Button -->
        <div class="user-profile-row">
          <button
            type="button"
            class="profile-btn"
            title={$_("settings", {}, "User profile & settings")}
            on:click={() => {
              if (isOverlaySidebar || (typeof window !== "undefined" && window.innerWidth < 768)) {
                sidebarOpen = false;
              }
              dispatch("open-settings", { section: "profile" });
            }}
          >
            <div class="profile-avatar">
              {#if $profileStore.image}
                <img src={$profileStore.image} alt="User" class="avatar-img" />
              {:else}
                <div class="avatar-fallback">
                  {($profileStore.name || $profileStore.user || "A")[0].toUpperCase()}
                </div>
              {/if}
            </div>

            <div class="profile-meta">
              <span class="profile-display-name">
                {$profileStore.name || $profileStore.user || $_("unlinked", {}, "Local User")}
              </span>
              <span class="profile-handle">
                {$profileStore.user ? "@" + $profileStore.user : "Local Device"}
              </span>
            </div>
          </button>

          <button
            class="wx-signout-btn"
            type="button"
            title={$_("logout", {}, "Sign out")}
            on:click={() => window.__omdLogout && window.__omdLogout()}
          >
            <svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor">
              <path fill-rule="evenodd" d="M6 8a.5.5 0 0 0 .5.5h5.793l-2.147 2.146a.5.5 0 0 0 .708.708l3-3a.5.5 0 0 0 0-.708l-3-3a.5.5 0 0 0-.708.708L12.293 7.5H6.5A.5.5 0 0 0 6 8m-2.5 7a.5.5 0 0 1-.5-.5v-13a.5.5 0 0 1 1 0v13a.5.5 0 0 1-.5.5"/>
            </svg>
          </button>
        </div>
      </div>
    </aside>

    <!-- Main Chat Surface -->
    <main class="main-content" bind:clientWidth={mainContentWidth}>
      <!-- Top Chat Header / Action Bar -->
      {#if activeView === "chat"}
        <header class="applet-header" class:overlay-header={isOverlayHeader}>
          <div class="header-left">
            {#if !sidebarOpen}
              <button
                class="icon-btn"
                title={$_("unfold", {}, "Expand sidebar")}
                on:click={() => setSidebarOpen(true)}
              >
                <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
                  <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
                  <line x1="9" y1="3" x2="9" y2="21" />
                </svg>
              </button>
            {/if}
          </div>

          <div class="header-right">
            <!-- Attached Documents Menu (paperclip) -->
            {#if attachedCards.length > 0}
              <div class="header-attach-wrap" on:click|stopPropagation>
                <button
                  class="icon-btn attach-menu-btn"
                  class:active={attachMenuOpen}
                  title={$_("attached_docs_count", {}, "Attached documents to this chat") + ` (${attachedCards.length})`}
                  on:click={() => (attachMenuOpen = !attachMenuOpen)}
                >
                  <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M21.44 11.05l-9.19 9.19a6 6 0 0 1-8.49-8.49l9.19-9.19a4 4 0 0 1 5.66 5.66l-9.2 9.19a2 2 0 0 1-2.83-2.83l8.49-8.48" />
                  </svg>
                </button>

                {#if attachMenuOpen}
                  <div class="attach-menu">
                    {#each attachedCards as c}
                      <button
                        type="button"
                        class="attach-menu-item"
                        on:click={() => {
                          attachMenuOpen = false;
                          chatComponent?.scrollToAttachedCard(c.docId);
                        }}
                      >
                        <svg class="attach-item-icon" viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
                          <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" />
                          <polyline points="14 2 14 8 20 8" />
                          <line x1="9" y1="13" x2="15" y2="13" />
                          <line x1="9" y1="17" x2="13" y2="17" />
                        </svg>
                        <span class="attach-item-title">{c.title}</span>
                      </button>
                    {/each}
                  </div>
                {/if}
              </div>
            {/if}

            <!-- Close / Back to Files Button (hidden when chat is the root app) -->
            {#if !isMainApp}
              <button class="icon-btn close-btn" title={$_("close", {}, "Close")} on:click={closeApp}>
                <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
                  <line x1="18" y1="6" x2="6" y2="18" />
                  <line x1="6" y1="6" x2="18" y2="18" />
                </svg>
              </button>
            {/if}
          </div>
        </header>
      {/if}

      {#if !hasKey}
        <div class="key-prompt-banner">
          <div class="prompt-box">
            <div class="prompt-icon">🔐</div>
            <h2>{$_("api_key_required", {}, "API key required")}</h2>
            <p>
              {$_("api_key_desc", {}, "Please configure your omd_key in Settings → Security & Keys to unlock encrypted chats and local knowledge synchronization.")}
            </p>
            <button class="primary-btn" on:click={() => dispatch("open-settings")}>
              {$_("open_settings", {}, "Open Settings")}
            </button>
          </div>
        </div>
      {:else if storeError}
        <div class="key-prompt-banner">
          <div class="prompt-box error">
            <div class="prompt-icon">⚠️</div>
            <h2>{$_("storage_error", {}, "Storage error")}</h2>
            <p>{storeError}</p>
          </div>
        </div>
      {:else if activeView === "creations"}
        <KreaStudio
          httpDrive={drive}
          deviceDrive={effectiveDeviceDrive}
          targetDevice={userStorageDevice || targetDevice}
          targetLinkId={(userStorageDevice && userStorageDevice.linkId) || targetLinkId || targetDevice?.linkId || ""}
          isPrivateMode={computedPrivateMode}
          {omdKey}
          {chatStore}
          {isDarkTheme}
          {contentMode}
          {isOverlayHeader}
          {sidebarOpen}
          sdxlSupported={imageGenSupported}
          {kreaSupported}
          defaultStorage={$profileStore.defaultStorage || ""}
          userName={$profileStore.name || $profileStore.user || ""}
          on:count-change={(e) => (creationsCount = e.detail)}
          on:toggle-sidebar={toggleSidebar}
          on:close={closeApp}
          on:open-file={(e) => forwardOpenFile(e.detail)}
          on:open-settings={(e) => dispatch("open-settings", e.detail)}
        />
      {:else if activeView === "memory"}
        <MemoryCards
          knowledgeList={allKnowledgeCards}
          {isDarkTheme}
          {isOverlayHeader}
          {sidebarOpen}
          isOwner={computedPrivateMode}
          {hasOwnHardwareNode}
          loading={deviceMemoryLoading}
          thumbFetcher={fetchDeviceThumb}
          httpDrive={drive}
          {targetDevice}
          targetLinkId={targetLinkId || targetDevice?.linkId || ""}
          on:create={openCreateMemoryCard}
          on:index-folder={openIndexFolderPicker}
          on:edit={(e) => openEditMemoryCard(e.detail.card)}
          on:delete={(e) => handleDeleteKnowledge(e.detail.id)}
          on:open-file={(e) => handleCardOpenFile(e.detail)}
          on:open-folder={(e) => handleOpenFolder(e.detail)}
          on:refresh={() => loadDeviceMemory(true)}
          on:toggle-sidebar={toggleSidebar}
          on:close={closeApp}
        />
      {:else}
        <Chat
          bind:this={chatComponent}
          bind:isChatLoading
          chatId={activeChatId}
          httpDrive={drive}
          deviceDrive={effectiveDeviceDrive}
          {targetDevice}
          storageDevice={userStorageDevice}
          targetLinkId={targetLinkId || targetDevice?.linkId || ""}
          isPrivateMode={computedPrivateMode}
          {ownLinkIds}
          {omdKey}
          thumbFetcher={fetchDeviceThumb}
          {chatStore}
          {isDarkTheme}
          {contentMode}
          {isOverlayHeader}
          defaultStorage={$profileStore.defaultStorage || ""}
          currentChatTitle={activeChat?.title || activeChat?.name || ""}
          userName={$profileStore.name || $profileStore.user || ""}
          on:title-change={handleTitleChange}
          on:new-chat={startNewChat}
          on:open-knowledge={() => (activeView = "memory")}
          on:open-file={(e) => forwardOpenFile(e.detail)}
          on:attached-change={(e) => (attachedCards = e.detail)}
        />
      {/if}
    </main>

    <MemoryCardModal
      bind:open={showMemoryModal}
      card={selectedMemoryCard}
      kindIntent={selectedKindIntent}
      {chatStore}
      httpDrive={drive}
      device={targetDevice}
      targetLinkId={targetLinkId || targetDevice?.linkId || ""}
      isPrivateMode={computedPrivateMode}
      {isDarkTheme}
      thumbFetcher={fetchDeviceThumb}
      on:open-file={(e) => forwardOpenFile(e.detail)}
      on:open-folder={(e) => handleOpenFolder(e.detail)}
      on:imported={() => loadDeviceMemory(true)}
      on:save={() => loadDeviceMemory(true)}
    />

    {#if showIndexSourceChooser}
      <!-- svelte-ignore a11y-click-events-have-key-events -->
      <!-- svelte-ignore a11y-no-static-element-interactions -->
      <div class="source-chooser-backdrop" on:click|self={() => (showIndexSourceChooser = false)}>
        <div class="source-chooser">
          <div class="source-chooser-header">
            <h3>{$_("index_source_choose", {}, "Where are the files?")}</h3>
            <p>{$_("index_source_hint", {}, "Index a folder on one of your own devices. The node reads it directly from the device — nothing is uploaded through the chat.")}</p>
          </div>
          <div class="source-list">
            {#each indexSources as src (src.linkId || src.deviceId)}
              <button type="button" class="source-item" on:click={() => openIndexFolderForSource(src)}>
                <span class="source-icon">
                  {#if src.local === true}
                    <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="4" width="20" height="14" rx="2"/><path d="M2 9h20"/></svg>
                  {:else}
                    <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="8" rx="2"/><rect x="2" y="13" width="20" height="8" rx="2"/><path d="M6 7h.01M6 17h.01"/></svg>
                  {/if}
                </span>
                <span class="source-name">{src.name || $_("this_device", {}, "This device")}</span>
                <span class="source-badge">{src.local === true ? $_("this_device", {}, "This device") : $_("my_device", {}, "My device")}</span>
              </button>
            {/each}
          </div>
          {#if indexSourceError}
            <div class="source-error">{indexSourceError}</div>
          {/if}
          <div class="source-chooser-actions">
            <button type="button" class="source-add" on:click={addLocalShareForIndex}>
              {$_("add_folder_this_device", {}, "Add folder on this device")}
            </button>
            <button type="button" class="source-cancel" on:click={() => (showIndexSourceChooser = false)}>
              {$_("cancel", {}, "Cancel")}
            </button>
          </div>
        </div>
      </div>
    {/if}

    {#if showIndexFolderPicker}
      <FolderPicker
        device={indexSourceDevice || targetDevice}
        mode="folder"
        uploadInFolder={(indexSourceDevice?.local === true)}
        onAddFolder={(indexSourceDevice?.local === true) ? addLocalShareForIndex : null}
        title={$_("index_folder_pick", {}, "Select folder to index")}
        on:select={handleIndexFolderSelected}
        on:cancel={() => (showIndexFolderPicker = false)}
      />
    {/if}

    {#if indexFolderStatusText}
      <!-- svelte-ignore a11y-click-events-have-key-events -->
      <!-- svelte-ignore a11y-no-static-element-interactions -->
      <div class="progress-overlay" role="button" tabindex="-1">
        <div class="progress-box">
          <p>{indexFolderStatusText}</p>
          <div class="progress-bar">
            {#if indexFolderCounts && !indexFolderCounts.done}
              <div class="progress-fill indeterminate"></div>
            {:else}
              <div class="progress-fill indeterminate"></div>
            {/if}
          </div>
          {#if indexFolderCounts}
            <p class="progress-text">
              {indexFolderCounts.indexed} indexed{#if indexFolderCounts.failed} · {indexFolderCounts.failed} failed{/if}
            </p>
          {/if}
          <button class="cancel-btn" on:click={cancelIndexFolder}>Close</button>
        </div>
      </div>
    {/if}
  </div>
</div>

<style>
  /* Выбор устройства-источника для реиндекса (гость публичной ноды) */
  .source-chooser-backdrop {
    position: fixed;
    top: 0; left: 0; right: 0; bottom: 0;
    background: rgba(15, 17, 26, 0.8);
    display: flex;
    justify-content: center;
    align-items: center;
    padding: 20px;
    z-index: 1100;
    backdrop-filter: blur(4px);
  }

  .source-chooser {
    background: var(--bg-secondary, #1A121D);
    border: 1px solid var(--border-color, #352339);
    border-radius: 14px;
    width: min(460px, 100%);
    max-height: 80vh;
    overflow: auto;
    padding: 20px;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }

  .source-chooser-header h3 {
    margin: 0 0 6px;
    font-size: 16px;
  }

  .source-chooser-header p {
    margin: 0;
    font-size: 12px;
    line-height: 1.5;
    opacity: 0.7;
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
    border: 1px solid var(--border-color, #352339);
    background: var(--bg-primary, #0F0A10);
    color: inherit;
    cursor: pointer;
    text-align: left;
  }

  .source-item:hover { border-color: var(--accent, #EC4899); }

  .source-icon { display: inline-flex; opacity: 0.8; }

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

  .source-error {
    font-size: 12px;
    color: #ff6b6b;
  }

  .source-chooser-actions {
    display: flex;
    gap: 8px;
    justify-content: flex-end;
    flex-wrap: wrap;
  }

  .source-add, .source-cancel {
    padding: 8px 14px;
    border-radius: 8px;
    border: 1px solid var(--border-color, #352339);
    background: transparent;
    color: inherit;
    cursor: pointer;
    font-size: 13px;
  }

  .source-add { border-color: var(--accent, #EC4899); }

  /* Indexed folder progress — same look as the "folder upload" dialog in App.svelte */
  .progress-overlay {
    position: fixed;
    top: 0; left: 0; right: 0; bottom: 0;
    background: rgba(15, 10, 16, 0.8);
    display: flex;
    justify-content: center;
    align-items: center;
    z-index: 1000;
    backdrop-filter: blur(4px);
  }

  .progress-box {
    background: var(--bg-secondary, #1A121D);
    padding: 24px;
    border-radius: 12px;
    box-shadow: 0 10px 25px rgba(0,0,0,0.5);
    width: 300px;
    text-align: center;
    border: 1px solid var(--border-color, #352339);
  }

  .progress-box p {
    margin: 0 0 12px 0;
    font-weight: 500;
    color: var(--text-primary, #FDF8FC);
  }

  .progress-bar {
    height: 8px;
    background: var(--bg-primary, #0F0A10);
    border-radius: 4px;
    overflow: hidden;
    margin-bottom: 8px;
  }

  .progress-fill {
    height: 100%;
    background: var(--accent, #EC4899);
    transition: width 0.3s;
  }

  .progress-fill.indeterminate {
    width: 50%;
    animation: indeterminate-bounce 1.5s infinite ease-in-out;
  }

  @keyframes indeterminate-bounce {
    0% { transform: translateX(-100%); }
    50% { transform: translateX(100%); width: 70%; }
    100% { transform: translateX(200%); width: 50%; }
  }

  .progress-text {
    font-size: 11px;
    color: var(--text-muted, #998094) !important;
    margin-bottom: 4px !important;
  }

  .cancel-btn {
    margin-top: 16px;
    background: transparent;
    border: 1px solid var(--border-color, #352339);
    color: var(--text-primary, #FDF8FC);
    border-radius: 4px;
    padding: 6px 16px;
    cursor: pointer;
    font-size: 13px;
    transition: all 0.2s;
  }

  .applet-root {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    background: var(--bg-primary);
    color: var(--text-primary);
    overflow: hidden;
  }

  /* Header Bar inside main content */
  .applet-header {
    height: calc(56px + env(safe-area-inset-top, 0px));
    padding: env(safe-area-inset-top, 0px) 14px 0 14px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    background: var(--bg-primary);
    border-bottom: none;
    flex-shrink: 0;
    z-index: 10;
    box-sizing: border-box;
    transition: background 0.15s ease;
  }

  /* Overlay Header: activated when horizontal space is plenty (>= 960px) */
  .applet-header.overlay-header {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    background: transparent;
    border-bottom: none;
    pointer-events: none;
    z-index: 20;
  }

  .applet-header.overlay-header .header-left,
  .applet-header.overlay-header .header-right {
    pointer-events: auto;
  }

  .header-left,
  .header-right {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-top: 0;
  }

  .sidebar-brand {
    height: calc(56px + env(safe-area-inset-top, 0px));
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: env(safe-area-inset-top, 0px) 14px 0 14px;
    border-bottom: none;
    flex-shrink: 0;
    box-sizing: border-box;
    gap: 6px;
  }

  .sidebar-logo-img {
    height: 29px;
    max-width: calc(100% - 80px);
    width: auto;
    object-fit: contain;
    user-select: none;
    display: block;
    margin: 0 auto;
  }

  .sidebar-brand.logo-align-left .sidebar-logo-img {
    margin-left: 4px;
    margin-right: auto;
  }

  .sidebar-brand.logo-align-right .sidebar-logo-img {
    margin-left: auto;
    margin-right: 4px;
  }

  .sidebar-brand-spacer {
    width: 34px;
    height: 34px;
    flex-shrink: 0;
  }

  .icon-btn {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    cursor: pointer;
    width: 34px;
    height: 34px;
    padding: 0;
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    transition: all 0.15s;
  }

  .icon-btn:hover {
    background: var(--bg-card-hover);
    color: var(--text-primary);
  }

  .icon-btn.active {
    background: var(--accent-soft);
    color: var(--accent);
  }

  /* Attached documents menu (paperclip dropdown) */
  .header-attach-wrap {
    position: relative;
    display: flex;
    align-items: center;
  }

  .attach-menu {
    position: absolute;
    top: calc(100% + 6px);
    right: 0;
    min-width: 240px;
    max-width: 320px;
    max-height: 320px;
    overflow-y: auto;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.35);
    padding: 6px;
    z-index: 50;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .attach-menu-item {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
    padding: 8px 10px;
    border: none;
    border-radius: 8px;
    background: transparent;
    color: var(--text-primary);
    cursor: pointer;
    text-align: left;
    transition: background 0.15s;
  }

  .attach-menu-item:hover {
    background: var(--bg-card-hover);
  }

  .attach-item-icon {
    flex-shrink: 0;
    color: var(--text-muted);
    display: block;
  }

  .attach-item-title {
    flex: 1;
    min-width: 0;
    font-size: 13px;
    line-height: 1.3;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* Body */
  .applet-body {
    display: flex;
    flex: 1;
    min-height: 0;
    position: relative;
    height: 100%;
  }

  /* Sidebar */
  .sidebar {
    width: 270px;
    background: var(--sidebar-bg);
    border-right: none;
    box-shadow: var(--sidebar-shadow);
    position: relative;
    display: flex;
    flex-direction: column;
    flex-shrink: 0;
    transition: transform 0.25s ease, width 0.25s ease;
    z-index: 20;
  }

  .sidebar.collapsed {
    display: none;
  }

  .sidebar-top {
    padding: 0 14px 8px 10px;
  }

  .new-chat-action-btn {
    width: 100%;
    display: flex;
    align-items: center;
    gap: 10px;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 20px;
    padding: 9px 16px;
    color: var(--text-primary);
    font-size: 0.9rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s ease;
    box-shadow: 0 2px 6px rgba(0, 0, 0, 0.08);
  }

  .new-chat-action-btn:hover {
    background: var(--bg-card-hover);
    border-color: var(--accent-blue);
    color: var(--accent-blue);
  }

  /* Sessions List */
  .sessions-scroll {
    flex: 1;
    overflow-y: auto;
    overflow-x: hidden;
    padding: 4px 10px;
    scrollbar-width: thin;
    scrollbar-color: transparent transparent;
    transition: scrollbar-color 0.2s ease;
  }

  .sessions-scroll::-webkit-scrollbar {
    width: 6px;
  }

  .sessions-scroll::-webkit-scrollbar-track {
    background: transparent;
  }

  .sessions-scroll::-webkit-scrollbar-thumb {
    background: transparent;
    border-radius: 4px;
    transition: background 0.2s ease;
  }

  /* Show scrollbar ONLY when sidebar is focused or hovered */
  .sidebar:hover .sessions-scroll,
  .sidebar:focus-within .sessions-scroll {
    scrollbar-color: var(--scrollbar-thumb) transparent;
  }

  .sidebar:hover .sessions-scroll::-webkit-scrollbar-thumb,
  .sidebar:focus-within .sessions-scroll::-webkit-scrollbar-thumb {
    background: var(--scrollbar-thumb);
  }

  .sidebar:hover .sessions-scroll::-webkit-scrollbar-thumb:hover,
  .sidebar:focus-within .sessions-scroll::-webkit-scrollbar-thumb:hover {
    background: var(--scrollbar-thumb-hover);
  }

  .sessions-empty {
    padding: 24px 12px;
    text-align: center;
    color: var(--text-muted);
    font-size: 0.85rem;
  }

  .sessions-list {
    display: flex;
    flex-direction: column;
    gap: 3px;
  }

  .session-row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 8px 10px;
    border-radius: 10px;
    cursor: pointer;
    transition: background 0.15s ease;
    position: relative;
    color: var(--text-primary);
  }

  .session-row:hover {
    background: var(--bg-card-hover);
  }

  .session-row.active {
    background: var(--accent-soft);
    font-weight: 500;
  }

  .session-title {
    flex: 1;
    font-size: 0.88rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* Archive widget styles */
  .archive-widget {
    margin-top: 8px;
    border-top: 1px solid var(--border-subtle);
    padding-top: 6px;
  }

  .archive-header-btn {
    width: 100%;
    display: flex;
    align-items: center;
    justify-content: space-between;
    background: transparent;
    border: none;
    color: var(--text-secondary);
    padding: 8px 10px;
    border-radius: 8px;
    cursor: pointer;
    font-size: 0.84rem;
    font-weight: 500;
    transition: background 0.15s ease, color 0.15s ease;
  }

  .archive-header-btn:hover {
    background: var(--bg-card-hover);
    color: var(--text-primary);
  }

  .archive-header-left {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .archive-folder-icon {
    font-size: 0.95rem;
  }

  .archive-title {
    font-size: 0.85rem;
    font-weight: 600;
  }

  .archive-count {
    background: var(--accent-soft);
    color: var(--accent);
    font-size: 0.72rem;
    padding: 1px 6px;
    border-radius: 10px;
    font-weight: 600;
  }

  .chevron-icon {
    color: var(--text-muted);
    transition: transform 0.2s ease;
  }

  .chevron-icon.expanded {
    transform: rotate(180deg);
  }

  .archived-items-list {
    display: flex;
    flex-direction: column;
    gap: 2px;
    margin-top: 4px;
    padding-left: 6px;
  }

  .archived-row {
    opacity: 0.85;
    font-size: 0.84rem;
  }

  .archived-row:hover {
    opacity: 1;
  }

  .archived-title {
    color: var(--text-secondary);
  }

  .archived-actions {
    display: flex;
    align-items: center;
    gap: 4px;
  }

  .session-inline-btn {
    background: transparent;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    padding: 3px;
    border-radius: 4px;
    display: flex;
    align-items: center;
    transition: color 0.15s ease, background 0.15s ease;
  }

  .session-inline-btn:hover {
    color: var(--accent-blue);
    background: var(--bg-card-hover);
  }

  .session-inline-btn.delete:hover {
    color: #ef4444;
  }

  .rename-input {
    flex: 1;
    background: var(--bg-primary);
    border: 1px solid var(--accent-blue);
    border-radius: 4px;
    padding: 2px 6px;
    font-size: 0.85rem;
    color: var(--text-primary);
    outline: none;
  }

  .session-options-wrapper {
    position: relative;
  }

  .session-more-btn {
    background: transparent;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    padding: 2px 4px;
    border-radius: 4px;
    display: flex;
    align-items: center;
    opacity: 0;
    transition: opacity 0.15s;
  }

  .session-row:hover .session-more-btn,
  .session-row.active .session-more-btn {
    opacity: 1;
  }

  .session-more-btn:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.1);
  }

  .session-menu-dropdown {
    position: absolute;
    right: 0;
    top: 100%;
    margin-top: 4px;
    background: var(--bg-secondary);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 4px;
    display: flex;
    flex-direction: column;
    gap: 2px;
    z-index: 50;
    box-shadow: 0 4px 16px rgba(0, 0, 0, 0.25);
    min-width: 110px;
  }

  .menu-item {
    display: flex;
    align-items: center;
    gap: 8px;
    background: transparent;
    border: none;
    padding: 6px 8px;
    font-size: 0.82rem;
    color: var(--text-primary);
    cursor: pointer;
    border-radius: 4px;
    text-align: left;
    width: 100%;
  }

  .menu-item:hover {
    background: var(--bg-card-hover);
  }

  .menu-item.delete {
    color: #ef4444;
  }

  .menu-item.delete:hover {
    background: rgba(239, 68, 68, 0.12);
  }

  /* Sidebar Bottom */
  .sidebar-bottom {
    padding: 12px 14px calc(12px + env(safe-area-inset-bottom, 0px));
    border-top: 1px solid var(--border-color);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  /* Work / Fun Switch */
  .mode-switch-container {
    padding: 0;
  }

  .mode-switch-toggle {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 10px;
    border-radius: 8px;
    color: var(--text-secondary);
    cursor: pointer;
    user-select: none;
    transition: all 0.15s;
    width: 100%;
    box-sizing: border-box;
  }

  .mode-switch-toggle:hover {
    background: var(--bg-card-hover);
    color: var(--text-primary);
  }

  .mode-switch-toggle input {
    display: none;
  }

  .switch-slider {
    position: relative;
    width: 38px;
    height: 20px;
    background: var(--border-color);
    border-radius: 10px;
    transition: background 0.2s;
    flex-shrink: 0;
  }

  .switch-slider::after {
    content: "";
    position: absolute;
    top: 2px;
    left: 2px;
    width: 15px;
    height: 15px;
    border-radius: 50%;
    background: #fff;
    transition: transform 0.2s;
  }

  .mode-switch-toggle input:checked + .switch-slider {
    background: var(--accent);
  }

  .mode-switch-toggle input:checked + .switch-slider::after {
    transform: translateX(18px);
  }

  .mode-switch-label {
    font-size: 0.88rem;
    font-weight: 600;
  }

  .sidebar-nav-links {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .nav-link-btn {
    display: flex;
    align-items: center;
    gap: 10px;
    background: transparent;
    border: none;
    padding: 8px 10px;
    border-radius: 8px;
    color: var(--text-secondary);
    font-size: 0.88rem;
    cursor: pointer;
    transition: all 0.15s;
    text-align: left;
  }

  .nav-link-btn:hover {
    background: var(--bg-card-hover);
    color: var(--text-primary);
  }

  .nav-link-btn.active {
    background: var(--accent-soft);
    color: var(--accent);
  }

  .nav-count-badge {
    margin-left: auto;
    background: var(--accent-soft);
    color: var(--accent);
    font-size: 0.72rem;
    padding: 1px 6px;
    border-radius: 10px;
    font-weight: 600;
  }

  .sidebar-divider {
    height: 1px;
    background: var(--border-color);
    margin: 2px 0;
  }

  /* User Profile Card */
  .user-profile-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 6px;
    padding: 6px 4px;
    width: 100%;
    box-sizing: border-box;
  }

  .profile-btn {
    display: flex;
    align-items: center;
    gap: 10px;
    background: transparent;
    border: none;
    padding: 4px 6px;
    border-radius: 8px;
    cursor: pointer;
    text-align: left;
    flex-grow: 1;
    min-width: 0;
    color: inherit;
    font-family: inherit;
    transition: background 0.15s ease;
  }

  .profile-btn:hover {
    background: var(--bg-card-hover);
  }

  .profile-avatar {
    width: 34px;
    height: 34px;
    border-radius: 50%;
    overflow: hidden;
    flex-shrink: 0;
  }

  .avatar-img {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }

  .avatar-fallback {
    width: 100%;
    height: 100%;
    background: var(--accent-blue);
    color: #fff;
    display: flex;
    align-items: center;
    justify-content: center;
    font-weight: 700;
    font-size: 0.88rem;
  }

  .profile-meta {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }

  .profile-display-name {
    font-size: 0.85rem;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    line-height: 1.2;
  }

  .profile-handle {
    font-size: 0.75rem;
    color: var(--text-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    line-height: 1.2;
    margin-top: 2px;
  }

  .wx-signout-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    background: transparent;
    border: none;
    color: var(--text-muted);
    padding: 7px;
    border-radius: 6px;
    cursor: pointer;
    flex-shrink: 0;
    transition: all 0.15s ease;
  }

  .wx-signout-btn:hover {
    color: #ef4444;
    background-color: rgba(239, 68, 68, 0.12);
  }

  /* Main Content */
  .main-content {
    flex: 1;
    display: flex;
    flex-direction: column;
    min-width: 0;
    position: relative;
  }

  .key-prompt-banner {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 24px;
  }

  .prompt-box {
    max-width: 440px;
    text-align: center;
    background: var(--bg-secondary);
    border: 1px solid var(--border-color);
    border-radius: 16px;
    padding: 32px 24px;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.15);
  }

  .prompt-icon {
    font-size: 2.5rem;
    margin-bottom: 12px;
  }

  .prompt-box h2 {
    margin: 0 0 10px;
    font-size: 1.25rem;
  }

  .prompt-box p {
    color: var(--text-secondary);
    font-size: 0.9rem;
    line-height: 1.5;
    margin-bottom: 20px;
  }

  .primary-btn {
    background: var(--accent-blue);
    color: #fff;
    border: none;
    padding: 10px 22px;
    border-radius: 10px;
    font-size: 0.92rem;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.2s;
  }

  .primary-btn:hover {
    background: var(--accent-blue-hover);
  }

  .sidebar-backdrop {
    display: none;
  }

  /* Applet Overlay Sidebar Mode (Creations / Creative Studio / Knowledge) */
  .applet-body.overlay-sidebar .sidebar-backdrop {
    display: block;
    position: fixed;
    inset: 0;
    background: rgba(15, 10, 16, 0.45);
    backdrop-filter: blur(3px);
    z-index: 200;
    animation: sidebarFadeIn 0.16s ease;
  }

  .applet-body.overlay-sidebar .sidebar {
    position: fixed;
    top: 0;
    bottom: 0;
    left: 0;
    height: 100%;
    height: 100dvh;
    z-index: 210;
    box-shadow: 8px 0 32px rgba(0, 0, 0, 0.35);
    animation: sidebarSlideIn 0.2s cubic-bezier(0.16, 1, 0.3, 1);
  }

  @keyframes sidebarSlideIn {
    from { transform: translateX(-100%); }
    to { transform: translateX(0); }
  }

  @keyframes sidebarFadeIn {
    from { opacity: 0; }
    to { opacity: 1; }
  }

  /* Responsive Rules */
  @media (max-width: 768px) {
  .applet-root {
      flex-direction: column;
    }

    .applet-header {
      height: calc(48px + env(safe-area-inset-top, 0px));
      padding: env(safe-area-inset-top, 0px) 12px 0 12px;
      background: var(--bg-card);
      border-bottom: none;
    }

    .sidebar-brand {
      height: calc(48px + env(safe-area-inset-top, 0px));
      padding: env(safe-area-inset-top, 0px) 12px 0 12px;
    }

    .sidebar-backdrop {
      display: block;
      position: fixed;
      inset: 0;
      background: rgba(0, 0, 0, 0.5);
      backdrop-filter: blur(3px);
      z-index: 200;
    }

    .sidebar {
      position: fixed;
      top: 0;
      bottom: 0;
      left: 0;
      height: 100%;
      height: 100dvh;
      z-index: 210;
      box-shadow: 4px 0 24px rgba(0, 0, 0, 0.35);
      animation: sidebarSlideIn 0.2s cubic-bezier(0.16, 1, 0.3, 1);
    }

  }
</style>
