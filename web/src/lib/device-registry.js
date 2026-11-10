export class DeviceRegistry {
  constructor() {
    this.devices = new Map();
  }

  add(deviceId, entry) {
    this.devices.set(deviceId, entry);
  }

  get(deviceId) {
    if (!deviceId) return undefined;
    const direct = this.devices.get(deviceId);
    if (direct) return direct;
    for (const dev of this.devices.values()) {
      if (dev && (dev.peerId === deviceId || dev.shortId === deviceId || dev.deviceId === deviceId)) {
        return dev;
      }
    }
    return undefined;
  }

  has(deviceId) {
    return Boolean(this.get(deviceId));
  }

  getByName(name) {
    if (!name) return null;
    const norm = String(name).toLowerCase();
    let connectedMatch = null;
    let fallbackMatch = null;
    for (const device of this.devices.values()) {
      if (device && device.name && device.name.toLowerCase() === norm) {
        if (device.local) return device;
        if (!connectedMatch && (device.status === "connected" || device.drive?.isConnected)) {
          connectedMatch = device;
        }
        if (!fallbackMatch) fallbackMatch = device;
      }
    }
    return connectedMatch || fallbackMatch;
  }

  // Resolve a device by its namespace segment (hostname prefix, linkId or name).
  // prefix may come with or without a leading "/".
  getByPrefix(prefix) {
    if (!prefix) return null;
    const norm = String(prefix).replace(/^\//, "").toLowerCase();
    let exactConnected = null;
    let exactFallback = null;
    let nameLocal = null;
    let nameConnected = null;
    let nameFallback = null;

    for (const device of this.devices.values()) {
      if (!device) continue;
      const devPrefix = device.prefix ? String(device.prefix).replace(/^\//, "").toLowerCase() : "";
      const devLinkId = device.linkId ? String(device.linkId).toLowerCase() : "";
      const devPeerId = device.peerId ? String(device.peerId).toLowerCase() : "";
      const devShortId = device.shortId ? String(device.shortId).toLowerCase() : "";
      const devName = device.name ? String(device.name).toLowerCase() : "";
      const isOnline = device.status === "connected" || device.drive?.isConnected;

      if (devPrefix === norm || devLinkId === norm || devPeerId === norm || devShortId === norm) {
        if (device.local) return device;
        if (!exactConnected && isOnline) exactConnected = device;
        if (!exactFallback) exactFallback = device;
      }
      if (devName === norm) {
        if (device.local && !nameLocal) nameLocal = device;
        if (!nameConnected && isOnline) nameConnected = device;
        if (!nameFallback) nameFallback = device;
      }
    }
    return exactConnected || nameLocal || exactFallback || nameConnected || nameFallback;
  }

  getDrive(deviceId) {
    const device = this.devices.get(deviceId);
    return device ? device.drive : null;
  }

  delete(deviceId) {
    let targetKey = deviceId;
    let device = this.devices.get(deviceId);
    if (!device) {
      device = this.getByPrefix(deviceId) || this.getByName(deviceId);
      if (device) {
        for (const [key, val] of this.devices.entries()) {
          if (val === device) {
            targetKey = key;
            break;
          }
        }
      }
    }
    // close(), not disconnect(): permanent teardown, no auto-reconnect
    if (device && device.drive) {
      try {
        if (typeof device.drive.close === "function") {
          device.drive.close();
        } else if (typeof device.drive.disconnect === "function") {
          device.drive.disconnect();
        }
      } catch (err) {
        console.warn("[DeviceRegistry] Error tearing down drive on delete:", err);
      }
    }
    this.devices.delete(targetKey);
    this.devices.delete(deviceId);
    if (device && device.linkId) this.devices.delete(device.linkId);
    if (device && device.prefix) this.devices.delete(device.prefix);
  }

  list() {
    return [...this.devices.values()];
  }

  getAll() {
    return this.list();
  }
}

export const deviceRegistry = new DeviceRegistry();

if (typeof window !== "undefined") {
  window.deviceRegistry = deviceRegistry;
  // Fallbacks must only pick RPC-capable drives: LocalDrive (browser device
  // host side) is always "connected" and must never be handed out for an
  // UNRELATED deviceId. An exact match is returned as-is - the SW bridge
  // serves local drives natively.
  const hasRpc = (d) => d && typeof d.sendRPC === "function";
  window.__omdGetDrive = (deviceId) => {
    if (deviceId) {
      const dev = deviceRegistry.getByPrefix(deviceId) || deviceRegistry.get(deviceId) || deviceRegistry.getByName(deviceId);
      if (dev && dev.drive) return dev.drive;
    }
    const connected = deviceRegistry.list().map(d => d.drive).find((d) => d && d.isConnected && hasRpc(d));
    if (connected) return connected;
    const any = deviceRegistry.list().map(d => d.drive).find(hasRpc);
    if (any) return any;
    return (window.currentP2PDrive && hasRpc(window.currentP2PDrive)) ? window.currentP2PDrive : null;
  };
}
