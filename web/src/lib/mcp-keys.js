import { writable } from "svelte/store";
import Gun from "gun/gun";
import "gun/sea";
import { omdKeyStore, getStoredOmdKey, deriveEncryptionKey } from "./omd-key.js";
import { isNativeMobile } from "./platform.js";

/**
 * MCP Application API Keys Vault.
 *
 * Stores API keys and tokens for external apps (e.g. Plane, Grade.app, custom MCP services)
 * E2EE in the user's DID graph:
 *   gun.user().get("keys").get("mcp_apps").get(appId) -> { ciphertext, iv, updatedAt } (AES via omd_key)
 *   Fallback/legacy: Gun.SEA.encrypt(payload, devicePair) -> { enc, updatedAt }
 *
 * LocalStorage mirror:
 *   "omd_mcp_keys:<mainPub>" -> instant availability on reload.
 */

import { getGatewayHubUrl, getActiveGateway } from "./gateway-config.js";

const LS_PREFIX = "omd_mcp_keys";
const GRAPH_ROOT = "keys";
const GRAPH_NODE = "mcp_apps";
const SHARED_GRAPH_PREFIX = "mcp_keys:";
const MCP_SALT = "omd-mcp-keys-salt-v1";

const ALL_PRESET_MCP_APPS = [
  {
    id: "civitai",
    name: "CivitAI",
    url: "https://civitai.com/user/account",
    placeholder: "civitai_api_token...",
    description: "CivitAI Publishing & Models Token",
    badgeColor: "#EC4899",
  },
  {
    id: "plane",
    name: "Plane",
    url: "https://app.plane.so",
    placeholder: "plane_api_...",
    description: "Plane Project Management",
    badgeColor: "#A855F7",
  },
];

export const PRESET_MCP_APPS = ALL_PRESET_MCP_APPS;

export function getActiveMcpPresets(gw = null) {
  if (isNativeMobile()) return [...ALL_PRESET_MCP_APPS];
  const active = gw || getActiveGateway();
  const extra = Array.isArray(active?.mcpPresets) ? active.mcpPresets : [];
  const seen = new Set();
  const merged = [];
  for (const p of [...extra, ...ALL_PRESET_MCP_APPS]) {
    if (!p || !p.id || seen.has(p.id)) continue;
    seen.add(p.id);
    merged.push(p);
  }
  return merged;
}

let identityRef = null;
let boundPub = "";
let unsubGraph = null;
let unsubOmdKey = null;

/** appId -> { appId, name, url, key, updatedAt } */
const cache = new Map();
/** appId -> raw encrypted entry { ciphertext, iv, enc, updatedAt } for deferred decryption when omd_key arrives */
const rawEntries = new Map();
/** appId -> tombstone updatedAt */
const deletedTombstones = new Map();

/** AES-GCM CryptoKey derived from omd_key */
let vaultCrypto = null;
let vaultKeySource = "";

export const mcpKeysStore = writable({ byApp: {}, ready: false });

function publish() {
  const byApp = {};
  for (const [appId, rec] of cache) {
    byApp[appId] = { ...rec };
  }
  mcpKeysStore.set({ byApp, ready: true });
}

function lsKey() {
  return LS_PREFIX + ":" + ((identityRef && identityRef.mainPub) || "anon");
}

function persistMirror() {
  if (typeof window === "undefined") return;
  const obj = {};
  for (const [appId, rec] of cache) obj[appId] = rec;
  const serialized = JSON.stringify(obj);
  const key = lsKey();
  try {
    window.localStorage.setItem(key, serialized);
    return;
  } catch (e) {
    // Evict disposable GunDB / prompt caches if localStorage hit the 5MB browser quota
    try {
      for (const k of Object.keys(window.localStorage)) {
        if (k === "gun/" || k.startsWith("gun/") || k.startsWith("radata") || k === "krea_file_prompts") {
          window.localStorage.removeItem(k);
        }
      }
      window.localStorage.setItem(key, serialized);
      return;
    } catch (e2) {
      try {
        window.sessionStorage.setItem(key, serialized);
      } catch (_) {}
      console.warn("[mcp-keys] local mirror write failed:", e2);
    }
  }
}

