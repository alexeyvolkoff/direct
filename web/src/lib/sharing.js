import Gun from "gun/gun";
import "gun/sea";

const RELAY_URL = "wss://disk.direct:8765/gun";

import { sharedLinkUrl, sanitizeShareUrl } from "./public-origin.js";

/**
 * Base HTTP URL of the auth hub for the current deployment.
 * @returns {string}
 */
function authBaseUrl() {
  if (typeof window === "undefined" || !window.location.hostname ||
      window.location.hostname === "localhost" || window.location.hostname === "127.0.0.1") {
    return "https://disk.direct:8765";
  }
  const host = window.location.hostname;
  const proto = (window.location.protocol === "http:") ? "http:" : "https:";
  return `${proto}//${host}:8765`;
}

/**
 * Generate a random URL-friendly ID for a share.
 */
export function generateShareId() {
  const chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  let result = "";
  const randomValues = new Uint8Array(12);
  if (typeof crypto !== "undefined" && crypto.getRandomValues) {
    crypto.getRandomValues(randomValues);
    for (let i = 0; i < 12; i++) {
      result += chars[randomValues[i] % chars.length];
    }
  } else {
    for (let i = 0; i < 12; i++) {
      result += chars[Math.floor(Math.random() * chars.length)];
    }
  }
  return result;
}

/**
 * Creates or updates a public / password-protected share in GunDB.
 *
 * @param {Object} options
 * @param {Object} options.identity - PersistentIdentity instance
 * @param {string} options.linkId - Target device LinkID
 * @param {string} options.path - Path to file or folder on target device
 * @param {string} options.name - File or folder display name
 * @param {boolean} options.isDir - Whether item is directory
 * @param {string} [options.mimeType] - MIME type for single file
 * @param {number} [options.size] - File size in bytes
 * @param {string} [options.password] - Optional password protection
 * @param {boolean} [options.writable] - Allow write access (default false)
 * @param {number} [options.expiresAt] - Expiration timestamp (0 = never)
 * @param {string} [options.existingShareId] - Reuse existing shareId if updating
 * @returns {Promise<{ shareId: string, address: string, shareUrl: string }>}
 */
