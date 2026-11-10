import { HttpDrive } from './http-drive.js';

export const APP_PROXY_MIME = 'application/x-web-app';
const REGISTRY_STORAGE_KEY = 'omd_proxied_apps_v1';
const COOKIE_STORAGE_PREFIX = 'omd_app_cookies_v1:';
// Cap open-ended Range: bytes=N- requests to 2 MB chunks so <video>/<audio>
// players start and seek immediately over WebRTC DataChannels without buffering
// entire multi-megabyte media files in memory.
const MAX_OPEN_RANGE_BYTES = 2 * 1024 * 1024;

// `${linkId}:${slug}` -> { linkId, slug, name, port, entryPath, icon, description }
const proxiedApps = new Map();
// `${linkId}:${port}` -> HttpDrive
const portDrives = new Map();
// linkId -> Array<AppItem>
const discoveredAppsCache = new Map();
// linkId -> Promise<Array<AppItem>>
const inFlightDiscovery = new Map();

let swBridgeInitialized = false;

function hydrateRegistryFromSession() {
    if (typeof sessionStorage === 'undefined') return;
    try {
        const raw = sessionStorage.getItem(REGISTRY_STORAGE_KEY);
        if (!raw) return;
        const parsed = JSON.parse(raw);
        if (Array.isArray(parsed)) {
            for (const entry of parsed) {
                if (entry && entry.linkId && entry.slug && entry.port > 0) {
                    proxiedApps.set(`${entry.linkId}:${entry.slug}`, entry);
                }
            }
        }
    } catch (_) {}
}

function persistRegistryToSession() {
    if (typeof sessionStorage === 'undefined') return;
    try {
        sessionStorage.setItem(REGISTRY_STORAGE_KEY, JSON.stringify(Array.from(proxiedApps.values())));
    } catch (_) {}
}

hydrateRegistryFromSession();

export function registerProxiedApp(linkId, appDef) {
    if (!linkId || !appDef || !appDef.slug) return null;
    const slug = String(appDef.slug).trim().toLowerCase();
    const port = parseInt(appDef.port, 10);
    if (!slug || isNaN(port) || port <= 0) return null;
    let entryPath = String(appDef.entryPath || '/').trim() || '/';
    if (!entryPath.startsWith('/')) entryPath = '/' + entryPath;
    const normalized = {
        linkId: String(linkId),
        slug,
        name: String(appDef.name || slug.charAt(0).toUpperCase() + slug.slice(1)),
        port,
        entryPath,
        icon: String(appDef.icon || slug),
        description: String(appDef.description || '')
    };
    proxiedApps.set(`${linkId}:${slug}`, normalized);
    persistRegistryToSession();
    return normalized;
}

export function getProxiedApp(linkId, slug) {
    if (!linkId || !slug) return null;
    const key = `${linkId}:${String(slug).trim().toLowerCase()}`;
    return proxiedApps.get(key) || null;
}

export function buildAppProxyUrl(linkId, slug, subPath = null) {
    const app = getProxiedApp(linkId, slug);
    const effPath = subPath != null ? subPath : (app ? app.entryPath : '/');
    const cleanPath = effPath.startsWith('/') ? effPath : '/' + effPath;
    return `/__app/${encodeURIComponent(linkId)}/${encodeURIComponent(slug)}${cleanPath}`;
}

export function getPortDrive(linkId, port) {
    const p = parseInt(port, 10);
    const key = `${linkId}:${p}`;
    let drive = portDrives.get(key);
    if (drive && !drive.closed) return drive;
    drive = new HttpDrive({
        targetLinkId: linkId,
        port: p,
        maxConns: 6,
        connectTimeoutMs: 25000,
        headerTimeoutMs: 120000,
        isPrivateMode: true
    });
    portDrives.set(key, drive);
    return drive;
}

export function setPortDrive(linkId, port, drive) {
    portDrives.set(`${linkId}:${parseInt(port, 10)}`, drive);
}

export function closePortDrive(linkId, port) {
    const key = `${linkId}:${parseInt(port, 10)}`;
    const drive = portDrives.get(key);
    if (drive) {
        try { drive.close(); } catch (_) {}
        portDrives.delete(key);
    }
}

