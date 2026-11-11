import { writable } from "svelte/store";
import { Capacitor, CapacitorHttp } from "@capacitor/core";
import { APP_MODE_CHAT, resolveAppMode } from "./app-mode.js";

export const DEFAULT_GATEWAY_ORIGIN = "https://disk.direct";
const ACTIVE_GATEWAY_STORAGE_KEY = "omd_active_gateway_v1";
const ACCEPTED_TOS_STORAGE_KEY = "omd_gateway_tos_accepted_v1";

const SAFE_COLOR_RE = /^(#[0-9a-fA-F]{3,8}|rgba?\(\s*\d{1,3}\s*,\s*\d{1,3}\s*,\s*\d{1,3}(?:\s*,\s*(?:0|1|0?\.\d+))?\s*\))$/;

const ALLOWED_CUSTOM_CSS_VARS = [
  "--accent",
  "--accent-hover",
  "--accent-active",
  "--accent-soft",
  "--bg-primary",
  "--bg-secondary",
  "--bg-card",
  "--sidebar-bg",
  "--border-color",
  "--wx-color-primary",
  "--wx-color-primary-hover",
  "--wx-color-primary-border",
  "--top-menu-color",
  "--top-menu-hover-color",
];

export const DEFAULT_GATEWAY = Object.freeze({
  schema: 1,
  id: "disk.direct",
  brandName: "Disk Direct",
  shortName: "Disk Direct",
  origin: "https://disk.direct",
  host: "disk.direct",
  relayWsUrl: "wss://disk.direct:8765/gun",
  hubUrl: "https://disk.direct:8765",
  turnHost: "disk.direct",
  oauthBridgeOrigin: "https://disk.direct",
  tosUrl: "https://disk.direct/tos",
  privacyUrl: "https://disk.direct/tos#gdpr",
  tosVersion: "2026-11",
  isCustom: false,
  skin: Object.freeze({
    preset: "cool-iris",
    logoDark: "/img/logo.svg",
    logoLight: "/img/logo-dark.svg",
    favicon: "/customtheme/onmydisk.svg",
    tokens: {},
  }),
});

function isLocalBrowserHost() {
  if (typeof window === "undefined") return true;
  if (Capacitor.isNativePlatform()) return true;
  const h = String(window.location.hostname || "").toLowerCase();
  return !h || h === "localhost" || h === "127.0.0.1" || h === "[::1]";
}

function isCommunityOmdHost(host) {
  const h = String(host || "").toLowerCase();
  return h === "onmydisk.net" || h.endsWith(".onmydisk.net") || h === "onmydisk.com" || h.endsWith(".onmydisk.com");
}

function sanitizeColor(val, fallback = "") {
  if (typeof val !== "string") return fallback;
  const trimmed = val.trim();
  return SAFE_COLOR_RE.test(trimmed) ? trimmed : fallback;
}

function sanitizeUrl(val, baseOrigin = DEFAULT_GATEWAY_ORIGIN, fallback = "") {
  if (typeof val !== "string" || !val.trim()) return fallback;
  const raw = val.trim();
  try {
    if (raw.startsWith("/")) {
      return isLocalBrowserHost() ? `${baseOrigin}${raw}` : raw;
    }
    const u = new URL(raw, baseOrigin);
    if (u.protocol === "https:" || u.protocol === "http:" || u.protocol === "wss:" || u.protocol === "ws:") {
      return u.toString().replace(/\/$/, raw.endsWith("/") && u.pathname !== "/" ? "/" : "");
    }
  } catch (_) {}
  return fallback;
}

export function normalizeGatewayInputUrl(rawInput) {
  if (typeof rawInput !== "string") return "";
  let raw = rawInput.trim();
  if (!raw) return "";
  if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:\/\//.test(raw)) {
    const isLocalIp = /^(localhost|127\.0\.0\.1|\[::1\])(?::\d+)?(\/|$)/i.test(raw);
    raw = `${isLocalIp ? "http" : "https"}://${raw}`;
  }
  try {
    const u = new URL(raw);
    if (u.protocol !== "https:" && u.protocol !== "http:") return "";
    return u.origin;
  } catch (_) {
    return "";
  }
}

