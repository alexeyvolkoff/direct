<script>
  import { onMount, onDestroy, createEventDispatcher } from "svelte";
  import { registerProxiedApp, getProxiedApp, buildAppProxyUrl, initAppProxyServiceWorkerBridge } from "../lib/app-proxy.js";

  export let targetLinkId = "";
  export let deviceName = "";
  export let appSlug = "";
  export let appName = "";
  export let port = 0;
  export let entryPath = "/";
  export let icon = "";
  export let isDarkTheme = true;

  const dispatch = createEventDispatcher();

  let swReady = false;
  let swError = "";
  let iframeLoading = true;
  let iframeEl = null;
  let iframeSrc = "";
  let reloadNonce = 0;
  let currentSubPath = entryPath || "/";

  function syncAppRegistration() {
    if (!targetLinkId || !appSlug) return;
    const existing = getProxiedApp(targetLinkId, appSlug);
    const effPort = port || existing?.port || 0;
    const effEntry = entryPath || existing?.entryPath || "/";
    const effName = appName || existing?.name || appSlug;
    if (effPort > 0) {
      registerProxiedApp(targetLinkId, {
        slug: appSlug,
        name: effName,
        port: effPort,
        entryPath: effEntry,
        icon: icon || existing?.icon || appSlug
      });
    }
    currentSubPath = effEntry;
    iframeSrc = buildAppProxyUrl(targetLinkId, appSlug, effEntry);
  }

  $: if (targetLinkId && appSlug) {
    syncAppRegistration();
  }

  async function ensureServiceWorkerControlled() {
    if (typeof navigator === "undefined" || !("serviceWorker" in navigator)) {
      swError = "Service Worker is not supported in this browser (HTTPS or localhost required).";
      return;
    }
    initAppProxyServiceWorkerBridge();
    if (navigator.serviceWorker.controller) {
      try {
        navigator.serviceWorker.controller.postMessage({ type: "REGISTER_APP_HOST" });
      } catch (_) {}
      swReady = true;
      return;
    }

    try {
      const reg = await navigator.serviceWorker.ready;
      if (navigator.serviceWorker.controller) {
        swReady = true;
        return;
      }
      if (reg && reg.active) {
        await new Promise((resolve) => {
          const done = () => {
            navigator.serviceWorker.removeEventListener("controllerchange", done);
            resolve();
          };
          navigator.serviceWorker.addEventListener("controllerchange", done);
          try {
            reg.active.postMessage({ type: "claim" });
          } catch (_) {}
          setTimeout(done, 1500);
        });
      }
      if (navigator.serviceWorker.controller) {
        try {
          navigator.serviceWorker.controller.postMessage({ type: "REGISTER_APP_HOST" });
        } catch (_) {}
      }
      swReady = true;
    } catch (err) {
      swError = (err && err.message) ? err.message : "Failed to initialize Service Worker tunnel.";
    }
  }

  function handleWindowMessage(event) {
    const data = event.data;
    if (!data || data.type !== "OMD_APP_PROXY_STATE") return;
    if (data.linkId !== targetLinkId || data.appSlug !== appSlug) return;
    if (data.path) currentSubPath = data.path;
    iframeLoading = false;
  }

  function handleIframeLoad() {
    iframeLoading = false;
  }

  function reloadApp() {
    iframeLoading = true;
    reloadNonce += 1;
  }

  function goHome() {
    iframeLoading = true;
    const base = buildAppProxyUrl(targetLinkId, appSlug, entryPath || "/");
    iframeSrc = base;
    reloadNonce += 1;
  }

  function openInNewTab() {
    const url = buildAppProxyUrl(targetLinkId, appSlug, currentSubPath || entryPath || "/");
    window.open(url, "_blank", "noopener");
  }

  function closeApp() {
    dispatch("close");
  }

  onMount(() => {
    window.addEventListener("message", handleWindowMessage);
    ensureServiceWorkerControlled();
  });

  onDestroy(() => {
    if (typeof window !== "undefined") {
      window.removeEventListener("message", handleWindowMessage);
    }
  });
</script>

