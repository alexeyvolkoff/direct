// Local browser device client (PWA / later Capacitor).
//
// Turns the current browser into an OnMyDisk device that exposes local folders
// (File System Access API) or OPFS storage as shares. `LocalDrive` implements
// the same SVAR data API surface as WebRTCDataChannelDrive (isConnected, dc,
// reconnect(), getFiles(), homeMkDir(), homeWrite(), resolvePath()) so the
// filemanager can treat it as a regular connected device.
//
// Phase 1 (this module): local registration + share creation + getFiles.
// Phase 2 (see .knowledge/pwa-client-plan.md): WebRTC answerer + gateway
// web->web routing so remote devices can connect to this browser.

import { isNativeStorageAvailable, NativeStorageDrive } from "./native-drive.js";
import { parseStoragePath } from "./storage-path.js";
import { deviceRegistry } from "./device-registry.js";

const DB_NAME = "omd_local";
const DB_VERSION = 1;
const STORE = "shares";
const FALLBACK_SHARES_KEY = "omd_local_shares_fallback";

const memoryShares = new Map();

function loadFallbackShares() {
  if (typeof localStorage === "undefined") return;
  try {
    const raw = localStorage.getItem(FALLBACK_SHARES_KEY);
    if (!raw) return;
    const arr = JSON.parse(raw);
    if (Array.isArray(arr)) {
      for (const s of arr) {
        if (s && s.name && !memoryShares.has(s.name)) {
          memoryShares.set(s.name, s);
        }
      }
    }
  } catch (_) {}
}

function persistFallbackShares() {
  if (typeof localStorage === "undefined") return;
  try {
    const serializable = [...memoryShares.values()].map((s) => ({
      name: s.name,
      kind: s.kind || "opfs",
      addedAt: s.addedAt || Date.now(),
    }));
    localStorage.setItem(FALLBACK_SHARES_KEY, JSON.stringify(serializable));
  } catch (_) {}
}

loadFallbackShares();

// Virtual in-memory OPFS fallback when browser WebStorage/OPFS bucket is corrupted or unavailable
class VirtualFileHandle {
  constructor(name, bytes = new Uint8Array(0)) {
    this.kind = "file";
    this.name = name;
    this._bytes = bytes;
    this._lastModified = Date.now();
  }
  async getFile() {
    const b = this._bytes;
    return new File([b], this.name, { lastModified: this._lastModified });
  }
  async createWritable() {
    const self = this;
    const chunks = [];
    return {
      async write(data) {
        if (data instanceof ArrayBuffer) chunks.push(new Uint8Array(data));
        else if (ArrayBuffer.isView(data)) chunks.push(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
        else if (typeof data === "string") chunks.push(new TextEncoder().encode(data));
        else if (data instanceof Blob) chunks.push(new Uint8Array(await data.arrayBuffer()));
      },
      async close() {
        const total = chunks.reduce((acc, c) => acc + c.byteLength, 0);
        const merged = new Uint8Array(total);
        let offset = 0;
        for (const c of chunks) {
          merged.set(c, offset);
          offset += c.byteLength;
        }
        self._bytes = merged;
        self._lastModified = Date.now();
      },
    };
  }
}

class VirtualDirHandle {
  constructor(name) {
    this.kind = "directory";
    this.name = name;
    this._children = new Map();
  }
  async getDirectoryHandle(name, opts = {}) {
    const existing = this._children.get(name);
    if (existing) {
      if (existing.kind !== "directory") throw Object.assign(new Error("TypeMismatchError"), { name: "TypeMismatchError" });
      return existing;
    }
    if (opts && opts.create) {
      const dir = new VirtualDirHandle(name);
      this._children.set(name, dir);
      return dir;
    }
    throw Object.assign(new Error("NotFoundError"), { name: "NotFoundError" });
  }
  async getFileHandle(name, opts = {}) {
    const existing = this._children.get(name);
    if (existing) {
      if (existing.kind !== "file") throw Object.assign(new Error("TypeMismatchError"), { name: "TypeMismatchError" });
      return existing;
    }
    if (opts && opts.create) {
      const f = new VirtualFileHandle(name);
      this._children.set(name, f);
      return f;
    }
    throw Object.assign(new Error("NotFoundError"), { name: "NotFoundError" });
  }
  async removeEntry(name) {
    this._children.delete(name);
  }
  async *entries() {
    for (const [k, v] of this._children.entries()) {
      yield [k, v];
    }
  }
}

let virtualOpfsRoot = null;
function getVirtualOpfsRoot() {
  if (!virtualOpfsRoot) virtualOpfsRoot = new VirtualDirHandle("");
  return virtualOpfsRoot;
}

async function getOpfsRoot() {
  if (typeof navigator !== "undefined" && navigator.storage && typeof navigator.storage.getDirectory === "function") {
    try {
      const root = await Promise.race([
        navigator.storage.getDirectory(),
        new Promise((_, reject) => setTimeout(() => reject(new Error("OPFS getDirectory timeout")), 2000)),
      ]);
      if (root) return root;
    } catch (err) {
      console.warn("[LocalDrive] navigator.storage.getDirectory failed, using virtual OPFS fallback:", err);
    }
  }
  return getVirtualOpfsRoot();
}

function openDB() {
  if (typeof indexedDB === "undefined") {
    return Promise.reject(new Error("indexedDB unavailable"));
  }
  return new Promise((resolve, reject) => {
    let settled = false;
    const timer = setTimeout(() => {
      if (!settled) {
        settled = true;
        reject(new Error("IndexedDB open timeout"));
      }
    }, 1500);
    try {
      const req = indexedDB.open(DB_NAME, DB_VERSION);
      req.onupgradeneeded = () => {
        try {
          if (!req.result.objectStoreNames.contains(STORE)) {
            req.result.createObjectStore(STORE, { keyPath: "name" });
          }
        } catch (_) {}
      };
      req.onsuccess = () => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          resolve(req.result);
        }
      };
      req.onerror = () => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          reject(req.error || new Error("IndexedDB open error"));
        }
      };
      req.onblocked = () => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          reject(new Error("IndexedDB open blocked"));
        }
      };
    } catch (err) {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        reject(err);
      }
    }
  });
}

