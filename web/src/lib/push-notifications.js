/**
 * Unified Rich Push Notifications helper for Svar PWA & Capacitor Mobile (Android / iOS).
 * - Web / Mobile PWA: Direct Peer-to-Peer W3C Web Push (RFC 8291 VAPID) via Service Worker.
 * - Capacitor Native (Android APK / iOS IPA): @capacitor/push-notifications (FCM / APNs)
 *   routed via OMD Push Relay on onmydisk.net.
 */
import { Capacitor } from "@capacitor/core";
import { PushNotifications } from "@capacitor/push-notifications";
import { translate } from "./i18n/index.js";

const NATIVE_SUB_STORAGE_KEY = "omd_native_push_sub";
const DEFAULT_RELAY_URL = "https://disk.direct:8765/api/push/native";
const DEFAULT_PUBLIC_ORIGIN = "https://disk.direct";

let _nativeListenersBound = false;

export function isNativeApp() {
  return typeof window !== "undefined" && Boolean(Capacitor?.isNativePlatform?.());
}

export function isNativePushAvailable() {
  return isNativeApp() && Boolean(Capacitor?.isPluginAvailable?.("PushNotifications"));
}

export function isWebBrowserPushSupported() {
  return (
    typeof window !== "undefined" &&
    !isNativeApp() &&
    "serviceWorker" in navigator &&
    "PushManager" in window &&
    "Notification" in window
  );
}

export function isWebPushSupported() {
  return isNativePushAvailable() || isWebBrowserPushSupported();
}

export function urlBase64ToUint8Array(base64String) {
  const padding = "=".repeat((4 - (base64String.length % 4)) % 4);
  const base64 = (base64String + padding).replace(/-/g, "+").replace(/_/g, "/");
  const rawData = window.atob(base64);
  const outputArray = new Uint8Array(rawData.length);
  for (let i = 0; i < rawData.length; ++i) {
    outputArray[i] = rawData.charCodeAt(i);
  }
  return outputArray;
}

function getSavedNativeSub() {
  if (typeof localStorage === "undefined") return null;
  try {
    const raw = localStorage.getItem(NATIVE_SUB_STORAGE_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    if (parsed && parsed.endpoint && parsed.token && parsed.platform) {
      return parsed;
    }
  } catch (_) {}
  return null;
}

export async function getBrowserPushSubscription() {
  if (isNativePushAvailable()) {
    if (typeof localStorage !== "undefined" && localStorage.getItem("omd_webpush_enabled") !== "1") {
      return null;
    }
    try {
      const perm = await PushNotifications.checkPermissions();
      if (perm?.receive !== "granted") return null;
    } catch (_) {
      return null;
    }
    return getSavedNativeSub();
  }

  if (!isWebBrowserPushSupported()) return null;
  try {
    const reg = await navigator.serviceWorker.ready;
    return await reg.pushManager.getSubscription();
  } catch (_) {
    return null;
  }
}

async function registerNativeDeviceToken(timeoutMs = 14000) {
  let perm = await PushNotifications.checkPermissions();
  if (perm.receive === "prompt" || perm.receive === "prompt-with-rationale") {
    perm = await PushNotifications.requestPermissions();
  }
  if (perm.receive !== "granted") {
    throw new Error(translate("push_err_perm_phone", {}, "Notification permission is disabled in device settings."));
  }

  return new Promise(async (resolve, reject) => {
    let settled = false;
    let regHandle = null;
    let errHandle = null;

    const cleanup = () => {
      if (regHandle && typeof regHandle.remove === "function") {
        try { regHandle.remove(); } catch (_) {}
      }
      if (errHandle && typeof errHandle.remove === "function") {
        try { errHandle.remove(); } catch (_) {}
      }
    };

    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      cleanup();
      reject(
        new Error(
          Capacitor.getPlatform() === "android"
            ? translate("push_err_fcm_timeout", {}, "Timed out waiting for FCM token (ensure Google Play Services are available).")
            : translate("push_err_apns_timeout", {}, "Timed out waiting for iOS APNs token.")
        )
      );
    }, timeoutMs);

    try {
      regHandle = await PushNotifications.addListener("registration", (token) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        cleanup();
        if (token && token.value) {
          resolve(String(token.value));
        } else {
          reject(new Error(translate("push_err_empty_token", {}, "Received an empty device Push token.")));
        }
      });

      errHandle = await PushNotifications.addListener("registrationError", (err) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        cleanup();
        reject(new Error(translate("push_err_reg_failed", { error: err?.error || JSON.stringify(err) }, "Push registration error: {error}")));
      });

      await PushNotifications.register();
    } catch (e) {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        cleanup();
        reject(e);
      }
    }
  });
}

