import http from 'http';
import http2 from 'http2';
import fs from 'fs';
import path from 'path';
import crypto from 'crypto';
import tls from 'tls';
import Gun from 'gun';
import 'gun/sea.js';

console.log('--- OMD GUN HUB + AUTH STARTUP ---');

// ── Polyfills ──────────────────────────────────────────────────

if (typeof Promise.withResolvers === 'undefined') {
  Promise.withResolvers = function () {
    let resolve, reject;
    const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
    return { promise, resolve, reject };
  };
}

// ── Global Error Guarding ──────────────────────────────────────

process.on('unhandledRejection', (reason) => {
  const msg = reason?.message || reason?.toString() || 'Unknown Rejection';
  console.warn(`[hub] Suppressed unhandled rejection: ${msg}`);
});

process.on('uncaughtException', (err) => {
  console.error('[hub] Uncaught Exception:', err?.message || err);
});

// ── Config ─────────────────────────────────────────────────────

const RELAY_PORT = Number(process.env.PORT) || 8765;
const GOOGLE_CLIENT_ID =
  process.env.GOOGLE_CLIENT_ID ||
  '959849209352-vbi0vfimrqj904598felj8ieeb6to8s2.apps.googleusercontent.com';
const GATEWAY_URL = (process.env.GATEWAY_URL || '').trim();
const CERT_TTL_MS = 30 * 24 * 60 * 60 * 1000; // 30 days

// ── SMTP Relay Config (Google Workspace) ────────────────────────
const SMTP_HOST = process.env.SMTP_HOST || 'smtp-relay.gmail.com';
const SMTP_PORT = Number(process.env.SMTP_PORT) || 465;
const SMTP_FROM = process.env.SMTP_FROM || 'onmydisk@volkoff.info';
const SMTP_FROM_NAME = process.env.SMTP_FROM_NAME || 'Alexey Volkov (OnMyDisk)';
const SMTP_DOMAIN = process.env.SMTP_DOMAIN || 'volkoff.info';

// ── Rate Limiting ──────────────────────────────────────────────
// Simple in-memory rate limiter: per-IP + per-alias

const rateLimits = new Map(); // key → { count, resetAt }
const RATE_WINDOW_MS = 15 * 60 * 1000; // 15 min window
const RATE_MAX_PER_IP = 20;            // max attempts per IP per window
const RATE_MAX_PER_ALIAS = 5;          // max attempts per alias per window
const RATE_BLOCK_MS = 30 * 60 * 1000;  // block for 30 min after exceeding

function rateLimit(key, max) {
  const now = Date.now();
  const entry = rateLimits.get(key);
  if (!entry || now > entry.resetAt) {
    rateLimits.set(key, { count: 1, resetAt: now + RATE_WINDOW_MS });
    return true;
  }
  entry.count++;
  return entry.count <= max;
}

function isRateLimited(ip, alias) {
  return null; // Disabled for development and testing
}

// Ephemeral single-use OAuth handoff sessions for mobile system-browser bridge
const mobileOauthSessions = new Map(); // session → { idToken, email, createdAt }
const MOBILE_OAUTH_TTL_MS = 3 * 60 * 1000; // 3 minutes

// Cleanup old entries every 5 min
setInterval(() => {
  const now = Date.now();
  for (const [key, entry] of rateLimits) {
    if (now > entry.resetAt) rateLimits.delete(key);
  }
  for (const [sid, entry] of mobileOauthSessions) {
    if (now - entry.createdAt > MOBILE_OAUTH_TTL_MS) mobileOauthSessions.delete(sid);
  }
}, 5 * 60 * 1000);

// ── GunDB + SEA ────────────────────────────────────────────────

const getRepoPath = () => {
  if (process.env.REPO_PATH) return process.env.REPO_PATH;
  if (process.env.VOYAGER_PATH) return process.env.VOYAGER_PATH;
  return './data/gundb';
};

const repoPath = getRepoPath();
fs.mkdirSync(repoPath, { recursive: true });
console.log('[hub] GunDB data store:', repoPath);

const server = http.createServer(handler);
// Optional peer replication: comma-separated list of other hub URLs, e.g.
// "wss://hub2.example.com:8765/gun,wss://hub3.example.com:8765/gun".
// Leave empty (default) for a standalone hub with no replication.
const GUN_PEERS = (process.env.GUN_PEERS || '')
  .split(',').map(s => s.trim()).filter(Boolean);

const gun = Gun({ web: server, file: repoPath + '/data.json', peers: GUN_PEERS });

if (GUN_PEERS.length) {
  console.log('[hub] Peer replication enabled:', GUN_PEERS.join(', '));
} else {
  console.log('[hub] Peer replication disabled (standalone mode)');
}
const serverPair = await Gun.SEA.pair();
console.log('[hub] SEA server pubkey:', serverPair.pub);

// ── Account deletion tokens ────────────────────────────────────
// Provider logins (Google/Apple) have no password to re-verify, so the hub
// issues a narrowly-scoped, signed token that authorises deleting exactly one
// DID. The secret is persisted next to the GunDB data so tokens survive
// restarts. Password accounts never need a token - they re-authenticate with
// their password when deleting.
const DELETE_TOKEN_TTL_MS = 365 * 24 * 60 * 60 * 1000;

function loadDeleteTokenSecret() {
  if (process.env.DELETE_TOKEN_SECRET) return process.env.DELETE_TOKEN_SECRET;
  const secretFile = path.join(repoPath, 'delete-token.secret');
  try {
    if (fs.existsSync(secretFile)) {
      const saved = fs.readFileSync(secretFile, 'utf8').trim();
      if (saved) return saved;
    }
    const generated = crypto.randomBytes(32).toString('hex');
    fs.writeFileSync(secretFile, generated, { mode: 0o600 });
    return generated;
  } catch (err) {
    console.warn('[hub] Could not persist delete-token secret:', err?.message || err);
    return crypto.randomBytes(32).toString('hex');
  }
}

const DELETE_TOKEN_SECRET = loadDeleteTokenSecret();

// ── Deleted DID tombstones ─────────────────────────────────────
// Keep tombstones outside the public Gun graph: any Gun peer can write public
// nodes, so a tombstone stored there could be forged or removed by an attacker.
// The hub is the only component that issues certificates, so a server-side
// file is authoritative and also avoids a Gun read on every auth request.
const DELETED_DIDS_FILE = path.join(repoPath, 'deleted-dids.json');

function loadDeletedDids() {
  const out = {};
  try {
    if (!fs.existsSync(DELETED_DIDS_FILE)) return out;
    const parsed = JSON.parse(fs.readFileSync(DELETED_DIDS_FILE, 'utf8'));
    if (Array.isArray(parsed)) {
      for (const mainPub of parsed) {
        if (typeof mainPub === 'string' && mainPub) {
          out[mainPub] = { deletedAt: 0, reason: 'legacy' };
        }
      }
      return out;
    }
    if (parsed && typeof parsed === 'object') {
      for (const [mainPub, value] of Object.entries(parsed)) {
        if (typeof mainPub === 'string' && mainPub) out[mainPub] = value;
      }
    }
  } catch (err) {
    console.warn('[hub] Could not read deleted-dids file:', err?.message || err);
  }
  return out;
}

function persistDeletedDids(next) {
  const tmpFile = DELETED_DIDS_FILE + '.tmp';
  fs.writeFileSync(tmpFile, JSON.stringify(next, null, 2), { mode: 0o600 });
  fs.renameSync(tmpFile, DELETED_DIDS_FILE);
}

let deletedDids = loadDeletedDids();

// ── Durable E2EE MCP API Keys Vault (per DID) ──────────────────
// Stores ONLY client-side AES-GCM ciphertexts ({ ciphertext, iv, updatedAt, deleted }).
// Plaintext API keys never touch the hub. Persisting here alongside GunDB graph
// guarantees keys survive browser re-logins, SEA cert changes, and radata restarts.
const MCP_KEYS_FILE = path.join(repoPath, 'mcp-keys.json');

function loadMcpKeysStore() {
  try {
    if (!fs.existsSync(MCP_KEYS_FILE)) return {};
    const parsed = JSON.parse(fs.readFileSync(MCP_KEYS_FILE, 'utf8'));
    if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) return parsed;
  } catch (err) {
    console.warn('[hub] Could not read mcp-keys file:', err?.message || err);
  }
  return {};
}

function persistMcpKeysStore(next) {
  try {
    const tmpFile = MCP_KEYS_FILE + '.tmp';
    fs.writeFileSync(tmpFile, JSON.stringify(next, null, 2), { mode: 0o600 });
    fs.renameSync(tmpFile, MCP_KEYS_FILE);
  } catch (err) {
    console.warn('[hub] Could not persist mcp-keys file:', err?.message || err);
  }
}

let mcpKeysByDid = loadMcpKeysStore();

function makeDeleteToken(mainPub, provider, subject) {
  const payload = Buffer.from(JSON.stringify({
    m: mainPub,
    p: provider || '',
    s: subject || '',
    e: Date.now() + DELETE_TOKEN_TTL_MS,
  })).toString('base64url');
  const sig = crypto.createHmac('sha256', DELETE_TOKEN_SECRET).update(payload).digest('base64url');
  return payload + '.' + sig;
}

function verifyDeleteToken(mainPub, token) {
  if (!mainPub || !token || typeof token !== 'string') return null;
  const parts = token.split('.');
  if (parts.length !== 2) return null;
  const [payload, sig] = parts;
  if (!payload || !sig) return null;

  const expected = crypto.createHmac('sha256', DELETE_TOKEN_SECRET).update(payload).digest('base64url');
  const sigBuf = Buffer.from(sig);
  const expectBuf = Buffer.from(expected);
  if (sigBuf.length !== expectBuf.length) return null;
  if (!crypto.timingSafeEqual(sigBuf, expectBuf)) return null;

  try {
    const data = JSON.parse(Buffer.from(payload, 'base64url').toString('utf8'));
    if (!data || data.m !== mainPub) return null;
    if (!data.e || Number(data.e) < Date.now()) return null;
    return data;
  } catch (_) {
    return null;
  }
}

// ── Helpers ────────────────────────────────────────────────────

// Derive mathematically stable SEA keypair from legacy username + password (used once on initial migration)
function deriveDeterministicPair(username, password) {
  const normUser = (username || '').toLowerCase().trim();
  const seed = crypto.pbkdf2Sync(password || '', 'omd_sea_legacy_v1:' + normUser, 100000, 64, 'sha256');
  const signSeed = seed.subarray(0, 32);
  const encSeed = seed.subarray(32, 64);

  function scalarToJwk(scalar) {
    const ecdh = crypto.createECDH('prime256v1');
    ecdh.setPrivateKey(scalar);
    const pubBuf = ecdh.getPublicKey();
    const x = pubBuf.subarray(1, 33).toString('base64url');
    const y = pubBuf.subarray(33, 65).toString('base64url');
    const d = scalar.toString('base64url');
    return { x, y, d };
  }

  const sign = scalarToJwk(signSeed);
  const enc = scalarToJwk(encSeed);

  return {
    pub: sign.x + '.' + sign.y,
    priv: sign.d,
    epub: enc.x + '.' + enc.y,
    epriv: enc.d,
  };
}

// Read canonical DIDs for known accounts from configuration file (keyed by sha256 of username/password or username)
function getCanonicalDid(username, password, email) {
  try {
    const candidates = [
      process.env.CANONICAL_DIDS_FILE,
      path.join(path.dirname(new URL(import.meta.url).pathname), 'canonical_dids.json'),
      path.join(repoPath, 'canonical_dids.json'),
      './canonical_dids.json',
      './gun-hub/canonical_dids.json',
    ].filter(Boolean);

    let data = null;
    for (const p of candidates) {
      if (fs.existsSync(p)) {
        data = JSON.parse(fs.readFileSync(p, 'utf8'));
        break;
      }
    }
    if (!data || typeof data !== 'object') return null;

    const normUser = (username || '').toLowerCase().trim();
    const normEmail = (email || '').toLowerCase().trim();

    if (!password) return null;

    const hashesToTry = [
      crypto.createHash('sha256').update(normUser + ':' + password).digest('hex'),
    ];
    if (normEmail) {
      hashesToTry.push(
        crypto.createHash('sha256').update(normEmail + ':' + password).digest('hex')
      );
    }

    for (const h of hashesToTry) {
      if (data[h]) {
        const val = data[h];
        return val.startsWith('did:gun:') ? val.replace('did:gun:', '') : val;
      }
    }
  } catch (err) {
    console.warn('[hub] Error reading canonical_dids.json:', err.message);
  }
  return null;
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      try { resolve(JSON.parse(Buffer.concat(chunks).toString())); }
      catch (e) { reject(e); }
    });
    req.on('error', reject);
  });
}

function json(res, status, data) {
  res.writeHead(status, {
    'Content-Type': 'application/json',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'POST, GET, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
  });
  res.end(JSON.stringify(data));
}

