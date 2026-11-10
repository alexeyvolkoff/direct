<script>
  import { onMount, onDestroy, createEventDispatcher } from 'svelte';
  export let url;
  export let hideShare = false;

  const dispatch = createEventDispatcher();

  let loading = true;
  let error = null;
  let blobUrl = null;
  let abortController = null;
  let iframeElement = null;

  function toggleFullscreen() {
    const target = iframeElement || document.documentElement;
    if (!document.fullscreenElement && !document.webkitFullscreenElement) {
      if (target.requestFullscreen) target.requestFullscreen().catch(() => {});
      else if (target.webkitRequestFullscreen) target.webkitRequestFullscreen();
    } else {
      if (document.exitFullscreen) document.exitFullscreen().catch(() => {});
      else if (document.webkitExitFullscreen) document.webkitExitFullscreen();
    }
  }

  function handleMessage(event) {
    if (event.data && event.data.action) {
      if (event.data.action === 'closeViewer') dispatch('close');
      else if (event.data.action === 'shareFile') dispatch('share');
      else if (event.data.action === 'signFile') dispatch('sign');
      else if (event.data.action === 'toggleFullscreen') {
        toggleFullscreen();
        dispatch('toggleFullscreen');
      }
    }
  }

  function notifyFullscreenChange() {
    if (iframeElement && iframeElement.contentWindow) {
      const isFs = !!(document.fullscreenElement || document.webkitFullscreenElement);
      iframeElement.contentWindow.postMessage({ action: 'fullscreenChanged', isFullscreen: isFs }, '*');
    }
  }

  async function loadPdf() {
    try {
      loading = true;
      error = null;
      if (blobUrl) {
        URL.revokeObjectURL(blobUrl);
        blobUrl = null;
      }
      abortController = new AbortController();
      const res = await fetch(url, { signal: abortController.signal });
      if (!res.ok) throw new Error("HTTP " + res.status);
      const blob = await res.blob();
      blobUrl = URL.createObjectURL(blob);
      loading = false;
    } catch(err) {
      if (err.name === 'AbortError') return;
      console.error("Failed to load PDF blob:", err);
      error = err.message || "Failed to load PDF document";
      loading = false;
    }
  }

  onMount(() => {
    window.addEventListener('message', handleMessage);
    document.addEventListener('fullscreenchange', notifyFullscreenChange);
    document.addEventListener('webkitfullscreenchange', notifyFullscreenChange);
    loadPdf();
  });

  onDestroy(() => {
    window.removeEventListener('message', handleMessage);
    document.removeEventListener('fullscreenchange', notifyFullscreenChange);
    document.removeEventListener('webkitfullscreenchange', notifyFullscreenChange);
    if (abortController) abortController.abort();
    if (blobUrl) {
      URL.revokeObjectURL(blobUrl);
      blobUrl = null;
    }
  });
</script>

<div class="pdf-frame-wrapper">
  {#if loading}
    <div class="pdf-loading-overlay">
      <div class="pdf-spinner"></div>
      <div class="pdf-loading-text">Загрузка документа...</div>
    </div>
  {:else if error}
    <div class="pdf-error-overlay">
      <div class="pdf-error-title">Ошибка загрузки PDF</div>
      <div class="pdf-error-msg">{error}</div>
      <div class="pdf-actions">
        <button class="pdf-btn" on:click={loadPdf}>Повторить</button>
        <button class="pdf-btn pdf-btn-close" on:click={() => dispatch('close')}>Закрыть</button>
      </div>
    </div>
  {:else if blobUrl}
    <iframe
      bind:this={iframeElement}
      src={`/pdfjs/web/viewer.html?file=${encodeURIComponent(blobUrl)}${hideShare ? '&hideShare=1' : ''}`}
      title="PDF Viewer"
      frameborder="0"
      allowfullscreen
    ></iframe>
  {/if}
</div>

<style>
  .pdf-frame-wrapper {
    position: absolute;
    inset: 0;
    overflow: hidden;
    background: #16131F;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  iframe {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    border: none;
    display: block;
  }

  .pdf-loading-overlay,
  .pdf-error-overlay {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    color: #ECE7F6;
    gap: 16px;
  }

  .pdf-spinner {
    width: 44px;
    height: 44px;
    border: 3.5px solid rgba(168, 85, 247, 0.2);
    border-top-color: #C084FC;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  .pdf-loading-text {
    font-size: 15px;
    font-weight: 500;
    color: #8B80A3;
  }

  .pdf-error-title {
    font-size: 16px;
    font-weight: 600;
    color: #f87171;
  }

  .pdf-error-msg {
    font-size: 13px;
    color: #8B80A3;
  }

  .pdf-actions {
    display: flex;
    gap: 10px;
  }

  .pdf-btn {
    padding: 8px 18px;
    border-radius: 6px;
    background: #7E22CE;
    color: white;
    border: none;
    cursor: pointer;
    font-weight: 500;
    transition: background 0.15s ease;
  }

  .pdf-btn:hover {
    background: #6B21A8;
  }

  .pdf-btn-close {
    background: rgba(255, 255, 255, 0.1);
  }

  .pdf-btn-close:hover {
    background: rgba(255, 255, 255, 0.2);
  }
</style>
