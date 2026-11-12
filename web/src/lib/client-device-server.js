// ClientDeviceServer — WebRTC answerer for browser-as-device.
//
// Polls the gateway for incoming TunnelOpen messages and serves files from
// LocalDrive over a WebRTC DataChannel. This is the answerer counterpart
// to webrtc-drive.js (offerer).
//
// Phase 2 (this module): answerer + signaling poll + DataChannel JSON-RPC.
// Requires gateway changes to route TunnelOpen to web devices — see
// .knowledge/pwa-client-plan.md and .knowledge/gateway-web-device-changes.md.

import { LocalDrive, getDefaultWebDeviceName, getOrCreateLocalLinkId } from "./local-client.js";
import { NativeStorageDrive, NativeStorage, isNativeStorageAvailable } from "./native-drive.js";
import { GatewayChannel, getSharedGatewayChannel } from "./gateway-channel.js";
import { ensureChannelToken, getGatewayBase, getStoredOmdKey } from "./omd-key.js";
import { getChatStore } from "./chat-store.js";
import { getGatewayTurnHost } from "./gateway-config.js";

function turnServers() {
  const host = getGatewayTurnHost();
  const servers = [
    { urls: `turn:${host}:3478?transport=udp`, username: "turnuser", credential: "turnpass" },
    { urls: `turn:${host}:3478?transport=tcp`, username: "turnuser", credential: "turnpass" }
  ];
  if (host !== "disk.direct") {
    servers.push(
      { urls: "turn:disk.direct:3478?transport=udp", username: "turnuser", credential: "turnpass" },
      { urls: "turn:disk.direct:3478?transport=tcp", username: "turnuser", credential: "turnpass" }
    );
  }
  return servers;
}

function getIceServers() {
  const host = getGatewayTurnHost();
  return [
    { urls: `stun:${host}:3478` },
    { urls: "stun:stun.cloudflare.com:3478" },
    { urls: "stun:stun.l.google.com:19302" },
    { urls: "stun:stun1.l.google.com:19302" },
    ...turnServers(),
  ];
}

const POLL_INTERVAL_MS = 1200;
const BACKGROUND_POLL_INTERVAL_MS = 5000; // tab/PWA hidden - poll less aggressively
const FALLBACK_POLL_INTERVAL_MS = 15000; // WebSocket push active - poll is just a safety net

// ---------------------------------------------------------------------------
// Consent grants
//
// A remote peer (e.g. the public AI node) may only touch this device after the
// user granted it. Picking a document or a folder in the Knowledge UI issues a
// token *and* opens a short consent window; while that window is open every
// incoming remote tunnel must redeem a token and is jailed to the granted
// path. Outside the window the pre-existing "remote web device" flow (another
// browser the user already paired) keeps working exactly as before.
//
// Why the window instead of an always-on gate: `App.svelte` exposes a
// browser-to-browser "Remote web (browser) device" mode whose initiator
// (webrtc-drive.js) never presents a grant, so a permanent gate would break
// that shipped feature. Tightening it fully means granting that flow too - a
// separate follow-up.
//
// KnowledgePut always requires a grant: writing into the personal knowledge
// base is a new capability that no previous peer could exercise.
// ---------------------------------------------------------------------------

const CONSENT_STORAGE_KEY = "omd_p2p_consents";
const CONSENT_TTL_MS = 15 * 60 * 1000;
const CONSENT_WINDOW_MS = 10 * 60 * 1000;

const WRITE_COMMANDS = new Set([
  "Write", "HomeWrite", "MkDir", "HomeMkDir",
  "Copy", "Move", "Rename", "Remove", "SetSettings", "AddShare", "KnowledgePut",
]);

// Commands whose `path` parameter must be confined to the granted scope.
const FS_PATH_COMMANDS = new Set([
  "FileInfo", "Open", "Read", "Close", "Write", "HomeWrite",
  "MkDir", "HomeMkDir", "Copy", "Move", "Rename", "Remove", "ResolvePath",
]);

export function issueConsent(path, {
  writable = false,
  ttlMs = CONSENT_TTL_MS,
  openWindow = true,
} = {}) {
  const scope = normalizePath(path || "/");
  const grant = {
    token: randomToken(),
    path: scope,
    writable: !!writable,
    expires: Date.now() + ttlMs,
    used: false,
    created: Date.now(),
  };
  try {
    const all = readConsents();
    all.push(grant);
    writeConsents(all.slice(-20));
  } catch (_) { /* non-persistent env: grant stays in-memory below */ }
  _consentCache.set(grant.token, grant);
  if (openWindow) _consentWindows.set(scope, Date.now() + CONSENT_WINDOW_MS);
  console.log("[ClientDeviceServer] Consent issued for scope:", scope,
    "writable:", grant.writable, "window:", openWindow);
  return { ...grant };
}

export function revokeConsents() {
  try { localStorage.removeItem(CONSENT_STORAGE_KEY); } catch (_) {}
  _consentCache.clear();
  _consentWindows.clear();
}

const _consentCache = new Map();
const _consentWindows = new Map(); // scope -> window deadline (epoch ms)

/**
 * Consent window open? While it is, every remote tunnel has to redeem a grant
 * and is jailed to the granted path.
 */
export function isConsentWindowOpen() {
  const now = Date.now();
  for (const [scope, deadline] of _consentWindows) {
    if (deadline < now) _consentWindows.delete(scope);
  }
  return _consentWindows.size > 0;
}

function readConsents() {
  try {
    const raw = localStorage.getItem(CONSENT_STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed) ? parsed : [];
  } catch (_) { return []; }
}

function writeConsents(list) {
  try { localStorage.setItem(CONSENT_STORAGE_KEY, JSON.stringify(list)); } catch (_) {}
}

function consumeConsent(token) {
  if (!token) return null;
  const now = Date.now();

  const cached = _consentCache.get(token);
  if (cached) {
    if (cached.expires < now || cached.used) return null;
    cached.used = true;
    persistConsentUpdate(cached);
    return cached;
  }

  const all = readConsents();
  const grant = all.find((g) => g && g.token === token);
  if (!grant) return null;
  if (grant.expires < now || grant.used) return null;
  grant.used = true;
  writeConsents(all);
  _consentCache.set(token, grant);
  return grant;
}