export async function createShare(options = {}) {
  const {
    identity,
    linkId,
    path,
    name,
    title = "",
    description = "",
    hasCover = false,
    customAttributes: rawCustomAttrs = null,
    item = {},
    isDir = false,
    mimeType = "",
    size = 0,
    password = "",
    writable = false,
    expiresAt = 0,
    existingShareId = null,
    coverData = null,
  } = options || {};

  if (!identity || !identity.gun || !identity.mainPub) {
    throw new Error("Persistent identity required to create shares");
  }

  if (!linkId || linkId === "undefined") {
    throw new Error("Target device LinkID is required to create a share link. Ensure your device is online.");
  }

  let actualShareId = existingShareId;
  const targetDrive = options.drive || (typeof window !== "undefined" && window.deviceRegistry?.get(linkId)?.drive);
  if (!actualShareId && targetDrive && typeof targetDrive.createShare === "function") {
    try {
      const nodeShare = await targetDrive.createShare(path, !!writable, name);
      if (nodeShare && nodeShare.status === "ok" && nodeShare.result?.shareId) {
        actualShareId = nodeShare.result.shareId;
      }
    } catch (e) {
      console.warn("[sharing] Failed to register opaque share on node:", e);
    }
  }

  const shareId = actualShareId || generateShareId();
  const mainPub = identity.mainPub;
  const isProtected = !!(password && password.trim().length > 0);


  let cleanName = name;
  if (!cleanName || cleanName === "Item" || cleanName === "Shared Item" || cleanName === shareId) {
    if (path && !path.startsWith("/shared-")) {
      cleanName = path.split("/").filter(Boolean).pop();
    }
  }
  if (!cleanName || cleanName === "Item" || cleanName === "Shared Item") {
    cleanName = "Shared Item";
  }
  const cleanIsDir = !!isDir;

  const passedCustomAttrs = rawCustomAttrs || item?.customAttributes || {};
  const itemTitle = title || passedCustomAttrs.title || cleanName;
  const itemDescription = description || passedCustomAttrs.description || "";
  const itemHasCover = !!(hasCover || passedCustomAttrs.cover || passedCustomAttrs.preview || item?.hasCover || item?.preview || coverData);

  const customAttributes = {
    title: itemTitle,
    description: itemDescription,
    cover: itemHasCover,
    preview: itemHasCover,
    ...passedCustomAttrs
  };

  const payload = {
    id: shareId,
    linkId,
    path,
    name: cleanName,
    title: itemTitle,
    description: itemDescription,
    hasCover: itemHasCover,
    coverData: coverData || null,
    customAttributes,
    isDir: cleanIsDir,
    type: cleanIsDir ? "folder" : "file",
    mimeType: mimeType || "",
    size: Number(size) || 0,
    writable: !!writable,
    expiresAt: Number(expiresAt) || 0,
    createdAt: Date.now(),
    ownerName: identity.profile?.name || identity.profile?.user || "OMD User",
  };

  let recordData;
  if (isProtected) {
    // Encrypt payload using Gun SEA with the provided password
    const encryptedData = await Gun.SEA.encrypt(JSON.stringify(payload), password.trim());
    recordData = {
      id: shareId,
      protected: true,
      name: cleanName,
      title: itemTitle,
      description: itemDescription,
      hasCover: itemHasCover,
      coverData: null, // Don't expose encrypted cover
      customAttributes,
      isDir: !!isDir,
      mimeType: mimeType || "",
      createdAt: payload.createdAt,
      encryptedData,
    };
  } else {
    recordData = {
      ...payload,
      protected: false,
      encryptedData: null,
    };
  }

  // 1. Save locally in localStorage for fast lookup and persistence
  try {
    const localSharesKey = `omd_shares_${mainPub}`;
    const localShares = JSON.parse(localStorage.getItem(localSharesKey) || "{}");
    localShares[shareId] = recordData;
    localStorage.setItem(localSharesKey, JSON.stringify(localShares));
    localStorage.setItem(`omd_share_record_${shareId}`, JSON.stringify(recordData));
  } catch (e) {
    console.warn("[sharing] LocalStorage save error:", e);
  }

  // 2. Put in GunDB via relay REST endpoint for guaranteed server-side persistence
  try {
    const hubHost = (typeof window !== "undefined" && window.location.hostname && window.location.hostname !== "localhost" && window.location.hostname !== "127.0.0.1")
      ? window.location.hostname
      : "disk.direct";
    const hubProtocol = (typeof window !== "undefined" && window.location.protocol === "http:" && hubHost !== "disk.direct") ? "http:" : "https:";
    const hubUrl = `${hubProtocol}//${hubHost}:8765`;

    fetch(`${hubUrl}/api/share-record`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ownerPub: mainPub, shareId, recordData })
    }).catch(e => console.warn("[sharing] hub /api/share-record error:", e));
  } catch (e) {}


  // 3. Put in GunDB under ~mainPub/sharing/<shareId> and sharing:<mainPub>/<shareId>
  if (identity.gun.user && identity.gun.user().is) {
    try {
      identity.gun.user().get("sharing").get(shareId).put(recordData);
    } catch (_) {}
  }

  const sharingNode = identity.gun.get("~" + mainPub).get("sharing");
  const publicSharingNode = identity.gun.get("sharing:" + mainPub);
  const cert = identity.certificate;

  await new Promise((resolve) => {
    let finished = false;
    const timeout = setTimeout(() => {
      if (!finished) {
        finished = true;
        resolve();
      }
    }, 1000);

    const onDone = (ack) => {
      if (!finished) {
        finished = true;
        clearTimeout(timeout);
        resolve(ack);
      }
    };

    try {
      if (cert) {
        sharingNode.get(shareId).put(recordData, onDone, { opt: { cert } });
      } else {
        sharingNode.get(shareId).put(recordData, onDone);
      }
      publicSharingNode.get(shareId).put(recordData);
    } catch (err) {
      if (!finished) {
        finished = true;
        clearTimeout(timeout);
        resolve();
      }
    }
  });

  const address = `${mainPub}.${shareId}`;
  const shareUrl = sharedLinkUrl(address);

  console.log(`[sharing] Created share: ${address} -> ${path} (protected: ${isProtected})`);
  return {
    shareId,
    address,
    shareUrl,
    record: recordData,
  };
}

/**
 * Revokes / deletes a share from GunDB.
 *
 * @param {Object} identity - PersistentIdentity instance
 * @param {string} shareId - Share ID to revoke
 * @returns {Promise<boolean>}
 */
export async function revokeShare(identity, shareId, linkId = null) {
  if (!identity || !identity.gun || !identity.mainPub) {
    throw new Error("Persistent identity required to revoke shares");
  }

  const mainPub = identity.mainPub;
  const cert = identity.certificate;
  const sharingNode = identity.gun.get("~" + mainPub).get("sharing");
  const publicSharingNode = identity.gun.get("sharing:" + mainPub);
  const putOptions = cert ? { opt: { cert } } : {};

  // Try revoking on connected device
  try {
    const drive = typeof window !== "undefined" && linkId ? window.deviceRegistry?.get(linkId)?.drive : null;
    if (drive && typeof drive.revokeShare === "function") {
      drive.revokeShare(shareId).catch(() => {});
    }
  } catch (_) {}

  try {
    const localSharesKey = `omd_shares_${mainPub}`;
    const localShares = JSON.parse(localStorage.getItem(localSharesKey) || "{}");
    delete localShares[shareId];
    localStorage.setItem(localSharesKey, JSON.stringify(localShares));
  } catch (e) {}


  await new Promise((resolve) => {
    let finished = false;
    const timeout = setTimeout(() => {
      if (!finished) {
        finished = true;
        resolve();
      }
    }, 1000);

    const onDone = (ack) => {
      if (!finished) {
        finished = true;
        clearTimeout(timeout);
        resolve(ack);
      }
    };

    try {
      if (identity.gun.user && identity.gun.user().is) {
        try { identity.gun.user().get("sharing").get(shareId).put(null); } catch (_) {}
      }
      if (cert) {
        sharingNode.get(shareId).put(null, onDone, { opt: { cert } });
      } else {
        sharingNode.get(shareId).put(null, onDone);
      }
      publicSharingNode.get(shareId).put(null);
    } catch (err) {
      if (!finished) {
        finished = true;
        clearTimeout(timeout);
        resolve();
      }
    }
  });

  console.log(`[sharing] Revoked share: ${shareId}`);
  return true;
}

