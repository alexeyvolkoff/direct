/**
 * Custom TikTok / Instagram style bottom pagination and navigation bar for LightGallery.
 *
 * - Disables floating arrows over the center of the image.
 * - Auto-hides on inactivity after 3.5 seconds; wakes up on mouse movement or single tap.
 * - For small photo sets (<= 15): shows interactive carousel dots with an active round dot.
 * - For large photo sets (> 15, e.g. thousands of camera photos): transforms into a smooth progress track with an active scrubber dot.
 * - Nav buttons (previous/next) are compact, integrated on both mobile and desktop.
 */

function escapeHtml(str) {
  if (!str) return '';
  return String(str)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

export function isJustFileName(str, ...paths) {
  if (!str) return true;
  const s = String(str).trim();
  if (!s) return true;
  // Generic MIME types like image/jpeg, image/png etc. are not custom descriptions
  if (/^(image|video|audio|application|text)\/[\w.+-]+$/i.test(s)) {
    return true;
  }
  for (const p of paths) {
    if (!p) continue;
    const base = String(p).split('/').pop().trim();
    if (s.toLowerCase() === base.toLowerCase() || s.toLowerCase() === String(p).trim().toLowerCase()) {
      return true;
    }
    const noExt = base.replace(/\.[^.]+$/, '');
    if (noExt && s.toLowerCase() === noExt.toLowerCase()) {
      return true;
    }
  }
  if (/^[\w,\s-]+\.(jpe?g|png|webp|gif|svg|bmp|tiff?|heic|mp4|mov|webm)$/i.test(s)) {
    return true;
  }
  return false;
}

/**
 * Extract custom title and description text from a gallery item.
 * Understands both pre-built `.omd-gallery-caption` HTML (item.subHtml)
 * and raw customAttributes (title / description / metaTitle / metaComment).
 * File-name-like strings are filtered out.
 * @returns {{title: string, desc: string}}
 */
export function getGalleryCaptionParts(f) {
  if (!f) return { title: '', desc: '' };
  const fn = (typeof f === 'object' ? (f.value || f.id || f.name || '') : '');

  // If already an HTML string or object with subHtml, validate that it contains real custom text
  let rawHtml = '';
  if (typeof f === 'string' && f.includes('omd-gallery-caption')) {
    rawHtml = f;
  } else if (f.subHtml && typeof f.subHtml === 'string' && f.subHtml.includes('omd-gallery-caption')) {
    rawHtml = f.subHtml;
  }

  if (rawHtml) {
    const div = document.createElement('div');
    div.innerHTML = rawHtml;
    const h4Text = div.querySelector('h4')?.textContent?.trim() || '';
    const pText = div.querySelector('p')?.textContent?.trim() || '';
    const validTitle = h4Text && !isJustFileName(h4Text, fn);
    const validDesc = pText && !isJustFileName(pText, fn);
    return {
      title: validTitle ? h4Text : '',
      desc: validDesc ? pText : ''
    };
  }

  const ca = f.customAttributes || f.attributes || {};
  const hasCustomTitle = ca.customtitle === true || ca.customTitle === true || ca.customtitle === 'true' || ca.customtitle === 1;
  const hasCustomDescr = ca.customdescr === true || ca.customDescr === true || ca.longdescr === true || ca.customdescr === 'true' || ca.customdescr === 1;

  let title = '';
  if (hasCustomTitle && ca.title) {
    title = String(ca.title).replace(/^#+\s*/, '').trim();
  } else if (ca.metaTitle) {
    title = String(ca.metaTitle).replace(/^#+\s*/, '').trim();
  } else if (f.title && !hasCustomTitle && !ca.title) {
    const cand = String(f.title).replace(/^#+\s*/, '').trim();
    if (!isJustFileName(cand, fn)) title = cand;
  }

  let desc = '';
  if (hasCustomDescr && ca.description) {
    desc = String(ca.description).trim();
  } else if (ca.metaComment) {
    desc = String(ca.metaComment).trim();
  } else if (f.description && !hasCustomDescr && !ca.description) {
    const cand = String(f.description).trim();
    if (!isJustFileName(cand, fn)) desc = cand;
  } else if (f.annotation) {
    const cand = String(f.annotation).trim();
    if (!isJustFileName(cand, fn)) desc = cand;
  }

  if (isJustFileName(title, fn)) title = '';
  if (isJustFileName(desc, fn)) desc = '';

  return { title, desc };
}

export function getGalleryCaption(f) {
  const { title, desc } = getGalleryCaptionParts(f);
  if (title && desc) {
    return `<div class="omd-gallery-caption"><h4>${escapeHtml(title)}</h4><p>${escapeHtml(desc)}</p></div>`;
  } else if (title) {
    return `<div class="omd-gallery-caption"><h4>${escapeHtml(title)}</h4></div>`;
  } else if (desc) {
    return `<div class="omd-gallery-caption"><p>${escapeHtml(desc)}</p></div>`;
  }
  return '';
}

const SVG_FS_ENTER = '<svg viewBox="0 0 24 24" width="20" height="20" fill="#ffffff"><path fill="#ffffff" d="M7 14H5v5h5v-2H7v-3zm-2-4h2V7h3V5H5v5zm12 7h-3v2h5v-5h-2v3zM14 5v2h3v3h2V5h-5z"/></svg>';
const SVG_FS_EXIT = '<svg viewBox="0 0 24 24" width="20" height="20" fill="#ffffff"><path fill="#ffffff" d="M5 16h3v3h2v-5H5v2zm3-8H5v2h5V5H8v3zm6 11h2v-3h3v-2h-5v5zm2-11V5h-2v5h5V8h-3z"/></svg>';
const SVG_INFO = '<svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor"><path fill="currentColor" d="M11 7h2v2h-2V7zm0 4h2v6h-2v-6zm1-9a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm0 18a8 8 0 1 1 0-16 8 8 0 0 1 0 16z"/></svg>';
const SVG_PLATE_CLOSE = '<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><line x1="6" y1="6" x2="18" y2="18"/><line x1="18" y1="6" x2="6" y2="18"/></svg>';

function isFullscreenActive() {
  return !!(
    document.fullscreenElement ||
    document.webkitFullscreenElement ||
    document.mozFullScreenElement ||
    document.msFullscreenElement
  );
}

function isMobileGallery() {
  return typeof window !== 'undefined' && (window.innerWidth <= 768 || ('ontouchstart' in window && window.innerWidth <= 1024));
}

function toggleGalleryFullscreen() {
  try {
    if (!isFullscreenActive()) {
      const docEl = document.documentElement;
      const req = docEl.requestFullscreen || docEl.webkitRequestFullscreen || docEl.mozRequestFullScreen || docEl.msRequestFullscreen;
      if (req) {
        req.call(docEl).catch((err) => {
          console.warn("[Gallery] Fullscreen request error:", err);
        });
      }
    } else {
      const exit = document.exitFullscreen || document.webkitExitFullscreen || document.mozCancelFullScreen || document.msExitFullscreen;
      if (exit) {
        exit.call(document).catch((err) => {
          console.warn("[Gallery] Exit fullscreen error:", err);
        });
      }
    }
  } catch (err) {
    console.error("[Gallery] Fullscreen toggle error:", err);
  }
}

export function setupGalleryPagination(lgEl, itemsOrCount, initialIndex = 0) {
  let items = Array.isArray(itemsOrCount) ? itemsOrCount : null;
  let totalCount = items ? items.length : (typeof itemsOrCount === 'number' ? itemsOrCount : 0);
  let lgInst = null;
  let hideTimer = null;
  let isBarHidden = false;
  let isOpen = false;
  // Info plate state: on mobile the plate is only shown via the toolbar
  // "info" button (isPlateForced); on desktop it auto-shows with the nav
  // pill, unless the user closed it manually (isPlateDismissed) — it comes
  // back automatically on the next slide with a caption.
  let isPlateForced = false;
  let isPlateDismissed = false;
  const isLarge = totalCount > 15;

  function showBar() {
    isBarHidden = false;
    const bar = document.querySelector('.omd-gallery-bottom-bar');
    if (bar) bar.classList.remove('is-hidden');
    if (!isMobileGallery()) {
      const plate = document.querySelector('.omd-gallery-info-plate');
      if (plate && !plate.classList.contains('is-empty') && !isPlateDismissed) {
        plate.classList.remove('is-hidden');
      }
      updateInfoButtonState();
    }
    resetInactivityTimer();
  }

  function hideBar() {
    isBarHidden = true;
    const bar = document.querySelector('.omd-gallery-bottom-bar');
    if (bar) bar.classList.add('is-hidden');
    if (!isMobileGallery()) {
      const plate = document.querySelector('.omd-gallery-info-plate');
      if (plate) plate.classList.add('is-hidden');
      updateInfoButtonState();
    }
  }

  function toggleBar() {
    if (isBarHidden) {
      showBar();
    } else {
      hideBar();
    }
  }

  function resetInactivityTimer() {
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => {
      hideBar();
    }, 3500);
  }

  const activityHandler = (e) => {
    // If click or interaction was inside close button, close immediately and prevent toggle
    if (e.target && e.target.closest && e.target.closest('.lg-close')) {
      if (e.type === 'click' || e.type === 'touchend') {
        try { e.preventDefault(); } catch (_) {}
        try { e.stopPropagation(); } catch (_) {}
        clearTimeout(hideTimer);
        const bars = document.querySelectorAll('.omd-gallery-bottom-bar');
        bars.forEach((b) => b.remove());
        if (lgInst && typeof lgInst.closeGallery === 'function') {
          lgInst.closeGallery(true);
        }
      }
      return;
    }

    // If interaction was inside bottom bar, info plate, download toast, toolbar, or any control button/link, keep bar alive without toggle
    if (e.target && e.target.closest && e.target.closest('.omd-gallery-bottom-bar, .omd-gallery-info-plate, .omd-gallery-download-toast, .lg-toolbar, .omd-gallery-fs-btn, .omd-gallery-info-btn, button, a')) {
      resetInactivityTimer();
      return;
    }

    if (e.type === 'click') {
      toggleBar();
    } else {
      showBar();
    }
  };

  function updateFsButtonState() {
    const btn = document.querySelector('.omd-gallery-fs-btn, .lg-toolbar .lg-fullscreen');
    if (btn) {
      const isFs = isFullscreenActive();
      btn.innerHTML = isFs ? SVG_FS_EXIT : SVG_FS_ENTER;
      btn.setAttribute('title', isFs ? 'Exit Fullscreen' : 'Fullscreen');
      btn.setAttribute('aria-label', isFs ? 'Exit Fullscreen' : 'Fullscreen');
    }
  }

  function updateInfoButtonState() {
    const btn = document.querySelector('.lg-toolbar .omd-gallery-info-btn');
    if (!btn) return;
    const plate = document.querySelector('.omd-gallery-info-plate');
    const empty = !plate || plate.classList.contains('is-empty');
    // Hide the button entirely when the photo has no title/description
    btn.classList.toggle('is-empty-state', !!empty);
    // Mutually exclusive visibility: while the info plate is shown, the
    // button is hidden — it re-appears to re-open the auto-hidden plate.
    const plateShown = !empty && !!plate && !plate.classList.contains('is-hidden');
    btn.classList.toggle('is-plate-shown', !!plateShown);
  }

  // Shared download progress toast (top-center bar inside the viewer)
  function ensureDownloadToast(outer) {
    let toast = document.querySelector('.omd-gallery-download-toast');
    document.querySelectorAll('.omd-gallery-download-toast').forEach((el) => { if (el !== toast) el.remove(); });
    if (!toast || !toast.parentNode || toast.parentNode !== outer) {
      toast = document.createElement('div');
      toast.className = 'omd-gallery-download-toast';
      toast.innerHTML = `
        <div class="omd-dl-row">
          <span class="omd-dl-label"></span>
          <span class="omd-dl-pct"></span>
        </div>
        <div class="omd-dl-track"><div class="omd-dl-fill"></div></div>
      `;
      outer.appendChild(toast);
    }
    return toast;
  }

  function updateDownloadToast(fileName, loaded, total) {
    const container = document.querySelector('.lg-container.lg-show');
    const outer = container ? container.querySelector('.lg-outer') : null;
    if (!outer) return;
    const toast = ensureDownloadToast(outer);
    const label = toast.querySelector('.omd-dl-label');
    const pct = toast.querySelector('.omd-dl-pct');
    const fill = toast.querySelector('.omd-dl-fill');
    if (label) label.textContent = fileName || '';
    toast.classList.remove('is-done', 'is-failed');
    if (total > 0) {
      toast.classList.remove('indeterminate');
      const percent = Math.min(100, Math.round((loaded / total) * 100));
      if (pct) pct.textContent = `${percent}%`;
      if (fill) fill.style.width = `${percent}%`;
    } else {
      toast.classList.add('indeterminate');
      if (pct) pct.textContent = '';
    }
  }

  function finishDownloadToast(ok, fileName) {
    const toast = document.querySelector('.omd-gallery-download-toast');
    if (!toast) return;
    toast.classList.toggle('is-done', !!ok);
    toast.classList.toggle('is-failed', !ok);
    toast.classList.remove('indeterminate');
    const label = toast.querySelector('.omd-dl-label');
    const pct = toast.querySelector('.omd-dl-pct');
    const fill = toast.querySelector('.omd-dl-fill');
    if (label) label.textContent = ok ? (`Saved` + (fileName ? `: ${fileName}` : '')) : 'Download failed';
    if (pct) pct.textContent = ok ? '100%' : '';
    if (fill) fill.style.width = ok ? '100%' : '0%';
    setTimeout(() => {
      const alive = document.querySelector('.omd-gallery-download-toast');
      if (alive) {
        alive.classList.add('is-hidden');
        setTimeout(() => { if (alive.parentNode) alive.remove(); }, 400);
      }
    }, ok ? 1400 : 2500);
  }

  // Creates/updates the info plate (title + description) in the bottom-right
  // corner of the viewer. Completely separate from the nav pill.
  function updatePlate(parts) {
    const container = document.querySelector('.lg-container.lg-show');
    const outer = container ? container.querySelector('.lg-outer') : null;
    if (!outer) return;

    let plate = document.querySelector('.omd-gallery-info-plate');
    // Remove plates left from previous/closed galleries
    document.querySelectorAll('.omd-gallery-info-plate').forEach((el) => { if (el !== plate) el.remove(); });

    if (!plate || !plate.parentNode) {
      plate = document.createElement('div');
      plate.className = 'omd-gallery-info-plate';
      plate.setAttribute('role', 'complementary');
      plate.setAttribute('aria-label', 'Photo info');
      outer.appendChild(plate);
    }

    const title = (parts && parts.title) || '';
    const desc = (parts && parts.desc) || '';
    const hasContent = !!(title || desc);

    plate.classList.toggle('no-title', !title);
    plate.classList.toggle('is-empty', !hasContent);

    if (hasContent) {
      plate.innerHTML = `
        <div class="omd-info-header">
          <h4 class="omd-info-title">${escapeHtml(title)}</h4>
          <button type="button" class="omd-info-close" title="Close" aria-label="Close">${SVG_PLATE_CLOSE}</button>
        </div>
        ${desc ? `<p class="omd-info-desc">${escapeHtml(desc)}</p>` : ''}
      `;
      const closeBtn = plate.querySelector('.omd-info-close');
      if (closeBtn) {
        closeBtn.onclick = (e) => {
          e.preventDefault();
          e.stopPropagation();
          hidePlateManually();
        };
      }
    } else {
      plate.innerHTML = '';
    }

    // Visibility
    if (!hasContent) {
      plate.classList.add('is-hidden');
    } else if (isMobileGallery()) {
      // Mobile: hidden unless the user opened it via the toolbar info button
      plate.classList.toggle('is-hidden', !isPlateForced);
    } else {
      // Desktop: new slide re-enables auto-show
      isPlateDismissed = false;
      plate.classList.remove('is-hidden');
    }
    updateInfoButtonState();
  }

  function hidePlateManually() {
    const plate = document.querySelector('.omd-gallery-info-plate');
    if (plate) plate.classList.add('is-hidden');
    if (isMobileGallery()) {
      isPlateForced = false;
    } else {
      isPlateDismissed = true;
    }
    updateInfoButtonState();
  }

  function togglePlate() {
    const plate = document.querySelector('.omd-gallery-info-plate');
    if (!plate || plate.classList.contains('is-empty')) return;
    if (isMobileGallery()) {
      isPlateForced = !isPlateForced;
      plate.classList.toggle('is-hidden', !isPlateForced);
    } else {
      // Desktop: true toggle — open if hidden, hide if visible
      if (plate.classList.contains('is-hidden')) {
        isPlateDismissed = false;
        plate.classList.remove('is-hidden');
        resetInactivityTimer();
      } else {
        isPlateDismissed = true;
        plate.classList.add('is-hidden');
      }
    }
    updateInfoButtonState();
  }

  function removeInfoPlates() {
    document.querySelectorAll('.omd-gallery-info-plate').forEach((p) => p.remove());
    document.querySelectorAll('.omd-gallery-download-toast').forEach((p) => p.remove());
  }

  function getCurrentGalleryItem() {
    const idx = (lgInst && typeof lgInst.index === 'number') ? lgInst.index : initialIndex;
    // items[] (our dynamicEl) keeps the original source object (`f`);
    // lightgallery's own galleryItems strips unknown props.
    return (items && items[idx]) || (lgInst && lgInst.galleryItems && lgInst.galleryItems[idx]) || null;
  }

  // Downloads the ORIGINAL photo via its /p2p/... URL (item.downloadUrl),
  // with a visible progress bar at the top so repeated clicks can't start
  // multiple downloads. Validates the response so an HTML error page is
  // never saved as a photo; falls back to a plain <a download> if the
  // fetch/gateway fails.
  let isDownloadingItem = false;

  async function downloadCurrentItem() {
    if (isDownloadingItem) return; // one download at a time
    const it = getCurrentGalleryItem();
    if (!it) return;
    const url = it.downloadUrl || it.src;
    if (!url || url === false) return;
    const src = it.f || {};
    const fallbackName = (() => {
      try {
        return decodeURIComponent((String(url).split('?')[0].split('/').pop() || '')) || 'file';
      } catch (_) {
        return 'file';
      }
    })();
    const fileName = (src && (src.name || src.value)) || it.name ||
      (typeof it.download === 'string' && it.download) || fallbackName;
    const saveBlob = (blob, name) => {
      const blobUrl = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = blobUrl;
      a.download = name;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      setTimeout(() => URL.revokeObjectURL(blobUrl), 10000);
    };

    isDownloadingItem = true;
    updateDownloadToast(fileName, 0, 0);
    let ok = false;
    try {
      const res = await fetch(url);
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const contentType = (res.headers.get('Content-Type') || '').toLowerCase();
      // Never save an HTML error / app page instead of the original photo
      if (contentType.includes('text/html')) {
        throw new Error('Gateway returned HTML instead of a file');
      }
      const totalSize = Number(res.headers.get('Content-Length')) || 0;
      let blob = null;
      if (res.body && typeof res.body.getReader === 'function') {
        const reader = res.body.getReader();
        const parts = [];
        let loaded = 0;
        updateDownloadToast(fileName, 0, totalSize);
        while (true) {
          const step = await reader.read();
          if (step.done) break;
          parts.push(step.value);
          loaded += step.value.length;
          updateDownloadToast(fileName, loaded, totalSize);
        }
        blob = new Blob(parts, { type: contentType || '' });
      } else {
        updateDownloadToast(fileName, 1, 1);
        blob = await res.blob();
      }
      if (!blob || !blob.size) throw new Error('Empty response');
      saveBlob(blob, fileName);
      ok = true;
      finishDownloadToast(true, fileName);
    } catch (e) {
      console.warn('[Gallery] Blob download failed, falling back to direct link:', e);
      finishDownloadToast(false);
      // Fallback: force a same-origin download of the /p2p/... original
      const a = document.createElement('a');
      a.href = url;
      a.download = fileName;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
    } finally {
      isDownloadingItem = false;
    }
  }

  function setupToolbarButtons() {
    const toolbar = (lgInst && lgInst.$toolbar && lgInst.$toolbar.get()) ||
                    document.querySelector('.lg-container.lg-show .lg-toolbar') ||
                    document.querySelector('.lg-toolbar:last-of-type') ||
                    document.querySelector('.lg-toolbar');
    if (!toolbar) return;

    const isMobile = isMobileGallery();
    if (isMobile) {
      const existingFs = toolbar.querySelector('.omd-gallery-fs-btn, .lg-fullscreen');
      if (existingFs) existingFs.remove();
    } else {
      let fsBtn = toolbar.querySelector('.omd-gallery-fs-btn');
      if (!fsBtn) {
        // Check if lightgallery created a default .lg-fullscreen button
        const defaultFs = toolbar.querySelector('.lg-fullscreen');
        if (defaultFs) {
          fsBtn = defaultFs;
          fsBtn.classList.add('omd-gallery-fs-btn');
        } else {
          fsBtn = document.createElement('button');
          fsBtn.type = 'button';
          fsBtn.className = 'lg-icon omd-gallery-fs-btn';
          const closeBtn = toolbar.querySelector('.lg-close');
          if (closeBtn) {
            toolbar.insertBefore(fsBtn, closeBtn);
          } else {
            toolbar.appendChild(fsBtn);
          }
        }
      }

      fsBtn.onclick = (e) => {
        e.preventDefault();
        e.stopPropagation();
        resetInactivityTimer();
        toggleGalleryFullscreen();
      };

      updateFsButtonState();
    }

    // "Info" toggle button (next to fullscreen / before close): shows the
    // photo info plate. Primary use is mobile, where the plate is hidden
    // by default; on desktop it just re-opens the auto-hidden plate.
    let infoBtn = toolbar.querySelector('.omd-gallery-info-btn');
    if (!infoBtn) {
      infoBtn = document.createElement('button');
      infoBtn.type = 'button';
      infoBtn.className = 'lg-icon omd-gallery-info-btn';
      infoBtn.innerHTML = SVG_INFO;
      const fsRef = toolbar.querySelector('.omd-gallery-fs-btn');
      const closeBtn = toolbar.querySelector('.lg-close');
      if (fsRef) {
        toolbar.insertBefore(infoBtn, fsRef.nextSibling);
      } else if (closeBtn) {
        toolbar.insertBefore(infoBtn, closeBtn);
      } else {
        toolbar.appendChild(infoBtn);
      }
    }
    infoBtn.onclick = (e) => {
      e.preventDefault();
      e.stopPropagation();
      resetInactivityTimer();
      togglePlate();
    };
    updateInfoButtonState();

    // Capture lg's download button: always download the ORIGINAL file via
    // its /p2p/... URL with a proper filename. Without this, lightgallery's
    // naked <a download> can navigate or save an HTML error page instead.
    const dlBtn = toolbar.querySelector('.lg-download');
    if (dlBtn && !dlBtn.__omdBound) {
      dlBtn.__omdBound = true;
      dlBtn.addEventListener('click', (e) => {
        e.preventDefault();
        e.stopPropagation();
        resetInactivityTimer();
        downloadCurrentItem();
      }, true);
    }

    // Deterministic toolbar layout: counter flush-left, controls clustered
    // on the right: [download] [fullscreen] [info] [close]
    [' .lg-counter', '.lg-download', '.omd-gallery-info-btn', '.omd-gallery-fs-btn', '.lg-close'].forEach((sel) => {
      const el = toolbar.querySelector(sel);
      if (el) toolbar.appendChild(el);
    });

    // Attach direct capture-phase close listener on all .lg-close buttons for all devices
    const doClose = (e) => {
      if (e) {
        try { e.preventDefault(); } catch (_) {}
        try { e.stopPropagation(); } catch (_) {}
        try { e.stopImmediatePropagation(); } catch (_) {}
      }
      clearTimeout(hideTimer);
      const bars = document.querySelectorAll('.omd-gallery-bottom-bar');
      bars.forEach((b) => b.remove());
      if (lgInst && typeof lgInst.closeGallery === 'function') {
        lgInst.closeGallery(true);
      } else {
        const escEvt = new KeyboardEvent('keydown', { key: 'Escape', keyCode: 27, code: 'Escape', bubbles: true });
        window.dispatchEvent(escEvt);
      }
    };

    const closeButtons = document.querySelectorAll('.lg-close');
    closeButtons.forEach((closeBtn) => {
      if (closeBtn && !closeBtn.__omdBound) {
        closeBtn.__omdBound = true;
        closeBtn.addEventListener('click', doClose, true);
        closeBtn.addEventListener('touchend', doClose, true);
        closeBtn.addEventListener('pointerdown', (e) => e.stopPropagation(), true);
        closeBtn.addEventListener('mousedown', (e) => e.stopPropagation(), true);
        closeBtn.addEventListener('touchstart', (e) => e.stopPropagation(), true);
      }
    });
  }

  function updateBar(currentIndex) {
    const currentItem = (items && items[currentIndex]) || (lgInst && lgInst.galleryItems && lgInst.galleryItems[currentIndex]) || null;
    const captionParts = getGalleryCaptionParts(currentItem);

    const hasNav = totalCount > 1;

    // Галерея уже закрылась — плашку не рисуем и не воссоздаём
    // (иначе после закрытия остаётся висеть блок с описанием фото).
    if (!isOpen) return;

    // Плашка живёт внутри активной (открытой) галереи — не в body и не в закрытом outer
    const container = document.querySelector('.lg-container.lg-show');
    const outer = container ? container.querySelector('.lg-outer') : null;
    if (!outer) return;

    let bar = document.querySelector('.omd-gallery-bottom-bar');
    // Убираем бары, оставшиеся от неактуальных/предыдущих галерей
    document.querySelectorAll('.omd-gallery-bottom-bar').forEach((el) => { if (el !== bar) el.remove(); });

    if (!bar || !bar.parentNode) {
      bar = document.createElement('div');
      bar.className = 'omd-gallery-bottom-bar';
      outer.appendChild(bar);
    }

    // Info plate (title + description) is a separate widget in the
    // bottom-right corner — never inside the nav pill.
    updatePlate(captionParts);

    let navHtml = '';
    if (hasNav) {
      const prevDisabled = currentIndex <= 0 ? 'disabled' : '';
      const nextDisabled = currentIndex >= totalCount - 1 ? 'disabled' : '';

      let paginationHtml = '';
      if (!isLarge) {
        let dotsHtml = '';
        for (let i = 0; i < totalCount; i++) {
          dotsHtml += `<button type="button" class="omd-dot ${i === currentIndex ? 'active' : ''}" data-index="${i}" aria-label="Photo ${i + 1}"></button>`;
        }
        paginationHtml = `<div class="omd-gallery-dots">${dotsHtml}</div>`;
      } else {
        const progress = totalCount > 1 ? (currentIndex / (totalCount - 1)) * 100 : 0;
        paginationHtml = `
          <div class="omd-gallery-track" title="${currentIndex + 1} / ${totalCount}">
            <div class="omd-track-bg">
              <div class="omd-track-fill" style="width: ${progress}%;"></div>
              <div class="omd-track-thumb" style="left: ${progress}%;"></div>
            </div>
          </div>
        `;
      }

      navHtml = `
        <div class="omd-gallery-nav-row">
          <button type="button" class="omd-gallery-nav-btn omd-nav-prev" ${prevDisabled} title="Previous">
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M15 18l-6-6 6-6"/></svg>
          </button>
          <div class="omd-gallery-pagination">
            ${paginationHtml}
          </div>
          <button type="button" class="omd-gallery-nav-btn omd-nav-next" ${nextDisabled} title="Next">
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M9 18l6-6-6-6"/></svg>
          </button>
        </div>
      `;
    }

    // Single photo: nothing to navigate — remove the empty pill entirely
    if (!hasNav) {
      if (bar && bar.parentNode) bar.remove();
      return;
    }

    bar.innerHTML = `${navHtml}`;

    const prevBtn = bar.querySelector('.omd-nav-prev');
    const nextBtn = bar.querySelector('.omd-nav-next');
    if (prevBtn) {
      prevBtn.onclick = (e) => {
        e.stopPropagation();
        resetInactivityTimer();
        if (lgInst && typeof lgInst.goToPrevSlide === 'function') {
          lgInst.goToPrevSlide();
        }
      };
    }
    if (nextBtn) {
      nextBtn.onclick = (e) => {
        e.stopPropagation();
        resetInactivityTimer();
        if (lgInst && typeof lgInst.goToNextSlide === 'function') {
          lgInst.goToNextSlide();
        }
      };
    }

    const dots = bar.querySelectorAll('.omd-dot');
    dots.forEach((d) => {
      d.onclick = (e) => {
        e.stopPropagation();
        resetInactivityTimer();
        const idx = parseInt(d.getAttribute('data-index'), 10);
        if (!isNaN(idx) && lgInst && typeof lgInst.slide === 'function') {
          lgInst.slide(idx);
        }
      };
    });

    const track = bar.querySelector('.omd-gallery-track');
    if (track) {
      track.onclick = (e) => {
        e.stopPropagation();
        resetInactivityTimer();
        const rect = track.getBoundingClientRect();
        const clickX = e.clientX - rect.left;
        const pct = Math.max(0, Math.min(1, clickX / rect.width));
        const targetIdx = Math.round(pct * (totalCount - 1));
        if (lgInst && typeof lgInst.slide === 'function') {
          lgInst.slide(targetIdx);
        }
      };
    }
  }

  const handleFsChange = () => {
    updateFsButtonState();
  };

  const openHandler = (e) => {
    if (e.detail?.instance) lgInst = e.detail.instance;
    isOpen = true;
    isPlateForced = false;
    isPlateDismissed = false;
    // Убираем бар и инфо-плашку, оставшиеся от предыдущих галерей
    document.querySelectorAll('.omd-gallery-bottom-bar').forEach((el) => el.remove());
    removeInfoPlates();
    updateBar(initialIndex);
    setupToolbarButtons();
    showBar();
    window.addEventListener('mousemove', activityHandler, { passive: true });
    window.addEventListener('pointerdown', activityHandler, { passive: true });
    window.addEventListener('click', activityHandler);
    document.addEventListener('fullscreenchange', handleFsChange);
    document.addEventListener('webkitfullscreenchange', handleFsChange);
  };
  const slideHandler = (e) => {
    if (e.detail?.instance) lgInst = e.detail.instance;
    const idx = e.detail?.index ?? initialIndex;
    updateBar(idx);
    setupToolbarButtons();
    showBar();
  };
  const closeHandler = () => {
    isOpen = false;
    isPlateForced = false;
    isPlateDismissed = false;
    clearTimeout(hideTimer);
    window.removeEventListener('mousemove', activityHandler);
    window.removeEventListener('pointerdown', activityHandler);
    window.removeEventListener('click', activityHandler);
    document.removeEventListener('fullscreenchange', handleFsChange);
    document.removeEventListener('webkitfullscreenchange', handleFsChange);
    // Убираем ВСЕ бары и инфо-плашки (могли остаться из параллельных/предыдущих галерей)
    document.querySelectorAll('.omd-gallery-bottom-bar').forEach((b) => b.remove());
    removeInfoPlates();
  };

  lgEl.addEventListener('lgBeforeClose', closeHandler);
  lgEl.addEventListener('lgAfterOpen', openHandler);
  lgEl.addEventListener('lgAfterSlide', slideHandler);
  lgEl.addEventListener('lgAfterClose', closeHandler);

  return {
    setInstance(inst) {
      lgInst = inst;
      updateBar(initialIndex);
      setupToolbarButtons();
    },
    updateItems(newItems) {
      // Отложенное обновление (догрузка описаний) после закрытия — игнорируем
      if (!isOpen) return;
      items = Array.isArray(newItems) ? newItems : items;
      totalCount = items ? items.length : totalCount;
      const idx = (lgInst && typeof lgInst.index === 'number') ? lgInst.index : initialIndex;
      updateBar(idx);
    },
    cleanup() {
      lgEl.removeEventListener('lgBeforeClose', closeHandler);
      lgEl.removeEventListener('lgAfterOpen', openHandler);
      lgEl.removeEventListener('lgAfterSlide', slideHandler);
      lgEl.removeEventListener('lgAfterClose', closeHandler);
      closeHandler();
    }
  };
}
