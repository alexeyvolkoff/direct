import { WebRTCDataChannelDrive } from "./webrtc-drive.js";

/**
 * Dedicated scoped drives for "Shared to me" shortcuts.
 *
 * These tunnels are NOT registered in deviceRegistry on purpose:
 * - SetScope downgrades the whole tunnel to a ScopedGuest, so reusing a
 *   sidebar device drive would break normal browsing of that device;
 * - connectDevice() hides the scope behind PairModal/sidebar bookkeeping.
 *
 * Each shortcut gets its own tunnel with prefix "/s-<shareId>". The host
 * resolves the real scope (path + writable) from its OWN share record, i.e.
 * revocation is re-checked on every SetScope.
 */

const drives = new Map(); // shareId -> entry
const pending = new Map(); // shareId -> in-flight Promise (single-flight)
const CONNECT_TIMEOUT = 20000;

function makeEntry({ linkId, shareId, path, writable, identity }) {
  const drive = new WebRTCDataChannelDrive(linkId);
  drive.didPub = (identity && identity.mainPub) || null;
  return {
    shareId,
    linkId,
    prefix: shareDrivePrefix(shareId),
    path,
    writable: !!writable,
    drive,
    ready: false,
    failed: false,
    error: null,
  };
}

export function shareDrivePrefix(shareId) {
  return "/s-" + shareId;
}

export function getShareDriveEntry(shareId) {
  return (shareId && drives.get(shareId)) || null;
}

export function getShareDriveByPrefix(prefix) {
  if (!prefix) return null;
  const key = prefix.startsWith("/") ? prefix.slice(1) : prefix;
  if (!key.startsWith("s-")) return null;
  return drives.get(key.slice(2)) || null;
}

export function listShareDrives() {
  return Array.from(drives.values());
}

export function closeShareDrive(shareId) {
  const entry = drives.get(shareId);
  if (!entry) return;
  drives.delete(shareId);
  pending.delete(shareId);
  try {
    entry.drive.disconnect();
  } catch (_) {}
}

/**
 * Returns a ready scoped share drive, connecting to the owner device and
 * applying SetScope on first use. Re-uses an existing ready tunnel.
 *
 * @param {Object} opts
 * @param {Object} [opts.identity] - PersistentIdentity (guest pairing DID)
 * @param {string} opts.linkId - owner device LinkID
 * @param {string} opts.shareId - 12-char share id, mandatory for SetScope
 * @param {string} [opts.path] - owner-real path, informational (host resolves it)
 * @param {boolean} [opts.writable]
 * @returns {Promise<Object>} entry { shareId, linkId, prefix, drive, ready, ... }
 */
export async function ensureShareDrive(opts = {}) {
  const { linkId, shareId } = opts;
  if (!linkId || !shareId) throw new Error("Share drive requires linkId and shareId");

  const existing = drives.get(shareId);
  if (existing) {
    if (existing.failed) {
      if (!opts.isWarmUp) {
        drives.delete(shareId);
      } else {
        throw existing.error || new Error("Share drive unavailable");
      }
    } else if (existing.ready) {
      return existing;
    }
  }
  if (pending.has(shareId)) return pending.get(shareId);

  const promise = (async () => {
    const entry = existing || makeEntry(opts);
    drives.set(shareId, entry);
    try {
      const { drive } = entry;
      // Already connected as this scope (e.g. after a page reload that kept the tunnel).
      if (drive.isConnected && drive.authLevel === "ScopedGuest" && drive.scopeShareId === shareId) {
        entry.ready = true;
        return entry;
      }
      if (!drive.isConnected) {
        await drive.connect();
      }
      const ok = await drive.waitUntilConnected(CONNECT_TIMEOUT);
      if (!ok || !drive.isConnected) {
        throw new Error("Could not open a connection to the shared device");
      }
      const res = await drive.setScope({ shareId, path: opts.path || "", writable: !!opts.writable });
      if (!res || res.status !== "ok") {
        const msg = (res && res.error) || "Share access was denied";
        // Roll back client-side scope flags: an unscoped tunnel must never be
        // used for file RPCs after a failed SetScope.
        drive.scopeShareId = "";
        drive.scopePath = "";
        drive.scopeWritable = false;
        throw new Error(msg);
      }
      entry.ready = true;
      return entry;
    } catch (e) {
      entry.failed = true;
      entry.error = e instanceof Error ? e : new Error(String(e));
      try { entry.drive.disconnect(); } catch (_) {}
      throw entry.error;
    }
  })();

  pending.set(shareId, promise);
  try {
    return await promise;
  } finally {
    pending.delete(shareId);
  }
}