async function verifyGoogleToken(idToken) {
  const resp = await fetch(
    `https://oauth2.googleapis.com/tokeninfo?id_token=${idToken}`
  );
  if (!resp.ok) throw new Error('Invalid Google token');
  const p = await resp.json();
  if (p.aud !== GOOGLE_CLIENT_ID) throw new Error('Token audience mismatch');
  if (Number(p.exp) * 1000 < Date.now()) throw new Error('Token expired');
  return { sub: p.sub, email: p.email, name: p.name, picture: p.picture };
}

let appleKeysCache = { keys: [], expiresAt: 0 };

async function getAppleSigningKey(kid) {
  if (!appleKeysCache.keys.length || Date.now() > appleKeysCache.expiresAt) {
    const res = await fetch('https://appleid.apple.com/auth/keys');
    if (!res.ok) throw new Error('Failed to fetch Apple public keys');
    const data = await res.json();
    appleKeysCache = {
      keys: data.keys || [],
      expiresAt: Date.now() + 24 * 60 * 60 * 1000,
    };
  }
  let keyObj = appleKeysCache.keys.find((k) => k.kid === kid);
  if (!keyObj) {
    // Refresh cache once if kid not matched
    const res = await fetch('https://appleid.apple.com/auth/keys');
    if (res.ok) {
      const data = await res.json();
      appleKeysCache = {
        keys: data.keys || [],
        expiresAt: Date.now() + 24 * 60 * 60 * 1000,
      };
      keyObj = appleKeysCache.keys.find((k) => k.kid === kid);
    }
  }
  return keyObj || null;
}

async function verifyAppleToken(idToken) {
  if (!idToken || typeof idToken !== 'string') throw new Error('Missing Apple idToken');
  const parts = idToken.split('.');
  if (parts.length !== 3) throw new Error('Invalid JWT format');

  const [headerB64, payloadB64, signatureB64] = parts;
  let header, payload;
  try {
    header = JSON.parse(Buffer.from(headerB64, 'base64url').toString('utf8'));
    payload = JSON.parse(Buffer.from(payloadB64, 'base64url').toString('utf8'));
  } catch (e) {
    throw new Error('Malformed Apple JWT payload');
  }

  if (header.alg !== 'RS256') throw new Error('Unsupported algorithm: ' + header.alg);

  const keyObj = await getAppleSigningKey(header.kid);
  if (!keyObj) throw new Error('Apple public key not found for kid: ' + header.kid);

  const publicKey = crypto.createPublicKey({ key: keyObj, format: 'jwk' });
  const signingInput = `${headerB64}.${payloadB64}`;
  const isValid = crypto.verify(
    'RSA-SHA256',
    Buffer.from(signingInput),
    publicKey,
    Buffer.from(signatureB64, 'base64url')
  );
  if (!isValid) throw new Error('Apple token signature verification failed');

  if (payload.iss !== 'https://appleid.apple.com') {
    throw new Error('Invalid issuer: ' + payload.iss);
  }

  // Allowed audiences: iOS App (net.onmydisk.app) and Web Service (net.onmydisk.app.service)
  const allowedAudiences = ['net.onmydisk.app', 'net.onmydisk.app.service'];
  if (!allowedAudiences.includes(payload.aud)) {
    throw new Error('Token audience mismatch: ' + payload.aud);
  }

  if (Number(payload.exp) * 1000 < Date.now()) {
    throw new Error('Token expired');
  }

  return {
    sub: payload.sub,
    email: payload.email || '',
    email_verified: payload.email_verified,
  };
}

function gunOnce(path) {
  return new Promise((resolve) => {
    let done = false;
    gun.get(path).once((data) => { if (!done) { done = true; resolve(data); } });
    setTimeout(() => { if (!done) { done = true; resolve(null); } }, 3000);
  });
}

// Write a Gun node and wait for the relay acknowledgement. Account deletion
// must not report success before the tombstone/credential removal is durable.
function gunPut(path, data, timeoutMs = 5000) {
  return new Promise((resolve) => {
    let done = false;
    const finish = (ok) => {
      if (done) return;
      done = true;
      resolve(ok);
    };
    setTimeout(() => finish(false), timeoutMs);
    try {
      gun.get(path).put(data, (ack) => finish(!(ack && ack.err)));
    } catch (_) {
      finish(false);
    }
  });
}

function gunPutChild(parentPath, childKey, data, timeoutMs = 5000) {
  return new Promise((resolve) => {
    let done = false;
    const finish = (ok) => {
      if (done) return;
      done = true;
      resolve(ok);
    };
    setTimeout(() => finish(false), timeoutMs);
    try {
      gun.get(parentPath).get(childKey).put(data, (ack) => finish(!(ack && ack.err)));
    } catch (_) {
      finish(false);
    }
  });
}

async function isDidDeleted(mainPub) {
  return !!(mainPub && Object.prototype.hasOwnProperty.call(deletedDids, mainPub));
}

async function markDidDeleted(mainPub, reason, meta) {
  if (!mainPub) return false;
  if (Object.prototype.hasOwnProperty.call(deletedDids, mainPub)) {
    if (meta && Object.keys(meta).length) {
      const next = { ...deletedDids, [mainPub]: { ...deletedDids[mainPub], ...meta } };
      try { persistDeletedDids(next); deletedDids = next; } catch (_) {}
    }
    return true;
  }
  const next = {
    ...deletedDids,
    [mainPub]: { deletedAt: Date.now(), reason: reason || 'user_request', ...(meta || {}) },
  };
  try {
    persistDeletedDids(next);
    deletedDids = next;
    return true;
  } catch (err) {
    console.error('[hub] Failed to persist deleted DID:', err?.message || err);
    return false;
  }
}

function updateDeletedDidMeta(mainPub, meta) {
  if (!mainPub || !deletedDids[mainPub] || !meta) return false;
  const next = { ...deletedDids, [mainPub]: { ...deletedDids[mainPub], ...meta } };
  try { persistDeletedDids(next); deletedDids = next; return true; }
  catch (err) { console.warn('[hub] Failed to update deleted DID metadata:', err?.message || err); return false; }
}

// Read a Gun node child value, following soul references if needed
function gunGet(parentPath, childKey) {
  return new Promise((resolve) => {
    let done = false;
    const finish = (val) => { if (!done) { done = true; resolve(val); } };
    setTimeout(() => finish(null), 5000);

    gun.get(parentPath).get(childKey).once((data) => {
      if (!data || typeof data !== 'object') return finish(data ?? null);
      // If it's a soul reference { '#': 'SOUL' }, follow it
      const soul = data['#'];
      if (soul && Object.keys(data).length === 1) {
        gun.get(soul).once((soulData) => finish(soulData ?? null));
      } else {
        finish(data);
      }
    });
  });
}

async function fetchShareRecord(ownerPub, shareId) {
  const realOwnerPub = (ownerPub || "").replace(/^did:gun:/, "").replace(/^~/, "");
  const lastPart = ownerPub && ownerPub.includes(".") ? ownerPub.split(".").pop() : "";

  return new Promise((resolve) => {
    let done = false;
    const finish = (val) => {
      if (!done) {
        done = true;
        resolve(val && typeof val === "object" && (val.name || val.title || val.linkId || val.protected) ? val : null);
      }
    };
    setTimeout(() => finish(null), 4000);

    const onData = (data) => {
      if (!data || typeof data !== "object") return;
      if (data.name || data.title || data.linkId || data.protected) {
        finish(data);
      }
    };

    try {
      gun.get("sharing:" + realOwnerPub).get(shareId).once(onData);
      gun.get("~" + realOwnerPub).get("sharing").get(shareId).once(onData);
      if (lastPart && lastPart !== realOwnerPub) {
        gun.get("sharing:" + lastPart).get(shareId).once(onData);
        gun.get("~" + lastPart).get("sharing").get(shareId).once(onData);
      }
      gun.get("sharing").get(shareId).once(onData);
    } catch (e) {}
  });
}

// ── Share Meta Resolver ────────────────────────────────────────

async function handleShareMeta(req, res) {
  const urlObj = new URL(req.url, 'http://localhost');
  const address = urlObj.searchParams.get('address');
  if (!address) return json(res, 400, { error: 'Missing address parameter' });

  // Parse ownerPub.shareId — shareId is always the 12-char suffix after last dot
  const dotIdx = address.lastIndexOf('.');
  if (dotIdx === -1) return json(res, 400, { error: 'Invalid address format' });

  const ownerPub = address.substring(0, dotIdx);
  const shareId  = address.substring(dotIdx + 1);

  if (!ownerPub || !shareId) return json(res, 400, { error: 'Invalid ownerPub or shareId' });

  const data = await fetchShareRecord(ownerPub, shareId);

  if (!data) {
    return json(res, 404, { error: 'Share not found' });
  }

  // Don't expose encrypted shares — return minimal public info only
  if (data.protected && data.encryptedData) {
    return json(res, 200, {
      title: data.name || 'Protected Share',
      description: 'This share is password-protected.',
      isDir: !!data.isDir,
      hasCover: false,
      ownerName: data.ownerName || 'OMD User',
      protected: true,
    });
  }

  const title       = data.title || data.name || 'Shared on OnMyDisk';
  const description = data.description ||
    (data.isDir ? `Shared folder "${title}"` : `Shared file "${title}"`) +
    ' via On My Disk P2P storage';
  const hasCover    = !!(data.hasCover || data.customAttributes?.cover || data.customAttributes?.preview);
  const ownerName   = data.ownerName || 'OMD User';
  const mimeType    = data.mimeType || '';
  const isDir       = !!(data.isDir || data.type === 'folder');

  return json(res, 200, {
    title,
    description,
    hasCover,
    ownerName,
    mimeType,
    isDir,
    protected: false,
  });
}

async function handleSaveShareRecord(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { ownerPub, shareId, recordData } = body;
  if (!ownerPub || !shareId || !recordData) {
    return json(res, 400, { error: 'Missing ownerPub, shareId or recordData' });
  }

  try {
    gun.get('sharing:' + ownerPub).get(shareId).put(recordData);
    gun.get('sharing').get(shareId).put(recordData);
    console.log(`[hub] Saved share record: ${ownerPub.slice(0, 10)}.../${shareId} (${recordData.title || recordData.name})`);
    return json(res, 200, { ok: true });
  } catch (err) {
    console.warn('[hub] Error saving share record:', err);
    return json(res, 500, { error: err.message });
  }
}

async function handlePublishDevice(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { mainPub, device } = body;
  if (!mainPub || !device || !device.linkId) {
    return json(res, 400, { error: 'Missing mainPub or device.linkId' });
  }

  const linkId = device.linkId;
  const data = {
    linkId: device.linkId,
    name: device.name || device.linkId,
    type: device.type || 'desktop',
    devicePub: device.devicePub || device.linkId,
    originDevicePub: device.originDevicePub || '',
    online: device.online !== false,
    last_seen: Date.now(),
    capabilities: Array.isArray(device.capabilities) ? device.capabilities.join(',') : (device.capabilities || 'files'),
  };

  try {
    gun.get('~' + mainPub).get('devices').get(linkId).put(data);
    gun.get('devices:' + mainPub).get(linkId).put(data);
    console.log(`[hub] Published device for ~${mainPub.slice(0, 10)}...: ${linkId} (${data.name})`);
    return json(res, 200, { ok: true });
  } catch (err) {
    console.warn('[hub] Error publishing device:', err);
    return json(res, 500, { error: err.message });
  }
}

async function handleRemoveDevice(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { mainPub, linkId } = body;
  if (!mainPub || !linkId) {
    return json(res, 400, { error: 'Missing mainPub or linkId' });
  }

  try {
    gun.get('~' + mainPub).get('devices').get(linkId).put(null);
    gun.get('devices:' + mainPub).get(linkId).put(null);
    console.log(`[hub] Removed device for ~${mainPub.slice(0, 10)}...: ${linkId}`);
    return json(res, 200, { ok: true });
  } catch (err) {
    console.warn('[hub] Error removing device:', err);
    return json(res, 500, { error: err.message });
  }
}

async function handleGetDevices(req, res) {
  const urlObj = new URL(req.url, 'http://localhost');
  const mainPub = urlObj.searchParams.get('mainPub');
  if (!mainPub) {
    return json(res, 400, { error: 'Missing mainPub' });
  }

  const devsMap = new Map();
  let answered = false;

  const finish = () => {
    if (answered) return;
    answered = true;
    json(res, 200, { ok: true, devices: Array.from(devsMap.values()) });
  };

  const timeout = setTimeout(finish, 1000);

  try {
    gun.get('devices:' + mainPub).map().once((v, k) => {
      if (v && typeof v === 'object' && v.linkId) {
        devsMap.set(v.linkId, {
          linkId: v.linkId,
          name: v.name || v.linkId,
          type: v.type || 'desktop',
          devicePub: v.devicePub || v.linkId,
          originDevicePub: v.originDevicePub || '',
          online: v.online !== false,
          lastSeen: v.last_seen || v.lastSeen || Date.now(),
          capabilities: v.capabilities || 'files',
        });
      }
    });
  } catch (err) {
    if (!answered) {
      answered = true;
      clearTimeout(timeout);
      json(res, 500, { error: err.message, devices: [] });
    }
  }
}

