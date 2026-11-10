// AI Node selection state: own device (auto/linkId) vs public system node.
//
// The public node ("On My Chat Public Node") is discovered through the
// gateway /api/ai-node endpoint. Access is guest-level: no pairing RPC,
// no device published into the user's DID device graph - the frontend
// only opens a P2P chat tunnel to the node after the user accepts the
// responsible-use agreement.
//
// Persistence:
//  - choice + acknowledgment: localStorage (fast, per-device boot state)
//  - mirrored into the E2EE GunDB assistant settings (`ai_node`,
//    `public_ai_ack`) by Settings.svelte when saving, so the choice can
//    be restored on other devices.

import { writable } from "svelte/store";
import { getGatewayBase } from "./omd-key.js";
import { WebRTCDataChannelDrive } from "./webrtc-drive.js";

export const AI_NODE_AGREEMENT_VERSION = "1.0";

export const AI_NODE_MODE_PUBLIC = "public";
export const AI_NODE_MODE_AUTO = "auto";
export const AI_NODE_MODE_CUSTOM = "custom";

// Runtime inference mode of the chat node's OnMyChat engine:
//   local   - everything on the local node (default)
//   hybrid  - local Ollama for WORK/RAG, AI Horde for FUN/RP
//   agentic - local node only indexes knowledge on CPU (SentenceTransformers +
//             ChromaDB); LLM (Ollama) and ComfyUI calls are tunneled over the
//             OMD P2P protocol to the node selected via `inferenceNodeId`.
//             Sent to OnMyChat as X-OMD-Ai-Mode / X-OMD-Ai-Node headers.
export const AI_MODE_LOCAL = "local";
export const AI_MODE_HYBRID = "hybrid";
export const AI_MODE_AGENTIC = "agentic";

const CHOICE_KEY = "omd_ai_node_choice";
const AI_MODE_KEY = "omd_ai_mode";
const INFERENCE_NODE_KEY = "omd_ai_infer_node";
const CUSTOM_AI_NODES_KEY = "omd_custom_ai_nodes";

export function readCustomAiNodesFromStorage() {
  if (typeof localStorage === "undefined") return [];
  try {
    const raw = JSON.parse(localStorage.getItem(CUSTOM_AI_NODES_KEY) || "[]");
    return Array.isArray(raw) ? raw : [];
  } catch (_) {
    return [];
  }
}

export function saveCustomAiNodesToStorage(list) {
  if (typeof localStorage === "undefined") return;
  try {
    localStorage.setItem(CUSTOM_AI_NODES_KEY, JSON.stringify(list || []));
  } catch (_) {}
}

export function addCustomAiNode({ linkId, name }) {
  const cleanId = (linkId || "").trim();
  if (!cleanId) return null;
  const current = readCustomAiNodesFromStorage();
  const idx = current.findIndex((c) => c.linkId === cleanId);
  const entry = {
    linkId: cleanId,
    name: name || cleanId,
    updatedAt: Date.now(),
  };
  let updated;
  if (idx >= 0) {
    updated = [...current];
    updated[idx] = { ...updated[idx], ...entry };
  } else {
    updated = [...current, entry];
  }
  saveCustomAiNodesToStorage(updated);
  aiNodeStore.update((s) => ({ ...s, customNodes: updated }));
  return entry;
}

export function removeCustomAiNode(linkId) {
  const cleanId = (linkId || "").trim();
  if (!cleanId) return;
  const current = readCustomAiNodesFromStorage();
  const updated = current.filter((c) => c.linkId !== cleanId);
  saveCustomAiNodesToStorage(updated);
  aiNodeStore.update((s) => {
    const next = { ...s, customNodes: updated };
    if (s.choice === cleanId) {
      next.choice = AI_NODE_MODE_AUTO;
      if (typeof localStorage !== "undefined") {
        localStorage.setItem(CHOICE_KEY, AI_NODE_MODE_AUTO);
      }
    }
    return next;
  });
}

