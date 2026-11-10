# Product Specification: **OnMyDisk** & **OnMyChat**

This document outlines the technological stack, value proposition, and architecture of the **OnMyDisk** and **OnMyChat** ecosystem, highlighting how they integrate to provide a private, self-hosted cloud and AI experience.

---

## 1. Unified Value Proposition

The core philosophy of the ecosystem is **taking the cloud out of the user's way**: personal data never needs to pass through provider-managed platforms to be accessible anywhere.

*   **Privacy First**: No user data — files, chats, or memories — is stored on provider-managed cloud servers. All information remains on user-owned hardware.
*   **Local Inference**: AI reasoning runs on-device or on a private network node, so data never leaks to external LLM providers.
*   **Direct Access**: Users reach their data straight from their devices — no forced synchronization, no staging on intermediary servers.
*   **Actionable Knowledge**: Personal documents and chat histories are turned into reusable, searchable knowledge through local vectorization.

---

## 2. Product Overview

### 2.1 **OnMyDisk** (OMD) - Self-Hosted Personal Cloud
**OnMyDisk** is a distributed file-sharing platform that provides secure, remote access to files stored on physical devices (PCs, laptops, NAS, or WiFi routers). It replaces the centralized "Cloud Storage" model with a decentralized, direct-access model. Devices connect to a gateway as a signaling relay; browsers reach the device directly over WebRTC P2P.

### 2.2 **OnMyChat** (OMC) - Local AI Assistant
**OnMyChat** is a personal AI assistant (typically represented by "June") that operates on the user's private data. It integrates with OMD storage to understand documents and provide context-aware assistance, running inference on the user's own AI node.

---

## 3. Technological Stack

| Layer | Component | Technology | Role |
| :--- | :--- | :--- | :--- |
| **Gateway** | Signaling relay | C++ / Qt 5.15 (core, network, sql, webchannel, websockets) | TCP/SSL link socket per connected node, WebRTC signaling, tunnel queues, site registry, `/signin` proxy to the legacy gateway. Knows no users. |
| **Node** | `Node.pro` | C++ / Qt 5.15 (embedded / router flavours) | Local HTTP server (PWA bootstrap, `/api/*`, webhooks), P2P DataChannel layer (libdatachannel), WebChannel RPCs (node-local search/ai/chat), local file system. |
| **Frontend** | svar (PWA) | Svelte / Vite | Web UI, ServiceWorker (`/sites/**` → `/p2p/sites/**`), P2P drive, DID identity, device list. |
| **Networking** | LinkSocket / WebRTC | SSL/TLS / libdatachannel | Secure command socket (gateway ↔ node); WebRTC DataChannels (browser ↔ node, with ICE/relay through the gateway). |
| **Identity** | DID auth | GunDB SEA (P-256), SEA signatures | Cryptographic identity; node pairing verified by OpenSSL ECDSA (no per-device secrets). |
| **Logic (OMC)** | AI node | Python (FastAPI) | RAG orchestration, `/rag/*` endpoints, inference dispatch. |
| **Inference** | Ollama | Llama, Mistral, DeepSeek | Local execution of Large Language Models. |
| **Data Sync** | OrbitDB / GunDB | P2P DB | Decentralized syncing of personal knowledge, device lists, and metadata. |
| **Search/Indexing** | Unified RAG 3.0 | ChromaDB + sentence transformers | One unified vector index per device; owner-only indexing events from the node. |
| **Vector Storage** | ChromaDB | Vector Database | Persistence for personal knowledge vectors on the AI node. |
| **Interface** | HTML5 / JS / Svelte | Front-end for interacting with the AI and file system. |

---

## 4. System Architecture

The architecture is divided into three zones, bridged by secure signaling and P2P tunnels.

### 4.1 Personal Zone (Edge)
*   **Personal Devices**: Handhelds and laptops running the **OnMyDisk Client** (PWA / mobile / router).
*   **OrbitDB / GunDB**: Local storage for "Personal Knowledge" and device lists. P2P syncing between user devices ensures metadata consistency without a central server.

### 4.2 Gateway (Signaling Relay)
*   **Component**: **OnMyDisk Gateway**.
*   **Role**: Signaling relay only. Maintains one command `LinkSocket` per connected node, WebRTC signaling, tunnel queues, and the site registry. Proxies `/signin` and user APIs to the legacy gateway.
*   **Function**: Bridges NAT boundaries for WebRTC P2P; provides the bootstrap UI and Jekyll site serving (`/api/site-meta`, `site-view.html`).

### 4.3 Private Network (Intelligence Layer)
*   **OnMyChat AI Node**: The core processing unit (FastAPI + ChromaDB + pandoc + sentence embeddings) running on the user's device.
*   **RAG (Retrieval-Augmented Generation)**: Unified vector index; owner-only indexing events from the C++ node (periodic, file change, mount, share add/remove).
*   **Inference Dispatcher**: Manages tasks sent to Ollama or other local providers.
*   **Jekyll Sites**: Served over P2P through a ServiceWorker page tunnel, with nginx as the domain-binding mechanism.

---

## 5. Component Roles in Data Flow

The following describes how data moves through the system during a typical interaction:

1.  **Request Initiation**: A user sends a query via the Web browser or mobile app (PWA).
2.  **Signaling**: The PWA negotiates a WebRTC DataChannel with the node through the gateway's signaling relay.
3.  **Direct Access**: File operations and RAG calls travel directly over the P2P tunnel (port 4000 for AI node) — no gateway proxying.
4.  **Context Retrieval (RAG)**: The AI node queries its local ChromaDB unified index; client-side memory cards (GunDB) are injected directly.
5.  **Inference**: The retrieved context and the user's query are formatted into a prompt; the AI node dispatches to Ollama.
6.  **Synchronization**: Updates and newly learned facts are saved as memory cards and synced via GunDB/OrbitDB.
7.  **Response Delivery**: The generated response is passed back over the P2P tunnel to the user's device.

---

## 6. Architecture Scheme Reference

The diagram illustrates the separation between the edge (personal devices), the gateway (signaling relay), and the private network (intelligence layer on the user's node), with P2P tunnels bridging NAT boundaries.

---

**Version**: 3.0  
**Authors**: Bineon d.o.o. Team (Alexey Volkov, Borut Rismal, Srdjan Petrovic, Anna Volkova)  
**Date**: October 2026