// ── E2EE MCP API Keys Vault Endpoints ──────────────────────────

function sanitizeEncryptedMcpEntry(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const updatedAt = Number(raw.updatedAt) || Date.now();
  if (raw.deleted === true) {
    return { deleted: true, ciphertext: '', iv: '', updatedAt };
  }
  const ciphertext = typeof raw.ciphertext === 'string' ? raw.ciphertext.trim() : '';
  const iv = typeof raw.iv === 'string' ? raw.iv.trim() : '';
  const enc = typeof raw.enc === 'string' ? raw.enc.trim() : '';
  if (ciphertext && iv) {
    return { ciphertext, iv, updatedAt, deleted: false };
  }
  if (enc) {
    return { enc, updatedAt, deleted: false };
  }
  return null;
}

async function handleSaveMcpKeys(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const mainPub = typeof body?.mainPub === 'string' ? body.mainPub.trim() : '';
  if (!mainPub || mainPub.length < 10) {
    return json(res, 400, { error: 'Missing or invalid mainPub' });
  }
  if (await isDidDeleted(mainPub)) {
    return json(res, 410, { error: 'Account has been deleted', code: 'account_deleted' });
  }

  const incoming = {};
  if (body.entries && typeof body.entries === 'object' && !Array.isArray(body.entries)) {
    for (const [k, v] of Object.entries(body.entries)) {
      const cleanId = String(k).trim().toLowerCase().replace(/[^a-z0-9_-]/g, '_');
      const sanitized = sanitizeEncryptedMcpEntry(v);
      if (cleanId && sanitized) incoming[cleanId] = sanitized;
    }
  } else if (body.appId) {
    const cleanId = String(body.appId).trim().toLowerCase().replace(/[^a-z0-9_-]/g, '_');
    const sanitized = sanitizeEncryptedMcpEntry(
      body.deleted === true ? { deleted: true, updatedAt: body.updatedAt } : body.entry
    );
    if (cleanId && sanitized) incoming[cleanId] = sanitized;
  }

  if (Object.keys(incoming).length === 0) {
    return json(res, 400, { error: 'No valid E2EE entries provided' });
  }

  const didVault = { ...(mcpKeysByDid[mainPub] || {}) };
  let changed = false;

  for (const [appId, nextEntry] of Object.entries(incoming)) {
    const prev = didVault[appId];
    const prevTs = Number(prev?.updatedAt || 0);
    if (!prev || nextEntry.updatedAt >= prevTs) {
      didVault[appId] = nextEntry;
      changed = true;
      try {
        gun.get('mcp_keys:' + mainPub).get(appId).put(nextEntry);
      } catch (e) {
        console.warn('[hub] Gun put mcp_keys warning:', e?.message || e);
      }
    }
  }

  if (changed) {
    mcpKeysByDid = { ...mcpKeysByDid, [mainPub]: didVault };
    persistMcpKeysStore(mcpKeysByDid);
  }

  return json(res, 200, { ok: true, count: Object.keys(incoming).length });
}

async function handleGetMcpKeys(req, res) {
  const urlObj = new URL(req.url, 'http://localhost');
  const mainPub = (urlObj.searchParams.get('mainPub') || '').trim();
  if (!mainPub) {
    return json(res, 400, { error: 'Missing mainPub' });
  }

  const merged = {};
  const fromDisk = mcpKeysByDid[mainPub] || {};
  for (const [appId, entry] of Object.entries(fromDisk)) {
    const sanitized = sanitizeEncryptedMcpEntry(entry);
    if (sanitized) merged[appId] = sanitized;
  }

  let answered = false;
  const finish = () => {
    if (answered) return;
    answered = true;
    const activeKeys = {};
    for (const [appId, entry] of Object.entries(merged)) {
      if (entry && !entry.deleted && ((entry.ciphertext && entry.iv) || entry.enc)) {
        activeKeys[appId] = entry;
      }
    }
    json(res, 200, { ok: true, keys: activeKeys });
  };

  const timeout = setTimeout(finish, 400);

  try {
    gun.get('mcp_keys:' + mainPub).map().once((v, k) => {
      if (!k || k === '_') return;
      const sanitized = sanitizeEncryptedMcpEntry(v);
      if (!sanitized) return;
      const prev = merged[k];
      if (!prev || sanitized.updatedAt > Number(prev.updatedAt || 0)) {
        merged[k] = sanitized;
      }
    });
  } catch (_) {
    clearTimeout(timeout);
    finish();
  }
}

// ── Knowledge: Chunks + Card retrieval (for node-side RAG) ─────

async function handleGetChunks(req, res) {
  const urlObj = new URL(req.url, 'http://localhost');
  const tokenHash = urlObj.searchParams.get('tokenHash');
  const memoryId = urlObj.searchParams.get('memoryId');
  if (!tokenHash || !memoryId) {
    return json(res, 400, { error: 'Missing tokenHash or memoryId' });
  }
  const data = await gunGet('chunks_' + tokenHash, memoryId);
  if (!data || !data.chunks) {
    return json(res, 404, { error: 'Chunks not found' });
  }
  let chunks = data.chunks;
  if (typeof chunks === 'string') {
    try { chunks = JSON.parse(chunks); } catch { /* keep raw */ }
  }
  return json(res, 200, { ok: true, chunks, updated: data.updated || null });
}

async function handleGetKnowledgeCard(req, res) {
  const urlObj = new URL(req.url, 'http://localhost');
  const tokenHash = urlObj.searchParams.get('tokenHash');
  const memoryId = urlObj.searchParams.get('memoryId');
  if (!tokenHash || !memoryId) {
    return json(res, 400, { error: 'Missing tokenHash or memoryId' });
  }
  const data = await gunGet('knowledge_' + tokenHash, memoryId);
  if (!data) {
    return json(res, 404, { error: 'Knowledge card not found' });
  }
  let annotation_embedding = null;
  if (data.annotation_embedding) {
    if (typeof data.annotation_embedding === 'string') {
      try { annotation_embedding = JSON.parse(data.annotation_embedding); } catch {}
    } else if (Array.isArray(data.annotation_embedding)) {
      annotation_embedding = data.annotation_embedding;
    }
  }
  let tags = data.tags;
  if (typeof tags === 'string') {
    try { tags = JSON.parse(tags); } catch { tags = tags.split(',').map(t => t.trim()); }
  }
  return json(res, 200, {
    ok: true,
    memoryId,
    title: data.title || '',
    tags: Array.isArray(tags) ? tags : [],
    annotation_embedding,
    chunk_count: data.chunk_count || 0,
    document_id: data.document_id || '',
  });
}

async function handleGetAllKnowledge(req, res) {
  const urlObj = new URL(req.url, 'http://localhost');
  const tokenHash = urlObj.searchParams.get('tokenHash');
  if (!tokenHash) {
    return json(res, 400, { error: 'Missing tokenHash' });
  }
  let answered = false;
  const list = [];
  const timeout = setTimeout(() => {
    if (!answered) {
      answered = true;
      json(res, 200, { ok: true, cards: list });
    }
  }, 3000);
  try {
    gun.get('knowledge_' + tokenHash).map().once((data, key) => {
      if (!data || typeof data !== 'object' || key === '_') return;
      let annotation_embedding = null;
      if (data.annotation_embedding) {
        if (typeof data.annotation_embedding === 'string') {
          try { annotation_embedding = JSON.parse(data.annotation_embedding); } catch {}
        } else if (Array.isArray(data.annotation_embedding)) {
          annotation_embedding = data.annotation_embedding;
        }
      }
      list.push({
        memoryId: key,
        title: data.title || '',
        tags: (() => { try { return JSON.parse(data.tags); } catch { return []; } })(),
        annotation_embedding,
        document_id: data.document_id || '',
      });
    });
  } catch {}
  setTimeout(() => {
    if (!answered) {
      answered = true;
      clearTimeout(timeout);
      json(res, 200, { ok: true, cards: list });
    }
  }, 3500);
}

async function handleShareCover(req, res) {
  const urlObj = new URL(req.url, 'http://localhost');
  const address = urlObj.searchParams.get('address');
  if (!address) return json(res, 400, { error: 'Missing address' });

  const dotIdx = address.lastIndexOf('.');
  if (dotIdx === -1) return json(res, 400, { error: 'Invalid address' });
  const ownerPub = address.substring(0, dotIdx);
  const shareId  = address.substring(dotIdx + 1);

  const data = await fetchShareRecord(ownerPub, shareId);
  if (!data || !data.coverData) {
    return json(res, 404, { error: 'Cover not found' });
  }

  try {
    let base64 = data.coverData;
    let mime = 'image/jpeg';
    if (base64.includes(';base64,')) {
      const parts = base64.split(';base64,');
      mime = parts[0].replace('data:', '') || 'image/jpeg';
      base64 = parts[1];
    }
    const buf = Buffer.from(base64, 'base64');
    res.writeHead(200, {
      'Content-Type': mime,
      'Content-Length': buf.length,
      'Cache-Control': 'public, max-age=86400',
      'Access-Control-Allow-Origin': '*',
    });
    return res.end(buf);
  } catch (err) {
    return json(res, 500, { error: 'Failed to decode cover' });
  }
}

async function issueCertificate(devicePub, paths, ttlMs = CERT_TTL_MS) {
  return await Gun.SEA.certify(devicePub, paths, serverPair, null, {
    expiry: Date.now() + ttlMs,
  });
}

// ── Auth: Google Link ──────────────────────────────────────────

async function handleGoogleLink(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { idToken, mainPub: clientMainPub, devicePub } = body;
  if (!idToken || !devicePub)
    return json(res, 400, { error: 'Missing idToken or devicePub' });

  let googleUser;
  try { googleUser = await verifyGoogleToken(idToken); }
  catch (e) { return json(res, 401, { error: 'Google auth failed: ' + e.message }); }

  const mappingKey = '~@google:' + googleUser.sub;
  let existing = await gunOnce(mappingKey);

  // A deleted DID leaves a tombstone. The provider token proves ownership of
  // this Google subject, so discard the stale mapping and create a fresh
  // identity instead of locking the user out forever.
  if (existing && existing.mainPub && await isDidDeleted(existing.mainPub)) {
    console.log('[auth] discarding tombstoned Google mapping:', existing.mainPub);
    await gunPut(mappingKey, null);
    existing = null;
  }

  let mainPub;
  if (existing && existing.mainPub) {
    mainPub = existing.mainPub;
    if (clientMainPub && clientMainPub !== mainPub)
      return json(res, 409, { error: 'Account linked to a different identity' });
  } else {
    const masterPair = await Gun.SEA.pair();
    mainPub = masterPair.pub;
    gun.get(mappingKey).put({
      mainPub,
      email: googleUser.email,
      name: googleUser.name,
      createdAt: Date.now(),
    });
    console.log('[auth] created DID for:', googleUser.email, mainPub);
  }

  if (await isDidDeleted(mainPub))
    return json(res, 410, { error: 'Account has been deleted', code: 'account_deleted' });

  const cert = await issueCertificate(devicePub, [
    { '*': 'devices', '+': '*' },
    { '*': 'chats', '+': '*' },
    { '*': 'knowledge', '+': '*' },
    { '*': 'prompts', '+': '*' },
    { '*': 'settings', '+': '*' },
    { '*': 'sharedToMe', '+': '*' },
    { '*': 'sharing', '+': '*' },
    { '*': 'did', '+': '*' },
    { '*': 'profile', '+': '*' },
  ]);

  // Proxy to legacy gateway (if explicitly configured via GATEWAY_URL) to obtain consistent omd_key
  let gatewayResp = null;
  const gatewaysToTry = GATEWAY_URL ? [GATEWAY_URL] : [];

  for (const gw of gatewaysToTry) {
    try {
      const resp = await fetch(gw + '/rest/signinwithgoogle', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        signal: AbortSignal.timeout(2500),
        body: JSON.stringify({
          action: 'signinwithgoogle',
          id_token: idToken,
          remember: true,
          deviceId: 'omd-hub-' + Date.now(),
        }),
      });
      const data = await resp.json();
      if (data && (data.result?.token || data.token || data.valid || data.result?.valid)) {
        gatewayResp = data.result || data;
        break;
      }
    } catch (e) {
      console.warn(`[hub] Gateway ${gw} signinwithgoogle failed:`, e.message);
    }
  }

  const omdProfile = gatewayResp?.profile || {};
  let omdKey = omdProfile.omd_key || gatewayResp?.token || gatewayResp?.device_key || '';
  if (!omdKey) {
    const existingProfile = await gunOnce('~' + mainPub + '/profile');
    if (existingProfile && existingProfile.omd_key) {
      omdKey = existingProfile.omd_key;
    } else {
      const userSeed = googleUser.email.endsWith('@gmail.com') ? googleUser.email.split('@')[0] : googleUser.email;
      omdKey = crypto.createHash('md5').update(userSeed + ':1678790674').digest('hex');
    }
  }

  const googleName = googleUser.name || googleUser.email.split('@')[0];
  const googleHandle = googleUser.email.split('@')[0];
  const googleEmail = googleUser.email;
  const googlePic = googleUser.picture || '';

  const finalProfile = {
    user: googleHandle,
    name: googleName,
    email: googleEmail,
    image: googlePic,
    omd_key: omdKey,
  };

  // Seed / update Gun profile
  gun.get('~' + mainPub).get('profile').put({
    user: googleHandle,
    name: googleName,
    email: googleEmail,
    image: googlePic,
    omd_key: omdKey,
    updatedAt: Date.now(),
  });

  json(res, 200, {
    certificate: cert,
    mainPub,
    did: 'did:gun:' + mainPub,
    googleUser: { email: googleEmail, name: googleName, picture: googlePic },
    profile: finalProfile,
    token: omdKey,
    omd_key: omdKey,
    deleteToken: makeDeleteToken(mainPub, 'google', googleUser.sub),
  });

  console.log('[auth] google cert →', devicePub.slice(0, 12) + '...', '→ did:gun:' + mainPub, 'omd_key:', omdKey ? 'set' : 'none');
}