export async function saveLocalShare(share) {
  if (share && share.name) {
    memoryShares.set(share.name, share);
    persistFallbackShares();
  }
  try {
    const db = await openDB();
    return await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, "readwrite");
      tx.objectStore(STORE).put(share);
      tx.oncomplete = () => resolve(share);
      tx.onerror = () => reject(tx.error);
    });
  } catch (err) {
    console.warn("[LocalDrive] saveLocalShare IndexedDB fallback:", err);
    return share;
  }
}

export async function listLocalShares() {
  loadFallbackShares();
  if (typeof indexedDB === "undefined") return [...memoryShares.values()];
  try {
    const db = await openDB();
    const idbShares = await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, "readonly");
      const req = tx.objectStore(STORE).getAll();
      req.onsuccess = () => resolve(req.result || []);
      req.onerror = () => reject(req.error);
    });
    for (const s of idbShares) {
      if (s && s.name) memoryShares.set(s.name, s);
    }
    persistFallbackShares();
    return [...memoryShares.values()];
  } catch (err) {
    console.warn("[LocalDrive] listLocalShares IndexedDB fallback:", err);
    return [...memoryShares.values()];
  }
}

async function getLocalShare(name) {
  loadFallbackShares();
  if (memoryShares.has(name)) {
    const cached = memoryShares.get(name);
    if (cached && (cached.kind !== "fs" || cached.handle)) return cached;
  }
  try {
    const db = await openDB();
    const found = await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, "readonly");
      const req = tx.objectStore(STORE).get(name);
      req.onsuccess = () => resolve(req.result || null);
      req.onerror = () => reject(req.error);
    });
    if (found && found.name) {
      memoryShares.set(found.name, found);
      persistFallbackShares();
      return found;
    }
  } catch (err) {
    console.warn("[LocalDrive] getLocalShare IndexedDB fallback:", err);
  }
  return memoryShares.get(name) || null;
}

async function removeLocalShare(name) {
  memoryShares.delete(name);
  persistFallbackShares();
  try {
    const db = await openDB();
    return await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, "readwrite");
      tx.objectStore(STORE).delete(name);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  } catch (_) {}
}

export async function clearAllLocalShares() {
  memoryShares.clear();
  persistFallbackShares();
  if (typeof indexedDB === "undefined") return;
  try {
    const db = await openDB();
    return await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, "readwrite");
      tx.objectStore(STORE).clear();
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  } catch (_) {}
}

