---
layout: page
title: Storage as Knowledge
permalink: /rag/
nav_order: 6
---
# 🧠 Storage as Knowledge (Private RAG & Memory Cards)
<hr>

**Retrieval-Augmented Generation (RAG)** turns your **Disk Direct** folders, notes, and camera roll into a living, multimodal memory for your **AI Assistant**. Even if you run a lightweight laptop in **Agentic P2P mode** (`AI_PROVIDER = agentic`), document parsing, embeddings, and your `ChromaDB` vector database remain **100% local on your own disk**.

> **Requires a Self-Hosted Node for Personal Indexing**: You can open **Knowledge** right from the left sidebar at any time. Without a paired hardware node, you browse public reference cards and your own manual encrypted notes; once you install your own **[Disk Direct Node](/advanced/)**, your photos and documents are indexed automatically!
{: .tip }

### On-Device Facial Recognition & Multimodal Retrieval
Search your memories in plain natural language — across documents, places, events, and specific people. Local vision models (`Gemma 4`) and on-device facial recognition (`InsightFace`) identify faces, scenes, and landmarks directly on your private storage node without uploading raw images to external clouds. Ask **June** questions about your documents or recent photos and receive grounded answers with inline source previews that open straight from your node:

![Multimodal RAG — Grounded Answers with Sources](/images/rag.png)

---

## 1. Semantic Memory Cards

Documents from your laptop and photos synced from your phone's camera roll are indexed automatically in the background. Inside **Knowledge** (`Memory Cards`), your knowledge base organizes everything into browsable, multimodal cards (in Grid or List view) with automated captions, storage links, semantic `#hashtags`, and entity recognition:

![Semantic Memory Cards](/images/memory_cards.png)

Filter and explore your knowledge graph effortlessly:
* **🖼️ Photos & Scenes**: Every recognized photo gets an AI-generated scene caption, date/location context, OCR text, and clickable topic/place `#tags` (saved alongside `<image>.Readme.md` sidecars). Search by visual description, landmark, season, or mood.
* **👥 People (Facial Recognition)**: On-device face clustering and recognition links portraits and group shots to named **People** cards — so you can filter photos of family, friends, or teammates in one click or ask the assistant for photos of a specific person.
* **📁 Folders, 📄 Docs, 💡 Notes & 🌐 Links**: Browse indexed Office/PDF documents, synced **Obsidian** / Markdown vaults, saved web links, and personal conversation facts (`👤 Personal`), or filter by `# Tags` and `🖥️ On-device` records.

---

## 2. Unified Index, `#tags` & Privacy Scopes

**Disk Direct** uses a single unified `ChromaDB` collection (`omd_unified`) per node instead of rigid separate databases:

* **Owner Scope (`private`)**: When you chat with your own paired node, the **AI Assistant** searches your full ChromaDB index (documents, photos, people, and personal memory facts) on disk.
* **Guest Scope (`public` + E2EE Silo)**: Guests (including users on the **Public Demo AI Node**) never write to or read private owner records on the node. Instead:
  * Guests can query public reference knowledge tagged in `PUBLIC_KNOWLEDGE_TAGS` (e.g. `direct`, `public`).
  * Any document, note, or personal fact a guest saves is stored in their own **GunDB** graph (`knowledge_<tokenHash>`), encrypted client-side (AES-256-GCM).
* **Collections as `#tags`**: Organize projects, places, and topics naturally with hashtags (`#travel`, `#finance`, `#project-x`, `#public`) rather than separate database IDs.

---

## 3. Chat Commands & Workflows

Use slash commands directly inside **Chats**:

* **`/learn <path> [#tags] [question]`** *(or `/import`)*:
  1. Click **+** → `/learn` to pick a file across your connected devices.
  2. Optionally attach `#tags` and ask an immediate question.
  3. On your own node, `/rag/import/local` indexes the file straight into `ChromaDB`. On a remote/guest node, the browser extracts/uploads bytes via P2P and stores the encrypted card in your GunDB silo, then answers your question grounded in that file.
* **`/explain #<tag> <question>`**: Focus retrieval specifically on Memory Cards carrying `#<tag>`.
* **`/recognize <path>`**: Run the vision model (`VISION_MODEL`) on an image to extract OCR text, scene details, and tags into `<image>.Readme.md` and current chat context.
* **`/doc` / `/mcp` / `/tools`**: Trigger the autonomous multi-step **MCP** agent to search Memory Cards, read files, and generate formatted `.docx` / `.xlsx` / `.pptx` reports via **WordCraft / GridCraft / DeckCraft**.

---

## 4. Configuration & Best Practices

Tune retrieval and automatic photo recognition in `config.ini`:

```ini
PUBLIC_KNOWLEDGE_TAGS = direct
AUTO_RECOGNIZE_IMAGES = true
SEARCH_THRESHOLD = 0.75
RAG_THRESHOLD = 0.75
RAG_RELATIVE_RATIO = 0.65
RAG_TOP_K = 5
SEARCH_TOP_K = 20
```

* **Use `#hashtags` generously**: Adding `#client`, `#trip`, or `#2026` to notes and `.Readme.md` sidecars boosts precision when filtering Memory Cards or using `/explain #tag`.
* **Attach files for one-off analysis**: Files attached directly via **+** in the chat bar bypass similarity thresholds and are injected into context unconditionally.
