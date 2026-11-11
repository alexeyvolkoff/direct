<script>
  import { onMount, onDestroy, createEventDispatcher } from "svelte";
  import { Capacitor } from "@capacitor/core";
  import { _, locale, setLocale, SUPPORTED_LANGUAGES } from "../lib/i18n/index.js";
  import { isFileSystemAccessAvailable, addShareFromPicker, getOrCreateLocalLinkId, getDefaultWebDeviceName } from "../lib/local-client.js";
  import { persistStoragePath, parseStoragePath, cleanStoragePath, DEFAULT_STORAGE_PATH } from "../lib/storage-path.js";
  import { isNativeStorageAvailable } from "../lib/native-drive.js";
  import { fetchNearbyDevices } from "../lib/local-discovery.js";
  import { getStoredOmdKey, omdKeyStore } from "../lib/omd-key.js";
  import {
    aiNodeStore,
    AI_NODE_MODE_AUTO,
    AI_NODE_MODE_PUBLIC,
    AI_NODE_MODE_CUSTOM,
    setAiNodeChoice,
    acceptPublicAiAgreement,
    refreshPublicAiNode,
    connectGuestAiNode,
    addCustomAiNode
  } from "../lib/ai-node.js";
  import { ensureAssistantDefaults } from "../lib/assistant-defaults.js";
  import { getChatStore } from "../lib/chat-store.js";
  import { HttpDrive } from "../lib/http-drive.js";
  import { profileStore } from "../lib/profile-store.js";

  export let isOpen = false;
  export let identity = null;
  export let devices = [];
  export let isDarkTheme = false;
  export let activeDrive = null;

  const dispatch = createEventDispatcher();

  // Step 1: Auth (already done before opening, marked as completed)
  // Step 2: Storage folder
  // Step 3: AI Node selection
  // Step 4: Congrats with special effects
  let currentStep = 2;

  let wasOpen = false;

  // DID-configured state flags
  let didHasConfiguredStorage = false;
  let didDefaultStoragePath = "";
  let didHasConfiguredAiNode = false;
  let didAiNodeChoice = "";
  let didHasConfiguredAssistant = false;
  let didSyncLoaded = false;

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

  function getRemoteSubfolder(dev = null) {
    if (didHasConfiguredStorage && didDefaultStoragePath) {
      const parsed = parseStoragePath(didDefaultStoragePath);
      if (parsed.folder && parsed.folder !== "/") {
        return parsed.folder.startsWith("/") ? parsed.folder : "/" + parsed.folder;
      }
    }
    return "/Data";
  }

  function applyConfiguredStorage(rawPath) {
    if (!rawPath) return;
    const clean = cleanStoragePath(rawPath);
    if (
      !clean ||
      clean === DEFAULT_STORAGE_PATH ||
      clean === "/Documents/Data" ||
      clean === "/Private/Data" ||
      clean === "/Documents/OnMyChat" ||
      clean === "/Private/OnMyChat"
    ) {
      return;
    }
    const parsed = parseStoragePath(clean);
    didHasConfiguredStorage = true;
    didDefaultStoragePath = clean;
    storageChoice = "remote_device";
    storagePath = clean;

    if (remoteDevices && remoteDevices.length > 0) {
      const match = remoteDevices.find((d) => {
        if (!d) return false;
        if (parsed.deviceName && (
          (d.name && d.name.toLowerCase() === parsed.deviceName.toLowerCase()) ||
          (d.linkId && d.linkId === parsed.deviceName) ||
          (d.prefix && d.prefix.replace(/^\//, "").toLowerCase() === parsed.deviceName.toLowerCase())
        )) return true;
        return false;
      });
      if (match) {
        selectedRemoteDevice = match;
      } else if (!selectedRemoteDevice) {
        selectedRemoteDevice = remoteDevices[0];
      }
    }
  }

  function applyConfiguredAiNode(nodeChoice) {
    if (!nodeChoice || typeof nodeChoice !== "string") return;
    const choice = nodeChoice.trim();
    if (!choice) return;
    didHasConfiguredAiNode = true;
    didAiNodeChoice = choice;

    if (choice === AI_NODE_MODE_AUTO) {
      selectedAiChoice = AI_NODE_MODE_AUTO;
    } else if (choice === AI_NODE_MODE_PUBLIC) {
      selectedAiChoice = AI_NODE_MODE_PUBLIC;
    } else if (embeddedDevices.some((d) => d.linkId === choice)) {
      selectedAiChoice = choice;
      selectedEmbeddedLinkId = choice;
    } else if (devices.some((d) => d && (d.linkId === choice || d.deviceId === choice))) {
      selectedAiChoice = choice;
    } else {
      selectedAiChoice = "custom";
      customNodeLinkId = choice;
    }
  }

  async function syncDefaultsFromIdentity() {
    // 1. Check synchronous fast paths first
    const fastStorage =
      $profileStore?.defaultStorage ||
      identity?.profile?.defaultStorage ||
      (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "");
    if (fastStorage) {
      applyConfiguredStorage(fastStorage);
    }

    const fastAi =
      $aiNodeStore?.choice ||
      (typeof localStorage !== "undefined" ? localStorage.getItem("omd_ai_node_choice") : "");
    if (fastAi && fastAi !== AI_NODE_MODE_AUTO) {
      applyConfiguredAiNode(fastAi);
    }

    // 2. Read DID GunDB settings store
    try {
      const key = getSettingsKey();
      if (key) {
        const store = getChatStore(key);
        const settings = await store.getAssistantSettings();
        if (settings) {
          if (settings.defaultStorage) {
            applyConfiguredStorage(settings.defaultStorage);
          }
          if (settings.ai_node) {
            applyConfiguredAiNode(settings.ai_node);
          }
          if (settings.assistant_name || settings.title || settings.assistant_avatar) {
            didHasConfiguredAssistant = true;
            if (settings.assistant_name) assistantSummary.name = settings.assistant_name;
            if (settings.title) assistantSummary.title = settings.title;
            if (settings.assistant_avatar && typeof settings.assistant_avatar === "string") {
              const rawAvatar = settings.assistant_avatar.trim();
              if (rawAvatar.startsWith("data:image/")) {
                assistantSummary.avatarUrl = rawAvatar;
                assistantSummary.ready = true;
              } else if (rawAvatar.length > 50 && /^[A-Za-z0-9+/=]+$/.test(rawAvatar.replace(/\s+/g, ""))) {
                assistantSummary.avatarUrl = "data:image/jpeg;base64," + rawAvatar;
                assistantSummary.ready = true;
              }
            }
          }
        }
      }
    } catch (e) {
      console.warn("[Onboarding] Error syncing defaults from DID:", e);
    } finally {
      didSyncLoaded = true;
    }
  }

  $: if (isOpen && !wasOpen) {
    wasOpen = true;
    currentStep = 2;
    savingNode = false;
    storageChoice = isNativeStorageAvailable() ? "mobile_native" : "opfs";
    storagePath = isNativeStorageAvailable() ? "/Documents/Data" : DEFAULT_STORAGE_PATH;
    syncDefaultsFromIdentity().catch(() => {});
    fetchAssistantSummary().catch(() => {});
  } else if (!isOpen && wasOpen) {
    wasOpen = false;
  }

  $: isMobile = isNativeStorageAvailable();

  // Step 2 state: Storage
  // 'opfs' | 'local_folder' | 'mobile_native' | 'remote_device'
  let storageChoice = isNativeStorageAvailable() ? "mobile_native" : "opfs";
  let localFolderName = "";
  let storagePath = isNativeStorageAvailable() ? "/Documents/Data" : DEFAULT_STORAGE_PATH;
  let pickingFolder = false;
  let folderPickError = "";

  // Remote Device State (Option C: Connect Device)
  let selectedRemoteDevice = null;
  let remoteLinkId = "";
  let detectedNearby = [];
  let scanningNearby = false;
  let pendingPairLinkId = "";

  $: localLinkId = getOrCreateLocalLinkId();
  $: localDevName = (typeof localStorage !== "undefined" ? localStorage.getItem("omd_local_devicename") : "") || getDefaultWebDeviceName();

  $: remoteDevices = (devices || []).filter(
    (d) => d && !d.isLocal && !d.local && !d.isAiNode && d.linkId !== localLinkId && d.name !== localDevName
  );

  // Auto-select remote device if one was just paired or if remote_device chosen
  $: if (pendingPairLinkId && remoteDevices.length) {
    const found = remoteDevices.find((d) => d.linkId === pendingPairLinkId);
    if (found) {
      selectedRemoteDevice = found;
      storageChoice = "remote_device";
      pendingPairLinkId = "";
    }
  }

  // Preselect matching remote device if DID has configured storage
  $: if (didHasConfiguredStorage && didDefaultStoragePath && remoteDevices.length) {
    const parsed = parseStoragePath(didDefaultStoragePath);
    const match = remoteDevices.find((d) => {
      if (!d) return false;
      if (parsed.deviceName && (
        (d.name && d.name.toLowerCase() === parsed.deviceName.toLowerCase()) ||
        (d.linkId && d.linkId === parsed.deviceName) ||
        (d.prefix && d.prefix.replace(/^\//, "").toLowerCase() === parsed.deviceName.toLowerCase())
      )) return true;
      return false;
    });
    if (match && (!selectedRemoteDevice || selectedRemoteDevice.linkId !== match.linkId)) {
      selectedRemoteDevice = match;
      if (storageChoice !== "remote_device") {
        storageChoice = "remote_device";
      }
    }
  }

  $: if (storageChoice === "remote_device" && !selectedRemoteDevice && remoteDevices.length) {
    selectedRemoteDevice = remoteDevices[0];
  }

  $: if (selectedRemoteDevice && !remoteDevices.some((d) => d.linkId === selectedRemoteDevice.linkId)) {
    selectedRemoteDevice = remoteDevices[0] || null;
  }

  // Step 3 state: AI Node
  let selectedAiChoice = AI_NODE_MODE_AUTO;
  let customNodeLinkId = "";
  let termsAccepted = true;
  let savingNode = false;

  // Re-match embedded AI node if DID configured choice arrives after devices populate
  $: if (didHasConfiguredAiNode && didAiNodeChoice && embeddedDevices.length) {
    if (embeddedDevices.some((d) => d.linkId === didAiNodeChoice)) {
      selectedAiChoice = didAiNodeChoice;
      selectedEmbeddedLinkId = didAiNodeChoice;
    }
  }

  // Step 4 state: Animation & Summary
  let canvasEl;
  let animFrameId;
  let assistantSummary = { name: "June", title: "Junior assistant", avatarUrl: "", ready: false };
  let userLogin = "User";
  $: {
    const pUser = ($profileStore?.user || "").trim();
    const idUser = (identity?.profile?.user || "").trim();
    const pName = ($profileStore?.name || "").trim();
    const idName = (identity?.profile?.name || "").trim();

    const isValidUser = (u) => u && u.toLowerCase() !== "anonymous" && u.toLowerCase() !== "user";

    if (isValidUser(pUser)) {
      userLogin = pUser;
    } else if (isValidUser(idUser)) {
      userLogin = idUser;
    } else if (pName && pName !== "Guest User" && pName !== "User") {
      userLogin = pName;
    } else if (idName && idName !== "Guest User" && idName !== "User") {
      userLogin = idName;
    } else if (identity?.type === "ephemeral" || $profileStore?.isEphemeral) {
      userLogin = "demo";
    } else {
      userLogin = $_("user", {}, "User");
    }
  }
  $: hasFsAccess = isFileSystemAccessAvailable();
  $: activeLangMeta = SUPPORTED_LANGUAGES.find((l) => l.code === $locale) || SUPPORTED_LANGUAGES[0];

  // Detect user's own embedded/advanced devices suitable for inference (exclude web/mobile)
  $: embeddedDevices = (devices || []).filter((d) => {
    if (!d || d.isLocal || d.local || d.isAiNode) return false;
    if (d.type === "ai" || d.type === "ai-node") return false;
    const dt = (d.deviceType || "").toLowerCase();
    const t = (d.type || "").toLowerCase();
    if (dt === "web" || t === "web" || dt === "phone" || t === "phone" || dt === "tablet" || t === "tablet") {
      return false;
    }
    return dt === "embedded" || t === "embedded" || t === "node";
  });

  let selectedEmbeddedLinkId = "";
  $: if (embeddedDevices.length && !selectedEmbeddedLinkId) {
    selectedEmbeddedLinkId = embeddedDevices[0].linkId;
  }
  $: isEmbeddedDeviceSelected = embeddedDevices.some((d) => d.linkId === selectedAiChoice);
  $: selectedEmbeddedDevice = embeddedDevices.find((d) => d.linkId === selectedEmbeddedLinkId) || embeddedDevices[0];
  $: selectedEmbeddedDeviceName = selectedEmbeddedDevice?.name || selectedEmbeddedDevice?.linkId || "";
  $: if (isEmbeddedDeviceSelected && selectedAiChoice) {
    selectedEmbeddedLinkId = selectedAiChoice;
  }

  function handleSelectEmbeddedOption() {
    if (selectedEmbeddedLinkId) {
      selectedAiChoice = selectedEmbeddedLinkId;
    } else if (embeddedDevices.length > 0) {
      selectedEmbeddedLinkId = embeddedDevices[0].linkId;
      selectedAiChoice = selectedEmbeddedLinkId;
    }
  }

  function handleEmbeddedDeviceChange(e) {
    selectedEmbeddedLinkId = e.target.value;
    selectedAiChoice = selectedEmbeddedLinkId;
  }

  let connectingCustomNode = false;
  let customNodeError = "";
  let customNodeSuccess = "";

  async function handleConnectCustomNode() {
    const linkId = (customNodeLinkId || "").trim();
    if (!linkId) return;
    connectingCustomNode = true;
    customNodeError = "";
    customNodeSuccess = "";
    try {
      const didPub = identity?.mainPub || "";
      const res = await connectGuestAiNode(linkId, didPub);
      addCustomAiNode({ linkId: res.linkId, name: res.name });
      customNodeSuccess = res.name || res.linkId;
      selectedAiChoice = res.linkId;
    } catch (err) {
      console.warn("[Onboarding] connectGuestAiNode error, opening pair fallback:", err);
      // Fallback: also dispatch pairDevice so PairModal can open if credentials needed
      dispatch("pairDevice", { linkId });
    } finally {
      connectingCustomNode = false;
    }
  }

  onMount(() => {
    if (isNativeStorageAvailable()) {
      storageChoice = "mobile_native";
      storagePath = "/Documents/Data";
    }
    syncDefaultsFromIdentity().catch(() => {});
    refreshNearby().catch(() => {});
    refreshPublicAiNode().catch(() => {});
    fetchAssistantSummary().catch(() => {});
  });

  async function refreshNearby() {
    scanningNearby = true;
    try {
      const list = await fetchNearbyDevices();
      const activeIds = new Set((devices || []).map((d) => d.linkId));
      detectedNearby = (list || []).filter((d) => d.linkId && !activeIds.has(d.linkId) && d.linkId !== localLinkId && d.name !== localDevName);
    } catch (err) {
      console.warn("[Onboarding] fetchNearbyDevices error:", err);
    } finally {
      scanningNearby = false;
    }
  }

  function handleConnectRemote(linkIdToUse) {
    const id = (linkIdToUse || remoteLinkId).trim();
    if (!id) return;
    pendingPairLinkId = id;
    remoteLinkId = "";
    dispatch("pairDevice", { linkId: id });
  }

  async function handlePickLocalFolder() {
    folderPickError = "";
    pickingFolder = true;
    try {
      const share = await addShareFromPicker();
      if (share && share.name) {
        localFolderName = share.name;
        storageChoice = "local_folder";
        storagePath = "/" + share.name + "/Data";
      }
    } catch (err) {
      if (err?.name !== "AbortError" && err?.code !== 20) {
        folderPickError = err?.message || $_("failed_to_add_folder", {}, "Failed to add folder");
      }
    } finally {
      pickingFolder = false;
    }
  }

  function goToStep3() {
    currentStep = 3;
    refreshPublicAiNode().catch(() => {});
    fetchAssistantSummary().catch(() => {});
  }

  let lastFetchedChoice = null;
  $: if (currentStep >= 3 && selectedAiChoice !== lastFetchedChoice) {
    lastFetchedChoice = selectedAiChoice;
    fetchAssistantSummary(selectedAiChoice).catch(() => {});
  }

  function resolveSelectedAiDrive(effectiveNodeChoice) {
    let targetId = null;
    if (effectiveNodeChoice && effectiveNodeChoice !== AI_NODE_MODE_AUTO && effectiveNodeChoice !== AI_NODE_MODE_PUBLIC) {
      targetId = effectiveNodeChoice;
    } else if (effectiveNodeChoice === AI_NODE_MODE_AUTO && embeddedDevices.length > 0) {
      targetId = embeddedDevices[0].linkId;
    } else if ($aiNodeStore?.info?.linkID) {
      targetId = $aiNodeStore.info.linkID;
    }

    if (targetId) {
      if (typeof window !== "undefined" && window.deviceRegistry?.get(targetId)?.drive) {
        return window.deviceRegistry.get(targetId).drive;
      }
      const matchedDev = (devices || []).find((d) => d && (d.linkId === targetId || d.deviceId === targetId));
      if (matchedDev?.drive) return matchedDev.drive;
      const token = identity?.token || (typeof localStorage !== "undefined" ? localStorage.getItem("omd_key") : "");
      const isPrivate = targetId !== $aiNodeStore?.info?.linkID;
      return new HttpDrive({ targetLinkId: targetId, port: 4000, isPrivateMode: isPrivate, authToken: token });
    }

    return activeDrive || null;
  }

  async function loadAssistantInfoForDrive(drive) {
    if (!drive || typeof drive.fetch !== "function") return;
    try {
      const res = await drive.fetch("/assistant");
      if (res && res.ok) {
        const data = await res.json();
        const info = data.result || data;
        if (info && (info.assistant_name || info.title)) {
          assistantSummary = {
            ...assistantSummary,
            name: info.assistant_name || assistantSummary.name,
            title: info.title || assistantSummary.title,
            ready: true,
          };
        }
      }
    } catch (err) {
      console.warn("[Onboarding] Error querying /assistant from node:", err);
    }

    try {
      const resAv = await drive.fetch("/assistant/avatar?size=160");
      if (resAv && resAv.ok) {
        const blob = await resAv.blob();
        if (blob && blob.size > 0) {
          if (assistantSummary.avatarUrl && assistantSummary.avatarUrl.startsWith("blob:")) {
            try { URL.revokeObjectURL(assistantSummary.avatarUrl); } catch (_) {}
          }
          assistantSummary = {
            ...assistantSummary,
            avatarUrl: URL.createObjectURL(blob),
          };
        }
      }
    } catch (_) {}
  }

  let assistantFetchSeq = 0;
  async function fetchAssistantSummary(effectiveChoice) {
    const seq = ++assistantFetchSeq;
    const choice = effectiveChoice !== undefined ? effectiveChoice : (
      selectedAiChoice === "custom" && customNodeLinkId.trim()
        ? customNodeLinkId.trim()
        : selectedAiChoice
    );

    // 1. Check chatStore for existing assistant settings / avatar (GunDB or localStorage mirror)
    try {
      const key = getSettingsKey();
      if (key) {
        const store = getChatStore(key);
        const settings = await store.getAssistantSettings();
        if (seq !== assistantFetchSeq) return;
        if (settings) {
          if (settings.assistant_name) {
            assistantSummary.name = settings.assistant_name;
            didHasConfiguredAssistant = true;
          }
          if (settings.title) {
            assistantSummary.title = settings.title;
            didHasConfiguredAssistant = true;
          }
          if (settings.assistant_avatar && typeof settings.assistant_avatar === "string") {
            const rawAvatar = settings.assistant_avatar.trim();
            if (rawAvatar.startsWith("data:image/")) {
              assistantSummary.avatarUrl = rawAvatar;
              assistantSummary.ready = true;
              didHasConfiguredAssistant = true;
            } else if (rawAvatar.length > 50 && /^[A-Za-z0-9+/=]+$/.test(rawAvatar.replace(/\s+/g, ""))) {
              assistantSummary.avatarUrl = "data:image/jpeg;base64," + rawAvatar;
              assistantSummary.ready = true;
              didHasConfiguredAssistant = true;
            }
          }
        }
      }
    } catch (_) {}

    // If DID already has custom assistant configured, keep it and do not overwrite with node defaults or June
    if (didHasConfiguredAssistant && assistantSummary.avatarUrl) {
      return;
    }

    // 2. Query the selected drive
    let targetDrive = null;
    try {
      targetDrive = resolveSelectedAiDrive(choice);
    } catch (_) {}

    if (targetDrive && typeof targetDrive.fetch === "function") {
      try {
        await loadAssistantInfoForDrive(targetDrive);
        if (seq !== assistantFetchSeq) return;
      } catch (_) {}
    }

    // 3. Fallback to same-origin fetch (/assistant & /assistant/avatar?size=160)
    // Matches the local node or dev proxy endpoint directly
    if (!assistantSummary.avatarUrl || !assistantSummary.ready) {
      try {
        const resAv = await window.fetch("/assistant/avatar?size=160");
        if (seq !== assistantFetchSeq) return;
        if (resAv && resAv.ok) {
          const blob = await resAv.blob();
          if (blob && blob.size > 0) {
            if (assistantSummary.avatarUrl && assistantSummary.avatarUrl.startsWith("blob:")) {
              try { URL.revokeObjectURL(assistantSummary.avatarUrl); } catch (_) {}
            }
            assistantSummary = {
              ...assistantSummary,
              avatarUrl: URL.createObjectURL(blob),
            };
          }
        }
      } catch (_) {}

      if (!assistantSummary.avatarUrl) {
        try {
          const resImg = await window.fetch("/img/june.png");
          if (resImg && resImg.ok) {
            const blob = await resImg.blob();
            if (blob && blob.size > 0) {
              assistantSummary = {
                ...assistantSummary,
                avatarUrl: URL.createObjectURL(blob),
              };
            }
          }
        } catch (_) {}
      }

      try {
        const resInfo = await window.fetch("/assistant");
        if (seq !== assistantFetchSeq) return;
        if (resInfo && resInfo.ok) {
          const data = await resInfo.json();
          const info = data.result || data;
          if (info && (info.assistant_name || info.title)) {
            assistantSummary = {
              ...assistantSummary,
              name: didHasConfiguredAssistant && assistantSummary.name ? assistantSummary.name : (info.assistant_name || assistantSummary.name),
              title: didHasConfiguredAssistant && assistantSummary.title ? assistantSummary.title : (info.title || assistantSummary.title),
              ready: true,
            };
          }
        }
      } catch (_) {}
    }
  }

  async function finishConfiguration() {
    savingNode = true;
    try {
      // 1. Persist storage path
      let chosenPath = DEFAULT_STORAGE_PATH;
      if (storageChoice === "mobile_native") {
        chosenPath = "/Documents/Data";
      } else if (storageChoice === "local_folder" && localFolderName) {
        chosenPath = "/" + localFolderName + "/Data";
      } else if (storageChoice === "remote_device" && selectedRemoteDevice) {
        const devName = selectedRemoteDevice.name || selectedRemoteDevice.linkId;
        chosenPath = "/" + devName + getRemoteSubfolder(selectedRemoteDevice);
      } else {
        chosenPath = DEFAULT_STORAGE_PATH;
      }
      await persistStoragePath(chosenPath, identity);

      // 2. Persist AI Node choice & accept terms if public/auto
      let effectiveNodeChoice = selectedAiChoice;
      if (selectedAiChoice === "custom" && customNodeLinkId.trim()) {
        effectiveNodeChoice = customNodeLinkId.trim();
      }
      setAiNodeChoice(effectiveNodeChoice);

      if (termsAccepted || effectiveNodeChoice === AI_NODE_MODE_AUTO || effectiveNodeChoice === AI_NODE_MODE_PUBLIC) {
        acceptPublicAiAgreement();
      }

      // 3. Mark onboarding as completed for this identity
      const key = "omd_onboarded_" + (identity?.did || $profileStore?.user || identity?.profile?.user || "default");
      try {
        localStorage.setItem(key, "true");
        localStorage.setItem("omd_onboarding_completed", "true");
      } catch (_) {}

      // 4. Try fetching real assistant info and avatar from the chosen AI node drive if user didn't have persona in DID
      if (!didHasConfiguredAssistant) {
        const targetDrive = resolveSelectedAiDrive(effectiveNodeChoice);
        const settingsKey = getSettingsKey();
        ensureAssistantDefaults(targetDrive, settingsKey, false).catch(() => {});
      }
      try {
        await Promise.race([
          fetchAssistantSummary(effectiveNodeChoice),
          new Promise((r) => setTimeout(r, 1200))
        ]);
      } catch (_) {}

      // Move to Congrats step!
      currentStep = 4;
      if (!assistantSummary.avatarUrl) {
        fetchAssistantSummary(effectiveNodeChoice).catch(() => {});
      }
      setTimeout(() => {
        startConfetti();
      }, 50);
    } catch (err) {
      console.error("[Onboarding] Error during save:", err);
      currentStep = 4;
      setTimeout(() => startConfetti(), 50);
    } finally {
      savingNode = false;
    }
  }

  // --- Confetti Engine (zero external dependencies, 60fps) ---
  function startConfetti() {
    if (!canvasEl) return;
    const ctx = canvasEl.getContext("2d");
    if (!ctx) return;

    const width = (canvasEl.width = canvasEl.offsetWidth);
    const height = (canvasEl.height = canvasEl.offsetHeight);

    const colors = [
      "#A65581", "#8E74BD", "#10B981", "#A28AD0",
      "#CA7DA5", "#F59E0B", "#7D60AC", "#F43F5E"
    ];

    const particles = [];
    const particleCount = 100;

    for (let i = 0; i < particleCount; i++) {
      particles.push({
        x: width / 2 + (Math.random() - 0.5) * 60,
        y: height * 0.45 + (Math.random() - 0.5) * 40,
        vx: (Math.random() - 0.5) * 16,
        vy: -Math.random() * 14 - 4,
        size: Math.random() * 8 + 4,
        color: colors[Math.floor(Math.random() * colors.length)],
        rotation: Math.random() * 360,
        vrot: (Math.random() - 0.5) * 10,
        gravity: 0.28,
        friction: 0.985,
        opacity: 1,
        life: 0,
        maxLife: Math.random() * 100 + 120,
      });
    }

    function render() {
      ctx.clearRect(0, 0, width, height);

      let activeCount = 0;
      for (const p of particles) {
        p.life++;
        p.vx *= p.friction;
        p.vy *= p.friction;
        p.vy += p.gravity;
        p.x += p.vx;
        p.y += p.vy;
        p.rotation += p.vrot;

        if (p.life > p.maxLife * 0.7) {
          p.opacity = Math.max(0, 1 - (p.life - p.maxLife * 0.7) / (p.maxLife * 0.3));
        }

        if (p.opacity > 0 && p.y < height + 50) {
          activeCount++;
          ctx.save();
          ctx.translate(p.x, p.y);
          ctx.rotate((p.rotation * Math.PI) / 180);
          ctx.globalAlpha = p.opacity;
          ctx.fillStyle = p.color;
          ctx.fillRect(-p.size / 2, -p.size / 2, p.size, p.size * 0.6);
          ctx.restore();
        }
      }

      if (activeCount > 0) {
        animFrameId = requestAnimationFrame(render);
      }
    }

    render();
  }

  function handleStartChat() {
    dispatch("open-chat");
    closeModal();
  }

  function handleStartFiles() {
    dispatch("open-files");
    closeModal();
  }

  function closeModal() {
    if (animFrameId) cancelAnimationFrame(animFrameId);
    isOpen = false;
    dispatch("complete");
  }

  onDestroy(() => {
    if (animFrameId) cancelAnimationFrame(animFrameId);
    if (assistantSummary?.avatarUrl && assistantSummary.avatarUrl.startsWith("blob:")) {
      try { URL.revokeObjectURL(assistantSummary.avatarUrl); } catch (_) {}
    }
  });
</script>

{#if isOpen}
  <div class="onboard-overlay {isDarkTheme ? 'dark' : 'light'}">
    <div class="onboard-modal-wrap">
      <div class="onboard-card">
        <!-- Stepper Indicator -->
        <div class="onboard-stepper">
          <div class="stepper-track">
            <div class="step-badge done" title={$_("onboarding_step_auth_done", {}, "Авторизация выполнена")}>
              <span class="step-icon">✓</span>
              <span class="step-label">{$_("onboarding_step_auth", {}, "1. Аккаунт")}</span>
            </div>
            <div class="step-line {currentStep >= 2 ? 'active' : ''}"></div>
            <div class="step-badge {currentStep === 2 ? 'active' : currentStep > 2 ? 'done' : ''}">
              <span class="step-icon">{currentStep > 2 ? '✓' : '2'}</span>
              <span class="step-label">{$_("onboarding_step_storage", {}, "2. Хранилище")}</span>
            </div>
            <div class="step-line {currentStep >= 3 ? 'active' : ''}"></div>
            <div class="step-badge {currentStep === 3 ? 'active' : currentStep > 3 ? 'done' : ''}">
              <span class="step-icon">{currentStep > 3 ? '✓' : '3'}</span>
              <span class="step-label">{$_("onboarding_step_ainode", {}, "3. AI-нода")}</span>
            </div>
            <div class="step-line {currentStep >= 4 ? 'active' : ''}"></div>
            <div class="step-badge {currentStep === 4 ? 'active done' : ''}">
              <span class="step-icon">🎉</span>
              <span class="step-label">{$_("onboarding_step_done", {}, "4. Готово")}</span>
            </div>
          </div>

          <!-- Language Selector (top right of modal) -->
          <div class="onboard-lang-select-wrap">
            <div class="onboard-lang-display" title={$_("language", {}, "Language")}>
              <svg class="onboard-lang-globe" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                <circle cx="12" cy="12" r="10"></circle>
                <line x1="2" y1="12" x2="22" y2="12"></line>
                <path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"></path>
              </svg>
              <span class="onboard-lang-code">{activeLangMeta.shortCode || activeLangMeta.code.toUpperCase()}</span>
              <svg class="onboard-lang-arrow" viewBox="0 0 24 24" width="11" height="11" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>
            </div>
            <select
              class="onboard-lang-select-hidden"
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
        </div>

        <!-- STEP 2: Storage Folder Selection -->
        {#if currentStep === 2}
          <div class="step-content">
            <div class="step-header">
              <div class="header-icon-box">📁</div>
              <h2>{$_("onboarding_storage_title", {}, "Куда сохранять материалы?")}</h2>
              <p class="subtitle">
                {$_("onboarding_storage_subtitle", {}, "Выберите папку по умолчанию для совместной работы с документами и сохранения генерируемых файлов")}
              </p>
            </div>

            <div class="options-grid">
              {#if isMobile}
                <!-- Option for Mobile: Native Storage /Documents/Data -->
                <label class="option-card {storageChoice === 'mobile_native' ? 'selected' : 'collapsed'}">
                  <input
                    type="radio"
                    name="storage"
                    value="mobile_native"
                    checked={storageChoice === 'mobile_native'}
                    on:change={() => { storageChoice = 'mobile_native'; }}
                  />
                  <div class="option-body">
                    <div class="option-top">
                      <span class="option-title">{$_("onboarding_storage_mobile_title", {}, "Хранилище телефона (Documents)")}</span>
                      {#if !didHasConfiguredStorage}
                        <span class="badge-rec">{$_("onboarding_recommended", {}, "Рекомендуется")}</span>
                      {/if}
                    </div>
                    {#if storageChoice === 'mobile_native'}
                      <div class="option-expand-content">
                        <p class="option-desc">
                          {$_("onboarding_storage_mobile_desc", {}, "Системная папка «Документы» на вашем мобильном устройстве. Файлы и результаты генераций сохраняются прямо в память телефона.")}
                        </p>
                        <div class="option-path-preview">
                          <code>/Documents/Data</code>
                        </div>
                      </div>
                    {/if}
                  </div>
                </label>
              {:else}
                <!-- Option A: OPFS Default (Desktop / Web) -->
                <label class="option-card {storageChoice === 'opfs' ? 'selected' : 'collapsed'}">
                  <input
                    type="radio"
                    name="storage"
                    value="opfs"
                    checked={storageChoice === 'opfs'}
                    on:change={() => { storageChoice = 'opfs'; }}
                  />
                  <div class="option-body">
                    <div class="option-top">
                      <span class="option-title">{$_("onboarding_storage_opfs_title", {}, "Встроенное хранилище браузера (OPFS)")}</span>
                      {#if !didHasConfiguredStorage}
                        <span class="badge-rec">{$_("onboarding_recommended", {}, "Рекомендуется")}</span>
                      {/if}
                    </div>
                    {#if storageChoice === 'opfs'}
                      <div class="option-expand-content">
                        <p class="option-desc">
                          {$_("onboarding_storage_opfs_desc", {}, "Быстрое приватное хранилище (Origin Private File System). Работает в любой операционной системе и не требует подтверждения прав доступа к файлам ОС.")}
                        </p>
                        <div class="option-path-preview">
                          <code>/Private/Data</code>
                        </div>
                      </div>
                    {/if}
                  </div>
                </label>

                <!-- Option B: Local Folder (File System Access - Desktop) -->
                <div
                  class="option-card {storageChoice === 'local_folder' ? 'selected' : 'collapsed'}"
                  role="button"
                  tabindex="0"
                  on:click={() => {
                    const wasSelected = storageChoice === 'local_folder';
                    storageChoice = 'local_folder';
                    if (hasFsAccess && (!localFolderName || wasSelected)) {
                      handlePickLocalFolder();
                    }
                  }}
                  on:keydown={(e) => {
                    if (e.key === 'Enter' || e.key === ' ') {
                      storageChoice = 'local_folder';
                      if (hasFsAccess) handlePickLocalFolder();
                    }
                  }}
                >
                  <input
                    type="radio"
                    name="storage"
                    value="local_folder"
                    checked={storageChoice === 'local_folder'}
                  />
                  <div class="option-body">
                    <div class="option-top">
                      <span class="option-title">{$_("onboarding_storage_local_title", {}, "Локальная папка на вашем компьютере")}</span>
                      {#if localFolderName}
                        <span class="badge-chosen">/{localFolderName}/Data</span>
                      {/if}
                    </div>
                    {#if storageChoice === 'local_folder'}
                      <div class="option-expand-content">
                        <p class="option-desc">
                          {$_("onboarding_storage_local_desc", {}, "Работайте с файлами прямо в папке вашего компьютера (например, «Документы» или «Проекты»). Кликните для выбора папки.")}
                        </p>
                        {#if localFolderName}
                          <div class="option-path-preview">
                            <code>/{localFolderName}/Data</code>
                          </div>
                        {:else if !hasFsAccess}
                          <span class="note-fs">{$_("onboarding_storage_local_note", {}, "Доступно в Chrome, Edge и Яндекс.Браузере")}</span>
                        {/if}
                        {#if folderPickError}
                          <div class="error-msg">{folderPickError}</div>
                        {/if}
                      </div>
                    {/if}
                  </div>
                </div>
              {/if}

              <!-- Option C: Connect Remote Device (Desktop, NAS, Server) -->
              <label class="option-card {storageChoice === 'remote_device' ? 'selected' : 'collapsed'}">
                <input
                  type="radio"
                  name="storage"
                  value="remote_device"
                  checked={storageChoice === 'remote_device'}
                  on:change={() => {
                    storageChoice = 'remote_device';
                    refreshNearby();
                  }}
                />
                <div class="option-body">
                  <div class="option-top">
                    <span class="option-title">{$_("onboarding_storage_remote_title", {}, "Подключить устройство Disk Direct")}</span>
                    {#if didHasConfiguredStorage}
                      <span class="badge-rec">{$_("onboarding_recommended", {}, "Рекомендуется")}</span>
                    {/if}
                    {#if selectedRemoteDevice}
                      <span class="badge-chosen">{$_("onboarding_selected_device", { name: selectedRemoteDevice.name || selectedRemoteDevice.linkId }, `Выбрано: ${selectedRemoteDevice.name || selectedRemoteDevice.linkId}`)}</span>
                    {/if}
                  </div>
                  {#if storageChoice === 'remote_device'}
                    <div class="option-expand-content">
                      <p class="option-desc">
                        {$_("onboarding_storage_remote_desc", {}, "Подключите узел или клиент Disk Direct (Desktop, NAS, Raspberry Pi), используя 10-значный Link ID. Папка по умолчанию: Data на этом устройстве.")}
                      </p>
                      {#if selectedRemoteDevice}
                        <div class="option-path-preview">
                          <code>/{selectedRemoteDevice.name || selectedRemoteDevice.linkId}{getRemoteSubfolder(selectedRemoteDevice)}</code>
                        </div>
                      {/if}

                      <!-- Embedded Connect Device Widget -->
                      <div class="remote-connect-widget" on:click|stopPropagation>
                        {#if remoteDevices.length > 0}
                          <div class="device-chips-row">
                            {#each remoteDevices as dev}
                              <button
                                type="button"
                                class="device-chip {selectedRemoteDevice && selectedRemoteDevice.linkId === dev.linkId ? 'active' : ''}"
                                on:click={() => {
                                  selectedRemoteDevice = dev;
                                  storageChoice = 'remote_device';
                                }}
                              >
                                <span class="device-chip-dot"></span>
                                <span class="device-chip-name">{dev.name || dev.linkId}</span>
                                <span class="device-chip-path">{getRemoteSubfolder(dev)}</span>
                              </button>
                            {/each}
                          </div>
                        {/if}

                        <div class="widget-form-row">
                          <input
                            type="text"
                            bind:value={remoteLinkId}
                            placeholder={$_("onboarding_remote_input_placeholder", {}, "Введите Link ID устройства")}
                            class="widget-link-input"
                            maxlength="64"
                            on:keydown={(e) => { if (e.key === 'Enter') handleConnectRemote(); }}
                          />
                          <div class="widget-actions-wrap">
                            <button
                              type="button"
                              class="widget-btn-connect"
                              disabled={!remoteLinkId.trim()}
                              on:click={() => handleConnectRemote()}
                            >
                              {$_("onboarding_btn_connect", {}, "Подключить")}
                            </button>
                            <button
                              type="button"
                              class="widget-btn-rescan"
                              disabled={scanningNearby}
                              title={$_("onboarding_rescan_title", {}, "Повторить поиск в сети")}
                              on:click={refreshNearby}
                            >
                              <svg class:spin={scanningNearby} viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2.2">
                                <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
                              </svg>
                              <span>{scanningNearby ? '...' : $_("onboarding_rescan_label", {}, "Поиск")}</span>
                            </button>
                          </div>
                        </div>

                        {#if detectedNearby.length > 0}
                          <div class="widget-detected-section">
                            <div class="widget-detected-header">
                              <span>{$_("onboarding_detected_lan", { count: detectedNearby.length }, `Найдено в локальной сети (${detectedNearby.length})`)}</span>
                            </div>
                            <div class="widget-detected-list">
                              {#each detectedNearby as dev}
                                <div class="widget-detected-item">
                                  <div class="widget-detected-info">
                                    <span class="detected-name">{dev.name || dev.linkId}</span>
                                    <span class="detected-id">Link ID: <code>{dev.linkId}</code></span>
                                  </div>
                                  <button
                                    type="button"
                                    class="widget-detected-btn"
                                    on:click={() => handleConnectRemote(dev.linkId)}
                                  >
                                    {$_("onboarding_btn_connect", {}, "Подключить")}
                                  </button>
                                </div>
                              {/each}
                            </div>
                          </div>
                        {/if}
                      </div>
                    </div>
                  {/if}
                </div>
              </label>
            </div>

            {#if !isMobile}
              <!-- Advanced Client Promo Box -->
              <div class="promo-box client-promo">
                <div class="promo-icon">⚡</div>
                <div class="promo-content">
                  <div class="promo-title">{$_("onboarding_promo_advanced_title", {}, "Продвинутый клиент Disk Direct для ОС")}</div>
                  <div class="promo-text">
                    {$_("onboarding_promo_advanced_text", {}, "Хотите фоновую синхронизацию фотографий с мобильного телефона, бэкап важных документов со всех ваших устройств, базу знаний с поиском по документам и фотографиям?")}
                  </div>
                </div>
                <a
                  href="https://docs.disk.direct/advanced/"
                  target="_blank"
                  rel="noopener noreferrer"
                  class="promo-action-btn"
                >
                  <span>{$_("onboarding_promo_more", {}, "Подробнее")}</span>
                  <span class="arrow-icon">↗</span>
                </a>
              </div>
            {/if}

            <div class="modal-footer">
              <div class="user-info-hint">{$_("onboarding_logged_in_as", {}, "Вход выполнен как:")} <strong>@{userLogin}</strong></div>
              <button class="btn-primary" on:click={goToStep3}>
                <span>{$_("onboarding_btn_continue", {}, "Продолжить")}</span>
                <span class="btn-arrow">→</span>
              </button>
            </div>
          </div>
        {/if}

        <!-- STEP 3: AI Node Selection -->
        {#if currentStep === 3}
          <div class="step-content">
            <div class="step-header">
              <div class="header-icon-box ai-icon-box">🧠</div>
              <h2>{$_("onboarding_ai_title", {}, "Выбор AI-ноды")}</h2>
              <p class="subtitle">
                {$_("onboarding_ai_subtitle", {}, "AI-нода предоставляет вычислительный ресурс для ИИ-ассистента, RAG-поиска по вашей базе знаний и генерации контента.")}
              </p>
            </div>

            <div class="options-grid">
              <!-- Option A: Auto (Recommended) -->
              <label class="option-card {selectedAiChoice === AI_NODE_MODE_AUTO ? 'selected' : 'collapsed'}">
                <input
                  type="radio"
                  name="ainode"
                  value={AI_NODE_MODE_AUTO}
                  checked={selectedAiChoice === AI_NODE_MODE_AUTO}
                  on:change={() => { selectedAiChoice = AI_NODE_MODE_AUTO; }}
                />
                <div class="option-body">
                  <div class="option-top">
                    <span class="option-title">{$_("onboarding_ai_auto_title", {}, "Автоматически (Auto)")}</span>
                    {#if !didHasConfiguredAiNode || selectedAiChoice === AI_NODE_MODE_AUTO}
                      <span class="badge-rec">{$_("onboarding_recommended", {}, "Рекомендуется")}</span>
                    {/if}
                  </div>
                  {#if selectedAiChoice === AI_NODE_MODE_AUTO}
                    <div class="option-expand-content">
                      <p class="option-desc">
                        {$_("onboarding_ai_auto_desc", {}, "Использует вашу персональную AI-ноду, если она подключена и онлайн. Если своей ноды нет — автоматически переключается на защищённую публичную ноду AI Assistant.")}
                      </p>
                    </div>
                  {/if}
                </div>
              </label>

              <!-- Option B: Own Embedded Device (dropdown for multiple) -->
              {#if embeddedDevices.length > 0}
                <label class="option-card {isEmbeddedDeviceSelected ? 'selected' : 'collapsed'}">
                  <input
                    type="radio"
                    name="ainode"
                    value={selectedEmbeddedLinkId}
                    checked={isEmbeddedDeviceSelected}
                    on:change={handleSelectEmbeddedOption}
                  />
                  <div class="option-body">
                    <div class="option-top">
                      <span class="option-title">{$_("onboarding_ai_own_device", {}, "Моё устройство")}</span>
                      {#if didHasConfiguredAiNode && isEmbeddedDeviceSelected}
                        <span class="badge-rec">{$_("onboarding_recommended", {}, "Рекомендуется")}</span>
                      {/if}
                      {#if selectedEmbeddedDeviceName}
                        <span class="device-chip chip-right">
                          <span class="device-chip-dot"></span>
                          <span class="device-chip-name">{selectedEmbeddedDeviceName}</span>
                        </span>
                      {/if}
                    </div>
                    {#if isEmbeddedDeviceSelected}
                      <div class="option-expand-content">
                        <p class="option-desc">
                          {$_("onboarding_ai_own_desc", {}, "Прямое подключение к вашему собственному серверу/ПК без посредников.")}
                        </p>
                        <div class="embedded-device-select-row" on:click|stopPropagation>
                          <label class="select-label" for="embedded-node-select">{$_("onboarding_device_label", {}, "Устройство:")}</label>
                          <select
                            id="embedded-node-select"
                            class="select-styled embedded-device-select"
                            value={selectedEmbeddedLinkId}
                            on:change={handleEmbeddedDeviceChange}
                          >
                            {#each embeddedDevices as dev}
                              <option value={dev.linkId}>
                                {dev.name || dev.linkId}
                              </option>
                            {/each}
                          </select>
                        </div>
                      </div>
                    {/if}
                  </div>
                </label>
              {/if}

              <!-- Option C: Public Node -->
              <label class="option-card {selectedAiChoice === AI_NODE_MODE_PUBLIC ? 'selected' : 'collapsed'}">
                <input
                  type="radio"
                  name="ainode"
                  value={AI_NODE_MODE_PUBLIC}
                  checked={selectedAiChoice === AI_NODE_MODE_PUBLIC}
                  on:change={() => { selectedAiChoice = AI_NODE_MODE_PUBLIC; }}
                />
                <div class="option-body">
                  <div class="option-top">
                    <span class="option-title">{$_("onboarding_ai_public_title", {}, "Публичная облачная AI-нода AI Assistant")}</span>
                    {#if didHasConfiguredAiNode && selectedAiChoice === AI_NODE_MODE_PUBLIC}
                      <span class="badge-rec">{$_("onboarding_recommended", {}, "Рекомендуется")}</span>
                    {/if}
                  </div>
                  {#if selectedAiChoice === AI_NODE_MODE_PUBLIC}
                    <div class="option-expand-content">
                      <p class="option-desc">
                        {$_("onboarding_ai_public_desc", {}, "Готовый к работе облачный сервис для знакомства с возможностями ИИ-ассистента, RAG и генерацией.")}
                      </p>
                    </div>
                  {/if}
                </div>
              </label>

              <!-- Option D: Custom Node -->
              <label class="option-card {selectedAiChoice === 'custom' ? 'selected' : 'collapsed'}">
                <input
                  type="radio"
                  name="ainode"
                  value="custom"
                  checked={selectedAiChoice === 'custom'}
                  on:change={() => { selectedAiChoice = 'custom'; }}
                />
                <div class="option-body">
                  <div class="option-top">
                    <span class="option-title">{$_("onboarding_ai_custom_title", {}, "Пользовательская нода по LinkID")}</span>
                    {#if didHasConfiguredAiNode && selectedAiChoice === 'custom'}
                      <span class="badge-rec">{$_("onboarding_recommended", {}, "Рекомендуется")}</span>
                    {/if}
                    {#if selectedAiChoice !== 'custom' && customNodeLinkId.trim()}
                      <span class="badge-chosen">{customNodeLinkId.trim().slice(0, 10)}...</span>
                    {/if}
                  </div>
                  {#if selectedAiChoice === 'custom'}
                    <div class="option-expand-content">
                      <p class="option-desc">
                        {$_("onboarding_ai_custom_desc", {}, "Подключите ноду друга или корпоративный сервер, указав 10-значный LinkID.")}
                      </p>
                      <div class="widget-form-row custom-node-form-row" on:click|stopPropagation>
                        <input
                          type="text"
                          class="widget-link-input"
                          placeholder={$_("onboarding_ai_custom_placeholder", {}, "Введите LinkID (например, c814de38f3...)")}
                          bind:value={customNodeLinkId}
                          on:keydown={(e) => { if (e.key === 'Enter') handleConnectCustomNode(); }}
                        />
                        <button
                          type="button"
                          class="widget-btn-connect"
                          disabled={!customNodeLinkId.trim() || connectingCustomNode}
                          on:click={handleConnectCustomNode}
                        >
                          {#if connectingCustomNode}
                            {$_("onboarding_btn_connecting", {}, "Подключение...")}
                          {:else}
                            {$_("onboarding_btn_connect", {}, "Подключить")}
                          {/if}
                        </button>
                      </div>
                      {#if customNodeSuccess}
                        <div class="custom-node-feedback success">
                          {$_("onboarding_ai_connected", { name: customNodeSuccess }, `✓ Подключено: ${customNodeSuccess}`)}
                        </div>
                      {:else if customNodeError}
                        <div class="custom-node-feedback error">
                          {customNodeError}
                        </div>
                      {/if}
                    </div>
                  {/if}
                </div>
              </label>
            </div>

            <!-- Terms acceptance note -->
            <label class="terms-accept-box">
              <input type="checkbox" bind:checked={termsAccepted} />
              <span>
                {$_("onboarding_terms_agree", {}, "Я принимаю")} <a href={Capacitor.isNativePlatform() ? "https://aiassistant.direct/tos#ai" : "/tos#ai"} target="_blank" rel="noopener noreferrer">{$_("onboarding_terms_tos", {}, "условия ответственного использования ИИ")}</a> {$_("onboarding_terms_privacy", {}, "и политику конфиденциальности Disk Direct.")}
              </span>
            </label>

            <!-- Own AI Node Promo Box -->
            <div class="promo-box ainode-promo">
              <div class="promo-icon">🛡️</div>
              <div class="promo-content">
                <div class="promo-title">{$_("onboarding_promo_ainode_title", {}, "Запустите свою AI-ноду (Home / Office)")}</div>
                <div class="promo-text">
                  {$_("onboarding_promo_ainode_text", {}, "100% приватность: все документы и промпты обрабатываются локально на вашей видеокарте без лимитов токенов и сторонних облаков.")}
                </div>
              </div>
              <a
                href="https://docs.disk.direct/ainode/"
                target="_blank"
                rel="noopener noreferrer"
                class="promo-action-btn"
              >
                <span>{$_("onboarding_promo_guide", {}, "Инструкция")}</span>
                <span class="arrow-icon">↗</span>
              </a>
            </div>

            <div class="modal-footer">
              <button class="btn-secondary" on:click={() => { currentStep = 2; }}>
                <span>{$_("onboarding_btn_back", {}, "Назад")}</span>
              </button>
              <button class="btn-primary" disabled={savingNode} on:click={finishConfiguration}>
                <span>{savingNode ? $_("onboarding_btn_finishing", {}, "Настройка...") : $_("onboarding_btn_finish", {}, "Завершить настройку")}</span>
                <span class="btn-arrow">→</span>
              </button>
            </div>
          </div>
        {/if}

        <!-- STEP 4: Congrats & Special Effects -->
        {#if currentStep === 4}
          <div class="step-content congrats-step">
            <canvas bind:this={canvasEl} class="confetti-canvas"></canvas>

            <div class="congrats-hero">
              <div class="celebration-badge-wrap">
                <div class="glow-pulse"></div>
                <div class="celebration-badge">🎉</div>
              </div>
              <h2>{$_("onboarding_congrats_title", {}, "Поздравляем! Всё готово к работе")}</h2>
              <p class="subtitle">
                {$_("onboarding_congrats_subtitle", {}, "Ваше персональное децентрализованное рабочее пространство успешно настроено.")}
              </p>
            </div>

            <div class="summary-grid">
              <div class="summary-card">
                <div class="summary-icon">📁</div>
                <div class="summary-info">
                  <div class="summary-label">{$_("onboarding_summary_folder", {}, "Папка для сохранения")}</div>
                  <div class="summary-val">
                    {#if storageChoice === 'mobile_native'}
                      {$_("onboarding_summary_folder_phone", {}, "Хранилище телефона (/Documents/Data)")}
                    {:else if storageChoice === 'local_folder' && localFolderName}
                      /{localFolderName}/Data
                    {:else if storageChoice === 'remote_device' && selectedRemoteDevice}
                      {selectedRemoteDevice.name || selectedRemoteDevice.linkId} ({getRemoteSubfolder(selectedRemoteDevice)})
                    {:else}
                      OPFS (/Private/Data)
                    {/if}
                  </div>
                </div>
              </div>

              <div class="summary-card">
                <div class="summary-icon">🧠</div>
                <div class="summary-info">
                  <div class="summary-label">{$_("onboarding_summary_ainode", {}, "AI-нода")}</div>
                  <div class="summary-val">
                    {#if selectedAiChoice === AI_NODE_MODE_AUTO}
                      {$_("onboarding_summary_ainode_auto", {}, "Автоматически (Auto)")}
                    {:else if selectedAiChoice === AI_NODE_MODE_PUBLIC}
                      {$_("onboarding_summary_ainode_public", {}, "Публичная облачная нода AI Assistant")}
                    {:else if isEmbeddedDeviceSelected}
                      {selectedEmbeddedDeviceName || $_("onboarding_ai_own_device", {}, "Моё устройство")}
                    {:else if selectedAiChoice === 'custom'}
                      {customNodeLinkId.trim() ? `Нода ${customNodeLinkId.trim().slice(0, 10)}...` : $_("onboarding_summary_ainode_custom", {}, "Собственный узел")}
                    {:else}
                      {$_("onboarding_summary_ainode_custom", {}, "Собственный узел")}
                    {/if}
                  </div>
                </div>
              </div>

              <div class="summary-card">
                <div class="summary-icon">
                  {#if assistantSummary.avatarUrl}
                    <img src={assistantSummary.avatarUrl} alt={assistantSummary.name || 'Assistant'} class="summary-avatar-img" />
                  {:else}
                    <img src="/img/june.png" alt={assistantSummary.name || 'Assistant'} class="summary-avatar-img" />
                  {/if}
                </div>
                <div class="summary-info">
                  <div class="summary-label">{$_("onboarding_summary_assistant", {}, "ИИ-ассистент")}</div>
                  <div class="summary-val">{assistantSummary.name || 'June'} ({assistantSummary.title || 'Junior Assistant'})</div>
                </div>
              </div>

              <div class="summary-card">
                <div class="summary-icon">🔐</div>
                <div class="summary-info">
                  <div class="summary-label">{$_("onboarding_summary_privacy", {}, "Приватность")}</div>
                  <div class="summary-val">{$_("onboarding_summary_privacy_val", {}, "E2EE + WebRTC P2P")}</div>
                </div>
              </div>
            </div>

            <div class="congrats-actions">
              <button class="btn-primary btn-large btn-chat-hero" on:click={handleStartChat}>
                <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.2">
                  <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>
                </svg>
                <span>{$_("onboarding_btn_start_chat", {}, "Начать общение в AI Assistant")}</span>
              </button>

              <button class="btn-secondary btn-large" on:click={handleStartFiles}>
                <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
                  <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>
                </svg>
                <span>{$_("onboarding_btn_start_files", {}, "Открыть Disk Direct с файлами")}</span>
              </button>
            </div>
          </div>
        {/if}
      </div>
    </div>
  </div>
{/if}

<style>
  .onboard-overlay {
    position: fixed;
    inset: 0;
    z-index: 99999;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px;
    background: rgba(13, 11, 18, 0.76);
    backdrop-filter: blur(10px);
    -webkit-backdrop-filter: blur(10px);
    overflow-y: auto;
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  }

  .onboard-modal-wrap {
    width: 100%;
    max-width: 680px;
    margin: auto;
  }

  .onboard-card {
    background: #ffffff;
    border-radius: 20px;
    border: 1px solid #DCDAE4;
    box-shadow: 0 24px 64px rgba(0, 0, 0, 0.22);
    overflow: hidden;
    position: relative;
    display: flex;
    flex-direction: column;
    animation: onboardCardEnter 0.25s cubic-bezier(0.16, 1, 0.3, 1);
  }

  .onboard-overlay.dark .onboard-card {
    background: var(--bg-card, #252330);
    border-color: var(--border-color, #3E3A4D);
    box-shadow: 0 24px 64px rgba(0, 0, 0, 0.55);
    color: #ECE7F6;
  }

  @keyframes onboardCardEnter {
    from {
      opacity: 0;
      transform: scale(0.96) translateY(12px);
    }
    to {
      opacity: 1;
      transform: scale(1) translateY(0);
    }
  }

  /* Stepper */
  .onboard-stepper {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 18px 24px;
    background: #F8F7FC;
    border-bottom: 1px solid #ECE7F6;
    gap: 16px;
  }

  .stepper-track {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: space-between;
  }

  /* Language Selector in Stepper */
  .onboard-lang-select-wrap {
    position: relative;
    display: inline-flex;
    align-items: center;
    cursor: pointer;
    flex-shrink: 0;
  }

  .onboard-lang-display {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 10px;
    border-radius: 8px;
    font-size: 12px;
    font-weight: 700;
    pointer-events: none;
    background-color: #ffffff;
    color: #5B5270;
    border: 1px solid #DCDAE4;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.05);
    transition: all 0.2s ease;
  }

  .onboard-overlay.dark .onboard-lang-display {
    background-color: var(--bg-card-hover, #312E3E);
    color: #ECE7F6;
    border: 1px solid rgba(139, 128, 163, 0.28);
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.2);
  }

  .onboard-lang-select-wrap:hover .onboard-lang-display,
  .onboard-lang-select-wrap:focus-within .onboard-lang-display {
    border-color: var(--accent, #563F7A);
  }

  .onboard-overlay.dark .onboard-lang-select-wrap:hover .onboard-lang-display,
  .onboard-overlay.dark .onboard-lang-select-wrap:focus-within .onboard-lang-display {
    border-color: var(--accent-hover, #A28AD0);
    background-color: var(--bg-card-hover, #282138);
  }

  .onboard-lang-code {
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.5px;
    line-height: 1;
  }

  .onboard-lang-globe,
  .onboard-lang-arrow {
    opacity: 0.7;
    flex-shrink: 0;
  }

  .onboard-lang-select-hidden {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    opacity: 0;
    cursor: pointer;
    z-index: 2;
  }

  .onboard-overlay.dark .onboard-lang-select-hidden {
    color-scheme: dark;
  }

  .onboard-overlay.dark .onboard-lang-select-hidden option {
    background-color: #252330;
    color: #ECE7F6;
  }

  .onboard-overlay.dark .onboard-stepper {
    background: var(--bg-primary, #181720);
    border-bottom-color: var(--border-color, #3E3A4D);
  }

  .step-badge {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.82rem;
    font-weight: 500;
    color: #8B80A3;
    white-space: nowrap;
  }

  .step-badge .step-icon {
    width: 24px;
    height: 24px;
    border-radius: 50%;
    background: #ECE7F6;
    color: #5B5270;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 0.75rem;
    font-weight: 700;
  }

  .onboard-overlay.dark .step-badge .step-icon {
    background: var(--border-color, #3E3A4D);
    color: #8B80A3;
  }

  .step-badge.active {
    color: var(--accent, #563F7A);
    font-weight: 600;
  }

  .step-badge.active .step-icon {
    background: var(--accent, #563F7A);
    color: #ffffff;
    box-shadow: 0 0 12px var(--accent-glow, rgba(142, 116, 189, 0.40));
  }

  .step-badge.done {
    color: #10b981;
  }

  .step-badge.done .step-icon {
    background: #10b981;
    color: #ffffff;
  }

  .step-line {
    flex: 1;
    height: 2px;
    background: #ECE7F6;
    margin: 0 10px;
    transition: background 0.2s ease;
  }

  .onboard-overlay.dark .step-line {
    background: var(--border-color, #3E3A4D);
  }

  .step-line.active {
    background: var(--accent, #563F7A);
  }

  /* Step Content */
  .step-content {
    padding: 28px 32px 32px 32px;
    display: flex;
    flex-direction: column;
    gap: 20px;
  }

  .step-header {
    text-align: center;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 8px;
  }

  .header-icon-box {
    width: 52px;
    height: 52px;
    border-radius: 14px;
    background: var(--accent-soft, rgba(142, 116, 189, 0.14));
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 26px;
    margin-bottom: 4px;
  }

  .ai-icon-box {
    background: rgba(125, 96, 172, 0.14);
  }

  .step-header h2 {
    font-size: 1.45rem;
    font-weight: 700;
    margin: 0;
    color: #181326;
    letter-spacing: -0.02em;
  }

  .onboard-overlay.dark .step-header h2 {
    color: #F8F7FC;
  }

  .step-header .subtitle {
    font-size: 0.92rem;
    color: #5B5270;
    margin: 0;
    max-width: 520px;
    line-height: 1.5;
  }

  .onboard-overlay.dark .step-header .subtitle {
    color: #8B80A3;
  }

  /* Options Grid */
  .options-grid {
    display: flex;
    flex-direction: column;
    gap: 12px;
  }

  .option-card {
    display: flex;
    align-items: flex-start;
    gap: 14px;
    padding: 16px 18px;
    border-radius: 12px;
    border: 1.5px solid #ECE7F6;
    background: #ffffff;
    cursor: pointer;
    transition: all 0.18s ease;
    max-width: 100%;
    box-sizing: border-box;
    overflow: hidden;
  }

  .onboard-overlay.dark .option-card {
    background: var(--bg-card, #2A2736);
    border-color: var(--border-color, #3E3A4D);
  }

  .option-card:hover {
    border-color: var(--accent-hover, #A28AD0);
    background: var(--bg-hover, #F5F0FF);
  }

  .onboard-overlay.dark .option-card:hover {
    border-color: var(--accent, #563F7A);
    background: var(--bg-card-hover, #312E3E);
  }

  .option-card.selected {
    border-color: var(--accent, #563F7A);
    background: var(--bg-hover, #F5F0FF);
    box-shadow: 0 4px 14px var(--accent-soft, rgba(142, 116, 189, 0.12));
  }

  .onboard-overlay.dark .option-card.selected {
    border-color: var(--accent-hover, #A28AD0);
    background: var(--bg-soft, rgba(142, 116, 189, 0.16));
    box-shadow: 0 4px 14px var(--accent-soft, rgba(162, 138, 208, 0.18));
  }

  .option-card.collapsed {
    padding: 12px 18px;
    align-items: center;
    background: #fafafa;
  }

  .onboard-overlay.dark .option-card.collapsed {
    background: var(--bg-primary, #1F1D28);
    border-color: var(--border-subtle, #211C30);
  }

  .option-card.collapsed:hover {
    background: var(--bg-hover, #F5F0FF);
    border-color: var(--accent-border, #C4B6E0);
  }

  .onboard-overlay.dark .option-card.collapsed:hover {
    background: var(--bg-card-hover, #312E3E);
    border-color: var(--accent-hover, #7D60AC);
  }

  .option-card input[type="radio"] {
    margin-top: 4px;
    accent-color: var(--accent, #563F7A);
    cursor: pointer;
    width: 17px;
    height: 17px;
    flex-shrink: 0;
  }

  .option-card.collapsed input[type="radio"] {
    margin-top: 0;
  }

  .option-expand-content {
    display: flex;
    flex-direction: column;
    gap: 8px;
    margin-top: 6px;
    animation: expandFade 0.2s ease-out;
    min-width: 0;
    max-width: 100%;
  }

  @keyframes expandFade {
    from {
      opacity: 0;
      transform: translateY(-4px);
    }
    to {
      opacity: 1;
      transform: translateY(0);
    }
  }

  .option-body {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 6px;
    min-width: 0;
    max-width: 100%;
  }

  .option-top {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    flex-wrap: wrap;
  }

  .option-title {
    font-size: 0.98rem;
    font-weight: 600;
    color: #181326;
    min-width: 0;
    word-break: break-word;
  }

  .onboard-overlay.dark .option-title {
    color: #F3EFFA;
  }

  .option-desc {
    font-size: 0.84rem;
    color: #5B5270;
    margin: 0;
    line-height: 1.45;
  }

  .onboard-overlay.dark .option-desc {
    color: #8B80A3;
  }

  .badge-rec {
    font-size: 0.72rem;
    font-weight: 600;
    background: var(--accent-soft, #EFEBF7);
    color: var(--accent, #563F7A);
    padding: 3px 8px;
    border-radius: 999px;
  }

  .badge-chosen {
    font-size: 0.72rem;
    font-weight: 600;
    background: #dcfce7;
    color: #15803d;
    padding: 3px 8px;
    border-radius: 999px;
  }

  .badge-online {
    font-size: 0.72rem;
    font-weight: 600;
    background: #dcfce7;
    color: #15803d;
    padding: 3px 8px;
    border-radius: 999px;
  }

  .option-path-preview code {
    font-size: 0.76rem;
    background: #F3EFFA;
    color: #5B5270;
    padding: 2px 6px;
    border-radius: 4px;
  }

  .onboard-overlay.dark .option-path-preview code {
    background: var(--bg-primary, #181720);
    color: #DCDAE4;
  }

  .folder-pick-row {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-top: 6px;
  }

  .btn-pick-folder {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 14px;
    font-size: 0.84rem;
    font-weight: 600;
    border-radius: 8px;
    border: 1px solid #DCDAE4;
    background: #ffffff;
    color: #181326;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .onboard-overlay.dark .btn-pick-folder {
    background: var(--bg-card-hover, #312E3E);
    border-color: var(--border-color, #3E3A4D);
    color: var(--text-primary, #F8F7FC);
  }

  .btn-pick-folder:hover:not(:disabled) {
    background: #F8F7FC;
    border-color: #8B80A3;
  }

  .btn-pick-folder:disabled {
    opacity: 0.55;
    cursor: not-allowed;
  }

  .note-fs {
    font-size: 0.75rem;
    color: #8B80A3;
  }

  /* Remote Device Widget */
  .remote-connect-widget {
    margin-top: 10px;
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 12px;
    background: #F8F7FC;
    border: 1px solid #ECE7F6;
    border-radius: 10px;
    min-width: 0;
    max-width: 100%;
    box-sizing: border-box;
  }

  .onboard-overlay.dark .remote-connect-widget {
    background: var(--bg-primary, #1F1D28);
    border-color: var(--border-color, #3E3A4D);
  }

  .widget-sub-label {
    font-size: 0.76rem;
    font-weight: 600;
    color: #5B5270;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    margin-bottom: 4px;
    display: block;
  }

  .onboard-overlay.dark .widget-sub-label {
    color: #8B80A3;
  }

  .device-chips-row {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    max-width: 100%;
  }

  .device-chip {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 10px;
    border-radius: 8px;
    border: 1px solid #DCDAE4;
    background: #ffffff;
    font-size: 0.82rem;
    color: #5B5270;
    cursor: pointer;
    transition: all 0.15s ease;
    max-width: 100%;
    box-sizing: border-box;
    min-width: 0;
  }

  .device-chip-name {
    max-width: 140px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    min-width: 0;
  }

  .onboard-overlay.dark .device-chip {
    background: var(--bg-card-hover, #312E3E);
    border-color: var(--border-color, #3E3A4D);
    color: var(--text-primary, #F8F7FC);
  }

  .device-chip.active {
    border-color: var(--accent, #563F7A);
    background: var(--accent-soft, #EFEBF7);
    color: var(--accent-hover, #7D60AC);
    font-weight: 600;
  }

  .onboard-overlay.dark .device-chip.active {
    background: var(--accent-soft, rgba(142, 116, 189, 0.24));
    border-color: var(--accent-hover, #A28AD0);
    color: var(--bg-hover, #F5F0FF);
  }

  .device-chip.chip-right {
    margin-left: auto;
    cursor: default;
    padding: 3px 9px;
    font-size: 0.78rem;
    font-weight: 500;
  }

  .device-chip-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: #10b981;
    flex-shrink: 0;
  }

  .device-chip-path {
    font-size: 0.72rem;
    opacity: 0.7;
    font-family: monospace;
    white-space: nowrap;
    flex-shrink: 0;
  }

  .widget-form-row {
    display: flex;
    gap: 8px;
    align-items: center;
    width: 100%;
    box-sizing: border-box;
  }

  .widget-actions-wrap {
    display: flex;
    gap: 8px;
    align-items: center;
    flex-shrink: 0;
  }

  .widget-link-input {
    flex: 1;
    min-width: 0;
    padding: 8px 12px;
    font-size: 0.88rem;
    font-family: monospace;
    border-radius: 8px;
    border: 1px solid #DCDAE4;
    background: #ffffff;
    color: #181326;
    outline: none;
    transition: border-color 0.15s ease;
    box-sizing: border-box;
  }

  .widget-link-input:focus {
    border-color: var(--accent, #563F7A);
  }

  .onboard-overlay.dark .widget-link-input {
    background: var(--bg-primary, #181720);
    border-color: var(--border-color, #3E3A4D);
    color: var(--text-primary, #F8F7FC);
  }

  .widget-btn-connect {
    padding: 8px 16px;
    font-size: 0.86rem;
    font-weight: 600;
    border-radius: 8px;
    border: none;
    background: var(--accent, #563F7A);
    color: #ffffff;
    cursor: pointer;
    transition: background 0.15s ease;
    white-space: nowrap;
    display: inline-flex;
    align-items: center;
    justify-content: center;
  }

  .widget-btn-connect:hover:not(:disabled) {
    background: var(--accent-hover, #7D60AC);
  }

  .widget-btn-connect:disabled {
    opacity: 0.45;
    cursor: not-allowed;
  }

  .widget-detected-section {
    background: #ffffff;
    border: 1px solid #ECE7F6;
    border-radius: 8px;
    padding: 8px 10px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .onboard-overlay.dark .widget-detected-section {
    background: var(--bg-card, #252330);
    border-color: var(--border-color, #3E3A4D);
  }

  .widget-detected-header {
    font-size: 0.75rem;
    font-weight: 600;
    color: #5B5270;
  }

  .onboard-overlay.dark .widget-detected-header {
    color: #8B80A3;
  }

  .widget-detected-list {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .widget-detected-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 6px 8px;
    background: #F8F7FC;
    border-radius: 6px;
  }

  .onboard-overlay.dark .widget-detected-item {
    background: var(--bg-primary, #1F1D28);
  }

  .widget-detected-info {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .detected-name {
    font-size: 0.82rem;
    font-weight: 600;
    color: #181326;
  }

  .onboard-overlay.dark .detected-name {
    color: #F3EFFA;
  }

  .detected-id {
    font-size: 0.72rem;
    color: #5B5270;
  }

  .detected-id code {
    background: rgba(0, 0, 0, 0.05);
    padding: 1px 4px;
    border-radius: 3px;
  }

  .onboard-overlay.dark .detected-id code {
    background: rgba(255, 255, 255, 0.08);
    color: #DCDAE4;
  }

  .widget-detected-btn {
    padding: 4px 10px;
    font-size: 0.78rem;
    font-weight: 600;
    border-radius: 6px;
    border: 1px solid var(--accent, #563F7A);
    background: transparent;
    color: var(--accent, #563F7A);
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .widget-detected-btn:hover {
    background: var(--accent, #563F7A);
    color: #ffffff;
  }

  .widget-btn-rescan {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    padding: 8px 12px;
    font-size: 0.84rem;
    font-weight: 600;
    border-radius: 8px;
    border: 1px solid #DCDAE4;
    background: #ffffff;
    color: #5B5270;
    cursor: pointer;
    transition: all 0.15s ease;
    white-space: nowrap;
  }

  .onboard-overlay.dark .widget-btn-rescan {
    background: var(--bg-card-hover, #312E3E);
    border-color: var(--border-color, #3E3A4D);
    color: var(--text-primary, #F8F7FC);
  }

  .widget-btn-rescan:hover:not(:disabled) {
    background: #F3EFFA;
    border-color: #8B80A3;
  }

  .onboard-overlay.dark .widget-btn-rescan:hover:not(:disabled) {
    background: var(--bg-card-hover, #282138);
  }

  .widget-btn-rescan:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .custom-linkid-wrap {
    margin-top: 8px;
  }

  .custom-linkid-input {
    width: 100%;
    padding: 8px 12px;
    font-size: 0.88rem;
    font-family: monospace;
    border-radius: 8px;
    border: 1px solid #DCDAE4;
    background: #ffffff;
    color: #181326;
    box-sizing: border-box;
  }

  .onboard-overlay.dark .custom-linkid-input {
    background: var(--bg-primary, #181720);
    border-color: var(--border-color, #3E3A4D);
    color: #F3EFFA;
  }

  .embedded-device-select-row {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-top: 4px;
  }

  .select-label {
    font-size: 0.82rem;
    font-weight: 500;
    color: #5B5270;
    white-space: nowrap;
  }

  .onboard-overlay.dark .select-label {
    color: #8B80A3;
  }

  .embedded-device-select,
  .select-styled {
    flex: 1;
    padding: 7px 36px 7px 12px;
    font-size: 0.86rem;
    font-weight: 500;
    border-radius: 8px;
    border: 1px solid #DCDAE4;
    background-color: #ffffff;
    color: #181326;
    outline: none;
    cursor: pointer;
    appearance: none;
    -webkit-appearance: none;
    -moz-appearance: none;
    background-repeat: no-repeat !important;
    background-position: calc(100% - 12px) center !important;
    background-size: 14px 14px !important;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%235B5270' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E") !important;
    transition: border-color 0.15s ease, box-shadow 0.15s ease;
  }

  .embedded-device-select:focus,
  .select-styled:focus {
    border-color: var(--accent, #563F7A);
    box-shadow: 0 0 0 2px var(--accent-soft, rgba(142, 116, 189, 0.16));
  }

  .onboard-overlay.dark .embedded-device-select,
  .onboard-overlay.dark .select-styled {
    background-color: var(--bg-primary, #181720);
    border-color: var(--border-color, #3E3A4D);
    color: #F3EFFA;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%238B80A3' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E") !important;
  }

  .custom-node-form-row {
    margin-top: 6px;
  }

  .custom-node-feedback {
    font-size: 0.8rem;
    font-weight: 500;
    margin-top: 6px;
  }

  .custom-node-feedback.success {
    color: #16a34a;
  }

  .custom-node-feedback.error {
    color: #dc2626;
  }

  .onboard-overlay.dark .custom-node-feedback.success {
    color: #4ade80;
  }

  .onboard-overlay.dark .custom-node-feedback.error {
    color: #f87171;
  }

  .terms-accept-box {
    display: flex;
    align-items: center;
    gap: 10px;
    font-size: 0.82rem;
    color: #5B5270;
    cursor: pointer;
    margin-top: 4px;
  }

  .onboard-overlay.dark .terms-accept-box {
    color: #8B80A3;
  }

  .terms-accept-box a {
    color: var(--accent, #563F7A);
    text-decoration: underline;
  }

  /* Promo Cards */
  .promo-box {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 14px 18px;
    border-radius: 12px;
    border: 1px solid;
    position: relative;
    overflow: hidden;
  }

  .client-promo {
    background: linear-gradient(135deg, var(--bg-soft, rgba(142, 116, 189, 0.10)) 0%, rgba(162, 138, 208, 0.03) 100%);
    border-color: var(--accent-border, rgba(142, 116, 189, 0.28));
  }

  .ainode-promo {
    background: linear-gradient(135deg, rgba(125, 96, 172, 0.09) 0%, rgba(162, 138, 208, 0.03) 100%);
    border-color: rgba(125, 96, 172, 0.28);
  }

  .promo-icon {
    font-size: 24px;
    flex-shrink: 0;
  }

  .promo-content {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .promo-title {
    font-size: 0.88rem;
    font-weight: 600;
    color: #181326;
  }

  .onboard-overlay.dark .promo-title {
    color: #F3EFFA;
  }

  .promo-text {
    font-size: 0.78rem;
    color: #5B5270;
    line-height: 1.4;
  }

  .onboard-overlay.dark .promo-text {
    color: #8B80A3;
  }

  .promo-action-btn {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 6px 12px;
    font-size: 0.8rem;
    font-weight: 600;
    border-radius: 8px;
    background: var(--accent, #563F7A);
    color: #ffffff;
    text-decoration: none;
    flex-shrink: 0;
    transition: all 0.15s ease;
  }

  .promo-action-btn:hover {
    background: var(--accent-hover, #7D60AC);
    transform: translateY(-1px);
  }

  /* Modal Footer */
  .modal-footer {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-top: 8px;
    padding-top: 18px;
    border-top: 1px solid #ECE7F6;
  }

  .onboard-overlay.dark .modal-footer {
    border-top-color: var(--border-color, #3E3A4D);
  }

  .user-info-hint {
    font-size: 0.82rem;
    color: #5B5270;
  }

  .onboard-overlay.dark .user-info-hint {
    color: #8B80A3;
  }

  .btn-primary {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 10px 22px;
    font-size: 0.92rem;
    font-weight: 600;
    border-radius: 10px;
    background: var(--accent, #563F7A);
    color: #ffffff;
    border: none;
    cursor: pointer;
    transition: all 0.15s ease;
    box-shadow: 0 4px 12px var(--accent-glow, rgba(142, 116, 189, 0.28));
  }

  .btn-primary:hover:not(:disabled) {
    background: var(--accent-hover, #7D60AC);
    box-shadow: 0 6px 16px var(--accent-glow, rgba(142, 116, 189, 0.36));
    transform: translateY(-1px);
  }

  .btn-secondary {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 10px 18px;
    font-size: 0.9rem;
    font-weight: 500;
    border-radius: 10px;
    background: #F3EFFA;
    color: #5B5270;
    border: 1px solid #DCDAE4;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .onboard-overlay.dark .btn-secondary {
    background: var(--bg-card-hover, #312E3E);
    border-color: var(--border-color, #3E3A4D);
    color: var(--text-secondary, #C4B8DB);
  }

  .btn-secondary:hover {
    background: #ECE7F6;
  }

  /* STEP 4: Congrats & Special Effects */
  .congrats-step {
    position: relative;
    text-align: center;
    overflow: hidden;
    padding-top: 40px;
  }

  .confetti-canvas {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    pointer-events: none;
    z-index: 10;
  }

  .congrats-hero {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 12px;
    z-index: 20;
    position: relative;
  }

  .celebration-badge-wrap {
    position: relative;
    width: 80px;
    height: 80px;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .celebration-badge {
    width: 72px;
    height: 72px;
    border-radius: 50%;
    background: var(--accent-gradient, linear-gradient(135deg, #8E74BD 0%, #A65581 100%));
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 34px;
    box-shadow: 0 8px 24px var(--accent-glow, rgba(142, 116, 189, 0.40));
    z-index: 2;
    animation: bounceBadge 0.6s cubic-bezier(0.175, 0.885, 0.32, 1.275);
  }

  .glow-pulse {
    position: absolute;
    inset: -6px;
    border-radius: 50%;
    background: radial-gradient(circle, var(--accent-glow, rgba(162, 138, 208, 0.50)) 0%, rgba(162, 138, 208, 0) 70%);
    animation: pulseAura 2s infinite ease-in-out;
  }

  @keyframes bounceBadge {
    0% { transform: scale(0.3) rotate(-20deg); opacity: 0; }
    70% { transform: scale(1.15) rotate(5deg); }
    100% { transform: scale(1) rotate(0deg); opacity: 1; }
  }

  @keyframes pulseAura {
    0%, 100% { transform: scale(0.95); opacity: 0.6; }
    50% { transform: scale(1.3); opacity: 0.2; }
  }

  .congrats-hero h2 {
    font-size: 1.6rem;
    font-weight: 800;
    margin: 0;
    color: #181326;
    letter-spacing: -0.02em;
  }

  .onboard-overlay.dark .congrats-hero h2 {
    color: #F8F7FC;
  }

  .congrats-hero .subtitle {
    font-size: 0.95rem;
    color: #5B5270;
    margin: 0;
    max-width: 480px;
  }

  .onboard-overlay.dark .congrats-hero .subtitle {
    color: #8B80A3;
  }

  /* Summary Grid */
  .summary-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 12px;
    margin: 10px 0;
    z-index: 20;
    position: relative;
    text-align: left;
  }

  .summary-card {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 12px 14px;
    border-radius: 12px;
    background: #F8F7FC;
    border: 1px solid #ECE7F6;
  }

  .onboard-overlay.dark .summary-card {
    background: var(--bg-primary, #1F1D28);
    border-color: var(--border-color, #3E3A4D);
  }

  .summary-icon {
    font-size: 20px;
    flex-shrink: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 28px;
  }

  .summary-avatar-img {
    width: 32px;
    height: 32px;
    border-radius: 50%;
    object-fit: cover;
    display: block;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.2);
  }

  .summary-icon .avatar-hero-spark {
    width: 30px;
    height: 30px;
    border-radius: 50%;
    background: var(--accent-gradient, linear-gradient(135deg, #7D60AC, #A65581));
    display: flex;
    align-items: center;
    justify-content: center;
    color: #fff;
    box-shadow: 0 1px 3px var(--accent-glow, rgba(142, 116, 189, 0.35));
  }

  .summary-info {
    display: flex;
    flex-direction: column;
    gap: 2px;
    overflow: hidden;
  }

  .summary-label {
    font-size: 0.74rem;
    color: #5B5270;
    font-weight: 500;
  }

  .onboard-overlay.dark .summary-label {
    color: #8B80A3;
  }

  .summary-val {
    font-size: 0.86rem;
    font-weight: 600;
    color: #181326;
    white-space: nowrap;
    overflow: hidden;
    text-transform: capitalize;
    text-overflow: ellipsis;
  }

  .onboard-overlay.dark .summary-val {
    color: #F3EFFA;
  }

  /* Action Buttons */
  .congrats-actions {
    display: flex;
    flex-direction: column;
    gap: 10px;
    z-index: 20;
    position: relative;
    margin-top: 6px;
  }

  .btn-large {
    padding: 14px 24px;
    font-size: 1rem;
    justify-content: center;
  }

  .btn-chat-hero {
    background: var(--accent-gradient, linear-gradient(135deg, #563F7A 0%, #8E74BD 100%));
    box-shadow: 0 6px 20px var(--accent-glow, rgba(142, 116, 189, 0.36));
  }

  .btn-chat-hero:hover {
    box-shadow: 0 8px 26px var(--accent-glow, rgba(142, 116, 189, 0.46));
    transform: translateY(-2px);
  }

  .error-msg {
    font-size: 0.8rem;
    color: #ef4444;
  }

  @media (max-width: 600px) {
    .onboard-stepper {
      padding: 16px;
    }
    .step-badge .step-label {
      display: none;
    }
    .step-content {
      padding: 20px 14px 24px 14px;
    }
    .option-card {
      padding: 12px 14px;
      gap: 10px;
    }
    .widget-form-row {
      flex-direction: column;
      align-items: stretch;
      gap: 8px;
    }
    .widget-link-input {
      width: 100%;
    }
    .widget-actions-wrap {
      width: 100%;
      display: flex;
      gap: 8px;
    }
    .widget-btn-connect {
      flex: 1;
      justify-content: center;
      text-align: center;
    }
    .widget-btn-rescan {
      flex: 0 0 auto;
    }
    .summary-grid {
      grid-template-columns: 1fr;
    }
    .promo-box {
      flex-direction: column;
      align-items: flex-start;
      gap: 10px;
    }
    .promo-action-btn {
      align-self: stretch;
      justify-content: center;
    }
  }
</style>
