import { writable } from 'svelte/store';
import { Capacitor, CapacitorHttp, registerPlugin } from '@capacitor/core';
import { CapacitorUpdater } from '@capgo/capacitor-updater';
import { translate } from './i18n/index.js';
import { isIosNative } from './platform.js';
import { peekSavedSessionSync, saveSessionEverywhere } from './identity/sessionBackup.js';

const NativeStorage = registerPlugin('NativeStorage');

// Native Android Live OTA endpoints (never used by regular web browsers)
const OTA_VERSION_URL = 'https://forge.disk.direct/repo/DiskDirect/version.json';
const OTA_BUNDLE_URL = 'https://forge.disk.direct/repo/DiskDirect/Android/direct-frontend.zip';

// Same-origin web deployment manifest emitted by Vite into dist/assets/web-version.json
const WEB_VERSION_PATH = '/assets/web-version.json';

const DEFAULT_VERSION = (typeof import.meta !== 'undefined' && import.meta.env && import.meta.env.VITE_APP_VERSION) || '3.1.6000';
const WEB_BUILD_ID = (typeof import.meta !== 'undefined' && import.meta.env && import.meta.env.VITE_WEB_BUILD_ID) || '';

let latestWebBuildId = '';

async function getNativeAppVersion() {
  if (typeof window !== 'undefined' && Capacitor.isNativePlatform()) {
    try {
      if (NativeStorage && typeof NativeStorage.getAppVersion === 'function') {
        const res = await NativeStorage.getAppVersion();
        if (res && res.version && res.version !== '1.0' && !res.version.startsWith('1.0.')) {
          return res.version;
        }
      }
    } catch (_) {}
  }
  return null;
}

function getInitialAppVersion() {
  if (typeof import.meta !== 'undefined' && import.meta.env && import.meta.env.DEV) {
    return DEFAULT_VERSION;
  }

  if (typeof window !== 'undefined' && Capacitor.isNativePlatform()) {
    const stored = localStorage.getItem('omd_app_version');
    if (stored && stored !== 'builtin' && stored !== 'undefined' && stored !== '1.0' && stored !== '1.0.0' && !stored.startsWith('1.0.')) {
      return stored;
    }
    return DEFAULT_VERSION;
  }

  // On web, always reflect the actual running frontend bundle version
  return DEFAULT_VERSION;
}

const initialVersion = getInitialAppVersion();

export const appVersion = writable(initialVersion);
export const otaState = writable({
  version: initialVersion,
  currentVersion: initialVersion,
  serverVersion: '',
  updateAvailable: false,
  isUpdating: false,
  isNative: typeof window !== 'undefined' && Capacitor.isNativePlatform(),
  otaSupported: !isIosNative(),
  statusText: '',
  dismissed: false,
  downloadPercent: 0,
  error: null
});

function updateStores(version, extra = {}) {
  if (version && version !== 'builtin' && version !== 'undefined') {
    appVersion.set(version);
    try {
      if (typeof window !== 'undefined') {
        if (Capacitor.isNativePlatform()) {
          localStorage.setItem('omd_app_version', version);
        }
        window.__omdAppVersion = version;
        window.dispatchEvent(new CustomEvent('omd-version-update', { detail: { version, ...extra } }));
      }
    } catch (_) {}
  }
  otaState.update(s => ({
    ...s,
    ...(version && version !== 'builtin' && version !== 'undefined' ? { version, currentVersion: version } : {}),
    ...extra
  }));
}

// ═══════════════════════════════════════════════════════════════════════
// CRITICAL: notifyAppReady() MUST run synchronously at module load time
// BEFORE any async/onMount code to confirm the bundle to the native plugin
// and prevent automatic rollback.
// ═══════════════════════════════════════════════════════════════════════
if (typeof window !== 'undefined') {
  if (Capacitor.isNativePlatform()) {
    CapacitorUpdater.notifyAppReady()
      .then(async () => {
        console.log('[OTA] notifyAppReady called successfully during initialization');
        try {
          const current = await CapacitorUpdater.current();
          const bundle = current?.bundle || current;
          const runningVer = bundle && bundle.version ? String(bundle.version) : null;
          if (runningVer && runningVer !== 'builtin' && runningVer !== '1.0' && runningVer !== 'undefined') {
            updateStores(runningVer);
          } else {
            const nativeVer = await getNativeAppVersion();
            updateStores(nativeVer || DEFAULT_VERSION);
          }
        } catch (_) {}
      })
      .catch((err) => {
        console.error('[OTA] notifyAppReady failed:', err);
      });
  }
}

/**
 * Semantic/build-number version comparator.
 * Returns true only if remoteVer is strictly higher than currentVer.
 */
