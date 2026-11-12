import { WebRTCDataChannelDrive } from './webrtc-drive.js';
import { getStoredOmdKey } from './omd-key.js';
import { getClientPlatform } from './platform.js';

const CR = 13;
const LF = 10;
const SEND_CHUNK_SIZE = 16384;
const MAX_HEADER_BYTES = 65536;
const BUFFERED_LIMIT = 1048576;

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

function abortError() {
    if (typeof DOMException !== 'undefined') {
        return new DOMException('The operation was aborted.', 'AbortError');
    }
    const e = new Error('The operation was aborted.');
    e.name = 'AbortError';
    return e;
}

function latin1(buf, start, end) {
    let s = '';
    const STEP = 4096;
    for (let i = start; i < end; i += STEP) {
        s += String.fromCharCode.apply(null, buf.subarray(i, Math.min(end, i + STEP)));
    }
    return s;
}

function findCRLF(buf, from, to) {
    for (let i = from; i < to - 1; i++) {
        if (buf[i] === CR && buf[i + 1] === LF) return i;
    }
    return -1;
}

function findHeaderEnd(buf, len) {
    for (let i = 0; i < len - 3; i++) {
        if (buf[i] === CR && buf[i + 1] === LF && buf[i + 2] === CR && buf[i + 3] === LF) return i;
    }
    return -1;
}

function concatBytes(a, b) {
    const out = new Uint8Array(a.length + b.length);
    out.set(a, 0);
    out.set(b, a.length);
    return out;
}

export class HttpResponseParser {
    constructor(callbacks) {
        this.cb = callbacks || {};
        this.method = (this.cb.method ? String(this.cb.method) : 'GET').toUpperCase();
        this.buf = new Uint8Array(16384);
        this.len = 0;
        this.state = 'headers';
        this.status = 0;
        this.statusText = '';
        this.headers = [];
        this.httpMinor = 1;
        this.serverWillClose = false;
        this.chunkRemain = 0;
        this.finished = false;
        this.failed = false;
    }

    _fail(err) {
        if (this.finished || this.failed) return;
        this.failed = true;
        if (this.cb.onError) this.cb.onError(err);
    }

    _finish() {
        if (this.finished || this.failed) return;
        this.finished = true;
        this.state = 'done';
        if (this.cb.onDone) this.cb.onDone();
    }

    _ensure(extra) {
        if (this.len + extra <= this.buf.length) return;
        let cap = this.buf.length * 2;
        while (cap < this.len + extra) cap *= 2;
        const nb = new Uint8Array(cap);
        nb.set(this.buf.subarray(0, this.len));
        this.buf = nb;
    }

    _consume(n) {
        this.buf.copyWithin(0, n, this.len);
        this.len -= n;
    }

    push(bytes) {
        if (this.finished || this.failed) return;
        if (!bytes || bytes.length === 0) return;
        this._ensure(bytes.length);
        this.buf.set(bytes, this.len);
        this.len += bytes.length;
        this._process();
    }

    end() {
        if (this.finished || this.failed) return;
        if (this.state === 'untilClose') {
            this._finish();
        } else {
            this._fail(new Error('Connection closed mid-response'));
        }
    }

    abort() {
        if (this.finished || this.failed) return;
        this.failed = true;
    }

    _getHeader(nameLower) {
        for (const [k, v] of this.headers) {
            if (k.toLowerCase() === nameLower) return v;
        }
        return null;
    }

    _parseHead(end) {
        const head = latin1(this.buf, 0, end);
        const lines = head.split('\r\n');
        const m = /^HTTP\/(\d)\.(\d)\s+(\d{3})(?:\s+(.*))?$/.exec(lines[0]);
        if (!m) throw new Error('Malformed HTTP status line');
        this.status = parseInt(m[3], 10);
        this.statusText = m[4] || '';
        this.headers = [];
        for (let i = 1; i < lines.length; i++) {
            const idx = lines[i].indexOf(':');
            if (idx <= 0) continue;
            this.headers.push([lines[i].substring(0, idx).trim(), lines[i].substring(idx + 1).trim()]);
        }
        const connHeader = (this._getHeader('connection') || '').toLowerCase();
        if (connHeader.includes('close')) this.serverWillClose = true;
        const noBody = this.method === 'HEAD' || this.status === 204 || this.status === 205 || this.status === 304 || (this.status >= 100 && this.status < 200);
        if (noBody) {
            this._emitHeaders();
            this._finish();
            return true;
        }
        const te = (this._getHeader('transfer-encoding') || '').toLowerCase();
        if (te.includes('chunked')) {
            this.state = 'chunkSize';
        } else {
            const cl = this._getHeader('content-length');
            if (cl !== null) {
                const n = parseInt(cl, 10);
                if (isNaN(n) || n < 0) throw new Error('Bad Content-Length');
                if (n === 0) {
                    this._emitHeaders();
                    this._finish();
                    return true;
                }
                this.state = 'body';
                this.chunkRemain = n;
            } else {
                this.state = 'untilClose';
            }
        }
        this._emitHeaders();
        return false;
    }

