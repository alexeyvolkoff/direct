import { writable } from "svelte/store";
import { NativeStorage, isNativeStorageAvailable } from "./native-drive.js";
import { WebRTCDataChannelDrive } from "./webrtc-drive.js";
import { deviceRegistry } from "./device-registry.js";

const DB_NAME = "omd_camera_sync";
const DB_VERSION = 2;
const STORE_NAME = "media_manifest";
const HANDLE_STORE = "pwa_sync_handle";
const CHUNK_SIZE = 16 * 1024; // 16KB per chunk

export function sanitizeTargetFolder(folder, userName = "") {
  if (!folder) return "/PhoneBackup";
  let f = String(folder).trim().replace(/\\/g, "/");
  // Strip user home path prefix like /home/alexey or /Users/alexey
  f = f.replace(/^\/(home|Users)\/[^\/]+/, "");
  if (userName) {
    const cleanUser = userName.replace(/^\/+|\/+$/g, "");
    if (f === `/${cleanUser}` || f === cleanUser) {
      f = "/PhoneBackup";
    } else if (f.startsWith(`/${cleanUser}/`)) {
      f = f.slice(cleanUser.length + 1);
    } else if (f.startsWith(`${cleanUser}/`)) {
      f = "/" + f.slice(cleanUser.length + 1);
    }
  }
  if (!f.startsWith("/")) f = "/" + f;
  return f || "/PhoneBackup";
}

export function isFileSystemAccessAvailable() {
  return typeof window !== "undefined" && !!window.showDirectoryPicker;
}

function openSyncDB() {
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === "undefined") {
      resolve(null);
      return;
    }
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        const store = db.createObjectStore(STORE_NAME, { keyPath: "id" });
        store.createIndex("status", "status", { unique: false });
        store.createIndex("date", "date", { unique: false });
      }
      if (!db.objectStoreNames.contains(HANDLE_STORE)) {
        db.createObjectStore(HANDLE_STORE, { keyPath: "id" });
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function getStoredItem(id) {
  const db = await openSyncDB();
  if (!db) return null;
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_NAME, "readonly");
    const req = tx.objectStore(STORE_NAME).get(id);
    req.onsuccess = () => resolve(req.result || null);
    req.onerror = () => reject(req.error);
  });
}

async function saveStoredItem(item) {
  const db = await openSyncDB();
  if (!db) return;
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_NAME, "readwrite");
    tx.objectStore(STORE_NAME).put(item);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

async function getAllStoredItems() {
  const db = await openSyncDB();
  if (!db) return [];
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_NAME, "readonly");
    const req = tx.objectStore(STORE_NAME).getAll();
    req.onsuccess = () => resolve(req.result || []);
    req.onerror = () => reject(req.error);
  });
}

async function getStoredPwaHandle() {
  const db = await openSyncDB();
  if (!db) return null;
  return new Promise((resolve) => {
    try {
      const tx = db.transaction(HANDLE_STORE, "readonly");
      const req = tx.objectStore(HANDLE_STORE).get("current_folder");
      req.onsuccess = () => resolve(req.result?.handle || null);
      req.onerror = () => resolve(null);
    } catch (_) {
      resolve(null);
    }
  });
}

async function saveStoredPwaHandle(handle) {
  const db = await openSyncDB();
  if (!db) return;
  return new Promise((resolve, reject) => {
    try {
      const tx = db.transaction(HANDLE_STORE, "readwrite");
      tx.objectStore(HANDLE_STORE).put({ id: "current_folder", handle });
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    } catch (e) {
      resolve();
    }
  });
}

async function scanDirectoryHandleRecursive(dirHandle, currentPath = "", result = []) {
  for await (const entry of dirHandle.values()) {
    const entryPath = currentPath ? `${currentPath}/${entry.name}` : entry.name;
    if (entry.kind === "file") {
      if (entry.name.startsWith(".") || entry.name.startsWith("~")) continue;
      try {
        const file = await entry.getFile();
        result.push({
          id: entryPath,
          name: entry.name,
          relativePath: entryPath,
          size: file.size,
          date: file.lastModified,
          type: file.type || "application/octet-stream",
          fileHandle: entry
        });
      } catch (_) {}
    } else if (entry.kind === "directory") {
      if (entry.name.startsWith(".") || entry.name === "node_modules" || entry.name === ".git") continue;
      await scanDirectoryHandleRecursive(entry, entryPath, result);
    }
  }
  return result;
}

