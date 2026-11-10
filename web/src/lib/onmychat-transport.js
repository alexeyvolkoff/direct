import { getClientPlatform } from './platform.js';

const TEXT_ID = 'txt';
const REASONING_ID = 'think';

export class SseLineBuffer {
    constructor(onEvent) {
        this.onEvent = onEvent;
        this.buf = '';
        this.dec = new TextDecoder();
    }

    push(bytes) {
        this.buf += this.dec.decode(bytes, { stream: true });
        while (true) {
            const res = this._findFrameEnd();
            if (!res) break;
            const frame = this.buf.slice(0, res.index);
            this.buf = this.buf.slice(res.index + res.length);
            this._dispatch(frame);
        }
    }

    end() {
        if (!this.buf) return;
        this.buf += this.dec.decode();
        while (true) {
            const res = this._findFrameEnd();
            if (!res) break;
            const frame = this.buf.slice(0, res.index);
            this.buf = this.buf.slice(res.index + res.length);
            this._dispatch(frame);
        }
        if (this.buf.trim()) {
            this._dispatch(this.buf.trim());
            this.buf = '';
        }
    }

    _findFrameEnd() {
        const a = this.buf.indexOf('\n\n');
        const b = this.buf.indexOf('\r\n\r\n');
        if (a === -1 && b === -1) return null;
        if (a !== -1 && (b === -1 || a < b)) {
            return { index: a, length: 2 };
        }
        return { index: b, length: 4 };
    }

    _dispatch(frame) {
        if (!frame) return;
        for (let line of frame.split(/\r?\n/)) {
            line = line.trim();
            if (!line.startsWith('data:')) continue;
            const payload = line.slice(5).trim();
            if (!payload || payload === '[DONE]') continue;
            try {
                this.onEvent(JSON.parse(payload));
            } catch (_) {
                this.onEvent({ delta: payload });
            }
        }
    }
}

