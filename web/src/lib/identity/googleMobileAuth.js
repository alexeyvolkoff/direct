import { Capacitor, registerPlugin } from "@capacitor/core";

export const GOOGLE_WEB_CLIENT_ID =
  "959849209352-vbi0vfimrqj904598felj8ieeb6to8s2.apps.googleusercontent.com";

const HUB_AUTH_URL = "https://direct.onmydisk.net:8765";
const MOBILE_BRIDGE_BASE_URL = "https://direct.onmydisk.net";

const NativeGoogleAuth = registerPlugin("NativeGoogleAuth");

let activeBridgeAbort = null;

export function isNativeMobilePlatform() {
  try {
    return Capacitor.isNativePlatform();
  } catch (_) {
    return false;
  }
}

export function isGoogleSignInCanceledError(err) {
  if (!err) return false;
  if (err.isUserCancel) return true;
  const msg = String(err.message || err.error || err.code || err).toLowerCase();
  return (
    msg.includes("cancel") ||
    msg.includes("user_canceled") ||
    msg.includes("getcredentialcancellationexception") ||
    msg.includes("activity_cancelled") ||
    msg === "16"
  );
}

function makeSessionId() {
  const bytes = new Uint8Array(24);
  if (typeof crypto !== "undefined" && crypto.getRandomValues) {
    crypto.getRandomValues(bytes);
  } else {
    for (let i = 0; i < bytes.length; i++) {
      bytes[i] = Math.floor(Math.random() * 256);
    }
  }
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

function openExternalAuthUrl(url, _isSyncGesture = true) {
  try {
    const a = document.createElement("a");
    a.href = url;
    a.target = "_blank";
    a.rel = "noopener noreferrer";
    a.style.display = "none";
    document.body.appendChild(a);
    a.click();
    setTimeout(() => {
      try {
        document.body.removeChild(a);
      } catch (_) {}
    }, 300);
  } catch (_) {
    window.open(url, "_blank");
  }
}

/**
 * Opens https://direct.onmydisk.net/?mobile_google_auth=<session> in the
 * system browser (Safari on iOS / Chrome on Android) and polls gun-hub for the
 * verified single-use Google ID token.
 *
 * Used automatically on untouched iOS 3.0 binaries (where NativeGoogleAuth is
 * not compiled in) and as a seamless fallback on Android if Play Services / GCP
 * SHA-1 credentials are unavailable.
 */
export function startBrowserOAuthBridge(provider = "google", isSyncGesture = true) {
  if (activeBridgeAbort) {
    try {
      activeBridgeAbort();
    } catch (_) {}
    activeBridgeAbort = null;
  }

  const session = makeSessionId();
  const lang =
    typeof navigator !== "undefined" && navigator.language
      ? navigator.language.slice(0, 5)
      : "en";
  const queryKey = provider === "apple" ? "mobile_apple_auth" : "mobile_google_auth";
  const bridgeUrl = `${MOBILE_BRIDGE_BASE_URL}/?${queryKey}=${encodeURIComponent(session)}&lang=${encodeURIComponent(lang)}`;

  // Open synchronously before any await so iOS WKWebView never popup-blocks it
  openExternalAuthUrl(bridgeUrl, isSyncGesture);

  return new Promise((resolve, reject) => {
    let settled = false;
    let timer = null;
    let pollInFlight = false;
    const startedAt = Date.now();
    const timeoutMs = 3 * 60 * 1000;

    function cleanup() {
      settled = true;
      if (timer) clearInterval(timer);
      timer = null;
      if (typeof document !== "undefined") {
        document.removeEventListener("visibilitychange", onForeground);
      }
      if (typeof window !== "undefined") {
        window.removeEventListener("focus", onForeground);
      }
      if (activeBridgeAbort === abortFn) {
        activeBridgeAbort = null;
      }
    }

    function abortFn() {
      if (settled) return;
      cleanup();
      const cancelErr = new Error(`${provider} sign-in canceled`);
      cancelErr.isUserCancel = true;
      reject(cancelErr);
    }

    activeBridgeAbort = abortFn;

    async function pollOnce() {
      if (settled || pollInFlight) return;
      if (Date.now() - startedAt > timeoutMs) {
        cleanup();
        reject(new Error(`${provider} sign-in timed out`));
        return;
      }
      pollInFlight = true;
      try {
        const resp = await fetch(
          `${HUB_AUTH_URL}/api/auth/mobile-oauth-poll?session=${encodeURIComponent(session)}&t=${Date.now()}`,
          { method: "GET", cache: "no-store" }
        );
        if (!resp.ok) return;
        const data = await resp.json();
        if (settled) return;
        if (data && data.pending === false && data.idToken) {
          cleanup();
          if (provider === "apple") {
            resolve({ idToken: data.idToken, user: data.user || null });
          } else {
            resolve(data.idToken);
          }
        } else if (data && data.error) {
          cleanup();
          reject(new Error(data.error));
        }
      } catch (_) {
        // Network transition while switching between browser and app — keep polling
      } finally {
        pollInFlight = false;
      }
    }

    function onForeground() {
      if (!settled && !document.hidden) {
        pollOnce();
      }
    }

    if (typeof document !== "undefined") {
      document.addEventListener("visibilitychange", onForeground);
    }
    if (typeof window !== "undefined") {
      window.addEventListener("focus", onForeground);
    }

    timer = setInterval(pollOnce, 1200);
  });
}

export function startBrowserGoogleOAuthBridge(isSyncGesture = true) {
  return startBrowserOAuthBridge("google", isSyncGesture);
}

export function signInWithAppleMobile() {
  return startBrowserOAuthBridge("apple", true);
}

/**
 * Signs in with Google on native Capacitor mobile platforms:
 * 1. Uses native Android CredentialManager (NativeGoogleAuth plugin) when compiled into the binary.
 * 2. Falls back seamlessly to the system-browser bridge on https://direct.onmydisk.net
 *    when NativeGoogleAuth is absent (e.g. untouched iOS 3.0 release) or Play Services config fails.
 */
export async function signInWithGoogleMobile() {
  const hasNativeGooglePlugin =
    typeof Capacitor.isPluginAvailable === "function" &&
    Capacitor.isPluginAvailable("NativeGoogleAuth");

  if (!hasNativeGooglePlugin) {
    // Synchronous call preserves user gesture on iOS WKWebView
    return await startBrowserGoogleOAuthBridge(true);
  }

  try {
    const res = await NativeGoogleAuth.signIn({
      webClientId: GOOGLE_WEB_CLIENT_ID,
    });
    const idToken = res?.idToken;
    if (idToken) {
      return idToken;
    }
    throw new Error("No idToken returned from NativeGoogleAuth");
  } catch (err) {
    if (isGoogleSignInCanceledError(err)) {
      throw err;
    }
    console.warn("[GoogleAuth] NativeGoogleAuth unavailable or unconfigured, falling back to browser OAuth bridge:", err?.message || err);
    return await startBrowserGoogleOAuthBridge(false);
  }
}

/**
 * Lightweight standalone Google / Apple OAuth bridge rendered when the system browser
 * opens https://direct.onmydisk.net/?mobile_google_auth=<session> or ?mobile_apple_auth=<session>.
 */
export function mountMobileGoogleBridgeIfRequested(targetEl) {
  if (typeof window === "undefined" || isNativeMobilePlatform()) return false;
  let params;
  try {
    params = new URLSearchParams(window.location.search);
  } catch (_) {
    return false;
  }
  const googleSession = (params.get("mobile_google_auth") || "").trim();
  const appleSession = (params.get("mobile_apple_auth") || "").trim();
  const provider = appleSession ? "apple" : "google";
  const session = appleSession || googleSession;
  if (!session || !/^[a-zA-Z0-9_-]{16,128}$/.test(session)) {
    return false;
  }

  const splash = document.getElementById("omd-app-splash");
  if (splash) splash.style.display = "none";

  const rawLang = (params.get("lang") || navigator.language || "en").toLowerCase();
  const isRu = rawLang.startsWith("ru") || rawLang.startsWith("uk") || rawLang.startsWith("be");
  const isSl = rawLang.startsWith("sl");
  const providerLabel = provider === "apple" ? "Apple" : "Google";

  const copy = isRu
    ? {
        title: "Вход в On My Disk",
        desc: `Продолжите вход через ${providerLabel} для мобильного приложения On My Disk.`,
        btnApple: "Войти через Apple",
        verifying: "Подтверждаем вход...",
        doneTitle: "Вход выполнен!",
        doneDesc: "Возвращаемся в приложение On My Disk...",
        openAppBtn: "Открыть On My Disk",
        errPrefix: "Ошибка авторизации: ",
      }
    : isSl
    ? {
        title: "Prijava v On My Disk",
        desc: `Nadaljujte prijavo prek računa ${providerLabel} za mobilno aplikacijo On My Disk.`,
        btnApple: "Prijava z računom Apple",
        verifying: "Preverjanje prijave...",
        doneTitle: "Prijava uspešna!",
        doneDesc: "Vračanje v aplikacijo On My Disk...",
        openAppBtn: "Odpri On My Disk",
        errPrefix: "Napaka pri prijavi: ",
      }
    : {
        title: "Sign in to On My Disk",
        desc: `Continue with your ${providerLabel} account to sign in to the On My Disk mobile app.`,
        btnApple: "Sign in with Apple",
        verifying: "Verifying sign-in...",
        doneTitle: "Signed in!",
        doneDesc: "Returning to the On My Disk app...",
        openAppBtn: "Open On My Disk",
        errPrefix: "Sign-in error: ",
      };

  const prefersDark =
    window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches;

  targetEl.innerHTML = `
    <div style="min-height:100dvh;display:flex;align-items:center;justify-content:center;padding:24px;box-sizing:border-box;font-family:'Inter',system-ui,-apple-system,sans-serif;background:${prefersDark ? "#101C22" : "#F6F8F9"};color:${prefersDark ? "#F1F5F9" : "#0F172A"};">
      <div style="width:100%;max-width:380px;background:${prefersDark ? "#182630" : "#FFFFFF"};border:1px solid ${prefersDark ? "rgba(255,255,255,0.1)" : "rgba(15,23,42,0.1)"};border-radius:18px;padding:32px 24px;box-shadow:0 16px 40px rgba(0,0,0,0.22);text-align:center;">
        <img src="/customtheme/onmydisk.svg" alt="On My Disk" style="width:52px;height:52px;margin:0 auto 16px;display:block;" />
        <h1 id="omd-bridge-title" style="font-size:20px;font-weight:700;margin:0 0 8px;">${copy.title}</h1>
        <p id="omd-bridge-desc" style="font-size:14px;line-height:1.5;margin:0 0 24px;color:${prefersDark ? "#94A3B8" : "#475569"};">${copy.desc}</p>
        <div id="omd-bridge-btn" style="display:flex;justify-content:center;min-height:44px;"></div>
        <div id="omd-bridge-return" style="display:none;margin-top:16px;">
          <a href="onmydisk://auth-complete" style="display:inline-flex;align-items:center;justify-content:center;width:100%;height:44px;border-radius:10px;background:#0284C7;color:#FFFFFF;font-weight:600;font-size:15px;text-decoration:none;">${copy.openAppBtn}</a>
        </div>
        <p id="omd-bridge-status" style="font-size:13px;margin:16px 0 0;min-height:20px;color:${prefersDark ? "#38BDF8" : "#0284C7"};"></p>
      </div>
    </div>
  `;

  const btnHost = document.getElementById("omd-bridge-btn");
  const returnHost = document.getElementById("omd-bridge-return");
  const titleEl = document.getElementById("omd-bridge-title");
  const descEl = document.getElementById("omd-bridge-desc");
  const statusEl = document.getElementById("omd-bridge-status");

  async function completeHandoff(payload) {
    if (btnHost) btnHost.style.display = "none";
    if (statusEl) {
      statusEl.style.color = prefersDark ? "#38BDF8" : "#0284C7";
      statusEl.textContent = copy.verifying;
    }
    try {
      const r = await fetch(`${HUB_AUTH_URL}/api/auth/mobile-oauth-complete`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ session, provider, ...payload }),
      });
      const data = await r.json().catch(() => ({}));
      if (!r.ok || !data.ok) {
        throw new Error(data.error || `HTTP ${r.status}`);
      }
      if (titleEl) {
        titleEl.textContent = "✓ " + copy.doneTitle;
        titleEl.style.color = prefersDark ? "#34D399" : "#059669";
      }
      if (descEl) descEl.textContent = copy.doneDesc;
      if (statusEl) statusEl.textContent = data.email || "";
      if (returnHost) returnHost.style.display = "block";
      setTimeout(() => {
        try {
          window.location.href = "onmydisk://auth-complete";
        } catch (_) {}
      }, 300);
      setTimeout(() => {
        try {
          window.close();
        } catch (_) {}
      }, 1200);
    } catch (err) {
      if (btnHost) btnHost.style.display = "flex";
      if (statusEl) {
        statusEl.style.color = "#EF4444";
        statusEl.textContent = copy.errPrefix + (err?.message || String(err));
      }
    }
  }

  if (provider === "apple") {
    if (btnHost) {
      const appleBtn = document.createElement("button");
      appleBtn.type = "button";
      appleBtn.style.cssText = `display:inline-flex;align-items:center;justify-content:center;gap:10px;width:100%;max-width:300px;height:44px;border-radius:8px;border:1px solid ${prefersDark ? "rgba(255,255,255,0.22)" : "#0F172A"};background:${prefersDark ? "#FFFFFF" : "#0F172A"};color:${prefersDark ? "#0F172A" : "#FFFFFF"};font-size:15px;font-weight:600;cursor:pointer;`;
      appleBtn.innerHTML = `<svg viewBox="0 0 170 170" width="18" height="18" fill="currentColor"><path d="M150.37 130.25c-2.45 5.66-5.35 10.87-8.71 15.66-4.58 6.53-8.33 11.05-11.22 13.56-4.48 4.12-9.28 6.23-14.42 6.35-3.69 0-8.14-1.05-13.32-3.18-5.19-2.12-9.97-3.17-14.34-3.17-4.58 0-9.49 1.05-14.75 3.17-5.26 2.13-9.5 3.24-12.74 3.35-4.35.13-9.16-1.9-14.42-6.08-3.7-3.04-7.6-7.77-11.7-14.19-5.77-8.91-10.28-18.73-13.53-29.47-3.25-10.74-4.88-21.13-4.88-31.18 0-13.91 3.53-25.54 10.6-34.89 7.07-9.35 15.82-14.13 26.24-14.34 5.01 0 10.22 1.25 15.63 3.75 5.41 2.5 9.07 3.86 10.99 4.08 1.93-.22 5.64-1.58 11.13-4.08 5.49-2.5 10.43-3.65 14.83-3.46 9.68.43 17.78 4.12 24.31 11.07-8.59 5.22-12.8 12.38-12.63 21.48.16 7.6 2.98 13.96 8.44 19.08 5.46 5.12 12.08 8.01 19.86 8.68-2.18 6.32-4.88 12.83-8.1 19.53zM119.22 32.73c0-7.39 2.66-14.41 7.98-21.06 5.32-6.65 11.98-10.88 19.98-12.69.11 1.09.16 2.07.16 2.94 0 7.39-2.77 14.46-8.31 21.21-5.54 6.75-12.33 10.79-20.37 12.13-.11-.87-.16-1.71-.16-2.53z"/></svg><span>${copy.btnApple}</span>`;
      btnHost.appendChild(appleBtn);

      const loadAppleScript = () =>
        new Promise((resolve, reject) => {
          if (window.AppleID) return resolve(window.AppleID);
          const s = document.createElement("script");
          s.src = "https://appleid.cdn-apple.com/appleauth/static/jsapi/appleid/1/en_US/appleid.auth.js";
          s.async = true;
          s.onload = () => resolve(window.AppleID);
          s.onerror = () => reject(new Error("Failed to load Apple Sign In SDK"));
          document.head.appendChild(s);
        });

      loadAppleScript().catch(() => {});

      appleBtn.addEventListener("click", async () => {
        if (statusEl) statusEl.textContent = "";
        try {
          const AppleID = await loadAppleScript();
          AppleID.auth.init({
            clientId: "net.onmydisk.app.service",
            scope: "name email",
            redirectURI: window.location.origin,
            usePopup: true,
          });
          const res = await AppleID.auth.signIn();
          const idToken = res?.authorization?.id_token;
          if (!idToken) throw new Error("No id_token returned from Apple");
          await completeHandoff({ idToken, user: res.user || null });
        } catch (err) {
          if (err?.error === "popup_closed_by_user") return;
          if (statusEl) {
            statusEl.style.color = "#EF4444";
            statusEl.textContent = copy.errPrefix + (err?.message || err?.error || String(err));
          }
        }
      });
    }
    return true;
  }

  async function onGoogleCred(resp) {
    if (!resp?.credential) return;
    await completeHandoff({ idToken: resp.credential });
  }

  function initGsi() {
    if (!window.google?.accounts?.id) return false;
    window.google.accounts.id.initialize({
      client_id: GOOGLE_WEB_CLIENT_ID,
      callback: onGoogleCred,
      auto_select: false,
    });
    if (btnHost) {
      window.google.accounts.id.renderButton(btnHost, {
        type: "standard",
        theme: prefersDark ? "filled_black" : "outline",
        size: "large",
        width: 300,
        shape: "rectangular",
      });
    }
    try {
      window.google.accounts.id.prompt();
    } catch (_) {}
    return true;
  }

  if (!initGsi()) {
    let attempts = 0;
    const iv = setInterval(() => {
      attempts++;
      if (initGsi() || attempts > 50) {
        clearInterval(iv);
      }
    }, 120);
  }

  return true;
}

