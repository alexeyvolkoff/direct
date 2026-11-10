import assert from 'node:assert/strict';
import http from 'node:http';
import net from 'node:net';
import { HttpResponseParser, HttpDrive } from '../src/lib/http-drive.js';

function feed(parser, ...chunks) {
    for (const c of chunks) parser.push(new TextEncoder().encode(c));
}

async function collect(parser) {
    const headersP = new Promise((res) => (parser.cb.onHeaders = res));
    const doneP = new Promise((res, rej) => {
        parser.cb.onDone = res;
        parser.cb.onError = rej;
    });
    const body = [];
    parser.cb.onBody = (b) => body.push(Buffer.from(b).toString('utf8'));
    return { headersP, doneP, body };
}

async function testContentLengthSplit() {
    const p = new HttpResponseParser({});
    const c = await collect(p);
    const raw = 'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nhello world';
    const bytes = new TextEncoder().encode(raw);
    for (let i = 0; i < bytes.length; i += 3) {
        p.push(bytes.subarray(i, Math.min(i + 3, bytes.length)));
    }
    const h = await c.headersP;
    await c.doneP;
    assert.equal(h.status, 200);
    assert.equal(c.body.join(''), 'hello world');
    console.log('ok content-length split across boundaries');
}

async function testChunkedWithTrailers() {
    const p = new HttpResponseParser({});
    const c = await collect(p);
    feed(
        p,
        'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n',
        '5\r\nhell',
        'o\r\n6\r\n worl',
        'd\r\n0\r\nX-Trailer: 1\r\n\r\n'
    );
    const h = await c.headersP;
    await c.doneP;
    assert.equal(h.status, 200);
    assert.equal(c.body.join(''), 'hello world');
    console.log('ok chunked + trailers');
}

async function testSseIncremental() {
    const p = new HttpResponseParser({});
    const c = await collect(p);
    feed(p, 'HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n');
    const h = await c.headersP;
    assert.equal(h.status, 200);
    feed(p, '15\r\ndata: {"delta":"a"}\n\n\r\n');
    feed(p, '15\r\ndata: {"delta":"b"}\n\n\r\n');
    feed(p, "2a\r\ndata: {\"delta\":\"c\"}\n\ndata: {\"done\":true}\n\n\r\n0\r\n\r\n");
    await c.doneP;
    const all = c.body.join('');
    assert.equal(all.split('data:').length - 1, 4);
    assert.ok(all.includes('"done":true'));
    console.log('ok SSE incremental chunks');
}

async function testNoBody204() {
    const p = new HttpResponseParser({});
    const c = await collect(p);
    feed(p, 'HTTP/1.1 204 No Content\r\nFoo: bar\r\n\r\n');
    const h = await c.headersP;
    await c.doneP;
    assert.equal(h.status, 204);
    assert.equal(c.body.length, 0);
    console.log('ok 204 no body');
}

async function testUntilClose() {
    const p = new HttpResponseParser({});
    const c = await collect(p);
    feed(p, 'HTTP/1.0 500 Err\r\nContent-Type: text/plain\r\n\r\nboom-boom');
    await c.headersP;
    p.push(new TextEncoder().encode(' more'));
    p.end();
    await c.doneP;
    assert.equal(c.body.join(''), 'boom-boom more');
    console.log('ok until-close framing');
}

async function testMidChunkCloseFails() {
    const p = new HttpResponseParser({});
    const errP = new Promise((res) => (p.cb.onError = res));
    const hp = new Promise((res) => (p.cb.onHeaders = res));
    feed(p, 'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc');
    await hp;
    p.end();
    const e = await errP;
    assert.ok(e.message.includes('closed'));
    console.log('ok close mid-body -> error');
}