export function isNewerVersion(remoteVer, currentVer) {
  if (!remoteVer || !currentVer) return false;
  if (remoteVer === currentVer) return false;

  const parseParts = (v) => {
    return String(v)
      .replace(/^[^\d]*/, '')
      .split('.')
      .map(part => parseInt(part, 10) || 0);
  };

  const p1 = parseParts(remoteVer);
  const p2 = parseParts(currentVer);
  const maxLen = Math.max(p1.length, p2.length);

  for (let i = 0; i < maxLen; i++) {
    const num1 = p1[i] !== undefined ? p1[i] : 0;
    const num2 = p2[i] !== undefined ? p2[i] : 0;
    if (num1 > num2) return true;
    if (num1 < num2) return false;
  }
  return false;
}

let isChecking = false;

/**
 * Check for new updates:
 * - Native Android: checks OTA_VERSION_URL (Capgo OTA bundle)
 * - Web Browser: checks same-origin /assets/web-version.json buildId against baked WEB_BUILD_ID
 * - Native iOS / Vite dev: no-op
 */
export async function checkForOtaUpdate(force = false) {
  if ((typeof import.meta !== 'undefined' && import.meta.env && import.meta.env.DEV) || isIosNative()) {
    return;
  }

  if (typeof window === 'undefined' || isChecking) {
    return;
  }

  let activeUpdating = false;
  otaState.subscribe(s => activeUpdating = s.isUpdating)();
  if (activeUpdating) return;

  // ── Path A: Regular Web Browser (same-origin bundle buildId check) ──
  if (!Capacitor.isNativePlatform()) {
    isChecking = true;
    try {
      const res = await fetch(`${WEB_VERSION_PATH}?t=${Date.now()}`, {
        cache: 'no-store',
        headers: { Accept: 'application/json' }
      });
      if (!res.ok) return;
      const data = await res.json();
      const serverBuildId = String(data?.buildId || '').trim();
      const serverVersion = String(data?.version || '').trim();

      if (!WEB_BUILD_ID || !serverBuildId || serverBuildId === WEB_BUILD_ID) {
        updateStores(DEFAULT_VERSION, { updateAvailable: false, serverVersion: '', isUpdating: false });
        return;
      }

      // Loop guard: if this tab already reloaded once for this exact serverBuildId, never re-prompt
      let alreadyReloadedForBuild = false;
      try {
        alreadyReloadedForBuild = sessionStorage.getItem('omd_reloaded_build_id') === serverBuildId;
      } catch (_) {}
      if (alreadyReloadedForBuild && !force) {
        updateStores(DEFAULT_VERSION, { updateAvailable: false, serverVersion: '', isUpdating: false });
        return;
      }

      latestWebBuildId = serverBuildId;
      const lastDismissed = parseInt(localStorage.getItem('omd_ota_dismissed_time') || '0', 10);
      const dismissedBuild = localStorage.getItem('omd_ota_dismissed_version') || '';
      const isCooldown = !force && (dismissedBuild === serverBuildId || dismissedBuild === serverVersion) && (Date.now() - lastDismissed < 30 * 60 * 1000);

      updateStores(DEFAULT_VERSION, {
        updateAvailable: true,
        serverVersion: serverVersion || DEFAULT_VERSION,
        newVersion: serverVersion || DEFAULT_VERSION,
        isNative: false,
        dismissed: isCooldown
      });
    } catch (_) {
      // Ignore transient network errors on web
    } finally {
      isChecking = false;
    }
    return;
  }

  // ── Path B: Native Android Capacitor Live OTA ──
  if (!force) {
    try {
      const failedCount = parseInt(localStorage.getItem('omd_ota_failed_count') || '0', 10);
      const lastFailed = parseInt(localStorage.getItem('omd_ota_last_failed') || '0', 10);
      if (failedCount > 0 && lastFailed > 0) {
        const cooldownMinutes = failedCount === 1 ? 5 : failedCount === 2 ? 15 : failedCount === 3 ? 30 : 60;
        const elapsed = Date.now() - lastFailed;
        if (elapsed < cooldownMinutes * 60 * 1000) {
          console.log(`[OTA] Cooldown active (${failedCount} fail(s), retry in ${Math.round((cooldownMinutes * 60 * 1000 - elapsed) / 1000)}s)`);
          return;
        }
      }
    } catch (_) {}
  }

  isChecking = true;
  try {
    const response = await CapacitorHttp.get({
      url: `${OTA_VERSION_URL}?t=${Date.now()}`,
      connectTimeout: 20000,
      readTimeout: 20000
    });
    if (response.status !== 200 || !response.data) return;
    const data = typeof response.data === 'string' ? JSON.parse(response.data) : response.data;
    const serverVersion = String(data?.version || '').trim();
    if (!serverVersion) return;

    let currentVersion = DEFAULT_VERSION;
    try {
      const current = await CapacitorUpdater.current();
      const bundle = current?.bundle || current;
      const bundleVer = bundle && bundle.version ? String(bundle.version) : 'builtin';
      if (bundleVer && bundleVer !== 'builtin' && bundleVer !== '1.0' && bundleVer !== 'undefined') {
        currentVersion = bundleVer;
      } else {
        const nativeVer = await getNativeAppVersion();
        currentVersion = nativeVer || localStorage.getItem('omd_app_version') || DEFAULT_VERSION;
      }
    } catch (_) {
      currentVersion = localStorage.getItem('omd_app_version') || DEFAULT_VERSION;
    }

    updateStores(currentVersion);

    if (isNewerVersion(serverVersion, currentVersion)) {
      console.log(`[OTA] Newer version available: ${serverVersion} (current: ${currentVersion})`);

      const lastDismissed = parseInt(localStorage.getItem('omd_ota_dismissed_time') || '0', 10);
      const dismissedVer = localStorage.getItem('omd_ota_dismissed_version') || '';
      const isCooldown = !force && (dismissedVer === serverVersion) && (Date.now() - lastDismissed < 30 * 60 * 1000);

      updateStores(currentVersion, {
        updateAvailable: true,
        serverVersion,
        newVersion: serverVersion,
        isNative: true,
        dismissed: isCooldown
      });
    } else {
      console.log(`[OTA] Up to date (server: ${serverVersion}, current: ${currentVersion})`);
      updateStores(currentVersion, { updateAvailable: false, serverVersion: '', isUpdating: false });
    }
  } catch (err) {
    console.warn('[OTA] Check failed (will retry next launch):', err);
  } finally {
    isChecking = false;
  }
}

