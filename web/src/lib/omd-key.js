import { writable } from "svelte/store";
import Gun from "gun/gun";
import "gun/sea";

/**
 * OnMyDisk API key (omd_key) vault.
 *
 * For persistent identities the key lives in the user's DID graph,
 * E2EE with the browser's SEA pair: gun.user().get("keys")/omd_key → { enc }.
 * The relay never sees plaintext. A legacy plaintext localStorage slot is
 * supported only for guests and one-time import into the vault (purged after).
 * Fallback source: window.__omdProfile.omd_key (legacy profile from server).
 */

const OMD_KEY_STORAGE = "omd_key";
const GRAPH_NODE = "keys";
const GRAPH_LEAF = "omd_key";
const CHAT_SALT = "omd-chat-salt-v1";
const PBKDF2_ITERATIONS = 100000;
const OMD_KEY_RE = /^[0-9a-f]{32}$/;

export function generateOmdKey() {
  if (typeof crypto !== "undefined" && crypto.getRandomValues) {
    const bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
  }
  let res = "";
  for (let i = 0; i < 32; i++) {
    res += Math.floor(Math.random() * 16).toString(16);
  }
  return res;
}

export const omdKeyStore = writable({ key: "", source: "none", valid: false });

let identityRef = null;
let boundPub = "";
let unsubGraph = null;

/** Key decrypted from the DID graph (persistent identities). */
let vaultKey = "";
/** Legacy/local-session key: guests, or plaintext awaiting vault import. */
let localKey = "";

function normalizeCandidate(key) {
  return typeof key === "string" && OMD_KEY_RE.test(key.trim().toLowerCase())
    ? key.trim().toLowerCase()
    : "";
}

function profileKey() {
  if (typeof window === "undefined") return "";
  try {
    return normalizeCandidate(window.__omdProfile && window.__omdProfile.omd_key);
  } catch (e) {
    return "";
  }
}

export function readLegacyStorage() {
  if (typeof window === "undefined") return "";
  try {
    const mainPub = identityRef && identityRef.mainPub;
    if (mainPub) {
      const perUser = normalizeCandidate(window.localStorage.getItem(`${OMD_KEY_STORAGE}:${mainPub}`));
      if (perUser) return perUser;
      return "";
    }
    return normalizeCandidate(window.localStorage.getItem(OMD_KEY_STORAGE));
  } catch (e) {
    return "";
  }
}

function publish() {
  const key = vaultKey || localKey || profileKey();
  const source = vaultKey ? "vault" : localKey ? "local" : profileKey() ? "profile" : "none";
  omdKeyStore.set({
    key,
    source,
    valid: !!key,
  });
}

/** Currently active key (vault → local session → legacy profile). */
export function getStoredOmdKey() {
  return vaultKey || localKey || profileKey();
}

export function isValidOmdKey(key) {
  return typeof key === "string" && OMD_KEY_RE.test(key.trim().toLowerCase());
}

export function isPublicSharedRoute() {
  if (typeof window === "undefined") return false;
  return !!(window.isSharedRoute || window.location.pathname.startsWith("/shared/"));
}

/**
 * Set the key. With a persistent identity bound it goes straight into the
 * E2EE DID graph; without one it stays in the local session (guest mode).
 * Returns the normalized key synchronously; graph persistence is async.
 */
export function storeOmdKey(key) {
  const normalized = normalizeCandidate(key);
  if (!normalized) {
    throw new Error("Invalid omd_key: expected 32 hex characters");
  }
  localKey = normalized;
  // Always maintain persistent local cache so on page reload it is immediately available
  if (typeof window !== "undefined" && !isPublicSharedRoute()) {
    try {
      window.localStorage.setItem(OMD_KEY_STORAGE, normalized);
      if (identityRef && identityRef.mainPub) {
        window.localStorage.setItem(`${OMD_KEY_STORAGE}:${identityRef.mainPub}`, normalized);
      }
    } catch (e) { /* ignore */ }
  }

  if (identityRef) {
    vaultKey = normalized;
    persistToGraph(normalized);
  }
  publish();
  return normalized;
}

