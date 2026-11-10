/**
 * Global auto-hide controller for LightGallery caption pill (.lg-sub-html).
 *
 * Rules:
 * 1. Automatically fades out after 3 seconds of inactivity.
 * 2. Smooth fade-in on:
 *    - Hover in the bottom area of the screen (bottom 160px or over .lg-components/.lg-sub-html).
 *    - Single tap / click / touchstart anywhere on the gallery.
 *    - Slide change / image change.
 * 3. Stays visible while the cursor remains in the bottom area.
 * 4. When cursor leaves the bottom area, a 3-second countdown starts before fading out.
 */

let isInitialized = false;
let hideTimer = null;
let isHoveringBottom = false;
let lastSubHtmlText = "";

function getActiveSubHtml() {
  const outer = document.querySelector(".lg-outer");
  if (!outer) return null;
  const sub = outer.querySelector(".lg-sub-html");
  if (!sub || sub.classList.contains("lg-empty-html")) return null;
  if (!sub.textContent || !sub.textContent.trim()) return null;
  return sub;
}

export function showGalleryCaption() {
  const sub = getActiveSubHtml();
  if (!sub) return;
  sub.classList.remove("omd-caption-hidden");
  resetHideTimer();
}

export function hideGalleryCaption() {
  if (isHoveringBottom) return;
  const sub = getActiveSubHtml();
  if (!sub) return;
  sub.classList.add("omd-caption-hidden");
}

function resetHideTimer() {
  clearTimeout(hideTimer);
  if (isHoveringBottom) return;
  hideTimer = setTimeout(() => {
    hideGalleryCaption();
  }, 3000);
}

export function initGalleryCaptionAutoHide() {
  if (isInitialized || typeof window === "undefined") return;
  isInitialized = true;

  const onMouseMove = (e) => {
    const sub = getActiveSubHtml();
    if (!sub) {
      isHoveringBottom = false;
      return;
    }

    const inBottomZone = e.clientY >= window.innerHeight - 160;
    if (inBottomZone) {
      isHoveringBottom = true;
      sub.classList.remove("omd-caption-hidden");
      clearTimeout(hideTimer);
    } else {
      if (isHoveringBottom) {
        isHoveringBottom = false;
        resetHideTimer();
      }
    }
  };

  const onUserInteraction = () => {
    const sub = getActiveSubHtml();
    if (!sub) return;
    sub.classList.remove("omd-caption-hidden");
    resetHideTimer();
  };

  window.addEventListener("mousemove", onMouseMove, { passive: true });
  window.addEventListener("pointerdown", onUserInteraction, { passive: true });
  window.addEventListener("keydown", onUserInteraction, { passive: true });
  window.addEventListener("touchstart", onUserInteraction, { passive: true });

  // Observer to detect when a gallery opens or changes slide content
  const observer = new MutationObserver(() => {
    const sub = getActiveSubHtml();
    if (sub) {
      const curText = sub.innerHTML;
      if (curText !== lastSubHtmlText) {
        lastSubHtmlText = curText;
        sub.classList.remove("omd-caption-hidden");
        resetHideTimer();
      }
    } else {
      lastSubHtmlText = "";
      clearTimeout(hideTimer);
      isHoveringBottom = false;
    }
  });

  observer.observe(document.body, {
    childList: true,
    subtree: true,
    characterData: true
  });
}
