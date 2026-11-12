import { getGatewayBase, ensureChannelToken } from "./omd-key.js";
import { getSharedGatewayChannel } from "./gateway-channel.js";
import { getOrCreateLocalLinkId } from "./local-client.js";
import { getViewerSettings } from "./viewer-settings.js";
import { getGatewayTurnHost } from "./gateway-config.js";


let _cachedPublicNodeLinkId = null;
let _fetchingPublicNodePromise = null;
const _activePeerDrives = new Set();
const MAX_ACTIVE_PEER_DRIVES = 120;

function _reapOrphanedPeerDrives() {
    const now = Date.now();
    for (const d of Array.from(_activePeerDrives)) {
        if (!d || !d.pc) {
            _activePeerDrives.delete(d);
            continue;
        }
        const pcState = d.pc.connectionState;
        const iceState = d.pc.iceConnectionState;
        const dcState = d.dc ? d.dc.readyState : "closed";
        const ageMs = now - (d._pcCreatedAt || now);
        if (
            d._disposed ||
            pcState === "closed" ||
            pcState === "failed" ||
            iceState === "closed" ||
            iceState === "failed" ||
            (!d.autoReconnectEnabled && dcState === "closed" && ageMs > 5000) ||
            (!d.isConnected && !d._isConnecting && ageMs > 20000) ||
            (!d.isConnected && ageMs > 35000)
        ) {
            try { d.disconnect(true); } catch (_) {}
            _activePeerDrives.delete(d);
        }
    }
    if (_activePeerDrives.size >= MAX_ACTIVE_PEER_DRIVES) {
        const sorted = Array.from(_activePeerDrives).sort((a, b) => (a._pcCreatedAt || 0) - (b._pcCreatedAt || 0));
        for (const d of sorted) {
            if (_activePeerDrives.size < MAX_ACTIVE_PEER_DRIVES) break;
            if (!d.autoReconnectEnabled || !d.isConnected) {
                try { d.disconnect(true); } catch (_) {}
                _activePeerDrives.delete(d);
            }
        }
    }
}

if (typeof window !== "undefined") {
    window.addEventListener("omd-gateway-changed", () => {
        _cachedPublicNodeLinkId = null;
        _fetchingPublicNodePromise = null;
    });
}

export async function resolvePublicNodeLinkId() {
    if (_cachedPublicNodeLinkId) return _cachedPublicNodeLinkId;

    if (typeof window !== "undefined") {
        if (window.__publicNodeLinkId) {
            _cachedPublicNodeLinkId = window.__publicNodeLinkId;
            return _cachedPublicNodeLinkId;
        }
        if (window.__publicAiNodeLinkId) {
            _cachedPublicNodeLinkId = window.__publicAiNodeLinkId;
            return _cachedPublicNodeLinkId;
        }
        if (window.deviceRegistry && typeof window.deviceRegistry.list === "function") {
            const pub = window.deviceRegistry.list().find((d) => d.isAiNode || d.type === "ai");
            if (pub && (pub.linkId || pub.deviceId)) {
                _cachedPublicNodeLinkId = pub.linkId || pub.deviceId;
                return _cachedPublicNodeLinkId;
            }
        }
    }

    if (typeof localStorage !== "undefined") {
        const stored = localStorage.getItem("omd_public_ai_link_id") || localStorage.getItem("omd_public_node_link_id");
        if (stored) {
            _cachedPublicNodeLinkId = stored;
            return _cachedPublicNodeLinkId;
        }
    }

    if (_fetchingPublicNodePromise) return _fetchingPublicNodePromise;

    _fetchingPublicNodePromise = (async () => {
        try {
            const gatewayBase = getGatewayBase();
            const resp = await fetch(`${gatewayBase}/api/ai-node`, {
                method: "POST",
                credentials: "same-origin",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({}),
            });
            if (resp.ok) {
                const data = await resp.json();
                const linkId = data && data.aiNode && data.aiNode.linkID ? data.aiNode.linkID : null;
                if (linkId) {
                    _cachedPublicNodeLinkId = linkId;
                    if (typeof localStorage !== "undefined") {
                        try { localStorage.setItem("omd_public_ai_link_id", linkId); } catch (_) {}
                    }
                    if (typeof window !== "undefined") {
                        window.__publicNodeLinkId = linkId;
                    }
                    return linkId;
                }
            }
        } catch (e) {
            console.warn("[WebRTC] Could not fetch default public node linkID:", e);
        } finally {
            _fetchingPublicNodePromise = null;
        }
        return null;
    })();

    return _fetchingPublicNodePromise;
}

export class WebRTCDataChannelDrive {

    constructor(targetLinkId, signalingUrl = "/api/signaling", port = 0, service = "") {
        this.targetLinkId = targetLinkId
            || (typeof window !== "undefined" && (window.__publicNodeLinkId || window.__publicAiNodeLinkId))
            || (typeof localStorage !== "undefined" && (localStorage.getItem("omd_public_ai_link_id") || localStorage.getItem("omd_public_node_link_id")))
            || "";
        this.port = port;
        this.service = service;
        if (signalingUrl.startsWith("/")) {
            if (typeof window !== "undefined") {
                signalingUrl = getGatewayBase() + signalingUrl;
            }
        }
        this.signalingUrl = signalingUrl;
        this.pc = null;
        this.dc = null;
        this.reqId = 1;
        this.pendingRequests = new Map();
        this.isConnected = false;
        this.tunnelId = "p2p-tunnel-" + Math.random().toString(36).substring(2, 10);
        this.onStatusChange = null;
        this._handshakeTimer = null;
        this._wsSignalHandler = null;
        this._isReconnecting = false;
        this.autoReconnectEnabled = true;
        this._retryAttempt = 0;
        this._maxRetries = 10;
        this._disposed = false;
        this._keepaliveTimer = null;
        this._keepaliveMissed = 0;
        this.didPub = null;
        this.authLevel = "unauthenticated";
        this.scopeShareId = "";
        this.scopePath = "";
        this.scopeWritable = false;
        this.iceConnectionType = "direct";
    }


    _detachRtcObjects() {
        const dc = this.dc;
        this.dc = null;
        if (dc) {
            try {
                dc.onopen = null;
                dc.onclose = null;
                dc.onmessage = null;
                dc.onerror = null;
                dc.close();
            } catch (_) {}
        }
        const pc = this.pc;
        this.pc = null;
        if (pc) {
            try {
                pc.onicecandidate = null;
                pc.onconnectionstatechange = null;
                pc.oniceconnectionstatechange = null;
                pc.ondatachannel = null;
                pc.close();
            } catch (_) {}
        }
        _activePeerDrives.delete(this);
    }

    /** Permanent teardown - disables auto-reconnect for this drive. */
    close() {
        this.disconnect(true);
    }

    disconnect(explicit = false) {
        if (explicit) this._disposed = true;
        if (this._pollTimer) {
            clearInterval(this._pollTimer);
            this._pollTimer = null;
        }
        if (this._handshakeTimer) {
            clearTimeout(this._handshakeTimer);
            this._handshakeTimer = null;
        }
        if (this._keepaliveTimer) {
            clearInterval(this._keepaliveTimer);
            this._keepaliveTimer = null;
        }
        this._keepaliveMissed = 0;
        if (this._readyTimer) {
            clearTimeout(this._readyTimer);
            this._readyTimer = null;
        }
        if (this._readyResolve) {
            const resolve = this._readyResolve;
            this._readyResolve = null;
            resolve();
        }
        this._readyReceived = false;
        this._detachRtcObjects();
        if (this._wsSignalHandler) {
            const gatewayChan = getSharedGatewayChannel();
            if (gatewayChan) gatewayChan.offTunnelOpen(this._wsSignalHandler);
            this._wsSignalHandler = null;
        }
        this.isConnected = false;
        this._isConnecting = false;
        for (const [id, { reject }] of this.pendingRequests.entries()) {
            try { reject(new Error("DataChannel disconnected")); } catch (_) {}
        }
        this.pendingRequests.clear();
        this.msgBuffer = "";
        this._notifyStatus("disconnected");
    }

