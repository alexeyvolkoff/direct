<script>
  import { onMount, onDestroy, createEventDispatcher } from "svelte";
  import { _ } from "../lib/i18n";
  import videojs from "video.js";
  import "video.js/dist/video-js.css";

  export let url = "";
  export let filename = "Video";
  export let poster = "";
  export let title = "";
  export let description = "";
  export let playlist = [];
  export let currentIndex = 0;
  export let showClose = true;
  export let autoplay = true;
  export let showInfo = true;

  const dispatch = createEventDispatcher();
  let videoElement;
  let player;
  let headerVisible = true;
  let headerTimeout;
  let isFullscreen = false;
  let headerElement;
  let containerNode;

  let activeIndex = currentIndex;
  let isMobile = typeof window !== "undefined" ? window.innerWidth <= 768 : false;
  let showPlaylist = !isMobile && (playlist && playlist.length > 1);


  // Sync activeIndex with currentIndex prop
  $: if (typeof currentIndex === "number" && currentIndex !== activeIndex && playlist && playlist[currentIndex]) {
    activeIndex = currentIndex;
  }

  $: if (!playlist || playlist.length <= 1) {
    showPlaylist = false;
  }

  // Active video item
  $: currentVideo = (playlist && playlist[activeIndex]) ? playlist[activeIndex] : {
    url: url || "",
    poster: poster || "",
    name: filename || "Video",
    title: title || filename || "Video",
    description: description || ""
  };

  $: activeUrl = currentVideo.url || url || "";
  $: activeTitle = currentVideo.title || currentVideo.name || filename || "Video";
  $: activeDesc = currentVideo.description || description || "";
  $: activePoster = currentVideo.poster || poster || TRANSPARENT_POSTER;

  const TRANSPARENT_POSTER = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

  let lastLoadedUrl = "";
  $: if (player && activeUrl && activeUrl !== lastLoadedUrl) {
    lastLoadedUrl = activeUrl;
    player.src({ src: activeUrl });
    if (activePoster && activePoster !== TRANSPARENT_POSTER) {
      player.poster(activePoster);
    }
    if (autoplay) {
      player.play().catch(() => {});
    }
  }

  // Auto-hide top header bar after 3.5s of inactivity
  function resetHeaderTimeout() {
    headerVisible = true;
    clearTimeout(headerTimeout);
    headerTimeout = setTimeout(() => {
      // Keep controls visible if video is paused
      if (player && player.paused && player.paused()) {
        headerVisible = true;
        return;
      }
      headerVisible = false;
    }, 3500);
  }

  function isFsActive() {
    return !!(
      (player && typeof player.isFullscreen === "function" && player.isFullscreen()) ||
      document.fullscreenElement ||
      document.webkitFullscreenElement ||
      document.mozFullScreenElement ||
      document.msFullscreenElement
    );
  }

  function getVideoElement() {
    // The header element is re-parented into the player element by JS
    if (player && typeof player.el === "function") {
      const vid = player.el()?.querySelector("video");
      if (vid) return vid;
    }
    return containerNode?.querySelector("video") || null;
  }

  function toggleFullscreen() {
    try {
      if (isFsActive()) {
        // Exit fullscreen
        if (player && typeof player.exitFullscreen === "function") {
          try { player.exitFullscreen(); } catch (_) {}
        }
        const exit = document.exitFullscreen || document.webkitExitFullscreen || document.mozCancelFullScreen || document.msExitFullscreen;
        if (exit) {
          try { exit.call(document); } catch (_) {}
        }
        const vid = getVideoElement();
        if (vid && vid.webkitExitFullscreen) {
          try { vid.webkitExitFullscreen(); } catch (_) {}
        }
        isFullscreen = false;
      } else {
        // iOS: native AVKit player
        const vid = getVideoElement();
        const isIOS = typeof navigator !== "undefined" && /iphone|ipad|ipod/i.test(navigator.userAgent);
        if (isIOS && vid && vid.webkitEnterFullscreen) {
          try { vid.webkitEnterFullscreen(); isFullscreen = true; return; } catch (_) {}
        }

        // Standard Video.js requestFullscreen expands player.el() (including our headerElement)
        // to full screen across all desktop browsers and Android WebView.
        if (player && typeof player.requestFullscreen === "function") {
          player.requestFullscreen().catch((err) => {
            console.warn("[VideoViewer] player.requestFullscreen rejected:", err);
            const target = (player && typeof player.el === "function" ? player.el() : null) || containerNode || document.documentElement;
            const req = target.requestFullscreen || target.webkitRequestFullscreen || target.mozRequestFullScreen || target.msRequestFullscreen;
            if (req) {
              try { req.call(target); } catch (_) {}
            }
          });
        } else {
          const target = (player && typeof player.el === "function" ? player.el() : null) || containerNode || document.documentElement;
          const req = target.requestFullscreen || target.webkitRequestFullscreen || target.mozRequestFullScreen || target.msRequestFullscreen;
          if (req) {
            try { req.call(target); } catch (_) {}
          }
        }
      }
    } catch (err) {
      console.error("[VideoViewer] Fullscreen error:", err);
    }
  }

  function selectVideo(index) {
    if (!playlist || playlist.length === 0) return;
    if (index < 0 || index >= playlist.length) return;
    activeIndex = index;
    if (isMobile) showPlaylist = false;

    const v = playlist[index];
    if (!v) return;

    if (player) {
      player.poster(v.poster || TRANSPARENT_POSTER);
      player.src({ src: v.url });
      player.play().catch(() => {});
    }
    resetHeaderTimeout();
    dispatch("change", { index, video: v });
  }

  const prefetchedUrls = new Set();
  function prefetchNextTrack() {
    if (!playlist || playlist.length < 2) return;
    const nextIdx = (activeIndex + 1) % playlist.length;
    const v = playlist[nextIdx];
    if (!v || !v.url || prefetchedUrls.has(v.url)) return;
    prefetchedUrls.add(v.url);
    // Warm up the tunnel + device open for the next item with a small ranged request
    fetch(v.url, { headers: { "Range": "bytes=0-65535" } }).then(r => r.arrayBuffer()).catch(() => {
      prefetchedUrls.delete(v.url);
    });
  }

  $: if (playlist && playlist.length > 1 && activeIndex !== undefined && activeUrl) {
    prefetchNextTrack();
  }



  // TikTok-like Touch Swipe Gestures
  let touchStartY = 0;
  let touchStartX = 0;
  let touchStartTime = 0;
  let isSwiping = false;

  function handleTouchStart(e) {
    resetHeaderTimeout();
    if (e.touches && e.touches.length === 1) {
      touchStartY = e.touches[0].clientY;
      touchStartX = e.touches[0].clientX;
      touchStartTime = Date.now();
      isSwiping = true;
    }
  }

  function handleTouchEnd(e) {
    if (!isSwiping) return;
    isSwiping = false;
    if (!e.changedTouches || e.changedTouches.length === 0) return;

    const deltaY = e.changedTouches[0].clientY - touchStartY;
    const deltaX = e.changedTouches[0].clientX - touchStartX;
    const duration = Date.now() - touchStartTime;

    if (Math.abs(deltaY) > 50 && Math.abs(deltaY) > Math.abs(deltaX) * 1.3 && duration < 700) {
      if (deltaY < 0) {
        // Swiped UP -> Next video
        if (playlist && activeIndex < playlist.length - 1) {
          selectVideo(activeIndex + 1);
        }
      } else {
        // Swiped DOWN -> Previous video
        if (playlist && activeIndex > 0) {
          selectVideo(activeIndex - 1);
        }
      }
    }
  }

  // Mouse wheel scroll to next / prev video
  let wheelLock = false;
  function handleWheel(e) {
    if (wheelLock || !playlist || playlist.length <= 1) return;
    if (Math.abs(e.deltaY) > 75 && Math.abs(e.deltaY) > Math.abs(e.deltaX) * 2) {
      if (e.deltaY > 0 && activeIndex < playlist.length - 1) {
        wheelLock = true;
        selectVideo(activeIndex + 1);
        setTimeout(() => { wheelLock = false; }, 600);
      } else if (e.deltaY < 0 && activeIndex > 0) {
        wheelLock = true;
        selectVideo(activeIndex - 1);
        setTimeout(() => { wheelLock = false; }, 600);
      }
    }
  }

  function handleKeydown(e) {
    if (e.key === "ArrowDown" || e.key === "PageDown") {
      if (playlist && activeIndex < playlist.length - 1) {
        e.preventDefault();
        selectVideo(activeIndex + 1);
      }
    } else if (e.key === "ArrowUp" || e.key === "PageUp") {
      if (playlist && activeIndex > 0) {
        e.preventDefault();
        selectVideo(activeIndex - 1);
      }
    } else if (e.key === "Escape") {
      if (isMobile && showPlaylist) {
        showPlaylist = false;
      } else if (showClose) {
        dispatch("close", { index: activeIndex, video: currentVideo });
      }
    }
  }

  function formatBytes(bytes) {
    if (!bytes || bytes === 0) return "";
    const k = 1024;
    const sizes = ["B", "KB", "MB", "GB", "TB"];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + " " + sizes[i];
  }

  function updatePlaylistCounter() {
    if (!player) return;
    const counterComp = player.controlBar && typeof player.controlBar.getChild === "function" ? player.controlBar.getChild("playlistCounter") : null;
    if (counterComp && counterComp.el()) {
      if (playlist && Array.isArray(playlist) && playlist.length > 1) {
        counterComp.el().textContent = `${activeIndex + 1} / ${playlist.length}`;
        counterComp.el().style.display = "inline-flex";
      } else {
        counterComp.el().textContent = "";
        counterComp.el().style.display = "none";
      }
    }
  }

  $: if (player && (activeIndex !== undefined || playlist)) {
    updatePlaylistCounter();
  }

  onMount(() => {
    const handleResize = () => {
      isMobile = window.innerWidth <= 768;
    };
    window.addEventListener("resize", handleResize);

    const hasMultipleVideos = !!(playlist && Array.isArray(playlist) && playlist.length > 1);

    if (hasMultipleVideos) {
      const vjsComponent = videojs.getComponent("Component");
      if (vjsComponent && !videojs.getComponent("PlaylistCounter")) {
        class PlaylistCounter extends vjsComponent {
          constructor(player, options) {
            super(player, options);
            this.addClass("vjs-playlist-counter");
          }
          createEl() {
            return videojs.dom.createEl("div", {
              className: "vjs-playlist-counter vjs-control"
            });
          }
        }
        videojs.registerComponent("PlaylistCounter", PlaylistCounter);
      }
    }

    const posterUrl = (activePoster && typeof activePoster === "string" && activePoster.trim()) ? activePoster.trim() : TRANSPARENT_POSTER;
    const playerOptions = {
      controls: true,
      autoplay: autoplay,
      preload: autoplay ? "auto" : "metadata",
      fill: true,
      poster: posterUrl,
      playbackRates: [0.5, 1, 1.25, 1.5, 2],
      controlBar: {
        children: [
          "playToggle",
          "volumePanel",
          "currentTimeDisplay",
          "timeDivider",
          "durationDisplay",
          "progressControl",
          ...(hasMultipleVideos ? ["playlistCounter"] : []),
          "playbackRateMenuButton",
          "fullscreenToggle"
        ],
        volumePanel: {
          inline: true
        }
      }
    };
    player = videojs(videoElement, playerOptions);
    if (activeUrl) {
      lastLoadedUrl = activeUrl;
      player.src({ src: activeUrl });
    }

    try {
      const savedVolStr = typeof localStorage !== "undefined" ? localStorage.getItem("omd_media_volume") : null;
      const savedMutedStr = typeof localStorage !== "undefined" ? localStorage.getItem("omd_media_muted") : null;
      if (savedVolStr !== null) {
        const v = parseFloat(savedVolStr);
        if (!isNaN(v)) player.volume(Math.max(0, Math.min(1, v)));
      }
      if (savedMutedStr !== null) {
        player.muted(savedMutedStr === "true");
      }
    } catch (_) {}

    player.on("volumechange", () => {
      try {
        if (typeof localStorage !== "undefined") {
          localStorage.setItem("omd_media_volume", String(player.volume()));
          localStorage.setItem("omd_media_muted", String(player.muted()));
        }
      } catch (_) {}
    });

    if (headerElement && player && player.el()) {
      player.el().appendChild(headerElement);
    }
    if (player) {
      player.on("useractive", resetHeaderTimeout);
      player.on("mousemove", resetHeaderTimeout);
    }

    if (hasMultipleVideos) {
      updatePlaylistCounter();
    }
    resetHeaderTimeout();
    const handleFsChange = () => {
      isFullscreen = isFsActive();
    };
    if (player) {
      player.on("fullscreenchange", handleFsChange);
    }
    document.addEventListener("fullscreenchange", handleFsChange);
    document.addEventListener("webkitfullscreenchange", handleFsChange);

    return () => {
      window.removeEventListener("resize", handleResize);
      document.removeEventListener("fullscreenchange", handleFsChange);
      document.removeEventListener("webkitfullscreenchange", handleFsChange);
    };
  });

  onDestroy(() => {
    clearTimeout(headerTimeout);
    if (headerElement && headerElement.parentNode) {
      headerElement.parentNode.removeChild(headerElement);
    }
    if (player) {
      player.dispose();
    }
  });
