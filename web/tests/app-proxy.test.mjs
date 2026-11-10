import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { HttpDrive } from '../src/lib/http-drive.js';
import {
    APP_PROXY_MIME,
    registerProxiedApp,
    getProxiedApp,
    buildAppProxyUrl,
    appDefToFileItem,
    mergeAppsIntoRootItems,
    setPortDrive,
    closeAppProxyDrivesForNode,
    discoverNodeApps,
    executeProxiedFetch
} from '../src/lib/app-proxy.js';
import { resolveAppIconName, resolveAppIconDataUri } from '../src/lib/app-icons.js';

function makeLocalSocketTransport(port) {
    return () => {
        let sock = null;
        return {
            get isOpen() {
                return !!sock && !sock.destroyed;
            },
            get bufferedAmount() {
                return sock && sock.writableLength > 0 ? sock.writableLength : 0;
            },
            async connect(onData, onClose) {
                sock = net.connect(port, '127.0.0.1');
                sock.on('data', (d) => onData(new Uint8Array(d)));
                sock.on('close', () => onClose());
                await new Promise((resolve, reject) => {
                    sock.once('connect', resolve);
                    sock.once('error', reject);
                });
            },
            send(bytes) {
                sock.write(Buffer.from(bytes));
            },
            close() {
                if (sock) sock.destroy();
                sock = null;
            }
        };
    };
}

async function testLiveAutopilotOverHttpDrive() {
    const drive = new HttpDrive({
        transportFactory: makeLocalSocketTransport(4000),
        maxConns: 4,
        isPrivateMode: true
    });

    // 1. Discover apps on live node
    const appsResp = await drive.fetch('/api/node/apps', {
        method: 'GET',
        headers: [['X-Client', 'svar']]
    });
    assert.equal(appsResp.status, 200);
    const { apps } = await appsResp.json();
    const ap = apps.find((a) => a.slug === 'autopilot');
    assert.ok(ap, 'Autopilot discovered in /api/node/apps');
    assert.equal(ap.port, 4000);
    assert.equal(ap.entryPath, '/autopilot');

    // 2. Verify shortcut item & placement next to Terminal and Chats
    const item = appDefToFileItem('testnode123', ap);
    assert.equal(item.mimeType, APP_PROXY_MIME);
    assert.equal(getProxiedApp('testnode123', 'autopilot').port, 4000);
    assert.equal(buildAppProxyUrl('testnode123', 'autopilot'), '/__app/testnode123/autopilot/autopilot');

    const baseRoot = [
        { id: '/beelink/Documents', name: 'Documents', type: 'folder', isDir: true },
        { id: '/beelink/Terminal', name: 'Terminal', mimeType: 'application/x-terminal' },
        { id: '/beelink/Chats', name: 'Chats', mimeType: 'application/x-chat' }
    ];
    const merged = mergeAppsIntoRootItems('/beelink', baseRoot, [item]);
    assert.equal(merged.length, 4);
    assert.equal(merged[3].name, 'Autopilot');
    assert.equal(merged[3].id, '/beelink/Autopilot');

    // 3. Tunnel GET /autopilot HTML & GET /autopilot/api/state over HttpDrive
    const htmlResp = await drive.fetch('/autopilot', {
        method: 'GET',
        headers: [['Host', '127.0.0.1:4000']]
    });
    assert.equal(htmlResp.status, 200);
    const html = await htmlResp.text();
    assert.ok(html.includes('Autopilot') || html.includes('/autopilot/api/state'), 'Autopilot UI HTML loaded');

    const stateResp = await drive.fetch('/autopilot/api/state', {
        method: 'GET',
        headers: [['Host', '127.0.0.1:4000']]
    });
    assert.equal(stateResp.status, 200);
    const stateJson = await stateResp.json();
    assert.equal(typeof stateJson, 'object');

    drive.close();
    console.log('ok live Autopilot discovery + HTML + API state over HttpDrive');
}

