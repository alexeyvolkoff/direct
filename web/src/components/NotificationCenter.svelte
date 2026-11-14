<script>
  import { createEventDispatcher } from "svelte";
  import { _ } from "../lib/i18n/index.js";
  import {
    unreadNotifications,
    activeNotificationToast,
    notificationsModalOpen,
    dismissNotificationToast,
    markNotificationRead,
    markAllNotificationsRead,
    closeNotificationsModal
  } from "../lib/notification-inbox.js";

  export let fetchFn = null;
  export let omdKey = "";

  const dispatch = createEventDispatcher();

  function formatTime(ts) {
    if (!ts) return "";
    try {
      return new Date(ts).toLocaleString(undefined, {
        month: "short",
        day: "numeric",
        hour: "2-digit",
        minute: "2-digit"
      });
    } catch (_) {
      return "";
    }
  }

  function isSafePreviewSrc(url) {
    if (!url || typeof url !== "string") return false;
    return /^(https?:\/\/|data:image\/|blob:)/i.test(url.trim());
  }

  function hasOpenTarget(item) {
    const d = item?.data || {};
    return Boolean(d.widget_url || d.action_url || (item?.url && item.url !== "/"));
  }

  async function triggerAction(action, item) {
    await markNotificationRead(item.id, fetchFn, omdKey);
    closeNotificationsModal();
    dispatch("action", { action: action || "open", payload: item });
  }

  async function dismissItem(item) {
    await markNotificationRead(item.id, fetchFn, omdKey);
  }

  async function dismissAll() {
    await markAllNotificationsRead(fetchFn, omdKey);
    closeNotificationsModal();
  }
</script>