export function closeAppProxyDrivesForNode(linkId) {
    for (const [key, drive] of portDrives.entries()) {
        if (key.startsWith(`${linkId}:`)) {
            try { drive.close(); } catch (_) {}
            portDrives.delete(key);
        }
    }
    discoveredAppsCache.delete(linkId);
}

// ── Isolated Per-(linkId, slug) Cookie Jar ──────────────────────────────────

function loadCookieJar(linkId, slug) {
    if (typeof sessionStorage === 'undefined') return {};
    try {
        const raw = sessionStorage.getItem(`${COOKIE_STORAGE_PREFIX}${linkId}:${slug}`);
        return raw ? JSON.parse(raw) : {};
    } catch (_) {
        return {};
    }
}

function saveCookieJar(linkId, slug, jar) {
    if (typeof sessionStorage === 'undefined') return;
    try {
        sessionStorage.setItem(`${COOKIE_STORAGE_PREFIX}${linkId}:${slug}`, JSON.stringify(jar));
    } catch (_) {}
}

function storeSetCookieHeaders(linkId, slug, rawHeaders) {
    if (!Array.isArray(rawHeaders) || rawHeaders.length === 0) return;
    let jar = null;
    let mutated = false;
    const now = Date.now();

    for (const [k, v] of rawHeaders) {
        if (!k || k.toLowerCase() !== 'set-cookie' || !v) continue;
        if (!jar) jar = loadCookieJar(linkId, slug);
        const parts = String(v).split(';').map((s) => s.trim());
        const firstEq = parts[0].indexOf('=');
        if (firstEq <= 0) continue;
        const cName = parts[0].slice(0, firstEq).trim();
        const cVal = parts[0].slice(firstEq + 1);
        if (!cName) continue;

        let expiresAt = null;
        let cookiePath = '/';
        let deleted = cVal === '' || cVal === 'deleted';

        for (let i = 1; i < parts.length; i++) {
            const attr = parts[i];
            const eq = attr.indexOf('=');
            const ak = (eq >= 0 ? attr.slice(0, eq) : attr).trim().toLowerCase();
            const av = eq >= 0 ? attr.slice(eq + 1).trim() : '';
            if (ak === 'max-age') {
                const secs = parseInt(av, 10);
                if (!isNaN(secs)) {
                    if (secs <= 0) deleted = true;
                    else expiresAt = now + secs * 1000;
                }
            } else if (ak === 'expires' && expiresAt === null) {
                const ts = Date.parse(av);
                if (!isNaN(ts)) {
                    if (ts <= now) deleted = true;
                    else expiresAt = ts;
                }
            } else if (ak === 'path' && av) {
                cookiePath = av.startsWith('/') ? av : '/' + av;
            }
        }

        if (deleted) {
            delete jar[cName];
        } else {
            jar[cName] = { value: cVal, path: cookiePath, expiresAt };
        }
        mutated = true;
    }

    if (mutated && jar) {
        saveCookieJar(linkId, slug, jar);
    }
}

function getCookieHeaderForPath(linkId, slug, reqPath) {
    const jar = loadCookieJar(linkId, slug);
    const entries = Object.entries(jar);
    if (entries.length === 0) return '';
    const now = Date.now();
    const cleanPath = (reqPath || '/').split('?')[0] || '/';
    const pairs = [];
    let expiredFound = false;

    for (const [name, meta] of entries) {
        if (!meta || typeof meta !== 'object') continue;
        if (meta.expiresAt && meta.expiresAt <= now) {
            delete jar[name];
            expiredFound = true;
            continue;
        }
        const cPath = meta.path || '/';
        if (cleanPath === cPath || cleanPath.startsWith(cPath.endsWith('/') ? cPath : cPath + '/') || cPath === '/') {
            pairs.push(`${name}=${meta.value}`);
        }
    }
    if (expiredFound) saveCookieJar(linkId, slug, jar);
    return pairs.join('; ');
}

// ── Header Rewriting & Early HTML Bootstrap Injection ───────────────────────

