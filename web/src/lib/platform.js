import { Capacitor } from "@capacitor/core";

/**
 * True only when running inside the native iOS Capacitor wrapper (WKWebView).
 * Mobile Safari / PWA on iOS returns false so web functionality stays untouched.
 */
export function isIosNative() {
  if (typeof window === "undefined") return false;
  try {
    return Boolean(Capacitor?.isNativePlatform?.()) && Capacitor.getPlatform() === "ios";
  } catch (_) {
    return false;
  }
}

export function isAndroidNative() {
  if (typeof window === "undefined") return false;
  try {
    return Boolean(Capacitor?.isNativePlatform?.()) && Capacitor.getPlatform() === "android";
  } catch (_) {
    return false;
  }
}

export function isNativeMobile() {
  if (typeof window === "undefined") return false;
  try {
    return Boolean(Capacitor?.isNativePlatform?.()) || isIosNative() || isAndroidNative();
  } catch (_) {
    return false;
  }
}

export function getClientPlatform() {
  if (isIosNative()) return "ios";
  if (isAndroidNative()) return "android";
  return "web";
}

/**
 * True only in a standard web browser hosted on the primary Disk Direct / AI Assistant web gateway.
 * Always false on iOS, Android, and third-party / self-hosted gateways.
 */
export function isPrimaryWebGateway() {
  if (typeof window === "undefined" || isNativeMobile()) return false;
  const h = String(window.location?.hostname || "").toLowerCase();
  return (
    h === "disk.direct" ||
    h.endsWith(".disk.direct") ||
    h === "aiassistant.direct" ||
    h.endsWith(".aiassistant.direct")
  );
}

const EXTERNAL_CHECKOUT_PATH_RE = /\/(?:checkout|creative-studio-direct|sponsorship-program)(?:\/|$|\?)/i;
const EXTERNAL_UPSELL_TEXT_RE = /(?:\/checkout\/|creative-studio-direct|sponsorship-program|top[\s-]?up|пополн\S*\s+кредит|join\s+the\s+studio|присоединяйтесь\s+к\s+студии|вступайте\s+в)/i;

export function isExternalCheckoutUrl(url) {
  if (!url || typeof url !== "string") return false;
  return EXTERNAL_CHECKOUT_PATH_RE.test(url);
}

/**
 * Strips external digital-purchase URLs & upsell CTAs on native mobile apps
 * (both iOS and Android) and non-primary gateways.
 */
export function sanitizeExternalCheckoutText(text) {
  if (!text || typeof text !== "string") return text || "";
  if (!isNativeMobile() && isPrimaryWebGateway()) return text;
  if (!EXTERNAL_UPSELL_TEXT_RE.test(text)) {
    return text;
  }
  const hasCyrillic = /[\u0400-\u04FF]/.test(text);
  const lines = text
    .split(/\n+/)
    .map((l) => l.replace(/https?:\/\/[^\s)\]]+/gi, (u) => (EXTERNAL_CHECKOUT_PATH_RE.test(u) ? "" : u)).trim())
    .filter((l) => l && !EXTERNAL_UPSELL_TEXT_RE.test(l));
  if (lines.length > 0) {
    return lines.join("\n").replace(/[:\s]+$/, "");
  }
  return hasCyrillic
    ? "✨ Расширенный режим доступен при подключении собственного локального ИИ-узла в Настройках → ИИ."
    : "✨ Extended mode is available when connected to your own self-hosted AI node in Settings → AI.";
}