/** Remove the key from the active source(s). */
export function clearOmdKey() {
  vaultKey = "";
  localKey = "";
  if (typeof window !== "undefined") {
    try {
      window.localStorage.removeItem(OMD_KEY_STORAGE);
      if (identityRef && identityRef.mainPub) {
        window.localStorage.removeItem(`${OMD_KEY_STORAGE}:${identityRef.mainPub}`);
      }
    } catch (e) { /* ignore */ }
  }
  if (identityRef && identityRef.gun) {
    try {
      identityRef.gun.user().get(GRAPH_NODE).get(GRAPH_LEAF).put(null);
    } catch (e) { /* ignore */ }
  }
  publish();
}

function persistToGraph(key) {
  const pair = identityRef && identityRef.devicePair;
  const gun = identityRef && identityRef.gun;
  if (!pair || !gun) return;
  Gun.SEA.encrypt(key, pair)
    .then((enc) => {
      if (!enc) throw new Error("encrypt failed");
      gun.user().get(GRAPH_NODE).get(GRAPH_LEAF).put({ enc, updatedAt: Date.now() });
    })
    .catch((e) => console.warn("[omd-key] graph put failed:", e));
}

function detachGraph() {
  if (unsubGraph) {
    try { unsubGraph(); } catch (e) { /* ignore */ }
    unsubGraph = null;
  }
}

/** Bind the vault to a persistent identity (no-op when already bound). */
export function setOmdKeyIdentity(identity) {
  if (!identity || identity.type !== "persistent" || !identity.devicePair) return;
  const pub = identity.devicePair.pub || "";
  if (identityRef === identity && pub === boundPub) return;
  detachGraph();
  identityRef = identity;
  boundPub = pub;

  // Immediately adopt local cache or profile omd_key for this identity if present
  const cached = readLegacyStorage() || (identity.profile && normalizeCandidate(identity.profile.omd_key));
  if (cached) {
    vaultKey = cached;
    localKey = cached;
    publish();
  }

  const gun = identity.gun;
  const pair = identity.devicePair;
  try {
    const handle = gun.user().get(GRAPH_NODE).get(GRAPH_LEAF).on((data) => {
      const enc = data && typeof data === "object" ? data.enc : null;
      if (!enc) {
        // Nothing stored in GunDB yet. If we have a cached key, backfill GunDB now.
        if (vaultKey) {
          persistToGraph(vaultKey);
        }
        return;
      }
      Gun.SEA.decrypt(enc, pair)
        .then((dec) => {
          const key = normalizeCandidate(typeof dec === "string" ? dec : dec && dec.key);
          if (key) {
            vaultKey = key;
            localKey = key;
            if (typeof window !== "undefined") {
              try {
                window.localStorage.setItem(OMD_KEY_STORAGE, key);
                if (identity.mainPub) {
                  window.localStorage.setItem(`${OMD_KEY_STORAGE}:${identity.mainPub}`, key);
                }
              } catch (e) { /* ignore */ }
            }
            publish();
          } else {
            console.warn("[omd-key] stored key failed validation");
          }
        })
        .catch((e) => console.warn("[omd-key] decrypt failed:", e));
    });
    unsubGraph = () => {
      try { handle.off(); } catch (e) { /* ignore */ }
    };
  } catch (e) {
    console.warn("[omd-key] graph subscription failed:", e);
  }

  console.log("[omd-key] vault bound to", identity.did);
}

/** Unbind (logout / ephemeral). Keeps local-session key usable. */
export function clearOmdKeyIdentity() {
  detachGraph();
  identityRef = null;
  boundPub = "";
  vaultKey = "";
  localKey = readLegacyStorage();
  publish();
}

if (typeof window !== "undefined") {
  localKey = readLegacyStorage();
  publish();
  window.addEventListener("omd-profile-update", () => publish());
}

const OMD_SESSION_COOKIE = "OMD_SESSION_ID";

function readSessionCookie() {
  if (typeof document === "undefined") return "";
  const m = document.cookie.match(new RegExp(`(?:^|;\\s*)${OMD_SESSION_COOKIE}=([^;]+)`));
  return m ? decodeURIComponent(m[1]) : "";
}