function loadLocalMirror() {
  if (typeof window === "undefined") return;
  try {
    const key = lsKey();
    const raw = window.localStorage.getItem(key) || window.sessionStorage.getItem(key);
    if (!raw) return;
    const obj = JSON.parse(raw) || {};
    for (const [appId, rec] of Object.entries(obj)) {
      if (rec && typeof rec === "object" && rec.key) {
        cache.set(appId, {
          appId: rec.appId || appId,
          name: rec.name || appId,
          url: rec.url || "",
          key: String(rec.key),
          updatedAt: rec.updatedAt || Date.now(),
        });
      }
    }
  } catch (e) {
    console.warn("[mcp-keys] local mirror read failed:", e);
  }
}

function uint8ToBase64(u8) {
  let binary = "";
  const len = u8.byteLength;
  for (let i = 0; i < len; i++) {
    binary += String.fromCharCode(u8[i]);
  }
  return btoa(binary);
}

function base64ToUint8(b64) {
  const binary = atob(b64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

async function refreshVaultCrypto() {
  const key = getStoredOmdKey();
  if (!key) {
    vaultCrypto = null;
    vaultKeySource = "";
    return;
  }
  if (vaultKeySource === key && vaultCrypto) return;
  try {
    const cryptoKey = await deriveEncryptionKey(key, MCP_SALT);
    vaultCrypto = cryptoKey;
    vaultKeySource = key;
  } catch (e) {
    console.warn("[mcp-keys] failed to derive vault crypto:", e);
    vaultCrypto = null;
    vaultKeySource = "";
  }
}

async function aesEncrypt(rec) {
  if (!vaultCrypto || typeof crypto === "undefined" || !crypto.subtle) return null;
  try {
    const encoder = new TextEncoder();
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const data = encoder.encode(JSON.stringify(rec));
    const ciphertext = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, vaultCrypto, data);
    return {
      ciphertext: uint8ToBase64(new Uint8Array(ciphertext)),
      iv: uint8ToBase64(iv),
      updatedAt: rec.updatedAt || Date.now(),
    };
  } catch (e) {
    console.warn("[mcp-keys] AES encrypt failed:", e);
    return null;
  }
}

async function aesDecrypt(payload) {
  if (!vaultCrypto || typeof crypto === "undefined" || !crypto.subtle) return null;
  try {
    const iv = base64ToUint8(payload.iv);
    const ciphertext = base64ToUint8(payload.ciphertext);
    const decrypted = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, vaultCrypto, ciphertext);
    const decoded = new TextDecoder().decode(decrypted);
    let parsed;
    try { parsed = JSON.parse(decoded); } catch (e) { parsed = decoded; }
    if (parsed && typeof parsed === "object" && parsed.key) {
      return parsed;
    }
    if (typeof parsed === "string") {
      return { key: parsed };
    }
  } catch (e) {
    console.warn("[mcp-keys] AES decrypt failed:", e);
  }
  return null;
}

async function decryptEntry(entry, appId) {
  if (!entry || typeof entry !== "object") return null;

  // 1. AES payload encrypted via derived omd_key
  if (entry.ciphertext && entry.iv) {
    await refreshVaultCrypto();
    const rec = await aesDecrypt(entry);
    if (rec) {
      return {
        appId: rec.appId || appId,
        name: rec.name || appId,
        url: rec.url || "",
        key: String(rec.key),
        updatedAt: entry.updatedAt || rec.updatedAt || Date.now(),
      };
    }
  }

  // 2. SEA-encrypted fallback with identity devicePair
  if (typeof entry.enc === "string" && identityRef && identityRef.devicePair) {
    try {
      const dec = await Gun.SEA.decrypt(entry.enc, identityRef.devicePair);
      let parsed = dec;
      if (typeof dec === "string") {
        try { parsed = JSON.parse(dec); } catch (e) { parsed = { key: dec }; }
      }
      if (parsed && (parsed.key || typeof parsed === "string")) {
        return {
          appId: parsed.appId || appId,
          name: parsed.name || appId,
          url: parsed.url || "",
          key: String(parsed.key || parsed),
          updatedAt: entry.updatedAt || parsed.updatedAt || Date.now(),
        };
      }
    } catch (e) {
      console.warn("[mcp-keys] SEA decrypt failed for", appId, e);
    }
  }

  return null;
}

