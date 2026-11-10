import { deviceRegistry } from "../device-registry.js";
import { getOrCreateLocalLinkId, getDefaultWebDeviceName } from "../local-client.js";

/**
 * Bridges PersistentIdentity's GunDB device list with App.svelte's
 * connectDevice() / filesData flow.
 *
 * Usage from App.svelte:
 *
 *   import { loadMyDevices } from "./lib/identity/device-list.js";
 *   // after identity init + connectDevice exist:
 *   const unsub = loadMyDevices(identity, connectDevice, () => filesData, (v) => filesData = v, () => fmApi);
 */
export function loadMyDevices(identity, registerDevicePlaceholder, getFilesData, setFilesData, getFmApi) {
  if (!identity || identity.type !== "persistent") return () => {};

  const seen = new Set(); // linkIds we already handed to registerDevicePlaceholder

  return identity.subscribeDeviceList((devices) => {
    const activeIds = new Set(devices.map(d => d.linkId).filter(Boolean));
    const localId = getOrCreateLocalLinkId();
    const myPub = identity.devicePub || "";

    for (const id of seen) {
      if (!activeIds.has(id)) seen.delete(id);
    }

    for (const dev of devices) {
      if (!dev.linkId) continue;
      if (dev.linkId === localId) continue;
      const existing = deviceRegistry.get(dev.linkId);
      if (existing && existing.local) continue;

      // Skip only stale linkIds that originated from THIS exact browser instance
      // (same devicePub), never another machine sharing a default OS name like "Linux-desktop".
      if (myPub && dev.originDevicePub && dev.originDevicePub === myPub) {
        continue;
      }

      if (!seen.has(dev.linkId)) {
        seen.add(dev.linkId);
        console.log("[device-list] registering saved device placeholder:", dev.name, dev.linkId);
        registerDevicePlaceholder(dev.linkId, dev.name);
      }
    }
  });
}
