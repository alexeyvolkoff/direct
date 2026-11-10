<script>
  import { createEventDispatcher } from "svelte";
  import { marked } from "marked";
  import markedKatex from "marked-katex-extension";
  import "katex/dist/katex.min.css";
  import "../lib/markdown-styles.css";

  export let url = "";
  export let content = "";
  export let filename = "";
  export let isDarkTheme = false;
  export let canEdit = false;

  const dispatch = createEventDispatcher();

  let markdownContent = "";
  let markdownHtml = "";
  let loading = false;
  let isFullscreen = false;

  marked.use(markedKatex({ throwOnError: false, nonStandard: true }));
  marked.setOptions({ gfm: true, breaks: false, async: false });

  $: isMarkdownView = !filename || ["md", "markdown"].includes((filename.split(".").pop() || "").toLowerCase());
  $: if (typeof content === "string" && content.length > 0) {
    loading = false;
    markdownContent = content;
    markdownHtml = marked.parse(content);
  } else if (isMarkdownView && url && !markdownContent) {
    load();
  }

  async function load() {
    loading = true;
    try {
      const res = await fetch(url);
      if (res.ok) {
        markdownContent = await res.text();
        markdownHtml = marked.parse(markdownContent || "");
      }
    } catch (e) {
      console.warn("[MarkdownViewer] Failed to load:", e);
    } finally {
      loading = false;
    }
  }

  function toggleFullscreen() {
    const el = document.documentElement;
    if (!document.fullscreenElement && !document.webkitFullscreenElement) {
      if (el.requestFullscreen) el.requestFullscreen().catch(() => {});
      else if (el.webkitRequestFullscreen) el.webkitRequestFullscreen();
    } else {
      if (document.exitFullscreen) document.exitFullscreen().catch(() => {});
      else if (document.webkitExitFullscreen) document.webkitExitFullscreen();
    }
  }

  function onEdit() {
    dispatch("edit", { content: markdownContent, filename, url });
  }

  function onClose() {
    dispatch("close");
  }
</script>

<div class="md-viewer md-view {isDarkTheme ? 'dark-theme' : 'light-theme'}">
  <div class="md-toolbar">
    <div class="md-toolbar-left">
      <span class="md-filename">{filename}</span>
    </div>
    <div class="md-toolbar-right">
      {#if canEdit}
        <button class="md-toolbar-btn" on:click={onEdit} title="Edit" aria-label="Edit">
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 20h9"/><path d="M16.5 3.5a2.121 2.121 0 0 1 3 3L7 19l-4 1 1-4L16.5 3.5z"/></svg>
        </button>
      {/if}
      <button class="md-toolbar-btn" on:click={toggleFullscreen} title="Full Screen View" aria-label="Full Screen View">
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
          <path d="M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3"/>
        </svg>
      </button>
      <button class="md-toolbar-btn" on:click={onClose} title="Close" aria-label="Close">
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
      </button>
    </div>
  </div>
  <div class="md-content">
    {#if loading}
      <div class="md-loading"><div class="spinner"></div></div>
    {:else}
      <div class="markdown-body">
        {@html markdownHtml || ""}
      </div>
    {/if}
  </div>
</div>

<style>
  .md-viewer {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    min-height: 60vh;
    background: var(--md-stage-bg);
    color: inherit;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
  }

  .md-toolbar {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 8px 12px;
    border-bottom: 1px solid var(--md-border);
    gap: 8px;
  }

  .md-toolbar-left {
    display: flex;
    align-items: center;
    min-width: 0;
  }

  .md-filename {
    font-size: 13px;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .md-toolbar-right {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .md-toolbar-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 36px;
    height: 36px;
    padding: 0;
    font-size: 13px;
    font-weight: 600;
    border: 1px solid var(--md-border-strong);
    border-radius: 8px;
    background: var(--md-code-bg);
    color: inherit;
    cursor: pointer;
    transition: background 0.15s ease;
  }

  .md-toolbar-btn svg {
    flex: none;
  }

  .md-toolbar-btn:hover {
    background: var(--md-inline-code-bg);
  }

  .md-content {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    background: var(--md-stage-bg);
    color: inherit;
    padding-bottom: calc(24px + env(safe-area-inset-bottom, 0px));
    box-sizing: border-box;
  }

  .md-loading {
    display: flex;
    align-items: center;
    justify-content: center;
    height: 200px;
  }

  .spinner {
    width: 28px;
    height: 28px;
    border: 3px solid var(--md-border);
    border-top-color: var(--md-link);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }
</style>