/** Persistent identity for the browser-as-device. */
export function getOrCreateLocalLinkId() {
  if (typeof localStorage === "undefined") return "local-device";
  let id = localStorage.getItem("omd_local_linkid");
  // Migration: older builds stored ids with a redundant "browser-" prefix;
  // strip it so users enter 10 characters like for native clients.
  if (id && id.startsWith("browser-")) {
    id = id.slice("browser-".length);
    localStorage.setItem("omd_local_linkid", id);
  }
  if (!id) {
    id = Math.random().toString(36).slice(2, 12);
    localStorage.setItem("omd_local_linkid", id);
  }
  return id;
}

export function isFileSystemAccessAvailable() {
  return typeof window !== "undefined" && !!window.showDirectoryPicker;
}

/** Pick a local folder with the File System Access API and register a share. */
export async function addShareFromPicker() {
  if (!isFileSystemAccessAvailable()) {
    throw new Error("File System Access API not available in this browser");
  }
  const handle = await window.showDirectoryPicker({ mode: "readwrite" });
  const name = handle.name || "LocalFolder";
  const share = { name, kind: "fs", handle, addedAt: Date.now() };
  await saveLocalShare(share);
  return { name, kind: "fs" };
}

export const DEFAULT_STORAGE_PATH = "/Private/Data";

/** Register an OPFS share (fallback when no File System Access API). */
export async function addOpfsShare(name = "LocalStorage") {
  await getOpfsRoot(); // verify OPFS (or virtual fallback) is available
  const share = { name, kind: "opfs", addedAt: Date.now() };
  await saveLocalShare(share);
  return { name, kind: "opfs" };
}

/** Ensure default OPFS share and subfolder hierarchy exist for browser devices. */
export async function ensureDefaultOpfsShare(defaultStorage = DEFAULT_STORAGE_PATH) {
  let shareName = "Private";
  try {
    const parsed = parseStoragePath(defaultStorage || DEFAULT_STORAGE_PATH);
    let raw = (parsed.folder || DEFAULT_STORAGE_PATH).replace(/^\/+|\/+$/g, "");
    const localName = (typeof localStorage !== "undefined" ? localStorage.getItem("omd_local_devicename") : "") || getDefaultWebDeviceName();

    // If the storage path is qualified with a foreign device (e.g. "/remote-node/Data" or "/remote-node"),
    // do NOT treat that foreign device name as a local OPFS share! Local fallback is always Private.
    if (parsed.isDeviceQualified && parsed.deviceName && parsed.deviceName.toLowerCase() !== localName.toLowerCase()) {
      raw = "Private/Data";
    }

    if (raw.toLowerCase().startsWith(localName.toLowerCase() + "/")) {
      raw = raw.slice(localName.length).replace(/^\/+/, "");
    } else if (raw.toLowerCase() === localName.toLowerCase()) {
      raw = "Private";
    }
    // Collapse accidental duplicate "Private/Private"
    raw = raw.replace(/^Private\/Private(\/|$)/i, "Private$1");
    // If given only "/Data" or "/OnMyChat", qualify it with the share prefix "/Private/..."
    if (!raw.toLowerCase().startsWith("private/") && (
      raw.toLowerCase() === "data" || raw.toLowerCase().startsWith("data/") ||
      raw.toLowerCase() === "onmychat" || raw.toLowerCase().startsWith("onmychat/")
    )) {
      raw = "Private/" + raw;
    }
    const parts = raw.split("/").filter(Boolean);
    shareName = parts[0] || "Private";
    // Check if shareName collides with any known non-local device (e.g. "remote-node")
    if (deviceRegistry.list().some(d => d && !d.local && d.name && d.name.toLowerCase() === shareName.toLowerCase())) {
      shareName = "Private";
    }
    const subPath = parts.slice(1).join("/") || "Data";

    const opfsRoot = await getOpfsRoot();
    const shareDir = await opfsRoot.getDirectoryHandle(shareName, { create: true });

    // Clean up accidental nested share directory (e.g. opfsRoot/Private/Private)
    try {
      const nested = await shareDir.getDirectoryHandle(shareName, { create: false });
      if (nested) {
        for await (const [childName, childHandle] of nested.entries()) {
          if (childHandle.kind === "directory") {
            const targetSub = await shareDir.getDirectoryHandle(childName, { create: true });
            for await (const [subName, subHandle] of childHandle.entries()) {
              if (subHandle.kind === "file") {
                const srcFile = await subHandle.getFile();
                const dstFileHandle = await targetSub.getFileHandle(subName, { create: true });
                const writable = await dstFileHandle.createWritable();
                await writable.write(await srcFile.arrayBuffer());
                await writable.close();
              }
            }
          } else if (childHandle.kind === "file") {
            const srcFile = await childHandle.getFile();
            const dstFileHandle = await shareDir.getFileHandle(childName, { create: true });
            const writable = await dstFileHandle.createWritable();
            await writable.write(await srcFile.arrayBuffer());
            await writable.close();
          }
        }
        await shareDir.removeEntry(shareName, { recursive: true });
        console.log("[LocalDrive] Cleaned up accidental nested", shareName, "folder in OPFS");
      }
    } catch (_) {
      // Normal: no nested Private folder
    }

    let currentDir = shareDir;
    for (const seg of subPath.split("/").filter(Boolean)) {
      currentDir = await currentDir.getDirectoryHandle(seg, { create: true });
    }
    await currentDir.getDirectoryHandle("generated", { create: true });

    let share = await getLocalShare(shareName);
    if (!share) {
      share = { name: shareName, kind: "opfs", addedAt: Date.now() };
      await saveLocalShare(share);
    }
    // Clean up any old orphan share entries that might cause duplicate listings (e.g. named after device)
    try {
      const existingShares = await listLocalShares();
      const allDevNames = deviceRegistry.list()
        .filter(d => d && !d.local && d.name)
        .map(d => d.name.toLowerCase());
      for (const s of existingShares) {
        if (!s.name) continue;
        const sNameLower = s.name.toLowerCase();
        const isDeviceCollision = allDevNames.includes(sNameLower);
        if (sNameLower === "onmychat" || sNameLower === localName.toLowerCase() || isDeviceCollision) {
          await removeLocalShare(s.name);
          try {
            await opfsRoot.removeEntry(s.name, { recursive: true });
            console.log("[LocalDrive] Cleaned up accidental share named after device:", s.name);
          } catch (_) {}
        }
      }
    } catch (_) {}

    return share;
  } catch (e) {
    console.warn("[LocalDrive] ensureDefaultOpfsShare error, registering fallback share:", e);
    const fallbackShare = { name: shareName || "Private", kind: "opfs", addedAt: Date.now() };
    await saveLocalShare(fallbackShare);
    return fallbackShare;
  }
}

