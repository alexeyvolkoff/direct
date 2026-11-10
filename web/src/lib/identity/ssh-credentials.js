import { writable } from "svelte/store";
import Gun from "gun/gun";
import "gun/sea";
import { omdKeyStore, getStoredOmdKey, deriveEncryptionKey } from "../omd-key.js";

/**
 * SSH credential vault for terminal quick-login.
 *
 * Per-device system credentials (username + password of a REAL device user,
 * unrelated to omd_key) are stored E2EE so any device of the same DID can
 * decrypt them:
 *   - key: AES-GCM key derived (PBKDF2) from the user's omd_key, which is
 *     itself shared across all devices via the DID graph. Thus every device
 *     of the same identity sees the same SSH vault.
 *   - graph: ~{mainPub}/ssh → <linkId> → { ciphertext, iv, updatedAt }
 *   - localStorage mirror (decrypted) under "omd_ssh_creds:<mainPub>" for
 *     instant availability on reload.
 * The relay never sees plaintext.
 *
 * Legacy fallback: entries previously encrypted with the browser's devicePair
 * (gun.user().get("ssh")) are still decrypted with that pair so no vault is
 * lost; new writes always go to the shared node with the omd_key key.
 */

const LS_PREFIX = "omd_ssh_creds";
const GRAPH_NODE = "ssh";
const SHARED_SSH_PREFIX = "ssh_creds:";
const SSH_SALT = "omd-ssh-salt-v1";

let identityRef = null;
let boundPub = "";
let unsubGraph = null;
let unsubOmdKey = null;

/** linkId -> { user, pass } */
const cache = new Map();
/** linkId -> raw encrypted payload for deferred decryption when omd_key arrives */
const rawEntries = new Map();

/** AES-GCM CryptoKey + the omd_key it was derived from, or null. */
let vaultCrypto = null;
let vaultKeySource = "";

export const sshCredsStore = writable({ byDevice: {}, ready: false });

function publish() {
  const byDevice = {};
  for (const [linkId, rec] of cache) {
    byDevice[linkId] = { ...rec };
  }
  sshCredsStore.set({ byDevice, ready: true });
}

function lsKey() {
  return LS_PREFIX + ":" + ((identityRef && identityRef.mainPub) || "anon");
}

function persistMirror() {
  try {
    const obj = {};
    for (const [linkId, rec] of cache) obj[linkId] = rec;
    window.localStorage.setItem(lsKey(), JSON.stringify(obj));
  } catch (e) {
    console.warn("[ssh-creds] local mirror write failed:", e);
  }
}

