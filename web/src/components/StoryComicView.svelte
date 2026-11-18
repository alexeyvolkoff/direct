<script>
  import { createEventDispatcher } from "svelte";
  import { marked } from "marked";
  import { translate } from "../lib/i18n/index.js";
  import { contentModeStore, hasSensitiveReadmeContent, hasSensitiveTag } from "../lib/content-mode.js";

  export let markdown = "";
  export let folderTitle = "";
  export let isDarkTheme = true;
  export let resolveMediaUrl = null; // (relPath) => string
  export let resolvePosterUrl = null; // (relPath) => string
  export let isSensitiveMedia = null; // (relPath) => boolean
  export let mediaVersion = 0; // reactive bump when async blob URLs resolve
  export let canShare = false;
  export let canPublishCivitai = false;
  export let defaultCollapsed = false;
  export let onImageClick = null;
  export let onShare = null;
  export let onPublishCivitai = null;

  const dispatch = createEventDispatcher();

  const VIDEO_RE = /\.(mp4|webm|mov|m4v|mkv|avi)(?:[?#].*)?$/i;
  const IMAGE_RE = /\.(jpe?g|png|webp|gif|avif|bmp|svg)(?:[?#].*)?$/i;

  let collapsed = defaultCollapsed;
  let revealedPanels = new Set();

  $: effectiveMode = $contentModeStore || "work";
  $: storyLevelSensitive = Boolean(
    hasSensitiveTag(markdown) || hasSensitiveReadmeContent(markdown)
  );

  function normalizeRelTarget(rawHref = "") {
    const trimmed = String(rawHref || "").trim().split(/\s+/)[0] || "";
    if (/^(?:https?:|data:|blob:)/i.test(trimmed)) {
      return { isExternal: true, url: trimmed, filename: trimmed };
    }
    const clean = trimmed.replace(/^\.\//, "").replace(/^\/+/, "");
    let decoded = clean;
    try {
      decoded = decodeURIComponent(clean.split("?")[0].split("#")[0]);
    } catch (_) {}
    const filename = decoded.split("/").pop() || decoded;
    return { isExternal: false, relPath: decoded, filename };
  }

  function lookupUrl(target, _ver) {
    if (!target) return "";
    if (target.isExternal) return target.url;
    if (typeof resolveMediaUrl === "function") {
      return resolveMediaUrl(target.relPath || target.filename, target.filename) || "";
    }
    return target.relPath || target.filename || "";
  }

  function lookupPoster(target, _ver) {
    if (!target || target.isExternal) return "";
    if (typeof resolvePosterUrl === "function") {
      return resolvePosterUrl(target.relPath || target.filename, target.filename) || "";
    }
    return "";
  }

  function checkPanelSensitive(target) {
    if (effectiveMode === "fun") return false;
    if (storyLevelSensitive) return true;
    if (target && !target.isExternal && typeof isSensitiveMedia === "function") {
      return Boolean(isSensitiveMedia(target.relPath || target.filename, target.filename));
    }
    return false;
  }

  function revealPanel(idx, e) {
    if (e) e.stopPropagation();
    const next = new Set(revealedPanels);
    next.add(idx);
    revealedPanels = next;
  }

  function isGenericCaption(caption = "", filename = "") {
    const c = String(caption || "").trim();
    if (!c) return true;
    const f = String(filename || "").trim();
    const stem = f.replace(/\.[^/.]+$/, "");
    if (c.toLowerCase() === f.toLowerCase() || c.toLowerCase() === stem.toLowerCase()) {
      return true;
    }
    if (/^(?:image|photo|img|frame|video)[\s_-]*\d*$/i.test(c)) {
      return true;
    }
    return false;
  }

  function sanitizeInlineHtml(html = "", _ver = 0) {
    return String(html || "")
      .replace(/<script[\s\S]*?>[\s\S]*?<\/script>/gi, "")
      .replace(/\son\w+\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>]+)/gi, "")
      .replace(/<img\s+([^>]*?)src=(["'])([^"']+)\2([^>]*)>/gi, (full, pre, q, src, post) => {
        const tgt = normalizeRelTarget(src);
        const resolved = lookupUrl(tgt, _ver);
        if (!resolved) return full;
        return `<img ${pre}src=${q}${resolved}${q} class="omd-comic-inline-img"${post}>`;
      });
  }

  function parseComicDocument(rawMd = "", fallbackTitle = "", _ver = 0) {
    const cleaned = String(rawMd || "")
      .replace(/<!--[\s\S]*?-->/g, "")
      .replace(/(?:^|\n)\s*(?:#(?:sensitive|nsfw)\s*)+$/gi, "")
      .trim();

    if (!cleaned) {
      return { title: fallbackTitle, blocks: [] };
    }

    const lines = cleaned.split(/\r?\n/);
    let extractedTitle = "";
    let startIdx = 0;

    for (let i = 0; i < lines.length; i++) {
      const t = lines[i].trim();
      if (!t) continue;
      const mH1 = t.match(/^#\s+(.+)$/);
      if (mH1) {
        extractedTitle = mH1[1].trim();
        startIdx = i + 1;
      }
      break;
    }

    const blocks = [];
    let proseBuffer = [];

    const flushProse = () => {
      const mdChunk = proseBuffer.join("\n").trim();
      proseBuffer = [];
      if (!mdChunk) return;
      const rawHtml = marked.parse(mdChunk, { gfm: true, breaks: false, async: false });
      blocks.push({
        type: "prose",
        html: sanitizeInlineHtml(rawHtml, _ver),
      });
    };

    const standaloneMediaRe = /^\s*!\[([^\]]*)\]\(([^)]+)\)\s*$/;
    const standaloneVideoLinkRe = /^\s*\[([^\]]*)\]\(([^)]+\.(?:mp4|webm|mov|m4v|mkv))\)\s*$/i;

    for (let i = startIdx; i < lines.length; i++) {
      const line = lines[i];
      const mImg = line.match(standaloneMediaRe);
      const mVidLink = !mImg ? line.match(standaloneVideoLinkRe) : null;

      if (mImg || mVidLink) {
        const caption = (mImg ? mImg[1] : mVidLink[1]).trim();
        const rawHref = (mImg ? mImg[2] : mVidLink[2]).trim();
        const target = normalizeRelTarget(rawHref);
        const isVideo = VIDEO_RE.test(target.filename || rawHref);
        flushProse();
        blocks.push({
          type: isVideo ? "video" : "image",
          caption,
          showCaption: !isGenericCaption(caption, target.filename),
          target,
        });
      } else {
        // Also split paragraphs that embed `![alt](src)` mid-line
        if (/!\[[^\]]*\]\([^)]+\)/.test(line) && !line.trim().startsWith("`")) {
          let cursor = 0;
          const inlineRe = /!\[([^\]]*)\]\(([^)]+)\)/g;
          let m;
          while ((m = inlineRe.exec(line)) !== null) {
            const before = line.slice(cursor, m.index).trim();
            if (before) proseBuffer.push(before);
            flushProse();
            const caption = (m[1] || "").trim();
            const rawHref = (m[2] || "").trim();
            const target = normalizeRelTarget(rawHref);
            const isVideo = VIDEO_RE.test(target.filename || rawHref);
            blocks.push({
              type: isVideo ? "video" : "image",
              caption,
              showCaption: !isGenericCaption(caption, target.filename),
              target,
            });
            cursor = m.index + m[0].length;
          }
          const after = line.slice(cursor).trim();
          if (after) proseBuffer.push(after);
        } else {
          proseBuffer.push(line);
        }
      }
    }
    flushProse();

    return {
      title: extractedTitle || fallbackTitle || translate("story_modal_title", {}, "Story"),
      blocks,
    };
  }

  $: parsedDoc = parseComicDocument(markdown, folderTitle, mediaVersion);

  function handlePanelClick(block, idx) {
    if (checkPanelSensitive(block.target) && !revealedPanels.has(idx)) {
      revealPanel(idx);
      return;
    }
    const payload = {
      filename: block.target?.filename || "",
      relPath: block.target?.relPath || "",
      url: lookupUrl(block.target, mediaVersion),
    };
    if (typeof onImageClick === "function") onImageClick(payload);
    dispatch("image-click", payload);
  }

  function triggerShare() {
    if (typeof onShare === "function") onShare();
    dispatch("share");
  }

  function triggerPublishCivitai() {
    if (typeof onPublishCivitai === "function") onPublishCivitai();
    dispatch("publish-civitai");
  }
