---
layout: page
title: Semantic Search
permalink: /searchnode/
nav_order: 5
---
# 🔍 Semantic Search & Unified Indexing
<hr>

**Disk Direct** provides multilingual semantic search across your files, notes, and photos, powered by your **AI Assistant** node. Instead of matching only exact filenames or keywords, semantic search understands meaning across languages — for example, searching for *"income"* (or *"доход"*) surfaces documents mentioning *"revenue"*, *"salary"*, or *"financial report"*.

---

## 1. Unified Multimodal Index (`omd_unified`)

Search and RAG share a single embedded **ChromaDB** vector store (`memory_index/unified_index`, collection `omd_unified`) right on your node:

* **Multilingual Embeddings on CPU**: Uses the 384-dimensional **`paraphrase-multilingual-MiniLM-L12-v2`** model (`SentenceTransformers`), connecting queries and documents seamlessly across English, Russian, Slovenian, Ukrainian, German, and 50+ languages — fast enough to run locally on CPU even in **Agentic mode**.
* **One Unified Index + Tag Scopes**: Every indexed item carries flexible **`#tag`** metadata (`t_<tag>`). Publicly visible reference knowledge is controlled via `PUBLIC_KNOWLEDGE_TAGS` (e.g. `direct`, `public`).
* **Strict Owner vs. Guest Isolation**:
  * **Device Owner**: Full access to the node's local ChromaDB index; filesystem watchers and crawlers index your shares automatically.
  * **Guest Users**: Never write into the host's private index. Guest knowledge cards live in the guest's own decentralized **GunDB** silo (`knowledge_<tokenHash>`), encrypted end-to-end with their own key.

---

## 2. How Automatic Indexing Works

1. **Real-Time Filesystem Events**: Modifying, uploading, or deleting files inside shared folders triggers debounced (`2s`) incremental `/rag/index` updates (`source_stamp` deduplication skips unchanged files).
2. **Periodic Background Verification**: The C++ node crawler periodically reconciles share roots (`autoreindex`, every `6h`) and newly auto-mounted USB drives (`UDisks2`).
3. **Document Extraction & Chunking**: Text and Office documents are converted (`pdftotext`, `pandoc`, and `Craft` readers), split into overlapping semantic chunks, embedded with `paraphrase-multilingual-MiniLM-L12-v2`, and stored with recency-aware ranking.
4. **Automatic Photo & People Recognition (`AUTO_RECOGNIZE_IMAGES`)**: Local vision models (such as `Gemma 4`) and on-device facial recognition (`InsightFace`) analyze synced photos in the background — extracting scenes, landmarks, dates, OCR text, semantic `#tags`, and recognized people into `<image>.Readme.md` sidecars and visual **Memory Cards** without sending raw images outside your local network. Manual `.Readme.md` files are never overwritten.

---

## 3. Live Search & Grounded Chat Execution

1. **Direct P2P Transport**: The Web App / Mobile App queries your node directly over an encrypted **WebRTC DataChannel** (`HttpDrive` to `127.0.0.1:4000`) — search queries never pass through third-party search servers.
2. **Cosine HNSW Retrieval**: `ChromaDB` executes cosine-similarity search filtered by caller scope (`owner` vs. `public`) and any `#tag` filters.
3. **Interactive Source Chips in Chat**: During `POST /chat/stream`, retrieved document chunks ground the LLM response (whether running on local Ollama/RKLLAMA or over a P2P **Agentic** tunnel) and appear as clickable source chips that open the exact file in the built-in viewer.

---

## 4. Supported File Formats

* **Documents & Books**: `pdf`, `docx`, `odt`, `rtf`, `pptx`, `xlsx`, `csv`, `epub`, `fb2`, `latex`
* **Notes, Web & Code**: `md` / `markdown`, `txt`, `html`, source code files
* **Images** *(with `AUTO_RECOGNIZE_IMAGES = true`)*: `jpg`, `jpeg`, `png`, `webp` (via generated `<image>.Readme.md` sidecars)

---

## 5. Tuning Search & Relevance (`config.ini`)

```ini
# Multilingual 384-dim embedding model (changing requires re-indexing):
EMBEDDING_MODEL = paraphrase-multilingual-MiniLM-L12-v2

# Similarity & retrieval thresholds:
SEARCH_THRESHOLD = 0.75
RAG_THRESHOLD = 0.75
RAG_RELATIVE_RATIO = 0.65
RAG_TOP_K = 5
SEARCH_TOP_K = 20

# Comma-separated tags visible to non-owner guests:
PUBLIC_KNOWLEDGE_TAGS = direct

# Vision sidecar generation for bare photos:
AUTO_RECOGNIZE_IMAGES = true
```