async function readChunkForItem(item, offset, length) {
  try {
    if (item.fileHandle) {
      const file = await item.fileHandle.getFile();
      const slice = file.slice(offset, offset + length);
      const arrayBuffer = await slice.arrayBuffer();
      const bytes = new Uint8Array(arrayBuffer);
      let binary = "";
      const len = bytes.byteLength;
      for (let i = 0; i < len; i++) {
        binary += String.fromCharCode(bytes[i]);
      }
      const base64 = btoa(binary);
      return {
        data: base64,
        bytesRead: len,
        eof: offset + len >= file.size,
        totalSize: file.size
      };
    } else if (isNativeStorageAvailable()) {
      const targetPath = item.realPath || item.id;
      const res = await NativeStorage.readChunk({
        path: targetPath,
        offset,
        length
      });
      return res;
    }
  } catch (err) {
    console.error(`[CameraSync] Error reading chunk for "${item.name}" at offset ${offset}:`, err);
    throw err;
  }
  return { data: "", bytesRead: 0, eof: true };
}

// Initial Sync State
const savedSettings = typeof window !== "undefined" ? (() => {
  try {
    return JSON.parse(localStorage.getItem("omd_camera_sync_settings") || "{}");
  } catch (_) {
    return {};
  }
})() : {};

export const syncState = writable({
  isSyncing: false,
  isNative: isNativeStorageAvailable(),
  hasFSAccess: isFileSystemAccessAvailable(),
  localFolderName: savedSettings.localFolderName || "",
  totalMedia: 0,
  syncedMedia: 0,
  pendingMedia: 0,
  currentFile: "",
  progressPercent: 0,
  lastSyncTime: savedSettings.lastSyncTime || 0,
  autoSyncEnabled: !!savedSettings.autoSyncEnabled,
  targetLinkId: savedSettings.targetLinkId || "",
  targetFolder: savedSettings.targetFolder || "",
  timeRange: savedSettings.timeRange || "all",
  sources: savedSettings.sources || {
    photos: true,
    videos: true,
    screenshots: false,
    documents: false
  },
  statusMsg: ""
});

export function updateSettings(patch) {
  syncState.update((s) => {
    const next = { ...s, ...patch };
    try {
      if (typeof window !== "undefined") {
        localStorage.setItem(
          "omd_camera_sync_settings",
          JSON.stringify({
            autoSyncEnabled: next.autoSyncEnabled,
            targetLinkId: next.targetLinkId,
            targetFolder: next.targetFolder,
            localFolderName: next.localFolderName,
            timeRange: next.timeRange,
            sources: next.sources,
            lastSyncTime: next.lastSyncTime
          })
        );
      }
    } catch (_) {}
    if (patch.autoSyncEnabled !== undefined) {
      if (patch.autoSyncEnabled) cameraSyncManager?.startAutoSync();
      else cameraSyncManager?.stopAutoSync();
    }
    return next;
  });
}

export class CameraSyncManager {
  constructor() {
    this._abortController = null;
    this._isSyncRunning = false;
    this._currentPwaHandle = null;
    this._autoSyncTimer = null;
    this._mediaListenerHandle = null;
    setTimeout(() => this._initAutoSync(), 1000);
  }

  _initAutoSync() {
    if (typeof window === "undefined") return;
    let s = {};
    syncState.subscribe(val => s = val)();
    if (s.autoSyncEnabled) {
      this.startAutoSync();
    }
  }

  startAutoSync(intervalMs = 60000) {
    if (this._autoSyncTimer) return;
    console.log(`[CameraSync] 🔄 Background auto-sync started (interval: ${intervalMs / 1000}s)`);

    // Initial check shortly after startup
    setTimeout(() => {
      this._runAutoSyncCheck();
    }, 4000);

    this._autoSyncTimer = setInterval(() => {
      this._runAutoSyncCheck();
    }, intervalMs);

    // Instant trigger when new photos/videos are added on native mobile
    if (isNativeStorageAvailable() && !this._mediaListenerHandle) {
      try {
        NativeStorage.addListener("mediaChanged", () => {
          console.log("[CameraSync] 📸 New media detected on device -> triggering auto-sync");
          this._runAutoSyncCheck();
        }).then(h => {
          this._mediaListenerHandle = h;
        }).catch(() => {});
      } catch (_) {}
    }
  }

  stopAutoSync() {
    if (this._autoSyncTimer) {
      clearInterval(this._autoSyncTimer);
      this._autoSyncTimer = null;
      console.log("[CameraSync] ⏸️ Background auto-sync stopped");
    }
    if (this._mediaListenerHandle) {
      try {
        if (typeof this._mediaListenerHandle.remove === "function") {
          this._mediaListenerHandle.remove();
        }
      } catch (_) {}
      this._mediaListenerHandle = null;
    }
  }