const DROPPED_REQ_HEADERS = new Set([
    'host',
    'connection',
    'keep-alive',
    'accept-encoding',
    'content-length',
    'transfer-encoding',
    'cookie',
    'origin',
    'referer',
    'sec-fetch-dest',
    'sec-fetch-mode',
    'sec-fetch-site',
    'sec-fetch-user',
    'upgrade-insecure-requests'
]);

const DROPPED_RES_HEADERS = new Set([
    'set-cookie',
    'x-frame-options',
    'content-length',
    'transfer-encoding',
    'connection',
    'keep-alive'
]);

function sanitizeCsp(cspValue) {
    if (!cspValue) return '';
    return String(cspValue)
        .split(';')
        .map((d) => d.trim())
        .filter((d) => d && !d.toLowerCase().startsWith('frame-ancestors'))
        .join('; ');
}

function normalizeProxySubPath(rawPath, linkId, slug) {
    const prefix = `/__app/${encodeURIComponent(linkId)}/${encodeURIComponent(slug)}`;
    const rawPrefix = `/__app/${linkId}/${slug}`;
    let p = rawPath || '/';
    if (p.startsWith(prefix)) {
        p = p.slice(prefix.length) || '/';
    } else if (p.startsWith(rawPrefix)) {
        p = p.slice(rawPrefix.length) || '/';
    }
    if (!p.startsWith('/')) p = '/' + p;
    return p;
}

function rewriteLocationHeader(locationVal, linkId, slug, port, currentRealPath) {
    if (!locationVal) return null;
    const prefix = `/__app/${encodeURIComponent(linkId)}/${encodeURIComponent(slug)}`;
    const trimmed = String(locationVal).trim();

    try {
        if (/^https?:\/\//i.test(trimmed)) {
            const u = new URL(trimmed);
            const isLocalAppOrigin =
                ((u.hostname === '127.0.0.1' || u.hostname === 'localhost' || u.hostname === '0.0.0.0') &&
                    (u.port === String(port) || u.port === '')) ||
                (typeof window !== 'undefined' && u.host === window.location.host);
            if (!isLocalAppOrigin) {
                return trimmed; // External OAuth/link redirect stays untouched
            }
            const sub = normalizeProxySubPath(u.pathname, linkId, slug);
            return `${prefix}${sub}${u.search}${u.hash}`;
        }
        const resolved = new URL(trimmed, `http://127.0.0.1:${port}${currentRealPath || '/'}`);
        const sub = normalizeProxySubPath(resolved.pathname, linkId, slug);
        return `${prefix}${sub}${resolved.search}${resolved.hash}`;
    } catch (_) {
        return trimmed.startsWith('/') ? `${prefix}${trimmed}` : `${prefix}/${trimmed}`;
    }
}

function injectAppProxyBootstrap(htmlText, linkId, slug, realPathWithQuery) {
    const safeLinkId = JSON.stringify(String(linkId));
    const safeSlug = JSON.stringify(String(slug));
    const safeRealPath = JSON.stringify(String(realPathWithQuery || '/'));
    const prefix = `/__app/${encodeURIComponent(linkId)}/${encodeURIComponent(slug)}`;
    const safePrefix = JSON.stringify(prefix);

    const snippet = `<script data-omd-app-proxy="1">(function(){` +
        `var L=${safeLinkId},S=${safeSlug},P=${safePrefix},R=${safeRealPath};` +
        `function reg(){try{if(navigator.serviceWorker&&navigator.serviceWorker.controller){` +
        `navigator.serviceWorker.controller.postMessage({type:"REGISTER_APP_CLIENT",linkId:L,appSlug:S});` +
        `}}catch(e){}}` +
        `reg();` +
        `if(navigator.serviceWorker){try{navigator.serviceWorker.addEventListener("controllerchange",reg);}catch(e){}}` +
        `try{if(location.pathname.indexOf("/__app/")===0){` +
        `var target=R+(location.hash||"");` +
        `history.replaceState(history.state,"",target);` +
        `}}catch(e){}` +
        `function notify(){try{if(window.parent&&window.parent!==window){` +
        `window.parent.postMessage({type:"OMD_APP_PROXY_STATE",linkId:L,appSlug:S,path:location.pathname+location.search+location.hash,title:document.title||""},"*");` +
        `}}catch(e){}}` +
        `try{var ps=history.pushState,rs=history.replaceState;` +
        `history.pushState=function(){var r=ps.apply(this,arguments);notify();return r;};` +
        `history.replaceState=function(){var r=rs.apply(this,arguments);notify();return r;};` +
        `window.addEventListener("popstate",notify);` +
        `window.addEventListener("DOMContentLoaded",notify);` +
        `window.addEventListener("load",notify);` +
        `}catch(e){}` +
        `})();</script>`;

    const headMatch = /<head(\s[^>]*)?>/i.exec(htmlText);
    if (headMatch) {
        const insertIdx = headMatch.index + headMatch[0].length;
        return htmlText.slice(0, insertIdx) + snippet + htmlText.slice(insertIdx);
    }
    if (/<!doctype\s+html[^>]*>/i.test(htmlText)) {
        return htmlText.replace(/<!doctype\s+html[^>]*>/i, (m) => m + snippet);
    }
    return snippet + htmlText;
}