    _emitHeaders() {
        if (this.cb.onHeaders) {
            this.cb.onHeaders({ status: this.status, statusText: this.statusText, headers: this.headers });
        }
    }

    _process() {
        while (!this.finished && !this.failed) {
            if (this.state === 'headers') {
                const end = findHeaderEnd(this.buf, this.len);
                if (end < 0) {
                    if (this.len > MAX_HEADER_BYTES) this._fail(new Error('HTTP headers too large'));
                    return;
                }
                let consumed;
                try {
                    consumed = this._parseHead(end) ? end + 4 : null;
                } catch (e) {
                    this._consume(this.len);
                    this._fail(e);
                    return;
                }
                if (consumed === null) {
                    this._consume(end + 4);
                } else {
                    this._consume(consumed);
                    return;
                }
            } else if (this.state === 'body') {
                if (this.len === 0) return;
                const avail = Math.min(this.len, this.chunkRemain);
                if (this.cb.onBody) this.cb.onBody(this.buf.slice(0, avail));
                this._consume(avail);
                this.chunkRemain -= avail;
                if (this.chunkRemain === 0) this._finish();
            } else if (this.state === 'untilClose') {
                if (this.len === 0) return;
                if (this.cb.onBody) this.cb.onBody(this.buf.slice(0, this.len));
                this._consume(this.len);
                return;
            } else if (this.state === 'chunkSize') {
                const i = findCRLF(this.buf, 0, this.len);
                if (i < 0) {
                    if (this.len > MAX_HEADER_BYTES) this._fail(new Error('Chunk size line too large'));
                    return;
                }
                const size = parseInt(latin1(this.buf, 0, i).split(';')[0].trim(), 16);
                if (isNaN(size) || size < 0) {
                    this._fail(new Error('Bad chunk size'));
                    return;
                }
                this._consume(i + 2);
                if (size === 0) {
                    this.state = 'trailers';
                } else {
                    this.chunkRemain = size;
                    this.state = 'chunkData';
                }
            } else if (this.state === 'chunkData') {
                if (this.len === 0) return;
                const avail = Math.min(this.len, this.chunkRemain);
                if (this.cb.onBody) this.cb.onBody(this.buf.slice(0, avail));
                this._consume(avail);
                this.chunkRemain -= avail;
                if (this.chunkRemain === 0) this.state = 'chunkEnd';
            } else if (this.state === 'chunkEnd') {
                if (this.len < 2) return;
                if (this.buf[0] !== CR || this.buf[1] !== LF) {
                    this._fail(new Error('Missing chunk terminator'));
                    return;
                }
                this._consume(2);
                this.state = 'chunkSize';
            } else if (this.state === 'trailers') {
                const i = findCRLF(this.buf, 0, this.len);
                if (i < 0) {
                    if (this.len > MAX_HEADER_BYTES) this._fail(new Error('Trailers too large'));
                    return;
                }
                this._consume(i + 2);
                if (i === 0) {
                    this._finish();
                }
            } else {
                return;
            }
        }
    }
}