  async _runAutoSyncCheck() {
    if (this._isSyncRunning) return;
    let s = {};
    syncState.subscribe(val => s = val)();
    if (!s.autoSyncEnabled) return;

    let targetLinkId = s.targetLinkId;
    if (!targetLinkId) {
      const devList = deviceRegistry.list();
      const onlineDev = devList.find(d => (d.isConnected || d.online) && !d.isLocal);
      const anyDev = onlineDev || devList.find(d => !d.isLocal) || devList[0];
      if (anyDev && anyDev.linkId) {
        targetLinkId = anyDev.linkId;
        updateSettings({ targetLinkId });
      }
    }
    if (!targetLinkId) {
      console.log("[CameraSync] Auto-sync pass skipped: no target device registered yet");
      return;
    }

    try {
      console.log(`[CameraSync] 🔍 Checking for new media in background (target: ${targetLinkId})...`);
      await this.startSync(targetLinkId, s.targetFolder);
    } catch (err) {
      console.warn("[CameraSync] Background auto-sync pass skipped:", err && err.message);
    }
  }

  /**
   * Select a local directory in Desktop PWA / Browser via File System Access API.
   */
  async selectPwaFolder() {
    if (!isFileSystemAccessAvailable()) {
      throw new Error("File System Access API not supported in this browser");
    }
    const handle = await window.showDirectoryPicker({ mode: "read" });
    const name = handle.name || "LocalFolder";
    this._currentPwaHandle = handle;
    await saveStoredPwaHandle(handle);
    updateSettings({ localFolderName: name });
    console.log(`[CameraSync] Selected local PWA folder: "${name}"`);
    await this.scanCameraRoll({ handle });
    return { name, handle };
  }

  /**
   * Scan camera roll or local PWA directory and update manifest statistics with filters.
   */
  async scanCameraRoll(options = {}) {
    console.log("[CameraSync] 🔍 Starting media scan with options:", options);
    try {
      let currentSettings = {};
      syncState.subscribe(s => currentSettings = s)();
      const timeRange = options.timeRange || currentSettings.timeRange || "all";
      const sources = options.sources || currentSettings.sources || { photos: true, videos: true, screenshots: false, documents: false };

      let cutoff = 0;
      const now = Date.now();
      if (timeRange === "3m") cutoff = now - 90 * 24 * 60 * 60 * 1000;
      else if (timeRange === "1y") cutoff = now - 365 * 24 * 60 * 60 * 1000;
      else if (timeRange === "5y") cutoff = now - 5 * 365 * 24 * 60 * 60 * 1000;

      let allItems = [];

      if (isNativeStorageAvailable()) {
        console.log("[CameraSync] Native platform detected: calling NativeStorage.listMedia()...");
        const res = await NativeStorage.listMedia({ limit: 5000, offset: 0 });
        allItems = (res && res.items) || [];
        console.log(`[CameraSync] 📱 NativeStorage.listMedia returned ${allItems.length} raw items`, allItems.slice(0, 5));
      } else if (isFileSystemAccessAvailable()) {
        const handle = options.handle || this._currentPwaHandle || (await getStoredPwaHandle());
        if (handle) {
          this._currentPwaHandle = handle;
          updateSettings({ localFolderName: handle.name || "LocalFolder" });
          console.log(`[CameraSync] Scanning PWA directory handle: "${handle.name}"...`);
          allItems = await scanDirectoryHandleRecursive(handle);
          console.log(`[CameraSync] 💻 PWA scan found ${allItems.length} local files`);
        }
      }

      // Apply source & time range filters
      const filteredItems = allItems.filter((item) => {
        if (cutoff > 0 && item.date && item.date < cutoff) return false;

        const isVideo = item.type && item.type.startsWith("video");
        const isPhoto = item.type && item.type.startsWith("image");
        const isScreenshot = /screenshot/i.test(item.name || "") || /screenshot/i.test(item.id || "");

        if (isScreenshot && !sources.screenshots) return false;
        if (isVideo && !sources.videos) return false;
        if (isPhoto && !isScreenshot && !sources.photos) return false;
        if (!isVideo && !isPhoto && !sources.documents) return false;

        return true;
      });

      console.log(`[CameraSync] 🎯 Filter applied (timeRange: ${timeRange}, sources: ${JSON.stringify(sources)}) => ${filteredItems.length} matching files`);

      const stored = await getAllStoredItems();
      const storedMap = new Map(stored.map((i) => [i.id, i]));

      let synced = 0;
      let pending = 0;

      for (const item of filteredItems) {
        const exist = storedMap.get(item.id);
        if (exist && exist.status === "synced" && exist.size === item.size) {
          synced++;
        } else {
          pending++;
          if (!exist) {
            await saveStoredItem({
              id: item.id,
              name: item.name,
              relativePath: item.relativePath || item.name,
              size: item.size,
              date: item.date,
              type: item.type,
              status: "pending"
            });
          }
        }
      }

      console.log(`[CameraSync] 📊 Manifest status: Total ${filteredItems.length} | Synced ${synced} | Pending ${pending}`);

      syncState.update((s) => ({
        ...s,
        totalMedia: filteredItems.length,
        syncedMedia: synced,
        pendingMedia: pending
      }));

      return { total: filteredItems.length, pending, synced, items: filteredItems };
    } catch (e) {
      console.error("[CameraSync] ❌ Scan failed:", e);
      return { total: 0, pending: 0, synced: 0, items: [] };
    }
  }