export async function subscribeToNodeWebPush({
  fetchFn,
  omdKey = "",
  defaultStorage = "",
  deviceLabel = ""
}) {
  const effectiveFetch = fetchFn || ((p, opts) => window.fetch(p, opts));

  // ── Path A: Native Android / iOS App (Capacitor) ──
  if (isNativeApp()) {
    if (!isNativePushAvailable()) {
      throw new Error(translate("push_err_update_app", {}, "Update the On My Disk mobile app to enable native Push notifications."));
    }
    const platform = Capacitor.getPlatform(); // 'android' | 'ios'
    const token = await registerNativeDeviceToken();
    const endpoint = `native://${platform}/${token}`;
    const subJson = {
      kind: "native",
      platform,
      token,
      endpoint,
      relay_url: DEFAULT_RELAY_URL,
      public_origin: DEFAULT_PUBLIC_ORIGIN
    };
    const autoLabel =
      deviceLabel || (platform === "ios" ? "iPhone / iPad App" : "Android App");

    const saveRes = await effectiveFetch("/push/subscribe", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        subscription: subJson,
        omd_key: omdKey || "",
        default_storage: defaultStorage || "",
        device_label: autoLabel
      })
    });
    if (!saveRes || !saveRes.ok) {
      throw new Error(translate("push_err_save_token_node", {}, "Failed to register Push token on AI node."));
    }
    const saveData = await saveRes.json();
    try {
      localStorage.setItem(NATIVE_SUB_STORAGE_KEY, JSON.stringify(subJson));
      localStorage.setItem("omd_webpush_enabled", "1");
    } catch (_) {}
    return { subscription: subJson, ...saveData };
  }

  // ── Path B: Browser / Mobile PWA (W3C Web Push + VAPID) ──
  if (!isWebBrowserPushSupported()) {
    throw new Error(translate("push_err_browser_unsupported", {}, "Your browser does not support Web Push notifications (on iOS, add the app to your Home Screen)."));
  }

  const perm = await Notification.requestPermission();
  if (perm !== "granted") {
    throw new Error(translate("push_err_perm_browser", {}, "Notification permission was denied in browser settings."));
  }

  const keyRes = await effectiveFetch("/push/vapid-key");
  if (!keyRes || !keyRes.ok) {
    throw new Error(translate("push_err_vapid_fetch", {}, "Failed to fetch VAPID key from AI node (check node connection)."));
  }
  const keyData = await keyRes.json();
  const publicKey = keyData && keyData.publicKey;
  if (!publicKey) {
    throw new Error(translate("push_err_vapid_empty", {}, "AI node returned an empty VAPID key."));
  }

  const reg = await navigator.serviceWorker.ready;
  let sub = await reg.pushManager.getSubscription();
  const appServerKey = urlBase64ToUint8Array(publicKey);

  if (sub) {
    try {
      const existingKey = sub.options && sub.options.applicationServerKey;
      if (existingKey) {
        const existingArr = new Uint8Array(existingKey);
        const mismatch =
          existingArr.length !== appServerKey.length ||
          existingArr.some((b, idx) => b !== appServerKey[idx]);
        if (mismatch) {
          await sub.unsubscribe();
          sub = null;
        }
      }
    } catch (_) {}
  }

  if (!sub) {
    sub = await reg.pushManager.subscribe({
      userVisibleOnly: true,
      applicationServerKey: appServerKey
    });
  }

  const subJson = sub.toJSON();
  const autoLabel =
    deviceLabel ||
    (/Android/i.test(navigator.userAgent)
      ? "Android PWA"
      : /iPhone|iPad/i.test(navigator.userAgent)
      ? "iOS PWA"
      : "Desktop Browser");

  const saveRes = await effectiveFetch("/push/subscribe", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      subscription: subJson,
      omd_key: omdKey || "",
      default_storage: defaultStorage || "",
      device_label: autoLabel
    })
  });
  if (!saveRes || !saveRes.ok) {
    throw new Error(translate("push_err_save_sub_node", {}, "Failed to register Push subscription on AI node."));
  }
  const saveData = await saveRes.json();
  try {
    localStorage.setItem("omd_webpush_enabled", "1");
  } catch (_) {}
  return { subscription: subJson, ...saveData };
}

