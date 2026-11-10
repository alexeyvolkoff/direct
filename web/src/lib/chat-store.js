import Gun from "gun/gun";
import { simpleHash, createChatCrypto } from "./omd-key.js";

const RELAY_URL = "wss://direct.onmydisk.net:8765/gun";
const PEERS = [
  "wss://direct.onmydisk.net:8765/gun",
  "wss://staging.onmydisk.net:8765/gun"
];
const GUN_GET_TIMEOUT = 2500;

function serializeForGun(obj) {
  if (obj === null || typeof obj !== "object") return obj;
  const result = {};
  for (const key in obj) {
    if (!Object.prototype.hasOwnProperty.call(obj, key)) continue;
    const val = obj[key];
    if (val === undefined) continue;
    if (Array.isArray(val) || (typeof val === "object" && val !== null)) {
      result[key] = JSON.stringify(val);
    } else {
      result[key] = val;
    }
  }
  return result;
}

function deserializeFromGun(clean) {
  if (!clean) return clean;
  for (const key in clean) {
    if (!Object.prototype.hasOwnProperty.call(clean, key)) continue;
    if (["ciphertext", "iv", "nonce", "timestamp", "role"].includes(key)) continue;
    const val = clean[key];
    if (typeof val === "string" && (val.startsWith("[") || val.startsWith("{"))) {
      try {
        const parsed = JSON.parse(val);
        if (key === "content") {
          if (parsed && parsed.encrypted && parsed.ciphertext) {
            clean[key] = parsed;
          }
        } else {
          clean[key] = parsed;
        }
      } catch (e) {
        // keep raw string
      }
    }
  }
  return clean;
}

async function resolveGunLinks(clean, gun) {
  if (!clean) return clean;
  for (const prop in clean) {
    if (!Object.prototype.hasOwnProperty.call(clean, prop)) continue;
    const val = clean[prop];
    if (val && typeof val === "object" && "#" in val) {
      const resolvedVal = await getWithTimeout(gun.get(val["#"]), 1500);
      if (resolvedVal) {
        const cleanResolved = { ...resolvedVal };
        delete cleanResolved._;
        clean[prop] = cleanResolved;
      } else {
        clean[prop] = null;
      }
    }
  }
  return clean;
}

function getWithTimeout(gunChain, ms = GUN_GET_TIMEOUT) {
  let timer;
  const timeout = new Promise((resolve) => {
    timer = setTimeout(() => resolve(null), ms);
  });
  const read = new Promise((resolve) => {
    try {
      gunChain.once((data) => resolve(data));
    } catch (e) {
      resolve(null);
    }
  });
  return Promise.race([read, timeout]).finally(() => clearTimeout(timer));
}

export class ChatStore {
  constructor(token) {
    if (!token) throw new Error("ChatStore requires omd_key");
    this.tokenHash = simpleHash(token);
    this.crypto = createChatCrypto(token);
    this.gun = Gun({ peers: PEERS, localStorage: false });
    this.chatsCache = new Map();
    this._chatsCallbacks = new Set();
    this._notifyTimer = null;
    this._listener = null;
    this.knowledgeCache = new Map();
    this._knowledgeCallbacks = new Set();
    this._knowledgeListener = null;
    this.promptsCache = new Map();
    this._promptsCallbacks = new Set();
    this._promptsListener = null;
    this._promptsNotifyTimer = null;
  }

  get ready() {
    return this.crypto.ready;
  }

  onChatsChanged(cb) {
    this._chatsCallbacks.add(cb);
    if (!this._listener) this._subscribeChats();
    Promise.resolve().then(() => {
      if (this._chatsCallbacks.has(cb)) cb(this.listChats());
    });
    return () => this._chatsCallbacks.delete(cb);
  }

  _notify(immediate = false) {
    if (this._notifyTimer) {
      clearTimeout(this._notifyTimer);
      this._notifyTimer = null;
    }
    const fire = () => {
      const list = this.listChats();
      for (const cb of this._chatsCallbacks) cb(list);
    };
    if (immediate) {
      fire();
    } else {
      this._notifyTimer = setTimeout(fire, 50);
    }
  }

