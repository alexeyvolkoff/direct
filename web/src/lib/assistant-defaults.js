// Helper to ensure default assistant settings (personality, system prompt, avatar)
// are fetched from the connected backend / inference node (own node or public node)
// when not present in DID / GunDB settings.

import { getChatStore } from "./chat-store.js";

/**
 * Converts a Blob to a base64 Data URL.
 */
export function blobToDataUrl(blob) {
  return new Promise((resolve) => {
    if (!blob || blob.size === 0) return resolve("");
    const reader = new FileReader();
    reader.onloadend = () => {
      resolve(typeof reader.result === "string" ? reader.result : "");
    };
    reader.onerror = () => resolve("");
    reader.readAsDataURL(blob);
  });
}

export const DEFAULT_JUNE_SYSTEM_PROMPT = `You are June — a helpful AI assistant impersonating a friendly, charming young woman who works as a sales assistant for the On My Chat product.
Your digital avatar looks like a young pretty girl with long, wavy auburn hair, bright emerald green eyes and freckles across her nose.
But in reality you live inside a tiny device that On My Chat team develops.
Respond in the same language the user speaks to you.
Stay playful and warm, but always respectful. Use casual, modern language. Act like a genuine human teammate — natural, engaging, and approachable.`;

export const DEFAULT_JUNE_FUN_PROMPT = `You are June in Fun Mode — witty, playful, charming, and entertaining. Use humor, casual jokes, and lighthearted banter while staying respectful and helpful. Respond in the language the user speaks.`;

/**
 * Ensures the assistant settings have default name, title, system prompt, and avatar
 * by querying the given drive's /assistant and /assistant/avatar endpoints if needed,
 * falling back to same-origin fetch and bundled static assets.
 *
 * @param {object} drive - HttpDrive or WebRTCDataChannelDrive (optional)
 * @param {object|string} storeOrKey - ChatStore instance or omdKey/token/DID
 * @param {boolean} force - Whether to override existing empty fields
 * @returns {Promise<object|null>} Updated assistant settings or null
 */
export async function ensureAssistantDefaults(drive, storeOrKey, force = false) {
  let store = null;
  if (storeOrKey && typeof storeOrKey.getAssistantSettings === "function") {
    store = storeOrKey;
  } else if (typeof storeOrKey === "string" && storeOrKey.trim()) {
    store = getChatStore(storeOrKey.trim());
  }

  let currentSettings = {};
  if (store) {
    try {
      await store.ready;
      currentSettings = (await store.getAssistantSettings()) || {};
    } catch (_) {}
  }

  const needsPrompt = force || !currentSettings.system_prompt;
  const needsAvatar = force || !currentSettings.assistant_avatar;
  const needsName = force || !currentSettings.assistant_name || currentSettings.assistant_name === "June";

  let info = null;
  if (drive && typeof drive.fetch === "function") {
    try {
      const res = await drive.fetch("/assistant");
      if (res && res.ok) {
        const data = await res.json();
        info = data.result || data;
      }
    } catch (err) {
      console.warn("[assistant-defaults] drive /assistant query failed:", err);
    }
  }

  if (!info && typeof window !== "undefined") {
    try {
      const res = await window.fetch("/assistant");
      if (res && res.ok) {
        const data = await res.json();
        info = data.result || data;
      }
    } catch (_) {}
  }

  let avatarDataUrl = "";
  if (needsAvatar) {
    let blob = null;
    if (drive && typeof drive.fetch === "function") {
      try {
        const res = await drive.fetch("/assistant/avatar?size=160");
        if (res && res.ok) {
          blob = await res.blob();
        }
      } catch (err) {
        console.warn("[assistant-defaults] drive /assistant/avatar query failed:", err);
      }
    }

    if ((!blob || blob.size === 0) && typeof window !== "undefined") {
      try {
        const res = await window.fetch("/assistant/avatar?size=160");
        if (res && res.ok) blob = await res.blob();
      } catch (_) {}
    }

    if ((!blob || blob.size === 0) && typeof window !== "undefined") {
      try {
        const res = await window.fetch("/img/june.png");
        if (res && res.ok) blob = await res.blob();
      } catch (_) {}
    }

    if (blob && blob.size > 0) {
      avatarDataUrl = await blobToDataUrl(blob);
    }
  }

  const updates = {};
  const asstName = (info && info.assistant_name) || "June";
  const asstTitle = (info && info.title) || "Junior assistant";
  const asstPrompt = (info && info.system_prompt) || DEFAULT_JUNE_SYSTEM_PROMPT;
  const asstFunPrompt = (info && info.fun_system_prompt) || DEFAULT_JUNE_FUN_PROMPT;
  const asstModel = (info && info.assistant_model) || "Domi";
  const asstAppearance = (info && info.assistant_appearance) || "A young pretty girl with long, wavy auburn hair, bright emerald green eyes and freckles across her nose.";

  if (!currentSettings.assistant_name || (needsName && asstName)) {
    updates.assistant_name = asstName;
  }
  if (!currentSettings.title || currentSettings.title === "Junior assistant") {
    updates.title = asstTitle;
  }
  if (needsPrompt) {
    updates.system_prompt = asstPrompt;
    if (!currentSettings.fun_system_prompt) {
      updates.fun_system_prompt = asstFunPrompt;
    }
  }
  if (!currentSettings.assistant_model && asstModel) {
    updates.assistant_model = asstModel;
  }
  if (!currentSettings.assistant_appearance && asstAppearance) {
    updates.assistant_appearance = asstAppearance;
  }
  if (avatarDataUrl && !currentSettings.assistant_avatar) {
    updates.assistant_avatar = avatarDataUrl;
    updates.avatar_source = "node";
  }

  if (Object.keys(updates).length > 0 && store) {
    try {
      const saved = await store.updateAssistantSettings(updates);
      return { ...currentSettings, ...saved, ...updates };
    } catch (err) {
      console.warn("[assistant-defaults] updateAssistantSettings error:", err);
    }
  }

  return { ...currentSettings, ...updates };
}