export function buildManifestForOrigin(origin, rawManifest = null) {
  const cleanOrigin = normalizeGatewayInputUrl(origin) || DEFAULT_GATEWAY_ORIGIN;
  let parsedUrl;
  try {
    parsedUrl = new URL(cleanOrigin);
  } catch (_) {
    parsedUrl = new URL(DEFAULT_GATEWAY_ORIGIN);
  }

  const host = parsedUrl.hostname.toLowerCase();
  const isHttps = parsedUrl.protocol === "https:";
  const wsProto = isHttps ? "wss:" : "ws:";
  const httpProto = isHttps ? "https:" : "http:";

  if (cleanOrigin === DEFAULT_GATEWAY_ORIGIN && (!rawManifest || rawManifest.id === "disk.direct")) {
    return {
      ...DEFAULT_GATEWAY,
      skin: {
        ...DEFAULT_GATEWAY.skin,
        logoDark: isLocalBrowserHost() ? `${DEFAULT_GATEWAY_ORIGIN}/img/logo.svg` : "/img/logo.svg",
        logoLight: isLocalBrowserHost() ? `${DEFAULT_GATEWAY_ORIGIN}/img/logo-dark.svg` : "/img/logo-dark.svg",
      },
    };
  }

  const isOmd = isCommunityOmdHost(host) || rawManifest?.skin?.preset === "omd-classic";
  const defaultBrand = isOmd
    ? "On My Disk"
    : host === "disk.direct" || host.endsWith(".disk.direct")
      ? "Disk Direct"
      : rawManifest?.brandName || host;

  const presetRaw = String(rawManifest?.skin?.preset || (isOmd ? "omd-classic" : "custom")).toLowerCase();
  const preset = ["cool-iris", "omd-classic", "custom"].includes(presetRaw)
    ? presetRaw
    : (isOmd ? "omd-classic" : "cool-iris");

  const defaultLogoDark = `${cleanOrigin}/img/logo.svg`;
  const defaultLogoLight = `${cleanOrigin}/img/logo-dark.svg`;

  const rawTokens = (rawManifest?.skin?.tokens && typeof rawManifest.skin.tokens === "object")
    ? rawManifest.skin.tokens
    : {};
  const cleanTokens = {};
  for (const [k, v] of Object.entries(rawTokens)) {
    const safe = sanitizeColor(v);
    if (safe) cleanTokens[k] = safe;
  }

  return {
    schema: 1,
    id: String(rawManifest?.id || host).slice(0, 80),
    brandName: String(rawManifest?.brandName || defaultBrand).slice(0, 64),
    shortName: String(rawManifest?.shortName || rawManifest?.brandName || defaultBrand).slice(0, 32),
    origin: cleanOrigin,
    host,
    relayWsUrl: sanitizeUrl(rawManifest?.relayWsUrl, cleanOrigin, `${wsProto}//${host}:8765/gun`),
    hubUrl: sanitizeUrl(rawManifest?.hubUrl, cleanOrigin, `${httpProto}//${host}:8765`),
    turnHost: String(rawManifest?.turnHost || host).replace(/[^a-zA-Z0-9.-]/g, "") || host,
    oauthBridgeOrigin: sanitizeUrl(rawManifest?.oauthBridgeOrigin, cleanOrigin, DEFAULT_GATEWAY_ORIGIN),
    tosUrl: sanitizeUrl(rawManifest?.tosUrl, cleanOrigin, `${cleanOrigin}/tos`),
    privacyUrl: sanitizeUrl(rawManifest?.privacyUrl, cleanOrigin, `${cleanOrigin}/tos#gdpr`),
    tosVersion: String(rawManifest?.tosVersion || "1.0").slice(0, 32),
    isCustom: cleanOrigin !== DEFAULT_GATEWAY_ORIGIN,
    skin: {
      preset,
      logoDark: sanitizeUrl(rawManifest?.skin?.logoDark, cleanOrigin, defaultLogoDark),
      logoLight: sanitizeUrl(rawManifest?.skin?.logoLight, cleanOrigin, defaultLogoLight),
      favicon: sanitizeUrl(rawManifest?.skin?.favicon, cleanOrigin, `${cleanOrigin}/favicon.ico`),
      tokens: cleanTokens,
    },
  };
}

