<script>
  import { onMount, onDestroy, createEventDispatcher } from 'svelte';
  import { _ } from '../lib/i18n/index.js';

  const dispatch = createEventDispatcher();

  let videoElement;
  let canvasElement;
  let stream = null;
  let scanInterval = null;
  let isScanning = true;
  let cameraError = "";
  let isTorchOn = false;
  let hasTorch = false;
  let availableCameras = [];
  let currentCameraIndex = 0;
  let jsQrLoaded = false;
  let isLoadingCamera = true;

  onMount(async () => {
    // Check for native BarcodeDetector or load jsQR as fallback
    if (!('BarcodeDetector' in window)) {
      loadJsQrFallback();
    }
    await initCamera();
  });

  onDestroy(() => {
    stopCamera();
  });

  function loadJsQrFallback() {
    if (window.jsQR) {
      jsQrLoaded = true;
      return;
    }
    const script = document.createElement('script');
    script.src = 'https://cdn.jsdelivr.net/npm/jsqr@1.4.0/dist/jsQR.min.js';
    script.async = true;
    script.onload = () => {
      jsQrLoaded = true;
      console.log('[QrScanner] jsQR fallback library loaded');
    };
    script.onerror = () => {
      console.warn('[QrScanner] Failed to load jsQR library');
    };
    document.head.appendChild(script);
  }

  let selectedDeviceId = null;

  async function getCameras() {
    try {
      if (!navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices) return [];
      const devices = await navigator.mediaDevices.enumerateDevices();
      return devices.filter(d => d.kind === 'videoinput' && d.deviceId);
    } catch (_) {
      return [];
    }
  }

  async function initCamera() {
    isLoadingCamera = true;
    cameraError = "";
    stopCamera();

    const constraintOptions = [];

    // 1. User-selected specific device
    if (selectedDeviceId) {
      constraintOptions.push({
        video: { deviceId: selectedDeviceId },
        audio: false
      });
    }

    // 2. Ideal environment (back) camera with preferred resolution
    constraintOptions.push({
      video: {
        facingMode: { ideal: "environment" },
        width: { ideal: 1280 },
        height: { ideal: 720 }
      },
      audio: false
    });

    // 3. Simple environment facingMode
    constraintOptions.push({
      video: { facingMode: "environment" },
      audio: false
    });

    // 4. Ultimate fallback: any available video input
    constraintOptions.push({
      video: true,
      audio: false
    });

    let lastErr = null;
    for (const constraints of constraintOptions) {
      try {
        stream = await navigator.mediaDevices.getUserMedia(constraints);
        if (stream) break;
      } catch (e) {
        lastErr = e;
        console.warn('[QrScanner] Constraints attempt failed, trying next:', constraints, e && (e.message || e.name));
      }
    }

    if (stream && videoElement) {
      try {
        videoElement.srcObject = stream;
        await videoElement.play();
        isLoadingCamera = false;
        checkTorchCapability();
        
        // Enumerate devices now that permission is granted
        try {
          availableCameras = await getCameras();
        } catch (_) {}

        startScanLoop();
        return;
      } catch (err) {
        lastErr = err;
      }
    }

    isLoadingCamera = false;
    console.error('[QrScanner] Camera access error:', lastErr);
    if (lastErr) {
      if (lastErr.name === 'NotAllowedError' || lastErr.name === 'PermissionDeniedError') {
        cameraError = 'Доступ к камере запрещён. Разрешите доступ в настройках браузера для сканирования QR-кода.';
      } else if (lastErr.name === 'NotFoundError' || lastErr.name === 'DevicesNotFoundError') {
        cameraError = 'Камера не найдена на этом устройстве.';
      } else {
        cameraError = 'Не удалось запустить камеру: ' + (lastErr.message || lastErr.name);
      }
    }
  }

  function checkTorchCapability() {
    if (!stream) return;
    const track = stream.getVideoTracks()[0];
    if (track && typeof track.getCapabilities === 'function') {
      const caps = track.getCapabilities();
      hasTorch = !!caps.torch;
    } else {
      hasTorch = false;
    }
  }

  async function toggleTorch() {
    if (!stream) return;
    const track = stream.getVideoTracks()[0];
    if (track && typeof track.applyConstraints === 'function') {
      try {
        isTorchOn = !isTorchOn;
        await track.applyConstraints({
          advanced: [{ torch: isTorchOn }]
        });
      } catch (e) {
        console.warn('[QrScanner] Torch error:', e);
        isTorchOn = false;
      }
    }
  }

  async function switchCamera() {
    if (availableCameras.length < 2) return;
    currentCameraIndex = (currentCameraIndex + 1) % availableCameras.length;
    selectedDeviceId = availableCameras[currentCameraIndex].deviceId;
    await initCamera();
  }

  function stopCamera() {
    if (scanInterval) {
      clearInterval(scanInterval);
      scanInterval = null;
    }
    if (stream) {
      stream.getTracks().forEach(track => track.stop());
      stream = null;
    }
  }

  function startScanLoop() {
    if (scanInterval) clearInterval(scanInterval);

    let barcodeDetector = null;
    if ('BarcodeDetector' in window) {
      try {
        barcodeDetector = new window.BarcodeDetector({ formats: ['qr_code'] });
      } catch (_) {
        barcodeDetector = null;
      }
    }

    scanInterval = setInterval(async () => {
      if (!videoElement || videoElement.readyState < 2 || !isScanning) return;

      // 1. Try native BarcodeDetector API (fastest & hardware accelerated)
      if (barcodeDetector) {
        try {
          const barcodes = await barcodeDetector.detect(videoElement);
          if (barcodes && barcodes.length > 0 && barcodes[0].rawValue) {
            handleScanSuccess(barcodes[0].rawValue);
            return;
          }
        } catch (_) {}
      }

      // 2. Try jsQR canvas fallback
      if (window.jsQR && canvasElement) {
        try {
          const ctx = canvasElement.getContext('2d', { willReadFrequently: true });
          const w = videoElement.videoWidth;
          const h = videoElement.videoHeight;
          if (w && h) {
            canvasElement.width = w;
            canvasElement.height = h;
            ctx.drawImage(videoElement, 0, 0, w, h);
            const imgData = ctx.getImageData(0, 0, w, h);
            const code = window.jsQR(imgData.data, imgData.width, imgData.height, {
              inversionAttempts: "dontInvert"
            });
            if (code && code.data) {
              handleScanSuccess(code.data);
              return;
            }
          }
        } catch (_) {}
      }
    }, 150);
  }

  function handleScanSuccess(raw) {
    if (!isScanning) return;
    isScanning = false;
    stopCamera();

    // Haptic feedback if supported on mobile
    if (typeof navigator !== 'undefined' && navigator.vibrate) {
      try { navigator.vibrate(60); } catch (_) {}
    }

    let linkId = "";
    let name = "";

    try {
      if (raw.includes("claimLink=") || raw.startsWith("http://") || raw.startsWith("https://")) {
        const parsedUrl = new URL(raw, window.location.origin);
        linkId = parsedUrl.searchParams.get("claimLink") || "";
        name = parsedUrl.searchParams.get("name") || "";
      } else if (raw.includes("claimLink=")) {
        const match = raw.match(/claimLink=([^&]+)/);
        if (match) linkId = decodeURIComponent(match[1]);
        const nameMatch = raw.match(/name=([^&]+)/);
        if (nameMatch) name = decodeURIComponent(nameMatch[1]);
      } else {
        linkId = raw.trim();
      }
    } catch (_) {
      linkId = raw.trim();
    }

    dispatch('scan', { raw, linkId, name });
  }

  function handleClose() {
    stopCamera();
    dispatch('close');
  }