export function mergeCustomAiNodes(list) {
  if (!Array.isArray(list) || list.length === 0) return;
  const current = readCustomAiNodesFromStorage();
  const map = new Map();
  for (const item of current) {
    if (item && item.linkId) map.set(item.linkId, item);
  }
  for (const item of list) {
    if (item && item.linkId) {
      const existing = map.get(item.linkId);
      map.set(item.linkId, { ...existing, ...item });
    }
  }
  const merged = Array.from(map.values());
  saveCustomAiNodesToStorage(merged);
  aiNodeStore.update((s) => ({ ...s, customNodes: merged }));
}

export async function connectGuestAiNode(linkId, didPub) {
  const cleanId = (linkId || "").trim();
  if (!cleanId) throw new Error("LinkID is required");
  const drive = new WebRTCDataChannelDrive(cleanId);
  drive.didPub = didPub || "";

  return new Promise((resolve, reject) => {
    let resolved = false;
    const timer = setTimeout(() => {
      if (resolved) return;
      resolved = true;
      drive.close();
      reject(new Error("Connection timeout"));
    }, 15000);

    drive.onStatusChange = async (st) => {
      if (st === "connected" && !resolved) {
        try {
          const info = await drive.authInfo();
          const r = (info && info.result) || {};
          if (!r.paired && r.guestPairing) {
            await drive.guestPair(didPub || "");
          } else if (r.authLevel === "guest") {
            drive.authLevel = "guest";
          }
          const nodeName = drive.rootName || r.nodeName || r.name || ("AI Node " + cleanId.slice(0, 6));
          resolved = true;
          clearTimeout(timer);
          drive.close();
          resolve({ linkId: cleanId, name: nodeName });
        } catch (e) {
          if (!resolved) {
            resolved = true;
            clearTimeout(timer);
            drive.close();
            reject(e);
          }
        }
      } else if ((st === "failed" || st === "disconnected") && !resolved) {
        resolved = true;
        clearTimeout(timer);
        drive.close();
        reject(new Error("Connection failed"));
      }
    };
    drive.connect();
  });
}

function readAiModeFromStorage() {
  if (typeof localStorage === "undefined") return AI_MODE_LOCAL;
  const stored = (localStorage.getItem(AI_MODE_KEY) || "").trim().toLowerCase();
  return [AI_MODE_LOCAL, AI_MODE_HYBRID, AI_MODE_AGENTIC].includes(stored) ? stored : AI_MODE_LOCAL;
}

function readInferenceNodeFromStorage() {
  if (typeof localStorage === "undefined") return "";
  return (localStorage.getItem(INFERENCE_NODE_KEY) || "").trim();
}

function readAckFromStorage() {
  if (typeof localStorage === "undefined") return false;
  try {
    const raw = JSON.parse(localStorage.getItem("omd_public_ai_ack") || "null");
    return !!raw && raw.version === AI_NODE_AGREEMENT_VERSION;
  } catch (_) {
    return false;
  }
}

function readChoiceFromStorage() {
  if (typeof localStorage === "undefined") return AI_NODE_MODE_AUTO;
  const stored = localStorage.getItem(CHOICE_KEY);
  return stored || AI_NODE_MODE_AUTO;
}

export const aiNodeStore = writable({
  choice: readChoiceFromStorage(),
  ack: readAckFromStorage(),
  info: null,      // {linkID, name, shareName, owner} from gateway
  aiMode: readAiModeFromStorage(),
  inferenceNodeId: readInferenceNodeFromStorage(),
  customNodes: readCustomAiNodesFromStorage(),
  hydrated: false,
});

export function isPublicAiActive(state) {
  return !!state && state.choice === AI_NODE_MODE_PUBLIC && state.ack && !!(state.info && state.info.linkID);
}

// Agentic routing headers for onmychat requests (empty when not applicable).
export function aiOnmychatHeaders(state) {
  if (!state || state.aiMode !== AI_MODE_AGENTIC || !state.inferenceNodeId) return {};
  return {
    "X-OMD-Ai-Mode": "agentic",
    "X-OMD-Ai-Node": state.inferenceNodeId,
  };
}

