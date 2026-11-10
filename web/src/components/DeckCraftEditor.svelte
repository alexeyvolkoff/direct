<script>
  /**
   * DeckCraftEditor — presentations (pptx/potx/ppsx) edited by DeckCraft WASM in
   * an iframe. Uses our fork of deckcraft-web with the host bridge (same family
   * protocol as GridCraft's `?host=parent`):
   *
   *   → parent: {type:"deckcraft:ready"}, {type:"deckcraft:save", name, bytes},
   *             {type:"deckcraft:dirty", dirty}, {type:"deckcraft:docClosed"},
   *             {type:"deckcraft:failed", error}
   *   ← app:    {type:"deckcraft:open", name, bytes}, {type:"deckcraft:cmd", id, params?}
   *
   * Close flow (no browser popups): the ✕ asks through a native OMD modal when
   * the deck is dirty — Save routes through the app's own save (kept in the
   * format the deck was opened as), Don't Save sends `file.close` — and
   * `deckcraft:docClosed` closes the OMD viewer.
   */
  import { onMount, onDestroy, createEventDispatcher } from 'svelte';

  export let filename = 'deck.pptx';
  export let filePath = '';
  export let bytes = null; // ArrayBuffer of the ORIGINAL file
  export let activeDevice = null;

  const dispatch = createEventDispatcher();

  // Version-pinned fork build served from public/ (same pattern as /pdfjs).
  const DC_BASE = '/deckcraft/v0.7.0/';

  let frame;
  let dcReady = false;

  function cleanDevicePath(p) {
    if (!p || p === '~') return '/';
    let devPath = p;
    if (devPath.startsWith('~/')) devPath = '/' + devPath.slice(2);
    else if (!devPath.startsWith('/')) devPath = '/' + devPath;
    return devPath;
  }

  // Chunked so multi-MB decks don't blow the call stack.
  function arrayBufferToBase64(buffer) {
    const arr = new Uint8Array(buffer);
    const chunk = 0x8000;
    let binary = '';
    for (let i = 0; i < arr.length; i += chunk) {
      binary += String.fromCharCode.apply(null, arr.subarray(i, i + chunk));
    }
    return btoa(binary);
  }

  function post(msg) {
    if (frame && frame.contentWindow) frame.contentWindow.postMessage(msg, window.location.origin);
  }

  function sendCmd(id, params) {
    post({ type: 'deckcraft:cmd', id, params: JSON.stringify(params || {}) });
  }

  function postOpen() {
    dcReady = true;
    if (bytes) {
      post({ type: 'deckcraft:open', name: filename, bytes });
      // Close lives in DeckCraft's own Quick Access Toolbar and runs their
      // guarded close flow (Save / Don't Save / Cancel, like Ctrl+W).
      sendCmd('app.qat', { add: 'file.close', label: 'Close', icon: 'close' });
    }
  }

  async function handleBridgeSave(m) {
    try {
      const name = m.name || filename;
      const ext = (name.split('.').pop() || '').toLowerCase();
      let target = cleanDevicePath(filePath || filename);
      const origExt = ((filePath || filename).split('.').pop() || '').toLowerCase();
      // The app may save under a different format (the deck's own .deckcraft,
      // PDF exports): keep the original directory, use the saved extension.
      if (ext && ext !== origExt) target = target.replace(/\.[^/.]+$/, '') + '.' + ext;
      if (activeDevice && activeDevice.drive && typeof activeDevice.drive.writeFileChunked === 'function') {
        const base64 = arrayBufferToBase64(m.bytes);
        await activeDevice.drive.writeFileChunked(target, base64);
        console.log('[DeckCraftEditor] saved to', target, m.bytes.byteLength, 'bytes');
        dispatch('save', { filename: name, filePath: target, bytes: m.bytes });
      } else {
        throw new Error('No device drive to write to');
      }
    } catch (err) {
      console.error('[DeckCraftEditor] save failed:', err);
      alert('Failed to save ' + (m.name || filename) + ': ' + ((err && err.message) || err));
    }
  }

  function onMessage(e) {
    const m = e.data;
    if (!m || typeof m !== 'object' || typeof m.type !== 'string' || !m.type.startsWith('deckcraft:')) return;
    if (e.source !== frame?.contentWindow) return;
    switch (m.type) {
      case 'deckcraft:ready':
        postOpen();
        break;
      case 'deckcraft:save':
        handleBridgeSave(m);
        break;
      case 'deckcraft:dirty':
        break;
      case 'deckcraft:docClosed':
        // File › Close inside the app: close the OMD viewer.
        dispatch('close');
        break;
      case 'deckcraft:failed':
        // The app couldn't start (WebGPU/WebGL2): the host falls back to a download.
        dispatch('failed', { name: filename, path: filePath, error: m.error });
        break;
    }
  }

  onMount(() => {
    window.addEventListener('message', onMessage);
  });
  onDestroy(() => {
    window.removeEventListener('message', onMessage);
  });
</script>

<div class="dc-wrap">
  <iframe
    bind:this={frame}
    src={DC_BASE + 'index.html?host=parent'}
    title={filename}
    class="dc-frame"
  ></iframe>
</div>

<style>
  /* The viewer container stretches every direct child to 100%x100%: keep the
     iframe and the floating close inside one positioned wrapper. */
  .dc-wrap {
    position: relative;
    width: 100%;
    height: 100%;
  }
  .dc-frame {
    display: block;
    width: 100%;
    height: 100%;
    border: 0;
    background: #e6e6e6;
  }
  /* Native-style OMD modal (no browser confirm() popups) */
</style>