{#if $activeNotificationToast}
  {@const toast = $activeNotificationToast}
  <div class="omd-rich-toast" role="status" aria-live="polite">
    <div class="omd-rich-toast-head">
      <div class="omd-rich-toast-icon" aria-hidden="true">
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9" />
          <path d="M13.73 21a2 2 0 0 1-3.46 0" />
        </svg>
      </div>
      <div class="omd-rich-toast-copy">
        <div class="omd-rich-toast-title">{toast.title}</div>
        {#if toast.body}
          <div class="omd-rich-toast-body">{toast.body}</div>
        {/if}
      </div>
      <button
        type="button"
        class="omd-rich-toast-close"
        title={$_("close", {}, "Close")}
        aria-label={$_("close", {}, "Close")}
        on:click={dismissNotificationToast}
      >
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
          <line x1="18" y1="6" x2="6" y2="18" />
          <line x1="6" y1="6" x2="18" y2="18" />
        </svg>
      </button>
    </div>

    {#if isSafePreviewSrc(toast.image)}
      <img class="omd-rich-toast-img" src={toast.image} alt="" loading="lazy" />
    {/if}

    {#if (toast.actions && toast.actions.length > 0) || hasOpenTarget(toast)}
      <div class="omd-rich-toast-actions">
        {#if toast.actions && toast.actions.length > 0}
          {#each toast.actions as act}
            <button
              type="button"
              class="omd-notif-btn primary"
              on:click={() => triggerAction(act.action || "open", toast)}
            >
              {act.title || act.action || $_("open", {}, "Open")}
            </button>
          {/each}
        {:else if hasOpenTarget(toast)}
          <button
            type="button"
            class="omd-notif-btn primary"
            on:click={() => triggerAction("open", toast)}
          >
            {$_("open", {}, "Open")}
          </button>
        {/if}
        <button
          type="button"
          class="omd-notif-btn ghost"
          on:click={() => dismissItem(toast)}
        >
          {$_("dismiss", {}, "Dismiss")}
        </button>
      </div>
    {/if}
  </div>
{/if}

{#if $notificationsModalOpen}
  <!-- svelte-ignore a11y-click-events-have-key-events -->
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div class="omd-notif-backdrop" on:click|self={closeNotificationsModal}>
    <div class="omd-notif-modal" role="dialog" aria-modal="true" aria-label={$_("notifications", {}, "Notifications")}>
      <div class="omd-notif-modal-header">
        <div class="omd-notif-modal-title-wrap">
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="var(--accent)" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9" />
            <path d="M13.73 21a2 2 0 0 1-3.46 0" />
          </svg>
          <h3>{$_("notifications", {}, "Notifications")}</h3>
          {#if $unreadNotifications.length > 0}
            <span class="omd-notif-pill">{$unreadNotifications.length}</span>
          {/if}
        </div>
        <div class="omd-notif-modal-head-actions">
          {#if $unreadNotifications.length > 0}
            <button type="button" class="omd-notif-clear-btn" on:click={dismissAll}>
              {$_("clear_notifications", {}, "Dismiss notifications")}
            </button>
          {/if}
          <button
            type="button"
            class="omd-rich-toast-close"
            title={$_("close", {}, "Close")}
            aria-label={$_("close", {}, "Close")}
            on:click={closeNotificationsModal}
          >
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
              <line x1="18" y1="6" x2="6" y2="18" />
              <line x1="6" y1="6" x2="18" y2="18" />
            </svg>
          </button>
        </div>
      </div>

      <div class="omd-notif-list">
        {#if $unreadNotifications.length === 0}
          <div class="omd-notif-empty">{$_("no_unread_notifications", {}, "No unread notifications")}</div>
        {:else}
          {#each $unreadNotifications as item (item.id)}
            <div class="omd-notif-item">
              <div class="omd-notif-item-top">
                <div class="omd-notif-item-meta">
                  <span class="omd-notif-item-title">{item.title}</span>
                  {#if item.timestamp}
                    <span class="omd-notif-item-time">{formatTime(item.timestamp)}</span>
                  {/if}
                </div>
                <button
                  type="button"
                  class="omd-rich-toast-close"
                  title={$_("dismiss", {}, "Dismiss")}
                  aria-label={$_("dismiss", {}, "Dismiss")}
                  on:click={() => dismissItem(item)}
                >
                  <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                    <line x1="18" y1="6" x2="6" y2="18" />
                    <line x1="6" y1="6" x2="18" y2="18" />
                  </svg>
                </button>
              </div>
              {#if item.body}
                <div class="omd-notif-item-body">{item.body}</div>
              {/if}
              {#if isSafePreviewSrc(item.image)}
                <img class="omd-rich-toast-img" src={item.image} alt="" loading="lazy" />
              {/if}
              {#if (item.actions && item.actions.length > 0) || hasOpenTarget(item)}
                <div class="omd-rich-toast-actions">
                  {#if item.actions && item.actions.length > 0}
                    {#each item.actions as act}
                      <button
                        type="button"
                        class="omd-notif-btn primary"
                        on:click={() => triggerAction(act.action || "open", item)}
                      >
                        {act.title || act.action || $_("open", {}, "Open")}
                      </button>
                    {/each}
                  {:else if hasOpenTarget(item)}
                    <button
                      type="button"
                      class="omd-notif-btn primary"
                      on:click={() => triggerAction("open", item)}
                    >
                      {$_("open", {}, "Open")}
                    </button>
                  {/if}
                </div>
              {/if}
            </div>
          {/each}
        {/if}
      </div>
    </div>
  </div>
{/if}

<style>
  .omd-rich-toast {
    position: fixed;
    top: calc(env(safe-area-inset-top, 0px) + 16px);
    right: 16px;
    z-index: 10010;
    width: min(380px, calc(100vw - 32px));
    background: var(--bg-secondary, #ffffff);
    color: var(--text-primary, #161925);
    border: 1px solid var(--border-color, #d8dce8);
    border-radius: 14px;
    padding: 12px 14px;
    box-shadow: 0 14px 36px rgba(0, 0, 0, 0.28);
    display: flex;
    flex-direction: column;
    gap: 10px;
    animation: omdToastSlideIn 0.2s cubic-bezier(0.16, 1, 0.3, 1);
  }

  @keyframes omdToastSlideIn {
    from {
      opacity: 0;
      transform: translateY(-10px);
    }
    to {
      opacity: 1;
      transform: translateY(0);
    }
  }

  .omd-rich-toast-head {
    display: flex;
    align-items: flex-start;
    gap: 10px;
  }

  .omd-rich-toast-icon {
    width: 30px;
    height: 30px;
    border-radius: 8px;
    background: var(--accent-soft, rgba(123, 134, 196, 0.14));
    color: var(--accent, #3d467a);
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .omd-rich-toast-copy {
    flex: 1;
    min-width: 0;
  }

  .omd-rich-toast-title {
    font-size: 13.5px;
    font-weight: 600;
    color: var(--text-primary);
    line-height: 1.3;
  }

  .omd-rich-toast-body {
    font-size: 12.5px;
    color: var(--text-secondary);
    margin-top: 3px;
    line-height: 1.4;
    word-break: break-word;
  }

  .omd-rich-toast-close {
    background: transparent;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    padding: 4px;
    border-radius: 6px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }

  .omd-rich-toast-close:hover {
    background: var(--bg-card-hover);
    color: var(--text-primary);
  }

  .omd-rich-toast-img {
    width: 100%;
    max-height: 160px;
    object-fit: cover;
    border-radius: 8px;
    border: 1px solid var(--border-color);
  }

  .omd-rich-toast-actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }

  .omd-notif-btn {
    padding: 6px 12px;
    border-radius: 8px;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
    border: 1px solid transparent;
    transition: opacity 0.15s ease, background 0.15s ease;
  }

  .omd-notif-btn.primary {
    background: var(--accent);
    color: var(--accent-on-brand, #ffffff);
  }

  .omd-notif-btn.primary:hover {
    opacity: 0.92;
  }

  .omd-notif-btn.ghost {
    background: transparent;
    border-color: var(--border-color);
    color: var(--text-secondary);
  }

  .omd-notif-btn.ghost:hover {
    background: var(--bg-card-hover);
    color: var(--text-primary);
  }

  .omd-notif-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    backdrop-filter: blur(4px);
    z-index: 10020;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px;
  }

  .omd-notif-modal {
    width: 100%;
    max-width: 460px;
    max-height: 80vh;
    background: var(--bg-secondary);
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    border-radius: 16px;
    box-shadow: 0 20px 48px rgba(0, 0, 0, 0.35);
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }

  .omd-notif-modal-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    padding: 14px 18px;
    border-bottom: 1px solid var(--border-color);
  }

  .omd-notif-modal-title-wrap {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
  }

  .omd-notif-modal-title-wrap h3 {
    margin: 0;
    font-size: 15px;
    font-weight: 600;
    color: var(--text-primary);
  }

  .omd-notif-pill {
    font-size: 11px;
    font-weight: 600;
    padding: 1px 7px;
    border-radius: 999px;
    background: var(--accent-soft);
    color: var(--accent);
  }

  .omd-notif-modal-head-actions {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .omd-notif-clear-btn {
    background: transparent;
    border: 1px solid var(--border-color);
    color: var(--text-secondary);
    border-radius: 8px;
    padding: 5px 10px;
    font-size: 12px;
    cursor: pointer;
  }

  .omd-notif-clear-btn:hover {
    background: var(--bg-card-hover);
    color: var(--text-primary);
  }

  .omd-notif-list {
    overflow-y: auto;
    padding: 12px 14px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .omd-notif-empty {
    text-align: center;
    padding: 28px 16px;
    font-size: 13px;
    color: var(--text-muted);
  }

  .omd-notif-item {
    background: var(--bg-primary);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    padding: 12px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .omd-notif-item-top {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 8px;
  }

  .omd-notif-item-meta {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
  }

  .omd-notif-item-title {
    font-size: 13.5px;
    font-weight: 600;
    color: var(--text-primary);
  }

  .omd-notif-item-time {
    font-size: 11px;
    color: var(--text-muted);
  }

  .omd-notif-item-body {
    font-size: 12.5px;
    color: var(--text-secondary);
    line-height: 1.4;
    word-break: break-word;
  }
</style>

