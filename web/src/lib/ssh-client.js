const ERROR_EAGAIN = -37;
const ERROR_CHANNEL_UNKNOWN = -23;
const BASE_URL = '/libssh2';

let modulePromise = null;

function loadModule() {
  if (!modulePromise) {
    // Loaded through Function indirection: invisible to Vite's import-analysis
    // (which would append ?import and break the public/ module in dev).
    const dynamicImport = new Function('p', 'return import(p)');
    modulePromise = dynamicImport(BASE_URL + '/libssh2.mjs').then((mod) =>
      mod.default({ locateFile: (f) => BASE_URL + '/' + f })
    );
  }
  return modulePromise;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function debugPreview(buf) {
  if (!buf) return "[empty]";
  let arr;
  if (typeof buf === "string") {
    arr = new TextEncoder().encode(buf);
  } else if (buf instanceof Uint8Array) {
    arr = buf;
  } else if (buf instanceof ArrayBuffer) {
    arr = new Uint8Array(buf);
  } else if (ArrayBuffer.isView(buf)) {
    arr = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
  } else {
    arr = new Uint8Array(0);
  }
  const len = arr.length;
  // Try to decode ASCII preview
  let ascii = "";
  for (let i = 0; i < Math.min(len, 40); i++) {
    const c = arr[i];
    ascii += (c >= 32 && c <= 126) ? String.fromCharCode(c) : ".";
  }
  return `[${len} bytes] "${ascii}"`;
}

// Browser-side SSH client (libssh2.js wasm). Transport-agnostic: feed bytes
// with push() and provide a send() callback via bind(). Works over any
// reliable ordered channel (P2P DataChannel, WebSocket, ...).
export class SshClient {
  constructor({ user, pass, onData, onStatus, onError }) {
    this.user = user;
    this.pass = pass;
    this.onData = onData;
    this.onStatus = onStatus;
    this.onError = onError;
    this.sess = null;
    this.ch = null;
    this.closed = false;
    this._send = null;
    this._pendingWrite = [];
    this._readTimer = null;
  }

  _status(s) {
    console.log("[SSH Status]", s);
    if (this.onStatus) this.onStatus(s);
  }

  _error(e) {
    console.error("[SSH Error]", e);
    if (this.onError) this.onError(e);
  }

  // Loads the wasm module and prepares the SSH session. No transport I/O yet.
  async init() {
    if (this.closed) throw new Error('client closed');
    this._status('loading');
    const ssh2 = await loadModule();
    if (this.closed) throw new Error('client closed');
    this.ssh2 = ssh2;
    this.sess = new ssh2._SESSION({});
    this.sess.send = (buf) => {
      if (this._send) this._send(buf);
    };
    this._status('module ready');
  }

  // Provide the transport: send(bytes) is called for every outgoing SSH packet.
  bind({ send }) {
    this._send = send;
  }

  // Feed incoming transport bytes into the SSH stack.
  push(data) {
    if (!this.sess || this.closed) return;
    try {
      let buf;
      if (data instanceof Uint8Array) {
        buf = data;
      } else if (data instanceof ArrayBuffer) {
        buf = new Uint8Array(data);
      } else if (ArrayBuffer.isView(data)) {
        buf = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
      } else if (typeof data === "string") {
        buf = new TextEncoder().encode(data);
      } else {
        console.warn("[SSH PUSH] Unknown data type:", typeof data, data);
        return;
      }
      console.log(`[SSH PUSH IN] ${buf.length} bytes -> libssh2`);
      const CHUNK_SIZE = 16384;
      for (let i = 0; i < buf.length; i += CHUNK_SIZE) {
        const rawChunk = buf.subarray(i, Math.min(i + CHUNK_SIZE, buf.length));
        const chunk = new Uint8Array(rawChunk.length);
        chunk.set(rawChunk);
        try {
          const rc = this.sess.pushdata(chunk);
          console.log(`[SSH pushdata rc] ${rc} for chunk len=${chunk.length}`);
        } catch (e) {
          console.error("[SSH pushdata Exception]", e);
        }
        this._drain();
      }
    } catch (err) {
      console.error("[Protocol Error] pushdata exception:", err);
      this._error('pushdata: ' + err.message);
    }
  }

  // SSH password authentication with retries while handshake is in flight
  // (login() returns EAGAIN/-23 until the banner and kex complete).
  async login(user, pass) {
    const u = user !== undefined ? user : this.user;
    const p = pass !== undefined ? pass : this.pass;
    this._status('authenticating');
    for (let i = 0; i < 100; i++) {
      if (this.closed) throw new Error('client closed');
      let rc;
      try {
        rc = this.sess.login(u, p);
      } catch (e) {
        console.error("[SSH Auth Exception]", e);
        throw new Error('Login error: ' + (e.message || e));
      }
      console.log(`[SSH Auth Attempt #${i+1}] rc=${rc}`);
      if (rc === 0) {
        this._status('authenticated');
        return;
      }
      if (rc !== ERROR_EAGAIN && rc !== ERROR_CHANNEL_UNKNOWN) {
        console.error(`[SSH Auth Failed] rc=${rc}`);
        throw new Error('Incorrect username or password');
      }
      await sleep(200);
    }
    throw new Error('Login timeout (peer did not complete handshake)');
  }

  // Open an interactive shell channel: pty-req + shell.
  async openShell() {
    this._status('opening channel');
    let ch = null;
    for (let i = 0; i < 60; i++) {
      if (this.closed) throw new Error('client closed');
      try {
        ch = this.sess.channel();
      } catch (e) {
        console.error("[SSH channel Exception]", e);
      }
      console.log(`[SSH Channel Open Attempt #${i+1}] ch=${ch ? (ch.active ? 'active' : 'inactive') : 'null'}`);
      if (ch && ch.active) {
        break;
      }
      await sleep(100);
    }

    if (!ch || !ch.active) {
      throw new Error('channel open timeout');
    }

    this.ch = ch;

    let hasPty = false;
    let hasShell = false;
    for (let i = 0; i < 60 && !(hasPty && hasShell); i++) {
      if (this.closed) throw new Error('client closed');

      if (!hasPty) {
        let rcPty;
        try {
          // Full-screen TUIs (opencode/bubbletea) need 256-color terminfo.
          rcPty = ch.pty('xterm-256color');
        } catch (e) {
          console.error('[SSH PTY Exception]', e);
        }
        console.log(`[SSH PTY Attempt #${i+1}] rc=${rcPty}`);
        if (rcPty === 0) {
          hasPty = true;
        } else if (rcPty !== ERROR_EAGAIN && rcPty !== ERROR_CHANNEL_UNKNOWN && rcPty !== undefined) {
          throw new Error('pty request failed: ' + rcPty);
        }
      }

      if (hasPty && !hasShell) {
        let rcShell;
        try {
          rcShell = ch.shell();
        } catch (e) {
          console.error('[SSH Shell Exception]', e);
        }
        console.log(`[SSH Shell Attempt #${i+1}] rc=${rcShell}`);
        if (rcShell === 0) {
          hasShell = true;
        } else if (rcShell !== ERROR_EAGAIN && rcShell !== ERROR_CHANNEL_UNKNOWN && rcShell !== undefined) {
          throw new Error('shell request failed: ' + rcShell);
        }
      }

      if (!(hasPty && hasShell)) await sleep(100);
    }

    if (!(hasPty && hasShell)) {
      this.ch = null;
      throw new Error('openShell timeout (pty=' + hasPty + ', shell=' + hasShell + ')');
    }

    this._status('shell');
    try {
      console.log("[SSH Channel methods]", Object.getOwnPropertyNames(Object.getPrototypeOf(this.ch)));
      console.log("[SSH Session methods]", Object.getOwnPropertyNames(Object.getPrototypeOf(this.sess)));
    } catch (e) {}

    // Apply a resize that was requested before the channel existed
    // (pty starts at the libssh2 default 80x24, then follows window-change).
    if (this._lastCols && this._lastRows) {
      try {
        console.log(`[SSH Init Resize] cols=${this._lastCols}, rows=${this._lastRows}`);
        this.ch.pty_size(this._lastCols, this._lastRows);
      } catch (e) {
        console.error('[SSH Init Resize Exception]', e);
      }
    }

    this._startReadLoop();
  }

  _drain() {
    if (!this.ch || this.closed) return;
    this._flushPending();
    for (let i = 0; i < 256 && !this.closed; i++) {
      let gotData = false;
      try {
        const msgOut = this.ch.read();
        if (msgOut && msgOut.length > 0) {
          gotData = true;
          this.onData && this.onData(msgOut);
        }
      } catch (e) {
        console.error("[Protocol Error] SSH ch.read exception:", e);
        break;
      }
      try {
        if (typeof this.ch.read_err === "function") {
          const msgErr = this.ch.read_err();
          if (msgErr && msgErr.length > 0) {
            gotData = true;
            this.onData && this.onData(msgErr);
          }
        }
      } catch (e) {
        console.error("[Protocol Error] SSH ch.read_err exception:", e);
        break;
      }
      if (!gotData) break;
    }
  }

  _flushPending() {
    while (this._pendingWrite.length && this.ch) {
      const data = this._pendingWrite[0];
      let rc;
      try {
        rc = this.ch.write(data);
        console.log(`[SSH FLUSH WRITE] len=${data.length} -> rc=${rc}`);
        if (rc < 0 && rc !== ERROR_EAGAIN) console.error("[Protocol Error] SSH flush write returned error code:", rc);
      } catch (e) {
        console.error("[Protocol Error] SSH ch.write exception in flush:", e);
        this._pendingWrite.shift();
        return;
      }
      if (rc === data.length) {
        this._pendingWrite.shift();
        continue;
      }
      if (rc === ERROR_EAGAIN || rc === 0) return;
      if (rc > 0) this._pendingWrite[0] = data.slice(rc);
      else this._pendingWrite.shift();
    }
  }

  _startReadLoop() {
    this._readTimer = setInterval(() => {
      if (this.closed) return;
      this._drain();
    }, 100);
  }

  write(data) {
    if (!this.ch || this.closed) return;
    console.log("[SSH WRITE FROM USER/XTERM]", debugPreview(typeof data === "string" ? new TextEncoder().encode(data) : data));
    let rc;
    try {
      rc = this.ch.write(data);
      console.log(`[SSH ch.write] rc=${rc}`);
      if (rc < 0 && rc !== ERROR_EAGAIN) console.error("[Protocol Error] SSH write returned error code:", rc);
    } catch (e) {
      console.error("[Protocol Error] SSH ch.write exception in write():", e);
      return;
    }
    if (rc === ERROR_EAGAIN || rc === 0) {
      console.log(`[SSH WRITE QUEUED] Pending queue size=${this._pendingWrite.length + 1}`);
      this._pendingWrite.push(data);
    } else if (rc > 0 && rc < data.length) {
      console.log(`[SSH WRITE PARTIAL QUEUED] rc=${rc} < len=${data.length}`);
      this._pendingWrite.push(data.slice(rc));
    }
  }

  resize(cols, rows) {
    this._lastCols = cols;
    this._lastRows = rows;
    if (!this.ch) return;
    try {
      console.log(`[SSH RESIZE] cols=${cols}, rows=${rows}`);
      const rc = this.ch.pty_size(cols, rows);
      console.log(`[SSH pty_size rc] ${rc}`);
    } catch (e) {
      console.error('[SSH RESIZE Exception]', e);
    }
  }

  close() {
    this.closed = true;
    if (this._readTimer) clearInterval(this._readTimer);
    this._readTimer = null;
    try {
      if (this.ch) this.ch.close();
    } catch (e) {
      // ignore
    }
    this.ch = null;
    this._send = null;
  }
}
