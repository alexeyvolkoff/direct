<script>
  /**
   * GridCraftEditor — spreadsheets (xlsx/csv/tsv) edited by GridCraft WASM in an
   * iframe. GridCraft ships a built-in host bridge (`?host=parent`); our fork of
   * it adds `gridcraft:cmd` (run engine commands) with `gridcraft:response` and
   * `gridcraft:docClosed`. The fork also adds a Close button to GridCraft's own
   * title bar next to Save.
   *
   *   → parent: {type:"gridcraft:ready"}, {type:"gridcraft:save", name, bytes},
   *             {type:"gridcraft:dirty", dirty}, {type:"gridcraft:docClosed"},
   *             {type:"gridcraft:response", ok, value}, {type:"gridcraft:failed", error}
   *   ← app:    {type:"gridcraft:open", name, bytes}, {type:"gridcraft:cmd", id, params?}
   *
   * Close flow (no browser popups): Close lives in the app's own title bar —
   * `file.close` runs the app's own Save/Don't Save/Cancel dialog on unsaved
   * changes — and `docClosed` closes the OMD viewer. Saving goes through the
   * same bridge into the original document path. The theme follows the
   * browser's appearance.
   */
  import { onMount, onDestroy, createEventDispatcher } from 'svelte';

  export let filename = 'Book.xlsx';
  export let filePath = '';
  export let bytes = null; // ArrayBuffer of the ORIGINAL file
  export let activeDevice = null;

  const dispatch = createEventDispatcher();

  // Version-pinned fork build served from public/ (same pattern as /pdfjs).
  const GC_BASE = '/gridcraft/v0.7.0/';

  let frame;
  let gcReady = false;

  function cleanDevicePath(p) {
    if (!p || p === '~') return '/';
    let devPath = p;
    if (devPath.startsWith('~/')) devPath = '/' + devPath.slice(2);
    else if (!devPath.startsWith('/')) devPath = '/' + devPath;
    return devPath;
  }

  // Chunked so multi-MB workbooks don't blow the call stack.
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
    post({ type: 'gridcraft:cmd', id, params: JSON.stringify(params || {}) });
  }

  function postOpen() {
    gcReady = true;
    if (bytes) {
      post({ type: 'gridcraft:open', name: filename, bytes });
    }
  }

  async function handleBridgeSave(m) {
    try {
      const name = m.name || filename;
      const ext = (name.split('.').pop() || '').toLowerCase();
      let target = cleanDevicePath(filePath || filename);
      const origExt = ((filePath || filename).split('.').pop() || '').toLowerCase();
      // The app may save under a different format (CSV → xlsx on save, PDF
      // exports): keep the original directory, use the saved name's extension.
      if (ext && ext !== origExt) target = target.replace(/\.[^/.]+$/, '') + '.' + ext;
      if (activeDevice && activeDevice.drive && typeof activeDevice.drive.writeFileChunked === 'function') {
        const base64 = arrayBufferToBase64(m.bytes);
        await activeDevice.drive.writeFileChunked(target, base64);
        console.log('[GridCraftEditor] saved to', target, m.bytes.byteLength, 'bytes');
        dispatch('save', { filename: name, filePath: target, bytes: m.bytes });
      } else {
        throw new Error('No device drive to write to');
      }
    } catch (err) {
      console.error('[GridCraftEditor] save failed:', err);
      alert('Failed to save ' + (m.name || filename) + ': ' + ((err && err.message) || err));
    }
  }

  function onMessage(e) {
    const m = e.data;
    if (!m || typeof m !== 'object' || typeof m.type !== 'string' || !m.type.startsWith('gridcraft:')) return;
    if (e.source !== frame?.contentWindow) return;
    switch (m.type) {
      case 'gridcraft:ready':
        postOpen();
        break;
      case 'gridcraft:save':
        handleBridgeSave(m);
        break;
      case 'gridcraft:docClosed':
        // Close inside the app (the title bar button, File › Close): close the OMD viewer.
        dispatch('close');
        break;
      case 'gridcraft:response':
        if (m.ok === false) console.warn('[GridCraftEditor] cmd failed:', m.value);
        break;
      case 'gridcraft:failed':
        // The app couldn't start (WebGPU/WebGL2): the host reopens through Univer.
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

<div class="gc-wrap">
  <iframe
    bind:this={frame}
    src={GC_BASE + 'index.html?host=parent'}
    title={filename}
    class="gc-frame"
  ></iframe>
</div>

<style>
  /* The viewer container stretches every direct child to 100%x100%: keep the
     iframe inside this positioned wrapper. */
  .gc-wrap {
    position: relative;
    width: 100%;
    height: 100%;
  }
  .gc-frame {
    display: block;
    width: 100%;
    height: 100%;
    border: 0;
    background: #e6e6e6;
  }
</style>
