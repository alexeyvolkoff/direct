<script>
  import { createEventDispatcher, onMount, onDestroy } from "svelte";
  import { locale, _, translate, SUPPORTED_LANGUAGES, setLocale } from "../lib/i18n/index.js";
  import { profileStore } from "../lib/profile-store.js";
  import { deviceRegistry } from "../lib/device-registry.js";
  import { omdKeyStore, storeOmdKey, clearOmdKey, isValidOmdKey, getGatewayBase } from "../lib/omd-key.js";
  import { getDidTag } from "../lib/identity/persistent.js";
  import { isNativeMobilePlatform, signInWithGoogleMobile, isGoogleSignInCanceledError } from "../lib/identity/googleMobileAuth.js";
  import { sshCredsStore, saveSshCredential, clearSshCredential } from "../lib/identity/ssh-credentials.js";
  import { mcpKeysStore, saveMcpAppKey, removeMcpAppKey, PRESET_MCP_APPS, setMcpKeysIdentity } from "../lib/mcp-keys.js";
  import { fetchNearbyDevices } from "../lib/local-discovery.js";
  import { cameraSyncManager, syncState, updateSettings } from "../lib/camera-sync.js";
  import { isNativeStorageAvailable } from "../lib/native-drive.js";
  import { appVersion, otaState, performOtaUpdate, checkForOtaUpdate } from "../lib/ota-updater.js";
  import { isIosNative, isExternalCheckoutUrl } from "../lib/platform.js";
  import FolderPicker from "./FolderPicker.svelte";
  import AssistantAvatarModal from "./AssistantAvatarModal.svelte";
  import { getChatStore } from "../lib/chat-store.js";
  import { HttpDrive } from "../lib/http-drive.js";
  import { aiNodeStore, refreshPublicAiNode, setAiNodeChoice, acceptPublicAiAgreement, applyRemotePublicAiAck, publicAiAckPayload, AI_NODE_MODE_AUTO, AI_NODE_MODE_PUBLIC, AI_NODE_MODE_CUSTOM, AI_MODE_LOCAL, AI_MODE_HYBRID, AI_MODE_AGENTIC, setAiMode, setInferenceNode, addCustomAiNode, removeCustomAiNode, mergeCustomAiNodes, connectGuestAiNode } from "../lib/ai-node.js";
  import { get as getStoreValue } from "svelte/store";
  import { parseStoragePath, formatStoragePath, persistStoragePath, DEFAULT_STORAGE_PATH, getLocalDevice } from "../lib/storage-path.js";
  import { DEFAULT_JUNE_SYSTEM_PROMPT, DEFAULT_JUNE_FUN_PROMPT, blobToDataUrl } from "../lib/assistant-defaults.js";
  import {
    isNativeApp,
    isWebPushSupported,
    getBrowserPushSubscription,
    subscribeToNodeWebPush,
    unsubscribeFromNodeWebPush,
    sendTestNodeWebPush
  } from "../lib/push-notifications.js";

  export let identity = null;
  export let isDarkTheme = true;
  export let targetLinkId = "";
  export let port = 4000;
  export let httpDrive = null;

  const dispatch = createEventDispatcher();

  export let activeSection = "profile"; // "profile" | "devices" | "network" | "appearance" | "assistant" | "security"

  let isMobile = typeof window !== "undefined" ? window.innerWidth <= 768 : false;
  let showSidebar = !isMobile;
  let hasCameraSupport = false;

  function updateMobileState() {
    isMobile = window.innerWidth <= 768;
    if (typeof navigator !== "undefined" && navigator.mediaDevices && typeof navigator.mediaDevices.getUserMedia === "function") {
      hasCameraSupport = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent) ||
                         (isMobile && 'ontouchstart' in window);
    }
  }

  onMount(() => {
    updateMobileState();
    if (isMobile) {
      showSidebar = false;
    }
    window.addEventListener("resize", updateMobileState);
  });

  onDestroy(() => {
    if (typeof window !== "undefined") {
      window.removeEventListener("resize", updateMobileState);
    }
    if (assistantAvatarUrl && assistantAvatarUrl.startsWith("blob:")) {
      try { URL.revokeObjectURL(assistantAvatarUrl); } catch (_) {}
    }
  });

  function selectSection(secId) {
    activeSection = secId;
    if (isMobile) {
      showSidebar = false;
    }
    if (secId === "camera") {
      cameraSyncManager.scanCameraRoll();
    }
  }
  let copiedField = "";
  let newLinkId = "";
  let connectingDevice = false;
  let connectError = "";

  let omdKeyInput = "";
  let showOmdKey = false;
  let omdKeyMessage = "";
  let omdKeyMessageType = "info";
  $: isOmdKeyInputValid = isValidOmdKey(omdKeyInput);
  $: canSaveOmdKey = isOmdKeyInputValid;
  $: if ($omdKeyStore.valid && !omdKeyInput) {
    omdKeyInput = $omdKeyStore.key;
  }

  async function saveOmdKey() {
    try {
      const normalized = storeOmdKey(omdKeyInput);
      omdKeyInput = normalized;
      omdKeyMessage = identity && identity.type === "persistent"
        ? "Key saved to your encrypted DID graph."
        : "Key saved for this browser session. Sign in with your ID to store it in your DID graph.";
      omdKeyMessageType = "success";

      // Also sync to server profile/database so payment/console accounts update
      if (typeof window !== "undefined") {
        try {
          const user = identity?.profile?.user || $profileStore?.user || "";
          if (user && user !== "anonymous") {
            const base = getGatewayBase();
            await fetch(`${base}/rest`, {
              method: "POST",
              headers: {
                "Content-Type": "application/json",
                "authorization": `token:${normalized}`,
              },
              body: JSON.stringify({
                action: "updateProfileData",
                profile: { user, omd_key: normalized },
              }),
            });
          }
        } catch (serverErr) {
          console.warn("[settings] server profile update notice:", serverErr);
        }
      }
    } catch (e) {
      omdKeyMessage = e.message || "Failed to save key";
      omdKeyMessageType = "error";
    }
  }

  function removeOmdKey() {
    clearOmdKey();
    omdKeyInput = "";
    omdKeyMessage = "Key removed from your DID graph and this browser.";
    omdKeyMessageType = "info";
  }

  // SSH quick-login vault (per-device system credentials)
  let sshEditId = "";
  let sshUserInput = "";
  let sshPassInput = "";
  let sshBusy = false;
  $: sshByDevice = ($sshCredsStore && $sshCredsStore.byDevice) || {};

  function startSshEdit(dev) {
    sshEditId = dev.linkId;
    const saved = sshByDevice[dev.linkId];
    sshUserInput = saved ? saved.user : "";
    sshPassInput = saved ? saved.pass : "";
  }

  function cancelSshEdit() {
    sshEditId = "";
    sshUserInput = "";
    sshPassInput = "";
  }

  async function submitSshEdit() {
    if (!sshEditId || !sshUserInput.trim()) return;
    sshBusy = true;
    try {
      await saveSshCredential(sshEditId, sshUserInput.trim(), sshPassInput);
      cancelSshEdit();
    } finally {
      sshBusy = false;
    }
  }

  async function removeSshCredential(linkId) {
    await clearSshCredential(linkId);
    if (sshEditId === linkId) cancelSshEdit();
  }

  // MCP API Keys vault (Club Pass, CivitAI, Plane, custom integrations)
  $: if (identity) {
    setMcpKeysIdentity(identity);
  }
  const isIosApp = isIosNative();
  let mcpSelectedPreset = isIosApp ? "civitai" : "club_pass"; // "club_pass" | "civitai" | "plane" | "custom"
  let mcpCustomName = "";
  let mcpCustomUrl = "";
  let mcpKeyInput = "";
  let showMcpAddModal = false;
  let showMcpKeyToggles = {}; // appId -> boolean
  let mcpEditingKeys = {}; // appId -> string
  let mcpBusy = false;
  let mcpMessage = "";
  let mcpMessageType = "info";

  $: mcpAppsMap = ($mcpKeysStore && $mcpKeysStore.byApp) || {};
  $: mcpAppsList = Object.values(mcpAppsMap).filter(
    (app) => !(isIosApp && (app.appId === "club_pass" || app.appId === "whop" || isExternalCheckoutUrl(app.url)))
  );

  function toggleMcpKeyVisibility(appId) {
    showMcpKeyToggles = { ...showMcpKeyToggles, [appId]: !showMcpKeyToggles[appId] };
  }

  function getMcpInputVal(app) {
    if (mcpEditingKeys[app.appId] !== undefined) return mcpEditingKeys[app.appId];
    return app.key || "";
  }

  function setMcpInputVal(appId, val) {
    mcpEditingKeys = { ...mcpEditingKeys, [appId]: val };
  }

  async function handleSaveMcpApp(app) {
    const val = (mcpEditingKeys[app.appId] !== undefined ? mcpEditingKeys[app.appId] : app.key || "").trim();
    if (!val) return;
    mcpBusy = true;
    try {
      await saveMcpAppKey(app.appId, {
        name: app.name,
        url: app.url,
        key: val,
      });
      mcpMessage = $_("key_saved_in_did", {}, "Key saved in DID graph");
      mcpMessageType = "success";
      setTimeout(() => { if (mcpMessage) mcpMessage = ""; }, 4000);
    } catch (err) {
      mcpMessage = String(err.message || err);
      mcpMessageType = "error";
    } finally {
      mcpBusy = false;
    }
  }

  async function handleRemoveMcpApp(appId) {
    mcpBusy = true;
    try {
      await removeMcpAppKey(appId);
      const copy = { ...mcpEditingKeys };
      delete copy[appId];
      mcpEditingKeys = copy;
      mcpMessage = $_("key_removed_from_did", {}, "Key removed from DID graph");
      mcpMessageType = "info";
      setTimeout(() => { if (mcpMessage) mcpMessage = ""; }, 4000);
    } catch (err) {
      mcpMessage = String(err.message || err);
      mcpMessageType = "error";
    } finally {
      mcpBusy = false;
    }
  }

  function openAddMcpModal(presetId = "plane") {
    mcpSelectedPreset = presetId;
    const preset = PRESET_MCP_APPS.find(p => p.id === presetId);
    if (preset) {
      if (presetId === "civitai" && mcpAppsMap["civitai"]) {
        mcpCustomName = "";
      } else {
        mcpCustomName = preset.name;
      }
      mcpCustomUrl = preset.url;
    } else {
      mcpCustomName = "";
      mcpCustomUrl = "";
    }
    mcpKeyInput = "";
    showMcpAddModal = true;
  }

  function closeAddMcpModal() {
    showMcpAddModal = false;
    mcpKeyInput = "";
    mcpCustomName = "";
    mcpCustomUrl = "";
  }

  async function submitAddMcpKey() {
    let appId = mcpSelectedPreset;
    let name = mcpCustomName.trim();
    let url = mcpCustomUrl.trim();
    const key = mcpKeyInput.trim();

    if (mcpSelectedPreset !== "custom") {
      const preset = PRESET_MCP_APPS.find(p => p.id === mcpSelectedPreset);
      if (preset) {
        url = preset.url;
        if (mcpSelectedPreset === "civitai" && mcpAppsMap["civitai"]) {
          const slug = name.toLowerCase().replace(/^@/, "").replace(/[^a-z0-9_-]/g, "_").replace(/^civitai_+/, "");
          if (slug) {
            appId = `civitai_${slug}`;
            name = name.toLowerCase().includes("civitai") ? name : `CivitAI (${name})`;
          } else {
            let idx = 2;
            while (mcpAppsMap[`civitai_${idx}`]) idx++;
            appId = `civitai_${idx}`;
            name = `CivitAI #${idx}`;
          }
        } else {
          name = name || preset.name;
        }
      }
    } else {
      if (!name) return;
      appId = name.toLowerCase().replace(/[^a-z0-9_-]/g, "_");
    }

    if (!key) return;

    mcpBusy = true;
    try {
      await saveMcpAppKey(appId, { name, url, key });
      closeAddMcpModal();
      mcpMessage = $_("key_saved_in_did", {}, "Key saved in DID graph");
      mcpMessageType = "success";
      setTimeout(() => { if (mcpMessage) mcpMessage = ""; }, 4000);
    } catch (err) {
      mcpMessage = String(err.message || err);
      mcpMessageType = "error";
    } finally {
      mcpBusy = false;
    }
  }

  // Auth upgrade form state for ephemeral mode
  let authMode = "google"; // "google" | "password" | "register"
  let signinUser = "";
  let signinPass = "";
  let regUser = "";
  let regEmail = "";
  let regPass = "";
  let regPassConfirm = "";
  let regAvatar = "";
  let authLoading = false;
  let authError = "";

  function handleRegAvatarChange(e) {
    const file = e.target.files?.[0];
    if (!file) return;
    if (file.size > 1024 * 1024 * 2) {
      authError = "Avatar image must be under 2MB";
      return;
    }
    const reader = new FileReader();
    reader.onload = () => {
      regAvatar = reader.result;
    };
    reader.readAsDataURL(file);
  }

  // Profile editing state
  let editDisplayName = "";
  let editUsername = "";
  let editAvatar = "";
  let savingProfile = false;
  let profileSaveMsg = "";
  let profileSaveError = "";

  // Image load error flags (for graceful fallback)
  let profileImgFailed = false;
  let editAvatarFailed = false;
  let regAvatarFailed = false;

  $: if ($profileStore.image) {
    profileImgFailed = false;
  }
  $: if (editAvatar) {
    editAvatarFailed = false;
  }
  $: if (regAvatar) {
    regAvatarFailed = false;
  }

  // Email change state
  let showChangeEmail = false;
  let newEmailInput = "";
  let requestingEmailChange = false;
  let emailChangeMsg = "";
  let emailChangeError = "";

  $: if ($profileStore && !savingProfile) {
    if (!editDisplayName && $profileStore.name && $profileStore.name !== 'Guest User') {
      editDisplayName = $profileStore.name;
    }
    if (!editUsername && $profileStore.user && $profileStore.user !== 'anonymous') {
      editUsername = $profileStore.user;
    }
    if (!editAvatar && $profileStore.image) {
      editAvatar = $profileStore.image;
    }
  }

  async function handleSaveProfile() {
    savingProfile = true;
    profileSaveMsg = "";
    profileSaveError = "";
    try {
      if (identity && identity.updateProfile) {
        await identity.updateProfile({
          name: editDisplayName.trim(),
          user: editUsername.trim(),
          image: editAvatar,
        });
      }
      profileSaveMsg = "Profile saved successfully!";
      setTimeout(() => { profileSaveMsg = ""; }, 3000);
    } catch (e) {
      profileSaveError = e.message || "Failed to save profile";
    } finally {
      savingProfile = false;
    }
  }

  async function handleRequestEmailChange() {
    if (!newEmailInput || !newEmailInput.includes("@")) {
      emailChangeError = "Please enter a valid email address";
      return;
    }
    requestingEmailChange = true;
    emailChangeMsg = "";
    emailChangeError = "";
    try {
      if (identity && identity.requestEmailChange) {
        const res = await identity.requestEmailChange(newEmailInput.trim());
        emailChangeMsg = res.message || `Verification link sent to ${newEmailInput}.`;
      } else {
        emailChangeMsg = `Verification link sent to ${newEmailInput}. Please check your inbox.`;
      }
    } catch (e) {
      emailChangeError = e.message || "Failed to request email change";
    } finally {
      requestingEmailChange = false;
    }
  }

  function handleAvatarFileChange(e) {
    const file = e.target.files?.[0];
    if (!file) return;
    if (file.size > 1024 * 1024 * 2) {
      profileSaveError = "Avatar image must be under 2MB";
      return;
    }
    const reader = new FileReader();
    reader.onload = () => {
      editAvatar = reader.result;
    };
    reader.readAsDataURL(file);
  }

  // Viewer settings — app-wide (replaces per-device View section)
  // generateIconsFromVideos is always on when thumbnails are supported
  let viewerSettings = {
    generateThumbs: true,
    downscaleImages: true,
    scaleTo: 1200,
  };
  const viewerScaleOptions = [960, 1200, 1600];
  function loadViewerSettings() {
    try {
      const raw = typeof localStorage !== "undefined" ? localStorage.getItem("omd_viewer_settings") : null;
      if (raw) {
        const parsed = JSON.parse(raw);
        // migration: Video Icons is now always on, drop legacy key
        if ("generateIconsFromVideos" in parsed) delete parsed.generateIconsFromVideos;
        viewerSettings = { ...viewerSettings, ...parsed };
      }
    } catch (_) {}
  }
  function saveViewerSettings() {
    try { localStorage.setItem("omd_viewer_settings", JSON.stringify(viewerSettings)); } catch (_) {}
    try { window.dispatchEvent(new CustomEvent("omd-viewer-settings", { detail: { ...viewerSettings } })); } catch (_) {}
  }

  // Device list from registry + identity
  let devices = [];
  let deviceToDelete = null;
  let detectedDevices = [];
  let scanningDetected = false;

  function refreshDevices() {
    // AI pseudo nodes (e.g. AI Assistant Public Node, guest AI nodes) are not devices:
    // never listed, never counted as sync/storage destinations.
    const publicLinkId = $aiNodeStore?.info?.linkID;
    const pubName = $_("public_ai_node_name", {}, "AI Assistant Public Node");
    const customLinkIds = new Set(($aiNodeStore?.customNodes || []).map(c => c.linkId));
    devices = deviceRegistry.list().filter(d =>
      !d.isAiNode &&
      d.type !== "ai" &&
      d.type !== "ai-node" &&
      !customLinkIds.has(d.linkId) &&
      !customLinkIds.has(d.deviceId) &&
      (!publicLinkId || (d.linkId !== publicLinkId && d.deviceId !== publicLinkId)) &&
      d.name !== pubName &&
      d.name !== "AI Assistant Public Node" && d.name !== "AI Assistant Public Node"
    );
  }

  async function refreshDetected() {
    scanningDetected = true;
    try {
      const list = await fetchNearbyDevices();
      const activeIds = new Set();
      for (const d of (devices || [])) {
        if (d.linkId) activeIds.add(d.linkId);
        if (d.peerId) activeIds.add(d.peerId);
        if (d.shortId) activeIds.add(d.shortId);
        if (d.deviceId) activeIds.add(d.deviceId);
      }
      detectedDevices = (list || []).filter(d => d.linkId && !activeIds.has(d.linkId) && !deviceRegistry.has(d.linkId));
    } catch (e) {
      console.warn("[Settings] scan detected devices error:", e);
    } finally {
      scanningDetected = false;
    }
  }

  function handleConnectDetected(dev) {
    if (!dev || !dev.linkId) return;
    dispatch("connectDevice", { linkId: dev.linkId, name: dev.name });
  }

  function handleConfirmDeleteDevice() {
    if (!deviceToDelete) return;
    const linkId = deviceToDelete.linkId;
    deviceToDelete = null;
    devices = devices.filter(d => d.linkId !== linkId && d.deviceId !== linkId);
    dispatch("disconnectDevice", { linkId });
    setTimeout(() => {
      refreshDevices();
      refreshDetected();
    }, 200);
  }

  let showDeleteAccountModal = false;
  let deletingAccount = false;
  let deleteAccountPassword = "";
  $: deleteAccountNeedsPassword = !(identity && identity.deleteToken);

  async function handleConfirmDeleteAccount() {
    if (deletingAccount) return;
    if (deleteAccountNeedsPassword && !deleteAccountPassword) return;
    deletingAccount = true;
    try {
      showDeleteAccountModal = false;
      dispatch("delete-account", { password: deleteAccountPassword });
      deleteAccountPassword = "";
    } finally {
      deletingAccount = false;
    }
  }

  let prevSection = activeSection;
  $: if (activeSection === "devices" && prevSection !== "devices") {
    prevSection = activeSection;
    refreshDetected();
  } else if (activeSection !== "devices") {
    prevSection = activeSection;
  }

  onMount(() => {
    loadViewerSettings();
    refreshDevices();
    if (activeSection === "devices") {
      refreshDetected();
    }
    const interval = setInterval(() => {
      refreshDevices();
    }, 3000);
    return () => {
      clearInterval(interval);
    };
  });

  function copyToClipboard(text, fieldName) {
    if (!text) return;
    navigator.clipboard.writeText(text);
    copiedField = fieldName;
    setTimeout(() => {
      if (copiedField === fieldName) copiedField = "";
    }, 2000);
  }

  function handleConnectNewDevice() {
    if (!newLinkId.trim()) return;
    connectingDevice = true;
    connectError = "";
    dispatch("connectDevice", { linkId: newLinkId.trim() });
    setTimeout(() => {
      newLinkId = "";
      connectingDevice = false;
      refreshDevices();
      refreshDetected();
    }, 1500);
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

  async function handleSettingsMobileGoogle() {
    authLoading = true;
    authError = "";
    try {
      const idToken = await signInWithGoogleMobile();
      dispatch("googleLogin", { credential: idToken });
    } catch (e) {
      authLoading = false;
      if (!isGoogleSignInCanceledError(e)) {
        authError = e?.message || "Google sign-in failed";
      }
    }
  }

  let gsiInitialized = false;
  function googleButtonAction(node) {
    if (isNativeMobilePlatform()) {
      return { destroy() {} };
    }
    let active = true;

    loadGoogleGsi().then(() => {
      if (!active || !window.google?.accounts?.id) return;
      if (!gsiInitialized) {
        window.google.accounts.id.initialize({
          client_id: "959849209352-vbi0vfimrqj904598felj8ieeb6to8s2.apps.googleusercontent.com",
          callback: (resp) => {
            if (resp?.credential) {
              authLoading = true;
              dispatch("googleLogin", { credential: resp.credential });
            }
          },
          auto_select: false,
        });
        gsiInitialized = true;
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

  let settingsKeyfileInput = null;
  let keyfileExportMsg = "";
  let keyfileExportErr = "";

  $: if ($profileStore.isPersistent && authLoading) {
    authLoading = false;
  }

  function handleExportDidKeyfile() {
    keyfileExportMsg = "";
    keyfileExportErr = "";
    try {
      if (identity && typeof identity.downloadKeyfile === "function") {
        identity.downloadKeyfile();
        keyfileExportMsg = $_("keyfile_downloaded_toast", {}, "DID keyfile (.json) saved");
        setTimeout(() => { keyfileExportMsg = ""; }, 3500);
      } else {
        keyfileExportErr = "No active persistent DID session";
      }
    } catch (e) {
      keyfileExportErr = e?.message || "Failed to export DID keyfile";
    }
  }

  async function handleImportKeyfileChange(e) {
    const file = e?.target?.files?.[0];
    if (e?.target) e.target.value = "";
    if (!file) return;
    authLoading = true;
    authError = "";
    keyfileExportErr = "";
    try {
      const content = await file.text();
      dispatch("keyfileLogin", { content });
    } catch (err) {
      authLoading = false;
      authError = err?.message || "Could not read keyfile";
      keyfileExportErr = authError;
    }
  }

  function handlePasswordSignin() {
    if (!signinUser || !signinPass) {
      authError = "Enter username and password";
      return;
    }
    authLoading = true;
    authError = "";
    dispatch("passwordLogin", { user: signinUser, pass: signinPass });
  }

  function handleRegisterNew() {
    if (!regUser || !regPass || !regPassConfirm) {
      authError = "All fields are required";
      return;
    }
    if (regPass.length < 8) {
      authError = "Password must be at least 8 characters";
      return;
    }
    if (regPass !== regPassConfirm) {
      authError = "Passwords do not match";
      return;
    }
    authLoading = true;
    authError = "";
    dispatch("register", { user: regUser, pass: regPass, email: regEmail || null, image: regAvatar });
  }

  // Own remote storage nodes; the Folder & File Backup section is only
  // relevant once the user has more than one own node.
  $: ownDeviceCount = (devices || []).filter(d => !d.isLocal && !d.isAiNode).length;

  $: sections = [
    {
      id: "profile",
      label: $_("settings_profile", {}, "Profile & Identity"),
      icon: `<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>`,
    },
    {
      id: "devices",
      label: $_("settings_devices", {}, "Devices & Nodes"),
      icon: `<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="3" width="20" height="14" rx="2" ry="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>`,
    },
    ...(ownDeviceCount >= 2 ? [{
      id: "sync",
      label: $syncState.isNative ? $_("settings_camera_sync", {}, "Photo & Backup Sync") : $_("pwa_folder_sync_title", {}, "Folder & File Backup"),
      icon: `<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>`,
    }] : []),
    {
      id: "network",
      label: $_("settings_network", {}, "Network & P2P"),
      icon: `<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="2" y1="12" x2="22" y2="12"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/></svg>`,
    },
    {
      id: "appearance",
      label: $_("settings_appearance", {}, "Appearance"),
      icon: `<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="5"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>`,
    },
    {
      id: "assistant",
      label: $_("ai_assistant", {}, "AI Assistant"),
      icon: `<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg>`,
    },
    {
      id: "security",
      label: $_("settings_security", {}, "Security & Keys"),
      icon: `<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>`,
    },
  ];

  let selectedSyncTarget = $syncState.targetLinkId || "";
  let customSyncFolder = $syncState.targetFolder || "";
  let selectedTimeRange = $syncState.timeRange || "all";
  let syncSources = {
    photos: $syncState.sources?.photos !== false,
    videos: $syncState.sources?.videos !== false,
    screenshots: !!$syncState.sources?.screenshots,
    documents: !!$syncState.sources?.documents,
  };
  let showSyncFolderPicker = false;

  function isDeviceOnline(dev) {
    if (!dev) return false;
    if (dev.status === "connected" || dev.status === "online") return true;
    if (dev.drive && (dev.drive.isConnected || dev.drive.dc?.readyState === "open")) return true;
    return dev.online === true;
  }

  function handleFilterChange() {
    updateSettings({ timeRange: selectedTimeRange, sources: syncSources });
    cameraSyncManager.scanCameraRoll({ timeRange: selectedTimeRange, sources: syncSources });
  }

  // Automatically select first online desktop device as sync target if none selected
  $: if (!selectedSyncTarget && devices && devices.length > 0) {
    // Sync destination is storage only: AI pseudo nodes (public node) are
    // never offered as backup destinations.
    const onlineDev = devices.find(d => isDeviceOnline(d) && !d.isLocal && !d.isAiNode);
    if (onlineDev) {
      selectedSyncTarget = onlineDev.linkId;
    } else {
      const anyRemote = devices.find(d => !d.isLocal && !d.isAiNode);
      if (anyRemote) selectedSyncTarget = anyRemote.linkId;
    }
  }

  $: if (selectedSyncTarget && selectedSyncTarget !== $syncState.targetLinkId) {
    updateSettings({ targetLinkId: selectedSyncTarget });
  }

  $: if (customSyncFolder !== undefined && customSyncFolder !== $syncState.targetFolder) {
    updateSettings({ targetFolder: customSyncFolder });
  }

  async function handleStartCameraSync() {
    try {
      await cameraSyncManager.startSync(selectedSyncTarget, customSyncFolder);
    } catch (e) {
      alert(e.message || String(e));
    }
  }

  async function handleSelectLocalPwaFolder() {
    try {
      await cameraSyncManager.selectPwaFolder();
    } catch (e) {
      if (e && e.name !== "AbortError") {
        alert(e.message || String(e));
      }
    }
  }

  function handleStopCameraSync() {
    cameraSyncManager.stopSync();
  }

  function handleScanMedia() {
    cameraSyncManager.scanCameraRoll({ timeRange: selectedTimeRange, sources: syncSources });
  }

  // AI Assistant Section State
  let assistantName = "June";
  let assistantTitle = "Junior assistant";
  let assistantPersonality = "";
  let assistantFunPrompt = "";
  let promptMode = "work"; // "work" | "fun"
  let assistantUserName = "";
  let assistantStorage =
    (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
    $profileStore?.defaultStorage ||
    identity?.profile?.defaultStorage ||
    "";
  let assistantAvatarUrl = "";
  let assistantStyle = "realistic";
  let assistantModel = "Domi";
  let assistantAppearance = "";
  let assistantSaving = false;
  let assistantSaveMessage = "";
  let assistantSaveType = "success";
  let showAvatarModal = false;
  let showAssistantFolderPicker = false;
  let showStorageSourceChooser = false;
  let storagePickerDevice = null;
  let storageSources = [];
  let assistantLoaded = false;
  let characterFileInput = null;
  let isImportingCharacter = false;
  let assistantFirstMessage = "";

  $: currentAiNodeDevice = (targetLinkId
    ? (deviceRegistry.get(targetLinkId) || deviceRegistry.getByPrefix(targetLinkId) || deviceRegistry.getByName(targetLinkId))
    : null)
    || (devices && devices.find(d => !d.isLocal && !d.isAiNode && (d.isOwner === true || (identity?.devices && (identity.devices.has?.(d.linkId) || identity.devices.get?.(d.linkId))))))
    || null;
  $: isNodeOwner = !isSpecifyingCustomNode &&
    aiNodeChoice !== AI_NODE_MODE_PUBLIC &&
    aiNodeChoice !== AI_NODE_MODE_CUSTOM &&
    !isCurrentCustomNode &&
    ownChatDevices.length > 0 &&
    (aiNodeChoice === AI_NODE_MODE_AUTO || ownChatDevices.some((d) => (d.linkId || d.deviceId) === aiNodeChoice)) &&
    ((currentAiNodeDevice && !currentAiNodeDevice.isAiNode && (
      currentAiNodeDevice.isOwner === true ||
      Boolean(identity?.devices && (
        (typeof identity.devices.has === "function" && identity.devices.has(currentAiNodeDevice.linkId)) ||
        (typeof identity.devices.get === "function" && Boolean(identity.devices.get(currentAiNodeDevice.linkId)))
      ))
    )) || (identity?.type === "persistent" && ownChatDevices.length > 0));

  $: parsedStorageInfo = (() => {
    const res = parseStoragePath(assistantStorage, currentAiNodeDevice || getLocalDevice());
    if (res.isDeviceQualified && res.deviceName) {
      return {
        label: res.device?.local ? $_("this_device", {}, "This device") : (res.device?.name || res.deviceName),
        folder: res.folder
      };
    }
    if (res.device) {
      return {
        label: res.device.local ? $_("this_device", {}, "This device") : (res.device.name || res.deviceName),
        folder: res.folder
      };
    }
    if (isNodeOwner) {
      return {
        label: $_("this_ai_node", {}, "This AI Node"),
        folder: res.folder
      };
    }
    return {
      label: $_("this_device", {}, "This device"),
      folder: res.folder
    };
  })();

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
    const allDevs = [...(devices || []), ...deviceRegistry.list()];
    for (const d of allDevs) {
      if (d.local === true || d.isAiNode === true) continue;
      if (d.type === "ai" || d.type === "ai-node") continue;
      const pubName = $_("public_ai_node_name", {}, "AI Assistant Public Node");
      if (d.name === pubName || d.name === "AI Assistant Public Node" || d.name === "AI Assistant Public Node") continue;
      push(d);
    }
    const local = getLocalDevice() || devices.find(d => d.local) || deviceRegistry.list().find(d => d.local);
    if (local) push(local);
    return out;
  }

  function handleBrowseStorage() {
    refreshDevices();
    const sources = buildStorageSources();
    storageSources = sources;
    if (sources.length > 0) {
      showStorageSourceChooser = true;
    } else {
      storagePickerDevice = currentAiNodeDevice || getLocalDevice() || null;
      showAssistantFolderPicker = true;
    }
  }

  function selectStorageSource(src) {
    storagePickerDevice = src;
    showStorageSourceChooser = false;
    showAssistantFolderPicker = true;
  }

  // AI Node selection: own chat-capable device (linkId) or the public node
  let aiNodeChoice = (typeof localStorage !== "undefined" && localStorage.getItem("omd_ai_node_choice")) || $aiNodeStore?.choice || AI_NODE_MODE_AUTO;
  // Agentic runtime mode + remote inference node (persisted in GunDB assistant settings)
  let aiMode = (typeof localStorage !== "undefined" && localStorage.getItem("omd_ai_mode")) || $aiNodeStore?.aiMode || AI_MODE_LOCAL;
  let inferenceNodeId = (typeof localStorage !== "undefined" && localStorage.getItem("omd_ai_infer_node")) || $aiNodeStore?.inferenceNodeId || "";
  let publicAiAgreementOpen = false;
  let pendingAiNodeChoice = aiNodeChoice;
  let aiNodeStatusMsg = "";

  const POPULAR_HORDE_MODELS = [
    { id: "aphrodite/TheDrummer/Behemoth-X-123B-v2.1", label: "Behemoth-X-123B-v2.1 (Top RP / 123B)" },
    { id: "aphrodite/TheDrummer/Skyfall-31B-v4.2", label: "Skyfall-31B-v4.2 (Creative RP / 31B)" },
    { id: "koboldcpp/L3-8B-Stheno-v3.2", label: "L3-8B-Stheno-v3.2 (Fast 8B RP)" },
    { id: "koboldcpp/Magidonia-24B-v4.3", label: "Magidonia-24B-v4.3 (Creative RP / 24B)" },
    { id: "koboldcpp/mini-magnum-12b-v1.1", label: "Mini-Magnum-12B-v1.1 (12B RP)" },
    { id: "koboldcpp/Rocinante-X-12B", label: "Rocinante-X-12B (12B RP)" },
    { id: "aphrodite/TheDrummer/Behemoth-128B-v3-NVFP4", label: "Behemoth-128B-v3-NVFP4 (128B)" },
  ];

  let hordeModels = [...POPULAR_HORDE_MODELS];
  let loadingHordeModels = false;
  let hordeApiKey = (typeof localStorage !== "undefined" && localStorage.getItem("omd_horde_api_key")) || "0000000000";
  let hordeModel = (typeof localStorage !== "undefined" && localStorage.getItem("omd_horde_model")) || "aphrodite/TheDrummer/Behemoth-X-123B-v2.1";
  let selectedHordeModel = hordeModel;
  let isCustomHordeModel = false;
  let hordeKeyRevealed = false;
  let hordeKeyCopied = false;

  let isSpecifyingCustomNode = false;
  let customNodeInput = "";
  let connectingCustomNode = false;
  let customNodeError = "";
  let pendingCustomNodeLinkId = "";

  $: customAiNodes = $aiNodeStore?.customNodes || [];
  $: isCurrentCustomNode = customAiNodes.some((c) => c.linkId === aiNodeChoice);

  $: ownChatDevices = (devices || []).filter((d) => {
    if (d.isLocal || d.isAiNode) return false;
    if (d.type === "ai" || d.type === "ai-node") return false;
    const customLinkIds = new Set(customAiNodes.map((c) => c.linkId));
    if (customLinkIds.has(d.linkId) || customLinkIds.has(d.deviceId)) return false;
    const publicLinkId = $aiNodeStore?.info?.linkID;
    if (publicLinkId && (d.linkId === publicLinkId || d.deviceId === publicLinkId)) return false;
    const pubName = $_("public_ai_node_name", {}, "AI Assistant Public Node");
    if (d.name === pubName || d.name === "AI Assistant Public Node" || d.name === "AI Assistant Public Node") return false;
    if (Array.isArray(d.capabilities) && d.capabilities.includes("chat")) return true;
    if (!d.isLocal && (d.type === "node" || d.deviceType === "node" || !d.type)) return true;
    return false;
  });

  $: aiNodeDeviceOptions = ownChatDevices.map((d) => ({
    value: d.linkId || d.deviceId,
    label: d.name || d.linkId
  }));

  $: aiNodeSelectValue = isSpecifyingCustomNode ? "custom" : (aiNodeChoice || AI_NODE_MODE_AUTO);

  async function persistAiNodeChoice() {
    try {
      saveAssistantConfig();
    } catch (_) {
      // saveAssistantConfig sets its own error message; keep going
    }
  }

  async function onAiNodeChange(event) {
    let choice = event && event.target ? event.target.value : AI_NODE_MODE_AUTO;
    if (choice === "custom") {
      isSpecifyingCustomNode = true;
      customNodeInput = "";
      customNodeError = "";
      return;
    }
    isSpecifyingCustomNode = false;
    const st = getStoreValue(aiNodeStore);
    if (choice === AI_NODE_MODE_PUBLIC && !st.ack) {
      // Agreement first: hold the previous selection until accepted
      pendingAiNodeChoice = choice;
      publicAiAgreementOpen = true;
      aiNodeChoice = st.choice || AI_NODE_MODE_AUTO; // revert combо until agreed
      return;
    }
    aiNodeChoice = choice;
    setAiNodeChoice(choice);
    await persistAiNodeChoice();
    aiNodeStatusMsg = choice === AI_NODE_MODE_PUBLIC
      ? $_("ai_node_public_selected", {}, "Connected to AI Assistant Public Node.")
      : "";
    setTimeout(() => { aiNodeStatusMsg = ""; }, 3000);
  }

  async function handleConnectCustomNode() {
    const linkId = (customNodeInput || "").trim();
    if (!linkId) return;
    customNodeError = "";
    const st = getStoreValue(aiNodeStore);
    if (!st.ack) {
      pendingCustomNodeLinkId = linkId;
      publicAiAgreementOpen = true;
      return;
    }
    await executeConnectCustomNode(linkId);
  }

  async function executeConnectCustomNode(linkId) {
    connectingCustomNode = true;
    customNodeError = "";
    try {
      const didPub = identity?.mainPub || getStoredOmdKey() || "";
      const res = await connectGuestAiNode(linkId, didPub);
      addCustomAiNode({ linkId: res.linkId, name: res.name });
      isSpecifyingCustomNode = false;
      customNodeInput = "";
      aiNodeChoice = res.linkId;
      setAiNodeChoice(res.linkId);
      await persistAiNodeChoice();
      aiNodeStatusMsg = $_("custom_ai_node_connected", { name: res.name }, `Connected to guest AI node: ${res.name}.`);
      setTimeout(() => { aiNodeStatusMsg = ""; }, 4000);
    } catch (err) {
      console.error("[Settings] connectGuestAiNode error:", err);
      customNodeError = err.message || $_("failed_to_connect", {}, "Failed to connect to node");
    } finally {
      connectingCustomNode = false;
    }
  }

  async function handleForgetCustomNode(linkId) {
    removeCustomAiNode(linkId);
    aiNodeChoice = AI_NODE_MODE_AUTO;
    setAiNodeChoice(AI_NODE_MODE_AUTO);
    await persistAiNodeChoice();
    aiNodeStatusMsg = $_("custom_node_removed", {}, "Guest AI node removed.");
    setTimeout(() => { aiNodeStatusMsg = ""; }, 3000);
  }

  async function onPublicAiAgree() {
    acceptPublicAiAgreement();
    publicAiAgreementOpen = false;
    if (pendingCustomNodeLinkId) {
      const linkId = pendingCustomNodeLinkId;
      pendingCustomNodeLinkId = "";
      await executeConnectCustomNode(linkId);
      return;
    }
    aiNodeChoice = AI_NODE_MODE_PUBLIC;
    setAiNodeChoice(AI_NODE_MODE_PUBLIC);
    await persistAiNodeChoice();
    aiNodeStatusMsg = $_("ai_node_public_selected", {}, "Connected to AI Assistant Public Node.");
    setTimeout(() => { aiNodeStatusMsg = ""; }, 3000);
  }

  function onPublicAiDecline() {
    publicAiAgreementOpen = false;
    pendingAiNodeChoice = AI_NODE_MODE_AUTO;
    pendingCustomNodeLinkId = "";
    isSpecifyingCustomNode = false;
  }

  async function onAiModeChange(event) {
    const mode = event && event.target ? event.target.value : AI_MODE_LOCAL;
    aiMode = mode;
    setAiMode(mode);
    if (mode === AI_MODE_HYBRID) {
      if (hordeModels.length <= POPULAR_HORDE_MODELS.length) {
        fetchHordeModels();
      }
      loadNodeAiSettings();
    }
    await syncNodeAiSettings(mode);
    await persistAiNodeChoice();
    aiNodeStatusMsg = mode === AI_MODE_AGENTIC
      ? $_("ai_mode_agentic_selected", {}, "Agentic mode: knowledge indexing stays local, inference runs on the selected node.")
      : (mode === AI_MODE_HYBRID
        ? $_("ai_mode_hybrid_selected", {}, "Hybrid mode: local inference for work, AI Horde for roleplay/fun.")
        : "");
    setTimeout(() => { aiNodeStatusMsg = ""; }, 3000);
  }

  async function onInferenceNodeChange(event) {
    const id = event && event.target ? event.target.value : "";
    inferenceNodeId = id;
    setInferenceNode(id);
    await persistAiNodeChoice();
    aiNodeStatusMsg = id
      ? $_("ai_infer_node_selected", {}, "Inference node selected.")
      : "";
    setTimeout(() => { aiNodeStatusMsg = ""; }, 3000);
  }

  function getPrimaryAiDrive() {
    if (aiNodeChoice && aiNodeChoice !== AI_NODE_MODE_AUTO && aiNodeChoice !== AI_NODE_MODE_PUBLIC) {
      const dev = (devices || []).find(d => (d.linkId || d.deviceId) === aiNodeChoice) || deviceRegistry.get(aiNodeChoice);
      if (dev && dev.drive) return dev.drive;
    }
    if (currentAiNodeDevice && currentAiNodeDevice.drive) {
      return currentAiNodeDevice.drive;
    }
    const onlineDev = (devices || []).find(d => !d.isLocal && !d.isAiNode && isDeviceOnline(d));
    if (onlineDev && onlineDev.drive) {
      return onlineDev.drive;
    }
    const act = getActiveDrive();
    if (act && typeof act.setSettings === "function") return act;
    return null;
  }

  function updateHordeSelectionState() {
    if (!hordeModel) {
      hordeModel = "aphrodite/TheDrummer/Behemoth-X-123B-v2.1";
      selectedHordeModel = hordeModel;
      isCustomHordeModel = false;
      return;
    }
    const found = hordeModels.find(m => m.id === hordeModel);
    if (found) {
      selectedHordeModel = hordeModel;
      isCustomHordeModel = false;
    } else {
      selectedHordeModel = "custom";
      isCustomHordeModel = true;
    }
  }

  function handleHordeModelChange(e) {
    const val = e.target.value;
    selectedHordeModel = val;
    if (val === "custom") {
      isCustomHordeModel = true;
    } else {
      isCustomHordeModel = false;
      hordeModel = val;
      if (typeof localStorage !== "undefined") {
        localStorage.setItem("omd_horde_model", val);
      }
      syncNodeAiSettings(aiMode);
      persistAiNodeChoice();
    }
  }

  function handleHordeModelCustomChange(e) {
    const val = (e.target.value || "").trim();
    hordeModel = val;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_horde_model", val);
    }
    syncNodeAiSettings(aiMode);
    persistAiNodeChoice();
  }

  function handleHordeKeyChange(e) {
    const val = (e.target.value || "").trim();
    hordeApiKey = val;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_horde_api_key", val);
    }
    syncNodeAiSettings(aiMode);
    persistAiNodeChoice();
  }

  async function copyHordeKey() {
    if (!hordeApiKey) return;
    try {
      await navigator.clipboard.writeText(hordeApiKey);
    } catch (_) {
      const ta = document.createElement("textarea");
      ta.value = hordeApiKey;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand("copy");
      ta.remove();
    }
    hordeKeyCopied = true;
    setTimeout(() => { hordeKeyCopied = false; }, 1800);
  }

  async function fetchHordeModels() {
    loadingHordeModels = true;
    try {
      let rawList = [];
      const drv = getPrimaryAiDrive();
      if (drv && typeof drv.fetch === "function") {
        try {
          const pRes = await drv.fetch("/assistant/horde_models");
          if (pRes.ok) {
            const pData = await pRes.json();
            rawList = pData.models || [];
          }
        } catch (_) {}
      }
      if (!rawList || rawList.length === 0) {
        try {
          const pRes = await fetch("/assistant/horde_models");
          if (pRes.ok) {
            const pData = await pRes.json();
            rawList = pData.models || [];
          }
        } catch (_) {}
      }
      if (!rawList || rawList.length === 0) {
        try {
          const hRes = await fetch("https://aihorde.net/api/v2/status/models?type=text");
          if (hRes.ok) {
            rawList = await hRes.json();
          }
        } catch (_) {}
      }
      if (Array.isArray(rawList) && rawList.length > 0) {
        const active = rawList
          .filter(m => m.type === "text" && ((m.count || 0) > 0 || (m.queued || 0) > 0))
          .sort((a, b) => (b.count || 0) - (a.count || 0));

        const activeList = active.map(m => {
          const countStr = (m.count || 0) === 1 ? "1 worker" : `${m.count || 0} workers`;
          const etaStr = (m.eta && m.eta > 0) ? `, ~${m.eta}s ETA` : ", no queue";
          const shortName = m.name.replace(/^aphrodite\//, "").replace(/^koboldcpp\//, "");
          return {
            id: m.name,
            label: `${shortName} (${countStr}${etaStr})`
          };
        });

        const seen = new Set();
        const combined = [];
        for (const item of activeList) {
          seen.add(item.id);
          combined.push(item);
        }
        for (const item of POPULAR_HORDE_MODELS) {
          if (!seen.has(item.id)) {
            combined.push(item);
            seen.add(item.id);
          }
        }
        hordeModels = combined;
        updateHordeSelectionState();
      }
    } catch (e) {
      console.warn("[settings] Could not fetch AI Horde active models:", e);
    } finally {
      loadingHordeModels = false;
    }
  }

  async function syncNodeAiSettings(modeToSet) {
    const drv = getPrimaryAiDrive();
    if (!drv || typeof drv.setSettings !== "function") return;
    try {
      let cur = {};
      try {
        if (typeof drv.getSettings === "function") {
          cur = (await drv.getSettings()) || {};
        }
      } catch (_) {}

      const provider = modeToSet === AI_MODE_HYBRID ? "hybrid" : (modeToSet === AI_MODE_LOCAL ? "local" : (cur.aiProvider || "local"));
      await drv.setSettings({
        ...cur,
        aiProvider: provider,
        hordeApiKey: hordeApiKey || "0000000000",
        hordeModel: hordeModel || "aphrodite/TheDrummer/Behemoth-X-123B-v2.1"
      });
      console.log("[settings] Synced settings to primary node:", { aiProvider: provider, hordeApiKey, hordeModel });
    } catch (err) {
      console.warn("[settings] Failed to sync settings to primary node:", err);
    }
  }

  async function loadNodeAiSettings() {
    const drv = getPrimaryAiDrive();
    if (!drv || typeof drv.getSettings !== "function") return;
    try {
      const s = await drv.getSettings();
      if (s) {
        if (s.hordeApiKey && (!hordeApiKey || hordeApiKey === "0000000000")) {
          hordeApiKey = s.hordeApiKey;
          if (typeof localStorage !== "undefined") localStorage.setItem("omd_horde_api_key", hordeApiKey);
        }
        if (s.hordeModel && (!hordeModel || hordeModel === "aphrodite/TheDrummer/Behemoth-X-123B-v2.1")) {
          hordeModel = s.hordeModel;
          if (typeof localStorage !== "undefined") localStorage.setItem("omd_horde_model", hordeModel);
          updateHordeSelectionState();
        }
      }
    } catch (_) {}
  }

  let assistantLoading = false;
  let assistantLoadedKey = "";

  function getSettingsKey() {
    return (
      $omdKeyStore?.key ||
      getStoredOmdKey() ||
      identity?.profile?.omd_key ||
      $profileStore?.omd_key ||
      (typeof localStorage !== "undefined" ? (localStorage.getItem("omd_key") || (identity?.mainPub ? localStorage.getItem(`omd_key:${identity.mainPub}`) : null)) : null) ||
      identity?.token ||
      identity?.mainPub ||
      identity?.did ||
      "omd_user"
    );
  }

  async function loadAssistantSettings() {
    if (assistantLoading) return;
    assistantLoading = true;
    try {
      const key = getSettingsKey();
      if (key) {
        const store = getChatStore(key);
        const settings = await store.getAssistantSettings();
        if (settings) {
          if (settings.assistant_name) assistantName = settings.assistant_name;
          if (settings.title) assistantTitle = settings.title;
          if (settings.system_prompt) assistantPersonality = settings.system_prompt;
          if (settings.fun_system_prompt) assistantFunPrompt = settings.fun_system_prompt;
          if (settings.name) assistantUserName = settings.name;
          const cachedLocal = typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "";
          if (cachedLocal) {
            assistantStorage = cachedLocal;
          } else if (settings.defaultStorage) {
            let s = settings.defaultStorage.replace(/^\/Private\/Private(\/|$)/i, "/Private$1");
            assistantStorage = s;
          }
          if (settings.style) assistantStyle = settings.style;
          if (settings.assistant_model) assistantModel = settings.assistant_model;
          if (settings.assistant_appearance) assistantAppearance = settings.assistant_appearance;
          if (settings.first_message) assistantFirstMessage = settings.first_message;
          if (typeof settings.ai_node === "string") {
            if (typeof localStorage !== "undefined" && !localStorage.getItem("omd_ai_node_choice")) {
              aiNodeChoice = settings.ai_node || AI_NODE_MODE_AUTO;
              setAiNodeChoice(aiNodeChoice);
            } else if (typeof localStorage !== "undefined" && localStorage.getItem("omd_ai_node_choice")) {
              aiNodeChoice = localStorage.getItem("omd_ai_node_choice");
            } else {
              aiNodeChoice = settings.ai_node || AI_NODE_MODE_AUTO;
            }
          } else if (typeof localStorage !== "undefined" && localStorage.getItem("omd_ai_node_choice")) {
            aiNodeChoice = localStorage.getItem("omd_ai_node_choice");
          }
          if (typeof settings.ai_mode === "string" && [AI_MODE_LOCAL, AI_MODE_HYBRID, AI_MODE_AGENTIC].includes(settings.ai_mode)) {
            if (typeof localStorage === "undefined" || !localStorage.getItem("omd_ai_mode")) {
              aiMode = settings.ai_mode;
              setAiMode(aiMode);
            }
          }
          if (typeof settings.ai_node_infer === "string") {
            if (typeof localStorage === "undefined" || !localStorage.getItem("omd_ai_infer_node")) {
              inferenceNodeId = settings.ai_node_infer;
              if (aiMode === AI_MODE_AGENTIC || inferenceNodeId) setInferenceNode(inferenceNodeId);
            }
          }
          if (settings.horde_api_key) {
            hordeApiKey = settings.horde_api_key;
            if (typeof localStorage !== "undefined") localStorage.setItem("omd_horde_api_key", hordeApiKey);
          }
          if (settings.horde_model) {
            hordeModel = settings.horde_model;
            if (typeof localStorage !== "undefined") localStorage.setItem("omd_horde_model", hordeModel);
            updateHordeSelectionState();
          }
          if (settings.custom_ai_nodes) {
            try {
              const list = JSON.parse(settings.custom_ai_nodes);
              if (Array.isArray(list)) mergeCustomAiNodes(list);
            } catch (_) {}
          }
          if (settings.public_ai_ack) {
            applyRemotePublicAiAck(settings.public_ai_ack);
          }
          if (settings.assistant_avatar && typeof settings.assistant_avatar === "string") {
            const rawAvatar = settings.assistant_avatar.trim();
            if (rawAvatar.startsWith("blob:") || rawAvatar.includes("blob:")) {
              assistantAvatarUrl = "";
              if (store && store._writeSettingsLocal) {
                store._writeSettingsLocal({ ...settings, assistant_avatar: "" });
              }
            } else if (rawAvatar.startsWith("data:image/")) {
              assistantAvatarUrl = rawAvatar;
            } else if (rawAvatar.length > 50 && /^[A-Za-z0-9+/=]+$/.test(rawAvatar.replace(/\s+/g, ""))) {
              assistantAvatarUrl = "data:image/jpeg;base64," + rawAvatar;
            } else {
              assistantAvatarUrl = "";
            }
          }
          assistantLoadedKey = key;
          loadNodeAiSettings();
          if (aiMode === AI_MODE_HYBRID) {
            fetchHordeModels();
          }
        }
      }
      // Defaults for a new user or when personality is still empty: take June profile from device or fallback
      const effectiveDrive = getActiveDrive();
      let info = null;
      if (effectiveDrive) {
        try {
          const res = await effectiveDrive.fetch("/assistant");
          if (res.ok) {
            const data = await res.json();
            info = data.result || data;
          }
        } catch (_) {}
      }
      if (!info && typeof window !== "undefined") {
        try {
          const res = await window.fetch("/assistant");
          if (res.ok) {
            const data = await res.json();
            info = data.result || data;
          }
        } catch (_) {}
      }

      if (info) {
        if (!assistantName || assistantName === "June") assistantName = info.assistant_name || "June";
        if (!assistantTitle || assistantTitle === "Junior assistant") assistantTitle = info.title || "Junior assistant";
        if (!assistantPersonality && info.system_prompt) assistantPersonality = info.system_prompt;
        if (!assistantFunPrompt && info.fun_system_prompt) assistantFunPrompt = info.fun_system_prompt;
        if (!assistantModel && info.assistant_model) assistantModel = info.assistant_model;
        if (!assistantAppearance && info.assistant_appearance) assistantAppearance = info.assistant_appearance;
      }
      if (!assistantPersonality) {
        assistantPersonality = DEFAULT_JUNE_SYSTEM_PROMPT;
      }
      if (!assistantFunPrompt) {
        assistantFunPrompt = DEFAULT_JUNE_FUN_PROMPT;
      }

      // Fallback to device default avatar (same as Chat new window)
      if (!assistantAvatarUrl) {
        let blob = null;
        if (effectiveDrive) {
          try {
            const res = await effectiveDrive.fetch("/assistant/avatar?size=160");
            if (res.ok) blob = await res.blob();
          } catch (_) {}
        }
        if ((!blob || blob.size === 0) && typeof window !== "undefined") {
          try {
            const res = await window.fetch("/assistant/avatar?size=160");
            if (res.ok) blob = await res.blob();
          } catch (_) {}
        }
        if ((!blob || blob.size === 0) && typeof window !== "undefined") {
          try {
            const res = await window.fetch("/img/june.png");
            if (res.ok) blob = await res.blob();
          } catch (_) {}
        }
        if (blob && blob.size > 0) {
          const dataUrl = await blobToDataUrl(blob);
          if (dataUrl) {
            assistantAvatarUrl = dataUrl;
          }
        }
      }

      // Persist defaults to GunDB / chat store if this is a fresh user
      if (key && (assistantPersonality || assistantAvatarUrl)) {
        try {
          const store = getChatStore(key);
          await store.updateAssistantSettings({
            assistant_name: assistantName,
            title: assistantTitle,
            system_prompt: assistantPersonality,
            fun_system_prompt: assistantFunPrompt,
            assistant_appearance: assistantAppearance,
            ...(assistantAvatarUrl ? { assistant_avatar: assistantAvatarUrl, avatar_source: "node" } : {})
          });
        } catch (_) {}
      }
      if (!assistantUserName) {
        assistantUserName =
          identity?.profile?.name ||
          $profileStore?.name ||
          identity?.profile?.user ||
          $profileStore?.user ||
          "";
      }
      if (!assistantStorage || assistantStorage.startsWith("/Private/Private") || !parseStoragePath(assistantStorage).isDeviceQualified) {
        let s =
          assistantStorage ||
          (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
          $profileStore?.defaultStorage ||
          identity?.profile?.defaultStorage ||
          DEFAULT_STORAGE_PATH;
        s = s.replace(/^\/Private\/Private(\/|$)/i, "/Private$1");
        if (!s) s = DEFAULT_STORAGE_PATH;
        const parsed = parseStoragePath(s);
        if (!parsed.isDeviceQualified) {
          const autoDev = currentAiNodeDevice || (devices && devices.find(d => !d.local && !d.isAiNode)) || deviceRegistry.list().find(d => !d.local && !d.isAiNode);
          if (autoDev && autoDev.name) {
            s = formatStoragePath(autoDev, parsed.folder || s);
          }
        }
        assistantStorage = s;
      }
    } catch (e) {
      console.warn("[settings] loadAssistantSettings error:", e);
    } finally {
      assistantLoaded = true;
      assistantLoading = false;
    }
  }

  let avatarErrorHandled = false;
  async function handleAvatarError() {
    if (avatarErrorHandled) {
      assistantAvatarUrl = "";
      return;
    }
    avatarErrorHandled = true;
    const oldUrl = assistantAvatarUrl;
    assistantAvatarUrl = "";
    if (oldUrl && oldUrl.startsWith("blob:")) {
      try { URL.revokeObjectURL(oldUrl); } catch (_) {}
    }
    const effectiveDrive = getActiveDrive();
    if (effectiveDrive) {
      try {
        const res = await effectiveDrive.fetch("/assistant/avatar?size=160");
        if (res.ok) {
          const blob = await res.blob();
          if (blob && blob.size > 0) {
            assistantAvatarUrl = URL.createObjectURL(blob);
          }
        }
      } catch (_) {}
    }
  }

  async function saveAssistantConfig() {
    assistantSaving = true;
    assistantSaveMessage = "";
    try {
      const key = getSettingsKey();
      // Persist storage path across profileStore, localStorage, DID profile immediately
      await persistStoragePath(assistantStorage, identity, key);

      const store = getChatStore(key);
      const updatePayload = {
        assistant_name: assistantName,
        title: assistantTitle,
        system_prompt: assistantPersonality,
        fun_system_prompt: assistantFunPrompt,
        name: assistantUserName,
        defaultStorage: assistantStorage,
        style: assistantStyle,
        assistant_model: assistantModel,
        assistant_appearance: assistantAppearance,
        ai_node: aiNodeChoice,
        ai_mode: aiMode,
        ai_node_infer: aiMode === AI_MODE_AGENTIC ? inferenceNodeId : "",
        horde_api_key: hordeApiKey,
        horde_model: hordeModel,
        custom_ai_nodes: JSON.stringify($aiNodeStore?.customNodes || []),
      };
      if (assistantFirstMessage) {
        updatePayload.first_message = assistantFirstMessage;
      }
      if (assistantAvatarUrl && assistantAvatarUrl.startsWith("data:image/")) {
        updatePayload.assistant_avatar = assistantAvatarUrl;
      } else {
        updatePayload.assistant_avatar = "";
        updatePayload.avatar_source = "default";
      }
      if (aiNodeChoice === AI_NODE_MODE_PUBLIC && publicAiAckPayload()) {
        updatePayload.public_ai_ack = publicAiAckPayload();
      }
      await store.updateAssistantSettings(updatePayload);
      await syncNodeAiSettings(aiMode);
      assistantSaveMessage = $_("assistant_saved_success", {}, "Assistant persona settings saved to your encrypted DID graph.");
      assistantSaveType = "success";
      setTimeout(() => {
        if (assistantSaveMessage) assistantSaveMessage = "";
      }, 4000);
    } catch (err) {
      console.error("[settings] saveAssistantConfig error:", err);
      assistantSaveMessage = err.message || "Failed to save assistant settings";
      assistantSaveType = "error";
    } finally {
      assistantSaving = false;
    }
  }

  // If the sync section has been hidden (fewer than two own nodes), leave it
  $: if (activeSection === "sync" && ownDeviceCount < 2) {
    activeSection = "profile";
  }

  let lastAssistantLoadedKey = "";
  let lastAssistantLoadedSection = "";
  $: {
    const currentKey = getSettingsKey();
    if (activeSection === "assistant") {
      const keyChanged = !!(currentKey && currentKey !== lastAssistantLoadedKey);
      const sectionChanged = lastAssistantLoadedSection !== "assistant";
      if ((!assistantLoaded || keyChanged || sectionChanged) && !assistantLoading) {
        lastAssistantLoadedKey = currentKey;
        lastAssistantLoadedSection = "assistant";
        refreshDevices();
        refreshPublicAiNode();
        loadAssistantSettings();
      }
    } else {
      lastAssistantLoadedSection = activeSection;
    }
  }

  $: {
    const isPublic = (id) => !!($aiNodeStore?.info?.linkID && id === $aiNodeStore.info.linkID);
    const linkId = targetLinkId || selectedSyncTarget || (devices && devices.find(d => isDeviceOnline(d) && !d.isLocal)?.linkId) || devices?.[0]?.linkId || $aiNodeStore?.info?.linkID || "";
    if (linkId && (!httpDrive || httpDrive.targetLinkId !== linkId)) {
      if (httpDrive) httpDrive.close();
      const token = $omdKeyStore?.key || identity?.token || (typeof localStorage !== "undefined" ? localStorage.getItem("omd_key") : "");
      const isPrivate = !isPublic(linkId) && isNodeOwner;
      httpDrive = new HttpDrive({ targetLinkId: linkId, port: port || 4000, isPrivateMode: isPrivate, authToken: token });
    }
  }

  function getActiveDrive() {
    const isPublic = (id) => !!($aiNodeStore?.info?.linkID && id === $aiNodeStore.info.linkID);
    const linkId = targetLinkId || selectedSyncTarget || (devices && devices.find(d => isDeviceOnline(d) && !d.isLocal)?.linkId) || devices?.[0]?.linkId || $aiNodeStore?.info?.linkID || "";
    if (httpDrive && httpDrive.targetLinkId === linkId) return httpDrive;
    if (linkId) {
      if (httpDrive) httpDrive.close();
      const token = $omdKeyStore?.key || identity?.token || (typeof localStorage !== "undefined" ? localStorage.getItem("omd_key") : "");
      const isPrivate = !isPublic(linkId) && isNodeOwner;
      httpDrive = new HttpDrive({ targetLinkId: linkId, port: port || 4000, isPrivateMode: isPrivate, authToken: token });
      return httpDrive;
    }
    return null;
  }

  $: if (activeSection === "assistant" && $aiNodeStore?.info && (!assistantPersonality || !assistantAvatarUrl)) {
    loadAssistantSettings();
  }

  // Custom Rich Web Push subscription state
  let webPushSupported = isWebPushSupported();
  let webPushSubscribed = false;
  let webPushBusy = false;
  let webPushMsg = "";
  let webPushMsgKey = "";
  let webPushMsgType = "info";

  async function refreshWebPushState() {
    webPushSupported = isWebPushSupported();
    if (!webPushSupported) {
      webPushSubscribed = false;
      return;
    }
    const sub = await getBrowserPushSubscription();
    webPushSubscribed = Boolean(sub);
  }

  $: if (activeSection === "appearance" || activeSection === "assistant" || activeSection === "profile") {
    refreshWebPushState();
  }

  function resolveEffectiveStorageForPush() {
    const raw =
      assistantStorage ||
      (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
      $profileStore?.defaultStorage ||
      identity?.profile?.defaultStorage ||
      DEFAULT_STORAGE_PATH;
    return String(raw || "").trim();
  }

  async function handleToggleWebPush() {
    webPushBusy = true;
    webPushMsg = "";
    webPushMsgKey = "";
    try {
      const drv = getActiveDrive() || getPrimaryAiDrive();
      const fetchFn = drv ? (p, opts) => drv.fetch(p, opts) : (p, opts) => window.fetch(p, opts);
      const key = getSettingsKey();
      if (webPushSubscribed) {
        await unsubscribeFromNodeWebPush({ fetchFn, omdKey: key });
        webPushSubscribed = false;
        webPushMsgKey = "push_msg_disabled";
        webPushMsg = translate("push_msg_disabled", {}, "Push notifications on this device have been disabled.");
        webPushMsgType = "info";
      } else {
        await subscribeToNodeWebPush({
          fetchFn,
          omdKey: key,
          defaultStorage: resolveEffectiveStorageForPush()
        });
        webPushSubscribed = true;
        webPushMsgKey = "push_msg_enabled";
        webPushMsg = translate("push_msg_enabled", {}, "Push notifications enabled! Image previews and action buttons are active.");
        webPushMsgType = "success";
      }
    } catch (err) {
      webPushMsg = err?.message || String(err);
      webPushMsgType = "error";
    } finally {
      webPushBusy = false;
    }
  }

  async function handleTestWebPush() {
    webPushBusy = true;
    webPushMsg = "";
    webPushMsgKey = "";
    try {
      const drv = getActiveDrive() || getPrimaryAiDrive();
      const fetchFn = drv ? (p, opts) => drv.fetch(p, opts) : (p, opts) => window.fetch(p, opts);
      const key = getSettingsKey();
      const res = await sendTestNodeWebPush({
        fetchFn,
        omdKey: key,
        defaultStorage: resolveEffectiveStorageForPush()
      });
      if (res && res.sent > 0) {
        webPushMsgKey = "push_msg_test_sent";
        webPushMsg = translate("push_msg_test_sent", {}, "Test push notification sent!");
        webPushMsgType = "success";
      } else {
        webPushMsgKey = "push_msg_sub_missing";
        webPushMsg = translate("push_msg_sub_missing", {}, "Subscription not found on node — toggle Push off and on again.");
        webPushMsgType = "error";
      }
    } catch (err) {
      webPushMsg = err?.message || String(err);
      webPushMsgType = "error";
    } finally {
      webPushBusy = false;
    }
  }

  function parseSillyTavernCardBytes(bytes) {
    try {
      let offset = 8;
      while (offset < bytes.length - 8) {
        const len = (bytes[offset] << 24) | (bytes[offset + 1] << 16) | (bytes[offset + 2] << 8) | bytes[offset + 3];
        const type = String.fromCharCode(bytes[offset + 4], bytes[offset + 5], bytes[offset + 6], bytes[offset + 7]);
        if (type === "tEXt") {
          const chunkData = bytes.slice(offset + 8, offset + 8 + len);
          let nullIdx = chunkData.indexOf(0);
          if (nullIdx !== -1) {
            const keyword = new TextDecoder().decode(chunkData.slice(0, nullIdx));
            if (keyword === "chara") {
              const rawB64 = new TextDecoder().decode(chunkData.slice(nullIdx + 1));
              const jsonStr = atob(rawB64);
              const cardData = JSON.parse(decodeURIComponent(escape(jsonStr)));
              return cardData.data || cardData;
            }
          }
        }
        offset += 12 + len;
      }
    } catch (e) {
      console.warn("[settings] client card parse warning:", e);
    }
    return null;
  }

  async function handleCharacterSelected(e) {
    const file = e.target?.files?.[0];
    if (!file) return;
    isImportingCharacter = true;
    assistantSaveMessage = "";
    try {
      const [dataUrl, buffer] = await Promise.all([
        new Promise((resolve) => {
          const r = new FileReader();
          r.onload = (re) => resolve(re.target?.result);
          r.onerror = () => resolve(null);
          r.readAsDataURL(file);
        }),
        file.arrayBuffer()
      ]);

      const parsedChara = parseSillyTavernCardBytes(new Uint8Array(buffer));
      let charName = parsedChara?.name || "Companion";
      let charFirstMes = parsedChara?.first_mes || "";
      let charSystemPrompt = "";

      if (parsedChara) {
        const sections = [];
        if (parsedChara.system_prompt) sections.push(parsedChara.system_prompt.trim());
        if (parsedChara.description) sections.push(`### Character Description:\n${parsedChara.description.trim()}`);
        if (parsedChara.personality) sections.push(`### Personality:\n${parsedChara.personality.trim()}`);
        if (parsedChara.scenario) sections.push(`### Scenario / Setting:\n${parsedChara.scenario.trim()}`);
        if (parsedChara.mes_example) sections.push(`### Dialogue Examples:\n${parsedChara.mes_example.trim()}`);
        charSystemPrompt = sections.join("\n\n");
      }

      const drive = getActiveDrive();
      if (drive) {
        try {
          const formData = new FormData();
          formData.append("file", file);
          const key = $omdKeyStore?.key || identity?.token || (typeof localStorage !== "undefined" ? localStorage.getItem("omd_key") : null);
          if (key) formData.append("omd_key", key);

          const res = await drive.fetch("/assistant/import_character", {
            method: "POST",
            body: formData
          });
          if (res.ok) {
            const data = await res.json();
            if (data.name) charName = data.name;
            if (data.system_prompt) charSystemPrompt = data.system_prompt;
            if (data.first_message) charFirstMes = data.first_message;
            if (data.assistant_model) assistantModel = data.assistant_model;
            if (data.title) assistantTitle = data.title;
          }
        } catch (be) {
          console.warn("[settings] Backend import_character warning:", be);
        }
      }

      if (!charSystemPrompt && !parsedChara) {
        throw new Error($_("invalid_character_card", {}, "Selected image does not contain a SillyTavern character card."));
      }

      assistantName = charName;
      if (!assistantTitle || assistantTitle === "Junior assistant") {
        assistantTitle = "Companion";
      }
      if (charSystemPrompt) {
        assistantPersonality = charSystemPrompt;
        assistantFunPrompt = charSystemPrompt;
      }
      if (charFirstMes) {
        assistantFirstMessage = charFirstMes;
      }
      if (dataUrl) {
        assistantAvatarUrl = dataUrl;
      }

      await saveAssistantConfig();

      assistantSaveMessage = $_("character_imported_success", {}, `Character "${charName}" imported successfully!`);
      assistantSaveType = "success";
      setTimeout(() => {
        if (assistantSaveMessage) assistantSaveMessage = "";
      }, 4000);
    } catch (err) {
      console.error("[settings] handleCharacterSelected error:", err);
      assistantSaveMessage = err.message || "Failed to import character card";
      assistantSaveType = "error";
    } finally {
      isImportingCharacter = false;
      if (characterFileInput) characterFileInput.value = "";
    }
  }
</script>

<div class="settings-root {isDarkTheme ? 'dark-theme' : 'light-theme'}">
  <!-- Top Navigation Bar -->
  <header class="settings-header">
    <div class="header-left">
      <button class="back-btn" on:click={() => dispatch("close")} title={$_("back_to_files", {}, "Back to files (Esc)")}>
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2"><path d="M19 12H5M12 19l-7-7 7-7"/></svg>
        <span class="back-btn-text">{$_("back_to_files", {}, "Back to Files")}</span>
      </button>
      <div class="header-divider"></div>

      <!-- Mobile Sidebar Toggle Button -->
      <button 
        class="mobile-sidebar-toggle-btn" 
        title="Toggle Settings Menu" 
        on:click={() => (showSidebar = !showSidebar)}
      >
        <svg width="18" height="18" viewBox="0 0 16 16"><path fill="currentColor" d="M1 3.5A1.5 1.5 0 0 1 2.5 2h11A1.5 1.5 0 0 1 15 3.5v9a1.5 1.5 0 0 1-1.5 1.5h-11A1.5 1.5 0 0 1 1 12.5v-9zM2.5 3a.5.5 0 0 0-.5.5V12.5a.5.5 0 0 0 .5.5H5V3H2.5zm3.5 10h7.5a.5.5 0 0 0 .5-.5V3.5a.5.5 0 0 0-.5-.5H6v10z"/></svg>
      </button>

      <div class="header-title-wrap">
        <h1 class="header-title">{$_("settings_title", {}, "Settings")}</h1>
        <span class="header-badge {$profileStore.isEphemeral ? 'ephemeral' : ''}">
          {$profileStore.isEphemeral ? $_("guest_session", {}, "Guest Session") : $_("decentralized_id", {}, "Decentralized ID")}
        </span>
      </div>
    </div>

    <div class="header-right">
      <button class="icon-close-btn" on:click={() => dispatch("close")}>
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
      </button>
    </div>
  </header>

  <!-- Settings Body Layout -->
  <div class="settings-body">
    {#if isMobile && showSidebar}
      <div class="settings-sidebar-backdrop" on:click={() => (showSidebar = false)}></div>
    {/if}

    <!-- Left Navigation Sidebar -->
    <aside class="settings-sidebar" class:show-mobile={showSidebar}>
      <nav class="settings-nav">
        {#each sections as sec}
          <button
            class="nav-item {activeSection === sec.id ? 'active' : ''}"
            on:click={() => selectSection(sec.id)}
          >
            <span class="nav-icon">{@html sec.icon}</span>
            <span class="nav-label">{sec.label}</span>
          </button>
        {/each}
      </nav>

      <!-- Sidebar Footer / User summary -->
      <div class="sidebar-user-footer">
        <div class="user-avatar-mini {$profileStore.isEphemeral ? 'ephemeral' : ''}">
          {#if $profileStore.image && !profileImgFailed}
            <img
              src={$profileStore.image}
              alt=""
              referrerpolicy="no-referrer"
              on:error={() => { profileImgFailed = true; }}
            />
          {:else if $profileStore.isEphemeral}
            <svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor"><path d="M8 8a3 3 0 1 0 0-6 3 3 0 0 0 0 6m2-3a2 2 0 1 1-4 0 2 2 0 0 1 4 0m4 8c0 1-1 1-1 1H3s-1 0-1-1 1-4 6-4 6 3 6 4m-1-.004c-.001-.246-.154-.986-.832-1.664C11.516 10.68 10.289 10 8 10s-3.516.68-4.168 1.332c-.678.678-.83 1.418-.832 1.664z"/></svg>
          {:else}
            <span>{($profileStore.name || $profileStore.user || "U").charAt(0).toUpperCase()}</span>
          {/if}
          <span class="online-indicator {$profileStore.isEphemeral ? 'ephemeral' : ''}"></span>
        </div>
        <div class="user-info-mini">
          <span class="user-name-mini">{$profileStore.name}</span>
          <span class="user-sub-mini">{$profileStore.isEphemeral ? 'Direct P2P Link' : ($profileStore.user ? '@' + $profileStore.user : ($profileStore.email || 'Online'))}</span>
        </div>
        <button class="logout-btn-mini" title={$profileStore.isEphemeral ? "Disconnect session" : "Sign out"} on:click={() => dispatch("logout")}>
          <svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor"><path fill-rule="evenodd" d="M6 8a.5.5 0 0 0 .5.5h5.793l-2.147 2.146a.5.5 0 0 0 .708.708l3-3a.5.5 0 0 0 0-.708l-3-3a.5.5 0 0 0-.708.708L12.293 7.5H6.5A.5.5 0 0 0 6 8m-2.5 7a.5.5 0 0 1-.5-.5v-13a.5.5 0 0 1 1 0v13a.5.5 0 0 1-.5.5"/></svg>
        </button>
      </div>
    </aside>

    <!-- Main Content Panel -->
    <main class="settings-content">
      <div class="content-container">
        <!-- 1. PROFILE & IDENTITY -->
        {#if activeSection === "profile"}
          <div class="section-header">
            <h2 class="section-title">{$_("profile_section_title", {}, "Profile & Identity")}</h2>
            <p class="section-desc">{$_("profile_section_desc", {}, "Manage your decentralized GunDB SEA identity, credentials and linked accounts.")}</p>
          </div>

          <div class="card-group">
            <!-- Profile Identity Card -->
            <div class="card">
              <div class="card-header-row">
                <div class="avatar-large {$profileStore.isEphemeral ? 'ephemeral' : ''}">
                  {#if $profileStore.image && !profileImgFailed}
                    <img
                      src={$profileStore.image}
                      alt=""
                      referrerpolicy="no-referrer"
                      on:error={() => { profileImgFailed = true; }}
                    />
                  {:else if $profileStore.isEphemeral}
                    <svg viewBox="0 0 24 24" width="32" height="32" fill="none" stroke="currentColor" stroke-width="2"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>
                  {:else}
                    <div class="avatar-fallback-large">
                      {($profileStore.name || $profileStore.user || "U").charAt(0).toUpperCase()}
                    </div>
                  {/if}
                </div>
                <div class="profile-header-info">
                  <h3 class="profile-display-name">{$profileStore.name}</h3>
                  <p class="profile-display-sub">
                    {#if $profileStore.isEphemeral}
                      {$_("direct_p2p_session_sub", {}, "Direct P2P Link • Session keys held only in memory")}
                    {:else}
                      {$profileStore.email || ($profileStore.user ? '@' + $profileStore.user : $_("persistent_node", {}, "Persistent Node"))}
                    {/if}
                  </p>
                  <div class="profile-badge-row">
                    <span class="did-verified-tag {$profileStore.isEphemeral ? 'ephemeral' : 'success'}">
                      <span class="did-tag-dot"></span>
                      {$profileStore.isEphemeral ? $_("guest", {}, "Guest") : $_("did_verified", {}, "DID Verified")}
                    </span>
                    {#if !$profileStore.isEphemeral && getDidTag($profileStore.did || identity?.did)}
                      <span
                        class="did-verified-tag"
                        style="background: rgba(99, 102, 241, 0.14); color: #818cf8; border: 1px solid rgba(99, 102, 241, 0.32); font-family: monospace;"
                        title={$_("did_tag_tooltip", {}, "4-character fingerprint derived from your public cryptographic key")}
                      >
                        #{getDidTag($profileStore.did || identity?.did)}
                      </span>
                    {/if}
                  </div>
                </div>
              </div>

              <div class="card-divider"></div>

              {#if $profileStore.isPersistent}
                <!-- DID Row -->
                <div class="setting-row col-layout">
                  <div class="row-text">
                    <span class="row-label">{$_("did_label", {}, "Decentralized Identifier (DID)")}</span>
                    <span class="row-sub">{$_("did_sub", {}, "Unique cryptographic identity on the GunDB decentralized network")}</span>
                  </div>
                  <div class="row-action-full">
                    <div class="code-box">
                      <code>{$profileStore.did || (identity?.did) || "did:gun:..."}</code>
                      <div class="code-box-actions">
                        <button class="copy-btn" on:click={() => copyToClipboard($profileStore.did || identity?.did, 'did')}>
                          {copiedField === 'did' ? $_("copied", {}, "Copied!") : $_("copy_did", {}, "Copy DID")}
                        </button>
                        <button class="copy-btn" style="background: rgba(99, 102, 241, 0.18); color: #a5b4fc; border-color: rgba(99, 102, 241, 0.38);" on:click={handleExportDidKeyfile}>
                          {keyfileExportMsg ? "✓" : $_("save_keyfile_json_btn", {}, "Save Keyfile")}
                        </button>
                      </div>
                    </div>
                  </div>
                </div>

                <!-- Certificate Status -->
                <div class="setting-row">
                  <div class="row-text">
                    <span class="row-label">{$_("cert_label", {}, "Device Access Certificate")}</span>
                    <span class="row-sub">{$_("cert_sub", {}, "Signed SEA capability granting this device write permissions to your user graph")}</span>
                  </div>
                  <div class="row-value-badge">
                    <span class="badge-pill success">{$_("active_valid", {}, "Active & Valid")}</span>
                  </div>
                </div>

              {:else}
                <!-- Ephemeral Notice Row -->
                <div class="setting-row">
                  <div class="row-text">
                    <span class="row-label">{$_("guest_mode_label", {}, "Anonymous Guest Mode")}</span>
                    <span class="row-sub">{$_("guest_mode_sub", {}, "You are connected directly to a device without a permanent account. Session keys exist only in this browser memory.")}</span>
                  </div>
                  <div class="row-value-badge">
                    <span class="badge-pill warning">{$_("ephemeral", {}, "Ephemeral")}</span>
                  </div>
                </div>
              {/if}

              <!-- Onboarding Wizard Trigger -->
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("onboarding_wizard_label", {}, "Quick Setup Wizard")}</span>
                  <span class="row-sub">{$_("onboarding_wizard_sub", {}, "Reconfigure storage folder for materials and AI node mode")}</span>
                </div>
                <div class="row-value-badge">
                  <button class="btn-secondary-action btn-sm" on:click={() => dispatch("restart-onboarding")}>
                    <span>{$_("run_onboarding_btn", {}, "Start Wizard")}</span>
                  </button>
                </div>
              </div>

              <!-- App Version & Live Update -->
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("app_version", {}, "App Version")}</span>
                  <span class="row-sub">
                    {#if $otaState.updateAvailable}
                      <span style="color: var(--accent, #A28AD0); font-weight: 600;">{$_("update_available_sub", { version: $otaState.serverVersion || $otaState.newVersion }, `Version v${$otaState.serverVersion || $otaState.newVersion} available!`)}</span>
                    {:else}
                      {$_("app_version_sub", {}, "Installed release version on this device")}
                    {/if}
                  </span>
                </div>
                <div class="row-value-badge" style="display: flex; align-items: center; gap: 8px;">
                  <span class="badge-pill success" style="font-family: monospace;">v{$appVersion}</span>
                  {#if $otaState.otaSupported !== false}
                    {#if $otaState.updateAvailable}
                      <button class="btn-primary-action btn-sm" on:click={() => performOtaUpdate()} style="display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; font-size: 12px;">
                        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2.2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3"/></svg>
                        <span>{$_("update_now_btn", {}, "Update")}</span>
                      </button>
                    {:else}
                      <button class="btn-secondary-action btn-sm" on:click={() => checkForOtaUpdate(true)} style="padding: 4px 10px; font-size: 12px;">
                        <span>{$_("check_updates", {}, "Check")}</span>
                      </button>
                    {/if}
                  {/if}
                </div>
              </div>

              <!-- About Project Row -->
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("about_project", {}, "About Project")}</span>
                  <span class="row-sub">{$_("about_project_sub", {}, "Open-source overview, architecture, and self-hosting guide")}</span>
                </div>
                <div class="row-value-badge">
                  <a
                    href={$otaState.isNative ? "https://aiassistant.direct/about" : "/about"}
                    target="_blank"
                    rel="noopener noreferrer"
                    class="btn-secondary-action btn-sm"
                    style="text-decoration: none; display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; font-size: 12px;"
                  >
                    <span>{$_("about_btn", {}, "About")}</span>
                    <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6M15 3h6v6M10 14L21 3"/></svg>
                  </a>
                </div>
              </div>

              <!-- Support & Help Row -->
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("support_help", {}, "Support & Help")}</span>
                  <span class="row-sub">{$_("support_help_sub", {}, "Community Discord and GitHub discussions")}</span>
                </div>
                <div class="row-value-badge">
                  <a
                    href="https://discord.gg/qAQpfmrBPW"
                    target="_blank"
                    rel="noopener noreferrer"
                    class="btn-secondary-action btn-sm"
                    style="text-decoration: none; display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; font-size: 12px;"
                  >
                    <span>{$_("contact_support", {}, "Discord")}</span>
                    <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6M15 3h6v6M10 14L21 3"/></svg>
                  </a>
                </div>
              </div>

              <!-- Terms & Privacy Row -->
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("legal_terms_privacy", {}, "Terms & Privacy")}</span>
                  <span class="row-sub">{$_("legal_terms_privacy_sub", {}, "Terms of Service, Zero-Tolerance UGC Policy, and GDPR Privacy")}</span>
                </div>
                <div class="row-value-badge">
                  <a
                    href={$otaState.isNative ? "https://aiassistant.direct/tos" : "/tos"}
                    target="_blank"
                    rel="noopener noreferrer"
                    class="btn-secondary-action btn-sm"
                    style="text-decoration: none; display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; font-size: 12px;"
                  >
                    <span>{$_("view_terms", {}, "Terms & Privacy")}</span>
                    <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6M15 3h6v6M10 14L21 3"/></svg>
                  </a>
                </div>
              </div>
            </div>

            {#if $profileStore.isPersistent}
              <!-- Account Details Card (for permanent users) -->
              <div class="card">
                <div class="card-header-row" style="margin-bottom: 20px;">
                  <div>
                    <h4 class="card-title" style="margin: 0 0 4px 0;">{$_("personal_details", {}, "Personal Details")}</h4>
                    <p class="card-subtitle" style="margin: 0;">{$_("personal_details_sub", {}, "Customize your decentralized profile, avatar, and linked email.")}</p>
                  </div>
                </div>

                <!-- Avatar Edit Row -->
                <div class="setting-row">
                  <div class="row-text">
                    <span class="row-label">{$_("profile_avatar", {}, "Profile Avatar")}</span>
                    <span class="row-sub">{$_("profile_avatar_sub", {}, "Image shown in conversations, nodes, and sidebar")}</span>
                  </div>
                  <div class="avatar-edit-controls">
                    <div class="avatar-preview-box">
                      {#if editAvatar && !editAvatarFailed}
                        <img
                          src={editAvatar}
                          alt=""
                          referrerpolicy="no-referrer"
                          on:error={() => { editAvatarFailed = true; }}
                        />
                      {:else}
                        <div class="avatar-fallback-medium">
                          {(editDisplayName || editUsername || "U").charAt(0).toUpperCase()}
                        </div>
                      {/if}
                    </div>
                    <div class="avatar-buttons-col">
                      <label class="btn-secondary-action btn-sm">
                        <span>{$_("upload_photo", {}, "Upload photo")}</span>
                        <input type="file" accept="image/*" on:change={handleAvatarFileChange} style="display: none;" />
                      </label>
                      {#if editAvatar}
                        <button class="btn-ghost-danger btn-sm" on:click={() => { editAvatar = ""; }}>
                          {$_("remove", {}, "Remove")}
                        </button>
                      {/if}
                    </div>
                  </div>
                </div>

                <!-- Display Name Row -->
                <div class="setting-row">
                  <div class="row-text">
                    <label class="row-label" for="sett-displayname">{$_("display_name", {}, "Full / Display Name")}</label>
                    <span class="row-sub">{$_("display_name_sub", {}, "Public name shown to other users and devices")}</span>
                  </div>
                  <div class="row-input-wrap">
                    <input
                      id="sett-displayname"
                      class="text-input"
                      type="text"
                      placeholder="e.g. Alexey"
                      bind:value={editDisplayName}
                    />
                  </div>
                </div>

                <!-- Username / Alias Row -->
                <div class="setting-row">
                  <div class="row-text">
                    <label class="row-label" for="sett-handle">{$_("login_nickname_alias", {}, "Login Nickname / Alias")}</label>
                    <span class="row-sub">{$_("login_nickname_alias_sub", {}, "Used with your password for quick sign-in — need not be unique (#tag distinguishes same names)")}</span>
                  </div>
                  <div class="row-input-wrap">
                    <div class="input-with-prefix">
                      <span class="input-prefix">@</span>
                      <input
                        id="sett-handle"
                        class="text-input with-prefix"
                        type="text"
                        placeholder="username"
                        bind:value={editUsername}
                      />
                    </div>
                  </div>
                </div>

                <!-- Primary Email Row with Verification -->
                <div class="setting-row email-setting-row">
                  <div class="row-text">
                    <span class="row-label">{$_("primary_email", {}, "Primary Email")}</span>
                    <span class="row-sub">{$_("primary_email_sub", {}, "Used for account recovery and verified notifications")}</span>
                  </div>
                  <div class="email-details-wrap">
                    <div class="current-email-badge">
                      <span class="email-text">{$profileStore.email || $_("not_linked", {}, "Not linked")}</span>
                      {#if $profileStore.email}
                        <span class="badge-pill success">{$_("verified", {}, "Verified")}</span>
                      {/if}
                      <button class="btn-link-action" on:click={() => { showChangeEmail = !showChangeEmail; emailChangeMsg = ""; emailChangeError = ""; }}>
                        {showChangeEmail ? $_("cancel", {}, "Cancel") : $_("change_email", {}, "Change Email")}
                      </button>
                    </div>

                    {#if showChangeEmail}
                      <div class="change-email-box">
                        <p class="change-email-notice">
                          {$_("change_email_notice", {}, "Changing your primary recovery email requires email confirmation. A verification link will be sent to your new address.")}
                        </p>
                        <div class="change-email-row">
                          <input
                            class="text-input"
                            type="email"
                            placeholder="new-email@example.com"
                            bind:value={newEmailInput}
                            disabled={requestingEmailChange}
                          />
                          <button
                            class="btn-primary-action"
                            on:click={handleRequestEmailChange}
                            disabled={requestingEmailChange || !newEmailInput.trim()}
                          >
                            {requestingEmailChange ? $_("sending", {}, "Sending...") : $_("send_link", {}, "Send Link")}
                          </button>
                        </div>
                        {#if emailChangeMsg}
                          <p class="success-text mt-2">{emailChangeMsg}</p>
                        {/if}
                        {#if emailChangeError}
                          <p class="error-text mt-2">{emailChangeError}</p>
                        {/if}
                      </div>
                    {/if}
                  </div>
                </div>

                <!-- Save Profile Row -->
                <div class="card-footer-action-row">
                  <button class="btn-primary-action" on:click={handleSaveProfile} disabled={savingProfile}>
                    {savingProfile ? $_("saving_changes", {}, "Saving Changes...") : $_("save_profile_changes", {}, "Save Profile Changes")}
                  </button>
                  {#if profileSaveMsg}
                    <span class="save-feedback-success">{profileSaveMsg}</span>
                  {/if}
                  {#if profileSaveError}
                    <span class="save-feedback-error">{profileSaveError}</span>
                  {/if}
                </div>
              </div>

              <!-- Danger Zone: Delete Account -->
              <div class="card" style="margin-top: 20px; border: 1px solid rgba(239, 68, 68, 0.35); background: rgba(239, 68, 68, 0.04);">
                <div class="card-header-row" style="margin-bottom: 12px;">
                  <div>
                    <h4 class="card-title" style="margin: 0 0 4px 0; color: #ef4444;">{$_("danger_zone", {}, "Danger Zone")}</h4>
                    <p class="card-subtitle" style="margin: 0;">{$_("danger_zone_sub", {}, "Irreversible actions for your decentralized identity and account.")}</p>
                  </div>
                </div>
                <div class="setting-row" style="border-bottom: none; padding-bottom: 0;">
                  <div class="row-text">
                    <span class="row-label" style="color: #ef4444;">{$_("delete_account", {}, "Delete Account")}</span>
                    <span class="row-sub">{$_("delete_account_sub", {}, "Permanently delete your profile, credentials, and remove your DID from this device.")}</span>
                  </div>
                  <button class="btn-ghost-danger" on:click={() => { deleteAccountPassword = ""; showDeleteAccountModal = true; }}>
                    {$_("delete_account_btn", {}, "Delete Account")}
                  </button>
                </div>
              </div>
            {:else}
              <!-- Upgrade to Permanent DID Card (for guest/ephemeral users) -->
              <div class="card upgrade-card">
                <div class="upgrade-header">
                  <div class="upgrade-icon">
                    <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 2l-2 2m-7.61 7.61a5.5 5.5 0 1 1-7.778 7.778 5.5 5.5 0 0 1 7.777-7.777zm0 0L15.5 7.5m0 0l3 3L22 7l-3-3m-3.5 3.5L19 4"/></svg>
                  </div>
                  <div>
                    <h4 class="card-title" style="margin: 0 0 4px 0;">Create or Link Decentralized ID</h4>
                    <p class="card-subtitle" style="margin: 0;">Sign in or create an ID to access files accross multiple devices.</p>
                  </div>
                </div>

                <div class="auth-tabs">
                  <button class="auth-tab-btn {authMode === 'google' ? 'active' : ''}" on:click={() => { authMode = 'google'; authError = ''; }}>
                    Google Sign-In
                  </button>
                  <button class="auth-tab-btn {authMode === 'password' ? 'active' : ''}" on:click={() => { authMode = 'password'; authError = ''; }}>
                    Sign in with ID
                  </button>
                  <button class="auth-tab-btn {authMode === 'register' ? 'active' : ''}" on:click={() => { authMode = 'register'; authError = ''; }}>
                    Create New ID
                  </button>
                </div>

                {#if authMode === 'google'}
                  <div class="auth-tab-pane">
                    <p class="auth-pane-desc">Sign in with your Google account to get an instant permanent GunDB DID.</p>
                    <div class="google-btn-wrapper">
                      {#if isNativeMobilePlatform()}
                        <button class="google-custom-btn" style="pointer-events: auto;" disabled={authLoading} on:click={handleSettingsMobileGoogle}>
                          <svg viewBox="0 0 24 24" width="18" height="18" style="flex-shrink:0"><path fill="#4285F4" d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92a5.06 5.06 0 0 1-2.2 3.32v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.1z"/><path fill="#34A853" d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z"/><path fill="#FBBC05" d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.07H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.93l2.85-2.22.81-.62z"/><path fill="#EA4335" d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.07l3.66 2.84c.87-2.6 3.3-4.53 6.16-4.53z"/></svg>
                          <span>{authLoading ? 'Signing in...' : 'Sign in with Google'}</span>
                        </button>
                      {:else}
                        <button class="google-custom-btn" tabindex="-1">
                          <svg viewBox="0 0 24 24" width="18" height="18" style="flex-shrink:0"><path fill="#4285F4" d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92a5.06 5.06 0 0 1-2.2 3.32v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.1z"/><path fill="#34A853" d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z"/><path fill="#FBBC05" d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.07H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.93l2.85-2.22.81-.62z"/><path fill="#EA4335" d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.07l3.66 2.84c.87-2.6 3.3-4.53 6.16-4.53z"/></svg>
                          <span>{authLoading ? 'Signing in...' : 'Sign in with Google'}</span>
                        </button>
                        <div use:googleButtonAction class="google-overlay"></div>
                      {/if}
                    </div>
                  </div>
                {:else if authMode === 'password'}
                  <form class="auth-tab-pane" on:submit|preventDefault={handlePasswordSignin}>
                    <div class="form-group">
                      <label class="form-label" for="sett-user">{$_("nickname_or_did", {}, "Nickname or DID")}</label>
                      <input class="text-input" id="sett-user" type="text" placeholder={$_("enter_nickname_or_did_placeholder", {}, "Nickname (e.g. Alice), Name#tag, or did:gun:...")} bind:value={signinUser} />
                    </div>
                    <div class="form-group">
                      <label class="form-label" for="sett-pass">{$_("password", {}, "Password")}</label>
                      <input class="text-input" id="sett-pass" type="password" placeholder={$_("enter_password_placeholder", {}, "Enter password")} bind:value={signinPass} />
                    </div>
                    <div style="display: flex; flex-wrap: wrap; gap: 10px;">
                      <button class="btn-primary-action" type="submit" disabled={authLoading}>
                        {#if authLoading}
                          <div class="login-spinner"></div>
                          <span>{$_("signin", {}, "Signing in...")}</span>
                        {:else}
                          <span>{$_("signin_with_id", {}, "Sign In with ID")}</span>
                        {/if}
                      </button>
                      <button type="button" class="btn-secondary-action" disabled={authLoading} on:click={() => settingsKeyfileInput && settingsKeyfileInput.click()}>
                        <span>{$_("signin_with_keyfile", {}, "Sign in with Keyfile")}</span>
                      </button>
                    </div>
                  </form>
                {:else if authMode === 'register'}
                  <form class="auth-tab-pane" on:submit|preventDefault={handleRegisterNew}>
                    <div class="avatar-edit-controls mb-2" style="justify-content: center; width: 100%;">
                      <div class="avatar-preview-box">
                        {#if regAvatar && !regAvatarFailed}
                          <img
                            src={regAvatar}
                            alt=""
                            referrerpolicy="no-referrer"
                            on:error={() => { regAvatarFailed = true; }}
                          />
                        {:else}
                          <div class="avatar-fallback-medium">
                            {(regUser || "U").charAt(0).toUpperCase()}
                          </div>
                        {/if}
                      </div>
                      <div class="avatar-buttons-col">
                        <label class="btn-secondary-action btn-sm">
                          <span>{regAvatar ? 'Change Photo' : 'Add Photo'}</span>
                          <input type="file" accept="image/*" on:change={handleRegAvatarChange} style="display: none;" disabled={authLoading} />
                        </label>
                        {#if regAvatar}
                          <button type="button" class="btn-ghost-danger btn-sm" on:click={() => { regAvatar = ""; }} disabled={authLoading}>
                            Remove
                          </button>
                        {/if}
                      </div>
                    </div>

                    <div class="form-group">
                      <label class="form-label" for="sett-reg-user">{$_("display_nickname_label", {}, "Nickname / Display Name")}</label>
                      <input class="text-input" id="sett-reg-user" type="text" placeholder={$_("choose_nickname_placeholder", {}, "Any display name (e.g. Alice)")} bind:value={regUser} disabled={authLoading} />
                      <span class="row-sub" style="display: block; margin-top: 4px;">
                        {$_("decentralized_nickname_hint", {}, "Doesn't need to be unique — your identity is a cryptographic keypair (DID), not a central server account.")}
                      </span>
                    </div>
                    <div class="form-group">
                      <label class="form-label" for="sett-reg-email">Email (optional, for recovery)</label>
                      <input class="text-input" id="sett-reg-email" type="email" placeholder="you@example.com" bind:value={regEmail} disabled={authLoading} />
                    </div>
                    <div class="form-group">
                      <label class="form-label" for="sett-reg-pass">Password</label>
                      <input class="text-input" id="sett-reg-pass" type="password" placeholder="Min. 8 characters" bind:value={regPass} disabled={authLoading} />
                    </div>
                    <div class="form-group">
                      <label class="form-label" for="sett-reg-pass2">Confirm Password</label>
                      <input class="text-input" id="sett-reg-pass2" type="password" placeholder="Confirm password" bind:value={regPassConfirm} disabled={authLoading} />
                    </div>
                    <button class="btn-primary-action" type="submit" disabled={authLoading}>
                      {#if authLoading}
                        <div class="login-spinner"></div>
                        <span>Creating ID...</span>
                      {:else}
                        <span>Create Decentralized ID</span>
                      {/if}
                    </button>
                  </form>
                {/if}

                {#if authError}
                  <p class="error-text mt-3" style="text-align: center;">{authError}</p>
                {/if}
              </div>
            {/if}
          </div>

        <!-- 2. DEVICES & NODES -->
        {:else if activeSection === "devices"}
          <div class="section-header">
            <h2 class="section-title">{$_("devices_section_title", {}, "Devices & Nodes")}</h2>
            <p class="section-desc">{$_("devices_section_desc", {}, "View and manage all storage nodes and devices linked under your DID.")}</p>
          </div>

          <div class="card-group">
            {#if $profileStore.isPersistent}
              <!-- Add Device Card -->
              <div class="card">
                <h4 class="card-title">{$_("connect_new_device", {}, "Connect a New Device")}</h4>
                <p class="card-subtitle">{$_("connect_new_device_desc", {}, "Enter the 10-character Link ID of your Disk Direct node (e.g. Raspberry Pi, NAS, or Desktop client).")}</p>
                
                <div class="connect-input-row">
                  <input
                    type="text"
                    class="text-input"
                    placeholder={$_("enter_linkid_placeholder", {}, "Enter device Link ID")}
                    bind:value={newLinkId}
                    disabled={connectingDevice}
                  />
                  <button
                    class="btn-primary-action"
                    on:click={handleConnectNewDevice}
                    disabled={connectingDevice || !newLinkId.trim()}
                  >
                    {connectingDevice ? $_("connecting", {}, "Connecting...") : $_("pair_connect_btn", {}, "Connect")}
                  </button>
                </div>
                {#if connectError}
                  <p class="error-text">{connectError}</p>
                {/if}
              </div>
            {/if}

            <!-- Connected Devices List -->
            <div class="card">
              <h4 class="card-title">{$_("active_devices", {}, "Active Devices")} ({devices.length})</h4>
              {#if devices.length === 0}
                <div class="empty-state">
                  <svg viewBox="0 0 24 24" width="36" height="36" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="2" y="3" width="20" height="14" rx="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>
                  <p>{$_("no_devices_connected", {}, "No devices currently connected to this session.")}</p>
                  {#if $profileStore.isPersistent}
                    <span>{$_("connect_first_device_hint", {}, "Connect your first device above using its Link ID.")}</span>
                  {/if}
                </div>
              {:else}
                <div class="devices-grid">
                  {#each devices as dev}
                    <div class="device-item-card">
                      <div class="device-icon-wrap">
                        <svg viewBox="0 0 24 24" width="24" height="24" fill="currentColor"><path d="M4 5a2 2 0 0 0-2 2v2a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2H4zm0 8a2 2 0 0 0-2 2v2a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-2a2 2 0 0 0-2-2H4zm14-5a1 1 0 1 1 0-2 1 1 0 0 1 0 2zm0 8a1 1 0 1 1 0-2 1 1 0 0 1 0 2z"/></svg>
                      </div>
                      <div class="device-details">
                        <span class="device-name">{dev.name || dev.linkId}</span>
                        <div class="device-meta">
                          <span class="device-linkid">Link ID: <code>{dev.linkId}</code></span>
                        </div>
                      </div>
                      <div class="device-status-wrap">
                        {#if dev.status === 'connected'}
                          {@const iceType = dev.drive?.iceConnectionType || 'direct'}
                          <span class="badge-pill {iceType === 'relay' ? 'relay' : 'direct'}" title={iceType === 'relay' ? 'Relayed via TURN' : 'Direct P2P'}>
                            {iceType === 'relay' ? '⚡ Relay' : '⚡ Direct'}
                          </span>
                        {/if}
                        <span class="badge-pill {dev.status === 'connected' ? 'success' : 'neutral'}">
                          {dev.status === 'connected' ? $_("online", {}, "Online") : dev.status}
                        </span>
                        <button
                          class="btn-icon-danger"
                          title={$_("disconnect_forget_device", {}, "Disconnect & Forget Device")}
                          on:click={() => { deviceToDelete = dev; }}
                        >
                          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                            <path d="M3 6h18M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M10 11v6M14 11v6"/>
                          </svg>
                        </button>
                      </div>
                    </div>
                  {/each}
                </div>
              {/if}
            </div>

            <!-- Detected Devices Card -->
            <div class="card">
              <div class="detected-header-row">
                <div>
                  <h4 class="card-title" style="margin: 0 0 6px 0;">{$_("detected_devices", {}, "Detected Devices")} ({detectedDevices.length})</h4>
                  <p class="card-subtitle" style="margin: 0; line-height: 1.4;">{$_("detected_devices_desc", {}, "Storage nodes discovered in your local network.")}</p>
                </div>
                <div class="detected-header-actions">
                  <button
                    class="btn-secondary-action btn-sm"
                    on:click={refreshDetected}
                    disabled={scanningDetected}
                    style="display: inline-flex; align-items: center; gap: 6px;"
                  >
                    <svg class:spin={scanningDetected} viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2.2">
                      <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
                    </svg>
                    <span>{scanningDetected ? $_("scanning", {}, "Scanning...") : $_("rescan_network", {}, "Rescan")}</span>
                  </button>

                  {#if hasCameraSupport}
                    <button
                      class="btn-secondary-action btn-sm"
                      on:click={() => dispatch('openQrScanner')}
                      style="display: inline-flex; align-items: center; gap: 6px;"
                    >
                      <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                        <rect x="3" y="3" width="7" height="7" rx="1"></rect>
                        <rect x="14" y="3" width="7" height="7" rx="1"></rect>
                        <rect x="14" y="14" width="7" height="7" rx="1"></rect>
                        <rect x="3" y="14" width="7" height="7" rx="1"></rect>
                      </svg>
                      <span>{$_("scan_qr", {}, "Scan QR")}</span>
                    </button>
                  {/if}
                </div>
              </div>

              {#if detectedDevices.length === 0}
                <div class="empty-state" style="padding: 16px 0;">
                  <p style="margin: 0; font-size: 13px; color: var(--text-muted);">{$_("no_unlinked_detected", {}, "No new unlinked devices detected on your network.")}</p>
                </div>
              {:else}
                <div class="devices-grid" style="margin-top: 12px;">
                  {#each detectedDevices as dev}
                    <div class="device-item-card">
                      <div class="device-icon-wrap">
                        <svg viewBox="0 0 24 24" width="24" height="24" fill="currentColor"><path d="M4 5a2 2 0 0 0-2 2v2a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2H4zm0 8a2 2 0 0 0-2 2v2a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-2a2 2 0 0 0-2-2H4zm14-5a1 1 0 1 1 0-2 1 1 0 0 1 0 2zm0 8a1 1 0 1 1 0-2 1 1 0 0 1 0 2z"/></svg>
                      </div>
                      <div class="device-details">
                        <span class="device-name">{dev.name || dev.linkId}</span>
                        <div class="device-meta">
                          <span class="device-linkid">Link ID: <code>{dev.linkId}</code></span>
                        </div>
                      </div>
                      <div class="device-status-wrap">
                        <button
                          class="btn-primary-action btn-sm"
                          on:click={() => handleConnectDetected(dev)}
                        >
                          {$_("pair_connect_btn", {}, "Connect")}
                        </button>
                      </div>
                    </div>
                  {/each}
                </div>
              {/if}
            </div>
          </div>

        <!-- PHOTO & BACKUP SYNC / PWA FOLDER BACKUP -->
        {:else if activeSection === "sync"}
          <div class="section-header">
            <h2 class="section-title">
              {$syncState.isNative ? $_("camera_sync_title", {}, "Photo & Backup Sync") : $_("pwa_folder_sync_title", {}, "Folder & File Backup")}
            </h2>
            <p class="section-desc">
              {$syncState.isNative ? $_("camera_sync_desc", {}, "Direct peer-to-peer backup of your mobile photos, videos, and documents to your home storage nodes.") : $_("pwa_folder_sync_desc", {}, "Direct peer-to-peer backup of your local folders and files to your home storage nodes.")}
            </p>
          </div>

          <div class="card-group">
            <!-- Sync Statistics & Source Selection -->
            <div class="card">
              <div style="display: flex; align-items: flex-start; justify-content: space-between; gap: 16px; margin-bottom: 16px;">
                <div>
                  <h4 class="card-title" style="margin: 0 0 6px 0;">
                    {$syncState.isNative ? $_("camera_roll_status", {}, "Camera Roll & Media Status") : $_("local_folder_label", {}, "Source Local Folder:")}
                  </h4>
                  <p class="card-subtitle" style="margin: 0;">
                    {#if $syncState.isNative}
                      {$_("camera_roll_sub", {}, "Scanned automatically from your device's Camera & Gallery.")}
                    {:else if $syncState.localFolderName}
                      <span style="color: var(--accent-blue); font-weight: 600;">📁 {$syncState.localFolderName}</span>
                    {:else}
                      {$_("no_local_folder_selected", {}, "No local folder selected yet")}
                    {/if}
                  </p>
                </div>
                <div style="display: flex; align-items: center; gap: 8px;">
                  {#if !$syncState.isNative}
                    <button
                      class="btn-secondary-action btn-sm"
                      on:click={handleSelectLocalPwaFolder}
                      disabled={$syncState.isSyncing}
                      style="display: inline-flex; align-items: center; gap: 6px;"
                    >
                      <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                        <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path>
                      </svg>
                      <span>{$syncState.localFolderName ? $_("change_local_folder", {}, "Change Folder...") : $_("select_local_folder", {}, "Choose Local Folder...")}</span>
                    </button>
                  {/if}
                  <button
                    class="btn-secondary-action btn-sm"
                    on:click={handleScanMedia}
                    disabled={$syncState.isSyncing}
                    style="display: inline-flex; align-items: center; gap: 6px;"
                  >
                    <svg class:spin={$syncState.isSyncing} viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2.2">
                      <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
                    </svg>
                    <span>{$_("rescan", {}, "Rescan")}</span>
                  </button>
                </div>
              </div>

              <div class="sync-stats-grid">
                <div class="sync-stat-box">
                  <span class="sync-stat-lbl">{$syncState.isNative ? $_("total_photos", {}, "Total Media") : $_("total_files", {}, "Total Files")}</span>
                  <span class="sync-stat-val">{$syncState.totalMedia}</span>
                </div>
                <div class="sync-stat-box success">
                  <span class="sync-stat-lbl">{$_("synced", {}, "Synced")}</span>
                  <span class="sync-stat-val">{$syncState.syncedMedia}</span>
                </div>
                <div class="sync-stat-box warning">
                  <span class="sync-stat-lbl">{$_("pending_backup", {}, "Pending")}</span>
                  <span class="sync-stat-val">{$syncState.pendingMedia}</span>
                </div>
              </div>

              {#if $syncState.isSyncing}
                <div class="sync-progress-wrap">
                  <div class="sync-progress-bar">
                    <div class="sync-progress-fill" style="width: {$syncState.progressPercent}%"></div>
                  </div>
                  <div class="sync-progress-info">
                    <span class="sync-file-name">{$syncState.statusMsg || $syncState.currentFile}</span>
                    <span class="sync-pct">{$syncState.progressPercent}%</span>
                  </div>
                </div>
              {:else if $syncState.statusMsg}
                <p class="sync-status-msg">{$syncState.statusMsg}</p>
              {/if}

              {#if $syncState.lastSyncTime > 0}
                <p class="sync-last-time">
                  {$_("last_synced_at", {}, "Last synced:")} {new Date($syncState.lastSyncTime).toLocaleString()}
                </p>
              {/if}
            </div>

            <!-- Destination & Controls -->
            <div class="card">
              <h4 class="card-title">{$_("sync_destination", {}, "Backup Destination & Options")}</h4>
              <p class="card-subtitle">{$_("sync_destination_sub", {}, "Select which online storage node will receive your camera backup over encrypted P2P.")}</p>

              <!-- 1. Destination Node -->
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("target_storage_node", {}, "Destination Storage Node")}</span>
                  <span class="row-sub">{$_("target_node_sub", {}, "Computer or NAS running Disk Direct")}</span>
                </div>
                <div class="row-input-wrap">
                  <select class="text-input select-input" bind:value={selectedSyncTarget} disabled={$syncState.isSyncing}>
                    {#if devices && devices.filter(d => !d.isLocal && !d.isAiNode).length > 0}
                      {#each devices.filter(d => !d.isLocal && !d.isAiNode) as dev}
                        <option value={dev.linkId}>
                          {dev.name || dev.linkId} {isDeviceOnline(dev) ? "🟢 (Online)" : "⚪ (Offline)"}
                        </option>
                      {/each}
                    {:else}
                      <option value="">{$_("no_nodes_available", {}, "No remote storage nodes linked")}</option>
                    {/if}
                  </select>
                </div>
              </div>

              <!-- 2. Time Range Filter -->
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("time_filter", {}, "Time Range")}</span>
                  <span class="row-sub">{$_("time_filter_sub", {}, "Only back up photos and videos taken within this period")}</span>
                </div>
                <div class="row-input-wrap">
                  <select class="text-input select-input" bind:value={selectedTimeRange} on:change={handleFilterChange} disabled={$syncState.isSyncing}>
                    <option value="all">{$_("time_all", {}, "All Time (Full Backup)")}</option>
                    <option value="3m">{$_("time_3m", {}, "Last 3 Months")}</option>
                    <option value="1y">{$_("time_1y", {}, "Last 12 Months")}</option>
                    <option value="5y">{$_("time_5y", {}, "Last 5 Years")}</option>
                  </select>
                </div>
              </div>

              <!-- 3. Backup Sources & Folders (Mobile) -->
              {#if $syncState.isNative}
                <div class="setting-row sources-row">
                  <div class="row-text">
                    <span class="row-label">{$_("backup_sources", {}, "Media Folders & Types")}</span>
                    <span class="row-sub">{$_("backup_sources_sub", {}, "Select which phone folders to include in backup")}</span>
                  </div>
                  <div class="sources-checkboxes-grid">
                    <label class="source-checkbox-item">
                      <input type="checkbox" bind:checked={syncSources.photos} on:change={handleFilterChange} disabled={$syncState.isSyncing} />
                      <span>{$_("source_photos", {}, "Camera & Gallery Photos")}</span>
                    </label>
                    <label class="source-checkbox-item">
                      <input type="checkbox" bind:checked={syncSources.videos} on:change={handleFilterChange} disabled={$syncState.isSyncing} />
                      <span>{$_("source_videos", {}, "Videos & Movies")}</span>
                    </label>
                    <label class="source-checkbox-item">
                      <input type="checkbox" bind:checked={syncSources.screenshots} on:change={handleFilterChange} disabled={$syncState.isSyncing} />
                      <span>{$_("source_screenshots", {}, "Screenshots")}</span>
                    </label>
                    <label class="source-checkbox-item">
                      <input type="checkbox" bind:checked={syncSources.documents} on:change={handleFilterChange} disabled={$syncState.isSyncing} />
                      <span>{$_("source_documents", {}, "Documents & Downloads")}</span>
                    </label>
                  </div>
                </div>
              {/if}

              <!-- 4. Destination Folder -->
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("backup_folder", {}, "Destination Folder")}</span>
                  <span class="row-sub">{$_("backup_folder_sub", {}, "Folder on target node where media will be saved")}</span>
                </div>
                <div class="row-input-wrap" style="display: flex; gap: 8px; align-items: center;">
                  <input
                    class="text-input"
                    type="text"
                    bind:value={customSyncFolder}
                    disabled={$syncState.isSyncing}
                    placeholder={$syncState.isNative ? "/PhoneBackup" : "/DesktopBackup"}
                    style="flex: 1;"
                  />
                  <button
                    class="btn-secondary-action btn-sm"
                    type="button"
                    disabled={$syncState.isSyncing || !selectedSyncTarget}
                    on:click={() => { showSyncFolderPicker = true; }}
                    style="display: inline-flex; align-items: center; gap: 6px; white-space: nowrap; padding: 7px 12px;"
                  >
                    <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                      <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path>
                    </svg>
                    <span>{$_("browse_folder", {}, "Browse...")}</span>
                  </button>
                </div>
              </div>

              <!-- Auto Sync in Background Checkbox -->
              <div class="setting-item" style="margin-top: 14px; margin-bottom: 14px;">
                <label class="source-checkbox-item" style="display: flex; align-items: flex-start; gap: 12px; cursor: pointer;">
                  <input
                    type="checkbox"
                    checked={$syncState.autoSyncEnabled}
                    on:change={(e) => {
                      updateSettings({ autoSyncEnabled: e.target.checked });
                    }}
                    style="margin-top: 2px;"
                  />
                  <div style="display: flex; flex-direction: column; gap: 2px;">
                    <span style="font-weight: 600; font-size: 13.5px; color: var(--text-primary);">
                      {$_("auto_sync_background", {}, "Sync automatically in background")}
                    </span>
                    <span style="font-size: 11.5px; color: var(--text-secondary); line-height: 1.4;">
                      {$_("auto_sync_background_desc", {}, "Automatically back up new photos and files as soon as they are added")}
                    </span>
                  </div>
                </label>
              </div>

              <!-- Live Sync Progress Indicator -->
              {#if $syncState.isSyncing}
                <div class="sync-progress-card" style="margin-bottom: 14px; padding: 12px 14px; border-radius: 10px; background: var(--accent-soft, rgba(142, 116, 189, 0.14)); border: 1px solid var(--accent-border, rgba(142, 116, 189, 0.35));">
                  <div style="display: flex; justify-content: space-between; align-items: center; font-size: 12.5px; font-weight: 600; margin-bottom: 6px;">
                    <span style="color: var(--text-primary); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 75%;">
                      {$syncState.currentFile || $_("syncing", {}, "Syncing...")}
                    </span>
                    <span style="color: var(--accent, #8E74BD); font-family: monospace;">{$syncState.progressPercent || 0}%</span>
                  </div>
                  <div style="width: 100%; height: 6px; border-radius: 3px; background: rgba(255, 255, 255, 0.1); overflow: hidden;">
                    <div style="height: 100%; width: {$syncState.progressPercent || 0}%; background: var(--accent-gradient, linear-gradient(90deg, #7D60AC, #A28AD0)); transition: width 0.2s ease;"></div>
                  </div>
                  <div style="display: flex; justify-content: space-between; font-size: 11.5px; color: var(--text-secondary); margin-top: 6px;">
                    <span style="overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 65%;">{$syncState.statusMsg || ""}</span>
                    <span>{$syncState.syncedMedia || 0} / {$syncState.totalMedia || 0}</span>
                  </div>
                </div>
              {/if}

              <div class="sync-action-buttons">
                {#if $syncState.isSyncing}
                  <button class="btn-danger-outline" on:click={handleStopCameraSync}>
                    {$_("stop_sync", {}, "Cancel Sync")}
                  </button>
                {:else}
                  <button
                    class="btn-primary-action"
                    disabled={!selectedSyncTarget || (!$syncState.isNative && !$syncState.localFolderName)}
                    on:click={handleStartCameraSync}
                    style="display: inline-flex; align-items: center; gap: 8px;"
                  >
                    <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
                      <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M17 8l-5-5-5 5M12 3v12"/>
                    </svg>
                    <span>{$_("sync_btn_label", {}, "Sync")}</span>
                  </button>
                {/if}
              </div>
            </div>

            <!-- How P2P Sync Works Info -->
            <div class="card">
              <h4 class="card-title">{$_("how_it_works", {}, "Decentralized P2P Backup")}</h4>
              <p style="font-size: 13.5px; color: var(--text-secondary); line-height: 1.6; margin: 0;">
                {$_("p2p_sync_explainer", {}, "Your photos and videos are transferred directly between your mobile phone and your chosen computer via encrypted WebRTC DataChannels. No files are ever stored on cloud servers or third-party servers, guaranteeing absolute privacy and maximum transfer speed over LAN or internet.")}
              </p>
            </div>
          </div>

        <!-- 3. NETWORK & P2P -->
        {:else if activeSection === "network"}
          <div class="section-header">
            <h2 class="section-title">{$_("network_section_title", {}, "Network & P2P Architecture")}</h2>
            <p class="section-desc">{$_("network_section_desc", {}, "Decentralized GunDB Relay, WebRTC signaling and mesh connectivity.")}</p>
          </div>

          <div class="card-group">
            <div class="card">
              <h4 class="card-title">{$_("gundb_relay_hub", {}, "GunDB Relay & Authentication Hub")}</h4>
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("relay_ws_endpoint", {}, "Relay WebSocket Endpoint")}</span>
                  <span class="row-sub">{$_("relay_ws_sub", {}, "Encrypted decentralized graph synchronization")}</span>
                </div>
                <div class="code-box-inline">
                  <code>wss://disk.direct:8765/gun</code>
                  <span class="badge-pill success">{$_("connected", {}, "Connected")}</span>
                </div>
              </div>

              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("rest_gateway", {}, "REST Auth & Certificate Gateway")}</span>
                  <span class="row-sub">{$_("rest_gateway_sub", {}, "Issue SEA certificates and manage account links")}</span>
                </div>
                <div class="code-box-inline">
                  <code>https://disk.direct:8765</code>
                  <span class="badge-pill success">{$_("active", {}, "Active")}</span>
                </div>
              </div>
            </div>

            <div class="card">
              <h4 class="card-title">{$_("p2p_datachannels", {}, "P2P DataChannels (WebRTC)")}</h4>
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("direct_transfer", {}, "Direct Node-to-Node Transfer")}</span>
                  <span class="row-sub">{$_("direct_transfer_sub", {}, "Files and terminal streams bypass intermediate servers via WebRTC DataChannel")}</span>
                </div>
                <span class="badge-pill success">{$_("enabled", {}, "Enabled")}</span>
              </div>
            </div>
          </div>

        <!-- 4. APPEARANCE -->
        {:else if activeSection === "appearance"}
          <div class="section-header">
            <h2 class="section-title">{$_("settings_appearance", {}, "Appearance & Theme")}</h2>
            <p class="section-desc">{$_("appearance_desc", {}, "Customize the visual theme and interface preferences.")}</p>
          </div>

          <div class="card-group">
            <div class="card">
              <h4 class="card-title">{$_("color_scheme", {}, "Color Scheme")}</h4>
              <div class="theme-options-grid">
                <button
                  class="theme-card {isDarkTheme ? 'selected' : ''}"
                  on:click={() => dispatch("toggleTheme", { dark: true })}
                >
                  <div class="theme-preview dark-preview">
                    <svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor"><path d="M12 3a9 9 0 1 0 9 9c0-.46-.04-.92-.1-1.36a5.389 5.389 0 0 1-4.4 2.26 5.403 5.403 0 0 1-3.14-9.8c-.44-.06-.9-.1-1.36-.1z"/></svg>
                    <span>{$_("theme_dark", {}, "Dark Theme")}</span>
                  </div>
                </button>
                <button
                  class="theme-card {!isDarkTheme ? 'selected' : ''}"
                  on:click={() => dispatch("toggleTheme", { dark: false })}
                >
                  <div class="theme-preview light-preview">
                    <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="5"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>
                    <span>{$_("theme_light", {}, "Light Theme")}</span>
                  </div>
                </button>
              </div>
            </div>

            <!-- Language Card (single compact row) -->
            <div class="card">
              <div class="setting-row" style="margin: 0;">
                <div class="row-text">
                  <span class="row-label">{$_("settings_language", {}, "Language")}</span>
                  <span class="row-sub">{$_("select_language_desc", {}, "Choose your preferred interface language.")}</span>
                </div>
                <select class="select-styled text-input" style="max-width:240px; font-weight: 500;" value={$locale} on:change={(e) => setLocale(e.target.value)}>
                  {#each SUPPORTED_LANGUAGES as lang}
                    <option value={lang.code}>{lang.nativeName} ({lang.shortCode || lang.code.toUpperCase()})</option>
                  {/each}
                </select>
              </div>
            </div>

            <div class="card">
              <h4 class="card-title">{$_("viewer", {}, "Viewer")}</h4>
              <p class="card-subtitle">{$_("viewer_desc", {}, "Applies to all devices — this is now the single universal client.")}</p>
              <div class="setting-row">
                <div class="row-text"><span class="row-label">{$_("generate_thumbnails", {}, "Generate Thumbnails")}</span><span class="row-sub">{$_("create_previews_desc", {}, "Create previews for files")}</span></div>
                <label class="switch"><input type="checkbox" bind:checked={viewerSettings.generateThumbs} on:change={saveViewerSettings} /><span class="slider"></span></label>
              </div>
              <div class="setting-row">
                <div class="row-text"><span class="row-label">{$_("downscale_images", {}, "Downscale Images")}</span><span class="row-sub">{$_("serve_scaled_desc", {}, "Serve scaled variants")}</span></div>
                <label class="switch"><input type="checkbox" bind:checked={viewerSettings.downscaleImages} on:change={saveViewerSettings} /><span class="slider"></span></label>
              </div>
              <div class="setting-row">
                <div class="row-text"><span class="row-label">{$_("scale_to", {}, "Scale To")}</span><span class="row-sub">{$_("max_preview_dimension", {}, "Max preview dimension")}</span></div>
                <select class="select-styled text-input" style="max-width:140px" bind:value={viewerSettings.scaleTo} on:change={saveViewerSettings}>
                  {#each viewerScaleOptions as v}<option value={v}>{v}px</option>{/each}
                </select>
              </div>
            </div>

            <div class="card">
              <h4 class="card-title">{$_("web_push_title", {}, "Push Notifications")}</h4>
              <p class="card-subtitle">{$_("web_push_desc", {}, "Instant notifications from your AI node directly to this device (with image previews and 1-tap moderation).")}</p>
              {#if !webPushSupported}
                <p class="row-sub" style="margin-top: 8px;">
                  {isNativeApp()
                    ? $_("push_unsupported_native", {}, "Update the Disk Direct mobile app to the latest version to enable native Push notifications.")
                    : $_("push_unsupported_web", {}, "Your browser does not support Web Push (on iOS, add the web app to your Home Screen or use the native app).")}
                </p>
              {:else}
                <div class="setting-row">
                  <div class="row-text">
                    <span class="row-label">{$_("push_receive_label", {}, "Receive Push on this device")}</span>
                    <span class="row-sub">{isNativeApp() ? $_("push_sub_native", {}, "Native system Push notifications (Android FCM / iOS APNs)") : $_("push_sub_web", {}, "Direct VAPID subscription over P2P without third-party messengers")}</span>
                  </div>
                  <label class="switch" style={webPushBusy ? "opacity: 0.6; pointer-events: none;" : ""}>
                    <input
                      type="checkbox"
                      checked={webPushSubscribed}
                      disabled={webPushBusy}
                      on:change={async (e) => {
                        const el = e.currentTarget;
                        await handleToggleWebPush();
                        if (el) el.checked = webPushSubscribed;
                      }}
                    />
                    <span class="slider"></span>
                  </label>
                </div>
                {#if webPushSubscribed}
                  <div class="setting-row">
                    <div class="row-text">
                      <span class="row-label">{$_("push_test_label", {}, "Test notifications")}</span>
                      <span class="row-sub">{$_("push_test_sub", {}, "Send a test push notification to this device")}</span>
                    </div>
                    <div class="row-value-badge">
                      <button
                        type="button"
                        class="btn-secondary-action btn-sm"
                        disabled={webPushBusy}
                        on:click={handleTestWebPush}
                        style="padding: 4px 12px; font-size: 12px;"
                      >
                        <span>{webPushBusy ? "..." : $_("push_test_btn", {}, "Test")}</span>
                      </button>
                    </div>
                  </div>
                {/if}
                {#if webPushMsgKey || webPushMsg}
                  <div style="margin-top: 8px; font-size: 12.5px; color: {webPushMsgType === 'error' ? '#ef5350' : (webPushMsgType === 'success' ? '#2ac769' : 'inherit')};">
                    {webPushMsgKey ? $_(webPushMsgKey, {}, webPushMsg) : webPushMsg}
                  </div>
                {/if}
              {/if}
            </div>
          </div>

        <!-- 5. AI ASSISTANT -->
        {:else if activeSection === "assistant"}
          <!-- 5. AI Node Section -->
          <div class="section-header">
            <h2 class="section-title">{$_("ai_node", {}, "AI Node")}</h2>
            <p class="section-desc">{$_("ai_node_desc", {}, "Choose the AI node your chats connect to: your own node or the public one.")}</p>
          </div>

          <div class="card-group">
            <!-- 5. AI Node Selection (public vs own device) -->
            <div class="card ai-node-card">
              <div class="field-item">
                <label class="field-label" for="aiNodeSelect">{$_("ai_node", {}, "AI Node")}:</label>
                <div class="ai-node-select-row">
                  <select
                    id="aiNodeSelect"
                    class="text-input-styled"
                    value={aiNodeSelectValue}
                    on:change={onAiNodeChange}
                  >
                    <option value="auto">{$_("ai_node_auto", {}, "Auto (own AI node)")}</option>
                    {#if aiNodeDeviceOptions.length > 0}
                      {#each aiNodeDeviceOptions as opt}
                        <option value={opt.value}>{opt.label}</option>
                      {/each}
                    {/if}
                    {#if customAiNodes.length > 0}
                      {#each customAiNodes as cNode}
                        <option value={cNode.linkId}>{cNode.name ? `${cNode.name} (${$_("guest_node_tag", {}, "гостевая")})` : cNode.linkId}</option>
                      {/each}
                    {/if}
                    <option value="public">{$_("public_ai_node_name", {}, "AI Assistant Public Node")}</option>
                    <option value="custom">{$_("ai_node_custom", {}, "Указанная...")}</option>
                  </select>
                  {#if isCurrentCustomNode}
                    <button
                      class="icon-btn-danger"
                      type="button"
                      title={$_("forget_node", {}, "Забыть эту ноду")}
                      on:click={() => handleForgetCustomNode(aiNodeChoice)}
                    >
                      <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
                    </button>
                  {/if}
                </div>
              </div>
              {#if isSpecifyingCustomNode}
                <div class="field-item custom-node-block">
                  <label class="field-label" for="customNodeLinkId">{$_("link_id_label", {}, "Идентификатор:")}</label>
                  <div class="custom-node-input-row">
                    <input
                      id="customNodeLinkId"
                      type="text"
                      class="text-input-styled custom-node-input"
                      bind:value={customNodeInput}
                      placeholder="LinkID"
                      disabled={connectingCustomNode}
                      spellcheck="false"
                      on:keydown={(e) => { if (e.key === 'Enter') handleConnectCustomNode(); }}
                    />
                    <button
                      class="connect-btn"
                      type="button"
                      on:click={handleConnectCustomNode}
                      disabled={!customNodeInput.trim() || connectingCustomNode}
                    >
                      {#if connectingCustomNode}
                        <div class="spinner-sm"></div>
                        <span>{$_("connecting", {}, "Connecting...")}</span>
                      {:else}
                        <span>{$_("connect", {}, "Connect")}</span>
                      {/if}
                    </button>
                  </div>
                  {#if customNodeError}
                    <div class="custom-node-err">{customNodeError}</div>
                  {/if}
                </div>
              {/if}
              {#if isNodeOwner}
                <div class="field-item">
                  <label class="field-label" for="aiModeSelect">{$_("ai_mode", {}, "Runtime mode")}:</label>
                  <select
                    id="aiModeSelect"
                    class="text-input-styled"
                    value={aiMode}
                    on:change={onAiModeChange}
                  >
                    <option value={AI_MODE_LOCAL}>{$_("ai_mode_local", {}, "Local (all inference on this node)")}</option>
                    <option value={AI_MODE_HYBRID}>{$_("ai_mode_hybrid", {}, "Hybrid (AI Horde for fun mode)")}</option>
                    <option value={AI_MODE_AGENTIC}>{$_("ai_mode_agentic", {}, "Agentic (local index + remote inference)")}</option>
                  </select>
                </div>
                {#if aiMode === AI_MODE_HYBRID}
                  <div class="field-item">
                    <label class="field-label" for="hordeApiKey">{$_("horde_api_key", {}, "Horde API Key (0000000000 = anonymous)")}:</label>
                    <div class="token-input-wrap">
                      <input
                        id="hordeApiKey"
                        type={hordeKeyRevealed ? "text" : "password"}
                        class="text-input-styled token-input"
                        bind:value={hordeApiKey}
                        on:change={handleHordeKeyChange}
                        autocomplete="off"
                        spellcheck="false"
                        placeholder="0000000000"
                      />
                      <button
                        class="icon-btn"
                        type="button"
                        title={hordeKeyRevealed ? $_("hide_key", {}, "Hide") : $_("show_key", {}, "Show")}
                        on:click={() => hordeKeyRevealed = !hordeKeyRevealed}
                      >
                        {#if hordeKeyRevealed}
                          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></svg>
                        {:else}
                          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>
                        {/if}
                      </button>
                      <button
                        class="icon-btn"
                        type="button"
                        title={$_("copy", {}, "Copy")}
                        on:click={copyHordeKey}
                      >
                        {#if hordeKeyCopied}
                          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 6L9 17l-5-5"/></svg>
                        {:else}
                          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v3"/></svg>
                        {/if}
                      </button>
                    </div>
                  </div>

                  <div class="field-item">
                    <div class="field-label-row">
                      <label class="field-label" for="hordeModelSelect">{$_("horde_model", {}, "Horde Model")}:</label>
                      <button
                        type="button"
                        class="refresh-link-btn"
                        disabled={loadingHordeModels}
                        on:click={fetchHordeModels}
                        title="Refresh active models from AI Horde"
                      >
                        {#if loadingHordeModels}
                          <span class="spinner-sm" style="border-width: 1.5px; width: 10px; height: 10px;"></span>
                          <span>{$_("loading", {}, "Loading...")}</span>
                        {:else}
                          <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2"><path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/></svg>
                          <span>{$_("refresh_models", {}, "Refresh list")}</span>
                        {/if}
                      </button>
                    </div>

                    <select
                      id="hordeModelSelect"
                      value={selectedHordeModel}
                      on:change={handleHordeModelChange}
                      class="text-input-styled"
                    >
                      <optgroup label={$_("horde_models_list", {}, "Active & Popular Models")}>
                        {#each hordeModels as m}
                          <option value={m.id}>{m.label}</option>
                        {/each}
                      </optgroup>
                      <option value="custom">{$_("horde_model_custom", {}, "Custom model name...")}</option>
                    </select>

                    {#if isCustomHordeModel}
                      <input
                        type="text"
                        bind:value={hordeModel}
                        on:change={handleHordeModelCustomChange}
                        placeholder="e.g. aphrodite/TheDrummer/Behemoth-X-123B-v2.1"
                        class="text-input-styled"
                        style="margin-top: 4px;"
                      />
                    {/if}
                  </div>
                {/if}
                {#if aiMode === AI_MODE_AGENTIC}
                  <div class="field-item">
                    <label class="field-label" for="aiInferNodeSelect">{$_("ai_inference_node", {}, "Inference node")}:</label>
                    <select
                      id="aiInferNodeSelect"
                      class="text-input-styled"
                      value={inferenceNodeId}
                      on:change={onInferenceNodeChange}
                    >
                      {#if $aiNodeStore.info}
                        <option value={$aiNodeStore.info.linkID}>{$_("public_ai_node_name", {}, "AI Assistant Public Node")}</option>
                      {/if}
                      {#each aiNodeDeviceOptions as opt}
                        <option value={opt.value}>{opt.label}</option>
                      {/each}
                    </select>
                    <p class="card-subtext">{$_("ai_infer_node_desc", {}, "When Agentic is selected, pick the node carrying Ollama and ComfyUI. This node keeps local knowledge indexing on CPU.")}</p>
                    {#if !$aiNodeStore.info}
                      <p class="card-subtext">{$_("ai_node_public_unavailable", {}, "The public AI node is currently offline or unreachable.")}</p>
                    {/if}
                  </div>
                {/if}
              {/if}
              {#if aiNodeStatusMsg}
                <div class="save-status-msg success"><span>{aiNodeStatusMsg}</span></div>
              {/if}
              {#if aiNodeChoice === "public" && !$aiNodeStore.info}
                <p class="card-subtext">{$_("ai_node_public_unavailable", {}, "The public AI node is currently offline or unreachable.")}</p>
              {/if}
            </div>
          </div>

          <!-- 5a. Assistant Settings Section -->
          <div class="section-header section-header-spaced">
            <h2 class="section-title">{$_("assistant_settings", {}, "AI Assistant Settings")}</h2>
            <p class="section-desc">{$_("assistant_settings_desc", {}, "Configure your private assistant's identity, personality, fun mode, and storage.")}</p>
          </div>

          <div class="card-group">
            <!-- 5a. Profile & Appearance Card -->
            <div class="card">
              <div class="card-header-flex">
                <h4 class="card-title">{$_("assistant_profile", {}, "Assistant Profile")}</h4>
                <div class="card-actions">
                  <input
                    type="file"
                    accept="image/png"
                    bind:this={characterFileInput}
                    on:change={handleCharacterSelected}
                    style="display: none;"
                  />
                  <button
                    type="button"
                    class="btn-secondary-action btn-sm"
                    disabled={isImportingCharacter}
                    on:click={() => characterFileInput && characterFileInput.click()}
                    title={$_("import_character_tooltip", {}, "Import SillyTavern Character Card (PNG)")}
                    style="display: inline-flex; align-items: center; gap: 6px; padding: 6px 12px; font-size: 0.82rem;"
                  >
                    <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                      <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
                      <polyline points="17 8 12 3 7 8"/>
                      <line x1="12" y1="3" x2="12" y2="15"/>
                    </svg>
                    <span>{isImportingCharacter ? $_("importing", {}, "Importing...") : $_("import_character", {}, "Import Character")}</span>
                  </button>
                </div>
              </div>

              <div class="assistant-hero-profile">
                <div class="assistant-avatar-box">
                  <button
                    type="button"
                    class="assistant-avatar-preview-btn"
                    on:click={() => (showAvatarModal = true)}
                    title={$_("edit_avatar", {}, "Customize appearance and avatar")}
                  >
                    {#if assistantAvatarUrl}
                      <img src={assistantAvatarUrl} alt={assistantName} class="assistant-img" on:error={handleAvatarError} />
                    {:else}
                      <img src="/img/june.png" alt={assistantName} class="assistant-img" on:error={handleAvatarError} />
                    {/if}
                  </button>
                  <button
                    type="button"
                    class="avatar-label-btn"
                    on:click={() => (showAvatarModal = true)}
                  >
                    {$_("customize_appearance", {}, "Customize Appearance")}
                  </button>
                </div>

                <div class="assistant-identity-fields">
                  <div class="field-item">
                    <label class="field-label" for="asstName">{$_("name", {}, "Assistant Name")}:</label>
                    <input
                      id="asstName"
                      type="text"
                      class="text-input-styled"
                      bind:value={assistantName}
                      placeholder="June"
                    />
                  </div>

                  <div class="field-item">
                    <label class="field-label" for="asstTitle">{$_("title", {}, "Role / Title")}:</label>
                    <input
                      id="asstTitle"
                      type="text"
                      class="text-input-styled"
                      bind:value={assistantTitle}
                      placeholder="Junior assistant"
                    />
                  </div>

                  <div class="field-item">
                    <label class="field-label" for="asstUserName">{$_("call_you", {}, "How should assistant address you?")}:</label>
                    <input
                      id="asstUserName"
                      type="text"
                      class="text-input-styled"
                      bind:value={assistantUserName}
                      placeholder={identity?.profile?.user || "User"}
                    />
                  </div>
                </div>
              </div>
            </div>

            <!-- 5b. Personality & System Prompts (Work vs Fun Mode) Card -->
            <div class="card">
              <div class="card-header-flex">
                <h4 class="card-title">{$_("personality", {}, "Personality & System Prompts")}</h4>
                <div class="prompt-mode-indicator">
                  {#if promptMode === "work"}
                    <span class="mode-badge work">💼 {$_("work_mode", {}, "Work Mode")}</span>
                  {:else}
                    <span class="mode-badge fun">🎭 {$_("fun_mode", {}, "Fun Mode")}</span>
                  {/if}
                </div>
              </div>

              <!-- Textarea for active mode prompt -->
              <div class="prompt-editor-box">
                {#if promptMode === "work"}
                  <textarea
                    class="prompt-textarea"
                    rows="6"
                    bind:value={assistantPersonality}
                    placeholder={$_("work_prompt_placeholder", {}, "Enter custom system prompt for work mode (instructions, tone, reasoning style, and task behavior)...")}
                  ></textarea>
                {:else}
                  <textarea
                    class="prompt-textarea fun-tint"
                    rows="6"
                    bind:value={assistantFunPrompt}
                    placeholder={$_("fun_prompt_placeholder", {}, "Enter custom system prompt for fun mode (witty humor, casual conversation, jokes, banter, and entertainment)...")}
                  ></textarea>
                {/if}
              </div>

              <!-- Switcher under the editor as requested by user -->
              <div class="prompt-switch-row">
                <div class="prompt-mode-toggle">
                  <button
                    type="button"
                    class="mode-toggle-btn"
                    class:active={promptMode === "work"}
                    on:click={() => (promptMode = "work")}
                  >
                    <span class="mode-btn-icon">💼</span>
                    <span class="mode-btn-text">{$_("work_mode", {}, "Work")}</span>
                  </button>
                  <button
                    type="button"
                    class="mode-toggle-btn"
                    class:active={promptMode === "fun"}
                    on:click={() => (promptMode = "fun")}
                  >
                    <span class="mode-btn-icon">🎭</span>
                    <span class="mode-btn-text">{$_("fun_mode", {}, "Fun")}</span>
                  </button>
                </div>

                <div class="prompt-mode-desc">
                  {#if promptMode === "work"}
                    <span>{$_("work_prompt_desc", {}, "Defines assistant behavior for problem-solving, document analysis, and tasks.")}</span>
                  {:else}
                    <span>{$_("fun_prompt_desc", {}, "Defines assistant persona when unwinding, sharing humor, or casual chat.")}</span>
                  {/if}
                </div>
              </div>
            </div>

            <!-- 5c. Storage & Output Card -->
            <div class="card">
              <h4 class="card-title">{$_("generated_files_saved_to", {}, "Generated Files & Memory Storage")}</h4>
              <p class="card-subtext">{$_("storage_path_sub", {}, "Destination folder for images, avatar candidates, and indexed knowledge.")}</p>
              
              <div class="storage-picker-row" style="display: flex; gap: 10px; align-items: center; margin-top: 10px;">
                <input
                  type="text"
                  class="text-input"
                  style="flex: 1;"
                  bind:value={assistantStorage}
                  placeholder="/[Device]/OnMyDisk"
                />
                <button
                  type="button"
                  class="btn-secondary-action btn-sm"
                  on:click={handleBrowseStorage}
                  title={$_("select_destination_folder", {}, "Select destination folder")}
                  style="display: inline-flex; align-items: center; gap: 6px; white-space: nowrap; padding: 7px 12px;"
                >
                  <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path>
                  </svg>
                  <span>{$_("browse_folder", {}, "Browse...")}</span>
                </button>
              </div>
              {#if parsedStorageInfo?.label}
                <div style="margin-top: 8px; font-size: 12px; color: var(--text-muted); display: flex; align-items: center; gap: 6px;">
                  <span style="opacity: 0.7;">{$_("destination_target", {}, "Target:")}</span>
                  <span class="badge" style="font-size: 11px; padding: 2px 8px; border-radius: 4px; background: var(--bg-hover, rgba(255,255,255,0.06)); font-weight: 500;">
                    {parsedStorageInfo.label}
                  </span>
                  <span style="opacity: 0.7; font-family: monospace;">{parsedStorageInfo.folder}</span>
                </div>
              {/if}
            </div>

            <!-- 5d. Save & Feedback Action Bar -->
            <div class="assistant-save-bar">
              {#if assistantSaveMessage}
                <div class="save-status-msg {assistantSaveType}">
                  {#if assistantSaveType === "success"}
                    <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.5">
                      <polyline points="20 6 9 17 4 12" />
                    </svg>
                  {:else}
                    <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                      <circle cx="12" cy="12" r="10" />
                      <line x1="12" y1="8" x2="12" y2="12" />
                      <line x1="12" y1="16" x2="12.01" y2="16" />
                    </svg>
                  {/if}
                  <span>{assistantSaveMessage}</span>
                </div>
              {/if}
              <button
                type="button"
                class="btn-primary-lg"
                on:click={saveAssistantConfig}
                disabled={assistantSaving}
              >
                {#if assistantSaving}
                  <span class="spinner-inline"></span>
                  {$_("saving", {}, "Saving...")}
                {:else}
                  <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z" />
                    <polyline points="17 21 17 13 7 13 7 21" />
                    <polyline points="7 3 7 8 15 8" />
                  </svg>
                  {$_("save_assistant_settings", {}, "Save Assistant Settings")}
                {/if}
              </button>
            </div>
          </div>

        <!-- 6. SECURITY & KEYS -->
        {:else if activeSection === "security"}
          <div class="section-header">
            <h2 class="section-title">{$_("security_section_title", {}, "Security & Key Management")}</h2>
            <p class="section-desc">{$_("security_section_desc", {}, "Manage cryptographic keypairs, certificates, and session security.")}</p>
          </div>

          <div class="card-group">
            <div class="card">
              <h4 class="card-title">{$_("did_keyfile_backup_title", {}, "DID Keypair Backup (.json)")}</h4>
              <p class="omd-key-hint">
                {$_("did_keyfile_backup_desc", {}, "Export your cryptographic DID keypair as a portable .json file in one click. You can use this file to sign in instantly on any browser or device without entering a password.")}
              </p>
              <div style="display: flex; flex-wrap: wrap; align-items: center; gap: 10px; margin-top: 10px;">
                {#if identity?.mainPub}
                  <button class="btn-primary-action btn-sm" on:click={handleExportDidKeyfile} style="display: inline-flex; align-items: center; gap: 6px;">
                    <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2.2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3"/></svg>
                    <span>{$_("save_keyfile_json_btn", {}, "Save Keyfile")}</span>
                  </button>
                {/if}
                <button class="btn-secondary-action btn-sm" on:click={() => settingsKeyfileInput && settingsKeyfileInput.click()} style="display: inline-flex; align-items: center; gap: 6px;">
                  <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2.2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M17 8l-5-5-5 5M12 3v12"/></svg>
                  <span>{$_("load_keyfile_btn", {}, "Load Keyfile")}</span>
                </button>
                {#if keyfileExportMsg}
                  <span class="save-feedback-success">{keyfileExportMsg}</span>
                {/if}
                {#if keyfileExportErr}
                  <span class="save-feedback-error">{keyfileExportErr}</span>
                {/if}
              </div>
            </div>

            <div class="card">
              <h4 class="card-title">{$_("device_public_key", {}, "Device Public Key")}</h4>
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$_("local_device_pubkey", {}, "Local Device Pubkey")}</span>
                  <span class="row-sub">{$_("local_device_pubkey_sub", {}, "Unique keypair generated in this browser (private key never leaves device)")}</span>
                </div>
                <div class="code-box">
                  <code>{identity?.devicePair?.pub || "Local device key active"}</code>
                  <button class="copy-btn" on:click={() => copyToClipboard(identity?.devicePair?.pub, 'devicePub')}>
                    {copiedField === 'devicePub' ? $_("copied", {}, "Copied!") : $_("copy", {}, "Copy")}
                  </button>
                </div>
              </div>
            </div>

            <!-- 6b. ONMYCHAT API KEY -->
            <div class="card">
              <h4 class="card-title">{$_("omd_api_key", {}, "Disk Direct API Key")}</h4>
              <p class="omd-key-hint">
                {$_("omd_key_hint", {}, "API key to access your AI chats and knowledge base across multiple devices and from external apps. Stored encrypted in your DID graph — only this browser's identity can read it.")}
              </p>
              <div class="omd-key-editor">
                <div class="omd-key-field">
                  <input
                    class="omd-key-input"
                    type={showOmdKey ? "text" : "password"}
                    placeholder="32-character hex key (e.g. a3f…)"
                    bind:value={omdKeyInput}
                    spellcheck="false"
                    autocomplete="off"
                  />
                  <button
                    class="eye-btn"
                    type="button"
                    title={showOmdKey ? $_("hide_key", {}, "Hide key") : $_("show_key", {}, "Show key")}
                    on:click={() => (showOmdKey = !showOmdKey)}
                  >
                    {#if showOmdKey}
                      <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></svg>
                    {:else}
                      <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>
                    {/if}
                  </button>
                </div>
                <button class="btn-primary-sm subtle" disabled={!isOmdKeyInputValid} on:click={() => copyToClipboard(omdKeyInput.trim().toLowerCase(), 'omdKey')}>
                  {copiedField === 'omdKey' ? $_("copied", {}, "Copied!") : $_("copy", {}, "Copy")}
                </button>
                <button class="btn-primary-sm" disabled={!canSaveOmdKey} on:click={saveOmdKey}>{$_("save", {}, "Save")}</button>
                {#if $omdKeyStore.valid}
                  <button class="btn-danger-outline" on:click={removeOmdKey}>{$_("remove", {}, "Remove")}</button>
                {/if}
              </div>
              {#if omdKeyMessage}
                <p class="omd-key-message {omdKeyMessageType}">{omdKeyMessage}</p>
              {:else if omdKeyInput && !isOmdKeyInputValid}
                <p class="omd-key-message error">{$_("invalid_key_format", {}, "The key must be exactly 32 hexadecimal characters (0-9, a-f).")}</p>
              {/if}
            </div>

            <!-- 6c. MCP API KEYS -->
            <div class="card">
              <div class="mcp-card-header">
                <div class="mcp-header-text">
                  <h4 class="card-title">{$_("mcp_api_keys", {}, "MCP API Keys")}</h4>
                  <p class="omd-key-hint">
                    {isIosApp
                      ? $_("mcp_api_keys_desc", {}, "API keys for integrating external services and applications (CivitAI, Plane, etc.) via Model Context Protocol. Stored encrypted in your DID graph.").replace(/Club Pass[,،、]\s*/gi, "")
                      : $_("mcp_api_keys_desc", {}, "API keys for integrating external services and applications (Club Pass, CivitAI, Plane, etc.) via Model Context Protocol. Stored encrypted in your DID graph.")}
                  </p>
                </div>
                <div class="mcp-header-actions">
                  {#if !isIosApp && !mcpAppsMap['club_pass'] && !mcpAppsMap['whop']}
                    <button class="btn-preset-chip" type="button" on:click={() => openAddMcpModal('club_pass')}>
                      <span class="preset-dot club-pass-dot"></span>
                      <span>+ Club Pass</span>
                    </button>
                  {/if}
                  <button class="btn-preset-chip" type="button" on:click={() => openAddMcpModal('civitai')}>
                    <span class="preset-dot civitai-dot"></span>
                    <span>+ CivitAI</span>
                  </button>
                  {#if !mcpAppsMap['plane']}
                    <button class="btn-preset-chip" type="button" on:click={() => openAddMcpModal('plane')}>
                      <span class="preset-dot plane-dot"></span>
                      <span>+ Plane</span>
                    </button>
                  {/if}
                  <button class="btn-primary-sm" type="button" on:click={() => openAddMcpModal('custom')}>
                    + {$_("add_app_key", {}, "Add application key")}
                  </button>
                </div>
              </div>

              {#if mcpMessage}
                <p class="omd-key-message {mcpMessageType}" style="margin: 10px 0;">{mcpMessage}</p>
              {/if}

              <!-- Add App Key Inline Form -->
              {#if showMcpAddModal}
                <div class="mcp-add-form">
                  <div class="mcp-form-header">
                    <span class="mcp-form-title">{$_("add_app_key", {}, "Add application key")}</span>
                    <button class="close-btn-sm" type="button" on:click={closeAddMcpModal}>×</button>
                  </div>
                  <div class="mcp-form-grid">
                    <div class="form-field">
                      <label class="field-label-sm" for="mcpPresetSelect">{$_("app_name_label", {}, "Application")}:</label>
                      <select
                        id="mcpPresetSelect"
                        class="text-input-styled"
                        bind:value={mcpSelectedPreset}
                        on:change={() => {
                          const p = PRESET_MCP_APPS.find(x => x.id === mcpSelectedPreset);
                          if (p) { mcpCustomName = p.name; mcpCustomUrl = p.url; }
                        }}
                      >
                        {#each PRESET_MCP_APPS as preset}
                          <option value={preset.id}>{preset.name} ({preset.url})</option>
                        {/each}
                        <option value="custom">{$_("custom_app", {}, "Custom application...")}</option>
                      </select>
                    </div>

                    {#if mcpSelectedPreset === "civitai" && mcpAppsMap["civitai"]}
                      <div class="form-field">
                        <label class="field-label-sm" for="mcpCivAccLabelInput">{$_("account_label_opt", {}, "Account label (optional)")}:</label>
                        <input
                          id="mcpCivAccLabelInput"
                          class="omd-key-input"
                          type="text"
                          placeholder="e.g. @shinylark, Personal..."
                          bind:value={mcpCustomName}
                          spellcheck="false"
                        />
                      </div>
                    {/if}

                    {#if mcpSelectedPreset === "custom"}
                      <div class="form-field">
                        <label class="field-label-sm" for="mcpCustomNameInput">{$_("app_name_label", {}, "Application name")}:</label>
                        <input
                          id="mcpCustomNameInput"
                          class="omd-key-input"
                          type="text"
                          placeholder="e.g. Linear, GitHub..."
                          bind:value={mcpCustomName}
                          spellcheck="false"
                        />
                      </div>
                      <div class="form-field">
                        <label class="field-label-sm" for="mcpCustomUrlInput">{$_("app_url_label", {}, "Service URL (optional)")}:</label>
                        <input
                          id="mcpCustomUrlInput"
                          class="omd-key-input"
                          type="text"
                          placeholder="https://..."
                          bind:value={mcpCustomUrl}
                          spellcheck="false"
                        />
                      </div>
                    {/if}

                    <div class="form-field full-width">
                      <label class="field-label-sm" for="mcpKeyValInput">{$_("api_key_label", {}, "API key or token")}:</label>
                      <input
                        id="mcpKeyValInput"
                        class="omd-key-input"
                        type="password"
                        placeholder={mcpSelectedPreset === "club_pass" ? "pass_... / muse_..." : mcpSelectedPreset === "civitai" ? "civitai_api_token..." : mcpSelectedPreset === "plane" ? "plane_api_..." : "api_key_..."}
                        bind:value={mcpKeyInput}
                        autocomplete="off"
                        spellcheck="false"
                      />
                    </div>
                  </div>
                  <div class="mcp-form-actions">
                    <button
                      class="btn-primary-sm"
                      type="button"
                      disabled={mcpBusy || !mcpKeyInput.trim() || (mcpSelectedPreset === 'custom' && !mcpCustomName.trim())}
                      on:click={submitAddMcpKey}
                    >
                      {mcpBusy ? $_("saving", {}, "Saving…") : $_("save", {}, "Save")}
                    </button>
                    <button class="btn-danger-outline" type="button" on:click={closeAddMcpModal}>
                      {$_("cancel", {}, "Cancel")}
                    </button>
                  </div>
                </div>
              {/if}

              <!-- Configured Apps List -->
              <div class="mcp-items-container">
                {#if mcpAppsList.length === 0 && !showMcpAddModal}
                  <div class="mcp-empty-state">
                    <svg viewBox="0 0 24 24" width="28" height="28" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" style="opacity: 0.5; margin-bottom: 8px;"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>
                    <p>{isIosApp
                      ? $_("no_mcp_keys_yet", {}, "No application keys saved yet. Add CivitAI, Plane or a custom service.").replace(/Club Pass[,،、]\s*/gi, "")
                      : $_("no_mcp_keys_yet", {}, "No application keys saved yet. Add Club Pass, CivitAI, Plane or a custom service.")}</p>
                  </div>
                {:else}
                  {#each mcpAppsList as app (app.appId)}
                    {@const isVisible = !!showMcpKeyToggles[app.appId]}
                    {@const currentVal = getMcpInputVal(app)}
                    {@const isModified = currentVal.trim() !== (app.key || "").trim()}
                    <div class="mcp-app-item">
                      <div class="mcp-app-header">
                        <div class="mcp-app-identity">
                          <span class="mcp-app-badge {app.appId === 'plane' ? 'plane' : (app.appId === 'club_pass' || app.appId === 'whop') ? 'club_pass' : (app.appId === 'civitai' || app.appId.startsWith('civitai_')) ? 'civitai' : 'custom'}">
                            {app.appId === 'plane' ? 'PL' : (app.appId === 'club_pass' || app.appId === 'whop') ? 'CP' : (app.appId === 'civitai' || app.appId.startsWith('civitai_')) ? 'CV' : (app.name || 'APP').slice(0, 2).toUpperCase()}
                          </span>
                          <div class="mcp-app-title-box">
                            <span class="mcp-app-title">{app.name || app.appId}</span>
                            {#if app.url}
                              <a class="mcp-app-url" href={app.url} target="_blank" rel="noopener noreferrer">
                                {app.url.replace(/^https?:\/\//, '')}
                                <svg viewBox="0 0 24 24" width="11" height="11" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align: middle; margin-left: 2px;"><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/><polyline points="15 3 21 3 21 9"/><line x1="10" y1="14" x2="21" y2="3"/></svg>
                              </a>
                            {/if}
                          </div>
                        </div>
                        <span class="ssh-badge">{$_("saved", {}, "saved")}</span>
                      </div>

                      <div class="omd-key-editor" style="margin-top: 8px;">
                        <div class="omd-key-field">
                          <input
                            class="omd-key-input"
                            type={isVisible ? "text" : "password"}
                            placeholder="API Key / Token"
                            value={currentVal}
                            on:input={(e) => setMcpInputVal(app.appId, e.target.value)}
                            spellcheck="false"
                            autocomplete="off"
                          />
                          <button
                            class="eye-btn"
                            type="button"
                            title={isVisible ? $_("hide_key", {}, "Hide key") : $_("show_key", {}, "Show key")}
                            on:click={() => toggleMcpKeyVisibility(app.appId)}
                          >
                            {#if isVisible}
                              <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></svg>
                            {:else}
                              <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>
                            {/if}
                          </button>
                        </div>
                        <button
                          class="btn-primary-sm subtle"
                          type="button"
                          disabled={!currentVal}
                          on:click={() => copyToClipboard(currentVal, 'mcp_' + app.appId)}
                        >
                          {copiedField === ('mcp_' + app.appId) ? $_("copied", {}, "Copied!") : $_("copy", {}, "Copy")}
                        </button>
                        {#if isModified}
                          <button
                            class="btn-primary-sm"
                            type="button"
                            disabled={mcpBusy || !currentVal.trim()}
                            on:click={() => handleSaveMcpApp(app)}
                          >
                            {$_("save", {}, "Save")}
                          </button>
                        {/if}
                        <button
                          class="btn-danger-outline"
                          type="button"
                          disabled={mcpBusy}
                          on:click={() => handleRemoveMcpApp(app.appId)}
                        >
                          {$_("remove", {}, "Remove")}
                        </button>
                      </div>
                    </div>
                  {/each}
                {/if}
              </div>
            </div>

            <!-- 6d. SSH ACCESS (TERMINAL QUICK-LOGIN) -->
            <div class="card">
              <h4 class="card-title">{$_("ssh_access", {}, "SSH Access")}</h4>
              <p class="card-subtitle" style="margin: 0 0 12px 0;">
                {$_("ssh_access_desc", {}, "Device login for the Terminal app. Credentials are the device's real system user, stored encrypted in your DID graph — the terminal logs in automatically next time.")}
              </p>
              {#if $profileStore.isEphemeral}
                <div class="setting-row">
                  <div class="row-text">
                    <span class="row-label">{$_("not_available_guest", {}, "Not available in guest mode")}</span>
                    <span class="row-sub">{$_("signin_store_ssh", {}, "Sign in with your ID to store SSH access on this device.")}</span>
                  </div>
                </div>
              {:else if devices.length === 0}
                <div class="setting-row">
                  <div class="row-text">
                    <span class="row-label">{$_("no_linked_devices", {}, "No linked devices")}</span>
                    <span class="row-sub">{$_("connect_device_first_ssh", {}, "Connect a device first, then save its login here.")}</span>
                  </div>
                </div>
              {:else}
                {#each devices as dev (dev.linkId)}
                  {@const saved = sshByDevice[dev.linkId]}
                  <div class="setting-row">
                    <div class="row-text">
                      <span class="row-label">
                        {dev.name || dev.linkId}
                        {#if saved}
                          <span class="ssh-badge">{$_("saved", {}, "saved")}{saved.user ? ' · ' + saved.user : ''}</span>
                        {/if}
                      </span>
                      <span class="row-sub">{dev.status === 'connected' ? $_("connected", {}, "Connected") : $_("offline", {}, "Offline")} · {dev.linkId.slice(0, 12)}…</span>
                    </div>
                    <div class="ssh-actions">
                      <button class="btn-primary-sm" on:click={() => startSshEdit(dev)}>
                        {saved ? $_("edit", {}, "Edit") : $_("add", {}, "Add")}
                      </button>
                      {#if saved}
                        <button class="btn-danger-outline" on:click={() => removeSshCredential(dev.linkId)}>{$_("remove", {}, "Remove")}</button>
                      {/if}
                    </div>
                  </div>
                  {#if sshEditId === dev.linkId}
                    <div class="omd-key-editor">
                      <input
                        class="omd-key-input"
                        type="text"
                        placeholder="system username (e.g. alexey)"
                        bind:value={sshUserInput}
                        spellcheck="false"
                        autocomplete="off"
                      />
                      <input
                        class="omd-key-input"
                        type="password"
                        placeholder="password"
                        bind:value={sshPassInput}
                        autocomplete="new-password"
                      />
                      <button class="btn-primary-sm" disabled={!sshUserInput.trim() || sshBusy} on:click={submitSshEdit}>
                        {sshBusy ? $_("saving", {}, "Saving…") : $_("save", {}, "Save")}
                      </button>
                      <button class="btn-danger-outline" on:click={cancelSshEdit}>{$_("cancel", {}, "Cancel")}</button>
                    </div>
                  {/if}
                {/each}
              {/if}
            </div>

            <!-- Danger Zone -->
            <div class="card danger-card">
              <h4 class="card-title text-danger">{$_("session_management", {}, "Session Management")}</h4>
              <div class="setting-row">
                <div class="row-text">
                  <span class="row-label">{$profileStore.isEphemeral ? $_("disconnect_session", {}, "Disconnect device session") : $_("signout_device", {}, "Sign out from this device")}</span>
                  <span class="row-sub">{$profileStore.isEphemeral ? $_("ends_p2p_connection", {}, "Ends the temporary P2P connection") : $_("clears_device_cert", {}, "Clears local device certificate and storage keys")}</span>
                </div>
                <button class="btn-danger-outline" on:click={() => dispatch("logout")}>
                  {$profileStore.isEphemeral ? $_("disconnect_device", {}, "Disconnect") : $_("sign_out", {}, "Sign Out")}
                </button>
              </div>
            </div>
          </div>
        {/if}
      </div>
  </div>
</div>

{#if showSyncFolderPicker}
  {@const targetDev = devices.find(d => d.linkId === selectedSyncTarget)}
  {#if targetDev}
    <FolderPicker
      device={targetDev}
      mode="folder"
      title={$_("select_backup_folder", {}, "Select Backup Destination Folder")}
      on:select={(e) => {
        if (e.detail && (e.detail.path || e.detail.virtualPath)) {
          let chosen = e.detail.virtualPath || e.detail.path || "";
          chosen = chosen.replace(/^\/(home|Users)\/[^\/]+/, "");
          if (!chosen.startsWith("/")) chosen = "/" + chosen;
          customSyncFolder = chosen;
        }
        showSyncFolderPicker = false;
      }}
      on:cancel={() => { showSyncFolderPicker = false; }}
    />
  {/if}
{/if}

{#if showAvatarModal}
  <AssistantAvatarModal
    bind:open={showAvatarModal}
    chatStore={getChatStore($omdKeyStore?.key || identity?.token || (typeof localStorage !== "undefined" ? localStorage.getItem("omd_key") : null))}
    {httpDrive}
    currentAvatarUrl={assistantAvatarUrl}
    assistantName={assistantName}
    style={assistantStyle}
    character={assistantModel}
    appearancePrompt={assistantAppearance}
    defaultStorage={assistantStorage}
    on:saved={(e) => {
      if (e.detail?.avatarUrl) assistantAvatarUrl = e.detail.avatarUrl;
      if (e.detail?.style) assistantStyle = e.detail.style;
      if (e.detail?.assistant_model) assistantModel = e.detail.assistant_model;
      if (e.detail?.assistant_appearance) assistantAppearance = e.detail.assistant_appearance;
    }}
    on:close={() => (showAvatarModal = false)}
  />
{/if}

{#if publicAiAgreementOpen}
  <div class="modal-backdrop {isDarkTheme ? 'dark-theme' : 'light-theme'}" on:click|self={onPublicAiDecline}>
    <div class="modal ai-agreement-modal {isDarkTheme ? 'dark-theme' : 'light-theme'}" role="dialog" aria-modal="true">
      <h3 class="modal-title">{$_("ai_public_agreement_title", {}, "Responsible Use Agreement")}</h3>
      <div class="agreement-text" style="max-height: 50vh; overflow-y: auto; text-align: left; font-size: 14px; line-height: 1.6; margin: 12px 0;">
        <p>{$_("ai_public_agreement_body_1", {},
          "You are about to connect to the AI Assistant Public Node — a shared system node operated over a direct peer-to-peer channel. Chats, history, and your personal knowledge stay encrypted on your side; public knowledge and queries are processed by the public node.")}</p>
        <p>{$_("ai_public_agreement_body_2", {},
          "You agree to use the service responsibly and lawfully, and in particular NOT to generate unlawful, harmful, or abusive content, including content involving minors, violence, weapons, malware, or any activity prohibited by applicable law. Attempting to bypass content filters or abusing shared resources is not allowed.")}</p>
        <p>{$_("ai_public_agreement_body_3", {},
          "The service is provided on an \"as is\" basis without warranties. Usage may be rate-limited, and access may be revoked in case of abuse. Your acceptance is stored in your encrypted settings.")}</p>
      </div>
      <div style="display: flex; gap: 10px; justify-content: flex-end; margin-top: 8px;">
        <button
          type="button"
          class="btn-secondary-action"
          on:click={onPublicAiDecline}
        >
          {$_("ai_public_decline", {}, "Cancel")}
        </button>
        <button
          type="button"
          class="btn-primary-lg"
          on:click={onPublicAiAgree}
        >
          {$_("ai_public_agree", {}, "I Agree")}
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

{#if showAssistantFolderPicker}
  {@const targetDev = storagePickerDevice || (isNodeOwner ? currentAiNodeDevice : (devices.find(d => !d.local) || getLocalDevice() || devices[0]))}
  {#if targetDev}
    <FolderPicker
      device={targetDev}
      mode="folder"
      title={$_("select_destination_folder", {}, "Select Destination Folder")}
      on:select={async (e) => {
        if (e.detail && (e.detail.path || e.detail.virtualPath)) {
          let chosen = e.detail.virtualPath || e.detail.path || "";
          assistantStorage = formatStoragePath(targetDev, chosen);
          await saveAssistantConfig();
        }
        showAssistantFolderPicker = false;
      }}
      on:cancel={() => { showAssistantFolderPicker = false; }}
    />
  {/if}
{/if}

{#if deviceToDelete}
  <!-- svelte-ignore a11y-click-events-have-key-events -->
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div class="confirm-modal-overlay" on:click|self={() => (deviceToDelete = null)}>
    <div class="confirm-modal-box">
      <div class="confirm-modal-header">
        <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="#f85149" stroke-width="2" class="confirm-danger-icon">
          <path d="M3 6h18M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M10 11v6M14 11v6"/>
        </svg>
        <h3>{$_("disconnect_device", {}, "Disconnect Device")}</h3>
      </div>
      <div class="confirm-modal-body">
        <p>{$_("disconnect_device_confirm", { name: deviceToDelete.name || deviceToDelete.linkId }, "Are you sure you want to disconnect and forget this device?")}</p>
        <p class="confirm-modal-subtext">{$_("disconnect_device_subtext", {}, "This device will be removed from your profile and will no longer connect automatically.")}</p>
      </div>
      <div class="confirm-modal-actions">
        <button class="confirm-btn cancel" on:click={() => (deviceToDelete = null)}>{$_("cancel", {}, "Cancel")}</button>
        <button class="confirm-btn danger" on:click={handleConfirmDeleteDevice}>{$_("disconnect_forget_device", {}, "Disconnect & Forget")}</button>
      </div>
    </div>
  </div>
{/if}

<input
  type="file"
  accept=".json,application/json"
  bind:this={settingsKeyfileInput}
  on:change={handleImportKeyfileChange}
  style="display: none;"
/>

{#if showDeleteAccountModal}
  <!-- svelte-ignore a11y-click-events-have-key-events -->
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div class="confirm-modal-overlay" on:click|self={() => (showDeleteAccountModal = false)}>
    <div class="confirm-modal-box">
      <div class="confirm-modal-header">
        <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="#ef4444" stroke-width="2" class="confirm-danger-icon">
          <path d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/>
        </svg>
        <h3 style="color: #ef4444;">{$_("delete_account_modal_title", {}, "Delete Account")}</h3>
      </div>
      <div class="confirm-modal-body">
        <p><strong>{$_("delete_account_modal_warning", {}, "Are you sure you want to permanently delete your account?")}</strong></p>
        <p class="confirm-modal-subtext">
          {$_("delete_account_modal_desc", {}, "This will permanently remove your Decentralized Identity (DID), disconnect all paired nodes and storage devices, and clear all local credentials and cached session data on this device. In accordance with our GDPR Privacy Policy, server-side registration records will be permanently purged within 30 days. This action cannot be undone.")}
        </p>
        {#if deleteAccountNeedsPassword}
          <label class="confirm-modal-label" for="delete-account-password">{$_("password", {}, "Password")}</label>
          <input
            id="delete-account-password"
            class="confirm-modal-input"
            type="password"
            bind:value={deleteAccountPassword}
            placeholder={$_("enter_password", {}, "Enter your password")}
            autocomplete="current-password"
            disabled={deletingAccount}
          />
        {/if}
      </div>
      <div class="confirm-modal-actions">
        <button class="confirm-btn cancel" on:click={() => (showDeleteAccountModal = false)} disabled={deletingAccount}>
          {$_("cancel", {}, "Cancel")}
        </button>
        <button
          class="confirm-btn danger"
          on:click={handleConfirmDeleteAccount}
          disabled={deletingAccount || (deleteAccountNeedsPassword && !deleteAccountPassword)}
        >
          {deletingAccount ? $_("deleting", {}, "Deleting...") : $_("confirm_delete_account", {}, "Permanently Delete Account")}
        </button>
      </div>
    </div>
  </div>
{/if}

<style>
  .settings-root {
    display: flex;
    flex-direction: column;
    width: 100vw;
    height: 100vh;
    background-color: var(--bg-primary, #181720);
    color: var(--text-primary, #F8F7FC);
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    overflow: hidden;
    z-index: 1000;
  }

  .dark-theme {
    color-scheme: dark;
  }

  .light-theme {
    color-scheme: light;
  }

  /* Header */
  .settings-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    height: calc(60px + env(safe-area-inset-top, 0px));
    padding-top: env(safe-area-inset-top, 0px);
    padding-bottom: 0;
    padding-left: calc(24px + env(safe-area-inset-left, 0px));
    padding-right: calc(24px + env(safe-area-inset-right, 0px));
    background-color: var(--bg-secondary);
    border-bottom: none;
    box-shadow: none;
    flex-shrink: 0;
    box-sizing: border-box;
  }

  .header-left {
    display: flex;
    align-items: center;
    gap: 16px;
  }

  .back-btn {
    display: flex;
    align-items: center;
    gap: 8px;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
    padding: 6px 14px;
    border-radius: 6px;
    font-size: 14px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .back-btn:hover {
    background-color: var(--bg-card-hover);
    border-color: var(--accent);
    color: var(--accent-hover);
    box-shadow: 0 2px 8px var(--accent-glow, rgba(142, 116, 189, 0.22));
  }

  .header-divider {
    width: 1px;
    height: 24px;
    background-color: var(--border-color);
  }

  .header-title-wrap {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .header-title {
    font-size: 18px;
    font-weight: 700;
    margin: 0;
    color: var(--text-primary);
  }

  .header-badge {
    font-size: 11px;
    font-weight: 600;
    background: var(--accent-soft, rgba(142, 116, 189, 0.16));
    color: var(--accent);
    padding: 2px 8px;
    border-radius: 9999px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }

  .header-badge.ephemeral {
    background: var(--warning-bg);
    color: var(--warning-text);
  }

  .icon-close-btn {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    cursor: pointer;
    padding: 6px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: color 0.15s;
  }

  .icon-close-btn:hover {
    color: var(--text-primary);
    background-color: var(--bg-card-hover);
  }

  /* Body */
  .settings-body {
    display: flex;
    flex-grow: 1;
    overflow: hidden;
    background-color: var(--bg-secondary);
  }

  /* Sidebar */
  .settings-sidebar {
    width: 260px;
    background-color: var(--bg-secondary);
    border-right: none;
    box-shadow: var(--sidebar-shadow);
    position: relative;
    z-index: 10;
    display: flex;
    flex-direction: column;
    justify-content: space-between;
    padding: 16px 12px calc(16px + env(safe-area-inset-bottom, 0px));
    flex-shrink: 0;
    box-sizing: border-box;
  }

  .settings-nav {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .nav-item {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 10px 14px;
    background: transparent;
    border: none;
    border-radius: 6px;
    color: var(--text-secondary);
    font-size: 14px;
    font-weight: 500;
    cursor: pointer;
    text-align: left;
    transition: all 0.15s ease;
  }

  .nav-item:hover {
    background-color: var(--bg-card-hover);
    color: var(--text-primary);
  }

  .nav-item.active {
    background-color: var(--accent-soft, rgba(142, 116, 189, 0.24));
    color: var(--accent);
    font-weight: 600;
  }

  .nav-item.active .nav-icon {
    color: var(--accent);
  }

  .nav-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 20px;
  }

  /* Sidebar Footer */
  .sidebar-user-footer {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 12px;
    background-color: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 10px;
  }

  .user-avatar-mini {
    position: relative;
    width: 34px;
    height: 34px;
    border-radius: 50%;
    background: var(--accent-gradient, linear-gradient(135deg, #7D60AC, #A65581));
    display: flex;
    align-items: center;
    justify-content: center;
    font-weight: 700;
    font-size: 14px;
    color: white;
    flex-shrink: 0;
    overflow: hidden;
  }

  .user-avatar-mini.ephemeral {
    background: linear-gradient(135deg, #5B5270, #3E3A4D);
    color: #DCDAE4;
  }

  .user-avatar-mini img {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }

  .online-indicator {
    position: absolute;
    bottom: 0;
    right: 0;
    width: 8px;
    height: 8px;
    background-color: #10b981;
    border: 1.5px solid var(--bg-card);
    border-radius: 50%;
  }

  .online-indicator.ephemeral {
    background-color: #f59e0b;
  }

  .user-info-mini {
    display: flex;
    flex-direction: column;
    overflow: hidden;
    flex-grow: 1;
  }

  .user-name-mini {
    font-size: 13px;
    font-weight: 600;
    color: var(--text-primary);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .user-sub-mini {
    font-size: 11px;
    color: var(--text-muted);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .logout-btn-mini {
    background: transparent;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    padding: 4px;
    border-radius: 4px;
    transition: color 0.15s;
  }

  .logout-btn-mini:hover {
    color: #ef4444;
  }

  /* Content */
  .settings-content {
    flex-grow: 1;
    overflow-y: auto;
    padding: 32px 48px;
    scrollbar-width: thin;
    scrollbar-color: rgba(139, 128, 163, 0.42) transparent;
    background-color: var(--bg-primary);
    border-top-left-radius: 12px;
    border-top-right-radius: 12px;
  }

  .settings-content::-webkit-scrollbar {
    width: 6px;
    height: 6px;
  }

  .settings-content::-webkit-scrollbar-track {
    background: transparent;
  }

  .settings-content::-webkit-scrollbar-thumb {
    background: rgba(139, 128, 163, 0.42);
    border-radius: 3px;
  }

  .settings-content::-webkit-scrollbar-thumb:hover {
    background: rgba(139, 128, 163, 0.68);
  }

  .content-container {
    max-width: 840px;
    margin: 0 auto;
  }

  .section-header {
    margin-bottom: 24px;
  }

  .section-header.section-header-spaced {
    margin-top: 36px;
    margin-bottom: 24px;
  }

  .section-title {
    font-size: 24px;
    font-weight: 700;
    margin: 0 0 6px 0;
    color: var(--text-primary);
  }

  .section-desc {
    font-size: 14px;
    color: var(--text-secondary);
    margin: 0;
  }

  .card-group {
    display: flex;
    flex-direction: column;
    gap: 20px;
  }

  /* AI node selection & responsible-use agreement */
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 5000;
  }

  .ai-agreement-modal {
    background: var(--bg-card, #252330);
    color: var(--text-primary, #ffffff);
    border: 1px solid var(--border-color, #3E3A4D);
    border-radius: 14px;
    padding: 24px;
    width: min(560px, 92vw);
    box-shadow: 0 8px 40px rgba(0, 0, 0, 0.35);
    font-family: inherit;
    box-sizing: border-box;
  }

  .ai-agreement-modal .modal-title {
    margin: 0 0 12px 0;
    font-size: 18px;
    font-weight: 600;
    color: var(--text-primary, #ffffff);
  }

  .ai-agreement-modal .agreement-text {
    color: var(--text-secondary, #C4B8DB);
  }

  .ai-agreement-modal .agreement-text p {
    margin: 10px 0;
    line-height: 1.6;
    color: inherit;
  }

  /* Light theme */
  .modal-backdrop.light-theme .ai-agreement-modal,
  .ai-agreement-modal.light-theme {
    background: #ffffff;
    color: #181326;
    border-color: #DCDAE4;
    box-shadow: 0 8px 40px rgba(0, 0, 0, 0.12);
  }

  .modal-backdrop.light-theme .ai-agreement-modal .modal-title,
  .ai-agreement-modal.light-theme .modal-title {
    color: #181326;
  }

  .modal-backdrop.light-theme .ai-agreement-modal .agreement-text,
  .ai-agreement-modal.light-theme .agreement-text {
    color: #5B5270;
  }

  .modal-backdrop.light-theme .btn-secondary-action,
  .ai-agreement-modal.light-theme .btn-secondary-action {
    background-color: #ffffff;
    border-color: #DCDAE4;
    color: #181326;
  }

  .modal-backdrop.light-theme .btn-secondary-action:hover,
  .ai-agreement-modal.light-theme .btn-secondary-action:hover {
    background-color: #EFEBF8;
    border-color: var(--accent, #563F7A);
    color: var(--accent, #563F7A);
    box-shadow: 0 2px 8px var(--accent-glow, rgba(86, 63, 122, 0.18));
  }

  /* Dark theme */
  .modal-backdrop.dark-theme .ai-agreement-modal,
  .ai-agreement-modal.dark-theme {
    background: #252330;
    color: #ffffff;
    border-color: #3E3A4D;
    box-shadow: 0 8px 40px rgba(0, 0, 0, 0.45);
  }

  .modal-backdrop.dark-theme .ai-agreement-modal .modal-title,
  .ai-agreement-modal.dark-theme .modal-title {
    color: #ffffff;
  }

  .modal-backdrop.dark-theme .ai-agreement-modal .agreement-text,
  .ai-agreement-modal.dark-theme .agreement-text {
    color: #C4B8DB;
  }

  .modal-backdrop.dark-theme .btn-secondary-action,
  .ai-agreement-modal.dark-theme .btn-secondary-action {
    background-color: #252330;
    border-color: #3E3A4D;
    color: #ffffff;
  }

  .modal-backdrop.dark-theme .btn-secondary-action:hover,
  .ai-agreement-modal.dark-theme .btn-secondary-action:hover {
    background-color: #312E3E;
    border-color: var(--accent, #8E74BD);
    color: var(--accent-hover, #A28AD0);
    box-shadow: 0 2px 8px var(--accent-glow, rgba(142, 116, 189, 0.22));
  }

  .card {
    background-color: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    padding: 24px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
  }

  .card-title {
    font-size: 16px;
    font-weight: 600;
    margin: 0 0 16px 0;
    color: var(--text-primary);
  }

  .card-subtitle {
    font-size: 13px;
    color: var(--text-secondary);
    margin: -10px 0 16px 0;
  }

  .ai-node-card {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .ai-node-select-row {
    display: flex;
    gap: 8px;
    align-items: center;
  }

  .ai-node-select-row select {
    flex: 1;
    min-width: 0;
  }

  .custom-node-block {
    display: flex;
    flex-direction: column;
    gap: 6px;
    animation: fadeIn 0.18s ease-out;
  }

  .custom-node-input-row {
    display: flex;
    gap: 8px;
    align-items: center;
  }

  .custom-node-input {
    flex: 1;
    min-width: 0;
  }

  .connect-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 0 16px;
    height: 38px;
    background: var(--accent, #7D60AC);
    color: #ffffff;
    font-size: 13px;
    font-weight: 500;
    border: none;
    border-radius: 8px;
    cursor: pointer;
    white-space: nowrap;
    transition: background 0.15s;
    box-sizing: border-box;
    flex-shrink: 0;
  }

  .connect-btn:hover:not(:disabled) {
    background: var(--accent-hover, #563F7A);
  }

  .connect-btn:disabled {
    opacity: 0.55;
    cursor: not-allowed;
  }

  .custom-node-err {
    font-size: 12px;
    color: #ef4444;
    margin-top: 2px;
  }

  .icon-btn-danger {
    width: 38px;
    height: 38px;
    border-radius: 8px;
    border: 1px solid rgba(239, 68, 68, 0.3);
    background: rgba(239, 68, 68, 0.08);
    color: #ef4444;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    flex-shrink: 0;
    transition: 0.15s;
  }

  .icon-btn-danger:hover {
    background: rgba(239, 68, 68, 0.2);
  }

  .card-subtext {
    font-size: 0.84rem;
    color: var(--text-secondary);
    line-height: 1.45;
    margin: 4px 0 0 0;
  }

  .card-title + .card-subtext {
    margin: -10px 0 16px 0;
  }

  .field-label-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }

  .refresh-link-btn {
    background: none;
    border: none;
    color: var(--accent, #7D60AC);
    font-size: 0.8rem;
    cursor: pointer;
    padding: 0;
    display: inline-flex;
    align-items: center;
    gap: 4px;
    font-family: inherit;
    font-weight: 500;
  }

  .refresh-link-btn:hover {
    text-decoration: underline;
  }

  .refresh-link-btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .token-input-wrap {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .token-input {
    flex: 1;
    min-width: 0;
  }

  .icon-btn {
    width: 36px;
    height: 36px;
    border-radius: 10px;
    border: 1px solid var(--border-color);
    background: rgba(255, 255, 255, 0.05);
    color: var(--text-secondary);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s;
    flex-shrink: 0;
  }

  .icon-btn:hover {
    background: rgba(255, 255, 255, 0.1);
    color: var(--text-primary);
    border-color: var(--accent, #7D60AC);
  }

  .spinner-sm {
    width: 12px;
    height: 12px;
    border: 2px solid rgba(255, 255, 255, 0.3);
    border-top-color: currentColor;
    border-radius: 50%;
    animation: loginSpin 0.75s linear infinite;
    display: inline-block;
  }

  .card-header-row {
    display: flex;
    align-items: center;
    gap: 18px;
  }

  .avatar-large {
    position: relative;
    width: 64px;
    height: 64px;
    border-radius: 50%;
    background: var(--accent-gradient, linear-gradient(135deg, #7D60AC, #A65581));
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 26px;
    font-weight: 700;
    color: white;
    flex-shrink: 0;
  }

  .avatar-large.ephemeral {
    background: linear-gradient(135deg, #5B5270, #3E3A4D);
    color: #DCDAE4;
  }

  .avatar-large img {
    width: 100%;
    height: 100%;
    border-radius: 50%;
    object-fit: cover;
  }

  .profile-badge-row {
    margin-top: 6px;
    display: flex;
    align-items: center;
  }

  .did-verified-tag {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    background: rgba(16, 185, 129, 0.12);
    border: 1px solid rgba(16, 185, 129, 0.35);
    color: #10b981;
    font-size: 11.5px;
    font-weight: 600;
    padding: 3px 8px;
    border-radius: 6px;
    letter-spacing: 0.02em;
    user-select: none;
    width: fit-content;
  }

  .did-verified-tag.ephemeral {
    background: rgba(245, 158, 11, 0.12);
    border-color: rgba(245, 158, 11, 0.35);
    color: #f59e0b;
  }

  .did-tag-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background-color: currentColor;
  }

  .profile-display-name {
    font-size: 20px;
    font-weight: 700;
    margin: 0 0 4px 0;
    color: var(--text-primary);
  }

  .profile-display-sub {
    font-size: 14px;
    color: var(--text-secondary);
    margin: 0;
  }

  .card-divider {
    height: 1px;
    background-color: var(--border-color);
    margin: 20px 0;
  }

  .setting-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 0;
    border-bottom: 1px solid var(--border-subtle);
  }

  .setting-row.col-layout {
    flex-direction: column;
    align-items: stretch;
    gap: 8px;
  }

  .setting-row.col-layout .row-text {
    max-width: 100%;
  }

  .setting-row:last-child {
    border-bottom: none;
    padding-bottom: 0;
  }

  .row-text {
    display: flex;
    flex-direction: column;
    gap: 4px;
    max-width: 60%;
  }

  .row-label {
    font-size: 14px;
    font-weight: 600;
    color: var(--text-primary);
  }

  .row-sub {
    font-size: 12px;
    color: var(--text-muted);
  }

  .row-action-full {
    width: 100%;
    margin-top: 8px;
  }

  .code-box {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    background-color: var(--bg-primary);
    border: 1px solid var(--border-color);
    padding: 10px 12px;
    border-radius: 8px;
    font-family: 'Roboto Mono', monospace;
    font-size: 12px;
    color: var(--accent);
    gap: 10px 12px;
    width: 100%;
    box-sizing: border-box;
    min-width: 0;
  }

  .code-box code {
    flex: 1 1 240px;
    min-width: min(100%, 240px);
    line-height: 1.45;
    word-break: break-all;
    overflow-wrap: anywhere;
    white-space: pre-wrap;
  }

  .code-box-actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
    flex-shrink: 0;
  }

  .code-box-inline {
    display: flex;
    align-items: center;
    gap: 10px;
    font-family: 'Roboto Mono', monospace;
    font-size: 12px;
    color: var(--text-secondary);
    min-width: 0;
    max-width: 100%;
    flex-wrap: wrap;
  }

  .code-box-inline code {
    min-width: 0;
    max-width: 100%;
    word-break: break-all;
    overflow-wrap: anywhere;
  }

  .copy-btn {
    background-color: var(--bg-card-hover);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
    padding: 4px 10px;
    border-radius: 6px;
    font-size: 12px;
    font-weight: 500;
    cursor: pointer;
    flex-shrink: 0;
    transition: all 0.15s;
  }

  .copy-btn:hover {
    border-color: var(--accent);
    color: var(--accent-hover);
  }

  .badge-pill {
    font-size: 11px;
    font-weight: 600;
    padding: 3px 8px;
    border-radius: 9999px;
  }

  .badge-pill.success {
    background-color: var(--success-bg);
    color: var(--success-text);
  }

  .badge-pill.warning {
    background-color: var(--warning-bg);
    color: var(--warning-text);
  }

  .badge-pill.neutral {
    background-color: var(--bg-card-hover);
    color: var(--text-secondary);
  }

  /* Account Details Profile Editor */
  .avatar-edit-controls {
    display: flex;
    align-items: center;
    gap: 16px;
  }

  .avatar-preview-box {
    width: 48px;
    height: 48px;
    border-radius: 50%;
    overflow: hidden;
    background: var(--accent-gradient, linear-gradient(135deg, #7D60AC, #A65581));
    display: flex;
    align-items: center;
    justify-content: center;
    border: 2px solid var(--border-color);
    flex-shrink: 0;
  }

  .avatar-preview-box img {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }

  .avatar-fallback-medium {
    font-size: 20px;
    font-weight: 700;
    color: #ffffff;
  }

  .avatar-buttons-col {
    display: flex;
    gap: 8px;
    align-items: center;
  }

  .btn-secondary-action {
    background-color: var(--bg-card);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
    padding: 6px 14px;
    border-radius: 6px;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
    display: inline-flex;
    align-items: center;
    justify-content: center;
  }

  .btn-secondary-action:hover {
    background-color: var(--bg-card-hover);
    border-color: var(--accent);
    color: var(--accent-hover);
    box-shadow: 0 2px 8px var(--accent-glow, rgba(142, 116, 189, 0.22));
  }

  /* Photo & Backup Sync Styles */
  .sync-stats-grid {
    display: flex;
    flex-direction: column;
    gap: 8px;
    margin-bottom: 16px;
  }

  .sync-stat-box {
    background-color: var(--bg-primary);
    border: 1px solid var(--border-color);
    border-radius: 10px;
    padding: 10px 16px;
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    text-align: left;
    gap: 12px;
  }

  .sync-stat-box.success {
    border-color: rgba(16, 185, 129, 0.4);
    background: rgba(16, 185, 129, 0.05);
  }

  .sync-stat-box.warning {
    border-color: rgba(245, 158, 11, 0.4);
    background: rgba(245, 158, 11, 0.05);
  }

  .sync-stat-val {
    font-size: 18px;
    font-weight: 700;
    color: var(--text-primary);
    line-height: 1.1;
  }

  .sync-stat-box.success .sync-stat-val {
    color: #10b981;
  }

  .sync-stat-box.warning .sync-stat-val {
    color: #f59e0b;
  }

  .sync-stat-lbl {
    font-size: 12.5px;
    font-weight: 600;
    color: var(--text-secondary);
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }

  .sync-progress-wrap {
    margin-top: 14px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .sync-progress-bar {
    width: 100%;
    height: 8px;
    background-color: var(--bg-primary);
    border-radius: 9999px;
    overflow: hidden;
    border: 1px solid var(--border-color);
  }

  .sync-progress-fill {
    height: 100%;
    background: var(--accent-gradient, linear-gradient(90deg, #7D60AC, #A28AD0));
    border-radius: 9999px;
    transition: width 0.2s ease;
  }

  .sync-progress-info {
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-size: 12px;
    color: var(--text-secondary);
  }

  .sync-file-name {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    max-width: 80%;
  }

  .sync-pct {
    font-weight: 600;
    color: var(--accent);
  }

  .sync-status-msg {
    font-size: 12.5px;
    color: var(--text-secondary);
    margin: 8px 0 0 0;
  }

  .sync-last-time {
    font-size: 11.5px;
    color: var(--text-muted);
    margin: 8px 0 0 0;
  }

  .sync-action-buttons {
    margin-top: 16px;
    display: flex;
    justify-content: flex-end;
  }

  .sources-row {
    flex-direction: column;
    align-items: stretch !important;
    gap: 12px;
  }

  .sources-checkboxes-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 10px;
    width: 100%;
  }

  .source-checkbox-item {
    display: flex;
    align-items: center;
    gap: 10px;
    background-color: var(--bg-primary);
    border: 1px solid var(--border-color);
    padding: 10px 14px;
    border-radius: 8px;
    cursor: pointer;
    font-size: 13.5px;
    color: var(--text-primary);
    transition: all 0.15s ease;
    user-select: none;
  }

  .source-checkbox-item:hover {
    border-color: var(--accent);
    background-color: var(--bg-card-hover, rgba(255, 255, 255, 0.04));
  }

  .source-checkbox-item input[type="checkbox"] {
    appearance: none;
    -webkit-appearance: none;
    -moz-appearance: none;
    width: 18px;
    height: 18px;
    border-radius: 5px;
    border: 1.5px solid var(--border-color, #5B5270);
    background-color: var(--bg-secondary, #181326);
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    margin: 0;
    outline: none;
    transition: all 0.15s ease-in-out;
  }

  .source-checkbox-item input[type="checkbox"]:hover {
    border-color: var(--accent, #8E74BD);
  }

  .source-checkbox-item input[type="checkbox"]:checked {
    background-color: var(--accent, #8E74BD);
    border-color: var(--accent, #8E74BD);
    background-image: url("data:image/svg+xml,%3Csvg viewBox='0 0 16 16' fill='white' xmlns='http://www.w3.org/2000/svg'%3E%3Cpath d='M12.207 4.793a1 1 0 0 1 0 1.414l-5 5a1 1 0 0 1-1.414 0l-2-2a1 1 0 0 1 1.414-1.414L6.5 9.086l4.293-4.293a1 1 0 0 1 1.414 0z'/%3E%3C/svg%3E");
    background-repeat: no-repeat;
    background-position: center;
    background-size: 13px 13px;
  }

  .source-checkbox-item input[type="checkbox"]:focus-visible {
    box-shadow: 0 0 0 2px var(--accent-glow, rgba(142, 116, 189, 0.40));
  }

  .btn-ghost-danger {
    background: transparent;
    border: none;
    color: #ef4444;
    padding: 6px 10px;
    border-radius: 6px;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .btn-ghost-danger:hover {
    background-color: rgba(239, 68, 68, 0.1);
  }

  .btn-sm {
    font-size: 12px;
    padding: 5px 10px;
  }

  .row-input-wrap {
    width: 100%;
    max-width: 320px;
  }

  .input-with-prefix {
    display: flex;
    align-items: center;
    position: relative;
    width: 100%;
  }

  .input-prefix {
    position: absolute;
    left: 12px;
    color: var(--text-muted);
    font-size: 14px;
    font-weight: 600;
    pointer-events: none;
  }

  .text-input.with-prefix {
    padding-left: 28px;
  }

  .email-setting-row {
    align-items: flex-start;
  }

  .email-details-wrap {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 12px;
    width: 100%;
    max-width: 400px;
  }

  .current-email-badge {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 8px 10px;
  }

  .email-text {
    font-size: 14px;
    font-weight: 500;
    color: var(--text-primary);
    word-break: break-all;
    overflow-wrap: anywhere;
  }

  .btn-link-action {
    background: transparent;
    border: none;
    color: var(--accent);
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    padding: 2px 4px;
    transition: color 0.15s ease;
  }

  .btn-link-action:hover {
    text-decoration: underline;
  }

  .change-email-box {
    width: 100%;
    background-color: var(--bg-primary);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 14px;
    box-sizing: border-box;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .change-email-notice {
    font-size: 12px;
    color: var(--text-secondary);
    margin: 0;
    line-height: 1.4;
  }

  .change-email-row {
    display: flex;
    gap: 8px;
    width: 100%;
  }

  .change-email-row .text-input {
    flex-grow: 1;
  }

  .change-email-row .btn-primary-action {
    white-space: nowrap;
    padding: 8px 14px;
    font-size: 13px;
  }

  .card-footer-action-row {
    display: flex;
    align-items: center;
    gap: 14px;
    margin-top: 20px;
    padding-top: 16px;
    border-top: 1px solid var(--border-color);
  }

  .save-feedback-success {
    font-size: 13px;
    font-weight: 600;
    color: #10b981;
  }

  .save-feedback-error {
    font-size: 13px;
    font-weight: 600;
    color: #ef4444;
  }

  .success-text {
    font-size: 13px;
    color: #10b981;
    margin: 0;
  }

  /* Upgrade Card */
  .upgrade-card {
    border-color: var(--accent-border, rgba(142, 116, 189, 0.32));
    background: linear-gradient(180deg, var(--bg-card) 0%, var(--bg-soft, rgba(142, 116, 189, 0.06)) 100%);
  }

  .upgrade-header {
    display: flex;
    align-items: center;
    gap: 14px;
    margin-bottom: 20px;
  }

  .upgrade-icon {
    width: 44px;
    height: 44px;
    border-radius: 10px;
    background: var(--accent-soft, rgba(142, 116, 189, 0.16));
    color: var(--accent);
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .auth-tabs {
    display: flex;
    justify-content: center;
    gap: 8px;
    border-bottom: 1px solid var(--border-color);
    padding-bottom: 16px;
    margin-bottom: 20px;
  }

  .auth-tab-btn {
    background: transparent;
    border: 1px solid var(--border-color);
    color: var(--text-secondary);
    padding: 8px 18px;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .auth-tab-btn:hover {
    color: var(--text-primary);
    border-color: var(--accent);
  }

  .auth-tab-btn.active {
    background-color: var(--accent);
    color: white;
    border-color: var(--accent);
  }

  .auth-tab-pane {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 16px;
    width: 100%;
    max-width: 360px;
    margin: 0 auto;
    text-align: center;
  }

  .auth-pane-desc {
    font-size: 13px;
    color: var(--text-secondary);
    margin: 0 0 4px 0;
    text-align: center;
  }

  .auth-tab-pane .form-group {
    width: 100%;
    text-align: left;
  }

  .auth-tab-pane .btn-primary-action {
    width: 100%;
    margin-top: 4px;
  }

  .google-btn-wrapper {
    position: relative;
    height: 44px;
    width: 100%;
    max-width: 320px;
    margin: 0 auto;
  }

  .google-custom-btn {
    position: absolute;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    z-index: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 10px;
    padding: 0 16px;
    font-size: 14px;
    font-weight: 500;
    border-radius: 8px;
    border: 1px solid var(--border-color);
    background-color: var(--bg-primary);
    color: var(--text-primary);
    cursor: pointer;
    pointer-events: none;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.05);
  }

  .google-overlay {
    position: absolute;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    z-index: 10;
    opacity: 0.001;
    overflow: hidden;
    cursor: pointer;
  }

  .form-group {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .form-label {
    font-size: 13px;
    font-weight: 600;
    color: var(--text-primary);
  }

  .connect-input-row {
    display: flex;
    gap: 10px;
  }

  .text-input {
    width: 100%;
    box-sizing: border-box;
    background-color: var(--bg-primary);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
    padding: 10px 14px;
    border-radius: 8px;
    font-size: 14px;
    outline: none;
    transition: border-color 0.15s ease;
  }

  select.text-input,
  select.select-input {
    appearance: none;
    -webkit-appearance: none;
    -moz-appearance: none;
    padding: 9px 36px 9px 12px;
    background-repeat: no-repeat !important;
    background-position: right 14px center !important;
    background-size: 14px 14px !important;
    cursor: pointer;
    line-height: 1.4;
    box-sizing: border-box;
  }

  .dark-theme select.text-input,
  .dark-theme select.select-input {
    color-scheme: dark;
    background-color: var(--bg-card, #252330);
    color: var(--text-primary, #ffffff);
    border: 1px solid var(--border-color, #3E3A4D);
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%238B80A3' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E");
    background-repeat: no-repeat !important;
    background-position: right 14px center !important;
    background-size: 14px 14px !important;
  }

  .light-theme select.text-input,
  .light-theme select.select-input {
    color-scheme: light;
    background-color: var(--bg-card, #ffffff);
    color: var(--text-primary, #181326);
    border: 1px solid var(--border-color, #DCDAE4);
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%235B5270' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E");
    background-repeat: no-repeat !important;
    background-position: right 14px center !important;
    background-size: 14px 14px !important;
  }

  .dark-theme select.text-input option,
  .dark-theme select.select-input option,
  .dark-theme select.text-input optgroup,
  .dark-theme select.select-input optgroup {
    background-color: #252330 !important;
    color: #ffffff !important;
  }

  .light-theme select.text-input option,
  .light-theme select.select-input option,
  .light-theme select.text-input optgroup,
  .light-theme select.select-input optgroup {
    background-color: #ffffff !important;
    color: #181326 !important;
  }

  .text-input:focus {
    border-color: var(--accent);
  }

  .btn-primary-action {
    background-color: var(--accent);
    color: white;
    border: 1px solid var(--accent);
    padding: 8px 18px;
    border-radius: 6px;
    font-size: 14px;
    font-weight: 600;
    cursor: pointer;
    transition: background-color 0.15s ease, border-color 0.15s ease, box-shadow 0.15s ease;
  }

  .btn-primary-action:hover {
    background-color: var(--accent-hover);
    border-color: var(--accent-hover);
    box-shadow: 0 2px 8px var(--accent-glow, rgba(142, 116, 189, 0.35));
  }

  .devices-grid {
    display: grid;
    grid-template-columns: 1fr;
    gap: 12px;
    width: 100%;
    min-width: 0;
  }

  .device-item-card {
    display: flex;
    align-items: center;
    gap: 16px;
    background-color: var(--bg-primary);
    border: 1px solid var(--border-color);
    padding: 16px 20px;
    border-radius: 10px;
    width: 100%;
    min-width: 0;
    box-sizing: border-box;
  }

  .device-icon-wrap {
    color: var(--accent);
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .device-details {
    flex-grow: 1;
    display: flex;
    flex-direction: column;
    gap: 4px;
    min-width: 0;
    overflow: hidden;
  }

  .device-name {
    font-size: 15px;
    font-weight: 600;
    color: var(--text-primary);
    word-break: break-word;
  }

  .device-status-wrap {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    flex-shrink: 0;
  }

  .btn-icon-danger {
    background: transparent;
    border: none;
    color: var(--text-muted);
    padding: 6px;
    border-radius: 6px;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.15s ease;
  }

  .btn-icon-danger:hover {
    background-color: rgba(239, 68, 68, 0.15);
    color: #ef4444;
  }

  .device-meta {
    display: flex;
    gap: 14px;
    font-size: 12px;
    color: var(--text-muted);
    min-width: 0;
  }

  .device-linkid code {
    word-break: break-all;
  }

  .detected-header-row {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 16px;
    margin-bottom: 16px;
    width: 100%;
    box-sizing: border-box;
  }

  .detected-header-actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-shrink: 0;
  }

  .empty-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 32px 0;
    color: var(--text-muted);
    text-align: center;
    gap: 6px;
  }

  .theme-options-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 16px;
  }

  .theme-card {
    background-color: transparent;
    border: 2px solid var(--border-color);
    padding: 4px;
    border-radius: 12px;
    cursor: pointer;
    display: flex;
    flex-direction: column;
    color: var(--text-primary);
    font-weight: 600;
    transition: all 0.15s ease;
  }

  .theme-card:hover {
    border-color: var(--accent);
  }

  .theme-card.selected {
    border-color: var(--accent);
    box-shadow: 0 0 0 3px var(--accent-soft, rgba(142, 116, 189, 0.24));
  }

  .theme-preview {
    width: 100%;
    height: 68px;
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 10px;
    font-size: 15px;
    font-weight: 600;
    box-sizing: border-box;
    transition: all 0.15s ease;
  }

  .theme-preview.dark-preview {
    background: linear-gradient(135deg, #181720, #252330);
    color: #ffffff;
    border: 1px solid #3E3A4D;
  }

  .theme-preview.light-preview {
    background: linear-gradient(135deg, #FFFFFF, #F8F7FC);
    color: #181326;
    border: 1px solid #DCDAE4;
  }

  .danger-card {
    border-color: rgba(239, 68, 68, 0.3);
  }

  .text-danger {
    color: #ef4444;
  }

  .error-text {
    color: #ef4444;
    font-size: 13px;
    margin: 0;
  }

  .btn-danger-outline {
    background: transparent;
    border: 1px solid #ef4444;
    color: #ef4444;
    padding: 8px 16px;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s;
  }

  .btn-danger-outline:hover {
    background-color: #ef4444;
    color: white;
  }

  .online-badge.offline {
    background-color: #ef4444;
  }

  /* Confirmation Modal Styles */
  .confirm-modal-overlay {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(0, 0, 0, 0.7);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 10000;
  }

  .confirm-modal-box {
    background: #252330;
    border: 1px solid #3E3A4D;
    border-radius: 12px;
    width: 420px;
    max-width: 90vw;
    padding: 24px;
    box-shadow: 0 20px 40px rgba(0, 0, 0, 0.5);
    color: #F8F7FC;
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
    color: #F8F7FC;
  }

  .confirm-danger-icon {
    font-size: 1.4rem;
    color: #f85149;
    flex-shrink: 0;
  }

  .confirm-modal-body p {
    margin: 0 0 8px 0;
    font-size: 0.95rem;
    line-height: 1.5;
    color: #C4B8DB;
    word-break: break-word;
    overflow-wrap: anywhere;
  }

  .confirm-modal-body strong {
    word-break: break-word;
    overflow-wrap: anywhere;
  }

  .confirm-modal-subtext {
    font-size: 0.85rem !important;
    color: #8B80A3 !important;
  }

  .confirm-modal-label {
    display: block;
    margin-top: 14px;
    margin-bottom: 6px;
    font-size: 0.85rem;
    font-weight: 500;
    color: #C4B8DB;
  }

  .confirm-modal-input {
    width: 100%;
    box-sizing: border-box;
    padding: 9px 12px;
    border-radius: 6px;
    border: 1px solid #3E3A4D;
    background: #181720;
    color: #F8F7FC;
    font-size: 0.9rem;
    font-family: inherit;
  }

  .confirm-modal-input:focus {
    outline: none;
    border-color: #ef4444;
  }

  .confirm-modal-input:disabled {
    opacity: 0.6;
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
    background: #312E3E;
    border-color: #3E3A4D;
    color: #C4B8DB;
  }
  .confirm-btn.cancel:hover {
    background: #3E3A4D;
  }

  .confirm-btn.danger {
    background: #da3633;
    color: #ffffff;
  }
  .confirm-btn.danger:hover {
    background: #b82a28;
  }

  .omd-key-editor {
    display: flex;
    gap: 10px;
    align-items: center;
    margin-top: 12px;
    flex-wrap: wrap;
  }

  .omd-key-field {
    position: relative;
    flex: 1 1 260px;
    min-width: 240px;
  }

  /* Eye-toggle field variant: higher specificity than the base
     .omd-key-input rule below, so padding-right survives its
     `padding: 8px 12px` shorthand regardless of cascade order. */
  .omd-key-field .omd-key-input {
    width: 100%;
    padding-right: 40px;
  }

  .eye-btn {
    position: absolute;
    right: 6px;
    top: 50%;
    transform: translateY(-50%);
    display: flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 28px;
    padding: 0;
    border: none;
    border-radius: 6px;
    background: transparent;
    color: var(--text-secondary, #C4B8DB);
    cursor: pointer;
    opacity: 0.75;
  }

  .eye-btn:hover {
    opacity: 1;
    color: var(--accent, #8E74BD);
  }

  .omd-key-hint {
    margin: 4px 0 0;
    font-size: 12.5px;
    color: var(--text-secondary, #C4B8DB);
    line-height: 1.45;
  }

  .btn-primary-sm.subtle {
    background: transparent;
    border: 1px solid var(--accent, #8E74BD);
    color: var(--accent, #8E74BD);
  }

  .btn-primary-sm.subtle:hover:not(:disabled) {
    background: var(--accent-soft, rgba(142, 116, 189, 0.14));
  }

  .ssh-badge {
    display: inline-block;
    margin-left: 8px;
    padding: 2px 8px;
    border-radius: 999px;
    background: var(--success-bg, rgba(41, 204, 84, 0.15));
    color: var(--success-text, #4eda72);
    font-size: 11px;
    font-weight: 600;
    vertical-align: middle;
  }

  .ssh-actions {
    display: flex;
    gap: 8px;
    align-items: center;
  }

  .omd-key-input {
    flex: 1;
    min-width: 220px;
    box-sizing: border-box;
    background: var(--bg-primary, #181326);
    border: 1px solid var(--border-color, #3E3A4D);
    color: var(--text-primary, #ffffff);
    padding: 8px 12px;
    border-radius: 8px;
    font-size: 13px;
    font-family: 'JetBrains Mono', monospace;
  }

  .omd-key-input:focus {
    outline: none;
    border-color: var(--accent, #8E74BD);
  }

  .btn-primary-sm {
    background: var(--accent, #8E74BD);
    border: 1px solid var(--accent, #8E74BD);
    color: white;
    padding: 8px 16px;
    border-radius: 6px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: background-color 0.15s ease, border-color 0.15s ease, box-shadow 0.15s ease;
  }

  .btn-primary-sm:hover:not(:disabled) {
    background-color: var(--accent-hover, #A28AD0);
    border-color: var(--accent-hover, #A28AD0);
    box-shadow: 0 2px 8px var(--accent-glow, rgba(142, 116, 189, 0.35));
  }

  .btn-primary-sm:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .omd-key-message {
    margin: 8px 0 0;
    font-size: 12.5px;
  }

  .omd-key-message.success {
    color: var(--success-text, #4eda72);
  }

  .omd-key-message.error {
    color: #ef4444;
  }

  .omd-key-message.info {
    color: var(--text-secondary, #C4B8DB);
  }

  .mcp-card-header {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    flex-wrap: wrap;
    gap: 12px;
  }

  .mcp-header-text {
    flex: 1 1 280px;
  }

  .mcp-header-actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }

  .btn-preset-chip {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 5px 12px;
    border-radius: 999px;
    border: 1px solid var(--border-color, #3E3A4D);
    background: var(--bg-secondary, #252330);
    color: var(--text-primary, #EFEBF8);
    font-size: 12px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .btn-preset-chip:hover {
    border-color: var(--accent, #8E74BD);
    background: var(--accent-soft, rgba(142, 116, 189, 0.14));
    color: var(--accent, #8E74BD);
  }

  .preset-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
  }

  .preset-dot.club-pass-dot { background: linear-gradient(135deg, #8E74BD, #A65581); }
  .preset-dot.civitai-dot { background: #A65581; }
  .preset-dot.plane-dot { background: #8E74BD; }

  .mcp-add-form {
    margin-top: 14px;
    padding: 14px;
    border-radius: 8px;
    background: var(--bg-secondary, rgba(255, 255, 255, 0.04));
    border: 1px solid var(--border-color, #3E3A4D);
  }

  .mcp-form-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 12px;
  }

  .mcp-form-title {
    font-size: 13.5px;
    font-weight: 600;
    color: var(--text-primary, #EFEBF8);
  }

  .close-btn-sm {
    background: transparent;
    border: none;
    font-size: 18px;
    line-height: 1;
    color: var(--text-secondary, #8B80A3);
    cursor: pointer;
    padding: 0 4px;
  }

  .close-btn-sm:hover { color: var(--text-primary, #fff); }

  .mcp-form-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 10px;
  }

  .form-field {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .form-field.full-width {
    grid-column: 1 / -1;
  }

  .field-label-sm {
    font-size: 12px;
    font-weight: 500;
    color: var(--text-secondary, #8B80A3);
  }

  .mcp-form-actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    margin-top: 12px;
  }

  .mcp-items-container {
    margin-top: 14px;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }

  .mcp-empty-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 24px 16px;
    border-radius: 8px;
    border: 1px dashed var(--border-color, #3E3A4D);
    color: var(--text-secondary, #8B80A3);
    font-size: 13px;
    text-align: center;
  }

  .mcp-app-item {
    padding: 12px;
    border-radius: 8px;
    background: var(--bg-secondary, rgba(255, 255, 255, 0.03));
    border: 1px solid var(--border-color, #3E3A4D);
  }

  .mcp-app-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
  }

  .mcp-app-identity {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .mcp-app-badge {
    width: 28px;
    height: 28px;
    border-radius: 6px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    font-size: 11px;
    font-weight: 700;
    color: #fff;
  }

  .mcp-app-badge.club_pass { background: linear-gradient(135deg, #7D60AC, #733557); }
  .mcp-app-badge.civitai { background: #A65581; }
  .mcp-app-badge.plane { background: #8E74BD; }
  .mcp-app-badge.custom { background: #7D60AC; }

  .mcp-app-title-box {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .mcp-app-title {
    font-size: 13.5px;
    font-weight: 600;
    color: var(--text-primary, #EFEBF8);
  }

  .mcp-app-url {
    font-size: 11.5px;
    color: var(--accent, #8E74BD);
    text-decoration: none;
    display: inline-flex;
    align-items: center;
  }

  .mcp-app-url:hover {
    text-decoration: underline;
  }

  .mobile-sidebar-toggle-btn {
    display: none;
    background: transparent;
    border: 1px solid var(--border-color);
    color: var(--text-secondary);
    border-radius: 6px;
    padding: 4px 8px;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
    height: 32px;
    width: 32px;
    box-sizing: border-box;
    margin-right: 4px;
  }
  .mobile-sidebar-toggle-btn:hover {
    color: var(--text-primary);
    background-color: var(--bg-card-hover);
    border-color: var(--accent);
  }

  @media (max-width: 768px) {
    .settings-header {
      height: calc(56px + env(safe-area-inset-top, 0px)) !important;
      padding-top: env(safe-area-inset-top, 0px) !important;
      padding-bottom: 0 !important;
      padding-left: calc(12px + env(safe-area-inset-left, 0px)) !important;
      padding-right: calc(12px + env(safe-area-inset-right, 0px)) !important;
      box-sizing: border-box !important;
    }

    .header-left {
      gap: 8px !important;
      min-width: 0 !important;
      flex-shrink: 1 !important;
      overflow: hidden !important;
    }

    .header-title-wrap {
      min-width: 0 !important;
      flex-shrink: 1 !important;
      overflow: hidden !important;
      display: flex !important;
      align-items: center !important;
      gap: 6px !important;
    }

    .header-title {
      font-size: 16px !important;
      white-space: nowrap !important;
      text-overflow: ellipsis !important;
      overflow: hidden !important;
    }

    .header-badge {
      display: none !important;
    }

    .header-divider {
      display: none !important;
    }

    .back-btn-text {
      display: none !important;
    }

    .back-btn {
      width: 32px !important;
      height: 32px !important;
      padding: 0 !important;
      justify-content: center !important;
      box-sizing: border-box !important;
      flex-shrink: 0 !important;
    }

    .mobile-sidebar-toggle-btn {
      display: flex !important;
      flex-shrink: 0 !important;
    }

    .settings-body {
      position: relative !important;
      overflow: hidden !important;
    }

    .settings-content {
      padding: 14px 12px calc(14px + env(safe-area-inset-bottom, 0px)) !important;
      box-sizing: border-box !important;
    }

    .section-title {
      font-size: 20px !important;
    }

    .section-desc {
      font-size: 13px !important;
    }

    .card {
      padding: 16px 14px !important;
      margin-bottom: 16px !important;
      border-radius: 12px !important;
      width: 100% !important;
      box-sizing: border-box !important;
    }

    .setting-row {
      flex-direction: column !important;
      align-items: stretch !important;
      gap: 10px !important;
      padding: 14px 0 !important;
    }

    .row-text {
      max-width: 100% !important;
      width: 100% !important;
    }

    .code-box {
      flex-direction: column !important;
      align-items: stretch !important;
      gap: 10px !important;
    }

    .code-box code {
      flex: 0 0 auto !important;
      width: 100% !important;
      max-width: 100% !important;
      min-width: 0 !important;
      display: block !important;
    }

    .code-box-actions {
      width: 100% !important;
      display: flex !important;
      flex-wrap: wrap !important;
      gap: 8px !important;
    }

    .code-box-inline {
      width: 100% !important;
      justify-content: space-between !important;
      flex-wrap: wrap !important;
      gap: 8px !important;
    }

    .code-box-inline code {
      word-break: break-all !important;
      overflow-wrap: anywhere !important;
      max-width: 100% !important;
    }

    .row-input-wrap {
      max-width: 100% !important;
      width: 100% !important;
    }

    .email-details-wrap {
      max-width: 100% !important;
      width: 100% !important;
      align-items: stretch !important;
    }

    .current-email-badge {
      display: flex !important;
      flex-wrap: wrap !important;
      align-items: center !important;
      justify-content: flex-start !important;
      gap: 8px 12px !important;
      width: 100% !important;
      box-sizing: border-box !important;
    }

    .email-text {
      word-break: break-all !important;
      overflow-wrap: anywhere !important;
    }

    .change-email-row {
      flex-direction: column !important;
      align-items: stretch !important;
      gap: 8px !important;
    }

    .change-email-row .text-input {
      width: 100% !important;
    }

    .change-email-row .btn-primary-action {
      width: 100% !important;
    }

    .avatar-edit-controls {
      flex-direction: column !important;
      align-items: flex-start !important;
      gap: 12px !important;
      width: 100% !important;
    }

    .auth-tabs {
      flex-direction: column !important;
      width: 100% !important;
    }

    .auth-tab-btn {
      width: 100% !important;
      text-align: center !important;
    }

    .settings-sidebar-backdrop {
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background: rgba(0, 0, 0, 0.55);
      backdrop-filter: blur(2px);
      -webkit-backdrop-filter: blur(2px);
      z-index: 99;
    }

    .settings-sidebar {
      position: absolute !important;
      top: 0;
      left: 0;
      bottom: 0;
      z-index: 100;
      width: 280px !important;
      max-width: 85vw !important;
      background-color: var(--bg-secondary) !important;
      box-shadow: 4px 0 20px rgba(0, 0, 0, 0.5);
      transform: translateX(-100%);
      transition: transform 0.25s ease-in-out !important;
    }

    .settings-sidebar.show-mobile {
      transform: translateX(0) !important;
    }
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
  @keyframes loginSpin {
    to { transform: rotate(360deg); }
  }

  .switch { position: relative; display: inline-block; width: 38px; height: 22px; flex-shrink: 0; }
  .switch input { opacity: 0; width: 0; height: 0; }
  .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background: rgba(139, 128, 163, 0.32); border-radius: 22px; transition: 0.2s; }
  .slider::before { content: ""; position: absolute; height: 16px; width: 16px; left: 3px; bottom: 3px; background: white; border-radius: 50%; transition: 0.2s; }
  .switch input:checked + .slider { background: var(--accent, #8E74BD); }
  .switch input:checked + .slider::before { transform: translateX(16px); }
  /* Assistant Section Styles */
  .assistant-hero-profile {
    display: flex;
    align-items: center;
    gap: 28px;
    padding: 8px 0;
  }

  @media (max-width: 640px) {
    .assistant-hero-profile {
      flex-direction: column;
      align-items: center;
      gap: 16px;
    }

    .devices-grid {
      width: 100% !important;
      min-width: 0 !important;
    }

    .device-item-card {
      flex-wrap: wrap !important;
      padding: 12px 14px !important;
      gap: 10px !important;
      width: 100% !important;
      box-sizing: border-box !important;
      min-width: 0 !important;
    }

    .device-icon-wrap {
      margin-top: 2px !important;
    }

    .device-details {
      flex: 1 1 calc(100% - 44px) !important;
      min-width: 0 !important;
    }

    .device-name {
      font-size: 14px !important;
      word-break: break-word !important;
    }

    .device-meta {
      flex-wrap: wrap !important;
      gap: 4px 8px !important;
    }

    .device-linkid {
      word-break: break-all !important;
    }

    .device-status-wrap {
      width: 100% !important;
      justify-content: flex-start !important;
      flex-wrap: wrap !important;
      padding-top: 8px !important;
      margin-top: 2px !important;
      border-top: 1px solid var(--border-color) !important;
      gap: 6px !important;
    }

    .device-status-wrap .btn-icon-danger {
      margin-left: auto !important;
    }

    .device-status-wrap .btn-primary-action {
      width: 100% !important;
      justify-content: center !important;
      text-align: center !important;
    }

    .connect-input-row {
      flex-direction: column !important;
      gap: 8px !important;
    }

    .connect-input-row .text-input {
      width: 100% !important;
    }

    .connect-input-row .btn-primary-action {
      width: 100% !important;
      text-align: center !important;
      justify-content: center !important;
    }

    .detected-header-row {
      flex-direction: column !important;
      align-items: stretch !important;
      gap: 12px !important;
    }

    .detected-header-actions {
      width: 100% !important;
      display: flex !important;
      gap: 8px !important;
    }

    .detected-header-actions button {
      flex: 1 !important;
      justify-content: center !important;
    }
  }

  .assistant-avatar-box {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 10px;
    flex-shrink: 0;
  }

  .assistant-avatar-preview-btn {
    position: relative;
    width: 104px;
    height: 104px;
    border-radius: 50%;
    border: 3px solid var(--accent, #8E74BD);
    box-shadow: 0 0 20px var(--accent-glow, rgba(142, 116, 189, 0.40));
    background: #181720;
    overflow: hidden;
    padding: 0;
    cursor: pointer;
    transition: transform 0.2s ease, box-shadow 0.2s ease;
  }

  .assistant-avatar-preview-btn:hover {
    transform: scale(1.04);
    box-shadow: 0 0 26px var(--accent-glow, rgba(142, 116, 189, 0.58));
  }

  .assistant-img {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }

  .assistant-img-placeholder {
    width: 100%;
    height: 100%;
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--accent, #8E74BD);
  }

  .avatar-label-btn {
    background: transparent;
    border: none;
    color: var(--accent, #8E74BD);
    font-size: 0.82rem;
    font-weight: 600;
    cursor: pointer;
    padding: 2px 6px;
    border-radius: 6px;
    transition: color 0.15s;
  }

  .avatar-label-btn:hover {
    text-decoration: underline;
  }

  .assistant-identity-fields {
    flex: 1;
    width: 100%;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }

  .field-item {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .field-label {
    font-size: 0.82rem;
    font-weight: 600;
    color: var(--text-secondary);
  }

  .text-input-styled {
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid var(--border-color);
    border-radius: 10px;
    padding: 8px 14px;
    color: var(--text-primary);
    font-size: 0.92rem;
    outline: none;
    transition: border-color 0.2s, box-shadow 0.2s;
    font-family: inherit;
  }

  .text-input-styled:focus {
    border-color: var(--accent, #8E74BD);
    box-shadow: 0 0 0 2px var(--accent-glow, rgba(142, 116, 189, 0.22));
  }

  select.text-input-styled {
    appearance: none;
    -webkit-appearance: none;
    -moz-appearance: none;
    padding: 9px 38px 9px 14px;
    background-repeat: no-repeat !important;
    background-position: right 14px center !important;
    background-size: 14px 14px !important;
    cursor: pointer;
    line-height: 1.4;
    width: 100%;
    box-sizing: border-box;
  }

  .dark-theme .text-input-styled {
    background-color: rgba(255, 255, 255, 0.05);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
  }

  .light-theme .text-input-styled {
    background-color: #ffffff;
    border: 1px solid var(--border-color);
    color: var(--text-primary);
  }

  .dark-theme select.text-input-styled {
    color-scheme: dark;
    background-color: var(--bg-card, #252330);
    color: var(--text-primary, #ffffff);
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%238B80A3' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E");
    background-repeat: no-repeat !important;
    background-position: right 14px center !important;
    background-size: 14px 14px !important;
  }

  .light-theme select.text-input-styled {
    color-scheme: light;
    background-color: #ffffff;
    color: var(--text-primary, #181326);
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%235B5270' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E");
    background-repeat: no-repeat !important;
    background-position: right 14px center !important;
    background-size: 14px 14px !important;
  }

  .dark-theme select.text-input-styled option,
  .dark-theme select.text-input-styled optgroup {
    background-color: #252330 !important;
    color: #ffffff !important;
  }

  .light-theme select.text-input-styled option,
  .light-theme select.text-input-styled optgroup {
    background-color: #ffffff !important;
    color: #181326 !important;
  }

  .card-header-flex {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 12px;
  }

  .prompt-mode-indicator {
    display: flex;
    align-items: center;
  }

  .mode-badge {
    font-size: 0.76rem;
    font-weight: 600;
    padding: 3px 10px;
    border-radius: 12px;
    letter-spacing: 0.02em;
  }

  .mode-badge.work {
    background: var(--accent-soft, rgba(142, 116, 189, 0.16));
    color: var(--accent, #A28AD0);
    border: 1px solid var(--accent-border, rgba(162, 138, 208, 0.35));
  }

  .mode-badge.fun {
    background: rgba(166, 85, 129, 0.15);
    color: #CA7DA5;
    border: 1px solid rgba(202, 125, 165, 0.3);
  }

  .prompt-editor-box {
    margin-bottom: 12px;
  }

  .prompt-textarea {
    width: 100%;
    background: rgba(255, 255, 255, 0.04);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    padding: 12px 14px;
    color: var(--text-primary);
    font-size: 0.9rem;
    line-height: 1.5;
    outline: none;
    resize: vertical;
    font-family: inherit;
    box-sizing: border-box;
    transition: border-color 0.2s, box-shadow 0.2s;
    scrollbar-width: thin;
    scrollbar-color: rgba(139, 128, 163, 0.32) transparent;
  }

  .prompt-textarea:focus {
    border-color: var(--accent, #8E74BD);
    box-shadow: 0 0 0 2px var(--accent-glow, rgba(142, 116, 189, 0.22));
  }

  .prompt-textarea.fun-tint:focus {
    border-color: #A65581;
    box-shadow: 0 0 0 2px rgba(166, 85, 129, 0.2);
  }

  .prompt-textarea::-webkit-scrollbar {
    width: 6px;
    height: 6px;
  }

  .prompt-textarea::-webkit-scrollbar-track {
    background: transparent;
  }

  .prompt-textarea::-webkit-scrollbar-thumb {
    background: rgba(139, 128, 163, 0.28);
    border-radius: 3px;
  }

  .prompt-textarea::-webkit-scrollbar-thumb:hover {
    background: rgba(139, 128, 163, 0.48);
  }

  .prompt-textarea::-webkit-resizer {
    background-color: transparent;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='10' viewBox='0 0 10 10' fill='none'%3E%3Cpath d='M9 2L2 9M9 6L6 9' stroke='%238B80A3' stroke-width='1.5' stroke-linecap='round'/%3E%3C/svg%3E");
    background-repeat: no-repeat;
    background-position: bottom 3px right 3px;
    background-size: 8px 8px;
  }

  .prompt-switch-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
    flex-wrap: wrap;
  }

  .prompt-mode-toggle {
    display: inline-flex;
    background: rgba(255, 255, 255, 0.06);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    padding: 3px;
    gap: 4px;
  }

  .mode-toggle-btn {
    border: none;
    background: transparent;
    color: var(--text-secondary);
    padding: 6px 14px;
    border-radius: 9px;
    cursor: pointer;
    font-size: 0.85rem;
    font-weight: 600;
    display: inline-flex;
    align-items: center;
    gap: 6px;
    transition: all 0.2s ease;
    font-family: inherit;
  }

  .mode-toggle-btn:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.05);
  }

  .mode-toggle-btn.active {
    background: var(--accent, #8E74BD);
    color: #ffffff;
    box-shadow: 0 2px 8px var(--accent-glow, rgba(142, 116, 189, 0.35));
  }

  .mode-toggle-btn:nth-child(2).active {
    background: #733557;
    box-shadow: 0 2px 8px rgba(115, 53, 87, 0.35);
  }

  .prompt-mode-desc {
    flex: 1;
    min-width: 240px;
    font-size: 0.78rem;
    color: var(--text-muted);
    line-height: 1.35;
  }

  .storage-picker-row {
    display: flex;
    gap: 10px;
    align-items: center;
    margin-top: 8px;
  }

  .flex-1 {
    flex: 1;
  }

  .assistant-save-bar {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 16px;
    padding-top: 10px;
    flex-wrap: wrap;
  }

  .save-status-msg {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 0.85rem;
    font-weight: 500;
    padding: 6px 12px;
    border-radius: 8px;
  }

  .save-status-msg.success {
    background: rgba(16, 185, 129, 0.12);
    color: #34d399;
    border: 1px solid rgba(16, 185, 129, 0.25);
  }

  .save-status-msg.error {
    background: rgba(239, 68, 68, 0.12);
    color: #f87171;
    border: 1px solid rgba(239, 68, 68, 0.25);
  }

  .btn-primary-lg {
    background: var(--accent, #8E74BD);
    border: 1px solid var(--accent, #8E74BD);
    color: #ffffff;
    padding: 10px 24px;
    border-radius: 6px;
    font-size: 0.92rem;
    font-weight: 600;
    display: inline-flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
    box-shadow: 0 2px 8px var(--accent-soft, rgba(142, 116, 189, 0.24));
    transition: all 0.2s ease;
    font-family: inherit;
  }

  .btn-primary-lg:hover:not(:disabled) {
    background: var(--accent-hover, #A28AD0);
    border-color: var(--accent-hover, #A28AD0);
    box-shadow: 0 4px 14px var(--accent-glow, rgba(142, 116, 189, 0.40));
    transform: translateY(-1px);
  }

  .btn-primary-lg:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
  .source-chooser-backdrop {
    position: fixed;
    top: 0; left: 0; right: 0; bottom: 0;
    background: rgba(8, 6, 12, 0.80);
    display: flex;
    justify-content: center;
    align-items: center;
    padding: 20px;
    z-index: 1100;
    backdrop-filter: blur(4px);
  }

  .source-chooser {
    background: var(--bg-card, #252330);
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
    color: var(--text-muted, #8B80A3);
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
    border-color: var(--accent, #8E74BD);
    background: var(--bg-hover, var(--bg-hover, rgba(142, 116, 189, 0.10)));
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

  .source-cancel:hover {
    background: var(--bg-hover, rgba(255, 255, 255, 0.05));
  }
</style>