export function getResolutionDeviceType() {
  if (typeof window === "undefined") return "desktop";
  const w = window.innerWidth;
  if (w < 768) return "phone";
  if (w <= 1024) return "tablet";
  return "desktop";
}

export function getDefaultWebDeviceName() {
  const dt = getResolutionDeviceType();
  if (typeof navigator === "undefined") return "Linux-" + dt;
  const ua = navigator.userAgent || "";
  if (ua.includes("iPad")) return "iPad";
  if (ua.includes("iPod/") || ua.includes("iPod")) return "iPod";
  if (ua.includes("iPhone")) return "iPhone";
  if (ua.includes("Android")) return "Android-" + dt;
  if (ua.includes("Macintosh") || ua.includes("Mac OS")) return "Mac-" + dt;
  if (ua.includes("Windows")) return "Windows-" + dt;
  if (ua.includes("Linux")) return "Linux-" + dt;
  return "device-" + dt;
}

/** Build a device object that the SVAR app can register like a remote device. */
export function makeLocalDevice(name) {
  let devName = name;
  if (!devName || devName === "This Device" || devName.includes("(browser)")) {
    if (typeof localStorage !== "undefined") {
      const stored = localStorage.getItem("omd_local_devicename");
      if (stored && stored !== "This Device" && !stored.includes("(browser)")) {
        devName = stored;
      }
    }
  }
  if (!devName || devName === "This Device" || devName.includes("(browser)")) {
    devName = getDefaultWebDeviceName();
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_local_devicename", devName);
    }
  }
  const linkId = getOrCreateLocalLinkId();
  const drive = isNativeStorageAvailable() ? new NativeStorageDrive() : new LocalDrive();
  const dt = getResolutionDeviceType();
  return {
    deviceId: linkId,
    linkId,
    name: devName,
    prefix: "/" + devName,
    drive,
    status: "connected",
    local: true,
    type: "web",
    deviceType: dt,
    // Своё устройство в этом браузере/клиенте — права владельца: локальный
    // Chroma-индекс приватный. Подключённая публичная AI-нода НЕ local —
    // она передаётся с isOwner: false (гостевой уровень).
    isOwner: true,
  };
}

