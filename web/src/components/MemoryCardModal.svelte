<script>
  import { createEventDispatcher, onDestroy } from "svelte";
  import { fade } from "svelte/transition";
  import { _ } from "../lib/i18n/index.js";
  import { importUploadedFile, FILE_INPUT_ACCEPT } from "../lib/document-text.js";
  import { deviceRegistry } from "../lib/device-registry.js";
  import DocumentPicker from "./DocumentPicker.svelte";

  export let open = false;
  export let card = null; // null for new card, or existing card object
  export let chatStore = null;
  export let httpDrive = null;
  export let device = null;
  export let targetLinkId = "";
  export let isPrivateMode = undefined;
  export let isDarkTheme = true;
  export let kindIntent = "note"; // используем при создании новой карточки (note|document|link)
  export let thumbFetcher = null; // async (document_id, size) => objectURL|"" для ленивого превью on-device
  export let readOnly = false;

  $: isViewOnly = readOnly || card?.readOnly || (isPrivateMode === false && card?.onDevice);

  function cleanDisplayPath(p) {
    if (!p) return "";
    let s = String(p);
    if (!isPrivateMode) {
      s = s.replace(/^\/[^\/]+(?=\/)/, "");
    }
    return s;
  }

  const dispatch = createEventDispatcher();

  let title = "";
  let text = "";
  let kind = "note"; // "note" | "document" | "link"
  let documentId = "";
  let tags = [];
  let tagInput = "";
  let collection = "user";
  let isSaving = false;
  let isDeleting = false;
  let isImporting = false;
  let fileInputEl = null;
  let errorMessage = "";
  let showDocPicker = false;
  let importedDeviceDoc = false;
  let deviceMemoryId = "";
  let cardPreview = "";
  let isFolder = false;
  let isImage = false;

  // Face tagging state
  let markPeopleActive = false;
  let detectingFaces = false;
  let detectedFaces = [];
  let knownPeople = [];
  let selectedFaceIdx = null;
  let faceNameInput = "";
  let isSavingFace = false;
  let isDrawingMode = false;
  let isDrawing = false;
  let drawStartX = 0;
  let drawStartY = 0;
  let drawCurrentBox = null;
  let previewWrapEl = null;
  let previewImgEl = null;
  let imgNaturalW = 0;
  let imgNaturalH = 0;
  let stageEl = null;

  let draggingFaceIdx = null;
  let dragOffset = { x: 0, y: 0 };
  let dragStartBbox = null;

  let resizingFaceIdx = null;
  let resizeHandleType = null;
  let resizeStartBbox = null;
  let resizeStartPoint = { x: 0, y: 0 };

  const IMAGE_EXTS = new Set(["jpg", "jpeg", "png", "webp", "gif", "avif", "heic", "heif", "svg", "bmp", "tiff"]);
  const NON_IMAGE_EXTS = new Set([
    "pdf", "docx", "doc", "odt", "rtf", "xlsx", "xls", "csv", "pptx", "ppt",
    "txt", "md", "markdown", "html", "htm", "css", "js", "ts", "json", "xml",
    "yaml", "yml", "py", "cpp", "c", "h", "sh", "log", "zip", "tar", "gz", "rar"
  ]);

  function isImageCard(c) {
    if (!c || c.is_folder || c.isFolder || c.contentType === "folder" || c.sType === "folder") return false;
    if (c.contentType === "image" || (c.mimeType && c.mimeType.startsWith("image/"))) return true;
    const path = (c.document_id || c.title || "").toLowerCase();
    const ext = path.split(".").pop();
    if (IMAGE_EXTS.has(ext)) return true;
    if (NON_IMAGE_EXTS.has(ext)) return false;
    if (c.hasIcon || c.has_icon || c.customAttributes?.hasIcon) return true;
    return !!(c.imagePreview || c.image_preview);
  }

  const popularTagSuggestions = [
    "rag",
    "docs",
    "notes",
    "project",
    "p2p",
    "rules",
    "privacy",
  ];

  $: if (open) {
    initFromCard(card);
  }

  function initFromCard(c) {
    errorMessage = "";
    isSaving = false;
    isDeleting = false;
    isImporting = false;
    tagInput = "";
    showDocPicker = false;
    importedDeviceDoc = false;
    deviceMemoryId = "";
    cardPreview = "";
    isFolder = false;
    isImage = false;
    markPeopleActive = false;
    detectingFaces = false;
    detectedFaces = [];
    selectedFaceIdx = null;
    isDrawingMode = false;
    isDrawing = false;
    drawCurrentBox = null;

    if (c) {
      isFolder = !!(c.is_folder || c.isFolder || c.contentType === "folder" || c.sType === "folder");
      isImage = !isFolder && isImageCard(c);
      if (c.onDevice) {
        // Редактируем on-device карточку: обновляем ChromaDB, GunDB не трогаем
        importedDeviceDoc = true;
        deviceMemoryId = c.memory_id || c._id || "";
      }
      title = c.title || "";
      text = c.text || c.content || "";
      documentId = c.document_id || "";
      collection = c.collection || "user";

      // Показываем превью ТОЛЬКО если карточка действительно картинка или имеет флаг hasIcon/hasPreview/cover
      const canHavePreview = isImage || !!(c.hasIcon || c.has_icon || c.customAttributes?.hasIcon || (isFolder && (c.hasPreview || c.has_preview || c.hasCover || c.customAttributes?.cover)));
      if (canHavePreview) {
        const highRes = resolveHighResPreview(documentId, 1600);
        if (highRes) {
          cardPreview = highRes;
        } else {
          cardPreview = c.imagePreview || c.image_preview || "";
        }
        if (!cardPreview && c.onDevice && c.document_id && thumbFetcher && (c.hasPreview || isImage || c.hasIcon || c.has_icon)) {
          thumbFetcher(c.document_id, 1200)
            .then((url) => {
              if (url && cardPreview !== url) cardPreview = url;
            })
            .catch(() => {});
        }
      } else {
        cardPreview = "";
      }

      if (c.tags) {
        tags = Array.isArray(c.tags)
          ? [...c.tags]
          : String(c.tags).split(",").map((t) => t.trim()).filter(Boolean);
      } else {
        tags = [];
      }

      if (documentId.startsWith("http://") || documentId.startsWith("https://")) {
        kind = "link";
      } else if (documentId) {
        kind = "document";
      } else {
        kind = "note";
      }
    } else {
      title = "";
      text = "";
      kind = kindIntent || "note";
      documentId = "";
      tags = [];
      collection = "user";
    }
  }

  function resolveHighResPreview(docPath, size = 1600) {
    if (!docPath) return "";
    if (docPath.startsWith("http://") || docPath.startsWith("https://")) return docPath;
    if (docPath.startsWith("/")) {
      // document_id несёт устройство в себе (/beelink/Data/f.jpg): сегмент /p2p/
      // и путь внутри шары резолвим по первому сегменту, иначе — old behavior.
      const parts = docPath.split("/").filter(Boolean);
      let seg = "";
      let relPath = docPath;
      if (parts.length > 1) {
        const d = deviceRegistry.getByPrefix(parts[0]);
        if (d) {
          seg = (d.prefix || d.linkId || d.name || "").replace(/^\/+|\/+$/g, "");
          relPath = "/" + parts.slice(1).join("/");
        }
      }
      if (!seg) {
        seg = (device?.prefix || device?.linkId || targetLinkId || "").replace(/^\/+|\/+$/g, "");
        if (!seg) {
          try {
            const first = deviceRegistry.list().find((d) => d.drive && d.drive.isConnected) || deviceRegistry.list()[0];
            if (first) seg = (first.prefix || first.linkId || "").replace(/^\/+|\/+$/g, "");
          } catch (_) {}
        }
        seg = seg || "local";
        relPath = docPath.startsWith("/") ? docPath : "/" + docPath;
      }
      return `/p2p/${seg}${encodeURI(relPath)}?resize=true&width=${size}&height=${size}`;
    }
    return "";
  }

  function onPreviewImgError() {
    if (cardPreview && cardPreview.startsWith("/p2p/") && thumbFetcher && documentId && (isImage || card?.hasPreview || card?.hasIcon || card?.has_icon)) {
      thumbFetcher(documentId, 1200)
        .then((url) => {
          if (url) cardPreview = url;
          else cardPreview = card?.imagePreview || card?.image_preview || "";
        })
        .catch(() => {
          cardPreview = card?.imagePreview || card?.image_preview || "";
        });
    } else {
      cardPreview = card?.imagePreview || card?.image_preview || "";
    }
  }

  function onImageLoaded(e) {
    if (e?.target) {
      imgNaturalW = e.target.naturalWidth || e.target.width;
      imgNaturalH = e.target.naturalHeight || e.target.height;
    }
  }

  async function toggleMarkPeople() {
    if (markPeopleActive) {
      markPeopleActive = false;
      detectedFaces = [];
      selectedFaceIdx = null;
      isDrawingMode = false;
      return;
    }
    if (!httpDrive) return;
    markPeopleActive = true;
    detectingFaces = true;
    selectedFaceIdx = null;

    try {
      const peopleRes = await httpDrive.fetch("/api/faces/people").catch(() => null);
      if (peopleRes && peopleRes.ok) {
        const pd = await peopleRes.json();
        knownPeople = pd.people || [];
      }

      const body = documentId
        ? { path: documentId, document_id: documentId }
        : { image_b64: cardPreview };

      const res = await httpDrive.fetch("/api/faces/detect", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });

      if (!res.ok) {
        throw new Error(`Face detection failed (${res.status})`);
      }

      const data = await res.json();
      detectedFaces = (data.faces || []).map((f, i) => ({
        ...f,
        id: f.face_id || `detected_${i}`,
      }));

      if (detectedFaces.length > 0 && detectedFaces[0].img_width) {
        imgNaturalW = detectedFaces[0].img_width;
        imgNaturalH = detectedFaces[0].img_height;
      }
    } catch (err) {
      console.warn("[MemoryCardModal] detect faces error:", err);
      errorMessage = err?.message || "Failed to detect faces";
    } finally {
      detectingFaces = false;
    }
  }

  function selectFace(idx) {
    selectedFaceIdx = idx;
    faceNameInput = detectedFaces[idx]?.name || "";
  }

  async function saveFaceName(idx) {
    if (!faceNameInput.trim() || !httpDrive) return;
    isSavingFace = true;
    const face = detectedFaces[idx];
    const name = faceNameInput.trim();

    try {
      const payload = {
        name,
        path: documentId || "",
        document_id: documentId || "",
        bbox: face.bbox,
        image_b64: !documentId ? cardPreview : "",
        person_id: face.person_id || undefined,
      };

      const res = await httpDrive.fetch("/api/faces/register", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      });

      if (!res.ok) throw new Error("Failed to register face");
      const saved = await res.json();

      detectedFaces[idx].name = name;
      detectedFaces[idx].person_id = saved.person?.person_id || face.person_id;
      detectedFaces = [...detectedFaces];

      // Добавляем тег к карточке в интерфейсе
      if (!tags.includes(name)) {
        tags = [...tags, name];
      }

      selectedFaceIdx = null;

      // Обновляем список известных людей
      const pRes = await httpDrive.fetch("/api/faces/people").catch(() => null);
      if (pRes && pRes.ok) {
        const pd = await pRes.json();
        knownPeople = pd.people || [];
      }
    } catch (e) {
      console.error("[MemoryCardModal] register face error:", e);
      errorMessage = e.message || "Failed to save face name";
    } finally {
      isSavingFace = false;
    }
  }

  // Рисование области вручную (Drag BBox)
  function startDrawBox(e) {
    if (!isDrawingMode || !stageEl) return;
    const rect = stageEl.getBoundingClientRect();
    const clientX = e.touches ? e.touches[0].clientX : e.clientX;
    const clientY = e.touches ? e.touches[0].clientY : e.clientY;
    drawStartX = Math.max(0, Math.min(rect.width, clientX - rect.left));
    drawStartY = Math.max(0, Math.min(rect.height, clientY - rect.top));
    isDrawing = true;
    drawCurrentBox = { left: drawStartX, top: drawStartY, width: 0, height: 0 };
  }

  function moveDrawBox(e) {
    if (!isDrawing || !stageEl) return;
    const rect = stageEl.getBoundingClientRect();
    const clientX = e.touches ? e.touches[0].clientX : e.clientX;
    const clientY = e.touches ? e.touches[0].clientY : e.clientY;
    const curX = Math.max(0, Math.min(rect.width, clientX - rect.left));
    const curY = Math.max(0, Math.min(rect.height, clientY - rect.top));

    const left = Math.min(drawStartX, curX);
    const top = Math.min(drawStartY, curY);
    const width = Math.abs(curX - drawStartX);
    const height = Math.abs(curY - drawStartY);
    drawCurrentBox = { left, top, width, height };
  }

  function endDrawBox() {
    if (!isDrawing) return;
    isDrawing = false;
    if (drawCurrentBox && drawCurrentBox.width > 15 && drawCurrentBox.height > 15 && stageEl) {
      const rect = stageEl.getBoundingClientRect();
      const effW = imgNaturalW || rect.width;
      const effH = imgNaturalH || rect.height;

      const scaleX = effW / rect.width;
      const scaleY = effH / rect.height;

      const px1 = Math.round(drawCurrentBox.left * scaleX);
      const py1 = Math.round(drawCurrentBox.top * scaleY);
      const px2 = Math.round((drawCurrentBox.left + drawCurrentBox.width) * scaleX);
      const py2 = Math.round((drawCurrentBox.top + drawCurrentBox.height) * scaleY);

      const newFace = {
        id: `manual_${Date.now()}`,
        bbox: [px1, py1, px2, py2],
        score: 1.0,
        name: null,
        person_id: null,
        img_width: effW,
        img_height: effH,
      };

      detectedFaces = [...detectedFaces, newFace];
      isDrawingMode = false;
      selectFace(detectedFaces.length - 1);
    }
    drawCurrentBox = null;
  }

  // Перемещение (drag) существующей рамки лица
  function startDragFace(e, idx) {
    if (isDrawingMode) return;
    e.stopPropagation();
    selectFace(idx);

    const clientX = e.touches ? e.touches[0].clientX : e.clientX;
    const clientY = e.touches ? e.touches[0].clientY : e.clientY;

    draggingFaceIdx = idx;
    dragStartBbox = [...detectedFaces[idx].bbox];
    dragOffset = { x: clientX, y: clientY };

    window.addEventListener("mousemove", onFaceMouseMove);
    window.addEventListener("mouseup", onFaceMouseUp);
    window.addEventListener("touchmove", onFaceMouseMove);
    window.addEventListener("touchend", onFaceMouseUp);
  }

  function onFaceMouseMove(e) {
    if (draggingFaceIdx === null || !dragStartBbox || !stageEl) return;
    const clientX = e.touches ? e.touches[0].clientX : e.clientX;
    const clientY = e.touches ? e.touches[0].clientY : e.clientY;

    const dx = clientX - dragOffset.x;
    const dy = clientY - dragOffset.y;

    const rect = stageEl.getBoundingClientRect();
    if (!rect.width || !rect.height) return;

    const face = detectedFaces[draggingFaceIdx];
    const effW = face.img_width || imgNaturalW || rect.width;
    const effH = face.img_height || imgNaturalH || rect.height;

    const naturalDx = Math.round(dx * (effW / rect.width));
    const naturalDy = Math.round(dy * (effH / rect.height));

    const bw = dragStartBbox[2] - dragStartBbox[0];
    const bh = dragStartBbox[3] - dragStartBbox[1];

    let newX1 = dragStartBbox[0] + naturalDx;
    let newY1 = dragStartBbox[1] + naturalDy;

    newX1 = Math.max(0, Math.min(effW - bw, newX1));
    newY1 = Math.max(0, Math.min(effH - bh, newY1));
    let newX2 = newX1 + bw;
    let newY2 = newY1 + bh;

    detectedFaces[draggingFaceIdx].bbox = [newX1, newY1, newX2, newY2];
    detectedFaces = [...detectedFaces];
  }

  function onFaceMouseUp() {
    draggingFaceIdx = null;
    dragStartBbox = null;
    window.removeEventListener("mousemove", onFaceMouseMove);
    window.removeEventListener("mouseup", onFaceMouseUp);
    window.removeEventListener("touchmove", onFaceMouseMove);
    window.removeEventListener("touchend", onFaceMouseUp);
  }

  // Изменение размера рамки лица за угловые маркеры
  function startResizeFace(e, idx, handleType) {
    e.stopPropagation();
    resizingFaceIdx = idx;
    resizeHandleType = handleType;
    resizeStartBbox = [...detectedFaces[idx].bbox];

    const clientX = e.touches ? e.touches[0].clientX : e.clientX;
    const clientY = e.touches ? e.touches[0].clientY : e.clientY;
    resizeStartPoint = { x: clientX, y: clientY };

    window.addEventListener("mousemove", onResizeMouseMove);
    window.addEventListener("mouseup", onResizeMouseUp);
    window.addEventListener("touchmove", onResizeMouseMove);
    window.addEventListener("touchend", onResizeMouseUp);
  }

  function onResizeMouseMove(e) {
    if (resizingFaceIdx === null || !resizeStartBbox || !stageEl) return;
    const clientX = e.touches ? e.touches[0].clientX : e.clientX;
    const clientY = e.touches ? e.touches[0].clientY : e.clientY;

    const dx = clientX - resizeStartPoint.x;
    const dy = clientY - resizeStartPoint.y;

    const rect = stageEl.getBoundingClientRect();
    if (!rect.width || !rect.height) return;

    const face = detectedFaces[resizingFaceIdx];
    const effW = face.img_width || imgNaturalW || rect.width;
    const effH = face.img_height || imgNaturalH || rect.height;

    const naturalDx = Math.round(dx * (effW / rect.width));
    const naturalDy = Math.round(dy * (effH / rect.height));

    let [x1, y1, x2, y2] = resizeStartBbox;
    const minSize = 20;

    if (resizeHandleType.includes("w")) {
      x1 = Math.min(x2 - minSize, Math.max(0, x1 + naturalDx));
    }
    if (resizeHandleType.includes("e")) {
      x2 = Math.max(x1 + minSize, Math.min(effW, x2 + naturalDx));
    }
    if (resizeHandleType.includes("n")) {
      y1 = Math.min(y2 - minSize, Math.max(0, y1 + naturalDy));
    }
    if (resizeHandleType.includes("s")) {
      y2 = Math.max(y1 + minSize, Math.min(effH, y2 + naturalDy));
    }

    detectedFaces[resizingFaceIdx].bbox = [x1, y1, x2, y2];
    detectedFaces = [...detectedFaces];
  }

  function onResizeMouseUp() {
    resizingFaceIdx = null;
    resizeHandleType = null;
    resizeStartBbox = null;
    window.removeEventListener("mousemove", onResizeMouseMove);
    window.removeEventListener("mouseup", onResizeMouseUp);
    window.removeEventListener("touchmove", onResizeMouseMove);
    window.removeEventListener("touchend", onResizeMouseUp);
  }

  function removeFaceBox(idx) {
    if (idx === null || !detectedFaces[idx]) return;
    detectedFaces.splice(idx, 1);
    detectedFaces = [...detectedFaces];
    selectedFaceIdx = null;
  }

  onDestroy(() => {
    onFaceMouseUp();
    onResizeMouseUp();
  });

  function addTag(tag) {
    const clean = tag.trim().replace(/^#+/, "").toLowerCase();
    if (!clean) return;
    if (!tags.includes(clean)) {
      tags = [...tags, clean];
    }
    tagInput = "";
  }

  function handleTagKeydown(e) {
    if (e.key === "Enter" || e.key === ",") {
      e.preventDefault();
      addTag(tagInput);
    }
  }

  function removeTag(tagToRemove) {
    tags = tags.filter((t) => t !== tagToRemove);
  }

  // Owner mode: использование определяет подсказку по WSS-каналу (HttpDrive).
  // Только явный флаг: `!== false` проходил для undefined и давал гостю
  // публичной ноды private-доступ.
  function isOwnerMode() {
    return isPrivateMode === true || (isPrivateMode === undefined && device?.isOwner === true);
  }

  // Импорт документа через AI-ноду:
  // - Владелец: встроенный DocumentPicker (файл уже лежит на ноде) →
  //   POST /rag/import/local → чанки+карточка в ChromaDB (то же, что /learn).
  // - Гость: системный Select file → аплоад сырых байт по P2P (HttpDrive) →
  //   нода конвертит локально (pdftotext/pandoc) и возвращает stateless payload
  //   для персональной GunDB-карточки.
  function handleStartImport() {
    errorMessage = "";
    if (isImporting) return;
    if (isOwnerMode()) {
      showDocPicker = true;
    } else if (fileInputEl) {
      fileInputEl.click();
    }
  }

  function handleFileSelected(e) {
    const file = e.target?.files?.[0] || null;
    // сбрасываем value, чтобы повторный выбор того же файла тоже сработал
    e.target.value = "";
    if (file) runImport(file);
  }

  async function handleDeviceDocSelected(e) {
    showDocPicker = false;
    const realPath = e?.detail?.realPath || e?.detail?.path || "";
    if (!realPath) return;
    if (!httpDrive) {
      errorMessage = $_("storage_unavailable", {}, "AI node is not reachable (no httpDrive)");
      return;
    }
    isImporting = true;
    errorMessage = "";
    try {
      const res = await httpDrive.fetch("/rag/import/local", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Accept: "application/json",
        },
        body: JSON.stringify({
          source: realPath,
          tags: [...tags],
          scope: "private",
        }),
      });
      const data = await res.json().catch(() => null);
      if (!res.ok) {
        const msg = (data && (data.detail || data.error?.message || data.error)) || `Import failed (${res.status})`;
        throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
      }

      // Заполняем карточку результатом импорта
      title = data.title || realPath.split("/").pop() || "";
      text = data.annotation || "";
      documentId = data.path || realPath;
      deviceMemoryId = data.memoryId || "";
      importedDeviceDoc = !!deviceMemoryId;
      tags = [...new Set([...(tags || []), ...(data.tags || [])])];
      kind = "document";
      dispatch("imported", { docId: documentId, source: realPath, memoryId: deviceMemoryId });
    } catch (err) {
      errorMessage = err.message || "Import failed";
      console.error("[MemoryCardModal] device import error:", err);
    } finally {
      isImporting = false;
    }
  }

  async function runImport(file) {
    if (!httpDrive) {
      errorMessage = $_("storage_unavailable", {}, "AI node is not reachable (no httpDrive)");
      return;
    }

    isImporting = true;
    errorMessage = "";
    // владелец ищет по своей базе (scope=all/private), гость — только по public
    const ownerMode = isPrivateMode === true || (isPrivateMode === undefined && device?.isOwner === true);
    const ownerName = (typeof window !== "undefined" && window.__omdProfile?.userName) || "anonymous";
    const scope = ownerMode ? "private" : "public";

    try {
      // Аплоад сырых байтов, нода конвертит и векторизует сама
      const data = await importUploadedFile(httpDrive, file, {
        tags: [...tags],
        scope,
        owner: ownerName,
      });

      // Заполняем карточку результатом импорта
      title = data.title || file.name || "";
      text = data.annotation || "";
      documentId = file.name;
      const mergedTags = [...new Set([...(tags || []), ...(data.tags || [])])];
      tags = mergedTags;
      console.info("[MemoryCardModal] import tags:", mergedTags.join(", "));
      kind = "document";
      dispatch("imported", { docId: data.documentId || "", source: file.name });
    } catch (e) {
      errorMessage = e.message || "Import failed";
      console.error("[MemoryCardModal] RagImport error:", e);
    } finally {
      isImporting = false;
    }
  }

  function close() {
    open = false;
    dispatch("close");
  }

  function isClickablePath(p) {
    if (isViewOnly) return false;
    return !!p && (p.startsWith("/") || p.startsWith("http://") || p.startsWith("https://"));
  }

  function handleOpenFile() {
    if (!isClickablePath(documentId)) return;
    if (isFolder) {
      dispatch("open-folder", {
        path: documentId,
        id: documentId,
        title: title || documentId.split("/").pop(),
      });
    } else {
      dispatch("open-file", {
        path: documentId,
        id: documentId,
        title: title || documentId.split("/").pop(),
      });
    }
    close();
  }

  async function handleSave() {
    if (!text.trim()) {
      errorMessage = $_("card_text_required", {}, "Memory text or summary cannot be empty");
      return;
    }
    if (importedDeviceDoc && deviceMemoryId && !httpDrive) {
      errorMessage = $_("storage_unavailable", {}, "AI node is not reachable (no httpDrive)");
      return;
    }
    if (!importedDeviceDoc && !chatStore) {
      errorMessage = $_("storage_unavailable", {}, "Storage is not ready");
      return;
    }

    isSaving = true;
    errorMessage = "";

    try {
      if (importedDeviceDoc && deviceMemoryId) {
        // Владелец: карточка уже создана/обогащена в ChromaDB при импорте —
        // обновляем on-device карточку, GunDB-дубль не создаём.
        const res = await httpDrive.fetch(
          `/api/device_memory/${encodeURIComponent(deviceMemoryId)}`,
          {
            method: "PUT",
            headers: {
              "Content-Type": "application/json",
              Accept: "application/json",
            },
            body: JSON.stringify({ title: title.trim(), text: text.trim(), tags: [...tags] }),
          }
        );
        const d = await res.json().catch(() => null);
        if (!res.ok) {
          const msg = (d && (d.detail || d.error?.message || d.error)) || `Update failed (${res.status})`;
          throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
        }
        dispatch("save", {
          _id: deviceMemoryId,
          memory_id: deviceMemoryId,
          document_id: documentId,
          title: title.trim(),
          text: text.trim(),
          tags: [...tags],
          onDevice: true,
        });
        close();
        return;
      }

      let finalDocId = documentId.trim();
      if (kind === "note" && !finalDocId) {
        finalDocId = "user:note:" + Date.now();
      }

      const memo = {
        _id: card?._id || card?.memory_id || undefined,
        memory_id: card?.memory_id || card?._id || undefined,
        title: title.trim() || (finalDocId ? finalDocId.split("/").pop() : "Note"),
        text: text.trim(),
        document_id: finalDocId,
        tags: [...tags],
        collection,
        relevance: "contextual",
        created: card?.created || new Date().toISOString(),
      };

      const saved = await chatStore.addKnowledge(memo);
      dispatch("save", saved);
      close();
    } catch (e) {
      errorMessage = e.message || "Failed to save memory card";
    } finally {
      isSaving = false;
    }
  }

  async function handleDelete() {
    const id = card?._id || card?.memory_id;
    if (!id || !chatStore) return;

    if (!confirm($_("confirm_delete_card", {}, "Delete this Memory Card?"))) {
      return;
    }

    isDeleting = true;
    try {
      await chatStore.deleteKnowledge(id);
      dispatch("delete", id);
      close();
    } catch (e) {
      errorMessage = e.message || "Failed to delete card";
    } finally {
      isDeleting = false;
    }
  }