// ── Core Proxy Request Handler ──────────────────────────────────────────────

export async function executeProxiedFetch(reqData) {
    const { linkId, appSlug, path, method = 'GET', headers = {}, bodyBuffer = null, referrer = '' } = reqData;
    let app = getProxiedApp(linkId, appSlug);
    if (!app) {
        // Attempt on-demand node discovery if tab reloaded directly on an app URL
        try {
            await discoverNodeApps(linkId, { force: true });
            app = getProxiedApp(linkId, appSlug);
        } catch (_) {}
    }
    if (!app) {
        return {
            status: 404,
            statusText: 'App Not Registered',
            headers: { 'Content-Type': 'text/plain; charset=utf-8' },
            bodyBuffer: new TextEncoder().encode(`App "${appSlug}" is not available on this node.`).buffer
        };
    }

    const realPath = normalizeProxySubPath(path || '/', linkId, appSlug);
    const localOrigin = `http://127.0.0.1:${app.port}`;
    const outHeaders = [['Host', `127.0.0.1:${app.port}`]];

    for (const [k, v] of Object.entries(headers || {})) {
        const kl = k.toLowerCase();
        if (DROPPED_REQ_HEADERS.has(kl)) continue;
        if (kl.startsWith('x-omd-')) continue;
        let headerVal = String(v);
        if (kl === 'range') {
            const openRange = /^bytes=(\d+)-\s*$/i.exec(headerVal);
            if (openRange) {
                const start = parseInt(openRange[1], 10);
                if (!isNaN(start) && start >= 0) {
                    headerVal = `bytes=${start}-${start + MAX_OPEN_RANGE_BYTES - 1}`;
                }
            }
        }
        outHeaders.push([k, headerVal]);
    }

    const upperMethod = String(method || 'GET').toUpperCase();
    if (upperMethod !== 'GET' && upperMethod !== 'HEAD') {
        outHeaders.push(['Origin', localOrigin]);
    }
    let refSubPath = realPath;
    if (referrer) {
        try {
            const ru = new URL(referrer);
            refSubPath = normalizeProxySubPath(ru.pathname + ru.search, linkId, appSlug);
        } catch (_) {}
    }
    outHeaders.push(['Referer', `${localOrigin}${refSubPath}`]);

    const cookieStr = getCookieHeaderForPath(linkId, appSlug, realPath);
    if (cookieStr) {
        outHeaders.push(['Cookie', cookieStr]);
    }

    const drive = getPortDrive(linkId, app.port);
    const resp = await drive.fetch(realPath, {
        method: upperMethod,
        headers: outHeaders,
        body: bodyBuffer && bodyBuffer.byteLength > 0 ? bodyBuffer : null
    });

    const rawHeaders = resp._rawHeaders || [];
    storeSetCookieHeaders(linkId, appSlug, rawHeaders);

    if (resp.status === 301 || resp.status === 302 || resp.status === 303 || resp.status === 307 || resp.status === 308) {
        const loc = resp.headers.get('location');
        try { await resp.arrayBuffer(); } catch (_) {}
        if (loc) {
            const rewritten = rewriteLocationHeader(loc, linkId, appSlug, app.port, realPath);
            return {
                status: resp.status,
                statusText: resp.statusText || 'Found',
                redirectUrl: rewritten,
                headers: {}
            };
        }
    }

    const cleanHeaders = {};
    resp.headers.forEach((val, key) => {
        const kl = key.toLowerCase();
        if (DROPPED_RES_HEADERS.has(kl)) return;
        if (kl === 'content-security-policy') {
            const cleaned = sanitizeCsp(val);
            if (cleaned) cleanHeaders['Content-Security-Policy'] = cleaned;
            return;
        }
        cleanHeaders[key] = val;
    });
    cleanHeaders['Cache-Control'] = cleanHeaders['Cache-Control'] || 'no-store';

    if (upperMethod === 'HEAD' || resp.status === 204 || resp.status === 205 || resp.status === 304) {
        return {
            status: resp.status,
            statusText: resp.statusText || '',
            headers: cleanHeaders,
            bodyBuffer: null
        };
    }

    const contentType = (resp.headers.get('content-type') || '').toLowerCase();
    if (contentType.includes('text/html')) {
        const htmlText = await resp.text();
        const injected = injectAppProxyBootstrap(htmlText, linkId, appSlug, realPath);
        const encoded = new TextEncoder().encode(injected);
        return {
            status: resp.status,
            statusText: resp.statusText || 'OK',
            headers: cleanHeaders,
            bodyBuffer: encoded.buffer
        };
    }

    const ab = await resp.arrayBuffer();
    return {
        status: resp.status,
        statusText: resp.statusText || 'OK',
        headers: cleanHeaders,
        bodyBuffer: ab
    };
}

