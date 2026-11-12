<script>
  import { createEventDispatcher, onMount } from "svelte";
  import { _, locale, setLocale, SUPPORTED_LANGUAGES } from "../lib/i18n/index.js";
  import lightGallery from 'lightgallery';
  import lgFullscreen from 'lightgallery/plugins/fullscreen';
  import { setupGalleryPagination, getGalleryCaption } from "../lib/gallery-pagination.js";
  import 'lightgallery/css/lightgallery.css';
  import 'lightgallery/css/lg-fullscreen.css';
  import VideoViewer from "./VideoViewer.svelte";
  import AudioViewer from "./AudioViewer.svelte";
  import PdfViewer from "./PdfViewer.svelte";
  import { isOfficeDocument, extractDocTextFromBlob, isBinaryResponseType, univerDocToMarkdown } from "../lib/univer-doc-utils.js";
  import { marked } from "marked";
  import markedKatex from "marked-katex-extension";
  import "katex/dist/katex.min.css";
  import "../lib/markdown-styles.css";
  import { publicOrigin } from "../lib/public-origin.js";
  import { HttpDrive } from "../lib/http-drive.js";
  import { getStoredOmdKey } from "../lib/omd-key.js";
  import { getViewerSettings } from "../lib/viewer-settings.js";
  import { isCameraRollFolder, isImageOrVideo, isVideoFile, formatDuration, isIgnoredFile } from "../lib/svar-filemanager/components/CameraRoll/media-utils.js";
  import { resolveAppMode, APP_MODE_CHAT } from "../lib/app-mode.js";
  import { gatewayConfig } from "../lib/gateway-config.js";

  $: isChatDomain = resolveAppMode() === APP_MODE_CHAT;
  $: brandName = isChatDomain
    ? "AI Assistant"
    : ($gatewayConfig?.brandName || "Disk Direct");
  $: brandLogoSrc = isChatDomain
    ? (isDarkTheme ? "/customtheme/logo-chat.svg" : "/customtheme/logo-chat-dark.svg")
    : (isDarkTheme
        ? ($gatewayConfig?.skin?.logoDark || "/img/logo.svg")
        : ($gatewayConfig?.skin?.logoLight || "/img/logo-dark.svg"));

  export let shareData = null; // { name, path, isDir, mimeType, size, ownerName, linkId }
  export let isDarkTheme = false;
  export let targetLinkId = "";
  export let canEdit = false;

  const dispatch = createEventDispatcher();

  let currentSubPath = "";
  let folderItems = [];
  let loadingFolder = false;
  let folderError = "";
  let previewingFile = null;
  let folderViewMode = "cards"; // "cards" | "table"

  $: isCameraRoll = isCameraRollFolder(folderItems);
  $: crRealFiles = isCameraRoll ? (folderItems || []).filter(f => !isIgnoredFile(f)) : [];
  $: crSubfolders = isCameraRoll ? crRealFiles.filter(f => f.isDir || f.type === "folder") : [];
  $: crMediaFiles = isCameraRoll ? crRealFiles.filter(f => isImageOrVideo(f)).slice().sort((a, b) => new Date(b.date || 0).getTime() - new Date(a.date || 0).getTime()) : [];
  let lgInstance = null;
  let lgPagination = null;

  let isImage = false;
  let isVideo = false;
  let isAudio = false;
  let isPdf = false;
  let isMarkdown = false;
  let markdownContent = "";
  let markdownHtml = "";
  let loadingMarkdown = false;
  let isOfficeDoc = false;
  let docContent = "";
  let loadingDoc = false;
  let copied = false;

  marked.use(markedKatex({ throwOnError: false, nonStandard: true }));
  marked.setOptions({ gfm: true, breaks: false, async: false });
  let imgLoaded = false;
  let imgFailed = false;

  $: activeItem = previewingFile || (shareData && !shareData.isDir ? shareData : null);

  $: p2pUrl = activeItem ? getItemP2pUrl(activeItem) : (shareData ? getItemP2pUrl(shareData) : "");
  $: videoPoster = (() => {
    if (!p2pUrl) return "";
    const coverVer = activeItem?.customAttributes?.coverVersion || activeItem?.coverVersion || "";
    return p2pUrl + (p2pUrl.includes('?') ? '&' : '?') + 'poster' + (coverVer ? '&v=' + coverVer : '');
  })();

  $: activeLang = SUPPORTED_LANGUAGES.find(l => l.code === $locale) || SUPPORTED_LANGUAGES[0];

  $: if (p2pUrl) {
    const mime = (activeItem?.mimeType || shareData?.mimeType || "").toLowerCase();
    const ext = getItemExt(activeItem || shareData);
    isImage = mime.startsWith("image/") || ["jpg", "jpeg", "png", "gif", "webp", "svg", "bmp"].includes(ext);
    isVideo = mime.startsWith("video/") || ["mp4", "webm", "mkv", "mov", "avi"].includes(ext);
    isAudio = mime.startsWith("audio/") || ["mp3", "wav", "ogg", "flac", "m4a", "aac", "wma"].includes(ext);
    isPdf = mime === "application/pdf" || ext === "pdf";
    isMarkdown = ["md", "markdown"].includes(ext);
    isOfficeDoc = isOfficeDocument(activeItem?.name || shareData?.name || "", mime);

    imgLoaded = false;
    imgFailed = false;

    if (isMarkdown) {
      loadMarkdownText();
    } else if (isOfficeDoc) {
      loadDocText();

    }
  } else {
    isImage = false;
    isVideo = false;
    isAudio = false;
    isPdf = false;
    isMarkdown = false;
    isOfficeDoc = false;
    markdownContent = "";
    docContent = "";
    markdownHtml = "";
  }

  $: if (shareData && shareData.isDir) {
    loadFolderItems(currentSubPath);
  }

  $: videoPlaylist = (() => {
    if (!shareData || !shareData.isDir || !folderItems || folderItems.length === 0) {
      return [];
    }
    const list = folderItems.filter(f => {
      if (f.isDir) return false;
      const m = (f.mimeType || "").toLowerCase();
      const ext = getItemExt(f);
      return m.startsWith("video/") || ["mp4", "webm", "mkv", "mov", "avi"].includes(ext);
    }).map(f => {
      const originalUrl = getItemP2pUrl(f);
      const customAttrs = f.customAttributes || {};
      const coverVer = customAttrs.coverVersion || f.coverVersion || "";
      const posterUrl = originalUrl + (originalUrl.includes('?') ? '&' : '?') + 'poster' + (coverVer ? '&v=' + coverVer : '');
      const title = customAttrs.title || f.title || f.name;
      const description = customAttrs.description || f.description || "";
      return {
        rawItem: f,
        url: originalUrl,
        poster: posterUrl,
        name: f.name,
        title,
        description,
        customAttributes: customAttrs,
        size: f.size
      };
    });
    return list.length > 1 ? list : [];
  })();

  $: videoPlaylistIndex = (() => {
    if (!videoPlaylist || videoPlaylist.length === 0) return 0;
    const currentName = activeItem?.name || shareData?.name;
    const idx = videoPlaylist.findIndex(v => v.name === currentName || v.url === p2pUrl);
    return idx >= 0 ? idx : 0;
  })();

  $: audioPlaylist = (() => {
    if (!shareData || !shareData.isDir || !folderItems || folderItems.length === 0) {
      return [];
    }
    const list = folderItems.filter(f => {
      if (f.isDir) return false;
      const m = (f.mimeType || "").toLowerCase();
      const ext = getItemExt(f);
      return m.startsWith("audio/") || ["mp3", "wav", "ogg", "flac", "m4a", "aac", "wma"].includes(ext);
    }).map(f => {
      const originalUrl = getItemP2pUrl(f);
      const customAttrs = f.customAttributes || {};
      const coverVer = customAttrs.coverVersion || f.coverVersion || "";
      const posterUrl = originalUrl + (originalUrl.includes('?') ? '&' : '?') + 'poster' + (coverVer ? '&v=' + coverVer : '');
      const title = customAttrs.title || f.title || f.name;
      const description = customAttrs.description || f.description || "";
      return {
        rawItem: f,
        url: originalUrl,
        poster: posterUrl,
        hasCover: !!(customAttrs.hasCover || f.hasCover),
        name: f.name,
        title,
        description,
        customAttributes: customAttrs,
        size: f.size
      };
    });
    return list.length > 1 ? list : [];
  })();

  $: audioPlaylistIndex = (() => {
    if (!audioPlaylist || audioPlaylist.length === 0) return 0;
    const currentName = activeItem?.name || shareData?.name;
    const idx = audioPlaylist.findIndex(a => a.name === currentName || a.url === p2pUrl);
    return idx >= 0 ? idx : 0;
  })();

  $: breadcrumbs = (() => {
    if (!shareData || !shareData.isDir) return [];
    const rootName = shareData.title || shareData.name || "Shared Folder";
    const crumbs = [{ name: rootName, subPath: "" }];
    if (!currentSubPath) return crumbs;

    const segments = currentSubPath.split("/").filter(Boolean);
    let accumulated = "";
    for (const seg of segments) {
      accumulated += "/" + seg;
      crumbs.push({ name: seg, subPath: accumulated });
    }
    return crumbs;
  })();

  async function loadFolderItems(subPath) {
    loadingFolder = true;
    folderError = "";
    try {
      const devId = shareData.linkId || targetLinkId || "";
      const basePath = shareData.path.startsWith("/") ? shareData.path : "/" + shareData.path;
      const targetPath = (basePath.endsWith("/") ? basePath.slice(0, -1) : basePath) + (subPath.startsWith("/") ? subPath : "/" + subPath);
      const cleanPath = targetPath === "" ? "/" : targetPath;

      const drive = (typeof window !== "undefined" && window.__omdGetDrive) ? window.__omdGetDrive(devId) : null;
      let items = [];

      if (drive && drive.getFiles) {
        items = await drive.getFiles(cleanPath);
      } else {
        const devPrefix = devId ? (devId.startsWith("/") ? devId : "/" + devId) : "";
        const res = await fetch(`/p2p${devPrefix}` + encodeURI(cleanPath) + "?json=true");
        if (res.ok) {
          items = await res.json();
        }
      }

      folderItems = (items || []).map(item => {
        const isDir = !!(item.isDir || item.type === "folder");
        const name = item.name || item.value || (item.id ? item.id.split("/").filter(Boolean).pop() : "Item");
        const itemSubPath = (subPath.endsWith("/") ? subPath.slice(0, -1) : subPath) + "/" + name;
        return {
          ...item,
          name,
          isDir,
          type: isDir ? "folder" : "file",
          subPath: itemSubPath
        };
      });
    } catch (e) {
      console.error("[SingleView] Error loading folder items:", e);
      folderError = "Failed to load folder content";
    } finally {
      loadingFolder = false;
    }
  }

  onMount(() => {
    const handleReconnect = () => {
      console.log("[SingleView] P2P reconnected event received! Reloading folder items...");
      if (shareData && shareData.isDir) {
        loadFolderItems(currentSubPath);
      }
    };
    window.addEventListener("omd-p2p-reconnected", handleReconnect);
    return () => {
      window.removeEventListener("omd-p2p-reconnected", handleReconnect);
    };
  });

  function getItemExt(item) {
    return (item && item.name ? item.name : "").split(".").pop().toLowerCase();
  }

  const iconWhitelist = {
    "7z": true, rar: true, zip: true, css: true, html: true, js: true, php: true, md: true, xml: true, sql: true,
    aif: true, mid: true, mp3: true, waw: true, wma: true, doc: true, docx: true, txt: true,
    avi: true, mov: true, mp4: true, mpeg: true, mpg: true, pdf: true, xls: true, xlsx: true,
    gif: true, jpg: true, jpeg: true, png: true, psd: true, tiff: true, svg: true, folder: true, file: true, multiple: true, search: true, none: true
  };

  const BI_FOLDER_FILL = "M9.828 3h3.982a2 2 0 0 1 1.992 2.181l-.637 7A2 2 0 0 1 13.174 14H2.825a2 2 0 0 1-1.991-1.819l-.637-7a2 2 0 0 1 .342-1.31L.5 3a2 2 0 0 1 2-2h3.672a2 2 0 0 1 1.414.586l.828.828A2 2 0 0 0 9.828 3m-8.322.12q.322-.119.684-.12h5.396l-.707-.707A1 1 0 0 0 6.172 2H2.5a1 1 0 0 0-1 .981z";

  function biIconDataUri(d, color) {
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><path fill="${color}" d="${d}"/></svg>`;
    return "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg);
  }

  $: brandFolderColor = isDarkTheme ? "#7B86C4" : "#46508A";

  function getItemIconUrl(item, size = "big") {
    if (!item) return "";
    if (item.isDir || item.type === "folder") {
      return biIconDataUri(BI_FOLDER_FILL, brandFolderColor);
    }
    const ext = getItemExt(item);
    let iconKey = "file";

    if (iconWhitelist[ext]) {
      iconKey = ext;
    } else if (item.mimeType) {
      const mime = item.mimeType.toLowerCase();
      if (mime.startsWith("video/")) iconKey = "mp4";
      else if (mime.startsWith("image/")) iconKey = "jpg";
      else if (mime.startsWith("audio/")) iconKey = "mp3";
      else if (mime.includes("pdf")) iconKey = "pdf";
      else if (mime.includes("zip") || mime.includes("compressed") || mime.includes("tar") || mime.includes("gzip") || mime.includes("7z") || mime.includes("rar")) iconKey = "zip";
      else if (mime.includes("text") || mime.includes("javascript") || mime.includes("json") || mime.includes("xml") || mime.includes("html") || mime.includes("markdown")) iconKey = "txt";
      else if (mime.includes("excel") || mime.includes("spreadsheet") || mime.includes("sheet")) iconKey = "xlsx";
      else if (mime.includes("word") || mime.includes("document")) iconKey = "docx";
    }

    const s = size === "small" ? "small" : "big";
    return `https://cdn.svar.dev/icons/filemanager/vivid/${s}/${iconKey}.svg`;
  }

  function getFileCategory(item) {
    if (!item || item.isDir) return "folder";
    const mime = (item.mimeType || "").toLowerCase();
    const ext = getItemExt(item);

    if (mime.startsWith("video/") || ["mp4", "mov", "avi", "mkv", "webm", "mpeg", "mpg"].includes(ext)) return "video";
    if (mime.startsWith("audio/") || ["mp3", "wav", "flac", "m4a", "ogg", "wma"].includes(ext)) return "audio";
    if (mime === "application/pdf" || ext === "pdf") return "pdf";
    if (mime.includes("zip") || mime.includes("compressed") || ["zip", "rar", "7z", "tar", "gz"].includes(ext)) return "archive";
    if (mime.startsWith("image/") || ["jpg", "jpeg", "png", "gif", "webp", "bmp", "svg"].includes(ext)) return "image";
    if (["doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "md", "json", "js", "html", "css", "xml", "sql", "php"].includes(ext)) return "doc";
    return "generic";
  }

  function isImageItem(item) {
    if (!item || item.isDir) return false;
    const cat = getFileCategory(item);
    return cat === "image" || cat === "video" || !!item.hasCover || !!item.customAttributes?.cover;
  }

  function getItemP2pUrl(item) {
    if (!item || !shareData || !shareData.path) return "";
    const devId = shareData.linkId || targetLinkId || "";
    const devPrefix = devId ? (devId.startsWith("/") ? devId : "/" + devId) : "";
    const basePath = shareData.path.startsWith("/") ? shareData.path : "/" + shareData.path;

    let fullPath = basePath;
    if (item && item !== shareData) {
      const sub = item.subPath || (currentSubPath ? (currentSubPath + "/" + item.name) : ("/" + item.name));
      fullPath = (basePath.endsWith("/") ? basePath.slice(0, -1) : basePath) + (sub.startsWith("/") ? sub : "/" + sub);
    }

    let urlPath = fullPath;
    if (devPrefix && (urlPath === devPrefix || urlPath.startsWith(devPrefix + "/"))) {
      urlPath = urlPath.slice(devPrefix.length);
    }

    return `/p2p${devPrefix}` + encodeURI(urlPath.startsWith("/") ? urlPath : "/" + urlPath);
  }

  function getItemThumbnail(item) {
    const baseUrl = getItemP2pUrl(item);
    if (!baseUrl) return "";
    const vs = getViewerSettings();
    if (!vs.generateThumbs) return "";
    if (item.hasCover || item.customAttributes?.cover || getFileCategory(item) === "video") {
      return baseUrl + `?cover=360`;
    }
    return vs.downscaleImages ? baseUrl + `?resize=true&width=360&height=360` : baseUrl;
  }

  function downloadItem(item) {
    const baseUrl = getItemP2pUrl(item);
    if (!baseUrl) return;
    const downloadUrl = baseUrl + (baseUrl.includes("?") ? "&" : "?") + "download=true";
    const a = document.createElement("a");
    a.href = downloadUrl;
    a.download = item.name || "download";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  }

  function openImageGallery(file) {
    if (!folderItems || folderItems.length === 0) return;
    const images = folderItems.filter(f => isImageItem(f));
    if (images.length === 0) images.push(file);

    let startIndex = images.findIndex(f => f.name === file.name || f.subPath === file.subPath);
    if (startIndex < 0) startIndex = 0;

    const vs = getViewerSettings();
    const dynamicEl = images.map(f => {
      const originalUrl = getItemP2pUrl(f);
      const srcUrl = vs.downscaleImages
        ? `${originalUrl}?resize=true&width=${vs.scaleTo}&height=${vs.scaleTo}`
        : originalUrl;
      const thumbUrl = vs.generateThumbs
        ? `${originalUrl}?resize=true&width=360&height=360`
        : "";
      return {
        src: srcUrl,
        thumb: thumbUrl,
        downloadUrl: originalUrl + "?download=true",
        subHtml: getGalleryCaption(f)
      };
    });

    if (lgPagination) {
      try { lgPagination.cleanup(); } catch (_) {}
    }
    if (lgInstance) {
      try { lgInstance.destroy(true); } catch (_) {}
    }

    const lgEl = document.createElement('div');
    lgPagination = setupGalleryPagination(lgEl, dynamicEl, startIndex);

    lgInstance = lightGallery(lgEl, {
      licenseKey: "GPLv3",
      dynamic: true,
      dynamicEl,
      plugins: [],
      allowMediaOverlap: false,
      closable: true,
      showCloseIcon: true,
      fullScreen: false,
      closeOnTap: false,
      hideBarsDelay: 0,
      controls: false,
      speed: 300,
      swipeThreshold: 20,
      slideEndAnimation: false,
      preload: 2,
      easing: 'cubic-bezier(0.25, 1, 0.5, 1)',
      enableSwipe: true,
      enableDrag: true,
      mobileSettings: {
        controls: false,
        showCloseIcon: true,
        fullScreen: false,
        download: true,
        speed: 280,
        swipeThreshold: 10,
        preload: 1,
        slideEndAnimation: false,
        easing: 'cubic-bezier(0.25, 1, 0.5, 1)'
      }
    });
    lgPagination.setInstance(lgInstance);
    lgInstance.openGallery(startIndex);
  }

  function handleItemClick(item) {
    if (item.isDir) {
      previewingFile = null;
      currentSubPath = item.subPath;
      return;
    }

    const mime = (item.mimeType || "").toLowerCase();
    const ext = getItemExt(item);
    const isImg = mime.startsWith("image/") || ["jpg", "jpeg", "png", "gif", "webp", "svg"].includes(ext);

    if (isImg) {
      openImageGallery(item);
    } else {
      previewingFile = item;
    }
  }

  async function loadMarkdownText() {
    loadingMarkdown = true;
    try {
      const res = await fetch(p2pUrl);
      if (res.ok) {
        markdownContent = await res.text();
        markdownHtml = marked.parse(markdownContent || "");
      }
    } catch (e) {
      console.warn("[SingleView] Failed to load markdown:", e);
    } finally {
      loadingMarkdown = false;
    }
  }

  // Backend-based native conversion (onmychat /ondoc/convert): reads the
  // ORIGINAL file from the node filesystem and returns a Univer IDocumentData
  // snapshot. Used only when the current device runs an AI-capable onmychat
  // backend; any failure falls back to the legacy totext/local extraction.
  let docSnapshot = null;
  let ondDrive = null;
  function getOndDrive() {
    const existing = (typeof window !== "undefined" && window.__omdHttpDrive) || null;
    if (existing) return existing;
    const devId = shareData?.linkId || targetLinkId || "";
    if (!devId) return null;
    if (!ondDrive || ondDrive.targetLinkId !== devId) {
      if (ondDrive) ondDrive.close();
      ondDrive = new HttpDrive({
        targetLinkId: devId,
        port: 4000,
        isPrivateMode: true,
        authToken: getStoredOmdKey() || ""
      });
    }
    return ondDrive;
  }

  async function tryBackendSnapshot() {
    try {
      const drive = getOndDrive();
      if (!drive || typeof drive.fetch !== "function" || !p2pUrl) {
        console.warn('[SingleView] ondoc: no http drive available');
        return null;
      }
      // p2pUrl: /p2p/<device>/<path>?query -> device-local <path>
      const m = p2pUrl.match(/^\/p2p\/[^/]+(\/.*)$/);
      if (!m) return null;
      let filePath = decodeURIComponent(m[1].split('?')[0]);
      const res = await drive.fetch('/ondoc/convert', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ path: filePath })
      });
      if (!res.ok) {
        console.warn('[SingleView] ondoc convert HTTP status:', res.status);
        return null;
      }
      const data = await res.json();
      const snap = data?.result?.snapshot || data?.snapshot;
      if (!snap?.body?.dataStream) return null;
      console.log('[SingleView] ondoc snapshot loaded:', snap.body.dataStream.length, 'chars from', filePath);
      return snap;
    } catch (e) {
      console.warn('[SingleView] ondoc backend not available:', e.message || e);
      return null;
    }
  }

  async function loadDocText() {
    loadingDoc = true;
    docContent = "";
    docSnapshot = null;
    // 1. Preferred: device backend converts the original file into the native
    //    Univer snapshot directly on the node (no gateway, no pandoc).
    docSnapshot = await tryBackendSnapshot();
    if (docSnapshot) {
      try {
        const mdFromSnap = univerDocToMarkdown(docSnapshot);
        if (mdFromSnap && mdFromSnap.trim()) docContent = mdFromSnap;
      } catch (convErr) {
        console.warn("[SingleView] snapshot to markdown conversion failed:", convErr);
      }
      loadingDoc = false;
      return;
    }
    try {
      const toTextUrl = p2pUrl + (p2pUrl.includes('?') ? '&' : '?') + 'totext';
      const res = await fetch(toTextUrl);
      let converted = "";
      if (res.ok) {
        const contentType = res.headers.get('content-type') || '';
        converted = await res.text();
        console.log("[SingleView] totext:", contentType, "len", converted.length);
        if (!isBinaryResponseType(contentType) && converted.trim()) {
          docContent = converted;
        }
      } else {
        console.warn("[SingleView] totext HTTP status:", res.status);
      }
      if (!docContent.trim()) {
        // ?totext unavailable or ignored (P2P) — fetch the file and
        // extract text locally.
        try {
          const binRes = await fetch(p2pUrl);
          if (binRes.ok) {
            const blob = await binRes.blob();
            console.log("[SingleView] local conversion: blob size", blob.size, "type", blob.type);
            const md = await extractDocTextFromBlob(blob, activeItem?.name || shareData?.name || "");
            console.log("[SingleView] local conversion result len:", md === null ? null : md.length);
            if (md !== null && md.trim()) docContent = md;
          }
        } catch (e2) {
          console.warn("[SingleView] local doc extraction failed:", e2);
        }
      }
    } catch (e) {
      console.warn("[SingleView] Failed to load doc text:", e);
    } finally {
      loadingDoc = false;
    }
  }

  function formatBytes(bytes, decimals = 2) {
    if (!bytes || bytes === 0) return "0 Bytes";
    const k = 1024;
    const dm = decimals < 0 ? 0 : decimals;
    const sizes = ["Bytes", "KB", "MB", "GB", "TB"];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + " " + sizes[i];
  }

  function copyLink() {
    if (typeof navigator !== "undefined" && navigator.clipboard) {
      const pathname = (typeof window !== "undefined" && window.location.pathname) || "";
      const search = (typeof window !== "undefined" && window.location.search) || "";
      navigator.clipboard.writeText(`${publicOrigin()}${pathname}${search}`);
      copied = true;
      setTimeout(() => { copied = false; }, 2000);
    }
  }

  let isFullscreen = false;
  let stageElement = null;
  let videoElement = null;

  function toggleFullscreen() {
    if (isVideo && videoElement) {
      if (!document.fullscreenElement) {
        if (videoElement.requestFullscreen) {
          videoElement.requestFullscreen().catch(() => {});
        } else if (videoElement.webkitRequestFullscreen) {
          videoElement.webkitRequestFullscreen();
        }
      } else {
        if (document.exitFullscreen) {
          document.exitFullscreen().catch(() => {});
        }
      }
      return;
    }

    if (!stageElement) return;
    if (!document.fullscreenElement) {
      if (stageElement.requestFullscreen) {
        stageElement.requestFullscreen().catch(() => {
          isFullscreen = !isFullscreen;
        });
      } else {
        isFullscreen = !isFullscreen;
      }
    } else {
      if (document.exitFullscreen) {
        document.exitFullscreen().catch(() => {});
      }
    }
  }

  onMount(() => {
    const handleFsChange = () => {
      isFullscreen = !!document.fullscreenElement;
    };
    const handleMsg = (e) => {
      if (e.data && e.data.action === "toggleFullscreen") {
        toggleFullscreen();
      }
    };
    document.addEventListener("fullscreenchange", handleFsChange);
    document.addEventListener("webkitfullscreenchange", handleFsChange);
    window.addEventListener("message", handleMsg);
    return () => {
      document.removeEventListener("fullscreenchange", handleFsChange);
      document.removeEventListener("webkitfullscreenchange", handleFsChange);
      window.removeEventListener("message", handleMsg);
    };
  });

  function handleMarkdownEdit() {
    dispatch("edit", {
      path: (previewingFile && (previewingFile.realPath || previewingFile.id)) ? (previewingFile.realPath || previewingFile.id) : (shareData?.path || ""),
      name: (previewingFile && previewingFile.name) || shareData?.name || "",
      content: markdownContent,
      url: p2pUrl
    });
  }

  function handleDownload() {
    const downloadLink = document.createElement("a");
    downloadLink.href = p2pUrl + (p2pUrl.includes("?") ? "&" : "?") + "download=true";
    downloadLink.download = (activeItem && activeItem.name) || shareData?.name || "download";
    document.body.appendChild(downloadLink);
    downloadLink.click();
    document.body.removeChild(downloadLink);
  }