export class LocalDrive {
  constructor() {
    this.isConnected = true;
    // Satisfy the app's `drive.dc.readyState !== "open"` checks.
    this.dc = { readyState: "open" };
  }

  async reconnect() {
    this.isConnected = true;
    return true;
  }

  async close() {
    // Local browser storage is always available to its own browser tab
    this.isConnected = true;
  }

  async disconnect() {
    this.isConnected = true;
  }

  async _resolveDir(folderId, create = false) {
    let clean = String(folderId || "/").replace(/^\/+/, "");
    if (!clean.toLowerCase().startsWith("private/") && (
      clean.toLowerCase().startsWith("data/") || clean.toLowerCase() === "data" ||
      clean.toLowerCase().startsWith("onmychat/") || clean.toLowerCase() === "onmychat"
    )) {
      clean = "Private/" + clean;
    }
    const parts = clean.split("/").filter(Boolean);
    const shareName = parts.shift();
    if (!shareName) return { dirHandle: null, baseId: "/" };
    let share = await getLocalShare(shareName);
    if (!share && shareName.toLowerCase() === "private") {
      await ensureDefaultOpfsShare();
      share = await getLocalShare("Private") || { name: "Private", kind: "opfs", addedAt: Date.now() };
    }
    if (!share) return null;
    let dirHandle = await this._getShareRoot(share);
    for (const seg of parts) {
      if (!seg) continue;
      if (create) {
        dirHandle = await dirHandle.getDirectoryHandle(seg, { create: true });
      } else {
        try {
          dirHandle = await dirHandle.getDirectoryHandle(seg, { create: false });
        } catch (err) {
          let found = null;
          const targetLower = seg.toLowerCase();
          try {
            for await (const [name, handle] of dirHandle.entries()) {
              if (handle.kind === "directory" && name.toLowerCase() === targetLower) {
                found = handle;
                break;
              }
            }
          } catch (_) {}
          if (found) {
            dirHandle = found;
          } else {
            throw err;
          }
        }
      }
    }
    return { dirHandle, baseId: "/" + shareName + (parts.length ? "/" + parts.join("/") : "") };
  }

  /** Resolve a FILE path: parent segments as directories, last segment as a
   *  file entry. Returns { file, fileName } or null when not found. */
  async _resolveFile(virtualPath) {
    const parts = String(virtualPath || "").split("/").filter(Boolean);
    const fileName = parts.pop();
    if (!fileName) return null;
    const dir = await this._resolveDir(parts.length ? "/" + parts.join("/") : "/");
    if (!dir || !dir.dirHandle) return null;
    try {
      const fileHandle = await dir.dirHandle.getFileHandle(fileName, { create: false });
      const file = await fileHandle.getFile();
      return { file, fileName };
    } catch (e) {
      if (e && e.name === "NotFoundError") return null;
      throw e;
    }
  }

  async _getShareRoot(share) {
    if (share.handle) return share.handle;
    if (share.kind === "opfs") {
      const opfsRoot = await getOpfsRoot();
      if (share.name && share.name !== "LocalStorage") {
        return await opfsRoot.getDirectoryHandle(share.name, { create: true });
      }
      return opfsRoot;
    }
    return share.handle;
  }

  _shareToItem(s) {
    return {
      id: "/" + s.name,
      value: s.name,
      name: s.name,
      type: "folder",
      isDir: true,
      size: 0,
      date: s.addedAt ? new Date(s.addedAt) : new Date(),
      lazy: true,
      realPath: "/" + s.name,
    };
  }

  async _listDir(dirHandle, baseId) {
    const items = [];
    for await (const [name, handle] of dirHandle.entries()) {
      if (name.startsWith(".")) continue;
      const isDir = handle.kind === "directory";
      let size = 0;
      if (!isDir) {
        try {
          size = (await handle.getFile()).size;
        } catch (e) {
          size = 0;
        }
      }
      const item = {
        id: baseId + "/" + name,
        value: name,
        name,
        type: isDir ? "folder" : "file",
        isDir,
        size,
        date: new Date(),
        lazy: isDir,
        mimeType: "",
        realPath: baseId + "/" + name,
      };
      items.push(item);
    }
    items.sort((a, b) =>
      a.isDir === b.isDir ? a.name.localeCompare(b.name) : a.isDir ? -1 : 1
    );
    items.realPath = baseId;
    return items;
  }