async function testHeadAndEmptyPostFraming() {
    const server = http.createServer((req, res) => {
        if (req.method === 'HEAD') {
            res.writeHead(200, { 'Content-Type': 'text/html', 'Content-Length': '12345' });
            res.end();
            return;
        }
        if (req.method === 'POST') {
            res.writeHead(302, {
                'Set-Cookie': ['_sess=abc123; Path=/; HttpOnly', 'theme=dark; Path=/'],
                Location: 'http://127.0.0.1/dashboard',
                'Content-Length': '0'
            });
            res.end();
            return;
        }
        res.writeHead(204);
        res.end();
    });
    await new Promise((r) => server.listen(0, '127.0.0.1', r));
    const port = server.address().port;

    const drive = new HttpDrive({
        transportFactory: makeLocalSocketTransport(port),
        maxConns: 2
    });

    const headRes = await drive.fetch('/page', { method: 'HEAD', headers: [['Host', `127.0.0.1:${port}`]] });
    assert.equal(headRes.status, 200);
    assert.equal(await headRes.text(), '');

    const postRes = await drive.fetch('/login', { method: 'POST', headers: [['Host', `127.0.0.1:${port}`]] });
    assert.equal(postRes.status, 302);
    const setCookies = postRes._rawHeaders.filter(([k]) => k.toLowerCase() === 'set-cookie');
    assert.equal(setCookies.length, 2);

    const noContentRes = await drive.fetch('/ping', { method: 'GET' });
    assert.equal(noContentRes.status, 204);

    drive.close();
    await new Promise((r) => server.close(r));
    console.log('ok HEAD, empty POST Content-Length: 0, multi Set-Cookie, and 204 framing');
}