/**
 * Perform Live OTA update (Android) or clean session-safe reload (Web).
 */
export async function performOtaUpdate(serverVer) {
  if (isIosNative()) return;

  // Web browser path: record reloaded buildId in sessionStorage to prevent reload loops and reload cleanly
  if (typeof window !== 'undefined' && !Capacitor.isNativePlatform()) {
    try {
      if (latestWebBuildId) {
        sessionStorage.setItem('omd_reloaded_build_id', latestWebBuildId);
      }
      const activeSession = peekSavedSessionSync();
      if (activeSession) {
        await saveSessionEverywhere(activeSession);
      }
    } catch (_) {}
    window.location.reload();
    return;
  }

  otaState.update(s => ({
    ...s,
    isUpdating: true,
    error: null,
    downloadPercent: 0,
    statusText: translate('ota_downloading', {}, 'Downloading update...')
  }));

  let downloadListener = null;

  try {
    let targetVersion = serverVer;
    try {
      const response = await CapacitorHttp.get({
        url: `${OTA_VERSION_URL}?t=${Date.now()}`,
        connectTimeout: 20000,
        readTimeout: 20000
      });
      if (response.status === 200 && response.data) {
        const data = typeof response.data === 'string' ? JSON.parse(response.data) : response.data;
        if (data && data.version) {
          targetVersion = String(data.version).trim();
        }
      }
    } catch (netErr) {
      console.warn('[LiveUpdate] Failed to reach version endpoint:', netErr);
    }

    if (!targetVersion) {
      otaState.subscribe(s => targetVersion = s.serverVersion || s.newVersion)();
    }
    if (!targetVersion) {
      throw new Error('Target version not specified');
    }

    try {
      const listRes = await CapacitorUpdater.list();
      const bundles = listRes?.bundles || [];
      for (const b of bundles) {
        if (b.id && !b.isBuiltin && (b.status === 'error' || b.status === 'downloading' || b.version === targetVersion)) {
          try {
            await CapacitorUpdater.delete({ id: b.id });
          } catch (_) {}
        }
      }
    } catch (cleanErr) {
      console.warn('[LiveUpdate] Pre-cleanup warning:', cleanErr);
    }

    let failListener = null;
    let downloadErr = null;
    try {
      downloadListener = await CapacitorUpdater.addListener('download', (event) => {
        if (event && typeof event.percent === 'number') {
          const p = Math.min(100, Math.max(0, Math.round(event.percent)));
          let statusText = '';
          if (p <= 70) {
            statusText = `${translate('ota_downloading', {}, 'Downloading update...')} ${p}%`;
          } else if (p < 100) {
            statusText = `${translate('ota_unpacking', {}, 'Unpacking and verifying update...')} ${p}%`;
          } else {
            statusText = translate('ota_applying', {}, 'Applying update...');
          }
          otaState.update(s => ({
            ...s,
            downloadPercent: p,
            statusText
          }));
        }
      });
    } catch (e) {
      console.warn('[LiveUpdate] Could not attach download listener:', e);
    }

    try {
      failListener = await CapacitorUpdater.addListener('downloadFailed', (event) => {
        console.warn('[LiveUpdate] Native downloadFailed event:', event);
        if (event && event.error && !downloadErr) {
          downloadErr = new Error(event.error);
        }
      });
    } catch (_) {}

    console.log(`[LiveUpdate] Downloading LATEST bundle v${targetVersion}...`);
    let bundle = null;
    const MAX_ATTEMPTS = 2;

    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
      try {
        if (attempt > 1) {
          console.log(`[LiveUpdate] Retry attempt ${attempt}/${MAX_ATTEMPTS}...`);
          otaState.update(s => ({
            ...s,
            downloadPercent: 0,
            statusText: translate('ota_retrying', {}, 'Reconnecting and retrying...')
          }));
          await new Promise(r => setTimeout(r, 2000));
        }

        bundle = await CapacitorUpdater.download({
          url: `${OTA_BUNDLE_URL}?t=${Date.now()}`,
          version: targetVersion
        });

        if (bundle && bundle.id) {
          downloadErr = null;
          break;
        }
      } catch (err) {
        console.warn(`[LiveUpdate] Download attempt ${attempt} error:`, err);
        downloadErr = err;
      }
    }

    if (downloadListener) {
      try {
        await downloadListener.remove();
        downloadListener = null;
      } catch (_) {}
    }
    if (failListener) {
      try {
        await failListener.remove();
        failListener = null;
      } catch (_) {}
    }

    if (!bundle || !bundle.id) {
      throw downloadErr || new Error('Bundle download failed');
    }

    otaState.update(s => ({
      ...s,
      downloadPercent: 100,
      statusText: translate('ota_applying', {}, 'Applying update...')
    }));

    try {
      const listRes = await CapacitorUpdater.list();
      const bundles = listRes?.bundles || [];
      for (const b of bundles) {
        if (b.id && b.id !== bundle.id && !b.isBuiltin) {
          try {
            await CapacitorUpdater.delete({ id: b.id });
          } catch (_) {}
        }
      }
    } catch (_) {}

    console.log(`[LiveUpdate] Activating latest bundle ${bundle.id} (v${targetVersion})...`);
    try {
      localStorage.setItem('omd_app_version', targetVersion);
      window.__omdAppVersion = targetVersion;
      localStorage.removeItem('omd_ota_failed_count');
      localStorage.removeItem('omd_ota_last_failed');
      const activeSession = peekSavedSessionSync();
      if (activeSession) {
        await saveSessionEverywhere(activeSession);
      }
    } catch (_) {}

    await CapacitorUpdater.set({ id: bundle.id });
  } catch (err) {
    if (downloadListener) {
      try { await downloadListener.remove(); } catch (_) {}
    }
    console.error('[LiveUpdate] Update failed:', err);

    try {
      const prevFails = parseInt(localStorage.getItem('omd_ota_failed_count') || '0', 10);
      localStorage.setItem('omd_ota_failed_count', String(prevFails + 1));
      localStorage.setItem('omd_ota_last_failed', String(Date.now()));
    } catch (_) {}

    otaState.update(s => ({
      ...s,
      isUpdating: false,
      error: err?.message || String(err),
      statusText: `${translate('ota_error', {}, 'Update error')}: ${err?.message || String(err)}`
    }));
  }
}

/**
 * Dismiss update banner temporarily (sets 30 min cooldown)
 */
export function dismissOtaNotification() {
  let ver = '';
  otaState.subscribe(s => ver = s.serverVersion || s.newVersion)();
  try {
    localStorage.setItem('omd_ota_dismissed_time', String(Date.now()));
    localStorage.setItem('omd_ota_dismissed_version', latestWebBuildId || ver);
  } catch (_) {}
  otaState.update(s => ({ ...s, dismissed: true, error: null, isUpdating: false }));
}

/**
 * Immediate reload helper
 */
export async function applyOtaUpdateNow() {
  if (typeof window === 'undefined') return;
  if (Capacitor.isNativePlatform()) {
    try {
      await CapacitorUpdater.reload();
      return;
    } catch (_) {}
  }
  try {
    if (latestWebBuildId) {
      sessionStorage.setItem('omd_reloaded_build_id', latestWebBuildId);
    }
  } catch (_) {}
  window.location.reload();
}