// ── Auth: Mobile System-Browser OAuth Handoff ──────────────────

async function handleMobileOauthComplete(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const session = typeof body?.session === 'string' ? body.session.trim() : '';
  const idToken = typeof body?.idToken === 'string' ? body.idToken.trim() : '';
  const provider = typeof body?.provider === 'string' ? body.provider.trim().toLowerCase() : 'google';
  if (!/^[a-zA-Z0-9_-]{16,128}$/.test(session) || !idToken) {
    return json(res, 400, { error: 'Missing or invalid session or idToken' });
  }

  let verifiedEmail = '';
  let userObj = null;
  if (provider === 'apple') {
    try {
      const appleUser = await verifyAppleToken(idToken);
      verifiedEmail = appleUser.email || body?.user?.email || '';
      if (body?.user && typeof body.user === 'object') {
        userObj = body.user;
      }
    } catch (e) {
      return json(res, 401, { error: 'Apple auth failed: ' + e.message });
    }
  } else {
    try {
      const googleUser = await verifyGoogleToken(idToken);
      verifiedEmail = googleUser.email || '';
    } catch (e) {
      return json(res, 401, { error: 'Google auth failed: ' + e.message });
    }
  }

  // Cap map size to prevent unbounded growth
  if (mobileOauthSessions.size >= 500) {
    const now = Date.now();
    for (const [sid, entry] of mobileOauthSessions) {
      if (now - entry.createdAt > MOBILE_OAUTH_TTL_MS) mobileOauthSessions.delete(sid);
    }
    if (mobileOauthSessions.size >= 500) {
      const oldestKey = mobileOauthSessions.keys().next().value;
      if (oldestKey) mobileOauthSessions.delete(oldestKey);
    }
  }

  mobileOauthSessions.set(session, {
    idToken,
    provider,
    email: verifiedEmail,
    user: userObj,
    createdAt: Date.now(),
  });

  console.log(`[auth] mobile ${provider} oauth handoff stored for:`, verifiedEmail || 'user');
  return json(res, 200, { ok: true, email: verifiedEmail });
}

function handleMobileOauthPoll(req, res) {
  const parsed = new URL(req.url, 'http://localhost');
  const session = (parsed.searchParams.get('session') || '').trim();
  res.writeHead(200, {
    'Content-Type': 'application/json',
    'Cache-Control': 'no-store, no-cache, must-revalidate',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'POST, GET, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
  });
  if (!/^[a-zA-Z0-9_-]{16,128}$/.test(session)) {
    return res.end(JSON.stringify({ pending: false, error: 'invalid_session' }));
  }
  const entry = mobileOauthSessions.get(session);
  if (!entry) {
    return res.end(JSON.stringify({ pending: true }));
  }
  if (Date.now() - entry.createdAt > MOBILE_OAUTH_TTL_MS) {
    mobileOauthSessions.delete(session);
    return res.end(JSON.stringify({ pending: true }));
  }
  // Single-use: consume immediately
  mobileOauthSessions.delete(session);
  return res.end(JSON.stringify({
    pending: false,
    idToken: entry.idToken,
    provider: entry.provider || 'google',
    email: entry.email || '',
    user: entry.user || null,
  }));
}

// ── Auth: Apple Link ───────────────────────────────────────────

async function handleAppleLink(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { idToken, mainPub: clientMainPub, devicePub, user } = body;
  if (!idToken || !devicePub)
    return json(res, 400, { error: 'Missing idToken or devicePub' });

  let appleUser;
  try { appleUser = await verifyAppleToken(idToken); }
  catch (e) { return json(res, 401, { error: 'Apple auth failed: ' + e.message }); }

  const mappingKey = '~@apple:' + appleUser.sub;
  let existing = await gunOnce(mappingKey);

  if (existing && existing.mainPub && await isDidDeleted(existing.mainPub)) {
    console.log('[auth] discarding tombstoned Apple mapping:', existing.mainPub);
    await gunPut(mappingKey, null);
    existing = null;
  }

  let mainPub;
  if (existing && existing.mainPub) {
    mainPub = existing.mainPub;
    if (clientMainPub && clientMainPub !== mainPub)
      return json(res, 409, { error: 'Account linked to a different identity' });
  } else {
    const masterPair = await Gun.SEA.pair();
    mainPub = masterPair.pub;
    const providedName = user?.name ? `${user.name.givenName || ''} ${user.name.familyName || ''}`.trim() : '';
    gun.get(mappingKey).put({
      mainPub,
      email: appleUser.email || '',
      name: providedName || (appleUser.email ? appleUser.email.split('@')[0] : 'Apple User'),
      createdAt: Date.now(),
    });
    console.log('[auth] created DID for Apple user:', appleUser.sub, mainPub);
  }

  if (await isDidDeleted(mainPub))
    return json(res, 410, { error: 'Account has been deleted', code: 'account_deleted' });

  const cert = await issueCertificate(devicePub, [
    { '*': 'devices', '+': '*' },
    { '*': 'chats', '+': '*' },
    { '*': 'knowledge', '+': '*' },
    { '*': 'prompts', '+': '*' },
    { '*': 'settings', '+': '*' },
    { '*': 'sharedToMe', '+': '*' },
    { '*': 'sharing', '+': '*' },
    { '*': 'did', '+': '*' },
    { '*': 'profile', '+': '*' },
  ]);

  let omdKey = '';
  const existingProfile = await gunOnce('~' + mainPub + '/profile');
  if (existingProfile && existingProfile.omd_key) {
    omdKey = existingProfile.omd_key;
  } else {
    const userSeed = appleUser.email ? appleUser.email.split('@')[0] : appleUser.sub;
    omdKey = crypto.createHash('md5').update(userSeed + ':1678790674').digest('hex');
  }

  const providedName = user?.name ? `${user.name.givenName || ''} ${user.name.familyName || ''}`.trim() : '';
  const appleName = providedName || existingProfile?.name || (appleUser.email ? appleUser.email.split('@')[0] : 'Apple User');
  const appleHandle = appleUser.email ? appleUser.email.split('@')[0] : ('apple_' + appleUser.sub.slice(-8));
  const appleEmail = appleUser.email || existingProfile?.email || '';

  const finalProfile = {
    user: appleHandle,
    name: appleName,
    email: appleEmail,
    image: existingProfile?.image || '',
    omd_key: omdKey,
  };

  // Seed / update Gun profile
  gun.get('~' + mainPub).get('profile').put({
    user: appleHandle,
    name: appleName,
    email: appleEmail,
    image: finalProfile.image,
    omd_key: omdKey,
    updatedAt: Date.now(),
  });

  json(res, 200, {
    certificate: cert,
    mainPub,
    did: 'did:gun:' + mainPub,
    appleUser: { email: appleEmail, name: appleName, sub: appleUser.sub },
    profile: finalProfile,
    token: omdKey,
    omd_key: omdKey,
    deleteToken: makeDeleteToken(mainPub, 'apple', appleUser.sub),
  });

  console.log('[auth] apple cert →', devicePub.slice(0, 12) + '...', '→ did:gun:' + mainPub, 'omd_key:', omdKey ? 'set' : 'none');
}

// ── Auth: Legacy credential fallback (gateway retired) ──────────

// When the legacy gateway is unreachable (retired), legacy accounts keep
// working: their DID is a deterministic function of username+password and the
// GunDB migration mapping pins it. Password knowledge is proven either by
// re-deriving the pair (password unchanged since the first migration) or by
// the stored credential verifier (captured at the last successful legacy
// login - covers passwords changed after the first migration).
async function legacyCredentialFallback(res, usernamesToTry, password, devicePub) {
  const certScopes = [
    { '*': 'devices', '+': '*' },
    { '*': 'chats', '+': '*' },
    { '*': 'knowledge', '+': '*' },
    { '*': 'prompts', '+': '*' },
    { '*': 'settings', '+': '*' },
    { '*': 'sharedToMe', '+': '*' },
    { '*': 'sharing', '+': '*' },
    { '*': 'did', '+': '*' },
    { '*': 'profile', '+': '*' },
  ];

  for (const u of usernamesToTry) {
    const normUser = (u || '').toLowerCase().trim();
    if (!normUser) continue;
    const mappingKey = 'omd_legacy_user/' + normUser;
    const mapping = await gunOnce(mappingKey);
    if (!mapping || !mapping.mainPub) continue;

    const derived = deriveDeterministicPair(normUser, password);
    const credCheck = crypto.pbkdf2Sync(password || '', 'omd_cred_v1:' + normUser, 100000, 32, 'sha256').toString('hex');
    const credOk = (derived.pub === mapping.mainPub) || (mapping.credCheck === credCheck);
    if (!credOk) continue;

    const mainPub = mapping.mainPub;
    if (await isDidDeleted(mainPub))
      return json(res, 410, { error: 'Account has been deleted', code: 'account_deleted' });

    const cert = await issueCertificate(devicePub, certScopes);

    const existingGunProfile = await gunOnce('~' + mainPub + '/profile');
    const finalProfile = {
      user: (existingGunProfile && existingGunProfile.user) ? existingGunProfile.user : (mapping.user || normUser),
      name: (existingGunProfile && existingGunProfile.name) ? existingGunProfile.name : (mapping.name || normUser),
      email: (existingGunProfile && existingGunProfile.email) ? existingGunProfile.email : (mapping.email || ''),
      image: (existingGunProfile && existingGunProfile.image) ? existingGunProfile.image : (mapping.image || ''),
      omd_key: (existingGunProfile && existingGunProfile.omd_key) ? existingGunProfile.omd_key : (mapping.omd_key || ''),
    };

    console.log('[auth] legacy gateway unreachable - DID restored from GunDB mapping:', normUser, '→', mainPub);

    return json(res, 200, {
      valid: true,
      certificate: cert,
      mainPub,
      did: 'did:gun:' + mainPub,
      profile: finalProfile,
      token: finalProfile.omd_key,
    });
  }
  return null;
}

// ── Auth: Gateway Signin (proxy + DID migration) ───────────────