function cleanAppId(appId) {
  return String(appId || "").trim().toLowerCase().replace(/[^a-z0-9_-]/g, "_");
}

async function pushEntriesToHub(mainPub, entriesObj) {
  if (!mainPub || !entriesObj || Object.keys(entriesObj).length === 0) return;
  try {
    await fetch(`${getGatewayHubUrl()}/api/mcp-keys`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mainPub, entries: entriesObj }),
    });
  } catch (e) {
    console.warn("[mcp-keys] hub POST /api/mcp-keys failed:", e);
  }
}

async function fetchFromHub(mainPub) {
  if (!mainPub) return;
  try {
    const resp = await fetch(`${getGatewayHubUrl()}/api/mcp-keys?mainPub=${encodeURIComponent(mainPub)}`);
    if (!resp.ok) return;
    const data = await resp.json();
    const keys = data && data.keys && typeof data.keys === "object" ? data.keys : {};
    for (const [appId, entry] of Object.entries(keys)) {
      if (identityRef?.mainPub !== mainPub) return;
      await handleIncomingEntry(entry, appId, { isLegacy: false });
    }
  } catch (e) {
    console.warn("[mcp-keys] hub GET /api/mcp-keys failed:", e);
  }
}

async function handleIncomingEntry(entry, rawAppId, { isLegacy = false } = {}) {
  if (!rawAppId || rawAppId === "_") return;
  const appId = cleanAppId(rawAppId);
  if (!appId) return;

  // Explicit deletion (null in shared DID node, or { deleted: true })
  if (entry === null || (entry && typeof entry === "object" && entry.deleted === true)) {
    if (isLegacy) return; // Never let an empty per-device node wipe shared DID keys
    const delTs = Number(entry?.updatedAt || 0);
    const existing = cache.get(appId);
    if (existing && delTs > 0 && Number(existing.updatedAt || 0) > delTs) {
      return;
    }
    deletedTombstones.set(appId, delTs || Date.now());
    rawEntries.delete(appId);
    if (cache.delete(appId)) {
      publish();
      persistMirror();
    }
    return;
  }

  // GunDB frequently emits intermediate soul-pointer stubs ({ _: { '#': '...' } })
  // before child fields arrive. Ignore stubs rather than deleting cached keys!
  if (!entry || typeof entry !== "object" || (!entry.ciphertext && !entry.enc)) {
    return;
  }

  const incomingTs = Number(entry.updatedAt || 0);
  const tombTs = deletedTombstones.get(appId) || 0;
  if (tombTs && incomingTs && incomingTs <= tombTs) {
    return;
  }

  const prevRaw = rawEntries.get(appId);
  if (!prevRaw || incomingTs >= Number(prevRaw.updatedAt || 0)) {
    rawEntries.set(appId, {
      ciphertext: entry.ciphertext || "",
      iv: entry.iv || "",
      enc: entry.enc || "",
      updatedAt: incomingTs || Date.now(),
    });
  }

  const rec = await decryptEntry(entry, appId);
  if (rec && rec.key) {
    const existing = cache.get(appId);
    if (!existing || Number(rec.updatedAt || 0) >= Number(existing.updatedAt || 0)) {
      cache.set(appId, rec);
      publish();
      persistMirror();
    }
    // Auto-migrate legacy per-device keys into the shared DID vault
    if (isLegacy && identityRef?.mainPub) {
      syncSingleAppToVault(appId, cache.get(appId) || rec).catch(() => {});
    }
  }
}

async function redecryptPendingEntries() {
  if (rawEntries.size === 0) return;
  await refreshVaultCrypto();
  if (!vaultCrypto) return;
  let updated = false;
  for (const [appId, raw] of rawEntries.entries()) {
    const rec = await decryptEntry(raw, appId);
    if (rec && rec.key) {
      const existing = cache.get(appId);
      if (!existing || Number(rec.updatedAt || 0) >= Number(existing.updatedAt || 0)) {
        cache.set(appId, rec);
        updated = true;
      }
    }
  }
  if (updated) {
    publish();
    persistMirror();
  }
}