</script>

{#if parsedDoc.blocks.length > 0}
  <section class="omd-story-comic {isDarkTheme ? 'dark-theme' : 'light-theme'}" class:is-collapsed={collapsed}>
    <header class="omd-story-comic-bar">
      <div class="omd-story-comic-heading" on:click={() => (collapsed = !collapsed)} role="button" tabindex="0" on:keydown={(e) => e.key === "Enter" && (collapsed = !collapsed)}>
        <span class="omd-story-badge">📖</span>
        <h1 class="omd-story-title">{parsedDoc.title}</h1>
      </div>

      <div class="omd-story-actions">
        {#if canPublishCivitai}
          <button
            type="button"
            class="omd-story-btn civitai"
            on:click|stopPropagation={triggerPublishCivitai}
          >
            🚀 {translate("story_publish_civitai_btn", {}, "Publish to CivitAI")}
          </button>
        {/if}
        {#if canShare}
          <button
            type="button"
            class="omd-story-btn share"
            on:click|stopPropagation={triggerShare}
          >
            🔗 {translate("story_share_link_btn", {}, "Share Link")}
          </button>
        {/if}
        <button
          type="button"
          class="omd-story-btn toggle"
          on:click={() => (collapsed = !collapsed)}
          aria-label={collapsed ? "Expand story" : "Collapse story"}
          title={collapsed ? "Expand story" : "Collapse story"}
        >
          {collapsed ? "▾" : "▴"}
        </button>
      </div>
    </header>

    {#if !collapsed}
      <div class="omd-story-comic-body">
        {#each parsedDoc.blocks as block, idx}
          {#if block.type === "prose"}
            <div class="omd-comic-prose">
              {@html block.html}
            </div>
          {:else if block.type === "video"}
            {@const vidUrl = lookupUrl(block.target, mediaVersion)}
            {@const posterUrl = lookupPoster(block.target, mediaVersion)}
            {@const blurred = checkPanelSensitive(block.target) && !revealedPanels.has(idx)}
            <figure class="omd-comic-panel video-panel">
              <div class="omd-comic-stage" class:is-blurred={blurred}>
                {#if vidUrl}
                  <!-- svelte-ignore a11y-media-has-caption -->
                  <video
                    class="omd-comic-video"
                    src={vidUrl}
                    poster={posterUrl || undefined}
                    controls={!blurred}
                    playsinline
                    preload="metadata"
                  ></video>
                {:else}
                  <div class="omd-comic-placeholder">🎬 {block.target?.filename || "Video"}</div>
                {/if}
                {#if blurred}
                  <button type="button" class="omd-comic-sensitive-shield" on:click={(e) => revealPanel(idx, e)}>
                    <span>🔒 Sensitive frame · Click to reveal</span>
                  </button>
                {/if}
              </div>
              {#if block.showCaption}
                <figcaption class="omd-comic-caption">{block.caption}</figcaption>
              {/if}
            </figure>
          {:else if block.type === "image"}
            {@const imgUrl = lookupUrl(block.target, mediaVersion)}
            {@const blurred = checkPanelSensitive(block.target) && !revealedPanels.has(idx)}
            <figure class="omd-comic-panel image-panel">
              <!-- svelte-ignore a11y-click-events-have-key-events -->
              <!-- svelte-ignore a11y-no-static-element-interactions -->
              <div
                class="omd-comic-stage clickable"
                class:is-blurred={blurred}
                on:click={() => handlePanelClick(block, idx)}
              >
                {#if imgUrl}
                  <div
                    class="omd-comic-backdrop"
                    style="background-image: url('{imgUrl.replace(/'/g, '%27')}');"
                  ></div>
                  <img
                    class="omd-comic-img"
                    src={imgUrl}
                    alt={block.caption || block.target?.filename || ""}
                    loading="lazy"
                    decoding="async"
                  />
                {:else}
                  <div class="omd-comic-placeholder">🖼️ {block.target?.filename || ""}</div>
                {/if}
                {#if blurred}
                  <button type="button" class="omd-comic-sensitive-shield" on:click={(e) => revealPanel(idx, e)}>
                    <span>🔒 Sensitive frame · Click to reveal</span>
                  </button>
                {/if}
              </div>
              {#if block.showCaption}
                <figcaption class="omd-comic-caption">{block.caption}</figcaption>
              {/if}
            </figure>
          {/if}
        {/each}
      </div>
    {/if}
  </section>
{/if}

<style>
  .omd-story-comic {
    width: 100%;
    max-width: 980px;
    margin: 10px auto 22px;
    border-radius: 18px;
    overflow: hidden;
    box-sizing: border-box;
    transition: box-shadow 0.2s ease;
  }

  .omd-story-comic.dark-theme {
    background: linear-gradient(180deg, rgba(24, 24, 34, 0.94) 0%, rgba(17, 17, 25, 0.92) 100%);
    border: 1px solid rgba(255, 255, 255, 0.09);
    color: #ececf4;
    box-shadow: 0 14px 40px rgba(0, 0, 0, 0.34);
  }

  .omd-story-comic.light-theme {
    background: linear-gradient(180deg, #ffffff 0%, #faf9fc 100%);
    border: 1px solid rgba(15, 23, 42, 0.1);
    color: #1e293b;
    box-shadow: 0 10px 30px rgba(15, 23, 42, 0.07);
  }

  .omd-story-comic-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 16px 22px;
    border-bottom: 1px solid rgba(128, 128, 150, 0.14);
    flex-wrap: wrap;
  }

  .omd-story-comic.is-collapsed .omd-story-comic-bar {
    border-bottom: none;
  }

  .omd-story-comic-heading {
    display: flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
    flex: 1;
    cursor: pointer;
  }

  .omd-story-badge {
    font-size: 20px;
    line-height: 1;
    flex-shrink: 0;
  }

  .omd-story-title {
    margin: 0;
    font-size: clamp(18px, 2.3vw, 25px);
    font-weight: 800;
    letter-spacing: -0.02em;
    line-height: 1.25;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .omd-story-actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-shrink: 0;
  }

  .omd-story-btn {
    appearance: none;
    border: 1px solid rgba(139, 92, 246, 0.32);
    background: rgba(139, 92, 246, 0.12);
    color: inherit;
    border-radius: 999px;
    padding: 6px 13px;
    font-size: 12.5px;
    font-weight: 600;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    gap: 5px;
    transition: background 0.15s ease, transform 0.12s ease;
  }

  .omd-story-btn:hover {
    background: rgba(139, 92, 246, 0.24);
    transform: translateY(-1px);
  }

  .omd-story-btn.civitai {
    background: linear-gradient(135deg, rgba(59, 130, 246, 0.22), rgba(139, 92, 246, 0.28));
    border-color: rgba(96, 165, 250, 0.45);
  }

  .omd-story-btn.toggle {
    padding: 6px 10px;
    border-color: rgba(128, 128, 150, 0.25);
    background: rgba(128, 128, 150, 0.1);
  }

  .omd-story-comic-body {
    padding: 20px clamp(14px, 3.2vw, 34px) 28px;
    display: flex;
    flex-direction: column;
    gap: 18px;
  }

  .omd-comic-prose {
    max-width: 780px;
    width: 100%;
    margin: 0 auto;
    font-size: 16px;
    line-height: 1.72;
  }

  .omd-comic-prose :global(p) {
    margin: 0 0 12px;
  }

  .omd-comic-prose :global(p:last-child) {
    margin-bottom: 0;
  }

  .omd-comic-prose :global(h2),
  .omd-comic-prose :global(h3) {
    margin: 14px 0 8px;
    font-weight: 750;
    letter-spacing: -0.01em;
    line-height: 1.3;
  }

  .omd-comic-prose :global(h2) {
    font-size: 20px;
  }

  .omd-comic-prose :global(h3) {
    font-size: 17px;
  }

  .omd-comic-prose :global(blockquote) {
    margin: 6px 0 12px;
    padding: 10px 16px;
    border-left: 3px solid #8b5cf6;
    background: rgba(139, 92, 246, 0.08);
    border-radius: 0 10px 10px 0;
    font-style: italic;
    opacity: 0.94;
  }

  .omd-comic-prose :global(hr) {
    border: none;
    height: 1px;
    background: linear-gradient(90deg, transparent, rgba(139, 92, 246, 0.35), transparent);
    margin: 14px 0;
  }

  .omd-comic-prose :global(.omd-comic-inline-img) {
    max-width: 100%;
    border-radius: 12px;
    display: block;
    margin: 12px auto;
  }

  .omd-comic-panel {
    margin: 4px 0 8px;
    width: 100%;
    display: flex;
    flex-direction: column;
    border-radius: 14px;
    overflow: hidden;
    border: 1px solid rgba(128, 128, 150, 0.18);
    background: rgba(10, 10, 16, 0.65);
  }

  .light-theme .omd-comic-panel {
    background: #0f172a;
    border-color: rgba(15, 23, 42, 0.14);
  }

  .omd-comic-stage {
    position: relative;
    width: 100%;
    min-height: 220px;
    max-height: min(74vh, 720px);
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: hidden;
    background: #0b0b12;
  }

  .omd-comic-stage.clickable {
    cursor: zoom-in;
  }

  .omd-comic-backdrop {
    position: absolute;
    inset: -24px;
    background-size: cover;
    background-position: center;
    filter: blur(28px) brightness(0.48) saturate(1.15);
    transform: scale(1.08);
    pointer-events: none;
    z-index: 0;
  }

  .omd-comic-img {
    position: relative;
    z-index: 1;
    display: block;
    width: 100%;
    max-height: min(74vh, 720px);
    object-fit: contain;
    transition: transform 0.22s ease, filter 0.2s ease;
  }

  .omd-comic-stage.clickable:hover .omd-comic-img {
    transform: scale(1.008);
  }

  .omd-comic-video {
    position: relative;
    z-index: 1;
    display: block;
    width: 100%;
    max-height: min(74vh, 720px);
    background: #000;
  }

  .omd-comic-stage.is-blurred .omd-comic-img,
  .omd-comic-stage.is-blurred .omd-comic-video {
    filter: blur(30px) brightness(0.6);
  }

  .omd-comic-sensitive-shield {
    position: absolute;
    inset: 0;
    z-index: 3;
    border: none;
    background: rgba(10, 10, 18, 0.45);
    color: #fff;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .omd-comic-sensitive-shield span {
    padding: 8px 16px;
    border-radius: 999px;
    background: rgba(15, 15, 24, 0.84);
    border: 1px solid rgba(255, 255, 255, 0.2);
  }

  .omd-comic-placeholder {
    padding: 48px 20px;
    color: rgba(255, 255, 255, 0.65);
    font-size: 13.5px;
    text-align: center;
  }

  .omd-comic-caption {
    padding: 10px 16px;
    font-size: 13.5px;
    line-height: 1.45;
    text-align: center;
    background: rgba(15, 15, 24, 0.9);
    color: rgba(236, 236, 246, 0.88);
    border-top: 1px solid rgba(255, 255, 255, 0.07);
  }

  @media (max-width: 640px) {
    .omd-story-comic {
      margin: 6px 0 16px;
      border-radius: 14px;
    }
    .omd-story-comic-bar {
      padding: 12px 14px;
    }
    .omd-story-comic-body {
      padding: 14px 12px 20px;
      gap: 14px;
    }
    .omd-comic-prose {
      font-size: 15px;
    }
    .omd-comic-stage {
      min-height: 180px;
      max-height: 68vh;
    }
    .omd-comic-img,
    .omd-comic-video {
      max-height: 68vh;
    }
  }
</style>
