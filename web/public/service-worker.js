//OMD service worker
const translations = {
    en: {
        'file_event': 'File change notification',
        'call_event': 'Call invitation',
        'indexing_event': 'Indexing',
        'sign_request': 'Signature requested',
        'sign_completed': 'Signing completed',
        'file_updated': 'has updated',
        'file_removed': 'has removed',
        'external_user': 'External user',
        'indexing_started': 'Indexing started',
        'indexing_scompleted': 'Indexing completed',
    },
    sl: {
        'file_event': 'Sprememba datoteke',
        'call_event': 'Videoklic',
        'indexing_event': 'Indeksiranje',
        'sign_request': 'Zahtevek za podpis',
        'sign_completed': 'Podpisni postopek zaključen',
        'file_updated': 'je spremenil/a',
        'file_removed': 'je izbrisal/a',
        'external_user': 'Zunanji uporabnik',
        'indexing_started': 'Indeksiranje začeto',
        'indexing_scompleted': 'Indeksiranje zaključeno',
    },
    ru: {
        'file_event': 'Изменение файла',
        'call_event': 'Видеовстреча',
        'indexing_event': 'Индексирование',
        'sign_request': 'Запрос подписи',
        'sign_completed': 'Процесс подписи завершён',
        'file_updated': 'обновил/а',
        'file_removed': 'удалил/а',
        'external_user': 'Внешний пользователь',
        'indexing_started': 'Индексирование начато',
        'indexing_scompleted': 'Индексирование закончено',
    }
};

const swParams = new URL(self.location.href).searchParams;
var lang = swParams.get('lang') || 'en';
var authToken = swParams.get('token') || '';


const clientTokens = new Map();
const clientLangs = new Map();

console.log('[SW] Initialized with Token:', authToken ? 'Present' : 'Empty', 'Lang:', lang);

self.translate = function(key, clientId) {
    const activeLang = (clientId && clientLangs.get(clientId)) || lang;
    return translations[activeLang] && translations[activeLang][key] ? translations[activeLang][key] : '';
};


const CACHE_NAME = 'p2p-images-v11';
const APP_CLIENT_CACHE = 'omd-app-clients-v1';
const PRESERVED_CACHES = new Set([CACHE_NAME, 'omd-sites-v1', APP_CLIENT_CACHE]);

// Zero-Config P2P App Proxy client tracking
const hostClientIds = new Set();
const nonAppClientIds = new Set();
const appClients = new Map(); // clientId -> { linkId, appSlug }
const appHostByTarget = new Map(); // `${linkId}:${appSlug}` -> host clientId
let appClientsHydrated = false;
let appClientsHydratePromise = null;

function hydrateAppClientsOnce() {
    if (appClientsHydrated) return Promise.resolve();
    if (appClientsHydratePromise) return appClientsHydratePromise;
    appClientsHydratePromise = (async () => {
        try {
            const cache = await caches.open(APP_CLIENT_CACHE);
            const keys = await cache.keys();
            const prefix = `${self.location.origin}/__omd_app_client__/`;
            await Promise.all(keys.map(async (req) => {
                if (!req.url.startsWith(prefix)) return;
                const cid = decodeURIComponent(req.url.slice(prefix.length));
                if (!cid || hostClientIds.has(cid) || appClients.has(cid)) return;
                const res = await cache.match(req);
                if (!res) return;
                const parsed = await res.json();
                if (parsed && parsed.linkId && parsed.appSlug) {
                    appClients.set(cid, { linkId: parsed.linkId, appSlug: parsed.appSlug });
                }
            }));
        } catch (_) {}
        appClientsHydrated = true;
    })();
    return appClientsHydratePromise;
}

function parseAppProxyPath(pathname) {
    if (!pathname || !pathname.startsWith('/__app/')) return null;
    const rest = pathname.slice('/__app/'.length);
    const firstSlash = rest.indexOf('/');
    if (firstSlash <= 0) return null;
    const rawLink = rest.slice(0, firstSlash);
    const afterLink = rest.slice(firstSlash + 1);
    const secondSlash = afterLink.indexOf('/');
    const rawSlug = secondSlash >= 0 ? afterLink.slice(0, secondSlash) : afterLink;
    const realPath = secondSlash >= 0 ? afterLink.slice(secondSlash) : '/';
    if (!rawLink || !rawSlug) return null;
    try {
        return {
            linkId: decodeURIComponent(rawLink),
            appSlug: decodeURIComponent(rawSlug),
            realPath: realPath || '/'
        };
    } catch (_) {
        return { linkId: rawLink, appSlug: rawSlug, realPath: realPath || '/' };
    }
}