export function createChunkMapper(onStatus) {
    let textOpen = false;
    let reasoningOpen = false;
    let sawDelta = false;

    const closeText = () => {
        if (textOpen) {
            textOpen = false;
            return [{ type: 'text-end', id: TEXT_ID }];
        }
        return [];
    };

    const closeReasoning = () => {
        if (reasoningOpen) {
            reasoningOpen = false;
            return [{ type: 'reasoning-end', id: REASONING_ID }];
        }
        return [];
    };

    return function mapEvent(ev) {
        const chunks = [];

        if (!ev || typeof ev !== 'object') return chunks;

        if (ev.error && ev.done === true) {
            chunks.push(...closeReasoning(), ...closeText());
            chunks.push({ type: 'error', errorText: String(ev.error) });
            return chunks;
        }
        if (typeof ev.error === 'string') {
            if (typeof onStatus === 'function') onStatus('');
            chunks.push({ type: 'error', errorText: ev.error });
            return chunks;
        }

        const statusVal = typeof ev.status === 'string'
            ? ev.status
            : (ev.type === 'status' && typeof ev.content === 'string' ? ev.content : null);

        if (statusVal) {
            const normalizedStatus = (statusVal === 'generating' || statusVal === 'generating_image' || statusVal === 'image')
                ? 'generating'
                : statusVal;
            if (typeof onStatus === 'function') onStatus(normalizedStatus);
            chunks.push({ type: 'data-status', id: 'st', data: { state: normalizedStatus, args: ev.args } });
            return chunks;
        }

        if (ev.event === 'newchat' && ev.chatinfo) {
            chunks.push({ type: 'data-chatinfo', id: 'ci', data: ev.chatinfo });
            return chunks;
        }

        if (ev.action === 'rename' && typeof ev.title === 'string' && ev.title) {
            chunks.push({ type: 'data-rename', id: 'rn', data: { title: ev.title } });
            return chunks;
        }

        const rawSources = ev.sources || ev.changedFiles;
        if (Array.isArray(rawSources)) {
            for (const s of rawSources) {
                if (!s) continue;
                const isStr = typeof s === 'string';
                let rawVal = isStr ? s : (s.fullPath || s.path || s.document_id || s.url || '');
                if (rawVal.startsWith('http://') || rawVal.startsWith('https://')) {
                    try {
                        const u = new URL(rawVal);
                        rawVal = decodeURIComponent(u.pathname);
                    } catch (_) {
                        rawVal = rawVal.replace(/^https?:\/\/[^\/]+/, '');
                    }
                }
                const url = isStr ? rawVal : (s.url && !s.url.includes('onmydisk.net') ? s.url : rawVal);
                const fullPath = isStr ? rawVal : (s.fullPath || s.path || s.document_id || rawVal);
                const path = isStr ? rawVal : (s.path || s.fullPath || s.document_id || rawVal);
                const title = isStr ? rawVal.split('/').pop() : (s.title || path.split('/').pop() || rawVal);
                chunks.push({
                    type: 'source-url',
                    sourceId: 'src-' + (fullPath || path || url || title || Math.random()),
                    url,
                    title,
                    mimetype: isStr ? '' : (s.mimetype || ''),
                    clickable: isStr ? true : (s.clickable ?? true),
                    owner: isStr ? '' : (s.owner || ''),
                    fullPath,
                    path,
                    document_id: isStr ? rawVal : (s.document_id || fullPath || path),
                    linkId: isStr ? '' : (s.linkId || ''),
                    size: isStr ? 0 : (s.size || 0)
                });
            }
            // Do not return immediately; let the chunk also process done: true or content if present
        }

        if (ev.new_knowledge || ev.newFact) {
            chunks.push({ type: 'data-new-fact', id: 'nf', data: { fact: ev.new_knowledge || ev.newFact } });
        }

        // Streaming reasoning / thinking deltas
        const thinkDelta = (typeof ev.thought_delta === 'string' && ev.done !== true)
            ? ev.thought_delta
            : (ev.type === 'thought' && typeof ev.text === 'string' && ev.done !== true)
                ? ev.text
                : (ev.type === 'reasoning' && typeof ev.delta === 'string' && ev.done !== true)
                    ? ev.delta
                    : null;

        if (thinkDelta !== null) {
            if (typeof onStatus === 'function') onStatus('thinking');
            if (!reasoningOpen) {
                reasoningOpen = true;
                chunks.push({ type: 'reasoning-start', id: REASONING_ID });
            }
            if (thinkDelta) {
                chunks.push({ type: 'reasoning-delta', id: REASONING_ID, delta: thinkDelta });
            }
            return chunks;
        }

        // Streaming text deltas - clear status as text is visibly streaming
        const textDelta = (typeof ev.delta === 'string' && ev.done !== true)
            ? ev.delta
            : (ev.type === 'content' && typeof ev.text === 'string' && ev.done !== true)
                ? ev.text
                : (ev.content && ev.done === false && typeof ev.content === 'string')
                    ? ev.content
                    : null;

        if (textDelta !== null) {
            if (typeof onStatus === 'function') onStatus('');
            if (!textOpen) {
                textOpen = true;
                sawDelta = true;
                chunks.push(...closeReasoning());
                chunks.push({ type: 'text-start', id: TEXT_ID });
            }
            if (textDelta) {
                chunks.push({ type: 'text-delta', id: TEXT_ID, delta: textDelta });
            }
            return chunks;
        }

        if (ev.type === 'tool' && ev.action === 'part_update' && ev.state) {
            const st = ev.state;
            const toolCallId = ev.id || 'tool-' + st.tool;
            if (ev.id === 'mcp_todo' || st.tool === 'todo') {
                chunks.push({
                    type: 'data-plan',
                    id: 'plan-' + (ev.id || 'mcp_todo'),
                    data: {
                        status: st.status || 'running',
                        todos: st.input?.todos || []
                    }
                });
            }
            if (st.status === 'running') {
                if (typeof onStatus === 'function') onStatus('executing');
                chunks.push({
                    type: 'tool-input-available',
                    toolCallId,
                    toolName: st.tool || 'tool',
                    input: st.input ?? null
                });
            } else if (st.status === 'success') {
                chunks.push({ type: 'tool-output-available', toolCallId, output: st.output ?? null });
            } else if (st.status === 'error') {
                chunks.push({ type: 'tool-output-error', toolCallId, errorText: typeof st.output === 'string' ? st.output : JSON.stringify(st.output ?? '') });
            }
            return chunks;
        }

        if (ev.type === 'question.asked' || (ev.requestID && ev.question && ev.options)) {
            chunks.push({
                type: 'data-question',
                id: 'q-' + (ev.requestID || Date.now()),
                data: { requestID: ev.requestID, question: ev.question, options: ev.options || [] }
            });
            return chunks;
        }

        if (ev.type === 'permission.asked' || (ev.permissionID && ev.permission)) {
            chunks.push({
                type: 'data-permission',
                id: 'p-' + (ev.permissionID || Date.now()),
                data: ev.permissionID ? { permissionID: ev.permissionID, permission: ev.permission } : ev
            });
            return chunks;
        }

        if (ev.image && typeof ev.image === 'object') {
            if (typeof onStatus === 'function') onStatus('typing');
            chunks.push({
                type: 'data-image',
                id: 'img-' + (ev.prompt_id || Date.now()),
                data: {
                    path: ev.image.path,
                    title: ev.image.title || '',
                    description: ev.image.description || '',
                    url: ev.image.url || '',
                    data: ev.image.data || null,
                    prompt: ev.prompt || ev.image.prompt || '',
                    tokensConsumed: ev.tokens_consumed ?? null
                }
            });
            return chunks;
        }

        if (ev.newFact || ev.new_knowledge) {
            chunks.push({
                type: 'data-knowledge-update',
                id: 'ku-' + Date.now(),
                data: { fact: ev.newFact || null, knowledge: ev.new_knowledge || null }
            });
            return chunks;
        }

        if (ev.done === true) {
            if (typeof onStatus === 'function') onStatus('');
            chunks.push(...closeReasoning());
            const finalContent = (typeof ev.content === 'string' && ev.content)
                ? ev.content
                : (typeof ev.delta === 'string' && ev.delta)
                    ? ev.delta
                    : '';
            if (textOpen) {
                if (!sawDelta && finalContent) {
                    chunks.push({ type: 'text-delta', id: TEXT_ID, delta: finalContent });
                }
                chunks.push(...closeText());
            } else if (!sawDelta && finalContent) {
                chunks.push(
                    { type: 'text-start', id: TEXT_ID },
                    { type: 'text-delta', id: TEXT_ID, delta: finalContent },
                    { type: 'text-end', id: TEXT_ID }
                );
            }
            const metadata = {};
            if (ev.chatinfo) metadata.chatinfo = ev.chatinfo;
            if (ev.new_title) metadata.newTitle = ev.new_title;
            if (ev.summary) metadata.summary = ev.summary;
            if (ev.facts) metadata.facts = ev.facts;
            chunks.push({ type: 'finish', finishReason: 'stop', messageMetadata: metadata });
            return chunks;
        }

        if (typeof ev.facts === 'string' && ev.done === false) {
            chunks.push({ type: 'data-facts', id: 'fx', data: { facts: ev.facts } });
            return chunks;
        }

        return chunks;
    };
}