/**
 * Resolves a shared address (e.g. "pubKey.shareId") from GunDB.
 *
 * @param {string} address - Format: "<ownerPub>.<shareId>"
 * @param {string} [password] - Password if the share is protected
 * @param {import("gun/gun").GunChain} [gunInstance] - Optional Gun instance
 * @returns {Promise<{ ownerPub: string, shareId: string, linkId: string, path: string, name: string, isDir: boolean, mimeType: string, size: number, writable: boolean, protected: boolean, ownerName: string, isExpired: boolean }>}
 */
export async function resolveShare(address, password = "", gunInstance = null) {
  if (!address || typeof address !== "string") {
    throw new Error("Invalid share address");
  }

  // Support /shared/prefix stripped or direct address
  const cleanAddress = address.replace(/^\/shared\//, "").trim();
  const dotIndex = cleanAddress.lastIndexOf(".");
  if (dotIndex === -1) {
    throw new Error("Invalid share address format. Expected <ownerPub>.<shareId>");
  }

  const ownerPub = cleanAddress.substring(0, dotIndex);
  const shareId = cleanAddress.substring(dotIndex + 1);

  if (!ownerPub || !shareId) {
    throw new Error("Missing ownerPub or shareId in address");
  }

  // 1. Try local cache (direct record, owner map, and all local share records)
  try {
    const directCache = localStorage.getItem(`omd_received_record_${shareId}`)
      || localStorage.getItem(`omd_share_record_${shareId}`);
    if (directCache) {
      const parsed = JSON.parse(directCache);
      if (parsed && (parsed.linkId && parsed.path && !parsed.protected)) {
        return parseRawShareRecord(parsed, ownerPub, shareId, password);
      } else if (parsed && parsed.protected && parsed.encryptedData) {
        return parseRawShareRecord(parsed, ownerPub, shareId, password);
      }
    }

    // Also check the owner's share list cache
    const localSharesKey = `omd_shares_${ownerPub}`;
    const localShares = JSON.parse(localStorage.getItem(localSharesKey) || "{}");
    if (localShares[shareId]) {
      const parsed = localShares[shareId];
      if (parsed && (parsed.linkId && parsed.path && !parsed.protected)) {
        return parseRawShareRecord(parsed, ownerPub, shareId, password);
      } else if (parsed && parsed.protected && parsed.encryptedData) {
        return parseRawShareRecord(parsed, ownerPub, shareId, password);
      }
    }

    for (let i = 0; i < localStorage.length; i++) {
      const key = localStorage.key(i);
      if (key && key.startsWith("omd_shares_")) {
        const data = JSON.parse(localStorage.getItem(key) || "{}");
        if (data && data[shareId]) {
          const parsed = data[shareId];
          if (parsed && (parsed.linkId && parsed.path && !parsed.protected)) {
            return parseRawShareRecord(parsed, ownerPub, shareId, password);
          } else if (parsed && parsed.protected && parsed.encryptedData) {
            return parseRawShareRecord(parsed, ownerPub, shareId, password);
          }
        }
      }
    }
  } catch (e) {}

  const realOwnerPub = ownerPub.replace(/^did:gun:/, "").replace(/^~/, "");
  const lastPartPub = ownerPub.includes(".") ? ownerPub.split(".").pop() : "";

  // 2. Fetch from GunDB using active .on() streaming to receive full properties as they arrive
  const gun = gunInstance || Gun({ peers: [RELAY_URL], localStorage: false });

  const rawRecord = await new Promise((resolve) => {
    let resolved = false;
    let sub1, sub2, sub3;
    const pollTimers = [];

    const cleanup = () => {
      pollTimers.forEach((t) => clearTimeout(t));
      try {
        if (sub1 && sub1.off) sub1.off();
        if (sub2 && sub2.off) sub2.off();
        if (sub3 && sub3.off) sub3.off();
      } catch (e) {}
    };

    const timeoutTimer = setTimeout(() => {
      if (!resolved) {
        resolved = true;
        cleanup();
        resolve(null);
      }
    }, 10000);

    const onData = (data) => {
      if (resolved || !data || typeof data !== "object") return;

      // Gun sends data incrementally. We must wait until the essential fields arrive.
      const isPlainShare = !!(data.linkId && data.path && !data.protected);
      const isEncryptedShare = !!(data.protected && data.encryptedData);

      if (isPlainShare || isEncryptedShare) {
        resolved = true;
        clearTimeout(timeoutTimer);
        cleanup();
        resolve(data);
      }
    };

    try {
      sub1 = gun.get("sharing:" + realOwnerPub).get(shareId).on(onData);
      sub2 = gun.get("~" + realOwnerPub).get("sharing").get(shareId).on(onData);
      sub3 = gun.get("sharing").get(shareId).on(onData);
      if (lastPartPub && lastPartPub !== realOwnerPub) {
        gun.get("sharing:" + lastPartPub).get(shareId).on(onData);
      }

      // Re-trigger .on() queries at 300ms, 900ms, 2500ms in case WebSocket handshake completes shortly after
      const delays = [300, 900, 2500, 5000];
      for (const delay of delays) {
        pollTimers.push(setTimeout(() => {
          if (!resolved) {
            try {
              gun.get("sharing:" + realOwnerPub).get(shareId).on(onData);
              gun.get("~" + realOwnerPub).get("sharing").get(shareId).on(onData);
              if (lastPartPub && lastPartPub !== realOwnerPub) {
                gun.get("sharing:" + lastPartPub).get(shareId).on(onData);
              }
            } catch (e) {}
          }
        }, delay));
      }
    } catch (e) {
      console.warn("[resolveShare] Gun subscription error:", e);
    }
  });

  if (!rawRecord) {
    throw new Error("Share link unavailable. Device may be offline or link was revoked.");
  }

  // Cache fetched rawRecord locally for fast future loads. Received records
  // live under omd_received_record_* so they never leak into "Shared by me"
  // (which enumerates the owned omd_share_record_* namespace).
  try {
    localStorage.setItem(`omd_received_record_${shareId}`, JSON.stringify(rawRecord));
    const localSharesKey = `omd_shares_${ownerPub}`;
    const localShares = JSON.parse(localStorage.getItem(localSharesKey) || "{}");
    localShares[shareId] = rawRecord;
    localStorage.setItem(localSharesKey, JSON.stringify(localShares));
  } catch (e) {}

  return parseRawShareRecord(rawRecord, ownerPub, shareId, password);
}

async function parseRawShareRecord(rawRecord, ownerPub, shareId, password) {
  if (rawRecord.protected) {
    if (!password) {
      return {
        ownerPub,
        shareId,
        protected: true,
        name: rawRecord.name || "Protected Share",
        isDir: !!rawRecord.isDir,
        mimeType: rawRecord.mimeType || "",
        requiresPassword: true,
      };
    }

    try {
      const decryptedStr = await Gun.SEA.decrypt(rawRecord.encryptedData, password.trim());
      if (!decryptedStr) {
        throw new Error("Incorrect password");
      }
      const decrypted = JSON.parse(decryptedStr);
      
      // Check expiration
      const isExpired = decrypted.expiresAt > 0 && Date.now() > decrypted.expiresAt;

      const title = decrypted.title || rawRecord.title || decrypted.name || rawRecord.name || "";
      const description = decrypted.description || rawRecord.description || "";
      const hasCover = !!(decrypted.hasCover || rawRecord.hasCover);
      const customAttributes = decrypted.customAttributes || rawRecord.customAttributes || { title, description, cover: hasCover };

      return {
        ownerPub,
        shareId,
        linkId: decrypted.linkId,
        path: decrypted.path,
        name: decrypted.name || rawRecord.name,
        title,
        description,
        hasCover,
        customAttributes,
        isDir: !!decrypted.isDir,
        mimeType: decrypted.mimeType || rawRecord.mimeType || "",
        size: decrypted.size || 0,
        writable: !!decrypted.writable,
        protected: true,
        ownerName: decrypted.ownerName || "OMD User",
        createdAt: decrypted.createdAt || rawRecord.createdAt,
        expiresAt: decrypted.expiresAt || 0,
        isExpired,
      };
    } catch (e) {
      if (e.message === "Incorrect password") throw e;
      throw new Error("Failed to decrypt share. Check password.");
    }
  }

  // Unprotected share
  const isExpired = rawRecord.expiresAt > 0 && Date.now() > rawRecord.expiresAt;
  const title = rawRecord.title || rawRecord.name || "";
  const description = rawRecord.description || "";
  const hasCover = !!rawRecord.hasCover;
  const customAttributes = rawRecord.customAttributes || { title, description, cover: hasCover };

  return {
    ownerPub,
    shareId,
    linkId: rawRecord.linkId,
    path: rawRecord.path,
    name: rawRecord.name || "Shared File",
    title,
    description,
    hasCover,
    customAttributes,
    isDir: !!rawRecord.isDir,
    mimeType: rawRecord.mimeType || "",
    size: rawRecord.size || 0,
    writable: !!rawRecord.writable,
    protected: false,
    ownerName: rawRecord.ownerName || "OMD User",
    createdAt: rawRecord.createdAt,
    expiresAt: rawRecord.expiresAt || 0,
    isExpired,
  };
}

/**
 * Searches user's Gun graph for an existing share for given linkId and path.
 *
 * @param {Object} identity - PersistentIdentity instance
 * @param {string} linkId - Device LinkID
 * @param {string} itemPath - File or folder path
 * @returns {Promise<Object|null>}
 */
export async function findExistingShare(identity, linkId, itemPath) {
  if (!identity || !identity.gun || !identity.mainPub) return null;

  const mainPub = identity.mainPub;
  
  // 1. Check local cache first
  try {
    const localSharesKey = `omd_shares_${mainPub}`;
    const localShares = JSON.parse(localStorage.getItem(localSharesKey) || "{}");
    for (const [key, data] of Object.entries(localShares)) {
      if (data && data.linkId === linkId && data.path === itemPath) {
        return { ...data, id: key, address: `${mainPub}.${key}` };
      }
    }
  } catch (e) {}

  // 2. Query Gun graph
  return new Promise((resolve) => {
    let found = null;
    const timer = setTimeout(() => resolve(found), 1200);

    identity.gun
      .get("~" + mainPub)
      .get("sharing")
      .map()
      .once((data, key) => {
        if (!data || key === "_" || typeof data !== "object") return;
        if (data.linkId === linkId && data.path === itemPath) {
          found = { ...data, id: key, address: `${mainPub}.${key}` };
          clearTimeout(timer);
          resolve(found);
        }
      });
  });
}

/**
 * Normalizes and enriches a share item with cached metadata and clean folder names.
 */
export function normalizeShareItem(item, mainPub, shareId) {
  const address = item.address || `${mainPub}.${shareId}`;
  const shareUrl = sanitizeShareUrl(item.shareUrl, address);

  let cachedRec = null;
  try {
    const rawRec = typeof localStorage !== "undefined"
      ? (localStorage.getItem(`omd_received_record_${shareId}`) || localStorage.getItem(`omd_share_record_${shareId}`))
      : null;
    if (rawRec) cachedRec = JSON.parse(rawRec);
  } catch (_) {}

  const merged = cachedRec ? { ...cachedRec, ...item } : item;
  const itemPath = merged.path || item.path || "";

  let rawName = merged.name || item.name;
  if (!rawName || rawName === "Item" || rawName === "Shared Item" || rawName === shareId || (typeof rawName === "string" && rawName.includes(shareId))) {
    if (itemPath && !itemPath.startsWith("/shared-")) {
      rawName = itemPath.split("/").filter(Boolean).pop();
    }
  }
  if (!rawName || rawName === "Item" || rawName === "Shared Item") {
    rawName = itemPath ? itemPath.split("/").filter(Boolean).pop() : shareId;
  }

  const isDir = !!(merged.isDir === true || merged.isDir === "true" || merged.type === "folder" || item.isDir === true || item.isDir === "true" || item.type === "folder");

  return {
    ...merged,
    ...item,
    name: rawName || shareId,
    path: itemPath,
    isDir: isDir,
    type: isDir ? "folder" : "file",
    shareId,
    address,
    shareUrl
  };
}

/**
 * Loads all shares created by the current persistent identity from LocalStorage and GunDB.
 *
 * @param {Object} identity - PersistentIdentity instance
 * @returns {Promise<Array<Object>>}
 */
export async function loadSharedByMe(identity) {
  if (!identity || !identity.mainPub) return [];
  const mainPub = identity.mainPub;
  const localSharesKey = `omd_shares_${mainPub}`;
  const sharesMap = new Map();

  // 1. Load from localStorage cache
  try {
    const localShares = JSON.parse(localStorage.getItem(localSharesKey) || "{}");
    for (const [sId, item] of Object.entries(localShares)) {
      if (item && typeof item === "object") {
        const shareId = item.id || item.shareId || sId;
        if (!shareId || shareId === "undefined" || shareId === "_") continue;
        sharesMap.set(shareId, normalizeShareItem(item, mainPub, shareId));
      }
    }
  } catch (e) {
    console.warn("[sharing] LocalStorage read error in loadSharedByMe:", e);
  }

  // Also check individual share records in localStorage, but ONLY owned ones:
  // a record is owned if it is also present in the owner's shares map
  // (createShare writes both together). Orphans are received (resolved) share
  // records or revoked leftovers — relocate them so they never appear here.
  try {
    const recordKeys = [];
    for (let i = 0; i < localStorage.length; i++) {
      const k = localStorage.key(i);
      if (k && k.startsWith("omd_share_record_")) recordKeys.push(k);
    }
    for (const k of recordKeys) {
      const shareId = k.slice("omd_share_record_".length);
      if (!shareId) continue;
      if (sharesMap.has(shareId)) {
        const rec = JSON.parse(localStorage.getItem(k) || "null");
        if (rec && typeof rec === "object") {
          sharesMap.set(shareId, normalizeShareItem(rec, mainPub, shareId));
        }
        continue;
      }
      try {
        const rec = JSON.parse(localStorage.getItem(k) || "null");
        if (rec && typeof rec === "object") {
          localStorage.setItem(`omd_received_record_${shareId}`, JSON.stringify(rec));
        }
      } catch (_) {}
      localStorage.removeItem(k);
    }
  } catch (_) {}

  // 2. Fetch from GunDB user graph ~mainPub/sharing
  if (identity && identity.gun) {
    await new Promise((resolve) => {
      let finished = false;
      const timer = setTimeout(() => {
        if (!finished) {
          finished = true;
          resolve();
        }
      }, 1200);

      try {
        identity.gun
          .get("~" + mainPub)
          .get("sharing")
          .map()
          .once((data, key) => {
            if (!data || key === "_" || key === "undefined" || typeof data !== "object") return;
            const shareId = data.id || data.shareId || key;
            if (!shareId || shareId === "undefined" || shareId === "_") return;
            const existing = sharesMap.get(shareId);
            const merged = { ...existing, ...data };
            sharesMap.set(shareId, normalizeShareItem(merged, mainPub, shareId));
          });
      } catch (err) {
        if (!finished) {
          finished = true;
          clearTimeout(timer);
          resolve();
        }
      }
    });
  }

  // Update localStorage cache with normalized results
  try {
    const toCache = {};
    for (const [id, item] of sharesMap.entries()) {
      toCache[id] = item;
    }
    localStorage.setItem(localSharesKey, JSON.stringify(toCache));
  } catch (_) {}

  return Array.from(sharesMap.values());
}

/**
 * Subscribes to shares created by the persistent identity, invoking callback immediately
 * with cached data and continuously streaming updates as GunDB loads properties from peers.
 *
 * @param {Object} identity - PersistentIdentity instance
 * @param {Function} callback - Called with Array<Object> whenever shares are updated
 * @returns {Function} Unsubscribe function
 */
export function subscribeSharedByMe(identity, callback) {
  if (!identity || !identity.mainPub) return () => {};
  const mainPub = identity.mainPub;
  const localSharesKey = `omd_shares_${mainPub}`;
  const sharesMap = new Map();

  // 1. Initial read from localStorage
  try {
    const localShares = JSON.parse(localStorage.getItem(localSharesKey) || "{}");
    for (const [sId, item] of Object.entries(localShares)) {
      if (item && typeof item === "object") {
        const shareId = item.id || item.shareId || sId;
        if (!shareId || shareId === "undefined" || shareId === "_") continue;
        sharesMap.set(shareId, normalizeShareItem(item, mainPub, shareId));
      }
    }
  } catch (_) {}

  try {
    const recordKeys = [];
    for (let i = 0; i < localStorage.length; i++) {
      const k = localStorage.key(i);
      if (k && k.startsWith("omd_share_record_")) recordKeys.push(k);
    }
    for (const k of recordKeys) {
      const shareId = k.slice("omd_share_record_".length);
      if (!shareId) continue;
      if (sharesMap.has(shareId)) {
        const rec = JSON.parse(localStorage.getItem(k) || "null");
        if (rec && typeof rec === "object") {
          sharesMap.set(shareId, normalizeShareItem(rec, mainPub, shareId));
        }
        continue;
      }
      try {
        const rec = JSON.parse(localStorage.getItem(k) || "null");
        if (rec && typeof rec === "object") {
          localStorage.setItem(`omd_received_record_${shareId}`, JSON.stringify(rec));
        }
      } catch (_) {}
      localStorage.removeItem(k);
    }
  } catch (_) {}

  if (sharesMap.size > 0) {
    callback(Array.from(sharesMap.values()));
  }

  if (!identity.gun) return () => {};

  let debounceTimer = null;
  const notify = () => {
    if (debounceTimer) clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => {
      try {
        const toCache = {};
        for (const [id, item] of sharesMap.entries()) {
          toCache[id] = item;
        }
        localStorage.setItem(localSharesKey, JSON.stringify(toCache));
      } catch (_) {}
      callback(Array.from(sharesMap.values()));
    }, 50);
  };

  const handleGunItem = (data, key) => {
    if (!data || key === "_" || key === "undefined" || typeof data !== "object") return;
    const shareId = data.id || data.shareId || key;
    if (!shareId || shareId === "undefined" || shareId === "_") return;

    const existing = sharesMap.get(shareId);
    const merged = { ...existing, ...data };
    const normalized = normalizeShareItem(merged, mainPub, shareId);
    sharesMap.set(shareId, normalized);
    notify();
  };

  const sub = identity.gun.get("~" + mainPub).get("sharing").map().on(handleGunItem);

  return () => {
    if (debounceTimer) clearTimeout(debounceTimer);
    try {
      if (sub && sub.off) sub.off();
    } catch (_) {}
  };
}