export function createWebRTCTransport(targetLinkId, signalingUrl = '/api/signaling', port = 8000) {
    let drive = null;
    return {
        get isOpen() {
            return !!drive && drive.isConnected && drive.dc && drive.dc.readyState === 'open';
        },
        get bufferedAmount() {
            return drive && drive.dc ? drive.dc.bufferedAmount : 0;
        },
        async connect(onData, onClose, timeoutMs = 30000) {
            drive = new WebRTCDataChannelDrive(targetLinkId, signalingUrl, port, '');
            // One-shot tunnel per request - never auto-reconnect
            drive.autoReconnectEnabled = false;
            let disconnected = false;
            drive.onRawMessage = (data) => {
                let bytes = null;
                if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
                else if (ArrayBuffer.isView(data)) bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
                if (bytes) onData(bytes);
            };
            drive.onStatusChange = (status) => {
                if (status === 'disconnected' || status === 'error') {
                    disconnected = true;
                    onClose();
                }
            };
            await drive.connect();
            const deadline = Date.now() + timeoutMs;
            while (Date.now() < deadline) {
                if (drive.dc && drive.dc.readyState === 'open') return;
                if (disconnected || drive._disposed || (!drive.pc && !drive._isConnecting && !drive.isConnected)) {
                    try { drive.disconnect(true); } catch (_) {}
                    drive = null;
                    throw new Error('Target node offline or unreachable');
                }
                await sleep(40);
            }
            try { drive.disconnect(true); } catch (_) {}
            drive = null;
            throw new Error('P2P tunnel did not open in time');
        },
        send(bytes) {
            if (!drive || !drive.dc || drive.dc.readyState !== 'open') throw new Error('Channel not open');
            drive.dc.send(bytes);
        },
        close() {
            if (drive) {
                try { drive.disconnect(true); } catch (_) {}
            }
            drive = null;
        }
    };
}

class ChannelConn {
    constructor(transportFactory, opts = {}) {
        this.transportFactory = transportFactory;
        this.headerTimeoutMs = opts.headerTimeoutMs ?? 60000;
        this.onIdle = opts.onIdle || null;
        this.onDead = opts.onDead || null;
        this.transport = null;
        this.parser = null;
        this._streamCtrl = null;
        this._rejectHeaders = null;
        this.busy = false;
        this.dead = false;
        this.reuseCount = 0;
        this._countedInPool = false;
    }

    async open(timeoutMs) {
        const t = this.transportFactory();
        this.transport = t;
        await t.connect(
            (bytes) => {
                if (this.parser) this.parser.push(bytes);
            },
            () => this._onClose(),
            timeoutMs
        );
    }

    isOpen() {
        return !!this.transport && this.transport.isOpen && !this.dead;
    }

    isDead() {
        return this.dead;
    }

    canReuse() {
        return this.isOpen() && !this.busy && this._reuseOK !== false && this.parser === null;
    }

    _onClose() {
        this.dead = true;
        if (this.transport) {
            const t = this.transport;
            this.transport = null;
            try { t.close(); } catch (_) {}
        }
        if (this.parser) {
            const p = this.parser;
            p.end();
        }
        if (this._rejectHeaders) {
            const r = this._rejectHeaders;
            this._rejectHeaders = null;
            r(new Error('P2P connection closed before response'));
        }
        if (this._abortStream) {
            const f = this._abortStream;
            this._abortStream = null;
            f(new Error('P2P connection closed mid-response'));
        }
        if (!this.busy && this.onDead) {
            const cb = this.onDead;
            this.onDead = null;
            this.onIdle = null;
            cb(this);
        }
    }

    destroy() {
        this.dead = true;
        this._rejectHeaders = null;
        this._streamCtrl = null;
        const p = this.parser;
        this.parser = null;
        if (p) {
            p.abort();
        }
        if (this.transport) {
            try { this.transport.close(); } catch (_) {}
        }
        this.transport = null;
        this.busy = false;
    }

    _settleFailure(err) {
        if (this._rejectHeaders) {
            const r = this._rejectHeaders;
            this._rejectHeaders = null;
            r(err);
        }
        if (this._streamCtrl) {
            const ctrl = this._streamCtrl;
            this._streamCtrl = null;
            try { ctrl.error(err); } catch (_) {}
        }
        this.destroy();
        if (this.onDead) {
            const cb = this.onDead;
            this.onDead = null;
            this.onIdle = null;
            cb(this);
        }
    }

