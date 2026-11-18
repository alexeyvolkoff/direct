import { writable, get } from "svelte/store";

const STORAGE_KEY = "onmychat_mode";

function readInitialMode() {
    if (typeof window === "undefined") return "work";
    try {
        return localStorage.getItem(STORAGE_KEY) === "fun" ? "fun" : "work";
    } catch {
        return "work";
    }
}

export const contentModeStore = writable(readInitialMode());
export const nsfwUnblurStore = writable(false);

export function toggleNsfwUnblur({ syncContentMode = false } = {}) {
    const next = !get(nsfwUnblurStore);
    nsfwUnblurStore.set(next);
    if (syncContentMode) {
        setGlobalContentMode(next ? "fun" : "work");
    }
    return next;
}

if (typeof window !== "undefined") {
    window.addEventListener("storage", (e) => {
        if (e.key === STORAGE_KEY) {
            contentModeStore.set(e.newValue === "fun" ? "fun" : "work");
        }
    });
    window.addEventListener("onmychat-mode-changed", (e) => {
        const m = e?.detail?.mode;
        if (m === "fun" || m === "work") {
            contentModeStore.set(m);
        }
    });
}

export function setGlobalContentMode(mode) {
    const normalized = mode === "fun" ? "fun" : "work";
    if (typeof window !== "undefined") {
        try {
            localStorage.setItem(STORAGE_KEY, normalized);
        } catch {
            // ignore storage quota errors
        }
        contentModeStore.set(normalized);
        window.dispatchEvent(new CustomEvent("onmychat-mode-changed", { detail: { mode: normalized } }));
    } else {
        contentModeStore.set(normalized);
    }
    return normalized;
}

const SENSITIVE_TAG_NAMES = new Set(["sensitive", "nsfw", "18plus", "explicit"]);
const SENSITIVE_HASHTAG_RE = /(?:^|[\s([{#<])#(sensitive|nsfw|18plus|explicit)\b/i;

const SFW_GUARD_STRIP_RE =
    /\b(?:modest\s+(?:opaque\s+)?buttoned\s+(?:outfit|clothing)|modest\s+opaque\s+clothing|covered\s+chest|wholesome\s+(?:safe\s+)?attire|plain\s+unprinted\s+clothing|rating_safe|sfw)\b/gi;

const SENSITIVE_TEXT_MARKERS_RE =
    /\b(?:nsfw|uncensored|nude|nudity|naked|topless|nipples?|pokies|areolas?|underboob|sideboob|exposed\s+(?:breasts?|chest|buttocks?|genitals?|genitalia)|bare\s+(?:breasts?|chest|buttocks?|body)|uncovered\s+breasts?|deep\s+cleavage|unbuttoned\s+(?:shirt|blouse)|open\s+(?:shirt|blouse)\s+exposing|sheer\s+(?:lingerie|transparent|see[- ]through)|see[- ]through\s+(?:top|blouse|lingerie|dress|bra)|lingerie|thong|panties|micro[- ]?bikini|pussy|vagina|penis|cock|dick|anus|anal|blowjob|fellatio|cunnilingus|masturbat\w*|orgasm|cumshot|threesome|making\s+love|intercourse|erotic|sexually\s+explicit|hentai|ecchi|bdsm|bondage)\b|(?:обнажен|голая|голый|голые|топлес|соски|сосков|сиськ|интимн\w+\s+мест|пизд|влагалищ|пенис|минет|оргазм|мастурб|эротич|эротик|кружевн\w+\s+бель|нижне\w+\s+бель|стринг|трусик)/i;

export function hasSensitiveTag(input) {
    if (!input) return false;
    if (typeof input === "string") {
        return SENSITIVE_HASHTAG_RE.test(input);
    }
    if (Array.isArray(input)) {
        return input.some((t) => SENSITIVE_TAG_NAMES.has(String(t || "").trim().replace(/^#+/, "").toLowerCase()));
    }
    if (typeof input === "object") {
        if (input.sensitive === true || input.isSensitive === true) return true;
        if (input.customAttributes?.sensitive === true) return true;
        if (Array.isArray(input.tags) && hasSensitiveTag(input.tags)) return true;
        if (typeof input.tags === "string" && input.tags) {
            const parts = input.tags.split(/[,\s]+/).filter(Boolean);
            if (hasSensitiveTag(parts)) return true;
        }
        if (typeof input.text === "string" && SENSITIVE_HASHTAG_RE.test(input.text)) return true;
        if (typeof input.readme === "string" && hasSensitiveReadmeContent(input.readme)) return true;
    }
    return false;
}

export function hasSensitiveReadmeContent(readmeText) {
    if (!readmeText || typeof readmeText !== "string") return false;
    if (SENSITIVE_HASHTAG_RE.test(readmeText)) return true;
    const cleaned = readmeText.replace(SFW_GUARD_STRIP_RE, " ");
    return SENSITIVE_TEXT_MARKERS_RE.test(cleaned);
}

export function stripSensitiveHashtags(text) {
    if (!text || typeof text !== "string") return "";
    return text
        .replace(/(?:^|\s)#(?:sensitive|nsfw|18plus|explicit)\b/gi, " ")
        .replace(/\n{3,}/g, "\n\n")
        .trim();
}
