/**
 * Centralized viewer & thumbnail settings from the "Appearance / Внешний вид" section.
 * Key: "omd_viewer_settings" in localStorage.
 */

export function getViewerSettings() {
  const defaults = {
    generateThumbs: true,
    downscaleImages: true,
    scaleTo: 1200,
  };
  if (typeof localStorage === "undefined") return defaults;
  try {
    const raw = localStorage.getItem("omd_viewer_settings");
    if (!raw) return defaults;
    const parsed = JSON.parse(raw);
    return {
      generateThumbs: parsed.generateThumbs !== false,
      downscaleImages: parsed.downscaleImages !== false,
      scaleTo: parseInt(parsed.scaleTo, 10) || 1200,
    };
  } catch (_) {
    return defaults;
  }
}
