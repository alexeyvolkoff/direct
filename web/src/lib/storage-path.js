import { deviceRegistry } from "./device-registry.js";
import { profileStore } from "./profile-store.js";
import { getGatewayBase, getStoredOmdKey } from "./omd-key.js";
import { isNativeStorageAvailable } from "./native-drive.js";

export function getDefaultStoragePath() {
  if (typeof window !== "undefined" && isNativeStorageAvailable()) {
    return "/Documents/AIAssistant";
  }
  return "/Private/AIAssistant";
}

export const DEFAULT_STORAGE_PATH = "/Private/AIAssistant";

/**
 * Normalizes a virtual path:
 * - strips duplicate leading/trailing slashes
 * - removes OS home prefix like /home/user or /Users/user
 * - removes duplicate /Private/Private
 */
export function cleanStoragePath(pathStr) {
  if (!pathStr || typeof pathStr !== "string") return "";
  let clean = pathStr.trim();
  clean = clean.replace(/^\/(home|Users)\/[^\/]+(\/|$)/i, "/");
  clean = clean.replace(/^\/Private\/Private(\/|$)/i, "/Private$1");
  if (!clean.startsWith("/")) clean = "/" + clean;
  clean = clean.replace(/\/+$/, "");
  return clean || "/";
}

/**
 * Resolves local client device entry (browser OPFS/client) from registry.
 */
export function getLocalDevice() {
  return deviceRegistry.list().find((d) => d && d.local === true && d.drive) || null;
}

/**
 * Parses a storage path which may contain a device prefix, e.g.:
 *   "/MyLaptop/OnMyDisk" -> device: MyLaptop, folder: "/OnMyDisk"
 *   "/OnMyDisk"          -> device: fallbackDevice, folder: "/OnMyDisk"
 * 
 * Note: A device-qualified path requires at least 2 path segments (/[Device]/[Folder]).
 * A single segment like "/OnMyDisk" is always a folder, never a device qualification.
 * "Private" is also a reserved folder prefix, never a device name.
 */
export function parseStoragePath(storagePath, fallbackDevice = null) {
  const fallbackFolder = getDefaultStoragePath();
  const clean = cleanStoragePath(storagePath) || fallbackFolder;
  const parts = clean.split("/").filter(Boolean);
  if (!parts.length) {
    return {
      device: fallbackDevice,
      folder: fallbackFolder,
      deviceName: fallbackDevice?.name || "",
      isDeviceQualified: false,
    };
  }

  if (parts.length >= 2) {
    const firstSeg = parts[0];
    if (firstSeg.toLowerCase() !== "private") {
      const allDevices = deviceRegistry.list().sort((a, b) => {
        const aScore = (a?.local ? 4 : 0) + ((a?.status === "connected" || a?.drive?.isConnected) ? 2 : 0);
        const bScore = (b?.local ? 4 : 0) + ((b?.status === "connected" || b?.drive?.isConnected) ? 2 : 0);
        return bScore - aScore;
      });
      const matched = allDevices.find((d) => {
        if (!d) return false;
        if (d.name && d.name.toLowerCase() === firstSeg.toLowerCase()) return true;
        if (d.linkId && d.linkId === firstSeg) return true;
        if (d.prefix && d.prefix.replace(/^\//, "").toLowerCase() === firstSeg.toLowerCase()) return true;
        if (d.local && (firstSeg.toLowerCase() === "this device" || firstSeg.toLowerCase() === "local")) return true;
        return false;
      });

      const subParts = parts.slice(1);
      const sub = cleanStoragePath("/" + subParts.join("/"));

      if (matched) {
        return {
          device: matched,
          folder: sub,
          deviceName: matched.name || firstSeg,
          isDeviceQualified: true,
        };
      } else {
        // Device qualification is present in path even if device is offline
        return {
          device: fallbackDevice,
          folder: sub,
          deviceName: firstSeg,
          isDeviceQualified: true,
        };
      }
    }
  }

  return {
    device: fallbackDevice,
    folder: clean,
    deviceName: fallbackDevice?.name || "",
    isDeviceQualified: false,
  };
}

/**
 * Formats a storage path given a chosen device and a folder path inside that device.
 * For local browser OPFS / client: returns relative folder path (e.g. "/OnMyDisk").
 * For remote devices: ALWAYS stores qualified path (e.g. "/Linux-desktop/OnMyDisk").
 */
export function formatStoragePath(device, folderPath) {
  let sub = cleanStoragePath(folderPath || getDefaultStoragePath());
  if (!device) {
    return sub;
  }
  const devName = (device.name && device.name !== device.linkId)
    ? device.name
    : (device.prefix ? device.prefix.replace(/^\//, "") : (device.name || device.linkId || ""));

  if (!devName || devName.toLowerCase() === "private") {
    return sub;
  }

  // Avoid duplicate device prefix if sub already starts with /devName
  if (sub.toLowerCase().startsWith("/" + devName.toLowerCase() + "/")) {
    return sub;
  }
  if (sub.toLowerCase() === "/" + devName.toLowerCase()) {
    return sub;
  }
  return "/" + devName + sub;
}

/**
 * Persists the chosen storage path across all layers:
 * 1. Svelte profileStore & window.__omdProfile (immediate UI propagation)
 * 2. Encrypted DID profile (GunDB graph)
 * 3. Server backend database (login:defaultStorage via /rest)
 */
export async function persistStoragePath(storagePath, identity = null, key = null) {
  if (!storagePath) return;
  profileStore.update((p) => ({ ...p, defaultStorage: storagePath }));
  if (typeof window !== "undefined") {
    if (window.__omdProfile) {
      window.__omdProfile.defaultStorage = storagePath;
    }
    try {
      window.localStorage.setItem("omd_default_storage", storagePath);
    } catch (_) {}
  }
  if (identity && typeof identity.updateProfile === "function") {
    try {
      await identity.updateProfile({ defaultStorage: storagePath });
    } catch (e) {
      console.warn("[storage-path] identity.updateProfile notice:", e);
    }
  }
  if (typeof window !== "undefined") {
    try {
      const user = identity?.profile?.user || (typeof window !== "undefined" && window.__omdProfile?.user) || "";
      const effectiveKey = key || (identity && identity.token) || getStoredOmdKey() || "";
      if (user && user !== "anonymous" && effectiveKey) {
        const base = getGatewayBase();
        await fetch(`${base}/rest`, {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "authorization": `token:${effectiveKey}`,
          },
          body: JSON.stringify({
            action: "updateProfileData",
            profile: { user, defaultStorage: storagePath },
          }),
        });
      }
    } catch (e) {
      console.warn("[storage-path] server profile sync notice:", e);
    }
  }
}