  /**
   * Run P2P backup to the selected desktop node.
   */
  async startSync(targetLinkId, targetFolder = "") {
    let s = {};
    syncState.subscribe((val) => (s = val))();
    targetLinkId = targetLinkId || s.targetLinkId;
    targetFolder = (targetFolder !== undefined && targetFolder !== "") ? targetFolder : s.targetFolder;

    if (!targetLinkId) {
      const devList = deviceRegistry.list();
      const onlineDev = devList.find(d => (d.isConnected || d.online) && !d.isLocal);
      const anyDev = onlineDev || devList.find(d => !d.isLocal) || devList[0];
      if (anyDev && anyDev.linkId) {
        targetLinkId = anyDev.linkId;
      }
    }

    if (!targetLinkId) {
      console.error("[CameraSync] startSync aborted: No targetLinkId provided");
      throw new Error("Please select a target destination device");
    }
    if (this._isSyncRunning) {
      console.warn("[CameraSync] startSync ignored: Sync is already running");
      return;
    }

    const defaultFolder = isNativeStorageAvailable() ? "/PhoneBackup" : "/DesktopBackup";
    const userName = (typeof window !== "undefined" && window.__omdProfile?.user) || "";
    const finalFolder = sanitizeTargetFolder(targetFolder || defaultFolder, userName);

    console.log(`[CameraSync] 🚀 Starting Sync => Target LinkId: "${targetLinkId}", Target Folder: "${finalFolder}"`);

    this._isSyncRunning = true;
    this._abortController = new AbortController();

    updateSettings({ targetLinkId, targetFolder: finalFolder });
    syncState.update((s) => ({
      ...s,
      isSyncing: true,
      statusMsg: "Scanning media and connecting to target node..."
    }));

    let drive = null;

    try {
      // 1. Scan and detect pending files
      const scanRes = await this.scanCameraRoll();
      const items = scanRes.items || [];
      const pendingItems = [];

      for (const item of items) {
        const record = await getStoredItem(item.id);
        if (!record || record.status !== "synced" || record.size !== item.size) {
          pendingItems.push(item);
        }
      }

      console.log(`[CameraSync] 📋 Pending files to transfer: ${pendingItems.length}`);

      if (pendingItems.length === 0) {
        console.log("[CameraSync] ✨ All files are already up to date!");
        syncState.update((s) => ({
          ...s,
          isSyncing: false,
          statusMsg: "All files are up to date!",
          lastSyncTime: Date.now()
        }));
        updateSettings({ lastSyncTime: Date.now() });
        this._isSyncRunning = false;
        return;
      }

      // 2. Connect or reuse existing DataChannel to the remote device
      console.log(`[CameraSync] 📡 Acquiring WebRTC link to target node "${targetLinkId}"...`);
      syncState.update((s) => ({
        ...s,
        statusMsg: `Establishing direct P2P WebRTC link...`
      }));

      drive = deviceRegistry.getDrive(targetLinkId);
      if (!drive || !drive.isConnected || !drive.dc || drive.dc.readyState !== "open") {
        drive = new WebRTCDataChannelDrive(targetLinkId);
        const connected = await drive.reconnect();
        console.log(`[CameraSync] 🔗 WebRTC connection result: ${connected}`);
        if (!connected) {
          throw new Error("Could not connect to target device over WebRTC");
        }
      } else {
        console.log(`[CameraSync] 🔗 Reusing already connected WebRTC drive for: ${targetLinkId}`);
      }

      // 3. Ensure target root folder exists on remote node
      try {
        console.log(`[CameraSync] 📁 Making target folder "${finalFolder}" on remote node...`);
        await drive.homeMkDir(finalFolder);
      } catch (mkdirErr) {
        console.log("[CameraSync] Root folder creation response:", mkdirErr);
      }

      // 4. Transfer pending files sequentially in chunks
      let syncedCount = scanRes.synced;

      for (let i = 0; i < pendingItems.length; i++) {
        if (this._abortController.signal.aborted) {
          console.warn("[CameraSync] Sync aborted by user");
          syncState.update((s) => ({ ...s, statusMsg: "Sync cancelled" }));
          break;
        }

        const item = pendingItems[i];
        const fileName = item.name;
        const relPath = item.relativePath || item.name;
        const remoteFilePath = finalFolder.replace(/\/$/, "") + "/" + relPath.replace(/^\//, "");

        console.log(`[CameraSync] 📤 [${i + 1}/${pendingItems.length}] Transferring "${fileName}" (${(item.size / 1024 / 1024).toFixed(2)} MB) to "${remoteFilePath}"`);

        // Ensure subfolder exists if file is in a nested path
        const lastSlash = remoteFilePath.lastIndexOf("/");
        if (lastSlash > 0) {
          const parentDir = remoteFilePath.substring(0, lastSlash);
          try {
            await drive.homeMkDir(parentDir);
          } catch (_) {}
        }

        syncState.update((s) => ({
          ...s,
          currentFile: fileName,
          statusMsg: `Syncing (${i + 1}/${pendingItems.length}): ${fileName}`,
          progressPercent: Math.round(((i) / pendingItems.length) * 100)
        }));

        let offset = 0;
        let eof = false;
        let chunkCount = 0;

        const itemMtime = item.date ? (typeof item.date === "number" ? item.date : new Date(item.date).getTime()) : undefined;

        while (!eof) {
          if (this._abortController.signal.aborted) break;

          const chunkRes = await readChunkForItem(item, offset, CHUNK_SIZE);

          if (!chunkRes || !chunkRes.data) {
            if (offset === 0 && item.size === 0) {
              // Empty file
              await drive.sendRPC("HomeWrite", {
                path: remoteFilePath,
                offset: 0,
                data: "",
                truncate: true,
                eof: true,
                ...(itemMtime ? { mtime: itemMtime } : {})
              });
            }
            break;
          }

          const nextOffset = offset + (chunkRes.bytesRead || 0);
          eof = !!chunkRes.eof || (item.size > 0 && nextOffset >= item.size);

          // Send chunk over WebRTC DataChannel
          await drive.sendRPC("HomeWrite", {
            path: remoteFilePath,
            offset,
            data: chunkRes.data,
            truncate: offset === 0,
            eof: eof,
            ...(eof && itemMtime ? { mtime: itemMtime } : {})
          }, 60000);

          offset = nextOffset;
          chunkCount++;

          if (chunkCount % 10 === 0 || eof) {
            console.log(`[CameraSync] 📦 "${fileName}": ${offset}/${item.size} bytes (${Math.round((offset / Math.max(item.size, 1)) * 100)}%)`);
          }
        }

        // Mark as synced
        await saveStoredItem({
          id: item.id,
          name: item.name,
          relativePath: item.relativePath || item.name,
          size: item.size,
          date: item.date,
          type: item.type,
          status: "synced",
          syncedAt: Date.now()
        });

        console.log(`[CameraSync] ✅ [${i + 1}/${pendingItems.length}] "${fileName}" transferred successfully!`);

        syncedCount++;
        syncState.update((s) => ({
          ...s,
          syncedMedia: syncedCount,
          pendingMedia: Math.max(0, pendingItems.length - (i + 1))
        }));
      }

      const now = Date.now();
      updateSettings({ lastSyncTime: now });
      syncState.update((s) => ({
        ...s,
        isSyncing: false,
        statusMsg: "Sync completed successfully!",
        progressPercent: 100,
        lastSyncTime: now
      }));
      console.log(`[CameraSync] 🎉 Backup completed successfully! Total synced: ${syncedCount}`);
    } catch (e) {
      console.error("[CameraSync] ❌ Sync error:", e);
      syncState.update((s) => ({
        ...s,
        isSyncing: false,
        statusMsg: `Sync error: ${e.message || String(e)}`
      }));
      throw e;
    } finally {
      this._isSyncRunning = false;
      this._abortController = null;
      if (drive) {
        try { drive.close(); } catch (_) {}
      }
    }
  }

  stopSync() {
    if (this._abortController) {
      this._abortController.abort();
    }
    this._isSyncRunning = false;
    syncState.update((s) => ({ ...s, isSyncing: false, statusMsg: "Sync stopped" }));
    console.log("[CameraSync] Sync stopped by user");
  }
}

export const cameraSyncManager = new CameraSyncManager();