// ── Service Worker Bridge (Host Window Side) ────────────────────────────────

function notifySwHostReady() {
    if (typeof navigator === 'undefined' || !navigator.serviceWorker) return;
    // Never register an embedded /__app/ iframe itself as a host window
    if (typeof window !== 'undefined' && window.location.pathname.startsWith('/__app/')) return;
    const post = (sw) => {
        if (!sw) return;
        try { sw.postMessage({ type: 'REGISTER_APP_HOST' }); } catch (_) {}
    };
    if (navigator.serviceWorker.controller) {
        post(navigator.serviceWorker.controller);
    }
    navigator.serviceWorker.ready
        .then((reg) => post(reg && reg.active))
        .catch(() => {});
}

export function initAppProxyServiceWorkerBridge() {
    if (swBridgeInitialized) return;
    if (typeof navigator === 'undefined' || !navigator.serviceWorker) return;
    if (typeof window !== 'undefined' && window.location.pathname.startsWith('/__app/')) return;
    swBridgeInitialized = true;

    notifySwHostReady();
    navigator.serviceWorker.addEventListener('controllerchange', notifySwHostReady);

    navigator.serviceWorker.addEventListener('message', async (event) => {
        const data = event.data;
        if (!data || data.type !== 'APP_PROXY_FETCH') return;
        const replyPort = event.ports && event.ports[0];
        if (!replyPort) return;

        try {
            const res = await executeProxiedFetch(data);
            const transfer = res.bodyBuffer instanceof ArrayBuffer ? [res.bodyBuffer] : [];
            replyPort.postMessage(res, transfer);
        } catch (err) {
            console.error('[AppProxy] P2P fetch error:', data.appSlug, data.path, err);
            replyPort.postMessage({
                error: (err && err.message) ? err.message : 'P2P App Proxy tunnel failed',
                status: 502
            });
        }
    });
}

// ── Node Root Discovery & Item Formatting ───────────────────────────────────