</script>

<div class="qr-scanner-backdrop" on:click|self={handleClose}>
  <div class="qr-scanner-modal">
    <div class="qr-scanner-header">
      <div class="qr-header-title">
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
          <rect x="3" y="3" width="7" height="7"></rect>
          <rect x="14" y="3" width="7" height="7"></rect>
          <rect x="14" y="14" width="7" height="7"></rect>
          <rect x="3" y="14" width="7" height="7"></rect>
        </svg>
        <span>{$_('scan_qr_title', {}, 'Scan QR Code')}</span>
      </div>
      <button type="button" class="qr-close-btn" on:click={handleClose} title={$_('close', {}, 'Close')}>
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.5">
          <line x1="18" y1="6" x2="6" y2="18"></line>
          <line x1="6" y1="6" x2="18" y2="18"></line>
        </svg>
      </button>
    </div>

    <div class="qr-scanner-viewport">
      {#if cameraError}
        <div class="qr-scanner-error">
          <svg viewBox="0 0 24 24" width="40" height="40" fill="none" stroke="#f87171" stroke-width="2">
            <circle cx="12" cy="12" r="10"></circle>
            <line x1="12" y1="8" x2="12" y2="12"></line>
            <line x1="12" y1="16" x2="12.01" y2="16"></line>
          </svg>
          <p>{cameraError}</p>
          <button type="button" class="qr-retry-btn" on:click={initCamera}>{$_('retry', {}, 'Retry')}</button>
        </div>
      {:else}
        <!-- svelte-ignore a11y_media_has_caption -->
        <video bind:this={videoElement} playsinline muted class="qr-video"></video>
        <canvas bind:this={canvasElement} style="display: none;"></canvas>

        {#if isLoadingCamera}
          <div class="qr-loading-overlay">
            <div class="qr-spinner"></div>
            <span>{$_('loading', {}, 'Loading...')}</span>
          </div>
        {/if}

        <div class="qr-overlay-frame">
          <div class="qr-reticle">
            <div class="reticle-corner top-left"></div>
            <div class="reticle-corner top-right"></div>
            <div class="reticle-corner bottom-left"></div>
            <div class="reticle-corner bottom-right"></div>
            <div class="qr-scan-line"></div>
          </div>
        </div>
      {/if}
    </div>

    <div class="qr-scanner-footer">
      <p class="qr-scanner-hint">{$_('scan_qr_hint', {}, 'Point your camera at the QR code on the device screen or Claim Page')}</p>
      
      <div class="qr-controls">
        {#if hasTorch}
          <button type="button" class="qr-control-btn" class:active={isTorchOn} on:click={toggleTorch} title={$_('torch', {}, 'Flashlight')}>
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M18 6l-6 6-6-6"></path>
              <line x1="12" y1="12" x2="12" y2="21"></line>
            </svg>
            <span>{isTorchOn ? $_('torch_off', {}, 'Off') : $_('torch', {}, 'Flashlight')}</span>
          </button>
        {/if}

        {#if availableCameras.length > 1}
          <button type="button" class="qr-control-btn" on:click={switchCamera} title={$_('switch_camera', {}, 'Switch Camera')}>
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M23 4v6h-6M1 20v-6h6M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"></path>
            </svg>
            <span>{$_('switch_camera', {}, 'Camera')}</span>
          </button>
        {/if}
      </div>
    </div>
  </div>
</div>

<style>
  .qr-scanner-backdrop {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(15, 23, 42, 0.85);
    backdrop-filter: blur(8px);
    z-index: 100000;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px;
    box-sizing: border-box;
    animation: fadeIn 0.2s ease-out;
  }

  .qr-scanner-modal {
    background: #1e293b;
    border: 1px solid #334155;
    border-radius: 16px;
    width: 100%;
    max-width: 440px;
    display: flex;
    flex-direction: column;
    overflow: hidden;
    box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.7);
  }

  .qr-scanner-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 16px 20px;
    border-bottom: 1px solid #334155;
    background: #0f172a;
  }

  .qr-header-title {
    display: flex;
    align-items: center;
    gap: 10px;
    color: #f8fafc;
    font-size: 16px;
    font-weight: 600;
  }

  .qr-header-title svg {
    color: #38bdf8;
  }

  .qr-close-btn {
    background: none;
    border: none;
    color: #94a3b8;
    cursor: pointer;
    padding: 6px;
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: color 0.15s, background 0.15s;
  }

  .qr-close-btn:hover {
    color: #f8fafc;
    background: rgba(255, 255, 255, 0.08);
  }

  .qr-scanner-viewport {
    position: relative;
    width: 100%;
    height: 320px;
    background: #090d16;
    overflow: hidden;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .qr-video {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }

  .qr-loading-overlay {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(15, 23, 42, 0.9);
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 12px;
    color: #94a3b8;
    font-size: 14px;
  }

  .qr-spinner {
    width: 32px;
    height: 32px;
    border: 3px solid rgba(56, 189, 248, 0.2);
    border-top-color: #38bdf8;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  .qr-overlay-frame {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    pointer-events: none;
  }

  .qr-reticle {
    position: relative;
    width: 210px;
    height: 210px;
    border-radius: 12px;
  }

  .reticle-corner {
    position: absolute;
    width: 28px;
    height: 28px;
    border-color: #38bdf8;
    border-style: solid;
    border-width: 0;
  }

  .reticle-corner.top-left {
    top: 0;
    left: 0;
    border-top-width: 3px;
    border-left-width: 3px;
    border-top-left-radius: 10px;
  }

  .reticle-corner.top-right {
    top: 0;
    right: 0;
    border-top-width: 3px;
    border-right-width: 3px;
    border-top-right-radius: 10px;
  }

  .reticle-corner.bottom-left {
    bottom: 0;
    left: 0;
    border-bottom-width: 3px;
    border-left-width: 3px;
    border-bottom-left-radius: 10px;
  }

  .reticle-corner.bottom-right {
    bottom: 0;
    right: 0;
    border-bottom-width: 3px;
    border-right-width: 3px;
    border-bottom-right-radius: 10px;
  }

  .qr-scan-line {
    position: absolute;
    top: 0;
    left: 4px;
    right: 4px;
    height: 2px;
    background: linear-gradient(90deg, transparent, #38bdf8, transparent);
    box-shadow: 0 0 8px #38bdf8;
    animation: scanBeam 2s ease-in-out infinite;
  }

  @keyframes scanBeam {
    0%, 100% { top: 6px; }
    50% { top: calc(100% - 8px); }
  }

  .qr-scanner-error {
    padding: 24px;
    text-align: center;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 12px;
    color: #cbd5e1;
    font-size: 14px;
    line-height: 1.5;
  }

  .qr-retry-btn {
    margin-top: 6px;
    background: #38bdf8;
    color: #0f172a;
    border: none;
    border-radius: 8px;
    padding: 8px 16px;
    font-weight: 600;
    font-size: 13px;
    cursor: pointer;
  }

  .qr-scanner-footer {
    padding: 16px 20px;
    background: #0f172a;
    border-top: 1px solid #334155;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 12px;
  }

  .qr-scanner-hint {
    margin: 0;
    font-size: 13px;
    color: #94a3b8;
    text-align: center;
    line-height: 1.4;
  }

  .qr-controls {
    display: flex;
    gap: 10px;
  }

  .qr-control-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    background: #1e293b;
    border: 1px solid #334155;
    color: #e2e8f0;
    padding: 7px 12px;
    border-radius: 8px;
    font-size: 12px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s;
  }

  .qr-control-btn:hover {
    background: #334155;
    border-color: #475569;
  }

  .qr-control-btn.active {
    background: rgba(56, 189, 248, 0.15);
    border-color: #38bdf8;
    color: #38bdf8;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  @keyframes fadeIn {
    from { opacity: 0; transform: scale(0.96); }
    to { opacity: 1; transform: scale(1); }
  }
</style>
