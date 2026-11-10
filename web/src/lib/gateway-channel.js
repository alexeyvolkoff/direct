// GatewayChannel — persistent WebSocket (QWebChannel) connection to the OMD
// gateway. Replaces per-second signaling polling for browser devices: the
// gateway pushes tunnel-open / offer / candidate messages via the
// ClientNotifier::webTunnelOpen signal, exactly like the C++ clients receive
// them over their TLS control socket.
//
// Server side: wss://<host>:9988 → WebSocketClientWrapper → ClientNotifier.
// Subscribing with backend.token = omd_key (legacy style) creates the user
// session binding; assigning backend.linkID then registers the web device
// and maps this channel for signaling push.

import { ensureChannelToken } from "./omd-key.js";
import { deviceRegistry } from "./device-registry.js";
import { getOrCreateLocalLinkId } from "./local-client.js";

const WS_URL_OVERRIDE_KEY = "omd_ws_url";

function loadQWebChannelScript() {
  if (typeof window === "undefined") return Promise.reject(new Error("no window"));
  if (window.QWebChannel) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const s = document.createElement("script");
    const loc = window.location;
    const rawHost = loc.hostname || "";
    const isLocal = !rawHost || rawHost === "localhost" || rawHost === "127.0.0.1";
    s.src = isLocal ? "https://disk.direct/qwebchannel/qwebchannel.js" : "/qwebchannel/qwebchannel.js";
    s.onload = () => resolve();
    s.onerror = () => reject(new Error("Failed to load " + s.src));
    document.head.appendChild(s);
  });
}

function defaultGatewayUrl() {
  if (typeof window === "undefined") return "";
  const loc = window.location;
  const rawHost = loc.hostname || "";
  const isLocal = !rawHost || rawHost === "localhost" || rawHost === "127.0.0.1";
  const host = isLocal ? "disk.direct" : rawHost;
  const proto = (isLocal || host === "disk.direct" || loc.protocol === "https:") ? "wss:" : "ws:";
  const port = (typeof import.meta !== "undefined" && import.meta.env && import.meta.env.VITE_SIGNAL_PORT) || "9989";
  return `${proto}//${host}:${port}`;
}

export class GatewayChannel {
  constructor() {
    this.backend = null;
    this.connected = false;
    this._ws = null;
    this._closing = false;
    this._retryTimer = null;
    this._keepaliveTimer = null;
    this._retryAttempt = 0;
    this._linkId = typeof window !== "undefined" ? getOrCreateLocalLinkId() : null;
    this._tunnelOpenHandlers = [];
    this._linkConnectedHandlers = [];
    this._linkDisconnectedHandlers = [];
    this._devicesFetchedHandlers = [];
    this._stateChangeHandlers = [];
    this._connectPromise = null;

    const triggerImmediateReconnect = (reason) => {
      if (this._closing) return;
      const wsDead = !this._ws || (this._ws.readyState !== WebSocket.OPEN && this._ws.readyState !== WebSocket.CONNECTING);
      if (!this.connected || wsDead) {
        console.log(`[GatewayChannel] ${reason}: reconnecting WebSocket immediately`);
        if (this._retryTimer) { clearTimeout(this._retryTimer); this._retryTimer = null; }
        this._retryAttempt = 0;
        this.connected = false;
        this.backend = null;
        this.connect();
      } else if (this._linkId) {
        this.registerDevice(this._linkId);
      }
    };

    if (typeof document !== "undefined") {
      document.addEventListener("visibilitychange", () => {
        if (document.visibilityState === "visible") {
          triggerImmediateReconnect("Foreground wakeup");
        }
      });
    }
    if (typeof window !== "undefined") {
      window.addEventListener("online", () => {
        triggerImmediateReconnect("Network online");
      });
    }
  }

  get isConnected() { return this.connected; }