  /** Device root returns shares; deeper paths resolve into a share's tree. */
  async getFiles(folderId = "/", opts = {}) {
    const resolved = await this._resolveDir(folderId || "/");
    if (!resolved) return [];
    if (!resolved.dirHandle) {
      // Device root -> list shares
      let shares = await listLocalShares();
      if ((!shares || shares.length === 0) && !isNativeStorageAvailable()) {
        const defaultShare = await ensureDefaultOpfsShare();
        shares = await listLocalShares();
        if ((!shares || shares.length === 0) && defaultShare) {
          shares = [defaultShare];
        }
      }
      const items = (shares || []).map((s) => this._shareToItem(s));
      items.realPath = "/";
      return items;
    }
    return this._listDir(resolved.dirHandle, resolved.baseId);
  }

  /** Map a virtual path like "/Share/sub/.." to the share-relative virtual path. */
  async resolvePath(path = "") {
    const parts = String(path || "/").split("/").filter(Boolean);
    const shareName = parts.shift();
    if (!shareName) {
      return { realPath: "/" };
    }
    let share = await getLocalShare(shareName);
    if (!share && shareName.toLowerCase() === "private") {
      share = await ensureDefaultOpfsShare();
    }
    if (!share) throw new Error("Unknown share: " + shareName);
    return { realPath: "/" + shareName + (parts.length ? "/" + parts.join("/") : "") };
  }

  async mkDir(virtualPath) {
    return this.homeMkDir(virtualPath);
  }

  async homeMkDir(virtualPath) {
    const parts = String(virtualPath || "").split("/").filter(Boolean);
    if (parts.length < 2) throw new Error("Invalid path");
    const shareName = parts.shift();
    const share = await getLocalShare(shareName);
    if (!share) throw new Error("Unknown share: " + shareName);
    let dirHandle = await this._getShareRoot(share);
    const newName = parts.pop();
    for (const seg of parts) {
      if (!seg) continue;
      dirHandle = await dirHandle.getDirectoryHandle(seg, { create: true });
    }
    await dirHandle.getDirectoryHandle(newName, { create: true });
    return { path: virtualPath, name: newName };
  }

  async homeWrite(virtualPath, dataBase64) {
    let clean = String(virtualPath || "").replace(/^\/+/, "");
    if (!clean.toLowerCase().startsWith("private/") && (
      clean.toLowerCase().startsWith("data/") || clean.toLowerCase() === "data" ||
      clean.toLowerCase().startsWith("onmychat/") || clean.toLowerCase() === "onmychat"
    )) {
      clean = "Private/" + clean;
    }
    const parts = clean.split("/").filter(Boolean);
    if (parts.length < 2) throw new Error("Invalid path");
    const shareName = parts.shift();
    let share = await getLocalShare(shareName);
    if (!share && shareName.toLowerCase() === "private") {
      await ensureDefaultOpfsShare();
      share = await getLocalShare("Private");
    }
    if (!share) throw new Error("Unknown share: " + shareName);
    let dirHandle = await this._getShareRoot(share);
    const fileName = parts.pop();
    for (const seg of parts) {
      if (!seg) continue;
      dirHandle = await dirHandle.getDirectoryHandle(seg, { create: true });
    }
    const fileHandle = await dirHandle.getFileHandle(fileName, { create: true });
    const writable = await fileHandle.createWritable();
    const bytes = Uint8Array.from(atob(dataBase64), (c) => c.charCodeAt(0));
    await writable.write(bytes);
    await writable.close();
    return { path: virtualPath, name: fileName, size: bytes.length };
  }

  /** Read a file's bytes as base64 (for previews / content). */
  async readFile(virtualPath) {
    const resolved = await this._resolveFile(virtualPath);
    if (!resolved) throw new Error("Not found: " + virtualPath);
    const { file } = resolved;
    const buf = await file.arrayBuffer();
    const bytes = new Uint8Array(buf);
    let bin = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    return { data: btoa(bin), size: bytes.length };
  }