    _notifyStatus(newStatus) {
        if (typeof this.onStatusChange === "function") {
            try { this.onStatusChange(newStatus); } catch (e) { console.error("[WebRTC] onStatusChange error:", e); }
        }
        if (this._statusListeners && this._statusListeners.length > 0) {
            for (const fn of [...this._statusListeners]) {
                try { fn(newStatus); } catch (e) { console.error("[WebRTC] statusListener error:", e); }
            }
        }
    }

    _scheduleReconnect(reason, baseDelayMs = 2000) {
        if (this._disposed || !this.autoReconnectEnabled) return;
        if (this._isReconnecting || this._reconnectPromise) return;
        if (this.isConnected) return;
        if (this._retryAttempt >= this._maxRetries) {
            console.warn(`[WebRTC] Max reconnect retries (${this._maxRetries}) reached for ${this.targetLinkId}`);
            return;
        }
        this._retryAttempt++;
        const delay = Math.min(baseDelayMs * Math.pow(1.5, this._retryAttempt - 1), 30000);
        console.log(`[WebRTC] Scheduling reconnect attempt ${this._retryAttempt}/${this._maxRetries} in ${delay}ms (${reason})`);
        setTimeout(() => {
            if (!this._disposed && !this.isConnected && !this._isReconnecting) {
                this.reconnect().catch(() => {});
            }
        }, delay);
    }

    waitUntilConnected(timeoutMs = 15000) {
        if (this.isConnected && this.dc && this.dc.readyState === "open") {
            return Promise.resolve(true);
        }
        return new Promise((resolve) => {
            let timer = null;
            let checkInterval = null;

            const cleanup = () => {
                if (timer) { clearTimeout(timer); timer = null; }
                if (checkInterval) { clearInterval(checkInterval); checkInterval = null; }
                if (this._statusListeners) {
                    this._statusListeners = this._statusListeners.filter(fn => fn !== onStatus);
                }
            };

            const onStatus = (status) => {
                if (status === "connected" && this.isConnected && this.dc && this.dc.readyState === "open") {
                    cleanup();
                    resolve(true);
                }
            };

            if (!this._statusListeners) this._statusListeners = [];
            this._statusListeners.push(onStatus);

            checkInterval = setInterval(() => {
                if (this.isConnected && this.dc && this.dc.readyState === "open") {
                    cleanup();
                    resolve(true);
                }
            }, 150);

            timer = setTimeout(() => {
                cleanup();
                resolve(!!(this.isConnected && this.dc && this.dc.readyState === "open"));
            }, timeoutMs);
        });
    }

    async reconnect() {
        if (this._reconnectPromise) {
            return this._reconnectPromise;
        }
        if (this.isConnected && this.dc && this.dc.readyState === "open") {
            return true;
        }

        this._reconnectPromise = (async () => {
            this._isReconnecting = true;
            console.log("[WebRTC] Reconnecting to node:", this.targetLinkId);
            this.disconnect();
            this.tunnelId = "p2p-tunnel-" + Math.random().toString(36).substring(2, 10);
            let deadlineTimer = null;
            try {
                await Promise.race([
                    this.connect(),
                    new Promise((_, reject) => {
                        deadlineTimer = setTimeout(() => reject(new Error("Connection deadline exceeded")), 15000);
                    })
                ]);
            } catch (err) {
                console.error("[WebRTC] Reconnect failed:", err);
                this.disconnect();
            } finally {
                if (deadlineTimer) clearTimeout(deadlineTimer);
                this._isReconnecting = false;
            }
            return this.waitUntilConnected(15000);
        })().finally(() => {
            this._reconnectPromise = null;
        });

        return this._reconnectPromise;
    }

