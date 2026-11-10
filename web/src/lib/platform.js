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

export function getClientPlatform() {
  if (isIosNative()) return "ios";
  if (isAndroidNative()) return "android";
  return "web";
}

export function isExternalCheckoutUrl(url) {
  if (!url || typeof url !== "string") return false;
  return /whop\.com/i.test(url);
}

/**
 * Strips external digital-purchase URLs & upsell CTAs on native iOS
 * to comply with Apple App Store Review Guideline 3.1.1.
 */
export function sanitizeExternalCheckoutText(text) {
  if (!text || typeof text !== "string") return text || "";
  if (!isIosNative()) return text;
  if (!/whop\.com|club\s*pass|top[\s-]?up|пополн|кредит/i.test(text)) {
    return text;
  }
  const hasCyrillic = /[\u0400-\u04FF]/.test(text);
  // Remove raw whop URLs and external checkout CTA lines
  const lines = text
    .split(/\n+/)
    .map((l) => l.replace(/https?:\/\/[^\s)\]]*whop\.com[^\s)\]]*/gi, "").trim())
    .filter((l) => l && !/(join the studio|top up|присоединяйтесь к студии|вступайте в|пополните кредиты|whop\.com)/i.test(l));
  if (lines.length > 0) {
    return lines.join("\n").replace(/[:\s]+$/, "");
  }
  return hasCyrillic
    ? "✨ Расширенный студийный режим доступен при подключении собственного локального ИИ-узла в Настройках → ИИ."
    : "✨ Studio Pro mode is available when connected to your own self-hosted AI node in Settings → AI.";
}