async function handleGatewaySignin(req, res) {
  const ip = req.headers['x-forwarded-for'] || req.socket.remoteAddress || 'unknown';

  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { username, password, devicePub } = body;
  if (!username || !password || !devicePub)
    return json(res, 400, { error: 'Missing username, password, or devicePub' });

  // Rate limit check
  const rateError = isRateLimited(ip, username);
  if (rateError) return json(res, 429, { error: rateError });

  // Proxy to legacy gateway only if GATEWAY_URL is explicitly configured;
  // otherwise go straight to legacyCredentialFallback (legacy gateway retired).
  let gatewayResp = null;
  let gatewayReachable = false; //distinguish "wrong password" from "legacy gateway retired"
  const gatewaysToTry = GATEWAY_URL ? [GATEWAY_URL] : [];

  const usernamesToTry = [username];
  if (username.includes('@')) {
    usernamesToTry.push(username.split('@')[0]);
  }

  for (const u of usernamesToTry) {
    for (const gw of gatewaysToTry) {
      try {
        const resp = await fetch(gw + '/rest/signin', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          signal: AbortSignal.timeout(2500),
          body: JSON.stringify({
            action: 'signin',
            username: u,
            password,
            remember: true,
            deviceId: 'omd-hub-' + Date.now(),
          }),
        });
        if (resp.status >= 500) continue; //gateway trouble - treat as unreachable
        gatewayReachable = true;
        const data = await resp.json();
        if (data && data.valid) {
          gatewayResp = data;
          break;
        }
      } catch (e) {
        console.warn(`[hub] Gateway ${gw} signin failed:`, e.message);
      }
    }
    if (gatewayResp && gatewayResp.valid) break;
  }

  // Gateway returned invalid credentials
  if (!gatewayResp || !gatewayResp.valid) {
    if (!gatewayReachable) {
      //Legacy gateway is retired/unreachable: authenticate against the GunDB
      //migration mapping by re-deriving the deterministic pair (see below).
      const fallbackResp = await legacyCredentialFallback(res, usernamesToTry, password, devicePub);
      if (fallbackResp) return fallbackResp;
    }
    return json(res, 401, { error: 'Invalid credentials' });
  }

  // Extract profile from gateway response
  const omdProfile = gatewayResp.profile || {};
  const userHandle = omdProfile.user || username;
  const displayName = omdProfile.displayname || omdProfile.name || username;
  const userEmail = omdProfile.email || (username.includes('@') ? username : '');
  let userAvatar = omdProfile.profileimage || omdProfile.avatar || '';
  if (userAvatar && !userAvatar.startsWith('http') && !userAvatar.startsWith('data:')) {
    userAvatar = GATEWAY_URL + (userAvatar.startsWith('/') ? '' : '/') + userAvatar;
  }

  if (body.checkOnly) {
    return json(res, 200, {
      valid: true,
      profile: {
        user: userHandle,
        name: displayName,
        displayname: displayName,
        email: userEmail,
        image: userAvatar,
      }
    });
  }

  // Check/create DID mapping by PERMANENT username (not dynamic session token!)
  const userAccountKey = userHandle.toLowerCase();

  // 1. Check canonical DID from config file (read on demand, zero hardcode in code)
  const canonicalPub = getCanonicalDid(userAccountKey, password, userEmail);

  // 2. Check existing mapping in GunDB
  const mappingKey = 'omd_legacy_user/' + userAccountKey;
  let existing = await gunOnce(mappingKey);
  if (!existing || !existing.mainPub) {
    if (omdProfile.omd_key) {
      const oldKeyMapping = await gunOnce('omd_legacy/' + omdProfile.omd_key);
      if (oldKeyMapping && oldKeyMapping.mainPub) existing = oldKeyMapping;
    }
  }

  let mainPub = canonicalPub || (existing && existing.mainPub ? existing.mainPub : body.mainPub);
  if (canonicalPub) {
    console.log('[auth] canonical config DID mapped for user:', userAccountKey, '→', mainPub);
  } else if (existing && existing.mainPub) {
    console.log('[auth] existing DID found in GunDB for legacy user:', userAccountKey, '→', mainPub);
  } else if (mainPub) {
    console.log('[auth] client provided mainPub for legacy user:', userAccountKey, '→', mainPub);
  } else {
    // Первый логин в новую систему, пока новая учётка ещё не создана:
    // Математически стабильная формула вывода из старого юзернейм-пароля (выполняется 1 раз)
    const masterPair = deriveDeterministicPair(userAccountKey, password);
    mainPub = masterPair.pub;
    console.log('[auth] derived mathematically stable DID for first legacy login:', userAccountKey, '→', mainPub);
  }

  if (await isDidDeleted(mainPub))
    return json(res, 410, { error: 'Account has been deleted', code: 'account_deleted' });

  // Persist canonical mapping into GunDB so subsequent logins find it immediately in DB
  const canonicalLookup = await gunOnce(mappingKey);
  if (mainPub) {
    //Keep the credential verifier in sync with the last known legacy password
    //(slow hash, per-user salt) so logins keep working after the legacy
    //gateway is retired - including passwords changed after the first migration.
    const credCheck = crypto.pbkdf2Sync(password, 'omd_cred_v1:' + userAccountKey, 100000, 32, 'sha256').toString('hex');
    if (!canonicalLookup || !canonicalLookup.mainPub || canonicalLookup.mainPub !== mainPub) {
      gun.get(mappingKey).put({
        mainPub,
        did: 'did:gun:' + mainPub,
        email: userEmail,
        name: displayName,
        user: userHandle,
        image: userAvatar,
        credCheck,
        migratedAt: Date.now(),
      });
      console.log('[auth] persisted legacy username mapping to GunDB:', mappingKey, '→', mainPub);
    } else {
      gun.get(mappingKey).put({ credCheck });
    }
  }

  const omdKey = omdProfile.omd_key || gatewayResp.token || '';

  // Check existing Gun profile before overwriting with gateway defaults
  const existingGunProfile = await gunOnce('~' + mainPub + '/profile');
  const finalProfile = {
    user: (existingGunProfile && existingGunProfile.user) ? existingGunProfile.user : userHandle,
    name: (existingGunProfile && existingGunProfile.name && existingGunProfile.name !== userHandle) ? existingGunProfile.name : displayName,
    email: (existingGunProfile && existingGunProfile.email) ? existingGunProfile.email : userEmail,
    image: (existingGunProfile && existingGunProfile.image) ? existingGunProfile.image : userAvatar,
    omd_key: (existingGunProfile && existingGunProfile.omd_key) ? existingGunProfile.omd_key : omdKey,
  };

  // Populate Gun profile
  gun.get('~' + mainPub).get('profile').put({
    user: finalProfile.user,
    name: finalProfile.name,
    email: finalProfile.email,
    image: finalProfile.image,
    omd_key: finalProfile.omd_key,
    updatedAt: Date.now(),
  });

  const cert = await issueCertificate(devicePub, [
    { '*': 'devices', '+': '*' },
    { '*': 'chats', '+': '*' },
    { '*': 'knowledge', '+': '*' },
    { '*': 'prompts', '+': '*' },
    { '*': 'settings', '+': '*' },
    { '*': 'sharedToMe', '+': '*' },
    { '*': 'sharing', '+': '*' },
    { '*': 'did', '+': '*' },
    { '*': 'profile', '+': '*' },
  ]);

  json(res, 200, {
    valid: true,
    certificate: cert,
    mainPub,
    did: 'did:gun:' + mainPub,
    profile: finalProfile,
    token: omdKey,
  });
  console.log('[auth] gateway signin cert →', devicePub.slice(0, 12) + '...', '→ did:gun:' + mainPub, 'profile:', finalProfile.name);
}

// ── Auth: Link (certificate issuance for password/peer users) ────

async function handleLink(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { mainPub, devicePub } = body;
  if (!mainPub || !devicePub)
    return json(res, 400, { error: 'Missing mainPub or devicePub' });

  // Verify mainPub looks like a valid SEA public key
  if (typeof mainPub !== 'string' || mainPub.length < 10)
    return json(res, 400, { error: 'Invalid mainPub' });

  if (await isDidDeleted(mainPub))
    return json(res, 410, { error: 'Account has been deleted', code: 'account_deleted' });

  const cert = await issueCertificate(devicePub, [
    { '*': 'devices', '+': '*' },
    { '*': 'chats', '+': '*' },
    { '*': 'knowledge', '+': '*' },
    { '*': 'prompts', '+': '*' },
    { '*': 'settings', '+': '*' },
    { '*': 'sharedToMe', '+': '*' },
    { '*': 'sharing', '+': '*' },
    { '*': 'did', '+': '*' },
    { '*': 'profile', '+': '*' },
  ]);

  json(res, 200, {
    certificate: cert,
    mainPub,
    did: 'did:gun:' + mainPub,
  });
  console.log('[auth] link cert →', devicePub.slice(0, 12) + '...', '→ did:gun:' + mainPub);
}

// ── Welcome Email Sender ───────────────────────────────────────

function sendWelcomeEmail(toEmail, did) {
  return new Promise((resolve, reject) => {
    if (!toEmail || typeof toEmail !== 'string' || !toEmail.includes('@')) {
      return resolve(false);
    }

    const socket = tls.connect(SMTP_PORT, SMTP_HOST, () => {});
    socket.setEncoding('utf8');

    let step = 0;
    const timeout = setTimeout(() => {
      socket.destroy();
      reject(new Error('SMTP timeout'));
    }, 15000);

    const cleanup = (err) => {
      clearTimeout(timeout);
      socket.destroy();
      if (err) reject(err);
      else resolve(true);
    };

    socket.on('error', (err) => cleanup(err));

    socket.on('data', (data) => {
      const line = data.toString();
      const code = parseInt(line.substring(0, 3), 10);
      if (code >= 400) {
        socket.write('QUIT\r\n');
        return cleanup(new Error(`SMTP error ${code}: ${line.trim()}`));
      }

      if (step === 0 && code === 220) {
        step = 1;
        socket.write(`EHLO ${SMTP_DOMAIN}\r\n`);
      } else if (step === 1 && code === 250) {
        step = 2;
        socket.write(`MAIL FROM:<${SMTP_FROM}>\r\n`);
      } else if (step === 2 && code === 250) {
        step = 3;
        socket.write(`RCPT TO:<${toEmail}>\r\n`);
      } else if (step === 3 && code === 250) {
        step = 4;
        socket.write('DATA\r\n');
      } else if (step === 4 && code === 354) {
        step = 5;
        const msgId = `<${Date.now()}.${crypto.randomBytes(8).toString('hex')}@${SMTP_DOMAIN}>`;
        const dateStr = new Date().toUTCString();
        const boundary = `omd_bnd_${crypto.randomBytes(12).toString('hex')}`;

        const subject = 'Welcome to OnMyDisk!';
        const plainText = [
          'Hello,',
          '',
          'Welcome to OnMyDisk — your decentralized personal cloud!',
          '',
          'Your account has been successfully created. Here is your Decentralized Identifier (DID):',
          did,
          '',
          'How OnMyDisk works:',
          '• Zero central storage: Your files reside on your own devices and are never uploaded to a corporate server.',
          '• End-to-end cryptographic privacy: Your identity, data, and sessions are encrypted using keys under your sole control.',
          '• Direct peer-to-peer sync: Share files and collaborate directly with peers with complete privacy.',
          '',
          'Important Security Note:',
          'Please make sure you have saved your Recovery Key displayed during registration. Because OnMyDisk is decentralized and has no central administrator with access to your keys, your Recovery Key is the only way to restore your access if you switch devices or clear your browser data.',
          '',
          'Please feel free to reply directly to this email if you have any questions, ideas, or need assistance. I am always happy to help.',
          '',
          'Best regards,',
          'Alexey Volkov',
          'OnMyDisk',
          'onmydisk@volkoff.info'
        ].join('\r\n');

        const htmlText = `<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Welcome to OnMyDisk</title>
</head>
<body style="margin: 0; padding: 0; background-color: #f4f6f8; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; color: #1e293b;">
  <table width="100%" border="0" cellspacing="0" cellpadding="0" style="background-color: #f4f6f8; padding: 30px 15px;">
    <tr>
      <td align="center">
        <table width="100%" border="0" cellspacing="0" cellpadding="0" style="max-width: 580px; background-color: #ffffff; border-radius: 12px; overflow: hidden; box-shadow: 0 4px 12px rgba(0, 0, 0, 0.05); border: 1px solid #e2e8f0;">
          <tr>
            <td style="padding: 32px 36px; background: linear-gradient(135deg, #1e293b 0%, #0f172a 100%);">
              <h1 style="margin: 0; color: #ffffff; font-size: 22px; font-weight: 700; letter-spacing: -0.02em;">OnMyDisk</h1>
              <p style="margin: 6px 0 0 0; color: #94a3b8; font-size: 14px;">Your Private Decentralized Cloud</p>
            </td>
          </tr>
          <tr>
            <td style="padding: 36px 36px 28px 36px;">
              <h2 style="margin: 0 0 16px 0; color: #0f172a; font-size: 20px; font-weight: 600;">Welcome to OnMyDisk!</h2>
              <p style="margin: 0 0 20px 0; font-size: 15px; line-height: 1.6; color: #334155;">
                Thank you for joining OnMyDisk. Your decentralized account has been successfully created.
              </p>

              <div style="background-color: #f8fafc; border: 1px solid #e2e8f0; border-radius: 8px; padding: 16px; margin: 0 0 24px 0;">
                <div style="font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.05em; color: #64748b; margin-bottom: 6px;">Your Decentralized Identifier (DID)</div>
                <div style="font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; font-size: 13px; color: #0f172a; word-break: break-all; line-height: 1.4;">${did}</div>
              </div>

              <h3 style="margin: 0 0 12px 0; font-size: 15px; font-weight: 600; color: #0f172a;">How OnMyDisk works:</h3>
              <ul style="margin: 0 0 24px 0; padding-left: 20px; font-size: 14px; line-height: 1.6; color: #475569;">
                <li style="margin-bottom: 8px;"><strong>Zero central storage:</strong> Your files stay on your devices and are never uploaded to a corporate server.</li>
                <li style="margin-bottom: 8px;"><strong>End-to-end cryptographic privacy:</strong> Your identity and data are encrypted using keys under your sole control.</li>
                <li style="margin-bottom: 8px;"><strong>Direct peer-to-peer sync:</strong> Share files and collaborate directly with peers with complete privacy.</li>
              </ul>

              <div style="background-color: #eff6ff; border-left: 4px solid #3b82f6; border-radius: 4px; padding: 14px 16px; margin: 0 0 28px 0;">
                <strong style="display: block; font-size: 13px; color: #1e40af; margin-bottom: 4px;">Important Security Reminder</strong>
                <span style="font-size: 13px; line-height: 1.5; color: #1e3a8a;">
                  Please make sure you have safely stored your <strong>Recovery Key</strong> from the registration screen. Because OnMyDisk is decentralized and has no central administrator with access to your keys, your Recovery Key is the only way to recover your account if you switch devices or reset your browser.
                </span>
              </div>

              <p style="margin: 0 0 8px 0; font-size: 14px; line-height: 1.6; color: #334155;">
                Please feel free to reply directly to this email if you have any questions, ideas, or need assistance. I am always happy to hear from you.
              </p>

              <div style="margin-top: 28px; padding-top: 20px; border-top: 1px solid #f1f5f9; font-size: 14px; line-height: 1.5; color: #334155;">
                <strong>Alexey Volkov</strong><br>
                <span style="color: #64748b;">OnMyDisk</span><br>
                <a href="mailto:onmydisk@volkoff.info" style="color: #2563eb; text-decoration: none;">onmydisk@volkoff.info</a>
              </div>
            </td>
          </tr>
        </table>
      </td>
    </tr>
  </table>
</body>
</html>`;

        const msg = [
          `From: ${SMTP_FROM_NAME} <${SMTP_FROM}>`,
          `To: ${toEmail}`,
          `Subject: ${subject}`,
          `Date: ${dateStr}`,
          `Message-ID: ${msgId}`,
          `Reply-To: ${SMTP_FROM_NAME} <${SMTP_FROM}>`,
          'MIME-Version: 1.0',
          `Content-Type: multipart/alternative; boundary="${boundary}"`,
          '',
          `--${boundary}`,
          'Content-Type: text/plain; charset=UTF-8',
          'Content-Transfer-Encoding: 8bit',
          '',
          plainText,
          '',
          `--${boundary}`,
          'Content-Type: text/html; charset=UTF-8',
          'Content-Transfer-Encoding: 8bit',
          '',
          htmlText,
          '',
          `--${boundary}--`,
          '',
          '.'
        ].join('\r\n') + '\r\n';

        socket.write(msg);
      } else if (step === 5 && code === 250) {
        step = 6;
        socket.write('QUIT\r\n');
        cleanup();
      }
    });
  });
}