/**
 * Parses a pasted share reference into a canonical address.
 * Accepts a full URL ("https://host/shared/<ownerPub>.<shareId>"),
 * a path ("/shared/<ownerPub>.<shareId>") or a bare "<ownerPub>.<shareId>".
 *
 * @param {string} raw
 * @returns {{ ownerPub: string, shareId: string, address: string, shareUrl: string }|null}
 */
export function parseShareLink(raw) {
  if (typeof raw !== "string") return null;
  let s = raw.trim();
  if (!s) return null;

  // Accept any of: "/shared/<owner>.<shareId>", "/share/<owner>.<shareId>",
  // "#/shared/<owner>.<shareId>", full URLs containing those, or a bare
  // "<owner>.<shareId>".
  const embedded = s.match(/[#]?\/(?:shared|share)\/([^\/?#\s]+)/i);
  if (embedded && embedded[1]) {
    try {
      s = decodeURIComponent(embedded[1]);
    } catch (_) {
      s = embedded[1];
    }
  }
  s = s.split(/[?#]/)[0].trim().replace(/^\/+|\/+$/g, "");

  const dot = s.lastIndexOf(".");
  if (dot <= 0 || dot === s.length - 1) return null;
  const ownerPub = s.slice(0, dot).replace(/^~/, "").replace(/^did:gun:/, "").trim();
  const shareId = s.slice(dot + 1).trim();

  // The owner public key may itself contain dots (multi-part / device+owner
  // addressing); the LAST dot separates ownerPub from the opaque shareId —
  // the same convention resolveShare() uses when opening guest links.
  if (!/^[A-Za-z0-9_.-]{10,144}$/.test(ownerPub)) return null;
  if (!/^[A-Za-z0-9_-]{4,64}$/.test(shareId)) return null;

  const address = `${ownerPub}.${shareId}`;
  return { ownerPub, shareId, address, shareUrl: sharedLinkUrl(address) };
}

/**
 * Extracts the path rules from a hub-issued certificate
 * (Gun SEA compact format "SEA{...}" or JWT format "h.p.s").
 *
 * @param {string} certificate
 * @returns {Array<Object>}
 */
function certificateRules(certificate) {
  if (typeof certificate !== "string" || !certificate) return [];
  try {
    if (certificate.startsWith("SEA{")) {
      const parsed = JSON.parse(certificate.slice(3));
      return (parsed && parsed.m && parsed.m.w) || [];
    }
    if (certificate.includes(".")) {
      const payload = certificate.split(".")[1] || "";
      const b64 = payload.replace(/-/g, "+").replace(/_/g, "/");
      const parsed = JSON.parse(atob(b64.padEnd(Math.ceil(b64.length / 4) * 4, "=")));
      return parsed.w || [];
    }
  } catch (_) {}
  return [];
}

/**
 * True when the certificate grants write access to the given top-level path.
 *
 * @param {string} certificate
 * @param {string} path - e.g. "sharedToMe"
 * @returns {boolean}
 */
export function certificateAllowsPath(certificate, path) {
  if (!path) return false;
  return certificateRules(certificate).some((rule) => !!rule && rule["*"] === path);
}

/**
 * Makes sure the identity's certificate allows writes to ~mainPub/sharedToMe.
 * Refreshes it from the hub (no re-login) when the rule is missing.
 *
 * @param {Object} identity - PersistentIdentity instance
 * @returns {Promise<string|null>} current certificate
 */
export async function ensureSharedToMeCertificate(identity) {
  if (!identity || !identity.mainPub) return null;
  if (certificateAllowsPath(identity.certificate, "sharedToMe")) return identity.certificate;
  if (typeof identity.refreshCertificate !== "function") return identity.certificate;
  try {
    await identity.refreshCertificate();
  } catch (e) {
    console.warn("[sharing] certificate refresh failed:", (e && e.message) || e);
  }
  if (!certificateAllowsPath(identity.certificate, "sharedToMe")) {
    console.warn("[sharing] certificate still lacks 'sharedToMe' — Gun sync for shared-to-me disabled, localStorage only");
  }
  return identity.certificate;
}

/**
 * Loads all shares received/added by the current persistent identity.
 *
 * @param {Object} identity - PersistentIdentity instance
 * @returns {Promise<Array<Object>>}
 */
export function subscribeSharedToMe(identity, callback) {
  if (!identity || !identity.mainPub) return () => {};
  const mainPub = identity.mainPub;
  const localKey = `omd_shared_to_me_${mainPub}`;
  const sharesMap = new Map();

  try {
    const localShares = JSON.parse(localStorage.getItem(localKey) || "{}");
    for (const [addr, item] of Object.entries(localShares)) {
      if (item && typeof item === "object") {
        sharesMap.set(addr, { ...item, address: (item.address || addr) });
      }
    }
  } catch (_) {}

  if (sharesMap.size > 0) callback(Array.from(sharesMap.values()));

  if (!identity.gun) return () => {};

  let debounceTimer = null;
  const notify = () => {
    if (debounceTimer) clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => {
      try {
        const toCache = {};
        for (const [addr, item] of sharesMap.entries()) toCache[addr] = item;
        localStorage.setItem(localKey, JSON.stringify(toCache));
      } catch (_) {}
      callback(Array.from(sharesMap.values()));
    }, 50);
  };

  const handleGunItem = (data, key) => {
    if (!data || key === "_" || typeof data !== "object") return;
    const address = data.address || key;
    if (!address || address === "undefined") return;
    sharesMap.set(address, { ...data, address });
    notify();
  };

  const sub = identity.gun.get("~" + mainPub).get("sharedToMe").map().on(handleGunItem);

  return () => {
    if (debounceTimer) clearTimeout(debounceTimer);
    try {
      if (sub && sub.off) sub.off();
    } catch (_) {}
  };
}

export async function loadSharedToMe(identity) {
  if (!identity || !identity.mainPub) return [];
  const mainPub = identity.mainPub;

  const sharesMap = new Map();

  // 1. Load from localStorage cache
  try {
    const localSharedToMeKey = `omd_shared_to_me_${mainPub}`;
    const localShares = JSON.parse(localStorage.getItem(localSharedToMeKey) || "{}");
    for (const [addr, item] of Object.entries(localShares)) {
      if (item && typeof item === "object" && item !== null) {
        sharesMap.set(addr, item);
      }
    }
  } catch (e) {
    console.warn("[sharing] SharedToMe LocalStorage read error:", e);
  }

  // 2. Fetch from GunDB user graph ~mainPub/sharedToMe
  if (identity && identity.gun) {
    await new Promise((resolve) => {
      let finished = false;
      const timer = setTimeout(() => {
        if (!finished) {
          finished = true;
          resolve();
        }
      }, 1000);

      try {
        identity.gun
          .get("~" + mainPub)
          .get("sharedToMe")
          .map()
          .once((data, key) => {
            if (!data || key === "_" || typeof data !== "object") return;
            const address = data.address || key;
            sharesMap.set(address, {
              ...data,
              address
            });
          });
      } catch (err) {
        if (!finished) {
          finished = true;
          clearTimeout(timer);
          resolve();
        }
      }
    });
  }

  return Array.from(sharesMap.values());
}

/**
 * Adds a received share address to the user's sharedToMe list in GunDB and LocalStorage.
 *
 * @param {Object} identity - PersistentIdentity instance
 * @param {Object} shareRecord - Resolved share record
 */
export async function addSharedToMe(identity, shareRecord) {
  if (!identity || !identity.mainPub || !shareRecord || !shareRecord.address) return;
  const mainPub = identity.mainPub;
  const address = shareRecord.address;
  const ownerPub = shareRecord.ownerPub || address.split(".")[0];
  const shareId = shareRecord.shareId || address.split(".").pop();
  const localSharedToMeKey = `omd_shared_to_me_${mainPub}`;

  let addedAt = Date.now();
  try {
    const existing = JSON.parse(localStorage.getItem(localSharedToMeKey) || "{}")[address];
    if (existing && existing.addedAt) addedAt = existing.addedAt;
  } catch (_) {}

  const itemData = {
    address,
    shareId,
    shareUrl: sanitizeShareUrl(shareRecord.shareUrl, address),
    name: shareRecord.name || "Shared Item",
    isDir: !!shareRecord.isDir,
    mimeType: shareRecord.mimeType || "",
    size: Number(shareRecord.size) || 0,
    linkId: shareRecord.linkId || "",
    path: shareRecord.path || "",
    ownerPub,
    ownerName: shareRecord.ownerName || "OMD User",
    writable: !!shareRecord.writable,
    protected: !!shareRecord.protected,
    addedAt,
  };

  // 1. LocalStorage — always works (same-device fallback)
  try {
    const localShares = JSON.parse(localStorage.getItem(localSharedToMeKey) || "{}");
    localShares[address] = itemData;
    localStorage.setItem(localSharedToMeKey, JSON.stringify(localShares));
  } catch (e) {}

  // 2. GunDB ~mainPub/sharedToMe — requires a certificate covering the path
  if (identity.gun) {
    await ensureSharedToMeCertificate(identity);
    const cert = identity.certificate;
    const putOptions = cert ? { opt: { cert } } : {};
    try {
      identity.gun.get("~" + mainPub).get("sharedToMe").get(address).put(itemData, null, putOptions);
    } catch (e) {}
  }

  console.info(`[sharing] added to shared-to-me: ${address} (${itemData.isDir ? "folder" : "file"})`);
}

/**
 * Removes a received share address from the sharedToMe list.
 *
 * @param {Object} identity - PersistentIdentity instance
 * @param {string} address - "<ownerPub>.<shareId>"
 * @returns {Promise<boolean>}
 */
export async function removeSharedToMe(identity, address) {
  if (!identity || !identity.mainPub || !address) return false;
  const mainPub = identity.mainPub;
  const localSharedToMeKey = `omd_shared_to_me_${mainPub}`;

  try {
    const localShares = JSON.parse(localStorage.getItem(localSharedToMeKey) || "{}");
    delete localShares[address];
    if (Object.keys(localShares).length) {
      localStorage.setItem(localSharedToMeKey, JSON.stringify(localShares));
    } else {
      localStorage.removeItem(localSharedToMeKey);
    }
  } catch (_) {}

  if (identity.gun) {
    await ensureSharedToMeCertificate(identity);
    const cert = identity.certificate;
    const putOptions = cert ? { opt: { cert } } : {};
    await new Promise((resolve) => {
      let done = false;
      const timer = setTimeout(() => {
        if (!done) { done = true; resolve(); }
      }, 1500);
      try {
        identity.gun.get("~" + mainPub).get("sharedToMe").get(address).put(null, (ack) => {
          if (!done) { done = true; clearTimeout(timer); resolve(ack); }
        }, putOptions);
      } catch (_) {
        if (!done) { done = true; clearTimeout(timer); resolve(); }
      }
    });
  }

  console.info(`[sharing] removed from shared-to-me: ${address}`);
  return true;
}