    async request(method, path, headers, bodyBytes, { signal, timeoutMs } = {}) {
        if (!this.isOpen()) throw new Error('Channel not open');
        if (this.busy) throw new Error('Connection already has an active request');

        this.busy = true;
        this._reuseOK = true;

        const body = bodyBytes || new Uint8Array(0);
        const hasHeader = (nameLower) => headers.some(([k]) => k.toLowerCase() === nameLower);
        let head = method + ' ' + path + ' HTTP/1.1\r\n';
        if (!hasHeader('host')) head += 'Host: localhost\r\n';
        if (!hasHeader('accept-encoding')) head += 'Accept-Encoding: identity\r\n';
        if (!hasHeader('connection')) head += 'Connection: keep-alive\r\n';
        for (const [k, v] of headers) {
            const kl = k.toLowerCase();
            if (kl === 'content-length' || kl === 'transfer-encoding') continue;
            head += k + ': ' + v + '\r\n';
        }
        if (body.length > 0 || method === 'POST' || method === 'PUT' || method === 'PATCH') {
            head += 'Content-Length: ' + body.length + '\r\n';
        }
        head += '\r\n';

        const encoder = new TextEncoder();
        const out = concatBytes(encoder.encode(head), body);

        let headerResolve = null;
        let headerReject = null;
        const headersPromise = new Promise((resolve, reject) => {
            headerResolve = resolve;
            headerReject = reject;
        });
        this._rejectHeaders = headerReject;

        let streamCtrl = null;
        const queued = [];
        let completed = false;
        let completionResolve = null;
        const completion = new Promise((resolve) => {
            completionResolve = (how) => {
                if (!completed) {
                    completed = true;
                    resolve(how);
                }
            };
        });

        let headerSettled = false;
        const settleHeadersOk = (info) => {
            if (!headerSettled) {
                headerSettled = true;
                this._rejectHeaders = null;
                headerResolve(info);
            }
        };

        const parser = new HttpResponseParser({
            method,
            onHeaders: (info) => {
                settleHeadersOk(info);
            },
            onBody: (bytes) => {
                if (streamCtrl) streamCtrl.enqueue(bytes);
                else queued.push(bytes);
            },
            onDone: () => {
                this.parser = null;
                this._streamCtrl = null;
                if (streamCtrl) {
                    try { streamCtrl.close(); } catch (_) {}
                }
                completionResolve('done');
            },
            onError: (err) => {
                this.parser = null;
                this._streamCtrl = null;
                if (!headerSettled) {
                    headerSettled = true;
                    this._rejectHeaders = null;
                    headerReject(err);
                }
                if (streamCtrl) {
                    try { streamCtrl.error(err); } catch (_) {}
                }
                completionResolve('error');
            }
        });
        this.parser = parser;
        this._streamCtrl = null;

        const stream = new ReadableStream({
            start: (ctrl) => {
                streamCtrl = ctrl;
                this._streamCtrl = ctrl;
                while (queued.length > 0) ctrl.enqueue(queued.shift());
            },
            cancel: () => {
                if (completed) return;
                completionResolve('canceled');
                this.parser = null;
                this._reuseOK = false;
                this._settleFailure(abortError());
            }
        });

        const onSignal = () => {
            if (completed) return;
            if (headerSettled) {
                completionResolve('aborted');
                this.parser = null;
                this._reuseOK = false;
                this._settleFailure(abortError());
            }
        };
        if (signal) {
            if (signal.aborted) {
                this.busy = false;
                this.destroy();
                throw abortError();
            }
            signal.addEventListener('abort', onSignal);
        }

        const TIMEOUT = Symbol('timeout');
        const effectiveTimeout = timeoutMs || this.headerTimeoutMs;
        const timeoutPromise = sleep(effectiveTimeout).then(() => TIMEOUT);

        try {
            await this._sendAll(out, signal);
        } catch (e) {
            if (signal) signal.removeEventListener('abort', onSignal);
            this._settleFailure(e);
            throw e;
        }

        let info;
        try {
            const winner = await Promise.race([headersPromise, timeoutPromise]);
            if (winner === TIMEOUT) throw new Error('Response timeout');
            info = winner;
        } catch (e) {
            if (signal) signal.removeEventListener('abort', onSignal);
            this._settleFailure(e instanceof Error ? e : new Error(String(e)));
            throw e;
        }

        for (const [k, v] of info.headers) {
            if (k.toLowerCase() === 'connection' && v.toLowerCase().includes('close')) {
                this._reuseOK = false;
            }
        }
        if (parser.serverWillClose) this._reuseOK = false;

        completion.then((how) => {
            if (signal) signal.removeEventListener('abort', onSignal);
            if (how === 'done' && this._reuseOK !== false && this.isOpen()) {
                this.busy = false;
                this.reuseCount++;
                if (this.onIdle) this.onIdle(this);
            } else {
                this._reuseOK = false;
                this.destroy();
                if (this.onDead) this.onDead(this);
                if (how !== 'done' && !this.dead) {
                    this._settleFailure(how === 'aborted' ? abortError() : new Error('Response failed'));
                }
            }
        });

        const nullBodyStatus = info.status === 204 || info.status === 205 || info.status === 304 || (info.status >= 100 && info.status < 200);
        const resp = new Response(nullBodyStatus ? null : stream, {
            status: info.status,
            statusText: info.statusText,
            headers: new Headers(info.headers)
        });
        resp._rawHeaders = info.headers;
        return resp;
    }