async function bindAppClient(clientId, binding) {
    if (!clientId || !binding || !binding.linkId || !binding.appSlug) return;
    nonAppClientIds.delete(clientId);
    hostClientIds.delete(clientId);
    appClients.set(clientId, { linkId: binding.linkId, appSlug: binding.appSlug });
    try {
        const cache = await caches.open(APP_CLIENT_CACHE);
        await cache.put(
            new Request(`${self.location.origin}/__omd_app_client__/${encodeURIComponent(clientId)}`),
            new Response(JSON.stringify({ linkId: binding.linkId, appSlug: binding.appSlug, ts: Date.now() }), {
                headers: { 'Content-Type': 'application/json' }
            })
        );
    } catch (_) {}
}

async function getAppClientBinding(clientId) {
    if (!clientId || hostClientIds.has(clientId) || nonAppClientIds.has(clientId)) return null;
    const mem = appClients.get(clientId);
    if (mem) return mem;
    try {
        const cache = await caches.open(APP_CLIENT_CACHE);
        const res = await cache.match(`${self.location.origin}/__omd_app_client__/${encodeURIComponent(clientId)}`);
        if (res) {
            const parsed = await res.json();
            if (parsed && parsed.linkId && parsed.appSlug) {
                const binding = { linkId: parsed.linkId, appSlug: parsed.appSlug };
                appClients.set(clientId, binding);
                return binding;
            }
        }
    } catch (_) {}
    nonAppClientIds.add(clientId);
    return null;
}

async function findHostWindowClient(excludeClientId, linkId = '', appSlug = '', skipIds = null) {
    const targetKey = (linkId && appSlug) ? `${linkId}:${String(appSlug).toLowerCase()}` : '';
    if (targetKey && appHostByTarget.has(targetKey)) {
        const preferredId = appHostByTarget.get(targetKey);
        if (preferredId && preferredId !== excludeClientId && !(skipIds && skipIds.has(preferredId))) {
            try {
                const pc = await self.clients.get(preferredId);
                if (pc) return pc;
                appHostByTarget.delete(targetKey);
                hostClientIds.delete(preferredId);
            } catch (_) {
                appHostByTarget.delete(targetKey);
                hostClientIds.delete(preferredId);
            }
        }
    }

    const candidates = [];
    const seenIds = new Set();
    // Iterate MRU-first (newest registered host window first)
    for (const hid of Array.from(hostClientIds).reverse()) {
        if (!hid || hid === excludeClientId || (skipIds && skipIds.has(hid))) continue;
        try {
            const c = await self.clients.get(hid);
            if (c) {
                seenIds.add(c.id);
                candidates.push(c);
            } else {
                hostClientIds.delete(hid);
            }
        } catch (_) {
            hostClientIds.delete(hid);
        }
    }

    const all = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
    for (const c of all) {
        if (!c || c.id === excludeClientId || seenIds.has(c.id) || (skipIds && skipIds.has(c.id))) continue;
        if (appClients.has(c.id)) continue;
        try {
            const cu = new URL(c.url);
            if (cu.pathname.startsWith('/__app/')) continue;
        } catch (_) {}
        candidates.push(c);
    }

    return candidates.find((c) => c.focused) || candidates.find((c) => c.visibilityState === 'visible') || candidates[0] || null;
}

function sendAppProxyRequestToHost(hostClient, payload, bodyBuffer) {
    return new Promise((resolve) => {
        const mc = new MessageChannel();
        const timer = setTimeout(() => {
            resolve({ error: 'P2P App Proxy request timed out', status: 504 });
        }, 125000);

        mc.port1.onmessage = (msgEvent) => {
            clearTimeout(timer);
            resolve(msgEvent.data || {});
        };

        const transfer = [mc.port2];
        if (bodyBuffer && bodyBuffer.byteLength > 0) {
            transfer.push(bodyBuffer);
        }
        hostClient.postMessage(payload, transfer);
    });
}