  _processChatRecord(val, key) {
    if (!val || val.deleted) {
      if (this.chatsCache.get(key)) {
        this.chatsCache.delete(key);
        this._notify();
      }
      return;
    }
    try {
      let clean = { ...val };
      delete clean._;
      clean = deserializeFromGun(clean);
      if (clean.chatinfo && typeof clean.chatinfo === "string") {
        try { clean.chatinfo = JSON.parse(clean.chatinfo); } catch (e) {}
      }
      if (clean.archived === "true" || clean.archived === true || clean.updated === "1970-01-01T00:00:00Z") {
        clean.archived = true;
      } else {
        clean.archived = false;
      }
      if (clean.title || clean.name || clean.updated) {
        this.chatsCache.set(key, clean);
        this._notify();
      }
    } catch (e) {
      console.warn("[chat-store] chat record parse failed:", key, e);
    }
  }

  _subscribeChats() {
    const rootNode = this.gun.get("chats_" + this.tokenHash);

    // Initial load: fetch all keys and resolve souls
    rootNode.once((data) => {
      if (!data) return;
      for (const k of Object.keys(data)) {
        if (k === "_" || !k) continue;
        const item = data[k];
        if (item && typeof item === "object" && item["#"]) {
          this.gun.get(item["#"]).once((resolved) => {
            if (resolved) this._processChatRecord(resolved, k);
          });
        } else if (item && typeof item === "object") {
          this._processChatRecord(item, k);
        } else {
          rootNode.get(k).once((chatData) => {
            if (chatData) this._processChatRecord(chatData, k);
          });
        }
      }
    });

    // Real-time map listener
    this._listener = rootNode.map().on((val, key) => {
      if (!val) return;
      if (val["#"]) {
        this.gun.get(val["#"]).once((resolved) => {
          if (resolved) this._processChatRecord(resolved, key);
        });
      } else {
        this._processChatRecord(val, key);
      }
    });
  }

  listChats() {
    const list = Array.from(this.chatsCache.values());
    list.sort((a, b) => new Date(b.updated || 0) - new Date(a.updated || 0));
    return list;
  }

  // Документы, приаттаченные к чату через /learn (персистятся в записи чата).
  // Пока active=true — их факты инжектятся во все последующие ответы.
  getAttachedDocs(chatId) {
    const rec = this.chatsCache.get(chatId);
    if (!rec || !Array.isArray(rec.attached_docs)) return [];
    return rec.attached_docs.filter((d) => d && (d.docId || d.document_id));
  }

  async setAttachedDocs(chatId, docs) {
    await this.ready;
    return this.saveChat({ _id: chatId, attached_docs: Array.isArray(docs) ? docs : [] });
  }

  async saveChat(chatInfo) {
    await this.ready;
    const existing = this.chatsCache.get(chatInfo._id) || {};
    const record = {
      ...existing,
      _id: chatInfo._id,
      name: chatInfo.name || existing.name || "",
      title: chatInfo.title || existing.title || "",
      file: chatInfo.file || existing.file || "",
      archived: chatInfo.archived !== undefined ? !!chatInfo.archived : !!existing.archived,
      summary: typeof chatInfo.summary === "string" && chatInfo.summary
          ? chatInfo.summary
          : (existing.summary || ""),
      attached_docs: Array.isArray(chatInfo.attached_docs)
          ? chatInfo.attached_docs
          : (Array.isArray(existing.attached_docs) ? existing.attached_docs : []),
      updated: chatInfo.updated instanceof Date ? chatInfo.updated.toISOString() : (chatInfo.updated || new Date().toISOString()),
    };
    this.chatsCache.set(record._id, record);
    this._notify(true);
    const serialized = serializeForGun(record);
    return new Promise((resolve, reject) => {
      this.gun.get("chats_" + this.tokenHash).get(record._id).put(serialized, (ack) => {
        if (ack && ack.err) reject(new Error(ack.err));
        else resolve(record);
      });
    });
  }

  async archiveChat(chatId, archived = true) {
    await this.ready;
    const existing = this.chatsCache.get(chatId) || { _id: chatId };
    const updated = archived ? "1970-01-01T00:00:00Z" : new Date().toISOString();
    return this.saveChat({
      ...existing,
      archived,
      updated,
    });
  }

