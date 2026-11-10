import { Capacitor } from "@capacitor/core";

/**
 * Deployment mode of the svar frontend.
 *
 * The same backend serves both the chat domain (onmychat.com) and the files
 * domain (direct.onmydisk.net), so the gateway injects the mode into
 * index.html per request. Static bundles (Capacitor / OTA / offline) have no
 * gateway and use the build-time value instead.
 */
export const APP_MODE_CHAT = "chat";
export const APP_MODE_FILES = "files";

export const APP_MODE_STORAGE_KEY = "omd_main_app";
const OVERRIDE_STORAGE_KEY = "omd_app_mode_override";

const CHAT_ALIASES = new Set(["chat", "chats", "onmychat", "ai", "chat-app"]);
const FILES_ALIASES = new Set(["files", "file", "filemanager", "onmydisk", "disk", "files-app"]);

let cachedResult = null;

export function normalizeAppMode(value) {
  if (typeof value !== "string") return "";
  const raw = value.trim().toLowerCase();
  if (!raw || raw.includes("{") || raw.includes("}")) return "";
  if (CHAT_ALIASES.has(raw)) return APP_MODE_CHAT;
  if (FILES_ALIASES.has(raw)) return APP_MODE_FILES;
  return "";
}

function safeGet(store, key) {
  try {
    return store.getItem(key) || "";
  } catch (_) {
    return "";
  }
}

function safeSet(store, key, value) {
  try {
    store.setItem(key, value);
  } catch (_) {}
}

function safeRemove(store, key) {
  try {
    store.removeItem(key);
  } catch (_) {}
}

function devOverrideAllowed() {
  try {
    if (import.meta.env && import.meta.env.DEV) return true;
  } catch (_) {}
  try {
    return String(import.meta.env?.VITE_ALLOW_MODE_OVERRIDE || "").toLowerCase() === "true";
  } catch (_) {
    return false;
  }
}

function applyUrlOverride() {
  if (typeof window === "undefined" || !devOverrideAllowed()) return "";
  let value = "";
  try {
    value = new URLSearchParams(window.location.search).get("app") || "";
  } catch (_) {
    return "";
  }
  const normalized = normalizeAppMode(value);
  if (normalized) {
    safeSet(window.sessionStorage, OVERRIDE_STORAGE_KEY, normalized);
    return normalized;
  }
  if (value.trim().toLowerCase() === "auto" || value.trim().toLowerCase() === "reset") {
    safeRemove(window.sessionStorage, OVERRIDE_STORAGE_KEY);
    safeRemove(window.localStorage, APP_MODE_STORAGE_KEY);
  }
  return "";
}

function modeFromValue(value, source) {
  const normalized = normalizeAppMode(value);
  return normalized ? { mode: normalized, source } : null;
}

function modeFromServer() {
  if (typeof window === "undefined") return null;
  return modeFromValue(window.__omdAppMode, "server")
    || modeFromValue(window.__omdAppConfig?.mode, "server");
}

function modeFromNativePreference() {
  if (typeof window === "undefined" || !Capacitor.isNativePlatform()) return null;
  return modeFromValue(safeGet(window.localStorage, APP_MODE_STORAGE_KEY), "native");
}

function modeFromBuild() {
  let value = "";
  try {
    value = import.meta.env?.VITE_APP_MODE || "";
  } catch (_) {}
  if (!value) {
    try {
      // Replaced by Vite define; in dev it is "" when APP_MODE is not set.
      value = typeof __APP_MODE__ !== "undefined" ? __APP_MODE__ : "";
    } catch (_) {}
  }
  return modeFromValue(value, "build");
}

function modeFromHostname() {
  if (typeof window === "undefined") return null;
  const host = String(window.location.hostname || "").toLowerCase();
  if (!host) return null;

  if (host === "onmychat.com" || host.endsWith(".onmychat.com") || host.startsWith("chat.")) {
    return { mode: APP_MODE_CHAT, source: "hostname" };
  }
  if (
    host === "onmydisk.net" ||
    host.endsWith(".onmydisk.net") ||
    host === "onmydisk.com" ||
    host.endsWith(".onmydisk.com") ||
    host.startsWith("files.") ||
    host.startsWith("disk.")
  ) {
    return { mode: APP_MODE_FILES, source: "hostname" };
  }
  return null;
}

/**
 * Resolution order:
 *   1. ?app=...          — dev/QA only (or VITE_ALLOW_MODE_OVERRIDE=true)
 *   2. window.__omdAppMode — injected by the gateway from the request Host
 *   3. native preference — Capacitor only
 *   4. build-time VITE_APP_MODE / __APP_MODE__ — white-label / OTA default
 *   5. hostname heuristic — static dist without a gateway
 *   6. files
 */
export function resolveAppModeInfo() {
  if (cachedResult) return cachedResult;

  if (typeof window !== "undefined") {
    const fromUrl = applyUrlOverride();
    if (fromUrl) {
      cachedResult = { mode: fromUrl, source: "url" };
    }
    if (!cachedResult && devOverrideAllowed()) {
      const fromSession = modeFromValue(safeGet(window.sessionStorage, OVERRIDE_STORAGE_KEY), "url");
      if (fromSession) cachedResult = fromSession;
    }
  }

  cachedResult = cachedResult
    || modeFromServer()
    || modeFromNativePreference()
    || modeFromBuild()
    || modeFromHostname()
    || { mode: APP_MODE_FILES, source: "default" };

  try {
    console.info(`[app-mode] resolved "${cachedResult.mode}" from ${cachedResult.source}`);
  } catch (_) {}

  return cachedResult;
}

export function resolveAppMode() {
  return resolveAppModeInfo().mode;
}

export function getAppModeSource() {
  return resolveAppModeInfo().source;
}

export function setAppModePreference(mode) {
  const normalized = normalizeAppMode(mode);
  if (!normalized || typeof window === "undefined") return false;
  safeSet(window.localStorage, APP_MODE_STORAGE_KEY, normalized);
  cachedResult = null;
  return true;
}

export function clearAppModePreference() {
  if (typeof window === "undefined") return;
  safeRemove(window.localStorage, APP_MODE_STORAGE_KEY);
  safeRemove(window.sessionStorage, OVERRIDE_STORAGE_KEY);
  cachedResult = null;
}

export function canSwitchMainApp() {
  if (typeof window === "undefined") return false;
  return Capacitor.isNativePlatform() && getAppModeSource() !== "url";
}

export function isOnMyChatDomain() {
  if (typeof window === "undefined") return false;
  const host = String(window.location?.hostname || "").toLowerCase();
  return host === "onmychat.com" || host.endsWith(".onmychat.com");
}

export function getDomainBrandTitle() {
  return isOnMyChatDomain() ? "On My Chat" : "On My Disk";
}