    async _sendAll(bytes, signal) {
        for (let off = 0; off < bytes.length; off += SEND_CHUNK_SIZE) {
            if (signal && signal.aborted) throw abortError();
            while (this.transport.bufferedAmount > BUFFERED_LIMIT) {
                await sleep(20);
                if (signal && signal.aborted) throw abortError();
                if (!this.isOpen()) throw new Error('Connection lost during send');
            }
            const end = Math.min(off + SEND_CHUNK_SIZE, bytes.length);
            this.transport.send(bytes.slice(off, end));
        }
    }
}

function collectHeaders(initHeaders) {
    const out = [];
    if (!initHeaders) return out;
    if (typeof Headers !== 'undefined' && initHeaders instanceof Headers) {
        initHeaders.forEach((v, k) => out.push([k, v]));
    } else if (Array.isArray(initHeaders)) {
        for (const [k, v] of initHeaders) out.push([k, String(v)]);
    } else {
        for (const k of Object.keys(initHeaders)) out.push([k, String(initHeaders[k])]);
    }
    return out;
}

async function encodeBody(body) {
    if (body == null) return null;
    if (typeof body === 'string') return new TextEncoder().encode(body);
    if (body instanceof ArrayBuffer) return new Uint8Array(body);
    if (ArrayBuffer.isView(body)) return new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
    if (typeof Blob !== 'undefined' && body instanceof Blob) {
        return new Uint8Array(await body.arrayBuffer());
    }
    if (typeof URLSearchParams !== 'undefined' && body instanceof URLSearchParams) {
        return new TextEncoder().encode(body.toString());
    }
    throw new TypeError('Unsupported request body type');
}

function resolveStoredDidPub() {
    if (typeof window !== 'undefined') {
        const winDid = window.identity?.mainPub || window.identity?.pair?.pub;
        if (winDid) return winDid;
    }
    if (typeof localStorage !== 'undefined') {
        try {
            const raw = localStorage.getItem('omd_identity');
            if (raw) {
                const parsed = JSON.parse(raw);
                const pub = parsed?.mainPub || parsed?.pair?.pub || parsed?.pub;
                if (typeof pub === 'string' && pub.trim()) return pub.trim();
            }
        } catch (_) {}
    }
    return null;
}

export class HttpDrive {
    constructor(opts = {}) {
        this.targetLinkId = opts.targetLinkId;
        this.port = opts.port ?? 8000;
        this.signalingUrl = opts.signalingUrl ?? '/api/signaling';
        this.maxConns = opts.maxConns ?? 3;
        this.connectTimeoutMs = opts.connectTimeoutMs ?? 30000;
        this.headerTimeoutMs = opts.headerTimeoutMs ?? 60000;
        this.isPrivateMode = opts.isPrivateMode;
        this.didPub = opts.didPub || null;
        this.extraAiHeaders = opts.extraAiHeaders || null;   // {[header]: value} e.g. X-OMD-Ai-Mode/X-OMD-Ai-Node
        this.authToken = opts.authToken || getStoredOmdKey() || '';
        this.transportFactory =
            opts.transportFactory ||
            (() => createWebRTCTransport(this.targetLinkId, this.signalingUrl, this.port));
        this.idle = [];
        this.total = 0;
        this.waiters = [];
        this.closed = false;
    }

    setAuth(token) {
        this.authToken = token;
    }

    setPrivateMode(isPrivate) {
        this.isPrivateMode = isPrivate;
    }

    setAiHeaders(headers) {
        this.extraAiHeaders = headers || null;
    }

    close() {
        this.closed = true;
        for (const c of this.idle.splice(0)) {
            this._releaseConn(c);
        }
        this.total = 0;
        const waiters = this.waiters.splice(0);
        for (const w of waiters) w.reject(new Error('HttpDrive closed'));
    }

    _releaseConn(c) {
        if (!c) return;
        const idx = this.idle.indexOf(c);
        if (idx >= 0) this.idle.splice(idx, 1);
        c.destroy();
        if (c._countedInPool) {
            c._countedInPool = false;
            this.total = Math.max(0, this.total - 1);
        }
    }