    async connect() {
        if (this.isConnected && this.dc && this.dc.readyState === "open") {
            return;
        }
        if (this._isConnecting) {
            console.log("[WebRTC] Connection attempt already in progress for:", this.targetLinkId, "tunnel:", this.tunnelId);
            return;
        }
        this._isConnecting = true;
        this._disposed = false;
        this.tunnelId = "p2p-tunnel-" + Math.random().toString(36).substring(2, 10);
        if (!this.targetLinkId) {
            console.log("[WebRTC] No targetLinkId specified, attempting to resolve public node...");
            try {
                const defaultPubId = await resolvePublicNodeLinkId();
                if (defaultPubId) {
                    this.targetLinkId = defaultPubId;
                    console.log("[WebRTC] Defaulted empty targetLinkId to public node:", this.targetLinkId);
                }
            } catch (err) {
                console.warn("[WebRTC] Failed to resolve default public node:", err);
            }
        }

        if (!this.targetLinkId) {
            // No target: signaling to an empty linkID can never succeed.
            console.warn("[WebRTC] connect() called with empty targetLinkId and no public node found");
            this._isConnecting = false;
            this._notifyStatus("error");
            throw new Error("No target node specified");
        }
        console.log("[WebRTC] Initializing WebRTC connection to target node:", this.targetLinkId, "tunnel:", this.tunnelId);

        if (this._handshakeTimer) {
            clearTimeout(this._handshakeTimer);
            this._handshakeTimer = null;
        }
        this._detachRtcObjects();
        _reapOrphanedPeerDrives();

        const turnHost = getGatewayTurnHost();
        try {
            this.pc = new RTCPeerConnection({
                iceServers: [
                    { urls: `stun:${turnHost}:3478` },
                    { urls: "stun:stun.cloudflare.com:3478" },
                    { urls: "stun:stun.l.google.com:19302" },
                    { urls: "stun:stun1.l.google.com:19302" },
                    ...WebRTCDataChannelDrive.turnServers()
                ]
            });
        } catch (err) {
            this._isConnecting = false;
            this._notifyStatus("error");
            throw err;
        }
        this._pcCreatedAt = Date.now();
        _activePeerDrives.add(this);

        try {
        this.dc = this.pc.createDataChannel("p2p-tunnel");
        if (this.port || this.service || this.onRawMessage) {
            this.dc.binaryType = "arraybuffer";
        }

        // Ready handshake: the node signals __ready__ once its message handler
        // is connected. We wait for it before sending the first RPC so the
        // initial AuthInfo is not lost to a connect-time race.
        this._readyReceived = false;
        this._readyPromise = new Promise((resolve) => {
            this._readyResolve = resolve;
            this._readyTimer = setTimeout(() => {
                this._readyResolve = null;
                resolve();
            }, 5000);
        });

        this.dc.onopen = async () => {
            this._isConnecting = false;
            console.log("[WebRTC] P2P DataChannel OPEN to node:", this.targetLinkId, "tunnel:", this.tunnelId);
            if (this._pollTimer) {
                clearInterval(this._pollTimer);
                this._pollTimer = null;
            }
            if (this._handshakeTimer) {
                clearTimeout(this._handshakeTimer);
                this._handshakeTimer = null;
            }
            this.isConnected = true;
            this._retryAttempt = 0;

            // Start DataChannel keepalive (30s ping, 5 missed → dead)
            this._keepaliveMissed = 0;
            if (this._keepaliveTimer) clearInterval(this._keepaliveTimer);
            this._keepaliveTimer = setInterval(() => {
                if (!this.dc || this.dc.readyState !== "open") {
                    clearInterval(this._keepaliveTimer);
                    this._keepaliveTimer = null;
                    return;
                }
                // When tab is hidden in background, browser throttles timers — pause ping misses
                if (typeof document !== "undefined" && document.visibilityState === "hidden") {
                    return;
                }
                try {
                    this.dc.send("__ping__");
                    this._keepaliveMissed++;
                    if (this._keepaliveMissed > 4) {
                        console.warn("[WebRTC] Keepalive timeout — unanswered pings for:", this.targetLinkId);
                        clearInterval(this._keepaliveTimer);
                        this._keepaliveTimer = null;
                        this.isConnected = false;
                        this._notifyStatus("disconnected");
                        this._scheduleReconnect("keepalive timeout");
                    }
                } catch (e) {
                    console.error("[WebRTC] Keepalive send error:", e);
                }
            }, 30000);

            // Only run JSON-RPC auth on FS tunnels (where port === 0 and service is empty or p2p-fs)
            if (!this.port && (!this.service || this.service === "p2p-fs")) {
                // Wait for the node to signal readiness before sending the first
                // RPC - otherwise AuthInfo can be lost to a connect-time race.
                console.log("[WebRTC] Waiting for node __ready__ signal...");
                try {
                    await this._readyPromise;
                    console.log("[WebRTC] Node __ready__ received, sending AuthInfo");
                } catch (e) {
                    console.warn("[WebRTC] Node ready wait failed:", e?.message || e);
                }
                const effectiveDidPub = this.didPub || (typeof window !== "undefined" && window.identity?.mainPub) || null;
                try {
                    const info = await this.authInfo(effectiveDidPub);
                    if (info && info.status === "ok" && info.result) {
                        if (info.result.name) {
                            this.rootName = info.result.name;
                        }
                    }
                } catch (e) {
                    console.log("[WebRTC] AuthInfo on open error:", e?.message || e);
                }

                // Auto-authenticate with paired DID if available. Important: the
                // node only upgrades THIS session to admin after a PairAuth/Login
                // RPC, so "paired" reported by authInfo is not enough by itself —
                // otherwise on reload the session stays unauthenticated server-side
                // and the PairModal pops for already-paired devices.
                if (effectiveDidPub && this.authLevel !== "admin") {
                    try {
                        const authRes = await this.pairAuth(effectiveDidPub);
                        if (authRes && authRes.status === "ok") {
                            this.authLevel = "admin";
                            console.log("[WebRTC] Auto-authenticated with paired DID:", effectiveDidPub);
                        }
                    } catch (e) {
                        console.log("[WebRTC] DID not pre-paired or auth failed:", e?.message || e);
                    }
                }

                if (this.scopeShareId || this.scopePath) {
                    console.log("[WebRTC] Re-applying scope on open:", this.scopeShareId || this.scopePath);
                    try {
                        await this.setScope(this.scopeShareId ? { shareId: this.scopeShareId } : this.scopePath, !!this.scopeWritable);
                    } catch (e) {
                        console.warn("[WebRTC] Failed to re-apply scope on open:", e);
                    }
                }
            }

            this.getIceConnectionType().then((type) => {
                console.log(`[WebRTC] DataChannel connected for ${this.targetLinkId}, ICE type: ${type}`);
            }).catch(() => {});

            this._notifyStatus("connected");
        };

        this.dc.onclose = () => {
            console.log("[WebRTC] P2P DataChannel closed for:", this.targetLinkId);
            this.isConnected = false;
            for (const [id, req] of this.pendingRequests.entries()) {
                try { req.reject(new Error("DataChannel closed")); } catch (_) {}
            }
            this.pendingRequests.clear();
            this._detachRtcObjects();
            this._notifyStatus("disconnected");
            if (this.autoReconnectEnabled && !this._disposed) {
                this._scheduleReconnect("DataChannel closed");
            } else {
                this.disconnect(true);
            }
        };

        this.pc.onconnectionstatechange = () => {
            if (!this.pc) return;
            const connState = this.pc.connectionState;
            console.log("[WebRTC] PeerConnection state for", this.targetLinkId, ":", connState);
            if (connState === "connected") {
                this.getIceConnectionType().catch(() => {});
            } else if (connState === "failed" || connState === "closed") {
                this.isConnected = false;
                for (const [id, req] of this.pendingRequests.entries()) {
                    try { req.reject(new Error("PeerConnection " + connState)); } catch (_) {}
                }
                this.pendingRequests.clear();
                this._detachRtcObjects();
                this._notifyStatus("disconnected");
                if (this.autoReconnectEnabled && !this._disposed) {
                    this._scheduleReconnect("PeerConnection " + connState);
                } else {
                    this.disconnect(true);
                }
            }
        };

        this.pc.oniceconnectionstatechange = () => {
            if (!this.pc) return;
            const state = this.pc.iceConnectionState;
            console.log("[WebRTC] ICE Connection State for", this.targetLinkId, ":", state);
            if (state === "connected" || state === "completed") {
                this.getIceConnectionType().catch(() => {});
            } else if (state === "failed" || state === "closed") {
                this.isConnected = false;
                for (const [id, req] of this.pendingRequests.entries()) {
                    try { req.reject(new Error("ICE connection " + state)); } catch (_) {}
                }
                this.pendingRequests.clear();
                this._detachRtcObjects();
                this._notifyStatus("disconnected");
                if (this.autoReconnectEnabled && !this._disposed) {
                    this._scheduleReconnect("ICE connection " + state);
                } else {
                    this.disconnect(true);
                }
            }
        };

        this.dc.onmessage = (event) => {
            // Any message from the peer confirms the channel is alive
            this._keepaliveMissed = 0;

            // Keepalive: respond to __ping__ with __pong__, reset counter on __pong__
            let raw = "";
            if (typeof event.data === "string") {
                raw = event.data;
            } else if (event.data instanceof ArrayBuffer && event.data.byteLength < 32) {
                try {
                    raw = (this.textDecoder || new TextDecoder()).decode(new Uint8Array(event.data));
                } catch (_) {}
            } else if (ArrayBuffer.isView(event.data) && event.data.byteLength < 32) {
                try {
                    raw = (this.textDecoder || new TextDecoder()).decode(event.data);
                } catch (_) {}
            }

            if (raw === "__ping__") {
                try { this.dc.send("__pong__"); } catch (_) {}
                return;
            }
            if (raw === "__pong__") {
                return;
            }
            if (raw === "__ready__") {
                this._readyReceived = true;
                if (this._readyResolve) {
                    const resolve = this._readyResolve;
                    this._readyResolve = null;
                    clearTimeout(this._readyTimer);
                    resolve();
                }
                return;
            }
            if (this.onRawMessage) {
                this.onRawMessage(event.data);
                return;
            }
            if (this.port || this.service) {
                return; // Raw byte stream tunnel, skip JSON-RPC parsing
            }
            if (!this.msgBuffer) {
                this.msgBuffer = "";
                this.textDecoder = new TextDecoder();
            }
            let chunkStr = event.data;
            if (event.data instanceof ArrayBuffer) {
                chunkStr = this.textDecoder.decode(new Uint8Array(event.data), { stream: true });
            } else if (ArrayBuffer.isView(event.data)) {
                chunkStr = this.textDecoder.decode(event.data, { stream: true });
            } else if (event.data instanceof Blob) {
                const reader = new FileReader();
                reader.onload = () => {
                    this.dc.onmessage({ data: reader.result });
                };
                reader.readAsText(event.data);
                return;
            }
            
            this.msgBuffer += chunkStr;

            // Process any complete JSON objects in the buffer
            while (this.msgBuffer.length > 0) {
                const trimmed = this.msgBuffer.trimStart();
                if (!trimmed.startsWith("{")) {
                    const nextBrace = this.msgBuffer.indexOf("{");
                    if (nextBrace !== -1) {
                        this.msgBuffer = this.msgBuffer.substring(nextBrace);
                    } else {
                        this.msgBuffer = "";
                        break;
                    }
                }

                let depth = 0;
                let inString = false;
                let escaped = false;
                let endIdx = -1;

                for (let i = 0; i < this.msgBuffer.length; i++) {
                    const char = this.msgBuffer[i];
                    if (escaped) {
                        escaped = false;
                        continue;
                    }
                    if (char === "\\") {
                        escaped = true;
                        continue;
                    }
                    if (char === '"') {
                        inString = !inString;
                        continue;
                    }
                    if (!inString) {
                        if (char === "{") depth++;
                        else if (char === "}") {
                            depth--;
                            if (depth === 0) {
                                endIdx = i + 1;
                                break;
                            }
                        }
                    }
                }

                if (endIdx !== -1) {
                    const jsonStr = this.msgBuffer.substring(0, endIdx);
                    this.msgBuffer = this.msgBuffer.substring(endIdx);
                    try {
                        const response = JSON.parse(jsonStr);
                        if (response.id && this.pendingRequests.has(response.id)) {
                            const { resolve, reject } = this.pendingRequests.get(response.id);
                            this.pendingRequests.delete(response.id);
                            if (response.status === "ok") {
                                resolve(response);
                            } else {
                                reject(new Error(response.error || "P2P error"));
                            }
                        }
                    } catch (err) {
                        console.error("[WebRTC] Error parsing extracted JSON RPC response:", err);
                    }
                } else {
                    // Incomplete JSON fragment, wait for next chunk
                    break;
                }
            }

            if (this.msgBuffer.length > 5000000) {
                console.error("[WebRTC] P2P message buffer overflow reset");
                this.msgBuffer = "";
            }
        };

        this._pendingLocalCandidates = [];
        this._pendingRemoteCandidates = [];
        this._offerSent = false;

        this.pc.onicecandidate = async (e) => {
            if (e.candidate && e.candidate.candidate) {
                if (!this._offerSent) {
                    this._pendingLocalCandidates.push(e.candidate.candidate);
                } else {
                    console.log("[WebRTC] Local candidate gathered:", e.candidate.candidate);
                    await this.sendSignaling("candidate", e.candidate.candidate);
                }
            }
        };

        // Direct QWebChannel WebSocket push delivery (zero-latency)
        const gatewayChan = getSharedGatewayChannel();
        if (this._wsSignalHandler) {
            gatewayChan.offTunnelOpen(this._wsSignalHandler);
            this._wsSignalHandler = null;
        }

        this._wsSignalHandler = async (sig) => {
            const sigTunnelId = sig?.tunnelID || sig?.ID;
            if (!sig || sigTunnelId !== this.tunnelId) return;
            console.log("[WebRTC] Received direct WebSocket push signal:", sig.kind, "for tunnel:", this.tunnelId, sig);
            await this.handleSignalingResponse(sig.kind, sig.payload);
        };
        gatewayChan.onTunnelOpen(this._wsSignalHandler);

        // Ensure gateway WebSocket is connected and subscribed before sending offer
        try { await gatewayChan.connect(); } catch (_) {}


        // Fallback polling timer (picks up any early answer/candidates queued before WebSocket finished registering)
        if (this._pollTimer) {
            clearInterval(this._pollTimer);
            this._pollTimer = null;
        }
        this._pollTimer = setInterval(async () => {
            if (this.isConnected || !this.pc) {
                if (this._pollTimer) {
                    clearInterval(this._pollTimer);
                    this._pollTimer = null;
                }
                return;
            }
            try {
                const res = await this.sendSignaling("poll", "");
                if (res && res.signals && Array.isArray(res.signals)) {
                    for (const sig of res.signals) {
                        await this.handleSignalingResponse(sig.kind, sig.payload);
                    }
                }
            } catch (_) {}
        }, 1000);

        // One-shot handshake deadline timer (10 seconds)
        this._handshakeTimer = setTimeout(() => {
            if (!this.isConnected) {
                console.log("[WebRTC] Handshake timed out, device unreachable:", this.targetLinkId);
                this.disconnect();
                this._notifyStatus("disconnected");
                // Offline node / unreachable: do NOT retry in a loop! When device wakes up, gateway sends linkConnected.
            }
        }, 10000);

        if (!this.pc) return;
        const offer = await this.pc.createOffer();
        if (!this.pc) return;
        await this.pc.setLocalDescription(offer);

        console.log("[WebRTC] Sending offer SDP to node via HTTP:", this.targetLinkId);
        const res = await this.sendSignaling("offer", offer.sdp);
        this._offerSent = true;

        const isOffline = !res ||
            res.status === "error" ||
            res.error ||
            (res.result && res.result.success === false);

        if (isOffline) {
            const errMsg = res?.error?.message || res?.error || res?.status || "Target node not connected";
            console.log("[WebRTC] Node is offline on gateway:", this.targetLinkId, `(${errMsg})`);
            this.disconnect();
            this._notifyStatus("disconnected");
            return;
        }

        if (this._pendingLocalCandidates && this._pendingLocalCandidates.length > 0) {
            for (const cand of this._pendingLocalCandidates) {
                await this.sendSignaling("candidate", cand);
            }
            this._pendingLocalCandidates = [];
        }
        } catch (err) {
            this.disconnect();
            throw err;
        }
    }

