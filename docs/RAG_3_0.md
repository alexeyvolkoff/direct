# Unified RAG 3.0 — Architecture & AI Node Contract

Rework of Search/RAG for OMD 3.0 (svar frontend + C++ storage node + OnMyChat AI node).
Supersedes the "twin-collection" design in `SEARCH_AND_RAG.md`.

---

## 1. Principles

1. **One unified ChromaDB index per device.** No more separate DBs per user, no
   split of `omd` vs `omd_search`, no separate "search index" vs "RAG memory".
   Search and RAG are the same thing.
2. **One owner per device.** The physical index lives on the device's AI node.
   Whoever has access to the device (owner or people they granted) has access to
   the same index. Access is already enforced by the P2P layer / shares.
3. **No separate knowledge areas (kbids).** Replaced by **tags** on every record.
   Knowledge domains are just tags. The legacy `kbid="omd"` behavior is reproduced
   with the tag `public`.
4. **Everything is vectorized** — notes, fact cards, documents, web links
   (after conversion to markdown).
5. **Guest vs owner storage split:**
   - **Owner** = the user who owns the AI node/device. Vectors live in the
     device's ChromaDB (unified index).
   - **Guest** = any other user allowed to use the node for inference. Guests
     do **not** write into the device's index. Their docs live on their own web
     device; their **vectors and cards live in their own GunDB** silo
     (`knowledge_<guestTokenHash>`), E2EE with their own omd_key.
   Public knowledge is shared implicitly via the `public` tag.

---

## 2. Node / transport

- Frontend talks to the AI node over the **P2P raw HTTP tunnel** (`HttpDrive`,
  port 4000, WebRTC data channel → TCP bridge on the node → local HTTP server).
- **No gateway/nginx proxying** for RAG calls.
- Same auth as today: `Token` header = omd_key, `X-OMD-Private-Mode: 1|0`.

### Endpoints

All request/response bodies are JSON unless noted. Errors are plain
`{"error": {...}}` with HTTP 4xx/5xx.

#### `POST /rag/import/local` and `POST /rag/import/raw`

Import and vectorize a document, web link, or plain note/fact.
Returns annotation + tag cloud, ready to persist as a memory card in GunDB.

The `/local` variant streams a file that already lives on the device (absolute
path inside a share); `/raw` accepts uploaded bytes. Both produce the same
response.

Request:
```json
{
  "source": "/path/to/file.pdf",        // absolute path on the device, OR
  // "source": "https://example.com/a",  // web link
  // "text": "raw note text",            // plain note/fact (no source)
  "title": "optional title",
  "tags": ["omd", "marketing"],         // may be empty
  "scope": "public" | "private",        // default: "private"
  "owner": "username-or-omd_key_hash"   // who owns the import
}
```

The node fetches the file (owner path) or URL, converts to markdown (pandoc /
internal converters), chunks, embeds, stores into the unified index, and
annotates via the LLM.

Response:
```json
{
  "docId": "uuid",
  "documentId": "uuid-or-source-path",
  "title": "Title",
  "annotation": "markdown summary text",
  "tags": ["auto", "detected", "tags"],
  "chunkCount": 12
}
```

#### `POST /rag/recognize/local` and `POST /rag/recognize/raw`

Recognition variant (ChromaDB + memory_card). Used by the memory-cards
interface and the `/learn` flow. Returns the same payload as `/rag/import/*`.

Notes on guest vs owner:
- owner: vector & metadata stored in device ChromaDB.
- guest: AI node returns the same payload but does NOT persist to device index;
  the client persists the card (incl. computed vector if needed) to its GunDB.

#### `POST /rag/search`