async function syncSingleAppToVault(appId, rec) {
  if (!identityRef || !identityRef.mainPub || !rec) return;
  await refreshVaultCrypto();
  let payload = await aesEncrypt(rec);
  if (!payload && identityRef.devicePair) {
    try {
      const enc = await Gun.SEA.encrypt(JSON.stringify(rec), identityRef.devicePair);
      if (enc) payload = { enc, updatedAt: rec.updatedAt || Date.now() };
    } catch (_) {}
  }
  if (!payload) return;

  const mainPub = identityRef.mainPub;
  rawEntries.set(appId, payload);
  deletedTombstones.delete(appId);

  if (identityRef.gun) {
    try {
      identityRef.gun.get(SHARED_GRAPH_PREFIX + mainPub).get(appId).put(payload);
    } catch (e) {
      console.warn("[mcp-keys] shared graph put failed for", appId, e);
    }
  }
  await pushEntriesToHub(mainPub, { [appId]: payload });
}

async function syncToGraph() {
  if (!identityRef || !identityRef.mainPub) return;
  await refreshVaultCrypto();
  await redecryptPendingEntries();
  if (cache.size === 0) return;

  const mainPub = identityRef.mainPub;
  const sharedNode = identityRef.gun ? identityRef.gun.get(SHARED_GRAPH_PREFIX + mainPub) : null;
  const hubBatch = {};

  for (const [appId, rec] of cache) {
    let payload = await aesEncrypt(rec);
    if (!payload && identityRef.devicePair) {
      try {
        const enc = await Gun.SEA.encrypt(JSON.stringify(rec), identityRef.devicePair);
        if (enc) payload = { enc, updatedAt: rec.updatedAt || Date.now() };
      } catch (_) {}
    }
    if (payload) {
      hubBatch[String(appId)] = payload;
      if (sharedNode) {
        try {
          sharedNode.get(String(appId)).put(payload);
        } catch (e) {
          console.warn("[mcp-keys] graph put failed for", appId, e);
        }
      }
    }
  }

  if (Object.keys(hubBatch).length > 0) {
    await pushEntriesToHub(mainPub, hubBatch);
  }
}

function subscribeGraph() {
  const gun = identityRef && identityRef.gun;
  const mainPub = identityRef && identityRef.mainPub;
  if (!gun || !mainPub) return;

  const handles = [];
  try {
    // 1. Primary DID-scoped E2EE vault shared across all browsers & mobile
    const sharedHandle = gun.get(SHARED_GRAPH_PREFIX + mainPub).map().on((entry, appId) => {
      handleIncomingEntry(entry, appId, { isLegacy: false });
    });
    handles.push(sharedHandle);

    // 2. Read-only legacy migration from per-device ~<devicePub>/keys/mcp_apps
    if (gun.user && gun.user().is) {
      const legacyHandle = gun.user().get(GRAPH_ROOT).get(GRAPH_NODE).map().on((entry, appId) => {
        handleIncomingEntry(entry, appId, { isLegacy: true });
      });
      handles.push(legacyHandle);
    }

    unsubGraph = () => {
      for (const h of handles) {
        try { if (h && typeof h.off === "function") h.off(); } catch (_) {}
      }
    };
  } catch (e) {
    console.warn("[mcp-keys] graph subscription failed:", e);
  }
}

function detach() {
  if (unsubGraph) {
    try { unsubGraph(); } catch (e) { /* ignore */ }
    unsubGraph = null;
  }
  if (unsubOmdKey) {
    try { unsubOmdKey(); } catch (e) { /* ignore */ }
    unsubOmdKey = null;
  }
}

export function setMcpKeysIdentity(identity) {
  if (!identity || identity.type !== "persistent" || (!identity.mainPub && !identity.devicePair)) return;
  const pub = identity.mainPub || identity.devicePair?.pub || "";
  if (identityRef === identity && pub === boundPub) return;

  detach();
  identityRef = identity;
  boundPub = pub;
  cache.clear();
  rawEntries.clear();
  deletedTombstones.clear();
  loadLocalMirror();
  publish();
  subscribeGraph();

  if (identity.mainPub) {
    fetchFromHub(identity.mainPub);
  }

  refreshVaultCrypto().then(() => syncToGraph());
  unsubOmdKey = omdKeyStore.subscribe(() => {
    refreshVaultCrypto().then(() => syncToGraph());
  });
}