async function handleAppProxyFetch(event, linkId, appSlug, targetPathWithQuery) {
    const req = event.request;
    const method = (req.method || 'GET').toUpperCase();
    let rawBodyBytes = null;
    if (method !== 'GET' && method !== 'HEAD') {
        try {
            const ab = await req.arrayBuffer();
            if (ab && ab.byteLength > 0) {
                rawBodyBytes = new Uint8Array(ab);
            }
        } catch (_) {
            rawBodyBytes = null;
        }
    }

    const headers = {};
    req.headers.forEach((value, key) => {
        headers[key] = value;
    });

    const skipIds = new Set();
    let data = null;
    for (let attempt = 0; attempt < 3; attempt++) {
        const hostClient = await findHostWindowClient(event.clientId, linkId, appSlug, skipIds);
        if (!hostClient) {
            if (!data) {
                return new Response('Disk Direct P2P host window is not open. Please open this app from your node workspace.', {
                    status: 503,
                    headers: { 'Content-Type': 'text/plain; charset=utf-8' }
                });
            }
            break;
        }

        const bodyCopy = rawBodyBytes ? rawBodyBytes.slice().buffer : null;
        data = await sendAppProxyRequestToHost(hostClient, {
            type: 'APP_PROXY_FETCH',
            linkId,
            appSlug,
            path: targetPathWithQuery,
            method,
            headers,
            bodyBuffer: bodyCopy,
            mode: req.mode,
            referrer: req.referrer || ''
        }, bodyCopy);

        const errStr = data && data.error ? String(data.error) : '';
        if (errStr && errStr.includes('PeerConnections')) {
            skipIds.add(hostClient.id);
            hostClientIds.delete(hostClient.id);
            for (const [k, cid] of appHostByTarget.entries()) {
                if (cid === hostClient.id) appHostByTarget.delete(k);
            }
            continue;
        }
        break;
    }

    if (!data || data.error) {
        return new Response(String((data && data.error) || 'P2P App Proxy tunnel failed'), {
            status: (data && data.status) || 502,
            headers: { 'Content-Type': 'text/plain; charset=utf-8' }
        });
    }
    if (data.redirectUrl && (data.status === 301 || data.status === 302 || data.status === 303 || data.status === 307 || data.status === 308)) {
        const absRedirect = new URL(data.redirectUrl, self.location.origin).toString();
        return Response.redirect(absRedirect, data.status);
    }
    const status = data.status || 200;
    const resHeaders = new Headers(data.headers || {});
    const nullBody = method === 'HEAD' || status === 204 || status === 205 || status === 304;
    return new Response(nullBody ? null : (data.bodyBuffer || new ArrayBuffer(0)), {
        status,
        statusText: data.statusText || '',
        headers: resHeaders
    });
}

// Install Service Worker and activate immediately
self.addEventListener('install', (event) => {
    console.log('[SW] Service Worker Installing...');
    self.skipWaiting();
});

// Activate Service Worker and clean up old caches
self.addEventListener('activate', (event) => {
    console.log('[SW] Service Worker Activating...');
    event.waitUntil(
        caches.keys().then(keys => {
            return Promise.all(
                keys.map(key => {
                    if (!PRESERVED_CACHES.has(key)) {
                        console.log('[SW] Deleting stale cache:', key);
                        return caches.delete(key);
                    }
                })
            );
        }).then(() => self.clients.claim()).then(() => {
            return self.clients.matchAll().then(allClients => {
                allClients.forEach(client => {
                    client.postMessage({ type: 'gettoken' });
                });
            });
        })
    );
});

