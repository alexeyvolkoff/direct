<script>
  import { onMount, onDestroy, createEventDispatcher } from "svelte";
  import { _ } from "../lib/i18n";
  import WaveSurfer from "wavesurfer.js";

  export let url = "";
  export let filename = "Audio";
  export let poster = "";
  export let hasCover = false;
  export let title = "";
  export let description = "";
  export let playlist = [];
  export let currentIndex = 0;
  export let showClose = true;
  export let autoplay = true;

  const dispatch = createEventDispatcher();

  let waveformContainer;
  let wavesurfer = null;
  let isPlaying = false;
  let isReady = false;
  let isLoading = true;
  let isDestroyed = false;
  let currentTime = "0:00";
  let totalDuration = "0:00";
  let currentSeconds = 0;
  let durationSeconds = 0;

  const savedVolStr = typeof localStorage !== "undefined" ? localStorage.getItem("omd_media_volume") : null;
  const savedMutedStr = typeof localStorage !== "undefined" ? localStorage.getItem("omd_media_muted") : null;
  let volume = (savedVolStr !== null && !isNaN(parseFloat(savedVolStr))) ? Math.max(0, Math.min(1, parseFloat(savedVolStr))) : 1.0;
  let isMuted = savedMutedStr === "true";

  let playbackRate = 1.0;
  let repeatMode = "all"; // 'off' | 'all' | 'one'
  let isShuffle = false;

  let activeIndex = currentIndex;
  let isMobile = typeof window !== "undefined" ? window.innerWidth <= 768 : false;
  let showPlaylist = !isMobile && (playlist && playlist.length > 1);
  let showSpeedMenu = false;

  // Sync activeIndex with currentIndex prop
  $: if (typeof currentIndex === "number" && currentIndex !== activeIndex && playlist && playlist[currentIndex]) {
    activeIndex = currentIndex;
  }

  $: if (!playlist || playlist.length <= 1) {
    showPlaylist = false;
  }

  // Active track item
  $: currentTrack = (playlist && playlist[activeIndex]) ? playlist[activeIndex] : {
    url: url || "",
    poster: poster || "",
    hasCover: hasCover || false,
    name: filename || "Audio",
    title: title || filename || "Audio",
    description: description || ""
  };

  $: activeUrl = currentTrack.url || url || "";
  $: activeTitle = currentTrack.title || currentTrack.name || filename || "Audio";
  $: activeDesc = currentTrack.description || description || "";
  $: activePoster = (currentTrack.hasCover || hasCover) ? (currentTrack.poster || poster || "") : "";

  let coverLoadFailed = false;
  let lastPosterUrl = "";
  $: if (activePoster !== lastPosterUrl) {
    lastPosterUrl = activePoster;
    coverLoadFailed = false;
  }

  let lastLoadedUrl = "";
  $: if (wavesurfer && activeUrl && activeUrl !== lastLoadedUrl) {
    loadTrack(activeUrl);
  }

  function formatTime(seconds) {
    if (!seconds || isNaN(seconds) || seconds < 0) return "0:00";
    const mins = Math.floor(seconds / 60);
    const secs = Math.floor(seconds % 60);
    return `${mins}:${secs < 10 ? "0" : ""}${secs}`;
  }

  function initWaveSurfer() {
    if (!waveformContainer || wavesurfer) return;
    createWaveSurfer("");
  }

  function attachWaveHandlers(ws) {
    const media = typeof ws.getMediaElement === "function" ? ws.getMediaElement() : null;
    if (media) {
      media.addEventListener("play", () => {
        if (!isDestroyed) isPlaying = true;
      });
      media.addEventListener("playing", () => {
        if (!isDestroyed) isPlaying = true;
      });
      media.addEventListener("pause", () => {
        isPlaying = false;
      });
      media.addEventListener("ended", () => {
        isPlaying = false;
      });
      media.addEventListener("error", () => {
        isPlaying = false;
        isLoading = false;
      });
    }

    ws.on("loading", () => {
      if (isDestroyed) return;
      isLoading = true;
      isReady = false;
    });

    ws.on("ready", () => {
      if (isDestroyed) return;
      isLoading = false;
      isReady = true;
      durationSeconds = ws.getDuration();
      totalDuration = formatTime(durationSeconds);
      ws.setVolume(isMuted ? 0 : volume);
      ws.setPlaybackRate(playbackRate);

      const m = typeof ws.getMediaElement === "function" ? ws.getMediaElement() : null;
      if (autoplay && m) {
        const p = ws.play();
        if (p && typeof p.then === "function") {
          p.then(() => {
            if (!isDestroyed) {
              isPlaying = m ? !m.paused : true;
            }
          }).catch(() => {
            isPlaying = false;
          });
        } else {
          isPlaying = m ? !m.paused : false;
        }
      } else {
        isPlaying = false;
      }
    });

    ws.on("play", () => {
      if (isDestroyed) return;
      const m = typeof ws.getMediaElement === "function" ? ws.getMediaElement() : null;
      isPlaying = m ? !m.paused : true;
      isLoading = false;
    });

    ws.on("pause", () => {
      isPlaying = false;
    });

    ws.on("timeupdate", (currentTimeSec) => {
      currentSeconds = currentTimeSec;
      currentTime = formatTime(currentTimeSec);
    });

    ws.on("finish", () => {
      isPlaying = false;
      handleTrackFinished();
    });

    ws.on("error", (err) => {
      console.warn("[AudioViewer] WaveSurfer error:", err);
      isLoading = false;
      isPlaying = false;
    });
  }

  function getAudioContext() {
    try {
      if (typeof window !== "undefined" && (window.AudioContext || window.webkitAudioContext)) {
        return new (window.AudioContext || window.webkitAudioContext)();
      }
    } catch (_) {}
    return null;
  }

  const WAVE_BUCKETS_PER_CHUNK = 40;
  const WAVE_SAMPLE_COUNT = 8;
  const WAVE_SAMPLE_RANGE = 262144; // 256 KB per sample chunk

  async function fetchRangeProbe(url) {
    const res = await fetch(url, { headers: { "Range": "bytes=0-0" } });
    const cr = res.headers.get("Content-Range") || res.headers.get("content-range");
    const total = cr ? parseInt(cr.split("/")[1], 10) : 0;
    return total;
  }

  async function prepareWaveData(trackUrl) {
    if (typeof window === "undefined" || !trackUrl) return null;
    try {
      const totalSize = await fetchRangeProbe(trackUrl);
      if (!totalSize || totalSize <= 0) return null;

      // Small files: no need to sample, allow full-quality decode in fallback
      if (totalSize <= 4 * 1024 * 1024) return null;

      const audioCtx = getAudioContext();
      if (!audioCtx) return null;

      const sampleCount = Math.min(WAVE_SAMPLE_COUNT, Math.max(2, Math.ceil(totalSize / (4 * WAVE_SAMPLE_RANGE))));
      const chunkSize = Math.min(WAVE_SAMPLE_RANGE, Math.floor(totalSize / sampleCount));
      const gaps = Math.max(0, sampleCount - 1);
      const step = gaps > 0 ? Math.floor((totalSize - chunkSize) / gaps) : 0;

      const chunkPeaks = [];
      const bitrates = [];

      for (let i = 0; i < sampleCount; i++) {
        const offset = i * step;
        if (offset + chunkSize > totalSize) break;
        const res = await fetch(trackUrl, { headers: { "Range": `bytes=${offset}-${offset + chunkSize - 1}` } });
        if (!res.ok && res.status !== 206) break;
        const buf = await res.arrayBuffer();
        if (!buf || !buf.byteLength) break;
        const decoded = await audioCtx.decodeAudioData(buf).catch(() => null);
        if (!decoded || !decoded.duration || decoded.duration <= 0) break;

        const bytes = buf.byteLength;
        const dur = decoded.duration;
        if (dur > 0.0001) bitrates.push((bytes * 8) / dur);

        const data = decoded.getChannelData(0);
        const buckets = WAVE_BUCKETS_PER_CHUNK;
        const peaksArr = new Float32Array(buckets);
        const per = Math.floor(data.length / buckets);
        for (let b = 0; b < buckets; b++) {
          let max = 0;
          const startIdx = b * per;
          const endIdx = startIdx + per;
          for (let j = startIdx; j < endIdx; j += 16) { // stride for speed
            const v = Math.abs(data[j] || 0);
            if (v > max) max = v;
          }
          peaksArr[b] = max;
        }
        chunkPeaks.push(peaksArr);
      }

      if (chunkPeaks.length === 0) return null;

      bitrates.sort((a, b) => a - b);
      const mid = Math.floor(bitrates.length / 2);
      const medianBitrate = bitrates.length % 2 ? bitrates[mid] : (bitrates[mid - 1] + bitrates[mid]) / 2;
      const estDuration = (totalSize * 8) / medianBitrate;
      if (!estDuration || !isFinite(estDuration)) return null;

      // Concatenate per-chunk peaks with amplitude normalization
      const buckets = chunkPeaks.length * WAVE_BUCKETS_PER_CHUNK;
      let globalMax = 0.0001;
      for (const cp of chunkPeaks) {
        for (let i = 0; i < cp.length; i++) globalMax = Math.max(globalMax, cp[i]);
      }
      const peaks = new Float32Array(buckets);
      let idx = 0;
      for (const cp of chunkPeaks) {
        for (let i = 0; i < cp.length; i++) {
          peaks[idx++] = Math.min(1, cp[i] / globalMax);
        }
      }

      audioCtx.close().catch(() => {});
      return { peaks, duration: estDuration };
    } catch (e) {
      console.warn("[AudioViewer] Waveform sampling failed, falling back to full load:", e);
      return null;
    }
  }

  function createWaveSurfer(trackUrl, peaks = null, duration = null) {
    if (wavesurfer) {
      try {
        const oldMedia = typeof wavesurfer.getMediaElement === "function" ? wavesurfer.getMediaElement() : null;
        if (oldMedia) {
          oldMedia.pause();
          oldMedia.removeAttribute("src");
          oldMedia.load();
        }
        wavesurfer.destroy();
      } catch (_) {}
      wavesurfer = null;
    }
    if (!waveformContainer) return null;

    try {
      const options = {
        container: waveformContainer,
        waveColor: "rgba(162, 138, 208, 0.35)",
        progressColor: "#A28AD0",
        cursorColor: "#F8F7FC",
        cursorWidth: 2,
        barWidth: 3,
        barGap: 2,
        barRadius: 3,
        height: 72,
        normalize: true,
      };
      if (peaks && duration) {
        options.url = trackUrl;
        options.peaks = [peaks];
        options.duration = duration;
      }

      const ws = WaveSurfer.create(options);
      wavesurfer = ws;
      attachWaveHandlers(ws);
      return ws;
    } catch (err) {
      console.error("[AudioViewer] Init error:", err);
      isLoading = false;
      isPlaying = false;
      return null;
    }
  }

  let loadToken = 0;
  async function loadTrack(trackUrl) {
    if (!trackUrl) return;
    if (!waveformContainer) return;
    const token = ++loadToken;
    lastLoadedUrl = trackUrl;
    isLoading = true;
    isReady = false;
    isPlaying = false;
    currentTime = "0:00";
    currentSeconds = 0;
    try {
      // Try streaming-friendly mode: sampled waveform peaks + native media element
      const waveData = await prepareWaveData(trackUrl);
      if (token !== loadToken || isDestroyed) return;
      const ws = createWaveSurfer(trackUrl, waveData ? waveData.peaks : null, waveData ? waveData.duration : null);
      if (!ws) return;
      if (!waveData) {
        // Fallback: classic full decode
        await ws.load(trackUrl);
      }
    } catch (e) {
      if (token !== loadToken || isDestroyed) return;
      console.error("[AudioViewer] load track error:", e);
      isLoading = false;
      isPlaying = false;
    }
  }

  function togglePlay() {
    if (!wavesurfer || isLoading || !isReady) return;
    const m = typeof wavesurfer.getMediaElement === "function" ? wavesurfer.getMediaElement() : null;
    const isCurrentlyPlaying = m ? !m.paused : (typeof wavesurfer.isPlaying === "function" ? wavesurfer.isPlaying() : isPlaying);

    if (isCurrentlyPlaying) {
      wavesurfer.pause();
      isPlaying = false;
    } else {
      const p = wavesurfer.play();
      if (p && typeof p.then === "function") {
        p.then(() => {
          if (!isDestroyed) {
            isPlaying = m ? !m.paused : true;
          }
        }).catch(() => {
          isPlaying = false;
        });
      } else {
        isPlaying = m ? !m.paused : true;
      }
    }
  }

  function handleTrackFinished() {
    if (repeatMode === "one") {
      wavesurfer.seekTo(0);
      wavesurfer.play().catch(() => {});
      return;
    }

    if (playlist && playlist.length > 1) {
      nextTrack();
    }
  }

  function selectTrack(index) {
    if (!playlist || playlist.length === 0) return;
    if (index < 0 || index >= playlist.length) return;
    activeIndex = index;
    if (isMobile) showPlaylist = false;

    const t = playlist[index];
    if (!t) return;

    loadTrack(t.url);
    dispatch("change", { index, audio: t });
  }

  function nextTrack() {
    if (!playlist || playlist.length <= 1) {
      if (wavesurfer) {
        wavesurfer.seekTo(0);
        wavesurfer.play().catch(() => {});
      }
      return;
    }

    let nextIdx;
    if (isShuffle) {
      do {
        nextIdx = Math.floor(Math.random() * playlist.length);
      } while (nextIdx === activeIndex && playlist.length > 1);
    } else {
      nextIdx = activeIndex + 1;
      if (nextIdx >= playlist.length) {
        if (repeatMode === "off") return;
        nextIdx = 0;
      }
    }
    selectTrack(nextIdx);
  }

  function prevTrack() {
    if (currentSeconds > 3) {
      if (wavesurfer) wavesurfer.seekTo(0);
      return;
    }

    if (!playlist || playlist.length <= 1) {
      if (wavesurfer) wavesurfer.seekTo(0);
      return;
    }

    let prevIdx = activeIndex - 1;
    if (prevIdx < 0) {
      prevIdx = playlist.length - 1;
    }
    selectTrack(prevIdx);
  }

  function toggleRepeat() {
    if (repeatMode === "off") repeatMode = "all";
    else if (repeatMode === "all") repeatMode = "one";
    else repeatMode = "off";
  }

  function toggleShuffle() {
    isShuffle = !isShuffle;
  }

  function setRate(rate) {
    playbackRate = rate;
    showSpeedMenu = false;
    if (wavesurfer) wavesurfer.setPlaybackRate(rate);
  }

  function saveVolumePref() {
    try {
      if (typeof localStorage !== "undefined") {
        localStorage.setItem("omd_media_volume", String(volume));
        localStorage.setItem("omd_media_muted", String(isMuted));
      }
    } catch (_) {}
  }

  function handleVolumeChange(e) {
    volume = parseFloat(e.target.value);
    isMuted = volume === 0;
    if (wavesurfer) wavesurfer.setVolume(isMuted ? 0 : volume);
    saveVolumePref();
  }

  function toggleMute() {
    isMuted = !isMuted;
    if (wavesurfer) wavesurfer.setVolume(isMuted ? 0 : volume);
    saveVolumePref();
  }

  function handleKeydown(e) {
    if (e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA") return;

    if (e.code === "Space") {
      e.preventDefault();
      togglePlay();
    } else if (e.code === "ArrowRight") {
      e.preventDefault();
      if (e.shiftKey) nextTrack();
      else if (wavesurfer && !isLoading) wavesurfer.skip(5);
    } else if (e.code === "ArrowLeft") {
      e.preventDefault();
      if (e.shiftKey) prevTrack();
      else if (wavesurfer && !isLoading) wavesurfer.skip(-5);
    } else if (e.code === "KeyM") {
      e.preventDefault();
      toggleMute();
    } else if (e.code === "Escape") {
      e.preventDefault();
      handleClose();
    }
  }

  function stopAndCleanup() {
    isDestroyed = true;
    isLoading = false;
    isPlaying = false;
    isReady = false;
    if (wavesurfer) {
      try {
        wavesurfer.pause();
        wavesurfer.stop();
        const media = typeof wavesurfer.getMediaElement === "function" ? wavesurfer.getMediaElement() : null;
        if (media) {
          media.pause();
          media.removeAttribute("src");
          media.load();
        }
        wavesurfer.destroy();
      } catch (err) {
        console.warn("[AudioViewer] Cleanup error:", err);
      }
      wavesurfer = null;
    }
  }

  function handleClose() {
    stopAndCleanup();
    dispatch("close", { audio: currentTrack });
  }

  onMount(() => {
    initWaveSurfer();
    window.addEventListener("keydown", handleKeydown);
  });

  onDestroy(() => {
    if (typeof window !== "undefined") {
      window.removeEventListener("keydown", handleKeydown);
    }
    stopAndCleanup();
  });
</script>

<div class="audio-viewer-backdrop" role="dialog" aria-modal="true">
  <!-- Top Navigation Bar -->
  <header class="audio-viewer-header">
    <div class="header-left">
      {#if showClose}
        <button class="icon-btn" on:click={handleClose} title={$_("close", {}, "Close")}>
          <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
            <line x1="18" y1="6" x2="6" y2="18"></line>
            <line x1="6" y1="6" x2="18" y2="18"></line>
          </svg>
        </button>
      {/if}
      <div class="track-meta">
        <h2 class="track-title" title={activeTitle}>{activeTitle}</h2>
        {#if activeDesc}
          <p class="track-artist">{activeDesc}</p>
        {/if}
      </div>
    </div>

    <div class="header-right">
      {#if playlist && playlist.length > 1}
        <button
          class="icon-btn playlist-toggle-btn"
          class:active={showPlaylist}
          on:click={() => showPlaylist = !showPlaylist}
          title={$_("playlist", {}, "Playlist")}
        >
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
            <line x1="8" y1="6" x2="21" y2="6"></line>
            <line x1="8" y1="12" x2="21" y2="12"></line>
            <line x1="8" y1="18" x2="21" y2="18"></line>
            <circle cx="4" cy="6" r="1" fill="currentColor"></circle>
            <circle cx="4" cy="12" r="1" fill="currentColor"></circle>
            <circle cx="4" cy="18" r="1" fill="currentColor"></circle>
          </svg>
          <span class="playlist-count">{playlist.length}</span>
        </button>
      {/if}
    </div>
  </header>

  <!-- Main Body Layout -->
  <div class="audio-viewer-body">
    <!-- Center Audio Stage -->
    <main class="audio-stage">
      <!-- Clean Album Cover Card -->
      <div class="cover-container">
        <div class="album-cover-card" class:has-image={!!activePoster && !coverLoadFailed} class:is-playing={isPlaying}>
          {#if activePoster && !coverLoadFailed}
            <img
              src={activePoster}
              alt="Album Cover"
              class="cover-image"
              on:error={(e) => {
                const imgEl = e.currentTarget;
                if (imgEl && !imgEl.dataset.retried) {
                  imgEl.dataset.retried = 'true';
                  imgEl.src = activePoster + (activePoster.includes('?') ? '&' : '?') + 't=' + Date.now();
                } else {
                  coverLoadFailed = true;
                }
              }}
            />
          {:else}
            <div class="cover-placeholder">
              <div class="music-icon-wrapper">
                <svg viewBox="0 0 24 24" width="56" height="56" fill="none" stroke="#A28AD0" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M9 18V5l12-2v13"></path>
                  <circle cx="6" cy="18" r="3"></circle>
                  <circle cx="18" cy="16" r="3"></circle>
                </svg>
              </div>
            </div>
          {/if}
        </div>
      </div>

      <!-- Track Info in Stage -->
      <div class="stage-track-info">
        <h1 class="stage-title" title={activeTitle}>{activeTitle}</h1>
        {#if activeDesc}
          <p class="stage-desc">{activeDesc}</p>
        {/if}
      </div>

      <!-- Waveform Section -->
      <div class="waveform-section">
        <div class="waveform-container" bind:this={waveformContainer}></div>
        <div class="time-display">
          <span>{currentTime}</span>
          <span>{totalDuration}</span>
        </div>
      </div>

      <!-- Playback Controls -->
      <div class="controls-toolbar">
        <!-- Shuffle -->
        <button
          class="control-btn small-btn"
          class:active={isShuffle}
          on:click={toggleShuffle}
          title="Shuffle"
        >
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
            <polyline points="16 3 21 3 21 8"></polyline>
            <line x1="4" y1="20" x2="21" y2="3"></line>
            <polyline points="21 16 21 21 16 21"></polyline>
            <line x1="15" y1="15" x2="21" y2="21"></line>
            <line x1="4" y1="4" x2="9" y2="9"></line>
          </svg>
        </button>

        <!-- Prev Track -->
        <button class="control-btn" on:click={prevTrack} title="Previous">
          <svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor">
            <path d="M6 6h2v12H6zm3.5 6l8.5 6V6z"></path>
          </svg>
        </button>

        <!-- Play / Pause / Loading Primary -->
        <button
          class="play-pause-btn"
          class:is-loading={isLoading}
          disabled={isLoading}
          on:click={togglePlay}
          title={isLoading ? $_("loading", {}, "Loading...") : (isPlaying ? $_("pause", {}, "Pause") : $_("play", {}, "Play"))}
        >
          {#if isLoading}
            <div class="audio-spinner"></div>
          {:else if isPlaying}
            <svg viewBox="0 0 24 24" width="26" height="26" fill="currentColor">
              <rect x="6" y="4" width="4" height="16" rx="1"></rect>
              <rect x="14" y="4" width="4" height="16" rx="1"></rect>
            </svg>
          {:else}
            <svg viewBox="0 0 24 24" width="26" height="26" fill="currentColor" style="margin-left: 2px;">
              <polygon points="5 3 19 12 5 21 5 3"></polygon>
            </svg>
          {/if}
        </button>

        <!-- Next Track -->
        <button class="control-btn" on:click={nextTrack} title="Next">
          <svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor">
            <path d="M6 18l8.5-6L6 6v12zM16 6v12h2V6h-2z"></path>
          </svg>
        </button>

        <!-- Repeat Mode -->
        <button
          class="control-btn small-btn"
          class:active={repeatMode !== "off"}
          on:click={toggleRepeat}
          title={`Repeat: ${repeatMode}`}
        >
          {#if repeatMode === "one"}
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M17 2l4 4-4 4"></path>
              <path d="M3 11v-1a4 4 0 0 1 4-4h14"></path>
              <path d="M7 22l-4-4 4-4"></path>
              <path d="M21 13v1a4 4 0 0 1-4 4H3"></path>
              <text x="10" y="14" font-size="8" fill="currentColor" font-weight="bold">1</text>
            </svg>
          {:else}
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
              <polyline points="17 1 21 5 17 9"></polyline>
              <path d="M3 11V9a4 4 0 0 1 4-4h14"></path>
              <polyline points="7 23 3 19 7 15"></polyline>
              <path d="M21 13v2a4 4 0 0 1-4 4H3"></path>
            </svg>
          {/if}
        </button>

        <!-- Volume & Speed Settings -->
        <div class="extra-controls">
          <!-- Speed Menu Toggle -->
          <div class="speed-menu-wrapper">
            <button class="speed-btn" on:click={() => showSpeedMenu = !showSpeedMenu}>
              {playbackRate}x
            </button>
            {#if showSpeedMenu}
              <div class="speed-dropdown">
                {#each [0.75, 1.0, 1.25, 1.5, 2.0] as rate}
                  <button class="speed-opt" class:active={playbackRate === rate} on:click={() => setRate(rate)}>
                    {rate}x
                  </button>
                {/each}
              </div>
            {/if}
          </div>

          <!-- Volume Controls -->
          <div class="volume-box">
            <button class="icon-btn volume-btn" on:click={toggleMute} title="Mute">
              {#if isMuted || volume === 0}
                <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
                  <polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"></polygon>
                  <line x1="23" y1="9" x2="17" y2="15"></line>
                  <line x1="17" y1="9" x2="23" y2="15"></line>
                </svg>
              {:else}
                <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
                  <polygon points="11 5 6 9 2 9 2 15 6 15 11 5"></polygon>
                  <path d="M19.07 4.93a10 10 0 0 1 0 14.14M15.54 8.46a5 5 0 0 1 0 7.07"></path>
                </svg>
              {/if}
            </button>
            <input
              type="range"
              min="0"
              max="1"
              step="0.05"
              value={isMuted ? 0 : volume}
              on:input={handleVolumeChange}
              class="volume-slider"
            />
          </div>
        </div>
      </div>
    </main>

    <!-- Side Playlist Panel -->
    {#if showPlaylist && playlist && playlist.length > 0}
      <aside class="playlist-sidebar">
        <div class="playlist-header">
          <h3>{$_("playlist", {}, "Playlist")}</h3>
          <span class="badge">{playlist.length}</span>
        </div>
        <ul class="playlist-items">
          {#each playlist as item, idx (item.id || item.url || idx)}
            <li
              class="playlist-item"
              class:active={idx === activeIndex}
              on:click={() => selectTrack(idx)}
            >
              <div class="item-index">
                {#if idx === activeIndex && isPlaying}
                  <span class="playing-bars">
                    <span></span><span></span><span></span>
                  </span>
                {:else}
                  {idx + 1}
                {/if}
              </div>
              {#if item.hasCover && (item.thumb || item.poster)}
                <img
                  src={item.thumb || item.poster.replace('cover=600', 'cover=120')}
                  alt=""
                  class="item-thumb"
                  loading="lazy"
                  on:error={(e) => { e.currentTarget.style.display = 'none'; }}
                />
              {/if}
              <div class="item-info">
                <div class="item-name">{item.title || item.name}</div>
                {#if item.description}
                  <div class="item-sub">{item.description}</div>
                {/if}
              </div>
            </li>
          {/each}
        </ul>
      </aside>
    {/if}
  </div>
</div>

<style>
  .audio-viewer-backdrop {
    width: 100%;
    height: 100%;
    min-height: 420px;
    background: radial-gradient(circle at center, rgba(13, 11, 18, 0.96) 0%, rgba(2, 6, 23, 0.98) 100%);
    backdrop-filter: blur(24px);
    -webkit-backdrop-filter: blur(24px);
    display: flex;
    flex-direction: column;
    color: #F8F7FC;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    user-select: none;
    position: relative;
    overflow: hidden;
  }

  /* Header */
  .audio-viewer-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 16px 24px;
    background: rgba(13, 11, 18, 0.5);
    border-bottom: 1px solid rgba(255, 255, 255, 0.08);
    flex-shrink: 0;
  }

  .header-left {
    display: flex;
    align-items: center;
    gap: 16px;
    min-width: 0;
  }

  .track-meta {
    min-width: 0;
  }

  .track-title {
    margin: 0;
    font-size: 15px;
    font-weight: 600;
    color: #F8F7FC;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    max-width: 50vw;
  }

  .track-artist {
    margin: 2px 0 0 0;
    font-size: 12px;
    color: #8B80A3;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .icon-btn {
    background: transparent;
    border: none;
    color: #8B80A3;
    cursor: pointer;
    padding: 8px;
    border-radius: 8px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    transition: all 0.2s ease;
  }

  .icon-btn:hover {
    color: #F8F7FC;
    background: rgba(255, 255, 255, 0.1);
  }

  .playlist-toggle-btn {
    position: relative;
    gap: 6px;
    padding: 6px 12px;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.1);
  }

  .playlist-toggle-btn.active {
    color: #A28AD0;
    background: rgba(142, 116, 189, 0.15);
    border-color: rgba(142, 116, 189, 0.4);
  }

  .playlist-count {
    font-size: 12px;
    font-weight: 600;
  }

  /* Body Layout */
  .audio-viewer-body {
    flex: 1;
    display: flex;
    overflow: hidden;
    position: relative;
  }

  .audio-stage {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 20px 24px 20px 24px;
    gap: 16px;
    overflow-y: auto;
    overflow-x: hidden;
    box-sizing: border-box;
  }

  /* Album Cover Card (Static & Crisp) */
  .cover-container {
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .album-cover-card {
    width: clamp(130px, 20vh, 190px);
    height: clamp(130px, 20vh, 190px);
    border-radius: 18px;
    background: linear-gradient(135deg, rgba(22, 19, 31, 0.8) 0%, rgba(13, 11, 18, 0.9) 100%);
    border: 1px solid rgba(255, 255, 255, 0.12);
    box-shadow: 0 16px 40px rgba(0, 0, 0, 0.6);
    overflow: hidden;
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: transform 0.3s ease, box-shadow 0.3s ease;
  }

  .album-cover-card.is-playing {
    box-shadow: 0 20px 50px rgba(0, 0, 0, 0.7), 0 0 30px rgba(142, 116, 189, 0.25);
    border-color: rgba(142, 116, 189, 0.3);
  }

  .cover-image {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
  }

  .cover-placeholder {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 100%;
    height: 100%;
    background: radial-gradient(circle at center, #252330 0%, #181720 100%);
  }

  .music-icon-wrapper {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 88px;
    height: 88px;
    border-radius: 50%;
    background: rgba(142, 116, 189, 0.08);
    border: 1px solid rgba(142, 116, 189, 0.2);
  }

  /* Stage Track Info */
  .stage-track-info {
    text-align: center;
    max-width: 600px;
  }

  .stage-title {
    margin: 0;
    font-size: 20px;
    font-weight: 700;
    color: #F8F7FC;
    letter-spacing: -0.02em;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .stage-desc {
    margin: 4px 0 0 0;
    font-size: 13px;
    color: #8B80A3;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  /* Waveform Section */
  .waveform-section {
    width: 100%;
    max-width: 680px;
    background: rgba(22, 19, 31, 0.4);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 16px;
    padding: 18px 24px 12px 24px;
    backdrop-filter: blur(12px);
    box-sizing: border-box;
  }

  .waveform-container {
    width: 100%;
    height: 72px;
    cursor: pointer;
  }

  .time-display {
    display: flex;
    justify-content: space-between;
    margin-top: 8px;
    font-size: 13px;
    color: #8B80A3;
    font-variant-numeric: tabular-nums;
  }

  /* Controls */
  .controls-toolbar {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 16px;
    width: 100%;
    max-width: 680px;
    flex-wrap: wrap;
    padding: 6px 0 6px 0;
    flex-shrink: 0;
    box-sizing: border-box;
  }

  .control-btn {
    background: rgba(255, 255, 255, 0.06);
    border: 1px solid rgba(255, 255, 255, 0.1);
    color: #ECE7F6;
    width: 44px;
    height: 44px;
    border-radius: 50%;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.2s ease;
    flex-shrink: 0;
  }

  .control-btn:hover {
    background: rgba(255, 255, 255, 0.15);
    color: #F8F7FC;
    transform: scale(1.05);
  }

  .control-btn.small-btn {
    width: 36px;
    height: 36px;
  }

  .control-btn.active {
    color: #A28AD0;
    background: rgba(142, 116, 189, 0.15);
    border-color: rgba(142, 116, 189, 0.4);
  }

  .play-pause-btn {
    width: 58px;
    height: 58px;
    border-radius: 50%;
    background: linear-gradient(135deg, #A28AD0 0%, #563F7A 100%);
    color: #020617;
    border: none;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    box-shadow: 0 4px 20px rgba(142, 116, 189, 0.4);
    transition: all 0.2s cubic-bezier(0.4, 0, 0.2, 1);
    flex-shrink: 0;
  }

  .play-pause-btn:hover:not(:disabled) {
    transform: scale(1.08);
    box-shadow: 0 6px 25px rgba(142, 116, 189, 0.6);
  }

  .play-pause-btn.is-loading,
  .play-pause-btn:disabled {
    cursor: wait;
    opacity: 0.85;
    transform: none !important;
  }

  .audio-spinner {
    width: 24px;
    height: 24px;
    border: 3px solid rgba(2, 6, 23, 0.25);
    border-top-color: #020617;
    border-radius: 50%;
    animation: audio-spin 0.8s linear infinite;
  }

  @keyframes audio-spin {
    0% { transform: rotate(0deg); }
    100% { transform: rotate(360deg); }
  }

  .extra-controls {
    display: flex;
    align-items: center;
    gap: 16px;
    margin-left: 12px;
  }

  /* Speed Menu */
  .speed-menu-wrapper {
    position: relative;
  }

  .speed-btn {
    background: rgba(255, 255, 255, 0.06);
    border: 1px solid rgba(255, 255, 255, 0.1);
    color: #8B80A3;
    padding: 4px 10px;
    border-radius: 6px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s;
  }

  .speed-btn:hover {
    color: #F8F7FC;
    background: rgba(255, 255, 255, 0.12);
  }

  .speed-dropdown {
    position: absolute;
    bottom: calc(100% + 8px);
    left: 50%;
    transform: translateX(-50%);
    background: #252330;
    border: 1px solid rgba(255, 255, 255, 0.1);
    border-radius: 8px;
    padding: 4px;
    display: flex;
    flex-direction: column;
    gap: 2px;
    box-shadow: 0 10px 25px rgba(0,0,0,0.5);
    z-index: 10;
  }

  .speed-opt {
    background: transparent;
    border: none;
    color: #C4B8DB;
    padding: 6px 12px;
    font-size: 12px;
    border-radius: 4px;
    cursor: pointer;
    white-space: nowrap;
  }

  .speed-opt:hover, .speed-opt.active {
    color: #A28AD0;
    background: rgba(142, 116, 189, 0.15);
  }

  /* Volume */
  .volume-box {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .volume-slider {
    width: 80px;
    accent-color: #A28AD0;
    cursor: pointer;
  }

  /* Sidebar Playlist */
  .playlist-sidebar {
    width: 320px;
    background: rgba(13, 11, 18, 0.7);
    border-left: 1px solid rgba(255, 255, 255, 0.08);
    display: flex;
    flex-direction: column;
    flex-shrink: 0;
  }

  .playlist-header {
    padding: 16px 20px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.06);
    display: flex;
    align-items: center;
    justify-content: space-between;
  }

  .playlist-header h3 {
    margin: 0;
    font-size: 15px;
    font-weight: 600;
  }

  .badge {
    background: rgba(255, 255, 255, 0.1);
    color: #8B80A3;
    padding: 2px 8px;
    border-radius: 12px;
    font-size: 12px;
  }

  .playlist-items {
    list-style: none;
    margin: 0;
    padding: 8px;
    overflow-y: auto;
    flex: 1;
  }

  .playlist-item {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 10px 12px;
    border-radius: 8px;
    cursor: pointer;
    transition: all 0.2s;
  }

  .playlist-item:hover {
    background: rgba(255, 255, 255, 0.05);
  }

  .playlist-item.active {
    background: rgba(142, 116, 189, 0.15);
    border: 1px solid rgba(142, 116, 189, 0.3);
  }

  .item-index {
    font-size: 13px;
    color: #73688A;
    width: 20px;
    text-align: center;
    flex-shrink: 0;
  }

  .playlist-item.active .item-index {
    color: #A28AD0;
  }

  .item-thumb {
    width: 36px;
    height: 36px;
    border-radius: 6px;
    object-fit: cover;
    flex-shrink: 0;
    border: 1px solid rgba(255, 255, 255, 0.1);
  }

  .item-info {
    min-width: 0;
    flex: 1;
  }

  .item-name {
    font-size: 13px;
    font-weight: 500;
    color: #ECE7F6;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .playlist-item.active .item-name {
    color: #A28AD0;
    font-weight: 600;
  }

  .item-sub {
    font-size: 11px;
    color: #8B80A3;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    margin-top: 2px;
  }

  /* Equalizer Bars Animation */
  .playing-bars {
    display: inline-flex;
    align-items: flex-end;
    gap: 2px;
    height: 12px;
  }

  .playing-bars span {
    width: 2px;
    background: #A28AD0;
    animation: eqBounce 0.8s ease infinite alternate;
  }

  .playing-bars span:nth-child(1) { height: 40%; animation-delay: 0.1s; }
  .playing-bars span:nth-child(2) { height: 100%; animation-delay: 0.3s; }
  .playing-bars span:nth-child(3) { height: 60%; animation-delay: 0.2s; }

  @keyframes eqBounce {
    from { height: 20%; }
    to { height: 100%; }
  }

  @media (max-width: 768px) {
    .audio-stage {
      padding: 16px;
      gap: 16px;
      overflow-x: hidden;
    }
    .waveform-section {
      padding: 14px 16px 10px 16px;
    }
    .album-cover-card {
      width: 160px;
      height: 160px;
      border-radius: 14px;
    }
    .extra-controls {
      display: none;
    }
    .playlist-sidebar {
      position: absolute;
      top: 0;
      right: 0;
      bottom: 0;
      width: 85vw;
      max-width: 320px;
      z-index: 20;
      box-shadow: -10px 0 30px rgba(0,0,0,0.8);
    }
  }
</style>