// ── Auth: Register (new DID + email mapping + certificate) ─────

async function handleRegister(req, res) {
  const ip = req.headers['x-forwarded-for'] || req.socket.remoteAddress || 'unknown';

  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { mainPub, devicePub, email, omdKey: clientKey } = body;
  if (!mainPub || !devicePub)
    return json(res, 400, { error: 'Missing mainPub or devicePub' });

  if (await isDidDeleted(mainPub))
    return json(res, 410, { error: 'Account has been deleted', code: 'account_deleted' });

  // Rate limit check
  const rateError = isRateLimited(ip, email || mainPub);
  if (rateError) return json(res, 429, { error: rateError });

  let omdKey = clientKey;
  if (!omdKey || typeof omdKey !== 'string' || omdKey.length < 16) {
    omdKey = crypto.randomBytes(16).toString('hex');
  }

  // Store email → DID mapping (for future recovery)
  if (email && typeof email === 'string' && email.includes('@')) {
    const emailKey = '~@email:' + email.toLowerCase();
    const existingEmail = await gunOnce(emailKey);
    if (existingEmail && existingEmail.mainPub && existingEmail.mainPub !== mainPub) {
      return json(res, 409, { error: 'Email already associated with another account' });
    }
    gun.get(emailKey).put({
      mainPub,
      did: 'did:gun:' + mainPub,
      createdAt: Date.now(),
    });
    console.log('[auth] registered email mapping:', email, '→', mainPub);

    // Send welcome email asynchronously
    sendWelcomeEmail(email, 'did:gun:' + mainPub)
      .then(() => console.log('[auth] welcome email sent to', email))
      .catch((err) => console.error('[auth] failed to send welcome email to', email, err.message));
  }

  // Issue certificate
  const cert = await issueCertificate(devicePub, [
    { '*': 'devices', '+': '*' },
    { '*': 'chats', '+': '*' },
    { '*': 'knowledge', '+': '*' },
    { '*': 'prompts', '+': '*' },
    { '*': 'settings', '+': '*' },
    { '*': 'sharedToMe', '+': '*' },
    { '*': 'sharing', '+': '*' },
    { '*': 'did', '+': '*' },
    { '*': 'profile', '+': '*' },
  ]);

  // Ensure omd_key is stored in Gun profile
  gun.get('~' + mainPub).get('profile').get('omd_key').put(omdKey);

  json(res, 200, {
    certificate: cert,
    mainPub,
    did: 'did:gun:' + mainPub,
    token: omdKey,
    omd_key: omdKey,
  });
  console.log('[auth] register cert →', devicePub.slice(0, 12) + '...', '→ did:gun:' + mainPub, 'omd_key:', omdKey);
}

// ── Auth: Legacy Link ──────────────────────────────────────────

async function handleLegacyLink(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { omdKey, devicePub } = body;
  if (!omdKey || !devicePub)
    return json(res, 400, { error: 'Missing omdKey or devicePub' });

  const mappingKey = '~@legacy:' + omdKey;
  const existing = await gunOnce(mappingKey);

  let mainPub;
  if (existing && existing.mainPub) {
    mainPub = existing.mainPub;
  } else {
    const masterPair = await Gun.SEA.pair();
    mainPub = masterPair.pub;
    gun.get(mappingKey).put({
      mainPub,
      did: 'did:gun:' + mainPub,
      migratedAt: Date.now(),
    });
    console.log('[auth] migrated legacy user → DID:', mainPub);
  }

  if (await isDidDeleted(mainPub))
    return json(res, 410, { error: 'Account has been deleted', code: 'account_deleted' });

  const cert = await issueCertificate(devicePub, [
    { '*': 'devices', '+': '*' },
    { '*': 'chats', '+': '*' },
    { '*': 'knowledge', '+': '*' },
    { '*': 'prompts', '+': '*' },
    { '*': 'settings', '+': '*' },
    { '*': 'sharedToMe', '+': '*' },
    { '*': 'sharing', '+': '*' },
    { '*': 'did', '+': '*' },
    { '*': 'profile', '+': '*' },
  ]);

  const existingProfile = await gunOnce('~' + mainPub + '/profile');
  const profile = {
    user: existingProfile?.user || (existing?.user || ''),
    name: existingProfile?.name || (existing?.name || ''),
    email: existingProfile?.email || (existing?.email || ''),
    image: existingProfile?.image || (existing?.image || ''),
  };

  json(res, 200, { certificate: cert, mainPub, did: 'did:gun:' + mainPub, profile });
  console.log('[auth] legacy cert →', devicePub.slice(0, 12) + '...', '→ did:gun:' + mainPub);
}

// ── Auth: Delete Account ───────────────────────────────────────

// Verify a password against the credentials stored by GunDB for a native
// account, or against the legacy gateway migration record.
async function verifyPasswordForDid(username, password, mainPub) {
  const norm = (username || '').toLowerCase().trim();
  if (!norm || !password) return false;

  // Native GunDB account: ~@alias -> ~pub/auth (SEA-encrypted master key)
  const aliasData = await gunOnce('~@' + norm);
  if (aliasData) {
    const pubs = new Set();
    for (const key of Object.keys(aliasData)) {
      if (key === '_' || !key) continue;
      let pub = key.startsWith('~') ? key.slice(1) : key;
      const ref = aliasData[key];
      if (ref && typeof ref === 'object' && ref['#']) {
        let soul = ref['#'];
        if (typeof soul === 'string' && soul.startsWith('~')) soul = soul.slice(1);
        if (soul) pub = soul;
      }
      if (pub === mainPub) pubs.add(pub);
    }

    for (const pub of pubs) {
      const authRaw = await gunGet('~' + pub, 'auth');
      if (!authRaw) continue;
      let authObj = authRaw;
      if (typeof authRaw === 'string') {
        try { authObj = JSON.parse(authRaw); } catch (_) { continue; }
      }
      if (!authObj || !authObj.s || !authObj.ek) continue;
      try {
        const proof = await Gun.SEA.work(password, authObj.s);
        if (!proof) continue;
        const dec = await Gun.SEA.decrypt(authObj.ek, proof);
        if (dec && dec.priv) return true;
      } catch (_) {}
    }
  }

  // Legacy migrated account: username mapping + password verifier / derived key
  const legacy = await gunOnce('omd_legacy_user/' + norm);
  if (legacy && legacy.mainPub === mainPub) {
    const derived = deriveDeterministicPair(norm, password);
    const credCheck = crypto.pbkdf2Sync(password || '', 'omd_cred_v1:' + norm, 100000, 32, 'sha256').toString('hex');
    if (derived.pub === mainPub || legacy.credCheck === credCheck) return true;
  }

  return false;
}

async function deleteAliasMapping(alias, mainPub) {
  const norm = (alias || '').toLowerCase().trim();
  if (!norm) return;
  const aliasPath = '~@' + norm;
  const data = await gunOnce(aliasPath);
  if (!data || typeof data !== 'object') return;

  for (const key of Object.keys(data)) {
    if (key === '_' || !key) continue;
    let pub = key.startsWith('~') ? key.slice(1) : key;
    const ref = data[key];
    if (ref && typeof ref === 'object' && ref['#']) {
      let soul = ref['#'];
      if (typeof soul === 'string' && soul.startsWith('~')) soul = soul.slice(1);
      if (soul) pub = soul;
    }
    if (pub === mainPub) {
      await gunPutChild(aliasPath, key, null);
    }
  }
}

async function deleteNodeIfMatches(path, mainPub) {
  const record = await gunOnce(path);
  if (!record || typeof record !== 'object') return;
  if (mainPub && record.mainPub !== mainPub) return;
  // GunDB rejects `put(null)` at the root of a node ("Data at root of graph
  // must be a node"), so clear the fields individually. This also avoids
  // leaving a stale `mainPub` that would block re-registration with the same
  // email/legacy key.
  for (const key of Object.keys(record)) {
    if (key === '_' || key === '#' || key === '>') continue;
    await gunPutChild(path, key, null);
  }
}

async function clearFlatNode(path) {
  const data = await gunOnce(path);
  if (!data || typeof data !== 'object') return;
  for (const key of Object.keys(data)) {
    if (key === '_' || key === '#' || key === '>') continue;
    await gunPutChild(path, key, null);
  }
}

async function purgeDidGraph(mainPub) {
  const childKeys = [
    'auth', 'epub', 'alias', 'pub', 'profile', 'devices',
    'did', 'settings', 'prompts', 'sharing', 'sharedToMe',
  ];
  for (const key of childKeys) {
    await gunPutChild('~' + mainPub, key, null);
  }
  await clearFlatNode('devices:' + mainPub);
  await clearFlatNode('sharing:' + mainPub);
  await clearFlatNode('mcp_keys:' + mainPub);
  await clearFlatNode('ssh_creds:' + mainPub);
  if (Object.prototype.hasOwnProperty.call(mcpKeysByDid, mainPub)) {
    const nextMcp = { ...mcpKeysByDid };
    delete nextMcp[mainPub];
    mcpKeysByDid = nextMcp;
    persistMcpKeysStore(mcpKeysByDid);
  }
}

async function verifyDeleteProof(body) {
  const mainPub = body.mainPub;

  const tokenData = verifyDeleteToken(mainPub, body.deleteToken);
  if (tokenData) {
    return { type: 'token', provider: tokenData.p || '', subject: tokenData.s || '' };
  }

  if (body.password && typeof body.password === 'string') {
    const profile = await gunGet('~' + mainPub, 'profile');
    const names = new Set();
    if (body.username) names.add(String(body.username).toLowerCase().trim());
    if (profile && profile.user) names.add(String(profile.user).toLowerCase().trim());
    for (const name of names) {
      if (!name) continue;
      if (await verifyPasswordForDid(name, body.password, mainPub)) {
        return { type: 'password', username: name };
      }
    }
  }

  if (body.googleIdToken) {
    try {
      const googleUser = await verifyGoogleToken(body.googleIdToken);
      const mapping = await gunOnce('~@google:' + googleUser.sub);
      if (mapping && mapping.mainPub === mainPub) {
        return { type: 'google', provider: 'google', subject: googleUser.sub };
      }
    } catch (_) {}
  }

  if (body.appleIdToken) {
    try {
      const appleUser = await verifyAppleToken(body.appleIdToken);
      const mapping = await gunOnce('~@apple:' + appleUser.sub);
      if (mapping && mapping.mainPub === mainPub) {
        return { type: 'apple', provider: 'apple', subject: appleUser.sub };
      }
    } catch (_) {}
  }

  return null;
}