  /** Read a whole file as a Blob/File (viewers, downloads). Mirrors the
   *  WebRTCDataChannelDrive API so UI code can treat both uniformly. */
  async getFileContent(virtualPath) {
    const resolved = await this._resolveFile(virtualPath);
    if (!resolved) throw new Error("Not found: " + virtualPath);
    return resolved.file;
  }

  /** Return file metadata (size, mimeType) without reading content. */
  async open(virtualPath, query = "") {
    const resolved = await this._resolveFile(virtualPath);
    if (!resolved) throw new Error("Not found: " + virtualPath);
    const { file, fileName } = resolved;
    return {
      name: fileName,
      path: virtualPath,
      isDir: false,
      size: file.size,
      mimeType: file.type || "application/octet-stream",
    };
  }

  /** Read a byte range of a file, returning base64-encoded data. */
  async read(virtualPath, offset = 0, size = 65536, query = "") {
    const resolved = await this._resolveFile(virtualPath);
    if (!resolved) throw new Error("Not found: " + virtualPath);
    const slice = resolved.file.slice(offset, Math.min(offset + size, resolved.file.size));
    const buf = await slice.arrayBuffer();
    const bytes = new Uint8Array(buf);
    let bin = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    return { data: btoa(bin), offset, size: bytes.length };
  }

  /** Native block read: bytes [offset, offset+size) straight from the
   *  File System Access / OPFS handle. size=0 → till EOF. Mirrors the
   *  P2P Open/Read protocol so the SW bridge can serve local devices
   *  exactly like remote ones. */
  async readRange(virtualPath, offset = 0, size = 0) {
    const resolved = await this._resolveFile(virtualPath);
    if (!resolved) throw new Error("Not found: " + virtualPath);
    const end = size > 0 ? Math.min(offset + size, resolved.file.size) : resolved.file.size;
    const bytes = await resolved.file.slice(offset, end).arrayBuffer();
    return { bytes, totalSize: resolved.file.size };
  }

  /** Write bytes to a file at the given offset. */
  async write(virtualPath, offset = 0, dataBase64 = "", truncate = false) {
    const parts = String(virtualPath || "").split("/").filter(Boolean);
    const fileName = parts.pop();
    if (!fileName) throw new Error("Invalid path");
    const dir = await this._resolveDir(parts.length ? "/" + parts.join("/") : "/");
    if (!dir || !dir.dirHandle) throw new Error("Not found: " + virtualPath);
    const fileHandle = await dir.dirHandle.getFileHandle(fileName, { create: true });
    const writable = await fileHandle.createWritable({ keepExistingData: !truncate });
    if (offset > 0) await writable.seek(offset);
    const bytes = Uint8Array.from(atob(dataBase64), (c) => c.charCodeAt(0));
    await writable.write(bytes);
    await writable.close();
    return { written: bytes.length };
  }

  /** Copy a file from sourcePath to targetPath (both virtual paths within shares). */
  async copyFile(sourcePath, targetPath) {
    const srcParts = sourcePath.split("/").filter(Boolean);
    const srcFileName = srcParts.pop();
    const srcDir = await this._resolveDir(srcParts.length ? "/" + srcParts.join("/") : "/");
    if (!srcDir || !srcDir.dirHandle) throw new Error("Source not found: " + sourcePath);
    const srcHandle = await srcDir.dirHandle.getFileHandle(srcFileName);
    const srcFile = await srcHandle.getFile();

    const tgtParts = targetPath.split("/").filter(Boolean);
    if (tgtParts.length === 0 || tgtParts[tgtParts.length - 1] !== srcFileName) {
      tgtParts.push(srcFileName);
    }
    const tgtFileName = tgtParts.pop();
    const tgtDirPath = tgtParts.length ? "/" + tgtParts.join("/") : "/";
    const tgtDir = await this._resolveDir(tgtDirPath, true);
    if (!tgtDir || !tgtDir.dirHandle) throw new Error("Target not found: " + targetPath);
    const tgtHandle = await tgtDir.dirHandle.getFileHandle(tgtFileName, { create: true });
    const writable = await tgtHandle.createWritable();
    await writable.write(srcFile);
    await writable.close();
    return {};
  }