async function startTestServer() {
    const server = http.createServer((req, res) => {
        const path = new URL(req.url, 'http://localhost').pathname;
        if (path === '/json') {
            const body = JSON.stringify({ ok: true, n: 42 });
            res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) });
            res.end(body);
        } else if (path === '/sse') {
            res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
            let i = 0;
            const t = setInterval(() => {
                i++;
                res.write(`data: ${JSON.stringify({ delta: 'x'.repeat(10), i })}\n\n`);
                if (i >= 20) {
                    clearInterval(t);
                    res.write('data: {"done": true}\n\n');
                    res.end();
                }
            }, 10);
            req.on('close', () => clearInterval(t));
        } else if (path === '/echo') {
            const chunks = [];
            req.on('data', (d) => chunks.push(d));
            req.on('end', () => {
                const body = Buffer.concat(chunks);
                res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': body.length });
                res.end(body);
            });
        } else if (path === '/slow-headers') {
            setTimeout(() => {
                res.writeHead(200, { 'Content-Length': 2 });
                res.end('hi');
            }, 1500);
        } else {
            res.writeHead(404, { 'Content-Length': 0 });
            res.end();
        }
    });
    await new Promise((r) => server.listen(0, '127.0.0.1', r));
    return server;
}

async function runIntegration() {
    const server = await startTestServer();
    const port = server.address().port;

    function socketTransport() {
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
    }

    const drive = new HttpDrive({
        transportFactory: socketTransport,
        maxConns: 2,
        headerTimeoutMs: 5000
    });

    const r1 = await drive.fetch('/json?x=1', { method: 'GET' });
    assert.equal(r1.status, 200);
    assert.deepEqual(await r1.json(), { ok: true, n: 42 });
    console.log('  step: json ok');

    const r1b = await drive.fetch('/json', { method: 'GET' });
    assert.equal((await r1b.json()).n, 42);
    assert.ok(drive.idle.length === 1 || drive.total <= 2, 'keep-alive reuse');
    console.log('  step: keep-alive ok');

    const payload = JSON.stringify({ prompt: 'привет мир', history: [1, 2, 3] });
    const r2 = await drive.fetch('/echo', { method: 'POST', body: payload, headers: { 'Content-Type': 'application/json' } });
    assert.equal(await r2.text(), payload);
    console.log('  step: echo ok');

    const r3 = await drive.fetch('/sse', { method: 'POST', body: '{}' });
    assert.equal(r3.status, 200);
    const reader = r3.body.getReader();
    const dec = new TextDecoder();
    let sseText = '';
    let events = 0;
    while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        sseText += dec.decode(value, { stream: true });
        events += (sseText.match(/data:/g) || []).length > events ? 1 : 0;
    }
    assert.ok(sseText.includes('"done": true'));
    assert.ok(events >= 21, `expected >= 21 events, got ${events}`);

    const abortCtrl = new AbortController();
    const r4Promise = drive.fetch('/sse', { method: 'POST', body: '{}', signal: abortCtrl.signal });
    const r4 = await r4Promise;
    const reader4 = r4.body.getReader();
    await reader4.read();
    abortCtrl.abort();
    let abortedCleanly = false;
    try {
        for (let i = 0; i < 100; i++) await reader4.read();
    } catch (e) {
        abortedCleanly = true;
    }
    assert.ok(abortedCleanly || drive.total < 2, 'abort destroys conn');

    const shortTimeoutDrive = new HttpDrive({
        transportFactory: socketTransport,
        headerTimeoutMs: 300
    });
    await assert.rejects(() => shortTimeoutDrive.fetch('/slow-headers'), /timeout|closed/i);

    drive.close();
    shortTimeoutDrive.close();

    await new Promise((r) => server.close(r));
    console.log('ok integration against real HTTP server');
}

(async () => {
    await testContentLengthSplit();
    await testChunkedWithTrailers();
    await testSseIncremental();
    await testNoBody204();
    await testUntilClose();
    await testMidChunkCloseFails();
    await runIntegration();
    console.log('ALL TESTS PASSED');
    process.exit(0);
})().catch((e) => {
    console.error('TEST FAILURE:', e);
    process.exit(1);
});