</script>

<div class="singleview-wrapper {isDarkTheme ? 'dark-theme' : 'light-theme'}">
  <!-- Top Bar -->
  <header class="singleview-nav">
    <div class="nav-left">
      <img src={brandLogoSrc} alt={brandName} class="brand-logo" />
      <span class="brand-badge">{$_("p2p_sharing", {}, "P2P Sharing")}</span>
    </div>
    <div class="nav-right">
      <!-- Guest Language Switcher (Compact code button) -->
      <div class="guest-lang-wrapper" title={$_("language", {}, "Language")}>
        <div class="guest-lang-display">
          <span class="guest-lang-code">{activeLang.shortCode || activeLang.code.toUpperCase()}</span>
          <svg class="guest-lang-arrow" viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>
        </div>
        <select
          class="guest-lang-select-hidden"
          value={$locale}
          on:change={(e) => setLocale(e.target.value)}
          aria-label={$_("language", {}, "Language")}
        >
          {#each SUPPORTED_LANGUAGES as lang}
            <option value={lang.code}>{lang.nativeName} ({lang.shortCode || lang.code.toUpperCase()})</option>
          {/each}
        </select>
      </div>

      <button class="nav-btn" on:click={copyLink} title={$_("copy_share_url", {}, "Copy link to clipboard")}>
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>
        <span>{copied ? $_("copied", {}, "Copied!") : $_("share_link", {}, "Share Link")}</span>
      </button>
      {#if !shareData?.isDir || previewingFile}
        <button class="nav-btn primary" on:click={handleDownload}>
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
          <span>{$_("download", {}, "Download")} ({formatBytes(activeItem?.size || shareData?.size)})</span>
        </button>
      {/if}
    </div>
  </header>

  <!-- Main Content Viewer -->
  <main class="singleview-main">
    <div class="viewer-card">
      <!-- Media Preview -->
      <div class="preview-stage" bind:this={stageElement} class:is-fullscreen={isFullscreen} class:is-audio-stage={isAudio}>
        {#if (isImage || isMarkdown) && !isPdf && !isVideo}
          <button
            class="fullscreen-toggle-btn"
            on:click={toggleFullscreen}
            title={isFullscreen ? "Exit Fullscreen (Esc)" : "Full Screen View"}
          >
            {#if isFullscreen}
              <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M8 3v3a2 2 0 0 1-2 2H3m18 0h-3a2 2 0 0 1-2-2V3m0 18v-3a2 2 0 0 1 2-2h3M3 16h3a2 2 0 0 1 2 2v3"/>
              </svg>
            {:else}
              <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3"/>
              </svg>
            {/if}
          </button>
          {#if canEdit && isMarkdown}
            <button
              class="fullscreen-toggle-btn"
              on:click={handleMarkdownEdit}
              title="Edit Document"
            >
              <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 20h9"/><path d="M16.5 3.5a2.121 2.121 0 0 1 3 3L7 19l-4 1 1-4L16.5 3.5z"/></svg>
            </button>
          {/if}
        {/if}

        {#if shareData?.isDir && !previewingFile}
          <!-- Shared Folder View -->
          <div class="shared-folder-view">
            <!-- Folder Breadcrumb & View Toggle Bar -->
            <div class="folder-crumbs-bar">
              <div class="crumbs-trail">
                <i class="wxi-folder crumb-folder-icon"></i>
                {#each breadcrumbs as crumb, i}
                  {#if i > 0}<span class="crumb-slash">/</span>{/if}
                  <button
                    class="crumb-link {i === breadcrumbs.length - 1 ? 'current' : ''}"
                    on:click={() => { previewingFile = null; currentSubPath = crumb.subPath; }}
                  >
                    {crumb.name}
                  </button>
                {/each}
              </div>
              <div class="view-mode-toggle" role="group" aria-label="View mode">
                <button
                  type="button"
                  class="view-mode-btn {folderViewMode === 'cards' ? 'active' : ''}"
                  on:click={() => { folderViewMode = 'cards'; }}
                  title={$_("cards_view", {}, "Cards View")}
                  aria-label={$_("cards_view", {}, "Cards View")}
                >
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                    <rect x="3" y="3" width="7" height="7" rx="1.5"/>
                    <rect x="14" y="3" width="7" height="7" rx="1.5"/>
                    <rect x="14" y="14" width="7" height="7" rx="1.5"/>
                    <rect x="3" y="14" width="7" height="7" rx="1.5"/>
                  </svg>
                </button>
                <button
                  type="button"
                  class="view-mode-btn {folderViewMode === 'table' ? 'active' : ''}"
                  on:click={() => { folderViewMode = 'table'; }}
                  title={$_("table_view", {}, "Table View")}
                  aria-label={$_("table_view", {}, "Table View")}
                >
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                    <line x1="8" y1="6" x2="21" y2="6"/>
                    <line x1="8" y1="12" x2="21" y2="12"/>
                    <line x1="8" y1="18" x2="21" y2="18"/>
                    <line x1="3" y1="6" x2="3.01" y2="6"/>
                    <line x1="3" y1="12" x2="3.01" y2="12"/>
                    <line x1="3" y1="18" x2="3.01" y2="18"/>
                  </svg>
                </button>
              </div>
            </div>

            <!-- Folder Contents Stage -->
            <div class="folder-stage-body">
              {#if loadingFolder}
                <div class="folder-state-loading">
                  <div class="spinner"></div>
                  <span>{$_("loading_folder", {}, "Loading folder contents...")}</span>
                </div>
              {:else if folderError}
                <div class="folder-state-error">
                  <p>{folderError}</p>
                </div>
              {:else if folderItems.length === 0}
                <div class="folder-state-empty">
                  <i class="wxi-folder" style="font-size: 48px; color: var(--accent, #7B86C4);"></i>
                  <p>{$_("folder_empty", {}, "This folder is empty")}</p>
                </div>
              {:else if folderViewMode === 'cards'}
                {#if isCameraRoll}
                  <!-- Camera Roll Mode (Pure media folders) -->
                  <div class="wx-cameraroll-container">
                    {#if crSubfolders.length}
                      <div class="wx-cr-folders-row">
                        {#each crSubfolders as folder}
                          <!-- svelte-ignore a11y_click_events_have_key_events -->
                          <!-- svelte-ignore a11y_no_static_element_interactions -->
                          <div
                            class="wx-cr-folder-chip"
                            title={folder.name}
                            on:click={() => handleItemClick(folder)}
                          >
                            <i class="wxi-folder"></i>
                            <span class="wx-cr-folder-name">{folder.name}</span>
                          </div>
                        {/each}
                      </div>
                    {/if}

                    {#if crMediaFiles.length}
                      <div class="wx-cameraroll-grid">
                        {#each crMediaFiles as item}
                          <!-- svelte-ignore a11y_click_events_have_key_events -->
                          <!-- svelte-ignore a11y_no_static_element_interactions -->
                          <div
                            class="wx-cr-item"
                            title={item.name}
                            on:click={() => handleItemClick(item)}
                          >
                            <div class="wx-cr-preview-wrapper">
                              {#if getItemThumbnail(item) && !item._imgFailed}
                                <img
                                  src={getItemThumbnail(item)}
                                  alt={item.name}
                                  loading="lazy"
                                  class="wx-cr-img"
                                  on:error={() => {
                                    item._imgFailed = true;
                                    folderItems = folderItems;
                                  }}
                                />
                              {:else}
                                <div class="wx-cr-fallback">
                                  <i class="wxi-{isVideoFile(item) ? 'video' : 'image'}"></i>
                                </div>
                              {/if}

                              {#if isVideoFile(item)}
                                <div class="wx-cr-video-badge">
                                  <svg viewBox="0 0 24 24" width="12" height="12" fill="currentColor">
                                    <path d="M8 5v14l11-7z" />
                                  </svg>
                                  {#if item.customAttributes?.videoDuration || item.duration}
                                    <span class="wx-cr-duration">{formatDuration(item.customAttributes?.videoDuration || item.duration)}</span>
                                  {/if}
                                </div>
                              {/if}

                              <button
                                type="button"
                                class="wx-cr-download-btn"
                                title={$_("download_file", {}, "Download file")}
                                on:click|stopPropagation={() => downloadItem(item)}
                              >
                                <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                                  <polyline points="7 10 12 15 17 10" />
                                  <line x1="12" y1="15" x2="12" y2="3" />
                                </svg>
                              </button>
                            </div>
                          </div>
                        {/each}
                      </div>
                    {/if}
                  </div>
                {:else}
                  <!-- Cards Mode (Styled like SVAR Filemanager cards) -->
                  <div class="folder-cards-grid">
                  {#each folderItems as item}
                    <div
                      class="svar-like-card {item.isDir ? 'is-folder-card' : 'is-file-card'}"
                      on:click={() => handleItemClick(item)}
                    >
                      <div class="card-thumb-area">
                        {#if item.isDir}
                          <div class="card-icon-area">
                            <img src={getItemIconUrl(item, "big")} alt="" class="card-mime-img is-folder" />
                          </div>
                        {:else if isImageItem(item) && getItemThumbnail(item) && !item._imgFailed}
                          <img
                            src={getItemThumbnail(item)}
                            alt={item.name}
                            class="card-cover-img"
                            on:error={() => {
                              item._imgFailed = true;
                              folderItems = folderItems;
                            }}
                          />
                        {:else}
                          <div class="card-icon-area">
                            <img src={getItemIconUrl(item, "big")} alt="" class="card-mime-img" />
                          </div>
                        {/if}

                        {#if !item.isDir}
                          <button
                            class="card-hover-dl-btn"
                            on:click|stopPropagation={() => downloadItem(item)}
                            title={$_("download_file", {}, "Download file")}
                          >
                            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                          </button>
                        {/if}
                      </div>

                      <div class="card-meta-bar">
                        <span class="card-name-text" title={item.name}>{item.name}</span>
                        <span class="card-sub-text">{item.isDir ? $_("folder", {}, "Folder") : formatBytes(item.size)}</span>
                      </div>
                    </div>
                  {/each}
                </div>
              {/if}
              {:else}
                <!-- Table Mode (Styled like SVAR Filemanager table) -->
                <div class="folder-table-wrapper">
                  <table class="svar-like-table">
                    <thead>
                      <tr>
                        <th style="width: 50%;">{$_("name", {}, "Name")}</th>
                        <th style="width: 20%;">{$_("size", {}, "Size")}</th>
                        <th style="width: 15%;">{$_("type", {}, "Type")}</th>
                        <th style="width: 15%; text-align: right;">{$_("action", {}, "Action")}</th>
                      </tr>
                    </thead>
                    <tbody>
                      {#each folderItems as item}
                        <tr class="table-row-item" on:click={() => handleItemClick(item)}>
                          <td class="td-name">
                            <div class="td-name-cell">
                              <img src={getItemIconUrl(item, "small")} alt="" class="table-mime-img {item.isDir ? 'is-folder' : ''}" />
                              <span class="table-item-name" title={item.name}>{item.name}</span>
                            </div>
                          </td>
                          <td class="td-size">{item.isDir ? '-' : formatBytes(item.size)}</td>
                          <td class="td-type">{item.isDir ? $_("folder", {}, "Folder") : getItemExt(item).toUpperCase()}</td>
                          <td class="td-actions" style="text-align: right;">
                            {#if !item.isDir}
                              <button
                                class="table-dl-btn"
                                on:click|stopPropagation={() => downloadItem(item)}
                                title={$_("download_file", {}, "Download file")}
                              >
                                <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                              </button>
                            {/if}
                          </td>
                        </tr>
                      {/each}
                    </tbody>
                  </table>
                </div>
              {/if}
            </div>
          </div>
        {:else}
          {#if previewingFile && !isVideo}
            <div class="stage-top-bar">
              <button class="stage-back-btn" on:click={() => { previewingFile = null; }}>
                <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg>
                <span>{$_("back_to_folder", {}, "Back to Folder")}</span>
              </button>
              <span class="stage-active-name">{previewingFile.name}</span>
            </div>
          {/if}

          {#if isImage}
            <div class="image-container">
              <img
                src={p2pUrl}
                alt={activeItem?.name || shareData?.name}
                referrerpolicy="no-referrer"
                class="preview-img {imgLoaded ? 'loaded' : ''}"
                on:load={() => { imgLoaded = true; }}
                on:error={() => { imgFailed = true; }}
              />
              {#if !imgLoaded && !imgFailed}
                <div class="preview-loading">
                  <div class="spinner"></div>
                </div>
              {/if}
              {#if imgFailed}
                <div class="preview-fallback-icon">
                  <svg viewBox="0 0 24 24" width="64" height="64" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="8.5" cy="8.5" r="1.5"/><polyline points="21 15 16 10 5 21"/></svg>
                  <p>{$_("failed_load_image", {}, "Failed to load image preview")}</p>
                </div>
              {/if}
            </div>
          {:else if isVideo}
            <div class="video-viewer-wrapper">
              <VideoViewer
                url={p2pUrl}
                poster={videoPoster}
                filename={activeItem?.name || shareData?.name}
                title={activeItem?.title || activeItem?.customAttributes?.title || shareData?.title || shareData?.customAttributes?.title || activeItem?.name || shareData?.name}
                description={activeItem?.description || activeItem?.customAttributes?.description || shareData?.description || shareData?.customAttributes?.description || ''}
                playlist={videoPlaylist}
                currentIndex={videoPlaylistIndex}
                showClose={!!(shareData?.isDir || previewingFile)}
                showInfo={false}
                on:change={(e) => {
                  if (e.detail?.video?.rawItem) {
                    previewingFile = e.detail.video.rawItem;
                  }
                }}
                on:close={() => { previewingFile = null; }}
              />
            </div>
          {:else if isAudio}
            <div class="audio-viewer-wrapper">
              <AudioViewer
                url={p2pUrl}
                poster={videoPoster}
                hasCover={activeItem?.hasCover || activeItem?.customAttributes?.hasCover || shareData?.hasCover || false}
                filename={activeItem?.name || shareData?.name}
                title={activeItem?.title || activeItem?.customAttributes?.title || shareData?.title || shareData?.customAttributes?.title || activeItem?.name || shareData?.name}
                description={activeItem?.description || activeItem?.customAttributes?.description || shareData?.description || shareData?.customAttributes?.description || ''}
                playlist={audioPlaylist}
                currentIndex={audioPlaylistIndex}
                showClose={!!(shareData?.isDir || previewingFile)}
                on:change={(e) => {
                  if (e.detail?.audio?.rawItem) {
                    previewingFile = e.detail.audio.rawItem;
                  }
                }}
                on:close={() => { previewingFile = null; }}
              />
            </div>
          {:else if isMarkdown}
            <div class="markdown-container md-view {isDarkTheme ? 'dark-theme' : 'light-theme'}">
              {#if loadingMarkdown}
                <div class="preview-loading"><div class="spinner"></div></div>
              {:else}
                <div class="markdown-body">
                  {@html markdownHtml || `<p>${$_("no_text_content", {}, "No text content")}</p>`}
                </div>
              {/if}
            </div>
          {:else if isPdf}
            <div class="pdf-container">
              <PdfViewer url={p2pUrl} hideShare={true} on:close={() => { previewingFile = null; }} on:toggleFullscreen={toggleFullscreen} />
            </div>
          {:else if isOfficeDoc}
            <div class="doc-viewer-container markdown-container md-view {isDarkTheme ? 'dark-theme' : 'light-theme'}" style="width: 100%; height: 100%; min-height: 520px; display: flex; flex-direction: column;">
              {#if loadingDoc}
                <div class="preview-loading"><div class="spinner"></div></div>
              {:else}
                <div class="markdown-body">
                  {@html marked.parse(docContent || "")}
                </div>
              {/if}
            </div>
          {:else}
            <!-- Generic File Placeholder -->
            <div class="generic-file-box">
              <div class="file-icon-wrap">
                <svg viewBox="0 0 24 24" width="72" height="72" fill="none" stroke="currentColor" stroke-width="1.2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/><polyline points="10 9 9 9 8 9"/></svg>
              </div>
              <h2 class="generic-file-name">{activeItem?.name || shareData?.name}</h2>
              <p class="generic-file-size">{formatBytes(activeItem?.size || shareData?.size)} • {activeItem?.mimeType || shareData?.mimeType || "Binary file"}</p>
              <button class="btn-download-hero" on:click={handleDownload}>
                <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
                <span>{$_("download_file", {}, "Download File")}</span>
              </button>
            </div>
          {/if}
        {/if}
      </div>

      <!-- File Metadata & Footer -->
      <div class="viewer-footer">
        <div class="file-info-col">
          <h1 class="file-name-title">{activeItem ? activeItem.name : (shareData?.title || shareData?.name)}</h1>
          {#if shareData?.description || shareData?.customAttributes?.description}
            <p class="file-description">{shareData?.description || shareData?.customAttributes?.description}</p>
          {/if}
          <div class="file-meta-row">
            <span>{activeItem && !activeItem.isDir ? formatBytes(activeItem.size) : (shareData?.isDir ? $_("shared_folder", {}, "Shared Folder") : formatBytes(shareData?.size))}</span>
            <span class="meta-dot">•</span>
            <span>{$_("shared_by_user", { user: shareData?.ownerName || $_("device_owner", {}, "Device Owner") }, "Shared by " + (shareData?.ownerName || "Device Owner"))}</span>
          </div>
        </div>

        <div class="file-actions-col">
          {#if !shareData?.isDir || previewingFile}
            <button class="action-btn" on:click={handleDownload}>
              <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
              <span>{$_("download", {}, "Download")}</span>
            </button>
          {/if}
          <button class="action-btn secondary" on:click={copyLink}>
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg>
            <span>{copied ? $_("link_copied", {}, "Link Copied") : $_("direct_link", {}, "Direct Link")}</span>
          </button>
        </div>
      </div>
    </div>
  </main>

  <!-- SingleView Brand Footer -->
  <footer class="singleview-bottom-footer">
    <span>{$_("powered_by_omd", {}, "Direct P2P Storage Powered by")} <a href="https://disk.direct" target="_blank" rel="noreferrer">Disk Direct</a></span>
  </footer>
</div>

<style>
  .singleview-wrapper {
    display: flex;
    flex-direction: column;
    min-height: 100vh;
    width: 100%;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    transition: background 0.2s ease, color 0.2s ease;
  }

  .singleview-wrapper.light-theme {
    background: #F6F7FA;
    color: #161925;
    --md-stage-bg: #F6F7FA;
  }

  .singleview-wrapper.dark-theme {
    background: #151821;
    color: #ffffff;
    --md-stage-bg: #191D28;
  }

  /* SingleView Bottom Brand Footer */
  .singleview-bottom-footer {
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 16px 24px calc(16px + env(safe-area-inset-bottom, 0px));
    font-size: 12px;
    font-weight: 500;
    margin-top: auto;
    width: 100%;
    box-sizing: border-box;
    transition: background 0.2s ease, border-color 0.2s ease;
  }

  .light-theme .singleview-bottom-footer {
    background: #F3EFFA;
    border-top: 1px solid #ECE7F6;
    color: #5A6278;
  }

  .light-theme .singleview-bottom-footer a {
    color: #5863A6;
    text-decoration: none;
  }

  .light-theme .singleview-bottom-footer a:hover {
    text-decoration: underline;
  }

  .dark-theme .singleview-bottom-footer {
    background: #191D28;
    border-top: 1px solid #32394B;
    color: #7E869B;
  }

  .dark-theme .singleview-bottom-footer a {
    color: #7B86C4;
    text-decoration: none;
  }

  .dark-theme .singleview-bottom-footer a:hover {
    text-decoration: underline;
  }

  /* Nav Header */
  .singleview-nav {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: calc(12px + env(safe-area-inset-top, 0px)) calc(24px + env(safe-area-inset-right, 0px)) 12px calc(24px + env(safe-area-inset-left, 0px));
    backdrop-filter: blur(12px);
    border-bottom: 1px solid rgba(139, 128, 163, 0.16);
    z-index: 10;
    box-sizing: border-box;
  }

  .light-theme .singleview-nav {
    background: rgba(255, 255, 255, 0.9);
    border-bottom-color: #D8DCE8;
  }

  .dark-theme .singleview-nav {
    background: #191D28;
    border-bottom-color: #32394B;
  }

  .nav-left {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .brand-logo {
    height: 34px;
    width: auto;
  }

  .brand-badge {
    font-size: 11px;
    font-weight: 600;
    padding: 3px 8px;
    border-radius: 12px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    background: rgba(123, 134, 196, 0.16);
    color: #5863A6;
  }

  .nav-right {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .guest-lang-wrapper {
    position: relative;
    display: inline-flex;
    align-items: center;
    cursor: pointer;
  }

  .guest-lang-display {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 7px 10px;
    border-radius: 8px;
    font-size: 12px;
    font-weight: 700;
    pointer-events: none;
    transition: background-color 0.2s, border-color 0.2s, color 0.2s;
  }

  .light-theme .guest-lang-display {
    background-color: #F6F7FA;
    color: #5A6278;
    border: 1px solid #D8DCE8;
  }

  .guest-lang-wrapper:hover .guest-lang-display,
  .guest-lang-wrapper:focus-within .guest-lang-display {
    border-color: #5863A6;
  }

  .light-theme .guest-lang-wrapper:hover .guest-lang-display {
    background-color: #ffffff;
  }

  .dark-theme .guest-lang-display {
    background-color: #282E3D;
    color: #F6F7FA;
    border: 1px solid rgba(139, 128, 163, 0.28);
  }

  .dark-theme .guest-lang-wrapper:hover .guest-lang-display,
  .dark-theme .guest-lang-wrapper:focus-within .guest-lang-display {
    border-color: #929CD4;
    background-color: #282138;
  }

  .guest-lang-code {
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.5px;
    line-height: 1;
    display: inline-block;
  }

  .guest-lang-arrow {
    opacity: 0.6;
    flex-shrink: 0;
  }

  .guest-lang-select-hidden {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    opacity: 0;
    cursor: pointer;
    z-index: 2;
  }

  .dark-theme .guest-lang-select-hidden {
    color-scheme: dark;
  }

  .dark-theme .guest-lang-select-hidden option {
    background-color: #1F2430;
    color: #ffffff;
  }

  .light-theme .guest-lang-select-hidden {
    color-scheme: light;
  }

  .light-theme .guest-lang-select-hidden option {
    background-color: #ffffff;
    color: #161925;
  }

  .nav-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 8px 14px;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    border: 1px solid rgba(139, 128, 163, 0.28);
    background: transparent;
    color: inherit;
    transition: all 0.15s ease;
  }

  .nav-btn:hover {
    background: rgba(139, 128, 163, 0.12);
  }

  .nav-btn.primary {
    background: #5863A6;
    border-color: #5863A6;
    color: #ffffff;
    font-weight: 600;
  }

  .nav-btn.primary:hover {
    background: #7B86C4;
  }

  /* Main Section */
  .singleview-main {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .viewer-card {
    width: 100%;
    max-width: 75%;
    border-radius: 16px;
    overflow: hidden;
    display: flex;
    flex-direction: column;
    box-shadow: 0 12px 36px rgba(0, 0, 0, 0.12);
    border: 1px solid rgba(139, 128, 163, 0.22);
  }

  .light-theme .viewer-card {
    background: #ffffff;
    border-color: #D8DCE8;
  }

  .dark-theme .viewer-card {
    background: #1F2430;
    border-color: #32394B;
  }

  /* Stage & Previews */
  .preview-stage {
    min-height: 380px;
    max-height: 70vh;
    display: flex;
    flex-direction: column;
    align-items: stretch;
    justify-content: flex-start;
    overflow: hidden;
    position: relative;
    width: 100%;
    border-top-left-radius: 16px;
    border-top-right-radius: 16px;
    transition: background 0.2s ease;
  }

  .preview-stage.is-audio-stage {
    min-height: 540px;
    max-height: none;
    height: auto;
  }

  .light-theme .preview-stage {
    background: #F6F7FA;
  }

  .dark-theme .preview-stage {
    background: #191D28 !important;
  }

  /* Fullscreen Toggle Button */
  .fullscreen-toggle-btn {
    position: absolute;
    top: 10px;
    right: 10px;
    z-index: 100;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 32px;
    height: 32px;
    border-radius: 6px;
    background: rgba(139, 128, 163, 0.22);
    backdrop-filter: blur(8px);
    border: 1px solid rgba(139, 128, 163, 0.35);
    color: var(--fs-toggle-color, #161925);
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .dark-theme .fullscreen-toggle-btn {
    --fs-toggle-color: #ffffff;
  }

  .light-theme .fullscreen-toggle-btn {
    --fs-toggle-color: #161925;
  }

  .fullscreen-toggle-btn:hover {
    background: var(--wx-color-secondary-hover, rgba(123, 134, 196, 0.35));
    border-color: var(--wx-color-primary, #7B86C4);
    color: var(--wx-color-secondary-font, #5863A6);
  }





  /* Native & Fallback Fullscreen Stage Styling */
  .preview-stage:fullscreen,
  .preview-stage:-webkit-full-screen,
  .preview-stage.is-fullscreen {
    position: fixed !important;
    top: 0 !important;
    left: 0 !important;
    width: 100vw !important;
    height: 100vh !important;
    max-width: 100vw !important;
    max-height: 100vh !important;
    min-height: 100vh !important;
    z-index: 99999 !important;
    border-radius: 0 !important;
    background: var(--md-stage-bg, #000000) !important;
    color: inherit;
  }

  .preview-stage:fullscreen .markdown-container,
  .preview-stage:-webkit-full-screen .markdown-container,
  .preview-stage.is-fullscreen .markdown-container {
    max-height: 100vh !important;
    height: 100vh;
    padding: 16px 32px;
  }

  .preview-stage:fullscreen .pdf-container,
  .preview-stage:-webkit-full-screen .pdf-container,
  .preview-stage.is-fullscreen .pdf-container {
    width: 100vw !important;
    height: 100vh !important;
    max-height: 100vh !important;
  }

  .preview-stage:fullscreen .pdf-frame,
  .preview-stage:-webkit-full-screen .pdf-frame,
  .preview-stage.is-fullscreen .pdf-frame {
    width: 100vw !important;
    height: 100vh !important;
  }

  .preview-stage:fullscreen .preview-img,
  .preview-stage:-webkit-full-screen .preview-img,
  .preview-stage.is-fullscreen .preview-img {
    max-width: 100vw !important;
    max-height: 100vh !important;
    object-fit: contain !important;
  }

  .image-container {
    width: 100%;
    height: 100%;
    flex: 1;
    min-height: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    position: relative;
  }

  .preview-img {
    max-width: 100%;
    max-height: 60vh;
    object-fit: contain;
    opacity: 0;
    transition: opacity 0.2s ease-in-out;
  }

  .preview-img.loaded {
    opacity: 1;
  }

  .preview-loading {
    position: absolute;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .spinner {
    width: 36px;
    height: 36px;
    border: 3px solid rgba(255, 255, 255, 0.2);
    border-top-color: #7B86C4;
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  .preview-fallback-icon {
    display: flex;
    flex-direction: column;
    align-items: center;
    color: #7E869B;
    gap: 8px;
  }

  .video-viewer-wrapper,
  .audio-viewer-wrapper {
    width: 100%;
    height: 100%;
    min-height: 540px;
    flex: 1;
    display: flex;
    position: relative;
    border-radius: 12px;
    overflow: hidden;
  }

  /* Fullscreen Video Override */
  .preview-video:fullscreen,
  .preview-video:-webkit-full-screen,
  .video-container:fullscreen,
  .video-container:-webkit-full-screen,
  .video-container:fullscreen .preview-video,
  .video-container:-webkit-full-screen .preview-video,
  :global(video:fullscreen),
  :global(video:-webkit-full-screen) {
    max-height: 100vh !important;
    max-width: 100vw !important;
    width: 100vw !important;
    height: 100vh !important;
    object-fit: contain !important;
    background: #000000 !important;
    border-radius: 0 !important;
    box-shadow: none !important;
  }

  .audio-container {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 40px;
    gap: 16px;
    color: #F6F7FA;
  }

  .audio-disc {
    width: 90px;
    height: 90px;
    border-radius: 50%;
    background: rgba(123, 134, 196, 0.16);
    color: #7B86C4;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .preview-audio {
    width: 320px;
    max-width: 90vw;
  }

  .markdown-container {
    width: 100%;
    height: 100%;
    max-height: 65vh;
    padding: 24px;
    overflow-y: auto;
    background: inherit;
    color: inherit;
  }

  .pdf-container {
    width: 100%;
    height: 65vh;
  }

  .pdf-frame {
    width: 100%;
    height: 100%;
    border: none;
  }

  /* Generic file box */
  .generic-file-box {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 48px 24px;
    text-align: center;
    background: inherit;
    color: inherit;
    gap: 12px;
  }

  .file-icon-wrap {
    color: #5863A6;
    margin-bottom: 8px;
  }

  .generic-file-name {
    font-size: 20px;
    font-weight: 600;
    margin: 0;
    max-width: 80%;
    word-break: break-word;
  }

  .generic-file-size {
    font-size: 14px;
    color: #7E869B;
    margin: 0;
  }

  .btn-download-hero {
    margin-top: 12px;
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 12px 24px;
    background: #5863A6;
    color: #ffffff;
    border: none;
    border-radius: 10px;
    font-size: 15px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s ease;
  }

  .btn-download-hero:hover {
    background: #7B86C4;
  }

  /* Footer details */
  .viewer-footer {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 20px 24px;
    border-top: 1px solid rgba(139, 128, 163, 0.16);
    flex-wrap: wrap;
    gap: 16px;
  }

  .light-theme .viewer-footer {
    background: #F3EFFA;
    border-top-color: #D8DCE8;
  }

  .dark-theme .viewer-footer {
    background: #1F2430;
    border-top-color: #32394B;
  }

  .file-info-col {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .file-name-title {
    font-size: 18px;
    font-weight: 600;
    margin: 0;
    word-break: break-word;
  }

  .file-description {
    font-size: 14px;
    color: #D8DCE8;
    margin: 2px 0 4px 0;
    line-height: 1.4;
    word-break: break-word;
  }

  .file-meta-row {
    font-size: 13px;
    color: #7E869B;
    display: flex;
    align-items: center;
    gap: 6px;
    flex-wrap: wrap;
  }

  .meta-dot {
    opacity: 0.6;
  }

  .node-badge {
    font-family: monospace;
    font-size: 11px;
    padding: 2px 6px;
    border-radius: 4px;
    background: rgba(139, 128, 163, 0.16);
  }

  .file-actions-col {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .action-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 9px 16px;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    background: #5863A6;
    border: 1px solid #5863A6;
    color: #ffffff;
    transition: all 0.15s ease;
  }

  .action-btn:hover {
    background: #7B86C4;
    border-color: #7B86C4;
  }

  .action-btn.secondary {
    background: transparent;
    border-color: rgba(139, 128, 163, 0.28);
    color: inherit;
  }

  .action-btn.secondary:hover {
    background: rgba(139, 128, 163, 0.12);
  }

  .singleview-bottom-footer {
    text-align: center;
    padding: 16px;
    font-size: 12px;
    color: #7E869B;
  }

  .singleview-bottom-footer a {
    color: #5863A6;
    text-decoration: none;
  }

  .singleview-bottom-footer a:hover {
    text-decoration: underline;
  }

  @media (max-width: 768px) {
    .fullscreen-toggle-btn {
      display: none !important;
    }
    .brand-badge {
      display: none;
    }
    .singleview-nav .nav-btn span {
      display: none;
    }
    .singleview-nav .nav-btn {
      padding: 8px 12px;
    }
    .singleview-main {
      padding: 0;
    }
    .viewer-card {
      max-width: 100%;
      border-radius: 0;
      border-left: none;
      border-right: none;
      box-shadow: none;
    }
    .preview-stage {
      border-top-left-radius: 0;
      border-top-right-radius: 0;
      min-height: 380px;
      max-height: 80vh;
    }
    .viewer-footer {
      flex-direction: column;
      align-items: stretch;
      padding: 16px;
    }
    .file-actions-col {
      width: 100%;
    }
    .action-btn {
      flex: 1;
      justify-content: center;
    }
  }

  @media (max-width: 640px) {
    .viewer-footer {
      flex-direction: column;
      align-items: stretch;
    }

    .file-actions-col {
      width: 100%;
    }

    .action-btn {
      flex: 1;
    }
  }

  /* Shared Folder View & Stage Top Bar */
  .stage-top-bar {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 12px 16px;
    width: 100%;
    border-bottom: 1px solid rgba(139, 128, 163, 0.16);
  }

  .light-theme .stage-top-bar {
    background: #F3EFFA;
    border-bottom-color: #D8DCE8;
  }

  .dark-theme .stage-top-bar {
    background: #1F2430;
    border-bottom-color: #32394B;
  }

  .stage-back-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 12px;
    background: rgba(123, 134, 196, 0.16);
    color: #5863A6;
    border: 1px solid rgba(123, 134, 196, 0.30);
    border-radius: 6px;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .stage-back-btn:hover {
    background: rgba(123, 134, 196, 0.30);
    color: #7B86C4;
  }

  .stage-active-name {
    font-size: 14px;
    font-weight: 600;
    color: #F6F7FA;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .shared-folder-view {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    min-height: 400px;
    max-height: 70vh;
  }

  .folder-crumbs-bar {
    padding: 12px 20px;
    border-bottom: 1px solid rgba(139, 128, 163, 0.16);
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
  }

  .light-theme .folder-crumbs-bar {
    background: #F3EFFA;
    border-bottom-color: #D8DCE8;
  }

  .dark-theme .folder-crumbs-bar {
    background: #1F2430;
    border-bottom-color: #32394B;
  }

  .crumb-folder-icon {
    font-size: 18px;
    color: #5863A6;
    margin-right: 4px;
  }

  .crumbs-trail {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
    font-size: 14px;
  }

  .crumb-slash {
    color: #5A6278;
  }

  .crumb-link {
    background: none;
    border: none;
    color: #7E869B;
    font-size: 14px;
    font-weight: 500;
    padding: 2px 6px;
    border-radius: 4px;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .crumb-link:hover {
    color: #7B86C4;
    background: rgba(123, 134, 196, 0.12);
  }

  .crumb-link.current {
    color: #F6F7FA;
    font-weight: 600;
  }

  /* View Mode Switcher */
  .view-mode-toggle {
    display: inline-flex;
    align-items: center;
    gap: 2px;
    background: #151821;
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 8px;
    padding: 3px;
    box-sizing: border-box;
    flex-shrink: 0;
  }

  .view-mode-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 32px;
    height: 32px;
    min-width: 32px;
    max-width: 32px;
    min-height: 32px;
    max-height: 32px;
    padding: 0;
    margin: 0;
    box-sizing: border-box;
    border-radius: 6px;
    background: transparent;
    border: none;
    outline: none;
    color: #7E869B;
    cursor: pointer;
    transition: all 0.15s ease;
    flex: 0 0 32px;
  }

  .view-mode-btn svg {
    display: block;
    width: 16px;
    height: 16px;
    stroke: currentColor;
    pointer-events: none;
  }

  .view-mode-btn.active {
    background: #5863A6;
    color: #ffffff;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3);
  }

  .view-mode-btn:hover:not(.active) {
    color: #ffffff;
    background: rgba(255, 255, 255, 0.06);
  }

  .folder-stage-body {
    flex: 1;
    padding: 20px;
    overflow-y: auto;
    transition: background 0.2s ease;
  }

  .dark-theme .folder-stage-body {
    background: #191D28;
  }

  .light-theme .folder-stage-body {
    background: #F6F7FA;
  }

  /* Custom Slim Theme-Adaptive Scrollbars */
  .folder-stage-body::-webkit-scrollbar {
    width: 6px;
    height: 6px;
  }

  .folder-stage-body::-webkit-scrollbar-track {
    background: transparent;
  }

  .dark-theme .folder-stage-body::-webkit-scrollbar-thumb {
    background: rgba(139, 128, 163, 0.28);
    border-radius: 4px;
  }

  .dark-theme .folder-stage-body::-webkit-scrollbar-thumb:hover {
    background: rgba(123, 134, 196, 0.60);
  }

  .light-theme .folder-stage-body::-webkit-scrollbar-thumb {
    background: rgba(139, 128, 163, 0.36);
    border-radius: 4px;
  }

  .light-theme .folder-stage-body::-webkit-scrollbar-thumb:hover {
    background: rgba(123, 134, 196, 0.60);
  }

  .dark-theme .folder-stage-body {
    scrollbar-width: thin;
    scrollbar-color: rgba(139, 128, 163, 0.32) transparent;
  }

  .light-theme .folder-stage-body {
    scrollbar-width: thin;
    scrollbar-color: rgba(139, 128, 163, 0.42) transparent;
  }

  .folder-state-loading, .folder-state-error, .folder-state-empty {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    height: 100%;
    min-height: 250px;
    gap: 12px;
    color: #7E869B;
  }

  /* CameraRoll Mode in SingleView */
  .wx-cameraroll-container {
    display: flex;
    flex-direction: column;
    width: 100%;
    box-sizing: border-box;
    cursor: default;
  }

  .wx-cr-folders-row {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    padding: 10px 10px 8px;
    flex-shrink: 0;
    background: transparent;
    cursor: default;
  }

  .wx-cr-folder-chip {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 12px;
    border-radius: 8px;
    background: rgba(255, 255, 255, 0.06);
    border: 1px solid rgba(255, 255, 255, 0.12);
    color: #F6F7FA;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    white-space: nowrap;
    user-select: none;
    transition: background-color 0.15s ease, border-color 0.15s ease, transform 0.1s ease;
  }

  .light-theme .wx-cr-folder-chip {
    background: rgba(0, 0, 0, 0.04);
    border-color: rgba(0, 0, 0, 0.1);
    color: #161925;
  }

  .wx-cr-folder-chip:hover {
    background: rgba(255, 255, 255, 0.12);
    border-color: var(--wx-color-primary, #929CD4);
    transform: translateY(-1px);
  }

  .light-theme .wx-cr-folder-chip:hover {
    background: rgba(0, 0, 0, 0.08);
    border-color: var(--wx-color-primary, #5863A6);
  }

  .wx-cr-folder-chip i {
    font-size: 16px;
    color: var(--wx-color-primary, #7B86C4) !important;
  }

  .wx-cameraroll-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(175px, 1fr));
    gap: 4px;
    padding: 2px 0;
    align-content: flex-start;
    box-sizing: border-box;
    width: 100%;
    cursor: default;
  }

  @media (max-width: 600px) {
    .wx-cameraroll-grid {
      grid-template-columns: repeat(3, 1fr);
      gap: 2px;
      padding: 0;
    }
  }

  @media (min-width: 601px) and (max-width: 960px) {
    .wx-cameraroll-grid {
      grid-template-columns: repeat(auto-fill, minmax(130px, 1fr));
      gap: 3px;
      padding: 2px 0;
    }
  }

  .wx-cr-item {
    position: relative;
    aspect-ratio: 1 / 1;
    width: 100%;
    height: 100%;
    overflow: hidden;
    border-radius: 4px;
    background-color: rgba(125, 125, 125, 0.12);
    cursor: pointer;
    user-select: none;
    box-sizing: border-box;
    display: block;
  }

  .wx-cr-item:hover {
    filter: brightness(1.05);
  }

  .wx-cr-preview-wrapper {
    width: 100%;
    height: 100%;
    position: relative;
    overflow: hidden;
    background: rgba(0, 0, 0, 0.08);
  }

  .wx-cr-img {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
    position: relative;
    transition: transform 0.2s ease;
  }

  .wx-cr-item:hover .wx-cr-img {
    transform: scale(1.025);
  }

  .wx-cr-fallback {
    width: 100%;
    height: 100%;
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--wx-color-primary, #929CD4);
    font-size: 44px;
  }

  .wx-cr-video-badge {
    position: absolute;
    bottom: 6px;
    right: 6px;
    background: rgba(0, 0, 0, 0.65);
    backdrop-filter: blur(4px);
    color: #ffffff;
    font-size: 11px;
    font-weight: 500;
    padding: 2px 6px;
    border-radius: 4px;
    display: flex;
    align-items: center;
    gap: 4px;
    pointer-events: none;
    z-index: 2;
    line-height: 1;
  }

  .wx-cr-duration {
    font-size: 10px;
    letter-spacing: 0.2px;
  }

  .wx-cr-download-btn {
    position: absolute;
    top: 6px;
    right: 6px;
    width: 28px;
    height: 28px;
    border-radius: 50%;
    background: rgba(0, 0, 0, 0.55);
    backdrop-filter: blur(4px);
    border: none;
    color: #ffffff;
    display: flex;
    align-items: center;
    justify-content: center;
    opacity: 0;
    transform: scale(0.85);
    transition: opacity 0.15s ease, transform 0.15s ease, background 0.15s ease;
    z-index: 3;
    cursor: pointer;
  }

  .wx-cr-item:hover .wx-cr-download-btn {
    opacity: 1;
    transform: scale(1);
  }

  .wx-cr-download-btn:hover {
    background: rgba(0, 0, 0, 0.85);
    color: #ffffff;
  }

  @media (max-width: 768px) {
    .wx-cr-download-btn {
      opacity: 0.85;
      transform: scale(1);
    }
  }

  /* SVAR-like Cards Mode */
  .folder-cards-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(170px, 1fr));
    gap: 16px;
    cursor: default;
  }

  .svar-like-card {
    position: relative;
    display: flex;
    flex-direction: column;
    background: #1F2430;
    border: 1px solid #32394B;
    border-radius: 12px;
    overflow: hidden;
    cursor: pointer;
    transition: all 0.2s ease;
  }

  .svar-like-card:hover {
    transform: translateY(-3px);
    background: #282E3D;
    border-color: #5863A6;
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.4);
  }

  .card-thumb-area {
    position: relative;
    width: 100%;
    height: 130px;
    background: #191D28;
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: hidden;
  }

  .card-cover-img {
    width: 100%;
    height: 100%;
    object-fit: cover;
    transition: transform 0.25s ease;
  }

  .svar-like-card:hover .card-cover-img {
    transform: scale(1.05);
  }

  .card-icon-area {
    width: 100%;
    height: 100%;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .card-mime-img {
    width: 64px;
    height: 64px;
    max-width: 64px;
    max-height: 64px;
    object-fit: contain;
    transition: transform 0.2s ease;
  }

  .card-mime-img.is-folder {
    width: 68px;
    height: 68px;
    max-width: 68px;
    max-height: 68px;
  }

  .svar-like-card:hover .card-mime-img {
    transform: scale(1.06);
  }

  .table-mime-img {
    width: 20px;
    height: 20px;
    max-width: 20px;
    max-height: 20px;
    object-fit: contain;
    flex-shrink: 0;
  }

  .card-hover-dl-btn {
    position: absolute;
    top: 8px;
    right: 8px;
    width: 32px;
    height: 32px;
    border-radius: 50%;
    background: rgba(21, 24, 33, 0.86);
    color: #AEB5C7;
    border: 1px solid #32394B;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    opacity: 0;
    transition: all 0.15s ease;
    z-index: 2;
  }

  .svar-like-card:hover .card-hover-dl-btn {
    opacity: 1;
  }

  .card-hover-dl-btn:hover {
    background: #5863A6;
    color: #ffffff;
    border-color: #5863A6;
  }

  .card-meta-bar {
    padding: 12px;
    display: flex;
    flex-direction: column;
    gap: 3px;
    background: #1A1625;
    border-top: 1px solid #32394B;
  }

  .card-name-text {
    font-size: 13px;
    font-weight: 500;
    color: #F6F7FA;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    width: 100%;
  }

  .card-sub-text {
    font-size: 11px;
    color: #7E869B;
  }

  /* SVAR-like Table Mode */
  .folder-table-wrapper {
    width: 100%;
    overflow-x: auto;
  }

  .svar-like-table {
    width: 100%;
    border-collapse: collapse;
    font-size: 13px;
    text-align: left;
  }

  .table-folder-icon {
    font-size: 20px;
    color: #5863A6;
  }

  .svar-like-table th {
    padding: 10px 14px;
    color: #7E869B;
    font-weight: 600;
    font-size: 12px;
    border-bottom: 1px solid rgba(139, 128, 163, 0.16);
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }

  .table-row-item {
    border-bottom: 1px solid rgba(139, 128, 163, 0.10);
    cursor: pointer;
    transition: background 0.15s ease;
  }

  .table-row-item:hover {
    background: rgba(123, 134, 196, 0.12);
  }

  .td-name {
    padding: 12px 14px;
    color: #F6F7FA;
    font-weight: 500;
  }

  .td-name-cell {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .table-item-name {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .td-size, .td-type {
    padding: 12px 14px;
    color: #7E869B;
  }

  .td-actions {
    padding: 12px 14px;
  }

  .table-dl-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 28px;
    border-radius: 6px;
    background: rgba(123, 134, 196, 0.16);
    color: #5863A6;
    border: 1px solid rgba(123, 134, 196, 0.30);
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .table-dl-btn:hover {
    background: #5863A6;
    color: #ffffff;
  }

  /* Light Theme Overrides for Folder View */
  .light-theme .folder-crumbs-bar {
    background: #F3EFFA;
    border-bottom-color: #F6F7FA;
  }

  .light-theme .crumb-link {
    color: #5A6278;
  }

  .light-theme .crumb-link:hover {
    color: #5863A6;
    background: rgba(123, 134, 196, 0.12);
  }

  .light-theme .crumb-link.current {
    color: #161925;
  }

  .light-theme .view-mode-toggle {
    background: #ECE7F6;
    border-color: #D8DCE8;
  }

  .light-theme .view-mode-btn {
    color: #5A6278;
  }

  .light-theme .view-mode-btn.active {
    background: #ffffff;
    color: #5863A6;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.12);
  }

  .light-theme .view-mode-btn:hover:not(.active) {
    color: #161925;
    background: rgba(0, 0, 0, 0.05);
  }

  .light-theme .svar-like-card {
    background: #ffffff;
    border-color: #F6F7FA;
  }

  .light-theme .svar-like-card:hover {
    background: #F6F7FA;
    border-color: #5863A6;
    box-shadow: 0 6px 16px rgba(0, 0, 0, 0.08);
  }

  .light-theme .card-thumb-area {
    background: #F3EFFA;
  }

  .light-theme .card-meta-bar {
    background: #ffffff;
    border-top-color: #F6F7FA;
  }

  .light-theme .card-name-text {
    color: #161925;
  }

  .light-theme .card-sub-text {
    color: #5A6278;
  }

  .light-theme .svar-like-table th {
    color: #5A6278;
    border-bottom-color: #F6F7FA;
  }

  .light-theme .table-row-item {
    border-bottom-color: #F6F7FA;
  }

  .light-theme .table-row-item:hover {
    background: rgba(61, 70, 122, 0.08);
  }

  .light-theme .td-name {
    color: #161925;
  }

  .light-theme .td-size,
  .light-theme .td-type {
    color: #5A6278;
  }

  .light-theme .stage-top-bar {
    background: #F3EFFA;
    border-bottom-color: #F6F7FA;
  }

  .light-theme .stage-active-name {
    color: #161925;
  }

  .light-theme .card-hover-dl-btn {
    background: rgba(255, 255, 255, 0.95);
    color: #5A6278;
    border-color: #D8DCE8;
    box-shadow: 0 2px 6px rgba(0, 0, 0, 0.1);
  }

  .light-theme .card-hover-dl-btn:hover {
    background: #5863A6;
    color: #ffffff;
    border-color: #5863A6;
  }

  .light-theme .table-dl-btn {
    background: rgba(123, 134, 196, 0.12);
    color: #5863A6;
    border-color: rgba(61, 70, 122, 0.25);
  }

  .light-theme .table-dl-btn:hover {
    background: #5863A6;
    color: #ffffff;
    border-color: #5863A6;
  }

  /* LightGallery Toolbar & Fullscreen Button */
  :global(.lg-toolbar .omd-gallery-fs-btn svg),
  :global(.lg-toolbar .lg-fullscreen svg) {
    display: block !important;
    margin: auto !important;
    pointer-events: none !important;
    flex-shrink: 0 !important;
    fill: #ffffff !important;
    color: #ffffff !important;
  }
  :global(.lg-toolbar .omd-gallery-fs-btn svg path),
  :global(.lg-toolbar .lg-fullscreen svg path) {
    fill: #ffffff !important;
  }
  :global(.lg-toolbar .omd-gallery-fs-btn:after),
  :global(.lg-toolbar .lg-fullscreen:after) {
    display: none !important;
    content: "" !important;
  }
  :global(.lg-toolbar .lg-icon),
  :global(.lg-toolbar .omd-gallery-fs-btn) {
    color: #ffffff !important;
  }

  /* Themed LightGallery backdrop & toolbar (html gets light-theme/dark-theme classes) */
  :global(.dark-theme .lg-backdrop) {
    background-color: rgba(8, 6, 12, 0.92) !important;
  }
  :global(.light-theme .lg-backdrop) {
    background-color: rgba(248, 247, 252, 0.94) !important;
  }
  :global(.dark-theme .lg-outer.lg-show-backdrop .lg-backdrop) {
    background-color: rgba(8, 6, 12, 0.92) !important;
  }
  :global(.light-theme .lg-toolbar) {
    background: linear-gradient(180deg, rgba(255, 255, 255, 0.52) 0%, rgba(255, 255, 255, 0) 100%) !important;
  }
  :global(.light-theme .lg-toolbar .lg-icon),
  :global(.light-theme .lg-toolbar .omd-gallery-fs-btn) {
    color: #161925 !important;
    background: rgba(255, 255, 255, 0.48) !important;
    border-color: rgba(255, 255, 255, 0.55) !important;
    box-shadow: 0 2px 10px rgba(22, 25, 37, 0.12) !important;
    filter: none !important;
  }
  :global(.light-theme .lg-toolbar .omd-gallery-fs-btn svg) {
    fill: #161925 !important;
    color: #161925 !important;
  }
  :global(.light-theme .lg-toolbar .omd-gallery-fs-btn svg path),
  :global(.light-theme .lg-toolbar .lg-fullscreen svg path) {
    fill: #161925 !important;
  }
  :global(.light-theme .lg-outer .lg-counter) {
    color: #161925 !important;
    text-shadow: none !important;
  }
  :global(.dark-theme .lg-outer .lg-counter) {
    color: #ECE7F6 !important;
  }

  @media (max-width: 768px) {
    .folder-cards-grid {
      grid-template-columns: repeat(auto-fill, minmax(130px, 1fr));
      gap: 12px;
    }
    .card-hover-dl-btn {
      opacity: 1;
    }
  }
</style>