  onStateChange(cb) { this._stateChangeHandlers.push(cb); }
  onTunnelOpen(cb) { this._tunnelOpenHandlers.push(cb); }
  offTunnelOpen(cb) { this._tunnelOpenHandlers = this._tunnelOpenHandlers.filter(h => h !== cb); }
  onLinkConnected(cb) { this._linkConnectedHandlers.push(cb); }
  onLinkDisconnected(cb) { this._linkDisconnectedHandlers.push(cb); }
  onDevicesFetched(cb) { this._devicesFetchedHandlers.push(cb); }
  async getDevices(targetLinkIds = []) {
    if (!this.connected || !this.backend || typeof this.backend.getDevices !== "function") {
      await this.connect();
    }
    if (!this.backend || typeof this.backend.getDevices !== "function") {
      console.warn("[GatewayChannel] getDevices called but backend is null or missing getDevices method");
      return [];
    }
    try {
      const rawList = await new Promise((resolve) => {
        try {
          this.backend.getDevices(targetLinkIds, (devices) => {
            resolve(devices || []);
          });
        } catch (err) {
          console.warn("[GatewayChannel] getDevices invocation error:", err);
          resolve([]);
        }
      });
      if (Array.isArray(rawList)) {
        for (const cb of this._devicesFetchedHandlers) {
          try { cb(rawList); } catch (e) { console.warn("[GatewayChannel] devicesFetched handler error:", e); }
        }
      }
      return rawList;
    } catch (e) {
      console.warn("[GatewayChannel] Error fetching devices from backend:", e);
      return [];
    }
  }

  async sendSignaling(linkId, tunnelId, kind, payloadB64, port = 0, service = "") {
    if (!this.connected || !this.backend || typeof this.backend.sendSignaling !== "function") {
      await this.connect();
    }
    if (!this.backend || typeof this.backend.sendSignaling !== "function") {
      return false;
    }
    return new Promise((resolve) => {
      try {
        this.backend.sendSignaling(linkId, tunnelId, kind, payloadB64, port, service, (res) => {
          resolve(!!res);
        });
      } catch (err) {
        console.warn("[GatewayChannel] sendSignaling invocation error:", err);
        resolve(false);
      }
    });
  }

  _setState(connected) {
    if (this.connected === connected) return;
    this.connected = connected;
    for (const cb of this._stateChangeHandlers) {
      try { cb(connected); } catch (err) { console.warn("[GatewayChannel] state change handler error:", err); }
    }
  }

  _startKeepalive() {
    this._stopKeepalive();
    this._keepaliveTimer = setInterval(() => {
      if (this._closing) {
        this._stopKeepalive();
        return;
      }
      if (!this._ws || this._ws.readyState !== WebSocket.OPEN) {
        this._stopKeepalive();
        this._setState(false);
        this.backend = null;
        this._scheduleReconnect();
        return;
      }
      if (this._linkId) {
        this.registerDevice(this._linkId);
      }
    }, 25000);
  }

  _stopKeepalive() {
    if (this._keepaliveTimer) {
      clearInterval(this._keepaliveTimer);
      this._keepaliveTimer = null;
    }
  }