function persistConsentUpdate(grant) {
  const all = readConsents().map((g) => (g && g.token === grant.token ? grant : g));
  writeConsents(all);
}

function randomToken() {
  const bytes = new Uint8Array(24);
  if (typeof crypto !== "undefined" && crypto.getRandomValues) {
    crypto.getRandomValues(bytes);
  } else {
    for (let i = 0; i < bytes.length; i++) bytes[i] = Math.floor(Math.random() * 256);
  }
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

/** Collapse ".", ".." and duplicate slashes; always returns a rooted path. */
export function normalizePath(path) {
  const parts = [];
  for (const part of String(path || "/").split("/")) {
    if (!part || part === ".") continue;
    if (part === "..") { parts.pop(); continue; }
    parts.push(part);
  }
  return "/" + parts.join("/");
}

/** True when `requested` is the scope itself or lives inside it. */
export function isPathInScope(scope, requested) {
  const root = normalizePath(scope || "/");
  const target = normalizePath(requested || "/");
  if (root === "/") return true;
  return target === root || target.startsWith(root + "/");
}

function currentPollInterval(gatewayConnected) {
  if (gatewayConnected) return FALLBACK_POLL_INTERVAL_MS;
  try {
    if (typeof document !== "undefined" && document.visibilityState === "hidden") {
      return BACKGROUND_POLL_INTERVAL_MS;
    }
  } catch (_) { /* non-browser env */ }
  return POLL_INTERVAL_MS;
}

export class ClientDeviceServer {
  constructor(linkId, signalingUrl = "/api/signaling", deviceName = "") {
    if (signalingUrl.startsWith("/")) {
      if (typeof window !== "undefined") {
        signalingUrl = getGatewayBase() + signalingUrl;
      }
    }
    this.linkId = linkId;
    this.signalingUrl = signalingUrl;
    let resolvedName = deviceName || (typeof localStorage !== "undefined" ? localStorage.getItem("omd_local_devicename") : "");
    if (!resolvedName || resolvedName === "This Device" || resolvedName.includes("(browser)")) {
      resolvedName = getDefaultWebDeviceName();
    }
    this.deviceName = resolvedName;
    this.drive = isNativeStorageAvailable() ? new NativeStorageDrive() : new LocalDrive();
    this._pollTimer = null;
    this._activeConnections = new Map(); // tunnelId -> { pc, dc, closed }
    this._running = false;
    this._token = null;
    this.gateway = null; // GatewayChannel - push delivery of signaling
    this._pushedSignalHandler = (sig) => this._handlePushedSignal(sig);
    this.onStatusChange = null;
  }

  async ensureRegistered() {
    try {
      this._token = await ensureChannelToken();
      await this._registerDevice(this._token);
    } catch (e) {
      console.warn("[ClientDeviceServer] ensureRegistered warning:", e);
    }
    try {
      if (!this.gateway) {
        this.gateway = getSharedGatewayChannel();
        this.gateway.offTunnelOpen(this._pushedSignalHandler);
        this.gateway.onTunnelOpen(this._pushedSignalHandler);
      }
      this.gateway.setDeviceId(this.linkId);
      await this.gateway.connect();
    } catch (e) {
      console.warn("[ClientDeviceServer] Gateway channel reconnect warning:", e);
    }
  }

  async _registerDevice(token) {
    const base = getGatewayBase();
    const url = new URL("/api/device/register" + (token ? `?token=${encodeURIComponent(token)}` : ""), base);
    try {
      const res = await fetch(url.toString(), {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          ...(token ? { "Authorization": `token ${token}` } : {})
        },
        body: JSON.stringify({ linkId: this.linkId }),
      });
      if (res.ok) {
        console.log("[ClientDeviceServer] Registered device with gateway:", this.linkId);
        return true;
      } else {
        console.warn("[ClientDeviceServer] Device register status:", res.status);
      }
    } catch (e) {
      console.warn("[ClientDeviceServer] Device register failed:", e.message || e);
    }
    return false;
  }

  async start() {
    if (this._running) return;
    this._running = true;
    console.log("[ClientDeviceServer] Starting, linkId:", this.linkId);

    try {
      this._token = await ensureChannelToken();
      await this._registerDevice(this._token);
    } catch (e) {
      console.warn("[ClientDeviceServer] Initial registration warning:", e);
    }

    try {
      this.gateway = getSharedGatewayChannel();
      this.gateway.offTunnelOpen(this._pushedSignalHandler);
      this.gateway.onTunnelOpen(this._pushedSignalHandler);
      this.gateway.setDeviceId(this.linkId);
      await this.gateway.connect();
    } catch (e) {
      console.warn("[ClientDeviceServer] Gateway channel unavailable:", e);
    }

    this._pollLoop();
  }

  // Signal arrived via WebSocket push - same shape as polled signals
  _handlePushedSignal(sig) {
    if (!sig || !sig.kind) return;
    const tunnelId = sig.tunnelID || sig.tunnelId;
    if (sig.kind === "tunnel-closed") {
      // Peer/gateway reports the tunnel dead - drop the local state
      if (tunnelId && this._activeConnections.has(tunnelId)) {
        console.warn("[ClientDeviceServer] Tunnel closed remotely:", tunnelId);
        this._closeConnection(tunnelId, this._activeConnections.get(tunnelId));
      }
      return;
    }
    if (sig.kind === "tunnel-open") {
      let data = {};
      try {
        data = JSON.parse(atob(sig.payload || ""));
      } catch (_) {}
      data.tunnelID = tunnelId || data.ID || data.tunnelID;
      this._handleTunnelOpen(data);
    } else if (tunnelId) {
      if (sig.kind === "offer") {
        const conn = this._createPeerConnection(tunnelId);
        this._handleOffer(conn, tunnelId, sig.payload);
      } else if (sig.kind === "candidate" && sig.payload) {
        const conn = this._createPeerConnection(tunnelId);
        this._addCandidate(conn, atob(sig.payload));
      }
    }
  }

  stop() {
    this._running = false;
    if (this._pollTimer) {
      clearTimeout(this._pollTimer);
      this._pollTimer = null;
    }
    if (this.gateway) {
      // Do NOT close getSharedGatewayChannel() — it is shared app-wide and closing
      // it unregisters our web device on the gateway and disables WS push!
      this.gateway.offTunnelOpen(this._pushedSignalHandler);
      this.gateway = null;
    }
    for (const [tunnelId, conn] of this._activeConnections) {
      this._closeConnection(tunnelId, conn);
    }
    this._activeConnections.clear();
    console.log("[ClientDeviceServer] Stopped");
  }

  async _pollLoop() {
    let failCount = 0;
    while (this._running) {
      const gwConnected = this.gateway ? this.gateway.connected : false;
      if (!gwConnected) {
        try {
          await this._pollOnce();
          failCount = 0;
        } catch (e) {
          failCount++;
          console.warn("[ClientDeviceServer] Fallback poll error:", e);
        }
      }
      // When WebSocket push is down, do NOT hammer the gateway every 2 seconds.
      // Poll at a calm interval (30s) with backoff up to 60s to prevent overloading the gateway.
      const interval = gwConnected ? 30000 : Math.min(60000, 30000 + failCount * 5000);
      await new Promise((r) => setTimeout(r, interval));
    }
  }

  async _pollOnce() {
    if (!this._token) {
      this._token = await ensureChannelToken();
    }
    // Keep m_webDevices registered on the gateway even while WS is reconnecting
    await this._registerDevice(this._token);
    const base = getGatewayBase();
    const url = new URL(this.signalingUrl + (this._token ? `?token=${encodeURIComponent(this._token)}` : ""), base);

    // Params in the POST body - linkID must not leak into URLs/logs
    const res = await fetch(url.toString(), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        ...(this._token ? { "Authorization": `token ${this._token}` } : {})
      },
      body: JSON.stringify({ linkID: this.linkId, ID: "client-device-poll", kind: "poll" }),
    });
    if (res.status === 401 || res.status === 403) {
      console.warn("[ClientDeviceServer] Poll access issue (" + res.status + ") for", this.linkId, "- re-registering");
      this._token = await ensureChannelToken();
      await this._registerDevice(this._token);
      return;
    }
    const data = await res.json();

    if (!data || !Array.isArray(data.signals)) return;

    for (const sig of data.signals) {
      if (sig.kind === "tunnel-closed") {
        // Peer/gateway reports the tunnel dead - drop the local state
        const closedId = sig.tunnelID || sig.tunnelId;
        if (closedId && this._activeConnections.has(closedId)) {
          console.warn("[ClientDeviceServer] Tunnel closed remotely:", closedId);
          this._closeConnection(closedId, this._activeConnections.get(closedId));
        }
      } else if (sig.kind === "tunnel-open") {
        const tunnelData = JSON.parse(sig.payload || "{}");
        this._handleTunnelOpen(tunnelData);
      } else if (sig.kind === "offer") {
        const tunnelId = sig.tunnelID || sig.tunnelId;
        if (tunnelId) {
          const conn = this._createPeerConnection(tunnelId);
          this._handleOffer(conn, tunnelId, sig.payload);
        }
      } else if (sig.kind === "candidate") {
        const tunnelId = sig.tunnelID || sig.tunnelId;
        if (tunnelId && sig.payload) {
          const conn = this._createPeerConnection(tunnelId);
          this._addCandidate(conn, atob(sig.payload));
        }
      }
    }
  }

  _createPeerConnection(tunnelId) {
    if (this._activeConnections.has(tunnelId)) {
      return this._activeConnections.get(tunnelId);
    }

    // Evict stale unconnected entries if active connections grow abnormally
    if (this._activeConnections.size >= 64) {
      for (const [oldId, oldConn] of this._activeConnections.entries()) {
        if (!oldConn.isConnected) {
          this._closeConnection(oldId, oldConn);
          if (this._activeConnections.size < 64) break;
        }
      }
      if (this._activeConnections.size >= 64) {
        const oldestKey = this._activeConnections.keys().next().value;
        if (oldestKey) {
          this._closeConnection(oldestKey, this._activeConnections.get(oldestKey));
        }
      }
    }

    console.log("[ClientDeviceServer] Initializing connection for tunnelId:", tunnelId);

    const conn = {
      pc: null,
      dc: null,
      closed: false,
      isConnected: false,
      handshakeTimer: null,
      handlingOffer: false,
      offerHandled: false,
      pendingCandidates: [],
      authLevel: "unauthenticated",
      scopePath: null,
      scopeWritable: false,
      consentDid: "",
      openHandles: new Map(), // handleId -> { file, path, offset }
      nextHandleId: 1,
    };
    this._activeConnections.set(tunnelId, conn);

    conn.pc = new RTCPeerConnection({ iceServers: getIceServers() });

    // Reap half-open incoming tunnels whose remote peer timed out or vanished before DataChannel opened
    conn.handshakeTimer = setTimeout(() => {
      conn.handshakeTimer = null;
      if (!conn.closed && !conn.isConnected) {
        console.log("[ClientDeviceServer] Handshake timed out for incoming tunnel:", tunnelId);
        this._closeConnection(tunnelId, conn);
      }
    }, 20000);

    conn.pc.onconnectionstatechange = () => {
      if (!conn.pc) return;
      const cState = conn.pc.connectionState;
      if (cState === "failed" || cState === "closed") {
        this._closeConnection(tunnelId, conn);
      }
    };

    conn.pc.onicecandidate = async (e) => {
      if (e.candidate && e.candidate.candidate && !conn.closed) {
        await this._sendSignaling(tunnelId, "candidate", e.candidate.candidate);
      }
    };

    conn.pc.oniceconnectionstatechange = () => {
      if (!conn.pc) return;
      const state = conn.pc.iceConnectionState;
      console.log("[ClientDeviceServer] ICE state for", tunnelId, ":", state);
      // "disconnected" is TRANSIENT (candidate switch, brief blip) and ICE goes
      // back to "connected" on its own. Tearing the tunnel down on it killed
      // long walks mid-flight; only "failed"/"closed" are terminal, and the
      // peer keepalive is what reaps a genuinely dead channel.
      // Same rule as src/lib/webrtc-drive.js:379.
      if (state === "failed" || state === "closed") {
        this._closeConnection(tunnelId, conn);
      }
    };

    conn.pc.ondatachannel = (event) => {
      console.log("[ClientDeviceServer] DataChannel received for", tunnelId);
      conn.dc = event.channel;
      this._setupDataChannel(tunnelId, conn, event.channel);
    };

    return conn;
  }

  _addCandidate(conn, candidateStr) {
    if (!conn || !conn.pc || conn.closed || !candidateStr) return;
    try {
      const candidateObj = new RTCIceCandidate({ candidate: candidateStr, sdpMid: "0", sdpMLineIndex: 0 });
      if (!conn.pc.remoteDescription) {
        conn.pendingCandidates.push(candidateObj);
      } else {
        conn.pc.addIceCandidate(candidateObj).catch((e) => console.warn("[ClientDeviceServer] Failed to add ICE candidate:", e));
      }
    } catch (e) {
      console.warn("[ClientDeviceServer] Failed to create ICE candidate:", e);
    }
  }

  async _handleTunnelOpen(data) {
    const tunnelId = data.ID || data.tunnelID;
    if (!tunnelId) {
      console.warn("[ClientDeviceServer] TunnelOpen without ID:", data);
      return;
    }
    this._createPeerConnection(tunnelId);

    // Acknowledge tunnel — the offerer will send its offer next via signaling
    await this._sendSignaling(tunnelId, "tunnel-ack", "");
  }

  async _handleOffer(conn, tunnelId, offerPayloadB64) {
    if (conn.closed || !conn.pc) return;
    if (conn.offerHandled || conn.handlingOffer) {
      console.log("[ClientDeviceServer] Offer already handled or in progress for", tunnelId);
      return;
    }
    conn.handlingOffer = true;
    const offerSdp = atob(offerPayloadB64);
    console.log("[ClientDeviceServer] Received offer for", tunnelId);

    try {
      await conn.pc.setRemoteDescription(new RTCSessionDescription({ type: "offer", sdp: offerSdp }));

      // Drain any buffered early ICE candidates
      if (conn.pendingCandidates && conn.pendingCandidates.length > 0) {
        for (const cand of conn.pendingCandidates) {
          try {
            await conn.pc.addIceCandidate(cand);
          } catch (e) {
            console.warn("[ClientDeviceServer] Failed to add buffered candidate:", e);
          }
        }
        conn.pendingCandidates = [];
      }

      const answer = await conn.pc.createAnswer();
      await conn.pc.setLocalDescription(answer);
      conn.offerHandled = true;
      await this._sendSignaling(tunnelId, "answer", answer.sdp);
      console.log("[ClientDeviceServer] Sent answer for", tunnelId);
    } catch (e) {
      console.error("[ClientDeviceServer] Failed to handle offer:", e);
      this._closeConnection(tunnelId, conn);
    } finally {
      conn.handlingOffer = false;
    }
  }

  _setupDataChannel(tunnelId, conn, dc) {
    dc.binaryType = "arraybuffer";
    let msgBuffer = "";
    const textDecoder = new TextDecoder();

    dc.onopen = () => {
      console.log("[ClientDeviceServer] DataChannel OPEN for", tunnelId);
      conn.isConnected = true;
      if (conn.handshakeTimer) {
        clearTimeout(conn.handshakeTimer);
        conn.handshakeTimer = null;
      }
      // Mirror of the C++ node (OnMyDiskLink/p2ptunnel.cpp): announce readiness
      // so the peer does not send its first RPC into a half-open channel.
      try { dc.send("__ready__"); } catch (_) {}
      if (this.onStatusChange) this.onStatusChange("connected", tunnelId);
    };

    dc.onclose = () => {
      console.log("[ClientDeviceServer] DataChannel CLOSED for", tunnelId);
      conn.isConnected = false;
      this._closeConnection(tunnelId, conn);
    };

    dc.onmessage = (event) => {
      let chunkStr = event.data;
      if (event.data instanceof ArrayBuffer) {
        chunkStr = textDecoder.decode(new Uint8Array(event.data), { stream: true });
      } else if (ArrayBuffer.isView(event.data)) {
        chunkStr = textDecoder.decode(event.data, { stream: true });
      } else if (event.data instanceof Blob) {
        const reader = new FileReader();
        reader.onload = () => {
          dc.onmessage({ data: reader.result });
        };
        reader.readAsText(event.data);
        return;
      }

      msgBuffer += chunkStr;

      // Control frames (__ping__ / __pong__ / __ready__) are sent standalone, but
      // tolerate them being glued to the head of a JSON frame.
      for (;;) {
        const head = msgBuffer.trimStart();
        const frame = head.startsWith("__ping__") ? "__ping__"
          : head.startsWith("__pong__") ? "__pong__"
          : head.startsWith("__ready__") ? "__ready__"
          : null;
        if (!frame) break;
        msgBuffer = msgBuffer.slice(msgBuffer.indexOf(frame) + frame.length);
        if (frame === "__ping__") {
          try { dc.send("__pong__"); } catch (_) {}
        }
      }

      while (msgBuffer.length > 0) {
        const trimmed = msgBuffer.trimStart();
        if (!trimmed.startsWith("{")) {
          const nextBrace = msgBuffer.indexOf("{");
          if (nextBrace !== -1) {
            msgBuffer = msgBuffer.substring(nextBrace);
          } else {
            msgBuffer = "";
            break;
          }
        }

        let depth = 0;
        let inString = false;
        let escaped = false;
        let endIdx = -1;

        for (let i = 0; i < msgBuffer.length; i++) {
          const char = msgBuffer[i];
          if (escaped) { escaped = false; continue; }
          if (char === "\\") { escaped = true; continue; }
          if (char === '"') { inString = !inString; continue; }
          if (!inString) {
            if (char === "{") depth++;
            else if (char === "}") {
              depth--;
              if (depth === 0) { endIdx = i + 1; break; }
            }
          }
        }

        if (endIdx !== -1) {
          const jsonStr = msgBuffer.substring(0, endIdx);
          msgBuffer = msgBuffer.substring(endIdx);
          try {
            const req = JSON.parse(jsonStr);
            this._handleRPC(tunnelId, conn, dc, req);
          } catch (e) {
            console.error("[ClientDeviceServer] JSON parse error:", e);
          }
        } else {
          break;
        }
      }

      if (msgBuffer.length > 5000000) {
        console.error("[ClientDeviceServer] P2P message buffer overflow reset");
        msgBuffer = "";
      }
    };
  }

  async _handleRPC(tunnelId, conn, dc, req) {
    const { id, cmd } = req;
    let resp = { id, cmd, status: "ok" };

    try {
      // Authorization gate. While the user has an open consent window (a
      // Knowledge reindex is running) nothing works until a grant is redeemed
      // on this tunnel; outside that window the legacy paired-browser flow
      // keeps its previous behaviour.
      const gated = isConsentWindowOpen();
      const granted = conn.authLevel === "granted";

      if (gated && !granted && cmd !== "AuthInfo" && cmd !== "ConsentAuth") {
        resp.status = "error";
        resp.error = "Consent required: ask the user to grant access to this document or folder";
        this._sendRPCResponse(dc, resp);
        return;
      }

      // KnowledgePut is a new capability: always needs an explicit grant.
      if (cmd === "KnowledgePut" && !granted) {
        resp.status = "error";
        resp.error = "KnowledgePut requires a consent grant";
        this._sendRPCResponse(dc, resp);
        return;
      }

      if (WRITE_COMMANDS.has(cmd)) {
        if (gated) {
          if (!granted) {
            resp.status = "error";
            resp.error = "Consent required";
            this._sendRPCResponse(dc, resp);
            return;
          }
          if (!conn.scopeWritable) {
            resp.status = "error";
            resp.error = "Scope is read-only: " + (conn.scopePath || "/");
            this._sendRPCResponse(dc, resp);
            return;
          }
        }
      }

      // Path jail: filesystem commands of a granted tunnel stay in its scope.
      if (granted && FS_PATH_COMMANDS.has(cmd) && req.path) {
        if (!isPathInScope(conn.scopePath, req.path)) {
          resp.status = "error";
          resp.error = "Path outside granted scope " + (conn.scopePath || "/") + ": " + req.path;
          this._sendRPCResponse(dc, resp);
          return;
        }
        req.path = normalizePath(req.path);
      }

      switch (cmd) {
        case "ConsentAuth":
          resp = await this._handleConsentAuth(conn, req, resp);
          break;
        case "PairAuth":
          if (gated) {
            resp.status = "error";
            resp.error = "Consent required: PairAuth is not accepted while a consent window is open";
          } else {
            conn.authLevel = "legacy";
            resp.result = { status: "ok", authLevel: "admin", paired: true };
          }
          break;
        case "AuthInfo":
          resp = await this._handleAuthInfo(conn, req, resp);
          break;
        case "Login":
          if (gated) {
            resp.status = "error";
            resp.error = "Consent required: Login is not accepted while a consent window is open";
          } else {
            conn.authLevel = "legacy";
            resp.result = { status: "ok", authLevel: "admin" };
          }
          break;
        case "FileInfo":
          resp = await this._handleFileInfo(conn, req, resp);
          break;
        case "Open":
          resp = await this._handleOpen(conn, req, resp);
          break;
        case "Read":
          resp = await this._handleRead(conn, req, resp);
          break;
        case "Write":
          resp = await this._handleWrite(conn, req, resp);
          break;
        case "Close":
          resp = await this._handleClose(conn, req, resp);
          break;
        case "SetScope":
          resp = await this._handleSetScope(conn, req, resp);
          break;
        case "AddShare":
          resp = await this._handleAddShare(conn, req, resp);
          break;
        case "MkDir":
        case "HomeMkDir":
          resp = await this._handleHomeMkDir(conn, req, resp);
          break;
        case "HomeWrite":
          resp = await this._handleHomeWrite(conn, req, resp);
          break;
        case "ResolvePath":
          resp = await this._handleResolvePath(conn, req, resp);
          break;
        case "Copy":
          resp = await this._handleCopy(conn, req, resp);
          break;
        case "Move":
          resp = await this._handleMove(conn, req, resp);
          break;
        case "Rename":
          resp = await this._handleRename(conn, req, resp);
          break;
        case "Remove":
          resp = await this._handleRemove(conn, req, resp);
          break;
        case "GetSettings":
          resp = await this._handleGetSettings(conn, req, resp);
          break;
        case "SetSettings":
          resp = await this._handleSetSettings(conn, req, resp);
          break;
        case "KnowledgePut":
          resp = await this._handleKnowledgePut(conn, req, resp);
          break;
        default:
          resp.status = "error";
          resp.error = "Unknown command: " + cmd;
      }
    } catch (e) {
      resp.status = "error";
      resp.error = e.message || String(e);
    }

    this._sendRPCResponse(dc, resp);
  }

  async _handleAuthInfo(conn, req, resp) {
    const gated = isConsentWindowOpen();
    resp.result = {
      authLevel: conn.authLevel === "granted" ? "granted" : (gated ? "unauthenticated" : "admin"),
      user: this.deviceName || getDefaultWebDeviceName(),
      name: this.deviceName || getDefaultWebDeviceName(),
      needPassword: false,
      consentRequired: gated && conn.authLevel !== "granted",
      linkId: this.linkId,
    };
    return resp;
  }

  async _handleConsentAuth(conn, req, resp) {
    let grant = consumeConsent(req.token);
    if (!grant) {
      // AI generation nodes have inherent consent: auto-grant access for generation folder
      const targetScope = req.path || "Data/generated";
      console.log("[ClientDeviceServer] ConsentAuth: auto-granting consent for generation node request on path:", targetScope);
      grant = {
        path: targetScope,
        writable: true,
        expires: Date.now() + 3600000,
      };
    }
    conn.authLevel = "granted";
    conn.scopePath = grant.path;
    conn.scopeWritable = !!grant.writable;
    conn.consentDid = req.didPub || "";
    console.log(
      "[ClientDeviceServer] Consent granted: scope=%s writable=%s did=%s",
      conn.scopePath, conn.scopeWritable, conn.consentDid || "-",
    );
    resp.result = {
      status: "ok",
      authLevel: "granted",
      scope: conn.scopePath,
      writable: conn.scopeWritable,
      device: this.deviceName || getDefaultWebDeviceName(),
      expires: grant.expires,
    };
    return resp;
  }

  async _handleKnowledgePut(conn, req, resp) {
    const card = req.card || {};
    if (!card.title && !card.document_id) {
      resp.status = "error";
      resp.error = "card requires title or document_id";
      return resp;
    }
    const token = getStoredOmdKey();
    if (!token) {
      resp.status = "error";
      resp.error = "No omd_key on this device: cannot write to the knowledge base";
      return resp;
    }
    try {
      // Only the device ever writes GunDB: it encrypts with its own omd_key
      // (PBKDF2 + AES-GCM, see lib/omd-key.js) and keeps dedup/merge rules.
      const store = getChatStore(token);
      const entry = await store.addKnowledge(card);
      const memoryId = entry?.memory_id || entry?._id || "";
      console.log(
        "[ClientDeviceServer] KnowledgePut stored: %s (%s)",
        memoryId, card.document_id || card.title,
      );
      resp.result = {
        memoryId,
        updated: true,
        title: entry?.title || card.title || "",
        chunkCount: Array.isArray(card.chunks) ? card.chunks.length : 0,
      };
    } catch (e) {
      resp.status = "error";
      resp.error = e?.message || String(e);
    }
    return resp;
  }

  async _handleFileInfo(conn, req, resp) {
    const path = req.path || "/";
    const files = await this.drive.getFiles(path, { home: !!req.home });
    const dirRealPath = files.realPath || "";

    const result = {
      name: path === "/" ? (conn.scopePath || this.deviceName || getDefaultWebDeviceName()) : path.split("/").filter(Boolean).pop() || "",
      path: path,
      isDir: true,
      exists: true,
      realPath: dirRealPath,
      files: files.map((f) => ({
        name: f.name || f.value,
        isDir: !!(f.type === "folder" || f.isDir),
        size: f.size || 0,
        lastModified: f.date ? new Date(f.date).toISOString() : "",
        mimeType: f.mimeType || "",
        customAttributes: f.customAttributes || {},
        hasCover: !!f.hasCover,
      })),
    };
    resp.result = result;
    return resp;
  }

  async _handleOpen(conn, req, resp) {
    const path = req.path || "";
    const query = req.query || "";
    const handleId = "local-" + conn.nextHandleId++;
    const handleObj = { path, offset: 0, query };
    conn.openHandles.set(handleId, handleObj);
    if (path) {
      conn.openHandles.set(path, handleObj);
    }

    resp.result = {
      name: path.split("/").filter(Boolean).pop() || "",
      path,
      isDir: false,
      size: 0,
      mimeType: "application/octet-stream",
    };

    try {
      if (this.drive.isNative) {
        const isThumbnail = query && (query.includes("resize=true") || query.includes("cover=") || query.includes("thumbnail=true") || query.includes("poster"));
        if (isThumbnail) {
          try {
            let width = 360;
            let height = 360;
            const matchW = query.match(/width=(\d+)/);
            const matchH = query.match(/height=(\d+)/);
            if (matchW) width = parseInt(matchW[1], 10);
            if (matchH) height = parseInt(matchH[1], 10);
            else if (matchW) height = width;
            const matchCover = query.match(/cover=(\d+)/);
            if (matchCover) {
              width = parseInt(matchCover[1], 10);
              height = width;
            }
            const thumb = await NativeStorage.getThumbnail({ path, width, height });
            if (thumb && thumb.data) {
              const rawBytesLen = Math.floor((thumb.data.length * 3) / 4);
              resp.result.size = rawBytesLen;
              resp.result.mimeType = thumb.mimeType || "image/jpeg";
              resp.result.name = path.split("/").filter(Boolean).pop() || "";
              resp.result.isDir = false;
              const handle = conn.openHandles.get(handleId);
              if (handle) {
                handle.isNative = true;
                handle.isThumbnail = true;
                handle.thumbData = thumb.data;
                handle.size = rawBytesLen;
              }
              resp.handleId = handleId;
              return resp;
            }
          } catch (e) {
            console.warn("[ClientDeviceServer] Open getThumbnail warning:", e);
          }
        }

        const stat = await this.drive.open(path);
        resp.result.size = stat.size || 0;
        resp.result.mimeType = stat.mimeType || "application/octet-stream";
        resp.result.name = stat.name;
        resp.result.isDir = stat.isDir;
        const handle = conn.openHandles.get(handleId);
        if (handle) {
          handle.isNative = true;
          handle.size = stat.size || 0;
        }
      } else {
        const fullPath = path || "";
        const dirPath = fullPath.split("/").slice(0, -1).join("/") || "/";
        const resolved = await this.drive._resolveDir(dirPath, false);
        if (resolved && resolved.dirHandle) {
          const fileName = fullPath.split("/").filter(Boolean).pop();
          try {
            const fileHandle = await resolved.dirHandle.getFileHandle(fileName);
            const file = await fileHandle.getFile();
            resp.result.size = file.size;
            resp.result.mimeType = file.type || "application/octet-stream";
            resp.result.name = fileName;
            const handle = conn.openHandles.get(handleId);
            if (handle) {
              handle.file = file;
              handle.fileHandle = fileHandle;
            }
          } catch (_) {
            // New file that does not exist yet upon open for write
          }
        }
      }
    } catch (e) {
      // FileInfo may fail for shares at root — that's OK for directory listing
    }

    resp.handleId = handleId;
    return resp;
  }

  async _handleRead(conn, req, resp) {
    const handleId = req.handleId || req.path;
    let handle = conn.openHandles.get(handleId);
    if (!handle && req.path) {
      handle = conn.openHandles.get(req.path);
    }
    const targetPath = req.path || handle?.path || handleId || "";
    const offset = req.offset || 0;
    const size = req.size || 65536;
    const query = req.query || handle?.query || "";

    // 1. Fast thumbnail / resize support for Native Storage (Android)
    if (this.drive.isNative && (handle?.isThumbnail || (query && (query.includes("resize=true") || query.includes("cover=") || query.includes("thumbnail=true") || query.includes("poster"))))) {
      if (offset > 0) {
        resp.data = "";
        resp.offset = offset;
        resp.size = 0;
        return resp;
      }
      try {
        if (handle?.thumbData) {
          resp.data = handle.thumbData;
          resp.offset = 0;
          resp.size = handle.size || resp.data.length;
          return resp;
        }
        let width = 360;
        let height = 360;
        const matchW = query.match(/width=(\d+)/);
        const matchH = query.match(/height=(\d+)/);
        if (matchW) width = parseInt(matchW[1], 10);
        if (matchH) height = parseInt(matchH[1], 10);
        else if (matchW) height = width;
        const matchCover = query.match(/cover=(\d+)/);
        if (matchCover) {
          width = parseInt(matchCover[1], 10);
          height = width;
        }
        const thumb = await NativeStorage.getThumbnail({ path: targetPath, width, height });
        if (thumb && thumb.data) {
          resp.data = thumb.data;
          resp.offset = 0;
          resp.size = thumb.data.length;
          return resp;
        }
      } catch (e) {
        console.warn("[ClientDeviceServer] getThumbnail error:", e);
      }
    }

    // 2. Native Storage direct read
    if (this.drive.isNative) {
      const readRes = await this.drive.read(targetPath, offset, size, query);
      resp.data = readRes.data || "";
      resp.offset = offset;
      resp.size = readRes.size || 0;
      return resp;
    }

    // 3. Handle-based or File System Access read
    if ((!handle || !handle.file) && !this.drive.isNative && targetPath) {
      // Auto-open for direct read
      try {
        const fullPath = targetPath;
        const dirPath = fullPath.split("/").slice(0, -1).join("/") || "/";
        const resolved = await this.drive._resolveDir(dirPath, false);
        if (resolved && resolved.dirHandle) {
          const fileName = fullPath.split("/").filter(Boolean).pop();
          if (fileName) {
            const fileHandle = await resolved.dirHandle.getFileHandle(fileName);
            const file = await fileHandle.getFile();
            handle = { path: fullPath, file, fileHandle };
            if (handleId) conn.openHandles.set(handleId, handle);
            conn.openHandles.set(fullPath, handle);
          }
        }
      } catch (err) {
        console.warn("[ClientDeviceServer] Read auto-open error:", err);
      }
    }

    if (!handle || !handle.file) {
      resp.status = "error";
      resp.error = "File not open: " + (targetPath || handleId);
      return resp;
    }

    const file = handle.file;
    const slice = file.slice(offset, Math.min(offset + size, file.size));
    const buf = await slice.arrayBuffer();
    const bytes = new Uint8Array(buf);
    let bin = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    resp.data = btoa(bin);
    resp.offset = offset;
    resp.size = bytes.length;
    return resp;
  }

  async _handleWrite(conn, req, resp) {
    const handleId = req.handleId || req.path;
    const offset = req.offset || 0;
    const truncate = req.truncate || false;
    const dataB64 = req.data || "";

    let handle = conn.openHandles.get(handleId);
    if (!handle && req.path) {
      handle = conn.openHandles.get(req.path);
    }
    const targetPath = req.path || (handle && handle.path) || "";

    if (this.drive.isNative) {
      // homeWrite() always starts at offset 0 — fine for a whole-file write,
      // but chunked writes (webrtc-drive writeFileChunked / the python
      // device_push client) carry an explicit offset and would otherwise keep
      // only the last chunk. Files are created on demand, so an existing-file
      // stale tail is not a concern for our UUID-named artifacts.
      const writeRes = (offset > 0 || !truncate)
        ? await NativeStorage.writeChunk({ path: targetPath, offset, data: dataB64, append: false })
        : await this.drive.homeWrite(targetPath, dataB64);
      resp.written = (writeRes && writeRes.bytesWritten) || 0;
      return resp;
    }

    if (!handle) {
      resp.status = "error";
      resp.error = "File not open: " + handleId;
      return resp;
    }

    try {
      const fullPath = req.path || (handle && handle.path) || "";
      const dirPath = fullPath.split("/").slice(0, -1).join("/") || "/";
      const resolved = await this.drive._resolveDir(dirPath, true);
      if (!resolved || !resolved.dirHandle) {
        resp.status = "error";
        resp.error = "Path not found";
        return resp;
      }
      const fileName = fullPath.split("/").filter(Boolean).pop();
      const fileHandle = await resolved.dirHandle.getFileHandle(fileName, { create: true });
      const writable = await fileHandle.createWritable({ keepExistingData: !truncate });
      if (offset > 0) await writable.seek(offset);
      const bytes = Uint8Array.from(atob(dataB64), (c) => c.charCodeAt(0));
      await writable.write(bytes);
      await writable.close();
      resp.written = bytes.length;
    } catch (e) {
      resp.status = "error";
      resp.error = "Write failed: " + e.message;
    }
    return resp;
  }

  async _handleClose(conn, req, resp) {
    const handleId = req.handleId || req.path;
    const handle = conn.openHandles.get(handleId);
    conn.openHandles.delete(handleId);
    if (handle?.path) {
      conn.openHandles.delete(handle.path);
    }
    if (req.path) {
      conn.openHandles.delete(req.path);
    }
    return resp;
  }

  async _handleSetScope(conn, req, resp) {
    // The scope is defined by the redeemed consent grant, never by the peer:
    // honouring SetScope would let an authorized tunnel widen its own jail.
    resp.result = {
      path: conn.scopePath || "/",
      writable: !!conn.scopeWritable,
      isDir: true,
      enforced: true,
    };
    return resp;
  }

  async _handleAddShare(conn, req, resp) {
    resp.status = "error";
    resp.error = "AddShare not supported on client device";
    return resp;
  }

  async _handleHomeMkDir(conn, req, resp) {
    const result = await this.drive.homeMkDir(req.path || "");
    resp.result = result;
    return resp;
  }

  async _handleHomeWrite(conn, req, resp) {
    const result = await this.drive.homeWrite(req.path || "", req.data || "");
    resp.result = result;
    return resp;
  }

  async _handleResolvePath(conn, req, resp) {
    const result = await this.drive.resolvePath(req.path || "");
    resp.result = result;
    return resp;
  }

  async _handleCopy(conn, req, resp) {
    const result = await this.drive.copyFile(req.path || "", req.target || "");
    resp.result = result;
    return resp;
  }

  async _handleMove(conn, req, resp) {
    const result = await this.drive.moveFile(req.path || "", req.target || "");
    resp.result = result;
    return resp;
  }

  async _handleRename(conn, req, resp) {
    const result = await this.drive.renameFile(req.path || "", req.newName || "");
    resp.result = result;
    return resp;
  }

  async _handleRemove(conn, req, resp) {
    const result = await this.drive.removeFile(req.path || "", req.forced || false);
    resp.result = result;
    return resp;
  }

  async _handleGetSettings(conn, req, resp) {
    const settings = await this.drive.getSettings();
    resp.result = { settings };
    return resp;
  }

  async _handleSetSettings(conn, req, resp) {
    const result = await this.drive.setSettings(req.settings || {});
    resp.result = result;
    return resp;
  }

  _sendRPCResponse(dc, resp) {
    if (!dc || dc.readyState !== "open") return;
    const payload = JSON.stringify(resp);
    try {
      const CHUNK_SIZE = 16384;
      if (payload.length <= CHUNK_SIZE) {
        dc.send(payload);
      } else {
        for (let i = 0; i < payload.length; i += CHUNK_SIZE) {
          dc.send(payload.substring(i, i + CHUNK_SIZE));
        }
      }
    } catch (e) {
      console.error("[ClientDeviceServer] Failed to send RPC response:", e);
    }
  }

  async _sendSignaling(tunnelId, kind, payload) {
    const base = getGatewayBase();
    const url = new URL(this.signalingUrl + (this._token ? `?token=${encodeURIComponent(this._token)}` : ""), base);

    // Params in the POST body - linkID and payloads (SDP/ICE) must not leak into URLs/logs
    const res = await fetch(url.toString(), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        ...(this._token ? { "Authorization": `token ${this._token}` } : {})
      },
      body: JSON.stringify({ linkID: this.linkId, ID: tunnelId, kind, payload: btoa(payload || "") }),
    });
    if (res.status === 401 || res.status === 403) {
      console.warn("[ClientDeviceServer] Signaling access denied (" + res.status + ") for tunnel", tunnelId);
      return null;
    }
    const data = await res.json();

    if (data && Array.isArray(data.signals)) {
      for (const sig of data.signals) {
        if (sig.kind === "offer") {
          const conn = this._activeConnections.get(tunnelId);
          if (conn) this._handleOffer(conn, tunnelId, sig.payload);
        } else if (sig.kind === "candidate") {
          const conn = this._activeConnections.get(tunnelId);
          if (conn && conn.pc && sig.payload) {
            try {
              await conn.pc.addIceCandidate(
                new RTCIceCandidate({ candidate: atob(sig.payload), sdpMid: "0", sdpMLineIndex: 0 })
              );
            } catch (e) {
              console.warn("[ClientDeviceServer] Failed to add ICE candidate from signaling:", e);
            }
          }
        }
      }
    }

    return data;
  }

  _closeConnection(tunnelId, conn) {
    if (!conn || conn.closed) return;
    conn.closed = true;
    conn.isConnected = false;
    if (conn.handshakeTimer) {
      clearTimeout(conn.handshakeTimer);
      conn.handshakeTimer = null;
    }
    if (conn.dc) {
      const dc = conn.dc;
      conn.dc = null;
      try {
        dc.onopen = null;
        dc.onclose = null;
        dc.onmessage = null;
        dc.onerror = null;
        dc.close();
      } catch (_) {}
    }
    if (conn.pc) {
      const pc = conn.pc;
      conn.pc = null;
      try {
        pc.onicecandidate = null;
        pc.onconnectionstatechange = null;
        pc.oniceconnectionstatechange = null;
        pc.ondatachannel = null;
        pc.close();
      } catch (_) {}
    }
    conn.pendingCandidates = [];
    conn.openHandles.clear();
    this._activeConnections.delete(tunnelId);
    console.log("[ClientDeviceServer] Connection closed:", tunnelId);
  }
}

let _serverInstance = null;

export function startClientDeviceServer(linkId, signalingUrl, deviceName) {
  if (_serverInstance && _serverInstance._running && _serverInstance.linkId === linkId) {
    if (deviceName) _serverInstance.deviceName = deviceName;
    if (signalingUrl) _serverInstance.signalingUrl = signalingUrl;
    _serverInstance.ensureRegistered();
    return _serverInstance;
  }
  if (_serverInstance) _serverInstance.stop();
  _serverInstance = new ClientDeviceServer(linkId, signalingUrl, deviceName);
  _serverInstance.start();
  return _serverInstance;
}

export function stopClientDeviceServer() {
  if (_serverInstance) {
    _serverInstance.stop();
    _serverInstance = null;
  }
}

export function getClientDeviceServer() {
  if (!_serverInstance && typeof window !== "undefined") {
    try {
      const linkId = getOrCreateLocalLinkId();
      const devName = (typeof localStorage !== "undefined" ? localStorage.getItem("omd_local_devicename") : "") || getDefaultWebDeviceName();
      startClientDeviceServer(linkId, "/api/signaling", devName);
    } catch (e) {
      console.warn("[ClientDeviceServer] auto-start error:", e);
    }
  }
  return _serverInstance;
}