Vector search over the unified index. Used both for the legacy search UI and for
RAG context retrieval (it's the same thing).

Request:
```json
{
  "query": "what does the marketing doc say about Q3?",
  "tags": ["omd"],              // filter; empty = no filter (owner "all")
  "scope": "public" | "all" | "self",
  "topK": 8,
  "maxChars": 4000
}
```

Scope semantics:
- `public` — only records tagged `public` (default for guests).
- `self` — records owned by the caller (guest vectors in their own store), or
  device index for owner.
- `all` — whole device index (owner only; guests get 403/empty).

Response:
```json
{
  "chunks": [
    {
      "chunk": "exact text chunk",
      "metadata": { "docId": "...", "title": "...", "source": "...", "tags": ["..."], "owner": "..." },
      "score": 0.87
    }
  ]
}
```

#### `POST /rag/delete`

Remove records from the index (used when files/shares are deleted, or a card
is removed).

Request:
```json
{ "source": "/path/removed/file.pdf" }
```
or
```json
{ "docId": "uuid" }
```

Response: `{"deleted": n}`.

#### `POST /rag/index`

Asynchronous (re-)index of a path inside shares. Called by the C++ node on
scheduled indexing, file save/upload, mount of an external disk, or new share.

Request:
```json
{
  "path": "/share/root",
  "recursive": true,
  "tags": ["public"],            // optional: tag all records from this crawl
  "force": false                 // if true, re-vector even if unchanged
}
```

Response immediately: `{"status": "indexing_started"}`. Progress is logged
server-side; clients poll `/rag/status?path=...`.

#### `GET /rag/status?path=...&docId=...`

Return indexing progress for a path or document.

Response:
```json
{ "path": "/share/root", "indexed": 120, "pending": 4, "failed": 1, "lastError": null }
```

---

## 3. Knowledge injection into chat

`POST /chat/stream` (existing) gains two optional fields:

```json
{
  "knowledge": [
    {
      "id": "memory-card-id",
      "title": "...",
      "text": "decrypted fact text from GunDB",
      "tags": ["omd", "public"]
    }
  ],
  "rag_focus": {
    "docId": "just-imported-doc-id",
    "query": "text the user appended after /learn path"
  }
}
```

- `knowledge` — client-side memory-card facts (from GunDB), injected directly,
  without a server lookup. This replaces the legacy `kbid` context selection.
- `rag_focus` — used by the `/learn` flow: after import, the assistant answers
  the user's appended question by matching the prompt against the just-created
  document vector and pulling relevant chunks into context.

---

## 4. `/learn` user flow (chat widget)

1. User taps **+** → `/learn` → `DocumentPicker` opens (all text/convertible
   formats from `file-accept.js`).
2. On file selection the prompt becomes `/learn /path/to/doc `, cursor at end;
   user can append `#tags` and a question.
3. On send, the client:
   a. parses path, `#tags` (`parseTagsFromPrompt`), and the trailing question;
   b. calls `POST /rag/import` over `HttpDrive`;
   c. persists the returned card (title, annotation, tags, docId) to GunDB
      `knowledge_<tokenHash>` (encrypted);
   d. issues `POST /chat/stream` with `rag_focus={docId, query}` so the
      assistant answers grounded on the new document.

Import via the **memory-cards interface** does steps (a–c) only — no question,
no `rag_focus`, no chat answer.

---

## 5. Owner-only indexing events (C++ node)

Indexing covers **shares only** (the list maintained in
`onmydisklinkclient.cpp`), not everything reachable through the gateway.

Trigger events:
- **Periodic timer** over the share list (`autoReindex` setting), calling
  `POST /rag/index` per share root.
- **External disk mount** when `automount` is enabled (`QDrive`/UDisks2 dbus
  signal `deviceAdded` → new mountpoint share).
- **File change / save / upload** (`fileUpdated` / HomeWrite / Write) →
  `POST /rag/index?path=<file>` (or delete on removal).
- **New share added** → index its root.
- **File / share deleted** → `POST /rag/delete`.

---

## 6. Deployment ("from scratch")

The provisioning script must install, from zero:
- the OMD node (onmydisk),
- the AI node (OnMyChat: FastAPI + ChromaDB + pandoc + sentence embeddings),
- and create the empty unified ChromaDB index.

Target layout (per device):
```
memory_index/                # ChromaDB PersistentClient
  chroma.sqlite3
  9893e/...
```

(One collection — name versioned, e.g. `omd_v2` — replaces `omd` + `omd_search`.)