</script>

{#if open}
  <!-- svelte-ignore a11y-click-events-have-key-events -->
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div
    class="memory-modal-backdrop skin-chat"
    class:dark-theme={isDarkTheme}
    class:light-theme={!isDarkTheme}
    on:click|self={close}
  >
    <div class="memory-modal-card" role="dialog" aria-modal="true">
      <!-- Modal Header -->
      <div class="memory-modal-header">
        <div class="header-icon-box">
          {#if isFolder}
            📁
          {:else if isImage}
            🖼️
          {:else if kind === "document"}
            📄
          {:else if kind === "link"}
            🌐
          {:else}
            💡
          {/if}
        </div>
        <div class="header-titles">
          <h3>
            {#if isViewOnly}
              {$_("view_memory_card", {}, "Memory Card")}
            {:else if card}
              {$_("edit_memory_card", {}, "Edit Memory Card")}
            {:else}
              {$_("new_memory_card", {}, "New Memory Card")}
            {/if}
          </h3>
          <p class="header-sub">
            {$_("memory_card_sub", {}, "Knowledge extract, note, or document facts for RAG assistant")}
          </p>
        </div>
        <button type="button" class="close-btn" on:click={close} title={$_("close", {}, "Close")}>
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
            <line x1="18" y1="6" x2="6" y2="18" />
            <line x1="6" y1="6" x2="18" y2="18" />
          </svg>
        </button>
      </div>

      <!-- Modal Body -->
      <div class="memory-modal-body">
        {#if errorMessage}
          <div class="modal-error-banner">
            <span>⚠️ {errorMessage}</span>
          </div>
        {/if}

        <!-- Image Preview (recognized images) -->
        {#if cardPreview}
          <div class="form-group">
            <div class="preview-header-row">
              <label class="form-label">{$_("image_preview", {}, "Image")}</label>
              {#if isPrivateMode && !isViewOnly && isImage}
                <div class="face-actions-bar">
                  <button
                    type="button"
                    class="btn-mark-people"
                    class:active={markPeopleActive}
                    on:click={toggleMarkPeople}
                  >
                    {#if detectingFaces}
                      <span class="spin-icon">⏳</span>
                      <span>{$_("detecting_faces", {}, "Detecting...")}</span>
                    {:else if markPeopleActive}
                      <span>✕</span>
                      <span>{$_("hide_faces", {}, "Close marks")}</span>
                    {:else}
                      <span class="icon">👤</span>
                      <span>{$_("mark_people", {}, "Mark people")}</span>
                    {/if}
                  </button>

                  {#if markPeopleActive}
                    <button
                      type="button"
                      class="btn-draw-face"
                      class:active={isDrawingMode}
                      on:click={() => (isDrawingMode = !isDrawingMode)}
                      title={$_("draw_face_box_hint", {}, "Click & drag on photo to add a face")}
                    >
                      <span>➕ {isDrawingMode ? $_("cancel_draw", {}, "Cancel draw") : $_("add_box", {}, "Add region")}</span>
                    </button>
                  {/if}
                </div>
              {/if}
            </div>

            <div class="card-preview-wrap">
              <div
                class="image-stage-wrap"
                class:drawing-mode={isDrawingMode}
                bind:this={stageEl}
                on:mousedown={startDrawBox}
                on:mousemove={moveDrawBox}
                on:mouseup={endDrawBox}
                on:touchstart={startDrawBox}
                on:touchmove={moveDrawBox}
                on:touchend={endDrawBox}
              >
                <img
                  bind:this={previewImgEl}
                  class="card-preview-img"
                  src={cardPreview}
                  alt={title || "Image"}
                  loading="lazy"
                  on:load={onImageLoaded}
                  on:error={onPreviewImgError}
                />

                <!-- Face Overlay Layer -->
                {#if markPeopleActive}
                  <div class="face-overlay-layer">
                    {#each detectedFaces as face, idx}
                      {@const effW = face.img_width || imgNaturalW || (stageEl?.clientWidth || 1)}
                      {@const effH = face.img_height || imgNaturalH || (stageEl?.clientHeight || 1)}
                      {@const leftPct = ((face.bbox[0] / effW) * 100)}
                      {@const topPct = ((face.bbox[1] / effH) * 100)}
                      {@const widthPct = (((face.bbox[2] - face.bbox[0]) / effW) * 100)}
                      {@const heightPct = (((face.bbox[3] - face.bbox[1]) / effH) * 100)}
                      {@const isNamed = !!face.name}
                      {@const isSelected = selectedFaceIdx === idx}

                      <!-- svelte-ignore a11y-click-events-have-key-events -->
                      <!-- svelte-ignore a11y-no-static-element-interactions -->
                      <div
                        class="face-box"
                        class:named={isNamed}
                        class:selected={isSelected}
                        class:dragging={draggingFaceIdx === idx}
                        style="left: {leftPct}%; top: {topPct}%; width: {widthPct}%; height: {heightPct}%;"
                        on:mousedown={(e) => startDragFace(e, idx)}
                        on:touchstart={(e) => startDragFace(e, idx)}
                        on:click|stopPropagation={() => selectFace(idx)}
                      >
                        {#if isSelected}
                          <div
                            class="face-handle nw"
                            on:mousedown|stopPropagation={(e) => startResizeFace(e, idx, 'nw')}
                            on:touchstart|stopPropagation={(e) => startResizeFace(e, idx, 'nw')}
                          />
                          <div
                            class="face-handle ne"
                            on:mousedown|stopPropagation={(e) => startResizeFace(e, idx, 'ne')}
                            on:touchstart|stopPropagation={(e) => startResizeFace(e, idx, 'ne')}
                          />
                          <div
                            class="face-handle se"
                            on:mousedown|stopPropagation={(e) => startResizeFace(e, idx, 'se')}
                            on:touchstart|stopPropagation={(e) => startResizeFace(e, idx, 'se')}
                          />
                          <div
                            class="face-handle sw"
                            on:mousedown|stopPropagation={(e) => startResizeFace(e, idx, 'sw')}
                            on:touchstart|stopPropagation={(e) => startResizeFace(e, idx, 'sw')}
                          />
                        {/if}

                        <div class="face-badge" class:named={isNamed}>
                          {#if isNamed}
                            <span class="badge-icon">✓</span> {face.name}
                          {:else}
                            <span class="badge-icon">+</span> {$_("name_person", {}, "Name person")}
                          {/if}
                        </div>
                      </div>
                    {/each}

                    <!-- Currently drawing box -->
                    {#if isDrawing && drawCurrentBox}
                      <div
                        class="face-box-drawing"
                        style="left: {drawCurrentBox.left}px; top: {drawCurrentBox.top}px; width: {drawCurrentBox.width}px; height: {drawCurrentBox.height}px;"
                      ></div>
                    {/if}
                  </div>
                {/if}
              </div>
            </div>

            <!-- Face Name Editor Popover / Bar when a face is selected -->
            {#if markPeopleActive && selectedFaceIdx !== null && detectedFaces[selectedFaceIdx]}
              <div class="face-edit-panel" in:fade={{ duration: 150 }}>
                <div class="face-edit-header">
                  <span class="face-edit-title">
                    👤 {$_("tag_person", {}, "Tag person on photo")}
                  </span>
                  <button type="button" class="btn-close-face-panel" on:click={() => (selectedFaceIdx = null)}>✕</button>
                </div>

                <div class="face-input-row">
                  <input
                    type="text"
                    class="styled-input face-name-input"
                    placeholder={$_("person_name_ph", {}, "Enter name (e.g. Natasha)")}
                    bind:value={faceNameInput}
                    on:keydown={(e) => { if (e.key === "Enter") saveFaceName(selectedFaceIdx); }}
                    autofocus
                  />
                  <button
                    type="button"
                    class="btn-save-face"
                    disabled={isSavingFace || !faceNameInput.trim()}
                    on:click={() => saveFaceName(selectedFaceIdx)}
                  >
                    {isSavingFace ? "..." : $_("save", {}, "Save")}
                  </button>
                  <button
                    type="button"
                    class="btn-delete-face"
                    title={$_("delete", {}, "Delete")}
                    on:click={() => removeFaceBox(selectedFaceIdx)}
                  >
                    🗑️
                  </button>
                </div>

                <!-- Suggestions from existing known people -->
                {#if knownPeople.length > 0}
                  <div class="known-people-chips">
                    <span class="chips-hint">{$_("suggestions", {}, "Known")}:</span>
                    {#each knownPeople as kp}
                      <button
                        type="button"
                        class="kp-chip"
                        class:selected={faceNameInput === kp.name}
                        on:click={() => { faceNameInput = kp.name; saveFaceName(selectedFaceIdx); }}
                      >
                        {kp.name}
                      </button>
                    {/each}
                  </div>
                {/if}
              </div>
            {/if}
          </div>
        {/if}

        <!-- Title Field -->
        <div class="form-group">
          <label class="form-label" for="mem-title">{$_("card_title", {}, "Title")}</label>
          <input
            id="mem-title"
            type="text"
            class="styled-input"
            readonly={isViewOnly}
            placeholder={kind === "document"
              ? $_("title_doc_ph", {}, "e.g. Project Architecture Summary")
              : $_("title_note_ph", {}, "e.g. User Preferences or Fact")}
            bind:value={title}
          />
        </div>

        <!-- Source / Document Name (filled by upload) -->
        {#if kind === "document"}
          <div class="form-group">
            <label class="form-label" for="mem-source">
              {isFolder
                ? $_("folder_path", {}, "Folder Path")
                : isImage
                  ? $_("photo_path", {}, "Photo Path")
                  : $_("document_path", {}, "Document Path")}
            </label>
            <div class="input-with-action">
              <input
                id="mem-source"
                type="text"
                class="styled-input"
                readonly={isViewOnly}
                placeholder={$_("import_to_fill", {}, "Import a file to fill this in")}
                value={cleanDisplayPath(documentId)}
                on:input={(e) => { if (!isViewOnly) documentId = e.target.value; }}
              />
              {#if documentId && isClickablePath(documentId)}
                <button
                  type="button"
                  class="btn-open-file"
                  title={isFolder
                    ? $_("open_folder", {}, "Open Folder")
                    : isImage
                      ? $_("open_photo", {}, "Open Photo")
                      : $_("open_file", {}, "Open Document")}
                  on:click={handleOpenFile}
                >
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6" />
                    <polyline points="15 3 21 3 21 9" />
                    <line x1="10" y1="14" x2="21" y2="3" />
                  </svg>
                  <span>{$_("open", {}, "Open")}</span>
                </button>
              {/if}
            </div>

            {#if !isViewOnly && !isFolder && !isImage}
              <button
                type="button"
                class="btn-import-doc"
                disabled={isImporting}
                on:click={handleStartImport}
              >
                {#if isImporting}
                  <span class="spinner-dot"></span>
                  {$_("importing", {}, "Importing & vectorizing…")}
                {:else}
                  <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                    <polyline points="17 8 12 3 7 8" />
                    <line x1="12" y1="3" x2="12" y2="15" />
                  </svg>
                  {$_("import_document", {}, "Import & Vectorize")}
                {/if}
              </button>

              <!-- Системный file picker: байты файла аплоадятся на ноду напрямую -->
              <input
                type="file"
                class="file-input-hidden"
                accept={FILE_INPUT_ACCEPT}
                bind:this={fileInputEl}
                on:change={handleFileSelected}
              />
            {/if}
          </div>
        {:else if kind === "link"}
          <div class="form-group">
            <label class="form-label" for="mem-link">
              {$_("source_url", {}, "Source URL")}
            </label>
            <input
              id="mem-link"
              type="url"
              class="styled-input"
              readonly={isViewOnly}
              placeholder="https://example.com/article"
              bind:value={documentId}
            />
          </div>
        {/if}

        <!-- Text / Summary Field -->
        <div class="form-group">
          <label class="form-label" for="mem-text">
            {isFolder
              ? $_("folder_summary", {}, "Folder Summary & Description")
              : isImage
                ? $_("photo_summary", {}, "Photo Description & Caption")
                : kind === "document"
                  ? $_("doc_summary", {}, "Document Summary & Extracts")
                  : $_("note_content", {}, "Fact / Note Content")}
            <span class="required-star">*</span>
          </label>
          <textarea
            id="mem-text"
            class="styled-textarea"
            rows="6"
            readonly={isViewOnly}
            placeholder={isFolder
              ? $_("folder_summary_ph", {}, "Folder description, overview of contents, notes…")
              : isImage
                ? $_("photo_summary_ph", {}, "Photo description, location, subject…")
                : $_("mem_text_ph", {}, "Write the essential facts, rule, or extract that the AI should know…")}
            bind:value={text}
          ></textarea>
        </div>

        <!-- Tag Cloud Manager -->
        <div class="form-group">
          <label class="form-label">
            {$_("tag_cloud", {}, "Tag Cloud")}
            {#if !isViewOnly}
              <span class="label-hint">({$_("press_enter_to_add", {}, "press Enter to add")})</span>
            {/if}
          </label>
          
          <div class="tag-editor-box">
            <div class="active-tags-cloud">
              {#each tags as t}
                <span class="tag-chip active">
                  #{t}
                  {#if !isViewOnly}
                    <button type="button" class="tag-remove-btn" on:click={() => removeTag(t)}>×</button>
                  {/if}
                </span>
              {/each}
              {#if !isViewOnly}
                <input
                  type="text"
                  class="tag-inline-input"
                  placeholder={tags.length === 0 ? $_("add_tag_ph", {}, "Add tags (e.g. rag, work)…") : "+ tag"}
                  bind:value={tagInput}
                  on:keydown={handleTagKeydown}
                />
              {/if}
            </div>
          </div>

          {#if !isViewOnly}
            <!-- Suggested Quick Tags -->
            <div class="suggested-tags-row">
              <span class="suggest-label">{$_("suggestions", {}, "Suggestions")}:</span>
              {#each popularTagSuggestions as sug}
                {#if !tags.includes(sug)}
                  <button
                    type="button"
                    class="tag-chip suggest"
                    on:click={() => addTag(sug)}
                  >
                    +{sug}
                  </button>
                {/if}
              {/each}
            </div>
          {/if}
        </div>
      </div>

      <!-- Modal Footer -->
      <div class="memory-modal-footer">
        {#if !isViewOnly && card}
          <button
            type="button"
            class="btn-delete"
            disabled={isDeleting || isSaving}
            on:click={handleDelete}
          >
            {#if isDeleting}
              {$_("deleting", {}, "Deleting…")}
            {:else}
              {$_("delete", {}, "Delete Card")}
            {/if}
          </button>
        {:else}
          <div></div>
        {/if}

        <div class="footer-right-actions">
          <button type="button" class="btn-cancel" on:click={close}>
            {isViewOnly ? $_("close", {}, "Close") : $_("cancel", {}, "Cancel")}
          </button>
          {#if !isViewOnly}
            <button
              type="button"
              class="btn-save"
              disabled={isSaving || isImporting || !text.trim()}
              on:click={handleSave}
            >
              {#if isSaving}
                {$_("saving", {}, "Saving…")}
              {:else}
                {$_("save_card", {}, "Save Memory Card")}
              {/if}
            </button>
          {/if}
        </div>
      </div>
    </div>

    {#if showDocPicker}
      <DocumentPicker
        {device}
        title={$_("select_document", {}, "Select Document to Learn")}
        on:select={handleDeviceDocSelected}
        on:cancel={() => (showDocPicker = false)}
      />
    {/if}
  </div>
{/if}

<style>
  .memory-modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.65);
    z-index: 1000;
    display: flex;
    align-items: center;
    justify-content: center;
    padding-top: calc(16px + env(safe-area-inset-top, 0px));
    padding-bottom: calc(16px + env(safe-area-inset-bottom, 0px));
    padding-left: calc(16px + env(safe-area-inset-left, 0px));
    padding-right: calc(16px + env(safe-area-inset-right, 0px));
    box-sizing: border-box;
    animation: fadeIn 0.18s ease;
  }

  @keyframes fadeIn {
    from { opacity: 0; }
    to { opacity: 1; }
  }

  .memory-modal-card {
    width: 100%;
    max-width: 580px;
    background: var(--bg-secondary);
    border: 1px solid var(--border-color);
    border-radius: 18px;
    box-shadow: 0 24px 64px rgba(0, 0, 0, 0.4);
    display: flex;
    flex-direction: column;
    overflow: hidden;
    color: var(--text-primary);
    animation: slideUp 0.22s cubic-bezier(0.16, 1, 0.3, 1);
  }

  @keyframes slideUp {
    from { transform: translateY(14px) scale(0.97); }
    to { transform: translateY(0) scale(1); }
  }

  .memory-modal-header {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 16px 20px;
    border-bottom: 1px solid var(--border-color);
    background: var(--bg-secondary);
  }

  .header-icon-box {
    width: 36px;
    height: 36px;
    border-radius: 10px;
    background: var(--accent-blue-active);
    border: 1px solid var(--accent-blue);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1.1rem;
    flex-shrink: 0;
  }

  .header-titles {
    flex: 1;
    min-width: 0;
  }

  .header-titles h3 {
    margin: 0;
    font-size: 1.05rem;
    font-weight: 600;
    color: var(--text-primary);
  }

  .header-sub {
    margin: 2px 0 0;
    font-size: 0.78rem;
    color: var(--text-muted);
  }

  .close-btn {
    background: transparent;
    border: none;
    color: var(--text-muted);
    padding: 6px;
    border-radius: 8px;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.15s ease;
  }

  .close-btn:hover {
    color: var(--text-primary);
    background: var(--bg-card-hover);
  }

  .memory-modal-body {
    padding: 18px 20px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    max-height: 70vh;
    overflow-y: auto;
  }

  .modal-error-banner {
    background: rgba(239, 68, 68, 0.12);
    border: 1px solid rgba(239, 68, 68, 0.4);
    color: var(--text-primary);
    padding: 8px 12px;
    border-radius: 8px;
    font-size: 0.82rem;
  }

  .form-group {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .form-label {
    font-size: 0.8rem;
    font-weight: 600;
    color: var(--text-secondary);
    display: flex;
    align-items: center;
    gap: 4px;
  }

  .required-star {
    color: #f87171;
  }

  .label-hint {
    font-size: 0.72rem;
    font-weight: 400;
    color: var(--text-muted);
  }

  .preview-header-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 6px;
  }

  .face-actions-bar {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .btn-mark-people {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 4px 10px;
    border-radius: 6px;
    font-size: 0.78rem;
    font-weight: 500;
    cursor: pointer;
    background: var(--accent-soft);
    color: var(--accent, #EC4899);
    border: 1px solid var(--accent-border);
    transition: all 0.18s ease;
  }
  .btn-mark-people:hover,
  .btn-mark-people.active {
    background: var(--accent, #EC4899);
    color: #ffffff;
    border-color: transparent;
  }

  .btn-draw-face {
    display: inline-flex;
    align-items: center;
    padding: 4px 8px;
    border-radius: 6px;
    font-size: 0.75rem;
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    cursor: pointer;
  }
  .btn-draw-face.active {
    background: #eab308;
    color: #000;
    border-color: #ca8a04;
  }

  .card-preview-wrap {
    display: flex;
    justify-content: center;
    align-items: center;
    background: rgba(0, 0, 0, 0.25);
    border-radius: 10px;
    overflow: hidden;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.08));
    position: relative;
    max-height: 480px;
    width: 100%;
  }

  .memory-modal-backdrop.light-theme .card-preview-wrap {
    background: #F4F3F7;
  }

  .image-stage-wrap {
    position: relative;
    display: inline-flex;
    max-width: 100%;
    max-height: 480px;
    user-select: none;
  }
  .image-stage-wrap.drawing-mode {
    cursor: crosshair;
  }

  .face-overlay-layer {
    position: absolute;
    inset: 0;
    pointer-events: auto;
  }

  .face-box {
    position: absolute;
    border: 2px dashed rgba(255, 255, 255, 0.85);
    background: rgba(0, 0, 0, 0.2);
    border-radius: 6px;
    cursor: grab;
    transition: border-color 0.15s ease, background 0.15s ease;
    z-index: 10;
  }
  .face-box:hover {
    border-color: #F472B6;
    background: rgba(244, 114, 182, 0.18);
    box-shadow: 0 0 12px rgba(244, 114, 182, 0.4);
  }
  .face-box.dragging {
    cursor: grabbing;
    opacity: 0.9;
  }
  .face-box.named {
    border-style: solid;
    border-color: #10b981;
    background: rgba(16, 185, 129, 0.18);
  }
  .face-box.selected {
    border-color: #EC4899;
    border-style: solid;
    background: rgba(236, 72, 153, 0.25);
    box-shadow: 0 0 16px rgba(236, 72, 153, 0.6);
  }

  .face-handle {
    position: absolute;
    width: 9px;
    height: 9px;
    background: #ffffff;
    border: 1.5px solid #EC4899;
    border-radius: 2px;
    z-index: 20;
    box-shadow: 0 1px 4px rgba(0, 0, 0, 0.3);
  }
  .face-handle.nw { top: -5px; left: -5px; cursor: nwse-resize; }
  .face-handle.ne { top: -5px; right: -5px; cursor: nesw-resize; }
  .face-handle.se { bottom: -5px; right: -5px; cursor: nwse-resize; }
  .face-handle.sw { bottom: -5px; left: -5px; cursor: nesw-resize; }

  .face-badge {
    position: absolute;
    bottom: -22px;
    left: 50%;
    transform: translateX(-50%);
    background: rgba(15, 10, 16, 0.9);
    backdrop-filter: blur(6px);
    color: #fff;
    font-size: 0.68rem;
    font-weight: 600;
    padding: 2px 7px;
    border-radius: 4px;
    white-space: nowrap;
    border: 1px solid rgba(255, 255, 255, 0.15);
    pointer-events: none;
  }
  .face-badge.named {
    background: #059669;
    border-color: #10b981;
  }

  .face-box-drawing {
    position: absolute;
    border: 2px dashed #f59e0b;
    background: rgba(245, 158, 11, 0.2);
    border-radius: 4px;
    pointer-events: none;
  }

  .face-edit-panel {
    margin-top: 8px;
    padding: 10px 12px;
    background: rgba(236, 72, 153, 0.08);
    border: 1px solid rgba(236, 72, 153, 0.25);
    border-radius: 8px;
  }
  .face-edit-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 6px;
    font-size: 0.78rem;
    font-weight: 600;
    color: var(--text-primary);
  }
  .btn-close-face-panel {
    background: none;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    font-size: 0.8rem;
  }
  .face-input-row {
    display: flex;
    gap: 8px;
  }
  .face-name-input {
    flex: 1;
    padding: 6px 10px;
    font-size: 0.82rem;
  }
  .btn-save-face {
    padding: 6px 14px;
    background: var(--accent, #EC4899);
    color: #fff;
    border: none;
    border-radius: 6px;
    font-size: 0.8rem;
    font-weight: 500;
    cursor: pointer;
  }
  .btn-save-face:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
  .known-people-chips {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
    margin-top: 8px;
  }
  .chips-hint {
    font-size: 0.72rem;
    color: var(--text-muted);
  }
  .kp-chip {
    padding: 2px 8px;
    border-radius: 12px;
    font-size: 0.72rem;
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    cursor: pointer;
    transition: all 0.15s ease;
  }
  .kp-chip:hover {
    border-color: var(--accent, #EC4899);
    background: var(--accent-soft);
  }
  .kp-chip.selected {
    background: var(--accent, #EC4899);
    color: #fff;
  }

  .btn-delete-face {
    padding: 0 10px;
    border-radius: 8px;
    border: 1px solid var(--border-color);
    background: transparent;
    cursor: pointer;
    font-size: 0.9rem;
    color: var(--text-muted);
    transition: all 0.15s ease;
    display: inline-flex;
    align-items: center;
    justify-content: center;
  }
  .btn-delete-face:hover {
    background: rgba(239, 68, 68, 0.15);
    border-color: rgba(239, 68, 68, 0.4);
    color: #ef4444;
  }

  .card-preview-img {
    display: block;
    max-width: 100%;
    max-height: 480px;
    width: auto;
    height: auto;
    object-fit: contain;
    user-select: none;
    -webkit-user-drag: none;
    pointer-events: none;
  }

  .styled-input {
    width: 100%;
    background: var(--bg-primary);
    border: 1px solid var(--border-color);
    border-radius: 10px;
    padding: 9px 12px;
    color: var(--text-primary);
    font-size: 0.86rem;
    font-family: inherit;
    outline: none;
    transition: border-color 0.15s ease;
    box-sizing: border-box;
  }

  .styled-input:focus {
    border-color: var(--accent-blue-hover);
  }

  .input-with-action {
    display: flex;
    gap: 8px;
    align-items: center;
  }

  .input-with-action .styled-input {
    flex: 1;
  }

  .btn-open-file {
    display: flex;
    align-items: center;
    gap: 5px;
    background: transparent;
    border: 1px solid var(--accent-blue);
    color: var(--accent-blue);
    padding: 8px 12px;
    border-radius: 8px;
    font-size: 0.8rem;
    font-weight: 500;
    cursor: pointer;
    white-space: nowrap;
    transition: all 0.15s ease;
  }

  .btn-open-file:hover {
    background: var(--accent-blue-active);
    color: var(--accent-blue);
  }

  .file-input-hidden {
    position: absolute;
    width: 0;
    height: 0;
    opacity: 0;
    pointer-events: none;
  }

  .btn-import-doc {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 7px;
    width: 100%;
    background: transparent;
    border: 1px dashed var(--accent-blue);
    color: var(--accent-blue);
    padding: 9px 12px;
    border-radius: 10px;
    font-size: 0.82rem;
    font-weight: 600;
    cursor: pointer;
    margin-top: 8px;
    transition: all 0.15s ease;
    box-sizing: border-box;
  }

  .btn-import-doc:hover:not(:disabled) {
    background: var(--accent-blue-active);
    color: var(--accent-blue);
  }

  .btn-import-doc:disabled {
    opacity: 0.6;
    cursor: not-allowed;
  }

  .spinner-dot {
    width: 12px;
    height: 12px;
    border: 2px solid transparent;
    border-top-color: var(--accent-blue);
    border-radius: 50%;
    animation: importSpin 0.7s linear infinite;
  }

  @keyframes importSpin {
    to { transform: rotate(360deg); }
  }

  .styled-textarea {
    width: 100%;
    background: var(--bg-primary);
    border: 1px solid var(--border-color);
    border-radius: 10px;
    padding: 10px 12px;
    color: var(--text-primary);
    font-size: 0.86rem;
    line-height: 1.5;
    font-family: inherit;
    outline: none;
    resize: vertical;
    box-sizing: border-box;
    transition: border-color 0.15s ease;
  }

  .styled-textarea:focus {
    border-color: var(--accent-blue-hover);
  }

  /* Tag Cloud Editor */
  .tag-editor-box {
    background: var(--bg-primary);
    border: 1px solid var(--border-color);
    border-radius: 10px;
    padding: 8px 10px;
    min-height: 42px;
    box-sizing: border-box;
  }

  .active-tags-cloud {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
  }

  .tag-chip {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    padding: 3px 8px;
    border-radius: 12px;
    font-size: 0.75rem;
    font-weight: 500;
    transition: all 0.15s ease;
  }

  .tag-chip.active {
    background: var(--accent-blue-active);
    border: 1px solid var(--accent-blue);
    color: var(--accent-blue);
  }

  .tag-remove-btn {
    background: transparent;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    font-size: 13px;
    line-height: 1;
    padding: 0 1px;
    display: flex;
    align-items: center;
  }

  .tag-remove-btn:hover {
    color: #f87171;
  }

  .tag-inline-input {
    background: transparent;
    border: none;
    outline: none;
    color: var(--text-primary);
    font-size: 0.82rem;
    font-family: inherit;
    flex: 1;
    min-width: 90px;
  }

  .suggested-tags-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 5px;
    margin-top: 2px;
  }

  .suggest-label {
    font-size: 0.72rem;
    color: var(--text-muted);
  }

  .tag-chip.suggest {
    background: var(--bg-card-hover);
    border: 1px solid var(--border-color);
    color: var(--text-muted);
    cursor: pointer;
    padding: 2px 7px;
    font-size: 0.72rem;
  }

  .tag-chip.suggest:hover {
    background: var(--accent-blue-active);
    border-color: var(--accent-blue);
    color: var(--accent-blue);
  }

  /* Modal Footer */
  .memory-modal-footer {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 20px;
    border-top: 1px solid var(--border-color);
    background: var(--bg-secondary);
  }

  .footer-right-actions {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .btn-cancel {
    background: transparent;
    border: 1px solid var(--border-color);
    color: var(--text-secondary);
    padding: 8px 16px;
    border-radius: 10px;
    font-size: 0.84rem;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .btn-cancel:hover {
    background: var(--bg-card-hover);
    color: var(--text-primary);
  }

  .btn-save {
    background: var(--accent-blue);
    border: none;
    color: #ffffff;
    padding: 8px 18px;
    border-radius: 10px;
    font-size: 0.84rem;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s ease;
  }

  .btn-save:hover:not(:disabled) {
    background: var(--accent-blue-hover);
  }

  .btn-save:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .btn-delete {
    background: rgba(239, 68, 68, 0.08);
    border: 1px solid rgba(239, 68, 68, 0.35);
    color: #ef4444;
    padding: 8px 14px;
    border-radius: 10px;
    font-size: 0.82rem;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .btn-delete:hover:not(:disabled) {
    background: rgba(239, 68, 68, 0.16);
    color: #dc2626;
  }

  .btn-delete:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
</style>
