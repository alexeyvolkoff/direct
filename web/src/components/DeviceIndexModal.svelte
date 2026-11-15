<script>
  import { createEventDispatcher } from "svelte";
  import { fade, scale } from "svelte/transition";
  import { _ } from "../lib/i18n/index.js";
  import { clientIndexer, indexerStore, scanDeviceForIndexable } from "../lib/client-indexer.js";

  export let show = false;
  export let embedded = false;
  export let isDarkTheme = true;
  export let deviceDrive = null;
  export let httpDrive = null;
  export let chatStore = null;
  export let extraCards = [];
  export let deviceName = "Mobile";
  export let owner = "anonymous";

  const dispatch = createEventDispatcher();

  let includeDocs = true;
  let includePhotos = true;
  let timelineDays = 30;
  let previewCounts = null;
  let scanningPreview = false;

  $: state = $indexerStore;
  $: isBusy = state.status === "scanning" || state.status === "running" || state.status === "paused";
  $: pct = state.total > 0 ? Math.min(100, Math.round((state.processed / state.total) * 100)) : 0;

  function formatEta(sec) {
    if (!sec || sec <= 0) return "";
    if (sec < 60) return `~${sec}s`;
    const m = Math.floor(sec / 60);
    const s = sec % 60;
    if (m < 60) return s > 0 ? `~${m}m ${s}s` : `~${m}m`;
    const h = Math.floor(m / 60);
    return `~${h}h ${m % 60}m`;
  }

  async function refreshPreview() {
    if (!deviceDrive || isBusy) return;
    scanningPreview = true;
    try {
      const res = await scanDeviceForIndexable(deviceDrive, {
        deviceName,
        includeDocs,
        includePhotos,
        timelineDays,
        chatStore,
        extraCards,
      });
      previewCounts = {
        docs: res.docs.length,
        photos: res.photos.length,
        skipped: res.skippedCount,
      };
    } catch (_) {
      previewCounts = null;
    } finally {
      scanningPreview = false;
    }
  }

  $: if ((show || embedded) && deviceDrive && state.status === "idle") {
    includeDocs;
    includePhotos;
    timelineDays;
    refreshPreview();
  }

  async function startIndexing() {
    if (!deviceDrive || !httpDrive || !chatStore) return;
    await clientIndexer.run({
      drive: deviceDrive,
      httpDrive,
      chatStore,
      extraCards,
      deviceName,
      owner,
      includeDocs,
      includePhotos,
      timelineDays,
    });
    dispatch("indexed");
  }

  function runInBackground() {
    dispatch("background");
    dispatch("close");
  }

  function closeModal() {
    dispatch("close");
  }
</script>