function loadInitialGateway() {
  if (typeof window === "undefined") return DEFAULT_GATEWAY;

  // Never override chat mode domains (aiassistant.direct)
  if (resolveAppMode() === APP_MODE_CHAT) {
    return DEFAULT_GATEWAY;
  }

  try {
    const raw = window.localStorage.getItem(ACTIVE_GATEWAY_STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      if (parsed && typeof parsed.origin === "string" && parsed.origin.trim()) {
        return buildManifestForOrigin(parsed.origin, parsed);
      }
    }
  } catch (_) {}

  // In a regular web browser on a non-local domain (e.g. direct.onmydisk.net or enterprise web host),
  // derive the initial gateway from window.location.origin automatically.
  if (!isLocalBrowserHost()) {
    const browserOrigin = window.location.origin;
    if (browserOrigin && browserOrigin !== DEFAULT_GATEWAY_ORIGIN) {
      return buildManifestForOrigin(browserOrigin, null);
    }
  }

  return buildManifestForOrigin(DEFAULT_GATEWAY_ORIGIN, DEFAULT_GATEWAY);
}

let _currentGateway = loadInitialGateway();
export const gatewayConfig = writable(_currentGateway);

gatewayConfig.subscribe((val) => {
  if (val) _currentGateway = val;
});

export function getActiveGateway() {
  return _currentGateway || DEFAULT_GATEWAY;
}

export function getGatewayOrigin() {
  if (typeof window !== "undefined" && resolveAppMode() === APP_MODE_CHAT && !isLocalBrowserHost()) {
    return window.location.origin;
  }
  return getActiveGateway().origin || DEFAULT_GATEWAY_ORIGIN;
}

export function getGatewayHost() {
  if (typeof window !== "undefined" && resolveAppMode() === APP_MODE_CHAT && !isLocalBrowserHost()) {
    return window.location.hostname || "disk.direct";
  }
  return getActiveGateway().host || "disk.direct";
}

export function getGatewayRelayWsUrl() {
  return getActiveGateway().relayWsUrl || DEFAULT_GATEWAY.relayWsUrl;
}

export function getGatewayHubUrl() {
  return getActiveGateway().hubUrl || DEFAULT_GATEWAY.hubUrl;
}

export function getGatewayTurnHost() {
  return getActiveGateway().turnHost || "disk.direct";
}

export function getGatewayOauthBridgeOrigin() {
  return getActiveGateway().oauthBridgeOrigin || DEFAULT_GATEWAY_ORIGIN;
}

export function getGatewayTosUrl() {
  if (resolveAppMode() === APP_MODE_CHAT) {
    return isLocalBrowserHost() ? "https://aiassistant.direct/tos" : "/tos";
  }
  const gw = getActiveGateway();
  if (!isLocalBrowserHost() && !gw.isCustom && gw.origin === window.location.origin) {
    return "/tos";
  }
  return gw.tosUrl || `${gw.origin}/tos`;
}

export function getGatewayPrivacyUrl() {
  if (resolveAppMode() === APP_MODE_CHAT) {
    return isLocalBrowserHost() ? "https://aiassistant.direct/tos#gdpr" : "/tos#gdpr";
  }
  const gw = getActiveGateway();
  if (!isLocalBrowserHost() && !gw.isCustom && gw.origin === window.location.origin) {
    return "/tos#gdpr";
  }
  return gw.privacyUrl || `${gw.origin}/tos#gdpr`;
}