// Handle background push notifications & P2P Fetch
self.addEventListener('fetch', event => {
    const url = new URL(event.request.url);

    // Zero-Config P2P App Proxy: explicit /__app/<linkId>/<appSlug>/... routes
    if (url.origin === self.location.origin && url.pathname.startsWith('/__app/')) {
        const parsed = parseAppProxyPath(url.pathname);
        if (parsed) {
            event.respondWith((async () => {
                if (event.resultingClientId) {
                    await bindAppClient(event.resultingClientId, parsed);
                } else if (event.clientId && !hostClientIds.has(event.clientId) && event.request.mode === 'navigate') {
                    await bindAppClient(event.clientId, parsed);
                }
                return handleAppProxyFetch(event, parsed.linkId, parsed.appSlug, parsed.realPath + url.search);
            })());
            return;
        }
    }

    // Zero-Config P2P App Proxy: root-relative subresources & sub-navigations inside a bound app iframe
    if (
        url.origin === self.location.origin &&
        event.clientId &&
        !hostClientIds.has(event.clientId) &&
        !nonAppClientIds.has(event.clientId) &&
        url.pathname !== '/service-worker.js' &&
        !url.pathname.startsWith('/p2p/') &&
        !url.pathname.startsWith('/sites/')
    ) {
        const syncBinding = appClients.get(event.clientId);
        if (syncBinding) {
            event.respondWith((async () => {
                if (event.resultingClientId) {
                    await bindAppClient(event.resultingClientId, syncBinding);
                }
                return handleAppProxyFetch(event, syncBinding.linkId, syncBinding.appSlug, url.pathname + url.search);
            })());
            return;
        }
        // Cold SW wake-up fallback: hydrate persistent CacheStorage client map once, otherwise fall back to network
        if (!appClientsHydrated) {
            event.respondWith((async () => {
                await hydrateAppClientsOnce();
                const restored = appClients.get(event.clientId);
                if (restored) {
                    if (event.resultingClientId) {
                        await bindAppClient(event.resultingClientId, restored);
                    }
                    return handleAppProxyFetch(event, restored.linkId, restored.appSlug, url.pathname + url.search);
                }
                nonAppClientIds.add(event.clientId);
                return fetch(event.request);
            })());
            return;
        }
        nonAppClientIds.add(event.clientId);
    }

    //Static Jekyll sites: served P2P by the site loader page (site-view.html),
    //cached here. The loader reconnects the tunnel and refreshes the cache
    //when entries expire (they carry a 24h TTL).
    if (url.pathname === '/sites/' || /^\/sites\/[^\/]+(\/|$)/.test(url.pathname)) {
        event.respondWith((async () => {
            if (event.request.method !== 'GET') {
                return fetch(event.request);
            }
            const p2pUrl = new URL(event.request.url);
            p2pUrl.pathname = '/p2p' + p2pUrl.pathname;
            const cached = await caches.match(p2pUrl.toString());
            if (cached) {
                const dateStr = cached.headers.get('Date');
                const maxAge = parseInt(((cached.headers.get('Cache-Control') || '').match(/max-age=(\d+)/) || [])[1] || '0');
                if (!dateStr || !maxAge || (Date.now() - new Date(dateStr).getTime()) < maxAge * 1000) {
                    return cached;
                }
                //expired: drop and fall through to the loader
                const cache = await caches.open('omd-sites-v1');
                await cache.delete(p2pUrl.toString());
            }
            //first/expired visit: hand out the loader page; it reconnects the
            //tunnel, refreshes the cache and re-enters the site.
            return fetch('/site-view.html', { cache: 'no-store' });
        })());
        return;
    }

    if (url.pathname.startsWith('/p2p/')) {
        console.log('[SW] P2P Fetch intercepted:', event.request.url);
        event.respondWith((async () => {
            const isCacheable = url.pathname.startsWith('/p2p/sites/') || url.searchParams.has('cover') || url.searchParams.has('resize');
            const cacheUrl = new URL(event.request.url);
            cacheUrl.searchParams.delete('r');
            const cacheKey = cacheUrl.toString();
            
            if (isCacheable) {
                const cachedResponse = await caches.match(cacheKey);
                if (cachedResponse && cachedResponse.ok) {
                    const ct = cachedResponse.headers.get('content-type') || '';
                    if (ct.startsWith('image/') && !ct.includes('xcf')) {
                        console.log('[SW] Returning cached response for:', event.request.url);
                        return cachedResponse;
                    } else {
                        console.warn('[SW] Non-image or unrenderable format found in image cache, purging:', event.request.url, ct);
                        try {
                            const cache = await caches.open(CACHE_NAME);
                            await cache.delete(cacheKey);
                        } catch (_) {}
                    }
                }
            }

            return new Promise(async (resolve, reject) => {
                let client = null;
                if (event.clientId) {
                    client = await self.clients.get(event.clientId);
                }
                if (!client) {
                    const clientsList = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
                    console.log('[SW] Window clients found:', clientsList.length);
                    client = clientsList.find(c => c.focused) || clientsList.find(c => c.visibilityState === 'visible') || clientsList[0];
                }
                if (!client) {
                    const filename = url.pathname.split('/').pop();
                    if (url.pathname.includes('/generated/') && filename) {
                        try {
                            const size = url.searchParams.get('width') || url.searchParams.get('size');
                            const fbUrl = `/chat/image/${filename}` + (size ? `?size=${size}` : '');
                            const fbRes = await fetch(fbUrl);
                            if (fbRes.ok) {
                                if (isCacheable) {
                                    const cache = await caches.open(CACHE_NAME);
                                    cache.put(cacheKey, fbRes.clone()).catch(() => {});
                                }
                                return resolve(fbRes);
                            }
                        } catch (_) {}
                    }
                    if (isCacheable) {
                        try {
                            const rest = url.pathname.substring('/p2p/'.length);
                            const slashIdx = rest.indexOf('/');
                            const docPath = slashIdx >= 0 ? rest.substring(slashIdx) : rest;
                            const size = url.searchParams.get('width') || url.searchParams.get('size') || url.searchParams.get('cover') || 240;
                            const thumbFbUrl = `/api/device_memory_thumb?path=${encodeURIComponent(decodeURIComponent(docPath))}&size=${size}`;
                            console.log('[SW] No client available, trying thumb endpoint:', thumbFbUrl);
                            const fbRes = await fetch(thumbFbUrl);
                            if (fbRes.ok) {
                                const ct = fbRes.headers.get('content-type') || '';
                                if (ct.startsWith('image/') && !ct.includes('xcf')) {
                                    const cache = await caches.open(CACHE_NAME);
                                    cache.put(cacheKey, fbRes.clone()).catch(() => {});
                                    return resolve(fbRes);
                                }
                            }
                        } catch (_) {}
                    }
                    console.error('[SW] No window clients available to handle P2P_FETCH');
                    return resolve(new Response("No P2P client available", { status: 503 }));
                }
                
                const messageChannel = new MessageChannel();
                messageChannel.port1.onmessage = async (msgEvent) => {
                    const data = msgEvent.data;
                    console.log('[SW] Received response from client port:', data ? (data.error || `blob ${data.blob && data.blob.size} bytes`) : 'empty');
                    if (data.error) {
                        const isWriting = data.writing || data.status === 423 || (typeof data.error === 'string' && (data.error.includes('writing') || data.error.includes('locked')));
                        if (isWriting) {
                            console.log('[SW] File is currently being written, returning 423 without fallback or caching:', event.request.url);
                            return resolve(new Response("File is currently being written", {
                                status: 423,
                                statusText: "Locked",
                                headers: { 'Retry-After': '5' }
                            }));
                        }
                        const filename = url.pathname.split('/').pop();
                        if (url.pathname.includes('/generated/') && filename) {
                            try {
                                const size = url.searchParams.get('width') || url.searchParams.get('size');
                                const fbUrl = `/chat/image/${filename}` + (size ? `?size=${size}` : '');
                                console.log('[SW] P2P_FETCH failed, falling back to:', fbUrl);
                                const fbRes = await fetch(fbUrl);
                                if (fbRes.ok) {
                                    if (isCacheable) {
                                        const cache = await caches.open(CACHE_NAME);
                                        cache.put(cacheKey, fbRes.clone()).catch(() => {});
                                    }
                                    return resolve(fbRes);
                                }
                            } catch (_) {}
                        }
                        if (isCacheable) {
                            try {
                                const rest = url.pathname.substring('/p2p/'.length);
                                const slashIdx = rest.indexOf('/');
                                const docPath = slashIdx >= 0 ? rest.substring(slashIdx) : rest;
                                const size = url.searchParams.get('width') || url.searchParams.get('size') || url.searchParams.get('cover') || 240;
                                const thumbFbUrl = `/api/device_memory_thumb?path=${encodeURIComponent(decodeURIComponent(docPath))}&size=${size}`;
                                console.log('[SW] P2P_FETCH failed, falling back to thumb endpoint:', thumbFbUrl);
                                const fbRes = await fetch(thumbFbUrl);
                                if (fbRes.ok) {
                                    const ct = fbRes.headers.get('content-type') || '';
                                    if (ct.startsWith('image/') && !ct.includes('xcf')) {
                                        const cache = await caches.open(CACHE_NAME);
                                        cache.put(cacheKey, fbRes.clone()).catch(() => {});
                                        return resolve(fbRes);
                                    }
                                }
                            } catch (_) {}
                        }
                        resolve(new Response(data.error, { status: 500 }));
                    } else if (data.blob) {
                        const resHeaders = new Headers(data.headers || {});
                        if (!resHeaders.has('Cache-Control')) {
                            resHeaders.set('Cache-Control', 'public, max-age=86400, immutable');
                        }
                        
                        const status = data.status || 200;
                        const response = new Response(data.blob, {
                            status: status,
                            headers: resHeaders
                        });

                        const contentType = resHeaders.get('Content-Type') || '';
                        const isImage = contentType.startsWith('image/');
                        const isSiteFile = url.pathname.startsWith('/p2p/sites/');

                        if (isCacheable && status === 200 && data.blob.size > 0 && (isSiteFile || (isImage && !contentType.includes('xcf')))) {
                            const cache = await caches.open(isSiteFile ? 'omd-sites-v1' : CACHE_NAME);
                            cache.put(cacheKey, response.clone()).catch(() => {});
                        }

                        resolve(response);
                    } else {
                        resolve(new Response("Unknown response", { status: 500 }));
                    }
                };
                
                const headers = {};
                event.request.headers.forEach((value, key) => {
                    headers[key] = value;
                });

                console.log('[SW] Posting P2P_FETCH to window client...');
                client.postMessage({
                    type: 'P2P_FETCH',
                    url: event.request.url,
                    headers: headers
                }, [messageChannel.port2]);
            });
        })());
    }
});


