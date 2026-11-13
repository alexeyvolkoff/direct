import { parseStoragePath, DEFAULT_STORAGE_PATH } from "./storage-path.js";

function guessImageMime(filePath, currentType = "") {
  if (currentType && currentType.startsWith("image/")) return currentType;
  const lower = String(filePath || "").toLowerCase();
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
  if (lower.endsWith(".webp")) return "image/webp";
  if (lower.endsWith(".gif")) return "image/gif";
  return "image/png";
}

async function readDriveText(drive, filePath) {
  if (!drive || !filePath) return null;
  try {
    if (typeof drive.getFileContent === "function") {
      const blob = await drive.getFileContent(filePath);
      if (blob && typeof blob.text === "function") {
        return await blob.text();
      }
    } else if (typeof drive.getFile === "function") {
      const c = await drive.getFile(filePath);
      return typeof c === "string" ? c : JSON.stringify(c);
    }
  } catch (_) {}
  return null;
}

async function readDriveBlobAsDataUrl(drive, filePath) {
  if (!drive || !filePath) return null;
  try {
    if (typeof drive.getFileContent === "function") {
      const rawBlob = await drive.getFileContent(filePath);
      if (rawBlob && rawBlob.size > 0) {
        const mime = guessImageMime(filePath, rawBlob.type);
        const blob = rawBlob.type === mime ? rawBlob : new Blob([rawBlob], { type: mime });
        return await new Promise((resolve) => {
          const reader = new FileReader();
          reader.onloadend = () => resolve(reader.result || null);
          reader.onerror = () => resolve(null);
          reader.readAsDataURL(blob);
        });
      }
    }
  } catch (_) {}
  return null;
}