async function purgeAccountRecords(mainPub, proof, body) {
  const profile = (await gunGet('~' + mainPub, 'profile')) || {};

  const names = new Set();
  if (proof.username) names.add(proof.username.toLowerCase().trim());
  if (profile.user) names.add(String(profile.user).toLowerCase().trim());
  if (body.username) names.add(String(body.username).toLowerCase().trim());

  for (const name of names) {
    if (!name) continue;
    await deleteAliasMapping(name, mainPub);
    await deleteNodeIfMatches('omd_legacy_user/' + name, mainPub);
  }

  const emails = new Set();
  if (profile.email) emails.add(String(profile.email).toLowerCase().trim());
  if (body.email) emails.add(String(body.email).toLowerCase().trim());
  for (const email of emails) {
    if (!email || !email.includes('@')) continue;
    await deleteNodeIfMatches('~@email:' + email, mainPub);
  }

  const legacyKeys = new Set();
  if (profile.omd_key) legacyKeys.add(String(profile.omd_key));
  if (body.omdKey) legacyKeys.add(String(body.omdKey));
  for (const key of legacyKeys) {
    if (!key) continue;
    await deleteNodeIfMatches('~@legacy:' + key, mainPub);
    await deleteNodeIfMatches('omd_legacy/' + key, mainPub);
  }

  // Remove the provider mapping only if it still points at this DID. This
  // lets the same Google/Apple account create a fresh identity afterwards.
  if (proof.provider === 'google' && proof.subject) {
    const mapping = await gunOnce('~@google:' + proof.subject);
    if (mapping && mapping.mainPub === mainPub) {
      await gunPut('~@google:' + proof.subject, null);
    }
  }
  if (proof.provider === 'apple' && proof.subject) {
    const mapping = await gunOnce('~@apple:' + proof.subject);
    if (mapping && mapping.mainPub === mainPub) {
      await gunPut('~@apple:' + proof.subject, null);
    }
  }

  await purgeDidGraph(mainPub);
  return profile;
}

async function handleDeleteAccount(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { mainPub } = body;
  if (!mainPub || typeof mainPub !== 'string' || mainPub.length < 10)
    return json(res, 400, { error: 'Invalid mainPub' });

  // Idempotent: a retry after a lost response must not fail.
  if (await isDidDeleted(mainPub))
    return json(res, 200, { ok: true, alreadyDeleted: true, mainPub });

  const proof = await verifyDeleteProof(body);
  if (!proof)
    return json(res, 401, { error: 'Re-authentication required to delete this account' });

  // Mark the DID as deleted BEFORE removing credentials. Every
  // auth/certificate path checks this server-side tombstone, so credential
  // leftovers or replayed writes cannot make the account usable again.
  const marked = await markDidDeleted(mainPub, 'user_request');
  if (!marked)
    return json(res, 500, { error: 'Failed to record account deletion' });

  let cleanupPending = false;
  let deletedProfile = null;
  try {
    deletedProfile = await purgeAccountRecords(mainPub, proof, body);
  } catch (err) {
    cleanupPending = true;
    console.warn('[auth] account deleted, record cleanup incomplete:', err?.message || err);
  }

  try {
    const meta = {};
    if (proof.username) meta.username = String(proof.username).toLowerCase().trim();
    if (deletedProfile && deletedProfile.user) meta.username = String(deletedProfile.user).toLowerCase().trim();
    if (body.username) meta.username = String(body.username).toLowerCase().trim();
    if (deletedProfile && deletedProfile.email) meta.email = String(deletedProfile.email).toLowerCase().trim();
    if (body.email) meta.email = String(body.email).toLowerCase().trim();
    if (deletedProfile && deletedProfile.omd_key) meta.omdKey = String(deletedProfile.omd_key);
    if (body.omdKey) meta.omdKey = String(body.omdKey);
    if (proof.provider) meta.provider = proof.provider;
    if (proof.subject) meta.subject = proof.subject;
    await updateDeletedDidMeta(mainPub, meta);
  } catch (_) {}

  console.log('[auth] account deleted:', mainPub, 'proof:', proof.type, cleanupPending ? '(cleanup pending)' : '');
  json(res, 200, {
    ok: true,
    mainPub,
    did: 'did:gun:' + mainPub,
    deletedAt: Date.now(),
    cleanupPending,
  });
}

// ── Admin: purge deleted accounts' flat registration records ───

function adminAuthorized(req, body) {
  const secret = process.env.ADMIN_SECRET;
  if (!secret) return false;
  if (req.headers['x-admin-secret'] === secret) return true;
  if (body && body.adminSecret === secret) return true;
  return false;
}

async function handleAdminPurgeDeleted(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  if (!process.env.ADMIN_SECRET)
    return json(res, 500, { error: 'ADMIN_SECRET is not configured on the hub' });
  if (!adminAuthorized(req, body))
    return json(res, 401, { error: 'Admin authorization required' });

  const report = [];
  for (const [mainPub, record] of Object.entries(deletedDids)) {
    const item = { mainPub, purged: [] };
    try {
      await clearFlatNode('devices:' + mainPub);
      await clearFlatNode('sharing:' + mainPub);
      await clearFlatNode('mcp_keys:' + mainPub);
      await clearFlatNode('ssh_creds:' + mainPub);
      if (Object.prototype.hasOwnProperty.call(mcpKeysByDid, mainPub)) {
        const nextMcp = { ...mcpKeysByDid };
        delete nextMcp[mainPub];
        mcpKeysByDid = nextMcp;
        persistMcpKeysStore(mcpKeysByDid);
      }
      item.purged.push('devices', 'sharing', 'mcp_keys');

      if (record.username) {
        await deleteAliasMapping(record.username, mainPub);
        await deleteNodeIfMatches('omd_legacy_user/' + record.username, mainPub);
        item.purged.push('alias', 'legacy_user');
      }
      if (record.email) {
        await deleteNodeIfMatches('~@email:' + record.email, mainPub);
        item.purged.push('email');
      }
      if (record.omdKey) {
        await deleteNodeIfMatches('~@legacy:' + record.omdKey, mainPub);
        await deleteNodeIfMatches('omd_legacy/' + record.omdKey, mainPub);
        item.purged.push('legacy');
      }
      if (record.provider === 'google' && record.subject) {
        await deleteNodeIfMatches('~@google:' + record.subject, mainPub);
        item.purged.push('google');
      }
      if (record.provider === 'apple' && record.subject) {
        await deleteNodeIfMatches('~@apple:' + record.subject, mainPub);
        item.purged.push('apple');
      }
      item.ok = true;
    } catch (err) {
      item.ok = false;
      item.error = err?.message || String(err);
    }
    report.push(item);
  }

  json(res, 200, { count: report.length, report });
}

async function handleAdminDropAlias(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  if (!process.env.ADMIN_SECRET)
    return json(res, 500, { error: 'ADMIN_SECRET is not configured on the hub' });
  if (!adminAuthorized(req, body))
    return json(res, 401, { error: 'Admin authorization required' });

  const alias = String(body.alias || '').toLowerCase().trim();
  if (!alias)
    return json(res, 400, { error: 'alias is required' });

  const aliasPath = '~@' + alias;
  const data = await gunOnce(aliasPath);
  if (!data || typeof data !== 'object') {
    return json(res, 200, { alias, dropped: false, reason: 'not found' });
  }

  for (const key of Object.keys(data)) {
    if (key === '_' || key === '#' || key === '>') continue;
    await gunPutChild(aliasPath, key, null);
  }

  const after = await gunOnce(aliasPath);
  const remaining = after && typeof after === 'object'
    ? Object.keys(after).filter(k => k !== '_' && k !== '#' && k !== '>')
    : [];
  json(res, 200, { alias, dropped: remaining.length === 0, remaining });
}

// ── Auth: Request Email Change (with verification) ─────────────

async function handleRequestEmailChange(req, res) {
  let body;
  try { body = await readBody(req); }
  catch { return json(res, 400, { error: 'Invalid JSON' }); }

  const { mainPub, newEmail } = body;
  if (!mainPub || !newEmail || !newEmail.includes('@'))
    return json(res, 400, { error: 'Valid mainPub and newEmail required' });

  // Store pending email update in user graph
  gun.get('~' + mainPub).get('profile').get('pendingEmail').put({
    email: newEmail,
    requestedAt: Date.now(),
  });

  console.log('[auth] email change requested for:', mainPub, '→', newEmail);
  json(res, 200, {
    success: true,
    message: `Verification link sent to ${newEmail}. Click the confirmation link to finalize updating your recovery email.`,
    pendingEmail: newEmail,
  });
}

// ── Native Mobile Push Relay (FCM HTTP v1 + APNs HTTP/2) ───────
// Keeps Apple (.p8) and Firebase Service Account keys on the central OMD hub
// so self-hosted user AI nodes can dispatch rich native pushes without holding
// app-store signing secrets. Hot-loads credentials from /data (repoPath).

let _fcmTokenCache = { accessToken: '', expiresAt: 0, clientEmail: '' };
let _apnsJwtCache = { token: '', issuedAt: 0, keyId: '' };

function resolveFcmServiceAccount() {
  const candidates = [
    process.env.FCM_SERVICE_ACCOUNT_PATH,
    path.join(repoPath, 'fcm-service-account.json'),
    path.join(path.dirname(new URL(import.meta.url).pathname), 'fcm-service-account.json'),
  ].filter(Boolean);
  for (const p of candidates) {
    try {
      if (fs.existsSync(p)) {
        const parsed = JSON.parse(fs.readFileSync(p, 'utf8'));
        if (parsed && parsed.project_id && parsed.client_email && parsed.private_key) {
          return parsed;
        }
      }
    } catch (e) {
      console.warn('[push-relay] Failed reading FCM service account:', e?.message || e);
    }
  }
  return null;
}

function resolveApnsConfig() {
  let cfg = {
    teamId: process.env.APNS_TEAM_ID || 'UMAS9T29TH',
    bundleId: process.env.APNS_BUNDLE_ID || 'net.onmydisk.app',
    keyId: process.env.APNS_KEY_ID || '',
    keyPath: process.env.APNS_KEY_PATH || '',
  };
  const cfgJsonPath = path.join(repoPath, 'apns.json');
  try {
    if (fs.existsSync(cfgJsonPath)) {
      const loaded = JSON.parse(fs.readFileSync(cfgJsonPath, 'utf8'));
      cfg = { ...cfg, ...loaded };
    }
  } catch (_) {}

  if (!cfg.keyPath || !fs.existsSync(cfg.keyPath)) {
    try {
      const files = fs.readdirSync(repoPath);
      const p8 = files.find((f) => /^AuthKey_[A-Z0-9]{10}\.p8$/i.test(f)) || files.find((f) => f.endsWith('.p8'));
      if (p8) {
        cfg.keyPath = path.join(repoPath, p8);
        if (!cfg.keyId) {
          const m = p8.match(/^AuthKey_([A-Z0-9]{10})\.p8$/i);
          if (m) cfg.keyId = m[1];
        }
      }
    } catch (_) {}
  }

  if (cfg.keyPath && fs.existsSync(cfg.keyPath) && cfg.keyId && cfg.teamId && cfg.bundleId) {
    try {
      const privateKey = fs.readFileSync(cfg.keyPath, 'utf8');
      return { ...cfg, privateKey };
    } catch (_) {}
  }
  return null;
}

async function getFcmAccessToken(sa) {
  const nowSec = Math.floor(Date.now() / 1000);
  if (
    _fcmTokenCache.accessToken &&
    _fcmTokenCache.clientEmail === sa.client_email &&
    _fcmTokenCache.expiresAt - 300 > nowSec
  ) {
    return _fcmTokenCache.accessToken;
  }

  const header = Buffer.from(JSON.stringify({ alg: 'RS256', typ: 'JWT' })).toString('base64url');
  const claim = Buffer.from(
    JSON.stringify({
      iss: sa.client_email,
      scope: 'https://www.googleapis.com/auth/firebase.messaging',
      aud: sa.token_uri || 'https://oauth2.googleapis.com/token',
      iat: nowSec,
      exp: nowSec + 3600,
    })
  ).toString('base64url');

  const signingInput = `${header}.${claim}`;
  const sig = crypto.sign('RSA-SHA256', Buffer.from(signingInput), sa.private_key).toString('base64url');
  const assertion = `${signingInput}.${sig}`;

  const resp = await fetch(sa.token_uri || 'https://oauth2.googleapis.com/token', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      grant_type: 'urn:ietf:params:oauth:grant-type:jwt-bearer',
      assertion,
    }),
  });
  const tokenJson = await resp.json().catch(() => ({}));
  if (!resp.ok || !tokenJson.access_token) {
    throw new Error(`FCM OAuth2 token error (${resp.status}): ${JSON.stringify(tokenJson).slice(0, 200)}`);
  }
  _fcmTokenCache = {
    accessToken: tokenJson.access_token,
    expiresAt: nowSec + Number(tokenJson.expires_in || 3600),
    clientEmail: sa.client_email,
  };
  return _fcmTokenCache.accessToken;
}