/**
 * Token for the gateway WebSocket channel subscription
 * (ClientNotifier::doSubscribe), legacy style: the omd_key.
 * - persistent identities: key from the E2EE DID vault;
 * - guests with a stored/generated key: that key (an anonymous session is
 *   created under it server-side, see HttpHandler::bridgeSignIn);
 * - fallback: existing OMD_SESSION_ID cookie session.
 * Fresh guests get a locally generated 32-hex key persisted in localStorage,
 * editable later via the anonymous profile (storeOmdKey).
 */
export function getGatewayBase() {
  if (typeof window === "undefined") return "https://direct.onmydisk.net";
  const loc = window.location;
  const rawHost = loc.hostname || "";
  const isLocal = !rawHost || rawHost === "localhost" || rawHost === "127.0.0.1";
  const host = isLocal ? "direct.onmydisk.net" : rawHost;
  const proto = (isLocal || host === "direct.onmydisk.net" || loc.protocol === "https:") ? "https:" : "http:";
  return `${proto}//${host}`;
}

export async function ensureChannelToken() {
  const isShared = isPublicSharedRoute();
  let key = isShared ? localKey : getStoredOmdKey();
  if (!key) {
    if (!isShared) {
      const sid = readSessionCookie();
      if (sid) return sid;
    }
    // Mint a temporary guest channel token, but DO NOT overwrite permanent vault keys!
    const bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    const guestToken = Array.from(bytes).map((b) => b.toString(16).padStart(2, "0")).join("");
    localKey = guestToken;
    return guestToken;
  }
  return key;
}

export function simpleHash(str) {
  if (!str) return "empty";
  const normalized = str.toString().toLowerCase().trim();
  let hash = 0;
  for (let i = 0; i < normalized.length; i++) {
    const char = normalized.charCodeAt(i);
    hash = ((hash << 5) - hash) + char;
    hash = hash & hash;
  }
  return Math.abs(hash).toString(16);
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

export async function deriveEncryptionKey(token, salt = CHAT_SALT) {
  if (!token || typeof crypto === "undefined" || !crypto.subtle) {
    console.warn("[omd-key] WebCrypto unavailable, encryption disabled");
    return null;
  }
  const encoder = new TextEncoder();
  const baseKey = await crypto.subtle.importKey(
    "raw",
    encoder.encode(token),
    { name: "PBKDF2" },
    false,
    ["deriveKey"]
  );
  return crypto.subtle.deriveKey(
    {
      name: "PBKDF2",
      salt: encoder.encode(salt),
      iterations: PBKDF2_ITERATIONS,
      hash: "SHA-256",
    },
    baseKey,
    { name: "AES-GCM", length: 256 },
    false,
    ["encrypt", "decrypt"]
  );
}

export function createChatCrypto(token) {
  let encryptionKey = null;
  const decryptionCache = new Map();

  return {
    tokenHash: simpleHash(token),
    ready: deriveEncryptionKey(token).then((k) => {
      encryptionKey = k;
      return true;
    }),

    async encrypt(content) {
      if (!encryptionKey || content === undefined || content === null) return content;
      try {
        const encoder = new TextEncoder();
        const iv = crypto.getRandomValues(new Uint8Array(12));
        const data = encoder.encode(typeof content === "string" ? content : JSON.stringify(content));
        const ciphertext = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, encryptionKey, data);
        return {
          encrypted: true,
          ciphertext: uint8ToBase64(new Uint8Array(ciphertext)),
          iv: uint8ToBase64(iv),
        };
      } catch (e) {
        console.error("[omd-key] Encryption failed:", e);
        return content;
      }
    },

    async decrypt(content) {
      if (!encryptionKey || !content || !content.encrypted || !content.ciphertext) return content;
      if (decryptionCache.has(content.ciphertext)) {
        return decryptionCache.get(content.ciphertext);
      }
      try {
        const iv = base64ToUint8(content.iv);
        const ciphertext = base64ToUint8(content.ciphertext);
        const decrypted = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, encryptionKey, ciphertext);
        const decoded = new TextDecoder().decode(decrypted);
        let result;
        try {
          result = JSON.parse(decoded);
        } catch (e) {
          result = decoded;
        }
        decryptionCache.set(content.ciphertext, result);
        return result;
      } catch (e) {
        console.warn("[omd-key] Decryption failed:", e);
        throw e;
      }
    },
  };
}