</script>

<svelte:window on:keydown={handleKeydown} />

<div
  bind:this={containerNode}
  class="video-viewer-layout"
  on:mousemove={resetHeaderTimeout}
  on:click={resetHeaderTimeout}
  role="presentation"
>
  <!-- Main Player Section (Center/Left) -->
  <div class="player-stage">
    <!-- Top Floating Header -->
    <div bind:this={headerElement} class="player-header" class:visible={headerVisible}>
      <div class="header-actions">
        {#if playlist && playlist.length > 1 && !showPlaylist}
          <button
            type="button"
            class="header-btn playlist-toggle-btn"
            on:click|stopPropagation={() => showPlaylist = true}
            aria-label={$_("open_playlist", {}, "Open Playlist")}
            title={$_("open_playlist", {}, "Open Playlist")}
          >
            <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <line x1="8.5" y1="6" x2="20" y2="6"></line>
              <line x1="8.5" y1="12" x2="20" y2="12"></line>
              <line x1="8.5" y1="18" x2="20" y2="18"></line>
              <circle cx="4" cy="6" r="1.5" fill="currentColor"></circle>
              <circle cx="4" cy="12" r="1.5" fill="currentColor"></circle>
              <circle cx="4" cy="18" r="1.5" fill="currentColor"></circle>
            </svg>
          </button>
        {/if}

        <button type="button" class="header-btn fullscreen-btn" on:click|stopPropagation={toggleFullscreen} aria-label={$_("fullscreen", {}, "Fullscreen")} title={$_("fullscreen", {}, "Fullscreen")}>
          {#if isFullscreen}
            <svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor"><path d="M5 16h3v3h2v-5H5v2zm3-8H5v2h5V5H8v3zm6 11h2v-3h3v-2h-5v5zm2-11V5h-2v5h5V8h-3z"/></svg>
          {:else}
            <svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor"><path d="M7 14H5v5h5v-2H7v-3zm-2-4h2V7h3V5H5v5zm12 7h-3v2h5v-5h-2v3zM14 5v2h3v3h2V5h-5z"/></svg>
          {/if}
        </button>

        {#if showClose}
          <button type="button" class="header-btn close-btn" on:click|stopPropagation={() => dispatch("close", { index: activeIndex, video: currentVideo })} aria-label={$_("close", {}, "Close")} title={$_("close", {}, "Close")}>
            <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
              <line x1="18" y1="6" x2="6" y2="18"></line>
              <line x1="6" y1="6" x2="18" y2="18"></line>
            </svg>
          </button>
        {/if}
      </div>
    </div>

    <!-- Video Area with Touch Gestures -->
    <div
      class="video-display-area"
      on:touchstart={handleTouchStart}
      on:touchend={handleTouchEnd}
      on:wheel={handleWheel}
      role="region"
      aria-label="Video Player Stage"
    >


      <!-- Video.js Player Wrapper -->
      <div data-vjs-player class="player-wrapper">
        <!-- svelte-ignore a11y-media-has-caption -->
        <video bind:this={videoElement} class="video-js vjs-big-play-centered" playsinline poster={activePoster}></video>
      </div>
    </div>

    <!-- Title & Description Section Underneath Player (YouTube Style) -->
    {#if showInfo}
      <div class="video-info-section" class:visible={headerVisible}>
        <div class="video-info-main">
          <div class="video-info-header">
            <h2 class="video-info-title" title={activeTitle}>{activeTitle}</h2>
            {#if currentVideo.size}
              <span class="meta-tag">{formatBytes(currentVideo.size)}</span>
            {/if}
          </div>
          {#if activeDesc}
            <p class="video-info-desc">{activeDesc}</p>
          {/if}
        </div>
      </div>
    {/if}
  </div>

  <!-- Mobile Backdrop -->
  {#if isMobile && playlist && playlist.length > 1 && showPlaylist}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="mobile-backdrop" on:click={() => showPlaylist = false}></div>
  {/if}

  <!-- Right Playlist Sidebar (YouTube / TikTok Drawer) -->
  {#if playlist && playlist.length > 1}
    <aside class="playlist-sidebar" class:open={showPlaylist}>
      <div class="sidebar-header">
        <div class="sidebar-title-group">
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="#A28AD0" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
            <line x1="8" y1="6" x2="21" y2="6"></line>
            <line x1="8" y1="12" x2="21" y2="12"></line>
            <line x1="8" y1="18" x2="21" y2="18"></line>
            <circle cx="4" cy="6" r="1.5" fill="#A28AD0"></circle>
            <circle cx="4" cy="12" r="1.5" fill="#A28AD0"></circle>
            <circle cx="4" cy="18" r="1.5" fill="#A28AD0"></circle>
          </svg>
          <span class="sidebar-heading">{$_("playlist", {}, "Playlist")} ({playlist.length})</span>
        </div>
        <button type="button" class="sidebar-close-btn" on:click={() => showPlaylist = false} aria-label={$_("close_playlist", {}, "Close Playlist")} title={$_("close_playlist", {}, "Close Playlist")}>
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>
        </button>
      </div>

      <div class="playlist-scroll">
        {#each playlist as item, idx}
          <button
            type="button"
            class="playlist-card {idx === activeIndex ? 'active' : ''}"
            on:click={() => selectVideo(idx)}
          >
            <!-- Card Thumbnail with Cover Poster -->
            <div class="card-thumb">
              {#if item.poster}
                <img
                  src={item.poster}
                  alt={item.title || item.name}
                  class="thumb-img"
                  loading="lazy"
                  decoding="async"
                  on:error={(e) => { e.target.style.display = 'none'; }}
                />
              {/if}
              <div class="thumb-fallback">
                <svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>
              </div>

              {#if idx === activeIndex}
                <div class="playing-indicator">
                  <span class="eq-bar"></span>
                  <span class="eq-bar"></span>
                  <span class="eq-bar"></span>
                </div>
              {/if}
            </div>

            <!-- Card Information (Title, Description, Size) -->
            <div class="card-info">
              <div class="card-title" title={item.title || item.name}>
                {item.title || item.name}
              </div>
              {#if item.description}
                <div class="card-desc" title={item.description}>
                  {item.description}
                </div>
              {/if}
              <div class="card-meta">
                {#if item.size}
                  <span class="card-size">{formatBytes(item.size)}</span>
                {/if}
              </div>
            </div>
          </button>
        {/each}
      </div>
    </aside>
  {/if}
</div>

<style>
  .video-viewer-layout {
    width: 100%;
    height: 100%;
    min-height: 380px;
    display: flex;
    flex-direction: row;
    background: #000000;
    position: relative;
    overflow: hidden;
    color: #ffffff;
    font-family: inherit;
  }

  :global(.video-viewer-layout:fullscreen),
  :global(.video-viewer-layout:-webkit-full-screen),
  :global(.video-viewer-layout:-moz-full-screen),
  .video-viewer-layout:fullscreen {
    width: 100vw !important;
    height: 100vh !important;
    background: #000000 !important;
  }

  .player-stage {
    flex: 1 1 0%;
    min-width: 0;
    height: 100%;
    min-height: 380px;
    display: flex;
    flex-direction: column;
    background: #000000;
    position: relative;
    overflow: hidden;
  }

  .video-display-area {
    flex: 1 1 0%;
    min-height: 380px;
    height: 100%;
    width: 100%;
    display: flex;
    justify-content: center;
    align-items: center;
    background: #000000;
    position: relative;
    overflow: hidden;
  }

  .player-wrapper {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    width: 100%;
    height: 100%;
    display: flex;
  }

  /* Header Bar */
  .player-header {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    padding: 16px 20px;
    background: linear-gradient(to bottom, rgba(0, 0, 0, 0.75) 0%, rgba(0, 0, 0, 0) 100%);
    color: white;
    display: flex;
    justify-content: flex-end;
    align-items: center;
    gap: 16px;
    z-index: 25;
    opacity: 0;
    transition: opacity 0.3s ease;
    pointer-events: none;
  }

  .player-header.visible {
    opacity: 1;
    pointer-events: auto;
  }

  .header-actions {
    display: flex;
    gap: 10px;
    align-items: center;
    flex-shrink: 0;
    margin-left: auto;
  }

  .header-btn {
    background: rgba(255, 255, 255, 0.16);
    border: 1px solid rgba(255, 255, 255, 0.15);
    color: white;
    cursor: pointer;
    border-radius: 50%;
    width: 38px;
    height: 38px;
    min-width: 38px;
    min-height: 38px;
    padding: 0;
    margin: 0;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    transition: all 0.2s ease;
    backdrop-filter: blur(8px);
    box-sizing: border-box;
  }

  .header-btn svg {
    display: block;
    margin: auto;
    pointer-events: none;
    flex-shrink: 0;
  }

  .header-btn:hover,
  .header-btn.active {
    background: rgba(142, 116, 189, 0.7);
    border-color: #8E74BD;
    transform: scale(1.05);
  }

  /* Video Info Section Underneath Player (YouTube Style) */
  .video-info-section {
    position: absolute;
    bottom: 48px;
    left: 0;
    right: 0;
    padding: 16px 20px 8px 20px;
    background: linear-gradient(to top, rgba(0, 0, 0, 0.85) 0%, rgba(0, 0, 0, 0) 100%);
    display: flex;
    align-items: flex-end;
    justify-content: space-between;
    gap: 16px;
    z-index: 20;
    opacity: 0;
    transform: translateY(8px);
    transition: opacity 0.3s ease, transform 0.3s ease;
    pointer-events: none;
    max-height: 140px;
    overflow-y: auto;
    box-sizing: border-box;
  }

  .video-info-section.visible {
    opacity: 1;
    transform: translateY(0);
    pointer-events: auto;
  }

  .video-info-main {
    flex: 1;
    min-width: 0;
  }

  .video-info-header {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-bottom: 4px;
    flex-wrap: wrap;
  }

  .video-info-title {
    font-size: 15px;
    font-weight: 700;
    color: #F8F7FC;
    margin: 0;
    line-height: 1.4;
    word-break: break-word;
  }

  .video-info-badge {
    background: rgba(255, 255, 255, 0.18);
    border: 1px solid rgba(255, 255, 255, 0.25);
    padding: 2px 8px;
    border-radius: 12px;
    font-size: 11.5px;
    font-weight: 600;
    letter-spacing: 0.5px;
    white-space: nowrap;
    backdrop-filter: blur(8px);
    color: #ffffff;
    flex-shrink: 0;
  }

  .video-info-details {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
    margin-top: 4px;
  }

  .video-info-desc {
    font-size: 12.5px;
    color: #8B80A3;
    margin: 0;
    line-height: 1.45;
    word-break: break-word;
    white-space: pre-line;
  }

  .meta-tag {
    display: inline-block;
    background: rgba(255, 255, 255, 0.08);
    border: 1px solid rgba(255, 255, 255, 0.12);
    color: #C4B8DB;
    padding: 2px 7px;
    border-radius: 6px;
    font-size: 11.5px;
    font-weight: 500;
    flex-shrink: 0;
  }



  /* Playlist Sidebar (YouTube / Desktop layout) */
  .playlist-sidebar {
    width: 330px;
    max-width: 380px;
    height: 100%;
    background: #08060C;
    border-left: 1px solid rgba(255, 255, 255, 0.08);
    display: flex;
    flex-direction: column;
    flex-shrink: 0;
    z-index: 35;
    transition: width 0.28s cubic-bezier(0.16, 1, 0.3, 1), opacity 0.28s ease;
    overflow: hidden;
  }

  .playlist-sidebar:not(.open) {
    width: 0 !important;
    max-width: 0 !important;
    border-left: none !important;
    opacity: 0;
    pointer-events: none;
  }

  .sidebar-header {
    height: 52px;
    padding: 0 16px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    border-bottom: 1px solid rgba(255, 255, 255, 0.07);
    background: #0d1522;
    flex-shrink: 0;
  }

  .sidebar-title-group {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .sidebar-heading {
    font-size: 13.5px;
    font-weight: 600;
    color: #F4F1FA;
  }

  .sidebar-close-btn {
    background: transparent;
    border: none;
    color: #8B80A3;
    cursor: pointer;
    padding: 5px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.2s ease;
  }

  .sidebar-close-btn:hover {
    color: #ffffff;
    background: rgba(255, 255, 255, 0.1);
  }

  .playlist-scroll {
    flex: 1;
    overflow-y: auto;
    padding: 10px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  /* Playlist Card */
  .playlist-card {
    display: flex;
    gap: 12px;
    padding: 8px;
    border-radius: 10px;
    background: rgba(255, 255, 255, 0.03);
    border: 1px solid rgba(255, 255, 255, 0.05);
    color: inherit;
    text-align: left;
    cursor: pointer;
    transition: all 0.2s ease;
    width: 100%;
    box-sizing: border-box;
    align-items: flex-start;
  }

  .playlist-card:hover {
    background: rgba(255, 255, 255, 0.08);
    border-color: rgba(255, 255, 255, 0.15);
    transform: translateY(-1px);
  }

  .playlist-card.active {
    background: rgba(142, 116, 189, 0.22);
    border-color: rgba(55, 169, 239, 0.8);
    box-shadow: 0 4px 14px rgba(142, 116, 189, 0.25);
  }

  .card-thumb {
    width: 100px;
    height: 60px;
    border-radius: 6px;
    background: #252330;
    position: relative;
    overflow: hidden;
    flex-shrink: 0;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .thumb-img {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
    position: absolute;
    top: 0;
    left: 0;
  }

  .thumb-fallback {
    color: #73688A;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  /* Playing Equalizer Animated Badge */
  .playing-indicator {
    position: absolute;
    bottom: 4px;
    right: 4px;
    background: rgba(0, 0, 0, 0.75);
    padding: 3px 5px;
    border-radius: 4px;
    display: flex;
    align-items: flex-end;
    gap: 2px;
    height: 14px;
    box-sizing: border-box;
  }

  .eq-bar {
    width: 2.5px;
    background: #A28AD0;
    border-radius: 1px;
    animation: eqBounce 0.8s ease-in-out infinite alternate;
  }
  .eq-bar:nth-child(1) { height: 60%; animation-delay: 0.1s; }
  .eq-bar:nth-child(2) { height: 100%; animation-delay: 0.3s; }
  .eq-bar:nth-child(3) { height: 40%; animation-delay: 0.2s; }

  @keyframes eqBounce {
    0% { height: 20%; }
    100% { height: 100%; }
  }

  .card-info {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 3px;
  }

  .card-title {
    font-size: 13px;
    font-weight: 600;
    color: #F4F1FA;
    line-height: 1.35;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }

  .playlist-card.active .card-title {
    color: #A28AD0;
  }

  .card-desc {
    font-size: 11.5px;
    color: #8B80A3;
    line-height: 1.3;
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }

  .card-meta {
    font-size: 11px;
    color: #73688A;
    margin-top: 2px;
  }

  .mobile-backdrop {
    display: none;
  }

  /* Responsive / Mobile Drawer Styles */
  @media (max-width: 768px) {
    :global(.video-js .vjs-fullscreen-control) {
      display: none !important;
    }

    .playlist-sidebar {
      position: absolute;
      top: 0;
      right: 0;
      bottom: 0;
      width: 84%;
      max-width: 340px;
      transform: translateX(100%);
      transition: transform 0.3s cubic-bezier(0.16, 1, 0.3, 1);
      box-shadow: -8px 0 32px rgba(0, 0, 0, 0.85);
      border-left: 1px solid rgba(255, 255, 255, 0.12);
      background: #08060C;
      opacity: 1 !important;
    }

    .playlist-sidebar.open {
      transform: translateX(0);
      width: 84% !important;
      max-width: 340px !important;
      pointer-events: auto !important;
    }

    .playlist-sidebar:not(.open) {
      transform: translateX(100%);
      width: 84% !important;
      max-width: 340px !important;
      border-left: 1px solid rgba(255, 255, 255, 0.12) !important;
    }

    .sidebar-close-btn {
      display: flex;
    }

    .mobile-backdrop {
      display: block;
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background: rgba(0, 0, 0, 0.65);
      z-index: 32;
      backdrop-filter: blur(4px);
    }
  }

  /* VideoJS Global Styles */
  :global(.video-js) {
    width: 100% !important;
    height: 100% !important;
    position: absolute !important;
    top: 0 !important;
    left: 0 !important;
    right: 0 !important;
    bottom: 0 !important;
    font-family: inherit !important;
    background-color: #000000 !important;
  }

  :global(.video-js .vjs-tech) {
    width: 100% !important;
    height: 100% !important;
    position: absolute !important;
    top: 0 !important;
    left: 0 !important;
    right: 0 !important;
    bottom: 0 !important;
    object-fit: contain !important;
  }

  :global(.video-js .vjs-poster) {
    background-size: contain !important;
    background-position: center !important;
    background-repeat: no-repeat !important;
  }

  :global(.video-js .vjs-poster img) {
    object-fit: contain !important;
  }

  :global(video::-webkit-media-controls-overlay-play-button),
  :global(video::-webkit-media-controls-start-playback-button),
  :global(video::-webkit-media-controls),
  :global(video::-webkit-media-controls-enclosure) {
    display: none !important;
    -webkit-appearance: none !important;
    opacity: 0 !important;
  }

  :global(.video-js .vjs-poster:empty),
  :global(.video-js .vjs-poster[style*="none"]) {
    display: none !important;
  }

  :global(.video-js .vjs-control-bar) {
    position: absolute !important;
    bottom: 0 !important;
    left: 0 !important;
    right: 0 !important;
    width: 100% !important;
    height: 54px !important;
    background: linear-gradient(to top, rgba(13, 11, 18, 0.95) 0%, rgba(13, 11, 18, 0.7) 70%, rgba(0, 0, 0, 0) 100%) !important;
    display: flex !important;
    align-items: center !important;
    justify-content: flex-start !important;
    padding: 0 12px !important;
    box-sizing: border-box !important;
    z-index: 20 !important;
  }

  :global(.video-js .vjs-hidden),
  :global(.video-js .vjs-skip-backward-undefined),
  :global(.video-js .vjs-skip-forward-undefined),
  :global(.video-js .vjs-audio-button),
  :global(.video-js .vjs-subs-caps-button),
  :global(.video-js .vjs-chapters-button),
  :global(.video-js .vjs-descriptions-button) {
    display: none !important;
  }

  :global(.video-js .vjs-control-bar .vjs-button) {
    width: 48px !important;
    height: 48px !important;
    line-height: 48px !important;
    font-size: 22px !important;
    display: inline-flex !important;
    align-items: center !important;
    justify-content: center !important;
    margin: 0 2px !important;
    border-radius: 8px !important;
    transition: background 0.2s ease, color 0.2s ease !important;
    background: transparent !important;
    cursor: pointer !important;
  }

  :global(.video-js .vjs-control-bar .vjs-button:hover) {
    background: rgba(255, 255, 255, 0.18) !important;
    color: #8E74BD !important;
  }

  :global(.video-js .vjs-control-bar .vjs-button .vjs-icon-placeholder:before) {
    line-height: 48px !important;
    font-size: 22px !important;
    height: 48px !important;
    width: 48px !important;
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
  }

  :global(.video-js .vjs-volume-panel) {
    height: 48px !important;
    display: inline-flex !important;
    align-items: center !important;
    margin: 0 4px !important;
    transition: width 0.2s ease !important;
  }

  :global(.video-js .vjs-volume-panel.vjs-volume-panel-horizontal.vjs-slider-active),
  :global(.video-js .vjs-volume-panel.vjs-volume-panel-horizontal:hover) {
    width: 120px !important;
  }

  :global(.video-js .vjs-volume-control.vjs-volume-horizontal) {
    height: 48px !important;
    display: inline-flex !important;
    align-items: center !important;
    margin: 0 6px !important;
    width: 60px !important;
  }

  :global(.video-js .vjs-volume-bar.vjs-slider-horizontal) {
    height: 6px !important;
    width: 60px !important;
    border-radius: 3px !important;
    margin: 0 !important;
    background-color: rgba(255, 255, 255, 0.3) !important;
    align-self: center !important;
    top: 0 !important;
  }

  :global(.video-js .vjs-volume-level) {
    border-radius: 3px !important;
    background-color: #7D60AC !important;
    height: 100% !important;
  }

  :global(.video-js .vjs-volume-level:before) {
    font-size: 14px !important;
    line-height: 14px !important;
    top: -4px !important;
  }

  :global(.video-js .vjs-time-control) {
    height: 48px !important;
    line-height: 48px !important;
    font-size: 14px !important;
    font-weight: 500 !important;
    padding: 0 4px !important;
    display: inline-flex !important;
    align-items: center !important;
    color: #C4B8DB !important;
    min-width: auto !important;
  }

  :global(.video-js .vjs-progress-control) {
    height: 48px !important;
    display: flex !important;
    align-items: center !important;
    flex: 1 !important;
    margin: 0 12px !important;
  }

  :global(.video-js .vjs-progress-holder) {
    height: 8px !important;
    border-radius: 4px !important;
    background-color: rgba(255, 255, 255, 0.25) !important;
    margin: 0 !important;
    width: 100% !important;
  }

  :global(.video-js .vjs-play-progress) {
    border-radius: 4px !important;
    background-color: #7D60AC !important;
  }

  :global(.video-js .vjs-play-progress:before) {
    font-size: 16px !important;
    line-height: 16px !important;
    top: -4px !important;
    color: #ffffff !important;
  }

  :global(.video-js .vjs-playlist-counter) {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 2px 8px;
    margin: 0 4px;
    font-size: 12px;
    font-weight: 600;
    color: #ffffff;
    background: rgba(255, 255, 255, 0.15);
    border: 1px solid rgba(255, 255, 255, 0.22);
    border-radius: 12px;
    letter-spacing: 0.5px;
    white-space: nowrap;
    backdrop-filter: blur(8px);
    cursor: default;
    line-height: 1;
    height: 24px;
    box-sizing: border-box;
    user-select: none;
  }

  :global(.video-js .vjs-playlist-counter:empty),
  :global(.video-js .vjs-playlist-counter.vjs-hidden),
  :global(.video-js .vjs-playlist-counter[style*="display: none"]) {
    display: none !important;
  }

  :global(.video-js .vjs-playback-rate) {
    width: 48px !important;
    height: 48px !important;
    display: inline-flex !important;
    align-items: center !important;
    justify-content: center !important;
    margin: 0 4px !important;
    position: relative !important;
    border-radius: 8px !important;
    background: transparent !important;
  }

  :global(.video-js .vjs-playback-rate:hover) {
    background: rgba(255, 255, 255, 0.18) !important;
    color: #8E74BD !important;
  }

  :global(.video-js .vjs-playback-rate .vjs-playback-rate-value) {
    font-size: 14px !important;
    font-weight: 700 !important;
    line-height: 48px !important;
    height: 48px !important;
    width: 48px !important;
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
    color: #F4F1FA !important;
    text-align: center !important;
    pointer-events: none !important;
  }

  :global(.video-js .vjs-playback-rate .vjs-menu) {
    bottom: 54px !important;
    left: 50% !important;
    transform: translateX(-50%) !important;
    width: 70px !important;
    margin-bottom: 4px !important;
    z-index: 30 !important;
  }

  :global(.video-js .vjs-playback-rate .vjs-menu-content) {
    background: rgba(13, 11, 18, 0.95) !important;
    border: 1px solid rgba(139, 128, 163, 0.2) !important;
    border-radius: 8px !important;
    padding: 6px 0 !important;
    box-shadow: 0 10px 25px rgba(0, 0, 0, 0.5) !important;
    backdrop-filter: blur(8px) !important;
  }

  :global(.video-js .vjs-playback-rate .vjs-menu-item) {
    font-size: 13px !important;
    font-weight: 600 !important;
    padding: 8px 12px !important;
    text-align: center !important;
    color: #C4B8DB !important;
    border-radius: 4px !important;
    margin: 2px 4px !important;
    cursor: pointer !important;
    text-transform: none !important;
  }

  :global(.video-js .vjs-playback-rate .vjs-menu-item:hover),
  :global(.video-js .vjs-playback-rate .vjs-menu-item.vjs-selected) {
    background: #7D60AC !important;
    color: #ffffff !important;
  }
</style>
