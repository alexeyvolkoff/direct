import { Capacitor } from "@capacitor/core";
import { NativeStorage, isNativeStorageAvailable } from "../native-drive.js";

const SESSION_STORAGE_KEY = "omd_did_session";
const NATIVE_SESSION_PATH = "/AppData/.omd_auth_session.json";
const IDB_NAME = "omd_auth_vault";
const IDB_STORE = "kv";
const IDB_KEY = "did_session_v1";

function isValidSessionObject(obj) {
  return Boolean(
    obj &&
      typeof obj === "object" &&
      (obj.mainPub || (obj.devicePair && obj.devicePair.pub && obj.devicePair.priv))
  );
}

function encodeUtf8Base64(str) {
  const bytes = new TextEncoder().encode(str);
  let binary = "";
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunkSize));
  }
  return btoa(binary);
}

function decodeUtf8Base64(b64) {
  const binary = atob(b64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return new TextDecoder("utf-8").decode(bytes);
}

function isAndroidNativeFileBackupAvailable() {
  try {
    return (
      typeof window !== "undefined" &&
      Capacitor.isNativePlatform() &&
      Capacitor.getPlatform() === "android" &&
      isNativeStorageAvailable()
    );
  } catch (_) {
    return false;
  }
}

function openAuthIdb() {
  if (typeof indexedDB === "undefined") return Promise.resolve(null);
  return new Promise((resolve) => {
    try {
      const req = indexedDB.open(IDB_NAME, 1);
      req.onupgradeneeded = () => {
        const db = req.result;
        if (!db.objectStoreNames.contains(IDB_STORE)) {
          db.createObjectStore(IDB_STORE);
        }
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => resolve(null);
    } catch (_) {
      resolve(null);
    }
  });
}

async function writeIdbEnvelope(envelope) {
  const db = await openAuthIdb();
  if (!db) return;
  await new Promise((resolve) => {
    try {
      const tx = db.transaction(IDB_STORE, "readwrite");
      tx.objectStore(IDB_STORE).put(envelope, IDB_KEY);
      tx.oncomplete = () => {
        try { db.close(); } catch (_) {}
        resolve();
      };
      tx.onerror = () => {
        try { db.close(); } catch (_) {}
        resolve();
      };
    } catch (_) {
      try { db.close(); } catch (__) {}
      resolve();
    }
  });
}

async function readIdbEnvelope() {
  const db = await openAuthIdb();
  if (!db) return null;
  return new Promise((resolve) => {
    try {
      const tx = db.transaction(IDB_STORE, "readonly");
      const req = tx.objectStore(IDB_STORE).get(IDB_KEY);
      req.onsuccess = () => {
        const val = req.result || null;
        try { db.close(); } catch (_) {}
        resolve(val);
      };
      req.onerror = () => {
        try { db.close(); } catch (_) {}
        resolve(null);
      };
    } catch (_) {
      try { db.close(); } catch (__) {}
      resolve(null);
    }
  });
}

async function clearIdbEnvelope() {
  const db = await openAuthIdb();
  if (!db) return;
  await new Promise((resolve) => {
    try {
      const tx = db.transaction(IDB_STORE, "readwrite");
      tx.objectStore(IDB_STORE).delete(IDB_KEY);
      tx.oncomplete = () => {
        try { db.close(); } catch (_) {}
        resolve();
      };
      tx.onerror = () => {
        try { db.close(); } catch (_) {}
        resolve();
      };
    } catch (_) {
      try { db.close(); } catch (__) {}
      resolve();
    }
  });
}

async function writeAndroidNativeEnvelope(envelope) {
  if (!isAndroidNativeFileBackupAvailable()) return;
  try {
    const json = JSON.stringify(envelope);
    const data = encodeUtf8Base64(json);
    try {
      await NativeStorage.delete({ path: NATIVE_SESSION_PATH });
    } catch (_) {}
    await NativeStorage.writeChunk({
      path: NATIVE_SESSION_PATH,
      offset: 0,
      data,
      append: false,
    });
  } catch (err) {
    console.warn("[SessionBackup] Native file write warning:", err?.message || err);
  }
}

async function readAndroidNativeEnvelope() {
  if (!isAndroidNativeFileBackupAvailable()) return null;
  try {
    const stat = await NativeStorage.stat({ path: NATIVE_SESSION_PATH });
    if (!stat || !stat.exists || !stat.size) return null;
    const res = await NativeStorage.readChunk({
      path: NATIVE_SESSION_PATH,
      offset: 0,
      length: Math.max(Number(stat.size) || 0, 65536),
    });
    if (!res || !res.data) return null;
    const json = decodeUtf8Base64(res.data);
    return JSON.parse(json);
  } catch (_) {
    return null;
  }
}

async function clearAndroidNativeEnvelope() {
  if (!isAndroidNativeFileBackupAvailable()) return;
  try {
    await NativeStorage.delete({ path: NATIVE_SESSION_PATH });
  } catch (_) {}
}

function buildEnvelope(sessionData) {
  const ls = typeof localStorage !== "undefined" ? localStorage : null;
  const did = sessionData?.did || (sessionData?.mainPub ? `did:gun:${sessionData.mainPub}` : "");
  const mainPub = sessionData?.mainPub || "";
  const onboardedKey = did ? `omd_onboarded_${did}` : "";
  return {
    version: 1,
    updatedAt: Date.now(),
    session: sessionData,
    localLinkId: (ls && ls.getItem("omd_local_linkid")) || "",
    localDeviceName: (ls && ls.getItem("omd_local_devicename")) || "",
    omdKey: (ls && ls.getItem("omd_key")) || sessionData?.profile?.omd_key || "",
    locale: (ls && ls.getItem("omd_locale")) || sessionData?.profile?.locale || "",
    defaultStorage: (ls && ls.getItem("omd_default_storage")) || sessionData?.profile?.defaultStorage || "",
    onboardedKey,
    onboardedVal:
      (ls && onboardedKey && ls.getItem(onboardedKey)) ||
      (ls && ls.getItem("omd_onboarding_completed")) ||
      "",
    devicesJson: (ls && mainPub && ls.getItem(`omd_devices_${mainPub}`)) || "",
  };
}

function hydrateLocalStorageFromEnvelope(envelope) {
  if (!envelope || !isValidSessionObject(envelope.session)) return null;
  try {
    if (typeof localStorage !== "undefined") {
      localStorage.setItem(SESSION_STORAGE_KEY, JSON.stringify(envelope.session));
      if (envelope.localLinkId && !localStorage.getItem("omd_local_linkid")) {
        localStorage.setItem("omd_local_linkid", envelope.localLinkId);
      }
      if (envelope.localDeviceName && !localStorage.getItem("omd_local_devicename")) {
        localStorage.setItem("omd_local_devicename", envelope.localDeviceName);
      }
      if (envelope.omdKey && !localStorage.getItem("omd_key")) {
        localStorage.setItem("omd_key", envelope.omdKey);
      }
      if (envelope.locale && !localStorage.getItem("omd_locale")) {
        localStorage.setItem("omd_locale", envelope.locale);
      }
      if (envelope.defaultStorage && !localStorage.getItem("omd_default_storage")) {
        localStorage.setItem("omd_default_storage", envelope.defaultStorage);
      }
      if (envelope.onboardedVal) {
        if (envelope.onboardedKey && !localStorage.getItem(envelope.onboardedKey)) {
          localStorage.setItem(envelope.onboardedKey, envelope.onboardedVal);
        }
        if (!localStorage.getItem("omd_onboarding_completed")) {
          localStorage.setItem("omd_onboarding_completed", envelope.onboardedVal);
        }
      }
      if (envelope.devicesJson && envelope.session.mainPub) {
        const devKey = `omd_devices_${envelope.session.mainPub}`;
        if (!localStorage.getItem(devKey)) {
          localStorage.setItem(devKey, envelope.devicesJson);
        }
      }
    }
  } catch (err) {
    console.warn("[SessionBackup] localStorage hydration warning:", err);
  }
  return envelope.session;
}

let writeQueue = Promise.resolve();

export function peekSavedSessionSync() {
  if (typeof localStorage === "undefined") return null;
  try {
    const raw = localStorage.getItem(SESSION_STORAGE_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    return isValidSessionObject(parsed) ? parsed : null;
  } catch (_) {
    return null;
  }
}

export function saveSessionEverywhere(sessionData) {
  if (!isValidSessionObject(sessionData)) return Promise.resolve();
  try {
    if (typeof localStorage !== "undefined") {
      localStorage.setItem(SESSION_STORAGE_KEY, JSON.stringify(sessionData));
    }
  } catch (err) {
    console.warn("[SessionBackup] localStorage write warning:", err);
  }

  const envelope = buildEnvelope(sessionData);
  writeQueue = writeQueue
    .catch(() => {})
    .then(() =>
      Promise.all([
        writeAndroidNativeEnvelope(envelope),
        writeIdbEnvelope(envelope),
      ])
    )
    .catch(() => {});
  return writeQueue;
}

export async function loadSavedSessionResilient() {
  const syncSession = peekSavedSessionSync();
  if (syncSession) {
    // Self-heal native & IDB backups for already-authenticated sessions
    const envelope = buildEnvelope(syncSession);
    writeQueue = writeQueue
      .catch(() => {})
      .then(() =>
        Promise.all([
          writeAndroidNativeEnvelope(envelope),
          writeIdbEnvelope(envelope),
        ])
      )
      .catch(() => {});
    return syncSession;
  }

  // Fallback 1: Android private AppData file (survives WebView DOMStorage eviction/process kill)
  const nativeEnvelope = await readAndroidNativeEnvelope();
  if (nativeEnvelope && isValidSessionObject(nativeEnvelope.session)) {
    console.info("[SessionBackup] Restored DID session from Android NativeStorage backup");
    const restored = hydrateLocalStorageFromEnvelope(nativeEnvelope);
    writeIdbEnvelope(nativeEnvelope).catch(() => {});
    return restored;
  }

  // Fallback 2: IndexedDB vault (survives localStorage clear / WKWebView / PWA hiccups)
  const idbEnvelope = await readIdbEnvelope();
  if (idbEnvelope && isValidSessionObject(idbEnvelope.session)) {
    console.info("[SessionBackup] Restored DID session from IndexedDB vault");
    const restored = hydrateLocalStorageFromEnvelope(idbEnvelope);
    writeAndroidNativeEnvelope(idbEnvelope).catch(() => {});
    return restored;
  }

  return null;
}

export function clearSessionEverywhere() {
  try {
    if (typeof localStorage !== "undefined") {
      localStorage.removeItem(SESSION_STORAGE_KEY);
    }
  } catch (_) {}

  writeQueue = writeQueue
    .catch(() => {})
    .then(() =>
      Promise.all([
        clearAndroidNativeEnvelope(),
        clearIdbEnvelope(),
      ])
    )
    .catch(() => {});
  return writeQueue;
}