export function hydrateAiNodeState() {
  aiNodeStore.update((s) => ({
    ...s,
    choice: readChoiceFromStorage(),
    ack: readAckFromStorage(),
    aiMode: readAiModeFromStorage(),
    inferenceNodeId: readInferenceNodeFromStorage(),
    customNodes: readCustomAiNodesFromStorage(),
    hydrated: true,
  }));
}

export function setAiNodeChoice(choice) {
  if (typeof localStorage !== "undefined") {
    localStorage.setItem(CHOICE_KEY, choice);
  }
  aiNodeStore.update((s) => ({ ...s, choice }));
}

export function setAiMode(mode) {
  const m = [AI_MODE_LOCAL, AI_MODE_HYBRID, AI_MODE_AGENTIC].includes(mode) ? mode : AI_MODE_LOCAL;
  if (typeof localStorage !== "undefined") {
    localStorage.setItem(AI_MODE_KEY, m);
  }
  aiNodeStore.update((s) => ({ ...s, aiMode: m }));
}

export function setInferenceNode(linkId) {
  const id = (linkId || "").trim();
  if (typeof localStorage !== "undefined") {
    if (id) localStorage.setItem(INFERENCE_NODE_KEY, id);
    else localStorage.removeItem(INFERENCE_NODE_KEY);
  }
  aiNodeStore.update((s) => ({ ...s, inferenceNodeId: id }));
}

export function acceptPublicAiAgreement() {
  if (typeof localStorage !== "undefined") {
    localStorage.setItem("omd_public_ai_ack", JSON.stringify({ version: AI_NODE_AGREEMENT_VERSION, ts: Date.now() }));
  }
  aiNodeStore.update((s) => ({ ...s, ack: true }));
}

// Remote (GunDB) ack, mirrored back into local storage; remote is weaker
// than the local one: only applied when no local ack exists.
export function applyRemotePublicAiAck(payload) {
  if (readAckFromStorage()) return;
  if (!payload) return;
  const version = typeof payload === "object" ? payload.version : payload;
  if (version !== AI_NODE_AGREEMENT_VERSION) return;
  if (typeof localStorage !== "undefined") {
    localStorage.setItem("omd_public_ai_ack", JSON.stringify({ version: AI_NODE_AGREEMENT_VERSION, ts: Date.now() }));
  }
  aiNodeStore.update((s) => ({ ...s, ack: true }));
}

export function publicAiAckPayload() {
  if (typeof localStorage === "undefined") return null;
  try {
    return JSON.parse(localStorage.getItem("omd_public_ai_ack") || "null");
  } catch (_) {
    return null;
  }
}

// Ask the gateway for the connected system AI node. Calls repeating too
// quickly share the info from the store; otherwise hits /api/ai-node.
let _fetchingInfo = null;
export async function refreshPublicAiNode(force = false) {
  const current = get_store_value();
  if (current.info && !force) return current.info;
  if (_fetchingInfo) return _fetchingInfo;
  _fetchingInfo = (async () => {
    try {
      const gatewayBase = getGatewayBase();
      const resp = await fetch(`${gatewayBase}/api/ai-node`, {
        method: "POST",
        credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({}),
      });
      if (!resp.ok) return null;
      const data = await resp.json();
      const info = data && data.aiNode && data.aiNode.linkID ? data.aiNode : null;
      if (info && info.linkID) {
        if (typeof localStorage !== "undefined") {
          try { localStorage.setItem("omd_public_ai_link_id", info.linkID); } catch (_) {}
        }
        if (typeof window !== "undefined") {
          window.__publicNodeLinkId = info.linkID;
          window.__publicAiNodeLinkId = info.linkID;
        }
      }
      aiNodeStore.update((s) => ({ ...s, info }));
      return info;
    } catch (e) {
      console.warn("[ai-node] failed to fetch public AI node info:", e);
      return null;
    } finally {
      _fetchingInfo = null;
    }
  })();
  return _fetchingInfo;
}

function get_store_value() {
  let v = null;
  const unsub = aiNodeStore.subscribe((value) => { v = value; });
  unsub();
  return v;
}