function uiToLegacyHistory(messages) {
    const out = [];
    for (const m of messages) {
        if (!m || (m.role !== 'user' && m.role !== 'assistant')) continue;
        const text = (m.parts || [])
            .filter((p) => p.type === 'text')
            .map((p) => p.text)
            .join('');
        if (text) out.push({ role: m.role, content: text });
    }
    return out;
}

export class OnmyChatTransport {
    constructor({ httpDrive, chatId = 'default', getOmdKey = () => '', buildBody = null, onStatus = null, onUpdate = null, onChunk = null, onFinish = null }) {
        this.httpDrive = httpDrive;
        this.chatId = chatId;
        this.getOmdKey = getOmdKey;
        this.buildBody = buildBody;
        this.onStatus = onStatus;
        this.onUpdate = onUpdate;
        this.onChunk = onChunk;
        this.onFinish = onFinish;
    }

    async sendMessages({ messages, abortSignal, body = {} }) {
        if (typeof this.onStatus === 'function') {
            this.onStatus('processing');
        }

        const lastUser = [...(messages || [])].reverse().find((m) => m.role === 'user');
        const prompt = (lastUser?.parts || [])
            .filter((p) => p.type === 'text')
            .map((p) => p.text)
            .join('\n');
        const contextBody = (typeof this.buildBody === 'function')
            ? Object.assign({}, await this.buildBody(messages, body))
            : {};
        // Context compaction: when a backend summary is available, send only
        // the recent tail; the node rebuilds long-term context from the summary.
        let history;
        let resolvedSummary = contextBody.chat_summary || null;
        if (resolvedSummary) {
            const keep = (body.tail_messages ?? 20);
            const priorMsgs = messages.slice(0, Math.max(0, messages.length - 1));
            history = uiToLegacyHistory(priorMsgs.slice(-keep));
        } else {
            history = uiToLegacyHistory(messages.slice(0, Math.max(0, messages.length - 1)));
        }

        const payload = Object.assign(
            {
                omd_key: this.getOmdKey(),
                prompt,
                chat: contextBody.chat || this.chatId,
                history,
                client: 'svar',
                image_delivery: 'inline',
                settings: Object.assign(
                    { platform: getClientPlatform() },
                    body.content_mode ? { content_mode: body.content_mode } : {},
                    body.settings || {},
                    body.mcp_keys ? { mcp_keys: body.mcp_keys } : {}
                ),
                knowledge: body.knowledge ?? null,
                prompt_id: null,
                chat_summary: resolvedSummary,
                total_message_count: contextBody.total_message_count ?? null
            },
            this.buildBody ? await this.buildBody(messages, body) : {},
            body.extra || {}
        );

        let response;
        try {
            response = await this.httpDrive.fetch('/chat/stream', {
                method: 'POST',
                headers: {
                    Accept: 'text/event-stream',
                    'Content-Type': 'application/json',
                    'X-Client': 'svar',
                    'X-OMD-Platform': getClientPlatform(),
                    'X-Image-Delivery': 'inline'
                },
                body: JSON.stringify(payload),
                signal: abortSignal
            });
        } catch (fetchErr) {
            if (typeof this.onStatus === 'function') this.onStatus('');
            throw fetchErr;
        }

        if (!response.ok) {
            if (typeof this.onStatus === 'function') this.onStatus('');
            let errMsg = 'Request failed with status ' + response.status;
            try {
                const t = await response.text();
                if (t) {
                    try {
                        const j = JSON.parse(t);
                        errMsg = j.error || j.detail || errMsg;
                    } catch (_) {
                        errMsg = t;
                    }
                }
            } catch (_) {}
            throw new Error(errMsg);
        }

        const mapEvent = createChunkMapper(this.onStatus);
        const sse = new SseLineBuffer((ev) => {
            const mapped = mapEvent(ev);
            for (const c of mapped) {
                controller.enqueue(c);
                if (typeof this.onChunk === 'function') {
                    this.onChunk(c);
                }
            }
            if (typeof this.onUpdate === 'function') {
                this.onUpdate();
            }
        });

        let controller;
        let readerClosed = false;

        const stream = new ReadableStream({
            start: async (ctrl) => {
                controller = ctrl;
                ctrl.enqueue({ type: 'start' });
                try {
                    const reader = response.body.getReader();
                    while (true) {
                        const { done, value } = await reader.read();
                        if (done) break;
                        sse.push(value);
                    }
                    sse.end();
                } catch (e) {
                    if (!(abortSignal && abortSignal.aborted)) {
                        try {
                            ctrl.enqueue({ type: 'error', errorText: e.message || String(e) });
                        } catch (_) {}
                    }
                } finally {
                    if (typeof this.onStatus === 'function') this.onStatus('');
                }
                readerClosed = true;
                try {
                    ctrl.enqueue({ type: 'finish', finishReason: abortSignal && abortSignal.aborted ? 'abort' : 'stop' });
                } catch (_) {}
                if (typeof this.onFinish === 'function') {
                    this.onFinish();
                }
                try {
                    ctrl.close();
                } catch (_) {}
            },
            cancel: () => {
                readerClosed = true;
                if (typeof this.onStatus === 'function') this.onStatus('');
            }
        });

        return stream;
    }

    async reconnectToStream() {
        return null;
    }
}