function readAcceptedTosMap() {
  if (typeof window === "undefined") return {};
  try {
    const raw = window.localStorage.getItem(ACCEPTED_TOS_STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : {};
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch (_) {
    return {};
  }
}

export function isGatewayTosAccepted(manifest) {
  if (!manifest || !manifest.origin) return false;
  if (manifest.origin === DEFAULT_GATEWAY_ORIGIN) return true;
  const map = readAcceptedTosMap();
  const entry = map[manifest.origin];
  if (!entry) return false;
  return entry.version === (manifest.tosVersion || "1.0");
}

export function acceptGatewayTos(manifest) {
  if (typeof window === "undefined" || !manifest?.origin) return;
  try {
    const map = readAcceptedTosMap();
    map[manifest.origin] = {
      version: manifest.tosVersion || "1.0",
      acceptedAt: Date.now(),
      brandName: manifest.brandName || manifest.host || "",
    };
    window.localStorage.setItem(ACCEPTED_TOS_STORAGE_KEY, JSON.stringify(map));
  } catch (_) {}
}

async function httpGetJsonOrProbe(url, timeoutMs = 8000) {
  if (typeof window !== "undefined" && Capacitor.isNativePlatform() && CapacitorHttp?.get) {
    const res = await CapacitorHttp.get({
      url,
      connectTimeout: timeoutMs,
      readTimeout: timeoutMs,
      headers: { Accept: "application/json, text/html;q=0.8, */*;q=0.5" },
    });
    return {
      status: Number(res?.status || 0),
      data: res?.data,
    };
  }

  const ctrl = typeof AbortController !== "undefined" ? new AbortController() : null;
  const timer = ctrl ? setTimeout(() => ctrl.abort(), timeoutMs) : null;
  try {
    const res = await fetch(url, {
      method: "GET",
      mode: "cors",
      cache: "no-cache",
      signal: ctrl?.signal,
      headers: { Accept: "application/json" },
    });
    let data = null;
    const ct = String(res.headers.get("content-type") || "").toLowerCase();
    if (res.ok && (ct.includes("json") || url.endsWith(".json"))) {
      try {
        data = await res.json();
      } catch (_) {}
    }
    return { status: res.status, data };
  } finally {
    if (timer) clearTimeout(timer);
  }
}

/**
 * Fetches `/.well-known/direct-gateway.json` from the target gateway URL.
 * Falls back to a synthesized manifest if the gateway is alive (responds to
 * `/manifest.json` or `/tos`) so legacy/self-hosted OMD nodes also work cleanly.
 */
export async function fetchGatewayManifest(inputUrl) {
  const origin = normalizeGatewayInputUrl(inputUrl);
  if (!origin) {
    throw new Error("Invalid gateway URL");
  }

  if (origin === DEFAULT_GATEWAY_ORIGIN) {
    return buildManifestForOrigin(DEFAULT_GATEWAY_ORIGIN, DEFAULT_GATEWAY);
  }

  const manifestUrl = `${origin}/.well-known/direct-gateway.json`;
  try {
    const res = await httpGetJsonOrProbe(manifestUrl, 8000);
    if (res.status >= 200 && res.status < 300 && res.data) {
      const parsed = typeof res.data === "string" ? JSON.parse(res.data) : res.data;
      if (parsed && typeof parsed === "object") {
        return buildManifestForOrigin(origin, parsed);
      }
    }
  } catch (_) {
    // Fall through to reachability probe below
  }

  // Fallback reachability probe for nodes without /.well-known/direct-gateway.json
  let reachable = false;
  try {
    const probe = await httpGetJsonOrProbe(`${origin}/manifest.json`, 6000);
    if (probe.status >= 200 && probe.status < 500) {
      reachable = true;
      const pData = typeof probe.data === "string" ? (() => { try { return JSON.parse(probe.data); } catch (_) { return null; } })() : probe.data;
      if (pData && typeof pData === "object" && pData.name) {
        return buildManifestForOrigin(origin, {
          brandName: pData.name,
          shortName: pData.short_name || pData.name,
        });
      }
    }
  } catch (_) {}

  if (!reachable && typeof window !== "undefined" && !Capacitor.isNativePlatform()) {
    // Browser no-cors reachability check when target node hasn't added CORS on /manifest.json yet
    try {
      await fetch(`${origin}/favicon.ico`, { method: "GET", mode: "no-cors", cache: "no-cache" });
      reachable = true;
    } catch (_) {}
  }

  if (!reachable) {
    throw new Error(`Cannot reach gateway at ${origin}`);
  }

  return buildManifestForOrigin(origin, null);
}

/**
 * Applies the active gateway's CSS skin classes & whitelisted custom tokens to `<html>`.
 * Strictly no-ops in chat mode (`.skin-chat` / `aiassistant.direct`).
 */
export function applyGatewaySkinToDom(gateway = getActiveGateway()) {
  if (typeof document === "undefined") return;
  const root = document.documentElement;
  if (!root) return;

  // NEVER modify chat mode theme or assets!
  if (resolveAppMode() === APP_MODE_CHAT || root.classList.contains("skin-chat")) {
    return;
  }

  const preset = gateway?.skin?.preset || "cool-iris";
  root.classList.toggle("gateway-skin-omd", preset === "omd-classic");
  root.classList.toggle("gateway-skin-custom", preset === "custom");

  // Clear any previously injected custom gateway CSS variables
  for (const cssVar of ALLOWED_CUSTOM_CSS_VARS) {
    root.style.removeProperty(cssVar);
  }

  if (preset === "custom" && gateway?.skin?.tokens) {
    const t = gateway.skin.tokens;
    for (const cssVar of ALLOWED_CUSTOM_CSS_VARS) {
      const val = sanitizeColor(t[cssVar]);
      if (val) {
        root.style.setProperty(cssVar, val);
      }
    }
    const accent = sanitizeColor(t.accent || t["--accent"]);
    const accentHover = sanitizeColor(t.accentHover || t["--accent-hover"]) || accent;
    if (accent) {
      root.style.setProperty("--accent", accent);
      root.style.setProperty("--wx-color-primary", accent);
      root.style.setProperty("--wx-color-primary-border", accent);
      root.style.setProperty("--top-menu-color", accent);
    }
    if (accentHover) {
      root.style.setProperty("--accent-hover", accentHover);
      root.style.setProperty("--wx-color-primary-hover", accentHover);
      root.style.setProperty("--top-menu-hover-color", accentHover);
    }
  }
}

/**
 * Returns icon colors and Filemanager inline CSS variables matching the active gateway skin.
 */
export function getGatewayBrandPalette(gateway = getActiveGateway(), isDarkTheme = true) {
  const preset = gateway?.skin?.preset || "cool-iris";

  if (preset === "omd-classic") {
    return {
      appIconColor: isDarkTheme ? "#37A9EF" : "#184562",
      brandColor: isDarkTheme ? "#2996CC" : "#184562",
      brandHoverColor: isDarkTheme ? "#37A9EF" : "#2996CC",
      fmInlineStyle: isDarkTheme
        ? "--top-menu-color: #2996CC; --top-menu-hover-color: #37A9EF; --wx-color-primary: #2996CC; --wx-color-primary-border: #37A9EF; --wx-color-primary-hover: #37A9EF; --wx-color-primary-selected: rgba(41, 150, 204, 0.25); --wx-fm-select-background: rgba(41, 150, 204, 0.25); --wx-background: #202A2F; --wx-background-alt: #18242B; --wx-background-hover: #29363D; --wx-fm-background: #101C22; --bg-secondary: #202A2F; --bg-primary: #101C22; --border-color: #3D4F57; --wx-border: 1px solid #3D4F57; --selected-text: #D6EEFA; --sidebar-shadow: 4px 0 16px rgba(0, 0, 0, 0.25);"
        : "--top-menu-color: #184562; --top-menu-hover-color: #2996CC; --wx-color-primary: #184562; --wx-color-primary-border: #184562; --wx-color-primary-hover: #2996CC; --wx-color-primary-selected: #E4F1FA; --wx-fm-select-background: #E4F1FA; --wx-background: #FFFFFF; --wx-background-alt: #F4F8FA; --wx-background-hover: #ECF0F2; --wx-fm-background: #F6F8F9; --bg-secondary: #FFFFFF; --bg-primary: #F6F8F9; --border-color: #D6DDE1; --wx-border: 1px solid #D6DDE1; --selected-text: #184562; --sidebar-shadow: 3px 0 14px rgba(0, 0, 0, 0.05);",
    };
  }

  if (preset === "custom" && gateway?.skin?.tokens) {
    const t = gateway.skin.tokens;
    const darkAcc = sanitizeColor(t.accentDark || t.accent || t["--accent"], "#7B86C4");
    const darkHover = sanitizeColor(t.accentHoverDark || t.accentHover || t["--accent-hover"], darkAcc);
    const lightAcc = sanitizeColor(t.accentLight || t.accent || t["--accent"], "#46508A");
    const lightHover = sanitizeColor(t.accentHoverLight || t.accentHover || t["--accent-hover"], lightAcc);
    const c = isDarkTheme ? darkAcc : lightAcc;
    const ch = isDarkTheme ? darkHover : lightHover;
    return {
      appIconColor: c,
      brandColor: c,
      brandHoverColor: ch,
      fmInlineStyle: isDarkTheme
        ? `--top-menu-color: ${c}; --top-menu-hover-color: ${ch}; --wx-color-primary: ${c}; --wx-color-primary-border: ${c}; --wx-color-primary-hover: ${ch}; --wx-color-primary-selected: color-mix(in srgb, ${c} 22%, transparent); --wx-fm-select-background: color-mix(in srgb, ${c} 22%, transparent); --wx-background: #1F2430; --wx-background-alt: #191D28; --wx-background-hover: #282E3D; --wx-fm-background: #151821; --bg-secondary: #1F2430; --bg-primary: #151821; --border-color: #32394B; --wx-border: 1px solid #32394B; --selected-text: #EEF0F6; --sidebar-shadow: 4px 0 16px rgba(0, 0, 0, 0.25);`
        : `--top-menu-color: ${c}; --top-menu-hover-color: ${ch}; --wx-color-primary: ${c}; --wx-color-primary-border: ${c}; --wx-color-primary-hover: ${ch}; --wx-color-primary-selected: color-mix(in srgb, ${c} 14%, #FFFFFF); --wx-fm-select-background: color-mix(in srgb, ${c} 14%, #FFFFFF); --wx-background: #FFFFFF; --wx-background-alt: #F1F3F9; --wx-background-hover: #E9ECF5; --wx-fm-background: #F6F7FA; --bg-secondary: #FFFFFF; --bg-primary: #F6F7FA; --border-color: #D8DCE8; --wx-border: 1px solid #D8DCE8; --selected-text: ${c}; --sidebar-shadow: 3px 0 14px rgba(22, 25, 37, 0.05);`,
    };
  }

  // Default: Disk Direct "cool-iris" (Sketch #1)
  return {
    appIconColor: isDarkTheme ? "#8691CC" : "#46508A",
    brandColor: isDarkTheme ? "#7B86C4" : "#46508A",
    brandHoverColor: isDarkTheme ? "#929CD4" : "#5863A6",
    fmInlineStyle: isDarkTheme
      ? "--top-menu-color: #7B86C4; --top-menu-hover-color: #929CD4; --wx-color-primary: #5863A6; --wx-color-primary-border: #6672B8; --wx-color-primary-hover: #6874BA; --wx-color-primary-selected: rgba(123, 134, 196, 0.18); --wx-fm-select-background: rgba(123, 134, 196, 0.18); --wx-background: #1F2430; --wx-background-alt: #191D28; --wx-background-hover: #282E3D; --wx-fm-background: #151821; --bg-secondary: #1F2430; --bg-primary: #151821; --border-color: #32394B; --wx-border: 1px solid #32394B; --selected-text: #DCE0F5; --sidebar-shadow: 4px 0 16px rgba(0, 0, 0, 0.25);"
      : "--top-menu-color: #46508A; --top-menu-hover-color: #5863A6; --wx-color-primary: #3D467A; --wx-color-primary-border: #3D467A; --wx-color-primary-hover: #5863A6; --wx-color-primary-selected: #E9ECF7; --wx-fm-select-background: #E9ECF7; --wx-background: #FFFFFF; --wx-background-alt: #F1F3F9; --wx-background-hover: #E9ECF5; --wx-fm-background: #F6F7FA; --bg-secondary: #FFFFFF; --bg-primary: #F6F7FA; --border-color: #D8DCE8; --wx-border: 1px solid #D8DCE8; --selected-text: #343C6B; --sidebar-shadow: 3px 0 14px rgba(22, 25, 37, 0.05);",
  };
}

export function activateGateway(manifest) {
  const normalized = buildManifestForOrigin(manifest?.origin || DEFAULT_GATEWAY_ORIGIN, manifest);
  if (typeof window !== "undefined") {
    try {
      if (normalized.origin === DEFAULT_GATEWAY_ORIGIN && !normalized.isCustom) {
        window.localStorage.removeItem(ACTIVE_GATEWAY_STORAGE_KEY);
      } else {
        window.localStorage.setItem(ACTIVE_GATEWAY_STORAGE_KEY, JSON.stringify(normalized));
      }
    } catch (_) {}
  }
  _currentGateway = normalized;
  gatewayConfig.set(normalized);
  applyGatewaySkinToDom(normalized);
  if (typeof window !== "undefined") {
    window.dispatchEvent(new CustomEvent("omd-gateway-changed", { detail: normalized }));
  }
  return normalized;
}

export function resetGatewayToDefault() {
  if (typeof window !== "undefined") {
    try {
      window.localStorage.removeItem(ACTIVE_GATEWAY_STORAGE_KEY);
    } catch (_) {}
  }
  const def = buildManifestForOrigin(DEFAULT_GATEWAY_ORIGIN, DEFAULT_GATEWAY);
  _currentGateway = def;
  gatewayConfig.set(def);
  applyGatewaySkinToDom(def);
  if (typeof window !== "undefined") {
    window.dispatchEvent(new CustomEvent("omd-gateway-changed", { detail: def }));
  }
  return def;
}

// Apply persisted gateway skin immediately on module load to prevent theme flash
if (typeof document !== "undefined") {
  applyGatewaySkinToDom(_currentGateway);
}
