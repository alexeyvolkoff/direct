# Search & RAG (Retrieval-Augmented Generation) — Implementation Notes

> **Superseded design note.** The "twin-collection" ChromaDB architecture
> (separate `omd_search` and `omd` collections, gateway-side crawler, FastAPI
> `search_node.py` proxy) described in earlier versions of this document is **no
> longer the implementation**. The authoritative design is
> [`RAG_3_0.md`](RAG_3_0.md). This file documents how search/RAG is actually
> wired in the current codebase.

---

## 1. One unified index per device

There is a single ChromaDB collection per AI node (the OnMyChat FastAPI service
running on the node). Search and RAG are the same vector search — no split of
`omd` vs `omd_search`, no `kbid` knowledge areas (replaced by tags, with `public`
reproducing the legacy `kbid="omd"` behavior).

- Owner's vectors live in the device's ChromaDB index.
- Guests do not write into the device index; their cards/vectors live in their
  own GunDB silo (`knowledge_<guestTokenHash>`), E2EE with their own `omd_key`.
- Public knowledge is shared implicitly via the `public` tag.

## 2. Transport

The frontend (PWA) talks to the AI node over the **P2P raw HTTP tunnel**
(`HttpDrive`, port 4000 — WebRTC data channel → TCP bridge on the node → local
HTTP server). There is no gateway/nginx proxying for RAG calls. Auth uses the
same `Token` header (`omd_key`) and `X-OMD-Private-Mode`.

## 3. Endpoints (on the AI node)

`POST /rag/import`, `POST /rag/search`, `POST /rag/delete`, `POST /rag/index`,
`GET /rag/status` — request/response shapes are defined in `RAG_3_0.md` §2.
`POST /chat/stream` accepts optional `knowledge` (client-side memory cards) and
`rag_focus` (for the `/learn` flow).

## 4. `/learn` user flow (chat widget)

1. User taps **+** → `/learn` → `DocumentPicker` (formats from `file-accept.js`).
2. On selection the prompt becomes `/learn /path/to/doc ` with cursor at the end;
   user may append `#tags` and a question.
3. Client: parse path/tags/question → `POST /rag/import` over `HttpDrive` →
   persist the returned card to GunDB `knowledge_<tokenHash>` (encrypted) →
   `POST /chat/stream` with `rag_focus={docId, query}` for a grounded answer.
   The memory-cards interface does steps (a)–(c) only (no question).

## 5. Owner-only indexing events (C++ node)

Indexing covers **shares only** (the list maintained in
`onmydisklinkclient.cpp`). Trigger events:
- Periodic timer over the share list (`autoReindex`), calling
  `POST /rag/index` per share root.
- External disk mount when `automount` is enabled (UDisks2 dbus signal
  `deviceAdded` → new mountpoint share).
- File change / save / upload (`fileUpdated` / `HomeWrite` / `Write`) →
  `POST /rag/index?path=<file>` (or delete on removal).
- New share added → index its root.
- File / share deleted → `POST /rag/delete`.

## 6. Deployment

The provisioning script installs, from zero: the OMD node, the AI node
(OnMyChat: FastAPI + ChromaDB + pandoc + sentence embeddings), and creates the
empty unified ChromaDB index. Target layout per device:

```
memory_index/          # ChromaDB PersistentClient
  chroma.sqlite3
  9893e/...
```

One collection (name versioned, e.g. `omd_v2`) replaces the old `omd` +
`omd_search` pair.

## 7. Maintenance

To purge/rebuild an index for a device, use the AI node's `/rag/delete` (by
`source` or `docId`) or re-run `/rag/index` with `force: true`. Conversational
memory cards live in the user's GunDB silo and are managed client-side.