  /** Move a file from sourcePath to targetPath. */
  async moveFile(sourcePath, targetPath) {
    await this.copyFile(sourcePath, targetPath);
    const srcParts = sourcePath.split("/").filter(Boolean);
    const srcFileName = srcParts.pop();
    const srcDir = await this._resolveDir(srcParts.length ? "/" + srcParts.join("/") : "/");
    if (srcDir && srcDir.dirHandle) {
      await srcDir.dirHandle.removeEntry(srcFileName).catch(() => {});
    }
    return {};
  }

  /** Rename a file within its parent directory. */
  async renameFile(virtualPath, newName) {
    const parts = virtualPath.split("/").filter(Boolean);
    const oldName = parts.pop();
    if (!oldName) throw new Error("Invalid path");
    const dir = await this._resolveDir(parts.length ? "/" + parts.join("/") : "/");
    if (!dir || !dir.dirHandle) throw new Error("Not found: " + virtualPath);

    const oldHandle = await dir.dirHandle.getFileHandle(oldName);
    const oldFile = await oldHandle.getFile();
    const newHandle = await dir.dirHandle.getFileHandle(newName, { create: true });
    const writable = await newHandle.createWritable();
    await writable.write(oldFile);
    await writable.close();
    await dir.dirHandle.removeEntry(oldName).catch(() => {});
    return {};
  }

  /** Remove a file or directory. */
  async removeFile(virtualPath, forced = false) {
    const parts = virtualPath.split("/").filter(Boolean);
    const name = parts.pop();
    if (!name) throw new Error("Invalid path");

    // Case 1: Removing a root share (e.g. "/beelink", "/Storage")
    if (parts.length === 0) {
      const share = await getLocalShare(name);
      if (share) {
        if (share.kind === "opfs") {
          try {
            const opfsRoot = await navigator.storage.getDirectory();
            await opfsRoot.removeEntry(name, { recursive: true });
          } catch (_) {}
        }
        await removeLocalShare(name);
        return {};
      }
      // If not tracked in IndexedDB, try direct OPFS removal
      try {
        const opfsRoot = await navigator.storage.getDirectory();
        await opfsRoot.removeEntry(name, { recursive: true });
        return {};
      } catch (_) {}
      throw new Error("Share not found: " + virtualPath);
    }

    // Case 2: Removing a file/folder inside a share
    const dir = await this._resolveDir(parts.length ? "/" + parts.join("/") : "/");
    if (!dir || !dir.dirHandle) throw new Error("Not found: " + virtualPath);
    await dir.dirHandle.removeEntry(name, { recursive: forced });
    return {};
  }

  /** Get device settings (browser-local storage). */
  async getSettings() {
    const stored = typeof localStorage !== "undefined"
      ? localStorage.getItem("omd-local-settings")
      : null;
    let obj = stored ? JSON.parse(stored) : {};
    // Hostname must be the user-visible device name (Linux-desktop etc),
    // not the internal linkId. Sync with omd_local_devicename.
    try {
      let devName = typeof localStorage !== "undefined" ? localStorage.getItem("omd_local_devicename") : null;
      if (!devName || devName === "This Device" || devName.includes("(browser)")) {
        devName = getDefaultWebDeviceName();
        if (typeof localStorage !== "undefined") localStorage.setItem("omd_local_devicename", devName);
      }
      obj.hostname = devName;
      obj.linkId = getOrCreateLocalLinkId();
      // gateway shown in Identity section — must be the real gateway the
      // web device registers to (wss://<host>:9988), not window.location.
      // Migrate old "localhost" values. Do not append port in the display.
      const needsServerFix = !obj.server || obj.server === "localhost" || obj.server === "127.0.0.1"
        || String(obj.server).includes("localhost") || String(obj.server).includes("127.0.0.1");
      if (needsServerFix) {
        const h = typeof window !== "undefined" ? window.location.hostname : "";
        const host = (!h || h === "localhost" || h === "127.0.0.1") ? "disk.direct" : h;
        obj.server = host;
        // persist migrated value so next load is correct
        try { localStorage.setItem("omd-local-settings", JSON.stringify(obj)); } catch (_) {}
      }
    } catch (_) {}
    return obj;
  }

  /** Save device settings (browser-local storage). */
  async setSettings(settings) {
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd-local-settings", JSON.stringify(settings));
      if (settings.hostname) {
        localStorage.setItem("omd_local_devicename", settings.hostname);
      }
    }
    return {};
  }
}