async function testLiveGenVaultOverAppProxy() {
    const gvDir = '/home/alexey/projects/omd/genvault';
    const gvPy = path.join(gvDir, '.venv/bin/python');
    if (!fs.existsSync(gvPy)) {
        console.log('skip live GenVault test (genvault .venv not present)');
        return;
    }

    const tmpLib = fs.mkdtempSync(path.join(os.tmpdir(), 'omd-gv-proxy-'));
    // Pick a free ephemeral port for isolated test run
    const probeSrv = net.createServer();
    await new Promise((r) => probeSrv.listen(0, '127.0.0.1', r));
    const gvPort = probeSrv.address().port;
    await new Promise((r) => probeSrv.close(r));

    const proc = spawn(gvPy, ['serve.py'], {
        cwd: gvDir,
        env: { ...process.env, GENVAULT_ROOT: tmpLib, GENVAULT_PORT: String(gvPort) },
        stdio: 'ignore'
    });

    try {
        // Wait for GenVault /api/health
        let ready = false;
        for (let i = 0; i < 40; i++) {
            try {
                const res = await fetch(`http://127.0.0.1:${gvPort}/api/health`);
                if (res.ok) { ready = true; break; }
            } catch (_) {}
            await new Promise((r) => setTimeout(r, 150));
        }
        assert.ok(ready, 'GenVault test server booted');

        // 1. Generic config-driven /api/node/apps discovery + standard Bootstrap Icons
        assert.equal(resolveAppIconName('collection-play-fill'), 'collection-play-fill');
        assert.equal(resolveAppIconName('bi-diagram-3-fill'), 'diagram-3-fill');
        assert.equal(resolveAppIconName('unknown-custom-name'), 'window-stack');
        assert.ok(resolveAppIconDataUri('collection-play-fill', '#2996CC').startsWith('data:image/svg+xml'));

        const mockNodeApi = http.createServer((req, res) => {
            if (req.url === '/api/node/apps') {
                const body = JSON.stringify({
                    apps: [
                        {
                            slug: 'genvault',
                            name: 'GenVault',
                            port: gvPort,
                            entryPath: '/',
                            icon: 'collection-play-fill',
                            description: 'Local AI Media Vault'
                        }
                    ]
                });
                res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) });
                res.end(body);
                return;
            }
            res.writeHead(404);
            res.end();
        });
        await new Promise((r) => mockNodeApi.listen(0, '127.0.0.1', r));
        const nodeApiPort = mockNodeApi.address().port;

        const nodeApiDrive = new HttpDrive({
            transportFactory: makeLocalSocketTransport(nodeApiPort),
            maxConns: 2,
            isPrivateMode: true
        });
        const gvDrive = new HttpDrive({
            transportFactory: makeLocalSocketTransport(gvPort),
            maxConns: 4,
            isPrivateMode: true
        });
        setPortDrive('windesktop1', 4000, nodeApiDrive);
        setPortDrive('windesktop1', gvPort, gvDrive);

        const discovered = await discoverNodeApps('windesktop1', { force: true });
        await new Promise((r) => mockNodeApi.close(r));
        assert.equal(discovered.length, 1);
        assert.equal(discovered[0].name, 'GenVault');
        assert.equal(discovered[0].customAttributes.appSlug, 'genvault');
        assert.equal(discovered[0].customAttributes.icon, 'collection-play-fill');

        // 2. Fetch HTML index through executeProxiedFetch and verify bootstrap + relative app.js
        const htmlRes = await executeProxiedFetch({
            linkId: 'windesktop1',
            appSlug: 'genvault',
            path: '/__app/windesktop1/genvault/',
            method: 'GET'
        });
        assert.equal(htmlRes.status, 200);
        const htmlText = new TextDecoder().decode(htmlRes.bodyBuffer);
        assert.ok(htmlText.includes('REGISTER_APP_CLIENT'), 'App Proxy bootstrap injected');
        assert.ok(htmlText.includes('src="app.js"'), 'GenVault uses proxy-safe relative app.js');

        // 3. Fetch /api/config through executeProxiedFetch
        const cfgRes = await executeProxiedFetch({
            linkId: 'windesktop1',
            appSlug: 'genvault',
            path: '/api/config',
            method: 'GET'
        });
        assert.equal(cfgRes.status, 200);
        const cfgJson = JSON.parse(new TextDecoder().decode(cfgRes.bodyBuffer));
        assert.equal(cfgJson.platform, 'linux');
        assert.ok(Array.isArray(cfgJson.platforms));

        // 4. Multipart upload (POST /api/upload) of a >2.5 MB MP4 file + Range: bytes=0- chunk cap!
        const bigMp4Path = path.join(tmpLib, 'drop', 'bigtest.mp4');
        // Create a valid 2.6 MB dummy MP4 in drop folder or upload via multipart
        const payloadSize = Math.floor(2.6 * 1024 * 1024);
        const dummyBytes = Buffer.alloc(payloadSize, 0x42);
        const boundary = '----omdgvtestboundary';
        const headBuf = Buffer.from(
            `--${boundary}\r\nContent-Disposition: form-data; name="files"; filename="sample.mp4"\r\nContent-Type: video/mp4\r\n\r\n`
        );
        const tailBuf = Buffer.from(`\r\n--${boundary}--\r\n`);
        const multipartBody = Buffer.concat([headBuf, dummyBytes, tailBuf]);

        const upRes = await executeProxiedFetch({
            linkId: 'windesktop1',
            appSlug: 'genvault',
            path: '/api/upload',
            method: 'POST',
            headers: {
                'content-type': `multipart/form-data; boundary=${boundary}`,
                'content-length': String(multipartBody.byteLength)
            },
            bodyBuffer: multipartBody.buffer.slice(multipartBody.byteOffset, multipartBody.byteOffset + multipartBody.byteLength)
        });
        assert.equal(upRes.status, 200);
        const upJson = JSON.parse(new TextDecoder().decode(upRes.bodyBuffer));
        assert.equal(upJson.imported.length, 1);
        const mediaUrl = upJson.imported[0].media_url;

        // 5. Open-ended Range: bytes=0- request -> capped at 2 MB (2097152 bytes) 206 Partial Content!
        const rangeRes = await executeProxiedFetch({
            linkId: 'windesktop1',
            appSlug: 'genvault',
            path: mediaUrl,
            method: 'GET',
            headers: { range: 'bytes=0-' }
        });
        assert.equal(rangeRes.status, 206);
        assert.equal(rangeRes.bodyBuffer.byteLength, 2 * 1024 * 1024, 'Open-ended Range capped cleanly at 2 MB');
        assert.equal(
            rangeRes.headers['content-range'] || rangeRes.headers['Content-Range'],
            `bytes 0-${2 * 1024 * 1024 - 1}/${payloadSize}`
        );

        closeAppProxyDrivesForNode('windesktop1');
        console.log('ok live GenVault standalone discovery + HTML + API + 2.6MB multipart upload + 2MB Range streaming');
    } finally {
        proc.kill('SIGTERM');
        fs.rmSync(tmpLib, { recursive: true, force: true });
    }
}

(async () => {
    await testHeadAndEmptyPostFraming();
    await testLiveAutopilotOverHttpDrive();
    await testLiveGenVaultOverAppProxy();
    console.log('ALL APP-PROXY TESTS PASSED');
    process.exit(0);
})().catch((e) => {
    console.error('APP-PROXY TEST FAILURE:', e);
    process.exit(1);
});