export function appDefToFileItem(linkId, appDef) {
    const registered = registerProxiedApp(linkId, appDef);
    if (!registered) return null;
    const safeDisplayName = registered.name.replace(/\//g, '-');
    return {
        id: `/${safeDisplayName}`,
        value: registered.name,
        name: registered.name,
        type: 'code',
        itemType: 'app',
        isDir: false,
        lazy: false,
        ext: '',
        size: 0,
        date: new Date(),
        mimeType: APP_PROXY_MIME,
        customAttributes: {
            uiAction: 'web-app',
            appSlug: registered.slug,
            appName: registered.name,
            port: registered.port,
            entryPath: registered.entryPath,
            icon: registered.icon,
            description: registered.description
        }
    };
}

export function getCachedNodeAppItems(linkId) {
    return discoveredAppsCache.get(linkId) || null;
}

export async function discoverNodeApps(linkId, { force = false } = {}) {
    if (!linkId) return [];
    if (!force && discoveredAppsCache.has(linkId)) {
        return discoveredAppsCache.get(linkId);
    }
    if (!force && inFlightDiscovery.has(linkId)) {
        return inFlightDiscovery.get(linkId);
    }

    const run = (async () => {
        try {
            const drive = getPortDrive(linkId, 4000);
            const resp = await drive.fetch('/api/node/apps', {
                method: 'GET',
                headers: [['X-Client', 'svar']],
                connectTimeoutMs: 12000,
                timeoutMs: 12000
            });
            if (!resp.ok) {
                discoveredAppsCache.set(linkId, []);
                return [];
            }
            const payload = await resp.json();
            const rawApps = Array.isArray(payload?.apps) ? payload.apps : [];
            const items = rawApps
                .map((a) => appDefToFileItem(linkId, a))
                .filter(Boolean);
            discoveredAppsCache.set(linkId, items);
            return items;
        } catch (err) {
            closePortDrive(linkId, 4000);
            console.debug('[AppProxy] Node app discovery skipped for', linkId, err?.message || err);
            discoveredAppsCache.set(linkId, []);
            return [];
        } finally {
            inFlightDiscovery.delete(linkId);
        }
    })();

    inFlightDiscovery.set(linkId, run);
    return run;
}

export function mergeAppsIntoRootItems(devicePrefix, rootItems, appItems) {
    const base = Array.isArray(rootItems) ? rootItems : [];
    if (!Array.isArray(appItems) || appItems.length === 0) return base;

    const cleanPrefix = devicePrefix || '';
    const existingSlugs = new Set();
    for (const item of base) {
        if (!item) continue;
        if (item.mimeType === APP_PROXY_MIME && item.customAttributes?.appSlug) {
            existingSlugs.add(String(item.customAttributes.appSlug).toLowerCase());
        }
    }

    const toInsert = [];
    for (const appItem of appItems) {
        const slug = String(appItem.customAttributes?.appSlug || '').toLowerCase();
        if (!slug || existingSlugs.has(slug)) continue;
        existingSlugs.add(slug);
        let candidateId = cleanPrefix + appItem.id;
        if (base.some((f) => f && f.id === candidateId && f.mimeType !== APP_PROXY_MIME)) {
            candidateId = `${cleanPrefix}${appItem.id} (App)`;
        }
        toInsert.push({
            ...appItem,
            id: candidateId,
            parent: cleanPrefix || '/'
        });
    }

    if (toInsert.length === 0) return base;

    // Place proxied web apps right next to Terminal and Chats at the root of the node
    const withoutOld = base.filter((f) => !(f && f.mimeType === APP_PROXY_MIME && toInsert.some((n) => n.customAttributes?.appSlug === f.customAttributes?.appSlug)));
    let lastBuiltinAppIdx = -1;
    for (let i = 0; i < withoutOld.length; i++) {
        const m = withoutOld[i]?.mimeType;
        if (m === 'application/x-terminal' || m === 'application/x-chat' || m === APP_PROXY_MIME) {
            lastBuiltinAppIdx = i;
        }
    }
    if (lastBuiltinAppIdx >= 0) {
        return [
            ...withoutOld.slice(0, lastBuiltinAppIdx + 1),
            ...toInsert,
            ...withoutOld.slice(lastBuiltinAppIdx + 1)
        ];
    }
    return [...withoutOld, ...toInsert];
}