  async connect(urlOverride = null) {
    this._closing = false;
    if (this.connected && this.backend && this._ws && this._ws.readyState === WebSocket.OPEN) {
      return true;
    }
    if (this._ws && this._ws.readyState !== WebSocket.OPEN && this._ws.readyState !== WebSocket.CONNECTING) {
      this.connected = false;
      this.backend = null;
    }
    if (this._connectPromise) return this._connectPromise;

    this._connectPromise = (async () => {
      let storedUrl = localStorage.getItem(WS_URL_OVERRIDE_KEY);
      if (storedUrl && storedUrl.includes(":9988")) {
        storedUrl = storedUrl.replace(":9988", ":9989");
        localStorage.setItem(WS_URL_OVERRIDE_KEY, storedUrl);
      }
      const url = urlOverride || storedUrl || defaultGatewayUrl();
      try {
        await loadQWebChannelScript();
      } catch (e) {
        console.warn("[GatewayChannel] qwebchannel.js unavailable:", e.message);
        this._scheduleReconnect();
        this._connectPromise = null;
        return false;
      }

      return new Promise((resolve) => {
        let settled = false;
        const finish = (result) => {
          this._connectPromise = null;
          if (!settled) { settled = true; resolve(result); }
        };

      try {
        if (this._ws) {
          try { this._ws.close(); } catch (_) {}
          this._ws = null;
        }
        this._ws = new WebSocket(url);
        this._ws.addEventListener("message", (ev) => {
          try {
            const d = JSON.parse(ev.data);
            if (d.type === 1 || d.kind || d.tunnelID) {
              console.log("[GatewayWS Message Received]:", d);
            }
          } catch (_) {
            console.log("[GatewayWS Message Raw]:", ev.data);
          }
        });
      } catch (e) {
        console.warn("[GatewayChannel] WebSocket creation failed:", e);
        this._scheduleReconnect();
        finish(false);
        return;
      }

      this._ws.onopen = () => {
        // eslint-disable-next-line no-undef
        new QWebChannel(this._ws, async (channel) => {
          const backend = channel.objects.backend;
          this._retryAttempt = 0;

          // Connect signals synchronously immediately so QWebChannel subscribes without delay
          if (backend && backend.webTunnelOpen) {
            backend.webTunnelOpen.connect((sig) => {
              console.log("[GatewayChannel] backend.webTunnelOpen signal received:", sig);
              for (const cb of this._tunnelOpenHandlers) {
                try { cb(sig); } catch (err) { console.warn("[GatewayChannel] handler error:", err); }
              }
            });
          } else {
            console.warn("[GatewayChannel] backend.webTunnelOpen signal not available on backend");
          }

          if (backend && backend.linkConnected) {
            backend.linkConnected.connect((linkId) => {
              for (const cb of this._linkConnectedHandlers) {
                try { cb(linkId); } catch (err) { console.warn("[GatewayChannel] linkConnected handler error:", err); }
              }
            });
          }

          if (backend && backend.linkDisconnected) {
            backend.linkDisconnected.connect((linkId) => {
              for (const cb of this._linkDisconnectedHandlers) {
                try { cb(linkId); } catch (err) { console.warn("[GatewayChannel] linkDisconnected handler error:", err); }
              }
            });
          }

          // Subscribe BEFORE exposing this.backend so no calls can execute before registration is complete
          try {
            const token = await ensureChannelToken();
            if (backend && typeof backend.subscribeChannel === "function") {
              await new Promise((resolve) => {
                const timer = setTimeout(() => {
                  console.warn("[GatewayChannel] subscribeChannel timed out after 5s");
                  resolve(false);
                }, 5000);
                backend.subscribeChannel(token || "", this._linkId || "", (ok) => {
                  clearTimeout(timer);
                  console.log("[GatewayChannel] Channel registered on server:", ok);
                  resolve(ok);
                });
              });
            } else {
              if (token && backend && "token" in backend) backend.token = token;
              if (this._linkId && backend) backend.linkID = this._linkId;
            }
          } catch (e) {
            console.warn("[GatewayChannel] channel subscription failed:", e);
          }

          // Expose backend and mark connected ONLY AFTER subscription is 100% complete
          this.backend = backend;
          this._setState(true);
          this._startKeepalive();
          finish(true);
        });
      };

      this._ws.onclose = () => {
        const wasConnected = this.connected;
        this._stopKeepalive();
        this._setState(false);
        this.backend = null;
        if (!this._closing) {
          if (wasConnected) console.warn("[GatewayChannel] WebSocket closed, reconnecting...");
          this._scheduleReconnect();
        }
        finish(false);
      };

      this._ws.onerror = () => { /* onclose follows */ };
      });
    })();
    return this._connectPromise;
  }

  _scheduleReconnect() {
    if (this._closing || this._retryTimer) return;
    const delay = Math.min(30000, 1000 * Math.pow(2, this._retryAttempt++));
    this._retryTimer = setTimeout(() => {
      this._retryTimer = null;
      this.connect();
    }, delay);
  }

  setDeviceId(linkId) {
    this._linkId = linkId;
    this.registerDevice(linkId);
  }

  registerDevice(linkId) {
    if (!this.backend) return false;
    try {
      this.backend.linkID = linkId; // Q_PROPERTY write → setLinkId → gateway maps channel to linkId
      return true;
    } catch (e) {
      console.warn("[GatewayChannel] registerDevice failed:", e);
      return false;
    }
  }

  close() {
    this._closing = true;
    this._stopKeepalive();
    if (this._retryTimer) { clearTimeout(this._retryTimer); this._retryTimer = null; }
    try { if (this._ws) this._ws.close(); } catch (_) {}
    this._ws = null;
    this.backend = null;
    this._setState(false);
  }
}

let _sharedGatewayChannel = null;
export function getSharedGatewayChannel() {
  if (!_sharedGatewayChannel) {
    _sharedGatewayChannel = new GatewayChannel();
  }
  return _sharedGatewayChannel;
}
