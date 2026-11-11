import { getGatewayOrigin, DEFAULT_GATEWAY_ORIGIN } from "./gateway-config.js";

export const DEFAULT_PUBLIC_ORIGIN = DEFAULT_GATEWAY_ORIGIN;

export function publicOrigin() {
  return getGatewayOrigin();
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