export function resolvePersonasFolder({ defaultStorage = "", userName = "" } = {}) {
  const raw = (
    defaultStorage ||
    (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
    (typeof window !== "undefined" && window.__omdProfile?.defaultStorage) ||
    DEFAULT_STORAGE_PATH ||
    "/Data"
  ).trim();

  const parsed = (parseStoragePath && parseStoragePath(raw)) || {};
  let clean = (parsed.folder || parsed.subpath || raw || "").replace(/^\/+|\/+$/g, "");

  const knownUsers = [
    userName,
    typeof window !== "undefined" && window.__omdProfile?.user,
    typeof window !== "undefined" && window.__omdProfile?.name,
  ].filter(Boolean);

  for (const u of knownUsers) {
    const uLower = String(u).toLowerCase();
    if (clean.toLowerCase() === uLower) {
      clean = (DEFAULT_STORAGE_PATH || "/Data").replace(/^\/+/, "");
      break;
    } else if (clean.toLowerCase().startsWith(uLower + "/")) {
      clean = clean.slice(String(u).length).replace(/^\/+/, "");
    }
  }

  clean = clean.replace(/^(home|Users)\/[^/]+(\/|$)/i, "").replace(/^\/+|\/+$/g, "");
  clean = clean.replace(/^private\//i, "");
  if (clean.endsWith("/generated")) {
    clean = clean.slice(0, -"/generated".length);
  } else if (clean.endsWith("generated")) {
    clean = clean.slice(0, -"generated".length);
  }
  clean = clean.replace(/^\/+|\/+$/g, "");

  if (!clean) {
    clean = (DEFAULT_STORAGE_PATH || "/Data").replace(/^\/+/, "");
  }
  return `/${clean || "Data"}/personas`;
}

export function isCustomPersona(p) {
  if (!p || typeof p !== "object") return false;
  const id = String(p.id || "").trim().toLowerCase();
  return Boolean(id && id !== "june" && id !== "default" && id !== "base");
}

export function compactPersonaForChat(persona) {
  if (!isCustomPersona(persona)) return null;
  const isPersistableUrl = (u) => typeof u === "string" && u && !u.startsWith("data:") && !u.startsWith("blob:");
  const avatarUrl = isPersistableUrl(persona.avatar_url)
    ? persona.avatar_url
    : isPersistableUrl(persona.avatar)
    ? persona.avatar
    : "";
  return {
    id: persona.id,
    name: persona.name || "",
    title: persona.title || "",
    prompt: persona.prompt || "",
    personality: persona.personality || "",
    archetype: persona.archetype || "",
    lora: persona.lora || "",
    render_preset: persona.render_preset || null,
    avatar_url: avatarUrl,
  };
}

export function resolveFullPersona(chatPersonaOrId, availablePersonas = []) {
  if (!chatPersonaOrId) return null;
  const id = typeof chatPersonaOrId === "string" ? chatPersonaOrId : chatPersonaOrId.id;
  if (!id || id === "june" || id === "default" || id === "base") return null;

  const fromList = Array.isArray(availablePersonas)
    ? availablePersonas.find((p) => p && p.id === id)
    : null;
  const baseObj = typeof chatPersonaOrId === "object" ? chatPersonaOrId : { id };

  if (!fromList) {
    return {
      ...baseObj,
      avatar: baseObj.avatar || baseObj.avatar_url || "",
    };
  }
  return {
    ...baseObj,
    ...fromList,
    avatar: fromList.avatarData || fromList.avatar || baseObj.avatar || baseObj.avatar_url || "",
    avatarData: fromList.avatarData || (typeof fromList.avatar === "string" && fromList.avatar.startsWith("data:") ? fromList.avatar : ""),
  };
}

export async function loadAvailablePersonas({
  httpDrive = null,
  deviceDrive = null,
  defaultStorage = "",
  userName = "",
} = {}) {
  let loaded = [];
  const pFolder = resolvePersonasFolder({ defaultStorage, userName });
  const fetchFn =
    httpDrive && typeof httpDrive.fetch === "function"
      ? (p, opts) => httpDrive.fetch(p, opts)
      : (p, opts) => fetch(p, opts);

  try {
    const res = await fetchFn("/personas");
    if (res && res.ok) {
      const data = await res.json();
      if (Array.isArray(data.personas) && data.personas.length > 0) {
        loaded = data.personas;
      }
    }
  } catch (_) {}

  if (deviceDrive && !deviceDrive.isConnected && typeof deviceDrive.waitUntilConnected === "function") {
    await deviceDrive.waitUntilConnected(1500).catch(() => {});
  }

  if (deviceDrive && (deviceDrive.isConnected || deviceDrive.status === "connected")) {
    try {
      let files = [];
      if (typeof deviceDrive.getFiles === "function") {
        files = await deviceDrive.getFiles(pFolder);
      } else if (typeof deviceDrive.list === "function") {
        files = await deviceDrive.list(pFolder);
      }
      if (Array.isArray(files)) {
        const jsonFiles = files.filter((f) => {
          const name = f?.name || f?.value || f;
          return typeof name === "string" && name.endsWith(".json") && !name.startsWith(".");
        });
        const drivePersonas = [];
        for (const jf of jsonFiles) {
          const fname = jf?.name || jf?.value || jf;
          try {
            const text = await readDriveText(deviceDrive, `${pFolder}/${fname}`);
            if (text) {
              const pData = JSON.parse(text);
              const baseName = fname.replace(/\.json$/, "");
              const avatarData = await readDriveBlobAsDataUrl(deviceDrive, `${pFolder}/${baseName}.png`);
              if (avatarData) {
                pData.avatar = avatarData;
                pData.avatarData = avatarData;
              }
              drivePersonas.push(pData);
            }
          } catch (_) {}
        }
        if (drivePersonas.length > 0) {
          const driveIds = new Set(drivePersonas.map((p) => p.id));
          loaded = [...drivePersonas, ...loaded.filter((p) => !driveIds.has(p.id))];
        }
      }
    } catch (_) {}
  }

  for (const p of loaded) {
    if (
      p &&
      typeof p.avatar === "string" &&
      (p.avatar.startsWith("/personas/") || p.avatar.startsWith("/assistant/avatar")) &&
      !p.avatar.startsWith("data:")
    ) {
      try {
        const avRes = await fetchFn(p.avatar);
        if (avRes && avRes.ok) {
          const blob = await avRes.blob();
          if (blob && blob.size > 0) {
            const b64 = await new Promise((resolve) => {
              const reader = new FileReader();
              reader.onload = () => resolve(reader.result);
              reader.onerror = () => resolve(null);
              reader.readAsDataURL(blob);
            });
            if (b64) {
              p.avatar = b64;
              p.avatarData = b64;
            }
          }
        }
      } catch (_) {}
    }
  }

  const customPersonas = loaded.filter(isCustomPersona);
  return {
    allPersonas: loaded,
    customPersonas,
  };
}

