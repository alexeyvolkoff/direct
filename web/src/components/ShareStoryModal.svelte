<script>
  import { createEventDispatcher } from "svelte";
  import { translate, locale } from "../lib/i18n/index.js";
  import {
    suggestStoryFolderName,
    sanitizeStoryFolderName,
    splitPrefixedFolderPath,
    composeAndCreateStoryFolder,
  } from "../lib/story-composer.js";

  export let show = false;
  export let isDarkTheme = true;
  export let items = [];
  export let albumTitle = "";
  export let explicitParentFolderId = "";
  export let isCreations = false;
  export let targetDevice = null;
  export let storageDevice = null;
  export let deviceDrive = null;
  export let httpDrive = null;
  export let defaultStorage = "";
  export let omdKey = "";

  const dispatch = createEventDispatcher();

  let subfolderName = "";
  let plotHint = "";
  let excludedIndices = new Set();
  let isWorking = false;
  let progressStage = "";
  let progressDetail = "";
  let errorMsg = "";
  let result = null;
  let abortCtrl = null;
  let wasOpen = false;

  $: if (show && !wasOpen) {
    wasOpen = true;
    excludedIndices = new Set();
    plotHint = "";
    errorMsg = "";
    result = null;
    progressStage = "";
    progressDetail = "";
    subfolderName = suggestStoryFolderName({
      albumTitle,
      items,
      isCreations,
    });
  } else if (!show && wasOpen) {
    wasOpen = false;
    if (abortCtrl) {
      try {
        abortCtrl.abort();
      } catch (_) {}
      abortCtrl = null;
    }
    isWorking = false;
  }

  $: activeItems = (items || []).filter((_, idx) => !excludedIndices.has(idx));

  $: parentFolderDisplay = (() => {
    if (explicitParentFolderId && explicitParentFolderId !== "/") {
      return explicitParentFolderId.replace(/\/+$/, "");
    }
    for (const it of items || []) {
      const fullId = String(it?.docId || it?.raw?.id || it?.id || it?.path || "");
      if (fullId.includes("/")) {
        const p = fullId.slice(0, fullId.lastIndexOf("/"));
        if (p && p !== "/") return p;
      }
    }
    return defaultStorage ? `/${String(defaultStorage).replace(/^\/+|\/+$/g, "")}` : "/Pictures";
  })();

  $: detectedFactsSummary = (() => {
    const chips = [];
    const seen = new Set();
    const pushChip = (icon, val) => {
      const v = String(val || "").trim();
      if (!v || seen.has(v.toLowerCase()) || chips.length >= 8) return;
      seen.add(v.toLowerCase());
      chips.push(`${icon} ${v}`);
    };
    for (const it of activeItems) {
      for (const p of it?.people || []) pushChip("👤", p);
      if (it?.persona) pushChip("🎭", it.persona);
      if (it?.landmark) pushChip("🏛️", it.landmark);
      if (it?.namedEvent || it?.event) pushChip("🎉", it.namedEvent || it.event);
      if (it?.city || it?.place) pushChip("📍", it.city || it.place);
    }
    return chips;
  })();

  function getItemThumbUrl(it) {
    if (!it) return "";
    if (it.thumbBlobUrl) return it.thumbBlobUrl;
    if (it.blobUrl) return it.blobUrl;
    if (it.inlinePreview) return it.inlinePreview;
    if (it.previewUrl) return it.previewUrl;
    const raw = it.raw || it;
    if (raw.preview) return raw.preview;
    const fullId = String(it.docId || raw.id || it.path || "");
    const sp = splitPrefixedFolderPath(fullId);
    if (sp.prefix && sp.realPath && !sp.device?.local) {
      return `/p2p/${sp.prefix}${encodeURI(sp.realPath)}`;
    }
    return "";
  }

  function getItemLabel(it, idx) {
    return String(
      it?.filename ||
      it?.name ||
      it?.raw?.name ||
      it?.raw?.value ||
      `Frame ${idx + 1}`
    );
  }

  function isVideoThumbItem(it, idx) {
    return Boolean(it?.isVideo || /\.(mp4|webm|mov|m4v|mkv)$/i.test(getItemLabel(it, idx)));
  }

  function toggleItem(idx) {
    if (isWorking) return;
    const next = new Set(excludedIndices);
    if (next.has(idx)) {
      next.delete(idx);
    } else if (activeItems.length > 1) {
      next.add(idx);
    }
    excludedIndices = next;
  }

  function closeModal() {
    if (isWorking && abortCtrl) {
      try {
        abortCtrl.abort();
      } catch (_) {}
      abortCtrl = null;
      isWorking = false;
    }
    dispatch("close");
  }

  async function startComposeStory() {
    if (isWorking) return;
    const cleanSub = sanitizeStoryFolderName(subfolderName);
    if (!cleanSub) {
      errorMsg = translate("story_subfolder_label", {}, "Story subfolder");
      return;
    }
    if (activeItems.length === 0) {
      errorMsg = translate("story_select_items", {}, "Select at least one media item");
      return;
    }

    errorMsg = "";
    isWorking = true;
    progressStage = "copying";
    progressDetail = `0 / ${activeItems.length}`;
    abortCtrl = new AbortController();

    try {
      const created = await composeAndCreateStoryFolder({
        rawItems: activeItems,
        subfolderName: cleanSub,
        plotHint,
        albumTitle,
        explicitParentFolderId,
        targetDevice,
        storageDevice,
        deviceDrive,
        httpDrive,
        defaultStorage,
        omdKey,
        localeCode: $locale || "en",
        isCreations,
        signal: abortCtrl.signal,
        onProgress: ({ stage, detail }) => {
          progressStage = stage;
          progressDetail = detail || "";
        },
      });
      result = created;
      dispatch("story-created", created);
    } catch (err) {
      if (err?.name === "AbortError") {
        errorMsg = "";
      } else {
        errorMsg = err?.message || String(err);
      }
    } finally {
      isWorking = false;
      abortCtrl = null;
    }
  }

  function handleOpenStory() {
    if (!result) return;
    dispatch("open-story", result);
    dispatch("close");
  }

  function handleShareLink() {
    if (!result) return;
    dispatch("share-folder", result);
    dispatch("close");
  }

  function handlePublishCivitai() {
    if (!result) return;
    dispatch("publish-civitai", result);
    dispatch("close");
  }