  _getCachedMessages(chatId) {
    if (this._messagesCache && this._messagesCache.has(chatId)) {
      return [...this._messagesCache.get(chatId)];
    }
    if (typeof window !== "undefined" && window.localStorage) {
      try {
        const raw = localStorage.getItem("omd_chat_msgs:" + chatId);
        if (raw) {
          const list = JSON.parse(raw);
          if (Array.isArray(list)) {
            if (!this._messagesCache) this._messagesCache = new Map();
            this._messagesCache.set(chatId, list);
            return [...list];
          }
        }
      } catch (_) {}
    }
    return [];
  }

  _setCachedMessages(chatId, messages) {
    if (!this._messagesCache) this._messagesCache = new Map();
    this._messagesCache.set(chatId, messages);
    if (typeof window !== "undefined" && window.localStorage) {
      try {
        localStorage.setItem("omd_chat_msgs:" + chatId, JSON.stringify(messages));
      } catch (_) {}
    }
  }

  _clearCachedMessages(chatId) {
    if (this._messagesCache) this._messagesCache.delete(chatId);
    if (typeof window !== "undefined" && window.localStorage) {
      try {
        localStorage.removeItem("omd_chat_msgs:" + chatId);
      } catch (_) {}
    }
  }

  async deleteChat(chatId) {
    await this.ready;
    this.gun.get("chats_" + this.tokenHash).get(chatId).put(null);
    this.gun.get("chat_messages_" + chatId).put(null);
    this.chatsCache.delete(chatId);
    this._clearCachedMessages(chatId);
    this._notify(true);
  }

  async getMessages(chatId) {
    await this.ready;
    const cached = this._getCachedMessages(chatId);

    const toTime = (ts) => {
      if (!ts) return 0;
      if (typeof ts === "number") return ts;
      const num = Number(ts);
      if (!isNaN(num) && !String(ts).includes("-") && !String(ts).includes("T")) return num;
      const d = new Date(ts).getTime();
      return isNaN(d) ? 0 : d;
    };

    try {
      const nodeData = await getWithTimeout(this.gun.get("chat_messages_" + chatId), 1800);
      if (nodeData) {
        const keys = Object.keys(nodeData).filter((k) => k !== "_");
        if (keys.length) {
          await Promise.allSettled(
            keys.map(async (nonce) => {
              const val = await getWithTimeout(this.gun.get("chat_messages_" + chatId).get(nonce), 1500);
              if (val) {
                try {
                  let raw = { ...val };
                  delete raw._;
                  raw = await resolveGunLinks(raw, this.gun);
                  raw = deserializeFromGun(raw);
                  if (raw.content && raw.content.encrypted) {
                    try {
                      raw.content = await this.crypto.decrypt(raw.content);
                    } catch (e) {
                      raw.content = raw.content?.ciphertext ? "[unable to decrypt]" : raw.content;
                    }
                  }
                  if (raw.sender && !raw.role) raw.role = raw.sender;
                  const hasContent = raw.content && (typeof raw.content === "string" ? raw.content.trim().length > 0 : true);
                  const hasMedia = raw.image || raw.facts || raw.newFact || raw.sources || raw.diffs;
                  const isSystemEvent = ["delete", "removechat"].includes(raw.type);
                  if (hasContent || hasMedia || isSystemEvent || raw.role) {
                    const mNonce = String(raw.nonce || nonce);
                    const idx = cached.findIndex((m) => String(m.nonce) === mNonce);
                    if (idx >= 0) {
                      cached[idx] = { ...cached[idx], ...raw, nonce: mNonce };
                    } else {
                      cached.push({ ...raw, nonce: mNonce });
                    }
                  }
                } catch (e) {
                  console.warn("[chat-store] message parse failed:", nonce, e);
                }
              }
            })
          );
        }
      }
    } catch (e) {
      console.warn("[chat-store] remote message fetch fallback to local cache:", e);
    }

    cached.sort((a, b) => toTime(a.timestamp) - toTime(b.timestamp));
    this._setCachedMessages(chatId, cached);
    return cached;
  }