function getApnsJwt(apnsCfg) {
  const nowSec = Math.floor(Date.now() / 1000);
  if (_apnsJwtCache.token && _apnsJwtCache.keyId === apnsCfg.keyId && nowSec - _apnsJwtCache.issuedAt < 2700) {
    return _apnsJwtCache.token;
  }
  const header = Buffer.from(JSON.stringify({ alg: 'ES256', kid: apnsCfg.keyId })).toString('base64url');
  const claims = Buffer.from(JSON.stringify({ iss: apnsCfg.teamId, iat: nowSec })).toString('base64url');
  const signingInput = `${header}.${claims}`;
  const sig = crypto
    .sign('sha256', Buffer.from(signingInput), {
      key: apnsCfg.privateKey,
      dsaEncoding: 'ieee-p1363',
    })
    .toString('base64url');
  const jwt = `${signingInput}.${sig}`;
  _apnsJwtCache = { token: jwt, issuedAt: nowSec, keyId: apnsCfg.keyId };
  return jwt;
}

function sendApnsHttp2Once(host, deviceToken, apnsCfg, jwt, apnsPayload, collapseId = '') {
  return new Promise((resolve) => {
    const client = http2.connect(`https://${host}`);
    let settled = false;
    const finish = (result) => {
      if (settled) return;
      settled = true;
      try { client.close(); } catch (_) {}
      resolve(result);
    };

    client.on('error', (err) => {
      finish({ ok: false, status: 502, reason: err?.message || 'http2_connect_error' });
    });
    client.setTimeout(12000, () => {
      finish({ ok: false, status: 504, reason: 'apns_timeout' });
    });

    const headers = {
      ':method': 'POST',
      ':path': `/3/device/${encodeURIComponent(deviceToken)}`,
      authorization: `bearer ${jwt}`,
      'apns-topic': apnsCfg.bundleId,
      'apns-push-type': 'alert',
      'apns-priority': '10',
      'content-type': 'application/json',
    };
    if (collapseId) {
      headers['apns-collapse-id'] = String(collapseId).slice(0, 64);
    }

    const req = client.request(headers);
    let respStatus = 0;
    const chunks = [];

    req.on('response', (hdrs) => {
      respStatus = Number(hdrs[':status'] || 0);
    });
    req.on('data', (chunk) => chunks.push(chunk));
    req.on('end', () => {
      const raw = Buffer.concat(chunks).toString('utf8');
      let reason = raw;
      try {
        const j = JSON.parse(raw);
        if (j && j.reason) reason = j.reason;
      } catch (_) {}
      finish({
        ok: respStatus >= 200 && respStatus < 300,
        status: respStatus || 500,
        reason: reason || '',
      });
    });
    req.on('error', (err) => {
      finish({ ok: false, status: 502, reason: err?.message || 'apns_stream_error' });
    });

    req.end(JSON.stringify(apnsPayload));
  });
}

async function handlePushStatus(req, res) {
  const fcm = resolveFcmServiceAccount();
  const apns = resolveApnsConfig();
  return json(res, 200, {
    status: 'ok',
    fcmConfigured: Boolean(fcm),
    fcmProjectId: fcm ? fcm.project_id : null,
    apnsConfigured: Boolean(apns),
    apnsKeyId: apns ? apns.keyId : null,
    apnsTeamId: apns ? apns.teamId : null,
    bundleId: apns ? apns.bundleId : 'net.onmydisk.app',
  });
}

async function handleNativePushSend(req, res) {
  let body;
  try {
    body = await readBody(req);
  } catch (_) {
    return json(res, 400, { ok: false, error: 'Invalid JSON body' });
  }

  const platform = String(body?.platform || '').toLowerCase().trim();
  const token = String(body?.token || '').trim();
  const title = String(body?.title || 'On My Disk');
  const textBody = String(body?.body || '');
  const image = String(body?.image || '').trim();
  const tag = String(body?.tag || '').trim();
  const category = String(body?.category || '').trim();
  const rawData = body?.data && typeof body.data === 'object' ? body.data : {};

  if (!token || !['android', 'ios'].includes(platform)) {
    return json(res, 400, { ok: false, error: 'platform ("android"|"ios") and token are required' });
  }

  // Per-IP basic rate guard against abuse (max 120 push dispatches / 15 min per node IP)
  const callerIp = String(req.headers['x-forwarded-for'] || req.socket?.remoteAddress || 'unknown').split(',')[0].trim();
  if (!rateLimit(`push:${callerIp}`, 120)) {
    return json(res, 429, { ok: false, error: 'Rate limit exceeded' });
  }

  if (platform === 'android') {
    const sa = resolveFcmServiceAccount();
    if (!sa) {
      return json(res, 503, {
        ok: false,
        status: 503,
        error: 'FCM Service Account is not configured on OMD Push Relay (/data/fcm-service-account.json)',
      });
    }
    try {
      const accessToken = await getFcmAccessToken(sa);
      const stringData = {};
      for (const [k, v] of Object.entries(rawData)) {
        if (v !== undefined && v !== null) {
          stringData[String(k)] = typeof v === 'string' ? v : JSON.stringify(v);
        }
      }
      stringData.title = title;
      stringData.body = textBody;
      if (image) stringData.image = image;
      stringData.payload_json = JSON.stringify({
        title,
        body: textBody,
        image,
        tag,
        category,
        data: rawData,
      });

      const validHttpsImage = /^https:\/\//i.test(image) ? image : undefined;
      const fcmMessage = {
        message: {
          token,
          notification: {
            title,
            body: textBody,
            ...(validHttpsImage ? { image: validHttpsImage } : {}),
          },
          data: stringData,
          android: {
            priority: 'HIGH',
            ...(tag ? { collapse_key: tag.slice(0, 64) } : {}),
            notification: {
              channel_id: 'omd_autopilot_high',
              sound: 'default',
              default_vibrate_timings: true,
              ...(validHttpsImage ? { image: validHttpsImage } : {}),
            },
          },
        },
      };

      const fcmResp = await fetch(
        `https://fcm.googleapis.com/v1/projects/${encodeURIComponent(sa.project_id)}/messages:send`,
        {
          method: 'POST',
          headers: {
            Authorization: `Bearer ${accessToken}`,
            'Content-Type': 'application/json',
          },
          body: JSON.stringify(fcmMessage),
        }
      );
      const fcmJson = await fcmResp.json().catch(() => ({}));
      if (fcmResp.ok) {
        return json(res, 200, { ok: true, platform: 'android', id: fcmJson.name || '' });
      }
      const errCode = fcmJson?.error?.details?.[0]?.errorCode || fcmJson?.error?.status || '';
      const unregistered = errCode === 'UNREGISTERED' || fcmResp.status === 404 || fcmResp.status === 410;
      return json(res, unregistered ? 410 : fcmResp.status, {
        ok: false,
        status: unregistered ? 410 : fcmResp.status,
        unregistered,
        error: fcmJson?.error?.message || errCode || 'FCM send failed',
      });
    } catch (err) {
      return json(res, 502, { ok: false, status: 502, error: err?.message || String(err) });
    }
  }

  // iOS APNs HTTP/2
  const apnsCfg = resolveApnsConfig();
  if (!apnsCfg) {
    return json(res, 503, {
      ok: false,
      status: 503,
      error: 'APNs .p8 key is not configured on OMD Push Relay (/data/AuthKey_<KEYID>.p8)',
    });
  }
  try {
    const jwt = getApnsJwt(apnsCfg);
    const apnsPayload = {
      aps: {
        alert: { title, body: textBody },
        sound: 'default',
        'mutable-content': 1,
        ...(category ? { category } : {}),
      },
      image,
      data: rawData,
      payload_json: JSON.stringify({
        title,
        body: textBody,
        image,
        tag,
        category,
        data: rawData,
      }),
    };

    let result = await sendApnsHttp2Once('api.push.apple.com', token, apnsCfg, jwt, apnsPayload, tag);
    if (!result.ok && ['BadDeviceToken', 'DeviceTokenNotForTopic'].includes(result.reason)) {
      // Fallback for Xcode debug / sandbox tokens
      const sbResult = await sendApnsHttp2Once('api.sandbox.push.apple.com', token, apnsCfg, jwt, apnsPayload, tag);
      if (sbResult.ok) result = sbResult;
    }

    if (result.ok) {
      return json(res, 200, { ok: true, platform: 'ios' });
    }
    const unregistered = result.status === 410 || result.reason === 'Unregistered';
    return json(res, unregistered ? 410 : (result.status || 502), {
      ok: false,
      status: unregistered ? 410 : (result.status || 502),
      unregistered,
      error: result.reason || 'APNs send failed',
    });
  } catch (err) {
    return json(res, 502, { ok: false, status: 502, error: err?.message || String(err) });
  }
}

// ── HTTP Handler ───────────────────────────────────────────────

async function handler(req, res) {
  // CORS preflight
  if (req.method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'POST, GET, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type',
    });
    return res.end();
  }

  // Auth routes
  if (req.method === 'POST' && req.url === '/api/auth/google-link')
    return handleGoogleLink(req, res);
  if (req.method === 'POST' && req.url === '/api/auth/mobile-oauth-complete')
    return handleMobileOauthComplete(req, res);
  if (req.method === 'GET' && req.url.startsWith('/api/auth/mobile-oauth-poll'))
    return handleMobileOauthPoll(req, res);
  if (req.method === 'POST' && req.url === '/api/auth/apple-link')
    return handleAppleLink(req, res);
  if (req.method === 'POST' && req.url === '/api/auth/legacy-link')
    return handleLegacyLink(req, res);
  if (req.method === 'POST' && req.url === '/api/auth/link')
    return handleLink(req, res);
  if (req.method === 'POST' && req.url === '/api/auth/register')
    return handleRegister(req, res);
  if (req.method === 'POST' && req.url === '/api/auth/gateway-signin')
    return handleGatewaySignin(req, res);
  if (req.method === 'POST' && req.url === '/api/auth/delete-account')
    return handleDeleteAccount(req, res);
  if (req.method === 'POST' && req.url === '/api/admin/purge-deleted')
    return handleAdminPurgeDeleted(req, res);
  if (req.method === 'POST' && req.url === '/api/admin/drop-alias')
    return handleAdminDropAlias(req, res);
  if (req.method === 'POST' && req.url === '/api/auth/request-email-change')
    return handleRequestEmailChange(req, res);

  // Share meta resolver for bot/crawler OG tag generation (server-to-server)
  if (req.method === 'GET' && req.url.startsWith('/api/share-meta'))
    return handleShareMeta(req, res);
  if (req.method === 'GET' && req.url.startsWith('/api/share-cover'))
    return handleShareCover(req, res);
  if (req.method === 'POST' && req.url === '/api/share-record')
    return handleSaveShareRecord(req, res);
  if (req.method === 'POST' && req.url === '/api/publish-device')
    return handlePublishDevice(req, res);
  if (req.method === 'POST' && req.url === '/api/remove-device')
    return handleRemoveDevice(req, res);
  if (req.method === 'GET' && req.url.startsWith('/api/devices'))
    return handleGetDevices(req, res);
  if (req.method === 'POST' && req.url === '/api/mcp-keys')
    return handleSaveMcpKeys(req, res);
  if (req.method === 'GET' && req.url.startsWith('/api/mcp-keys'))
    return handleGetMcpKeys(req, res);

  // Knowledge endpoints for node-side RAG
  if (req.method === 'GET' && req.url.startsWith('/api/chunks'))
    return handleGetChunks(req, res);
  if (req.method === 'GET' && req.url.startsWith('/api/knowledge/card'))
    return handleGetKnowledgeCard(req, res);
  if (req.method === 'GET' && req.url.startsWith('/api/knowledge'))
    return handleGetAllKnowledge(req, res);

  // Native Mobile Push Relay (Android FCM HTTP v1 / iOS APNs HTTP/2)
  if (req.method === 'POST' && req.url === '/api/push/native')
    return handleNativePushSend(req, res);
  if (req.method === 'GET' && req.url === '/api/push/status')
    return handlePushStatus(req, res);

  // Health / status
  if (req.method === 'GET' && req.url === '/health')
    return json(res, 200, { status: 'ok', relay: 'active', auth: 'active' });

  // Default: relay health check (existing behavior)
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('Gun DB Relay Active\n');
}

// ── Start ──────────────────────────────────────────────────────

server.listen(RELAY_PORT, '0.0.0.0', () => {
  console.log(`[hub] Relay + Auth listening on port ${RELAY_PORT}`);
  console.log(`[hub] WebSocket: ws://0.0.0.0:${RELAY_PORT}/gun`);
  console.log(`[hub] POST /api/auth/google-link`);
  console.log(`[hub] POST /api/auth/apple-link`);
  console.log(`[hub] POST /api/auth/legacy-link`);
  console.log(`[hub] POST /api/auth/link`);
  console.log(`[hub] POST /api/auth/register`);
  console.log(`[hub] POST /api/auth/gateway-signin`);
  console.log(`[hub] POST /api/auth/delete-account`);
  console.log(`[hub] Gateway: ${GATEWAY_URL}`);
});