{#if embedded}
  <div class="dim-embedded" class:light-theme={!isDarkTheme} class:dark-theme={isDarkTheme}>
    <div class="dim-body dim-body-embedded">
      {#if !deviceDrive}
        <div class="dim-notice">
          {$_("device_index_no_drive", {}, "Local device storage is not available in this browser session.")}
        </div>
      {:else if !httpDrive}
        <div class="dim-notice">
          {$_("device_index_no_ai", {}, "Connect to an AI node (or enable Public AI Node) to vectorize and recognize files.")}
        </div>
      {:else}
        <div class="dim-options" class:disabled={isBusy}>
          <label class="dim-check">
            <input type="checkbox" bind:checked={includeDocs} disabled={isBusy} />
            <span>{$_("device_index_docs", {}, "Documents & Downloads (PDF, DOCX, TXT, MD)")}</span>
          </label>
          <label class="dim-check">
            <input type="checkbox" bind:checked={includePhotos} disabled={isBusy} />
            <span>{$_("device_index_photos", {}, "Photos (Camera & Pictures via Vision AI)")}</span>
          </label>

          {#if includePhotos}
            <div class="dim-row">
              <label class="dim-label" for="dim-timeline-emb">{$_("device_index_timeline", {}, "Photo timeline depth")}</label>
              <select id="dim-timeline-emb" class="dim-select" bind:value={timelineDays} disabled={isBusy}>
                <option value={7}>{$_("timeline_7d", {}, "Last 7 days")}</option>
                <option value={30}>{$_("timeline_30d", {}, "Last 30 days")}</option>
                <option value={90}>{$_("timeline_90d", {}, "Last 3 months")}</option>
                <option value={365}>{$_("timeline_365d", {}, "Last year")}</option>
                <option value={0}>{$_("timeline_all", {}, "All photos")}</option>
              </select>
            </div>
          {/if}
        </div>

        {#if state.status === "idle" || state.status === "done" || state.status === "error"}
          {#if scanningPreview}
            <div class="dim-preview">{$_("device_index_scanning", {}, "Scanning device folders…")}</div>
          {:else if previewCounts}
            <div class="dim-preview">
              {$_("device_index_found", { docs: previewCounts.docs, photos: previewCounts.photos, skipped: previewCounts.skipped }, `Found ${previewCounts.docs} new docs, ${previewCounts.photos} new photos (${previewCounts.skipped} already indexed).`)}
            </div>
          {/if}
        {/if}

        {#if isBusy || state.status === "done"}
          <div class="dim-progress-box">
            <div class="dim-progress-top">
              <span class="dim-status-label">
                {#if state.status === "scanning"}
                  {$_("device_index_scanning", {}, "Scanning device folders…")}
                {:else if state.status === "paused"}
                  {$_("device_index_paused", {}, "Paused")} ({state.processed}/{state.total})
                {:else if state.status === "done"}
                  {$_("device_index_done", {}, "Indexing complete")}
                {:else}
                  {state.processed} / {state.total} ({pct}%)
                {/if}
              </span>
              {#if state.etaSeconds > 0 && state.status === "running"}
                <span class="dim-eta">{$_("device_index_eta", { eta: formatEta(state.etaSeconds) }, `ETA ${formatEta(state.etaSeconds)}`)}</span>
              {/if}
            </div>

            <div class="dim-bar-track">
              <div class="dim-bar-fill" style="width: {state.status === 'done' ? 100 : pct}%"></div>
            </div>

            {#if state.currentFile}
              <div class="dim-current" title={state.currentFile}>{state.currentFile}</div>
            {/if}

            <div class="dim-stats">
              <span>✓ {state.indexed} {$_("device_index_indexed", {}, "indexed")}</span>
              <span>• {state.skipped} {$_("device_index_skipped", {}, "skipped")}</span>
              {#if state.failed > 0}
                <span class="dim-err">! {state.failed} {$_("device_index_failed", {}, "failed")}</span>
              {/if}
            </div>
          </div>
        {/if}

        {#if state.status === "error" && state.error}
          <div class="dim-notice dim-err">{state.error}</div>
        {/if}

        <div class="dim-inline-actions">
          {#if state.status === "running"}
            <button type="button" class="dim-btn secondary" on:click={() => clientIndexer.pause()}>
              {$_("pause", {}, "Pause")}
            </button>
            <button type="button" class="dim-btn danger" on:click={() => clientIndexer.stop()}>
              {$_("device_index_stop", {}, "Stop")}
            </button>
          {:else if state.status === "paused"}
            <button type="button" class="dim-btn primary" on:click={() => clientIndexer.resume()}>
              {$_("resume", {}, "Resume")}
            </button>
            <button type="button" class="dim-btn danger" on:click={() => clientIndexer.stop()}>
              {$_("device_index_stop", {}, "Stop")}
            </button>
          {:else if state.status !== "done"}
            <button
              type="button"
              class="dim-btn primary"
              disabled={!deviceDrive || !httpDrive || (!includeDocs && !includePhotos) || scanningPreview}
              on:click={startIndexing}
            >
              {$_("device_index_start", {}, "Start Indexing")}
            </button>
          {/if}
        </div>
      {/if}
    </div>
  </div>
{:else if show}
  <div
    class="dim-backdrop"
    class:light-theme={!isDarkTheme}
    class:dark-theme={isDarkTheme}
    transition:fade={{ duration: 150 }}
    on:click|self={closeModal}
    role="dialog"
    aria-modal="true"
  >
    <div class="dim-card" transition:scale={{ duration: 160, start: 0.96 }}>
      <div class="dim-header">
        <div class="dim-title-wrap">
          <span class="dim-icon">📱</span>
          <div>
            <h3 class="dim-title">{$_("device_index_title", {}, "Index This Device")}</h3>
            <p class="dim-sub">{$_("device_index_subtitle", {}, "Make documents & photos on this device searchable in AI chat.")}</p>
          </div>
        </div>
        <button type="button" class="dim-close" on:click={closeModal} aria-label="Close">×</button>
      </div>

      <div class="dim-body">
        {#if !deviceDrive}
          <div class="dim-notice">
            {$_("device_index_no_drive", {}, "Local device storage is not available in this browser session.")}
          </div>
        {:else if !httpDrive}
          <div class="dim-notice">
            {$_("device_index_no_ai", {}, "Connect to an AI node (or enable Public AI Node) to vectorize and recognize files.")}
          </div>
        {:else}
          <div class="dim-options" class:disabled={isBusy}>
            <label class="dim-check">
              <input type="checkbox" bind:checked={includeDocs} disabled={isBusy} />
              <span>{$_("device_index_docs", {}, "Documents & Downloads (PDF, DOCX, TXT, MD)")}</span>
            </label>
            <label class="dim-check">
              <input type="checkbox" bind:checked={includePhotos} disabled={isBusy} />
              <span>{$_("device_index_photos", {}, "Photos (Camera & Pictures via Vision AI)")}</span>
            </label>

            {#if includePhotos}
              <div class="dim-row">
                <label class="dim-label" for="dim-timeline">{$_("device_index_timeline", {}, "Photo timeline depth")}</label>
                <select id="dim-timeline" class="dim-select" bind:value={timelineDays} disabled={isBusy}>
                  <option value={7}>{$_("timeline_7d", {}, "Last 7 days")}</option>
                  <option value={30}>{$_("timeline_30d", {}, "Last 30 days")}</option>
                  <option value={90}>{$_("timeline_90d", {}, "Last 3 months")}</option>
                  <option value={365}>{$_("timeline_365d", {}, "Last year")}</option>
                  <option value={0}>{$_("timeline_all", {}, "All photos")}</option>
                </select>
              </div>
            {/if}
          </div>

          {#if state.status === "idle" || state.status === "done" || state.status === "error"}
            {#if scanningPreview}
              <div class="dim-preview">{$_("device_index_scanning", {}, "Scanning device folders…")}</div>
            {:else if previewCounts}
              <div class="dim-preview">
                {$_("device_index_found", { docs: previewCounts.docs, photos: previewCounts.photos, skipped: previewCounts.skipped }, `Found ${previewCounts.docs} new docs, ${previewCounts.photos} new photos (${previewCounts.skipped} already indexed).`)}
              </div>
            {/if}
          {/if}

          {#if isBusy || state.status === "done"}
            <div class="dim-progress-box">
              <div class="dim-progress-top">
                <span class="dim-status-label">
                  {#if state.status === "scanning"}
                    {$_("device_index_scanning", {}, "Scanning device folders…")}
                  {:else if state.status === "paused"}
                    {$_("device_index_paused", {}, "Paused")} ({state.processed}/{state.total})
                  {:else if state.status === "done"}
                    {$_("device_index_done", {}, "Indexing complete")}
                  {:else}
                    {state.processed} / {state.total} ({pct}%)
                  {/if}
                </span>
                {#if state.etaSeconds > 0 && state.status === "running"}
                  <span class="dim-eta">{$_("device_index_eta", { eta: formatEta(state.etaSeconds) }, `ETA ${formatEta(state.etaSeconds)}`)}</span>
                {/if}
              </div>

              <div class="dim-bar-track">
                <div class="dim-bar-fill" style="width: {state.status === 'done' ? 100 : pct}%"></div>
              </div>

              {#if state.currentFile}
                <div class="dim-current" title={state.currentFile}>{state.currentFile}</div>
              {/if}

              <div class="dim-stats">
                <span>✓ {state.indexed} {$_("device_index_indexed", {}, "indexed")}</span>
                <span>• {state.skipped} {$_("device_index_skipped", {}, "skipped")}</span>
                {#if state.failed > 0}
                  <span class="dim-err">! {state.failed} {$_("device_index_failed", {}, "failed")}</span>
                {/if}
              </div>
            </div>
          {/if}

          {#if state.status === "error" && state.error}
            <div class="dim-notice dim-err">{state.error}</div>
          {/if}
        {/if}
      </div>

      <div class="dim-footer">
        {#if state.status === "running"}
          <button type="button" class="dim-btn secondary" on:click={runInBackground}>
            {$_("device_index_background", {}, "Run in background")}
          </button>
          <button type="button" class="dim-btn secondary" on:click={() => clientIndexer.pause()}>
            {$_("pause", {}, "Pause")}
          </button>
          <button type="button" class="dim-btn danger" on:click={() => clientIndexer.stop()}>
            {$_("device_index_stop", {}, "Stop")}
          </button>
        {:else if state.status === "paused"}
          <button type="button" class="dim-btn secondary" on:click={runInBackground}>
            {$_("device_index_background", {}, "Run in background")}
          </button>
          <button type="button" class="dim-btn primary" on:click={() => clientIndexer.resume()}>
            {$_("resume", {}, "Resume")}
          </button>
          <button type="button" class="dim-btn danger" on:click={() => clientIndexer.stop()}>
            {$_("device_index_stop", {}, "Stop")}
          </button>
        {:else}
          <button type="button" class="dim-btn secondary" on:click={closeModal}>
            {$_("close", {}, "Close")}
          </button>
          <button
            type="button"
            class="dim-btn primary"
            disabled={!deviceDrive || !httpDrive || (!includeDocs && !includePhotos) || scanningPreview}
            on:click={startIndexing}
          >
            {$_("device_index_start", {}, "Start Indexing")}
          </button>
        {/if}
      </div>
    </div>
  </div>
{/if}

<style>
  .dim-backdrop, .dim-embedded {
    --dim-bg: #1e2028;
    --dim-surface: #272a35;
    --dim-border: rgba(255, 255, 255, 0.12);
    --dim-text: #f1f3f7;
    --dim-muted: #9aa0ae;
    --dim-accent: #5b8def;
    --dim-danger: #e55353;
    color: var(--dim-text);
  }
  .dim-backdrop.light-theme, .dim-embedded.light-theme {
    --dim-bg: #ffffff;
    --dim-surface: #f4f6f9;
    --dim-border: rgba(15, 23, 42, 0.12);
    --dim-text: #111827;
    --dim-muted: #6b7280;
    --dim-accent: #2563eb;
    --dim-danger: #dc2626;
  }
  .dim-backdrop {
    position: fixed;
    inset: 0;
    z-index: 1200;
    background: rgba(0, 0, 0, 0.55);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px;
  }
  .dim-card {
    width: 100%;
    max-width: 460px;
    max-height: 90vh;
    overflow-y: auto;
    background: var(--dim-bg);
    color: var(--dim-text);
    border: 1px solid var(--dim-border);
    border-radius: 16px;
    box-shadow: 0 20px 50px rgba(0, 0, 0, 0.35);
    overflow: hidden;
  }
  .dim-header {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 12px;
    padding: 18px 20px 12px;
    border-bottom: 1px solid var(--dim-border);
  }
  .dim-title-wrap {
    display: flex;
    gap: 12px;
    align-items: flex-start;
  }
  .dim-icon {
    font-size: 22px;
    line-height: 1.2;
  }
  .dim-title {
    margin: 0;
    font-size: 16px;
    font-weight: 600;
  }
  .dim-sub {
    margin: 4px 0 0;
    font-size: 12.5px;
    color: var(--dim-muted);
    line-height: 1.4;
  }
  .dim-close {
    background: transparent;
    border: none;
    color: var(--dim-muted);
    font-size: 22px;
    line-height: 1;
    cursor: pointer;
    padding: 2px 6px;
  }
  .dim-body {
    padding: 16px 20px;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }
  .dim-body-embedded {
    padding: 4px 0 8px;
  }
  .dim-inline-actions {
    display: flex;
    gap: 10px;
    justify-content: flex-start;
    flex-wrap: wrap;
  }
  .dim-options {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .dim-options.disabled {
    opacity: 0.65;
  }
  .dim-check {
    display: flex;
    align-items: center;
    gap: 10px;
    font-size: 13.5px;
    cursor: pointer;
  }
  .dim-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    margin-top: 4px;
  }
  .dim-label {
    font-size: 13px;
    color: var(--dim-muted);
  }
  .dim-select {
    appearance: none;
    -webkit-appearance: none;
    background-color: var(--dim-surface);
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'%3E%3Cpath fill='none' stroke='%239aa0ae' stroke-width='1.5' stroke-linecap='round' stroke-linejoin='round' d='M3 4.5L6 7.5L9 4.5'/%3E%3C/svg%3E");
    background-repeat: no-repeat;
    background-position: right 10px center;
    background-size: 12px;
    color: var(--dim-text);
    border: 1px solid var(--dim-border);
    border-radius: 8px;
    padding: 6px 28px 6px 10px;
    font-size: 13px;
  }
  .dim-preview, .dim-notice {
    background: var(--dim-surface);
    border: 1px solid var(--dim-border);
    border-radius: 10px;
    padding: 10px 12px;
    font-size: 12.5px;
    color: var(--dim-muted);
  }
  .dim-progress-box {
    background: var(--dim-surface);
    border: 1px solid var(--dim-border);
    border-radius: 12px;
    padding: 12px 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .dim-progress-top {
    display: flex;
    justify-content: space-between;
    align-items: center;
    font-size: 13px;
    font-weight: 500;
  }
  .dim-eta {
    color: var(--dim-accent);
    font-variant-numeric: tabular-nums;
  }
  .dim-bar-track {
    height: 7px;
    background: var(--dim-border);
    border-radius: 999px;
    overflow: hidden;
  }
  .dim-bar-fill {
    height: 100%;
    background: var(--dim-accent);
    border-radius: 999px;
    transition: width 0.25s ease;
  }
  .dim-current {
    font-size: 12px;
    color: var(--dim-muted);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .dim-stats {
    display: flex;
    gap: 14px;
    font-size: 12px;
    color: var(--dim-muted);
  }
  .dim-err {
    color: var(--dim-danger);
  }
  .dim-footer {
    display: flex;
    justify-content: flex-end;
    flex-wrap: wrap;
    gap: 10px;
    padding: 12px 20px 18px;
    border-top: 1px solid var(--dim-border);
  }
  .dim-btn {
    border-radius: 10px;
    padding: 8px 14px;
    font-size: 13.5px;
    font-weight: 500;
    cursor: pointer;
    border: 1px solid transparent;
  }
  .dim-btn:disabled {
    opacity: 0.45;
    cursor: not-allowed;
  }
  .dim-btn.primary {
    background: var(--dim-accent);
    color: #fff;
  }
  .dim-btn.secondary {
    background: var(--dim-surface);
    color: var(--dim-text);
    border-color: var(--dim-border);
  }
  .dim-btn.danger {
    background: var(--dim-danger);
    color: #fff;
  }
</style>