  async addMessage(chatId, msg) {
    await this.ready;
    const nonce = String(msg.nonce || ("msg_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 7)));
    const timestamp = typeof msg.timestamp === "number"
      ? new Date(msg.timestamp).toISOString()
      : (msg.timestamp || new Date().toISOString());

    const cleanMsg = {
      nonce,
      timestamp,
      role: msg.role === "assistant" ? "assistant" : "user",
      content: msg.content ?? "",
      ...(msg.image ? {
        image: {
          path: msg.image.path || "",
          title: msg.image.title || "",
          description: msg.image.description || "",
          prompt: msg.image.prompt || (typeof msg.content === "string" ? msg.content : "") || "",
          url: msg.image.url || "",
        }
      } : {}),
      ...(msg.sources ? { sources: msg.sources } : {}),
      ...(msg.facts ? { facts: msg.facts } : {}),
      ...(msg.newFact ? { newFact: msg.newFact } : {}),
    };

    // 1. Immediately update local persistent cache
    const cached = this._getCachedMessages(chatId);
    const existingIdx = cached.findIndex((m) => String(m.nonce) === nonce);
    if (existingIdx >= 0) {
      if (!msg.timestamp && cached[existingIdx].timestamp) {
        cleanMsg.timestamp = cached[existingIdx].timestamp;
      }
      cached[existingIdx] = cleanMsg;
    } else {
      cached.push(cleanMsg);
    }
    this._setCachedMessages(chatId, cached);

    // 2. Encrypt content and persist to Gun
    const encryptedContent = await this.crypto.encrypt(cleanMsg.content);
    const record = {
      ...cleanMsg,
      content: encryptedContent,
    };
    const serialized = serializeForGun(record);

    return new Promise((resolve) => {
      this.gun.get("chat_messages_" + chatId).get(nonce).put(serialized, (ack) => {
        if (ack && ack.err) {
          console.warn("[chat-store] Gun put message warning:", ack.err);
        }
        resolve(cleanMsg);
      });
      setTimeout(() => resolve(cleanMsg), 600);
    });
  }

  async deleteMessage(chatId, nonce) {
    if (!chatId || !nonce) return;
    await this.ready;
    const strNonce = String(nonce);

    // 1. Remove from local cache immediately
    const cached = this._getCachedMessages(chatId);
    const updated = cached.filter((m) => String(m.nonce || m.id) !== strNonce);
    this._setCachedMessages(chatId, updated);

    // 2. Put null in GunDB
    return new Promise((resolve) => {
      this.gun.get("chat_messages_" + chatId).get(strNonce).put(null, () => resolve(true));
      setTimeout(() => resolve(true), 400);
    });
  }

  async purgeMessagesFrom(chatId, nonce, text = "") {
    if (!chatId || (!nonce && !text)) return;
    await this.ready;
    const strNonce = nonce ? String(nonce) : "";

    const cached = this._getCachedMessages(chatId);
    let idx = strNonce ? cached.findIndex((m) => String(m.nonce || m.id) === strNonce) : -1;
    if (idx === -1 && text) {
      idx = cached.findIndex((m) => m.role === "user" && m.content === text);
    }
    if (idx !== -1) {
      const toRemove = cached.slice(idx);
      const remaining = cached.slice(0, idx);
      this._setCachedMessages(chatId, remaining);

      for (const m of toRemove) {
        const key = String(m.nonce || m.id);
        this.gun.get("chat_messages_" + chatId).get(key).put(null);
      }
    }
  }

  // --- Knowledge Management ---

  onKnowledgeChanged(cb) {
    this._knowledgeCallbacks.add(cb);
    if (!this._knowledgeListener) this._subscribeKnowledge();
    Promise.resolve().then(() => {
      if (this._knowledgeCallbacks.has(cb)) cb(this.listKnowledge());
    });
    return () => this._knowledgeCallbacks.delete(cb);
  }

  _notifyKnowledge() {
    const list = this.listKnowledge();
    for (const cb of this._knowledgeCallbacks) cb(list);
  }

  _subscribeKnowledge() {
    this._knowledgeListener = this.gun.get("knowledge_" + this.tokenHash).map().on(async (val, key) => {
      if (!val || val.deleted) {
        if (this.knowledgeCache.get(key)) {
          this.knowledgeCache.delete(key);
          this._notifyKnowledge();
        }
        return;
      }
      try {
        let clean = { ...val };
        delete clean._;
        clean = deserializeFromGun(clean);
        if (clean.text && clean.text.encrypted) {
          try {
            clean.text = await this.crypto.decrypt(clean.text);
          } catch (e) {
            clean.text = clean.text?.ciphertext ? "[encrypted]" : clean.text;
          }
        }
        if (clean.tags && typeof clean.tags === "string") {
          try {
            clean.tags = JSON.parse(clean.tags);
          } catch {
            clean.tags = clean.tags.split(",").map((t) => t.trim()).filter(Boolean);
          }
        }
        if (!Array.isArray(clean.tags)) clean.tags = clean.tags ? [clean.tags] : [];
        if (clean.annotation_embedding && typeof clean.annotation_embedding === "string") {
          try {
            clean.annotation_embedding = JSON.parse(clean.annotation_embedding);
          } catch {
            clean.annotation_embedding = null;
          }
        }

        this.knowledgeCache.set(key, clean);
        this._notifyKnowledge();
      } catch (e) {
        console.warn("[chat-store] knowledge parse failed:", key, e);
      }
    });
  }

  listKnowledge() {
    const list = Array.from(this.knowledgeCache.values()).filter((m) => m && !m.isChunk);
    list.sort((a, b) => new Date(b.created || b.timestamp || 0) - new Date(a.created || a.timestamp || 0));
    return list;
  }

  async getKnowledge() {
    await this.ready;
    return this.listKnowledge();
  }

  async deleteKnowledge(memoryId) {
    await this.ready;
    this.gun.get("knowledge_" + this.tokenHash).get(memoryId).put(null);
    this.knowledgeCache.delete(memoryId);
    await this.deleteChunks(memoryId);
    this._notifyKnowledge();
  }

  async saveChunks(memoryId, chunks) {
    if (!Array.isArray(chunks) || chunks.length === 0) return;
    const node = this.gun.get("chunks_" + this.tokenHash).get(memoryId);
    const serialized = serializeForGun({ chunks, updated: new Date().toISOString() });
    return new Promise((resolve) => {
      node.put(serialized, () => resolve(true));
      setTimeout(() => resolve(true), 400);
    });
  }

  async getChunks(memoryId) {
    await this.ready;
    const data = await getWithTimeout(this.gun.get("chunks_" + this.tokenHash).get(memoryId), 2000);
    if (!data || !data.chunks) return [];
    if (typeof data.chunks === "string") {
      try { return JSON.parse(data.chunks); } catch { return []; }
    }
    if (Array.isArray(data.chunks)) return data.chunks;
    return [];
  }

  async deleteChunks(memoryId) {
    this.gun.get("chunks_" + this.tokenHash).get(memoryId).put(null);
  }

  /** Дописывает annotation_embedding в существующую карточку (merge-put) и апдейтит кэш. */
  updateKnowledgeEmbedding(memoryId, embedding) {
    if (!memoryId || !Array.isArray(embedding)) return;
    this.gun
      .get("knowledge_" + this.tokenHash)
      .get(memoryId)
      .put({ annotation_embedding: JSON.stringify(embedding) });
    const cached = this.knowledgeCache.get(memoryId);
    if (cached) {
      const copy = { ...cached, annotation_embedding: embedding };
      this.knowledgeCache.set(memoryId, copy);
      this._notifyKnowledge();
    }
  }

  async addKnowledge(memo) {
    await this.ready;
    // Дедуп: если карточка для этого документа уже есть (по document_id-пути,
    // либо по имени для персональных), обновляем её вместо создания новой.
    const hasPath = !!memo.document_id && !memo.document_id.startsWith("user:note:");
    let existingId = null;
    for (const cached of this.knowledgeCache.values()) {
      if (!cached || cached.isChunk) continue;
      const cpath = cached.document_id || "";
      if (hasPath && cpath && cpath === memo.document_id) {
        existingId = cached._id || cached.memory_id || cached.id;
        break;
      }
      const personal = !cpath || cpath.startsWith("user:note:");
      const cTitle = (cached.title || "").toLowerCase().trim();
      const mTitle = (memo.title || "").toLowerCase().trim();
      if (!hasPath && personal && mTitle && cTitle === mTitle && !existingId) {
        existingId = cached._id || cached.memory_id || cached.id;
      }
    }
    const useExisting = existingId && !memo.memory_id && !memo._id;
    const memoryId = useExisting
      ? existingId
      : (memo.memory_id || memo._id || Math.random().toString(36).substr(2, 9));
    if (useExisting) {
      await this.deleteChunks(memoryId);
    }
    const tags = Array.isArray(memo.tags)
      ? memo.tags.filter(Boolean)
      : (typeof memo.tags === "string" ? memo.tags.split(",").map((t) => t.trim()).filter(Boolean) : []);
    const entry = {
      _id: memoryId,
      memory_id: memoryId,
      document_id: memo.document_id || "",
      title: memo.title || (memo.document_id ? memo.document_id.split("/").pop() : "Note"),
      text: await this.crypto.encrypt(memo.text || ""),
      tags,
      collection: memo.collection || "user",
      relevance: memo.relevance || "contextual",
      created: memo.created || new Date().toISOString(),
      timestamp: String(memo.timestamp || Date.now()),
      updated: new Date().toISOString(),
    };
    if (memo.imagePreview) {
      entry.imagePreview = memo.imagePreview;
    }
    if (Array.isArray(memo.annotation_embedding) && memo.annotation_embedding.length > 0) {
      entry.annotation_embedding = JSON.stringify(memo.annotation_embedding);
    }
    if (Array.isArray(memo.chunks) && memo.chunks.length > 0) {
      entry.chunk_count = memo.chunks.length;
      await this.saveChunks(memoryId, memo.chunks);
    }
    const serialized = serializeForGun(entry);
    return new Promise((resolve, reject) => {
      this.gun.get("knowledge_" + this.tokenHash).get(memoryId).put(serialized, (ack) => {
        if (ack && ack.err) reject(new Error(ack.err));
        else {
          const cacheEntry = { ...entry, text: memo.text, tags };
          if (entry.annotation_embedding) {
            cacheEntry.annotation_embedding = memo.annotation_embedding;
          }
          this.knowledgeCache.set(memoryId, cacheEntry);
          this._notifyKnowledge();
          resolve(cacheEntry);
        }
      });
    });
  }

  static cosineSimilarity(a, b) {
    if (!a || !b || a.length !== b.length || a.length === 0) return 0;
    let dot = 0, normA = 0, normB = 0;
    for (let i = 0; i < a.length; i++) {
      dot += a[i] * b[i];
      normA += a[i] * a[i];
      normB += b[i] * b[i];
    }
    const denom = Math.sqrt(normA) * Math.sqrt(normB);
    return denom === 0 ? 0 : dot / denom;
  }

  selectRelevantKnowledge(queryEmbedding, topN = 5) {
    if (!queryEmbedding || !Array.isArray(queryEmbedding) || queryEmbedding.length === 0) {
      return this.listKnowledge().map((m) => ({
        id: m._id || m.memory_id || m.id || "",
        title: m.title || "",
        text: m.text || "",
        tags: Array.isArray(m.tags) ? m.tags : [],
      }));
    }
    const scored = this.listKnowledge().map((m) => {
      const emb = m.annotation_embedding;
      const score = (Array.isArray(emb) && emb.length > 0)
        ? ChatStore.cosineSimilarity(queryEmbedding, emb)
        : -1;
      return { card: m, score };
    });
    scored.sort((a, b) => b.score - a.score);
    return scored.slice(0, topN).map(({ card: m }) => ({
      id: m._id || m.memory_id || m.id || "",
      title: m.title || "",
      text: m.text || "",
      tags: Array.isArray(m.tags) ? m.tags : [],
      score: scored.find((s) => s.card === m)?.score,
    }));
  }

  async getAssistantSettings() {
    await this.ready;
    const local = this._readSettingsLocal();
    const chain = this.gun.get("settings_" + this.tokenHash).get("current");
    // GunDB-реплика (relay) может отвечать с задержкой при перезагрузке страницы.
    // Если есть локальная копия из localStorage — пробуем GunDB с коротким таймаутом,
    // а при задержке мгновенно отдаём локальную копию.
    const timeoutMs = local ? 1200 : GUN_GET_TIMEOUT;
    for (let attempt = 0; attempt < 2; attempt++) {
      try {
        const raw = await getWithTimeout(chain, timeoutMs);
        if (raw) {
          let clean = { ...raw };
          delete clean._;
          if (clean.encrypted && clean.ciphertext) {
            try {
              const decrypted = await this.crypto.decrypt(clean);
              if (decrypted && typeof decrypted === "object") {
                this._writeSettingsLocal(decrypted);
                return decrypted;
              }
            } catch (e) {
              console.warn("[chat-store] decrypt settings failed:", e);
            }
          }
          this._writeSettingsLocal(clean);
          return clean;
        }
      } catch (e) {
        console.warn("[chat-store] getAssistantSettings attempt failed:", e);
      }
      if (local && attempt === 0) {
        // Локальная копия есть — отдаём немедленно, не задерживая рендеринг страницы
        return local;
      }
    }
    if (local) console.info("[chat-store] GunDB settings unreachable, using local mirror");
    return local;
  }

  subscribeAssistantSettings(callback) {
    if (!this.gun || !this.tokenHash) return () => {};
    const chain = this.gun.get("settings_" + this.tokenHash).get("current");
    chain.on(async (val) => {
      if (!val) return;
      let clean = { ...val };
      delete clean._;
      if (clean.encrypted && clean.ciphertext) {
        try {
          const decrypted = await this.crypto.decrypt(clean);
          if (decrypted && typeof decrypted === "object") {
            this._writeSettingsLocal(decrypted);
            callback(decrypted);
            return;
          }
        } catch (e) {}
      }
      callback(clean);
    });
    return () => {
      try { chain.off(); } catch (e) {}
    };
  }

  _settingsLocalKey() {
    return "omc_assistant_settings_" + this.tokenHash;
  }

  _writeSettingsLocal(obj) {
    if (typeof localStorage === "undefined") return;
    try {
      localStorage.setItem(this._settingsLocalKey(), JSON.stringify(obj));
    } catch (e) {
      console.warn("[chat-store] write settings local failed:", e);
    }
  }

  _readSettingsLocal() {
    if (typeof localStorage === "undefined") return null;
    try {
      const raw = localStorage.getItem(this._settingsLocalKey());
      if (raw) return JSON.parse(raw);
    } catch (e) {
      console.warn("[chat-store] read settings local failed:", e);
    }
    return null;
  }

  async updateAssistantSettings(settings) {
    await this.ready;
    if (!this.gun || !this.tokenHash) throw new Error("GunDB store not initialized");
    const existing = (await this.getAssistantSettings()) || {};
    delete existing._id;
    delete existing.encrypted;
    delete existing.ciphertext;
    delete existing.iv;

    const merged = { ...existing, ...settings };
    if (merged.avatar_source === "default") {
      delete merged.assistant_avatar;
      delete merged.avatarUrl;
    }
    merged.updated = new Date().toISOString();

    // Write to local cache immediately so UI and caller never lose changes
    this._writeSettingsLocal(merged);

    const encrypted = await this.crypto.encrypt(merged);
    const node = this.gun.get("settings_" + this.tokenHash).get("current");
    return new Promise((resolve) => {
      const timer = setTimeout(() => resolve(merged), 1000);
      try {
        node.put(encrypted, (ack) => {
          clearTimeout(timer);
          if (ack && ack.err) {
            console.warn("[chat-store] updateAssistantSettings put ack notice:", ack.err);
          }
          resolve(merged);
        });
      } catch (err) {
        clearTimeout(timer);
        resolve(merged);
      }
    });
  }

  onPromptsChanged(cb) {
    this._promptsCallbacks.add(cb);
    if (!this._promptsListener) this._subscribePrompts();
    Promise.resolve().then(() => {
      if (this._promptsCallbacks.has(cb)) cb(this.listPrompts());
    });
    return () => this._promptsCallbacks.delete(cb);
  }

  _notifyPrompts() {
    if (this._promptsNotifyTimer) {
      clearTimeout(this._promptsNotifyTimer);
      this._promptsNotifyTimer = null;
    }
    this._promptsNotifyTimer = setTimeout(() => {
      const list = this.listPrompts();
      for (const cb of this._promptsCallbacks) cb(list);
    }, 50);
  }

  _subscribePrompts() {
    this._promptsListener = this.gun.get("prompts_" + this.tokenHash).map().on((val, key) => {
      if (!val) {
        if (this.promptsCache.has(key)) {
          this.promptsCache.delete(key);
          this._notifyPrompts();
        }
        return;
      }
      try {
        let clean = { ...val };
        delete clean._;
        this.promptsCache.set(key, clean);
        this._notifyPrompts();
      } catch (e) {
        console.warn("[chat-store] prompt item parse error:", key, e);
      }
    });
  }

  listPrompts() {
    const list = Array.from(this.promptsCache.values());
    list.sort((a, b) => new Date(b.lastUsed || b.created || 0) - new Date(a.lastUsed || a.created || 0));
    return list;
  }

  async savePrompt(prompt, style = "", negative = "") {
    await this.ready;
    if (!prompt || !prompt.trim()) return null;
    const cleanPrompt = prompt.trim();
    const cleanStyle = (style || "").trim();
    const promptId = "p_" + simpleHash(cleanPrompt + cleanStyle);

    const node = this.gun.get("prompts_" + this.tokenHash).get(promptId);
    const existing = await getWithTimeout(node, 1000);

    let entry;
    if (existing) {
      entry = { ...existing };
      delete entry._;
      entry.lastUsed = new Date().toISOString();
      entry.usageCount = (entry.usageCount || 1) + 1;
    } else {
      entry = {
        _id: promptId,
        id: promptId,
        type: "prompt",
        text: cleanPrompt,
        style: cleanStyle,
        negative: (negative || "").trim(),
        created: new Date().toISOString(),
        lastUsed: new Date().toISOString(),
        usageCount: 1,
      };
    }

    return new Promise((resolve) => {
      node.put(entry, () => {
        this.promptsCache.set(promptId, entry);
        this._notifyPrompts();
        resolve(promptId);
      });
    });
  }

  async deletePrompt(promptId) {
    await this.ready;
    if (!promptId) return;
    const node = this.gun.get("prompts_" + this.tokenHash).get(promptId);
    return new Promise((resolve) => {
      node.put(null, () => {
        this.promptsCache.delete(promptId);
        this._notifyPrompts();
        resolve(true);
      });
    });
  }

  async associateFileWithPrompt(promptId, fileName, promptText = "") {
    await this.ready;
    if (!fileName) return;
    const cleanFileName = fileName.replace(/^\/+/, "");
    const assocId = "f_" + cleanFileName;
    const node = this.gun.get("prompt_associations_" + this.tokenHash).get(assocId);
    return new Promise((resolve) => {
      node.put({
        _id: assocId,
        type: "association",
        promptId: promptId || "",
        fileName: cleanFileName,
        promptText: promptText || "",
        timestamp: new Date().toISOString()
      }, () => resolve(true));
    });
  }

  async getPromptForFile(fileName) {
    await this.ready;
    if (!fileName) return null;
    const cleanFileName = fileName.replace(/^\/+/, "");
    const assocId = "f_" + cleanFileName;
    const node = this.gun.get("prompt_associations_" + this.tokenHash).get(assocId);
    const val = await getWithTimeout(node, 1000);
    if (val && (val.promptText || val.promptId)) {
      if (val.promptText) return val.promptText;
      if (val.promptId && this.promptsCache.has(val.promptId)) {
        return this.promptsCache.get(val.promptId).text;
      }
    }
    return null;
  }

  destroy() {
    this._chatsCallbacks.clear();
    this._knowledgeCallbacks.clear();
    this._promptsCallbacks.clear();
    if (this._promptsNotifyTimer) clearTimeout(this._promptsNotifyTimer);
    if (this._listener) {
      try { this._listener.off?.(); } catch (_) {}
      this._listener = null;
    }
    if (this._knowledgeListener) {
      try { this._knowledgeListener.off?.(); } catch (_) {}
      this._knowledgeListener = null;
    }
    if (this._promptsListener) {
      try { this._promptsListener.off?.(); } catch (_) {}
      this._promptsListener = null;
    }
  }
}

export function getChatIcon(title = "") {
  if (!title) return "💬";
  const emojiRegex = /^(\p{Emoji_Presentation}|\p{Extended_Pictographic})/u;
  const match = title.trim().match(emojiRegex);
  if (match) return match[0];
  const lower = title.toLowerCase();
  if (lower.includes("rag") || lower.includes("knowledge") || lower.includes("doc")) return "💡";
  if (lower.includes("guide") || lower.includes("help") || lower.includes("how")) return "✨";
  if (lower.includes("invoice") || lower.includes("bill") || lower.includes("finance")) return "📄";
  if (lower.includes("report") || lower.includes("summary") || lower.includes("draft")) return "📝";
  if (lower.includes("code") || lower.includes("bug") || lower.includes("dev")) return "💻";
  if (lower.includes("check") || lower.includes("test")) return "🔍";
  return "💬";
}

let _instance = null;

export function getChatStore(token) {
  if (_instance && _instance.currentToken === token) return _instance;
  if (_instance) _instance.destroy?.();
  _instance = new ChatStore(token);
  _instance.currentToken = token;
  return _instance;
}