</script>

{#if show}
  <!-- svelte-ignore a11y-click-events-have-key-events -->
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div
    class="omd-story-modal-backdrop {isDarkTheme ? 'dark-theme' : 'light-theme'}"
    on:click|self={closeModal}
  >
    <div class="omd-story-modal-card" role="dialog" aria-modal="true">
      <header class="omd-story-modal-header">
        <div class="omd-story-modal-title-wrap">
          <span class="omd-story-modal-icon">📖</span>
          <div>
            <h2>{translate("story_modal_title", {}, "Share a Story")}</h2>
            <p>{translate("story_modal_subtitle", {}, "Copy selected media into a story folder and compose an illustrated mini-comic")}</p>
          </div>
        </div>
        <button type="button" class="omd-story-close-btn" on:click={closeModal} aria-label="Close">✕</button>
      </header>

      {#if !result}
        <div class="omd-story-modal-body">
          <!-- Selected frames strip -->
          <div class="omd-story-field">
            <div class="omd-story-label-row">
              <span>{translate("story_selected_count", { count: activeItems.length }, `${activeItems.length} media selected`)}</span>
              {#if albumTitle}
                <span class="omd-story-album-pill">{albumTitle}</span>
              {/if}
            </div>
            <div class="omd-story-thumbs-strip">
              {#each items || [] as item, idx}
                {@const thumb = getItemThumbUrl(item)}
                {@const excluded = excludedIndices.has(idx)}
                <button
                  type="button"
                  class="omd-story-thumb-item"
                  class:excluded
                  disabled={isWorking}
                  on:click={() => toggleItem(idx)}
                  title={getItemLabel(item, idx)}
                >
                  {#if thumb}
                    <img src={thumb} alt={getItemLabel(item, idx)} loading="lazy" />
                  {:else}
                    <div class="omd-story-thumb-fallback">
                      {isVideoThumbItem(item, idx) ? "🎬" : "🖼️"}
                    </div>
                  {/if}
                  <span class="omd-story-thumb-check">{excluded ? "+" : "✓"}</span>
                </button>
              {/each}
            </div>
          </div>

          {#if detectedFactsSummary.length > 0}
            <div class="omd-story-facts-row">
              {#each detectedFactsSummary as chip}
                <span class="omd-story-fact-chip">{chip}</span>
              {/each}
            </div>
          {/if}

          <!-- Destination subfolder name -->
          <div class="omd-story-field">
            <label for="omd-story-subfolder-input">
              {translate("story_subfolder_label", {}, "Story subfolder")}
            </label>
            <div class="omd-story-path-input-wrap">
              <span class="omd-story-parent-prefix" title={parentFolderDisplay}>{parentFolderDisplay}/</span>
              <input
                id="omd-story-subfolder-input"
                type="text"
                bind:value={subfolderName}
                disabled={isWorking}
                placeholder="My_Story"
              />
            </div>
          </div>

          <!-- Plot hint -->
          <div class="omd-story-field">
            <label for="omd-story-plot-input">
              {translate("story_plot_label", {}, "Story plot hint")}
            </label>
            <textarea
              id="omd-story-plot-input"
              rows="3"
              bind:value={plotHint}
              disabled={isWorking}
              placeholder={translate(
                "story_plot_placeholder",
                {},
                "What should the story be about? Tone, mood, funny twist, or key idea..."
              )}
            ></textarea>
          </div>

          {#if isWorking}
            <div class="omd-story-progress-box">
              <div class="omd-story-spinner"></div>
              <div class="omd-story-progress-text">
                {#if progressStage === "copying"}
                  <strong>{translate("story_stage_copying", {}, "Copying media files into subfolder...")}</strong>
                  {#if progressDetail}<span>{progressDetail}</span>{/if}
                {:else if progressStage === "composing"}
                  <strong>{translate("story_stage_composing", {}, "AI node is writing your illustrated comic...")}</strong>
                {:else}
                  <strong>{translate("story_stage_saving", {}, "Saving Readme.md comic...")}</strong>
                {/if}
              </div>
            </div>
          {/if}

          {#if errorMsg}
            <div class="omd-story-error-box">{errorMsg}</div>
          {/if}
        </div>

        <footer class="omd-story-modal-footer">
          <button type="button" class="omd-story-btn-secondary" on:click={closeModal}>
            {translate("cancel", {}, "Cancel")}
          </button>
          <button
            type="button"
            class="omd-story-btn-primary"
            disabled={isWorking || activeItems.length === 0}
            on:click={startComposeStory}
          >
            ✨ {translate("story_create_btn", {}, "Create Story Comic")}
          </button>
        </footer>
      {:else}
        <div class="omd-story-modal-body ready-state">
          <div class="omd-story-ready-banner">
            <span class="ready-icon">🎉</span>
            <div>
              <h3>{result.storyTitle || translate("story_ready_title", {}, "Story comic is ready!")}</h3>
              <p class="ready-path">{result.folderId}/Readme.md</p>
            </div>
          </div>

          {#if result.storyDetail}
            <div class="omd-story-ready-excerpt">
              {result.storyDetail.slice(0, 320)}{result.storyDetail.length > 320 ? "..." : ""}
            </div>
          {/if}

          <div class="omd-story-ready-actions">
            <button type="button" class="omd-story-btn-primary" on:click={handleOpenStory}>
              📖 {translate("story_open_btn", {}, "Open Story")}
            </button>
            <button type="button" class="omd-story-btn-share" on:click={handleShareLink}>
              🔗 {translate("story_share_link_btn", {}, "Share Link")}
            </button>
            {#if isCreations || /\/generated(\/|$)/i.test(result.realFolderPath || "")}
              <button type="button" class="omd-story-btn-civitai" on:click={handlePublishCivitai}>
                🚀 {translate("story_publish_civitai_btn", {}, "Publish to CivitAI")}
              </button>
            {/if}
          </div>
        </div>
      {/if}
    </div>
  </div>
{/if}

<style>
  .omd-story-modal-backdrop {
    position: fixed;
    inset: 0;
    z-index: 10050;
    background: rgba(8, 8, 15, 0.72);
    backdrop-filter: blur(8px);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px;
    box-sizing: border-box;
  }

  .omd-story-modal-card {
    width: 100%;
    max-width: 580px;
    max-height: 90vh;
    display: flex;
    flex-direction: column;
    border-radius: 18px;
    overflow: hidden;
    box-shadow: 0 24px 64px rgba(0, 0, 0, 0.5);
  }

  .dark-theme .omd-story-modal-card {
    background: #171723;
    border: 1px solid rgba(255, 255, 255, 0.11);
    color: #f1f1f8;
  }

  .light-theme .omd-story-modal-card {
    background: #ffffff;
    border: 1px solid rgba(15, 23, 42, 0.12);
    color: #0f172a;
  }

  .omd-story-modal-header {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 12px;
    padding: 18px 22px 14px;
    border-bottom: 1px solid rgba(128, 128, 150, 0.16);
  }

  .omd-story-modal-title-wrap {
    display: flex;
    align-items: flex-start;
    gap: 12px;
  }

  .omd-story-modal-icon {
    font-size: 26px;
    line-height: 1.1;
  }

  .omd-story-modal-title-wrap h2 {
    margin: 0;
    font-size: 18px;
    font-weight: 750;
  }

  .omd-story-modal-title-wrap p {
    margin: 3px 0 0;
    font-size: 12.5px;
    opacity: 0.72;
    line-height: 1.4;
  }

  .omd-story-close-btn {
    border: none;
    background: rgba(128, 128, 150, 0.14);
    color: inherit;
    width: 30px;
    height: 30px;
    border-radius: 50%;
    cursor: pointer;
    font-size: 14px;
    flex-shrink: 0;
  }

  .omd-story-modal-body {
    padding: 18px 22px;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 15px;
  }

  .omd-story-field {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .omd-story-field label,
  .omd-story-label-row {
    font-size: 12.5px;
    font-weight: 650;
    opacity: 0.9;
  }

  .omd-story-label-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }

  .omd-story-album-pill {
    font-size: 11.5px;
    padding: 2px 9px;
    border-radius: 999px;
    background: rgba(139, 92, 246, 0.18);
    color: #c4b5fd;
    max-width: 240px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .light-theme .omd-story-album-pill {
    color: #6d28d9;
  }

  .omd-story-thumbs-strip {
    display: flex;
    gap: 8px;
    overflow-x: auto;
    padding-bottom: 4px;
  }

  .omd-story-thumb-item {
    position: relative;
    width: 62px;
    height: 62px;
    flex-shrink: 0;
    border-radius: 10px;
    overflow: hidden;
    padding: 0;
    border: 2px solid rgba(139, 92, 246, 0.65);
    background: rgba(0, 0, 0, 0.25);
    cursor: pointer;
    transition: opacity 0.15s ease, transform 0.15s ease;
  }

  .omd-story-thumb-item.excluded {
    opacity: 0.36;
    border-color: rgba(128, 128, 150, 0.35);
    transform: scale(0.95);
  }

  .omd-story-thumb-item img {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
  }

  .omd-story-thumb-fallback {
    width: 100%;
    height: 100%;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 20px;
  }

  .omd-story-thumb-check {
    position: absolute;
    top: 3px;
    right: 3px;
    width: 17px;
    height: 17px;
    border-radius: 50%;
    background: rgba(17, 17, 28, 0.82);
    color: #fff;
    font-size: 10px;
    font-weight: 700;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .omd-story-facts-row {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
  }

  .omd-story-fact-chip {
    font-size: 11.5px;
    padding: 3px 9px;
    border-radius: 999px;
    background: rgba(128, 128, 150, 0.14);
    opacity: 0.9;
  }

  .omd-story-path-input-wrap {
    display: flex;
    align-items: center;
    border-radius: 10px;
    border: 1px solid rgba(128, 128, 150, 0.28);
    background: rgba(128, 128, 150, 0.08);
    overflow: hidden;
  }

  .omd-story-parent-prefix {
    padding: 9px 0 9px 12px;
    font-size: 12.5px;
    opacity: 0.65;
    max-width: 210px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    flex-shrink: 0;
  }

  .omd-story-path-input-wrap input {
    flex: 1;
    min-width: 0;
    border: none;
    background: transparent;
    color: inherit;
    padding: 9px 12px 9px 4px;
    font-size: 13.5px;
    font-weight: 600;
    outline: none;
  }

  .omd-story-field textarea {
    width: 100%;
    box-sizing: border-box;
    border-radius: 10px;
    border: 1px solid rgba(128, 128, 150, 0.28);
    background: rgba(128, 128, 150, 0.08);
    color: inherit;
    padding: 10px 12px;
    font-family: inherit;
    font-size: 13.5px;
    line-height: 1.45;
    resize: vertical;
    outline: none;
  }

  .omd-story-field textarea:focus,
  .omd-story-path-input-wrap:focus-within {
    border-color: #8b5cf6;
  }

  .omd-story-progress-box {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 12px 14px;
    border-radius: 12px;
    background: rgba(139, 92, 246, 0.14);
    border: 1px solid rgba(139, 92, 246, 0.35);
    font-size: 13px;
  }

  .omd-story-spinner {
    width: 18px;
    height: 18px;
    border-radius: 50%;
    border: 2.5px solid rgba(139, 92, 246, 0.3);
    border-top-color: #a78bfa;
    animation: omdStorySpin 0.8s linear infinite;
    flex-shrink: 0;
  }

  @keyframes omdStorySpin {
    to {
      transform: rotate(360deg);
    }
  }

  .omd-story-progress-text {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .omd-story-progress-text span {
    font-size: 11.5px;
    opacity: 0.78;
  }

  .omd-story-error-box {
    padding: 10px 12px;
    border-radius: 10px;
    background: rgba(239, 68, 68, 0.14);
    border: 1px solid rgba(239, 68, 68, 0.35);
    color: #fca5a5;
    font-size: 12.5px;
  }

  .light-theme .omd-story-error-box {
    color: #b91c1c;
  }

  .omd-story-modal-footer {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 10px;
    padding: 14px 22px 18px;
    border-top: 1px solid rgba(128, 128, 150, 0.16);
  }

  .omd-story-btn-secondary,
  .omd-story-btn-primary,
  .omd-story-btn-share,
  .omd-story-btn-civitai {
    appearance: none;
    border-radius: 10px;
    padding: 9px 16px;
    font-size: 13.5px;
    font-weight: 650;
    cursor: pointer;
    border: 1px solid transparent;
    transition: transform 0.12s ease, opacity 0.15s ease;
  }

  .omd-story-btn-secondary {
    background: rgba(128, 128, 150, 0.14);
    color: inherit;
  }

  .omd-story-btn-primary {
    background: linear-gradient(135deg, #7c3aed, #6d28d9);
    color: #fff;
  }

  .omd-story-btn-primary:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .omd-story-btn-share {
    background: rgba(16, 185, 129, 0.16);
    border-color: rgba(16, 185, 129, 0.4);
    color: inherit;
  }

  .omd-story-btn-civitai {
    background: linear-gradient(135deg, rgba(37, 99, 235, 0.24), rgba(124, 58, 237, 0.28));
    border-color: rgba(96, 165, 250, 0.5);
    color: inherit;
  }

  .omd-story-ready-banner {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 14px 16px;
    border-radius: 12px;
    background: rgba(16, 185, 129, 0.12);
    border: 1px solid rgba(16, 185, 129, 0.3);
  }

  .ready-icon {
    font-size: 28px;
  }

  .omd-story-ready-banner h3 {
    margin: 0;
    font-size: 16.5px;
    font-weight: 750;
  }

  .ready-path {
    margin: 3px 0 0;
    font-size: 12px;
    opacity: 0.72;
    word-break: break-all;
  }

  .omd-story-ready-excerpt {
    padding: 12px 14px;
    border-radius: 10px;
    background: rgba(128, 128, 150, 0.09);
    font-size: 13px;
    line-height: 1.55;
    opacity: 0.9;
    white-space: pre-line;
  }

  .omd-story-ready-actions {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    margin-top: 4px;
  }
</style>
