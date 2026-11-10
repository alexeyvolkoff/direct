<script>
  /**
   * WordCraftEditor — documents (docx/doc/odt/rtf/...) edited by WordCraft WASM
   * (a fork of wordcraft-web with the OMD parent bridge) inside a same-origin
   * iframe. The original file travels in, saved bytes travel back and are
   * written to the ORIGINAL document path (no .univer.json sidecars).
   *
   * The editor renders ITS OWN full UI (ribbon + Quick Access Toolbar): on ready
   * we add file.close to its QAT so closing lives in their panel, not in a
   * second OMD toolbar. Saving uses the built-in Save (QAT/Ctrl+S) which goes
   * through the bridge's download slot; closing reports `docClosed`.
   *
   * Bridge protocol (see wordcraft repo apps/wordcraft-web/src/web/bridge.rs):
   *   host → editor: {wc:"open", name, bytes}, {wc:"save-ack", ticket, ok, error?},
   *                  {wc:"cmd", id, params?}
   *   editor → host: {wc:"ready"}, {wc:"failed", error}, {wc:"dirty", dirty},
   *                  {wc:"save", ticket, name, bytes}, {wc:"status", message},
   *                  {wc:"response", ok, value?}, {wc:"docClosed"}
   */
  import { onMount, onDestroy, createEventDispatcher } from 'svelte';

  export let filename = 'document.docx';
  export let filePath = '';
  export let bytes = null; // ArrayBuffer of the ORIGINAL file
  export let activeDevice = null;
  export let isDarkTheme = false;

  const dispatch = createEventDispatcher();

  // Lazy path served from public/ (same pattern as /pdfjs). Version-pinned fork.
  const WC_BASE = '/wordcraft/v0.7.0/';

  let frame;
  let wcReady = false;
  let saving = false;
  let lastTheme = null;

  function cleanDevicePath(p) {
    if (!p || p === '~') return '/';
    let devPath = p;
    if (devPath.startsWith('~/')) devPath = '/' + devPath.slice(2);
    else if (!devPath.startsWith('/')) devPath = '/' + devPath;
    return devPath;
  }

  // Chunked so multi-MB documents don't blow the call stack.
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
    post({ wc: 'cmd', id, params: JSON.stringify(params || {}) });
  }

  function postOpen() {
    wcReady = true;
    if (bytes) {
      // Theme first so the first paint is already dark/light.
      sendCmd('ui.dark', { value: !!isDarkTheme });
      lastTheme = !!isDarkTheme;
      // No transfer list: keep our copy so the viewer can reopen the same file.
      post({ wc: 'open', name: filename, bytes });
      // Close lives in WordCraft's own Quick Access Toolbar (next to Save).
      sendCmd('tools.customizeRibbon', { qatAdd: 'file.close' });
    }
  }

  async function handleBridgeSave(e, m) {
    if (saving) return;
    saving = true;
    try {
      const name = m.name || filename;
      const ext = (name.split('.').pop() || '').toLowerCase();
      let target = cleanDevicePath(filePath || filename);
      // PDF exports land next to the original document.
      if (ext === 'pdf') target = target.replace(/\.[^/.]+$/, '') + '.pdf';
      if (activeDevice && activeDevice.drive && typeof activeDevice.drive.writeFileChunked === 'function') {
        const base64 = arrayBufferToBase64(m.bytes);
        await activeDevice.drive.writeFileChunked(target, base64);
        console.log('[WordCraftEditor] saved to', target, m.bytes.byteLength, 'bytes');
      } else {
        throw new Error('No device drive to write to');
      }
      e.source.postMessage({ wc: 'save-ack', ticket: m.ticket, ok: true }, window.location.origin);
      dispatch('save', { filename: name, filePath: target, bytes: m.bytes });
    } catch (err) {
      console.error('[WordCraftEditor] save failed:', err);
      e.source.postMessage({ wc: 'save-ack', ticket: m.ticket, ok: false, error: String((err && err.message) || err) }, window.location.origin);
    } finally {
      saving = false;
    }
  }

  function onMessage(e) {
    const m = e.data;
    if (!m || typeof m !== 'object' || m.wc === undefined) return;
    if (e.source !== frame?.contentWindow) return;
    switch (m.wc) {
      case 'ready':
        postOpen();
        break;
      case 'failed':
        // WebGPU/WebGL2 unavailable: the host reopens through the old Univer path.
        dispatch('failed', { name: filename, path: filePath });
        break;
      case 'save':
        handleBridgeSave(e, m);
        break;
      case 'docClosed':
        // File › Close (or a QAT button) inside WordCraft: close the viewer.
        dispatch('close');
        break;
      case 'response':
        if (m.ok === false) console.warn('[WordCraftEditor] cmd failed:', m.value);
        break;
    }
  }

  // Follow the app theme while the editor is open.
  $: if (wcReady && frame && lastTheme !== !!isDarkTheme) {
    lastTheme = !!isDarkTheme;
    sendCmd('ui.dark', { value: !!isDarkTheme });
  }

  onMount(() => {
    window.addEventListener('message', onMessage);
  });
  onDestroy(() => {
    window.removeEventListener('message', onMessage);
  });
</script>

<iframe
  bind:this={frame}
  src={WC_BASE + 'index.html'}
  title={filename}
  class="wc-frame"
></iframe>

<style>
  .wc-frame {
    display: block;
    width: 100%;
    height: 100%;
    border: 0;
    background: #e6e6e6;
  }
</style>
