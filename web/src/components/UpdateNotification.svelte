<script>
  import { _ } from "../lib/i18n/index.js";
  import { otaState, performOtaUpdate, dismissOtaNotification } from "../lib/ota-updater.js";

  export let isDarkTheme = false;

  function handleUpdate() {
    performOtaUpdate();
  }

  function handleDismiss() {
    dismissOtaNotification();
  }
</script>

{#if (!import.meta.env || !import.meta.env.DEV) && $otaState.updateAvailable && !$otaState.dismissed && !$otaState.isUpdating && !$otaState.error}
  <!-- Floating Top Notification Banner -->
  <aside class="ota-banner-wrap" class:light-theme={!isDarkTheme} aria-live="polite" role="region">
    <div class="ota-banner-card">
      <div class="ota-banner-icon-wrap">
        <div class="ota-sparkle-icon">
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M21 2v6h-6"/>
            <path d="M3 12a9 9 0 0 1 15-6.7L21 8"/>
            <path d="M3 22v-6h6"/>
            <path d="M21 12a9 9 0 0 1-15 6.7L3 16"/>
          </svg>
        </div>
        <span class="ota-ping-dot"></span>
      </div>

      <div class="ota-banner-content">
        <h4 class="ota-banner-title">{$_("update_available_title", {}, "On My Disk Update Available")}</h4>
        <p class="ota-banner-sub">
          {#if $otaState.isNative}
            {$_("update_available_sub", { version: $otaState.serverVersion || $otaState.newVersion }, `Version v${$otaState.serverVersion || $otaState.newVersion} is ready to install.`)}
          {:else}
            {$_("update_available_browser_sub", { version: $otaState.serverVersion || $otaState.newVersion }, `New version v${$otaState.serverVersion || $otaState.newVersion} is available. Reload to update.`)}
          {/if}
        </p>
      </div>

      <div class="ota-banner-actions">
        <button class="ota-btn-primary" on:click={handleUpdate} title={$otaState.isNative ? $_("update_now_btn", {}, "Update Now") : $_("reload_app_btn", {}, "Reload")}>
          {#if $otaState.isNative}
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3"/>
            </svg>
            <span class="ota-btn-label">{$_("update_now_btn", {}, "Update Now")}</span>
          {:else}
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M23 4v6h-6M1 20v-6h6M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/>
            </svg>
            <span class="ota-btn-label">{$_("reload_app_btn", {}, "Reload")}</span>
          {/if}
        </button>
        <button class="ota-btn-ghost" on:click={handleDismiss} title={$_("update_later_btn", {}, "Later")}>
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <line x1="18" y1="6" x2="6" y2="18"></line>
            <line x1="6" y1="6" x2="18" y2="18"></line>
          </svg>
        </button>
      </div>
    </div>
  </aside>
{/if}

{#if $otaState.isUpdating || $otaState.error}
  <!-- Fullscreen Updating Animation Overlay (KvikApp-style) -->
  <div class="ota-overlay-backdrop">
    <div class="ota-overlay-box">
      {#if $otaState.error}
        <!-- Error State -->
        <div class="ota-logo-container">
          <div class="ota-error-icon-box">
            <svg viewBox="0 0 24 24" width="40" height="40" fill="none" stroke="#ef4444" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
              <circle cx="12" cy="12" r="10" />
              <line x1="12" y1="8" x2="12" y2="12" />
              <line x1="12" y1="16" x2="12.01" y2="16" />
            </svg>
          </div>
        </div>

        <h2 class="ota-updating-title ota-error-title">{$_("ota_error", {}, "Update Error")}</h2>
        <p class="ota-updating-sub ota-error-sub">
          {$otaState.error}
        </p>

        <div class="ota-dialog-actions">
          <button class="ota-btn-primary" on:click={handleUpdate}>
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M21 2v6h-6"/>
              <path d="M3 12a9 9 0 0 1 15-6.7L21 8"/>
              <path d="M3 22v-6h6"/>
              <path d="M21 12a9 9 0 0 1-15 6.7L3 16"/>
            </svg>
            <span>{$_("retry", {}, "Retry")}</span>
          </button>
          <button class="ota-btn-secondary" on:click={handleDismiss}>
            <span>{$_("close", {}, "Close")}</span>
          </button>
        </div>
      {:else}
        <!-- Spinning ring around Logo -->
        <div class="ota-logo-container">
          <div class="ota-spin-ring"></div>
          <div class="ota-logo-inner">
            <img src="/img/icon-512.png" alt="On My Disk Logo" class="ota-logo-img" />
          </div>
        </div>

        <h2 class="ota-updating-title">{$_("updating_app_title", {}, "Updating On My Disk...")}</h2>
        <p class="ota-updating-sub">
          {$otaState.statusText || $_("updating_app_sub", {}, "Downloading and applying the latest update. Please wait...")}
        </p>

        {#if $otaState.downloadPercent > 0}
          <div class="ota-progress-bar-wrap">
            <div class="ota-progress-track">
              <div class="ota-progress-fill" style="width: {$otaState.downloadPercent}%"></div>
            </div>
            <span class="ota-progress-text">{$otaState.downloadPercent}%</span>
          </div>
        {:else}
          <!-- Animated micro-dots indicator -->
          <div class="ota-dots-indicator">
            <span class="ota-dot" style="animation-delay: 0ms"></span>
            <span class="ota-dot" style="animation-delay: 200ms"></span>
            <span class="ota-dot" style="animation-delay: 400ms"></span>
          </div>
        {/if}
      {/if}
    </div>
  </div>
{/if}

<style>
  /* ─────────────────────────────────────────────────────────────
     Top Notification Banner
     ───────────────────────────────────────────────────────────── */
  .ota-banner-wrap {
    position: fixed;
    top: calc(14px + env(safe-area-inset-top, 0px));
    left: 50%;
    transform: translateX(-50%);
    z-index: 99999;
    width: calc(100% - 28px);
    max-width: 480px;
    animation: otaSlideDown 0.35s cubic-bezier(0.16, 1, 0.3, 1) forwards;
  }

  @keyframes otaSlideDown {
    from {
      opacity: 0;
      transform: translate(-50%, -24px);
    }
    to {
      opacity: 1;
      transform: translate(-50%, 0);
    }
  }

  .ota-banner-card {
    display: flex;
    align-items: center;
    gap: 12px;
    background: rgba(16, 28, 34, 0.94);
    backdrop-filter: blur(16px);
    -webkit-backdrop-filter: blur(16px);
    border: 1px solid rgba(14, 165, 233, 0.4);
    box-shadow: 0 12px 32px rgba(0, 0, 0, 0.6), 0 0 20px rgba(14, 165, 233, 0.15);
    padding: 12px 16px;
    border-radius: 14px;
    color: #ffffff;
  }

  .ota-banner-icon-wrap {
    position: relative;
    width: 38px;
    height: 38px;
    border-radius: 10px;
    background: rgba(14, 165, 233, 0.15);
    border: 1px solid rgba(14, 165, 233, 0.3);
    display: flex;
    align-items: center;
    justify-content: center;
    color: #38bdf8;
    flex-shrink: 0;
  }

  .ota-ping-dot {
    position: absolute;
    top: -2px;
    right: -2px;
    width: 9px;
    height: 9px;
    border-radius: 50%;
    background-color: #38bdf8;
    box-shadow: 0 0 8px #38bdf8;
    animation: otaPing 1.8s cubic-bezier(0, 0, 0.2, 1) infinite;
  }

  @keyframes otaPing {
    0% { transform: scale(0.9); opacity: 1; }
    50% { transform: scale(1.4); opacity: 0.4; }
    100% { transform: scale(0.9); opacity: 1; }
  }

  .ota-banner-content {
    flex-grow: 1;
    min-width: 0;
  }

  .ota-banner-title {
    margin: 0;
    font-size: 13.5px;
    font-weight: 700;
    color: #f8fafc;
    line-height: 1.25;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .ota-banner-sub {
    margin: 3px 0 0 0;
    font-size: 12px;
    color: #94a3b8;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .ota-banner-actions {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-shrink: 0;
  }

  .ota-btn-primary {
    background: linear-gradient(135deg, #0ea5e9, #2563eb);
    color: white;
    border: none;
    padding: 7px 12px;
    border-radius: 8px;
    font-size: 12.5px;
    font-weight: 600;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    gap: 6px;
    transition: all 0.15s ease;
    box-shadow: 0 2px 8px rgba(14, 165, 233, 0.35);
  }

  .ota-btn-label {
    display: inline;
  }

  @media (max-width: 600px) {
    .ota-btn-label {
      display: none;
    }
    .ota-btn-primary {
      padding: 8px 10px;
    }
  }

  .ota-btn-primary:hover {
    filter: brightness(1.1);
    transform: translateY(-1px);
  }

  .ota-btn-primary:active {
    transform: translateY(0);
  }

  .ota-btn-ghost {
    background: transparent;
    border: none;
    color: #64748b;
    padding: 6px;
    border-radius: 6px;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: color 0.15s ease;
  }

  .ota-btn-ghost:hover {
    color: #cbd5e1;
    background: rgba(255, 255, 255, 0.06);
  }

  /* ─────────────────────────────────────────────────────────────
     Fullscreen Updating Overlay (KvikApp Style)
     ───────────────────────────────────────────────────────────── */
  .ota-overlay-backdrop {
    position: fixed;
    inset: 0;
    z-index: 999999;
    background: rgba(9, 14, 18, 0.96);
    backdrop-filter: blur(20px);
    -webkit-backdrop-filter: blur(20px);
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 24px;
    text-align: center;
    user-select: none;
    animation: otaFadeIn 0.3s ease-out forwards;
  }

  @keyframes otaFadeIn {
    from { opacity: 0; }
    to { opacity: 1; }
  }

  .ota-overlay-box {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    max-width: 360px;
  }

  .ota-logo-container {
    position: relative;
    width: 84px;
    height: 84px;
    margin-bottom: 36px;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .ota-spin-ring {
    position: absolute;
    inset: -22px;
    border-radius: 50%;
    border: 3.5px solid rgba(255, 255, 255, 0.08);
    border-top-color: #0ea5e9;
    border-right-color: #38bdf8;
    animation: otaSpin 1s cubic-bezier(0.55, 0.15, 0.45, 0.85) infinite;
  }

  @keyframes otaSpin {
    from { transform: rotate(0deg); }
    to { transform: rotate(360deg); }
  }

  .ota-logo-inner {
    width: 100%;
    height: 100%;
    border-radius: 50%;
    background: #101c22;
    border: 2px solid rgba(255, 255, 255, 0.15);
    box-shadow: 0 0 25px rgba(14, 165, 233, 0.25);
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: hidden;
    padding: 10px;
    box-sizing: border-box;
  }

  .ota-logo-img {
    width: 100%;
    height: 100%;
    object-fit: contain;
    animation: otaPulse 2s ease-in-out infinite;
  }

  @keyframes otaPulse {
    0%, 100% { transform: scale(1); opacity: 0.95; }
    50% { transform: scale(1.05); opacity: 1; }
  }

  .ota-updating-title {
    font-size: 22px;
    font-weight: 800;
    color: #ffffff;
    margin: 0 0 10px 0;
    letter-spacing: -0.01em;
    animation: otaPulse 2s ease-in-out infinite;
  }

  .ota-updating-sub {
    font-size: 13.5px;
    color: #94a3b8;
    line-height: 1.5;
    margin: 0 0 20px 0;
  }

  .ota-dots-indicator {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .ota-dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background-color: #0ea5e9;
    box-shadow: 0 0 8px rgba(14, 165, 233, 0.6);
    animation: otaDotBounce 1.4s infinite ease-in-out both;
  }

  @keyframes otaDotBounce {
    0%, 80%, 100% {
      transform: scale(0.6);
      opacity: 0.4;
    }
    40% {
      transform: scale(1.3);
      opacity: 1;
    }
  }

  /* Progress Bar */
  .ota-progress-bar-wrap {
    width: 100%;
    max-width: 280px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 8px;
    margin-top: 4px;
  }

  .ota-progress-track {
    width: 100%;
    height: 8px;
    background: rgba(255, 255, 255, 0.1);
    border-radius: 999px;
    overflow: hidden;
    position: relative;
    box-shadow: inset 0 1px 2px rgba(0, 0, 0, 0.4);
  }

  .ota-progress-fill {
    height: 100%;
    background: linear-gradient(90deg, #0ea5e9, #38bdf8);
    border-radius: 999px;
    transition: width 0.25s ease-out;
    box-shadow: 0 0 10px rgba(56, 189, 248, 0.6);
  }

  .ota-progress-text {
    font-size: 13px;
    font-weight: 600;
    color: #38bdf8;
    letter-spacing: 0.02em;
  }

  /* Error State Styles */
  .ota-error-icon-box {
    width: 84px;
    height: 84px;
    border-radius: 50%;
    background: rgba(239, 68, 68, 0.12);
    border: 2px solid rgba(239, 68, 68, 0.35);
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 0 0 25px rgba(239, 68, 68, 0.2);
  }

  .ota-error-title {
    color: #f87171;
    animation: none;
  }

  .ota-error-sub {
    color: #cbd5e1;
    max-height: 80px;
    overflow-y: auto;
    word-break: break-word;
  }

  .ota-dialog-actions {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-top: 10px;
  }

  .ota-btn-secondary {
    background: rgba(255, 255, 255, 0.08);
    color: #cbd5e1;
    border: 1px solid rgba(255, 255, 255, 0.15);
    padding: 7px 16px;
    border-radius: 8px;
    font-size: 12.5px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .ota-btn-secondary:hover {
    background: rgba(255, 255, 255, 0.14);
    color: #ffffff;
  }

  /* ─────────────────────────────────────────────────────────────
     Light Theme Support
     ───────────────────────────────────────────────────────────── */
  :global(html:not(.omd-dark)) .ota-banner-card {
    background: rgba(255, 255, 255, 0.94);
    border: 1px solid rgba(41, 150, 204, 0.35);
    box-shadow: 0 12px 30px rgba(16, 28, 34, 0.12), 0 2px 10px rgba(41, 150, 204, 0.1);
    color: #101c22;
  }

  :global(html:not(.omd-dark)) .ota-banner-icon-wrap {
    background: rgba(41, 150, 204, 0.12);
    border: 1px solid rgba(41, 150, 204, 0.3);
    color: #184562;
  }

  :global(html:not(.omd-dark)) .ota-ping-dot {
    background-color: #2996cc;
    box-shadow: 0 0 8px #2996cc;
  }

  :global(html:not(.omd-dark)) .ota-banner-title {
    color: #101c22;
  }

  :global(html:not(.omd-dark)) .ota-banner-sub {
    color: #546b75;
  }

  :global(html:not(.omd-dark)) .ota-btn-ghost {
    color: #889fa8;
  }

  :global(html:not(.omd-dark)) .ota-btn-ghost:hover {
    color: #101c22;
    background: rgba(0, 0, 0, 0.06);
  }

  :global(html:not(.omd-dark)) .ota-overlay-backdrop {
    background: rgba(246, 248, 249, 0.96);
  }

  :global(html:not(.omd-dark)) .ota-logo-inner {
    background: #ffffff;
    border: 2px solid rgba(41, 150, 204, 0.25);
    box-shadow: 0 0 25px rgba(41, 150, 204, 0.2);
  }

  :global(html:not(.omd-dark)) .ota-spin-ring {
    border-color: rgba(0, 0, 0, 0.08);
    border-top-color: #0ea5e9;
    border-right-color: #38bdf8;
  }

  :global(html:not(.omd-dark)) .ota-updating-title {
    color: #101c22;
  }

  :global(html:not(.omd-dark)) .ota-updating-sub {
    color: #546b75;
  }

  :global(html:not(.omd-dark)) .ota-progress-track {
    background: rgba(0, 0, 0, 0.08);
  }

  :global(html:not(.omd-dark)) .ota-btn-secondary {
    background: rgba(0, 0, 0, 0.06);
    color: #334155;
    border: 1px solid rgba(0, 0, 0, 0.15);
  }

  :global(html:not(.omd-dark)) .ota-btn-secondary:hover {
    background: rgba(0, 0, 0, 0.1);
    color: #0f172a;
  }
</style>