    _newConn() {
        return new ChannelConn(this.transportFactory, {
            headerTimeoutMs: this.headerTimeoutMs,
            onIdle: (c) => {
                if (!this.closed && c.canReuse()) {
                    if (!this.idle.includes(c)) this.idle.push(c);
                } else {
                    this._releaseConn(c);
                }
                this._pump();
            },
            onDead: (c) => {
                this._releaseConn(c);
                this._pump();
            }
        });
    }

    async _acquire(connectTimeoutMs = 0) {
        if (this.closed) throw new Error('HttpDrive closed');
        while (this.idle.length > 0) {
            const idleConn = this.idle.pop();
            if (idleConn && idleConn.isOpen()) return idleConn;
            this._releaseConn(idleConn);
        }
        const effTimeout = connectTimeoutMs > 0 ? connectTimeoutMs : this.connectTimeoutMs;
        if (this.total < this.maxConns) {
            const conn = this._newConn();
            conn._countedInPool = true;
            this.total++;
            try {
                await conn.open(effTimeout);
            } catch (e) {
                this._releaseConn(conn);
                throw e;
            }
            return conn;
        }
        return new Promise((resolve, reject) => this.waiters.push({ resolve, reject, connectTimeoutMs: effTimeout }));
    }

    _pump() {
        if (this.closed) return;
        while (this.waiters.length > 0) {
            const w = this.waiters.shift();
            let liveIdle = null;
            while (this.idle.length > 0) {
                const candidate = this.idle.pop();
                if (candidate && candidate.isOpen()) {
                    liveIdle = candidate;
                    break;
                }
                this._releaseConn(candidate);
            }
            if (liveIdle) {
                w.resolve(liveIdle);
                continue;
            }
            if (this.total < this.maxConns) {
                const conn = this._newConn();
                conn._countedInPool = true;
                this.total++;
                conn
                    .open(w.connectTimeoutMs || this.connectTimeoutMs)
                    .then(() => w.resolve(conn))
                    .catch((e) => {
                        this._releaseConn(conn);
                        w.reject(e);
                    })
                    .finally(() => this._pump());
                return;
            }
            this.waiters.unshift(w);
            return;
        }
    }

    async fetch(input, init = {}) {
        const urlStr = typeof input === 'string' ? input : input && input.url ? input.url : String(input);
        const u = new URL(urlStr, 'https://p2p.local');
        const path = u.pathname + u.search;
        const method = (init.method || 'GET').toUpperCase();
        const headers = collectHeaders(init.headers);
        const activeToken = this.authToken || getStoredOmdKey() || '';
        if (activeToken && !headers.some(([k]) => k.toLowerCase() === 'token') && !headers.some(([k]) => k.toLowerCase() === 'authorization') && !headers.some(([k]) => k.toLowerCase() === 'x-omd-ai-token')) {
            headers.push(['Token', activeToken]);
        }
        const effectiveDid = this.didPub || resolveStoredDidPub();
        if (effectiveDid && !headers.some(([k]) => k.toLowerCase() === 'x-omd-caller-did')) {
            headers.push(['X-OMD-Caller-DID', effectiveDid]);
        }
        if (this.isPrivateMode !== undefined && !headers.some(([k]) => k.toLowerCase() === 'x-omd-private-mode')) {
            headers.push(['X-OMD-Private-Mode', this.isPrivateMode ? '1' : '0']);
        }
        if (!headers.some(([k]) => k.toLowerCase() === 'x-omd-platform')) {
            headers.push(['X-OMD-Platform', getClientPlatform()]);
        }
        if (this.extraAiHeaders && typeof this.extraAiHeaders === 'object') {
            for (const [k, v] of Object.entries(this.extraAiHeaders)) {
                if (v && !headers.some(([hk]) => hk.toLowerCase() === k.toLowerCase())) {
                    headers.push([k, v]);
                }
            }
        }
        const bodyBytes = await encodeBody(init.body);

        for (let attempt = 0; ; attempt++) {
            const conn = await this._acquire(init.connectTimeoutMs || 0);
            const wasReused = conn.reuseCount > 0;
            try {
                return await conn.request(method, path, headers, bodyBytes, {
                    signal: init.signal,
                    timeoutMs: init.timeoutMs || init.headerTimeoutMs
                });
            } catch (e) {
                if (init.signal && init.signal.aborted) throw e;
                if (attempt === 0 && wasReused && !(e && e.name === 'AbortError')) continue;
                throw e;
            }
        }
    }
}