export async function syncExistingWebPushSubscription({
  fetchFn,
  omdKey = "",
  defaultStorage = ""
}) {
  if (!fetchFn) return false;
  if (typeof localStorage !== "undefined" && localStorage.getItem("omd_webpush_enabled") !== "1") {
    return false;
  }

  if (isNativePushAvailable()) {
    try {
      const perm = await PushNotifications.checkPermissions();
      if (perm?.receive !== "granted") return false;
      // Re-register silently to pick up any refreshed FCM/APNs token
      const token = await registerNativeDeviceToken(8000);
      const platform = Capacitor.getPlatform();
      const subJson = {
        kind: "native",
        platform,
        token,
        endpoint: `native://${platform}/${token}`,
        relay_url: DEFAULT_RELAY_URL,
        public_origin: DEFAULT_PUBLIC_ORIGIN
      };
      localStorage.setItem(NATIVE_SUB_STORAGE_KEY, JSON.stringify(subJson));
      await fetchFn("/push/subscribe", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          subscription: subJson,
          omd_key: omdKey || "",
          default_storage: defaultStorage || ""
        })
      });
      return true;
    } catch (_) {
      return false;
    }
  }

  if (!isWebBrowserPushSupported()) return false;
  if (typeof Notification === "undefined" || Notification.permission !== "granted") return false;
  try {
    const sub = await getBrowserPushSubscription();
    if (!sub) return false;
    await fetchFn("/push/subscribe", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        subscription: sub.toJSON ? sub.toJSON() : sub,
        omd_key: omdKey || "",
        default_storage: defaultStorage || ""
      })
    });
    return true;
  } catch (_) {
    return false;
  }
}

export async function unsubscribeFromNodeWebPush({ fetchFn, omdKey = "" }) {
  const effectiveFetch = fetchFn || ((p, opts) => window.fetch(p, opts));

  if (isNativePushAvailable()) {
    const saved = getSavedNativeSub();
    if (saved && saved.endpoint) {
      try {
        await effectiveFetch("/push/unsubscribe", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ endpoint: saved.endpoint, omd_key: omdKey || "" })
        });
      } catch (_) {}
    }
    try {
      await PushNotifications.unregister();
    } catch (_) {}
    try {
      localStorage.removeItem(NATIVE_SUB_STORAGE_KEY);
      localStorage.removeItem("omd_webpush_enabled");
    } catch (_) {}
    return true;
  }

  const sub = await getBrowserPushSubscription();
  if (sub) {
    const endpoint = sub.endpoint;
    try {
      await effectiveFetch("/push/unsubscribe", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ endpoint, omd_key: omdKey || "" })
      });
    } catch (_) {}
    if (typeof sub.unsubscribe === "function") {
      try {
        await sub.unsubscribe();
      } catch (_) {}
    }
  }
  try {
    localStorage.removeItem("omd_webpush_enabled");
  } catch (_) {}
  return true;
}

export async function sendTestNodeWebPush({ fetchFn, omdKey = "", defaultStorage = "" }) {
  const effectiveFetch = fetchFn || ((p, opts) => window.fetch(p, opts));
  const sub = await getBrowserPushSubscription();
  const res = await effectiveFetch("/push/test", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      endpoint: sub ? sub.endpoint : "",
      omd_key: omdKey || "",
      default_storage: defaultStorage || ""
    })
  });
  if (!res || !res.ok) {
    const err = await res.json().catch(() => ({}));
    throw new Error(err.detail || translate("push_err_test_failed", {}, "Failed to send test Push notification."));
  }
  return await res.json();
}

function normalizeNativeNotificationPayload(notif) {
  const raw = notif?.data || {};
  if (raw.payload_json && typeof raw.payload_json === "string") {
    try {
      const parsed = JSON.parse(raw.payload_json);
      if (parsed && typeof parsed === "object") return parsed;
    } catch (_) {}
  }
  const innerData = raw.data && typeof raw.data === "object" ? raw.data : raw;
  return {
    title: notif?.title || raw.title || "",
    body: notif?.body || raw.body || "",
    image: raw.image || innerData.image || "",
    data: innerData
  };
}

export function initNativePushListeners(onPushAction, onForegroundBanner) {
  if (!isNativePushAvailable() || _nativeListenersBound) return;
  _nativeListenersBound = true;

  PushNotifications.addListener("pushNotificationActionPerformed", (event) => {
    const rawAction = String(event?.actionId || "tap").toLowerCase();
    const mappedAction = rawAction === "tap" ? "open" : rawAction;
    const payload = normalizeNativeNotificationPayload(event?.notification || {});
    if (typeof onPushAction === "function") {
      onPushAction(mappedAction, payload);
    }
  });

  PushNotifications.addListener("pushNotificationReceived", (notif) => {
    const payload = normalizeNativeNotificationPayload(notif || {});
    if (payload?.data?.widget_url && typeof onPushAction === "function") {
      onPushAction("open", payload);
    } else if (typeof onForegroundBanner === "function") {
      onForegroundBanner(payload);
    }
  });
}