self.addEventListener('notificationclick', function(event) {
    event.notification.close();
    event.waitUntil(
        clients.openWindow(event.notification.data.url || '/')
    );
});


self.addEventListener('message', function(event) {
    const clientId = event.source ? event.source.id : null;
    if (event.data.type === 'setlanguage') {
        lang = event.data.lang;
        if (clientId) {
            clientLangs.set(clientId, lang);
            console.log('[SW] Lang updated for client:', clientId, lang);
        }
    } else if (event.data.type === 'settoken') {
        const token = event.data.token;
        if (clientId) {
            clientTokens.set(clientId, token);
            console.log('[SW] Token updated for client:', clientId, token ? 'Present' : 'Empty');
        }
        // Fallback for requests without clientId (should not happen for media/API)
        // or for the first tab if it hasn't registered its clientId yet.
        if (!authToken || !clientId) {
            authToken = token;
        }
    } else if (event.data.type === 'claim') {
        // A page loaded without SW control (e.g. hard reload bypasses the SW,
        // so activate/clients.claim never ran for it). Re-claim it explicitly.
        self.clients.claim().then(() => {
            console.log('[SW] Reclaimed clients via claim message');
            if (event.source && event.source.id) {
                event.source.postMessage({ type: 'claimed' });
            }
        });
    } else if (event.data && event.data.type === 'REGISTER_APP_HOST') {
        if (clientId) {
            // Refresh MRU order so the most recently active host window is checked first
            hostClientIds.delete(clientId);
            hostClientIds.add(clientId);
            nonAppClientIds.add(clientId);
            appClients.delete(clientId);
            if (event.data.linkId && event.data.appSlug) {
                appHostByTarget.set(`${event.data.linkId}:${String(event.data.appSlug).toLowerCase()}`, clientId);
            }
        }
    } else if (event.data && event.data.type === 'REGISTER_APP_CLIENT') {
        if (clientId && event.data.linkId && event.data.appSlug) {
            bindAppClient(clientId, { linkId: event.data.linkId, appSlug: event.data.appSlug });
        }
    } else if (event.data && event.data.type === 'CLEAR_P2P_CACHE') {
        const urlPrefix = event.data.prefix || '';
        caches.keys().then(keys => {
            return Promise.all(keys.filter(k => k.startsWith('p2p-images')).map(async k => {
                if (!urlPrefix || urlPrefix === '/') {
                    return caches.delete(k);
                } else {
                    const c = await caches.open(k);
                    const reqs = await c.keys();
                    return Promise.all(reqs.map(r => {
                        if (r.url.includes(urlPrefix) || r.url.includes(encodeURI(urlPrefix))) {
                            return c.delete(r);
                        }
                    }));
                }
            }));
        }).then(() => {
            console.log('[SW] Cleared P2P cache for prefix:', urlPrefix || 'all');
        });
    }
});