<div class="app-proxy-shell" class:dark={isDarkTheme} class:light={!isDarkTheme}>
  <header class="app-proxy-toolbar">
    <div class="toolbar-left">
      <button type="button" class="toolbar-btn back-btn" on:click={closeApp} title="Back to files">
        <svg viewBox="0 0 16 16" width="15" height="15" fill="currentColor" aria-hidden="true">
          <path fill-rule="evenodd" d="M15 8a.5.5 0 0 0-.5-.5H2.707l3.147-3.146a.5.5 0 1 0-.708-.708l-4 4a.5.5 0 0 0 0 .708l4 4a.5.5 0 0 0 .708-.708L2.707 8.5H14.5A.5.5 0 0 0 15 8"/>
        </svg>
      </button>
      <span class="app-title">{appName || appSlug}</span>
      <span class="p2p-pill" title="End-to-end encrypted WebRTC P2P tunnel (no port forwarding)">
        <span class="p2p-dot"></span>
        P2P{deviceName ? ` · ${deviceName}` : ""}
      </span>
      {#if currentSubPath}
        <span class="app-path" title={currentSubPath}>{currentSubPath}</span>
      {/if}
    </div>

    <div class="toolbar-right">
      {#if iframeLoading && !swError}
        <span class="loading-spinner" aria-label="Loading"></span>
      {/if}
      <button type="button" class="toolbar-btn" on:click={goHome} title="App Home">
        <svg viewBox="0 0 16 16" width="14" height="14" fill="currentColor" aria-hidden="true">
          <path d="M8.707 1.5a1 1 0 0 0-1.414 0L.646 8.146a.5.5 0 0 0 .708.708L2 8.207V13.5A1.5 1.5 0 0 0 3.5 15h9a1.5 1.5 0 0 0 1.5-1.5V8.207l.646.647a.5.5 0 0 0 .708-.708L13 5.793V2.5a.5.5 0 0 0-.5-.5h-1a.5.5 0 0 0-.5.5v1.293z"/>
        </svg>
      </button>
      <button type="button" class="toolbar-btn" on:click={reloadApp} title="Reload">
        <svg viewBox="0 0 16 16" width="14" height="14" fill="currentColor" aria-hidden="true">
          <path fill-rule="evenodd" d="M8 3a5 5 0 1 0 4.546 2.914.5.5 0 0 1 .908-.417A6 6 0 1 1 8 2z"/>
          <path d="M8 4.466V.534a.25.25 0 0 1 .41-.192l2.36 1.966c.12.1.12.284 0 .384L8.41 4.658A.25.25 0 0 1 8 4.466"/>
        </svg>
      </button>
      <button type="button" class="toolbar-btn" on:click={openInNewTab} title="Open in new tab">
        <svg viewBox="0 0 16 16" width="14" height="14" fill="currentColor" aria-hidden="true">
          <path fill-rule="evenodd" d="M8.636 3.5a.5.5 0 0 0-.5-.5H1.5A1.5 1.5 0 0 0 0 4.5v10A1.5 1.5 0 0 0 1.5 16h10a1.5 1.5 0 0 0 1.5-1.5V7.864a.5.5 0 0 0-1 0V14.5a.5.5 0 0 1-.5.5h-10a.5.5 0 0 1-.5-.5v-10a.5.5 0 0 1 .5-.5h6.636a.5.5 0 0 0 .5-.5"/>
          <path fill-rule="evenodd" d="M16 .5a.5.5 0 0 0-.5-.5h-5a.5.5 0 0 0 0 1h3.793L6.146 9.146a.5.5 0 1 0 .708.708L15 1.707V5.5a.5.5 0 0 0 1 0z"/>
        </svg>
      </button>
      <button type="button" class="toolbar-btn close-btn" on:click={closeApp} title="Close">
        <svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" aria-hidden="true">
          <path d="M4.646 4.646a.5.5 0 0 1 .708 0L8 7.293l2.646-2.647a.5.5 0 0 1 .708.708L8.707 8l2.647 2.646a.5.5 0 0 1-.708.708L8 8.707l-2.646 2.647a.5.5 0 0 1-.708-.708L7.293 8 4.646 5.354a.5.5 0 0 1 0-.708"/>
        </svg>
      </button>
    </div>
  </header>

  <div class="app-proxy-stage">
    {#if swError}
      <div class="app-proxy-state error">
        <p>{swError}</p>
        <button type="button" class="retry-btn" on:click={() => { swError = ""; ensureServiceWorkerControlled(); }}>
          Retry
        </button>
      </div>
    {:else if !swReady || !iframeSrc}
      <div class="app-proxy-state">
        <span class="loading-spinner large"></span>
        <span>Opening P2P tunnel to {appName || appSlug}…</span>
      </div>
    {:else}
      {#key `${iframeSrc}::${reloadNonce}`}
        <iframe
          bind:this={iframeEl}
          class="app-proxy-iframe"
          src={iframeSrc}
          title={appName || appSlug}
          allow="clipboard-read; clipboard-write; fullscreen; microphone; camera"
          on:load={handleIframeLoad}
        ></iframe>
      {/key}
    {/if}
  </div>
</div>

<style>
  .app-proxy-shell {
    position: absolute;
    inset: 0;
    flex: 1 1 0%;
    min-height: 0;
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    overflow: hidden;
    background: #0f141b;
    color: #e6edf3;
  }
  .app-proxy-shell.light {
    background: #f6f8fa;
    color: #1f2328;
  }
  .app-proxy-toolbar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    height: 42px;
    min-height: 42px;
    padding: 0 10px;
    background: #161b22;
    border-bottom: 1px solid rgba(255, 255, 255, 0.08);
    user-select: none;
  }
  .app-proxy-shell.light .app-proxy-toolbar {
    background: #ffffff;
    border-bottom: 1px solid rgba(31, 35, 40, 0.12);
  }
  .toolbar-left,
  .toolbar-right {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
  }
  .toolbar-left {
    flex: 1 1 auto;
    overflow: hidden;
  }
  .app-title {
    font-size: 13.5px;
    font-weight: 600;
    white-space: nowrap;
  }
  .p2p-pill {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 2px 8px;
    border-radius: 999px;
    font-size: 11px;
    font-weight: 500;
    background: rgba(46, 160, 67, 0.15);
    color: #3fb950;
    border: 1px solid rgba(63, 185, 80, 0.3);
    white-space: nowrap;
  }
  .app-proxy-shell.light .p2p-pill {
    background: rgba(31, 136, 61, 0.1);
    color: #1a7f37;
    border-color: rgba(31, 136, 61, 0.25);
  }
  .p2p-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: currentColor;
  }
  .app-path {
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 11.5px;
    opacity: 0.6;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .toolbar-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 28px;
    padding: 0;
    border: none;
    border-radius: 6px;
    background: transparent;
    color: inherit;
    opacity: 0.78;
    cursor: pointer;
    transition: background 0.15s ease, opacity 0.15s ease;
  }
  .toolbar-btn:hover {
    opacity: 1;
    background: rgba(127, 140, 155, 0.18);
  }
  .close-btn:hover {
    background: rgba(248, 81, 73, 0.2);
    color: #ff7b72;
  }
  .app-proxy-stage {
    position: relative;
    flex: 1 1 0%;
    min-height: 0;
    width: 100%;
    overflow: hidden;
  }
  .app-proxy-iframe {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    border: 0;
    display: block;
    background: #0f141b;
  }
  .app-proxy-state {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 12px;
    width: 100%;
    height: 100%;
    font-size: 13.5px;
    opacity: 0.85;
    padding: 24px;
    text-align: center;
  }
  .retry-btn {
    padding: 6px 14px;
    border-radius: 6px;
    border: 1px solid rgba(255, 255, 255, 0.2);
    background: #2996cc;
    color: #fff;
    font-size: 13px;
    cursor: pointer;
  }
  .loading-spinner {
    width: 14px;
    height: 14px;
    border: 2px solid rgba(127, 140, 155, 0.3);
    border-top-color: #2996cc;
    border-radius: 50%;
    animation: omd-app-spin 0.75s linear infinite;
  }
  .loading-spinner.large {
    width: 24px;
    height: 24px;
  }
  @keyframes omd-app-spin {
    to {
      transform: rotate(360deg);
    }
  }
  @media (max-width: 640px) {
    .app-path {
      display: none;
    }
  }
</style>