function loadLocalMirror() {
  try {
    const raw = window.localStorage.getItem(lsKey());
    if (!raw) return;
    const obj = JSON.parse(raw) || {};
    for (const [linkId, rec] of Object.entries(obj)) {
      if (rec && typeof rec === "object" && rec.user) {
        cache.set(linkId, { user: String(rec.user), pass: String(rec.pass || "") });
      }
    }
  } catch (e) {
    console.warn("[ssh-creds] local mirror read failed:", e);
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

/** (Re)derive the AES key from the user's omd_key. */
async function refreshVaultCrypto() {
  const key = getStoredOmdKey();
  if (!key) {
    vaultCrypto = null;
    vaultKeySource = "";
    return;
  }
  if (vaultKeySource === key && vaultCrypto) return;
  try {
    const cryptoKey = await deriveEncryptionKey(key, SSH_SALT);
    vaultCrypto = cryptoKey;
    vaultKeySource = key;
  } catch (e) {
    console.warn("[ssh-creds] failed to derive vault crypto:", e);
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
    };
  } catch (e) {
    console.warn("[ssh-creds] AES encrypt failed:", e);
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
    if (parsed && parsed.user) {
      return { user: String(parsed.user), pass: String(parsed.pass || "") };
    }
  } catch (e) {
    console.warn("[ssh-creds] AES decrypt failed:", e);
  }
  return null;
}

async function decryptEntry(entry, linkId) {
  if (!entry || typeof entry !== "object") return null;
  // Shared vault: AES payload
  if (entry.ciphertext && entry.iv) {
    const rec = await aesDecrypt(entry);
    if (rec) return rec;
  }
  // Legacy: SEA-encrypted string with the browser's devicePair.
  if (typeof entry.enc === "string" && identityRef && identityRef.devicePair) {
    try {
      const dec = await Gun.SEA.decrypt(entry.enc, identityRef.devicePair);
      let parsed = dec;
      if (typeof dec === "string") {
        try { parsed = JSON.parse(dec); } catch (e) { return null; }
      }
      if (parsed && parsed.user) {
        return { user: String(parsed.user), pass: String(parsed.pass || "") };
      }
    } catch (e) {
      console.warn("[ssh-creds] legacy SEA decrypt failed for", linkId, e);
    }
  }
  return null;
}

async function redecryptPendingSshEntries() {
  if (rawEntries.size === 0) return;
  await refreshVaultCrypto();
  if (!vaultCrypto) return;
  let updated = false;
  for (const [linkId, raw] of rawEntries.entries()) {
    const rec = await decryptEntry(raw, linkId);
    if (rec) {
      cache.set(String(linkId), rec);
      updated = true;
    }
  }
  if (updated) {
    publish();
    persistMirror();
  }
}

async function syncToGraph() {
  if (!identityRef || !identityRef.gun || !identityRef.mainPub) return;
  await refreshVaultCrypto();
  await redecryptPendingSshEntries();
  const sharedGraph = identityRef.gun.get(SHARED_SSH_PREFIX + identityRef.mainPub);
  for (const [linkId, rec] of cache) {
    const payload = await aesEncrypt(rec);
    if (payload) {
      try {
        sharedGraph.get(String(linkId)).put({ ...payload, deleted: false, updatedAt: Date.now() });
      } catch (e) {
        console.warn("[ssh-creds] graph put failed for", linkId, e);
      }
    }
  }
}

function handleIncomingSshEntry(entry, linkId, { isLegacy = false } = {}) {
  if (!linkId || linkId === "_") return;
  if (entry === null || (entry && typeof entry === "object" && entry.deleted === true)) {
    if (isLegacy) return;
    rawEntries.delete(String(linkId));
    if (cache.delete(String(linkId))) {
      publish();
      persistMirror();
    }
    return;
  }
  // Ignore GunDB soul-pointer stubs rather than wiping cached SSH credentials
  if (!entry || typeof entry !== "object" || (!entry.ciphertext && !entry.enc)) {
    return;
  }
  rawEntries.set(String(linkId), entry);
  refreshVaultCrypto()
    .then(() => decryptEntry(entry, linkId))
    .then((rec) => {
      if (rec) {
        cache.set(String(linkId), rec);
        publish();
        persistMirror();
      }
    })
    .catch(() => {});
}

function subscribeGraph() {
  const gun = identityRef && identityRef.gun;
  const mainPub = identityRef && identityRef.mainPub;
  if (!gun || !mainPub) return;
  const handles = [];
  try {
    handles.push(
      gun.get(SHARED_SSH_PREFIX + mainPub).map().on((entry, linkId) => {
        handleIncomingSshEntry(entry, linkId, { isLegacy: false });
      })
    );
    handles.push(
      gun.get("~" + mainPub).get(GRAPH_NODE).map().on((entry, linkId) => {
        handleIncomingSshEntry(entry, linkId, { isLegacy: true });
      })
    );
    unsubGraph = () => {
      for (const h of handles) {
        try { if (h && typeof h.off === "function") h.off(); } catch (_) {}
      }
    };
  } catch (e) {
    console.warn("[ssh-creds] graph subscription failed:", e);
  }
}

/** Bind the vault to a persistent identity; no-op when already bound. */
export function setSshIdentity(identity) {
  if (!identity || identity.type !== "persistent" || !identity.devicePair) return;
  const pub = identity.mainPub || identity.devicePair.pub || "";
  if (identityRef === identity && pub === boundPub) return;
  detach();
  identityRef = identity;
  boundPub = pub;
  cache.clear();
  rawEntries.clear();
  loadLocalMirror();
  publish();
  subscribeGraph();

  // Wait for (or react to) the omd_key so the vault key is ready. If the key
  // changes later, re-derive and push any pending entries to the shared node.
  refreshVaultCrypto().then(() => syncToGraph());
  unsubOmdKey = omdKeyStore.subscribe(() => {
    refreshVaultCrypto().then(() => syncToGraph());
  });
  console.log("[ssh-creds] vault bound to", identity.did);
}

/** Drop the binding (logout / ephemeral mode). Keeps localStorage mirror. */
export function clearSshIdentity() {
  detach();
  identityRef = null;
  boundPub = "";
  vaultCrypto = null;
  vaultKeySource = "";
  cache.clear();
  rawEntries.clear();
  publish();
}

/** Saved credentials for a device, or null. */
export function getSshCredential(linkId) {
  if (!linkId) return null;
  const rec = cache.get(linkId);
  return rec ? { ...rec } : null;
}

/** Store credentials for a device (memory + mirror + E2EE graph). */
export async function saveSshCredential(linkId, user, pass) {
  if (!linkId || !user) return false;
  const rec = { user: String(user), pass: String(pass == null ? "" : pass) };
  cache.set(String(linkId), rec);
  publish();
  if (identityRef) persistMirror();

  const gun = identityRef && identityRef.gun;
  const mainPub = identityRef && identityRef.mainPub;
  await refreshVaultCrypto();
  if (gun && mainPub) {
    try {
      const payload = await aesEncrypt(rec);
      if (payload) {
        const entry = { ...payload, deleted: false, updatedAt: Date.now() };
        rawEntries.set(String(linkId), entry);
        gun.get(SHARED_SSH_PREFIX + mainPub).get(String(linkId)).put(entry);
      }
    } catch (e) {
      console.warn("[ssh-creds] graph put failed:", e);
    }
  }
  return true;
}

/** Remove stored credentials for a device everywhere. */
export async function clearSshCredential(linkId) {
  if (!linkId) return;
  rawEntries.delete(String(linkId));
  cache.delete(String(linkId));
  publish();
  try {
    if (typeof window === "undefined") return;
    const key = lsKey();
    const raw = window.localStorage.getItem(key);
    if (raw) {
      const obj = JSON.parse(raw) || {};
      delete obj[String(linkId)];
      window.localStorage.setItem(key, JSON.stringify(obj));
    }
  } catch (e) { /* ignore */ }
  const gun = identityRef && identityRef.gun;
  const mainPub = identityRef && identityRef.mainPub;
  if (gun && mainPub) {
    try {
      gun.get(SHARED_SSH_PREFIX + mainPub).get(String(linkId)).put({
        deleted: true,
        ciphertext: "",
        iv: "",
        updatedAt: Date.now(),
      });
      gun.get("~" + mainPub).get(GRAPH_NODE).get(String(linkId)).put(null);
    } catch (e) { /* ignore */ }
  }
}