    async sendSignaling(kind, payload) {
        const payloadB64 = btoa(payload || "");
        try {
            const url = this.signalingUrl || "/api/signaling";
            const localId = typeof window !== "undefined" ? getOrCreateLocalLinkId() : "";
            let token = "";
            try { token = await ensureChannelToken(); } catch (_) {}
            const body = {
                linkID: this.targetLinkId,
                ID: this.tunnelId,
                kind: kind,
                payload: payloadB64,
                port: this.port || 0,
                service: this.service || "",
                senderId: localId,
                token: token || ""
            };
            const resp = await fetch(url, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify(body),
                credentials: "same-origin"
            });
            if (!resp.ok) {
                return { status: "error", code: resp.status };
            }
            return await resp.json();
        } catch (err) {
            console.error("[WebRTC] HTTP sendSignaling error:", err);
            return { status: "error" };
        }
    }

    async handleSignalingResponse(kind, payloadB64) {
        if (kind === "tunnel-closed") {
            // The device reported the tunnel dead (ICE failed / closed).
            // Stop waiting and reconnect with a fresh tunnel automatically.
            console.warn("[WebRTC] Tunnel closed by device:", this.tunnelId);
            this.isConnected = false;
            this.disconnect();
            this._scheduleReconnect("tunnel closed by device");
            return;
        }
        if (!this.pc) return;
        let payload = payloadB64;
        try {
            payload = atob(payloadB64 || "");
        } catch (_) {
            payload = payloadB64 || "";
        }
        const cleanCand = (raw) => {
            if (!raw) return "";
            let s = raw.trim();
            if (s.startsWith("a=")) s = s.substring(2);
            return s;
        };

        if (kind === "answer") {
            if (this.pc.remoteDescription || this.pc.signalingState !== "have-local-offer") return;
            try {
                console.log("[WebRTC] Applying remote answer SDP for tunnel:", this.tunnelId);
                await this.pc.setRemoteDescription(new RTCSessionDescription({ type: "answer", sdp: payload }));
            } catch (err) {
                console.warn("[WebRTC] setRemoteDescription error:", err);
                return;
            }
            if (this._pendingRemoteCandidates && this._pendingRemoteCandidates.length > 0) {
                for (const cand of this._pendingRemoteCandidates) {
                    if (!this.pc) break;
                    try {
                        const c = cleanCand(cand);
                        if (c) {
                            console.log("[WebRTC] Applying buffered remote candidate:", c);
                            await this.pc.addIceCandidate(new RTCIceCandidate({ candidate: c, sdpMLineIndex: 0 }));
                        }
                    } catch (e) {
                        console.warn("[WebRTC] Error applying buffered remote candidate:", e);
                    }
                }
                this._pendingRemoteCandidates = [];
            }
        } else if (kind === "candidate") {
            if (!this.pc) return;
            const c = cleanCand(payload);
            if (!c) return;
            if (!this._appliedCandidates) this._appliedCandidates = new Set();
            if (this._appliedCandidates.has(c)) return;
            this._appliedCandidates.add(c);
            if (!this.pc.remoteDescription) {
                if (!this._pendingRemoteCandidates) this._pendingRemoteCandidates = [];
                this._pendingRemoteCandidates.push(c);
            } else {
                try {
                    console.log("[WebRTC] Adding remote candidate:", c);
                    await this.pc.addIceCandidate(new RTCIceCandidate({ candidate: c, sdpMLineIndex: 0 }));
                } catch (e) {
                    console.warn("[WebRTC] Error adding remote candidate:", e);
                }
            }
        } else if (kind === "done") {
            console.log("[WebRTC] Remote signaling done (end-of-candidates)");
            if (this.pc && this.pc.remoteDescription) {
                try {
                    await this.pc.addIceCandidate(null);
                } catch (_) {}
            }
        }
    }


    async getIceConnectionType() {
        if (!this.pc) return this.iceConnectionType || "direct";
        try {
            const stats = await this.pc.getStats();
            let selectedPair = null;
            for (const report of stats.values()) {
                if (report.type === "transport" && report.selectedCandidatePairId) {
                    selectedPair = stats.get(report.selectedCandidatePairId);
                    break;
                }
                if (report.type === "candidate-pair" && (report.selected || report.nominated || report.state === "succeeded")) {
                    selectedPair = report;
                    break;
                }
            }
            if (selectedPair) {
                const local = stats.get(selectedPair.localCandidateId);
                const remote = stats.get(selectedPair.remoteCandidateId);
                const isRelay = (local?.candidateType === "relay") || (remote?.candidateType === "relay");
                this.iceConnectionType = isRelay ? "relay" : "direct";
                return this.iceConnectionType;
            }
        } catch (_) {}
        return this.iceConnectionType || "direct";
    }

    async sendRPC(cmd, params = {}, timeoutMs = 30000) {
        if (!this.dc || this.dc.readyState !== "open") {
            console.warn(`[WebRTC] sendRPC called for '${cmd}' while DataChannel is not open. Attempting reconnect...`);
            const connected = await this.reconnect();
            if (!connected || !this.dc || this.dc.readyState !== "open") {
                throw new Error("DataChannel not connected");
            }
        }

        const isAuthCmd = (cmd === "Login" || cmd === "PairAuth" || cmd === "GuestPair" || cmd === "AuthInfo" || cmd === "SetScope");
        if (!isAuthCmd && this.authLevel === "unauthenticated" && !this.scopeShareId && !this.scopePath) {
            const effectiveDidPub = this.didPub || (typeof window !== "undefined" && window.identity?.mainPub) || null;
            if (effectiveDidPub) {
                try {
                    await this.pairAuth(effectiveDidPub);
                } catch (_) {}
            }
        }

        return new Promise((resolve, reject) => {
            const id = this.reqId++;
            this.pendingRequests.set(id, { resolve, reject });
            const payload = JSON.stringify({ id, cmd, ...params });
            if (cmd !== "HomeWrite" && cmd !== "Write") {
                console.log(`[WebRTC] Sending RPC: ${cmd} (ID: ${id}) with params:`, params);
            }
            try {
                this.dc.send(payload);
            } catch (err) {
                console.error(`[WebRTC] Error sending RPC ${cmd}:`, err);
                this.pendingRequests.delete(id);
                return reject(err);
            }
            
            setTimeout(() => {
                if (this.pendingRequests.has(id)) {
                    console.error(`[WebRTC] RPC Timeout for: ${cmd} (ID: ${id})`);
                    this.pendingRequests.delete(id);
                    reject(new Error(`RPC timeout for command: ${cmd}`));
                }
            }, timeoutMs);
        });
    }

    // SVAR Data API methods
    // opts.home (bool): browse the real node filesystem instead of logical
    // shares. Path "/" maps to the FileNode user home; returned items expose a
    // `realPath` for the current directory so a picker can build absolute paths.
    async getFiles(folderId = "/", opts = {}) {
        const params = { path: folderId };
        if (opts.home) params.home = true;
        const res = await this.sendRPC("FileInfo", params, 30000);
        if (folderId === "/" && !opts.home) {
            this.rootName = res.result.name;
        }
        const items = (res && res.result && Array.isArray(res.result.files)) ? res.result.files : [];
        const dirRealPath = (res && res.result && res.result.realPath) || "";
        const filteredResult = items.filter(f => {
            if (!f || typeof f !== "object") return false;
            const name = f.name || "";
            if (name === "" || name === "." || name === "..") return false;
            return !(name === 'folder.jpg' || name === 'cover.png' || name === 'cover.jpg' || name.endsWith('.cover.jpg') || name.endsWith('.cover.png') || name.endsWith('Readme.md') || name.endsWith('.Readme.md'));
        });

        const result = filteredResult.map(f => {
            const fileName = f.name || "Untitled";
            const id = folderId === "/" ? "/" + fileName : folderId + "/" + fileName;
            const p2pUrl = "/p2p" + encodeURI(id);
            const isMedia = !f.isDir && f.mimeType && (f.mimeType.startsWith("image/") || f.mimeType.startsWith("video/"));
            let fileType = f.isDir ? "folder" : "file";
            if (f.mimeType === "application/x-terminal") fileType = "code";
            if (f.mimeType === "application/x-chat") fileType = "image";
            if (f.mimeType === "application/x-web-app") fileType = "code";
            const isWebApp = f.mimeType === "application/x-web-app";
            
            // Extract file extension for SVAR icon resolution
            const ext = f.isDir ? "" : (fileName.includes(".") ? fileName.split(".").pop().toLowerCase() : "");
            
            const isShared = !!(f.isShared || f.shared || f.customAttributes?.shared || f.mimeType === "application/x-shared-folder");
            const folderIcon = isShared ? "wxi-folder-shared" : undefined;
            const hasCover = !!(f.hasCover || f.customAttributes?.cover);
            // Absolute real on-disk path of this item (only populated for home browsing)
            const itemRealPath = dirRealPath ? (dirRealPath.endsWith("/") ? dirRealPath + fileName : dirRealPath + "/" + fileName) : "";
            
            const rawDate = f.mediaDate || f.customAttributes?.mediaDate || f.lastModified;
            return {
                id,
                value: fileName,
                name: fileName,
                type: fileType || "file",
                itemType: isShared ? "shared" : (isWebApp ? "app" : undefined),
                isShared,
                icon: folderIcon,
                ext,
                date: rawDate ? new Date(rawDate) : new Date(),
                size: f.size || 0,
                lazy: !!f.isDir,  // folders load children lazily via request-data event
                mimeType: f.mimeType,
                hasCover,
                realPath: itemRealPath,
                customAttributes: f.customAttributes || {},
                ...(() => {
                    const vs = getViewerSettings();
                    if (!isMedia && !hasCover) return {};
                    if (!vs.generateThumbs) return {};
                    if (hasCover) {
                        return {
                            preview: p2pUrl + "?cover=240",
                            image: p2pUrl + `?cover=${vs.scaleTo}`,
                            thumb: p2pUrl + "?cover=240"
                        };
                    }
                    if (vs.downscaleImages) {
                        return {
                            preview: `${p2pUrl}?resize=true&width=240&height=240`,
                            image: `${p2pUrl}?resize=true&width=${vs.scaleTo}&height=${vs.scaleTo}`,
                            thumb: `${p2pUrl}?resize=true&width=240&height=240`
                        };
                    }
                    return { preview: p2pUrl, image: p2pUrl, thumb: p2pUrl };
                })()
            };
        });
        // Expose the current directory's absolute real path on the array itself
        // so a picker can navigate the real filesystem and build full paths.
        result.realPath = dirRealPath;
        return result;
    }

    /** Pairing secret storage: one secret per (browser, node) pair, issued
     *  by the node at Login and required by PairAuth - the DID pubkey alone
     *  is public and can not prove ownership. */
    _pairingSecretKey() {
        return "omd-pairing-" + this.targetLinkId;
    }

    _getPairingSecret() {
        if (typeof window === "undefined" || !window.localStorage) return "";
        return window.localStorage.getItem(this._pairingSecretKey()) || "";
    }

    _storePairingSecret(secret) {
        if (typeof window === "undefined" || !window.localStorage || !secret) return;
        try { window.localStorage.setItem(this._pairingSecretKey(), secret); } catch (_) {}
    }

    async authInfo(didPub = this.didPub) {
        const effectiveDidPub = didPub || (typeof window !== "undefined" && window.identity?.mainPub) || "";
        const res = await this.sendRPC("AuthInfo", { didPub: effectiveDidPub, deviceId: getOrCreateLocalLinkId() });
        if (res && res.status === "ok" && res.result) {
            if (res.result.name) this.rootName = res.result.name;
            if (res.result.authLevel) this.authLevel = res.result.authLevel;
        }
        return res;
    }

    async login(user, pass, didPub = this.didPub) {
        //The DID proof registers the DID on the node (the master key proves
        //ownership right after the DID login, when the master key is in memory).
        const params = { user, pass, didPub: didPub || "", deviceId: getOrCreateLocalLinkId() };
        const identity = (typeof window !== "undefined") ? window.identity : null;
        if (didPub && identity && identity.devicePub && typeof identity.signProof === "function") {
            try {
                const msg = 'did:' + didPub + ',device:' + identity.devicePub + ',session:' + params.deviceId;
                const proof = await identity.signProof(msg);
                if (proof) params.devicePub = identity.devicePub, params.didProof = proof;
                else console.debug("[WebRTC] login: signProof returned null (master not in memory yet?)");
            } catch (e) { console.warn("[WebRTC] DID proof sign failed:", e); }
        }
        const res = await this.sendRPC("Login", params);
        if (res.status === "ok") {
            this.authLevel = "admin";
            if (didPub) this.didPub = didPub;
            if (res.result && res.result.name) {
                this.rootName = res.result.name;
            }
        }
        return res;
    }

    async pairAuth(didPub = this.didPub) {
        if (!didPub) throw new Error("DID pubkey is required for pairAuth");
        const identity = (typeof window !== "undefined") ? window.identity : null;
        const deviceId = getOrCreateLocalLinkId();
        if (!identity || !identity.devicePub || typeof identity.signProof !== "function") {
            console.debug("[WebRTC] pairAuth: no DID identity/proof signer available for", didPub);
            return { status: "error", error: "No DID identity available for the pairing proof" };
        }
        try {
            const msg = 'did:' + didPub + ',device:' + identity.devicePub + ',session:' + deviceId;
            const proof = await identity.signProof(msg);
            if (!proof) {
                console.debug("[WebRTC] pairAuth: signProof returned null for", didPub);
                return { status: "error", error: "Failed to sign the pairing proof" };
            }
            const res = await this.sendRPC("PairAuth", { didPub, devicePub: identity.devicePub, deviceId, proof });
            if (res.status !== "ok") console.debug("[WebRTC] pairAuth rejected by node:", res.error, "(proof len:", proof.length, "devicePub:", identity.devicePub);
            if (res.status === "ok") {
                this.authLevel = "admin";
                this.didPub = didPub;
                if (res.result && res.result.name) {
                    this.rootName = res.result.name;
                }
            }
            return res;
        } catch (e) {
            return { status: "error", error: (e && e.message) || "Pairing proof failed" };
        }
    }

    // Guest pairing for public AI nodes: registers the DID on the node with
    // guest rights (no password). Node-side GuestPairing config must be on.
    async guestPair(didPub = this.didPub) {
        const effectiveDidPub = didPub || (typeof window !== "undefined" && window.identity?.mainPub) || "";
        if (!effectiveDidPub) throw new Error("DID pubkey is required for guestPair");
        const res = await this.sendRPC("GuestPair", { didPub: effectiveDidPub });
        if (res.status === "ok") {
            this.didPub = effectiveDidPub;
            if (res.result && res.result.name) {
                this.rootName = res.result.name;
            }
            this.authLevel = res.result?.authLevel || "guest";
        }
        return res;
    }

    async createShare(path, writable = false, name = "") {
        return this.sendRPC("CreateShare", { path, writable: !!writable, name });
    }

    async revokeShare(shareId) {
        return this.sendRPC("RevokeShare", { shareId });
    }

    async setScope(scopeIdentifier, writable = false) {
        if (typeof scopeIdentifier === "object" && scopeIdentifier !== null) {
            this.scopeShareId = scopeIdentifier.shareId || "";
            this.scopePath = scopeIdentifier.path || "";
            this.scopeWritable = !!scopeIdentifier.writable;
            return this.sendRPC("SetScope", scopeIdentifier);
        } else if (typeof scopeIdentifier === "string" && scopeIdentifier.startsWith("sh_")) {
            this.scopeShareId = scopeIdentifier;
            this.scopeWritable = !!writable;
            return this.sendRPC("SetScope", { shareId: scopeIdentifier, writable: !!writable });
        } else {
            this.scopePath = scopeIdentifier;
            this.scopeWritable = !!writable;
            return this.sendRPC("SetScope", { path: scopeIdentifier, writable: !!writable });
        }
    }


    async getFileContent(filePath, sizeToRead, onProgress = null, startOffset = 0, queryStr = "", signal = null, handleId = null) {
        let offset = startOffset;
        const endOffset = (sizeToRead && sizeToRead > 0) ? (startOffset + sizeToRead) : Infinity;
        const chunkSize = 262144; // 256KB chunks (optimizes throughput, fits in WebRTC frames via SCTP slicing)

        // Fast path for single-chunk reads or known small sizes (e.g. thumbnails or files <= 256KB)
        if (sizeToRead && sizeToRead > 0 && (endOffset - startOffset) <= chunkSize) {
            if (signal && signal.aborted) {
                throw new Error("AbortError: Download cancelled");
            }
            const readParams = { path: filePath, query: queryStr, offset: startOffset, size: (endOffset - startOffset) };
            if (handleId) readParams.handleId = handleId;
            const res = await this.sendRPC("Read", readParams);
            if (res.status !== "ok") throw new Error(res.error || "Failed to read file");
            const dataStr = res.data || "";
            if (!dataStr) return new Blob([]);
            const binaryString = atob(dataStr);
            const bytes = new Uint8Array(binaryString.length);
            for (let i = 0; i < binaryString.length; i++) {
                bytes[i] = binaryString.charCodeAt(i);
            }
            if (onProgress) onProgress(bytes.length, sizeToRead);
            return new Blob([bytes]);
        }

        // Pipelined multi-chunk reads when total size is known
        if (sizeToRead && sizeToRead > 0 && isFinite(endOffset)) {
            const totalBytes = endOffset - startOffset;
            const totalChunks = Math.ceil(totalBytes / chunkSize);
            const chunks = new Array(totalChunks);
            let nextChunkIdx = 0;
            let completedBytes = 0;
            let abort = false;
            let firstError = null;

            const concurrency = Math.min(3, totalChunks);

            const worker = async () => {
                while (!abort) {
                    if (signal && signal.aborted) {
                        abort = true;
                        throw new Error("AbortError: Download cancelled");
                    }
                    const idx = nextChunkIdx++;
                    if (idx >= totalChunks) break;

                    const chunkOffset = startOffset + idx * chunkSize;
                    const currentChunkSize = Math.min(chunkSize, endOffset - chunkOffset);
                    const readParams = { path: filePath, query: queryStr, offset: chunkOffset, size: currentChunkSize };
                    if (handleId) readParams.handleId = handleId;

                    try {
                        const res = await this.sendRPC("Read", readParams);
                        if (res.status !== "ok") throw new Error(res.error || "Failed to read file");
                        const dataStr = res.data || "";
                        if (!dataStr) {
                            chunks[idx] = new Uint8Array(0);
                            abort = true;
                            break;
                        }
                        const binaryString = atob(dataStr);
                        const bytes = new Uint8Array(binaryString.length);
                        for (let i = 0; i < binaryString.length; i++) {
                            bytes[i] = binaryString.charCodeAt(i);
                        }
                        chunks[idx] = bytes;
                        completedBytes += bytes.length;
                        if (onProgress) onProgress(completedBytes, sizeToRead);

                        if (bytes.length < currentChunkSize) {
                            // Hit EOF early
                            abort = true;
                            break;
                        }
                    } catch (err) {
                        abort = true;
                        if (!firstError) firstError = err;
                        throw err;
                    }
                }
            };

            const workers = [];
            for (let w = 0; w < concurrency; w++) {
                workers.push(worker());
            }
            await Promise.all(workers);
            if (firstError) throw firstError;

            const validChunks = chunks.filter(c => c && c.length > 0);
            return new Blob(validChunks);
        }

        // Sequential fallback when size is unknown / infinite
        const chunks = [];
        while (offset < endOffset) {
            if (signal && signal.aborted) {
                throw new Error("AbortError: Download cancelled");
            }
            const currentChunkSize = Math.min(chunkSize, endOffset - offset);
            const readParams = { path: filePath, query: queryStr, offset, size: currentChunkSize };
            if (handleId) readParams.handleId = handleId;
            const res = await this.sendRPC("Read", readParams);
            if (res.status !== "ok") throw new Error(res.error || "Failed to read file");
            
            const dataStr = res.data || "";
            if (!dataStr) break;

            const binaryString = atob(dataStr);
            if (binaryString.length === 0) break;

            const bytes = new Uint8Array(binaryString.length);
            for (let i = 0; i < binaryString.length; i++) {
                bytes[i] = binaryString.charCodeAt(i);
            }
            chunks.push(bytes);
            
            offset += bytes.length;
            if (onProgress) onProgress(offset - startOffset, sizeToRead);
            if (bytes.length < currentChunkSize) break;
        }
        
        return new Blob(chunks);
    }

    async readFile(filePath, offset = 0, size = 262144) {
        const res = await this.sendRPC("Read", { path: filePath, offset, size });
        return res.data;
    }

    async writeFile(filePath, offset = 0, dataBase64 = "", truncate = false, eof = true) {
        const res = await this.sendRPC("Write", { path: filePath, offset, data: dataBase64, truncate, eof });
        return res.written;
    }

    async writeFileChunked(filePath, dataBase64, chunkBytes = 262144) {
        if (!dataBase64) {
            await this.writeFile(filePath, 0, "", true, true);
            return 0;
        }
        const raw = atob(dataBase64);
        const total = raw.length;
        if (total === 0) {
            await this.writeFile(filePath, 0, "", true, true);
            return 0;
        }
        let offset = 0;
        let first = true;
        while (offset < total) {
            const end = Math.min(offset + chunkBytes, total);
            let chunk = "";
            for (let i = offset; i < end; i++) chunk += raw[i];
            const b64 = btoa(chunk);
            const isEof = (end >= total);
            await this.writeFile(filePath, offset, b64, first, isEof);
            first = false;
            offset = end;
        }
        return total;
    }

    // Run a shell command on the FileNode as the device user and return its output.
    async exec(dir, input) {
        const res = await this.sendRPC("Exec", { dir, input }, 30000);
        return res.result;
    }

    // Resolve a logical (share-relative) path like "/Knowledge/data" to the
    // real on-disk path on the node; "/" resolves to the FileNode user home.
    async resolvePath(path) {
        const res = await this.sendRPC("ResolvePath", { path });
        return res.result.realPath;
    }

    // Register a new logical share for a real on-disk folder path on the node.
    async addShare(fullPath) {
        const res = await this.sendRPC("AddShare", { path: fullPath }, 30000);
        return res.result;
    }

    // Create a directory on the remote device.
    // For shares, path is share-relative (e.g. "/Documents/MyFolder") -> sends RPC "MkDir".
    // Falls back to "HomeMkDir" if needed.
    async mkDir(path) {
        try {
            const res = await this.sendRPC("MkDir", { path }, 30000);
            return res.result;
        } catch (err) {
            const res = await this.sendRPC("HomeMkDir", { path }, 30000);
            return res.result;
        }
    }

    // Create a directory on the real node filesystem (home browsing).
    async homeMkDir(virtualPath) {
        try {
            const res = await this.sendRPC("HomeMkDir", { path: virtualPath }, 30000);
            return res.result;
        } catch (err) {
            const res = await this.sendRPC("MkDir", { path: virtualPath }, 30000);
            return res.result;
        }
    }

    // Upload a file to the real node filesystem (home browsing). The path is
    // virtual and relative to the FileNode user home; data is base64.
    async homeWrite(virtualPath, dataBase64) {
        const res = await this.sendRPC("HomeWrite", { path: virtualPath, data: dataBase64, eof: true }, 30000);
        return res.result;
    }

    // Copy a file/folder on the remote device. source and target are logical (share-relative) paths.
    async copyFile(sourcePath, targetPath) {
        return this.sendRPC("Copy", { path: sourcePath, target: targetPath });
    }

    // Move a file/folder on the remote device.
    async moveFile(sourcePath, targetPath) {
        return this.sendRPC("Move", { path: sourcePath, target: targetPath });
    }

    // Rename a file/folder on the remote device.
    async renameFile(path, newName) {
        return this.sendRPC("Rename", { path, newName });
    }

    // Remove a file/folder on the remote device.
    async removeFile(path, forced = false) {
        return this.sendRPC("Remove", { path, forced });
    }

    // Get device settings as JSON object.
    async getSettings() {
        const res = await this.sendRPC("GetSettings", {});
        return res.result.settings;
    }

    // Apply device settings from a JSON object.
    async setSettings(settings) {
        return this.sendRPC("SetSettings", { settings });
    }

    static turnServers() {
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
}

