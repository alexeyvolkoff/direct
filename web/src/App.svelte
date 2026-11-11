<script>
  import { onMount, onDestroy } from "svelte";
  import { Capacitor } from "@capacitor/core";
  import { locale, _, translate, getSvarLocale, SUPPORTED_LANGUAGES, setLocale } from "./lib/i18n/index.js";
  import { Filemanager } from "./lib/svar-filemanager";
  import { getMenuOptions } from "./lib/svar-filemanager/store/index.js";
  import { Willow, WillowDark, Locale } from "@svar-ui/svelte-core";
  import { WebRTCDataChannelDrive } from "./lib/webrtc-drive.js";
  import { ensureShareDrive, getShareDriveEntry, getShareDriveByPrefix, closeShareDrive } from "./lib/share-drives.js";
  import { HttpDrive } from "./lib/http-drive.js";
  import { addShareFromPicker, addOpfsShare, ensureDefaultOpfsShare, DEFAULT_STORAGE_PATH, saveLocalShare, makeLocalDevice, getOrCreateLocalLinkId, isFileSystemAccessAvailable, listLocalShares, clearAllLocalShares, getDefaultWebDeviceName, getResolutionDeviceType } from "./lib/local-client.js";
  import { isNativeStorageAvailable, onStorageChanged } from "./lib/native-drive.js";
  import { startClientDeviceServer, stopClientDeviceServer } from "./lib/client-device-server.js";
  import { EphemeralIdentity } from "./lib/identity/ephemeral.js";
  import { PersistentIdentity, getDidTag } from "./lib/identity/persistent.js";
  import { peekSavedSessionSync, loadSavedSessionResilient } from "./lib/identity/sessionBackup.js";
  import { signInWithGoogleMobile, signInWithAppleMobile, isGoogleSignInCanceledError } from "./lib/identity/googleMobileAuth.js";
  import { loadMyDevices } from "./lib/identity/device-list.js";
  import { setSshIdentity, clearSshIdentity } from "./lib/identity/ssh-credentials.js";
  import { setMcpKeysIdentity, clearMcpKeysIdentity } from "./lib/mcp-keys.js";
  import { getSharedGatewayChannel } from "./lib/gateway-channel.js";
  import { aiNodeStore, hydrateAiNodeState, refreshPublicAiNode, setAiNodeChoice, acceptPublicAiAgreement, applyRemotePublicAiAck, aiOnmychatHeaders, AI_NODE_MODE_AUTO, AI_NODE_MODE_PUBLIC, AI_NODE_AGREEMENT_VERSION } from "./lib/ai-node.js";
  import { ensureAssistantDefaults } from "./lib/assistant-defaults.js";
  import { setOmdKeyIdentity, clearOmdKeyIdentity, getStoredOmdKey, storeOmdKey, getGatewayBase } from "./lib/omd-key.js";
  import PdfViewer from "./components/PdfViewer.svelte";
  import VideoViewer from "./components/VideoViewer.svelte";
  import AudioViewer from "./components/AudioViewer.svelte";
  import Editor from "./components/Editor.svelte";
  import WordCraftEditor from "./components/WordCraftEditor.svelte";
  import GridCraftEditor from "./components/GridCraftEditor.svelte";
  import DeckCraftEditor from "./components/DeckCraftEditor.svelte";
  import { isOfficeDocument, extractDocTextFromBlob, isBinaryResponseType, WC_DOC_EXTS, GC_SHEET_EXTS, DC_PRES_EXTS, univerDocToMarkdown } from "./lib/univer-doc-utils.js";
  import Workspace from "./components/Workspace.svelte";
  import AppProxyView from "./components/AppProxyView.svelte";
  import Settings from "./components/Settings.svelte";
  import {
    APP_PROXY_MIME,
    initAppProxyServiceWorkerBridge,
    discoverNodeApps,
    getCachedNodeAppItems,
    mergeAppsIntoRootItems,
    registerProxiedApp,
    getProxiedApp,
    closeAppProxyDrivesForNode
  } from "./lib/app-proxy.js";
  import { resolveAppIconDataUri } from "./lib/app-icons.js";
  import ChatApplet from "./components/ChatApplet.svelte";
  import SingleView from "./components/SingleView.svelte";
  import MarkdownViewer from "./components/MarkdownViewer.svelte";
  import PairModal from "./components/PairModal.svelte";
  import ShareModal from "./components/ShareModal.svelte";
  import AddShareModal from "./components/AddShareModal.svelte";
  import FolderPicker from "./components/FolderPicker.svelte";
  import DeviceSettings from "./components/DeviceSettings.svelte";
  import QrScannerModal from "./components/QrScannerModal.svelte";
  import UpdateNotification from "./components/UpdateNotification.svelte";
  import OnboardingModal from "./components/OnboardingModal.svelte";
  import GatewayTosModal from "./components/GatewayTosModal.svelte";
  import {
    gatewayConfig,
    getGatewayTosUrl,
    getGatewayPrivacyUrl,
    getGatewayBrandPalette,
    getGatewayRelayWsUrl,
    getGatewayOauthBridgeOrigin,
    applyGatewaySkinToDom,
  } from "./lib/gateway-config.js";
  import { resolveShare, loadSharedByMe, subscribeSharedByMe, loadSharedToMe, subscribeSharedToMe, addSharedToMe, removeSharedToMe, revokeShare } from "./lib/sharing.js";
  import { publicOrigin, sharedLinkUrl, sanitizeShareUrl } from "./lib/public-origin.js";
  import { resolveAppMode, APP_MODE_CHAT, getDomainBrandTitle } from "./lib/app-mode.js";
  import { fetchNearbyDevices } from "./lib/local-discovery.js";
  import { checkForOtaUpdate } from "./lib/ota-updater.js";
  import { cameraSyncManager } from "./lib/camera-sync.js";

  import { setProfile } from "./lib/profile-store.js";
  import { parseStoragePath, formatStoragePath, persistStoragePath } from "./lib/storage-path.js";
  import { getViewerSettings } from "./lib/viewer-settings.js";
  import { syncExistingWebPushSubscription, initNativePushListeners } from "./lib/push-notifications.js";
  import lightGallery from 'lightgallery';
  import lgFullscreen from 'lightgallery/plugins/fullscreen';
  import { setupGalleryPagination, getGalleryCaption } from "./lib/gallery-pagination.js";
  import { initGalleryCaptionAutoHide } from "./lib/gallery-caption-autohide.js";
  import 'lightgallery/css/lightgallery.css';
  import 'lightgallery/css/lg-fullscreen.css';

  let targetLinkId = "";
  let status = "disconnected"; // 'disconnected', 'connecting', 'connected', 'error'
  let filesData = [];
  let fmApi = null;
  let lgInstance = null;
  let lgPagination = null;
  let viewerJustClosed = false; // set before history.back() from any viewer close
  let viewerFile = null; // { url, type, name }
  // WordCraft editor state: remember where the user clicked a document so the
  // Univer fallback can re-open it if WordCraft can't start (no WebGPU/WebGL2).
  let lastOfficeClick = null; // { file, parentPath }
  const wcUnsupportedIds = new Set(); // realIds whose WordCraft start failed
  const gcUnsupportedIds = new Set(); // realIds whose GridCraft start failed
  const dcUnsupportedIds = new Set(); // realIds whose DeckCraft start failed
  let lastPresClick = null; // { file, parentPath }

  // DeckCraft could not start (WebGPU/WebGL2 unavailable): fall back to a download.
  function handleDeckCraftFailed(e) {
    const rid = (e && e.detail && e.detail.path) || (viewerFile && viewerFile.path);
    if (rid) dcUnsupportedIds.add(rid);
    console.warn("[App] DeckCraft unavailable (WebGPU/WebGL2?), falling back to download for", rid, e && e.detail && e.detail.error);
    const last = lastPresClick;
    closeViewer(null);
    if (last && last.file) handleFileDownload(last.file);
  }

  // GridCraft could not start (WebGPU/WebGL2 unavailable): reopen through the
  // old Univer sheet viewer path. Skipped for good once per file.
  function handleGridCraftFailed(e) {
    const rid = (e && e.detail && e.detail.path) || (viewerFile && viewerFile.path);
    if (rid) gcUnsupportedIds.add(rid);
    console.warn("[App] GridCraft unavailable (WebGPU/WebGL2?), falling back to Univer for", rid, e && e.detail && e.detail.error);
    const last = lastOfficeClick;
    closeViewer(null);
    if (last && last.file) handleFileOpen(last.file, last.parentPath || "/");
  }
  let isDarkTheme = true;

  $: if (typeof window !== "undefined") {
    if (identity) window.identity = identity;
    window.deviceRegistry = deviceRegistry;
  }

  const initialAppMode = resolveAppMode();
  let appMode = initialAppMode;
  let mainApp = appMode === APP_MODE_CHAT ? "chat" : "filemanager";
  let currentApp = mainApp;
  let previousApp = mainApp;
  let filesOverlay = false;
  let chatAppMounted = false;
  let chatActiveChatId = "";
  let chatActiveView = "chat";
  $: if (currentApp === "chat") chatAppMounted = true;
  $: filesOverlay = appMode === APP_MODE_CHAT && currentApp === "filemanager";
  let settingsSection = "profile";
  let terminalPort = 22;
  let activeDeviceId = null;
  let activeWebApp = null; // { linkId, deviceName, slug, name, port, entryPath, icon }

  function openProxiedWebApp(device, appMeta = {}) {
    if (!device || !device.linkId) return;
    const slug = String(appMeta.slug || appMeta.appSlug || "").trim().toLowerCase();
    if (!slug) return;
    const existing = getProxiedApp(device.linkId, slug);
    const port = Number(appMeta.port || existing?.port || 0);
    const entryPath = appMeta.entryPath || existing?.entryPath || "/";
    const name = appMeta.name || existing?.name || slug;
    const icon = appMeta.icon || existing?.icon || slug;
    if (port > 0) {
      registerProxiedApp(device.linkId, { slug, name, port, entryPath, icon });
    }
    if (isRealFileManagerFolder(currentFolderId)) {
      lastFileManagerFolderId = currentFolderId;
    }
    if (currentApp !== "webapp") {
      previousApp = currentApp;
    }
    activeDeviceId = device.linkId;
    activeWebApp = {
      linkId: device.linkId,
      deviceName: device.name || device.linkId,
      slug,
      name,
      port,
      entryPath,
      icon
    };
    currentApp = "webapp";
    updateUrlHash(`#/app/${encodeURIComponent(device.linkId)}/${encodeURIComponent(slug)}`);
  }

  function refreshDeviceNodeApps(device, { force = false } = {}) {
    if (!device || !device.linkId || device.local || device.isAiNode || isSharedRoute) return;
    if (!device.drive || device.drive.authLevel !== "admin") return;

    discoverNodeApps(device.linkId, { force })
      .then((appItems) => {
        if (!Array.isArray(appItems) || appItems.length === 0) return;
        const prefix = device.prefix;
        if (!prefix || !window._deviceItems) return;
        const currentRoot = window._deviceItems[prefix] || [];
        const merged = mergeAppsIntoRootItems(prefix, currentRoot, appItems);
        window._deviceItems[prefix] = merged;
        if (!window._folderItems) window._folderItems = {};
        window._folderItems[prefix] = merged;
        updateRootFilesData();
        if (fmApi) {
          try {
            fmApi.exec("provide-data", { id: prefix, data: merged });
          } catch (_) {}
        }
      })
      .catch(() => {});
  }
  // Brand follows the resolved deployment domain / active gateway: "AI Assistant"
  // for the chat applet and the active gateway's brand ("Disk Direct" by default) everywhere else.
  let brandLogoBroken = false;
  let showGatewayModal = false;
  let pendingGatewayDeepLinkUrl = "";
  $: if ($gatewayConfig || isDarkTheme !== undefined) {
    brandLogoBroken = false;
  }
  $: rootName = appMode === APP_MODE_CHAT
    ? "AI Assistant"
    : ($gatewayConfig?.shortName || $gatewayConfig?.brandName || "Disk Direct");
  $: brandName = appMode === APP_MODE_CHAT
    ? "AI Assistant"
    : ($gatewayConfig?.brandName || "Disk Direct");
  $: brandLogoSrc = appMode === APP_MODE_CHAT
    ? (isDarkTheme ? "/customtheme/logo-chat.svg" : "/customtheme/logo-chat-dark.svg")
    : (brandLogoBroken
        ? (isDarkTheme ? "/img/logo.svg" : "/img/logo-dark.svg")
        : (isDarkTheme ? ($gatewayConfig?.skin?.logoDark || "/img/logo.svg") : ($gatewayConfig?.skin?.logoLight || "/img/logo-dark.svg")));
  $: if (typeof document !== "undefined" && !isSharedRoute) {
    document.title = appMode === APP_MODE_CHAT
      ? getDomainBrandTitle()
      : ($gatewayConfig?.brandName || getDomainBrandTitle());
  }

  // Device-namespaced id of the folder currently shown in the filemanager,
  // e.g. "/<devicePrefix>/Foo/Bar" (or "/" for the devices root).
  let currentFolderId = "/";
  let lastFileManagerFolderId = "/";

  function isRealFileManagerFolder(id) {
    if (!id || typeof id !== "string") return false;
    const clean = id.trim();
    if (
      clean === "/chats" ||
      clean === "/chat" ||
      clean.startsWith("/chat/") ||
      clean.startsWith("/chats/") ||
      clean === "/on-my-chat" ||
      clean.startsWith("/on-my-chat/") ||
      clean === "/creations" ||
      clean === "/generator" ||
      clean === "/memory" ||
      clean === "/knowledge" ||
      clean.startsWith("/settings") ||
      clean.startsWith("/workspace/")
    ) {
      return false;
    }
    return true;
  }
  // Real path on the device (e.g. "/home/user/project") the terminal should
  // start in. Empty = let the SSH shell start in the user's home.
  let terminalInitialPath = "";
  
  let appTitle = "";
  let appFilePath = "";
  let appContent = "";

  // ── P2P Public Sharing State ────────────────────────────────
  let isSharedRoute = typeof window !== "undefined" && window.location.pathname.startsWith("/shared/");
  // Deep links must be applied before the reactive hash writer runs; otherwise
  // the initial chat root would immediately rewrite #/files (or a folder path)
  // to #/chat.
  let initialHashPending = false;
  if (typeof window !== "undefined" && !isSharedRoute) {
    const initialHash = window.location.hash;
    initialHashPending = !!initialHash && initialHash !== "#/" && initialHash !== "#";
  }
  let sharedAddress = (() => {
    if (typeof window === "undefined") return "";
    const match = window.location.pathname.match(/^\/shared\/([^/?#]+)/);
    return (match && match[1]) ? match[1] : "";
  })();
  let resolvedShareData = null;
  let shareLoading = isSharedRoute;
  let shareError = "";
  let sharePasswordModal = false;
  let sharePasswordInput = "";
  let sharePasswordError = "";
  let showShareModal = false;
  let shareTargetItem = null;
  let shareTargetDevice = null;
  let showAddShareModal = false;
  let showConfirmRevokeModal = false;
  let revokeTargetShareId = "";
  let revokeTargetName = "";
  let showConfirmDisconnectModal = false;
  let deviceToDisconnect = null;

  // Universal folder picker state (used for adding root shares on a device)
  let showFolderPicker = false;
  let pickerDevice = null;

  // Device settings modal state
  let showDeviceSettings = false;
  let deviceSettingsDrive = null;
  let deviceSettingsName = "";
  let deviceSettingsIcon = "";
  let deviceSettingsType = "";

  // Device pairing modal state
  let showPairModal = false;
  let pairModalLinkId = "";
  let pairModalDeviceName = "";
  let pairModalOpenChatAfter = false;
  let pairModalTargetFolder = "";
  let pendingClaimDevice = null;
  let showQrScannerModal = false;
  let hasCameraSupport = false;

  let showOnboardingModal = false;

  function resetOnboardingFlags() {
    try {
      localStorage.removeItem("omd_onboarding_completed");
      const userKey = identityDid || identity?.profile?.user || identity?.profile?.name || "default";
      localStorage.removeItem("omd_onboarded_" + userKey);
      for (let i = localStorage.length - 1; i >= 0; i--) {
        const k = localStorage.key(i);
        if (k && k.startsWith("omd_onboarded_")) {
          localStorage.removeItem(k);
        }
      }
    } catch (_) {}
  }

  function restartOnboardingFlow() {
    resetOnboardingFlags();
    if (showOnboardingModal) {
      showOnboardingModal = false;
      setTimeout(() => {
        showOnboardingModal = true;
      }, 50);
    } else {
      showOnboardingModal = true;
    }
  }

  function checkTriggerOnboarding() {
    if (typeof window === "undefined" || isSharedRoute) return;
    const userKey = identityDid || identity?.profile?.user || identity?.profile?.name || "default";
    const onboardFlag = userKey !== "default"
      ? localStorage.getItem("omd_onboarded_" + userKey)
      : (localStorage.getItem("omd_onboarded_default") || localStorage.getItem("omd_onboarding_completed"));
    if (!onboardFlag) {
      showOnboardingModal = true;
    }
  }

  function openPairModal(linkId, name = "") {
    if (isSyncingHash || _isRestoringFolder) {
      console.log("[App] openPairModal blocked during hash sync or folder restore:", linkId);
      return;
    }
    pairModalLinkId = linkId || "";
    pairModalDeviceName = name || "";
    showPairModal = true;
  }

  function extractGatewayFromRawUrl(raw) {
    if (!raw || typeof raw !== "string") return "";
    const trimmed = raw.trim();
    try {
      if (/^(direct|diskdirect|onmydisk):\/\/gateway/i.test(trimmed)) {
        const qIndex = trimmed.indexOf("?");
        if (qIndex !== -1) {
          const params = new URLSearchParams(trimmed.slice(qIndex + 1));
          return params.get("url") || params.get("gateway") || "";
        }
      }
      if (trimmed.includes("gateway=")) {
        const u = new URL(trimmed, typeof window !== "undefined" ? window.location.origin : "https://disk.direct");
        return u.searchParams.get("gateway") || "";
      }
    } catch (_) {}
    return "";
  }

  function openGatewaySwitchWithUrl(targetUrl = "") {
    if (appMode === APP_MODE_CHAT) return;
    pendingGatewayDeepLinkUrl = targetUrl || "";
    showGatewayModal = false;
    setTimeout(() => {
      showGatewayModal = true;
    }, 0);
  }

  function handleQrScan(e) {
    showQrScannerModal = false;
    const { raw, linkId, name } = e.detail || {};
    const gwUrl = extractGatewayFromRawUrl(raw);
    if (gwUrl) {
      closeConnectModal();
      openGatewaySwitchWithUrl(gwUrl);
      return;
    }
    if (linkId) {
      closeConnectModal();
      openPairModal(linkId, name);
    }
  }


  function confirmRevokeShare(shareId, name) {
    revokeTargetShareId = shareId;
    revokeTargetName = name || "Item";
    showConfirmRevokeModal = false;
    setTimeout(() => {
      showConfirmRevokeModal = true;
    }, 0);
  }

  async function handleExecuteRevoke() {
    const targetId = revokeTargetShareId;
    showConfirmRevokeModal = false;
    revokeTargetShareId = "";
    revokeTargetName = "";
    if (targetId && identity) {
      await revokeShare(identity, targetId);
      await reloadSharedByMe();
    }
  }

  async function handleExecuteDisconnectDevice() {
    if (!deviceToDisconnect) return;
    const { linkId } = deviceToDisconnect;
    showConfirmDisconnectModal = false;
    deviceToDisconnect = null;
    await disconnectDevice(linkId);
  }

  // ── Identity & Auth ─────────────────────────────────────────
  let authorized = false;       // Set true on successful persistent login or ephemeral connect
  let identity = null;          // EphemeralIdentity | PersistentIdentity
  let identityType = "none";   // "none" | "ephemeral" | "persistent"
  $: mdViewerCanEdit = !isSharedRoute && identityType === "persistent";
  let identityDid = "";         // did:gun:<pub> (shown in UI)
  let identityEmail = "";       // Google email (shown in UI)
  let loginMode = "select";    // "select" | "key" | "password" | "register" | "recovery"

  // Keep <html> in sync with both active theme (dark/light) and active skin
  // ("skin-chat" Magenta/Orchid vs "skin-files" Amethyst/Duplex Stream).
  $: isChatSkin =
    !isSharedRoute &&
    (currentApp === "chat" ||
      (currentApp === "settings" && (previousApp === "chat" || (!previousApp && appMode === APP_MODE_CHAT))) ||
      ((!authorized || loginMode === "recovery") && appMode === APP_MODE_CHAT));

  $: if (typeof document !== "undefined") {
    document.documentElement.classList.toggle("omd-dark", isDarkTheme);
    document.documentElement.classList.toggle("dark-theme", isDarkTheme);
    document.documentElement.classList.toggle("light-theme", !isDarkTheme);
    document.documentElement.classList.toggle("skin-chat", isChatSkin);
    document.documentElement.classList.toggle("skin-files", !isChatSkin);
    document.documentElement.style.colorScheme = isDarkTheme ? "dark" : "light";
    const metaTheme = document.querySelector('meta[name="theme-color"]');
    if (metaTheme) {
      metaTheme.setAttribute(
        "content",
        isChatSkin
          ? (isDarkTheme ? "#0F0C12" : "#FAF9FB")
          : (isDarkTheme ? "#151821" : "#F6F7FA")
      );
    }
  }
  const initialSavedSession = !isSharedRoute ? peekSavedSessionSync() : null;
  const hasSavedSession = !!initialSavedSession;
  let loginLoading = hasSavedSession || (typeof window !== "undefined" && !isSharedRoute && Capacitor.isNativePlatform());
  let loginError = "";
  let legacyUser = "";
  let legacyPass = "";
  let regUser = "";
  let regEmail = "";
  let regPass = "";
  let regPassConfirm = "";
  $: activeLoginLangMeta = SUPPORTED_LANGUAGES.find((l) => l.code === $locale) || SUPPORTED_LANGUAGES[0];

  // SSH credential vault + API key vault follow the active identity: bound on
  // persistent login/restore, dropped on logout or ephemeral fallback.
  $: if (identity) {
    if (identity.type === "persistent" && identity.devicePair) {
      setSshIdentity(identity);
      setOmdKeyIdentity(identity);
      setMcpKeysIdentity(identity);
    } else if (identity.type && identity.type !== "persistent") {
      clearSshIdentity();
      clearOmdKeyIdentity();
      clearMcpKeysIdentity();
    }
  }
  let regAvatar = "";
  let recoveryKey = "";
  let unsubDeviceList = null;   // loadMyDevices unsubscribe handle
  let unsubSharedToMe = null;  // subscribeSharedToMe unsubscribe handle
  let unsubSharedByMe = null;   // subscribeSharedByMe unsubscribe handle
  let emptyStateLinkId = "";
  let emptyStateConnecting = false;
  let localDeviceBusy = false;
  let localDeviceError = "";
  let initialDeviceLoadDone = false;
  let showAddDevicePanel = false;
  let showRemoteDeviceInfo = false;

  function handleRegAvatarChange(e) {
    const file = e.target.files?.[0];
    if (!file) return;
    if (file.size > 1024 * 1024 * 2) {
      loginError = "Avatar image must be under 2MB";
      return;
    }
    const reader = new FileReader();
    reader.onload = () => {
      regAvatar = reader.result;
    };
    reader.readAsDataURL(file);
  }

  let isSyncingHash = false;
  let hashSyncTimeout = null;
  let _folderNavigationSeq = 0;

  function updateUrlHash(newHash, replace = false) {
    if (typeof window === "undefined" || isSharedRoute) return;
    const currentHash = window.location.hash || "#/";
    if (currentHash !== newHash) {
      isSyncingHash = true;
      if (hashSyncTimeout) clearTimeout(hashSyncTimeout);
      if (replace) {
        window.history.replaceState({ hash: newHash }, "", newHash);
      } else {
        window.history.pushState({ hash: newHash }, "", newHash);
      }
      hashSyncTimeout = setTimeout(() => {
        isSyncingHash = false;
        hashSyncTimeout = null;
      }, 80);
    }
  }

  async function applyStateFromHash() {
    if (typeof window === "undefined" || isSharedRoute) return;
    let rawHash = window.location.hash || "#/";
    if (rawHash.startsWith("#")) rawHash = rawHash.slice(1);

    const queryIndex = rawHash.indexOf("?");
    let hashPath = queryIndex !== -1 ? rawHash.slice(0, queryIndex) : rawHash;
    if (!hashPath.startsWith("/")) hashPath = "/" + hashPath;
    if (hashPath.length > 1 && hashPath.endsWith("/")) {
      hashPath = hashPath.slice(0, -1);
    }

    const mySeq = ++_folderNavigationSeq;
    isSyncingHash = true;
    initialHashPending = false;
    if (hashSyncTimeout) clearTimeout(hashSyncTimeout);

    // Dismiss active preview viewers or transient modals when navigating.
    // If a viewer was open and this navigation came from its own close
    // (history.back), skip any fm refresh: the folder did not change and the
    // selection stays on the document the user was just viewing.
    const viewerWasOpen = !!viewerFile || viewerJustClosed;
    viewerJustClosed = false;
    if (viewerFile) {
      if (viewerFile.blobUrl) {
        try { URL.revokeObjectURL(viewerFile.blobUrl); } catch (_) {}
      }
      viewerFile = null;
    }
    if (lgInstance) {
      try { lgInstance.closeGallery(); } catch (_) {}
      try { lgInstance.destroy(true); } catch (_) {}
      lgInstance = null;
    }
    if (showShareModal) showShareModal = false;
    if (showConfirmRevokeModal) showConfirmRevokeModal = false;
    if (showConfirmDisconnectModal) showConfirmDisconnectModal = false;
    if (showFolderPicker) showFolderPicker = false;
    if (showDeviceSettings) showDeviceSettings = false;
    if (showPairModal) showPairModal = false;
    if (showQrScannerModal) showQrScannerModal = false;
    if (showRemoteDeviceInfo) showRemoteDeviceInfo = false;

    // The viewer close path intentionally does nothing else here: the folder
    // listing must stay untouched (no re-fetch, no set-path) so the selection
    // remains on the document the user just viewed.
    if (viewerWasOpen) return;

    try {
      if (hashPath === "/onboard" || hashPath.startsWith("/onboard")) {
        restartOnboardingFlow();
        updateUrlHash("#/", true);
        return;
      }

      if (hashPath === "/" || hashPath === "") {
        if (showAddDevicePanel) showAddDevicePanel = false;
        if (appMode === APP_MODE_CHAT) {
          // In chat mode #/ is the chat root, not the file manager.
          if (currentApp !== "chat") currentApp = "chat";
          updateUrlHash("#/chat", true);
        } else {
          if (currentApp !== "filemanager") currentApp = "filemanager";
          currentFolderId = "/";
          if (fmApi && viewerWasOpen) {
            // Keep the folder list exactly as it was (no re-fetch, selection untouched).
          } else if (fmApi) {
            try { fmApi.exec("filter-files", { text: "" }); } catch (_) {}
            fmApi.exec("set-path", { id: "/" });
            fmApi.exec("request-data", { id: "/" });
          }
        }
      } else if (hashPath === "/files") {
        // Explicit file-manager root. In chat mode this is the files overlay;
        // in files mode it is an alias for #/.
        if (showAddDevicePanel) showAddDevicePanel = false;
        if (currentApp !== "filemanager") currentApp = "filemanager";
        currentFolderId = "/";
        if (fmApi && viewerWasOpen) {
          // Keep the current listing untouched when a viewer closes.
        } else if (fmApi) {
          try { fmApi.exec("filter-files", { text: "" }); } catch (_) {}
          fmApi.exec("set-path", { id: "/" });
          fmApi.exec("request-data", { id: "/" });
        }
      } else if (hashPath === "/connect-device") {
        if (currentApp !== "filemanager") currentApp = "filemanager";
        showAddDevicePanel = true;
      } else if (hashPath.startsWith("/workspace/")) {
        if (showAddDevicePanel) showAddDevicePanel = false;
        const parts = hashPath.slice("/workspace/".length).split("/");
        const appType = parts[0];
        const devId = parts[1] || "";
        const pathRest = "/" + parts.slice(2).join("/");
        if (appType === "terminal" || appType === "code") {
          currentApp = appType;
          activeDeviceId = devId;
          if (appType === "terminal") terminalInitialPath = pathRest;
          else appFilePath = pathRest;
        }
      } else if (hashPath.startsWith("/app/")) {
        if (showAddDevicePanel) showAddDevicePanel = false;
        const parts = hashPath.slice("/app/".length).split("/").filter(Boolean);
        const rawDev = parts[0] ? decodeURIComponent(parts[0]) : "";
        const rawSlug = parts[1] ? decodeURIComponent(parts[1]).toLowerCase() : "";
        if (rawDev && rawSlug) {
          const dev =
            deviceRegistry.get(rawDev) ||
            resolveDevice("/" + rawDev) ||
            deviceRegistry.list().find((d) => d && (d.linkId === rawDev || d.name === rawDev));
          const linkId = dev ? dev.linkId : rawDev;
          let reg = getProxiedApp(linkId, rawSlug);
          if (!reg) {
            try {
              await discoverNodeApps(linkId, { force: true });
              reg = getProxiedApp(linkId, rawSlug);
            } catch (_) {}
          }
          activeDeviceId = linkId;
          activeWebApp = {
            linkId,
            deviceName: dev?.name || linkId,
            slug: rawSlug,
            name: reg?.name || rawSlug,
            port: reg?.port || 0,
            entryPath: reg?.entryPath || "/",
            icon: reg?.icon || rawSlug
          };
          currentApp = "webapp";
        }
      } else if (hashPath.startsWith("/settings")) {
        if (showAddDevicePanel) showAddDevicePanel = false;
        currentApp = "settings";
        const parts = hashPath.slice("/settings".length).split("/").filter(Boolean);
        settingsSection = parts[0] || "profile";
      } else if (
        hashPath === "/chat" ||
        hashPath === "/chats" ||
        hashPath.startsWith("/chat/") ||
        hashPath === "/creations" ||
        hashPath === "/generator" ||
        hashPath === "/memory" ||
        hashPath === "/knowledge"
      ) {
        if (showAddDevicePanel) showAddDevicePanel = false;
        if (isRealFileManagerFolder(currentFolderId)) {
          lastFileManagerFolderId = currentFolderId;
        }
        currentApp = "chat";
        const st = $aiNodeStore;
        refreshPublicAiNode().catch(() => {});
        const ownChatDev = deviceRegistry.list().find((d) => isChatCapableDevice(d) && d.drive?.authLevel === "admin");
        const needsPublic = publicAiWanted(st) || (st.choice === AI_NODE_MODE_PUBLIC && !!st.info) || (!ownChatDev && !!st.info && !st.ack);
        if (needsPublic && !st.ack) {
          aiPublicOpenAfterAck = true;
          showAiPublicAgreement = true;
        } else {
          const target = defaultChatTarget();
          if (target && target.linkId && !activeDeviceId) {
            activeDeviceId = target.linkId;
          }
        }
        if (hashPath === "/chat/creations" || hashPath === "/creations" || hashPath === "/generator") {
          chatActiveView = "creations";
        } else if (hashPath === "/chat/memory" || hashPath === "/memory" || hashPath === "/knowledge") {
          chatActiveView = "memory";
        } else {
          chatActiveView = "chat";
          const maybeChatId = hashPath.replace(/^\/chat\/?/, "");
          if (maybeChatId && maybeChatId !== "chat" && maybeChatId !== "chats" && maybeChatId !== "creations" && maybeChatId !== "memory") {
            chatActiveChatId = maybeChatId;
          }
        }
      } else if (hashPath === "/shared-by-me" || hashPath.startsWith("/shared-by-me/")) {
        if (showAddDevicePanel) showAddDevicePanel = false;
        if (currentApp !== "filemanager") currentApp = "filemanager";
        currentFolderId = hashPath;
        if (hashPath === "/shared-by-me") {
          await reloadSharedByMe();
        } else {
          await ensureSharedFolder(hashPath);
        }
        if (mySeq !== _folderNavigationSeq) return;
        if (fmApi) {
          fmApi.exec("set-path", { id: hashPath });
        }
      } else if (hashPath === "/shared-to-me" || hashPath.startsWith("/shared-to-me/")) {
        if (showAddDevicePanel) showAddDevicePanel = false;
        if (currentApp !== "filemanager") currentApp = "filemanager";
        currentFolderId = hashPath;
        if (hashPath === "/shared-to-me") {
          await reloadSharedToMe();
        } else {
          await ensureSharedFolder(hashPath);
        }
        if (mySeq !== _folderNavigationSeq) return;
        if (fmApi) {
          fmApi.exec("set-path", { id: hashPath });
        }
      } else if (hashPath.startsWith("/shared/") || hashPath.startsWith("/share/")) {
        const addr = hashPath.replace(/^\/share[d]?\//, "").split("/")[0];
        if (addr) {
          isSharedRoute = true;
          sharedAddress = addr;
          handleOpenSharedRoute(addr);
        }
      } else {
        if (showAddDevicePanel) showAddDevicePanel = false;
        const folderId = hashPath;
        if (currentApp !== "filemanager") currentApp = "filemanager";
        currentFolderId = folderId;

        const parsed = splitDeviceId(folderId);
        if (parsed) {
          const device = resolveDevice(parsed.prefix);
          if (device && device.drive) {
            await ensureFolderAndParents(device, parsed.path);
            if (mySeq !== _folderNavigationSeq) return;
            const cached = window._folderItems?.[folderId] || window._deviceItems?.[folderId];
            if (cached && fmApi) {
              fmApi.exec("provide-data", { id: folderId, data: cached });
            }
          }
        }
        if (mySeq !== _folderNavigationSeq) return;
        if (fmApi) {
          fmApi.exec("set-path", { id: folderId });
        }
      }
    } finally {
      hashSyncTimeout = setTimeout(() => {
        if (mySeq === _folderNavigationSeq) {
          isSyncingHash = false;
          hashSyncTimeout = null;
        }
      }, 100);
    }
  }

  import { isFolderFetchingStore } from "./lib/folder-loading.js";

  let p2pReconnectingStatus = null; // null | 'reconnecting' | 'connected'
  let isRestoringP2P = false;
  let lastHeartbeat = Date.now();
  let hideToastTimer = null;
  let sleepCheckTimer = null;
  let isFolderFetching = false;
  let isGalleryLoading = false;
  let isChatLoading = false;
  let _folderFetchingCount = 0;

  function _beginFolderFetch() {
    _folderFetchingCount++;
    isFolderFetching = true;
    isFolderFetchingStore.set(true);
  }

  function _endFolderFetch() {
    _folderFetchingCount = Math.max(0, _folderFetchingCount - 1);
    if (_folderFetchingCount === 0) {
      isFolderFetching = false;
      isFolderFetchingStore.set(false);
    }
  }

  // Connect Device modal: one component serves both the manual "+" click and
  // the automatic empty-state. Auto-opens (same full modal) whenever the tree
  // has no device nodes; after a manual dismiss it stays closed until devices
  // reappear in filesData.
  let connectModalDismissed = false;
  $: hasDeviceNodes = filesData.some(f => f.isDevice || f.itemType === "device");
  $: if (hasDeviceNodes) connectModalDismissed = false;
  // Do not push the "Connect a device" modal when the public AI node is
  // available/accepted - chats work without connecting an own device.
  $: publicAiAvailable = !!publicAiDevice || (!!($aiNodeStore && $aiNodeStore.info) && !!($aiNodeStore && $aiNodeStore.ack));
  $: autoConnectModal = initialDeviceLoadDone && status !== 'connecting' && p2pReconnectingStatus !== 'reconnecting' && !hasDeviceNodes && !connectModalDismissed && !publicAiAvailable;

  function closeConnectModal() {
    showAddDevicePanel = false;
    connectModalDismissed = true;
    if (typeof window !== "undefined" && window.location.hash === "#/connect-device") {
      updateUrlHash(currentFolderId === "/" ? "#/" : "#" + currentFolderId, true);
    }
  }

  let lastWakeupTimestamp = 0;

  // Auto-reconnect WebRTC drives when mobile wakes up from sleep, network comes
  // back online, or the user clicks the "Retry" button on the empty-root state.
  async function handleAppWakeup(reason = "wakeup") {
    const now = Date.now();
    if (now - lastWakeupTimestamp < 2000) {
      return;
    }
    if (!isSharedRoute && (!authorized || identityType !== "persistent") && !loginLoading) {
      try {
        const recovered = await loadSavedSessionResilient();
        if (recovered) {
          console.log(`[App] Wakeup (${reason}): recovering persistent DID session from backup...`);
          await restoreIdentity();
        }
      } catch (recErr) {
        console.warn("[App] Wakeup session recovery warning:", recErr);
      }
    }
    if (isRestoringP2P) return;
    if (!initialDeviceLoadDone) return;
    isRestoringP2P = true;
    lastWakeupTimestamp = now;
    console.log(`[App] Phone wakeup / network change (${reason}): re-checking gateway online status...`);

    if (hideToastTimer) {
      clearTimeout(hideToastTimer);
      hideToastTimer = null;
    }

    try {
      // 1. Reconnect Gateway WebSocket first
      const gatewayChan = getSharedGatewayChannel();
      await gatewayChan.connect();

      // 2. Query gateway for online devices from registered list
      const myTargetIds = deviceRegistry.list()
        .filter(d => d && !d.local && d.linkId && d.linkId !== getOrCreateLocalLinkId())
        .map(d => d.linkId);

      let onlineDevices = [];
      if (myTargetIds.length > 0) {
        onlineDevices = await gatewayChan.getDevices(myTargetIds);
        console.log(`[App] Wakeup gateway online devices:`, onlineDevices);
      }

      const onlineLinkIds = new Set(
        (Array.isArray(onlineDevices) ? onlineDevices : [])
          .map(d => typeof d === "string" ? d : (d?.linkID || d?.linkId || d?.id))
          .filter(Boolean)
      );

      // 3. Only reconnect devices that are confirmed online by gateway
      const reconnectPromises = [];
      for (const dev of deviceRegistry.list()) {
        if (!dev || !dev.drive || dev.local || dev.linkId === getOrCreateLocalLinkId()) continue;
        if (onlineLinkIds.has(dev.linkId)) {
          if (!dev.drive.isConnected || dev.drive.pc?.iceConnectionState === "disconnected" || dev.drive.pc?.iceConnectionState === "failed" || dev.drive.dc?.readyState !== "open") {
            console.log("[App] Queuing auto-reconnect for confirmed online device:", dev.name, dev.linkId);
            reconnectPromises.push(dev.drive.connect());
          }
        }
      }

      if (isSharedRoute && window.__omdSharedDrive) {
        const sDrive = window.__omdSharedDrive;
        if (!sDrive.isConnected || sDrive.pc?.iceConnectionState === "disconnected" || sDrive.pc?.iceConnectionState === "failed" || sDrive.dc?.readyState !== "open") {
          console.log("[App] Queuing auto-reconnect for P2P shared drive");
          reconnectPromises.push(sDrive.connect());
        }
      }

      if (reconnectPromises.length > 0) {
        p2pReconnectingStatus = 'reconnecting';
        const results = await Promise.all(reconnectPromises);
        const anySucceeded = results.some(r => r === true);
        console.log(`[App] Reconnect results: ${results} — anySucceeded: ${anySucceeded}`);

        if (anySucceeded) {
          p2pReconnectingStatus = 'connected';
          window.dispatchEvent(new CustomEvent("omd-p2p-reconnected"));
          if (hideToastTimer) clearTimeout(hideToastTimer);
          hideToastTimer = setTimeout(() => {
            p2pReconnectingStatus = null;
            hideToastTimer = null;
          }, 1800);

          if (isSharedRoute && resolvedShareData) {
            shareError = "";
            shareLoading = false;
            sharePasswordModal = false;
            mainApp = "singleview";
            currentApp = "singleview";
          } else if (currentFolderId && currentFolderId !== "/") {
            const parsed = splitDeviceId(currentFolderId);
            if (parsed) {
              const dev = resolveDevice(parsed.prefix);
              if (dev && dev.drive && dev.drive.isConnected) {
                restoreActiveFolder(dev).catch(() => {});
              }
            }
          }
        } else {
          p2pReconnectingStatus = null;
        }
      } else {
        p2pReconnectingStatus = null;
      }
    } catch (err) {
      console.error("[App] WebRTC auto-reconnection error:", err);
      p2pReconnectingStatus = null;
    } finally {
      isRestoringP2P = false;
      checkForOtaUpdate();
      cameraSyncManager?._runAutoSyncCheck();
    }
  }

  onDestroy(() => {
    if (typeof window !== "undefined") {
      window.removeEventListener("popstate", applyStateFromHash);
      window.removeEventListener("hashchange", applyStateFromHash);
    }
    if (sleepCheckTimer) clearInterval(sleepCheckTimer);
    if (hideToastTimer) clearTimeout(hideToastTimer);
    if (lgInstance) {
      lgInstance.destroy(true);
    }
    if (identity) identity.destroy();
    if (unsubDeviceList) { unsubDeviceList(); unsubDeviceList = null; }
    if (unsubSharedByMe) { unsubSharedByMe(); unsubSharedByMe = null; }
    try { stopClientDeviceServer(); } catch (_) {}
  });

  onMount(() => {
    initAppProxyServiceWorkerBridge();
    initGalleryCaptionAutoHide();
    applyGatewaySkinToDom($gatewayConfig);

    // Handle gateway switch event (preserves DID, re-points GunDB & refreshes device list)
    const onGatewayChanged = (ev) => {
      const nextGw = ev?.detail;
      applyGatewaySkinToDom(nextGw || $gatewayConfig);
      if (identity && typeof identity.reconnectRelay === "function") {
        try {
          identity.reconnectRelay();
          publishAllConnectedDevices();
          startDeviceList();
        } catch (err) {
          console.warn("[App] Gateway switch Gun reconnect warning:", err);
        }
      }
    };
    window.addEventListener("omd-gateway-changed", onGatewayChanged);

    // Handle native/web deep links: direct://gateway?url=... or ?gateway=...
    const onNativeDeepLink = (ev) => {
      const rawUrl = ev?.detail?.url || "";
      const gwUrl = extractGatewayFromRawUrl(rawUrl);
      if (gwUrl) {
        openGatewaySwitchWithUrl(gwUrl);
      }
    };
    window.addEventListener("omd-deeplink", onNativeDeepLink);
    if (window.__pendingDeepLinkUrl) {
      const pending = window.__pendingDeepLinkUrl;
      window.__pendingDeepLinkUrl = "";
      const gwUrl = extractGatewayFromRawUrl(pending);
      if (gwUrl) openGatewaySwitchWithUrl(gwUrl);
    }
    try {
      const initGwParam = new URLSearchParams(window.location.search).get("gateway");
      if (initGwParam) {
        openGatewaySwitchWithUrl(initGwParam);
      }
    } catch (_) {}

    // Mark browser workspace as entered so future visits to `/` open the app directly
    // instead of the `/about` landing page, and strip `?launch=1` cleanly from the URL.
    if (!Capacitor.isNativePlatform() && !window.location.pathname.startsWith("/shared/")) {
      try {
        document.cookie = "omd_entered=1; path=/; max-age=31536000; SameSite=Lax";
        localStorage.setItem("omd_entered", "1");
        const cleanParams = new URLSearchParams(window.location.search);
        if (cleanParams.has("launch")) {
          cleanParams.delete("launch");
          const nextSearch = cleanParams.toString();
          const nextUrl = window.location.pathname + (nextSearch ? `?${nextSearch}` : "") + window.location.hash;
          window.history.replaceState({}, "", nextUrl);
        }
      } catch (_) {}
    }

    // Check for /shared/<address> public P2P route
    if (window.location.pathname.startsWith("/shared/")) {
      const match = window.location.pathname.match(/^\/shared\/([^/?#]+)/);
      if (match && match[1]) {
        isSharedRoute = true;
        sharedAddress = match[1];
        handleOpenSharedRoute(sharedAddress);
      }
    }

    // Check for /onboard route (path or hash) to reset flags and relaunch wizard
    if (window.location.pathname === "/onboard" || window.location.pathname.startsWith("/onboard/")) {
      restartOnboardingFlow();
      try {
        window.history.replaceState({}, "", window.location.hash && window.location.hash !== "#/onboard" ? window.location.hash : "/#/");
      } catch (_) {}
    }

    if (!isSharedRoute) {
      window.addEventListener("popstate", applyStateFromHash);
      window.addEventListener("hashchange", applyStateFromHash);
      if (window.location.hash && window.location.hash !== "#/" && window.location.hash !== "#") {
        setTimeout(applyStateFromHash, 100);
      } else {
        updateUrlHash(appMode === APP_MODE_CHAT ? "#/chat" : "#/", true);
      }
    }

    const urlParams = new URLSearchParams(window.location.search);
    const claimLink = urlParams.get('claimLink') || urlParams.get('claim');
    const claimName = urlParams.get('name') || '';
    if (claimLink) {
      pendingClaimDevice = { linkId: claimLink, name: claimName };
    }

    // Public AI node: hydrate persisted choice/ack, then discover the node
    hydrateAiNodeState();
    refreshPublicAiNode();
    setInterval(() => {
      refreshPublicAiNode(true).catch(() => {});
    }, 60000);

    const qsLinkId = urlParams.get('linkId');
    if (qsLinkId) {
      targetLinkId = qsLinkId;
      addDevice();
    }

    // React to device online/offline events pushed over the WebSocket
    const gatewayChan = getSharedGatewayChannel();
    gatewayChan.onDevicesFetched((devices) => {
      console.log("[App] WebSocket event - active devices on gateway:", devices);
      if (Array.isArray(devices)) {
        const localId = getOrCreateLocalLinkId();
        for (const dev of devices) {
          const candidateIds = typeof dev === "string"
            ? [dev]
            : [dev?.linkID, dev?.linkId, dev?.id, dev?.peerID, dev?.user].filter(Boolean);
          if (candidateIds.length === 0 || candidateIds.includes(localId)) continue;

          // Strictly connect ONLY if this device is in our DID list or shared link target
          const registered = candidateIds.map(id => deviceRegistry.get(id)).find(Boolean);
          if (registered && !registered.local) {
            if (typeof dev === "object" && dev) {
              if (dev.peerID) registered.peerId = dev.peerID;
              if (dev.linkID || dev.user) registered.shortId = dev.linkID || dev.user;
            }
            console.log("[App] Gateway online device matches our DID device! Connecting:", registered.linkId);
            connectDevice(registered.linkId);
          } else {
            console.log("[App] Ignoring online gateway device not in our DID list or local:", candidateIds[0]);
          }
        }
      }
    });
    gatewayChan.onLinkConnected((linkId) => {
      console.log("[App] WebSocket event - link connected:", linkId);
      if (!linkId || linkId === getOrCreateLocalLinkId()) return;
      let dev = deviceRegistry.get(linkId);
      if (!dev && identity && identity.devices && (identity.devices.has?.(linkId) || identity.devices.get?.(linkId))) {
        const saved = identity.devices.get?.(linkId);
        registerDevicePlaceholder(linkId, saved?.name || "");
        dev = deviceRegistry.get(linkId);
      }
      if (dev && !dev.local && (!dev.drive || !dev.drive.isConnected)) {
        console.log("[App] Newly connected device is in our DID list! Connecting:", dev.linkId);
        connectDevice(dev.linkId);
      }
    });
    gatewayChan.onLinkDisconnected((linkId) => {
      console.log("[App] WebSocket event - link disconnected:", linkId);
      if (!linkId || linkId === getOrCreateLocalLinkId()) return;
      const dev = deviceRegistry.get(linkId);
      if (dev && !dev.local) {
        dev.status = "disconnected";
        if (dev.drive) {
          dev.drive.disconnect();
        }
        deviceNodes = deviceNodes.filter(d => (d.linkId !== dev.linkId && d.id !== dev.prefix) || d.local);
        updateRootFilesData();
      }
    });
    gatewayChan.onStateChange((connected) => {
      console.log("[App] Gateway WebSocket state changed, connected:", connected);
      if (connected) {
        handleAppWakeup("gateway-ws-reconnected");
        refreshOnlineStatusForRegisteredDevices();
      }
    });

    // Unconditionally connect the gateway signaling WebSocket on startup for all users
    // (including guests / ephemeral sessions in private mode), so ICE candidates, tunnel-open,
    // and P2P WebRTC signaling can be received without creating a local web device.
    gatewayChan.connect().catch((err) => {
      console.warn("[App] Gateway WebSocket initial connect error:", err);
    });


    // Keep screen and network connections alive while in foreground
    if (typeof navigator !== "undefined" && "wakeLock" in navigator) {
      const acquireWakeLock = async () => {
        try {
          if (document.visibilityState === "visible") {
            await navigator.wakeLock.request("screen");
          }
        } catch (_) {}
      };
      acquireWakeLock();
      document.addEventListener("visibilitychange", acquireWakeLock);
    }

    // Try restoring a persistent identity session & local device
    if (!isSharedRoute) {
      (async () => {
        await restoreIdentity();
        checkForOtaUpdate();
        // restoreIdentity() already restores the local device for persistent sessions;
        // only invoke here when running in ephemeral mode so ClientDeviceServer isn't restarted twice.
        if (identityType !== "persistent") {
          await restoreLocalDeviceIfShares();
        }
      })();
    }

    if (isNativeStorageAvailable()) {
      onStorageChanged(async (info) => {
        console.log("[App] Native storage hotplug event:", info);
        try {
          const localDevice = makeLocalDevice();
          const existing = deviceRegistry.get(localDevice.linkId);
          const device = (existing && existing.local) ? existing : localDevice;
          await registerLocalDeviceNode(device);
          if (device && activeDeviceId === device.linkId) {
            await ensureFolderAndParents(device, "/");
          }
        } catch (e) {
          console.warn("[App] storage hotplug refresh error:", e);
        }
      });
    }

    const mediaQuery = window.matchMedia('(prefers-color-scheme: dark)');
    isDarkTheme = mediaQuery.matches;
    const themeListener = e => { isDarkTheme = e.matches; };
    if (mediaQuery.addEventListener) {
      mediaQuery.addEventListener('change', themeListener);
    } else {
      mediaQuery.addListener(themeListener);
    }
    
    if (window.location.hostname.startsWith("terminal.")) {
      // Terminal host keeps its own dedicated root; app mode is irrelevant there.
      mainApp = "terminal";
      currentApp = "terminal";
    }

    window.__omdOpenSettings = (sec = 'profile') => {
      if (currentApp !== 'settings') previousApp = currentApp;
      settingsSection = sec;
      currentApp = 'settings';
      updateUrlHash('#/settings/' + sec);
    };
    window.__omdOpenApplet = (view = 'chat') => {
      openChatsApp(view);
    };
    window.__omdLogout = () => { logoutIdentity(); };

    window.addEventListener("omd-open-settings", (e) => {
      if (currentApp !== 'settings') previousApp = currentApp;
      const sec = (e && e.detail && e.detail.section) || "profile";
      settingsSection = sec;
      currentApp = "settings";
      updateUrlHash("#/settings/" + sec);
    });

    window.addEventListener("omd-open-applet", (e) => {
      const view = (e && e.detail && e.detail.view) || "chat";
      openChatsApp(view);
    });

    window.addEventListener("omd-open-file", (e) => {
      handleOpenFileEvent(e.detail);
    });

    const flushIdentityOnBackground = () => {
      if (identity && identity.type === "persistent" && typeof identity.flushSessionBackup === "function") {
        identity.flushSessionBackup();
      }
    };

    document.addEventListener("visibilitychange", () => {
      if (document.visibilityState === "visible") {
        handleAppWakeup("visibilitychange");
      } else if (document.visibilityState === "hidden") {
        flushIdentityOnBackground();
      }
    });
    window.addEventListener("pagehide", flushIdentityOnBackground);
    window.addEventListener("online", () => handleAppWakeup("online"));
    window.addEventListener("pageshow", () => handleAppWakeup("pageshow"));

    // Sleep Detection: If JS execution freezes for > 4s (mobile phone sleep), trigger restoration
    lastHeartbeat = Date.now();
    sleepCheckTimer = setInterval(() => {
      const now = Date.now();
      const isMobile = typeof navigator !== "undefined" && /Android|iPhone|iPad|iPod/i.test(navigator.userAgent);
      if (now - lastHeartbeat > 4000) {
        if (isMobile && typeof document !== "undefined" && document.visibilityState === "visible") {
          console.warn("[App] Phone wakeup detected via timer gap (>4s)! Triggering P2P restoration...");
          handleAppWakeup("sleep-timer");
        }
      }
      lastHeartbeat = now;
    }, 2000);

    // Periodic OTA check every 2 minutes
    setInterval(() => {
      checkForOtaUpdate();
    }, 120000);

    if (typeof navigator !== 'undefined' && navigator.mediaDevices && typeof navigator.mediaDevices.getUserMedia === 'function') {
      hasCameraSupport = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent) ||
                         (typeof window !== 'undefined' && window.innerWidth <= 768 && 'ontouchstart' in window);
    }

    if (typeof navigator !== 'undefined' && 'serviceWorker' in navigator) {
      ensureServiceWorkerControl();
      navigator.serviceWorker.addEventListener('message', (ev) => {
        const msg = ev && ev.data;
        if (!msg) return;
        if (msg.type === 'omd-push-click') {
          handlePushActionDispatch(msg.action || 'open', msg.payload || {});
        }
      });
    }

    // Native Android / iOS Push Notification listeners (Capacitor)
    initNativePushListeners(
      (action, payload) => handlePushActionDispatch(action || 'open', payload || {}),
      (payload) => {
        const msg = [payload?.title, payload?.body].filter(Boolean).join(' — ');
        if (msg) showPushToast(msg);
      }
    );

    // Cold-start deep link from tapping a Rich Push Notification or its action button
    try {
      const urlObj = new URL(window.location.href);
      const pushAct = urlObj.searchParams.get('omd_push_action');
      const widgetUrl = urlObj.searchParams.get('widget_url') || '';
      const actionUrl = urlObj.searchParams.get('action_url') || '';
      const itemId = urlObj.searchParams.get('item_id') || '';
      const actToken = urlObj.searchParams.get('token') || '';
      const nodeLinkIdHint = urlObj.searchParams.get('node_link_id') || '';
      const nodeNameHint = urlObj.searchParams.get('node_name') || '';
      const storageHint = urlObj.searchParams.get('storage') || '';
      const fileHint = urlObj.searchParams.get('file') || '';
      if (pushAct && (widgetUrl || actionUrl)) {
        urlObj.searchParams.delete('omd_push_action');
        urlObj.searchParams.delete('widget_url');
        urlObj.searchParams.delete('action_url');
        urlObj.searchParams.delete('item_id');
        urlObj.searchParams.delete('token');
        urlObj.searchParams.delete('node_link_id');
        urlObj.searchParams.delete('node_name');
        urlObj.searchParams.delete('storage');
        urlObj.searchParams.delete('file');
        window.history.replaceState({}, '', urlObj.pathname + (urlObj.search ? urlObj.search : '') + urlObj.hash);
        handlePushActionDispatch(pushAct, {
          data: {
            kind: 'node_widget',
            widget_url: widgetUrl,
            action_url: actionUrl,
            item_id: itemId,
            action_token: actToken,
            node_link_id: nodeLinkIdHint,
            node_name: nodeNameHint,
            default_storage: storageHint,
            filename: fileHint
          }
        });
      }
    } catch (_) {}

    // Android Share Target receiver ("Отправить На мой диск")
    window.__handleAndroidShare = async (sharedFiles) => {
      if (!sharedFiles || !sharedFiles.length) return;
      console.log("[Android Share] Received shared files:", sharedFiles.length);
      const targetDevice = activeDevice || (filesData.find(f => f.isDevice && f.drive?.isConnected));
      if (!targetDevice || !targetDevice.drive) {
        alert("Please connect to a device to upload shared files.");
        return;
      }
      isFolderLoading = true;
      downloadingFile = "Saving shared files...";
      try {
        for (const file of sharedFiles) {
          const fileName = file.name || "shared_file";
          const dataBase64 = file.data || "";
          downloadingFile = `Uploading ${fileName}...`;
          await targetDevice.drive.writeFileChunked("/" + fileName, dataBase64);
        }
        await reloadDrive(targetDevice.linkId);
        alert(`Saved ${sharedFiles.length} file(s) to ${targetDevice.name || 'device'}`);
      } catch (err) {
        console.error("[Android Share] Upload error:", err);
        alert("Failed to save shared file: " + err.message);
      } finally {
        isFolderLoading = false;
        downloadingFile = null;
      }
    };

    if (window.__pendingAndroidShare) {
      const pending = window.__pendingAndroidShare;
      window.__pendingAndroidShare = null;
      setTimeout(() => window.__handleAndroidShare(pending), 1200);
    }
  });

  let currentProfileData = { name: "", user: "", email: "", image: "", studio: false };
  let unsubProfile = null;

  function syncIdentityProfile() {
    if (unsubProfile) { unsubProfile(); unsubProfile = null; }
    if (identity && identity.type === "persistent" && identity.subscribeProfile) {
      unsubProfile = identity.subscribeProfile((p) => {
        currentProfileData = { ...p };
        if (p?.locale && p.locale !== $locale) {
          setLocale(p.locale);
        }
        if (p?.omd_key && !getStoredOmdKey()) {
          try {
            storeOmdKey(p.omd_key);
          } catch (e) {
            console.warn("[App] storeOmdKey from profile sync error:", e);
          }
        }
        identity = identity;
      });
    }
  }

  $: {
    const isEph = identityType === "ephemeral";
    setProfile({
      authorized,
      identityType,
      isEphemeral: isEph,
      isPersistent: identityType === "persistent",
      did: identityDid || (identity?.did) || "",
      name: currentProfileData.name || identity?.profile?.name || identity?.name || (identityEmail ? identityEmail.split('@')[0] : (isEph ? "demo" : "Disk Direct User")),
      user: currentProfileData.user || identity?.profile?.user || (identityEmail ? identityEmail.split('@')[0] : (identityDid ? identityDid.slice(0, 10) : (isEph ? "demo" : ""))),
      email: currentProfileData.email || identity?.profile?.email || identityEmail || identity?.email || "",
      image: currentProfileData.image || identity?.profile?.image || identity?.image || identity?.picture || "",
      defaultStorage: (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") || currentProfileData.defaultStorage || identity?.profile?.defaultStorage || DEFAULT_STORAGE_PATH,
      online: true,
      deviceCount: filesData.filter(f => f.isDevice).length,
      omd_key: getStoredOmdKey() || "",
      studio: Boolean(currentProfileData.studio || identity?.profile?.studio),
    });
  }

  $: {
    if (typeof window !== "undefined" && !isSyncingHash && !initialHashPending) {
      if (showAddDevicePanel) {
        updateUrlHash("#/connect-device");
      } else if (currentApp === "filemanager") {
        if (!isRealFileManagerFolder(currentFolderId)) {
          currentFolderId = lastFileManagerFolderId || "/";
        }
        const isRootFolder = !currentFolderId || currentFolderId === "/";
        const targetHash = isRootFolder
          ? (appMode === APP_MODE_CHAT ? "#/files" : "#/")
          : "#" + (currentFolderId.startsWith("/") ? currentFolderId : "/" + currentFolderId);
        updateUrlHash(targetHash);
      } else if (currentApp === "terminal" || currentApp === "code") {
        const path = currentApp === "terminal" ? terminalInitialPath : appFilePath;
        const targetHash = "#/workspace/" + currentApp + "/" + (activeDeviceId || "") + (path ? (path.startsWith("/") ? path : "/" + path) : "/");
        updateUrlHash(targetHash);
      } else if (currentApp === "settings") {
        updateUrlHash("#/settings/" + (settingsSection || "profile"));
      } else if (currentApp === "chat") {
        const viewHash =
          chatActiveView === "creations" ? "#/chat/creations"
          : chatActiveView === "memory" ? "#/chat/memory"
          : (chatActiveChatId ? `#/chat/${chatActiveChatId}` : "#/chat");
        updateUrlHash(viewHash);
      }
    }
  }

  // P2P thumbnails go through the service worker. A hard reload (Ctrl+Shift+R)
  // or a stopped/updated SW loads the page WITHOUT SW control and clients.claim()
  // does not re-run (it lives in activate). Detect that case and ask the SW to
  // claim this page explicitly, then retry any /p2p/ images that failed earlier.
  let p2pRetried = new WeakSet();

  function retryP2pImages() {
    if (!document.images || !navigator.serviceWorker.controller) return;
    const targets = [...document.images].filter(img => {
      const src = img.currentSrc || img.src || "";
      return src.includes("/p2p/") && (!img.complete || img.naturalWidth === 0);
    });
    for (const img of targets) {
      if (p2pRetried.has(img)) continue;
      p2pRetried.add(img);
      const u = new URL(img.currentSrc || img.src);
      u.searchParams.set("r", Date.now());
      img.src = u.toString();
    }
  }

  async function ensureServiceWorkerControl() {
    try {
      const reg = await navigator.serviceWorker.register("/service-worker.js", { scope: "/" });
      console.log("[SW] Registered with scope:", reg.scope);
      try { await reg.update(); } catch (e) {}
      await navigator.serviceWorker.ready;

      // Images that already errored (403 direct, before SW control) get retried
      // through the SW once it is in charge.
      document.addEventListener("error", (e) => {
        const img = e.target;
        if (!img || img.tagName !== "IMG") return;
        const src = img.currentSrc || img.src || "";
        if (!src.includes("/p2p/")) return;
        if (!navigator.serviceWorker.controller || p2pRetried.has(img)) return;
        p2pRetried.add(img);
        const u = new URL(src);
        u.searchParams.set("r", Date.now());
        img.src = u.toString();
      }, true);

      if (!navigator.serviceWorker.controller) {
        await new Promise((resolve) => {
          const onChange = () => {
            navigator.serviceWorker.removeEventListener("controllerchange", onChange);
            resolve();
          };
          navigator.serviceWorker.addEventListener("controllerchange", onChange);
          if (reg.active) reg.active.postMessage({ type: "claim" });
          setTimeout(resolve, 3000);
        });
        if (navigator.serviceWorker.controller) {
          console.log("[SW] Control acquired after claim");
          retryP2pImages();
        } else if (location.hostname !== "localhost" && location.hostname !== "127.0.0.1") {
          // Last resort: a normal reload is intercepted by the now-running SW.
          console.log("[SW] Claim did not take effect, reloading to acquire control");
          location.reload();
        }
      } else {
        retryP2pImages();
      }
    } catch (err) {
      console.error("[SW] Registration failed:", err);
    }
  }

  // ── Identity functions ────────────────────────────────────────

  let isRestoringIdentity = false;

  function applyHydratedPersistentIdentity(inst) {
    if (!inst) return;
    identity = inst;
    identityType = "persistent";
    identityDid = inst.did || "";
    identityEmail = inst.profile?.email || inst.email || "";
    authorized = true;
    loginLoading = false;
    setSshIdentity(inst);
    setOmdKeyIdentity(inst);
    if (inst?.profile?.locale && inst.profile.locale !== $locale) {
      setLocale(inst.profile.locale);
    }
    if (inst?.profile?.omd_key && !getStoredOmdKey()) {
      try {
        storeOmdKey(inst.profile.omd_key);
      } catch (err) {
        console.warn("[App] storeOmdKey notice:", err);
      }
    }
    syncIdentityProfile();
    identity = inst;
  }

  async function restoreIdentity() {
    if (isRestoringIdentity) return;
    isRestoringIdentity = true;
    try {
      const saved = await loadSavedSessionResilient();
      if (!saved) {
        loginLoading = false;
        identity = new EphemeralIdentity();
        await identity.init();
        identityType = "ephemeral";
        return;
      }

      loginLoading = true;
      let hydrated = false;
      const persistent = new PersistentIdentity();
      identity = persistent;

      const result = await persistent.init({
        onHydrated: (inst) => {
          hydrated = true;
          applyHydratedPersistentIdentity(inst);
          // Start local shares & cached device list immediately without waiting for network auth
          try {
            publishAllConnectedDevices();
            restoreLocalDeviceIfShares().catch((err) =>
              console.warn("[identity] early restoreLocalDeviceIfShares notice:", err)
            );
            startDeviceList();
            if (!pendingClaimDevice) {
              checkTriggerOnboarding();
            }
          } catch (earlyErr) {
            console.warn("[identity] early post-hydration setup warning:", earlyErr);
          }
        },
        onRevoked: () => {
          logoutIdentity();
        },
      });

      if (result) {
        if (!hydrated) {
          applyHydratedPersistentIdentity(persistent);
        } else {
          // Refresh email/profile fields if hub check updated anything
          identityDid = persistent.did || identityDid;
          identityEmail = persistent.profile?.email || persistent.email || identityEmail;
          identity = persistent;
        }
        try {
          publishAllConnectedDevices();
          if (!hydrated) {
            await restoreLocalDeviceIfShares();
            startDeviceList();
            if (!pendingClaimDevice) {
              checkTriggerOnboarding();
            }
          }
          if (pendingClaimDevice) {
            setTimeout(() => {
              openPairModal(pendingClaimDevice.linkId, pendingClaimDevice.name);
              pendingClaimDevice = null;
            }, 300);
          }
        } catch (postErr) {
          console.warn("[identity] post-restore setup warning:", postErr);
        }
      } else if (identity === persistent && !hydrated) {
        // Saved session explicitly invalid (e.g. 410 deleted before hydration) → fall back to ephemeral
        identity = new EphemeralIdentity();
        await identity.init();
        identityType = "ephemeral";
      }
    } catch (e) {
      console.error("[identity] restore failed:", e);
      // Never downgrade an already-hydrated persistent session to ephemeral on transient network/init errors
      if (!identity || identity.type !== "persistent" || (!identity.mainPub && !identity.devicePair)) {
        identity = new EphemeralIdentity();
        await identity.init();
        identityType = "ephemeral";
      } else {
        authorized = true;
        identityType = "persistent";
      }
    } finally {
      loginLoading = false;
      isRestoringIdentity = false;
    }
  }

  function loadGoogleGsi() {
    if (window.google?.accounts?.id) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const existing = document.querySelector('script[src="https://accounts.google.com/gsi/client"]');
      if (existing) {
        existing.addEventListener('load', () => resolve());
        existing.addEventListener('error', (e) => reject(e));
        if (window.google?.accounts?.id) resolve();
        return;
      }
      const s = document.createElement("script");
      s.src = "https://accounts.google.com/gsi/client";
      s.async = true;
      s.defer = true;
      s.onload = () => resolve();
      s.onerror = (e) => reject(e);
      document.head.appendChild(s);
    });
  }

  function publishAllConnectedDevices() {
    if (!identity || identity.type !== "persistent" || !identity.gun) return;
    try {
      const devs = deviceRegistry.list();
      for (const dev of devs) {
        if (dev.status === "connected" || dev.local) {
          const devType = detectDeviceType(dev.name, dev.deviceType || dev.type);
          const caps = Array.isArray(dev.capabilities) && dev.capabilities.length > 0
            ? dev.capabilities
            : ["files"];
          const isNativeNode = caps.includes("chat") || caps.includes("terminal") || devType === "embedded";
          // Only publish our own local web/mobile device or paired C++ nodes —
          // never overwrite remote browser/mobile clients' metadata in GunDB.
          if (!dev.local && !isNativeNode) continue;
          identity.publishDevice(
            dev.linkId,
            dev.name || dev.linkId,
            devType,
            caps,
            { isLocal: !!dev.local }
          );
        }
      }
    } catch (e) {
      console.warn("[identity] publishAllConnectedDevices error:", e);
    }
  }

  async function handleGoogleCredential(resp) {
    if (!resp || !resp.credential) return;
    loginLoading = true;
    loginError = "";
    try {
      identity = new PersistentIdentity();
      await identity.googleLogin(resp.credential);
      identityType = "persistent";
      identityDid = identity.did || "";
      identityEmail = identity.email || "";
      authorized = true;
      setSshIdentity(identity);
      setOmdKeyIdentity(identity);
      if (identity?.profile?.omd_key) {
        try {
          storeOmdKey(identity.profile.omd_key);
        } catch (err) {
          console.warn("[App] storeOmdKey notice:", err);
        }
      }
      syncIdentityProfile();
      identity = identity;
      try {
        publishAllConnectedDevices();
        await restoreLocalDeviceIfShares();
        startDeviceList();
        checkTriggerOnboarding();
        if (typeof identity.flushSessionBackup === "function") {
          await identity.flushSessionBackup();
        }
      } catch (postLoginErr) {
        console.warn("[identity] post-google-login setup warning:", postLoginErr);
      }
    } catch (e) {
      console.error("[identity] google login error:", e);
      loginError = e.message || "Google sign-in failed";
    } finally {
      loginLoading = false;
    }
  }

  async function handleMobileGoogleSignIn() {
    loginLoading = true;
    loginError = "";
    try {
      const idToken = await signInWithGoogleMobile();
      await handleGoogleCredential({ credential: idToken });
    } catch (e) {
      if (!isGoogleSignInCanceledError(e)) {
        console.error("[identity] mobile google login error:", e);
        loginError = e?.message || "Google sign-in failed";
      }
      loginLoading = false;
    }
  }

  let gsiInitializedCallback = null;
  function googleButtonAction(node) {
    if (Capacitor.isNativePlatform()) {
      return { destroy() {} };
    }
    let active = true;

    loadGoogleGsi().then(() => {
      if (!active || !window.google?.accounts?.id) return;
      if (gsiInitializedCallback !== handleGoogleCredential) {
        window.google.accounts.id.initialize({
          client_id: "959849209352-vbi0vfimrqj904598felj8ieeb6to8s2.apps.googleusercontent.com",
          callback: handleGoogleCredential,
          auto_select: false,
        });
        gsiInitializedCallback = handleGoogleCredential;
      }

      const width = Math.min(Math.max(node.offsetWidth || 320, 200), 400);
      window.google.accounts.id.renderButton(node, {
        type: "standard",
        theme: isDarkTheme ? "filled_black" : "outline",
        size: "large",
        width: width,
        shape: "rectangular",
      });
    }).catch(err => {
      console.warn("[GSI] script load failed:", err);
    });

    return {
      destroy() {
        active = false;
      }
    };
  }

  let appleSdkLoaded = null;
  function loadAppleSdk() {
    if (appleSdkLoaded) return appleSdkLoaded;
    appleSdkLoaded = new Promise((resolve, reject) => {
      if (window.AppleID) return resolve(window.AppleID);
      const script = document.createElement("script");
      script.src = "https://appleid.cdn-apple.com/appleauth/static/jsapi/appleid/1/en_US/appleid.auth.js";
      script.async = true;
      script.onload = () => resolve(window.AppleID);
      script.onerror = () => {
        appleSdkLoaded = null;
        reject(new Error("Failed to load Apple Sign In SDK"));
      };
      document.head.appendChild(script);
    });
    return appleSdkLoaded;
  }

  async function handleAppleSignIn() {
    loginLoading = true;
    loginError = "";
    try {
      let idToken = "";
      let user = null;

      if (Capacitor.isNativePlatform() && Capacitor.getPlatform() === 'ios') {
        const { SignInWithApple } = await import("@capacitor-community/apple-sign-in");
        const res = await SignInWithApple.authorize({
          clientId: "net.onmydisk.app",
          redirectURI: getGatewayOauthBridgeOrigin(),
          scopes: "email name",
        });
        if (!res?.response?.identityToken) {
          throw new Error("No identityToken returned from Apple");
        }
        idToken = res.response.identityToken;
        if (res.response.givenName || res.response.familyName) {
          user = {
            name: {
              givenName: res.response.givenName || "",
              familyName: res.response.familyName || "",
            },
            email: res.response.email || "",
          };
        }
      } else if (Capacitor.isNativePlatform()) {
        const appleRes = await signInWithAppleMobile();
        idToken = appleRes?.idToken || "";
        user = appleRes?.user || null;
        if (!idToken) {
          throw new Error("No id_token returned from Apple");
        }
      } else {
        await loadAppleSdk();
        if (!window.AppleID) throw new Error("Apple Sign In SDK not available");

        const redirectURI = (window.location.hostname === 'localhost' || window.location.hostname === '127.0.0.1')
          ? "https://disk.direct"
          : window.location.origin;

        window.AppleID.auth.init({
          clientId: "net.onmydisk.app.service",
          scope: "name email",
          redirectURI: redirectURI,
          usePopup: true,
        });

        const res = await window.AppleID.auth.signIn();
        if (!res?.authorization?.id_token) {
          throw new Error("No id_token returned from Apple");
        }
        idToken = res.authorization.id_token;
        if (res.user) {
          user = res.user;
        }
      }

      identity = new PersistentIdentity();
      await identity.appleLogin(idToken, user);
      identityType = "persistent";
      identityDid = identity.did || "";
      identityEmail = identity.email || "";
      authorized = true;
      setSshIdentity(identity);
      setOmdKeyIdentity(identity);
      if (identity?.profile?.omd_key) {
        try {
          storeOmdKey(identity.profile.omd_key);
        } catch (err) {
          console.warn("[App] storeOmdKey notice:", err);
        }
      }
      syncIdentityProfile();
      identity = identity;
      try {
        publishAllConnectedDevices();
        await restoreLocalDeviceIfShares();
        startDeviceList();
        checkTriggerOnboarding();
        if (typeof identity.flushSessionBackup === "function") {
          await identity.flushSessionBackup();
        }
      } catch (postLoginErr) {
        console.warn("[identity] post-google-login setup warning:", postLoginErr);
      }
    } catch (e) {
      console.error("[identity] apple login error:", e);
      if (e?.error === 'popup_closed_by_user' || e?.message?.includes('canceled') || e?.message?.includes('cancelled')) {
        // User closed modal
      } else {
        loginError = e.message || "Apple sign-in failed";
      }
    } finally {
      loginLoading = false;
    }
  }

  let loginKeyfileInput = null;
  let keyfileSavedOnce = false;

  function handleSaveDidKeyfile() {
    try {
      if (identity && typeof identity.downloadKeyfile === "function") {
        identity.downloadKeyfile();
        keyfileSavedOnce = true;
        showToast($_("keyfile_downloaded_toast", {}, "DID keyfile (.json) saved"));
      }
    } catch (e) {
      console.error("[identity] keyfile export error:", e);
      loginError = e.message || "Failed to save keyfile";
    }
  }

  async function keyfileLogin(rawContent) {
    loginLoading = true;
    loginError = "";
    try {
      identity = new PersistentIdentity();
      await identity.loginWithKeyfile(rawContent);
      identityType = "persistent";
      identityDid = identity.did || "";
      authorized = true;
      setSshIdentity(identity);
      setOmdKeyIdentity(identity);
      if (identity?.profile?.omd_key) {
        try {
          storeOmdKey(identity.profile.omd_key);
        } catch (err) {
          console.warn("[App] storeOmdKey notice:", err);
        }
      }
      syncIdentityProfile();
      identity = identity;
      publishAllConnectedDevices();
      await restoreLocalDeviceIfShares();
      loginMode = "select";
      legacyUser = "";
      legacyPass = "";
      startDeviceList();
      checkTriggerOnboarding();
      showToast($_("signed_in_with_keyfile_toast", {}, "Signed in with DID keyfile"));
    } catch (e) {
      console.error("[identity] keyfile login error:", e);
      loginError = e.message || "Invalid DID keyfile";
    } finally {
      loginLoading = false;
    }
  }

  async function handleKeyfileInputChange(e) {
    const file = e?.target?.files?.[0];
    if (e?.target) e.target.value = "";
    if (!file) return;
    try {
      const text = await file.text();
      await keyfileLogin(text);
    } catch (err) {
      loginError = err?.message || "Could not read keyfile";
    }
  }

  async function passwordLogin() {
    loginLoading = true;
    loginError = "";
    try {
      if (!legacyUser || !legacyPass) {
        loginError = "Enter username and password.";
        return;
      }
      if (legacyPass.length < 8) {
        loginError = "Password must be at least 8 characters for SEA.";
        return;
      }
      identity = new PersistentIdentity();
      await identity.passwordLogin(legacyUser, legacyPass);
      identityType = "persistent";
      identityDid = identity.did || "";
      authorized = true;
      setSshIdentity(identity);
      setOmdKeyIdentity(identity);
      if (identity?.profile?.omd_key) {
        try {
          storeOmdKey(identity.profile.omd_key);
        } catch (err) {
          console.warn("[App] storeOmdKey notice:", err);
        }
      }
      syncIdentityProfile();
      identity = identity;
      publishAllConnectedDevices();
      await restoreLocalDeviceIfShares();
      loginMode = "select";
      legacyUser = "";
      legacyPass = "";
      startDeviceList();
      checkTriggerOnboarding();
    } catch (e) {
      console.error("[identity] password login error:", e);
      loginError = e.message || "Login failed";
    } finally {
      loginLoading = false;
    }
  }

  async function legacyMigrationLogin() {
    loginLoading = true;
    loginError = "";
    try {
      // Legacy key may live in the vault, the local session or the profile.
      const omdKey = getStoredOmdKey();
      if (!omdKey) {
        loginError = "No legacy account found. Please sign in via the legacy filemanager first.";
        return;
      }
      identity = new PersistentIdentity();
      await identity.legacyLogin(omdKey);
      identityType = "persistent";
      identityDid = identity.did || "";
      authorized = true;
      setSshIdentity(identity);
      setOmdKeyIdentity(identity);
      if (identity?.profile?.omd_key) {
        try {
          storeOmdKey(identity.profile.omd_key);
        } catch (err) {
          console.warn("[App] storeOmdKey notice:", err);
        }
      }
      syncIdentityProfile();
      identity = identity;
      try {
        publishAllConnectedDevices();
        await restoreLocalDeviceIfShares();
        startDeviceList();
        checkTriggerOnboarding();
        if (typeof identity.flushSessionBackup === "function") {
          await identity.flushSessionBackup();
        }
      } catch (postLoginErr) {
        console.warn("[identity] post-google-login setup warning:", postLoginErr);
      }
    } catch (e) {
      console.error("[identity] legacy migration error:", e);
      loginError = e.message || "Migration failed";
    } finally {
      loginLoading = false;
    }
  }

  async function registerAccount() {
    loginLoading = true;
    loginError = "";
    try {
      if (!regUser || !regPass || !regPassConfirm) {
        loginError = "All fields are required.";
        return;
      }
      if (regPass.length < 8) {
        loginError = "Password must be at least 8 characters.";
        return;
      }
      if (regPass !== regPassConfirm) {
        loginError = "Passwords do not match.";
        return;
      }
      if (regEmail && !regEmail.includes("@")) {
        loginError = "Please enter a valid email address.";
        return;
      }
      identity = new PersistentIdentity();
      keyfileSavedOnce = false;
      const result = await identity.register(regUser, regPass, regEmail || null, regAvatar, $locale);
      identityType = "persistent";
      identityDid = result.did;
      recoveryKey = result.recoveryKey;
      authorized = true;
      setSshIdentity(identity);
      setOmdKeyIdentity(identity);
      if (identity?.profile?.omd_key) {
        try {
          storeOmdKey(identity.profile.omd_key);
        } catch (err) {
          console.warn("[App] storeOmdKey notice:", err);
        }
      }
      syncIdentityProfile();
      identity = identity;
      acceptPublicAiAgreement();
      ensureAssistantDefaults(null, identity?.profile?.omd_key || identity?.token || identity?.did || getStoredOmdKey(), false).catch(() => {});
      publishAllConnectedDevices();
      await restoreLocalDeviceIfShares();
      startDeviceList();
      if (currentApp !== 'settings') {
        loginMode = "recovery";
      } else {
        checkTriggerOnboarding();
      }
    } catch (e) {
      console.error("[identity] registration error:", e);
      loginError = e.message || "Registration failed";
    } finally {
      loginLoading = false;
    }
  }

  // ─── Public AI node ("AI Assistant Public Node") ─────────────────────────────
  // Guest-only, chat-only pseudo device. Never published to the DID device
  // graph; pairing is a *guest* pairing only (no password): the node stores
  // the DID in its GuestDIDs (requires Settings/GuestPairing=true on the
  // public node) and the session reaches ScopedGuest at most. The chat
  // applet opens its own HTTP over DataChannel tunnel to the node's port.
  let publicAiDevice = null;
  let guestAiDevice = null;
  let showAiPublicAgreement = false;
  let aiPublicOpenAfterAck = false;
  let aiPublicOpenViewAfterAck = "chat";
  let pairModalOpenViewAfter = "chat";
  let chatTargetRetries = 0;

  function setupPublicAiStatusHandler(device) {
    if (!device || !device.drive) return;
    const linkId = device.linkId;
    device.drive.onStatusChange = async (newStatus) => {
      if (newStatus === "connected") {
        device.status = "connected";
        // Guest pairing handshake (never a PairModal, never admin rights):
        // AuthInfo reports whether the node allows GuestPairing; then pair
        // this device's DID as a guest without any credentials.
        try {
          const info = await device.drive.authInfo();
          const r = (info && info.result) || {};
          const isUserOwner = r.authLevel === "admin" || r.isOwner === true || !!(identity?.devices && (identity.devices.has?.(linkId) || identity.devices.get?.(linkId)));
          if (isUserOwner) {
            device.isOwner = true;
            device.drive.authLevel = "admin";
          } else if (!r.paired && r.guestPairing) {
            await device.drive.guestPair(device.drive.didPub || identity?.mainPub || getStoredOmdKey() || "");
            console.info("[App] Public AI node: guest DID paired:", linkId);
          } else if (r.authLevel === "guest") {
            device.drive.authLevel = "guest";
          }
          device.rootName = device.drive.rootName || device.name;
        } catch (e) {
          console.warn("[App] Public AI node pairing check failed:", e);
        }
      } else {
        device.status = newStatus;
      }
    };
  }

  function onAiPublicAgreementAccept() {
    acceptPublicAiAgreement();
    const st = $aiNodeStore;
    const ownChatDev = deviceRegistry.list().find((d) => isChatCapableDevice(d) && d.drive?.authLevel === "admin");
    if (!ownChatDev && st.choice !== AI_NODE_MODE_PUBLIC) {
      setAiNodeChoice(AI_NODE_MODE_PUBLIC);
    }
    showAiPublicAgreement = false;
    if (aiPublicOpenAfterAck) {
      aiPublicOpenAfterAck = false;
      const pendingView = aiPublicOpenViewAfterAck || "chat";
      aiPublicOpenViewAfterAck = "chat";
      // the reactive syncPublicAiDevice registers the guest device, then start
      setTimeout(() => openChatsApp(pendingView), 100);
    } else {
      updateRootFilesData();
    }
  }

  function onAiPublicAgreementDecline() {
    showAiPublicAgreement = false;
    aiPublicOpenAfterAck = false;
    aiPublicOpenViewAfterAck = "chat";
  }

  // Public AI node is active when explicitly selected (choice = "public"),
  // or in auto mode with no own chat-capable device available; in both cases
  // the agreement must have been accepted and the node discovered.
  function isChatCapableDevice(d) {
    if (!d || d.isAiNode || d.type === "ai" || d.type === "ai-node") return false;
    const publicLinkId = $aiNodeStore?.info?.linkID;
    if (publicLinkId && (d.linkId === publicLinkId || d.deviceId === publicLinkId)) return false;
    const pubName = $_("public_ai_node_name", {}, "AI Assistant Public Node");
    if (d.name === pubName || d.name === "AI Assistant Public Node" || d.name === "AI Assistant Public Node") return false;
    if (Array.isArray(d.capabilities) && d.capabilities.includes("chat")) return true;
    return false;
  }

  function publicAiWanted(st) {
    if (!st || !st.info) return false;
    if (st.choice === AI_NODE_MODE_PUBLIC) return true;
    if (st.choice === AI_NODE_MODE_AUTO) {
      return !deviceRegistry.list().some((d) => isChatCapableDevice(d) && d.drive?.authLevel === "admin");
    }
    return false;
  }

  // Default chat target when no device is explicitly selected:
  // own chat-capable device if present, otherwise the public AI node or guest AI node.
  function defaultChatTarget() {
    const st = $aiNodeStore;
    if (publicAiWanted(st) && publicAiDevice) return publicAiDevice;
    if (st.choice && guestAiDevice && guestAiDevice.linkId === st.choice) {
      return guestAiDevice;
    }
    const chosen = st.choice && st.choice !== AI_NODE_MODE_AUTO && st.choice !== AI_NODE_MODE_PUBLIC
      ? deviceRegistry.get(st.choice)
      : null;
    if (chosen) return chosen;
    const own = deviceRegistry.list().find((d) =>
      (d.status === "connected" || d.drive?.isConnected) && isChatCapableDevice(d) && d.drive?.authLevel === "admin"
    ) || deviceRegistry.list().find((d) => isChatCapableDevice(d) && d.drive?.authLevel === "admin");
    if (own) return own;
    const pubLinkId = (publicAiWanted(st) && st.info?.linkID)
      || st.info?.linkID
      || (typeof localStorage !== "undefined" && localStorage.getItem("omd_public_ai_link_id"))
      || "";
    if (pubLinkId) {
      return publicAiDevice || {
        deviceId: pubLinkId,
        linkId: pubLinkId,
        name: $_("public_ai_node_name", {}, "AI Assistant Public Node"),
        type: "ai",
        capabilities: ["chat"],
        isAiNode: true,
        isOwner: false,
      };
    }
    return publicAiDevice || guestAiDevice;
  }

  // Устройство, с которым реально открыт чат. Владение и приватность читаются
  // именно с него: раньше isOwner проверялся у activeDevice, который при
  // пустом activeDeviceId равен null, пока targetDevice уже указывал на
  // публичную AI-ноду — из-за этого гость получал owner-режим и пикер файлов
  // чужой ноды («Access Denied: home browsing requires administrator privileges»).
  $: chatTargetDevice = activeDevice
    || (activeDeviceId ? deviceRegistry.get(activeDeviceId) : null)
    || defaultChatTarget()
    || null;

  // linkId устройств пользователя для выбора источника в /learn и /recognize.
  // identity.devices — его собственные узлы; сюда же входят поделившиеся с ним
  // (считаем их своими). Флага isOwner на эти узлы может не быть вовсе: он
  // выставляется только при admin-паре, поэтому по нему список был неполным.
  $: ownDeviceLinkIds = (() => {
    const ids = new Set();
    try {
      if (identity && identity.devices && typeof identity.devices.keys === "function") {
        for (const k of identity.devices.keys()) if (k) ids.add(k);
      } else if (identity && identity.devices) {
        for (const k of Object.keys(identity.devices)) if (k) ids.add(k);
      }
    } catch (_) {}
    for (const s of sharedToMeNodes || []) if (s && s.linkId) ids.add(s.linkId);
    return Array.from(ids);
  })();

  $: chatTargetId = activeDeviceId || chatTargetDevice?.linkId || defaultChatTarget()?.linkId || "";
  $: isTargetOwn = !isSharedRoute && !!chatTargetId && (chatTargetDevice?.isOwner === true || ownDeviceLinkIds.includes(chatTargetId));
  $: isTargetPublicAi = !isTargetOwn && !!(chatTargetDevice?.isAiNode || chatTargetDevice === publicAiDevice || chatTargetDevice === guestAiDevice);
  $: chatPrivateMode = !isSharedRoute && isTargetOwn;

  // Detect whether the user owns at least one real OS/hardware node (as opposed
  // to browser OPFS / mobile client peers that publish with originDevicePub set).
  $: hasOwnHardwareNode = (() => {
    void deviceNodes;
    void currentProfileData;
    if (isSharedRoute) return false;
    if (isTargetOwn && chatTargetDevice && !chatTargetDevice.local && !chatTargetDevice.isAiNode) {
      return true;
    }
    try {
      if (identity && identity.devices) {
        const entries = typeof identity.devices.values === "function"
          ? Array.from(identity.devices.values())
          : Object.values(identity.devices);
        for (const dev of entries) {
          if (!dev || !dev.linkId) continue;
          if (dev.originDevicePub) continue; // browser/mobile local client peer
          const caps = Array.isArray(dev.capabilities) ? dev.capabilities : [];
          const t = String(dev.type || "").toLowerCase();
          if (caps.includes("chat") || caps.includes("terminal") || t === "node" || t === "desktop" || t === "embedded") {
            return true;
          }
        }
      }
    } catch (_) {}
    for (const d of deviceRegistry.list()) {
      if (!d || d.local || d.isAiNode || d === publicAiDevice || d === guestAiDevice) continue;
      if (d.isOwner !== true && d.drive?.authLevel !== "admin") continue;
      const idEntry = identity?.devices?.get ? identity.devices.get(d.linkId) : null;
      if (idEntry && idEntry.originDevicePub) continue;
      const caps = Array.isArray(d.capabilities) ? d.capabilities : [];
      const dt = String(d.deviceType || d.type || "").toLowerCase();
      if (caps.includes("chat") || caps.includes("terminal") || (dt !== "web" && dt !== "phone" && dt !== "tablet")) {
        return true;
      }
    }
    return false;
  })();

  function openChatsApp(initialView = "chat") {
    const targetView = (initialView === "creations" || initialView === "memory" || initialView === "chat")
      ? initialView
      : "chat";
    if (isRealFileManagerFolder(currentFolderId)) {
      lastFileManagerFolderId = currentFolderId;
    }
    const st = $aiNodeStore;
    // Keep the node availability/status fresh for the shortcut dot
    refreshPublicAiNode().catch(() => {});
    // First launch of chats toward the public node: show the responsible-use
    // agreement before any pairing / guest connection happens.
    const ownChatDev = deviceRegistry.list().find((d) => isChatCapableDevice(d) && d.drive?.authLevel === "admin");
    const needsPublic = publicAiWanted(st) || (st.choice === AI_NODE_MODE_PUBLIC && !!st.info) || (!ownChatDev && !!st.info && !st.ack);
    if (needsPublic && !st.ack) {
      aiPublicOpenAfterAck = true;
      aiPublicOpenViewAfterAck = targetView;
      showAiPublicAgreement = true;
      return;
    }
    let device = null;
    if (publicAiWanted(st) && publicAiDevice) {
      device = publicAiDevice;
    } else if (st.choice && guestAiDevice && guestAiDevice.linkId === st.choice) {
      device = guestAiDevice;
    } else if (st.choice && st.choice !== AI_NODE_MODE_AUTO && st.choice !== AI_NODE_MODE_PUBLIC && deviceRegistry.get(st.choice)) {
      device = deviceRegistry.get(st.choice);
    } else {
      device = deviceRegistry.list().find((d) =>
        (d.status === "connected" || d.drive?.isConnected) && isChatCapableDevice(d) && d.drive?.authLevel === "admin"
      ) || ownChatDev || publicAiDevice || guestAiDevice;
    }
    // Never mount the chat applet without a target: wait for the registration
    if (!device || !device.linkId) {
      if (chatTargetRetries < 10) {
        chatTargetRetries++;
        setTimeout(() => openChatsApp(targetView), 400);
        return;
      }
      chatTargetRetries = 0;
    }
    chatTargetRetries = 0;
    // If chat target is an own node / private device that requires pairing from this browser:
    if (!isSharedRoute && device && device.drive && device.drive.authLevel !== "admin" && !device.isAiNode && device !== publicAiDevice && device !== guestAiDevice) {
      const isExplicitChoice = st.choice === device.linkId && st.choice !== AI_NODE_MODE_AUTO && st.choice !== AI_NODE_MODE_PUBLIC;
      if (isExplicitChoice) {
        if (!device.drive.isConnected) {
          if (chatTargetRetries < 10) {
            chatTargetRetries++;
            setTimeout(() => openChatsApp(targetView), 400);
            return;
          }
        }
        console.log("[App] Chat target requires authentication on explicit selection, opening PairModal:", device.linkId);
        pairModalOpenChatAfter = true;
        pairModalOpenViewAfter = targetView;
        openPairModal(device.linkId, (device.drive && device.drive.rootName) || device.name || device.linkId);
        return;
      } else {
        device = publicAiDevice;
      }
    }
    if (device && device.linkId) activeDeviceId = device.linkId;
    chatActiveView = targetView;
    currentApp = "chat";
    const viewHash = targetView === "creations"
      ? "#/chat/creations"
      : targetView === "memory"
        ? "#/chat/memory"
        : "#/chat";
    updateUrlHash(viewHash);
  }

  function currentFilesFolder() {
    let folder = isRealFileManagerFolder(currentFolderId) ? currentFolderId : lastFileManagerFolderId;
    if (!isRealFileManagerFolder(folder)) folder = "/";
    return folder;
  }

  function filesHash(folder) {
    if (!folder || folder === "/") {
      return appMode === APP_MODE_CHAT ? "#/files" : "#/";
    }
    return "#" + (folder.startsWith("/") ? folder : "/" + folder);
  }

  // Open the file manager. In chat mode this is a closable overlay; in files
  // mode it is the main app (so it is equivalent to closing the chat applet).
  function returnToFiles(folderId = "") {
    const target = isRealFileManagerFolder(folderId) ? folderId : currentFilesFolder();
    if (isRealFileManagerFolder(currentFolderId)) lastFileManagerFolderId = currentFolderId;
    currentApp = "filemanager";
    currentFolderId = target;
    updateUrlHash(filesHash(target));
    if (fmApi) {
      try { fmApi.exec("set-path", { id: target }); } catch (_) {}
    }
  }

  function openFilesApp(folderId = "") {
    returnToFiles(folderId);
  }

  function closeChatsApp() {
    // In chat mode the chat applet is the root and has no close button.
    if (appMode === APP_MODE_CHAT) return;
    returnToFiles();
  }

  function closeFilesApp() {
    // Only meaningful for the files overlay in chat mode.
    if (appMode !== APP_MODE_CHAT) return;
    currentApp = "chat";
    updateUrlHash("#/chat");
  }

  function syncPublicAiDevice() {
    const st = $aiNodeStore;
    const active = publicAiWanted(st);

    if (active) {
      const linkId = st.info.linkID;
      const deviceName = $_("public_ai_node_name", {}, "AI Assistant Public Node");
      // Not a visible device: no registry entry, no tree/card node, no device
      // items - only the shortcut entry in SERVICE_SECTIONS ("Чаты") shows the
      // node status via the online dot.
      if (!publicAiDevice || publicAiDevice.linkId !== linkId) {
        if (publicAiDevice && publicAiDevice.drive) {
          publicAiDevice.drive.close();
        }
        const drive = new WebRTCDataChannelDrive(linkId);
        drive.didPub = identity?.mainPub || getStoredOmdKey() || "";
        publicAiDevice = {
          deviceId: linkId,
          linkId,
          name: deviceName,
          type: "ai",
          capabilities: ["chat"],
          drive,
          status: "connecting",
          isOwner: !!(identity?.devices && (identity.devices.has?.(linkId) || identity.devices.get?.(linkId))),
          local: false,
          isAiNode: true,
        };
        setupPublicAiStatusHandler(publicAiDevice);
        drive.connect();
      } else if (publicAiDevice.name !== deviceName) {
        publicAiDevice.name = deviceName;
      }
    } else if (publicAiDevice) {
      if (publicAiDevice.drive) publicAiDevice.drive.close();
      if (activeDeviceId === publicAiDevice.linkId) activeDeviceId = "";
      publicAiDevice = null;
    }
    updateRootFilesData();
  }

  function syncGuestAiDevice() {
    const st = $aiNodeStore;
    const customList = st?.customNodes || [];
    const customEntry = customList.find((c) => c && c.linkId === st.choice);

    if (customEntry) {
      const linkId = customEntry.linkId;
      const deviceName = customEntry.name || linkId;
      if (!guestAiDevice || guestAiDevice.linkId !== linkId) {
        if (guestAiDevice && guestAiDevice.drive) {
          guestAiDevice.drive.close();
        }
        const drive = new WebRTCDataChannelDrive(linkId);
        drive.didPub = identity?.mainPub || getStoredOmdKey() || "";
        guestAiDevice = {
          deviceId: linkId,
          linkId,
          name: deviceName,
          type: "ai",
          capabilities: ["chat"],
          drive,
          status: "connecting",
          isOwner: !!(identity?.devices && (identity.devices.has?.(linkId) || identity.devices.get?.(linkId))),
          local: false,
          isAiNode: true,
        };
        setupPublicAiStatusHandler(guestAiDevice);
        drive.connect();
      } else if (guestAiDevice.name !== deviceName) {
        guestAiDevice.name = deviceName;
      }
    } else if (guestAiDevice) {
      if (guestAiDevice.drive) guestAiDevice.drive.close();
      if (activeDeviceId === guestAiDevice.linkId) activeDeviceId = "";
      guestAiDevice = null;
    }
    updateRootFilesData();
  }

  $: syncPublicAiDevice(publicAiWanted($aiNodeStore));
  $: syncGuestAiDevice($aiNodeStore);

  // "Чаты" shortcut status dot: online state of the chat target node -
  // the selected own node (choices are linkIds) or the public AI node.
  $: chatsEntryOnline = (() => {
    // Explicitly track reactive variables that change on device connect / disconnect
    void deviceNodes;
    void status;

    // The chat applet is open with a working target - the dot is online
    if (currentApp === "chat" && activeDeviceId) return true;
    const st = $aiNodeStore;
    if (st.choice && guestAiDevice && guestAiDevice.linkId === st.choice) {
      return !!(guestAiDevice.drive?.isConnected || guestAiDevice.status === "connected");
    }
    const choiceDev = deviceRegistry.get(st.choice || "");
    if (choiceDev) return !!(choiceDev.drive?.isConnected || choiceDev.status === "connected");
    if (publicAiWanted(st)) {
      if (publicAiDevice && (publicAiDevice.drive?.isConnected || publicAiDevice.status === "connected")) return true;
      return !!st.info;
    }

    // 1. Check reactive deviceNodes
    const onlineChatNode = deviceNodes.find((d) => {
      if (!d.online) return false;
      const reg = deviceRegistry.getByPrefix(d.id) || deviceRegistry.get(d.linkId);
      if (reg && isChatCapableDevice(reg)) return true;
      return Array.isArray(d.capabilities) && d.capabilities.includes("chat");
    });
    if (onlineChatNode) return true;

    // 2. Check deviceRegistry list
    const ownChat = deviceRegistry.list().find((d) =>
      (d.status === "connected" || d.drive?.isConnected) && isChatCapableDevice(d)
    );
    return !!ownChat;
  })();

  $: {
    const section = SERVICE_SECTIONS.find((s) => s.id === "/chats");
    if (section && section.online !== chatsEntryOnline) {
      section.online = chatsEntryOnline;
      updateRootFilesData();
    }
  }

  let refreshDevicesTimer = null;
  function refreshOnlineStatusForRegisteredDevices() {
    if (refreshDevicesTimer) clearTimeout(refreshDevicesTimer);
    refreshDevicesTimer = setTimeout(async () => {
      refreshDevicesTimer = null;
      const myTargetIds = deviceRegistry.list()
        .filter(d => !d.local && d.linkId && d.linkId !== getOrCreateLocalLinkId())
        .map(d => d.linkId);
      if (myTargetIds.length === 0) return;
      console.log("[App] Querying HTTP /api/devices online status for registered devices:", myTargetIds);
      const gatewayBase = getGatewayBase();
      try {
        let resp = await fetch(`${gatewayBase}/api/devices`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ linkIds: myTargetIds })
        });
        if (!resp.ok) {
          resp = await fetch(`${gatewayBase}/api/signaling`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ kind: "devices", linkIds: myTargetIds })
          });
        }
        if (!resp.ok) return;
        const resData = await resp.json();
        const onlineDevices = resData?.devices || [];
        console.log("[App] HTTP /api/devices returned online devices:", onlineDevices);
        if (Array.isArray(onlineDevices)) {
          const localId = getOrCreateLocalLinkId();
          const myPub = identity?.devicePub || "";
          for (const dev of onlineDevices) {
            const candidateIds = typeof dev === "string"
              ? [dev]
              : [dev?.linkID, dev?.linkId, dev?.id, dev?.peerID, dev?.user].filter(Boolean);
            if (candidateIds.length === 0 || candidateIds.includes(localId)) continue;
            const registered = candidateIds.map(id => deviceRegistry.get(id)).find(Boolean);
            if (registered && typeof dev === "object" && dev) {
              if (dev.peerID) registered.peerId = dev.peerID;
              if (dev.linkID || dev.user) registered.shortId = dev.linkID || dev.user;
            }
            // Public AI node is a chat-only pseudo device: skip full connectDevice
            // (it would otherwise trigger pairing UI / file operations).
            if (registered && registered.isAiNode) continue;
            // Skip only stale linkIds belonging to this exact browser instance (same devicePub)
            const savedMeta = registered && identity?.devices?.get ? identity.devices.get(registered.linkId) : null;
            if (myPub && savedMeta?.originDevicePub && savedMeta.originDevicePub === myPub) {
              continue;
            }
            if (registered && !registered.local && (!registered.drive || !registered.drive.isConnected)) {
              console.log("[App] HTTP online device matches our DID device! Connecting:", registered.linkId);
              connectDevice(registered.linkId);
            }
          }
        }
      } catch (err) {
        console.warn("[App] Failed querying /api/devices:", err);
      }
    }, 300);
  }

  function setupDeviceStatusHandler(device) {
    if (!device || !device.drive) return;
    const linkId = device.linkId;
    device.drive.onStatusChange = async (newStatus) => {
      console.log(`[App] Device ${linkId} status changed to: ${newStatus}`);
      if (newStatus === "connected") {
        device.status = "connected";

        if (isSharedRoute) {
          status = "connected";
          authorized = true;
          initialDeviceLoadDone = true;
          if (resolvedShareData) {
            shareError = "";
            shareLoading = false;
            sharePasswordModal = false;
            mainApp = "singleview";
            currentApp = "singleview";
            if (device.drive) {
              const scopeArg = resolvedShareData.shareId
                ? { shareId: resolvedShareData.shareId, path: resolvedShareData.path, writable: !!resolvedShareData.writable }
                : resolvedShareData.path;
              device.drive.setScope(scopeArg).catch(e => {
                console.warn("[SharedRoute] setScope on reconnect warning:", e);
              });
            }
            window.dispatchEvent(new CustomEvent("omd-p2p-reconnected"));
          }
          return;
        }

        // Ensure device hostname is discovered via authInfo if not yet known
        if ((!device.drive.rootName || device.name === linkId) && typeof device.drive.authInfo === "function") {
          try {
            const info = await device.drive.authInfo();
            if (info && info.status === "ok" && info.result && info.result.name) {
              device.drive.rootName = info.result.name;
            }
          } catch (_) {}
        }

        // AI nodes are chat-only guest devices: never open PairModal or browse files on them
        const pubAiId = $aiNodeStore?.info?.linkID;
        if (device.isAiNode || (pubAiId && (linkId === pubAiId || device.deviceId === pubAiId))) {
          return;
        }

        const isAuthed = isSharedRoute || (device.drive && device.drive.authLevel === "admin");
        let rootItems = [];
        if (isAuthed) {
          rootItems = (await loadFolder(device.drive, "/")) || [];
        }
        if (rootItems || !isAuthed) {
          const resolvedName = (device.drive && device.drive.rootName) || (device.name && device.name !== linkId ? device.name : "") || (identity && identity.devices && identity.devices.get(linkId)?.name) || linkId;
          const prevPrefix = device.prefix;
          device.name = resolvedName;
          device.prefix = stableDevicePrefix(device.name, linkId, [prevPrefix, "/" + linkId]);
          if (prevPrefix !== device.prefix) {
            if (window._deviceItems) {
              for (const k of Object.keys(window._deviceItems)) {
                if (k === prevPrefix || k.startsWith(prevPrefix + "/")) delete window._deviceItems[k];
              }
            }
            if (window._folderItems) {
              for (const k of Object.keys(window._folderItems)) {
                if (k === prevPrefix || k.startsWith(prevPrefix + "/")) delete window._folderItems[k];
              }
            }
          }
          const hasTerminal = rootItems.some(f => f.mimeType === "application/x-terminal" || f.customAttributes?.uiAction === "terminal" || f.name === "Terminal");
          const hasChat = rootItems.some(f =>
            f.mimeType === "application/x-chat" ||
            f.customAttributes?.uiAction === "chat" ||
            f.name === "Chats" ||
            f.name === "Data" ||
            f.name?.toLowerCase() === "data" ||
            f.name === "OnMyChat" ||
            f.name?.toLowerCase() === "onmychat" ||
            f.name?.toLowerCase() === "chats"
          );
          const caps = Array.isArray(device.capabilities) && device.capabilities.includes("chat")
            ? [...device.capabilities]
            : ["files"];
          if (hasTerminal && !caps.includes("terminal")) caps.push("terminal");
          if (hasChat && !caps.includes("chat")) caps.push("chat");
          device.capabilities = caps;

          const baseDeviceItems = rootItems.map(f => ({ ...f, id: device.prefix + f.id }));
          const cachedApps = isAuthed ? getCachedNodeAppItems(linkId) : null;
          const deviceItems = cachedApps ? mergeAppsIntoRootItems(device.prefix, baseDeviceItems, cachedApps) : baseDeviceItems;
          const devType = detectDeviceType(device.name, (identity && identity.devices && identity.devices.get(linkId)?.type) || device.type);
          const devIcon = getDeviceIcon(devType);
          device.deviceType = devType;
          device.icon = devIcon;

          const deviceNode = {
            id: device.prefix,
            value: device.name,
            name: device.name,
            type: "folder",
            itemType: "device",
            isDevice: true,
            deviceType: devType,
            capabilities: caps,
            icon: devIcon,
            parent: "/",
            linkId,
            date: new Date(),
            size: 0,
            open: isAuthed,
            lazy: !isAuthed,
            online: true
          };

          if (!window._deviceItems) window._deviceItems = {};
          window._deviceItems[device.prefix] = deviceItems;
          if (isAuthed) {
            refreshDeviceNodeApps(device);
          }

          const chatsSec = SERVICE_SECTIONS.find(s => s.id === "/chats");
          if (chatsSec && hasChat) {
            chatsSec.online = true;
          }

          deviceNodes = [...deviceNodes.filter(d => d.linkId !== linkId && (d.id !== device.prefix || d.local)), deviceNode];
          updateRootFilesData();
          status = "connected";
          authorized = true;
          initialDeviceLoadDone = true;

          // If defaultStorage is not device-qualified, qualify it with the newly connected device
          const curStorage = (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") || currentProfileData?.defaultStorage || "";
          const parsedStorage = parseStoragePath(curStorage);
          if (!parsedStorage.isDeviceQualified && device && !device.isAiNode && !device.local) {
            const qualifiedPath = formatStoragePath(device, parsedStorage.folder || DEFAULT_STORAGE_PATH);
            persistStoragePath(qualifiedPath, identity);
          }

          if (identity && identity.type === "persistent" && !isSharedRoute && device.drive?.authLevel === "admin") {
            identity.publishDevice(
              linkId,
              device.name,
              devType,
              caps
            );
          }

          if (fmApi) {
            fmApi.exec("provide-data", { id: device.prefix, data: deviceItems });
            const activeDev = currentFolderId ? resolveDevice(splitDeviceId(currentFolderId)?.prefix) : null;
            if (currentFolderId && currentFolderId !== "/" && (
              currentFolderId === device.prefix || 
              currentFolderId.startsWith(device.prefix + "/") ||
              (activeDev && activeDev.linkId === device.linkId)
            )) {
              const restored = restoreActiveFolder(device);
              if (restored) restored.then(id => console.log("[App] Active folder restored after connect:", id));
            }
          }

          if (p2pReconnectingStatus === 'reconnecting' || status === 'connecting') {
            p2pReconnectingStatus = 'connected';
            window.dispatchEvent(new CustomEvent("omd-p2p-reconnected"));
            if (hideToastTimer) clearTimeout(hideToastTimer);
            hideToastTimer = setTimeout(() => { p2pReconnectingStatus = null; }, 1800);
          }
        }
      } else {
        device.status = newStatus;
        if (newStatus === "error") {
          status = "error";
          if (p2pReconnectingStatus === 'reconnecting') {
            p2pReconnectingStatus = null;
          }
        } else if (newStatus === "disconnected") {
          deviceNodes = deviceNodes.filter(d => (d.linkId !== linkId && d.id !== device.prefix) || d.local);
          updateRootFilesData();
          const anyConnected = deviceRegistry.list().some(d => d.status === "connected" || d.local);
          if (!anyConnected) {
            status = "disconnected";
            if (identityType !== "persistent" && !isSharedRoute) authorized = false;
          }
        }
      }
    };
  }

  function registerDevicePlaceholder(linkId, devName = "") {
    if (!linkId || linkId === "undefined") return;
    if (linkId === getOrCreateLocalLinkId()) return;
    const publicLinkId = $aiNodeStore?.info?.linkID;
    if (publicLinkId && linkId === publicLinkId) return;
    const pubName = $_("public_ai_node_name", {}, "AI Assistant Public Node");
    if (devName === pubName || devName === "AI Assistant Public Node" || devName === "AI Assistant Public Node") return;

    const savedDev = identity && identity.devices && identity.devices.get ? identity.devices.get(linkId) : null;
    const resolvedName = devName || savedDev?.name || linkId;
    const resolvedType = savedDev?.type || "";
    const resolvedCaps = Array.isArray(savedDev?.capabilities) ? savedDev.capabilities : ["files"];
    const myPub = identity?.devicePub || "";
    if (myPub && savedDev?.originDevicePub && savedDev.originDevicePub === myPub) {
      return;
    }

    const existing = deviceRegistry.get(linkId);
    if (existing) {
      if ((existing.local || existing.status === "connected" || existing.drive?.isConnected) && !deviceNodes.some(d => d.linkId === linkId)) {
        deviceNodes = [...deviceNodes.filter(d => d.linkId !== linkId), buildDeviceNode(existing)];
        updateRootFilesData();
      }
      return;
    }

    const drive = new WebRTCDataChannelDrive(linkId);
    drive.didPub = identity?.mainPub || null;
    const device = {
      deviceId: linkId,
      linkId,
      name: resolvedName,
      prefix: "/" + linkId,
      type: resolvedType,
      capabilities: resolvedCaps,
      drive,
      status: "disconnected",
    };
    setupDeviceStatusHandler(device);
    deviceRegistry.add(linkId, device);

    // Only show device in UI if already connected / online; do not show offline placeholders
    if (device.local || device.status === "connected" || device.drive?.isConnected) {
      if (!deviceNodes.some(d => d.linkId === linkId)) {
        deviceNodes = [...deviceNodes.filter(d => d.linkId !== linkId), buildDeviceNode(device)];
        updateRootFilesData();
      }
    }

    // Only connect if the gateway confirms the device is actually online
    refreshOnlineStatusForRegisteredDevices();
  }

  function startDeviceList() {
    if (unsubDeviceList) unsubDeviceList();
    if (unsubSharedByMe) { unsubSharedByMe(); unsubSharedByMe = null; }
    if (unsubSharedToMe) { unsubSharedToMe(); unsubSharedToMe = null; }
    setTimeout(() => {
      initialDeviceLoadDone = true;
    }, 8000);
    unsubSharedByMe = subscribeSharedByMe(identity, updateSharedByMeNodes);
    reloadSharedByMe();
    reloadSharedToMe();
    unsubSharedToMe = subscribeSharedToMe(identity, () => reloadSharedToMe());
    unsubDeviceList = loadMyDevices(
      identity,
      registerDevicePlaceholder,
      () => filesData,
      (v) => {
        filesData = v;
        initialDeviceLoadDone = true;
      },
      () => fmApi
    );
    // Actively initiate reconnect for all saved devices on startup / after login
    reconnectDisconnectedDevices().catch(e => console.warn("[App] startDeviceList reconnect warning:", e));
  }

  // Cloud-assisted nearby LAN discovery
  let detectedNearbyDevices = [];
  let scanningNearbyDevices = false;

  let prevShowAddDevicePanel = false;
  $: if (showAddDevicePanel && !prevShowAddDevicePanel && !isSharedRoute) {
    prevShowAddDevicePanel = true;
    refreshNearbyDevices();
  } else if (!showAddDevicePanel) {
    prevShowAddDevicePanel = false;
  }

  let prevAutoConnectModal = false;
  $: if (autoConnectModal && !prevAutoConnectModal && !isSharedRoute) {
    prevAutoConnectModal = true;
    refreshNearbyDevices();
  } else if (!autoConnectModal) {
    prevAutoConnectModal = false;
  }

  async function refreshNearbyDevices() {
    if (isSharedRoute) return;
    scanningNearbyDevices = true;
    try {
      const list = await fetchNearbyDevices();
      detectedNearbyDevices = (list || []).filter(d => d.linkId && !deviceRegistry.has(d.linkId));
    } catch (err) {
      console.warn("[App] refreshNearbyDevices error:", err);
    } finally {
      scanningNearbyDevices = false;
    }
  }

  async function reconnectDisconnectedDevices() {
    console.log("[App] Root refresh: attempting to reconnect disconnected devices...");
    // 1. Ensure shared gateway channel is connected
    try {
      const gatewayChan = getSharedGatewayChannel();
      if (gatewayChan && !gatewayChan.isConnected) {
        await gatewayChan.connect();
      }
    } catch (e) {
      console.warn("[App] Error connecting gateway channel on root refresh:", e);
    }

    // 2. Load / register placeholders for all saved devices in persistent identity
    if (identity && identity.type === "persistent" && identity.devices) {
      const localId = getOrCreateLocalLinkId();
      for (const dev of identity.devices.values()) {
        if (!dev.linkId || dev.linkId === localId) continue;
        const existing = deviceRegistry.get(dev.linkId);
        if (!existing) {
          registerDevicePlaceholder(dev.linkId, dev.name);
        }
      }
    }

    // 3. Trigger discovery & online status checks
    refreshNearbyDevices();
    refreshOnlineStatusForRegisteredDevices();

    // 4. Online devices are connected by refreshOnlineStatusForRegisteredDevices above
    // or by WebSocket linkConnected push events. Offline devices are left alone until they wake up.

    // 5. For any device that is already connected, make sure its node is in deviceNodes
    for (const dev of deviceRegistry.list()) {
      if (!dev.local && dev.drive && dev.drive.isConnected) {
        if (!deviceNodes.some(d => d.linkId === dev.linkId)) {
          try {
            deviceNodes = [...deviceNodes.filter(d => d.linkId !== dev.linkId), buildDeviceNode(dev)];
            if (dev.drive.authLevel === "admin") {
              await ensureFolderAndParents(dev, "/");
            }
          } catch (e) {
            console.warn("[App] Root refresh: failed to build node for connected dev:", dev.linkId, e);
          }
        }
      }
    }
    updateRootFilesData();
  }

  // Retry button on the "No devices" empty state: re-subscribe to the saved
  // device list, re-attempt connecting each device, and rebuild tree nodes for
  // any device that is already connected in the registry but missing from the UI.
  async function retryDeviceConnection() {
    status = "connecting";
    startDeviceList();
    await reconnectDisconnectedDevices();
    const anyConnected = deviceRegistry.list().some(d => d.status === "connected" || d.drive?.isConnected);
    status = anyConnected ? "connected" : "disconnected";
    initialDeviceLoadDone = true;
  }

  function detectDeviceType(name, type = "") {
    if (type === "node") return "embedded";
    if (type === "phone" || type === "tablet" || type === "desktop") return type;
    if (type === "web") {
      return getResolutionDeviceType();
    }
    const n = (name || "").toLowerCase();
    if (/tablet|ipad/i.test(n)) return "tablet";
    if (/phone|android|iphone|ipod|mobile/i.test(n)) return "phone";
    if (/brave|chrom|laptop|desktop|mac|pc|windows|linux/i.test(n)) return "desktop";
    return "embedded";
  }

  function getDeviceIcon(deviceType) {
    const iconMap = {
      phone: "wxi-phone",
      tablet: "wxi-tablet",
      desktop: "wxi-desktop",
      embedded: "wxi-embedded"
    };
    return iconMap[deviceType] || "wxi-embedded";
  }

  function buildDeviceNode(device) {
    const devType = detectDeviceType(device.name, device.deviceType || device.type);
    const devIcon = getDeviceIcon(devType);
    const isOnline = !!(device.local || (device.drive && device.drive.isConnected) || device.status === "connected");
    const isAuthed = !!(device.local || (device.drive && device.drive.authLevel === "admin"));
    return {
      id: device.prefix,
      value: device.name,
      name: device.name,
      type: "folder",
      itemType: "device",
      isDevice: true,
      deviceType: devType,
      capabilities: device.capabilities || [],
      icon: devIcon,
      parent: "/",
      linkId: device.linkId,
      date: new Date(),
      size: 0,
      open: isAuthed,
      lazy: !isAuthed,
      online: isOnline,
      local: !!device.local,
      isLocal: !!device.local,
      isAiNode: !!device.isAiNode,
    };
  }

  async function restoreLocalDeviceIfShares() {
    if (typeof indexedDB === "undefined") return;
    try {
      let shares = [];
      try { shares = await listLocalShares(); } catch (_) { shares = []; }
      if (!shares.length && isNativeStorageAvailable()) {
        const share = { name: "Storage", kind: "native", addedAt: Date.now() };
        try { await saveLocalShare(share); } catch (_) {}
        shares = [share];
      } else if (!isNativeStorageAvailable()) {
        try {
          const targetStorage = currentProfileData?.defaultStorage || identity?.profile?.defaultStorage || DEFAULT_STORAGE_PATH;
          const opfsShare = await ensureDefaultOpfsShare(targetStorage);
          if (opfsShare && !shares.some(s => s.name === opfsShare.name)) {
            shares = [...shares, opfsShare];
          }
        } catch (e) { console.warn("[App] ensureDefaultOpfsShare notice:", e); }
      }
      // Устройство нужно всегда, даже когда папок ещё нет: нода адресует
      // браузер по P2P через POST /api/device/register и WS linkID, иначе
      // публичная AI-нода не сможет ни стянуть документы, ни получить карточку
      // (KnowledgePut). Нативный клиент создаёт "Storage" сам, браузеру папки
      // добавляются кнопкой "Add folder on this device".
      const linkId = getOrCreateLocalLinkId();
      const existing = deviceRegistry.get(linkId);
      if (existing && existing.local) {
        // Re-register/publish the local web device on every start so it
        // always appears in the device list (even after in-memory restore).
        try {
          await registerLocalDeviceNode(existing);
        } catch (e) {
          console.warn("[App] registerLocalDeviceNode (existing) error:", e);
        }
        return;
      }
      const device = makeLocalDevice();
      let deviceName = localStorage.getItem("omd_local_devicename");
      if (!deviceName || deviceName === "This Device" || deviceName.includes("(browser)")) {
        deviceName = getDefaultWebDeviceName();
        localStorage.setItem("omd_local_devicename", deviceName);
      }
      device.name = deviceName;
      device.prefix = "/" + deviceName;
      await registerLocalDeviceNode(device);
      await ensureFolderAndParents(device, "/");
    } catch (e) {
      console.warn("[App] restoreLocalDeviceIfShares error:", e);
    }
  }

  async function registerLocalDeviceNode(device) {
    const myPub = identity?.devicePub || "";
    for (const d of deviceRegistry.list()) {
      if (d && !d.local && d.linkId !== device.linkId) {
        const savedMeta = identity?.devices?.get ? identity.devices.get(d.linkId) : null;
        if (myPub && savedMeta?.originDevicePub && savedMeta.originDevicePub === myPub) {
          deviceRegistry.delete(d.linkId);
        }
      }
    }
    deviceRegistry.add(device.linkId, device);
    deviceNodes = [
      ...deviceNodes.filter(d => d.linkId !== device.linkId && d.id !== device.prefix),
      buildDeviceNode(device)
    ];
    if (!window._deviceItems) window._deviceItems = {};
    try {
      const rootItems = await device.drive.getFiles("/");
      const remapped = (rootItems || [])
        .filter(f => f && typeof f === "object" && (f.isDir || f.type === "folder" || !(f.name || f.value || f.id || "").toLowerCase().endsWith(".univer.json")))
        .map(f => ({ ...f, id: device.prefix + f.id, parent: device.prefix }));
      window._deviceItems[device.prefix] = remapped;
    } catch (e) {
      console.error("[App] local device root load error:", e);
      if (!window._deviceItems[device.prefix]) {
        window._deviceItems[device.prefix] = [];
      }
    }
    updateRootFilesData();
    if (fmApi) {
      fmApi.exec("provide-data", { id: device.prefix, data: window._deviceItems[device.prefix] || [] });
    }
    if (identity && identity.type === "persistent" && identity.publishDevice) {
      try {
        if (myPub && identity.devices && typeof identity.removeDevice === "function") {
          for (const oldDev of identity.devices.values()) {
            if (!oldDev || !oldDev.linkId || oldDev.linkId === device.linkId) continue;
            // Remove only stale linkIds created earlier by THIS exact browser instance,
            // never another Linux/Windows desktop browser sharing the default OS name.
            if (oldDev.originDevicePub && oldDev.originDevicePub === myPub) {
              identity.removeDevice(oldDev.linkId).catch(() => {});
            }
          }
        }
        const devType = detectDeviceType(device.name, isNativeStorageAvailable() ? "phone" : "web");
        await identity.publishDevice(device.linkId, device.name, devType, ["files"], { isLocal: true });
      } catch (e) {
        console.warn("[App] publish local device error:", e);
      }
    }
    try {
      startClientDeviceServer(device.linkId, "/api/signaling", device.name);
    } catch (e) {
      console.warn("[App] Failed to start ClientDeviceServer:", e);
    }
  }

  // "Add folder on this device": register the current browser or mobile phone
  // as a device and expose local folders / device storage roots.
  async function addFolderOnThisDevice() {
    localDeviceBusy = true;
    localDeviceError = "";
    try {
      let share = null;
      if (isNativeStorageAvailable()) {
        share = { name: "Storage", kind: "native", addedAt: Date.now() };
        await saveLocalShare(share);
      } else if (isFileSystemAccessAvailable()) {
        try {
          share = await addShareFromPicker();
        } catch (e) {
          if (e && (e.name === "AbortError" || e.code === 20)) return; // user cancelled
          throw e;
        }
      } else {
        share = await addOpfsShare();
      }
      if (!share) return;
      const localDevice = makeLocalDevice();
      const existing = deviceRegistry.get(localDevice.linkId);
      const device = (existing && existing.local) ? existing : localDevice;
      let deviceName = localStorage.getItem("omd_local_devicename");
      if (!deviceName || deviceName === "This Device" || deviceName.includes("(browser)")) {
        deviceName = getDefaultWebDeviceName();
        localStorage.setItem("omd_local_devicename", deviceName);
      }
      device.name = deviceName;
      device.prefix = "/" + deviceName;
      await registerLocalDeviceNode(device);
      // Refresh the device root so the new share is immediately visible.
      await ensureFolderAndParents(device, "/");
    } catch (e) {
      console.error("[App] addFolderOnThisDevice error:", e);
      localDeviceError = e && e.message ? e.message : "Failed to add folder on this device";
    } finally {
      localDeviceBusy = false;
    }
  }

  async function logoutIdentity() {
    initialDeviceLoadDone = false;
    if (unsubProfile) { unsubProfile(); unsubProfile = null; }
    currentProfileData = { name: "", user: "", email: "", image: "" };
    if (unsubDeviceList) { unsubDeviceList(); unsubDeviceList = null; }
    if (unsubSharedByMe) { unsubSharedByMe(); unsubSharedByMe = null; }
    clearSshIdentity();
    clearOmdKeyIdentity();
    clearMcpKeysIdentity();
    // Disconnect all devices
    for (const dev of deviceRegistry.list()) {
      deviceRegistry.delete(dev.linkId);
    }
    filesData = [];
    status = "disconnected";
    authorized = false;
    if (identity) {
      await identity.logout();
      identity.destroy();
    }
    identity = new EphemeralIdentity();
    await identity.init();
    identityType = "ephemeral";
    identityDid = "";
    identityEmail = "";
    loginMode = "select";
    recoveryKey = "";
  }

  async function deleteAccount(event = null) {
    const password = (event && event.detail && event.detail.password) || "";
    try {
      if (!identity || identity.type !== "persistent" || typeof identity.deleteAccount !== "function") {
        throw new Error("No signed-in account to delete");
      }
      await identity.deleteAccount({ password });
      await logoutIdentity();
      try {
        localStorage.clear();
        sessionStorage.clear();
      } catch (e) {
        console.warn("Storage clear error:", e);
      }
      currentApp = "filemanager";
      updateUrlHash(filesHash("/"));
      showToast($_("account_deleted_toast", {}, "Account deleted and local data cleared."));
    } catch (err) {
      console.error("Failed to delete account:", err);
      showToast($_("error", {}, "Error") + ": " + (err && err.message ? err.message : String(err)));
    }
  }

  // drive info for the filemanager (used/total storage)
  const driveInfo = { used: 0, total: 0 };

  // Split a namespaced id like "/<devicePrefix>/Foo/Bar" into { prefix, path }.
  function splitDeviceId(id) {
    if (typeof id !== "string" || id === "/" || !id.startsWith("/")) return null;
    if (
      id === "/shared-to-me" || id === "/shared-by-me" ||
      id.startsWith("/shared-to-me/") || id.startsWith("/shared-by-me/") ||
      !isRealFileManagerFolder(id)
    ) {
      return null;
    }
    const rest = id.slice(1);
    const slashIdx = rest.indexOf("/");
    if (slashIdx < 0) return { prefix: rest, path: "/" };
    return { prefix: rest.slice(0, slashIdx), path: rest.slice(slashIdx) };
  }

  function resolveDevice(prefix) {
    return deviceRegistry.getByPrefix(prefix);
  }

  function resolveSharedPath(id) {
    if (typeof id !== "string") return null;
    let section = null;
    let rest = "";

    if (id === "/shared-by-me" || id.startsWith("/shared-by-me/")) {
      section = "shared-by-me";
      rest = id.slice("/shared-by-me/".length);
    } else if (id === "/shared-to-me" || id.startsWith("/shared-to-me/")) {
      section = "shared-to-me";
      rest = id.slice("/shared-to-me/".length);
    } else {
      return null;
    }

    if (!rest) {
      return { section, rootShareNode: null, relativeSubPath: "", realPath: "", device: null };
    }

    const segments = rest.split("/").filter(Boolean);
    const rootName = segments[0];
    const subSegments = segments.slice(1);
    const relativeSubPath = subSegments.length > 0 ? "/" + subSegments.join("/") : "";

    const nodeList = section === "shared-by-me" ? sharedByMeNodes : sharedToMeNodes;
    const rootId = `/${section}/${rootName}`;
    const rootNode = (nodeList || []).find(n => n && (n.id === rootId || n.name === rootName || n.value === rootName));

    if (!rootNode) {
      return { section, rootShareNode: null, relativeSubPath, realPath: "", device: null };
    }

    let device = null;
    if (rootNode.linkId) {
      device = deviceRegistry.get(rootNode.linkId);
    }
    if (!device && section === "shared-to-me") {
      const shareEntry = rootNode.shareId ? getShareDriveEntry(rootNode.shareId) : null;
      if (shareEntry && shareEntry.drive) {
        device = {
          linkId: rootNode.linkId,
          name: rootNode.name,
          prefix: shareEntry.prefix,
          drive: shareEntry.drive,
          isShareDrive: true,
          shareEntry
        };
      }
    }
    if (!device && section !== "shared-to-me") {
      const allDevs = deviceRegistry.getAll();
      device = allDevs.length > 0 ? allDevs[0] : null;
    }

    let baseRealPath = rootNode.path || "";
    if (baseRealPath && !baseRealPath.startsWith("/")) {
      baseRealPath = "/" + baseRealPath;
    }

    // Guard: if the stored path is itself a virtual shared-* id (e.g.
    // "/shared-by-me/IMG-...png") instead of a real device path, treat it
    // as missing so getPreview does not build a broken /p2p/ URL.
    if (baseRealPath.startsWith("/shared-by-me/") || baseRealPath.startsWith("/shared-to-me/")) {
      baseRealPath = "";
    }

    let realPath = baseRealPath;
    if (relativeSubPath) {
      realPath = (baseRealPath.endsWith("/") ? baseRealPath.slice(0, -1) : baseRealPath) + relativeSubPath;
    }

    return {
      section,
      rootShareNode: rootNode,
      rootName,
      relativeSubPath,
      realPath,
      device
    };
  }

  function getFileDeviceAndRealPath(file) {
    if (!file) return null;
    if (file.device && file.realPath) {
      const devPrefix = file.prefix || file.device.prefix || ("/" + (file.device.linkId || ""));
      const cleanPrefix = devPrefix.startsWith("/") ? devPrefix : "/" + devPrefix;
      let cleanRealPath = file.realPath;
      if (cleanRealPath.startsWith(cleanPrefix + "/")) {
        cleanRealPath = cleanRealPath.substring(cleanPrefix.length);
      } else if (cleanRealPath === cleanPrefix) {
        cleanRealPath = "/";
      }
      return {
        device: file.device,
        prefix: cleanPrefix,
        realPath: cleanRealPath.startsWith("/") ? cleanRealPath : "/" + cleanRealPath,
        isShared: !!file.isShared
      };
    }
    if (!file.id) return null;
    const fileId = typeof file.id === "string" ? file.id : "";

    if (fileId.startsWith("/shared-by-me/") || fileId.startsWith("/shared-to-me/")) {
      const resolved = resolveSharedPath(fileId);
      if (resolved && resolved.device && resolved.realPath) {
        return {
          device: resolved.device,
          prefix: resolved.device.prefix,
          realPath: resolved.realPath,
          rootShareNode: resolved.rootShareNode,
          isShared: true
        };
      }
    }

    const parsed = splitDeviceId(fileId);
    if (parsed) {
      let device = resolveDevice(parsed.prefix);
      if (!device && file.linkId) device = deviceRegistry.get(file.linkId);
      if (!device && activeDeviceId) device = deviceRegistry.get(activeDeviceId);
      if (!device) {
        const allDevs = deviceRegistry.list();
        if (allDevs.length > 0) device = allDevs[0];
      }
      if (device) {
        return {
          device,
          prefix: device.prefix || ("/" + device.linkId),
          realPath: parsed.path,
          isShared: false
        };
      }
    } else {
      const dev = (file.linkId && deviceRegistry.get(file.linkId)) ||
                  (activeDeviceId && deviceRegistry.get(activeDeviceId)) ||
                  deviceRegistry.list()[0];
      if (dev) {
        return {
          device: dev,
          prefix: dev.prefix || ("/" + dev.linkId),
          realPath: fileId.startsWith("/") ? fileId : "/" + fileId,
          isShared: false
        };
      }
    }

    return null;
  }

  async function ensureSharedFolder(id) {
    const resolved = resolveSharedPath(id);
    if (!resolved || !resolved.rootShareNode) return;

    const { rootShareNode, realPath } = resolved;

    // Make sure the dedicated scoped tunnel for this shortcut is live
    // before issuing any file RPC on it.
    let device = resolved.device;
    if (rootShareNode.shareId && resolved.section === "shared-to-me" && !(device && device.isShareDrive)) {
      try {
        await ensureShareDrive({
          identity,
          linkId: rootShareNode.linkId,
          shareId: rootShareNode.shareId,
          path: rootShareNode.path || rootShareNode.name,
          writable: rootShareNode.writable
        });
        const reResolved = resolveSharedPath(id);
        device = reResolved && reResolved.device;
      } catch (err) {
        console.error("[App] Share drive unavailable:", rootShareNode.shareId, err);
        showToast((err && err.message) || "Shared folder unavailable");
        const shortId = `/shared-to-me/${rootShareNode.name || rootShareNode.value || ""}`;
        if (window._folderItems) delete window._folderItems[shortId];
        if (fmApi) {
          try { fmApi.exec("provide-data", { id: shortId, data: [] }); } catch (_) {}
        }
        return;
      }
    }

    if (!realPath || !device || !device.drive) return;

    if (window._folderItems && window._folderItems[id]) {
      if (fmApi) {
        fmApi.exec("provide-data", { id: id, data: window._folderItems[id] });
      }
    }

    _beginFolderFetch();
    try {
      const res = await device.drive.getFiles(realPath);
      if (res && fmApi) {
        const remapped = res
          .filter(f => f && typeof f === "object" && (f.isDir || f.type === "folder" || !(f.name || f.value || f.id || "").toLowerCase().endsWith(".univer.json")))
          .map(f => {
            const isDir = !!(f.isDir || f.type === "folder");
            const childName = f.name || (f.id ? f.id.split("/").filter(Boolean).pop() : "item");
            const baseId = id.endsWith("/") ? id.slice(0, -1) : id;
            const childVirtualId = `${baseId}/${childName}`;

            return {
              ...f,
              id: childVirtualId,
              value: childName,
              name: childName,
              parent: id,
              type: isDir ? "folder" : "file",
              isDir: isDir,
              realPath: f.id || (realPath.endsWith("/") ? realPath + childName : realPath + "/" + childName),
              linkId: rootShareNode.linkId || device.linkId,
              shareId: rootShareNode.shareId,
              shareAddress: rootShareNode.shareAddress
            };
          });

        if (!window._folderItems) window._folderItems = {};
        window._folderItems[id] = remapped;
        fmApi.exec("provide-data", { id: id, data: remapped });
      }
    } catch (err) {
      console.error("[App] Error loading shared folder:", id, err);
    } finally {
      _endFolderFetch();
    }
  }

  // ── Service Sections Architecture (Shared to me & Shared by me at bottom) ──
  const SERVICE_SECTIONS = [
    {
      id: "/shared-to-me",
      value: "Shared to me",
      name: "Shared to me",
      type: "folder",
      isDir: true,
      open: true,
      parent: "/",
      isDevice: false,
      itemType: "shared",
      isRootSection: true,
      icon: "wxi-folder-shared"
    },
    {
      id: "/shared-by-me",
      value: "Shared by me",
      name: "Shared by me",
      type: "folder",
      isDir: true,
      open: true,
      parent: "/",
      isDevice: false,
      itemType: "shared",
      isRootSection: true,
      icon: "wxi-folder-links"
    },
    {
      // "Чаты" launcher (below shared sections in the sidebar): an app-type
      // entry, never a location - clicking it opens the chat applet directly
      // (no folder navigation). The status dot shows the target node state.
      id: "/chats",
      value: "chats",
      name: "chats",
      type: "file",
      isDir: false,
      open: false,
      parent: "/",
      isDevice: false,
      itemType: "app",
      mimeType: "application/x-chat",
      isRootSection: true,
      icon: "wxi-chat"
    }
  ];

  let sharedByMeNodes = [];
  let sharedToMeNodes = [];
  let deviceNodes = [];

  let _updatingRootData = false;
  function updateRootFilesData() {
    if (_updatingRootData) return;
    _updatingRootData = true;
    try {
      const cleanDeviceNodes = deviceNodes
        .filter(d => d && typeof d === "object" && d.id && (d.online || d.local))
        .map(d => ({ ...d, type: d.type || "folder" }));

      const allDeviceItems = [];
      if (window._deviceItems) {
        for (const [prefix, items] of Object.entries(window._deviceItems)) {
          const devNode = cleanDeviceNodes.find(d => d.id === prefix);
          if (!devNode) continue;
          if (Array.isArray(items)) {
            for (const item of items) {
              if (item && typeof item === "object" && item.id) {
                allDeviceItems.push({
                  ...item,
                  type: item.type || (item.isDir ? "folder" : "file")
                });
              }
            }
          }
        }
      }

      const cleanServiceSections = SERVICE_SECTIONS
        .filter(s => s && typeof s === "object" && s.id)
        .map(s => ({ ...s, type: s.type || "folder" }));

      const cleanSharedToMe = sharedToMeNodes
        .filter(s => s && typeof s === "object" && s.id)
        .map(s => ({ ...s, type: s.type || (s.isDir ? "folder" : "file") }));

      const cleanSharedByMe = sharedByMeNodes
        .filter(s => s && typeof s === "object" && s.id)
        .map(s => ({ ...s, type: s.type || (s.isDir ? "folder" : "file") }));

      const rawFilesData = [
        ...cleanDeviceNodes,        // Connected devices ON TOP!
        ...allDeviceItems,          // Child items of connected devices!
        ...cleanServiceSections,    // Shared folders AT BOTTOM!
        ...cleanSharedToMe,
        ...cleanSharedByMe
      ];
      const seenFiles = new Set();
      filesData = rawFilesData.filter(f => {
        if (!f || !f.id) return false;
        if (seenFiles.has(f.id)) return false;
        seenFiles.add(f.id);
        return true;
      });

      if (fmApi) {
        fmApi.exec("provide-data", { id: "/", data: [...cleanDeviceNodes, ...cleanServiceSections] });
        for (const dev of cleanDeviceNodes) {
          const items = window._deviceItems ? window._deviceItems[dev.id] : null;
          if (items && Array.isArray(items)) {
            const cleanItems = items
              .filter(i => i && typeof i === "object" && i.id)
              .map(i => ({ ...i, type: i.type || (i.isDir ? "folder" : "file") }));
            fmApi.exec("provide-data", { id: dev.id, data: cleanItems });
          }
        }
        fmApi.exec("provide-data", { id: "/shared-to-me", data: cleanSharedToMe });
        fmApi.exec("provide-data", { id: "/shared-by-me", data: cleanSharedByMe });

        if (window._folderItems) {
          for (const [folderId, items] of Object.entries(window._folderItems)) {
            if (folderId !== "/" && !folderId.startsWith("/shared-") && Array.isArray(items)) {
              const cleanItems = items
                .filter(i => i && typeof i === "object" && i.id)
                .map(i => ({ ...i, type: i.type || (i.isDir ? "folder" : "file") }));
              fmApi.exec("provide-data", { id: folderId, data: cleanItems });
            }
          }
        }
      }
    } finally {
      _updatingRootData = false;
    }
  }

  function updateSharedByMeNodes(shares) {
    const usedIds = new Set();
    sharedByMeNodes = (shares || [])
      .filter(s => s && typeof s === "object" && (s.shareId || s.id))
      .map((s) => {
        const shareId = s.shareId || s.id;
        const address = s.address || (identity && identity.mainPub ? `${identity.mainPub}.${shareId}` : shareId);
        const shareUrl = sanitizeShareUrl(s.shareUrl, address);

        let fileName = s.name;
        if (!fileName || fileName === "Item" || fileName === "Shared Item" || fileName === shareId) {
          if (s.path && !s.path.startsWith("/shared-")) {
            fileName = s.path.split("/").filter(Boolean).pop();
          }
        }
        if (!fileName || fileName === "Item" || fileName === "Shared Item") {
          fileName = shareId;
        }

        let baseId = "/shared-by-me/" + fileName;
        let nodeId = baseId;
        let count = 1;
        while (usedIds.has(nodeId)) {
          nodeId = `${baseId}_${count++}`;
        }
        usedIds.add(nodeId);

        const isDirectory = !!(s.isDir === true || s.isDir === "true" || s.type === "folder");

        return {
          id: nodeId,
          value: fileName,
          name: fileName,
          type: isDirectory ? "folder" : "file",
          isDir: isDirectory,
          size: Number(s.size) || 0,
          date: s.createdAt ? new Date(s.createdAt) : new Date(),
          parent: "/shared-by-me",
          shareId: shareId,
          shareAddress: address,
          shareUrl: shareUrl,
          path: s.path || "",
          linkId: s.linkId || "",
          itemType: isDirectory ? "folder" : "file"
        };
      });

    if (fmApi) {
      fmApi.exec("provide-data", { id: "/shared-by-me", data: sharedByMeNodes });
    }
    updateRootFilesData();
  }

  async function reloadSharedByMe() {
    try {
      const shares = await loadSharedByMe(identity);
      updateSharedByMeNodes(shares);
    } catch (err) {
      console.error("[App] reloadSharedByMe error:", err);
    }
  }

  async function reloadSharedToMe() {
    try {
      const shares = await loadSharedToMe(identity);
      const usedIds = new Set();
      sharedToMeNodes = (shares || [])
        .filter(s => s && typeof s === "object" && s.address)
        .map((s) => {
          const rawName = s.name && s.name !== "Item" && s.name !== "Shared Item"
            ? s.name
            : (s.path ? s.path.split("/").filter(Boolean).pop() : s.address);
          const fileName = rawName || s.address;

          let baseId = "/shared-to-me/" + fileName;
          let nodeId = baseId;
          let count = 1;
          while (usedIds.has(nodeId)) {
            nodeId = `${baseId}_${count++}`;
          }
          usedIds.add(nodeId);

          const isDirectory = !!(s.isDir === true || s.isDir === "true" || s.type === "folder");
          const shareId = s.shareId || (s.address ? s.address.split(".").pop() : "");
          const ext = fileName.includes(".") ? fileName.split(".").pop().toLowerCase() : "";

          return {
            id: nodeId,
            value: fileName,
            name: fileName,
            type: isDirectory ? "folder" : "file",
            isDir: isDirectory,
            size: Number(s.size) || 0,
            date: s.addedAt ? new Date(s.addedAt) : new Date(),
            parent: "/shared-to-me",
            shareAddress: s.address,
            shareId,
            shareUrl: s.shareUrl || "",
            linkId: s.linkId || "",
            path: s.path || "",
            ownerName: s.ownerName || "External User",
            writable: !!s.writable,
            mimeType: s.mimeType || "",
            ext,
            icon: isDirectory ? "wxi-folder-shared" : "",
            itemType: isDirectory ? "folder" : "file",
            isSharedShortcut: true
          };
        });
      updateRootFilesData();
      warmUpShareDrives();
    } catch (err) {
      console.error("[App] reloadSharedToMe error:", err);
    }
  }

  let _warmUpTimer = null;
  function warmUpShareDrives() {
    const candidates = (sharedToMeNodes || []).filter(n => n.shareId && n.linkId);
    if (!candidates.length) return;
    if (_warmUpTimer) clearTimeout(_warmUpTimer);
    candidates.forEach((n, i) => {
      setTimeout(() => {
        ensureShareDrive({
          identity,
          linkId: n.linkId,
          shareId: n.shareId,
          path: n.path || n.name,
          writable: n.writable,
          isWarmUp: true
        })
          .then(() => {
            updateRootFilesData();
            try {
              if (fmApi) fmApi.exec("provide-data", { id: "/shared-to-me", data: sharedToMeNodes });
            } catch (_) {}
          })
          .catch((err) => {
            console.warn("[App] Share drive warm-up failed:", n.shareId, err && err.message ? err.message : err);
          });
      }, i * 200);
    });
  }

  async function handleSharedToMeAdded(e) {
    const detail = (e && e.detail) || {};
    showAddShareModal = false;
    await reloadSharedToMe();
    showToast(
      translate("added_to_shared_to_me", {}, "Added to Shared to me") +
      (detail.name ? `: ${detail.name}` : (detail.address ? `: ${detail.address}` : ""))
    );
    try {
      if (fmApi) fmApi.exec("set-path", { id: "/shared-to-me" });
    } catch (_) {}
  }

  // Stable device namespaces: linkId -> prefix segment ("MyPhone"), cached so
  // reconnects never rename the device or orphan the active folder path.
  let _devicePrefixCache = {};
  try {
    _devicePrefixCache = JSON.parse(localStorage.getItem("omd_device_prefixes") || "{}") || {};
  } catch (e) {
    _devicePrefixCache = {};
  }

  function saveDevicePrefixCache() {
    try {
      localStorage.setItem("omd_device_prefixes", JSON.stringify(_devicePrefixCache));
    } catch (e) { /* storage unavailable — in-memory cache still applies */ }
  }

  // True if "/<seg>" is occupied by an entry that does NOT belong to this device.
  function foreignIdCollision(seg, linkId, ownPrefixes = []) {
    const id = "/" + seg;
    const idLower = id.toLowerCase();
    if (deviceRegistry.list().some(d => d && d.linkId && d.linkId !== linkId && (
      (d.prefix && d.prefix.toLowerCase() === idLower) ||
      (d.local && d.name && ("/" + d.name).toLowerCase() === idLower)
    ))) {
      return true;
    }
    if (deviceNodes.some(d => d && d.id && d.id.toLowerCase() === idLower && d.linkId && d.linkId !== linkId)) {
      return true;
    }
    return filesData.some(f => f && f.id === id && f.linkId && f.linkId !== linkId && !ownPrefixes.some(p =>
      p && typeof p === "string" && (f.id === p || f.id.startsWith(p + "/"))
    ));
  }

  // URL-safe namespace segment derived from the device hostname, deduplicated
  // against OTHER devices only.
  function stableDevicePrefix(name, linkId, ownPrefixes = []) {
    let seg = (name || "").trim().replace(/[^A-Za-z0-9._-]+/g, "-");
    if (!seg) seg = linkId;

    if (!foreignIdCollision(seg, linkId, ownPrefixes)) {
      _devicePrefixCache[linkId] = seg;
      saveDevicePrefixCache();
      return "/" + seg;
    }

    // Only if a different device already has the exact same name:
    seg = seg + "-" + (linkId.length > 4 ? linkId.slice(-4) : linkId);
    _devicePrefixCache[linkId] = seg;
    saveDevicePrefixCache();
    return "/" + seg;
  }


  let _isRestoringFolder = false;

  // After a (re)connect, keep viewing the folder the user was in: rewrite
  // currentFolderId onto the device's current namespace (its prefix may differ
  // from the one stored in the path), refresh the data chain for it and point
  // the filemanager back at the corrected id. Returns the corrected id or null.
  async function restoreActiveFolder(device) {
    if (!fmApi || !currentFolderId || currentFolderId === "/") return null;
    const parsed = splitDeviceId(currentFolderId);
    if (!parsed) return null;
    const dev = resolveDevice(parsed.prefix);
    if (!dev || dev.linkId !== device.linkId) return null;
    const targetId = device.prefix + (parsed.path === "/" ? "" : parsed.path);
    if (targetId !== currentFolderId) {
      console.log("[App] Remapping active folder after reconnect:", currentFolderId, "->", targetId);
      currentFolderId = targetId;
    }
    try {
      _isRestoringFolder = true;
      _folderLoadCancelled = false;
      const fullPathKey = (device.prefix || "") + ":" + (parsed.path || "/");
      _loadingFolderPromises.delete(fullPathKey);
      _folderFailures.delete(fullPathKey);

      await ensureFolderAndParents(device, parsed.path);

      // Force SVAR to refresh the view even if path is identical
      const cached = window._folderItems?.[targetId] || window._deviceItems?.[targetId];
      if (cached && fmApi) {
        fmApi.exec("provide-data", { id: targetId, data: cached });
      }
      fmApi.exec("set-path", { id: targetId });
      return targetId;
    } catch (e) {
      console.warn("[App] Folder restore failed:", e);
      return null;
    } finally {
      setTimeout(() => { _isRestoringFolder = false; }, 300);
    }
  }

  function getPreview(file, w, h) {
    if (!file) return null;
    const vs = getViewerSettings();
    if (!vs.generateThumbs) return null;
    const target = getFileDeviceAndRealPath(file);
    if (!target) return null;
    const { prefix, realPath: path } = target;
    const devPrefix = prefix.startsWith("/") ? prefix : "/" + prefix;
    const p2pUrl = `/p2p${devPrefix}` + encodeURI(path.startsWith("/") ? path : "/" + path);
    if (file.hasCover || file.customAttributes?.cover) {
      return p2pUrl + `?cover=${w || 240}`;
    }
    if (!vs.downscaleImages) {
      return p2pUrl;
    }
    const targetDim = Math.min(w || 240, vs.scaleTo);
    const ext = (file.name || file.value || "").split('.').pop().toLowerCase();
    const isWebImage = ["jpg", "jpeg", "png", "gif", "webp", "svg", "avif"].includes(ext);
    const isVideo = ["mp4", "mov", "avi", "mkv", "webm", "mpeg", "mpg"].includes(ext);

    if (file.mimeType && file.mimeType.startsWith('video/')) {
      return p2pUrl + `?cover=${targetDim}`;
    } else if (file.mimeType && file.mimeType.startsWith('image/') && isWebImage) {
      return p2pUrl + `?resize=true&width=${targetDim}&height=${targetDim}`;
    }
    if (isVideo) {
      return p2pUrl + `?cover=${targetDim}`;
    }
    if (isWebImage) {
      return p2pUrl + `?resize=true&width=${targetDim}&height=${targetDim}`;
    }
    return null;
  }

  function getExtraInfo(file) {
    if (!file.customAttributes) return null;
    const attrs = file.customAttributes;
    const info = {};
    if (attrs.title) info["Title"] = attrs.title;
    if (attrs.description) info["Description"] = attrs.description;
    if (attrs.videoDuration) info["Duration"] = attrs.videoDuration;
    if (attrs.videoDimensions) info["Dimensions"] = attrs.videoDimensions;
    if (attrs.artist) info["Artist"] = attrs.artist;
    if (attrs.album) info["Album"] = attrs.album;
    return Object.keys(info).length > 0 ? info : null;
  }

  function addDevice() {
    const linkId = targetLinkId.trim();
    if (!linkId) return;
    const existing = deviceRegistry.get(linkId);
    if (existing && existing.drive && existing.drive.isConnected && existing.drive.authLevel === "admin") {
      console.log("Device already connected:", linkId);
      return;
    }
    openPairModal(linkId);
  }

  const connectingLinkIds = new Set();

  async function connectDevice(linkId, existingDrive = null, customName = "") {
    if (!linkId || linkId === "undefined") {
      console.warn("[App] connectDevice ignored invalid linkId:", linkId);
      return;
    }
    if (linkId === getOrCreateLocalLinkId()) return;
    // AI pseudo device: connection and guest pairing are self-managed
    // (syncPublicAiDevice); generic handler would open the pairing modal.
    if (deviceRegistry.get(linkId)?.isAiNode) return;
    if (connectingLinkIds.has(linkId)) {
      console.log("[App] Connection attempt already in progress for:", linkId);
      return;
    }
    const existing = deviceRegistry.get(linkId);
    if (existing && existing.local) return;
    if (existing && existing.drive && existing.drive.isConnected && !existingDrive) {
      console.log("[App] Device already registered & connected:", linkId);
      if (customName && (existing.name === linkId || !existing.name)) {
        existing.name = customName;
        existing.prefix = "/" + customName;
        deviceNodes = deviceNodes.map(d => d.linkId === linkId ? { ...d, name: customName, value: customName, id: existing.prefix } : d);
        updateRootFilesData();
      }
      return;
    }

    connectingLinkIds.add(linkId);
    setTimeout(() => connectingLinkIds.delete(linkId), 5000);

    if (existing) {
      if (customName && (existing.name === linkId || !existing.name)) {
        existing.name = customName;
        existing.prefix = "/" + customName;
        deviceNodes = deviceNodes.map(d => d.linkId === linkId ? { ...d, name: customName, value: customName, id: existing.prefix } : d);
        updateRootFilesData();
      }
      if (existingDrive) {
        existing.drive = existingDrive;
      }
      setupDeviceStatusHandler(existing);
      if (existing.drive && existing.drive.isConnected) {
        existing.drive.onStatusChange?.("connected");
        return;
      }
      status = "connecting";
      try {
        await existing.drive.connect();
      } catch (err) {
        console.error("[App] Failed connecting device:", linkId, err);
      }
      return;
    }
    status = "connecting";
    const drive = existingDrive || new WebRTCDataChannelDrive(linkId);
    if (isSharedRoute) window.__omdSharedDrive = drive;
    drive.didPub = identity?.mainPub || null;
    const initialName = customName || (drive && drive.rootName) || (identity && identity.devices && identity.devices.get(linkId)?.name) || linkId;
    const device = {
      deviceId: linkId,
      linkId,
      name: initialName,
      prefix: "/" + initialName,

      // carry published metadata when available (e.g. type "web" for
      // browser-as-device) so remote UI can adapt its actions
      type: (identity && identity.devices && identity.devices.get(linkId)?.type) || "",
      capabilities: (identity && identity.devices && identity.devices.get(linkId)?.capabilities) || ["files"],
      drive,
      // Паринг через PairModal: admin-хэндшейк (пароль владельца) = права
      // владельца; гостевой (DID без пароля) = знания из личного GunDB, а не
      // из ChromaDB ноды. Безопасный дефолт: isOwner === false, пока admin
      // не доказан (см. computedPrivateMode в ChatApplet).
      isOwner: !!(drive?.authLevel === "admin"),
      status: drive.isConnected ? "connected" : "connecting",
    };
    setupDeviceStatusHandler(device);
    deviceRegistry.add(linkId, device);

    if (device.drive.isConnected) {
      device.drive.onStatusChange("connected");
    } else {
      try {
        await device.drive.connect();
      } catch (err) {
        console.error(err);
        device.status = "error";
        status = "error";
      }
    }
  }


  async function loadFolder(drive, path) {
    if (!drive) return [];
    if (!drive.isConnected || !drive.dc || drive.dc.readyState !== "open") {
      let isReady = false;
      if (typeof drive.waitUntilConnected === "function") {
        isReady = await drive.waitUntilConnected(3000);
      }
      if (!isReady && (!drive.isConnected || !drive.dc || drive.dc.readyState !== "open")) {
        console.log("[App] loadFolder called while drive is disconnected, attempting auto-reconnect...");
        await drive.reconnect();
      }
    }
    _beginFolderFetch();
    try {
      const res = await fetchSegmentWithRetry(drive, path);
      return res || [];
    } catch (err) {
      console.error("Failed to load folder:", path, err);
      if (err && String(err.message || err).includes("Authentication required") && !isSharedRoute) {
        console.warn("[App] loadFolder skipped; device requires authentication:", drive.targetLinkId || drive.linkId);
      }
      return [];
    } finally {
      _endFolderFetch();
    }
  }

  let _loadingFolderPromises = new Map();
  let _folderLoadCancelled = false;
  let _folderFailures = new Map();
  const FOLDER_RETRY_COOLDOWN = 15000;

  async function fetchSegmentWithRetry(drive, folderPath, attempts = 2) {
    for (let i = 0; i < attempts; i++) {
      if (_folderLoadCancelled) return null;
      try {
        return await drive.getFiles(folderPath);
      } catch (err) {
        if (_folderLoadCancelled) return null;
        if (i === attempts - 1) throw err;
        console.warn("[App] Folder fetch failed, retrying once:", folderPath, err && err.message ? err.message : err);
        await new Promise(r => setTimeout(r, 700));
        try {
          if (!drive.isConnected || !drive.dc || drive.dc.readyState !== "open") {
            await drive.reconnect();
          }
        } catch (e) {
          console.warn("[App] Drive recheck before retry failed:", e);
        }
      }
    }
  }

  async function ensureFolderAndParents(device, path) {
    if (!device || !device.drive) return;

    _folderLoadCancelled = false;
    const fullPathKey = (device.prefix || "") + ":" + (path || "/");
    if (_loadingFolderPromises.has(fullPathKey)) {
      return _loadingFolderPromises.get(fullPathKey);
    }

    const runLoad = async () => {
      _beginFolderFetch();
      try {
        if (!device.drive.isConnected || !device.drive.dc || device.drive.dc.readyState !== "open") {
          const ok = await device.drive.reconnect();
          if (!ok || !device.drive.isConnected || !device.drive.dc || device.drive.dc.readyState !== "open") {
            console.warn("[App] ensureFolderAndParents: drive not connected after reconnect, aborting:", fullPathKey);
            return;
          }
        }

        // If device is not yet authenticated with admin rights from this browser, do not fetch files
        if (!isSharedRoute && device.drive && device.drive.authLevel !== "admin" && !device.isAiNode && !device.local) {
          console.log("[App] ensureFolderAndParents skipped for unauthenticated device:", device.linkId);
          return;
        }

        const parts = path ? path.split("/").filter(Boolean) : [];
        const pathsToLoad = [];
        if (!window._deviceItems || !window._deviceItems[device.prefix]) {
          pathsToLoad.push("/");
        }
        let curr = "";
        for (const p of parts) {
          curr += "/" + p;
          if (curr !== "/" && !pathsToLoad.includes(curr)) {
            pathsToLoad.push(curr);
          }
        }
        if (pathsToLoad.length === 0) {
          pathsToLoad.push("/");
        }

        for (const folderPath of pathsToLoad) {
          if (_folderLoadCancelled) break;
          const fullId = device.prefix + (folderPath === "/" ? "" : folderPath);
          if (folderPath !== path && window._folderItems && window._folderItems[fullId]) {
            continue;
          }
          try {
            const res = await fetchSegmentWithRetry(device.drive, folderPath);
            if (_folderLoadCancelled) break;
            if (res === null) break;
            if (res && fmApi) {
              const remapped = res
                .filter(f => f && typeof f === "object" && (f.isDir || f.type === "folder" || !(f.name || f.value || f.id || "").toLowerCase().endsWith(".univer.json")))
                .map(f => ({
                  ...f,
                  type: f.type || (f.isDir ? "folder" : "file"),
                  id: device.prefix + f.id,
                }));
              if (!window._deviceItems) window._deviceItems = {};
              if (!window._folderItems) window._folderItems = {};
              let finalItems = remapped;
              if (folderPath === "/") {
                const cachedApps = getCachedNodeAppItems(device.linkId);
                if (cachedApps) {
                  finalItems = mergeAppsIntoRootItems(device.prefix, remapped, cachedApps);
                }
                window._deviceItems[device.prefix] = finalItems;
                updateRootFilesData();
                refreshDeviceNodeApps(device);
              }
              window._folderItems[fullId] = finalItems;
              fmApi.exec("provide-data", { id: fullId, data: finalItems });
            }
            _folderFailures.delete(fullPathKey);
          } catch (err) {
            console.error("[App] Error in ensureFolderAndParents for segment:", folderPath, err);
            _folderFailures.set(fullPathKey, Date.now());
          }
        }
      } finally {
        _endFolderFetch();
      }
    };

    const loadPromise = (async () => {
      let deadlineTimer;
      const deadline = new Promise((_, reject) => {
        deadlineTimer = setTimeout(() => reject(new Error("Folder load deadline exceeded")), 90000);
      });
      try {
        await Promise.race([runLoad(), deadline]);
      } catch (err) {
        console.warn("[App] ensureFolderAndParents aborted:", fullPathKey, err && err.message ? err.message : err);
        _folderFailures.set(fullPathKey, Date.now());
      } finally {
        clearTimeout(deadlineTimer);
        downloadingFile = null;
        isFolderLoading = false;
      }
    })();

    _loadingFolderPromises.set(fullPathKey, loadPromise);
    try {
      await loadPromise;
    } finally {
      _loadingFolderPromises.delete(fullPathKey);
    }
  }

  async function disconnectDevice(linkId) {
    if (!linkId) return;
    const dev = deviceRegistry.get(linkId) || deviceRegistry.getByPrefix(linkId);
    const prefix = dev ? dev.prefix : "/" + linkId;
    const isLocal = dev && dev.local;
    
    // 1. Remove from device registry and disconnect drive
    closeAppProxyDrivesForNode(linkId);
    deviceRegistry.delete(linkId);
    if (dev && dev.linkId && dev.linkId !== linkId) {
      deviceRegistry.delete(dev.linkId);
    }
    if (dev && dev.prefix && dev.prefix !== linkId) {
      deviceRegistry.delete(dev.prefix);
    }
    
    // 2. Remove from filesData, deviceNodes and local cached items
    filesData = filesData.filter(f => !f.id.startsWith(prefix) && f.linkId !== linkId && f.id !== prefix && (!dev || f.id !== dev.prefix));
    deviceNodes = deviceNodes.filter(d => d.linkId !== linkId && d.id !== prefix && (!dev || d.id !== dev.prefix));
    
    if (window._deviceItems) {
      delete window._deviceItems[prefix];
      if (dev && dev.prefix) delete window._deviceItems[dev.prefix];
      delete window._deviceItems["/" + linkId];
      delete window._deviceItems[linkId];
      for (const k of Object.keys(window._deviceItems)) {
        if (k === prefix || k.startsWith(prefix + "/") || (dev?.prefix && (k === dev.prefix || k.startsWith(dev.prefix + "/")))) {
          delete window._deviceItems[k];
        }
      }
    }
    if (window._folderItems) {
      for (const k of Object.keys(window._folderItems)) {
        if (k === prefix || k.startsWith(prefix + "/") || (dev?.prefix && (k === dev.prefix || k.startsWith(dev.prefix + "/")))) {
          delete window._folderItems[k];
        }
      }
    }

    // If it was a local web device, clear stored shares so it doesn't auto-restore on reload
    if (isLocal) {
      await clearAllLocalShares();
    }
    
    // 3. Remove from persistent identity (GunDB & localStorage)
    if (identity && identity.type === "persistent") {
      try {
        await identity.removeDevice(linkId);
      } catch (err) {
        console.warn("[App] identity.removeDevice error:", err);
      }
    }

    // 4. Update SVAR
    if (fmApi) {
      try {
        fmApi.exec("provide-data", { id: "/", data: filesData });
      } catch (e) {
        console.warn("[App] fmApi provide-data error on disconnect:", e);
      }
    }
    updateRootFilesData();
  }

  let downloadingFile = null;
  let downloadProgress = 0;
  let docLoadLabel = null; // slim top progress for document opens

  // WordCraft could not start (WebGPU/WebGL2 unavailable): reopen the same file
  // through the old Univer/pandoc path. Skipped for good once per file.
  function handleWordCraftFailed(e) {
    const rid = (e && e.detail && e.detail.path) || (viewerFile && viewerFile.path);
    if (rid) wcUnsupportedIds.add(rid);
    console.warn("[App] WordCraft unavailable (WebGPU/WebGL2?), falling back to Univer for", rid);
    const last = lastOfficeClick;
    closeViewer(null);
    if (last && last.file) handleFileOpen(last.file, last.parentPath || "/");
  }
  let downloadSpeed = "";
  let downloadAbortController = null;
  let isFolderLoading = false;
  let toastMessage = "";
  let toastTimeout = null;

  function showToast(msg) {
    if (toastTimeout) clearTimeout(toastTimeout);
    toastMessage = msg;
    toastTimeout = setTimeout(() => {
      toastMessage = "";
      toastTimeout = null;
    }, 3000);
  }
  
  function formatSpeed(bytesPerSec) {
    if (bytesPerSec === 0 || isNaN(bytesPerSec)) return "0 B/s";
    const k = 1024;
    const sizes = ['B/s', 'KB/s', 'MB/s', 'GB/s'];
    const i = Math.floor(Math.log(bytesPerSec) / Math.log(k));
    return parseFloat((bytesPerSec / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
  }
  
  $: activeDevice = activeDeviceId ? deviceRegistry.get(activeDeviceId) : null;
  $: {
    if (typeof window !== "undefined") {
      // Exact match returns the drive as-is (local drives are served
      // natively by the SW bridge); fallbacks must stay RPC-capable.
      const hasRpc = (d) => d && typeof d.sendRPC === "function";
      const connectedRpcDrive = () => deviceRegistry.list().map(d => d.drive).find((d) => d && d.isConnected && hasRpc(d)) || null;
      window.__omdGetDrive = (deviceId) => {
        if (!deviceId) {
          return ((activeDevice && hasRpc(activeDevice.drive)) ? activeDevice.drive : null) || connectedRpcDrive();
        }
        const shareEntry = getShareDriveByPrefix(deviceId);
        if (shareEntry && shareEntry.drive) return shareEntry.drive;
        const device = deviceRegistry.getByPrefix(deviceId) || deviceRegistry.get(deviceId) || deviceRegistry.getByName(deviceId);
        if (device && device.drive) return device.drive;
        return ((activeDevice && hasRpc(activeDevice.drive)) ? activeDevice.drive : null) || connectedRpcDrive();
      };
      window.currentP2PDrive = ((activeDevice && hasRpc(activeDevice.drive)) ? activeDevice.drive : null) || connectedRpcDrive();
    }
  }

  // HttpDrive instance for file manager semantic search and AI operations
  let fmHttpDrive = null;
  $: fmTargetLinkId = activeDeviceId || activeDevice?.linkId || deviceRegistry.list().find(d => d.drive?.isConnected)?.linkId || (chatTargetDevice?.linkId) || (defaultChatTarget()?.linkId) || deviceRegistry.list()[0]?.linkId || "";
  // `isOwner === true`, не `!== false`: устройство без явного флага (в т.ч.
  // публичная нода) не должно получать X-OMD-Private-Mode: 1.
  // Клауза `!activeDeviceId` убрана намеренно: пустой id означает «нода выбрана
  // по умолчанию» (defaultChatTarget -> публичная AI-нода), а не владение, и
  // из-за неё гость публичной ноды получал owner-режим и пикер чужих файлов.
  $: fmPrivateMode =
      (identity && identity.devices && !!identity.devices.get(activeDeviceId)) ||
      (sharedToMeNodes && sharedToMeNodes.some(s => s.linkId === activeDeviceId)) ||
      (activeDevice?.isOwner === true && !isSharedRoute);
  $: currentOmdKey = (identity && identity.profile && identity.profile.omd_key) || getStoredOmdKey() || "";

  $: {
    if (!fmHttpDrive || fmHttpDrive.targetLinkId !== fmTargetLinkId || fmHttpDrive.isPrivateMode !== fmPrivateMode) {
      if (fmHttpDrive) fmHttpDrive.close();
      if (fmTargetLinkId) {
        fmHttpDrive = new HttpDrive({
          targetLinkId: fmTargetLinkId,
          port: 4000,
          isPrivateMode: fmPrivateMode,
          authToken: currentOmdKey
        });
      } else {
        fmHttpDrive = null;
      }
    } else if (currentOmdKey && fmHttpDrive.authToken !== currentOmdKey) {
      fmHttpDrive.setAuth(currentOmdKey);
    }
    if (typeof window !== "undefined") {
      window.__omdHttpDrive = fmHttpDrive;
      window.__omdGetHttpDrive = (devOrId) => {
        let dev = typeof devOrId === "object" ? devOrId : resolveDevice(devOrId);
        if (!dev && typeof devOrId === "string") {
          dev = deviceRegistry.get(devOrId) || deviceRegistry.getByPrefix(devOrId) || { linkId: devOrId };
        }
        return getHttpDriveForDevice(dev || chatTargetDevice || defaultChatTarget());
      };
    }
  }

  // Agentic inference routing headers (X-OMD-Ai-Mode / X-OMD-Ai-Node) on the
  // filemanager http drive (Creations image endpoints go through it).
  $: {
    const aiHeaders = aiOnmychatHeaders($aiNodeStore);
    const normalized = aiHeaders && Object.keys(aiHeaders).length ? aiHeaders : null;
    const aiKey = normalized ? JSON.stringify(normalized) : "";
    if (fmHttpDrive && fmHttpDrive.__aiHeadersKey !== aiKey) {
      fmHttpDrive.__aiHeadersKey = aiKey;
      fmHttpDrive.setAiHeaders(normalized);
    }
  }

  // Multi-node search support across devices
  const httpDrivePool = new Map();

  function getHttpDriveForDevice(dev) {
    if (!dev || !dev.linkId) return null;
    // Пустой linkId уже отсечён выше; владение определяется только флагами.
    const isPrivate =
      (identity && identity.devices && !!identity.devices.get(dev.linkId)) ||
      (sharedToMeNodes && sharedToMeNodes.some(s => s.linkId === dev.linkId)) ||
      (dev.isOwner === true && !isSharedRoute);
    const key = (identity && identity.profile && identity.profile.omd_key) || getStoredOmdKey() || "";

    // Reuse already-warm fmHttpDrive connection pool when targeting the same node & mode
    if (fmHttpDrive && !fmHttpDrive.closed && fmHttpDrive.targetLinkId === dev.linkId && fmHttpDrive.isPrivateMode === isPrivate) {
      if (key && fmHttpDrive.authToken !== key) {
        fmHttpDrive.setAuth(key);
      }
      return fmHttpDrive;
    }

    let drv = httpDrivePool.get(dev.linkId);
    if (!drv || drv.closed || drv.isPrivateMode !== isPrivate) {
      if (drv) drv.close();
      drv = new HttpDrive({
        targetLinkId: dev.linkId,
        port: 4000,
        isPrivateMode: isPrivate,
        authToken: key
      });
      httpDrivePool.set(dev.linkId, drv);
    } else if (key && drv.authToken !== key) {
      drv.setAuth(key);
    }
    return drv;
  }

  function getSearchDrives(currentPath = "/") {
    // 1. If inside a specific device folder, target only that device
    if (currentPath && currentPath !== "/") {
      const parsed = splitDeviceId(currentPath);
      if (parsed) {
        const dev = resolveDevice(parsed.prefix);
        if (dev) {
          const drv = getHttpDriveForDevice(dev);
          return drv ? [{ drive: drv, device: dev }] : [];
        }
      }
    }

    // 2. If at root ("/"), fan out to all connected devices supporting chat or search
    const allDevs = deviceRegistry.list();
    const chatCapable = allDevs.filter(d => {
      if (!d) return false;
      const isConnected = d.status === "connected" || d.drive?.isConnected;
      if (!isConnected) return false;
      const hasChat = Array.isArray(d.capabilities) && (d.capabilities.includes("chat") || d.capabilities.includes("search"));
      return hasChat || d.isAiNode;
    });

    const targetDevs = chatCapable.length > 0
      ? chatCapable
      : allDevs.filter(d => d && (d.status === "connected" || d.drive?.isConnected));

    const result = [];
    for (const dev of targetDevs) {
      const drv = getHttpDriveForDevice(dev);
      if (drv) {
        result.push({ drive: drv, device: dev });
      }
    }
    return result;
  }

  $: {
    if (typeof window !== "undefined") {
      window.__omdGetSearchDrives = getSearchDrives;
    }
  }

  // ── Custom Rich Web Push Sync & Pre-Moderation Handler ─────────
  let pushSubscriptionSyncedOnce = false;
  let nodePushToast = "";
  let nodePushToastTimer = null;
  let showNodeWidgetModal = false;
  let nodeWidgetLoading = false;
  let nodeWidgetBusy = false;
  let nodeWidgetError = "";
  let nodeWidgetHostEl = null;
  let nodeWidgetCleanup = null;
  let activeNodeWidgetKey = "";
  let nodeWidgetReqSeq = 0;

  function showNodePushToast(msg, duration = 4500) {
    nodePushToast = msg;
    if (nodePushToastTimer) clearTimeout(nodePushToastTimer);
    nodePushToastTimer = setTimeout(() => {
      nodePushToast = "";
    }, duration);
  }

  function isSafeNodeRelativePath(p) {
    return typeof p === "string" && p.startsWith("/") && !p.startsWith("//");
  }

  function closeNodeWidgetModal() {
    nodeWidgetReqSeq++;
    activeNodeWidgetKey = "";
    if (typeof nodeWidgetCleanup === "function") {
      try { nodeWidgetCleanup(); } catch (_) {}
    }
    nodeWidgetCleanup = null;
    if (nodeWidgetHostEl) {
      nodeWidgetHostEl.innerHTML = "";
    }
    nodeWidgetBusy = false;
    nodeWidgetLoading = false;
    nodeWidgetError = "";
    showNodeWidgetModal = false;
  }

  function getPublicAiLinkId() {
    return (
      ($aiNodeStore && $aiNodeStore.info && $aiNodeStore.info.linkID) ||
      (typeof localStorage !== "undefined" && (localStorage.getItem("omd_public_ai_link_id") || localStorage.getItem("omd_public_node_link_id"))) ||
      ""
    ).trim();
  }

  function extractPushNodeNameHint(extra) {
    const explicit = String((extra && extra.node_name) || "").trim();
    if (explicit) return explicit.toLowerCase();
    let st = String((extra && extra.default_storage) || "").trim();
    st = st.replace(/^\/p2p\//i, "").replace(/^\/+/, "");
    const firstSeg = (st.split("/")[0] || "").trim().toLowerCase();
    if (firstSeg && firstSeg !== "local" && firstSeg !== "this device") {
      return firstSeg;
    }
    return "";
  }

  function collectOwnedPushCandidates(extra) {
    const pubId = getPublicAiLinkId();
    const localId = typeof window !== "undefined" ? getOrCreateLocalLinkId() : "";
    const nameHint = extractPushNodeNameHint(extra);
    const exactId = String((extra && extra.node_link_id) || "").trim();
    const byId = new Map();

    const addCand = (linkId, name = "", basePriority = 30, forceOwner = true) => {
      const id = String(linkId || "").trim();
      if (!id || (localId && id === localId)) return;
      const inIdentity = !!(identity && identity.devices && (identity.devices.has?.(id) || identity.devices.get?.(id)));
      if (pubId && id === pubId && id !== exactId && !inIdentity) return;

      const regDev = deviceRegistry.get(id);
      if (regDev && (regDev.local || regDev.type === "web" || regDev.type === "phone" || regDev.type === "mobile")) {
        return;
      }
      const isLiveConnected = !!(regDev && (regDev.status === "connected" || regDev.drive?.isConnected));
      const regName = regDev ? (regDev.name || regDev.rootName || regDev.prefix || "") : "";
      const cleanName = String(name || regName || "").replace(/^\/+/, "").trim().toLowerCase();
      const matchesName = !!(nameHint && cleanName && (cleanName === nameHint || cleanName.startsWith(nameHint + ".")));

      // Strongly prioritize live-connected nodes in deviceRegistry so stale/offline GunDB entries
      // (or older notifications carrying an inactive linkId for the same host) never stall ahead of an open tunnel.
      let effPrio = basePriority;
      if (id === exactId && isLiveConnected) effPrio = 0;
      else if (matchesName && isLiveConnected) effPrio = 1;
      else if (id === exactId) effPrio = 2;
      else if (isLiveConnected && regDev && isChatCapableDevice(regDev)) effPrio = Math.min(effPrio, 4);
      else if (matchesName) effPrio = Math.min(effPrio, 6);
      else if (isLiveConnected) effPrio = Math.min(effPrio, 10);

      const prev = byId.get(id);
      if (!prev || effPrio < prev.priority) {
        byId.set(id, {
          linkId: id,
          isOwner: forceOwner || inIdentity,
          isConnected: isLiveConnected,
          priority: effPrio
        });
      }
    };

    // 1. Active deviceRegistry entries FIRST (already hydrated & live-connected when app is open)
    for (const d of deviceRegistry.list()) {
      if (!d || !d.linkId || d.local || d.type === "web" || d.type === "phone" || d.type === "mobile") continue;
      if (d.isAiNode && !d.isOwner) continue;
      const dName = d.name || d.rootName || d.prefix || "";
      const prio = isChatCapableDevice(d) ? 15 : 25;
      addCand(d.linkId, dName, prio, d.isOwner === true || d.drive?.authLevel === "admin");
    }

    if (exactId) {
      addCand(exactId, nameHint, 2, true);
    }

    // 2. Owned devices from DID (identity.devices) — skip web/mobile client entries
    try {
      if (identity && identity.devices) {
        const iter = typeof identity.devices.entries === "function"
          ? identity.devices.entries()
          : Object.entries(identity.devices);
        for (const [k, meta] of iter) {
          if (!k || meta?.isLocal || meta?.type === "web" || meta?.type === "phone" || meta?.type === "mobile") continue;
          addCand(k, meta?.name || "", 30, true);
        }
      }
    } catch (_) {}

    // 3. Shared-to-me nodes
    for (const s of sharedToMeNodes || []) {
      if (s && s.linkId) addCand(s.linkId, s.name || "", 40, true);
    }

    return Array.from(byId.values()).sort((a, b) => a.priority - b.priority);
  }

  async function resolvePushNodeDrives(extra = {}, maxWaitMs = 6000) {
    const exactId = String((extra && extra.node_link_id) || "").trim();
    const nameHint = extractPushNodeNameHint(extra);
    const start = Date.now();
    // When the app is already open and device discovery finished, never wait more than 1.2s for stragglers
    const effectiveWaitMs = initialDeviceLoadDone ? Math.min(maxWaitMs, 1200) : maxWaitMs;

    while (Date.now() - start < effectiveWaitMs) {
      const cands = collectOwnedPushCandidates(extra);
      // Fast path (0ms): any live-connected match or chat-capable owned node, or explicit node_link_id
      if (cands.some((c) => c.isConnected && c.priority <= 4) || (exactId && cands.length > 0)) {
        return cands.map((c) => getHttpDriveForDevice(c)).filter(Boolean);
      }
      if (nameHint) {
        if (cands.some((c) => c.priority <= 6)) {
          return cands.map((c) => getHttpDriveForDevice(c)).filter(Boolean);
        }
      } else if (cands.length > 0) {
        return cands.map((c) => getHttpDriveForDevice(c)).filter(Boolean);
      }
      await new Promise((r) => setTimeout(r, 120));
    }

    // Final fallback after waiting: try any owned candidates, then chatTargetDevice / defaultChatTarget
    const lateCands = collectOwnedPushCandidates(extra);
    const drives = lateCands.map((c) => getHttpDriveForDevice(c)).filter(Boolean);
    const fallbackDev = chatTargetDevice || defaultChatTarget();
    if (fallbackDev && fallbackDev.linkId) {
      const fbDrv = getHttpDriveForDevice(fallbackDev);
      if (fbDrv && !drives.includes(fbDrv)) drives.push(fbDrv);
    }
    return drives;
  }

  $: if (!pushSubscriptionSyncedOnce && chatTargetDevice && chatTargetDevice.linkId) {
    const drv = getHttpDriveForDevice(chatTargetDevice);
    if (drv) {
      pushSubscriptionSyncedOnce = true;
      const curSt =
        (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
        currentProfileData?.defaultStorage ||
        identity?.profile?.defaultStorage ||
        DEFAULT_STORAGE_PATH;
      const key = (identity && identity.profile && identity.profile.omd_key) || getStoredOmdKey() || "";
      syncExistingWebPushSubscription({
        fetchFn: (p, opts) => drv.fetch(p, opts),
        omdKey: key,
        defaultStorage: curSt
      });
    }
  }

  function buildNodeP2pPath(storageStr, relPath) {
    const cleanRel = String(relPath || "").replace(/^\/+/, "");
    if (!cleanRel) return "";
    let st = String(storageStr || "").trim();
    if (!st) {
      st =
        (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
        currentProfileData?.defaultStorage ||
        identity?.profile?.defaultStorage ||
        "";
    }
    st = st.replace(/^\/p2p\//i, "").replace(/^\/+|\/+$/g, "");
    return st ? `/p2p/${st}/${cleanRel}` : `/p2p/${cleanRel}`;
  }

  async function handlePushActionDispatch(action, payload) {
    const extra = (payload && payload.data) || {};
    const widgetUrl = String(extra.widget_url || "").trim();
    const actionUrl = String(extra.action_url || "").trim();
    const itemId = String(extra.item_id || "").trim();
    const token = String(extra.action_token || "").trim();

    if (action === "open") {
      if (!isSafeNodeRelativePath(widgetUrl)) return;
      const reqKey = `${widgetUrl}::${itemId}`;
      // Deduplicate when foreground pushNotificationReceived already opened/started loading the exact same item
      if (showNodeWidgetModal && activeNodeWidgetKey === reqKey && !nodeWidgetError) {
        return;
      }
      closeNodeWidgetModal();
      const reqSeq = ++nodeWidgetReqSeq;
      activeNodeWidgetKey = reqKey;
      showNodeWidgetModal = true;
      nodeWidgetLoading = true;
      nodeWidgetError = "";

      try {
        const drives = await resolvePushNodeDrives(extra, 6000);
        if (reqSeq !== nodeWidgetReqSeq) return;
        const fetchCandidates = drives.length > 0
          ? drives.map((drv) => (p, opts) => drv.fetch(p, opts))
          : [(p, opts) => window.fetch(p, opts)];
        const key = (identity && identity.profile && identity.profile.omd_key) || getStoredOmdKey() || "";
        const curSt =
          extra.default_storage ||
          (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
          currentProfileData?.defaultStorage ||
          identity?.profile?.defaultStorage ||
          DEFAULT_STORAGE_PATH;

        const q = new URLSearchParams({ item_id: itemId, token, omd_key: key });
        const sep = widgetUrl.includes("?") ? "&" : "?";
        const targetWidgetPath = `${widgetUrl}${sep}${q.toString()}`;

        let res = null;
        let activeFetchFn = fetchCandidates[0];
        let lastErrStatus = "timeout";
        for (const candFetch of fetchCandidates) {
          if (reqSeq !== nodeWidgetReqSeq) return;
          try {
            const candRes = await candFetch(targetWidgetPath, { connectTimeoutMs: 6000, timeoutMs: 10000 });
            if (candRes && candRes.ok) {
              res = candRes;
              activeFetchFn = candFetch;
              break;
            }
            lastErrStatus = candRes ? candRes.status : "timeout";
            if (candRes && candRes.status !== 404) {
              res = candRes;
              break;
            }
          } catch (e) {
            lastErrStatus = e?.message || "network error";
          }
        }
        if (reqSeq !== nodeWidgetReqSeq) return;
        if (!res || !res.ok) {
          throw new Error(`HTTP ${res ? res.status : lastErrStatus}`);
        }
        const scriptCode = await res.text();
        if (reqSeq !== nodeWidgetReqSeq) return;
        nodeWidgetLoading = false;
        await new Promise((r) => setTimeout(r, 0));
        if (!showNodeWidgetModal || !nodeWidgetHostEl || reqSeq !== nodeWidgetReqSeq) return;

        const runner = new Function("host", scriptCode);
        const maybeCleanup = await runner({
          container: nodeWidgetHostEl,
          payload,
          extra,
          omdKey: key,
          defaultStorage: curSt,
          nodeFetch: (p, opts) => {
            if (!isSafeNodeRelativePath(p)) throw new Error("Blocked non-relative node path");
            return activeFetchFn(p, opts);
          },
          buildP2pPath: (st, rel) => buildNodeP2pPath(st, rel),
          showToast: (msg, ms) => showNodePushToast(msg, ms),
          setBusy: (b) => { nodeWidgetBusy = Boolean(b); },
          close: () => closeNodeWidgetModal()
        });
        if (typeof maybeCleanup === "function") {
          nodeWidgetCleanup = maybeCleanup;
        }
      } catch (err) {
        if (reqSeq !== nodeWidgetReqSeq) return;
        nodeWidgetLoading = false;
        nodeWidgetError = translate("push_widget_load_failed", { error: err?.message || err }, "Failed to load node widget: {error}");
      }
      return;
    }

    // Direct action tap from OS notification banner
    if (!isSafeNodeRelativePath(actionUrl)) return;
    showNodePushToast(translate("push_sending_action", {}, "⏳ Sending action to node..."));
    try {
      const drives = await resolvePushNodeDrives(extra, 6000);
      const fetchCandidates = drives.length > 0
        ? drives.map((drv) => (p, opts) => drv.fetch(p, opts))
        : [(p, opts) => window.fetch(p, opts)];
      const key = (identity && identity.profile && identity.profile.omd_key) || getStoredOmdKey() || "";
      let res = null;
      let resData = {};
      for (const candFetch of fetchCandidates) {
        try {
          const candRes = await candFetch(actionUrl, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            connectTimeoutMs: 6000,
            timeoutMs: 12000,
            body: JSON.stringify({
              ...extra,
              item_id: itemId,
              action,
              token,
              omd_key: key
            })
          });
          const candData = candRes ? await candRes.json().catch(() => ({})) : {};
          res = candRes;
          resData = candData;
          if (candRes && (candRes.ok || candRes.status !== 404)) {
            break;
          }
        } catch (_) {}
      }
      if (!res || !res.ok) {
        throw new Error(resData.detail || `HTTP ${res ? res.status : "timeout"}`);
      }
      showNodePushToast(resData.message || translate("push_action_done", {}, "✅ Done!"));
    } catch (err) {
      showNodePushToast(translate("push_action_error", { error: err?.message || err }, "❌ Error: {error}"), 6000);
    }
  }

  $: if (currentApp === 'filemanager') {
    setTimeout(() => {
      window.dispatchEvent(new Event('resize'));
    }, 60);
  }
  
  // Gallery state
  let currentImages = [];
  let currentImageIndex = -1;
  
  // Real device path for the terminal: the folder currently open in the
  // filemanager, but only if it belongs to the target device.
  function computeTerminalPath(device) {
    const parsed = splitDeviceId(currentFolderId);
    if (!parsed) return "";
    const cur = resolveDevice(parsed.prefix);
    if (!cur || cur.linkId !== device.linkId) return "";
    return parsed.path;
  }

  function openShareModal(file) {
    if (!file) return;
    const previewUrl = getPreview(file, 600);
    if (file.id && typeof file.id === "string" && file.id.startsWith("/shared-by-me/")) {
      const dev = file.linkId ? deviceRegistry.get(file.linkId) : null;
      shareTargetDevice = dev || (file.linkId ? { linkId: file.linkId, name: file.linkId } : activeDevice);
      shareTargetItem = {
        ...file,
        preview: previewUrl || file.preview || file.image,
        id: file.path || file.id,
        name: file.name || file.value || "Shared Item",
        linkId: dev?.linkId || file.linkId || activeDevice?.linkId || "",
      };
      showShareModal = true;
      return;
    }

    const resolved = getFileDeviceAndRealPath(file);
    const parsed = splitDeviceId(file.id);
    let dev = resolved?.device || (parsed ? resolveDevice(parsed.prefix) : null) || activeDevice;

    if (!dev) {
      if (file.linkId) dev = deviceRegistry.get(file.linkId);
      if (!dev && activeDeviceId) dev = deviceRegistry.get(activeDeviceId);
      if (!dev) {
        const allDevs = deviceRegistry.list();
        if (allDevs.length === 1) dev = allDevs[0];
      }
    }

    shareTargetDevice = dev || activeDevice;
    const realPath = resolved?.realPath || (parsed ? parsed.path : file.id);
    const rawName = file.name && file.name !== "Item" && file.name !== "Shared Item"
      ? file.name
      : (file.value && file.value !== "Item" && file.value !== "Shared Item" ? file.value : (realPath ? realPath.split("/").filter(Boolean).pop() : "Item"));
    
    shareTargetItem = {
      ...file,
      preview: previewUrl || file.preview || file.image,
      id: realPath,
      name: rawName,
      isDir: !!(file.type === "folder" || file.isDir),
      linkId: dev?.linkId || activeDevice?.linkId || file.linkId || "",
    };
    showShareModal = true;
  }

  function updateOpenGraphMeta(res) {
    if (typeof document === "undefined" || !res) return;
    const brandTitle = getDomainBrandTitle();
    const shareTitle = res.title || res.customAttributes?.title || res.name || "Shared Content";
    const shareDescr = res.description || res.customAttributes?.description || `Shared ${res.isDir ? 'folder' : 'file'} "${shareTitle}" via ${brandTitle} P2P storage`;
    const ownerName = res.ownerName || res.owner || `${brandTitle} User`;
    
    document.title = `${shareTitle} - ${brandTitle}`;

    const setMeta = (attrName, attrVal, content) => {
      let el = document.querySelector(`meta[${attrName}="${attrVal}"]`);
      if (!el) {
        el = document.createElement("meta");
        el.setAttribute(attrName, attrVal);
        document.head.appendChild(el);
      }
      el.setAttribute("content", content);
    };

    setMeta("name", "author", ownerName);
    setMeta("name", "description", shareDescr);

    // Twitter Card data
    setMeta("name", "twitter:card", "summary_large_image");
    setMeta("name", "twitter:title", shareTitle);
    setMeta("name", "twitter:description", shareDescr);
    setMeta("name", "twitter:site", brandTitle);

    // Open Graph data
    setMeta("property", "og:type", "website");
    setMeta("property", "og:title", shareTitle);
    setMeta("property", "og:url", window.location.href);
    setMeta("property", "og:description", shareDescr);
    setMeta("property", "og:site_name", brandTitle);
    setMeta("property", "og:author", "@" + ownerName);

    if (res.linkId && res.path) {
      const devPrefix = `/${res.linkId}`;
      const rawPath = res.path.startsWith("/") ? res.path : "/" + res.path;
      const previewUrl = window.location.origin + `/p2p${devPrefix}` + encodeURI(rawPath) + "?cover=400";
      setMeta("property", "og:image", previewUrl);
      setMeta("property", "og:image:url", previewUrl);
      setMeta("property", "og:image:secure_url", previewUrl);
      setMeta("name", "twitter:image", previewUrl);

      let imgLink = document.querySelector('link[rel="image_src"]');
      if (!imgLink) {
        imgLink = document.createElement("link");
        imgLink.setAttribute("rel", "image_src");
        document.head.appendChild(imgLink);
      }
      imgLink.setAttribute("href", previewUrl);
    }
  }

  async function handleOpenSharedRoute(address, password = "") {
    shareLoading = true;
    shareError = "";
    sharePasswordError = "";

    try {
      const res = await resolveShare(address, password, identity?.gun);

      if (res.requiresPassword) {
        sharePasswordModal = true;
        shareLoading = false;
        return;
      }

      if (res.isExpired) {
        shareError = "This share link has expired.";
        shareLoading = false;
        return;
      }

      resolvedShareData = res;
      sharePasswordModal = false;
      updateOpenGraphMeta(res);

      // Connect to target device via WebRTC ephemeral identity
      identity = new EphemeralIdentity();
      identityType = "ephemeral";
      targetLinkId = res.linkId;
      activeDeviceId = res.linkId;

      await connectDevice(res.linkId);
      const dev = deviceRegistry.get(res.linkId);

      // Explicitly wait for the WebRTC data channel to open (ICE + DTLS may take extra seconds)
      if (dev && dev.drive && !dev.drive.isConnected) {
        console.log("[SharedRoute] Waiting for DataChannel to open...");
        const connected = await dev.drive.waitUntilConnected(20000);
        if (!connected) {
          // Still not connected – set error but keep the drive alive so onStatusChange can recover
          throw new Error("Connection timed out. The device may be temporarily unreachable.");
        }
      }

      // Establish scoped root with host C++ client
      if (dev && dev.drive) {
        try {
          const scopeArg = res.shareId
            ? { shareId: res.shareId, path: res.path, writable: !!res.writable }
            : res.path;
          await dev.drive.setScope(scopeArg);
        } catch (e) {
          console.warn("[SharedRoute] setScope failed:", e);
        }
      }

      status = "connected";
      authorized = true;
      shareError = "";
      mainApp = "singleview";
      currentApp = "singleview";
    } catch (err) {
      console.error("[SharedRoute] Resolution error:", err);
      authorized = false;
      if (sharePasswordModal) {
        sharePasswordError = err.message || "Incorrect password";
      } else {
        shareError = err.message || "Failed to open shared link";
        // Keep resolvedShareData alive – if the drive connects later
        // onStatusChange will pick it up and clear the error automatically
      }
    } finally {
      shareLoading = false;
    }
  }

  // Extend the default context menu with "Open Terminal Here" on folders and "Share Link..." on items.
  // The action itself is dispatched by the patched filemanager Layout.svelte
  function openShareFolderPicker(overrideDev) {
    const parsed = splitDeviceId(currentFolderId || "/");
    const device = overrideDev || (parsed ? resolveDevice(parsed.prefix) : null);
    if (!device) return;
    pickerDevice = device;
    showFolderPicker = true;
  }

  async function addLocalShareViaNativePicker(overrideDev) {
    const parsed = splitDeviceId(currentFolderId || "/");
    const device = overrideDev || (parsed ? resolveDevice(parsed.prefix) : null);
    if (!device) return;
    try {
      if (isFileSystemAccessAvailable()) {
        await addShareFromPicker();
      } else {
        await addOpfsShare();
      }
      fmApi?.exec("request-data", { id: device.prefix });
    } catch (e) {
      if (e?.name === "AbortError" || e?.code === 20) return;
      alert(e?.message || "Failed to add folder");
    }
  }

  function handlePickerSelect({ detail }) {
    const { path, realPath } = detail;
    const targetPath = realPath || path;
    if (!pickerDevice || !pickerDevice.drive || !targetPath) return;
    (async () => {
      try {
        await pickerDevice.drive.addShare(targetPath);
        // Refresh the device root so the new logical share appears
        const deviceRootId = pickerDevice.prefix || "";
        if (fmApi && deviceRootId) fmApi.exec("request-data", { id: deviceRootId });
      } catch (err) {
        console.error("[App] addShare failed:", err);
        alert(err && err.message ? err.message : "Failed to add folder");
      } finally {
        pickerDevice = null;
        showFolderPicker = false;
      }
    })();
  }

  function customMenuOptions(type, item) {
    if (item && (item.id === "/shared-to-me" || item.id === "/shared-by-me" || item.itemType === "shared-to-me" || item.itemType === "shared-by-me" || item.isRootSection)) {
      return [];
    }

    if (type === "add") {
      const activePath = (item && (item.path || item.id)) || currentFolderId || "/";
      if (activePath === "/shared-by-me") {
        return [];
      }
      // "Shared to me" section: the toolbar "+" opens the paste-a-link modal
      if (activePath === "/shared-to-me") {
        return [{
          id: "add-share-link",
          text: translate("add_resource", {}, "Add resource"),
          icon: "wxi-shared",
          handler: () => { showAddShareModal = true; }
        }];
      }
      const isRoot = !activePath || activePath === "/";
      // First level inside a device: only "Add folder" (root share management)
      const parsedCur = splitDeviceId(activePath);
      const isDeviceRoot = !!parsedCur && parsedCur.path === "/" && !!parsedCur.prefix;
      if (isDeviceRoot) {
        const dev = resolveDevice(parsedCur.prefix);
        if (dev?.local) {
          return [{
            id: "add-local-share",
            text: translate("add_folder", {}, "Add folder"),
            icon: "wxi-folder-plus",
            handler: () => addLocalShareViaNativePicker(dev)
          }];
        }
        // Remote web (browser) device: new ids have no prefix - they are
        // published with type "web"; legacy ids carry the "browser-" prefix.
        if (dev?.type === "web" || dev?.linkId?.startsWith("browser-")) {
          return [{
            id: "remote-web-info",
            text: translate("add_folder", {}, "Add folder"),
            icon: "wxi-folder-plus",
            handler: () => { showRemoteDeviceInfo = true; }
          }];
        }
        return [
          {
            id: "add-share-folder",
            text: translate("add_folder", {}, "Add folder"),
            icon: "wxi-folder-plus",
            handler: () => openShareFolderPicker(dev)
          }
        ];
      }
      if (isRoot) {
        return [
          {
            id: "connect-device",
            text: translate("connect_device", {}, "Connect Device"),
            icon: "wxi-desktop",
            handler: () => { showAddDevicePanel = true; }
          }
        ];
      }
      return [
        { icon: "wxi-file-plus", text: translate("add_file", {}, "Add new file"), id: "add-file" },
        { icon: "wxi-folder-plus", text: translate("add_folder", {}, "Add new folder"), id: "add-folder" },
        { icon: "wxi-upload", text: translate("upload", {}, "Upload file"), id: "upload", type: "upload" }
      ];
    }

    const opts = [...getMenuOptions(type)];
    if (item && (item.isDevice || item.itemType === "device")) {
      return [
        {
          id: "open-device-settings",
          text: translate("device_settings", {}, "Properties"),
          icon: "wxi-device"
        },
        {
          id: "disconnect-device",
          text: translate("disconnect_device", {}, "Disconnect Device"),
          icon: "wxi-close"
        }
      ];
    }

    if (
      item &&
      (item.mimeType === APP_PROXY_MIME ||
        item.mimeType === "application/x-terminal" ||
        item.mimeType === "application/x-chat" ||
        item.itemType === "app")
    ) {
      return [
        {
          id: "open-app-item",
          text: translate("open", {}, "Open"),
          icon: "wxi-eye"
        }
      ];
    }

    if (item && item.id && typeof item.id === "string" && item.parent === "/shared-to-me") {
      return [
        { id: "copy-share-url", text: translate("copy_share_url", {}, "Copy Share Link"), icon: "wxi-content-copy" },
        { id: "remove-shared-to-me", text: translate("remove_shared_to_me", {}, "Remove from Shared to me"), icon: "wxi-close" }
      ];
    }

    if (item && item.id && typeof item.id === "string" && item.parent === "/shared-by-me") {
      return [
        { id: "copy-share-url", text: translate("copy_share_url", {}, "Copy Share Link"), icon: "wxi-content-copy" },
        { id: "manage-share", text: translate("manage_share", {}, "Manage Share..."), icon: "wxi-share" },
        { id: "revoke-share", text: translate("revoke_share", {}, "Revoke Share"), icon: "wxi-close" }
      ];
    }

    if (type === "folder" && item && item.type === "folder") {
      const parsed = item.id ? splitDeviceId(item.id) : null;
      const dev = (parsed ? resolveDevice(parsed.prefix) : null) || activeDevice;
      const supportsTerminal = !dev || !dev.capabilities || dev.capabilities.includes("terminal");
      if (supportsTerminal) {
        opts.push({
          id: "terminal-here",
          text: translate("open_terminal", {}, "Open in Terminal"),
          icon: "wxi-terminal"
        });
      }
    }
    if (item && item.id && item.id !== "/" && !item.isDevice && identity && identity.type === "persistent") {
      opts.push({
        id: "share-item",
        text: translate("share_link", {}, "Share Link..."),
        icon: "wxi-share"
      });
    }
    return opts;
  }

  // Called by Filemanager when it needs to expand a folder (lazy loading)
  function initFilemanager(api) {
    fmApi = api;
    
    // Provide initial root data if it was fetched before init
    if (filesData.length > 0) {
      setTimeout(() => {
        api.exec("provide-data", { id: "/", data: filesData.filter(f => f.parent === "/") });
        // Also provide device-level data if pre-fetched
        if (window._deviceItems) {
          for (const [prefix, items] of Object.entries(window._deviceItems)) {
            api.exec("provide-data", { id: prefix, data: items });
          }
        }
        if (typeof window !== "undefined" && window.location.hash && window.location.hash !== "#/" && window.location.hash !== "#") {
          applyStateFromHash();
        }
      }, 0);
    } else if (typeof window !== "undefined" && window.location.hash && window.location.hash !== "#/" && window.location.hash !== "#") {
      setTimeout(() => {
        applyStateFromHash();
      }, 0);
    }

    // Intercept navigation to non-folder entities like "/chats" or unauthenticated storage devices
    api.intercept("set-path", (config) => {
      const id = config && config.id;
      if (typeof id === "string") {
        if (!isRealFileManagerFolder(id)) {
          if (id === "/chats" || id === "/chat" || id === "/on-my-chat" || id === "/on-my-chat/Chats" || id.startsWith("/chat/")) {
            if (isRealFileManagerFolder(currentFolderId)) {
              lastFileManagerFolderId = currentFolderId;
            }
            openChatsApp();
            return false; // Prevent Svar filemanager from setting its internal path to "/chats"
          }
        } else if (id !== "/" && !isSharedRoute) {
          // Do not prompt for pairing during hash synchronization (page refresh / URL change),
          // folder restoration, or before devices have finished initial load.
          if (isSyncingHash || _isRestoringFolder || !initialDeviceLoadDone) {
            return true;
          }
          const parsed = splitDeviceId(id);
          if (parsed) {
            const device = resolveDevice(parsed.prefix);
            // Only prompt if device is connected (handshake finished, DID auto-auth attempted) and unauthenticated
            if (device && device.drive && device.drive.isConnected && device.drive.authLevel !== "admin" && !device.isAiNode && !device.local) {
              console.log("[App] User clicked on unauthenticated storage device, opening PairModal:", device.linkId);
              pairModalTargetFolder = id;
              openPairModal(device.linkId, (device.drive && device.drive.rootName) || (device.name !== device.linkId ? device.name : ""));
              return false; // Prevent navigating into unauthenticated device folder until paired!
            }
          }
        }
      }
      return true;
    });

    // Track the folder the user is currently viewing (navigation through
    // cards/table/tree/breadcrumbs all emit set-path).
    api.on("set-path", async ({ id }) => {
      if (typeof id === "string") {
        if (!isRealFileManagerFolder(id)) {
          if (id === "/chats" || id === "/on-my-chat" || id === "/on-my-chat/Chats") {
            if (isRealFileManagerFolder(currentFolderId)) {
              lastFileManagerFolderId = currentFolderId;
            }
            openChatsApp();
          }
          return;
        }

        currentFolderId = id;
        lastFileManagerFolderId = id;
        if (currentApp === "filemanager" && !isSyncingHash) {
          updateUrlHash(filesHash(id));
        }

        if (id === "/shared-by-me") {
          await reloadSharedByMe();
        } else if (id === "/shared-to-me") {
          await reloadSharedToMe();
        } else if (id.startsWith("/shared-by-me/") || id.startsWith("/shared-to-me/")) {
          await ensureSharedFolder(id);
        } else if (!_isRestoringFolder) {
          const parsed = splitDeviceId(id);
          if (parsed) {
            const device = resolveDevice(parsed.prefix);
            if (device && device.drive) {
              await ensureFolderAndParents(device, parsed.path);
            }
          }
        }
      }
    });

    api.on("disconnect-device", ({ id }) => {
      const file = api.getFile(id);
      const parsed = splitDeviceId(id);
      const dev = (parsed && resolveDevice(parsed.prefix)) || (file && file.linkId && deviceRegistry.get(file.linkId));
      const linkId = (dev && dev.linkId) || (file && file.linkId);
      const name = (dev && dev.name) || (file && (file.name || file.value)) || linkId;
      if (linkId) {
        deviceToDisconnect = { linkId, name };
        showConfirmDisconnectModal = true;
      }
    });

    // Dispatched by the patched filemanager Layout.svelte when the
    // "Open Terminal Here" context menu item is clicked.
    api.on("open-terminal", ({ id }) => {
      const file = api.getFile(id);
      if (!file || file.type !== "folder") return;
      const parsed = splitDeviceId(file.id);
      if (!parsed) return;
      const device = resolveDevice(parsed.prefix);
      if (!device) return;
      terminalPort = 22;
      terminalInitialPath = parsed.path;
      activeDeviceId = device.linkId;
      currentApp = "terminal";
    });

    // Dispatched by the patched filemanager Layout.svelte when the
    // "Share Link..." context menu item is clicked.
    api.on("share-item", ({ id }) => {
      const file = api.getFile(id);
      if (!file) return;
      openShareModal(file);
    });

    api.on("copy-share-url", ({ id }) => {
      const file = api.getFile(id);
      const shareId = (file && file.shareId) ? file.shareId : (typeof id === "string" ? id.split("/").pop() : null);
      const storedUrl = file?.shareUrl;
      const fallback = file?.shareAddress ? sharedLinkUrl(file.shareAddress) : (shareId && identity?.mainPub ? sharedLinkUrl(`${identity.mainPub}.${shareId}`) : "");
      const shareUrl = storedUrl ? (sanitizeShareUrl(storedUrl, file?.shareAddress) || fallback) : fallback;
      if (shareUrl) {
        if (typeof navigator !== "undefined" && navigator.clipboard) {
          navigator.clipboard.writeText(shareUrl);
          showToast("Share link copied to clipboard!");
        }
      }
    });

    api.on("manage-share", ({ id }) => {
      const file = api.getFile(id);
      if (file) openShareModal(file);
    });

    api.on("revoke-share", ({ id }) => {
      const file = api.getFile(id);
      const targetShareId = (file && file.shareId) ? file.shareId : (typeof id === "string" ? id.split("/").pop() : null);
      if (targetShareId) {
        const itemName = (file && (file.name || file.value)) ? (file.name || file.value) : targetShareId;
        confirmRevokeShare(targetShareId, itemName);
      }
    });

    api.on("remove-shared-to-me", async ({ id }) => {
      const file = api.getFile(id);
      const address = file && (file.shareAddress || file.address);
      if (!address) return;
      const itemName = (file && (file.name || file.value)) ? (file.name || file.value) : address;
      const confirmed = confirm(
        translate("remove_shared_to_me_confirm", {}, "Remove from 'Shared to me'?") +
        `\n${itemName}`
      );
      if (!confirmed) return;
      await removeSharedToMe(identity, address);
      if (file.shareId) closeShareDrive(file.shareId);
      if (window._folderItems) delete window._folderItems[`/shared-to-me/${itemName}`];
      await reloadSharedToMe();
      showToast(translate("removed_shared_to_me", {}, "Removed from Shared to me"));
    });

    api.on("request-data", async ({ id, refresh }) => {
      if (!isRealFileManagerFolder(id)) return;
      if (id === "/" || id === "/shared-by-me" || id === "/shared-to-me") {
        if (id === "/" && refresh) {
          await reconnectDisconnectedDevices();
        } else if (id === "/shared-by-me" && refresh) {
          reloadSharedByMe();
        } else if (id === "/shared-to-me" && refresh) {
          reloadSharedToMe();
        }
        // Always answer section navigation with the current nodes so the
        // panel does not sit in its "fetching" state (spinner) forever.
        if (fmApi && (id === "/shared-by-me" || id === "/shared-to-me")) {
          const sectionNodes = (id === "/shared-to-me" ? sharedToMeNodes : sharedByMeNodes)
            .filter(n => n && typeof n === "object" && n.id)
            .map(n => ({ ...n, type: n.type || (n.isDir ? "folder" : "file") }));
          fmApi.exec("provide-data", { id, data: sectionNodes });
        }
        return;
      }
      if (id.startsWith("/shared-by-me/") || id.startsWith("/shared-to-me/")) {
        await ensureSharedFolder(id);
        return;
      }
      const parsed = splitDeviceId(id);
      if (!parsed) return;
      const device = resolveDevice(parsed.prefix);
      if (!device || !device.drive) return;
      if (!isSharedRoute && device.drive.authLevel !== "admin" && !device.isAiNode && !device.local) {
        if (fmApi) fmApi.exec("provide-data", { id, data: [] });
        return;
      }

      const failKey = device.prefix + ":" + parsed.path;
      if (refresh) {
        _folderFailures.delete(failKey);
        const fullId = device.prefix + (parsed.path === "/" ? "" : parsed.path);
        if (window._folderItems) delete window._folderItems[fullId];
        if (typeof device.drive.clearCache === "function") {
          try { await device.drive.clearCache(); } catch (_) {}
        }
        if (parsed.path === "/") {
          refreshDeviceNodeApps(device, { force: true });
        }
      } else {
        const lastFail = _folderFailures.get(failKey) || 0;
        if (Date.now() - lastFail < FOLDER_RETRY_COOLDOWN) {
          console.log("[App] request-data suppressed after recent failure:", failKey);
          return;
        }
      }
      await ensureFolderAndParents(device, parsed.path);
    });

    api.intercept("open-file", async (config) => {
      const file = api.getFile(config.id) || config.file;
      if (file && (file.id === "/chats" || file.itemType === "chat" || file.mimeType === "application/x-chat")) {
        if (isRealFileManagerFolder(currentFolderId)) {
          lastFileManagerFolderId = currentFolderId;
        }
        openChatsApp();
        return false;
      }
      if (file && file.type !== "folder") {
        const parentPath = typeof file.parent === "string" ? file.parent : (file.parent ? file.parent.id : "");
        if (file.id && String(file.id).startsWith("/shared-to-me")) {
          const res = resolveSharedPath(file.id);
          const root = res && res.rootShareNode;
          if (root && root.shareId) {
            try {
              await ensureShareDrive({
                identity,
                linkId: root.linkId,
                shareId: root.shareId,
                path: root.path || root.name,
                writable: root.writable
              });
            } catch (err) {
              console.error("[App] open-file share drive unavailable:", root.shareId, err);
              showToast((err && err.message) || "Shared file unavailable");
              return false;
            }
          }
        }
        await handleFileOpen(file, parentPath);
        return false; // prevent default
      }
      return true; // let folder open normally
    });

    api.intercept("download-file", async (config) => {
      const file = api.getFile(config.id);
      if (file && file.type === "file") {
        await handleFileDownload(file);
        return false; // prevent default
      }
      return true;
    });

    // --- File mutation interceptors: P2P backend calls ---

    function resolveDriveAndPath(id) {
      const parsed = splitDeviceId(id);
      if (!parsed) {
        const shared = resolveSharedPath(id);
        if (shared && shared.device && shared.device.drive && shared.realPath) {
          return { drive: shared.device.drive, path: shared.realPath, prefix: shared.device.prefix };
        }
        return null;
      }
      const device = resolveDevice(parsed.prefix);
      return device?.drive ? { drive: device.drive, path: parsed.path, prefix: parsed.prefix } : null;
    }

async function blobToBase64(blob) {
      const buf = await blob.arrayBuffer();
      const bytes = new Uint8Array(buf);
      let bin = "";
      for (let i = 0; i < bytes.length; i += 0x8000) {
        bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
      }
      return btoa(bin);
    }

    api.intercept("copy-files", async (config) => {
      if (config.skipProvider) return true;
      try {
        const src = resolveDriveAndPath(config.ids[0]);
        const tgt = resolveDriveAndPath(config.target);
        if (!src || !tgt) return true;

        if (src.prefix === tgt.prefix) {
          for (const id of config.ids) {
            const s = resolveDriveAndPath(id);
            if (!s) continue;
            await src.drive.copyFile(s.path, tgt.path);
          }
        } else {
          for (const id of config.ids) {
            const s = resolveDriveAndPath(id);
            if (!s) continue;
            const fileName = s.path.split("/").pop();
            const blob = await src.drive.getFileContent(s.path);
            const b64 = await blobToBase64(blob);
            await tgt.drive.homeWrite(tgt.path + "/" + fileName, b64);
          }
        }
        fmApi.exec("request-data", { id: config.target });
      } catch (err) {
        console.error("[App] copy-files error:", err);
        showToast("Copy failed: " + err.message);
        fmApi.exec("request-data", { id: config.target });
      }
      return true;
    });

    api.intercept("move-files", async (config) => {
      if (config.skipProvider) return true;
      try {
        const src = resolveDriveAndPath(config.ids[0]);
        const tgt = resolveDriveAndPath(config.target);
        if (!src || !tgt) return true;

        if (src.prefix === tgt.prefix) {
          for (const id of config.ids) {
            const s = resolveDriveAndPath(id);
            if (!s) continue;
            await src.drive.moveFile(s.path, tgt.path);
          }
        } else {
          for (const id of config.ids) {
            const s = resolveDriveAndPath(id);
            if (!s) continue;
            const fileName = s.path.split("/").pop();
            const blob = await src.drive.getFileContent(s.path);
            const b64 = await blobToBase64(blob);
            await tgt.drive.homeWrite(tgt.path + "/" + fileName, b64);
            await src.drive.removeFile(s.path);
          }
        }
        fmApi.exec("request-data", { id: config.target });
        const srcParent = config.ids[0].split("/").slice(0, -1).join("/") || "/";
        if (srcParent !== config.target) {
          fmApi.exec("request-data", { id: srcParent });
        }
      } catch (err) {
        console.error("[App] move-files error:", err);
        showToast("Move failed: " + err.message);
        fmApi.exec("request-data", { id: config.target });
      }
      return true;
    });

    api.intercept("rename-file", async (config) => {
      try {
        const target = resolveDriveAndPath(config.id);
        if (!target) return true;
        await target.drive.renameFile(target.path, config.name);
      } catch (err) {
        console.error("[App] rename-file error:", err);
        showToast("Rename failed: " + err.message);
        const parsed = splitDeviceId(config.id);
        if (parsed) fmApi.exec("request-data", { id: "/" + parsed.prefix + parsed.path.split("/").slice(0, -1).join("/") });
      }
      return true;
    });

    api.intercept("delete-files", async (config) => {
      try {
        for (const id of (config.ids || [])) {
          const target = resolveDriveAndPath(id);
          if (!target) continue;
          await target.drive.removeFile(target.path, true);

          const parsed = splitDeviceId(id);
          if (parsed) {
            const isDeviceRootShare = !parsed.path.slice(1).includes("/");
            if (isDeviceRootShare) {
              if (window._deviceItems) delete window._deviceItems[parsed.prefix];
              fmApi?.exec("request-data", { id: parsed.prefix, refresh: true });
              updateRootFilesData();
            } else {
              const parentPath = "/" + parsed.prefix + parsed.path.split("/").slice(0, -1).join("/");
              if (window._folderItems) delete window._folderItems[parentPath];
              fmApi?.exec("request-data", { id: parentPath, refresh: true });
            }
          }
        }
      } catch (err) {
        console.error("[App] delete-files error:", err);
      }
      return true;
    });

    // Empty-folder CTA in the file manager: add a folder to this (web) device.
    api.intercept("add-local-folder", () => {
      addFolderOnThisDevice();
      return false;
    });

    api.intercept("create-file", async (config) => {
      try {
        const parentId = config.parent || currentFolderId || "/";
        const target = resolveDriveAndPath(parentId);
        if (!target || !target.drive) return true;

        const fileName = config.file?.name;
        if (!fileName) return true;

        const isFolder = config.file?.type === "folder";
        const fullPath = target.path === "/" ? "/" + fileName : target.path + "/" + fileName;

        if (isFolder) {
          if (config.file) config.file.lazy = true;
          if (typeof target.drive.mkDir === "function") {
            await target.drive.mkDir(fullPath);
          } else if (typeof target.drive.homeMkDir === "function") {
            await target.drive.homeMkDir(fullPath);
          }
        } else if (config.file?.file && config.file.file instanceof Blob) {
          const b64 = await blobToBase64(config.file.file);
          if (typeof target.drive.writeFileChunked === "function") {
            await target.drive.writeFileChunked(fullPath, b64);
          } else if (typeof target.drive.homeWrite === "function") {
            await target.drive.homeWrite(fullPath, b64);
          }
        } else {
          if (typeof target.drive.writeFileChunked === "function") {
            await target.drive.writeFileChunked(fullPath, "");
          } else if (typeof target.drive.homeWrite === "function") {
            await target.drive.homeWrite(fullPath, "");
          }
        }

        // Invalidate cache and refresh folder contents from device
        if (window._folderItems && window._folderItems[parentId]) {
          delete window._folderItems[parentId];
        }
        if (typeof target.drive.clearCache === "function") {
          try { await target.drive.clearCache(); } catch (_) {}
        }
        fmApi.exec("request-data", { id: parentId, refresh: true });
      } catch (err) {
        console.error("[App] create-file error:", err);
        showToast("Failed: " + err.message);
        return false;
      }
      return true;
    });

    api.on("open-app-item", async ({ id }) => {
      const file = api.getFile(id);
      if (file) {
        const parentPath = typeof file.parent === "string" ? file.parent : (file.parent ? file.parent.id : "");
        await handleFileOpen(file, parentPath);
      }
    });

    // Device properties modal
    api.on("open-device-settings", ({ id }) => {
      const parsed = splitDeviceId(id);
      if (!parsed) return;
      const device = resolveDevice(parsed.prefix);
      if (!device?.drive) return;
      deviceSettingsDrive = device.drive;
      deviceSettingsName = device.name || parsed.prefix;
      // use same icon/type as file tree (wxi-desktop / wxi-embedded etc)
      const node = deviceNodes.find(n => n.linkId === device.linkId) || filesData.find(f => f.linkId === device.linkId);
      deviceSettingsIcon = node?.icon || device.icon || (device.deviceType ? `wxi-${device.deviceType}` : "wxi-embedded");
      deviceSettingsType = node?.deviceType || device.deviceType || "";
      showDeviceSettings = true;
    });
  }

  function resolveSourceFileModel(src) {
    if (!src) return null;
    let raw = "";
    if (typeof src === "string") {
      raw = src;
      src = { id: src, path: src, fullPath: src, realPath: src };
    } else {
      raw = src.fullPath || src.path || src.realPath || src.itemPath || src.document_id || src.id || src.url || "";
    }
    if (typeof raw !== "string") raw = String(raw || "");

    // If it's a full URL (http:// or https://), strip protocol and domain
    if (raw.startsWith("http://") || raw.startsWith("https://")) {
      try {
        const u = new URL(raw);
        raw = decodeURIComponent(u.pathname);
      } catch (_) {
        raw = raw.replace(/^https?:\/\/[^\/]+/, "");
      }
    } else {
      try {
        raw = decodeURIComponent(raw);
      } catch (_) {}
    }

    let segments = raw.split("/").filter(Boolean);

    // 1. Strip user / owner prefix if present (e.g. "alexey", "tester", or logged in identity)
    const owner = src.owner || (identity && identity.user) || "";
    if (segments.length > 0) {
      if ((owner && segments[0].toLowerCase() === owner.toLowerCase()) || 
          (identity && identity.user && segments[0].toLowerCase() === identity.user.toLowerCase())) {
        segments.shift();
      }
    }

    // 2. Resolve device:
    let dev = (src.device && src.device.drive) ? src.device : null;
    if (segments.length > 0) {
      const candidate = segments[0];
      const matchedDev = deviceRegistry.getByPrefix(candidate) ||
                         deviceRegistry.get(candidate) ||
                         deviceRegistry.getByName(candidate);
      if (matchedDev) {
        if (!dev) dev = matchedDev;
        segments.shift();
      } else if (dev) {
        const devPrefix = (dev.prefix || dev.linkId || "").replace(/^\//, "");
        if (devPrefix && candidate.toLowerCase() === devPrefix.toLowerCase()) {
          segments.shift();
        }
      }
    }

    // Fallbacks for device resolution:
    if (!dev && src.linkId) {
      dev = deviceRegistry.get(src.linkId) || deviceRegistry.getByPrefix(src.linkId) || deviceRegistry.getByName(src.linkId);
    }
    if (!dev && (src.targetDevice || src.targetLinkId)) {
      dev = src.targetDevice || (src.targetLinkId ? (deviceRegistry.get(src.targetLinkId) || deviceRegistry.getByPrefix(src.targetLinkId)) : null);
    }
    if (!dev && activeDeviceId) {
      dev = deviceRegistry.get(activeDeviceId) || deviceRegistry.getByPrefix(activeDeviceId) || deviceRegistry.getByName(activeDeviceId);
    }
    if (!dev && (chatTargetDevice || defaultChatTarget())) {
      dev = chatTargetDevice || defaultChatTarget();
    }
    if (!dev) {
      const allDevs = deviceRegistry.list();
      if (segments.length > 0 && window._deviceItems) {
        for (const d of allDevs) {
          const rootItems = window._deviceItems[d.prefix] || [];
          if (rootItems.some(item => (item.name === segments[0] || item.value === segments[0] || item.id?.endsWith("/" + segments[0])))) {
            dev = d;
            break;
          }
        }
      }
      if (!dev && allDevs.length > 0) {
        dev = allDevs[0];
      }
    }

    const realPath = "/" + segments.join("/");
    const fileName = src.name || src.value || src.title || (segments.length > 0 ? segments[segments.length - 1] : "file");
    const linkId = dev ? dev.linkId : (src.targetLinkId || activeDeviceId || (chatTargetDevice?.linkId) || "");
    const mimeType = src.mimeType || src.mimetype || src.contentType || "";

    return {
      id: "/" + linkId + realPath,
      name: fileName,
      value: fileName,
      mimeType,
      type: "file",
      realPath,
      linkId,
      device: dev,
      prefix: dev && dev.prefix ? dev.prefix : ("/" + linkId),
      size: src.size || 0
    };
  }

  function handleOpenFolderEvent(detail) {
    if (!detail || !fmApi) return;
    const id = (detail.id && String(detail.id).startsWith("/"))
      ? detail.id
      : (detail.path || detail.fullPath || detail.realPath || detail.document_id || "");
    if (!id || !String(id).startsWith("/")) return;
    if (currentApp === "chat") previousApp = "chat";
    currentApp = "filemanager";
    fmApi.exec("set-path", { id });
  }

  function handleOpenFileEvent(detail) {
    if (!detail) return;
    const folderSrc = (detail.source && typeof detail.source === "object")
      ? detail.source
      : (detail.file && typeof detail.file === "object") ? detail.file : detail;
    if (folderSrc && (folderSrc.isDir === true || folderSrc.isDirectory === true ||
        folderSrc.type === "folder" || folderSrc.sType === "folder" ||
        folderSrc.contentType === "folder" || folderSrc.is_folder === true || folderSrc.isFolder === true)) {
      handleOpenFolderEvent(folderSrc);
      return;
    }
    let src = detail;
    if (detail.file && typeof detail.file === "object") {
      src = { ...detail.file };
      if (typeof detail.source === "string" && !src.path && !src.realPath && !src.fullPath) {
        src.realPath = detail.source;
      }
    } else if (detail.source) {
      src = detail.source;
    }
    const fileModel = resolveSourceFileModel(src);
    if (!fileModel) return;

    if (currentApp === "chat") {
      previousApp = "chat";
    }

    const parentPath = (typeof src === "object" && (src.parent || src.parentPath)) || "";
    handleFileOpen(fileModel, parentPath);
  }

  async function handleFileOpen(file, parentPath = "/") {
    try {
      // "Чаты" app entry at the root level: launch the applet, no file resolution
      if (file && (file.id === "/chats" || file.itemType === "chat")) {
        openChatsApp();
        return;
      }
      const target = getFileDeviceAndRealPath(file);
      if (!target) return;
      const { device, prefix, realPath: realId } = target;

      let realParent = "/";
      if (parentPath && parentPath !== "/" && parentPath !== "/shared-by-me" && parentPath !== "/shared-to-me") {
        const parentTarget = getFileDeviceAndRealPath({ id: parentPath });
        if (parentTarget && parentTarget.realPath) {
          realParent = parentTarget.realPath;
        } else {
          const lastSlash = realId.lastIndexOf("/");
          realParent = lastSlash > 0 ? realId.substring(0, lastSlash) : "/";
        }
      } else {
        const lastSlash = realId.lastIndexOf("/");
        realParent = lastSlash > 0 ? realId.substring(0, lastSlash) : "/";
      }

      const devPrefix = prefix.startsWith("/") ? prefix : "/" + prefix;
      const cleanRealId = realId.startsWith("/") ? realId : "/" + realId;
      const p2pUrl = `/p2p${devPrefix}` + encodeURI(cleanRealId);
      const mime = file.mimeType || "";
      const fileName = file.name || file.value || "";
      const ext = fileName.includes('.') ? fileName.split('.').pop().toLowerCase() : "";
      
      if (mime === "application/x-settings") {
        alert("Settings modal coming soon!");
        return;
      }
      if (mime === "application/x-terminal") {
        terminalPort = file.customAttributes && file.customAttributes.port ? file.customAttributes.port : 22;
        activeDeviceId = device.linkId;
        terminalInitialPath = computeTerminalPath(device);
        currentApp = "terminal";
        return;
      }
      if (mime === "application/x-chat") {
        activeDeviceId = device.linkId;
        currentApp = "chat";
        return;
      }
      if (mime === APP_PROXY_MIME) {
        const attrs = file.customAttributes || {};
        openProxiedWebApp(device, {
          slug: attrs.appSlug || fileName.toLowerCase(),
          name: attrs.appName || fileName || attrs.appSlug,
          port: attrs.port || 0,
          entryPath: attrs.entryPath || "/",
          icon: attrs.icon || attrs.appSlug || ""
        });
        return;
      }

      if (mime.startsWith("image/") || ["jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "svg"].includes(ext)) {
        // Use LightGallery for images
        try {
            isGalleryLoading = true;
            const folderData = await device.drive.getFiles(realParent);
            const images = folderData.filter(f => {
                if (f.isDir || f.type === "folder") return false;
                const fMime = (f.mimeType || f.type || "").toLowerCase();
                const fName = f.name || f.value || "";
                const fExt = fName.includes('.') ? fName.split('.').pop().toLowerCase() : "";
                return fMime.startsWith("image/") || ["jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "svg"].includes(fExt);
            });
            
            // Match current File Manager panel order if open, else natural sort by name
            let fmOrderMap = new Map();
            if (fmApi) {
              try {
                const fmState = fmApi.getState();
                const pIdx = fmState.activePanel || 0;
                const curPanel = fmState.panels && fmState.panels[pIdx];
                const pFiles = curPanel?._files || curPanel?.data || [];
                pFiles.forEach((item, idx) => {
                  const iName = item.value || item.name || item.id || "";
                  if (iName) fmOrderMap.set(iName, idx);
                  if (item.id) fmOrderMap.set(item.id, idx);
                  if (item.realPath) fmOrderMap.set(item.realPath, idx);
                });
              } catch (_) {}
            }

            images.sort((a, b) => {
              const nameA = a.name || a.value || "";
              const nameB = b.name || b.value || "";
              if (fmOrderMap.size > 0) {
                const idxA = fmOrderMap.has(a.id) ? fmOrderMap.get(a.id) : (fmOrderMap.has(nameA) ? fmOrderMap.get(nameA) : 99999);
                const idxB = fmOrderMap.has(b.id) ? fmOrderMap.get(b.id) : (fmOrderMap.has(nameB) ? fmOrderMap.get(nameB) : 99999);
                if (idxA !== idxB) return idxA - idxB;
              }
              return nameA.localeCompare(nameB, undefined, { numeric: true, sensitivity: "base" });
            });

            if (images.length === 0) images.push(file);

            let startIndex = images.findIndex(f => {
              const fName = f.name || f.value || "";
              const fId = f.id || "";
              const fReal = f.realPath || "";
              return (file.id && (fId === file.id || fReal === file.id)) ||
                     (realId && (fReal === realId || fId === realId)) ||
                     (fileName && fName === fileName);
            });
            if (startIndex < 0) {
              images.push(file);
              startIndex = images.length - 1;
            }

            const vs = getViewerSettings();
            const dynamicEl = images.map(f => {
                const fName = f.name || f.value || "";
                const rawPath = f.realPath || f.id || (f === file ? realId : "");
                const fPath = rawPath.startsWith("/") ? rawPath : "/" + rawPath;
                const originalUrl = `/p2p${devPrefix}` + encodeURI(fPath);
                const srcUrl = vs.downscaleImages
                  ? `${originalUrl}?resize=true&width=${vs.scaleTo}&height=${vs.scaleTo}`
                  : originalUrl;
                const thumbUrl = vs.generateThumbs
                  ? `${originalUrl}?resize=true&width=360&height=360`
                  : "";
                return {
                    f,
                    src: srcUrl,
                    thumb: thumbUrl,
                    downloadUrl: originalUrl + "?download=true",
                    subHtml: getGalleryCaption(f) || (f === file ? getGalleryCaption(file) : "")
                };
            });
            
            if (lgPagination) {
                try { lgPagination.cleanup(); } catch (_) {}
            }
            if (lgInstance) {
                try { lgInstance.destroy(true); } catch (_) {}
            }

            const lgEl = document.createElement('div');
            lgPagination = setupGalleryPagination(lgEl, dynamicEl, startIndex);

            lgInstance = lightGallery(lgEl, {
                licenseKey: "GPLv3",
                dynamic: true,
                dynamicEl,
                plugins: [],
                allowMediaOverlap: false,
                closable: true,
                showCloseIcon: true,
                fullScreen: false,
                closeOnTap: false,
                hideBarsDelay: 0,
                controls: false,
                speed: 300,
                swipeThreshold: 20,
                slideEndAnimation: false,
                preload: 2,
                easing: 'cubic-bezier(0.25, 1, 0.5, 1)',
                enableSwipe: true,
                enableDrag: true,
                mobileSettings: {
                    controls: false,
                    showCloseIcon: true,
                    fullScreen: false,
                    download: true,
                    speed: 280,
                    swipeThreshold: 10,
                    preload: 1,
                    slideEndAnimation: false,
                    easing: 'cubic-bezier(0.25, 1, 0.5, 1)'
                },
            });
            lgPagination.setInstance(lgInstance);
            var gallerySlideIndex = startIndex;
            lgEl.addEventListener('lgAfterSlide', (ev) => {
              try { if (ev && typeof ev.detail?.index === 'number') gallerySlideIndex = ev.detail.index; } catch (_) {}
            });
            lgEl.addEventListener('lgAfterClose', () => {
              // Чистим инстанс и DOM: закрытая галерея не должна оставлять
              // скрытый .lg-outer, из которого старая плашка/описание «воскресают».
              const dyn = dynamicEl;
              const lastIdx = gallerySlideIndex;
              const selected = dyn && dyn[lastIdx] && dyn[lastIdx].f;
              const selectedId = selected && (selected.id || selected.realPath || selected.value || selected.name);
              if (lgPagination) {
                try { lgPagination.cleanup(); } catch (_) {}
                lgPagination = null;
              }
              if (lgInstance) {
                try { lgInstance.destroy(true); } catch (_) {}
                lgInstance = null;
              }
              // Keep the FM selection on the image the user was viewing and
              // skip any folder refresh when closing the gallery.
              viewerJustClosed = true;
              setTimeout(() => { viewerJustClosed = false; }, 800);
              if (fmApi && selected) {
                try { fmApi.exec("select-file", { id: selectedId }); } catch (_) {}
              }
              if (typeof window !== "undefined" && window.history.state && window.history.state.viewer) {
                window.history.back();
              }
            });
            lgInstance.openGallery(startIndex);

            // Asynchronously enrich captions from companion Readme.md if not yet populated
            (async () => {
              let updated = false;
              for (let i = 0; i < dynamicEl.length; i++) {
                const item = dynamicEl[i];
                if (item.subHtml) continue;
                const f = item.f || images[i];
                const rawPath = f.realPath || f.id || (f === file ? realId : "");
                if (!rawPath) continue;
                const fPath = rawPath.startsWith("/") ? rawPath : "/" + rawPath;
                const noExt = fPath.replace(/\.[^.]+$/, "");
                const origUrl = `/p2p${devPrefix}` + encodeURI(fPath);
                const baseOrigUrl = `/p2p${devPrefix}` + encodeURI(noExt);

                try {
                  let res = await fetch(`${baseOrigUrl}.Readme.md`);
                  if (!res.ok) res = await fetch(`${origUrl}.Readme.md`);
                  if (res.ok) {
                    const md = await res.text();
                    if (md && md.trim()) {
                      const lines = md.trim().split("\n");
                      let title = "";
                      let descLines = [];
                      for (const rawLine of lines) {
                        const l = rawLine.trim();
                        if (!l) continue;
                        if (!title && l.startsWith("#")) {
                          title = l.replace(/^#+\s*/, "").trim();
                        } else if (!title) {
                          title = l;
                        } else {
                          descLines.push(l);
                        }
                      }
                      f.customAttributes = {
                        ...(f.customAttributes || {}),
                        title,
                        description: descLines.join(" ").trim(),
                        customtitle: true,
                        customdescr: true
                      };
                      item.subHtml = getGalleryCaption(f);
                      updated = true;
                    }
                  }
                } catch (_) {}
              }
              if (updated && lgPagination) {
                lgPagination.updateItems(dynamicEl);
              }
            })();

            if (typeof window !== "undefined" && (!window.history.state || !window.history.state.viewer)) {
              window.history.pushState({ viewer: true }, "");
            }
            
        } catch(e) {
            console.error("Failed to fetch gallery images", e);
        } finally {
            isGalleryLoading = false;
        }
      } else if (mime === "application/pdf" || ext === "pdf") {
        (async () => {
          let pdfUrl = p2pUrl;
          try {
            const aiDev = device || chatTargetDevice || defaultChatTarget();
            const drv = getHttpDriveForDevice(aiDev);
            if (drv) {
              const reqPath = realId || file.path || file.name || fileName;
              const kbRes = await drv.fetch("/api/device_memory_file?path=" + encodeURIComponent(reqPath));
              if (kbRes.ok) {
                const blob = await kbRes.blob();
                pdfUrl = URL.createObjectURL(blob);
              }
            }
          } catch (_) {}
          openViewerFile({
            url: pdfUrl,
            type: "pdf",
            name: fileName
          });
        })();
      } else if (mime.startsWith("video/") || ["mp4", "webm", "mkv", "avi", "mov"].includes(ext)) {
        try {
          _beginFolderFetch();
          const folderData = await device.drive.getFiles(realParent);
          const videos = folderData.filter(f => {
            if (f.isDir || f.type === "folder") return false;
            const fMime = (f.mimeType || f.type || "").toLowerCase();
            const fName = f.name || f.value || "";
            const fExt = fName.includes('.') ? fName.split('.').pop().toLowerCase() : "";
            return fMime.startsWith("video/") || ["mp4", "webm", "mkv", "avi", "mov"].includes(fExt);
          });

          const playlist = videos.map(f => {
            const fName = f.name || f.value || "";
            const rawPath = f.realPath || f.id || (f === file ? realId : "");
            const fPath = rawPath.startsWith("/") ? rawPath : "/" + rawPath;
            const originalUrl = `/p2p${devPrefix}` + encodeURI(fPath);
            const customAttrs = f.customAttributes || {};
            const coverVer = customAttrs.coverVersion || f.coverVersion || "";
            const posterUrl = originalUrl + (originalUrl.includes('?') ? '&' : '?') + 'poster' + (coverVer ? '&v=' + coverVer : '');
            const title = customAttrs.title || f.title || fName;
            const description = customAttrs.description || f.description || "";
            const itemFmId = f.id || (parentPath ? (parentPath.endsWith('/') ? parentPath + fName : parentPath + '/' + fName) : realId);
            return {
              id: itemFmId,
              url: originalUrl,
              poster: posterUrl,
              name: fName,
              title,
              description,
              customAttributes: customAttrs,
              size: f.size
            };
          });

          let startIndex = playlist.findIndex(v => v.url === p2pUrl || v.name === fileName || v.id === file.id);
          if (startIndex < 0) {
            startIndex = 0;
            if (playlist.length === 0) {
              playlist.push({
                id: file.id || realId,
                url: p2pUrl,
                poster: p2pUrl + (p2pUrl.includes('?') ? '&' : '?') + 'poster',
                name: fileName,
                title: file.customAttributes?.title || file.title || fileName,
                description: file.customAttributes?.description || file.description || "",
                customAttributes: file.customAttributes || {}
              });
            }
          }

          openViewerFile({
            id: playlist[startIndex].id,
            url: playlist[startIndex].url,
            poster: playlist[startIndex].poster,
            type: "video",
            name: playlist[startIndex].name,
            title: playlist[startIndex].title,
            description: playlist[startIndex].description,
            playlist: playlist.length > 1 ? playlist : [],
            currentIndex: startIndex
          });
        } catch (err) {
          console.error("Failed to load video playlist:", err);
          openViewerFile({
            id: file.id || realId,
            url: p2pUrl,
            poster: p2pUrl + (p2pUrl.includes('?') ? '&' : '?') + 'poster',
            type: "video",
            name: fileName,
            title: file.customAttributes?.title || file.title || fileName,
            description: file.customAttributes?.description || file.description || "",
            playlist: [],
            currentIndex: 0
          });
        } finally {
          _endFolderFetch();
        }
      } else if (mime.startsWith("audio/") || ["mp3", "wav", "ogg", "flac", "m4a", "aac", "opus", "wma"].includes(ext)) {
        try {
          _beginFolderFetch();
          const folderData = await device.drive.getFiles(realParent);
          const audios = folderData.filter(f => {
            if (f.isDir || f.type === "folder") return false;
            const fMime = (f.mimeType || f.type || "").toLowerCase();
            const fName = f.name || f.value || "";
            const fExt = fName.includes('.') ? fName.split('.').pop().toLowerCase() : "";
            return fMime.startsWith("audio/") || ["mp3", "wav", "ogg", "flac", "m4a", "aac", "opus", "wma"].includes(fExt);
          });

          // Match current File Manager panel order if open, else sort by ID3 track number or natural file name
          let fmOrderMap = new Map();
          if (fmApi) {
            try {
              const fmState = fmApi.getState();
              const pIdx = fmState.activePanel || 0;
              const curPanel = fmState.panels && fmState.panels[pIdx];
              const pFiles = curPanel?._files || curPanel?.data || [];
              pFiles.forEach((item, idx) => {
                const iName = item.value || item.name || item.id || "";
                if (iName) fmOrderMap.set(iName, idx);
                if (item.id) fmOrderMap.set(item.id, idx);
              });
            } catch (_) {}
          }

          audios.sort((a, b) => {
            const nameA = a.name || a.value || "";
            const nameB = b.name || b.value || "";
            if (fmOrderMap.size > 0) {
              const idxA = fmOrderMap.has(a.id) ? fmOrderMap.get(a.id) : (fmOrderMap.has(nameA) ? fmOrderMap.get(nameA) : 99999);
              const idxB = fmOrderMap.has(b.id) ? fmOrderMap.get(b.id) : (fmOrderMap.has(nameB) ? fmOrderMap.get(nameB) : 99999);
              if (idxA !== idxB) return idxA - idxB;
            }
            const trkA = a.customAttributes?.track || a.track || 0;
            const trkB = b.customAttributes?.track || b.track || 0;
            if (trkA > 0 && trkB > 0 && trkA !== trkB) {
              return trkA - trkB;
            }
            return nameA.localeCompare(nameB, undefined, { numeric: true, sensitivity: "base" });
          });

          const playlist = audios.map(f => {
            const fName = f.name || f.value || "";
            const rawPath = f.realPath || f.id || (f === file ? realId : "");
            const fPath = rawPath.startsWith("/") ? rawPath : "/" + rawPath;
            const originalUrl = `/p2p${devPrefix}` + encodeURI(fPath);
            const customAttrs = f.customAttributes || {};
            const coverVer = customAttrs.coverVersion || f.coverVersion || "";
            const hasCover = !!(f.hasCover || customAttrs.cover || customAttrs.preview);
            const posterUrl = hasCover ? (originalUrl + (originalUrl.includes('?') ? '&' : '?') + 'cover=600' + (coverVer ? '&v=' + coverVer : '')) : '';
            const title = customAttrs.title || f.title || fName;
            const description = customAttrs.description || f.description || "";
            const itemFmId = f.id || (parentPath ? (parentPath.endsWith('/') ? parentPath + fName : parentPath + '/' + fName) : realId);
            return {
              id: itemFmId,
              url: originalUrl,
              poster: posterUrl,
              hasCover,
              name: fName,
              title,
              description,
              customAttributes: customAttrs,
              size: f.size
            };
          });

          let startIndex = playlist.findIndex(v => v.url === p2pUrl || v.name === fileName || v.id === file.id);
          if (startIndex < 0) {
            startIndex = 0;
            if (playlist.length === 0) {
              const singleHasCover = !!(file.hasCover || file.customAttributes?.cover);
              playlist.push({
                id: file.id || realId,
                url: p2pUrl,
                poster: singleHasCover ? (p2pUrl + (p2pUrl.includes('?') ? '&' : '?') + 'cover=600') : '',
                hasCover: singleHasCover,
                name: fileName,
                title: file.customAttributes?.title || file.title || fileName,
                description: file.customAttributes?.description || file.description || "",
                customAttributes: file.customAttributes || {}
              });
            }
          }

          openViewerFile({
            id: playlist[startIndex].id,
            url: playlist[startIndex].url,
            poster: playlist[startIndex].poster,
            hasCover: playlist[startIndex].hasCover,
            type: "audio",
            name: playlist[startIndex].name,
            title: playlist[startIndex].title,
            description: playlist[startIndex].description,
            playlist: playlist.length > 1 ? playlist : [],
            currentIndex: startIndex
          });
        } catch (err) {
          console.error("Failed to load audio playlist:", err);
          const singleHasCover = !!(file.hasCover || file.customAttributes?.cover);
          openViewerFile({
            id: file.id || realId,
            url: p2pUrl,
            poster: singleHasCover ? (p2pUrl + (p2pUrl.includes('?') ? '&' : '?') + 'cover=600') : '',
            hasCover: singleHasCover,
            type: "audio",
            name: fileName,
            title: file.customAttributes?.title || file.title || fileName,
            description: file.customAttributes?.description || file.description || "",
            playlist: [],
            currentIndex: 0
          });
        } finally {
          _endFolderFetch();
        }
      } else if (isOfficeDocument(fileName, mime)) {
        try {
          downloadAbortController = new AbortController();
          downloadingFile = "Loading document...";
          isFolderLoading = true;
          docLoadLabel = "Loading document…";
          lastOfficeClick = { file, parentPath };

          // WordCraft: edit the ORIGINAL file (real .docx/.odt round trip on the
          // client, saved back to the original path — no .univer.json). Anything
          // else (epub/fb2, xlsx) keeps the Univer path below as the fallback.
          // Files whose WordCraft start failed once (no WebGPU/WebGL2) skip it.
          if (WC_DOC_EXTS.includes(ext) && !wcUnsupportedIds.has(realId)) {
            let wcBlob = null;
            try {
              if (device && device.drive && typeof device.drive.getFileContent === "function") {
                try {
                  wcBlob = await device.drive.getFileContent(realId, file.size || 0);
                } catch (driveErr) {
                  console.warn("[App] WC drive.getFileContent failed:", driveErr);
                }
              }
              if (!wcBlob) {
                const binRes = await fetch(p2pUrl, { signal: downloadAbortController.signal });
                if (binRes.ok) wcBlob = await binRes.blob();
              }
              if (!wcBlob) {
                try {
                  const aiDev = device || chatTargetDevice || defaultChatTarget();
                  const drv = getHttpDriveForDevice(aiDev);
                  if (drv) {
                    const reqPath = realId || file.path || file.name || fileName;
                    const kbRes = await drv.fetch("/api/device_memory_file?path=" + encodeURIComponent(reqPath), {
                      signal: downloadAbortController.signal
                    });
                    if (kbRes.ok) wcBlob = await kbRes.blob();
                  }
                } catch (_) {}
              }
            } catch (wcErr) {
              console.warn("[App] WordCraft binary fetch failed:", wcErr);
            }
            if (wcBlob) {
              const wcBytes = await wcBlob.arrayBuffer();
              console.log("[App] WordCraft: opening original", fileName, wcBytes.byteLength, "bytes");
              openViewerFile({
                id: file.id || realId,
                url: p2pUrl,
                type: "wordcraft",
                name: fileName,
                path: realId,
                bytes: wcBytes,
                activeDevice: device
              });
              return;
            }
            console.warn("[App] WordCraft: no original bytes for", fileName, "— falling back to the Univer path");
          }

          // GridCraft: edit the ORIGINAL workbook (built-in ?host=parent bridge,
          // real XLSX round trip). .xls/.ods import values only — they stay on
          // the Univer path below.
          if (GC_SHEET_EXTS.includes(ext) && !gcUnsupportedIds.has(realId)) {
            let gcBlob = null;
            try {
              if (device && device.drive && typeof device.drive.getFileContent === "function") {
                try {
                  gcBlob = await device.drive.getFileContent(realId, file.size || 0);
                } catch (driveErr) {
                  console.warn("[App] GC drive.getFileContent failed:", driveErr);
                }
              }
              if (!gcBlob) {
                const binRes = await fetch(p2pUrl, { signal: downloadAbortController.signal });
                if (binRes.ok) gcBlob = await binRes.blob();
              }
              if (!gcBlob) {
                try {
                  const aiDev = device || chatTargetDevice || defaultChatTarget();
                  const drv = getHttpDriveForDevice(aiDev);
                  if (drv) {
                    const reqPath = realId || file.path || file.name || fileName;
                    const kbRes = await drv.fetch("/api/device_memory_file?path=" + encodeURIComponent(reqPath), {
                      signal: downloadAbortController.signal
                    });
                    if (kbRes.ok) gcBlob = await kbRes.blob();
                  }
                } catch (_) {}
              }
            } catch (gcErr) {
              console.warn("[App] GridCraft binary fetch failed:", gcErr);
            }
            if (gcBlob) {
              const gcBytes = await gcBlob.arrayBuffer();
              console.log("[App] GridCraft: opening original", fileName, gcBytes.byteLength, "bytes");
              openViewerFile({
                id: file.id || realId,
                url: p2pUrl,
                type: "gridcraft",
                name: fileName,
                path: realId,
                bytes: gcBytes,
                activeDevice: device
              });
              return;
            }
            console.warn("[App] GridCraft: no original bytes for", fileName, "— falling back to the Univer path");
          }

          let docText = "";
          let loaded = false;

          // 0. Preferred path: the device's own onmychat backend reads the
          // ORIGINAL file from the node filesystem and returns the native
          // Univer snapshot (rich styles). Falls back to ?totext/local below.
          let docSnapshot = null;
          {
            try {
              const drv = getHttpDriveForDevice(device);
              if (drv && p2pUrl) {
                const m = p2pUrl.match(/^\/p2p\/[^/]+(\/.*)$/);
                if (m) {
                  const filePath = decodeURIComponent(m[1].split('?')[0]);
                  const res = await drv.fetch('/ondoc/convert', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ path: filePath })
                  });
                  if (res.ok) {
                    const data = await res.json();
                    const snap = data?.result?.snapshot || data?.snapshot;
                    if (snap && (snap?.body?.dataStream || snap?.kind === 'sheet' || !!snap?.sheets)) {
                      docSnapshot = snap;
                      loaded = true;
                      const info = snap?.body?.dataStream ? snap.body.dataStream.length + ' chars' : (snap.sheetCount + ' sheet(s)');
                      console.log('[App] ondoc snapshot loaded:', info, 'from', filePath);
                    }
                  } else {
                    console.warn('[App] ondoc convert HTTP status:', res.status);
                  }
                }
              }
            } catch (e) {
              console.warn('[App] ondoc backend skipped:', e.message || e);
            }
          }

          // Request with ?totext to invoke pandoc on the node
          if (!loaded) {
          const toTextUrl = p2pUrl + (p2pUrl.includes('?') ? '&' : '?') + 'totext';
          try {
            const res = await fetch(toTextUrl, { signal: downloadAbortController.signal });
            if (res.ok) {
              const contentType = res.headers.get('content-type') || '';
              docText = await res.text();
              console.log("[App] totext response:", contentType, "len:", docText.length);
              // Binary body means the ?totext conversion was ignored
              // (e.g. P2P Open/Read RPC) — fall back to local conversion.
              if (!isBinaryResponseType(contentType) && docText.trim()) {
                loaded = true;
              }
            } else {
              console.warn("[App] totext HTTP status:", res.status);
            }
          } catch (fetchErr) {
            console.warn("[App] fetch(toTextUrl) failed, trying direct drive content:", fetchErr);
          }

          if (!loaded) {
            let blob = null;
            if (device && device.drive && typeof device.drive.getFileContent === "function") {
              try {
                blob = await device.drive.getFileContent(realId, file.size || 0);
              } catch (driveErr) {
                console.error("[App] direct drive.getFileContent failed:", driveErr);
              }
            }
            if (!blob) {
              try {
                const binRes = await fetch(p2pUrl, { signal: downloadAbortController.signal });
                if (binRes.ok) blob = await binRes.blob();
              } catch (binErr) {
                console.warn("[App] fetch(p2pUrl) binary also failed:", binErr);
              }
            }
            if (blob) {
              console.log("[App] local conversion: blob size", blob.size, "type", blob.type);
              const md = await extractDocTextFromBlob(blob, fileName);
              console.log("[App] local conversion result len:", md === null ? null : md.length);
              if (md !== null && md.trim()) {
                docText = md;
                loaded = true;
              } else {
                console.warn("[App] local doc text extraction failed for", fileName);
              }
            }
          }
          }

          if (loaded) {
            // Convert a native backend snapshot into markdown. The light viewer
            // (MarkdownViewer) does not need Univer's heavy bundle.
            if (docSnapshot) {
              try {
                const mdFromSnap = univerDocToMarkdown(docSnapshot);
                if (mdFromSnap && mdFromSnap.trim()) docText = mdFromSnap;
              } catch (convErr) {
                console.warn("[App] snapshot to markdown conversion failed:", convErr);
              }
            }
            const isSheetDoc = docSnapshot && (docSnapshot.kind === 'sheet' || !!docSnapshot.sheets);
            openViewerFile({
              id: file.id || realId,
              url: p2pUrl,
              type: isSheetDoc ? "sheet" : "doc",
              name: fileName,
              path: realId,
              content: docText,
              nativeSnapshot: docSnapshot,
              activeDevice: device
            });
          } else {
            console.error("[App] Failed to load document content, realId:", realId);
            handleFileDownload(file);
          }
        } catch (err) {
          if (err.name !== 'AbortError') {
            console.error("Failed to load document:", err);
            handleFileDownload(file);
          }
        } finally {
          downloadingFile = null;
          docLoadLabel = null;
          isFolderLoading = false;
          downloadAbortController = null;
        }
      } else {
        const textExts = ["txt", "md", "js", "ts", "json", "html", "css", "scss", "cpp", "c", "h", "py", "sh", "bash", "xml", "log", "yml", "yaml", "svelte", "vue"];
        const mdExts = ["md", "markdown"];
        const isMdFile = mdExts.includes(ext) || mime === "text/markdown";
        if (isMdFile) {
          try {
            downloadAbortController = new AbortController();
            downloadingFile = "Loading document...";
            isFolderLoading = true;
            docLoadLabel = "Loading document…";
            let text = "";
            let loaded = false;
            try {
              const res = await fetch(p2pUrl, { signal: downloadAbortController.signal });
              if (res.ok) {
                text = await res.text();
                loaded = true;
              }
            } catch (fetchErr) {
              console.warn("[App] fetch(p2pUrl) failed:", fetchErr);
            }
            if (!loaded) {
              try {
                const aiDev = device || chatTargetDevice || defaultChatTarget();
                const drv = getHttpDriveForDevice(aiDev);
                if (drv) {
                  const reqPath = realId || file.path || file.name || fileName;
                  const kbRes = await drv.fetch("/api/device_memory_file?path=" + encodeURIComponent(reqPath), {
                    signal: downloadAbortController.signal
                  });
                  if (kbRes.ok) {
                    text = await kbRes.text();
                    loaded = true;
                  }
                }
              } catch (kbErr) {
                console.warn("[App] fetch kb file via tunnel failed:", kbErr);
              }
            }
            if (loaded) {
              appTitle = fileName;
              appFilePath = realId;
              appContent = text;
              openViewerFile({
                id: file.id || realId,
                url: p2pUrl,
                type: "markdown",
                name: fileName,
                path: realId,
                content: text
              });
            } else {
              console.error("[App] Failed to load markdown file from", p2pUrl, "realId:", realId);
              alert($_("failed_to_load_file", {}, "Failed to load file: ") + fileName);
            }
          } catch (err) {
            if (err.name === 'AbortError') {
              console.log("Text loading cancelled");
            } else {
              console.error("Failed to load markdown file:", err);
            }
          } finally {
            docLoadLabel = null;
            downloadingFile = null;
            isFolderLoading = false;
            downloadAbortController = null;
          }
        } else if (mime.startsWith("text/") || textExts.includes(ext) || mime === "application/json") {
          try {
            downloadAbortController = new AbortController();
            downloadingFile = "Loading text...";
            isFolderLoading = true;
            docLoadLabel = "Loading text…";
            let text = "";
            let loaded = false;
            try {
              const res = await fetch(p2pUrl, { signal: downloadAbortController.signal });
              if (res.ok) {
                text = await res.text();
                loaded = true;
              }
            } catch (fetchErr) {
              console.warn("[App] fetch(p2pUrl) failed, trying direct drive.getFileContent:", fetchErr);
            }
            if (!loaded && device && device.drive && typeof device.drive.getFileContent === "function") {
              try {
                const blob = await device.drive.getFileContent(realId, file.size || 0);
                if (blob) {
                  text = await blob.text();
                  loaded = true;
                }
              } catch (driveErr) {
                console.error("[App] direct drive.getFileContent also failed:", driveErr);
              }
            }
            if (!loaded) {
              try {
                const aiDev = device || chatTargetDevice || defaultChatTarget();
                const drv = getHttpDriveForDevice(aiDev);
                if (drv) {
                  const reqPath = realId || file.path || file.name || fileName;
                  const kbRes = await drv.fetch("/api/device_memory_file?path=" + encodeURIComponent(reqPath), {
                    signal: downloadAbortController.signal
                  });
                  if (kbRes.ok) {
                    text = await kbRes.text();
                    loaded = true;
                  }
                }
              } catch (kbErr) {
                console.warn("[App] fetch kb text file via tunnel failed:", kbErr);
              }
            }
            if (loaded) {
              appTitle = fileName;
              appFilePath = realId;
              activeDeviceId = device.linkId;
              appContent = text;
              currentApp = "code";
            } else {
              console.error("[App] Failed to load text file from", p2pUrl, "realId:", realId);
              alert($_("failed_to_load_file", {}, "Failed to load file: ") + fileName);
            }
          } catch (err) {
            if (err.name === 'AbortError') {
              console.log("Text loading cancelled");
            } else {
              console.error("Failed to load text file:", err);
            }
          } finally {
            docLoadLabel = null;
            downloadingFile = null;
            isFolderLoading = false;
            downloadAbortController = null;
          }
        } else if (DC_PRES_EXTS.includes(ext) && !dcUnsupportedIds.has(realId)) {
          // DeckCraft: edit the ORIGINAL presentation (fork with the deckcraft:*
          // parent bridge). Fallback: download the file.
          try {
            downloadAbortController = new AbortController();
            downloadingFile = "Loading presentation...";
            isFolderLoading = true;
            docLoadLabel = "Loading presentation…";
            lastPresClick = { file, parentPath };
            let dcBlob = null;
            if (device && device.drive && typeof device.drive.getFileContent === "function") {
              try {
                dcBlob = await device.drive.getFileContent(realId, file.size || 0);
              } catch (driveErr) {
                console.warn("[App] DC drive.getFileContent failed:", driveErr);
              }
            }
            if (!dcBlob) {
              const binRes = await fetch(p2pUrl, { signal: downloadAbortController.signal });
              if (binRes.ok) dcBlob = await binRes.blob();
            }
            if (dcBlob) {
              const dcBytes = await dcBlob.arrayBuffer();
              console.log("[App] DeckCraft: opening original", fileName, dcBytes.byteLength, "bytes");
              openViewerFile({
                id: file.id || realId,
                url: p2pUrl,
                type: "deckcraft",
                name: fileName,
                path: realId,
                bytes: dcBytes,
                activeDevice: device
              });
            } else {
              console.warn("[App] DeckCraft: no original bytes for", fileName);
              handleFileDownload(file);
            }
          } catch (dcErr) {
            console.warn("[App] DeckCraft fetch failed:", dcErr);
            handleFileDownload(file);
          } finally {
            docLoadLabel = null;
            downloadingFile = null;
            isFolderLoading = false;
            downloadAbortController = null;
          }
        } else {
          // Fallback - trigger download if unknown format
          handleFileDownload(file);
        }
      }
    } catch (err) {
      console.error("File open error:", err);
      alert("Failed to read file: " + err.message);
    }
  }

  async function handleFileDownload(file) {
    try {
      if (file.mimeType && file.mimeType.startsWith("application/x-")) {
        return; // prevent downloading virtual files
      }
      const target = getFileDeviceAndRealPath(file);
      if (!target) return;
      const { device, realPath: realId } = target;
      const fileName = file.name || file.value || "";
      downloadingFile = fileName;
      downloadProgress = 0;
      downloadSpeed = "";
      isFolderLoading = false;
      downloadAbortController = new AbortController();
      
      let lastReportTime = Date.now();
      let lastLoaded = 0;
      
      const blob = await device.drive.getFileContent(realId, file.size, (loaded, total) => {
        downloadProgress = Math.round((loaded / total) * 100);
        const now = Date.now();
        if (now - lastReportTime > 500) {
          const bytesPerSec = (loaded - lastLoaded) / ((now - lastReportTime) / 1000);
          downloadSpeed = formatSpeed(bytesPerSec);
          lastReportTime = now;
          lastLoaded = loaded;
        }
      }, 0, "", downloadAbortController.signal);
      
      const blobUrl = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = blobUrl;
      a.download = fileName;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      
      setTimeout(() => URL.revokeObjectURL(blobUrl), 10000);
      
    } catch (err) {
      if (err.message && err.message.includes("AbortError")) {
        console.log("Download cancelled");
      } else {
        console.error("Download error:", err);
        alert("Failed to download file: " + err.message);
      }
    } finally {
      downloadingFile = null;
      downloadProgress = 0;
      downloadSpeed = "";
      downloadAbortController = null;
    }
  }

  function cancelOperation() {
    _folderLoadCancelled = true;
    for (const k of _loadingFolderPromises.keys()) {
      _folderFailures.set(k, Date.now());
    }
    _loadingFolderPromises.clear();
    if (downloadAbortController) {
      downloadAbortController.abort();
      downloadAbortController = null;
    }
    // Clear folder/file loading modal
    downloadingFile = null;
    isFolderLoading = false;
  }

  function handleKeydown(event) {
    if (event.key === 'Escape') {
      if (viewerFile) closeViewer();
      else if (currentApp === 'settings') {
        currentApp = previousApp || mainApp;
        previousApp = "filemanager";
      }
    }
  }

  function handleVideoChange(detail) {
    if (detail && detail.video) {
      if (viewerFile) {
        viewerFile.url = detail.video.url;
        viewerFile.poster = detail.video.poster;
        viewerFile.name = detail.video.name;
        viewerFile.title = detail.video.title;
        viewerFile.description = detail.video.description;
        viewerFile.id = detail.video.id;
        viewerFile.currentIndex = detail.index;
      }
      if (detail.video.id && fmApi) {
        try {
          fmApi.exec("select-file", { id: detail.video.id });
        } catch (e) {}
      }
    }
  }

  function handleAudioChange(detail) {
    if (detail && detail.audio) {
      if (viewerFile) {
        viewerFile.url = detail.audio.url;
        viewerFile.poster = detail.audio.poster;
        viewerFile.hasCover = detail.audio.hasCover;
        viewerFile.name = detail.audio.name;
        viewerFile.title = detail.audio.title;
        viewerFile.description = detail.audio.description;
        viewerFile.id = detail.audio.id;
        viewerFile.currentIndex = detail.index;
      }
    }
  }

  function openViewerFile(data) {
    viewerFile = data;
    if (data && typeof window !== "undefined" && (!window.history.state || !window.history.state.viewer)) {
      window.history.pushState({ viewer: true }, "");
    }
  }

  function closeViewer(detail) {
    if (typeof window !== "undefined" && window.history.state && window.history.state.viewer) {
      window.history.back();
      return;
    }
    const targetId = (detail && detail.video && detail.video.id) || (detail && detail.audio && detail.audio.id) || (viewerFile && viewerFile.id);
    if (targetId && fmApi) {
      try {
        fmApi.exec("select-file", { id: targetId });
      } catch (e) {}
    }
    if (viewerFile && viewerFile.blobUrl) {
      URL.revokeObjectURL(viewerFile.blobUrl);
    }
    viewerFile = null;
  }

  const iconWhitelist = { "7z":1, rar:1, zip:1, css:1, html:1, js:1, php:1, md:1, xml:1, sql:1, aif:1, mid:1, mp3:1, waw:1, wma:1, doc:1, docx:1, txt:1, avi:1, mov:1, mp4:1, mpeg:1, mpg:1, pdf:1, xls:1, xlsx:1, gif:1, jpg:1, jpeg:1, png:1, psd:1, tiff:1, svg:1, folder:1, file:1, multiple:1, search:1, none:1 };

  // Bootstrap Icons glyphs reused from the legacy frontend (CSS mask style).
  const BI_TERMINAL_FILL = "M0 3a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2zm9.5 5.5h-3a.5.5 0 0 0 0 1h3a.5.5 0 0 0 0-1m-6.354-.354a.5.5 0 1 0 .708.708l2-2a.5.5 0 0 0 0-.708l-2-2a.5.5 0 1 0-.708.708L4.793 6.5z";
  const BI_CHAT_LEFT_TEXT_FILL = "M0 2a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H4.414a1 1 0 0 0-.707.293L.854 15.146A.5.5 0 0 1 0 14.793zm3.5 1a.5.5 0 0 0 0 1h9a.5.5 0 0 0 0-1zm0 2.5a.5.5 0 0 0 0 1h9a.5.5 0 0 0 0-1zm0 2.5a.5.5 0 0 0 0 1h5a.5.5 0 0 0 0-1z";
  const BI_HDD_FILL = "M0 10a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2zm2.5 1a.5.5 0 1 0 0 1 .5.5 0 0 0 0-1m2 0a.5.5 0 1 0 0 1 .5.5 0 0 0 0-1M.91 7.204A3 3 0 0 1 2 7h12c.384 0 .752.072 1.09.204l-1.87-4.363A1 1 0 0 0 12.3 2.2H3.7a1 1 0 0 0-.92.641z";
  const BI_LAPTOP = "M13.5 3a.5.5 0 0 1 .5.5V11H2V3.5a.5.5 0 0 1 .5-.5zm-11-1A1.5 1.5 0 0 0 1 3.5V12h14V3.5A1.5 1.5 0 0 0 13.5 2zM0 12.5h16a1.5 1.5 0 0 1-1.5 1.5h-13A1.5 1.5 0 0 1 0 12.5";
  const BI_PHONE = "M11 1a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V2a1 1 0 0 1 1-1zM5 0a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2V2a2 2 0 0 0-2-2z M8 14a1 1 0 1 0 0-2 1 1 0 0 0 0 2";
  const BI_TABLET = "M12 1a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V2a1 1 0 0 1 1-1zM4 0a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h8a2 2 0 0 0 2-2V2a2 2 0 0 0-2-2z M8 14a1 1 0 1 0 0-2 1 1 0 0 0 0 2";
  const BI_FOLDER_FILL = "M9.828 3h3.982a2 2 0 0 1 1.992 2.181l-.637 7A2 2 0 0 1 13.174 14H2.825a2 2 0 0 1-1.991-1.819l-.637-7a2 2 0 0 1 .342-1.31L.5 3a2 2 0 0 1 2-2h3.672a2 2 0 0 1 1.414.586l.828.828A2 2 0 0 0 9.828 3m-8.322.12q.322-.119.684-.12h5.396l-.707-.707A1 1 0 0 0 6.172 2H2.5a1 1 0 0 0-1 .981z";
  const BI_FOLDER_SYMLINK_FILL = "M13.81 3H9.828a2 2 0 0 1-1.414-.586l-.828-.828A2 2 0 0 0 6.172 1H2.5a2 2 0 0 0-2 2l.04.87a2 2 0 0 0-.342 1.311l.637 7A2 2 0 0 0 2.826 14h10.348a2 2 0 0 0 1.991-1.819l.637-7A2 2 0 0 0 13.81 3M2.19 3q-.362.002-.683.12L1.5 2.98a1 1 0 0 1 1-.98h3.672a1 1 0 0 1 .707.293L7.586 3zm9.608 5.271-3.182 1.97c-.27.166-.616-.036-.616-.372V9.1s-2.571-.3-4 2.4c.571-4.8 3.143-4.8 4-4.8v-.769c0-.336.346-.538.616-.371l3.182 1.969c.27.166.27.576 0 .742";

  function biIconDataUri(d, color, fillRule = "nonzero") {
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><path fill="${color}" fill-rule="${fillRule}" d="${d}"/></svg>`;
    return "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg);
  }

  function sharedFolderDataUri(color) {
    const svg = `<svg viewBox="0 0 16 16" width="64" height="64" xmlns="http://www.w3.org/2000/svg"><defs><mask id="peopleCutout"><rect x="0" y="0" width="16" height="16" fill="white"></rect><path fill="black" transform="translate(4, 4) scale(0.5)" d="M7 14s-1 0-1-1 1-4 5-4 5 3 5 4-1 1-1 1zm4-6a3 3 0 1 0 0-6 3 3 0 0 0 0 6m-5.784 6A2.24 2.24 0 0 1 5 13c0-1.355.68-2.75 1.936-3.72A6.3 6.3 0 0 0 5 9c-4 0-5 3-5 4s1 1 1 1zM4.5 8a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5"></path></mask></defs><path fill="${color}" mask="url(#peopleCutout)" d="M9.828 3h3.982a2 2 0 0 1 1.992 2.181l-.637 7A2 2 0 0 1 13.174 14H2.825a2 2 0 0 1-1.991-1.819l-.637-7a2 2 0 0 1 .342-1.31L.5 3a2 2 0 0 1 2-2h3.672a2 2 0 0 1 1.414.586l.828.828A2 2 0 0 0 9.828 3m-8.322.12q.322-.119.684-.12h5.396l-.707-.707A1 1 0 0 0 6.172 2H2.5a1 1 0 0 0-1 .981z"></path></svg>`;
    return "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg);
  }

  function sitesFolderDataUri(color) {
    const svg = `<svg viewBox="0 0 100.8 76.5" width="64" height="64" xmlns="http://www.w3.org/2000/svg"><path fill-rule="evenodd" clip-rule="evenodd" fill="${color}" d="M90.516 17.9999V20.6999C92.816 21.2999 94.516 23.3999 94.316 25.8999L91.016 69.8999C90.816 72.4999 88.616 74.4999 86.016 74.4999H10.316C7.71598 74.4999 5.51598 72.4999 5.31598 69.8999L2.01598 25.8999C1.81598 23.3999 3.51598 21.2999 5.81598 20.6999V13.3999C5.81598 9.5999 9.01598 6.3999 12.816 6.3999H38.816L43.716 12.9999H45.816H85.516C88.216 12.9999 90.516 15.1999 90.516 17.9999ZM14.716 16.6999C12.216 16.6999 10.016 18.2999 9.11598 20.4999H87.316C86.416 18.2999 84.216 16.6999 81.716 16.6999H14.716ZM35.6 47.5004C35.6 46.5566 35.7013 45.6365 35.8938 44.7504H41.313C41.1908 45.6472 41.1194 46.5541 41.1003 47.4671C41.0999 47.4893 41.0999 47.5115 41.1003 47.5337C41.1194 48.4467 41.1908 49.3536 41.313 50.2504H35.8938C35.7013 49.3643 35.6 48.4442 35.6 47.5004ZM41.9758 41.5504H37.0512C38.6557 38.4693 41.4675 36.1172 44.8594 35.1213C43.5825 37.1121 42.6115 39.2796 41.9758 41.5504ZM45.3193 41.5504C46.0315 39.378 47.1036 37.3297 48.5 35.4952C49.8964 37.3297 50.9685 39.378 51.6807 41.5504H45.3193ZM44.3004 47.5004C44.3212 46.5746 44.4042 45.656 44.5473 44.7504H52.4527C52.5958 45.656 52.6788 46.5746 52.6996 47.5004C52.6788 48.4262 52.5958 49.3447 52.4527 50.2504H44.5473C44.4042 49.3447 44.3212 48.4262 44.3004 47.5004ZM55.8997 47.5337C55.8806 48.4467 55.8092 49.3536 55.687 50.2504H61.1062C61.2987 49.3643 61.4 48.4442 61.4 47.5004C61.4 46.5566 61.2987 45.6365 61.1062 44.7504H55.687C55.8092 45.6472 55.8806 46.5541 55.8997 47.4671C55.9001 47.4893 55.9001 47.5115 55.8997 47.5337ZM48.5 59.5056C47.1036 57.6711 46.0315 55.6228 45.3193 53.4504H51.6807C50.9685 55.6228 49.8964 57.6711 48.5 59.5056ZM41.9758 53.4504C42.6115 55.7212 43.5825 57.8887 44.8594 59.8795C41.4675 58.8836 38.6557 56.5315 37.0512 53.4504H41.9758ZM55.0242 53.4504C54.3885 55.7212 53.4175 57.8887 52.1406 59.8795C55.5325 58.8836 58.3443 56.5315 59.9488 53.4504H55.0242ZM55.0242 41.5504H59.9488C58.3443 38.4693 55.5325 36.1172 52.1406 35.1213C53.4175 37.1121 54.3885 39.2796 55.0242 41.5504ZM48.5 31.4004C39.6082 31.4004 32.4 38.6086 32.4 47.5004C32.4 56.3922 39.6082 63.6004 48.5 63.6004C57.3918 63.6004 64.6 56.3922 64.6 47.5004C64.6 38.6086 57.3918 31.4004 48.5 31.4004Z"/></svg>`;
    return "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg);
  }

  $: gatewayBrandPalette = getGatewayBrandPalette($gatewayConfig, isDarkTheme);
  $: appIconColor = gatewayBrandPalette.appIconColor;
  $: brandColor = gatewayBrandPalette.brandColor;
  $: chatIconColor = appIconColor;
  $: brandHoverColor = gatewayBrandPalette.brandHoverColor;

  $: getCustomIcon = (file, size) => {
    if (file.mimeType === "application/x-terminal" || file.id === "subapp:terminal") {
      return biIconDataUri(BI_TERMINAL_FILL, appIconColor);
    }
    if (file.mimeType === "application/x-chat" || file.id === "subapp:chat" || file.id === "/chats" || file.itemType === "chat") {
      return biIconDataUri(BI_CHAT_LEFT_TEXT_FILL, chatIconColor);
    }
    if (file.mimeType === APP_PROXY_MIME) {
      return resolveAppIconDataUri(file.customAttributes?.icon, appIconColor);
    }
    if (file.id === "/shared-by-me" || file.itemType === "shared-by-me" || file.icon === "wxi-folder-links") {
      return biIconDataUri(BI_FOLDER_SYMLINK_FILL, brandColor);
    }
    if (file.id === "/shared-to-me" || file.itemType === "shared-to-me" || file.isShared || file.itemType === "shared" || file.icon === "wxi-folder-shared") {
      return sharedFolderDataUri(brandColor);
    }
    if (file.isDevice || file.itemType === "device") {
      if (file.deviceType === "ai" || file.icon === "wxi-chat") {
        return biIconDataUri(BI_CHAT_LEFT_TEXT_FILL, brandColor);
      }
      if (file.deviceType === "desktop" || file.icon === "wxi-desktop") {
        return biIconDataUri(BI_LAPTOP, brandColor);
      }
      if (file.deviceType === "phone" || file.icon === "wxi-phone") {
        return biIconDataUri(BI_PHONE, brandColor);
      }
      if (file.deviceType === "tablet" || file.icon === "wxi-tablet") {
        return biIconDataUri(BI_TABLET, brandColor);
      }
      return biIconDataUri(BI_HDD_FILL, brandColor);
    }
    if (file.icon === "wxi-folder-sites") {
      return sitesFolderDataUri(brandColor);
    }
    if (file.type === "folder") {
      return biIconDataUri(BI_FOLDER_FILL, brandColor);
    }
    const sizeDir = size === "small" ? "24" : "64";
    
    // Fallback to SVAR CDN default logic
    if (file.type === "folder") return false;
    let icon = "unknown";
    if (file.type && file.type !== "file" && iconWhitelist[file.type]) {
      icon = file.type;
    } else if (file.ext) {
      icon = iconWhitelist[file.ext] ? file.ext : "file";
    } else {
      icon = "unknown";
    }
    return `https://cdn.svar.dev/icons/filemanager/vivid/${size}/${icon}.svg`;
  };
</script>

<svelte:window on:keydown={handleKeydown} />

{#snippet filesOverlayClose()}
  <button
    type="button"
    class="omd-fm-exit-btn"
    on:click={closeFilesApp}
    title={$_("back_to_chat", {}, "Back to chats")}
    aria-label={$_("back_to_chat", {}, "Back to chats")}
  >
    <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
      <path d="M6 6l12 12M18 6L6 18" />
    </svg>
  </button>
{/snippet}

<main class="omd-app">
  <div class="content">
    {#if authorized && loginMode !== 'recovery'}
      <div class="filemanager-container" style="display: {!isSharedRoute && currentApp === 'filemanager' ? 'block' : 'none'}">
        <div style="display: none;">
          <Willow />
          <WillowDark />
        </div>
        <div
          class="skin-files {isDarkTheme ? 'wx-willow-dark-theme dark-theme' : 'wx-willow-theme light-theme'}"
          style="height: 100%; {gatewayBrandPalette.fmInlineStyle}"
        >
          {#key rootName + "_" + $locale}
            <Locale words={getSvarLocale($locale, rootName)}>
              <Filemanager
                data={filesData}
                panels={[{ path: "/" }]}
                drive={driveInfo}
                httpDrive={fmHttpDrive}
                searchDrivesProvider={getSearchDrives}
                theme={isDarkTheme ? "willow-dark" : "willow"}
                previews={getPreview}
                extraInfo={getExtraInfo}
                init={initFilemanager}
                icons={getCustomIcon}
                menuOptions={customMenuOptions}
                toolbarActions={filesOverlay ? filesOverlayClose : null}
              />
            </Locale>
          {/key}
        </div>

        {#if showAddDevicePanel || autoConnectModal}
          <div class="empty-root-container modal-mode {isDarkTheme ? 'dark-theme' : 'light-theme'}">
            <div class="empty-root-card">
              <button class="empty-root-close" on:click={closeConnectModal}>&times;</button>
              <div class="empty-root-icon">
                <svg viewBox="0 0 24 24" width="44" height="44" fill="none" stroke="currentColor" stroke-width="1.75">
                  <rect x="2" y="3" width="20" height="14" rx="2" stroke="currentColor"/>
                  <line x1="8" y1="21" x2="16" y2="21" stroke="currentColor"/>
                  <line x1="12" y1="17" x2="12" y2="21" stroke="currentColor"/>
                </svg>
              </div>
              <h2 class="empty-root-title">{$_("connect_device", {}, "Connect Device")}</h2>
              <p class="empty-root-desc">
                {$_("connect_device_desc", {}, "Connect a Disk Direct node or client (Desktop, NAS, Raspberry Pi) using a 10-character Link ID.")}
              </p>
              <form
                on:submit|preventDefault={() => {
                  if (emptyStateLinkId.trim()) {
                    const id = emptyStateLinkId.trim();
                    emptyStateLinkId = "";
                    closeConnectModal();
                    openPairModal(id);
                  }
                }}
                class="empty-root-form"
              >

                <input
                  type="text"
                  bind:value={emptyStateLinkId}
                  placeholder={$_("enter_link_id_placeholder", {}, "Enter device Link ID")}
                  class="empty-root-input"
                  maxlength="64"
                />
                <button type="submit" class="empty-root-btn" disabled={!emptyStateLinkId.trim()}>
                  {$_("pair_connect_btn", {}, "Connect")}
                </button>
              </form>

              {#if detectedNearbyDevices.length > 0}
                <div class="empty-root-detected-section">
                  <div class="empty-root-detected-header">
                    <span>{$_("detected_in_network", {}, "Detected on Network")} ({detectedNearbyDevices.length})</span>
                  </div>
                  <div class="empty-root-detected-list">
                    {#each detectedNearbyDevices as dev}
                      <div class="empty-root-detected-item">
                        <div class="empty-root-detected-info">
                          <div class="empty-root-detected-icon">
                            <svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor"><path d="M4 5a2 2 0 0 0-2 2v2a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2H4zm0 8a2 2 0 0 0-2 2v2a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-2a2 2 0 0 0-2-2H4zm14-5a1 1 0 1 1 0-2 1 1 0 0 1 0 2zm0 8a1 1 0 1 1 0-2 1 1 0 0 1 0 2z"/></svg>
                          </div>
                          <div class="empty-root-detected-text">
                            <span class="empty-root-detected-name">{dev.name || dev.linkId}</span>
                            <span class="empty-root-detected-id">Link ID: <code>{dev.linkId}</code></span>
                          </div>
                        </div>
                        <button
                          type="button"
                          class="empty-root-detected-btn"
                          on:click={() => {
                            closeConnectModal();
                            openPairModal(dev.linkId, dev.name);
                          }}
                        >
                          {$_("pair_connect_btn", {}, "Connect")}
                        </button>
                      </div>
                    {/each}
                  </div>
                </div>
              {/if}

              <button class="p2p-action-btn empty-root-retry" on:click={retryDeviceConnection} disabled={localDeviceBusy || scanningNearbyDevices}>
                <svg class:spin={scanningNearbyDevices} viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.2" style="margin-right: 2px;"><path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/></svg>
                <span>{scanningNearbyDevices ? $_("scanning", {}, "Scanning...") : $_("rescan_network", {}, "Rescan")}</span>
              </button>

              {#if hasCameraSupport}
                <button
                  type="button"
                  class="p2p-action-btn empty-root-scan-qr"
                  on:click={() => { showQrScannerModal = true; }}
                >
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.2" style="margin-right: 4px;">
                    <rect x="3" y="3" width="7" height="7" rx="1"></rect>
                    <rect x="14" y="3" width="7" height="7" rx="1"></rect>
                    <rect x="14" y="14" width="7" height="7" rx="1"></rect>
                    <rect x="3" y="14" width="7" height="7" rx="1"></rect>
                  </svg>
                  <span>{$_("scan_qr", {}, "Scan QR")}</span>
                </button>
              {/if}

              {#if !isNativeStorageAvailable()}
                <div class="empty-root-divider"><span>{$_("or", {}, "or")}</span></div>

                <div class="empty-root-local">
                  <p class="empty-root-local-hint">
                    {$_("use_this_device", {}, "Use this device")}
                  </p>
                  <button
                    class="p2p-outline-btn"
                    on:click={addFolderOnThisDevice}
                    disabled={localDeviceBusy}
                  >
                    {#if localDeviceBusy}
                      <span class="empty-root-spinner"></span>
                      <span>{$_("adding_folder", {}, "Adding...")}</span>
                    {:else}
                      <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/><line x1="12" y1="11" x2="12" y2="17"/><line x1="9" y1="14" x2="15" y2="14"/></svg>
                      <span>{$_("add_folder_this_device", {}, "Add folder on this device")}</span>
                    {/if}
                  </button>
                  {#if localDeviceError}
                    <p class="empty-root-error">{localDeviceError}</p>
                  {/if}
                </div>
              {/if}
            </div>
          </div>
        {/if}
      </div>

      {#if currentApp === 'terminal' || currentApp === 'code'}
        <div class="sub-app-container">
          <Workspace
            type={currentApp === 'terminal' ? 'terminal' : 'editor'}
            targetLinkId={activeDevice ? activeDevice.linkId : targetLinkId}
            port={terminalPort}
            service="terminal"
            fileContent={appContent}
            path={currentApp === 'terminal' ? terminalInitialPath : appFilePath}
            filePath={appFilePath}
            fileName={appTitle}
            on:close={() => {
              currentApp = previousApp || "filemanager";
              previousApp = null;
              if (typeof document !== "undefined") {
                try { document.title = getDomainBrandTitle(); } catch (e) {}
              }
              let returnFolder = isRealFileManagerFolder(currentFolderId) ? currentFolderId : lastFileManagerFolderId;
              if (!isRealFileManagerFolder(returnFolder)) returnFolder = "/";
              currentFolderId = returnFolder;
              const targetHash = currentApp === "chat" ? "#/chat" : filesHash(returnFolder);
              updateUrlHash(targetHash);
              if (currentApp === "filemanager" && fmApi) {
                try { fmApi.exec("set-path", { id: returnFolder }); } catch (_) {}
              }
            }} 
          />
        </div>
      {/if}

      {#if currentApp === 'webapp' && activeWebApp}
        <div class="sub-app-container">
          <AppProxyView
            targetLinkId={activeWebApp.linkId}
            deviceName={activeWebApp.deviceName}
            appSlug={activeWebApp.slug}
            appName={activeWebApp.name}
            port={activeWebApp.port}
            entryPath={activeWebApp.entryPath}
            icon={activeWebApp.icon}
            {isDarkTheme}
            on:close={() => {
              currentApp = previousApp || "filemanager";
              previousApp = null;
              activeWebApp = null;
              if (typeof document !== "undefined") {
                try { document.title = getDomainBrandTitle(); } catch (_) {}
              }
              let returnFolder = isRealFileManagerFolder(currentFolderId) ? currentFolderId : lastFileManagerFolderId;
              if (!isRealFileManagerFolder(returnFolder)) returnFolder = "/";
              currentFolderId = returnFolder;
              const targetHash = currentApp === "chat" ? "#/chat" : filesHash(returnFolder);
              updateUrlHash(targetHash);
              if (currentApp === "filemanager" && fmApi) {
                try { fmApi.exec("set-path", { id: returnFolder }); } catch (_) {}
              }
            }}
          />
        </div>
      {/if}

      {#if currentApp === 'settings'}
        <div class="sub-app-container">
          <Settings
            {identity}
            {isDarkTheme}
            targetLinkId={activeDeviceId || activeDevice?.linkId || (defaultChatTarget()?.linkId ?? "")}
            port={4000}
            bind:activeSection={settingsSection}
            on:close={() => {
              currentApp = previousApp || "filemanager";
              previousApp = null;
              if (currentApp === "filemanager") {
                let returnFolder = isRealFileManagerFolder(currentFolderId) ? currentFolderId : lastFileManagerFolderId;
                if (!isRealFileManagerFolder(returnFolder)) returnFolder = "/";
                currentFolderId = returnFolder;
                updateUrlHash(filesHash(returnFolder));
                if (fmApi) {
                  try { fmApi.exec("set-path", { id: returnFolder }); } catch (_) {}
                }
              }
            }}
            on:logout={logoutIdentity}
            on:delete-account={deleteAccount}
            on:toggleTheme={(e) => { isDarkTheme = e.detail.dark; }}
            on:connectDevice={(e) => { openPairModal(e.detail.linkId, e.detail.name || ""); }}
            on:pairDevice={(e) => { openPairModal(e.detail.linkId, e.detail.name); }}
            on:disconnectDevice={(e) => { disconnectDevice(e.detail.linkId); }}

            on:googleLogin={(e) => handleGoogleCredential({ credential: e.detail.credential })}
            on:passwordLogin={(e) => { legacyUser = e.detail.user; legacyPass = e.detail.pass; passwordLogin(); }}
            on:keyfileLogin={(e) => { keyfileLogin(e.detail.content); }}
            on:register={(e) => { regUser = e.detail.user; regPass = e.detail.pass; regPassConfirm = e.detail.pass; regEmail = e.detail.email; regAvatar = e.detail.image || ""; registerAccount(); }}
            on:openQrScanner={() => { showQrScannerModal = true; }}
            on:restart-onboarding={() => { restartOnboardingFlow(); }}
          />
        </div>
      {/if}

      {#if chatAppMounted}
        <div class="sub-app-container" style:display={currentApp === 'chat' ? 'flex' : 'none'}>
          <ChatApplet
            bind:isChatLoading
            bind:activeChatId={chatActiveChatId}
            bind:activeView={chatActiveView}
            targetLinkId={activeDeviceId || activeDevice?.linkId || (defaultChatTarget()?.linkId ?? "")}
            targetDevice={chatTargetDevice}
            isPrivateMode={chatPrivateMode}
            ownLinkIds={ownDeviceLinkIds}
            {hasOwnHardwareNode}
            port={4000}
            isDarkTheme={isDarkTheme}
            isMainApp={appMode === APP_MODE_CHAT}
            on:close={closeChatsApp}
            on:open-files={() => openFilesApp()}
            on:open-settings={(e) => {
              previousApp = "chat";
              if (e && e.detail && e.detail.section) settingsSection = e.detail.section;
              currentApp = "settings";
            }}
            on:open-file={(e) => handleOpenFileEvent(e.detail)}
            on:open-folder={(e) => handleOpenFolderEvent(e.detail)}
          />
        </div>
      {/if}

      {#if (isSharedRoute || currentApp === 'singleview') && resolvedShareData}
        <SingleView
          shareData={resolvedShareData}
          {isDarkTheme}
          {targetLinkId}
          canEdit={!isSharedRoute && identityType === "persistent"}
          on:edit={(e) => {
            const d = (e && e.detail) || {};
            appTitle = d.name || '';
            appFilePath = d.path || '';
            appContent = d.content || '';
            previousApp = 'singleview';
            currentApp = 'code';
          }}
        />
      {/if}

      {#if showShareModal && shareTargetItem}
        <ShareModal
          item={shareTargetItem}
          activeDevice={shareTargetDevice || activeDevice}
          {identity}
          {isDarkTheme}
          on:close={() => { showShareModal = false; shareTargetItem = null; shareTargetDevice = null; }}
          on:shareCreated={() => reloadSharedByMe()}
          on:shareRevoked={() => reloadSharedByMe()}
        />
      {/if}

      {#if showAddShareModal}
        <AddShareModal
          {identity}
          {isDarkTheme}
          existingAddresses={sharedToMeNodes.map(n => n.shareAddress).filter(Boolean)}
          on:close={() => { showAddShareModal = false; }}
          on:added={handleSharedToMeAdded}
        />
      {/if}

      {#if showFolderPicker && pickerDevice}
        <FolderPicker
          device={pickerDevice}
          mode="folder"
          title="Add Folder"
          on:select={handlePickerSelect}
          on:cancel={() => { showFolderPicker = false; pickerDevice = null; }}
        />
      {/if}

      {#if showDeviceSettings && deviceSettingsDrive}
        <DeviceSettings
          drive={deviceSettingsDrive}
          deviceName={deviceSettingsName}
          deviceIcon={deviceSettingsIcon}
          deviceType={deviceSettingsType}
          {isDarkTheme}
          on:saved={(e) => {
            const s = e.detail || {};
            const savedDrive = deviceSettingsDrive;
            const linkId = savedDrive ? (savedDrive.linkId || savedDrive.deviceId || "") : "";
            const dev = linkId ? deviceRegistry.get(linkId) : null;
            if (s.hostname && savedDrive && dev) {
              try {
                dev.name = s.hostname;
                const newPrefix = "/" + s.hostname;
                if (dev.prefix !== newPrefix) {
                  const old = dev.prefix;
                  dev.prefix = newPrefix;
                  // migrate cached _deviceItems
                  if (window._deviceItems && window._deviceItems[old]) { window._deviceItems[newPrefix] = window._deviceItems[old]; delete window._deviceItems[old]; }
                  deviceNodes = deviceNodes.map(n => n.linkId === linkId ? { ...n, id: newPrefix, value: s.hostname, name: s.hostname } : n);
                  updateRootFilesData();
                } else {
                  deviceNodes = deviceNodes.map(n => n.linkId === linkId ? { ...n, value: s.hostname, name: s.hostname } : n);
                  updateRootFilesData();
                }
                deviceSettingsName = s.hostname;
              } catch (_) {}
            }
            if (s._rcloneChanged && dev) {
              const refreshDeviceRoot = () => {
                try {
                  const targetDev = linkId ? deviceRegistry.get(linkId) : dev;
                  const rootId = targetDev && targetDev.prefix;
                  if (fmApi && rootId) fmApi.exec("request-data", { id: rootId });
                } catch (_) {}
              };
              setTimeout(refreshDeviceRoot, 900);
              setTimeout(refreshDeviceRoot, 2600);
            }
          }}
          on:close={() => { showDeviceSettings = false; deviceSettingsDrive = null; deviceSettingsName = ""; deviceSettingsIcon = ""; deviceSettingsType = ""; }}
        />
      {/if}

      {#if nodePushToast}
        <div style="position: fixed; bottom: 24px; left: 50%; transform: translateX(-50%); z-index: 2147483646; background: var(--bg-elevated, #1D1829); color: var(--text-primary, #F8F7FC); border: 1px solid var(--accent-border, rgba(168, 85, 247, 0.45)); border-radius: 12px; padding: 10px 18px; font-size: 13.5px; box-shadow: 0 12px 32px rgba(0,0,0,0.45); max-width: min(92vw, 440px); text-align: center;">
          {nodePushToast}
        </div>
      {/if}

      {#if showNodeWidgetModal}
        <!-- svelte-ignore a11y-click-events-have-key-events -->
        <!-- svelte-ignore a11y-no-static-element-interactions -->
        <div
          on:click|self={() => { if (!nodeWidgetBusy) closeNodeWidgetModal(); }}
          style="position: fixed; inset: 0; z-index: 2147483645; background: rgba(13, 11, 18, 0.74); backdrop-filter: blur(6px); display: flex; align-items: center; justify-content: center; padding: 14px;"
        >
          <div style="width: min(96vw, 480px); max-height: 90vh; overflow-y: auto; background: var(--bg-elevated, #1D1829); color: var(--text-primary, #F8F7FC); border: 1px solid var(--border-color, #2D2640); border-radius: 16px; padding: 16px; box-shadow: 0 20px 50px rgba(0,0,0,0.55);">
            {#if nodeWidgetLoading}
              <div style="padding: 28px 12px; text-align: center; font-size: 13.5px; opacity: 0.82;">
                {$_("push_connecting_node", {}, "⏳ Connecting to node...")}
              </div>
            {:else if nodeWidgetError}
              <div style="display: flex; align-items: center; justify-content: space-between; gap: 10px;">
                <div style="font-size: 13.5px; color: #ff8f8d;">{nodeWidgetError}</div>
                <button
                  type="button"
                  on:click={closeNodeWidgetModal}
                  style="background: transparent; border: none; color: inherit; font-size: 18px; cursor: pointer; padding: 4px 8px;"
                >✕</button>
              </div>
            {/if}
            <div bind:this={nodeWidgetHostEl} style="display: {nodeWidgetLoading || nodeWidgetError ? 'none' : 'block'};"></div>
          </div>
        </div>
      {/if}




      {#if showConfirmRevokeModal}
        <!-- svelte-ignore a11y-click-events-have-key-events -->
        <!-- svelte-ignore a11y-no-static-element-interactions -->
        <div class="confirm-modal-overlay" on:click|self={() => (showConfirmRevokeModal = false)}>
          <div class="confirm-modal-box">
            <div class="confirm-modal-header">
              <i class="wxi-close confirm-danger-icon" />
              <h3>Revoke Public Link</h3>
            </div>
            <div class="confirm-modal-body">
              <p>Are you sure you want to revoke the public share link for <strong>{revokeTargetName}</strong>?</p>
              <p class="confirm-modal-subtext">Anyone with this link will no longer be able to view or download this file.</p>
            </div>
            <div class="confirm-modal-actions">
              <button class="confirm-btn cancel" on:click={() => (showConfirmRevokeModal = false)}>Cancel</button>
              <button class="confirm-btn danger" on:click={handleExecuteRevoke}>Revoke Link</button>
            </div>
          </div>
        </div>
      {/if}

      {#if showConfirmDisconnectModal && deviceToDisconnect}
        <!-- svelte-ignore a11y-click-events-have-key-events -->
        <!-- svelte-ignore a11y-no-static-element-interactions -->
        <div class="confirm-modal-overlay" on:click|self={() => (showConfirmDisconnectModal = false)}>
          <div class="confirm-modal-box">
            <div class="confirm-modal-header">
              <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="#f85149" stroke-width="2" class="confirm-danger-icon">
                <path d="M3 6h18M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M10 11v6M14 11v6"/>
              </svg>
              <h3>Disconnect Device</h3>
            </div>
            <div class="confirm-modal-body">
              <p>Are you sure you want to disconnect and forget device <strong>{deviceToDisconnect.name}</strong>?</p>
              <p class="confirm-modal-subtext">This device will be removed from your profile and will no longer connect automatically.</p>
            </div>
            <div class="confirm-modal-actions">
              <button class="confirm-btn cancel" on:click={() => (showConfirmDisconnectModal = false)}>Cancel</button>
              <button class="confirm-btn danger" on:click={handleExecuteDisconnectDevice}>Disconnect & Forget</button>
            </div>
          </div>
        </div>
      {/if}

      {#if showAiPublicAgreement}
        <div class="confirm-modal-overlay {isDarkTheme ? 'dark-theme' : 'light-theme'}" on:click|self={onAiPublicAgreementDecline}>
          <div class="confirm-modal-box omd-ai-agreement {isDarkTheme ? 'dark-theme' : 'light-theme'}">
            <div class="confirm-modal-header">
              <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M21 11.5a8.38 8.38 0 0 1-.9 3.8 8.5 8.5 0 0 1-7.6 4.7 8.38 8.38 0 0 1-3.8-.9L3 21l1.9-5.7a8.38 8.38 0 0 1-.9-3.8 8.5 8.5 0 0 1 4.7-7.6 8.38 8.38 0 0 1 3.8-.9h.5a8.48 8.48 0 0 1 8 8v.5z"/>
              </svg>
              <h3>{$_("ai_public_agreement_title", {}, "Responsible Use Agreement")}</h3>
            </div>
            <div class="confirm-modal-body" style="max-height: 45vh; overflow-y: auto; text-align: left;">
              <p>{$_("ai_public_agreement_body_1", {}, "You are about to connect to the AI Assistant Public Node — a shared system node operated over a direct peer-to-peer channel. Chats, history, and your personal knowledge stay encrypted on your side; public knowledge and queries are processed by the public node.")}</p>
              <p>{$_("ai_public_agreement_body_2", {}, "You agree to use the service responsibly and lawfully, and in particular NOT to generate unlawful, harmful, or abusive content, including content involving minors, violence, weapons, malware, or any activity prohibited by applicable law. Attempting to bypass content filters or abusing shared resources is not allowed.")}</p>
              <p class="confirm-modal-subtext">{$_("ai_public_agreement_body_3", {}, "The service is provided on an \"as is\" basis without warranties. Usage may be rate-limited, and access may be revoked in case of abuse. Your acceptance is stored in your encrypted settings.")}</p>
            </div>
            <div class="confirm-modal-actions">
              <button class="confirm-btn cancel" on:click={onAiPublicAgreementDecline}>{$_("ai_public_decline", {}, "Cancel")}</button>
              <button class="confirm-btn primary" on:click={onAiPublicAgreementAccept}>{$_("ai_public_agree", {}, "I Agree")}</button>
            </div>
          </div>
        </div>
      {/if}

      {#if showRemoteDeviceInfo}
        <!-- svelte-ignore a11y-click-events-have-key-events -->
        <!-- svelte-ignore a11y-no-static-element-interactions -->
        <div class="modal-overlay" on:click|self={() => { showRemoteDeviceInfo = false; }}>
          <div class="share-modal connect-modal" style="max-width: 420px;">
            <div class="share-modal-header">
              <div class="share-modal-title">
                <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
                  <circle cx="12" cy="12" r="10"/>
                  <line x1="12" y1="16" x2="12" y2="12"/>
                  <line x1="12" y1="8" x2="12.01" y2="8"/>
                </svg>
                <h3>Добавление папок</h3>
              </div>
              <button class="share-modal-close" on:click={() => { showRemoteDeviceInfo = false; }}>
                <i class="wxi-close"></i>
              </button>
            </div>
            <div class="share-modal-body">
              <p class="share-modal-desc">
                Чтобы добавить папку на удалённом веб-устройстве, откройте его в браузере
                и используйте кнопку «Add folder» на корневом уровне.
              </p>
              <div style="display: flex; justify-content: flex-end; margin-top: 16px;">
                <button type="button" class="btn-primary" style="height: 36px; padding: 0 16px;" on:click={() => { showRemoteDeviceInfo = false; }}>Понятно</button>
              </div>
            </div>
          </div>
        </div>
      {/if}

      {#if docLoadLabel && !viewerFile}
        <div class="doc-load-topbar" role="status">
          <span class="doc-load-label">{docLoadLabel}</span>
          <div class="doc-load-track"><div class="doc-load-fill"></div></div>
        </div>
      {/if}

      {#if downloadingFile && !viewerFile && !docLoadLabel}
        <div class="progress-overlay">
          <div class="progress-box">
            {#if isFolderLoading}
              <p>{downloadingFile}</p>
              <div class="progress-bar">
                <div class="progress-fill indeterminate"></div>
              </div>
            {:else}
              <p>Downloading {downloadingFile}...</p>
              <div class="progress-bar">
                <div class="progress-fill" style="width: {downloadProgress}%"></div>
              </div>
              <p class="progress-text">{downloadProgress}% {#if downloadSpeed}({downloadSpeed}){/if}</p>
            {/if}
            <button class="cancel-btn" on:click={cancelOperation}>Cancel</button>
          </div>
        </div>
      {/if}

      {#if viewerFile}
        <div class="viewer-modal {isDarkTheme ? 'dark-theme' : 'light-theme'}">
          <div class="viewer-body">
            {#if viewerFile.type === 'pdf'}
              <PdfViewer 
                url={viewerFile.url} 
                on:close={closeViewer} 
                on:share={() => openShareModal(viewerFile)} 
                on:sign={() => alert('Sign feature coming soon!')}
                on:toggleFullscreen={() => {
                  if (!document.fullscreenElement && !document.webkitFullscreenElement) {
                    if (document.documentElement.requestFullscreen) document.documentElement.requestFullscreen().catch(() => {});
                  } else {
                    if (document.exitFullscreen) document.exitFullscreen().catch(() => {});
                  }
                }}
              />
            {:else if viewerFile.type === 'video'}
              <VideoViewer
                url={viewerFile.url}
                poster={viewerFile.poster || (viewerFile.url ? (viewerFile.url.includes('?') ? viewerFile.url + '&poster' : viewerFile.url + '?poster') : '')}
                filename={viewerFile.name}
                title={viewerFile.title || viewerFile.name}
                description={viewerFile.description || ''}
                playlist={viewerFile.playlist || []}
                currentIndex={viewerFile.currentIndex || 0}
                on:change={(e) => handleVideoChange(e.detail)}
                on:close={(e) => {
                  if (document.fullscreenElement || document.webkitFullscreenElement) {
                    if (document.exitFullscreen) document.exitFullscreen().catch(() => {});
                    else if (document.webkitExitFullscreen) document.webkitExitFullscreen();
                  }
                  closeViewer(e.detail);
                }}
              />
            {:else if viewerFile.type === 'audio'}
              <AudioViewer
                url={viewerFile.url}
                poster={viewerFile.poster || ''}
                hasCover={viewerFile.hasCover || false}
                filename={viewerFile.name}
                title={viewerFile.title || viewerFile.name}
                description={viewerFile.description || ''}
                playlist={viewerFile.playlist || []}
                currentIndex={viewerFile.currentIndex || 0}
                on:change={(e) => handleAudioChange(e.detail)}
                on:close={(e) => closeViewer(e.detail)}
              />
            {:else if viewerFile.type === 'markdown'}
              <MarkdownViewer
                url={viewerFile.url || ''}
                content={viewerFile.content || ''}
                filename={viewerFile.name || ''}
                {isDarkTheme}
                canEdit={mdViewerCanEdit}
                on:edit={(e) => {
                  appTitle = viewerFile.name || '';
                  appFilePath = viewerFile.path || '';
                  appContent = (e && e.detail && e.detail.content) || viewerFile.content || '';
                  previousApp = 'filemanager';
                  closeViewer(null);
                  currentApp = 'code';
                }}
                on:close={closeViewer}
              />
            {:else if viewerFile.type === 'text'}
              <Editor filename={viewerFile.name} value={viewerFile.content} on:close={closeViewer} />
            {:else if viewerFile.type === 'doc'}
              {#key viewerFile.id}
                <MarkdownViewer
                  filename={viewerFile.name}
                  content={viewerFile.content || ''}
                  {isDarkTheme}
                  on:close={closeViewer}
                />
              {/key}

            {:else if viewerFile.type === 'wordcraft'}
              <WordCraftEditor
                filename={viewerFile.name}
                filePath={viewerFile.path || viewerFile.name}
                bytes={viewerFile.bytes || null}
                activeDevice={viewerFile.activeDevice || activeDevice}
                isDarkTheme={isDarkTheme}
                on:close={closeViewer}
                on:failed={handleWordCraftFailed}
              />
            {:else if viewerFile.type === 'gridcraft'}
              <GridCraftEditor
                filename={viewerFile.name}
                filePath={viewerFile.path || viewerFile.name}
                bytes={viewerFile.bytes || null}
                activeDevice={viewerFile.activeDevice || activeDevice}
                on:close={closeViewer}
                on:failed={handleGridCraftFailed}
              />
            {:else if viewerFile.type === 'deckcraft'}
              <DeckCraftEditor
                filename={viewerFile.name}
                filePath={viewerFile.path || viewerFile.name}
                bytes={viewerFile.bytes || null}
                activeDevice={viewerFile.activeDevice || activeDevice}
                on:close={closeViewer}
                on:failed={handleDeckCraftFailed}
              />
            {:else if viewerFile.type === 'sheet'}
              {#key viewerFile.id}
                <MarkdownViewer
                  filename={viewerFile.name}
                  content={viewerFile.content || ''}
                  {isDarkTheme}
                  on:close={closeViewer}
                />
              {/key}

            {/if}
          </div>
        </div>
      {/if}
    {:else}
      {#if isSharedRoute && sharePasswordModal}
        <div class="login-body {isDarkTheme ? 'dark-theme' : 'light-theme'}">
          <div class="login-card">
            <div class="login-logo">
              <img src={brandLogoSrc} alt={brandName} height="60" on:error={() => { brandLogoBroken = true; }} />
            </div>
            <h2 class="login-title mb-2">Password Protected</h2>
            <p class="login-subtitle mb-4">This shared item is encrypted. Enter password to view.</p>
            <form on:submit|preventDefault={() => handleOpenSharedRoute(sharedAddress, sharePasswordInput)}>
              <div class="mb-3">
                <input
                  class="form-control"
                  type="password"
                  placeholder="Enter share password"
                  bind:value={sharePasswordInput}
                  autofocus
                />
              </div>
              {#if sharePasswordError}
                <p class="login-error">{sharePasswordError}</p>
              {/if}
              <button class="btn btn-primary w-100" type="submit" disabled={shareLoading || !sharePasswordInput.trim()}>
                {shareLoading ? 'Decrypting...' : 'Unlock Share'}
              </button>
            </form>
          </div>
        </div>
      {:else if isSharedRoute && shareLoading}
        <div class="p2p-loading-overlay {isDarkTheme ? 'dark-theme' : 'light-theme'}">
          <div class="p2p-loading-box text-center">
            <img src={brandLogoSrc} alt={brandName} class="p2p-brand-logo" />
            <div class="p2p-reconnect-spinner" style="width: 36px; height: 36px; min-width: 36px; min-height: 36px; max-width: 36px; max-height: 36px; border-width: 3px; margin: 20px auto 20px auto;"></div>
            <h3 class="p2p-status-title mb-2">Connecting to P2P Share...</h3>
            <p class="p2p-status-desc mb-0" style="max-width: 310px; margin: 0 auto; line-height: 1.5; opacity: 0.75;">
              Establishing direct peer-to-peer connection with the device...
            </p>
          </div>
        </div>
      {:else if isSharedRoute && shareError}
        <div class="p2p-loading-overlay {isDarkTheme ? 'dark-theme' : 'light-theme'}">
          <div class="p2p-loading-box text-center">
            <img src={brandLogoSrc} alt={brandName} class="p2p-brand-logo" />

            <div class="p2p-error-icon-badge my-3">
              <svg viewBox="0 0 24 24" width="30" height="30" fill="none" stroke="#f87171" stroke-width="1.8">
                <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/>
                <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>
                <line x1="2" y1="2" x2="22" y2="22" stroke="#ef4444" stroke-width="2.2"/>
              </svg>
            </div>

            <h3 class="p2p-status-title mb-2">Share Link Unavailable</h3>
            <p class="p2p-status-desc mb-4" style="max-width: 310px; margin: 0 auto 24px auto; line-height: 1.5;">
              {shareError || "This shared item could not be reached. The owner device may be offline, the link has expired, or the connection timed out."}
            </p>

            <div class="p2p-btn-stack">
              <button class="p2p-action-btn" on:click={() => handleOpenSharedRoute(sharedAddress)}>
                <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.2" style="margin-right: 2px;"><path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/></svg>
                <span>Try Again</span>
              </button>
              <a href="/" class="p2p-outline-btn">
                Go to {brandName} Home
              </a>
            </div>
          </div>
        </div>
      {:else if isSharedRoute}
        <div class="p2p-loading-overlay {isDarkTheme ? 'dark-theme' : 'light-theme'}">
          <div class="p2p-loading-box text-center">
            <img src={brandLogoSrc} alt={brandName} class="p2p-brand-logo" />
            <div class="p2p-reconnect-spinner" style="width: 36px; height: 36px; min-width: 36px; min-height: 36px; max-width: 36px; max-height: 36px; border-width: 3px; margin: 20px auto 20px auto;"></div>
            <h3 class="p2p-status-title mb-2">Connecting to P2P Share...</h3>
          </div>
        </div>
      {:else}
        <div class="login-body">
          <div class="login-card" class:loading={loginLoading}>
            <!-- Language Selector (top right) -->
            <div class="login-lang-select-wrap">
              <div class="login-lang-display" title={$_("language", {}, "Language")}>
                <svg class="login-lang-globe" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <circle cx="12" cy="12" r="10"></circle>
                  <line x1="2" y1="12" x2="22" y2="12"></line>
                  <path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"></path>
                </svg>
                <span class="login-lang-code">{activeLoginLangMeta.shortCode || activeLoginLangMeta.code.toUpperCase()}</span>
                <svg class="login-lang-arrow" viewBox="0 0 24 24" width="11" height="11" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>
              </div>
              <select
                class="login-lang-select-hidden"
                value={$locale}
                on:change={(e) => {
                  const val = e.target.value;
                  setLocale(val);
                  if (identity && identity.updateProfile) {
                    identity.updateProfile({ locale: val }).catch(() => {});
                  }
                }}
                aria-label={$_("language", {}, "Language")}
              >
                {#each SUPPORTED_LANGUAGES as lang}
                  <option value={lang.code}>{lang.nativeName} ({lang.shortCode || lang.code.toUpperCase()})</option>
                {/each}
              </select>
            </div>

            <div class="login-logo">
              <img src={brandLogoSrc} alt={brandName} height="60" on:error={() => { brandLogoBroken = true; }} />
            </div>

            <input
              type="file"
              accept=".json,application/json"
              bind:this={loginKeyfileInput}
              on:change={handleKeyfileInputChange}
              style="display: none;"
            />

            {#if loginLoading}
              <div class="login-loading-centered">
                <div class="login-spinner-circle mb-3"></div>
                <div class="login-loading-text">{$_("signin", {}, "Sign in")}</div>
              </div>
            {:else if loginMode === 'select'}
              <h2 class="login-title mb-4 text-center">{$_("signin_or_connect", {}, "Sign in or connect")}</h2>

              <!-- Password login (primary, like legacy) -->
              <button class="btn btn-primary w-100 mb-3 d-flex align-items-center justify-content-center gap-2" on:click={() => loginMode = 'password'}>
                  <svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" style="flex-shrink:0"><path d="M0 8a4 4 0 0 1 7.465-2H14a.5.5 0 0 1 .354.146l1.5 1.5a.5.5 0 0 1 0 .708l-1.5 1.5a.5.5 0 0 1-.708 0L13 9.207l-.646.647a.5.5 0 0 1-.708 0L11 9.207l-.646.647a.5.5 0 0 1-.708 0L9 9.207l-.646.647A.5.5 0 0 1 8 10h-.535A4 4 0 0 1 0 8m4-3a3 3 0 1 0 2.712 4.285A.5.5 0 0 1 7.163 9h.63a.5.5 0 0 1 .354.146l.646.647.646-.647a.5.5 0 0 1 .708 0l.646.647.646-.647a.5.5 0 0 1 .708 0l.646.647.646-.647a.5.5 0 0 1 .5-.146H13l1-1-1-1H7.163a.5.5 0 0 1-.451-.285A3 3 0 0 0 4 5"/><path d="M4 8a1 1 0 1 1-2 0 1 1 0 0 1 2 0"/></svg>
                  <span>{$_("signin_with_id", {}, "Sign in with ID")}</span>
                </button>

                <!-- 1-click JSON Keyfile login -->
                <div class="login-btn-row mb-3">
                  <button
                    type="button"
                    class="btn btn-outline-secondary w-100 d-flex align-items-center justify-content-center gap-2"
                    on:click={() => loginKeyfileInput && loginKeyfileInput.click()}
                  >
                    <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="flex-shrink:0">
                      <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"></path>
                      <polyline points="14 2 14 8 20 8"></polyline>
                      <circle cx="10" cy="15" r="2"></circle>
                      <path d="M12 15h4v2"></path>
                    </svg>
                    <span>{$_("signin_with_keyfile", {}, "Sign in with Keyfile")}</span>
                  </button>
                </div>

                <!-- Google Sign-In -->
                <div class="login-btn-row mb-3" style="position: relative; height: 44px;">
                  {#if Capacitor.isNativePlatform()}
                    <button
                      class="btn btn-outline-secondary w-100 d-flex align-items-center justify-content-center gap-2"
                      style="height: 100%;"
                      disabled={loginLoading}
                      on:click={handleMobileGoogleSignIn}
                    >
                      <svg viewBox="0 0 24 24" width="18" height="18" style="flex-shrink:0"><path fill="#4285F4" d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92a5.06 5.06 0 0 1-2.2 3.32v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.1z"/><path fill="#34A853" d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z"/><path fill="#FBBC05" d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.07H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.93l2.85-2.22.81-.62z"/><path fill="#EA4335" d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.07l3.66 2.84c.87-2.6 3.3-4.53 6.16-4.53z"/></svg>
                      <span>{$_("signin_google", {}, "Sign in with Google")}</span>
                    </button>
                  {:else}
                    <button class="btn btn-outline-secondary w-100 d-flex align-items-center justify-content-center gap-2" style="position: absolute; top: 0; left: 0; height: 100%; z-index: 1; pointer-events: none;" tabindex="-1">
                      <svg viewBox="0 0 24 24" width="18" height="18" style="flex-shrink:0"><path fill="#4285F4" d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92a5.06 5.06 0 0 1-2.2 3.32v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.1z"/><path fill="#34A853" d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z"/><path fill="#FBBC05" d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.07H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.93l2.85-2.22.81-.62z"/><path fill="#EA4335" d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.07l3.66 2.84c.87-2.6 3.3-4.53 6.16-4.53z"/></svg>
                      <span>{$_("signin_google", {}, "Sign in with Google")}</span>
                    </button>
                    <div use:googleButtonAction style="position: absolute; top: 0; left: 0; width: 100%; height: 100%; z-index: 10; opacity: 0.001; overflow: hidden; cursor: pointer;"></div>
                  {/if}
                </div>

                <!-- Apple Sign-In -->
                <div class="login-btn-row mb-3">
                  <button
                    class="btn btn-outline-secondary w-100 d-flex align-items-center justify-content-center gap-2"
                    on:click={handleAppleSignIn}
                  >
                    <svg viewBox="0 0 170 170" width="18" height="18" fill="currentColor" style="flex-shrink: 0; margin-bottom: 2px;">
                      <path d="M150.37 130.25c-2.45 5.66-5.35 10.87-8.71 15.66-4.58 6.53-8.33 11.05-11.22 13.56-4.48 4.12-9.28 6.23-14.42 6.35-3.69 0-8.14-1.05-13.32-3.18-5.19-2.12-9.97-3.17-14.34-3.17-4.58 0-9.49 1.05-14.75 3.17-5.26 2.13-9.5 3.24-12.74 3.35-4.35.13-9.16-1.9-14.42-6.08-3.7-3.04-7.6-7.77-11.7-14.19-5.77-8.91-10.28-18.73-13.53-29.47-3.25-10.74-4.88-21.13-4.88-31.18 0-13.91 3.53-25.54 10.6-34.89 7.07-9.35 15.82-14.13 26.24-14.34 5.01 0 10.22 1.25 15.63 3.75 5.41 2.5 9.07 3.86 10.99 4.08 1.93-.22 5.64-1.58 11.13-4.08 5.49-2.5 10.43-3.65 14.83-3.46 9.68.43 17.78 4.12 24.31 11.07-8.59 5.22-12.8 12.38-12.63 21.48.16 7.6 2.98 13.96 8.44 19.08 5.46 5.12 12.08 8.01 19.86 8.68-2.18 6.32-4.88 12.83-8.1 19.53zM119.22 32.73c0-7.39 2.66-14.41 7.98-21.06 5.32-6.65 11.98-10.88 19.98-12.69.11 1.09.16 2.07.16 2.94 0 7.39-2.77 14.46-8.31 21.21-5.54 6.75-12.33 10.79-20.37 12.13-.11-.87-.16-1.71-.16-2.53z"/>
                    </svg>
                    <span>{$_("signin_apple", {}, "Sign in with Apple")}</span>
                  </button>
                </div>

                <!-- Direct device connection (icon = tree device icon) -->
                <div class="login-btn-row mb-3">
                  <button class="btn btn-outline-secondary w-100 d-flex align-items-center justify-content-center gap-2" on:click={() => loginMode = 'key'}>
                    <svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" style="flex-shrink:0"><path d="M0 10a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v1a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2zm2.5 1a.5.5 0 1 0 0-1 .5.5 0 0 0 0 1m2 0a.5.5 0 1 0 0-1 .5.5 0 0 0 0 1M.91 7.204A3 3 0 0 1 2 7h12c.384 0 .752.072 1.09.204l-1.867-3.422A1.5 1.5 0 0 0 11.906 3H4.094a1.5 1.5 0 0 0-1.317.782z"/></svg>
                    <span>{$_("connect_device", {}, "Connect to device")}</span>
                  </button>
                </div>

                {#if loginError}
                  <p class="login-error">{loginError}</p>
                {/if}

                <div class="login-footer">
                  <div class="login-footer-switch">
                    <span class="text-secondary">{$_("dont_have_id", {}, "Don't have an ID?")}</span>
                    <button class="small-link" on:click={() => { loginMode = 'register'; loginError = ''; }}>{$_("create_your_id", {}, "Create your ID")}</button>
                  </div>

                  <div class="login-footer-terms">
                    {$_("terms_continue_agree", {}, "By continuing, you agree to our")} 
                    <a href={$gatewayConfig ? getGatewayTosUrl() : "/tos"} target="_blank" rel="noopener noreferrer">{$_("terms_of_service", {}, "Terms of Service")}</a> &amp; 
                    <a href={$gatewayConfig ? getGatewayPrivacyUrl() : "/tos#gdpr"} target="_blank" rel="noopener noreferrer">{$_("gdpr_cookies_ai_policy", {}, "GDPR, Cookies and AI Policy")}</a>.
                  </div>
                  {#if appMode !== APP_MODE_CHAT}
                    <div class="login-footer-gateway" style="margin-top: 10px; font-size: 12px; opacity: 0.82; display: flex; align-items: center; justify-content: center; gap: 6px;">
                      <span class="text-secondary">{$_("gateway_lbl", {}, "Gateway")}: <strong>{$gatewayConfig.host}</strong></span>
                      <span>&middot;</span>
                      <button type="button" class="small-link" style="font-size: 12px;" on:click={() => openGatewaySwitchWithUrl("")}>
                        {$_("change_btn", {}, "Change")}
                      </button>
                    </div>
                  {/if}
                </div>

          {:else if loginMode === 'key'}
            <!-- Link ID entry (login-key.html style) -->
            <h2 class="login-title">{$_("connect_device", {}, "Connect to device")}</h2>
            <p class="login-subtitle">{$_("enter_link_id_subtitle", {}, "Enter the Link ID of the device you want to access")}</p>

            {#if status === 'connecting'}
              <div class="login-loading-box text-center py-4 my-3">
                <div class="login-spinner-circle mb-3"></div>
                <div class="login-loading-text">{$_("Connecting", {}, "Connecting to device...")}</div>
              </div>
            {:else}
              <form on:submit|preventDefault={addDevice}>
                <div class="mb-3">
                  <input
                    class="form-control"
                    type="text"
                    placeholder={$_("enter_link_id_placeholder", {}, "Enter 10-character Link ID")}
                    bind:value={targetLinkId}
                    autofocus
                  />
                </div>

                <button class="btn btn-primary w-100 d-flex align-items-center justify-content-center gap-2" type="submit" disabled={!targetLinkId.trim()}>
                  <span>{$_("connect", {}, "Connect")}</span>
                </button>
              </form>

              {#if status === 'error'}
                <p class="login-error">{$_("connection_failed_check_id", {}, "Connection failed. Check the Link ID and try again.")}</p>
              {/if}

              <div class="text-center mt-4">
                <button class="small-link" on:click={() => { loginMode = 'select'; status = 'disconnected'; }}>
                  {$_("go_back", {}, "← Go back")}
                </button>
              </div>
            {/if}

          {:else if loginMode === 'password'}
            <!-- Password entry (login-user.html style) -->
            <h2 class="login-title">{$_("signin_with_id", {}, "Sign in with ID")}</h2>
            <p class="login-subtitle">{$_("login_enter_credentials", {}, "Enter your nickname + password, or load your .json keyfile")}</p>

            <form on:submit|preventDefault={passwordLogin}>
              <div class="mb-3">
                <label class="form-label" for="login-user">{$_("nickname_or_did", {}, "Nickname or DID")}</label>
                <input class="form-control" id="login-user" type="text" placeholder={$_("enter_nickname_or_did_placeholder", {}, "Nickname (e.g. Alice), Name#tag, or did:gun:...")} bind:value={legacyUser} autocapitalize="none" autocorrect="off" spellcheck="false" autofocus />
              </div>

              <div class="mb-3">
                <label class="form-label" for="login-pass">{$_("password", {}, "Password")}</label>
                <input class="form-control" id="login-pass" type="password" placeholder={$_("enter_password_placeholder", {}, "Enter password")} bind:value={legacyPass} autocapitalize="none" autocorrect="off" spellcheck="false" />
              </div>

              <div class="login-links mb-4">
                <button type="button" class="small-link" on:click={() => { loginMode = 'select'; loginError = ''; }}>{$_("go_back", {}, "← Go back")}</button>
              </div>

              <button class="btn btn-primary w-100 mb-2 d-flex align-items-center justify-content-center gap-2" type="submit">
                <span>{$_("signin", {}, "Sign in")}</span>
              </button>

              <button
                type="button"
                class="btn btn-outline-secondary w-100 mb-3 d-flex align-items-center justify-content-center gap-2"
                on:click={() => loginKeyfileInput && loginKeyfileInput.click()}
              >
                <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="flex-shrink:0">
                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path>
                  <polyline points="17 8 12 3 7 8"></polyline>
                  <line x1="12" y1="3" x2="12" y2="15"></line>
                </svg>
                <span>{$_("load_keyfile_btn", {}, "Load Keyfile")}</span>
              </button>
            </form>

            {#if loginError}
              <p class="login-error">{loginError}</p>
            {/if}

            <div class="login-footer">
              <div class="login-footer-switch">
                <span class="text-secondary">{$_("dont_have_id", {}, "Don't have an ID?")}</span>
                <button class="small-link" on:click={() => { loginMode = 'register'; loginError = ''; }}>{$_("create_your_id", {}, "Create your ID")}</button>
              </div>
            </div>

          {:else if loginMode === 'register'}
            <!-- Registration (register.html style) -->
            <h2 class="login-title">{$_("create_your_id", {}, "Create your ID")}</h2>
            <p class="login-subtitle">{$_("register_subtitle", {}, "Your identity is stored locally — no central server")}</p>

            <form on:submit|preventDefault={registerAccount}>
              <div class="avatar-edit-controls mb-3" style="display: flex; gap: 1rem; align-items: center; justify-content: center; width: 100%;">
                <div class="avatar-preview-box" style="width: 64px; height: 64px; border-radius: 50%; overflow: hidden; background: var(--bg-soft, rgba(168, 85, 247, 0.12)); border: 1px solid var(--border-color);">
                  {#if regAvatar}
                    <img src={regAvatar} alt="" referrerpolicy="no-referrer" style="width: 100%; height: 100%; object-fit: cover;" />
                  {:else}
                    <div style="display: flex; align-items: center; justify-content: center; width: 100%; height: 100%; font-size: 24px; font-weight: 700; color: var(--accent);">
                      {(regUser || "U").charAt(0).toUpperCase()}
                    </div>
                  {/if}
                </div>
                <div class="avatar-buttons-col" style="display: flex; flex-direction: column; gap: 0.5rem;">
                  <label class="btn btn-outline-secondary btn-sm" style="cursor: pointer;">
                    <span>{regAvatar ? 'Change Photo' : 'Add Photo'}</span>
                    <input type="file" accept="image/*" on:change={handleRegAvatarChange} style="display: none;" />
                  </label>
                  {#if regAvatar}
                    <button type="button" class="btn btn-link btn-sm" on:click={() => { regAvatar = ""; }}>
                      Remove
                    </button>
                  {/if}
                </div>
              </div>

              <div class="mb-3">
                <label class="form-label" for="reg-user">{$_("display_nickname_label", {}, "Nickname / Display Name")}</label>
                <input class="form-control" id="reg-user" type="text" placeholder={$_("choose_nickname_placeholder", {}, "Any display name (e.g. Alice)")} bind:value={regUser} autocorrect="off" spellcheck="false" autofocus />
                <small class="text-secondary" style="display: block; margin-top: 5px; font-size: 11.5px; line-height: 1.35;">
                  {$_("decentralized_nickname_hint", {}, "Doesn't need to be unique — your identity is a cryptographic keypair (DID), not a central server account.")}
                </small>
              </div>

              <div class="mb-3">
                <label class="form-label" for="reg-email">{$_("email_optional_hint", {}, "Email (optional, for recovery)")}</label>
                <input class="form-control" id="reg-email" type="email" placeholder="Enter email" bind:value={regEmail} autocapitalize="none" autocorrect="off" spellcheck="false" />
              </div>

              <div class="mb-3">
                <label class="form-label" for="reg-pass">{$_("password", {}, "Password")}</label>
                <input class="form-control" id="reg-pass" type="password" placeholder={$_("password_min_chars_placeholder", {}, "At least 8 characters")} bind:value={regPass} autocapitalize="none" autocorrect="off" spellcheck="false" />
              </div>

              <div class="mb-4">
                <label class="form-label" for="reg-pass2">{$_("confirm_password_label", {}, "Confirm password")}</label>
                <input class="form-control" id="reg-pass2" type="password" placeholder={$_("repeat_password_placeholder", {}, "Repeat password")} bind:value={regPassConfirm} autocapitalize="none" autocorrect="off" spellcheck="false" />
              </div>

              <div class="login-links mb-4">
                <button type="button" class="small-link" on:click={() => { loginMode = 'select'; loginError = ''; }}>{$_("go_back", {}, "← Go back")}</button>
              </div>

              <button class="btn btn-primary w-100 d-flex align-items-center justify-content-center gap-2" type="submit">
                <span>{$_("create_id_btn", {}, "Create ID")}</span>
              </button>
            </form>

            {#if loginError}
              <p class="login-error">{loginError}</p>
            {/if}

            <div class="login-footer">
              <div class="login-footer-switch">
                <span class="text-secondary">{$_("already_have_id", {}, "Already have an ID?")}</span>
                <button class="small-link" on:click={() => { loginMode = 'password'; loginError = ''; }}>{$_("signin", {}, "Sign in")}</button>
              </div>

              <div class="login-footer-terms">
                {$_("by_creating_agree", {}, "By creating an ID, you agree to our")} 
                <a href={$gatewayConfig ? getGatewayTosUrl() : "/tos"} target="_blank" rel="noopener noreferrer">{$_("terms_of_service", {}, "Terms of Service")}</a> &amp; 
                <a href={$gatewayConfig ? getGatewayPrivacyUrl() : "/tos#gdpr"} target="_blank" rel="noopener noreferrer">{$_("gdpr_cookies_ai_policy", {}, "GDPR, Cookies and AI Policy")}</a>.
              </div>
              {#if appMode !== APP_MODE_CHAT}
                <div class="login-footer-gateway" style="margin-top: 10px; font-size: 12px; opacity: 0.82; display: flex; align-items: center; justify-content: center; gap: 6px;">
                  <span class="text-secondary">{$_("gateway_lbl", {}, "Gateway")}: <strong>{$gatewayConfig.host}</strong></span>
                  <span>&middot;</span>
                  <button type="button" class="small-link" style="font-size: 12px;" on:click={() => openGatewaySwitchWithUrl("")}>
                    {$_("change_btn", {}, "Change")}
                  </button>
                </div>
              {/if}
            </div>

          {:else if loginMode === 'recovery'}
            <!-- Keyfile backup display (after registration) -->
            <h2 class="login-title">{$_("save_did_keyfile_title", {}, "Save your DID keyfile")}</h2>
            <p class="login-subtitle">{$_("save_did_keyfile_subtitle", {}, "Download your cryptographic keypair (.json) in one click to sign in or recover your DID on any device.")}</p>

            <div class="recovery-did mb-2" style="display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 10px 12px; border-radius: 10px; background: var(--bg-soft); border: 1px solid var(--accent-border);">
              <span style="font-weight: 600; font-size: 14px;">{identity?.profile?.name || regUser || "User"}</span>
              {#if getDidTag(identityDid)}
                <code style="font-size: 12px; font-weight: 700; padding: 2px 8px; border-radius: 999px; background: var(--accent-soft); color: var(--accent);">#{getDidTag(identityDid)}</code>
              {/if}
            </div>

            <div class="recovery-did mb-3">
              <span class="recovery-label">{$_("your_did", {}, "Your DID")}</span>
              <code class="recovery-value">{identityDid}</code>
            </div>

            <div class="recovery-actions mt-3" style="display: flex; flex-direction: column; gap: 10px;">
              <button class="btn btn-primary w-100 d-flex align-items-center justify-content-center gap-2" on:click={handleSaveDidKeyfile}>
                <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" style="flex-shrink:0">
                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path>
                  <polyline points="7 10 12 15 17 10"></polyline>
                  <line x1="12" y1="15" x2="12" y2="3"></line>
                </svg>
                <span>{keyfileSavedOnce ? $_("keyfile_saved_btn", {}, "✓ Keyfile Saved") : $_("save_keyfile_json_btn", {}, "Save Keyfile")}</span>
              </button>
              <button class="btn btn-outline-secondary w-100" on:click={() => { navigator.clipboard.writeText(identityDid); showToast($_("copied", {}, "Copied to clipboard")); }}>
                {$_("copy_did", {}, "Copy DID")}
              </button>
              <button class="btn {keyfileSavedOnce ? 'btn-primary' : 'btn-outline-secondary'} w-100" on:click={() => { loginMode = 'select'; showOnboardingModal = true; }}>
                {$_("continue_to_setup", {}, "Continue to setup →")}
              </button>
            </div>
          {/if}
        </div>
      </div>
      {/if}
    {/if}
  </div>
</main>

{#if toastMessage}
  <div class="omd-toast-notification">
    <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="var(--accent)" stroke-width="2.5"><polyline points="20 6 9 17 4 12"/></svg>
    <span>{toastMessage}</span>
  </div>
{/if}

{#if !viewerFile}
  {#if p2pReconnectingStatus === 'reconnecting'}
    <div class="p2p-reconnect-toast">
      <div class="p2p-reconnect-spinner"></div>
      <span>{$_("p2p_establishing", {}, "Establishing P2P connection...")}</span>
    </div>
  {:else if isGalleryLoading}
    <div class="p2p-reconnect-toast">
      <div class="p2p-reconnect-spinner"></div>
      <span>{$_("p2p_loading_gallery", {}, "Loading gallery...")}</span>
    </div>
  {:else if isChatLoading && currentApp === 'chat'}
    <div class="p2p-reconnect-toast">
      <div class="p2p-reconnect-spinner"></div>
      <span>{$_("loading_chat", {}, "Loading chat...")}</span>
    </div>
  {:else if isFolderFetching}
    <div class="p2p-reconnect-toast">
      <div class="p2p-reconnect-spinner"></div>
      <span>{$_("p2p_loading_folder", {}, "Loading folder...")}</span>
    </div>
  {:else if p2pReconnectingStatus === 'connected'}
    <div class="p2p-reconnect-toast success">
      <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <polyline points="20 6 9 17 4 12"/>
      </svg>
      <span>{$_("p2p_connected", {}, "Connection established")}</span>
    </div>
  {/if}
{/if}

{#if showPairModal}
  <PairModal
    bind:open={showPairModal}
    linkId={pairModalLinkId}
    deviceName={pairModalDeviceName}
    {identity}
    {isDarkTheme}
    on:paired={(e) => {
      connectDevice(e.detail.linkId, e.detail.drive, e.detail.name);
      if (pairModalOpenChatAfter) {
        pairModalOpenChatAfter = false;
        const pendingView = pairModalOpenViewAfter || "chat";
        pairModalOpenViewAfter = "chat";
        setTimeout(() => openChatsApp(pendingView), 300);
      } else if (pairModalTargetFolder) {
        const target = pairModalTargetFolder;
        pairModalTargetFolder = "";
        setTimeout(() => {
          if (fmApi) fmApi.exec("set-path", { id: target });
        }, 500);
      }
    }}
    on:close={() => {
      showPairModal = false;
      pairModalOpenChatAfter = false;
      pairModalTargetFolder = "";
    }}
  />
{/if}

{#if showQrScannerModal}
  <QrScannerModal
    on:scan={handleQrScan}
    on:close={() => { showQrScannerModal = false; }}
  />
{/if}

<OnboardingModal
  bind:isOpen={showOnboardingModal}
  {identity}
  devices={filesData.filter(f => f.isDevice)}
  {isDarkTheme}
  activeDrive={chatTargetDevice?.drive || publicAiDevice?.drive}
  on:pairDevice={(e) => {
    openPairModal(e.detail.linkId, e.detail.name || "");
  }}
  on:connectDevice={(e) => {
    openPairModal(e.detail.linkId, e.detail.name || "");
  }}
  on:complete={() => {
    showOnboardingModal = false;
  }}
  on:open-chat={() => {
    showOnboardingModal = false;
    openChatsApp();
  }}
  on:open-files={() => {
    showOnboardingModal = false;
    openFilesApp();
  }}
/>

<GatewayTosModal
  bind:open={showGatewayModal}
  initialUrl={pendingGatewayDeepLinkUrl}
  {isDarkTheme}
  on:close={() => { pendingGatewayDeepLinkUrl = ""; }}
/>

<UpdateNotification {isDarkTheme} />

<style>
  .omd-toast-notification {
    position: fixed;
    bottom: 24px;
    right: 24px;
    z-index: 999999;
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 12px 20px;
    background: var(--bg-secondary, #ffffff);
    color: var(--text-primary, #181326);
    border: 1px solid var(--border-color, #DDD6EB);
    border-radius: 10px;
    box-shadow: 0 10px 25px -5px rgba(24, 19, 38, 0.15);
    font-size: 14px;
    font-weight: 500;
    animation: toastSlideIn 0.25s cubic-bezier(0.16, 1, 0.3, 1);
  }

  :global(html.omd-dark) .omd-toast-notification {
    background: var(--bg-elevated, #1D1829);
    color: var(--text-primary, #F8F7FC);
    border: 1px solid var(--accent-border, rgba(192, 132, 252, 0.4));
    box-shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.5);
  }

  @keyframes toastSlideIn {
    from {
      transform: translateY(20px);
      opacity: 0;
    }
    to {
      transform: translateY(0);
      opacity: 1;
    }
  }

  :global(html), :global(body), :global(#app) {
    height: 100%;
    width: 100%;
    margin: 0;
    padding: 0;
    overflow: hidden;
  }

  :global(body) {
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background-color: var(--bg-primary);
    color: var(--text-primary);

    --wx-color-success: #29CC54;
    --wx-color-warning: #FFD966;
    --wx-color-info: var(--accent);
    --wx-color-danger: #F83B42;
    --wx-border-radius: 6px;
    --wx-radius-major: 8px;
    --wx-shadow-light: 0px 4px 20px rgba(24, 19, 38, 0.08);

    /* Custom "terminal" glyph (not present in the wxi icon font): the
       Bootstrap terminal path, used as a CSS mask for menu icons. */
    --omd-terminal-icon: url("data:image/svg+xml;charset=utf-8,%3Csvg%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%20viewBox%3D%220%200%2016%2016%22%3E%3Cpath%20fill%3D%22%23000%22%20d%3D%22M0%203a2%202%200%200%201%202-2h12a2%202%200%200%201%202%202v10a2%202%200%200%201-2%202H2a2%202%200%200%201-2-2zm9.5%205.5h-3a.5.5%200%200%200%200%201h3a.5.5%200%200%200%200-1m-6.354-.354a.5.5%200%201%200%20.708.708l2-2a.5.5%200%200%200%200-.708l-2-2a.5.5%200%201%200-.708.708L4.793%206.5z%22%2F%3E%3C%2Fsvg%3E");
  }

  :global(html.omd-dark body) {
    --wx-shadow-light: 0px 6px 24px rgba(0, 0, 0, 0.42);
  }

  /* ── Global scrollbar styling ────────────────────────────────
     Applies everywhere (Settings, Chat, pickers, filemanager, ...).
     Colors are driven by omd.css per active skin + theme on <html>. */
  :global(*) {
    scrollbar-width: thin;
    scrollbar-color: var(--scrollbar-thumb) var(--scrollbar-track);
  }
  :global(*::-webkit-scrollbar) {
    width: 8px;
    height: 8px;
  }
  :global(*::-webkit-scrollbar-track) {
    background: var(--scrollbar-track);
  }
  :global(*::-webkit-scrollbar-thumb) {
    background: var(--scrollbar-thumb);
    border-radius: 4px;
  }
  :global(*::-webkit-scrollbar-thumb:hover) {
    background: var(--scrollbar-thumb-hover);
  }
  :global(*::-webkit-scrollbar-corner),
  :global(textarea::-webkit-scrollbar-corner) {
    background: transparent !important;
    background-color: transparent !important;
  }
  :global(*::-webkit-resizer),
  :global(textarea::-webkit-resizer) {
    background-color: transparent !important;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='10' viewBox='0 0 10 10' fill='none'%3E%3Cpath d='M9 2L2 9M9 6L6 9' stroke='%238B80A3' stroke-width='1.5' stroke-linecap='round'/%3E%3C/svg%3E") !important;
    background-repeat: no-repeat !important;
    background-position: bottom 3px right 3px !important;
    background-size: 8px 8px !important;
  }

  /* ── Universal Select / Combobox Theme Protection ── */
  :global(.light-theme select),
  :global(html:not(.omd-dark) select) {
    color-scheme: light !important;
    color: var(--text-primary, #181326) !important;
  }
  :global(.light-theme select option),
  :global(.light-theme select optgroup),
  :global(html:not(.omd-dark) select option),
  :global(html:not(.omd-dark) select optgroup) {
    background-color: var(--bg-secondary, #FFFFFF) !important;
    color: var(--text-primary, #181326) !important;
  }

  :global(.dark-theme select),
  :global(html.omd-dark select) {
    color-scheme: dark !important;
    color: var(--text-primary, #F8F7FC) !important;
  }
  :global(.dark-theme select option),
  :global(.dark-theme select optgroup),
  :global(html.omd-dark select option),
  :global(html.omd-dark select optgroup) {
    background-color: var(--bg-secondary, #16131F) !important;
    color: var(--text-primary, #F8F7FC) !important;
  }

  :global(.wx-willow-theme) {
    --top-menu-color: #46508A !important;
    --top-menu-hover-color: #5863A6 !important;
    --wx-color-primary: #3D467A !important;
    --wx-color-primary-border: #3D467A !important;
    --wx-color-primary-hover: #5863A6 !important;
    --wx-color-primary-selected: #E9ECF7 !important;
    --wx-fm-select-background: #E9ECF7 !important;
    --wx-background: #FFFFFF !important;
    --wx-background-alt: #F1F3F9 !important;
    --wx-background-hover: #E9ECF5 !important;
    --wx-fm-background: #F6F7FA !important;
    --bg-secondary: #FFFFFF !important;
    --bg-primary: #F6F7FA !important;
    --border-color: #D8DCE8 !important;
    --selected-text: #343C6B !important;
    --logo-url: url('/img/logo-dark.svg') !important;
  }
  :global(.wx-willow-dark-theme) {
    --top-menu-color: #7B86C4;
    --top-menu-hover-color: #929CD4;
    --wx-color-primary: #5863A6;
    --wx-color-primary-border: #6672B8;
    --wx-color-primary-hover: #6874BA;
    --wx-color-primary-selected: rgba(123, 134, 196, 0.18);
    --wx-fm-select-background: rgba(123, 134, 196, 0.18);
    --wx-background: #1F2430;
    --wx-background-alt: #191D28;
    --wx-background-hover: #282E3D;
    --wx-fm-background: #151821;
    --bg-secondary: #1F2430;
    --bg-primary: #151821;
    --border-color: #32394B;
    --selected-text: #DCE0F5;
    --logo-url: url('/img/logo.svg') !important;
  }

  /* Custom wxi-terminal menu icon: masked with the terminal SVG above */
  :global(i.wxi-terminal:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: var(--omd-terminal-icon) center / contain no-repeat;
    mask: var(--omd-terminal-icon) center / contain no-repeat;
  }

  /* Chat icon (public AI node, Чаты sidebar link, chats entries) */
  :global(i.wxi-chat:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/chat-fill.svg") center / contain no-repeat;
    mask: url("/icons/chat-fill.svg") center / contain no-repeat;
  }

  /* Custom device, desktop, and shared folder icons matching legacy main.css */
  :global(i.wxi-embedded:before),
  :global(i.wxi-device:before),
  :global(i.wxi-devices:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/hdd-fill.svg") center / contain no-repeat;
    mask: url("/icons/hdd-fill.svg") center / contain no-repeat;
  }

  :global(i.wxi-desktop:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/laptop.svg") center / contain no-repeat;
    mask: url("/icons/laptop.svg") center / contain no-repeat;
  }

  :global(i.wxi-phone:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/phone.svg") center / contain no-repeat;
    mask: url("/icons/phone.svg") center / contain no-repeat;
  }

  :global(i.wxi-tablet:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/tablet.svg") center / contain no-repeat;
    mask: url("/icons/tablet.svg") center / contain no-repeat;
  }

  :global(i.wxi-folder-shared:before),
  :global(i.wxi-shared:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/img/folder-shared.svg") center / contain no-repeat;
    mask: url("/img/folder-shared.svg") center / contain no-repeat;
  }

  :global(i.wxi-folder-links:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/folder-symlink-fill.svg") center / contain no-repeat;
    mask: url("/icons/folder-symlink-fill.svg") center / contain no-repeat;
  }

  :global(i.wxi-folder-sites:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/img/folder-sites.svg") center / contain no-repeat;
    mask: url("/img/folder-sites.svg") center / contain no-repeat;
  }

  :global(i.wxi-folder:before),
  :global(i.wxi-folder-open:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/folder-fill.svg") center / contain no-repeat;
    mask: url("/icons/folder-fill.svg") center / contain no-repeat;
  }

  :global(i.wxi-share:before),
  :global(.wxi-share:before),
  :global(.wx-menu .wxi-share:before) {
    content: "" !important;
    display: inline-block;
    width: 1em;
    height: 1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/share.svg") center / contain no-repeat;
    mask: url("/icons/share.svg") center / contain no-repeat;
  }

  :global(i.wxi-file-plus:before),
  :global(.wxi-file-plus:before),
  :global(.wx-menu .wxi-file-plus:before) {
    content: "" !important;
    display: inline-block;
    width: 1.1em;
    height: 1.1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/file-plus.svg") center / contain no-repeat;
    mask: url("/icons/file-plus.svg") center / contain no-repeat;
  }

  :global(i.wxi-folder-plus:before),
  :global(.wxi-folder-plus:before),
  :global(.wx-menu .wxi-folder-plus:before) {
    content: "" !important;
    display: inline-block;
    width: 1.1em;
    height: 1.1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/folder-plus.svg") center / contain no-repeat;
    mask: url("/icons/folder-plus.svg") center / contain no-repeat;
  }

  :global(i.wxi-upload:before),
  :global(.wxi-upload:before),
  :global(.wx-menu .wxi-upload:before),
  :global(i.wxi-file-upload:before),
  :global(.wxi-file-upload:before),
  :global(.wx-menu .wxi-file-upload:before) {
    content: "" !important;
    display: inline-block;
    width: 1.1em;
    height: 1.1em;
    vertical-align: middle;
    background-color: currentColor;
    -webkit-mask: url("/icons/upload.svg") center / contain no-repeat;
    mask: url("/icons/upload.svg") center / contain no-repeat;
  }

  /* Global SVAR Component Styling to Match Figma Design */
  :global(.wx-filemanager) {
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif !important;
  }
  :global(.wx-content-wrapper) {
    margin-top: 0 !important;
  }
  :global(.wx-content-item-fit),
  :global(.wx-content-item) {
    padding: 0 10px 10px !important;
  }
  :global(.wx-sidebar .wx-button) {
    padding: 10px 8px 0 !important;
    box-sizing: border-box !important;
  }
  :global(.wx-sidebar .wx-button.wx-primary),
  :global(.wx-button.wx-primary) {
    background-color: var(--wx-color-primary, #3D467A) !important;
    border: 1px solid var(--wx-color-primary-border, var(--wx-color-primary, #3D467A)) !important;
    color: #ffffff !important;
    border-radius: 6px !important;
    font-family: 'Inter', sans-serif !important;
    font-weight: 600 !important;
    min-height: 36px !important;
    height: auto !important;
    padding: 6px 12px !important;
    line-height: 1.25 !important;
    text-align: center !important;
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
    white-space: normal !important;
    word-break: normal !important;
    overflow-wrap: break-word !important;
    width: 100% !important;
    box-sizing: border-box !important;
    transition: background-color 0.15s ease, border-color 0.15s ease, box-shadow 0.15s ease !important;
  }
  :global(.wx-sidebar .wx-button.wx-primary:hover),
  :global(.wx-button.wx-primary:hover) {
    background-color: var(--wx-color-primary-hover, #5863A6) !important;
    border-color: var(--wx-color-primary-hover, #5863A6) !important;
    color: #ffffff !important;
    box-shadow: 0 2px 8px var(--accent-glow, rgba(61, 70, 122, 0.22)) !important;
  }
  :global(.wx-willow-dark-theme .wx-sidebar .wx-button.wx-primary:hover),
  :global(.wx-willow-dark-theme .wx-button.wx-primary:hover) {
    box-shadow: 0 2px 8px rgba(88, 99, 166, 0.28) !important;
  }
  :global(.wx-button.wx-secondary) {
    border: 1px solid var(--border-color, var(--wx-border)) !important;
    border-radius: 6px !important;
    background-color: var(--wx-background-alt) !important;
    color: var(--wx-color-font) !important;
    font-family: 'Inter', sans-serif !important;
    height: 36px !important;
  }
  :global(.wx-button.wx-secondary:hover) {
    background-color: var(--wx-background-hover) !important;
    border-color: var(--wx-color-primary, #5863A6) !important;
    color: var(--wx-color-primary, #5863A6) !important;
  }

  /* 2. Filemanager Toolbar, Search & Controls */
  :global(.wx-toolbar) {
    justify-content: space-between !important;
    border-bottom: none !important;
    box-shadow: none !important;
    background-color: var(--bg-secondary) !important;
    height: calc(56px + env(safe-area-inset-top, 0px)) !important;
    padding-top: env(safe-area-inset-top, 0px) !important;
    padding-bottom: 0 !important;
    padding-left: calc(16px + env(safe-area-inset-left, 0px)) !important;
    padding-right: calc(16px + env(safe-area-inset-right, 0px)) !important;
    gap: 16px !important;
    box-sizing: border-box !important;
  }
  :global(.wx-toolbar .wx-left) {
    flex: 1;
    display: flex !important;
    align-items: center !important;
    justify-content: flex-start !important;
    gap: 16px !important;
  }
  :global(.wx-toolbar .wx-left .wx-name) {
    margin-right: auto !important;
    margin-left: 34px !important; /* Center with sidebar */
    font-size: 0 !important;
    width: 152px;
    height: 34px;
    display: flex;
    align-items: center;
    background-image: var(--logo-url);
    background-size: contain;
    background-repeat: no-repeat;
    background-position: left center;
    transform: translateY(1px);
  }

  /* Search Widget */
  :global(.wx-search-input) {
    position: relative !important;
    width: 260px !important;
    max-width: 320px !important;
    height: 36px !important;
    display: flex !important;
    align-items: center !important;
    border: none !important;
    padding: 0 !important;
    background: transparent !important;
    box-shadow: none !important;
  }
  :global(.wx-toolbar.wx-search-active .wx-search-input) {
    width: 440px !important;
    max-width: min(600px, 60vw) !important;
    transition: width 0.2s ease, max-width 0.2s ease !important;
  }
  :global(.wx-search-input .wx-text) {
    width: 100% !important;
    height: 36px !important;
    box-sizing: border-box !important;
    border: 1px solid var(--border-color, var(--wx-border)) !important;
    border-radius: 6px !important;
    background-color: var(--wx-background-alt) !important;
    padding: 0 34px 0 12px !important;
    font-family: 'Inter', sans-serif !important;
    font-size: 14px !important;
    color: var(--wx-color-font) !important;
    outline: none !important;
    transition: border-color 0.15s ease, box-shadow 0.15s ease, background-color 0.15s ease !important;
  }
  :global(.wx-search-input .wx-text:focus) {
    background-color: var(--bg-secondary) !important;
    border-color: var(--wx-color-primary, #9333EA) !important;
    box-shadow: 0 0 0 2px var(--accent-soft, rgba(168, 85, 247, 0.22)) !important;
  }
  :global(.wx-search-input .wx-icon),
  :global(.wx-search-input .wx-icon:before) {
    position: absolute !important;
    right: 8px !important;
    top: 50% !important;
    transform: translateY(-50%) !important;
    width: 20px !important;
    height: 20px !important;
    font-size: 16px !important;
    color: var(--wx-color-font-alt) !important;
    cursor: pointer !important;
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
    border: none !important;
    background: transparent !important;
    padding: 0 !important;
    margin: 0 !important;
    transition: color 0.15s ease !important;
  }
  :global(.wx-search-input .wx-icon:hover),
  :global(.wx-search-input .wx-icon:hover:before) {
    color: var(--wx-color-primary, #9333EA) !important;
  }
  @media (max-width: 600px) {
    :global(.wx-search-input) {
      width: 100% !important;
      max-width: 100% !important;
    }
  }

  /* Right Toolbar Actions */
  :global(.wx-toolbar .wx-right) {
    display: flex !important;
    align-items: center !important;
    gap: 10px !important;
  }
  :global(.wx-preview-icon) {
    margin: 0 !important;
  }
  :global(.wx-preview-icon button) {
    width: 36px !important;
    height: 36px !important;
    border-radius: 6px !important;
    border: none !important;
    background-color: var(--wx-background-alt) !important;
    color: var(--wx-color-font-alt) !important;
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
    padding: 0 !important;
    cursor: pointer !important;
    outline: none !important;
    box-shadow: none !important;
    transition: all 0.15s ease !important;
    box-sizing: border-box !important;
  }
  :global(.wx-preview-icon button:focus),
  :global(.wx-preview-icon button:focus-visible),
  :global(.wx-preview-icon button:active) {
    border: none !important;
    outline: none !important;
    box-shadow: none !important;
  }
  :global(.wx-preview-icon button i),
  :global(.wx-preview-icon button i:before) {
    font-size: 18px !important;
    line-height: 1 !important;
    height: 18px !important;
    color: var(--wx-color-font-alt) !important;
    transition: color 0.15s ease !important;
  }
  :global(.wx-preview-icon button:hover) {
    background-color: var(--wx-background-hover) !important;
    color: var(--wx-color-primary, #7E22CE) !important;
    border: none !important;
    outline: none !important;
  }
  :global(.wx-preview-icon button:hover i),
  :global(.wx-preview-icon button:hover i:before) {
    color: var(--wx-color-primary, #7E22CE) !important;
  }
  :global(.wx-preview-icon button.wx-selected),
  :global(.wx-preview-icon button.wx-pressed),
  :global(.wx-preview-icon button[aria-pressed="true"]) {
    background-color: var(--wx-color-primary, #7E22CE) !important;
    border: none !important;
    outline: none !important;
    color: #ffffff !important;
    box-shadow: none !important;
  }
  :global(.wx-preview-icon button.wx-selected i),
  :global(.wx-preview-icon button.wx-selected i:before),
  :global(.wx-preview-icon button.wx-pressed i),
  :global(.wx-preview-icon button.wx-pressed i:before),
  :global(.wx-preview-icon button[aria-pressed="true"] i),
  :global(.wx-preview-icon button[aria-pressed="true"] i:before) {
    color: #ffffff !important;
  }
  :global(.wx-preview-icon button.wx-selected:hover),
  :global(.wx-preview-icon button.wx-pressed:hover),
  :global(.wx-preview-icon button[aria-pressed="true"]:hover) {
    background-color: var(--wx-color-primary-hover, #9333EA) !important;
    border: none !important;
    outline: none !important;
    color: #ffffff !important;
  }
  :global(.wx-preview-icon button.wx-selected:hover i),
  :global(.wx-preview-icon button.wx-selected:hover i:before),
  :global(.wx-preview-icon button.wx-pressed:hover i),
  :global(.wx-preview-icon button.wx-pressed:hover i:before),
  :global(.wx-preview-icon button[aria-pressed="true"]:hover i),
  :global(.wx-preview-icon button[aria-pressed="true"]:hover i:before) {
    color: #ffffff !important;
  }

  /* View Mode Segmented Controls */
  :global(.wx-modes) {
    display: flex !important;
    align-items: center !important;
  }
  :global(.wx-modes .wx-segmented) {
    display: inline-flex !important;
    background-color: var(--wx-background-alt) !important;
    border: 1px solid var(--border-color, var(--wx-border)) !important;
    border-radius: 6px !important;
    padding: 2px !important;
    gap: 2px !important;
    height: 36px !important;
    box-sizing: border-box !important;
  }
  :global(.wx-modes .wx-segmented button) {
    width: 32px !important;
    height: 30px !important;
    min-width: 32px !important;
    padding: 0 !important;
    margin: 0 !important;
    border-radius: 4px !important;
    border: none !important;
    outline: none !important;
    background: transparent !important;
    color: var(--wx-color-font-alt) !important;
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
    cursor: pointer !important;
    transition: all 0.15s ease !important;
    box-sizing: border-box !important;
  }
  :global(.wx-modes .wx-segmented button:focus),
  :global(.wx-modes .wx-segmented button:focus-visible),
  :global(.wx-modes .wx-segmented button:active) {
    outline: none !important;
    box-shadow: none !important;
    border: none !important;
  }
  :global(.wx-modes .wx-segmented button .wx-icon),
  :global(.wx-modes .wx-segmented button .wx-icon:before) {
    color: var(--wx-color-font-alt) !important;
    transition: color 0.15s ease !important;
  }
  :global(.wx-modes .wx-segmented button:hover) {
    background-color: var(--wx-background-hover) !important;
  }
  :global(.wx-modes .wx-segmented button:hover .wx-icon),
  :global(.wx-modes .wx-segmented button:hover .wx-icon:before) {
    color: var(--wx-color-primary, #7E22CE) !important;
  }
  :global(.wx-modes .wx-segmented button.wx-selected) {
    background-color: var(--wx-color-primary, #7E22CE) !important;
    color: #ffffff !important;
    box-shadow: none !important;
    border: none !important;
    outline: none !important;
  }
  :global(.wx-modes .wx-segmented button.wx-selected .wx-icon),
  :global(.wx-modes .wx-segmented button.wx-selected .wx-icon:before) {
    color: #ffffff !important;
  }
  :global(.wx-modes .wx-segmented button.wx-selected:hover) {
    background-color: var(--wx-color-primary-hover, #9333EA) !important;
    color: #ffffff !important;
  }
  :global(.wx-modes .wx-segmented button.wx-selected:hover .wx-icon),
  :global(.wx-modes .wx-segmented button.wx-selected:hover .wx-icon:before) {
    color: #ffffff !important;
  }
  :global(.wx-modes .wx-segmented button .wx-icon) {
    font-size: 16px !important;
    line-height: 1 !important;
    height: 16px !important;
    color: inherit !important;
    opacity: 1 !important;
  }

  /* 3. Left Sidebar / Folder Tree */
  :global(.wx-sidebar) {
    padding: 0 !important;
    border-right: none !important;
    position: relative !important;
    z-index: 5 !important;
    box-shadow: var(--sidebar-shadow, 3px 0 14px rgba(24, 19, 38, 0.05)) !important;
    background-color: var(--bg-secondary) !important;
  }
  :global(.wx-sidebar .wx-wrapper) {
    border-radius: 0 !important;
    box-shadow: none !important;
    border-right: none !important;
  }
  :global(.wx-sidebar .wx-wrapper),
  :global(.wx-tree),
  :global(.wx-drive) {
    background-color: var(--bg-secondary) !important;
    box-shadow: none !important;
  }
  :global(.wx-tree) {
    padding: 4px 0 !important;
    overflow-y: auto !important;
    overflow-x: hidden !important;
  }
  :global(.wx-tree ul) {
    height: auto !important;
    min-height: 0 !important;
    max-height: none !important;
    background-color: transparent !important;
  }
  :global(.wx-folder) {
    background-color: transparent !important;
    border-radius: 6px !important;
    margin: 2px 8px !important;
    padding-top: 6px !important;
    padding-bottom: 6px !important;
    padding-right: 8px !important;
    font-family: 'Inter', sans-serif !important;
    font-size: 14px !important;
    font-weight: 500 !important;
    color: var(--wx-color-font) !important;
    transition: background-color 0.12s ease !important;
    width: calc(100% - 16px) !important;
    box-sizing: border-box !important;
  }
  :global(.wx-folder:hover) {
    background-color: var(--wx-background-hover) !important;
  }
  :global(.wx-folder:hover i) {
    color: var(--top-menu-hover-color, #5863A6) !important;
    opacity: 1 !important;
  }
  :global(.wx-folder.wx-selected) {
    background-color: var(--wx-fm-select-background, rgba(123, 134, 196, 0.18)) !important;
    color: var(--selected-text, var(--top-menu-color, #7B86C4)) !important;
    font-weight: 600 !important;
  }
  :global(.wx-folder i),
  :global(.wx-tree i) {
    color: var(--top-menu-color, #46508A) !important;
    opacity: 0.85 !important;
    transition: opacity 0.2s ease, color 0.2s ease !important;
  }
  :global(.wx-folder.wx-selected i),
  :global(.wx-tree .wx-selected i) {
    color: var(--top-menu-color, #46508A) !important;
    opacity: 1 !important;
  }
  :global(.wx-folder .wx-name) {
    font-family: 'Inter', sans-serif !important;
    font-size: 14px !important;
    font-weight: 500 !important;
    cursor: pointer !important;
  }

  /* 4. Breadcrumbs & Back link */
  :global(.wx-crumbs),
  :global(.wx-breadcrumbs),
  :global(.wx-search-header) {
    font-family: 'Inter', sans-serif !important;
    font-size: 14px !important;
    font-weight: 500 !important;
    background-color: transparent !important;
    border: none !important;
  }
  :global(.wx-crumbs .wx-item),
  :global(.wx-breadcrumbs .wx-item) {
    color: var(--wx-color-font-alt) !important;
    transition: color 0.15s ease !important;
  }
  :global(.wx-crumbs .wx-item:hover),
  :global(.wx-breadcrumbs .wx-item:hover) {
    color: var(--top-menu-hover-color, #C084FC) !important;
  }
  :global(.wx-crumbs .wx-item:last-child),
  :global(.wx-breadcrumbs .wx-item:last-child) {
    color: var(--wx-color-font) !important;
    font-weight: 600 !important;
  }
  :global(.wx-back),
  :global(.wx-back-item .wx-back) {
    color: var(--top-menu-color, #A855F7) !important;
    font-family: 'Inter', sans-serif !important;
    font-size: 13px !important;
    font-weight: 500 !important;
    cursor: pointer !important;
    transition: color 0.15s ease !important;
  }
  :global(.wx-back:hover),
  :global(.wx-back-item .wx-back:hover) {
    color: var(--top-menu-hover-color, #C084FC) !important;
  }

  /* Uniform styling for '+' (Add New) breadcrumb button to match refresh & home icons */
  :global(.wx-breadcrumb-add-btn) {
    width: 28px !important;
    height: 28px !important;
    border-radius: 4px !important;
    background-color: transparent !important;
    border: none !important;
    color: var(--wx-color-font-alt, #8B80A3) !important;
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
    padding: 0 !important;
    cursor: pointer !important;
    transform: none !important;
    transition: color 0.15s ease, background-color 0.15s ease !important;
    box-shadow: none !important;
  }
  :global(.wx-breadcrumb-add-btn:hover) {
    color: var(--top-menu-hover-color, #C084FC) !important;
    background-color: var(--wx-background-alt, rgba(168, 85, 247, 0.10)) !important;
    border: none !important;
    transform: none !important;
  }

  /* Hide 'Back to parent folder' icon button on desktop view, show only on compact mobile (<768px) */
  @media (min-width: 769px) {
    :global(.wx-crumbs .wx-back-item),
    :global(.wx-breadcrumbs .wx-back-item),
    :global(.wx-crumbs .wx-back),
    :global(.wx-breadcrumbs .wx-back),
    :global(.wx-crumbs .wx-item.wx-back),
    :global(.wx-breadcrumbs .wx-item.wx-back),
    :global(.wx-crumbs .wxi-arrow-left),
    :global(.wx-breadcrumbs .wxi-arrow-left),
    :global(.wx-back-icon) {
      display: none !important;
    }
  }

  /* 5. Cards View (Grid of Folders/Files) */
  :global(.wx-cards) {
    background-color: var(--wx-fm-background) !important;
  }
  @media (min-width: 601px) {
    :global(.wx-cards .wx-card),
    :global(.wx-cards .wx-item) {
      width: 210px !important;
      height: 200px !important;
      box-sizing: border-box !important;
      display: flex !important;
      flex-direction: column !important;
      border: 1px solid var(--border-color, var(--wx-border)) !important;
      border-radius: 12px !important;
      background-color: var(--wx-background) !important;
      padding: 0 !important;
      overflow: hidden !important;
      cursor: pointer !important;
      box-shadow: 0 2px 8px rgba(0, 0, 0, 0.15) !important;
      transition: transform 0.25s ease, box-shadow 0.25s ease, border-color 0.25s ease !important;
    }
    :global(.wx-cards .wx-card:hover),
    :global(.wx-cards .wx-item:hover) {
      transform: translateY(-2px) !important;
      border-color: var(--border-color, var(--wx-border)) !important;
      box-shadow: 0 6px 18px rgba(0, 0, 0, 0.12) !important;
    }
    :global(.wx-cards .wx-card:hover .wx-name),
    :global(.wx-cards .wx-item:hover .wx-name) {
      color: var(--top-menu-hover-color, #9333EA) !important;
    }
    :global(.wx-cards .wx-card.wx-selected),
    :global(.wx-cards .wx-item.wx-selected) {
      border: 1px solid var(--accent-border, var(--border-color)) !important;
      outline: none !important;
      box-shadow: 0 4px 14px rgba(0, 0, 0, 0.12) !important;
      background-color: var(--wx-fm-select-background, #F3E8FF) !important;
    }
    :global(.wx-cards .wx-card.wx-selected .wx-info),
    :global(.wx-cards .wx-item.wx-selected .wx-info) {
      background-color: var(--wx-fm-select-background, #F3E8FF) !important;
    }
    :global(.wx-cards .wx-card:focus),
    :global(.wx-cards .wx-card:focus-visible),
    :global(.wx-cards .wx-item:focus),
    :global(.wx-cards .wx-item:focus-visible) {
      outline: none !important;
      border: 1px solid var(--border-color, var(--wx-border)) !important;
    }
    :global(.wx-cards .wx-preview) {
      background-color: var(--wx-background-alt) !important;
      border-bottom: 1px solid var(--border-color, var(--wx-border)) !important;
      height: 144px !important;
      display: flex !important;
      align-items: center !important;
      justify-content: center !important;
      border-radius: 11px 11px 0 0 !important;
      overflow: hidden !important;
      box-sizing: border-box !important;
      flex-shrink: 0 !important;
    }
    :global(.wx-cards .wx-info) {
      background-color: var(--wx-background) !important;
      padding: 6px 8px 6px 12px !important;
      height: auto !important;
      min-height: 54px !important;
      flex-grow: 1 !important;
      box-sizing: border-box !important;
      border-radius: 0 0 11px 11px !important;
    }
    :global(.wx-cards .wx-more-mobile) {
      display: none !important;
    }
    :global(.wx-cards .wx-more-desktop) {
      width: 28px !important;
      height: 28px !important;
      min-width: 28px !important;
      min-height: 28px !important;
      max-width: 28px !important;
      max-height: 28px !important;
      aspect-ratio: 1 / 1 !important;
      padding: 0 !important;
      border-radius: 50% !important;
      margin-left: auto !important;
      flex-shrink: 0 !important;
      display: flex !important;
      align-items: center !important;
      justify-content: center !important;
      cursor: pointer !important;
      transition: background-color 0.15s ease, color 0.15s ease !important;
    }
    :global(.wx-cards .wx-more-desktop:hover) {
      background-color: var(--wx-button-background, rgba(168, 85, 247, 0.14)) !important;
      color: var(--top-menu-hover-color, #C084FC) !important;
      border-radius: 50% !important;
    }
    :global(.wx-cards .wx-more-desktop i) {
      font-size: 18px !important;
      line-height: 1 !important;
      display: flex !important;
      align-items: center !important;
      justify-content: center !important;
      width: 100% !important;
      height: 100% !important;
      margin: 0 !important;
      padding: 0 !important;
    }
    :global(.wx-cards .wx-preview i) {
      font-size: 80px !important;
      line-height: 1 !important;
      opacity: 0.84 !important;
      color: var(--top-menu-color, #7B86C4) !important;
      transition: opacity 0.25s ease, transform 0.25s ease, color 0.25s ease !important;
    }
    :global(.wx-cards .wx-preview i:before) {
      line-height: 1 !important;
    }
    :global(.wx-cards .wx-file-icon .wx-card-preview),
    :global(.wx-cards .wx-card-placeholder-icon),
    :global(.wx-cards .wx-file-preview .wx-card-placeholder-icon) {
      width: 80px !important;
      height: 80px !important;
      max-width: 80px !important;
      max-height: 80px !important;
      object-fit: contain !important;
      opacity: 0.84 !important;
      transition: opacity 0.25s ease, transform 0.25s ease !important;
    }
    :global(.wx-cards .wx-file-preview),
    :global(.wx-cards .wx-preview-wrapper) {
      width: 100% !important;
      height: 144px !important;
      flex-shrink: 0 !important;
      box-sizing: border-box !important;
    }
    :global(.wx-cards .wx-file-preview img.wx-card-preview),
    :global(.wx-cards .wx-file-preview .wx-card-preview) {
      width: 100% !important;
      height: 100% !important;
      max-width: 100% !important;
      max-height: 100% !important;
      object-fit: cover !important;
      border-radius: 11px 11px 0 0 !important;
      opacity: 1 !important;
    }
    :global(.wx-cards .wx-card:hover .wx-preview i),
    :global(.wx-cards .wx-item:hover .wx-preview i),
    :global(.wx-cards .wx-card:hover .wx-file-icon .wx-card-preview),
    :global(.wx-cards .wx-item:hover .wx-file-icon .wx-card-preview),
    :global(.wx-cards .wx-card:hover .wx-card-placeholder-icon),
    :global(.wx-cards .wx-item:hover .wx-card-placeholder-icon) {
      opacity: 1 !important;
      transform: scale(1.06) !important;
    }
    :global(.wx-cards .wx-card .wxi-folder),
    :global(.wx-cards .wx-item .wxi-folder) {
      color: var(--top-menu-color, #A855F7) !important;
      cursor: pointer !important;
    }
    :global(.wx-cards .wx-card:hover .wxi-folder),
    :global(.wx-cards .wx-item:hover .wxi-folder) {
      color: var(--top-menu-hover-color, #C084FC) !important;
      opacity: 1 !important;
    }
    :global(.wx-cards .wx-card .wx-name),
    :global(.wx-cards .wx-item .wx-name) {
      font-family: 'Inter', sans-serif !important;
      font-size: 14px !important;
      font-weight: 600 !important;
      color: var(--wx-color-font) !important;
      cursor: pointer !important;
    }
    :global(.wx-cards .wx-card .wx-type),
    :global(.wx-cards .wx-item .wx-type) {
      font-family: 'Inter', sans-serif !important;
      font-size: 11px !important;
      font-weight: 500 !important;
      color: var(--wx-color-font-alt) !important;
      text-transform: uppercase !important;
      letter-spacing: 0.4px !important;
    }
  }

  @media (max-width: 600px) {
    :global(.wx-cards .wx-card),
    :global(.wx-cards .wx-item) {
      border: none !important;
      box-shadow: none !important;
      background: transparent !important;
      border-radius: 8px !important;
    }
    :global(.wx-cards .wx-preview) {
      border: none !important;
      background: transparent !important;
      border-radius: 0 !important;
    }
    :global(.wx-cards .wx-info) {
      border: none !important;
      background: transparent !important;
    }
    :global(.wx-cards .wx-more-desktop) {
      display: none !important;
    }
    :global(.wx-cards .wx-more-mobile) {
      display: flex !important;
    }
  }

  /* Preview & Details Sidebar Panel */
  :global(.wx-icon-wrapper),
  :global(.wx-preview .wx-icon-wrapper),
  :global(.wx-sidebar .wx-preview .wx-icon-wrapper) {
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
    padding: 24px !important;
    overflow: hidden !important;
    box-sizing: border-box !important;
  }
  :global(.wx-icon-wrapper img),
  :global(.wx-preview .wx-icon-wrapper img),
  :global(.wx-sidebar .wx-preview .wx-icon-wrapper img),
  :global(.wx-content-item .wx-preview .wx-icon-wrapper img) {
    max-width: 96px !important;
    max-height: 96px !important;
    width: 96px !important;
    height: 96px !important;
    object-fit: contain !important;
    margin: 0 auto !important;
    display: block !important;
  }
  :global(.wx-icon-wrapper i),
  :global(.wx-preview .wx-icon-wrapper i),
  :global(.wx-sidebar .wx-preview .wx-icon-wrapper i) {
    font-size: 80px !important;
    width: 80px !important;
    height: 80px !important;
    line-height: 80px !important;
    color: var(--top-menu-color, #7E22CE) !important;
    margin: 0 auto !important;
  }
  :global(.wx-preview .wx-img-wrapper) {
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
    overflow: hidden !important;
    background-color: var(--wx-background-alt) !important;
  }
  :global(.wx-preview .wx-img-wrapper img) {
    width: 100% !important;
    height: 100% !important;
    max-width: 100% !important;
    max-height: 100% !important;
    object-fit: cover !important;
    border-radius: 6px 6px 0 0 !important;
  }
  :global(.wx-cards .wx-preview.wx-file-preview img.wx-card-preview),
  :global(.wx-cards .wx-file-preview .wx-card-preview) {
    width: 100% !important;
    height: 100% !important;
    max-width: 100% !important;
    max-height: 100% !important;
    object-fit: cover !important;
    border-radius: 9px 9px 0 0 !important;
    opacity: 1 !important;
  }

  /* 6. Table View */
  :global(.wx-grid) {
    font-family: 'Inter', sans-serif !important;
    background-color: var(--wx-background) !important;
  }
  :global(.wx-header-wrapper) {
    background-color: var(--wx-table-header-background, #F8F7FC) !important;
    border-bottom: 1px solid var(--wx-border) !important;
  }
  :global(.wx-header .wx-cell) {
    background-color: var(--wx-table-header-background, #F8F7FC) !important;
    font-family: 'Inter', sans-serif !important;
    font-size: 13px !important;
    font-weight: 600 !important;
    color: var(--wx-color-font-alt) !important;
  }
  :global(.wx-row) {
    border-bottom: 1px solid var(--wx-border) !important;
    background-color: var(--wx-background) !important;
    cursor: pointer !important;
    transition: background-color 0.12s ease !important;
  }
  :global(.wx-row:hover),
  :global(.wx-row:hover .wx-cell) {
    background-color: var(--wx-background-hover) !important;
  }
  :global(.wx-row:hover .wx-name) {
    color: var(--top-menu-color, #7E22CE) !important;
  }
  :global(.wx-row .wx-name-cell i),
  :global(.wx-row .wx-name-cell img) {
    opacity: 0.82 !important;
    color: var(--top-menu-color, #7E22CE) !important;
    transition: opacity 0.15s ease, color 0.15s ease !important;
  }
  :global(.wx-row:hover .wx-name-cell i),
  :global(.wx-row:hover .wx-name-cell img),
  :global(.wx-row.wx-selected .wx-name-cell i),
  :global(.wx-row.wx-selected .wx-name-cell img) {
    opacity: 1 !important;
    color: var(--top-menu-color, #7E22CE) !important;
  }
  :global(.wx-row.wx-selected),
  :global(.wx-row.wx-selected .wx-cell),
  :global(.wx-table-box.wx-active .wx-row.wx-selected),
  :global(.wx-table-box.wx-active .wx-row.wx-selected .wx-cell) {
    background-color: var(--wx-fm-select-background) !important;
    color: var(--wx-color-font) !important;
  }
  :global(.wx-row.wx-selected .wx-name),
  :global(.wx-table-box.wx-active .wx-row.wx-selected .wx-name) {
    color: var(--wx-color-primary) !important;
    font-weight: 600 !important;
  }
  :global(.wx-name-cell),
  :global(.wx-name-cell .wx-name),
  :global(.wx-name-cell .wx-icon),
  :global(.wx-name-cell i),
  :global(.wx-name-cell img) {
    cursor: pointer !important;
  }
  :global(.wx-row .wxi-folder) {
    color: var(--wx-color-primary) !important;
  }
  :global(.wx-row .wx-name) {
    font-family: 'Inter', sans-serif !important;
    font-size: 14px !important;
    color: var(--wx-color-font) !important;
    transition: color 0.12s ease !important;
  }

  /* 7. Context Menus & Action Menus */
  :global(.wx-menu) {
    background-color: var(--wx-background) !important;
    border: 1px solid var(--wx-border) !important;
    border-radius: 8px !important;
    box-shadow: var(--wx-shadow-light) !important;
    padding: 6px 0 !important;
    font-family: 'Inter', sans-serif !important;
  }
  :global(.wx-menu .wx-option) {
    margin: 2px 6px !important;
    padding: 0 12px !important;
    border-radius: 6px !important;
    font-family: 'Inter', sans-serif !important;
    font-size: 13px !important;
    font-weight: 500 !important;
    color: var(--wx-color-font) !important;
    height: 32px !important;
    line-height: 32px !important;
    transition: background-color 0.12s ease, color 0.12s ease !important;
  }
  :global(.wx-menu .wx-option:hover) {
    background-color: var(--wx-fm-select-background) !important;
    color: var(--wx-color-primary) !important;
  }
  :global(.wx-menu .wx-option .wx-icon),
  :global(.wx-menu .wx-option .wx-sub-icon) {
    color: var(--wx-color-primary) !important;
    font-size: 16px !important;
    width: 18px !important;
    height: 18px !important;
    margin-right: 8px !important;
    display: inline-flex !important;
    align-items: center !important;
    justify-content: center !important;
    flex-shrink: 0 !important;
  }
  :global(.wx-menu .wx-separator) {
    border-top: 1px solid var(--wx-border) !important;
    margin: 4px 0 !important;
  }

  /* LightGallery Overlay Toolbar floating with high z-index over full-height image */
  :global(.lg-toolbar) {
    position: absolute !important;
    top: 0 !important;
    left: 0 !important;
    right: 0 !important;
    background: linear-gradient(180deg, rgba(0, 0, 0, 0.42) 0%, rgba(0, 0, 0, 0) 100%) !important;
    padding-top: calc(8px + env(safe-area-inset-top, 0px)) !important;
    padding-bottom: 8px !important;
    padding-left: calc(16px + env(safe-area-inset-left, 0px)) !important;
    padding-right: calc(16px + env(safe-area-inset-right, 0px)) !important;
    z-index: 2100 !important;
    pointer-events: auto !important;
    display: flex !important;
    align-items: center !important;
    justify-content: flex-end !important;
    gap: 4px !important;
    box-sizing: border-box !important;
  }
  :global(.lg-components),
  :global(.lg-sub-html),
  :global(.lg-thumb-outer) {
    padding-bottom: env(safe-area-inset-bottom, 0px) !important;
    box-sizing: border-box !important;
  }
  :global(.lg-toolbar .lg-icon),
  :global(.lg-toolbar .omd-gallery-fs-btn) {
    color: #F8F7FC !important;
    font-size: 20px !important;
    cursor: pointer !important;
    transition: all 0.2s ease !important;
    filter: drop-shadow(0 1px 3px rgba(0, 0, 0, 0.6));
    float: none !important;
    display: inline-flex !important;
    align-items: center !important;
    justify-content: center !important;
    width: 38px !important;
    height: 38px !important;
    min-width: 38px !important;
    min-height: 38px !important;
    border-radius: 50% !important;
    background: rgba(255, 255, 255, 0.14) !important;
    border: 1px solid rgba(255, 255, 255, 0.15) !important;
    padding: 0 !important;
    margin: 0 3px !important;
    box-sizing: border-box !important;
    backdrop-filter: blur(8px) !important;
  }
  @media (hover: hover) and (pointer: fine) {
    :global(.lg-toolbar .lg-icon:hover),
    :global(.lg-toolbar .omd-gallery-fs-btn:hover) {
      background: var(--accent, rgba(88, 99, 166, 0.85)) !important;
      border-color: var(--accent-hover, #929CD4) !important;
      color: #ffffff !important;
      transform: scale(1.05) !important;
    }
  }
  :global(.lg-toolbar .lg-icon:active),
  :global(.lg-toolbar .omd-gallery-fs-btn:active) {
    background: var(--accent-active, rgba(61, 70, 122, 0.92)) !important;
    transform: scale(0.95) !important;
  }
  :global(.lg-toolbar .omd-gallery-fs-btn svg),
  :global(.lg-toolbar .lg-fullscreen svg) {
    display: block !important;
    margin: auto !important;
    pointer-events: none !important;
    flex-shrink: 0 !important;
    fill: #ffffff !important;
    color: #ffffff !important;
  }
  :global(.lg-toolbar .omd-gallery-fs-btn svg path),
  :global(.lg-toolbar .lg-fullscreen svg path) {
    fill: #ffffff !important;
  }
  :global(.lg-toolbar .omd-gallery-fs-btn:after),
  :global(.lg-toolbar .lg-fullscreen:after) {
    display: none !important;
    content: "" !important;
  }
  :global(.lg-toolbar .omd-gallery-info-btn svg) {
    display: block !important;
    margin: auto !important;
    pointer-events: none !important;
    fill: currentColor !important;
    color: currentColor !important;
  }
  /* Info button glyph follows the system theme accent */
  :global(.lg-toolbar .omd-gallery-info-btn) {
    color: var(--accent, #3D467A) !important;
  }
  :global(.lg-toolbar .omd-gallery-info-btn:hover) {
    color: #ffffff !important;
  }
  :global(.dark-theme .lg-toolbar .omd-gallery-info-btn) {
    color: var(--accent-hover, #929CD4) !important;
  }
  :global(.dark-theme .lg-toolbar .omd-gallery-info-btn:hover) {
    color: #ffffff !important;
  }
  :global(.lg-toolbar .omd-gallery-info-btn.active) {
    background: var(--accent, rgba(88, 99, 166, 0.85)) !important;
    border-color: var(--accent-hover, #929CD4) !important;
    color: #ffffff !important;
  }
  /* Hide the info button while the info plate is visible (mutual visibility),
     and when the current photo has no title/description at all */
  :global(.lg-toolbar .omd-gallery-info-btn.is-plate-shown),
  :global(.lg-toolbar .omd-gallery-info-btn.is-empty-state) {
    display: none !important;
  }

  /* Stable cursor inside the photo viewer:
     - no grab/grabbing over the image (source of chaotic hand/arrow cursor)
     - pointer only on real control buttons, uniform on their children */
  :global(.lg-outer.lg-grab img.lg-object),
  :global(.lg-outer.lg-grabbing img.lg-object) {
    cursor: default !important;
  }
  :global(.lg-toolbar .lg-icon),
  :global(.lg-toolbar .lg-icon *) {
    cursor: pointer !important;
  }
  /* Decorative svg/glyph children never take part in hit-testing */
  :global(.lg-toolbar .lg-icon svg) {
    pointer-events: none !important;
  }

  /* Download progress toast (top-center, inside the viewer) */
  :global(.omd-gallery-download-toast) {
    position: fixed;
    top: 64px;
    left: 50%;
    transform: translateX(-50%);
    width: min(360px, calc(100vw - 32px));
    background: rgba(255, 255, 255, 0.76);
    backdrop-filter: blur(16px) saturate(140%);
    -webkit-backdrop-filter: blur(16px) saturate(140%);
    border: 1px solid rgba(255, 255, 255, 0.55);
    border-radius: 12px;
    padding: 8px 14px 10px 14px;
    z-index: 2080;
    box-shadow: 0 8px 32px rgba(24, 19, 38, 0.16);
    color: #181326;
    box-sizing: border-box;
    pointer-events: none;
    user-select: none;
    cursor: default;
    transition: opacity 0.3s cubic-bezier(0.25, 1, 0.5, 1) !important;
  }
  :global(.dark-theme .omd-gallery-download-toast) {
    background: rgba(31, 36, 48, 0.78);
    border-color: var(--accent-border, rgba(123, 134, 196, 0.28));
    box-shadow: 0 8px 32px rgba(0, 0, 0, 0.45);
    color: var(--text-primary, #EEF0F6);
  }
  :global(.omd-gallery-download-toast.is-hidden) {
    opacity: 0 !important;
  }
  :global(.omd-gallery-download-toast .omd-dl-row) {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
  }
  :global(.omd-gallery-download-toast .omd-dl-label) {
    font-size: 12.5px;
    font-weight: 500;
    color: var(--text-secondary, #5A6278);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    min-width: 0;
  }
  :global(.omd-gallery-download-toast .omd-dl-pct) {
    flex: 0 0 auto;
    font-size: 11.5px;
    font-weight: 600;
    color: var(--accent, #3D467A);
    font-variant-numeric: tabular-nums;
  }
  :global(.omd-gallery-download-toast .omd-dl-track) {
    position: relative;
    margin-top: 6px;
    height: 4px;
    border-radius: 2px;
    background: rgba(22, 25, 37, 0.12);
    overflow: hidden;
  }
  :global(.omd-gallery-download-toast .omd-dl-fill) {
    position: absolute;
    left: 0;
    top: 0;
    height: 100%;
    width: 0%;
    border-radius: 2px;
    background: var(--accent, #5863A6);
    transition: width 0.2s ease;
  }
  :global(.omd-gallery-download-toast.is-done .omd-dl-fill) {
    background: #34c655;
  }
  :global(.omd-gallery-download-toast.is-failed .omd-dl-fill) {
    background: #fe6158;
  }
  /* Indeterminate (no Content-Length) sliding bar */
  :global(.omd-gallery-download-toast.indeterminate .omd-dl-fill) {
    width: 30% !important;
    animation: omd-dl-indeterminate 1.2s ease-in-out infinite;
  }
  @keyframes omd-dl-indeterminate {
    0% { left: -30%; }
    100% { left: 100%; }
  }
  :global(.dark-theme .omd-gallery-download-toast .omd-dl-label) {
    color: var(--text-primary, #EEF0F6);
  }
  :global(.dark-theme .omd-gallery-download-toast .omd-dl-pct) {
    color: var(--accent-hover, #929CD4);
  }
  :global(.dark-theme .omd-gallery-download-toast .omd-dl-track) {
    background: rgba(255, 255, 255, 0.14);
  }
  :global(.dark-theme .omd-gallery-download-toast .omd-dl-fill) {
    background: var(--accent, #7B86C4);
  }
  :global(.lg-outer .lg-counter) {
    order: 0 !important;
    margin-right: auto !important;
    color: #F8F7FC !important;
    font-size: 14px !important;
    font-weight: 500 !important;
    padding: 0 !important;
    line-height: 32px !important;
    text-shadow: 0 1px 4px rgba(0, 0, 0, 0.8) !important;
  }
  :global(.lg-toolbar .lg-download) {
    order: 10 !important;
    float: none !important;
  }
  :global(.lg-toolbar .lg-fullscreen),
  :global(.lg-toolbar .omd-gallery-fs-btn) {
    order: 20 !important;
    float: none !important;
    display: inline-flex !important;
    opacity: 1 !important;
    visibility: visible !important;
  }

  @media (max-width: 768px) {
    :global(.lg-toolbar .lg-fullscreen),
    :global(.lg-toolbar .omd-gallery-fs-btn),
    :global(.lg-fullscreen) {
      display: none !important;
    }
  }
  :global(.lg-toolbar .lg-close) {
    order: 30 !important;
    float: none !important;
    display: inline-flex !important;
    opacity: 1 !important;
    visibility: visible !important;
    cursor: pointer !important;
    pointer-events: auto !important;
    touch-action: manipulation !important;
    z-index: 2200 !important;
  }

  :global(.lg-outer:fullscreen),
  :global(.lg-outer:-webkit-full-screen),
  :global(.lg-outer:-moz-full-screen) {
    width: 100vw !important;
    height: 100vh !important;
    background: #000000 !important;
  }

  /* Completely remove zoom/lens button */
  :global(.lg-actual-size),
  :global(.lg-zoom-in),
  :global(.lg-zoom-out),
  :global(.lg-toolbar .lg-actual-size) {
    display: none !important;
  }

  /* Hide default floating middle navigation arrows */
  :global(.lg-actions),
  :global(.lg-prev),
  :global(.lg-next) {
    display: none !important;
  }

  /* Full-height overlay gallery: image occupies entire viewport height & width */
  :global(.lg-outer .lg-content) {
    top: 0 !important;
    bottom: 0 !important;
    height: 100% !important;
  }
  :global(.lg-outer .lg-item) {
    padding: 0 !important;
    top: 0 !important;
    bottom: 0 !important;
    left: 0 !important;
    right: 0 !important;
    height: 100% !important;
    will-change: transform, opacity !important;
    backface-visibility: hidden !important;
    -webkit-backface-visibility: hidden !important;
    opacity: 0;
    transition: transform 0.28s cubic-bezier(0.25, 1, 0.5, 1), opacity 0.22s ease !important;
  }
  :global(.lg-outer .lg-item.lg-current) {
    opacity: 1 !important;
    z-index: 2 !important;
  }
  :global(.lg-outer.lg-dragging .lg-item) {
    opacity: 1 !important;
    transition: none !important;
  }
  :global(.lg-outer.lg-sliding .lg-item.lg-prev-slide),
  :global(.lg-outer.lg-sliding .lg-item.lg-next-slide) {
    opacity: 1 !important;
  }
  :global(.lg-outer .lg-inner) {
    top: 0 !important;
    bottom: 0 !important;
    height: 100% !important;
  }
  :global(.lg-outer .lg-img-wrap) {
    top: 0 !important;
    bottom: 0 !important;
    height: 100% !important;
    padding: 0 !important;
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
  }
  :global(.lg-outer .lg-image) {
    max-height: 100vh !important;
    max-width: 100vw !important;
    height: auto !important;
    width: auto !important;
    object-fit: contain !important;
    margin: auto !important;
    transition: opacity 0.2s ease !important;
  }
  :global(.lg-outer .lg-current),
  :global(.lg-outer .lg-prev-slide),
  :global(.lg-outer .lg-next-slide) {
    transition-timing-function: cubic-bezier(0.25, 1, 0.5, 1) !important;
  }
  :global(.lg-outer .lg-inner) {
    touch-action: pan-y pinch-zoom !important;
  }

  /* Custom LightGallery Bottom Control Bar (TikTok / Instagram style unified pill).
     Theme-aware: tokens on the pill follow html.dark-theme / html.light-theme
     (kept in sync by App.svelte with prefers-color-scheme). */
  :global(.omd-gallery-bottom-bar) {
    /* Light theme tokens */
    --omd-pill-bg: rgba(255, 255, 255, 0.62);
    --omd-pill-border: rgba(255, 255, 255, 0.56);
    --omd-pill-shadow: 0 8px 28px rgba(22, 25, 37, 0.16);
    --omd-pill-btn-bg: rgba(22, 25, 37, 0.08);
    --omd-pill-btn-fg: var(--text-primary, #161925);
    --omd-pill-hover-bg: var(--accent, #3D467A);
    --omd-pill-hover-fg: #ffffff;
    --omd-dot-bg: rgba(22, 25, 37, 0.3);
    --omd-dot-bg-hover: rgba(22, 25, 37, 0.55);
    --omd-pill-accent: var(--accent, #3D467A);
    --omd-pill-glow: 0 0 8px var(--accent-glow, rgba(61, 70, 122, 0.32));
    --omd-track-bg: rgba(22, 25, 37, 0.2);
    --omd-thumb-bg: #ffffff;
    --omd-thumb-shadow: 0 0 8px var(--accent-glow, rgba(61, 70, 122, 0.32)), 0 2px 4px rgba(0, 0, 0, 0.25);
    position: fixed;
    bottom: 20px;
    left: 50%;
    transform: translateX(-50%);
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 0;
    padding: 5px 12px;
    background: var(--omd-pill-bg);
    backdrop-filter: blur(16px) saturate(140%);
    -webkit-backdrop-filter: blur(16px) saturate(140%);
    border: 1px solid var(--omd-pill-border);
    border-radius: 9999px;
    z-index: 1080;
    box-shadow: var(--omd-pill-shadow);
    pointer-events: auto;
    user-select: none;
    max-width: min(92vw, 560px);
    box-sizing: border-box;
    transition: opacity 0.3s cubic-bezier(0.25, 1, 0.5, 1), transform 0.3s cubic-bezier(0.25, 1, 0.5, 1), border-radius 0.25s ease, padding 0.25s ease !important;
    cursor: default;
  }
  :global(.dark-theme .omd-gallery-bottom-bar) {
    /* Dark theme tokens */
    --omd-pill-bg: rgba(31, 36, 48, 0.65);
    --omd-pill-border: var(--accent-border, rgba(123, 134, 196, 0.26));
    --omd-pill-shadow: 0 8px 28px rgba(0, 0, 0, 0.45);
    --omd-pill-btn-bg: rgba(255, 255, 255, 0.08);
    --omd-pill-btn-fg: var(--text-primary, #EEF0F6);
    --omd-pill-hover-bg: var(--accent, #7B86C4);
    --omd-pill-hover-fg: #ffffff;
    --omd-dot-bg: rgba(255, 255, 255, 0.35);
    --omd-dot-bg-hover: rgba(255, 255, 255, 0.7);
    --omd-pill-accent: var(--accent-hover, #929CD4);
    --omd-pill-glow: 0 0 8px var(--accent-glow, rgba(123, 134, 196, 0.5));
    --omd-track-bg: rgba(255, 255, 255, 0.2);
    --omd-thumb-bg: #ffffff;
    --omd-thumb-shadow: 0 0 8px var(--accent-glow, rgba(123, 134, 196, 0.5)), 0 2px 4px rgba(0, 0, 0, 0.4);
  }
  :global(.omd-gallery-bottom-bar.is-hidden),
  :global(.lg-outer.lg-hide-items .omd-gallery-bottom-bar) {
    opacity: 0 !important;
    transform: translate(-50%, 20px) scale(0.96) !important;
    pointer-events: none !important;
  }

  /* Nav row in top part of pill, aligned in center */
  :global(.omd-gallery-nav-row) {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 10px;
    width: 100%;
  }

  /* Nav Buttons inside the bottom bar (on ALL devices including mobile) */
  :global(.omd-gallery-nav-btn) {
    display: flex !important;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 28px;
    background: var(--omd-pill-btn-bg);
    border: none;
    border-radius: 50%;
    color: var(--omd-pill-btn-fg);
    cursor: pointer;
    transition: all 0.2s ease;
    padding: 0;
    flex-shrink: 0;
  }
  :global(.omd-gallery-nav-btn:hover:not(:disabled)) {
    background: var(--omd-pill-hover-bg);
    color: var(--omd-pill-hover-fg);
    transform: scale(1.1);
  }
  :global(.omd-gallery-nav-btn:disabled) {
    opacity: 0.2;
    cursor: not-allowed;
  }

  /* Dots mode (<= 15 items) */
  :global(.omd-gallery-dots) {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    padding: 2px 4px;
  }
  :global(.omd-dot) {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--omd-dot-bg);
    border: none;
    padding: 0;
    cursor: pointer;
    transition: all 0.2s ease;
  }
  :global(.omd-dot:hover) {
    background: var(--omd-dot-bg-hover);
    transform: scale(1.25);
  }
  :global(.omd-dot.active) {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--omd-pill-accent);
    box-shadow: var(--omd-pill-glow);
  }

  /* Progress Line Track mode (> 15 items, e.g. thousands of photos) */
  :global(.omd-gallery-track) {
    width: 160px;
    height: 24px;
    display: flex;
    align-items: center;
    cursor: pointer;
    padding: 0 4px;
  }
  :global(.omd-track-bg) {
    position: relative;
    width: 100%;
    height: 3px;
    background: var(--omd-track-bg);
    border-radius: 2px;
  }
  :global(.omd-track-fill) {
    position: absolute;
    left: 0;
    top: 0;
    height: 100%;
    background: var(--omd-pill-accent);
    border-radius: 2px;
    transition: width 0.15s ease;
  }
  :global(.omd-track-thumb) {
    position: absolute;
    top: 50%;
    transform: translate(-50%, -50%);
    width: 10px;
    height: 10px;
    border-radius: 50%;
    background: var(--omd-thumb-bg);
    box-shadow: var(--omd-thumb-shadow);
    border: 2px solid var(--omd-pill-accent);
    transition: left 0.15s ease, transform 0.15s ease;
  }
  :global(.omd-gallery-track:hover .omd-track-thumb) {
    transform: translate(-50%, -50%) scale(1.25);
  }

  /* Compact photo-info plate (title + description), bottom-right corner.
     Separate from the nav pill; fades in/out in sync with it on desktop.
     Colors follow the app/system theme (html.dark-theme / html.light-theme,
     kept in sync by App.svelte with prefers-color-scheme). */
  :global(.omd-gallery-info-plate) {
    position: fixed;
    right: 20px;
    bottom: 84px;
    width: min(340px, calc(100vw - 40px));
    max-height: 42vh;
    overflow-y: auto;
    background: rgba(255, 255, 255, 0.72);
    backdrop-filter: blur(16px) saturate(140%);
    -webkit-backdrop-filter: blur(16px) saturate(140%);
    border: 1px solid rgba(255, 255, 255, 0.56);
    border-radius: 14px;
    padding: 10px 12px 12px 14px;
    z-index: 1080;
    box-shadow: 0 8px 28px rgba(24, 19, 38, 0.16);
    color: #181326;
    box-sizing: border-box;
    user-select: text;
    transition: opacity 0.3s cubic-bezier(0.25, 1, 0.5, 1), transform 0.3s cubic-bezier(0.25, 1, 0.5, 1) !important;
    cursor: default;
    scrollbar-width: thin;
    scrollbar-color: rgba(24, 19, 38, 0.28) transparent;
  }
  :global(.omd-gallery-info-plate::-webkit-scrollbar) {
    width: 6px;
    height: 6px;
  }
  :global(.omd-gallery-info-plate::-webkit-scrollbar-track) {
    background: transparent !important;
  }
  :global(.omd-gallery-info-plate::-webkit-scrollbar-thumb) {
    background: rgba(24, 19, 38, 0.28) !important;
    border-radius: 3px !important;
  }
  :global(.omd-gallery-info-plate::-webkit-scrollbar-thumb:hover) {
    background: rgba(24, 19, 38, 0.5) !important;
  }
  :global(.omd-gallery-info-plate.is-hidden),
  :global(.lg-outer.lg-hide-items .omd-gallery-info-plate) {
    opacity: 0 !important;
    transform: translateY(10px) !important;
    pointer-events: none !important;
  }
  :global(.omd-gallery-info-plate.is-empty) {
    display: none !important;
  }
  :global(.omd-gallery-info-plate .omd-info-header) {
    display: flex;
    align-items: flex-start;
    gap: 8px;
  }
  :global(.omd-gallery-info-plate .omd-info-title) {
    margin: 0;
    flex: 1 1 auto;
    min-width: 0;
    font-size: 13.5px;
    font-weight: 600;
    color: #181326;
    line-height: 1.45;
    word-break: break-word;
  }
  :global(.omd-gallery-info-plate .omd-info-title:empty) {
    display: none;
  }
  :global(.omd-gallery-info-plate .omd-info-close) {
    flex: 0 0 auto;
    width: 24px;
    height: 24px;
    padding: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 50%;
    background: rgba(24, 19, 38, 0.06);
    border: 1px solid rgba(24, 19, 38, 0.15);
    color: #5B5270;
    cursor: pointer;
    transition: background-color 0.15s ease, transform 0.1s ease;
  }
  :global(.omd-gallery-info-plate .omd-info-close:hover) {
    background: var(--accent-soft, rgba(61, 70, 122, 0.14));
    color: var(--accent, #3D467A);
  }
  :global(.omd-gallery-info-plate .omd-info-close:active) {
    transform: scale(0.92);
  }
  :global(.omd-gallery-info-plate .omd-info-desc) {
    margin: 6px 0 0;
    font-size: 12.5px;
    color: #5B5270;
    line-height: 1.5;
    word-break: break-word;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }
  :global(.omd-gallery-info-plate.no-title .omd-info-desc) {
    margin-top: 0;
  }

  /* Dark theme (system / app toggle via html.dark-theme) */
  :global(.dark-theme .omd-gallery-info-plate) {
    background: rgba(31, 36, 48, 0.74);
    border-color: var(--accent-border, rgba(123, 134, 196, 0.26));
    box-shadow: 0 8px 28px rgba(0, 0, 0, 0.45);
    color: var(--text-primary, #EEF0F6);
    scrollbar-color: rgba(255, 255, 255, 0.28) transparent;
  }
  :global(.dark-theme .omd-gallery-info-plate::-webkit-scrollbar-thumb) {
    background: rgba(255, 255, 255, 0.28) !important;
  }
  :global(.dark-theme .omd-gallery-info-plate::-webkit-scrollbar-thumb:hover) {
    background: rgba(255, 255, 255, 0.5) !important;
  }
  :global(.dark-theme .omd-gallery-info-plate .omd-info-title) {
    color: var(--text-primary, #EEF0F6);
  }
  :global(.dark-theme .omd-gallery-info-plate .omd-info-desc) {
    color: var(--text-secondary, #AEB5C7);
  }
  :global(.dark-theme .omd-gallery-info-plate .omd-info-close) {
    background: rgba(255, 255, 255, 0.08);
    border-color: rgba(255, 255, 255, 0.15);
    color: var(--text-primary, #EEF0F6);
  }
  :global(.dark-theme .omd-gallery-info-plate .omd-info-close:hover) {
    background: var(--accent, rgba(123, 134, 196, 0.82));
    color: #ffffff;
  }

  /* Hide separate LightGallery subHtml box since the info lives in the plate */
  :global(.lg-sub-html) {
    display: none !important;
  }

  /* 8. Preview Panel */
  :global(.wx-file-preview .wx-card-preview) {
    object-fit: cover;
    border-radius: 6px !important;
  }
  :global(.wx-info) {
    background-color: var(--wx-background) !important;
    border-left: 1px solid var(--wx-border) !important;
    font-family: 'Inter', sans-serif !important;
  }

  /* Custom Slim Scrollbars for all SVAR & workspace views */
  :global(.filemanager-container *::-webkit-scrollbar),
  :global(.wx-filemanager *::-webkit-scrollbar),
  :global(.wx-cards::-webkit-scrollbar),
  :global(.wx-scroll::-webkit-scrollbar),
  :global(.wx-upload-area::-webkit-scrollbar),
  :global(.wx-content::-webkit-scrollbar) {
    width: 6px !important;
    height: 6px !important;
  }
  :global(.filemanager-container *::-webkit-scrollbar-track),
  :global(.wx-filemanager *::-webkit-scrollbar-track),
  :global(.wx-cards::-webkit-scrollbar-track),
  :global(.wx-scroll::-webkit-scrollbar-track),
  :global(.wx-upload-area::-webkit-scrollbar-track),
  :global(.wx-content::-webkit-scrollbar-track) {
    background: transparent !important;
  }
  :global(.filemanager-container *::-webkit-scrollbar-thumb),
  :global(.wx-filemanager *::-webkit-scrollbar-thumb),
  :global(.wx-cards::-webkit-scrollbar-thumb),
  :global(.wx-scroll::-webkit-scrollbar-thumb),
  :global(.wx-upload-area::-webkit-scrollbar-thumb),
  :global(.wx-content::-webkit-scrollbar-thumb) {
    background: var(--scrollbar-thumb, rgba(139, 128, 163, 0.45)) !important;
    border-radius: 3px !important;
  }
  :global(.filemanager-container *::-webkit-scrollbar-thumb:hover),
  :global(.wx-filemanager *::-webkit-scrollbar-thumb:hover),
  :global(.wx-cards::-webkit-scrollbar-thumb:hover),
  :global(.wx-scroll::-webkit-scrollbar-thumb:hover),
  :global(.wx-upload-area::-webkit-scrollbar-thumb:hover),
  :global(.wx-content::-webkit-scrollbar-thumb:hover) {
    background: var(--scrollbar-thumb-hover, rgba(139, 128, 163, 0.75)) !important;
  }
  :global(.filemanager-container *),
  :global(.wx-filemanager *) {
    scrollbar-width: thin !important;
    scrollbar-color: var(--scrollbar-thumb, rgba(139, 128, 163, 0.45)) transparent !important;
  }

  /* Filemanager Sidebar scrollbar: hidden by default, visible only on hover / focus (like in chats) */
  :global(.wx-sidebar),
  :global(.wx-sidebar *),
  :global(.wx-sidebar .wx-tree),
  :global(.wx-sidebar .wx-tree *) {
    scrollbar-width: thin !important;
    scrollbar-color: transparent transparent !important;
    transition: scrollbar-color 0.2s ease !important;
  }
  :global(.wx-sidebar::-webkit-scrollbar),
  :global(.wx-sidebar *::-webkit-scrollbar),
  :global(.wx-sidebar .wx-tree::-webkit-scrollbar) {
    width: 6px !important;
    height: 6px !important;
  }
  :global(.wx-sidebar::-webkit-scrollbar-track),
  :global(.wx-sidebar *::-webkit-scrollbar-track),
  :global(.wx-sidebar .wx-tree::-webkit-scrollbar-track) {
    background: transparent !important;
  }
  :global(.wx-sidebar::-webkit-scrollbar-thumb),
  :global(.wx-sidebar *::-webkit-scrollbar-thumb),
  :global(.wx-sidebar .wx-tree::-webkit-scrollbar-thumb) {
    background: transparent !important;
    border-radius: 3px !important;
    transition: background-color 0.2s ease !important;
  }

  /* Show sidebar scrollbar ONLY when sidebar is hovered or focused */
  :global(.wx-sidebar:hover),
  :global(.wx-sidebar:hover *),
  :global(.wx-sidebar:hover .wx-tree),
  :global(.wx-sidebar:hover .wx-tree *),
  :global(.wx-sidebar:focus-within),
  :global(.wx-sidebar:focus-within *),
  :global(.wx-sidebar:focus-within .wx-tree),
  :global(.wx-sidebar:focus-within .wx-tree *) {
    scrollbar-color: var(--scrollbar-thumb, rgba(139, 128, 163, 0.45)) transparent !important;
  }
  :global(.wx-sidebar:hover::-webkit-scrollbar-thumb),
  :global(.wx-sidebar:hover *::-webkit-scrollbar-thumb),
  :global(.wx-sidebar:hover .wx-tree::-webkit-scrollbar-thumb),
  :global(.wx-sidebar:focus-within::-webkit-scrollbar-thumb),
  :global(.wx-sidebar:focus-within *::-webkit-scrollbar-thumb),
  :global(.wx-sidebar:focus-within .wx-tree::-webkit-scrollbar-thumb) {
    background: var(--scrollbar-thumb, rgba(139, 128, 163, 0.45)) !important;
  }
  :global(.wx-sidebar:hover::-webkit-scrollbar-thumb:hover),
  :global(.wx-sidebar:hover *::-webkit-scrollbar-thumb:hover),
  :global(.wx-sidebar:hover .wx-tree::-webkit-scrollbar-thumb:hover),
  :global(.wx-sidebar:focus-within::-webkit-scrollbar-thumb:hover),
  :global(.wx-sidebar:focus-within *::-webkit-scrollbar-thumb:hover),
  :global(.wx-sidebar:focus-within .wx-tree::-webkit-scrollbar-thumb:hover) {
    background: var(--scrollbar-thumb-hover, rgba(139, 128, 163, 0.75)) !important;
  }

  /* Mobile Responsive Optimizations for Cards and Table */
  @media (max-width: 768px) {
    :global(.wx-toolbar) {
      height: calc(50px + env(safe-area-inset-top, 0px)) !important;
      padding-top: calc(6px + env(safe-area-inset-top, 0px)) !important;
      padding-bottom: 6px !important;
      padding-left: calc(8px + env(safe-area-inset-left, 0px)) !important;
      padding-right: calc(8px + env(safe-area-inset-right, 0px)) !important;
      gap: 6px !important;
      box-sizing: border-box !important;
    }
    :global(.wx-toolbar .wx-search-input) {
      min-width: 110px !important;
    }

    /* Table View Mobile: Hide Date Column on Mobile */
    :global(.wx-table-view .wx-header-row .wx-header-cell:nth-child(3)),
    :global(.wx-table-view .wx-header-cell:nth-child(3)),
    :global(.wx-table-view .wx-row .wx-cell:nth-child(3)),
    :global(.wx-grid .wx-header-cell:nth-child(3)),
    :global(.wx-grid .wx-cell:nth-child(3)) {
      display: none !important;
    }
    :global(.wx-table-view .wx-header .wx-cell),
    :global(.wx-table-view .wx-row .wx-cell) {
      padding: 6px 8px !important;
      font-size: 13px !important;
    }
  }

  .proton-app {
    position: absolute;
    top: 0; left: 0; right: 0; bottom: 0;
    display: flex;
    flex-direction: column;
  }

  .content {
    position: absolute;
    top: 0; left: 0; right: 0; bottom: 0;
  }

  .connect-box {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 16px;
    margin-top: 24px;
  }

  .connect-box input {
    background-color: var(--bg-card);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
    padding: 8px 14px;
    border-radius: 6px;
    width: 280px;
    font-size: 14px;
  }

  .connect-box input:focus {
    outline: none;
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent-soft);
  }

  /* Base button (legacy .btn) */
  .login-card .btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    font-weight: 600;
    border-radius: 8px;
    padding: 12px 16px;
    font-size: 16px;
    line-height: 1.5;
    cursor: pointer;
    box-sizing: border-box;
    border: 1px solid transparent;
    transition: background-color .15s ease-in-out, border-color .15s ease-in-out, box-shadow .15s ease-in-out;
    text-transform: none;
  }
  .login-card .btn:disabled { opacity: 0.65; cursor: not-allowed; }

  .login-card .btn-primary {
    background: var(--accent);
    border-color: var(--accent);
    color: var(--accent-on-brand, #FFFFFF);
  }
  .login-card .btn-primary:hover:not(:disabled) {
    background: var(--accent-hover);
    border-color: var(--accent-hover);
    box-shadow: 0 4px 14px var(--accent-glow);
  }

  .login-card .btn-outline-secondary {
    background-color: var(--bg-secondary);
    border-color: var(--border-color);
    color: var(--text-primary);
  }
  .login-card .btn-outline-secondary:hover:not(:disabled) {
    background-color: var(--bg-card-hover);
    border-color: var(--accent-border);
    color: var(--text-primary);
  }

  /* Login card — adapts automatically to skin-files (Amethyst) vs skin-chat (Magenta) */
  .login-body {
    position: absolute;
    top: 0; left: 0; right: 0; bottom: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: var(--bg-primary);
    transition: background 0.2s ease;
  }

  .login-card {
    position: relative;
    max-width: 420px;
    width: 100%;
    background-color: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 16px;
    padding: 32px;
    box-shadow: 0 8px 28px rgba(24, 19, 38, 0.06);
    color: var(--text-primary);
    margin: 16px;
    box-sizing: border-box;
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    transition: all 0.2s ease;
  }

  .dark-theme .login-card,
  :global(.dark-theme) .login-card,
  :global(html.omd-dark) .login-card {
    background-color: var(--bg-card);
    border-color: var(--border-color);
    box-shadow: 0 16px 40px rgba(0, 0, 0, 0.45);
    color: var(--text-primary);
  }

  /* Login Card Language Selector in top right */
  .login-lang-select-wrap {
    position: absolute;
    top: 18px;
    right: 18px;
    display: inline-flex;
    align-items: center;
    cursor: pointer;
    z-index: 15;
  }

  .login-lang-display {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 10px;
    border-radius: 8px;
    font-size: 12px;
    font-weight: 700;
    pointer-events: none;
    background-color: var(--bg-secondary);
    color: var(--text-secondary);
    border: 1px solid var(--border-color);
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.05);
    transition: all 0.2s ease;
  }

  .dark-theme .login-lang-display,
  :global(.dark-theme) .login-lang-display,
  :global(html.omd-dark) .login-lang-display {
    background-color: var(--bg-elevated);
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.2);
  }

  .login-lang-select-wrap:hover .login-lang-display,
  .login-lang-select-wrap:focus-within .login-lang-display {
    border-color: var(--accent);
    color: var(--accent);
  }

  .login-lang-code {
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.5px;
    line-height: 1;
  }

  .login-lang-globe,
  .login-lang-arrow {
    opacity: 0.7;
    flex-shrink: 0;
  }

  .login-lang-select-hidden {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    opacity: 0;
    cursor: pointer;
    z-index: 2;
  }

  .dark-theme .login-lang-select-hidden,
  :global(.dark-theme) .login-lang-select-hidden,
  :global(html.omd-dark) .login-lang-select-hidden {
    color-scheme: dark;
  }

  .dark-theme .login-lang-select-hidden option,
  :global(.dark-theme) .login-lang-select-hidden option,
  :global(html.omd-dark) .login-lang-select-hidden option {
    background-color: var(--bg-secondary);
    color: var(--text-primary);
  }

  .login-card.loading {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    min-height: 420px;
    text-align: center;
  }
  .login-card.loading .login-logo {
    margin-bottom: 28px;
  }

  .login-logo {
    text-align: center;
    margin-bottom: 24px;
  }

  .login-title {
    font-size: 32px;
    font-weight: 700;
    color: var(--text-primary);
    margin: 0 0 8px;
    text-align: center;
  }

  .login-subtitle {
    font-size: 16px;
    color: var(--text-secondary);
    margin: 0 0 24px;
    text-align: center;
  }

  /* Bootstrap spacing helpers used by legacy markup */
  .login-card .mb-3 { margin-bottom: 16px; }
  .login-card .mb-4 { margin-bottom: 24px; }
  .login-card .mt-4 { margin-top: 24px; }
  .login-card .w-100 { width: 100%; }
  .login-card .text-center { text-align: center; }

  .form-label {
    display: inline-block;
    font-size: 14px;
    font-weight: 500;
    margin-bottom: 4px;
    color: var(--text-primary);
  }

  .form-control {
    display: block;
    width: 100%;
    padding: 8px 12px;
    font-size: 16px;
    line-height: 1.5;
    background-color: var(--bg-primary);
    background-clip: padding-box;
    border: 1px solid var(--border-color);
    border-radius: 6px;
    color: var(--text-primary);
    box-sizing: border-box;
    box-shadow: none;
    transition: border-color .15s ease-in-out, box-shadow .15s ease-in-out;
  }
  .form-control:focus {
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent-soft);
    background-color: var(--bg-secondary);
    color: var(--text-primary);
    outline: none;
  }
  .form-control::placeholder { color: var(--text-muted); }

  /* 44px rows for outline buttons in selection stage (legacy container height) */
  .login-btn-row { height: 44px; }
  .login-btn-row .btn { height: 100%; padding: 0 12px; }

  .small-link {
    background: none;
    border: none;
    padding: 0;
    font-size: 14px;
    color: var(--accent);
    font-weight: 500;
    text-decoration: none;
    cursor: pointer;
  }
  .small-link:hover {
    color: var(--accent-hover);
    text-decoration: underline;
  }

  .text-secondary {
    font-size: 14px;
    color: var(--text-secondary);
  }

  /* Unified login footer with balanced spacing and clean typography */
  .login-footer {
    margin-top: 24px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 12px;
    text-align: center;
  }

  .login-footer-switch {
    font-size: 14px;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    line-height: 1.4;
  }

  .login-footer-terms {
    font-size: 12px;
    line-height: 1.6;
    color: var(--text-muted);
    max-width: 330px;
    margin: 0 auto;
  }

  .login-footer-terms a {
    color: inherit;
    text-decoration: underline;
    text-underline-offset: 2px;
    transition: color 0.15s ease;
  }

  .login-footer-terms a:hover {
    color: var(--accent);
  }

  .login-links {
    display: flex;
    justify-content: space-between;
    align-items: center;
  }

  .login-error {
    color: #F83B42;
    font-size: 14px;
    margin: 8px 0 0;
    text-align: center;
    width: 100%;
  }

  .optional {
    font-weight: 400;
    color: var(--text-muted);
    font-size: 12px;
  }

  /* Recovery screen */
  .recovery-did, .recovery-key {
    width: 100%;
    background: var(--bg-primary);
    border: 1px solid var(--border-color);
    border-radius: 6px;
    padding: 8px 12px;
    text-align: left;
    box-sizing: border-box;
  }
  .recovery-label {
    display: block;
    font-size: 14px;
    font-weight: 400;
    color: var(--text-secondary);
    margin-bottom: 4px;
  }
  .recovery-value {
    font-family: 'Roboto Mono', monospace;
    font-size: 14px;
    color: var(--text-primary);
    word-break: break-all;
  }
  .recovery-value--key {
    font-size: 16px;
    letter-spacing: 1px;
  }

  .recovery-actions {
    display: flex;
    gap: 16px;
    width: 100%;
  }

  .status-badge.connected {
    background-color: #064e3b;
    color: #34d399;
    padding: 6px 14px;
    border-radius: 20px;
    font-size: 13px;
    font-weight: 600;
  }

  .filemanager-container {
    position: absolute;
    top: 0; left: 0; right: 0; bottom: 0;
  }

  /* Exit from the files overlay in chat mode. Rendered inside the Svar
     toolbar next to the table/cards switch, so it follows Svar theming. */
  :global(.omd-fm-exit-btn) {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 32px;
    height: 32px;
    padding: 0;
    border: none;
    border-radius: 6px;
    background: transparent;
    color: var(--wx-fm-button-font-color, var(--wx-color-font, #C4B8DB));
    cursor: pointer;
    transition: background 0.15s ease, color 0.15s ease;
  }

  :global(.omd-fm-exit-btn:hover) {
    background: var(--wx-background-alt, rgba(168, 85, 247, 0.14));
    color: var(--top-menu-hover-color, #C084FC);
  }

  :global(.omd-fm-exit-btn:focus-visible) {
    outline: 2px solid var(--wx-color-primary, #9333EA);
    outline-offset: 1px;
  }

  .sub-app-container {
    position: absolute;
    top: 0; left: 0; right: 0; bottom: 0;
    display: flex;
    flex-direction: column;
    background-color: var(--bg-primary, #0D0B12);
  }

  .chat-placeholder {
    align-items: center;
    justify-content: center;
  }
  /* Force the SVAR widget root to fill the container and prevent flex collapse */
  .filemanager-container :global(.wx-filemanager) {
    position: absolute !important;
    top: 0; left: 0; right: 0; bottom: 0;
    height: 100% !important;
    width: 100% !important;
    background-color: var(--bg-secondary) !important;
  }
  
  /* SVAR Structural Bugfix: 
     ContextMenu and ActionMenu components output unstyled divs that break the flex height chain.
     We force them and their descendants to maintain proper flex propagation. */
  .filemanager-container :global(.wx-filemanager > div[data-menu-ignore="true"]) {
    display: flex !important;
    flex-direction: column !important;
    flex: 1 !important;
    min-height: 0 !important;
  }
  .filemanager-container :global(.wx-filemanager > div[data-menu-ignore="true"] > div[data-menu-ignore="true"]) {
    display: flex !important;
    flex-direction: row !important;
    flex: 1 !important;
    min-height: 0 !important;
  }
  .filemanager-container :global(.wx-filemanager > div[data-menu-ignore="true"] > div[data-menu-ignore="true"] > .wx-upload-area) {
    display: flex !important;
    flex-direction: column !important;
    flex: 1 !important;
    min-height: 0 !important;
  }
  .filemanager-container :global(.wx-content-wrapper) {
    height: 100% !important;
    max-height: 100% !important;
    min-height: 0 !important;
    overflow: hidden !important;
    background-color: var(--bg-secondary) !important;
  }
  .filemanager-container :global(.wx-content),
  .filemanager-container :global(.wx-search-zone) {
    height: 100% !important;
    max-height: 100% !important;
    flex-shrink: 1 !important;
    min-height: 0 !important;
    overflow: hidden !important;
    background-color: var(--bg-primary) !important;
    border-top-left-radius: 12px !important;
    border-top-right-radius: 12px !important;
    padding-bottom: env(safe-area-inset-bottom, 0px) !important;
    box-sizing: border-box !important;
  }
  .filemanager-container :global(.wx-sidebar) {
    padding-bottom: env(safe-area-inset-bottom, 0px) !important;
    box-sizing: border-box !important;
  }
  .filemanager-container :global(.wx-cards) {
    padding-bottom: calc(16px + env(safe-area-inset-bottom, 0px)) !important;
    box-sizing: border-box !important;
  }
  .filemanager-container :global(.wx-table-view) {
    padding-bottom: env(safe-area-inset-bottom, 0px) !important;
    box-sizing: border-box !important;
  }

  /* SVAR Grid / Table View: ensure sticky header has solid opaque background and border */
  .filemanager-container :global(.wx-header-wrapper) {
    background-color: var(--wx-background, #ffffff);
    border-bottom: 1px solid var(--wx-table-border, #e6e6e6);
  }
  .filemanager-container :global(.wx-header .wx-cell) {
    background-color: var(--wx-background, #ffffff);
  }

  .error {
    color: #f87171 !important;
    margin-top: 12px;
  }

  /* Progress Overlay */
  .progress-overlay {
    position: absolute;
    top: 0; left: 0; right: 0; bottom: 0;
    background: rgba(13, 11, 18, 0.78);
    display: flex;
    justify-content: center;
    align-items: center;
    z-index: 1000;
    backdrop-filter: blur(4px);
  }

  .progress-box {
    background: var(--bg-card, #16131F);
    padding: 24px;
    border-radius: 12px;
    box-shadow: 0 10px 25px rgba(0,0,0,0.45);
    width: 300px;
    text-align: center;
    border: 1px solid var(--border-color, #2D2640);
  }

  .progress-box p {
    margin: 0 0 12px 0;
    font-weight: 500;
    color: var(--text-primary, #F8F7FC);
  }

  .doc-load-topbar {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    z-index: 7;
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 6px 12px;
    background: var(--bg-secondary, #16131F);
    border-bottom: 1px solid var(--accent-border, rgba(168, 85, 247, 0.25));
  }
  .doc-load-label { font-size: 12px; color: var(--text-secondary, #C4B8DB); white-space: nowrap; }
  .doc-load-track { flex: 1; height: 4px; border-radius: 2px; overflow: hidden; background: var(--accent-soft, rgba(168, 85, 247, 0.15)); }
  .doc-load-fill {
    height: 100%;
    width: 40%;
    border-radius: 2px;
    background: var(--accent, #A855F7);
    animation: doc-load-slide 1.4s infinite ease-in-out;
  }
  @keyframes doc-load-slide {
    0% { margin-left: -30%; width: 30%; }
    60% { margin-left: 60%; width: 30%; }
    100% { margin-left: 100%; width: 10%; }
  }

  .progress-bar {
    height: 8px;
    background: var(--bg-primary, #0D0B12);
    border-radius: 4px;
    overflow: hidden;
    margin-bottom: 8px;
  }

  .progress-fill {
    height: 100%;
    background: var(--accent, #A855F7);
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
    font-size: 13px;
    color: var(--text-secondary, #8B80A3);
    margin: 8px 0 0 0 !important;
  }

  .cancel-btn {
    margin-top: 16px;
    background: transparent;
    border: 1px solid var(--border-color, #2D2640);
    color: var(--text-primary, #F8F7FC);
    border-radius: 6px;
    padding: 6px 16px;
    cursor: pointer;
    font-size: 13px;
    transition: all 0.2s;
  }
  
  .cancel-btn:hover {
    background: var(--bg-card-hover, #211C2E);
    border-color: var(--accent, #A855F7);
  }

  /* Viewer Modal */
  .viewer-modal {
    position: fixed;
    top: 0; left: 0; right: 0; bottom: 0;
    width: 100vw;
    height: 100vh;
    background: var(--bg-primary, #151821);
    z-index: 99999;
    display: flex;
    flex-direction: column;
    padding-top: env(safe-area-inset-top, 0px) !important;
    padding-bottom: env(safe-area-inset-bottom, 0px) !important;
    padding-left: env(safe-area-inset-left, 0px) !important;
    padding-right: env(safe-area-inset-right, 0px) !important;
    box-sizing: border-box !important;
  }

  .viewer-modal.light-theme {
    background: var(--bg-primary, #F8F7FC) !important;
  }

  .viewer-modal.dark-theme {
    background: var(--bg-secondary, #16131F) !important;
  }

  .viewer-body {
    flex: 1;
    min-height: 0;
    display: flex;
    overflow: hidden;
    position: relative;
  }

  .viewer-body > :global(*) {
    width: 100%;
    height: 100%;
  }

  /* Empty Root State (0 Devices Connected) */
  .empty-root-container {
    position: absolute;
    top: 56px;
    left: 240px;
    right: 0;
    bottom: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 24px;
    z-index: 10;
    pointer-events: none;
  }

  .empty-root-container.light-theme {
    --bg-secondary: #FFFFFF;
    --bg-card: #FFFFFF;
    --border-color: #D8DCE8;
    --text-primary: #161925;
    --wx-color-font: #161925;
    --wx-color-font-alt: #5A6278;
    --wx-color-primary: #3D467A;
    --wx-color-primary-hover: #5863A6;
    --wx-background: #FFFFFF;
    --wx-background-alt: #F1F3F9;
    --wx-background-hover: #E9ECF5;
  }
  .empty-root-container.dark-theme {
    --bg-secondary: #1F2430;
    --bg-card: #1F2430;
    --border-color: #32394B;
    --text-primary: #EEF0F6;
    --wx-color-font: #EEF0F6;
    --wx-color-font-alt: #7E869B;
    --wx-color-primary: #5863A6;
    --wx-color-primary-hover: #6874BA;
    --wx-background: #151821;
    --wx-background-alt: #191D28;
    --wx-background-hover: #282E3D;
  }
  .empty-root-container.modal-mode {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    z-index: 10000;
    pointer-events: auto;
    background: rgba(13, 11, 18, 0.65);
    backdrop-filter: blur(4px);
  }
  .empty-root-container.modal-mode .empty-root-card {
    position: relative;
  }
  .empty-root-close {
    position: absolute;
    top: 12px;
    right: 14px;
    background: none;
    border: none;
    color: var(--text-muted, #8B80A3);
    font-size: 22px;
    cursor: pointer;
    padding: 0;
    line-height: 1;
    z-index: 1;
    transition: color 0.15s;
  }
  .empty-root-close:hover {
    color: var(--text-primary, #F8F7FC);
  }
  @media (max-width: 768px) {
    .empty-root-container {
      left: 0;
    }
  }
  .empty-root-card {
    pointer-events: auto;
    background-color: var(--bg-secondary, #16131F);
    border: 1px solid var(--border-color, #2D2640);
    border-radius: 16px;
    padding: 36px 32px;
    max-width: 460px;
    width: 100%;
    text-align: center;
    box-shadow: 0 12px 36px rgba(0, 0, 0, 0.35);
    animation: fadeIn 0.2s ease-out;
  }
  .empty-root-icon {
    width: 72px;
    height: 72px;
    margin: 0 auto 18px;
    border-radius: 50%;
    background-color: var(--accent-soft, rgba(168, 85, 247, 0.14));
    color: var(--accent, #A855F7);
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .empty-root-title {
    font-size: 19px;
    font-weight: 600;
    color: var(--wx-color-font, #F8F7FC);
    margin: 0 0 8px 0;
    font-family: 'Inter', sans-serif;
  }
  .empty-root-desc {
    font-size: 13.5px;
    line-height: 1.5;
    color: var(--wx-color-font-alt, #8B80A3);
    margin: 0 0 22px 0;
    font-family: 'Inter', sans-serif;
  }
  .empty-root-form {
    display: flex;
    gap: 8px;
    width: 100%;
    box-sizing: border-box;
  }
  .empty-root-input {
    flex: 1 1 auto;
    min-width: 0;
    width: 100%;
    box-sizing: border-box;
    height: 40px;
    padding: 0 12px;
    background-color: var(--bg-card, #FFFFFF);
    border: 1px solid var(--border-color, #DDD6EB);
    border-radius: 8px;
    color: var(--wx-color-font, #181326);
    font-family: 'Roboto Mono', monospace;
    font-size: 13px;
    outline: none;
    transition: border-color 0.15s ease, box-shadow 0.15s ease;
  }
  .empty-root-input:focus {
    border-color: var(--wx-color-primary, #7E22CE);
    box-shadow: 0 0 0 2px var(--accent-soft, rgba(126, 34, 206, 0.18));
  }
  :global(html.omd-dark) .empty-root-input {
    background-color: var(--wx-background-alt, #120F1A);
    border-color: var(--border-color, #2D2640);
    color: var(--wx-color-font, #F8F7FC);
  }
  :global(html.omd-dark) .empty-root-input:focus {
    border-color: var(--wx-color-primary, #5863A6);
    box-shadow: 0 0 0 2px var(--accent-soft, rgba(123, 134, 196, 0.25));
  }
  .empty-root-btn {
    flex-shrink: 0;
    height: 40px;
    padding: 0 16px;
    background-color: var(--wx-color-primary, #7E22CE);
    border: none;
    border-radius: 8px;
    color: #FFFFFF;
    font-family: 'Inter', sans-serif;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
    transition: background-color 0.15s ease, opacity 0.15s ease;
  }
  .empty-root-btn:hover:not(:disabled) {
    background-color: var(--wx-color-primary-hover, #9333EA);
  }
  .empty-root-btn:disabled {
    opacity: 0.45;
    cursor: not-allowed;
  }
  .empty-root-detected-section {
    width: 100%;
    margin-top: 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
    text-align: left;
  }
  .empty-root-detected-header {
    font-size: 11.5px;
    font-weight: 600;
    color: var(--wx-color-font-alt, #8B80A3);
    text-transform: uppercase;
    letter-spacing: 0.5px;
    padding-left: 2px;
  }
  .empty-root-detected-list {
    display: flex;
    flex-direction: column;
    gap: 8px;
    max-height: 180px;
    overflow-y: auto;
  }
  .empty-root-detected-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 10px 12px;
    background: var(--bg-soft, rgba(168, 85, 247, 0.05));
    border: 1px solid var(--border-color, #2D2640);
    border-radius: 8px;
    gap: 10px;
  }
  .empty-root-detected-info {
    display: flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
  }
  .empty-root-detected-icon {
    color: var(--accent, #A855F7);
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }
  .empty-root-detected-text {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .empty-root-detected-name {
    font-size: 13px;
    font-weight: 600;
    color: var(--text-primary, #F8F7FC);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .empty-root-detected-id {
    font-size: 11px;
    color: var(--wx-color-font-alt, #8B80A3);
  }
  .empty-root-detected-id code {
    font-family: monospace;
  }
  .empty-root-detected-btn {
    background: var(--wx-color-primary, #7E22CE);
    color: white;
    border: none;
    padding: 6px 12px;
    border-radius: 8px;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
    transition: background 0.15s ease;
    flex-shrink: 0;
  }
  .empty-root-detected-btn:hover {
    background: var(--wx-color-primary-hover, #9333EA);
  }
  .empty-root-retry {
    margin-top: 14px;
  }
  .empty-root-scan-qr {
    margin-top: 10px;
    background: var(--bg-soft, rgba(168, 85, 247, 0.10));
    border: 1px solid var(--accent-border, rgba(168, 85, 247, 0.35));
    color: var(--accent, #A855F7);
  }
  .empty-root-scan-qr:hover {
    background: var(--accent-soft, rgba(168, 85, 247, 0.20));
    border-color: var(--accent-hover, #C084FC);
  }
  .empty-root-divider {
    display: flex;
    align-items: center;
    gap: 10px;
    margin: 22px 0 16px;
    color: var(--wx-color-font-alt, #8B80A3);
    font-size: 12px;
    font-family: 'Inter', sans-serif;
  }
  .empty-root-divider::before,
  .empty-root-divider::after {
    content: "";
    flex: 1;
    height: 1px;
    background: var(--border-color, #2D2640);
  }
  .empty-root-local-hint {
    font-size: 12.5px;
    line-height: 1.45;
    color: var(--wx-color-font-alt, #8B80A3);
    margin: 0 0 10px 0;
    font-family: 'Inter', sans-serif;
  }
  .empty-root-local .p2p-outline-btn {
    margin: 0;
  }
  .empty-root-spinner {
    width: 16px;
    height: 16px;
    border: 2px solid var(--accent-soft, rgba(168, 85, 247, 0.25));
    border-top-color: var(--accent, #A855F7);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }
  @keyframes spin {
    to { transform: rotate(360deg); }
  }
  .empty-root-error {
    font-size: 12px;
    color: #f83b42;
    margin: 8px 0 0 0;
    font-family: 'Inter', sans-serif;
  }

  /* ── Connect / Info Modal Overlay ─────────────────────── */
  :global(.modal-overlay) {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(13, 11, 18, 0.65);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 10000;
    padding: env(safe-area-inset-top, 0px) env(safe-area-inset-right, 0px) env(safe-area-inset-bottom, 0px) env(safe-area-inset-left, 0px);
    box-sizing: border-box;
  }

  .share-modal {
    background: var(--bg-card, #16131F);
    border: 1px solid var(--border-color, #2D2640);
    border-radius: 12px;
    width: 400px;
    max-width: 90vw;
    padding: 0;
    box-shadow: 0 20px 40px rgba(0, 0, 0, 0.45);
    color: var(--text-primary, #F8F7FC);
    font-family: 'Inter', sans-serif;
  }

  .share-modal-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 16px 20px;
    border-bottom: 1px solid var(--border-color, #2D2640);
  }

  .share-modal-title {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .share-modal-title h3 {
    margin: 0;
    font-size: 1rem;
    font-weight: 600;
    color: var(--text-primary, #F8F7FC);
  }

  .share-modal-close {
    background: none;
    border: none;
    color: var(--text-muted, #8B80A3);
    font-size: 18px;
    cursor: pointer;
    padding: 4px;
    line-height: 1;
    transition: color 0.15s;
  }

  .share-modal-close:hover {
    color: var(--text-primary, #F8F7FC);
  }

  .share-modal-body {
    padding: 20px;
  }

  .share-modal-desc {
    margin: 0 0 16px 0;
    font-size: 0.9rem;
    color: var(--text-secondary, #C4B8DB);
    line-height: 1.5;
  }

  .share-link-input {
    width: 100%;
    padding: 10px 14px;
    background: var(--bg-primary, #0D0B12);
    border: 1px solid var(--border-color, #2D2640);
    border-radius: 6px;
    color: var(--text-primary, #F8F7FC);
    font-size: 14px;
    font-family: monospace;
    outline: none;
    transition: border-color 0.2s;
    box-sizing: border-box;
  }

  .share-link-input:focus {
    border-color: var(--accent, #A855F7);
  }

  .share-link-input::placeholder {
    color: var(--text-muted, #8B80A3);
  }

  .btn-primary {
    background: var(--accent, #9333EA);
    color: #fff;
    border: none;
    border-radius: 6px;
    cursor: pointer;
    font-size: 14px;
    font-weight: 500;
    transition: background 0.15s;
  }

  .btn-primary:hover:not(:disabled) {
    background: var(--accent-hover, #A855F7);
  }

  .btn-primary:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  /* ── Custom Confirmation Modal Styles ─────────────────── */
  .confirm-modal-overlay {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(13, 11, 18, 0.72);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 10000;
    padding: env(safe-area-inset-top, 0px) env(safe-area-inset-right, 0px) env(safe-area-inset-bottom, 0px) env(safe-area-inset-left, 0px);
    box-sizing: border-box;
  }

  .confirm-modal-box {
    background: var(--bg-card, #16131F);
    border: 1px solid var(--border-color, #2D2640);
    border-radius: 12px;
    width: 420px;
    max-width: 90vw;
    padding: 24px;
    box-shadow: 0 20px 40px rgba(0, 0, 0, 0.45);
    color: var(--text-primary, #F8F7FC);
    font-family: 'Inter', sans-serif;
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
    color: var(--text-primary, #F8F7FC);
  }

  .confirm-danger-icon {
    font-size: 1.4rem;
    color: #f85149;
  }

  .confirm-modal-body p {
    margin: 0 0 8px 0;
    font-size: 0.95rem;
    line-height: 1.5;
    color: var(--text-secondary, #C4B8DB);
    word-break: break-word;
    overflow-wrap: anywhere;
  }

  .confirm-modal-body strong {
    word-break: break-word;
    overflow-wrap: anywhere;
  }

  .confirm-modal-subtext {
    font-size: 0.85rem !important;
    color: var(--text-muted, #8B80A3) !important;
  }

  .confirm-modal-actions {
    display: flex;
    justify-content: flex-end;
    gap: 10px;
    margin-top: 20px;
  }

  .confirm-btn {
    padding: 8px 16px;
    border-radius: 6px;
    font-size: 0.9rem;
    font-weight: 500;
    cursor: pointer;
    border: 1px solid transparent;
    transition: background 0.15s, border-color 0.15s;
  }

  .confirm-btn.cancel {
    background: var(--bg-elevated, #1D1829);
    border-color: var(--border-color, #2D2640);
    color: var(--text-primary, #F8F7FC);
  }
  .confirm-btn.cancel:hover {
    background: var(--bg-card-hover, #211C2E);
    border-color: var(--accent-border);
  }

  .confirm-btn.danger {
    background: #da3633;
    color: #ffffff;
  }
  .confirm-btn.danger:hover {
    background: #b82a28;
  }

  .confirm-btn.primary {
    background: var(--accent, #9333EA);
    color: #ffffff;
  }
  .confirm-btn.primary:hover {
    background: var(--accent-hover, #A855F7);
  }

  .omd-ai-agreement {
    width: min(560px, 92vw);
  }

  .confirm-modal-box.light-theme,
  :global(.light-theme) .confirm-modal-box.light-theme {
    background: var(--bg-card, #ffffff);
    border-color: var(--border-color, #DDD6EB);
    color: var(--text-primary, #181326);
    box-shadow: 0 20px 40px rgba(24, 19, 38, 0.15);
  }

  /* P2P Splash Screen Styles */
  .p2p-loading-overlay {
    position: fixed;
    top: 0;
    left: 0;
    width: 100vw;
    height: 100vh;
    display: flex;
    align-items: center;
    justify-content: center;
    background: var(--bg-primary);
    z-index: 99999;
    padding: 20px;
    box-sizing: border-box;
    transition: background 0.2s ease;
  }

  .p2p-loading-box {
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    max-width: 380px;
    width: 100%;
    padding: 32px 24px;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 16px;
    box-shadow: 0 8px 28px rgba(24, 19, 38, 0.06);
    color: var(--text-primary);
    box-sizing: border-box;
    transition: all 0.2s ease;
  }

  .dark-theme .p2p-loading-box,
  .p2p-loading-overlay.dark-theme .p2p-loading-box,
  :global(.dark-theme) .p2p-loading-box,
  :global(html.omd-dark) .p2p-loading-box {
    box-shadow: 0 16px 40px rgba(0, 0, 0, 0.45);
  }

  .p2p-brand-logo {
    height: 40px;
    width: auto;
    max-width: 180px;
    object-fit: contain;
    margin: 0 auto;
    display: block;
  }

  .p2p-spinner-container {
    position: relative;
    width: 64px;
    height: 64px;
    display: flex;
    align-items: center;
    justify-content: center;
    margin-bottom: 20px;
  }

  .p2p-spinner-ring {
    position: absolute;
    width: 64px;
    height: 64px;
    border-radius: 50%;
    border: 3px solid var(--accent-soft, rgba(168, 85, 247, 0.2));
    border-top-color: var(--accent-hover, #C084FC);
    animation: p2pSpin 1s cubic-bezier(0.55, 0.15, 0.45, 0.85) infinite;
  }

  @keyframes p2pSpin {
    0% { transform: rotate(0deg); }
    100% { transform: rotate(360deg); }
  }

  .p2p-spinner-core {
    color: var(--accent);
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .p2p-status-title {
    font-size: 18px;
    font-weight: 600;
    color: var(--text-primary);
    margin: 0 0 8px 0;
  }

  .p2p-status-desc {
    font-size: 13.5px;
    color: var(--text-secondary);
    margin: 0 0 16px 0;
    line-height: 1.5;
  }

  .p2p-dots-loader {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .p2p-dots-loader span {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background-color: var(--accent);
    animation: p2pBlink 1.4s infinite ease-in-out both;
  }

  .p2p-dots-loader span:nth-child(1) { animation-delay: -0.32s; }
  .p2p-dots-loader span:nth-child(2) { animation-delay: -0.16s; }
  .p2p-dots-loader span:nth-child(3) { animation-delay: 0s; }

  @keyframes p2pBlink {
    0%, 80%, 100% { transform: scale(0.6); opacity: 0.3; }
    40% { transform: scale(1.2); opacity: 1; background-color: var(--accent-hover); }
  }

  .p2p-error-icon-badge {
    width: 56px;
    height: 56px;
    border-radius: 50%;
    background: rgba(239, 68, 68, 0.08);
    border: 1px solid rgba(239, 68, 68, 0.2);
    display: flex;
    align-items: center;
    justify-content: center;
    margin: 0 auto;
  }

  .dark-theme .p2p-error-icon-badge,
  .p2p-loading-overlay.dark-theme .p2p-error-icon-badge,
  :global(.dark-theme) .p2p-error-icon-badge,
  :global(html.omd-dark) .p2p-error-icon-badge {
    background: rgba(239, 68, 68, 0.12);
    border-color: rgba(239, 68, 68, 0.3);
    box-shadow: 0 0 20px rgba(239, 68, 68, 0.15);
  }

  .p2p-btn-stack {
    display: flex;
    flex-direction: column;
    gap: 10px;
    width: 100%;
  }

  .p2p-action-btn {
    width: 100%;
    height: 42px;
    background: var(--accent);
    color: #ffffff;
    border: none;
    border-radius: 8px;
    font-size: 14px;
    font-weight: 600;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    cursor: pointer;
    transition: background 0.15s ease, transform 0.1s ease;
  }

  .p2p-action-btn:hover:not(:disabled) {
    background: var(--accent-hover);
  }

  .p2p-action-btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .p2p-outline-btn {
    width: 100%;
    height: 42px;
    background: var(--bg-secondary);
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    font-size: 14px;
    font-weight: 500;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    text-decoration: none;
    cursor: pointer;
    transition: all 0.15s ease;
    box-sizing: border-box;
  }

  .p2p-outline-btn:hover:not(:disabled) {
    background: var(--bg-card-hover);
    color: var(--accent);
    border-color: var(--accent-border);
  }

  .p2p-outline-btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  /* Mobile Phone Wakeup / P2P Reconnection / Loading Toast */
  .p2p-reconnect-toast {
    position: fixed;
    top: 12px;
    left: 50%;
    transform: translateX(-50%);
    z-index: 999999;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 0 16px;
    height: 32px;
    box-sizing: border-box;
    border-radius: 16px;
    background: var(--bg-secondary, #ffffff);
    border: 1px solid var(--border-color, #DDD6EB);
    box-shadow: 0 4px 16px rgba(24, 19, 38, 0.12);
    backdrop-filter: blur(12px);
    -webkit-backdrop-filter: blur(12px);
    color: var(--text-primary, #181326);
    font-size: 12.5px;
    font-weight: 500;
    white-space: nowrap;
    max-width: calc(100vw - 32px);
    pointer-events: none;
    animation: p2pToastIn 0.25s cubic-bezier(0.16, 1, 0.3, 1);
  }

  :global(html.omd-dark) .p2p-reconnect-toast {
    background: var(--bg-elevated, rgba(29, 24, 41, 0.94));
    border: 1px solid var(--border-color, #2D2640);
    box-shadow: 0 6px 20px rgba(0, 0, 0, 0.45);
    color: var(--text-primary, #F8F7FC);
  }

  .p2p-reconnect-toast span {
    display: inline-flex;
    align-items: center;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    line-height: 1;
    margin: 0;
    padding: 0;
  }

  .p2p-reconnect-toast svg {
    flex-shrink: 0;
    margin: 0;
    display: block;
  }

  @keyframes p2pToastIn {
    from {
      opacity: 0;
      transform: translate(-50%, -10px);
    }
    to {
      opacity: 1;
      transform: translate(-50%, 0);
    }
  }

  .p2p-reconnect-toast.success {
    background: #10b981;
    border-color: #059669;
    color: #ffffff;
  }

  .p2p-reconnect-spinner {
    width: 14px;
    height: 14px;
    min-width: 14px;
    min-height: 14px;
    max-width: 14px;
    max-height: 14px;
    flex-shrink: 0;
    margin: 0;
    padding: 0;
    display: inline-block;
    aspect-ratio: 1 / 1;
    box-sizing: border-box;
    border: 2px solid rgba(24, 19, 38, 0.12);
    border-top-color: var(--accent, #7E22CE);
    border-radius: 50%;
    animation: p2p-spin 0.8s linear infinite;
  }

  :global(html.omd-dark) .p2p-reconnect-spinner {
    border: 2px solid rgba(255, 255, 255, 0.22);
    border-top-color: var(--accent-hover, #C084FC);
  }

  @keyframes p2p-spin {
    to { transform: rotate(360deg); }
  }

  .login-spinner {
    width: 15px;
    height: 15px;
    min-width: 15px;
    min-height: 15px;
    border: 2px solid rgba(255, 255, 255, 0.35);
    border-top-color: #ffffff;
    border-radius: 50%;
    animation: loginSpin 0.75s linear infinite;
    display: inline-block;
    flex-shrink: 0;
  }
  .login-spinner.dark {
    border-color: rgba(24, 19, 38, 0.2);
    border-top-color: var(--accent);
  }
  .login-loading-box,
  .login-loading-centered {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    width: 100%;
    text-align: center;
  }
  .login-spinner-circle {
    width: 44px;
    height: 44px;
    min-width: 44px;
    min-height: 44px;
    border: 3.5px solid var(--accent-soft);
    border-top-color: var(--accent);
    border-radius: 50%;
    animation: loginSpin 0.8s linear infinite;
    margin: 0 auto;
  }
  .login-loading-text {
    font-size: 0.95rem;
    color: var(--text-secondary);
    font-weight: 500;
  }
  :global(.dark-theme) .login-spinner-circle,
  :global(.wx-willow-dark-theme) .login-spinner-circle {
    border-color: rgba(255, 255, 255, 0.15);
    border-top-color: var(--accent-hover);
  }
  :global(.dark-theme) .login-loading-text,
  :global(.wx-willow-dark-theme) .login-loading-text {
    color: var(--text-secondary);
  }
  @keyframes loginSpin {
    to { transform: rotate(360deg); }
  }

  /* Universal combobox / select dropdown chevron with proper right-margin and styling */
  :global(select.select-styled),
  :global(.select-styled) {
    appearance: none !important;
    -webkit-appearance: none !important;
    -moz-appearance: none !important;
    padding-right: 36px !important;
    background-repeat: no-repeat !important;
    background-position: calc(100% - 12px) center !important;
    background-size: 14px 14px !important;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%238B80A3' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E") !important;
    cursor: pointer;
    box-sizing: border-box;
  }

  :global(.dark-theme select.select-styled),
  :global(.dark-theme .select-styled),
  :global(.wx-willow-dark-theme select.select-styled),
  :global(.wx-willow-dark-theme .select-styled),
  :global(.onboard-overlay.dark select.select-styled),
  :global(.onboard-overlay.dark .select-styled) {
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%23C4B8DB' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E") !important;
  }

  :global(.light-theme select.select-styled),
  :global(.light-theme .select-styled),
  :global(.onboard-overlay:not(.dark) select.select-styled),
  :global(.onboard-overlay:not(.dark) .select-styled) {
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%235B5270' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E") !important;
  }
</style>