// Handle background push notifications (supports both Custom Rich Actionable Push & legacy file/index events)
self.addEventListener('push', event => {
    let payload = {};
    try {
        payload = event.data ? event.data.json() : {};
    } catch (_) {
        payload = { body: event.data ? event.data.text() : '' };
    }
    console.log('[SW] Push received:', payload);

    event.waitUntil(
        clients.matchAll({
            type: 'window',
            includeUncontrolled: true
        }).then(clientList => {
            let isFocused = false;
            for (let i = 0; i < clientList.length; i++) {
                if (clientList[i].focused) {
                    isFocused = true;
                    break;
                }
            }

            const isCustomPush = (
                payload.type === 'custom_push' ||
                Array.isArray(payload.actions) ||
                Boolean(payload.image) ||
                (payload.body && !payload.event)
            );

            if (isCustomPush) {
                for (let i = 0; i < clientList.length; i++) {
                    try {
                        clientList[i].postMessage({ type: 'omd-custom-push', payload });
                    } catch (_) {}
                }
                if (payload.silentWhenFocused && isFocused) {
                    return null;
                }
                const title = payload.title || 'Disk Direct';
                const opts = {
                    body: payload.body || '',
                    icon: payload.icon || '/customtheme/onmydisk.svg',
                    badge: payload.badge || '/customtheme/onmydisk.svg',
                    requireInteraction: Boolean(payload.requireInteraction),
                    data: payload
                };
                if (payload.image) {
                    opts.image = payload.image;
                }
                if (payload.tag) {
                    opts.tag = payload.tag;
                    opts.renotify = true;
                }
                if (Array.isArray(payload.actions) && payload.actions.length > 0) {
                    opts.actions = payload.actions;
                }
                return self.registration.showNotification(title, opts);
            }

            // Legacy file/index push notifications (shown only when app is unfocused)
            if (!isFocused) {
                var message = '';
                var title = payload.title || 'Notification';
                var evName = payload.event || '';
                if (evName === 'file_updated') {
                    title = self.translate('file_event');
                    message = (self.translate(payload.user) || payload.user) + ' ' + (self.translate(evName) || evName) + ' ' + payload.path;
                } else if (evName === 'file_removed') {
                    title = self.translate('file_event');
                    message = (self.translate(payload.user) || payload.user) + ' ' + (self.translate(evName) || evName) + ' ' + payload.path;
                } else if (evName.startsWith('indexing')) {
                    title = self.translate('indexing_event');
                    message = (self.translate(evName) || evName) + ': ' + payload.path;
                } else {
                    message = self.translate(evName) || evName || payload.body || '';
                }

                const notificationOptions = {
                    body: message || 'You have a new message.',
                    icon: payload.icon || '/customtheme/onmydisk.svg',
                    data: {
                        url: payload.path || '/'
                    }
                };

                return self.registration.showNotification(title, notificationOptions);
            }
            return null;
        })
    );
});

