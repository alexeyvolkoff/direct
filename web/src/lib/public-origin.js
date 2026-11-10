export const DEFAULT_PUBLIC_ORIGIN = "https://disk.direct";

export function publicOrigin() {
  if (typeof window === "undefined") return DEFAULT_PUBLIC_ORIGIN;
  const host = window.location.hostname;
  if (!host || host === "localhost" || host === "127.0.0.1" || host === "[::1]") {
    return DEFAULT_PUBLIC_ORIGIN;
  }
  return window.location.origin;
}

export function sharedLinkUrl(address) {
  return `${publicOrigin()}/shared/${address}`;
}

export function sanitizeShareUrl(url, address = "") {
  if (typeof url !== "string" || !url) {
    return address ? sharedLinkUrl(address) : "";
  }
  try {
    const u = new URL(url);
    if (!u.hostname || u.hostname === "localhost" || u.hostname === "127.0.0.1" || u.hostname === "[::1]") {
      return address ? sharedLinkUrl(address) : "";
    }
    return u.origin + u.pathname + u.search;
  } catch (_) {
    return address ? sharedLinkUrl(address) : "";
  }
}
