import assert from 'node:assert/strict';
import http from 'node:http';
import net from 'node:net';
import { HttpDrive } from '../src/lib/http-drive.js';
import { OnmyChatTransport, createChunkMapper, SseLineBuffer } from '../src/lib/onmychat-transport.js';

async function testMapperSequence() {
    const map = createChunkMapper();
    const out = [];
    const events = [
        { status: 'searching' },
        { sources: [{ url: '/f/a.pdf', title: 'A', owner: 'u', mimetype: 'application/pdf' }] },
        { thought_delta: 'думаю' },
        { delta: 'Прив', done: false },
        { delta: 'ет', done: false },
        {
            type: 'tool',
            action: 'part_update',
            id: 'tool_turn_1',
            state: { status: 'running', tool: 'todo', input: { todos: [] } }
        },
        {
            type: 'tool',
            action: 'part_update',
            id: 'tool_turn_1',
            state: { status: 'success', tool: 'todo', output: 'ok' }
        },
        { type: 'question.asked', requestID: 'rq1', question: 'Продолжить?', options: ['Да', 'Нет'] },
        { image: { path: '/img/x.png', title: 'X', description: '' }, prompt_id: 'p1', tokens_consumed: 5 },
        { newFact: 'факт' },
        {
            content: 'Привет',
            chatinfo: { name: 'c1', title: 'Тест' },
            new_title: 'Тест2',
            summary: 'сумма',
            done: true
        }
    ];
    for (const ev of events) out.push(...map(ev));
    out.push({ type: 'finish', finishReason: 'stop' });

    assert.equal(out[0].type, 'data-status');
    assert.equal(out[0].data.state, 'searching');
    assert.equal(out[1].type, 'source-url');
    assert.equal(out[1].url, '/f/a.pdf');
    assert.equal(out[2].type, 'reasoning-start');
    assert.equal(out[3].type, 'reasoning-delta');
    assert.equal(out[4].type, 'text-start');
    assert.deepEqual(
        out.filter((c) => c.type === 'text-delta').map((c) => c.delta),
        ['Прив', 'ет']
    );
    assert.ok(out.some((c) => c.type === 'tool-input-available' && c.toolCallId === 'tool_turn_1'));
    assert.ok(out.some((c) => c.type === 'tool-output-available' && c.toolCallId === 'tool_turn_1'));
    const q = out.find((c) => c.type === 'data-question');
    assert.equal(q.data.requestID, 'rq1');
    assert.deepEqual(q.data.options, ['Да', 'Нет']);
    assert.ok(out.some((c) => c.type === 'data-image' && c.data.path === '/img/x.png'));
    assert.ok(out.some((c) => c.type === 'data-knowledge-update' && c.data.fact === 'факт'));
    assert.ok(!out.some((c) => c.type === 'text-delta' && c.delta === 'Привет'), 'final content must not duplicate deltas');
    const fin = out.filter((c) => c.type === 'finish' && c.messageMetadata).pop();
    assert.ok(fin, 'metadata finish emitted');
    assert.equal(fin.messageMetadata.newTitle, 'Тест2');
    console.log('ok mapper sequence');
}

function testSseLineBufferPartial() {
    let got = [];
    const b = new SseLineBuffer((e) => got.push(e));
    const enc = new TextEncoder();
    b.push(enc.encode('data: {"del'));
    b.push(enc.encode('ta":"a"}\n\ndata: {"done":true}\n\n'));
    b.push(enc.encode('data: {"tail":1}'));
    b.end();
    assert.deepEqual(got, [{ delta: 'a' }, { done: true }, { tail: 1 }]);
    console.log('ok sse buffer partial frames + tail');
}

async function testTransportEndToEnd() {
    const server = http.createServer((req, res) => {
        if (req.url !== '/chat/stream') {
            res.writeHead(404).end();
            return;
        }
        let body = '';
        req.on('data', (d) => (body += d));
        req.on('end', () => {
            const payload = JSON.parse(body);
            assert.equal(payload.chat, 'testchat');
            assert.equal(payload.prompt, 'вопрос?');
            assert.deepEqual(payload.history, [
                { role: 'user', content: 'старый вопрос' },
                { role: 'assistant', content: 'старый ответ' }
            ]);
            res.writeHead(200, { 'Content-Type': 'text/event-stream' });
            const frames = [
                { status: 'thinking' },
                { delta: 'От', done: false },
                { delta: 'вет.', done: false },
                { content: 'Ответ.', chatinfo: { name: payload.chat }, done: true }
            ];
            for (const f of frames) res.write(`data: ${JSON.stringify(f)}\n\n`);
            res.end();
        });
    });
    await new Promise((r) => server.listen(0, '127.0.0.1', r));
    const port = server.address().port;

    function socketTransport() {
        let sock = null;
        return {
            get isOpen() {
                return !!sock && !sock.destroyed;
            },
            get bufferedAmount() {
                return 0;
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

    const drive = new HttpDrive({ transportFactory: socketTransport, headerTimeoutMs: 5000 });
    const transport = new OnmyChatTransport({
        httpDrive: drive,
        chatId: 'testchat',
        getOmdKey: () => ''
    });

    const messages = [
        { role: 'user', parts: [{ type: 'text', text: 'старый вопрос' }] },
        { role: 'assistant', parts: [{ type: 'text', text: 'старый ответ' }] },
        { role: 'user', parts: [{ type: 'text', text: 'вопрос?' }] }
    ];

    const chunkStream = await transport.sendMessages({ messages, abortSignal: null });
    const reader = chunkStream.getReader();
    const chunks = [];
    while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
    }

    assert.equal(chunks[0].type, 'start');
    assert.ok(chunks.some((c) => c.type === 'data-status' && c.data.state === 'thinking'));
    assert.equal(chunks[chunks.length - 1].type, 'finish');
    const text = chunks
        .filter((c) => c.type === 'text-delta')
        .map((c) => c.delta)
        .join('');
    assert.equal(text, 'Ответ.');
    const ci = chunks.find((c) => c.type === 'finish');
    assert.deepEqual(ci.messageMetadata.chatinfo, { name: 'testchat' });
    assert.ok(chunks.some((c) => c.type === 'text-start') && chunks.some((c) => c.type === 'text-end'));

    drive.close();
    await new Promise((r) => server.close(r));
    console.log('ok transport end-to-end over tunneled HTTP+SSE');
}

(async () => {
    testSseLineBufferPartial();
    await testMapperSequence();
    await testTransportEndToEnd();
    console.log('ALL TRANSPORT TESTS PASSED');
    process.exit(0);
})().catch((e) => {
    console.error('TEST FAILURE:', e);
    process.exit(1);
});