export function clearMcpKeysIdentity() {
  detach();
  identityRef = null;
  boundPub = "";
  vaultCrypto = null;
  vaultKeySource = "";
  cache.clear();
  rawEntries.clear();
  deletedTombstones.clear();
  loadLocalMirror();
  publish();
}

/**
 * Save or update an MCP application key in DID graph + Hub vault + local mirror.
 */
export async function saveMcpAppKey(appId, meta = {}) {
  const cleanId = cleanAppId(appId);
  if (!cleanId) throw new Error("Invalid application ID");

  const keyVal = String(meta.key || "").trim();
  if (!keyVal) throw new Error("API key cannot be empty");

  const rec = {
    appId: cleanId,
    name: meta.name ? String(meta.name).trim() : cleanId,
    url: meta.url ? String(meta.url).trim() : "",
    key: keyVal,
    updatedAt: Date.now(),
  };

  deletedTombstones.delete(cleanId);
  cache.set(cleanId, rec);
  publish();
  persistMirror();

  if (identityRef && identityRef.mainPub) {
    await syncSingleAppToVault(cleanId, rec);
  }

  return rec;
}

/**
 * Remove an MCP application key from DID graph + Hub vault + local mirror.
 */
export async function removeMcpAppKey(appId) {
  const cleanId = cleanAppId(appId);
  if (!cleanId) return;

  const tombTs = Date.now();
  deletedTombstones.set(cleanId, tombTs);
  rawEntries.delete(cleanId);
  cache.delete(cleanId);
  publish();
  persistMirror();

  const mainPub = identityRef && identityRef.mainPub;
  if (identityRef && identityRef.gun && mainPub) {
    try {
      identityRef.gun.get(SHARED_GRAPH_PREFIX + mainPub).get(cleanId).put({
        deleted: true,
        ciphertext: "",
        iv: "",
        updatedAt: tombTs,
      });
      if (identityRef.gun.user && identityRef.gun.user().is) {
        identityRef.gun.user().get(GRAPH_ROOT).get(GRAPH_NODE).get(cleanId).put(null);
      }
    } catch (e) {
      console.warn("[mcp-keys] graph remove failed:", e);
    }
  }
  if (mainPub) {
    try {
      await fetch(`${getGatewayHubUrl()}/api/mcp-keys`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mainPub, appId: cleanId, deleted: true, updatedAt: tombTs }),
      });
    } catch (e) {
      console.warn("[mcp-keys] hub remove failed:", e);
    }
  }
}

/**
 * Retrieve decrypted key string for a given appId.
 */
export function getStoredMcpAppKey(appId) {
  if (!appId) return "";
  const cleanId = String(appId).trim().toLowerCase().replace(/[^a-z0-9_-]/g, "_");
  const rec = cache.get(cleanId);
  return rec ? rec.key : "";
}

/**
 * Retrieve complete app record { appId, name, url, key, updatedAt }.
 */
export function getStoredMcpApp(appId) {
  if (!appId) return null;
  const cleanId = String(appId).trim().toLowerCase().replace(/[^a-z0-9_-]/g, "_");
  return cache.get(cleanId) ? { ...cache.get(cleanId) } : null;
}

/**
 * Retrieve all app records as an array.
 */
export function getAllStoredMcpApps() {
  return Array.from(cache.values()).map((r) => ({ ...r }));
}

/**
 * Retrieve map of keys and URLs for backend requests:
 * { "plane": "...", "plane_url": "...", "grade": "..." }
 */
export function getMcpPayload() {
  const res = {};
  for (const [id, rec] of cache) {
    if (rec.key) {
      res[id] = rec.key;
      if (rec.url) res[`${id}_url`] = rec.url;
      if (rec.name) res[`${id}_name`] = rec.name;
    }
  }
  return res;
}

// Global exposure for MCP tools and external helpers
if (typeof window !== "undefined") {
  loadLocalMirror();
  publish();

  window.__omdGetMcpKey = getStoredMcpAppKey;
  window.__omdGetMcpApp = getStoredMcpApp;
  window.__omdGetAllMcpKeys = () => {
    const res = {};
    for (const [id, rec] of cache) res[id] = rec.key;
    return res;
  };
  window.__omdGetMcpPayload = getMcpPayload;
}