self.addEventListener('notificationclick', event => {
    event.notification.close();
    const payload = (event.notification && event.notification.data) || {};
    const action = event.action || '';
    const extra = payload.data || {};

    event.waitUntil((async () => {
        const clientList = await clients.matchAll({
            type: 'window',
            includeUncontrolled: true
        });

        // Custom actionable push from a connected node extension
        if (payload.type === 'custom_push' || extra.widget_url || extra.action_url) {
            const act = action || 'open';
            const params = new URLSearchParams();
            params.set('omd_push_action', act);
            if (extra.widget_url) params.set('widget_url', extra.widget_url);
            if (extra.action_url) params.set('action_url', extra.action_url);
            if (extra.item_id) params.set('item_id', extra.item_id);
            if (extra.action_token) params.set('token', extra.action_token);
            if (extra.node_link_id) params.set('node_link_id', extra.node_link_id);
            if (extra.node_name) params.set('node_name', extra.node_name);
            if (extra.default_storage) params.set('storage', extra.default_storage);
            if (extra.filename) params.set('file', extra.filename);
            const targetUrl = '/?' + params.toString();

            for (let i = 0; i < clientList.length; i++) {
                const client = clientList[i];
                if (client && 'focus' in client) {
                    try { await client.focus(); } catch (_) {}
                    try {
                        client.postMessage({
                            type: 'omd-push-click',
                            action: act,
                            payload
                        });
                        return;
                    } catch (_) {}
                }
            }
            if (clients.openWindow) {
                return clients.openWindow(targetUrl);
            }
            return;
        }

        // Standard notification URL navigation
        const destUrl = payload.url || '/';
        for (let i = 0; i < clientList.length; i++) {
            const client = clientList[i];
            if (client && 'focus' in client) {
                try { return await client.focus(); } catch (_) {}
            }
        }
        if (clients.openWindow) {
            return clients.openWindow(destUrl);
        }
    })());
});