// Service Worker P2P Bridge
function setupP2PFetchListener() {
    if (typeof navigator === 'undefined' || !('serviceWorker' in navigator)) return;
    
    const handler = async (event) => {
        if (event.data && event.data.type === 'P2P_FETCH') {
            const port = event.ports && event.ports[0];
            if (!port) {
                console.error('[Window] P2P_FETCH received without port!');
                return;
            }
            try {
                const url = new URL(event.data.url);
                let deviceId = null;
                let pathPart = url.pathname;
                if (pathPart.startsWith('/p2p/')) {
                    const rest = pathPart.substring('/p2p/'.length);
                    const slashIdx = rest.indexOf('/');
                    if (slashIdx < 0) {
                        deviceId = rest;
                        pathPart = '/';
                    } else {
                        deviceId = rest.substring(0, slashIdx);
                        pathPart = rest.substring(slashIdx);
                    }
                }

                console.log('[Window] Handling P2P_FETCH url:', event.data.url, 'deviceId:', deviceId, 'pathPart:', pathPart);

                let drive = null;
                let attempts = 0;
                while (attempts < 35) {
                    if (deviceId && window.__omdGetDrive) {
                        drive = window.__omdGetDrive(deviceId);
                    }
                    if (!drive && window.currentP2PDrive) {
                        drive = window.currentP2PDrive;
                    }
                    if (drive && drive.isConnected) {
                        break;
                    }
                    if (drive && !drive.isConnected && typeof drive.connect === 'function' && !drive._isConnecting) {
                        console.log('[Window] P2P_FETCH: drive not connected, triggering drive.connect()...');
                        drive.connect().catch((e) => console.warn('[Window] Auto-connect on P2P_FETCH failed:', e));
                    }
                    await new Promise(r => setTimeout(r, 100));
                    attempts++;
                }

                const path = decodeURIComponent(pathPart);
                const query = url.search.substring(1);

                if (!drive || !drive.isConnected) {
                    const isThumb = query && (query.includes("resize=") || query.includes("cover=") || query.includes("thumbnail=") || query.includes("poster") || query.includes("width="));
                    if (isThumb) {
                        const sizeMatch = query.match(/(?:width|size|cover)=(\d+)/);
                        const size = sizeMatch ? parseInt(sizeMatch[1]) : 240;
                        const thumbPath = `/api/device_memory_thumb?path=${encodeURIComponent(path)}&size=${size}`;
                        // 1. Try window.__omdHttpDrive
                        const httpDrive = window.__omdHttpDrive;
                        if (httpDrive && typeof httpDrive.fetch === 'function') {
                            try {
                                console.log('[Window] P2P_FETCH fallback to httpDrive thumb:', thumbPath);
                                const res = await httpDrive.fetch(thumbPath);
                                if (res && res.ok) {
                                    const blob = await res.blob();
                                    const mime = res.headers?.get?.('content-type') || 'image/jpeg';
                                    port.postMessage({
                                        status: 200,
                                        headers: {
                                            'Content-Type': mime,
                                            'Content-Length': blob.size.toString()
                                        },
                                        blob: blob
                                    });
                                    return;
                                }
                            } catch (hfErr) {
                                console.warn('[Window] P2P_FETCH httpDrive thumb fallback error:', hfErr);
                            }
                        }
                        // 2. Try direct fetch (proxied via Vite / local node)
                        try {
                            console.log('[Window] P2P_FETCH direct fetch fallback:', thumbPath);
                            const dirRes = await fetch(thumbPath);
                            if (dirRes.ok) {
                                const blob = await dirRes.blob();
                                const mime = dirRes.headers?.get?.('content-type') || 'image/jpeg';
                                if (mime.startsWith('image/')) {
                                    port.postMessage({
                                        status: 200,
                                        headers: {
                                            'Content-Type': mime,
                                            'Content-Length': blob.size.toString()
                                        },
                                        blob: blob
                                    });
                                    return;
                                }
                            }
                        } catch (_) {}
                    }

                    // Fallback for full files via httpDrive / memory api
                    const httpDrive = (typeof window !== 'undefined' && window.__omdGetHttpDrive && deviceId && window.__omdGetHttpDrive(deviceId)) || (typeof window !== 'undefined' ? window.__omdHttpDrive : null);
                    const filePath = `/api/device_memory_file?path=${encodeURIComponent(path)}`;
                    if (httpDrive && typeof httpDrive.fetch === 'function') {
                        try {
                            console.log('[Window] P2P_FETCH fallback to httpDrive file:', filePath);
                            const res = await httpDrive.fetch(filePath);
                            if (res && res.ok) {
                                const blob = await res.blob();
                                const mime = res.headers?.get?.('content-type') || 'application/octet-stream';
                                port.postMessage({
                                    status: 200,
                                    headers: {
                                        'Content-Type': mime,
                                        'Content-Length': blob.size.toString(),
                                        'Accept-Ranges': 'bytes'
                                    },
                                    blob: blob
                                });
                                return;
                            }
                        } catch (hfErr) {
                            console.warn('[Window] P2P_FETCH httpDrive file fallback error:', hfErr);
                        }
                    }
                    try {
                        console.log('[Window] P2P_FETCH direct fetch file fallback:', filePath);
                        const dirRes = await fetch(filePath);
                        if (dirRes && dirRes.ok) {
                            const blob = await dirRes.blob();
                            const mime = dirRes.headers?.get?.('content-type') || 'application/octet-stream';
                            port.postMessage({
                                status: 200,
                                headers: {
                                    'Content-Type': mime,
                                    'Content-Length': blob.size.toString(),
                                    'Accept-Ranges': 'bytes'
                                },
                                blob: blob
                            });
                            return;
                        }
                    } catch (_) {}

                    console.error('[Window] P2P Drive not connected after wait, drive:', drive);
                    throw new Error("P2P Drive not connected");
                }

                const rangeHeader = event.data.headers ? event.data.headers['range'] : null;
                let offset = 0;
                let sizeToRead = 0;
                console.log("[Window] P2P_FETCH path: ", path, "query: ", query, "rangeHeader: ", rangeHeader);

                if (rangeHeader) {
                    const match = rangeHeader.match(/bytes=(\d+)-(\d*)/);
                    if (match) {
                        offset = parseInt(match[1]);
                        if (match[2]) {
                            sizeToRead = parseInt(match[2]) - offset + 1;
                        } else {
                            sizeToRead = 2097152; // Request up to 2MB if no end specified
                        }
                    }
                }

                // The bridge serves two device kinds over the SAME
                // Open/Read protocol:
                // - remote peers: JSON-RPC over the WebRTC DataChannel;
                // - own browser device (File System Access / OPFS): native
                //   block reads straight from its handles. No whole-file
                //   blobs, Range requests supported.
                const isLocal = typeof drive.sendRPC !== 'function';
                if (!isLocal) {
                    console.log('[Window] Resolved drive:', {
                        requested: deviceId,
                        targetLinkId: drive.targetLinkId,
                        tunnelId: drive.tunnelId,
                        ctor: drive.constructor ? drive.constructor.name : '?'
                    });
                }

                let totalSize = 0;
                let mimeType = 'application/octet-stream';
                let openRes = null;

                if (rangeHeader) {
                    if (isLocal) {
                        const meta = await drive.open(path, query);
                        if (!meta || meta.isDir) throw new Error('Path not found: ' + path);
                        totalSize = meta.size || 0;
                        mimeType = meta.mimeType || 'application/octet-stream';
                    } else {
                        openRes = await drive.sendRPC('Open', { path, query });
                        if (openRes.status !== 'ok') throw new Error('File not found or access denied: ' + (openRes.error || ''));
                        totalSize = openRes.result.size;
                        mimeType = openRes.result.mimeType || 'application/octet-stream';
                    }

                    if (offset >= totalSize) {
                        port.postMessage({ status: 416, headers: { 'Content-Range': `bytes */${totalSize}` }, blob: new Blob([]) });
                        return;
                    }
                    if (sizeToRead === 0 || offset + sizeToRead > totalSize) {
                        sizeToRead = totalSize - offset;
                    }

                    let chunkBlob;
                    if (isLocal) {
                        const { bytes } = await drive.readRange(path, offset, sizeToRead, query);
                        chunkBlob = new Blob([bytes], { type: mimeType });
                    } else {
                        chunkBlob = await drive.getFileContent(path, sizeToRead, null, offset, query, null, openRes?.handleId);
                    }

                    port.postMessage({
                        status: 206,
                        headers: {
                            'Content-Type': mimeType,
                            'Content-Range': `bytes ${offset}-${offset + chunkBlob.size - 1}/${totalSize}`,
                            'Content-Length': chunkBlob.size.toString(),
                            'Accept-Ranges': 'bytes'
                        },
                        blob: chunkBlob
                    });
                } else {
                    let blob;
                    if (isLocal) {
                        if (typeof drive.getFileContent === "function") {
                            blob = await drive.getFileContent(path, 0, null, 0, query);
                        } else {
                            const meta = await drive.open(path, query);
                            if (!meta || meta.isDir) throw new Error('Path not found: ' + path);
                            const { bytes } = await drive.readRange(path, 0, 0, query);
                            blob = new Blob([bytes], { type: meta.mimeType || mimeType });
                        }
                    } else {
                        openRes = await drive.sendRPC('Open', { path, query });
                        if (openRes.status !== 'ok') throw new Error('File not found or access denied: ' + (openRes.error || ''));
                        totalSize = openRes.result.size;
                        mimeType = openRes.result.mimeType || 'application/octet-stream';
                        blob = await drive.getFileContent(path, totalSize, null, 0, query, null, openRes?.handleId);
                    }
                    if (blob && blob.type) mimeType = blob.type;
                    console.log("[Window] Fetched blob size:", blob ? blob.size : 0, "mime:", mimeType);
                    port.postMessage({
                        status: 200,
                        headers: {
                            'Content-Type': mimeType,
                            'Content-Length': (blob ? blob.size : totalSize).toString()
                        },
                        blob: blob
                    });
                }
            } catch (e) {
                console.error("[Window] SW P2P Fetch error:", e);
                const isWriting = e.message && (e.message.includes("writing") || e.message.includes("locked") || e.message.includes("busy"));
                port.postMessage({ error: e.message, status: isWriting ? 423 : 500, writing: !!isWriting });
            }
        }
    };

    navigator.serviceWorker.addEventListener('message', handler);
    if (typeof window !== 'undefined') {
        window.addEventListener('message', handler);
    }
}
setupP2PFetchListener();
