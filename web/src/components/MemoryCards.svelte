<script>
  import { createEventDispatcher, onMount, onDestroy } from "svelte";
  import { fade, fly } from "svelte/transition";
  import { _ } from "../lib/i18n/index.js";
  import { deviceRegistry } from "../lib/device-registry.js";

  let innerWidth = typeof window !== "undefined" ? window.innerWidth : 1024;
  $: isMobile = innerWidth < 768;

  export let knowledgeList = [];
  export let isDarkTheme = true;
  export let isOverlayHeader = false;
  export let sidebarOpen = true;
  export let isOwner = false;
  export let loading = false; // on-device knowledge still loading
  export let thumbFetcher = null; // async (document_id, size) => objectURL|"" for onDevice previews
  export let httpDrive = null;
  export let targetDevice = null;
  export let targetLinkId = "";

  const dispatch = createEventDispatcher();

  let knownPeopleList = [];
  let loadingPeople = false;
  let scanStatus = null;
  let scanPollTimer = null;
  let scanningPersonId = "";

  async function loadKnownPeople() {
    if (!httpDrive) return;
    loadingPeople = true;
    try {
      const res = await httpDrive.fetch("/api/faces/people");
      if (res.ok) {
        const data = await res.json();
        knownPeopleList = data.people || [];
      }
    } catch (e) {
      console.warn("[MemoryCards] loadKnownPeople error:", e);
    } finally {
      loadingPeople = false;
    }
  }

  async function checkScanStatus() {
    if (!httpDrive) return;
    try {
      const res = await httpDrive.fetch("/api/faces/scan_status");
      if (res.ok) {
        const data = await res.json();
        scanStatus = data.scan || null;
        if (scanStatus && scanStatus.running) {
          if (!scanPollTimer) {
            scanPollTimer = setInterval(checkScanStatus, 2500);
          }
        } else {
          if (scanPollTimer) {
            clearInterval(scanPollTimer);
            scanPollTimer = null;
          }
          if (scanningPersonId) {
            scanningPersonId = "";
            loadKnownPeople();
            dispatch("refresh");
          }
        }
      }
    } catch (e) {
      if (scanPollTimer) {
        clearInterval(scanPollTimer);
        scanPollTimer = null;
      }
    }
  }

  async function startScanArchive(personId) {
    if (!httpDrive || (scanStatus && scanStatus.running)) return;
    scanningPersonId = personId;
    try {
      const res = await httpDrive.fetch("/api/faces/scan_archive", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ person_id: personId }),
      });
      if (res.ok) {
        checkScanStatus();
      }
    } catch (e) {
      console.error("[MemoryCards] scan_archive error:", e);
    }
  }

  async function deletePerson(personId) {
    if (!confirm($_("confirm_delete_person", {}, "Delete this person and their face records?"))) return;
    try {
      const res = await httpDrive.fetch(`/api/faces/people/${encodeURIComponent(personId)}`, {
        method: "DELETE",
      });
      if (res.ok) {
        loadKnownPeople();
      }
    } catch (e) {
      console.error("[MemoryCards] deletePerson error:", e);
    }
  }

  let selectedPersonPhotos = null;
  $: selectedPersonPhotosSet = selectedPersonPhotos && selectedPersonPhotos.length ? new Set(selectedPersonPhotos) : null;

  function filterPhotosByPerson(person) {
    selectedKindFilter = "image";
    selectedTagFilter = person.name;
    selectedPersonPhotos = person.photos || null;
    knowledgeSearch = "";
    knowledgeSearchInput = "";
    dispatch("refresh");
  }

  $: if (httpDrive) {
    loadKnownPeople();
    checkScanStatus();
  }

  let viewMode = typeof localStorage !== "undefined"
    ? localStorage.getItem("omd_memory_view") || "grid"
    : "grid"; // 'roll' | 'grid'

  let knowledgeSearch = "";
  // Debounce: тяжёлые фильтры/счётчики пересчитываем не на каждый символ.
  // Ввод биндится на knowledgeSearchInput, knowledgeSearch обновляется отложенно.
  let knowledgeSearchInput = "";
  let searchDebounceTimer = null;
  function onSearchInput(e) {
    knowledgeSearchInput = e?.target?.value ?? "";
    if (searchDebounceTimer) clearTimeout(searchDebounceTimer);
    searchDebounceTimer = setTimeout(() => {
      knowledgeSearch = knowledgeSearchInput;
      searchDebounceTimer = null;
    }, 250);
  }
  let selectedTagFilter = "";
  let selectedKindFilter = "all"; // 'all' | 'personal' | 'onDevice' | 'note' | 'document' | 'link'
  let showNewMenu = false;
  let showTagsMenu = false;
  let showScrollTop = false;
  let showScrollBottom = false;
  let scrollContainerEl = null;

  function handleWindowClick(e) {
    if (showNewMenu && !e.target?.closest?.(".new-card-menu-wrap")) {
      showNewMenu = false;
    }
    if (showTagsMenu && !e.target?.closest?.(".tags-menu-wrap")) {
      showTagsMenu = false;
    }
  }

  function handleWindowKeydown(e) {
    if (e.key === "Escape") {
      showNewMenu = false;
      showTagsMenu = false;
    }
  }

  function setViewMode(mode) {
    viewMode = mode;
    if (typeof localStorage !== "undefined") {
      localStorage.setItem("omd_memory_view", mode);
    }
  }

  function toggleTagFilter(tag) {
    selectedTagFilter = selectedTagFilter === tag ? "" : tag;
    selectedPersonPhotos = null;
    showTagsMenu = false;
  }

  let renderLimit = 80;

  function handleScroll() {
    if (!scrollContainerEl) return;
    const { scrollTop, scrollHeight, clientHeight } = scrollContainerEl;
    const distanceToBottom = scrollHeight - (scrollTop + clientHeight);
    showScrollTop = scrollTop > 160;
    showScrollBottom = distanceToBottom > 160;
    if (distanceToBottom < 1000 && renderLimit < filteredKnowledge.length) {
      renderLimit += 60;
    }
  }

  function scrollToTop(smooth = true) {
    if (scrollContainerEl) {
      scrollContainerEl.scrollTo({ top: 0, behavior: smooth ? "smooth" : "auto" });
      setTimeout(() => handleScroll(), 300);
    }
  }

  function scrollToBottom(smooth = true) {
    if (scrollContainerEl) {
      scrollContainerEl.scrollTo({ top: scrollContainerEl.scrollHeight, behavior: smooth ? "smooth" : "auto" });
      setTimeout(() => handleScroll(), 300);
    }
  }

  let cardImgFailed = {};
  function markCardImageFailed(item) {
    const key = item?.id || item?.document_id || item?.imagePreview;
    if (!key) return;
    if (!cardImgFailed[key]) {
      cardImgFailed[key] = true;
      cardImgFailed = { ...cardImgFailed };
      const docId = item?.document_id;
      if (docId && thumbFetcher && !thumbUrls[docId] && !thumbLoading.has(docId)) {
        thumbLoading.add(docId);
        thumbFetcher(docId, 480)
          .then((url) => {
            thumbUrls = { ...thumbUrls, [docId]: url || "" };
          })
          .catch(() => {
            thumbUrls = { ...thumbUrls, [docId]: "" };
          })
          .finally(() => thumbLoading.delete(docId));
      }
    }
  }

  function getDevSegment() {
    const dev = (targetDevice && targetDevice.drive) ? targetDevice : null;
    let seg = (dev?.prefix || dev?.linkId || targetLinkId || targetDevice?.linkId || targetDevice?.name || "").trim();
    if (!seg) {
      try {
        const first = deviceRegistry.list().find((d) => d.drive && d.drive.isConnected) || deviceRegistry.list()[0];
        if (first) {
          seg = first.prefix || first.linkId || "";
        }
      } catch (_) {}
    }
    seg = seg.replace(/^\/+|\/+$/g, "");
    return seg || "local";
  }

  // document_id теперь всегда полный: /beelink/Data/f.jpg — первый сегмент это
  // само устройство (hostname-префикс), поэтому сегмент /p2p/ и путь внутри
  // шары берём из него. Голые пути (без устройства) ведём как раньше.
  function previewSegAndPath(docId) {
    const parts = String(docId).split("/").filter(Boolean);
    if (parts.length > 1) {
      const dev = deviceRegistry.getByPrefix(parts[0]);
      if (dev) {
        const seg = (dev.prefix || dev.linkId || dev.name || "").replace(/^\/+|\/+$/g, "");
        if (seg) return { seg, path: "/" + parts.slice(1).join("/") };
      }
    }
    return {
      seg: getDevSegment(),
      path: docId.startsWith("/") ? docId : "/" + docId,
    };
  }

  function cardPreview(item, size = 480) {
    if (!item || !hasCardImage(item)) return "";
    const docId = item.document_id || item.path || "";
    const key = item.id || docId;
    if (cardImgFailed[key]) {
      return thumbUrls[docId] || item.imagePreview || item.image_preview || "";
    }
    if (isFolderCard(item)) {
      if (docId.startsWith("/")) {
        const { seg, path } = previewSegAndPath(docId);
        return `/p2p/${seg}${encodeURI(path)}?cover=${size}`;
      }
      return thumbUrls[docId] || item.imagePreview || item.image_preview || "";
    }
    if (docId.startsWith("http://") || docId.startsWith("https://")) {
      return docId;
    }
    if (docId.startsWith("/")) {
      const { seg, path } = previewSegAndPath(docId);
      return `/p2p/${seg}${encodeURI(path)}?resize=true&width=${size}&height=${size}`;
    }
    return thumbUrls[docId] || item.imagePreview || item.image_preview || "";
  }

  function hasCardImage(item) {
    if (!item) return false;
    if (isFolderCard(item)) {
      return !!(item.hasPreview || item.has_preview || item.hasCover || item.customAttributes?.cover || item.imagePreview || item.image_preview || thumbUrls[item.document_id]);
    }
    if (item.hasIcon || item.has_icon || item.customAttributes?.hasIcon) return true;
    return isImageCard(item);
  }

  // Ленивые on-device превью через thumbFetcher для фолбэков
  let thumbUrls = {};
  const thumbLoading = new Set();
  $: if (thumbFetcher && typeof thumbFetcher === "function") {
    for (const item of displayedKnowledge) {
      const docId = item?.document_id;
      if (!docId || !item.onDevice || thumbLoading.has(docId)) continue;
      if (thumbUrls[docId] !== undefined) continue;
      if (!hasCardImage(item)) continue;
      thumbLoading.add(docId);
      thumbFetcher(docId, 480)
        .then((url) => {
          thumbUrls = { ...thumbUrls, [docId]: url || "" };
        })
        .catch(() => {
          thumbUrls = { ...thumbUrls, [docId]: "" };
        })
        .finally(() => thumbLoading.delete(docId));
    }
  }

  $: knownPeopleNamesSet = new Set(knownPeopleList.map((p) => (p.name || "").trim().toLowerCase()).filter(Boolean));

  $: tagCounts = (() => {
    const counts = {};
    for (const k of knowledgeList) {
      for (const t of (k.tags || [])) {
        const clean = String(t).trim().toLowerCase();
        if (clean) counts[clean] = (counts[clean] || 0) + 1;
      }
    }
    return counts;
  })();

  $: allKnowledgeTags = (() => {
    const peopleTags = [];
    const otherTags = [];
    const seen = new Set();

    // 1. Теги известных людей всегда первыми
    for (const p of knownPeopleList) {
      const pName = (p.name || "").trim().toLowerCase();
      if (pName && !seen.has(pName)) {
        seen.add(pName);
        peopleTags.push({
          name: pName,
          displayName: p.name,
          count: tagCounts[pName] || p.photos_count || 0,
          isPerson: true,
        });
      }
    }
    peopleTags.sort((a, b) => b.count - a.count || a.displayName.localeCompare(b.displayName));

    // 2. Все остальные теги по частоте использования (популярные сначала)
    for (const [tag, count] of Object.entries(tagCounts)) {
      if (!seen.has(tag)) {
        seen.add(tag);
        otherTags.push({
          name: tag,
          displayName: tag,
          count,
          isPerson: false,
        });
      }
    }
    otherTags.sort((a, b) => b.count - a.count || a.name.localeCompare(b.name));

    // Объединяем: люди первыми, затем до 60 самых частых тегов
    const combined = [...peopleTags, ...otherTags.slice(0, 60)];
    if (selectedTagFilter && !combined.some((t) => t.name === selectedTagFilter.toLowerCase())) {
      combined.push({
        name: selectedTagFilter.toLowerCase(),
        displayName: selectedTagFilter,
        count: tagCounts[selectedTagFilter.toLowerCase()] || 1,
        isPerson: knownPeopleNamesSet.has(selectedTagFilter.toLowerCase()),
      });
    }
    return combined;
  })();

  const IMAGE_EXTS = new Set(["jpg", "jpeg", "png", "webp", "gif", "avif", "heic", "heif", "svg", "bmp", "tiff"]);
  const NON_IMAGE_EXTS = new Set([
    "pdf", "docx", "doc", "odt", "rtf", "xlsx", "xls", "csv", "pptx", "ppt",
    "txt", "md", "markdown", "html", "htm", "css", "js", "ts", "json", "xml",
    "yaml", "yml", "py", "cpp", "c", "h", "sh", "log", "zip", "tar", "gz", "rar"
  ]);

  function isDeviceCard(k) {
    return k.source === "device" || !!k.onDevice;
  }

  function isFolderCard(item) {
    if (!item) return false;
    return item.is_folder === true || item.isFolder === true || item.contentType === "folder" || item.sType === "folder";
  }

  function isImageCard(item) {
    if (!item || isFolderCard(item)) return false;
    if (item.contentType === "image" || (item.mimeType && item.mimeType.startsWith("image/"))) return true;
    const path = (item.document_id || item.title || "").toLowerCase();
    const ext = path.split(".").pop();
    if (IMAGE_EXTS.has(ext)) return true;
    if (NON_IMAGE_EXTS.has(ext)) return false;
    if (item.hasIcon || item.has_icon || item.customAttributes?.hasIcon) return true;
    return !!(item.imagePreview || item.image_preview);
  }

  $: notesCount = knowledgeList.filter((k) => !k.document_id || k.document_id.startsWith("user:note:")).length;
  $: foldersCount = knowledgeList.filter(isFolderCard).length;
  $: imagesCount = knowledgeList.filter(isImageCard).length;
  $: docsCount = knowledgeList.filter((k) => k.document_id && !k.document_id.startsWith("user:note:") && !k.document_id.startsWith("http") && !isFolderCard(k) && !isImageCard(k)).length;
  $: linksCount = knowledgeList.filter((k) => k.document_id && (k.document_id.startsWith("http://") || k.document_id.startsWith("https://"))).length;
  $: onDeviceCount = knowledgeList.filter((k) => k.onDevice).length;

  $: filteredKnowledge = knowledgeList
    .filter((k) => {
      const isOnDevice = isDeviceCard(k);
      const isFolder = isFolderCard(k);
      const isImage = isImageCard(k);
      const isDoc = k.document_id && !k.document_id.startsWith("user:note:") && !k.document_id.startsWith("http") && !isFolder && !isImage;
      const isLink = k.document_id && (k.document_id.startsWith("http://") || k.document_id.startsWith("https://"));
      const isNote = !isFolder && !isImage && !isDoc && !isLink;

      if (selectedKindFilter === "personal" && isOnDevice) return false;
      if (selectedKindFilter === "onDevice" && !isOnDevice) return false;
      if (selectedKindFilter === "folder" && !isFolder) return false;
      if (selectedKindFilter === "image" && !isImage) return false;
      if (selectedKindFilter === "document" && !isDoc) return false;
      if (selectedKindFilter === "note" && !isNote) return false;
      if (selectedKindFilter === "link" && !isLink) return false;

      if (selectedTagFilter) {
        const kTags = (k.tags || []).map((t) => String(t).toLowerCase());
        const matchesTag = kTags.includes(selectedTagFilter.toLowerCase());
        const matchesPhoto = selectedPersonPhotosSet && k.document_id && selectedPersonPhotosSet.has(k.document_id);
        if (!matchesTag && !matchesPhoto) return false;
      }

      if (!knowledgeSearch.trim()) return true;
      const q = knowledgeSearch.toLowerCase();
      const inTitle = k.title && k.title.toLowerCase().includes(q);
      const inText = (k.text || k.content) && (k.text || k.content).toLowerCase().includes(q);
      const inDoc = k.document_id && k.document_id.toLowerCase().includes(q);
      const inTags = (k.tags || []).some((t) => String(t).toLowerCase().includes(q));
      return inTitle || inText || inDoc || inTags;
    })
    .sort((a, b) => {
      // Папки всегда отображаются в самом начале списка
      const aFolder = isFolderCard(a) ? 0 : 1;
      const bFolder = isFolderCard(b) ? 0 : 1;
      if (aFolder !== bFolder) return aFolder - bFolder;

      const aTime = new Date(a.created || a.timestamp || 0).getTime();
      const bTime = new Date(b.created || b.timestamp || 0).getTime();
      if (aTime && bTime && aTime !== bTime) return bTime - aTime;
      return (a.document_id || a.title || "").localeCompare(b.document_id || b.title || "");
    });

  $: if (knowledgeSearch !== undefined || selectedTagFilter !== undefined || selectedKindFilter !== undefined) {
    renderLimit = 80;
  }

  $: displayedKnowledge = filteredKnowledge.slice(0, renderLimit);

  function formatCardDate(d) {
    if (!d) return "";
    try {
      const dt = new Date(isNaN(Number(d)) ? d : Number(d));
      if (isNaN(dt.getTime())) return "";
      return dt.toLocaleDateString(undefined, { month: "short", day: "numeric" });
    } catch {
      return "";
    }
  }

  function formatRelativeDate(d) {
    if (!d) return "";
    try {
      const dt = new Date(isNaN(Number(d)) ? d : Number(d));
      if (isNaN(dt.getTime())) return "";
      const now = new Date();
      const diffMs = now - dt;
      const diffMin = Math.floor(diffMs / 60000);
      if (diffMin < 1) return $_("just_now", {}, "just now");
      if (diffMin < 60) return `${diffMin}m`;
      const diffHours = Math.floor(diffMin / 60);
      if (diffHours < 24) return `${diffHours}h`;
      const diffDays = Math.floor(diffHours / 24);
      if (diffDays < 7) return `${diffDays}d`;
      return formatCardDate(d);
    } catch {
      return "";
    }
  }

  function formatDocPath(docId) {
    if (!docId) return "";
    let p = String(docId);
    if (!isOwner) {
      // Скрываем хост/имя устройства ноды для гостя (/beelink/Knowledge/OMD -> /Knowledge/OMD)
      p = p.replace(/^\/[^\/]+(?=\/)/, "");
    }
    return p;
  }

  function handleCardOpenFile(item) {
    if (isFolderCard(item) || item.readOnly || (!isOwner && item.onDevice)) {
      openEdit(item);
      return;
    }
    dispatch("open-file", {
      path: item.document_id,
      id: item.document_id,
      title: item.title || item.document_id.split("/").pop(),
    });
  }

  function openCreate(itemKind) {
    dispatch("create", { kind: itemKind || "note" });
  }

  function openIndexFolder() {
    dispatch("index-folder");
  }

  function openEdit(item) {
    dispatch("edit", { card: item });
  }

  function handleDelete(item) {
    dispatch("delete", { id: item._id || item.memory_id });
  }

  onMount(() => {
    if (typeof window !== "undefined" && window.innerWidth < 768) {
      setViewMode("grid");
    }
    loadKnownPeople();
    checkScanStatus();
  });

  onDestroy(() => {
    if (scanPollTimer) {
      clearInterval(scanPollTimer);
      scanPollTimer = null;
    }
  });
</script>

<svelte:window bind:innerWidth={innerWidth} on:click={handleWindowClick} on:keydown={handleWindowKeydown} />

<div class="memory-app skin-chat {isDarkTheme ? 'dark-theme' : 'light-theme'}" class:overlay-header={isOverlayHeader}>
  {#if loading}
    <div class="knowledge-loading-toast" in:fade={{ duration: 150 }} out:fade={{ duration: 150 }}>
      <div class="kl-spinner"></div>
      <span>{$_("loading_knowledge", {}, "Loading knowledge...")}</span>
    </div>
  {/if}

  <!-- Top Navigation Header -->
  <header class="memory-header">
    <div class="header-main-row">
      <div class="header-left">
      {#if !sidebarOpen}
        <button
          class="header-icon-btn sidebar-toggle-btn"
          title={$_("unfold", {}, "Expand sidebar")}
          on:click={() => dispatch("toggle-sidebar")}
        >
          <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
            <line x1="9" y1="3" x2="9" y2="21" />
          </svg>
        </button>
      {/if}

      <div class="header-title-box">
        <div class="header-title-row">
          <span class="memory-icon">🧠</span>
          <h1 class="header-title">{$_("memory_cards", {}, "Memory Cards")}</h1>
          <span class="count-pill">{knowledgeList.length}</span>
        </div>
      </div>    </div>

    <div class="header-right">
      <!-- Search Memories -->
      <div class="memory-search-wrap">
        <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2" class="search-icon-inside">
          <circle cx="11" cy="11" r="8" />
          <line x1="21" y1="21" x2="16.65" y2="16.65" />
        </svg>
        <input
          type="text"
          class="memory-search-input"
          placeholder={$_("search_memory_cards", {}, "Search memories, documents, or #tags…")}
          bind:value={knowledgeSearchInput}
          on:input={onSearchInput}
        />
        {#if knowledgeSearch}
          <button class="clear-search-btn" on:click={() => {
            knowledgeSearch = "";
            knowledgeSearchInput = "";
          }}>×</button>
        {/if}
      </div>

      <!-- Roll / Grid View Switcher (same as Creations) -->
      <!-- Feed / Grid switcher — desktop only -->
      {#if !isMobile}
      <div class="wx-modes">
        <div class="wx-segmented">
          <button
            type="button"
            class:wx-selected={viewMode === "roll"}
            title={$_("view_roll", {}, "Feed View")}
            on:click={() => setViewMode("roll")}
          >
            <i class="wx-icon wxi-view-sequential wx-only"></i>
          </button>
          <button
            type="button"
            class:wx-selected={viewMode === "grid"}
            title={$_("view_grid", {}, "Grid View")}
            on:click={() => setViewMode("grid")}
          >
            <i class="wx-icon wxi-view-grid wx-only"></i>
          </button>
        </div>
      </div>
      {/if}
      <!-- New Memory Card: plus + dropdown -->
      <div class="new-card-menu-wrap">
        <button
          class="header-icon-btn new-card-btn"
          title={$_("new_memory_card", {}, "New Memory Card")}
          on:click={() => {
            showNewMenu = !showNewMenu;
            if (showNewMenu) showTagsMenu = false;
          }}
        >
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
            <line x1="12" y1="5" x2="12" y2="19" />
            <line x1="5" y1="12" x2="19" y2="12" />
          </svg>
        </button>
        {#if showNewMenu}
          <div class="new-card-dropdown">
            <button type="button" class="menu-item" on:click={() => { showNewMenu = false; openCreate("note"); }}>
              <span class="menu-icon">💡</span>
              {$_("type_note", {}, "Note / Fact")}
            </button>
            <button type="button" class="menu-item" on:click={() => { showNewMenu = false; openCreate("link"); }}>
              <span class="menu-icon">🌐</span>
              {$_("type_link", {}, "Web Link")}
            </button>
            <button type="button" class="menu-item" on:click={() => { showNewMenu = false; openCreate("document"); }}>
              <svg class="menu-svg" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                <polyline points="17 8 12 3 7 8" />
                <line x1="12" y1="3" x2="12" y2="15" />
              </svg>
              {$_("menu_import_doc", {}, "Import & Vectorize doc")}
            </button>
            {#if isOwner}
              <div class="menu-separator"></div>
              <button type="button" class="menu-item" on:click={() => { showNewMenu = false; openIndexFolder(); }}>
                <svg class="menu-svg" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                  <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
                </svg>
                {$_("menu_index_folder", {}, "Index folder")}
              </button>
            {/if}
          </div>
        {/if}
      </div>

      <!-- Refresh Button -->
      <button class="header-icon-btn" title={$_("refresh", {}, "Refresh")} on:click={() => dispatch("refresh")}>
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
          <polyline points="23 4 23 10 17 10" />
          <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10" />
        </svg>
      </button>

      <!-- Close Button -->
      <button class="header-icon-btn close-btn" title={$_("close", {}, "Close")} on:click={() => dispatch("close")}>
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
          <line x1="18" y1="6" x2="6" y2="18" />
          <line x1="6" y1="6" x2="18" y2="18" />
        </svg>
      </button>
    </div>
    </div>
  </header>

  <!-- Filter zone above the cards: category pills & tags -->
  <div class="memory-toolbar">
    <div class="category-pills-row">
        <button
          class="cat-pill"
          class:active={selectedKindFilter === "all"}
          on:click={() => (selectedKindFilter = "all")}
        >
          {$_("filter_all", {}, "All")} ({knowledgeList.length})
        </button>
        <button
          class="cat-pill"
          class:active={selectedKindFilter === "personal"}
          on:click={() => (selectedKindFilter = "personal")}
        >
          👤 {$_("filter_personal", {}, "Personal")}
        </button>
        {#if foldersCount > 0}
          <button
            class="cat-pill"
            class:active={selectedKindFilter === "folder"}
            on:click={() => (selectedKindFilter = "folder")}
          >
            📁 {$_("filter_folders", {}, "Folders")} ({foldersCount})
          </button>
        {/if}
        {#if imagesCount > 0}
          <button
            class="cat-pill"
            class:active={selectedKindFilter === "image"}
            on:click={() => (selectedKindFilter = "image")}
          >
            🖼️ {$_("filter_images", {}, "Photos")} ({imagesCount})
          </button>
        {/if}
        {#if isOwner || knownPeopleList.length > 0}
          <button
            class="cat-pill"
            class:active={selectedKindFilter === "people"}
            on:click={() => { selectedKindFilter = "people"; loadKnownPeople(); }}
          >
            👥 {$_("filter_people", {}, "Known people")} ({knownPeopleList.length})
          </button>
        {/if}
        <button
          class="cat-pill"
          class:active={selectedKindFilter === "document"}
          on:click={() => (selectedKindFilter = "document")}
        >
          📄 {$_("filter_docs", {}, "Docs")} ({docsCount})
        </button>
        <button
          class="cat-pill"
          class:active={selectedKindFilter === "note"}
          on:click={() => (selectedKindFilter = "note")}
        >
          💡 {$_("filter_notes", {}, "Notes")} ({notesCount})
        </button>
        {#if linksCount > 0}
          <button
            class="cat-pill"
            class:active={selectedKindFilter === "link"}
            on:click={() => (selectedKindFilter = "link")}
          >
            🌐 {$_("filter_links", {}, "Links")} ({linksCount})
          </button>
        {/if}
        {#if onDeviceCount > 0}
          <button
            class="cat-pill"
            class:active={selectedKindFilter === "onDevice"}
            on:click={() => (selectedKindFilter = "onDevice")}
          >
            🖥️ {$_("filter_on_device", {}, "On-device")} ({onDeviceCount})
          </button>
        {/if}

        {#if allKnowledgeTags.length > 0}
          <div class="tags-menu-wrap">
            <button
              type="button"
              class="cat-pill tags-pill"
              class:active={!!selectedTagFilter}
              on:click={() => {
                showTagsMenu = !showTagsMenu;
                if (showTagsMenu) showNewMenu = false;
              }}
            >
              <span class="tags-pill-icon">#</span>
              {$_("tags", {}, "Tags")}
              {#if selectedTagFilter}
                <span class="tags-pill-selected">#{selectedTagFilter}</span>
              {/if}
              <svg
                class="tags-chevron"
                class:rotated={showTagsMenu}
                viewBox="0 0 24 24"
                width="12"
                height="12"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
              >
                <polyline points="6 9 12 15 18 9" />
              </svg>
            </button>
            {#if showTagsMenu}
              <div class="tags-dropdown" in:fly={{ y: 6, duration: 140 }}>
                <div class="tags-dropdown-head">
                  <span class="tags-cloud-label">{$_("tags", {}, "Tags")}</span>
                  {#if selectedTagFilter}
                    <button type="button" class="tag-clear-all" on:click={() => { selectedTagFilter = ""; showTagsMenu = false; }}>
                      {$_("reset", {}, "Reset")}
                    </button>
                  {/if}
                </div>
                <div class="tags-dropdown-cloud">
                  {#each allKnowledgeTags as tagItem}
                    <button
                      type="button"
                      class="tag-cloud-chip"
                      class:person-tag-chip={tagItem.isPerson}
                      class:active={selectedTagFilter && selectedTagFilter.toLowerCase() === tagItem.name}
                      on:click={() => toggleTagFilter(tagItem.displayName)}
                      title={tagItem.isPerson ? $_("filter_people", {}, "Known person") : ""}
                    >
                      {#if tagItem.isPerson}<span class="tag-person-icon">👤</span>{/if}#{tagItem.displayName}
                      {#if tagItem.count > 0}
                        <span class="tag-count-badge">{tagItem.count}</span>
                      {/if}
                      {#if selectedTagFilter && selectedTagFilter.toLowerCase() === tagItem.name}
                        <span class="clear-tag-x">×</span>
                      {/if}
                    </button>
                  {/each}
                </div>
              </div>
            {/if}
          </div>
        {/if}
      </div>
    </div>

  <!-- Scrollable Memory Cards Area -->
  <div
    class="memory-feed"
    bind:this={scrollContainerEl}
    on:scroll={handleScroll}
  >

    {#if selectedKindFilter === "people"}
      <!-- People Gallery View -->
      <div class="people-gallery-wrap" in:fade={{ duration: 180 }}>
        {#if scanStatus && scanStatus.running}
          {@const scanPct = scanStatus.total ? Math.round((scanStatus.processed / scanStatus.total) * 100) : 0}
          <div class="scan-progress-banner">
            <div class="scan-circular-progress">
              <svg class="scan-progress-svg" viewBox="0 0 36 36">
                <path
                  class="scan-circle-bg"
                  d="M18 2.0845 a 15.9155 15.9155 0 0 1 0 31.831 a 15.9155 15.9155 0 0 1 0 -31.831"
                />
                <path
                  class="scan-circle-fill"
                  stroke-dasharray="{scanPct}, 100"
                  d="M18 2.0845 a 15.9155 15.9155 0 0 1 0 31.831 a 15.9155 15.9155 0 0 1 0 -31.831"
                />
              </svg>
              <span class="scan-pct-label">{scanPct}%</span>
            </div>
            <div class="scan-text-wrap">
              <span class="scan-title">{$_("scanning_archive", {}, "Scanning photo archive for faces...")}</span>
              <span class="scan-sub">
                {$_("processed", {}, "Processed")}: {scanStatus.processed} / {scanStatus.total}
                {#if scanStatus.matched > 0}
                  <strong class="scan-matched-count">({scanStatus.matched} {$_("matches_found", {}, "found")})</strong>
                {/if}
              </span>
            </div>
            <div class="scan-progress-bar">
              <div class="scan-bar-fill" style="width: {scanPct}%;"></div>
            </div>
          </div>
        {/if}

        {#if loadingPeople}
          <div class="people-loading-state">
            <span class="spin-icon">⏳</span>
            <span>{$_("loading_people", {}, "Loading known people...")}</span>
          </div>
        {:else if knownPeopleList.length === 0}
          <div class="empty-memory-state" in:fade={{ duration: 200 }}>
            <div class="empty-memory-glow">
              <span style="font-size: 3rem;">👥</span>
            </div>
            <h2 class="empty-title">{$_("no_known_people", {}, "No known people yet")}</h2>
            <p class="empty-desc">
              {$_("no_people_desc", {}, "Open any photo in Knowledge and click 'Mark people' to tag faces.")}
            </p>
            <button class="ghost-reset-btn" on:click={() => (selectedKindFilter = "image")}>
              🖼️ {$_("view_photos", {}, "View photos")}
            </button>
          </div>
        {:else}
          <div class="people-grid">
            {#each knownPeopleList as person (person.person_id)}
              <div class="person-card" in:fly={{ y: 12, duration: 200 }}>
                <div class="person-avatar-wrap">
                  {#if person.avatar_b64}
                    <img class="person-avatar-img" src={person.avatar_b64} alt={person.name} />
                  {:else}
                    <div class="person-avatar-fallback">👤</div>
                  {/if}
                </div>

                <div class="person-info">
                  <h4 class="person-name">{person.name}</h4>
                  <span class="person-photos-count">
                    📷 {person.photos_count} {$_("photos_count_label", {}, "photos")}
                  </span>
                </div>

                <div class="person-actions">
                  <button
                    type="button"
                    class="btn-person-action primary"
                    on:click={() => filterPhotosByPerson(person)}
                    title={$_("show_person_photos", {}, "Show photos")}
                  >
                    🔍 {$_("show_photos", {}, "Photos")}
                  </button>
                  {#if isOwner}
                    <div class="person-sub-actions">
                      <button
                        type="button"
                        class="btn-person-action secondary"
                        class:running={scanStatus?.running && scanStatus?.person_id === person.person_id}
                        disabled={scanStatus?.running}
                        on:click={() => startScanArchive(person.person_id)}
                        title={$_("scan_person_archive_hint", {}, "Find this person across all photos")}
                      >
                        ⚡ {$_("scan_archive_btn", {}, "Find more")}
                      </button>
                      <button
                        type="button"
                        class="btn-person-delete"
                        on:click={() => deletePerson(person.person_id)}
                        title={$_("delete_person", {}, "Delete person")}
                      >
                        🗑️
                      </button>
                    </div>
                  {/if}
                </div>
              </div>
            {/each}
          </div>
        {/if}
      </div>
    {:else if !knowledgeList.length && !loading}
      <!-- Empty State -->
      <div class="empty-memory-state" in:fade={{ duration: 250 }}>
        <div class="empty-memory-glow">
          <svg viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="currentColor" stroke-width="1.5">
            <path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20" />
            <path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z" />
          </svg>
        </div>
        <h2 class="empty-title">{$_("no_memory_cards", {}, "No Memory Cards Found")}</h2>
        <p class="empty-desc">
          {$_("empty_memory_desc", {}, "Add a memory card or index documents to ground your assistant's answers.")}
        </p>
        <div class="empty-actions">
          <button class="empty-action-btn primary" on:click={() => openCreate("note")}>
            <span class="btn-emoji">💡</span>
            <span>{$_("type_note", {}, "Note / Fact")}</span>
          </button>
          <button class="empty-action-btn secondary" on:click={() => openCreate("link")}>
            <span class="btn-emoji">🌐</span>
            <span>{$_("type_link", {}, "Web Link")}</span>
          </button>
          <button class="empty-action-btn secondary" on:click={() => openCreate("document")}>
            <svg class="btn-svg" viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
              <polyline points="17 8 12 3 7 8" />
              <line x1="12" y1="3" x2="12" y2="15" />
            </svg>
            <span>{$_("menu_import_doc", {}, "Import & Vectorize doc")}</span>
          </button>
          {#if isOwner}
            <button class="empty-action-btn secondary" on:click={() => openIndexFolder()}>
              <svg class="btn-svg" viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
              </svg>
              <span>{$_("menu_index_folder", {}, "Index folder")}</span>
            </button>
          {/if}
        </div>
      </div>
    {:else if !filteredKnowledge.length}
      <!-- No Filter Matches -->
      <div class="empty-memory-state" in:fade={{ duration: 250 }}>
        <div class="empty-memory-glow">
          <svg viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="currentColor" stroke-width="1.5">
            <circle cx="11" cy="11" r="8" />
            <line x1="21" y1="21" x2="16.65" y2="16.65" />
          </svg>
        </div>
        <h2 class="empty-title">{$_("no_memory_cards", {}, "No Memory Cards Found")}</h2>
        <p class="empty-desc">
          {$_("no_filter_matches", {}, "Try clearing search or filters to see all cards.")}
        </p>
        <button class="ghost-reset-btn" on:click={() => { knowledgeSearch = ""; knowledgeSearchInput = ""; selectedTagFilter = ""; selectedKindFilter = "all"; selectedPersonPhotos = null; }}>
          {$_("reset_filters", {}, "Reset filters")}
        </button>
      </div>
    {:else}
      <!-- Memory Cards Container (Feed / Grid) -->
      <div class="memory-container {viewMode}">
        {#each displayedKnowledge as item (item._id || item.memory_id)}
          {@const isFolderItem = isFolderCard(item)}
          {@const isImageItem = isImageCard(item)}
          {@const isDoc = item.document_id && !item.document_id.startsWith("user:note:") && !item.document_id.startsWith("http") && !isFolderItem && !isImageItem}
          {@const isLink = item.document_id && (item.document_id.startsWith("http://") || item.document_id.startsWith("https://"))}
          {@const kind = isFolderItem ? "folder" : isImageItem ? "image" : isDoc ? "doc" : isLink ? "link" : "note"}
          {@const isReadOnlyCard = item.readOnly || (!isOwner && item.onDevice)}
          {@const sourceClickable = isLink || (!isReadOnlyCard && ((!isFolderItem && isDoc && item.document_id.startsWith("/")) || (isImageItem && item.document_id.startsWith("/"))))}
          {@const kindLabel = isFolderItem ? $_("folder_short", {}, "Folder") : isImageItem ? $_("image_short", {}, "Photo") : isDoc ? $_("doc_short", {}, "Doc") : isLink ? $_("link_short", {}, "Link") : $_("note_short", {}, "Note")}
          <!-- svelte-ignore a11y-click-events-have-key-events -->
          <!-- svelte-ignore a11y-no-static-element-interactions -->
          <div
            class="memory-card"
            class:kind-{kind}
            role="button"
            tabindex="0"
            on:click={() => openEdit(item)}
            on:keydown={(e) => (e.key === "Enter" || e.key === " ") && openEdit(item)}
            in:fly={{ y: 15, duration: 250 }}
          >
            <!-- Card Header Bar -->
            <div class="memory-card-header">
              <span class="card-kind-label">
                {#if isFolderItem}
                  <svg class="kind-icon-svg" viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
                  </svg>
                  {kindLabel}
                {:else if isImageItem}
                  <svg class="kind-icon-svg" viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                    <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
                    <circle cx="8.5" cy="8.5" r="1.5" />
                    <polyline points="21 15 16 10 5 21" />
                  </svg>
                  {kindLabel}
                {:else if isDoc}
                  <svg class="kind-icon-svg" viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" />
                    <polyline points="14 2 14 8 20 8" />
                  </svg>
                  {kindLabel}
                {:else if isLink}
                  <svg class="kind-icon-svg" viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71" />
                    <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71" />
                  </svg>
                  {kindLabel}
                {:else}
                  💡 {kindLabel}
                {/if}
              </span>
              {#if item.onDevice}
                <span class="card-device-badge" title={$_("on_device_card", {}, "Stored on device (AI node index)")}>🖥️</span>
              {/if}
              <span class="card-time">{formatRelativeDate(item.created || item.timestamp)}</span>
              {#if !isReadOnlyCard}
                <div class="card-top-actions">
                  <button
                    class="card-top-btn"
                    title={$_("edit_card", {}, "Edit card in modal")}
                    on:click|stopPropagation={() => openEdit(item)}
                  >
                    <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                      <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7" />
                      <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z" />
                    </svg>
                  </button>
                  <button
                    class="card-top-btn delete-btn"
                    title={$_("delete_card", {}, "Delete memory card")}
                    on:click|stopPropagation={() => handleDelete(item)}
                  >
                    <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
                      <polyline points="3 6 5 6 21 6" />
                      <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                    </svg>
                  </button>
                </div>
              {/if}
            </div>

            <!-- Card Content -->
            <div class="memory-card-body">
              {#if isFolderItem}
                {#if hasCardImage(item)}
                  <div class="card-image-preview-wrap card-folder-cover-wrap">
                    <img
                      class="card-image-preview"
                      src={cardPreview(item, 360)}
                      alt={item.title || item.document_id || "Folder cover"}
                      loading="lazy"
                      on:error={() => markCardImageFailed(item)}
                    />
                  </div>
                {:else}
                  <div class="card-image-preview-wrap card-folder-wrap">
                    <svg class="card-folder-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.4">
                      <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
                    </svg>
                  </div>
                {/if}
              {:else if hasCardImage(item)}
                {#if cardImgFailed[item.id || item.document_id] && !thumbUrls[item?.document_id] && !item.imagePreview && !item.image_preview}
                  <div class="card-image-preview-wrap image-on-another-device">
                    <span class="another-device-hint">{$_("on_another_device", {}, "(on another device)")}</span>
                  </div>
                {:else}
                  <div class="card-image-preview-wrap">
                    <img
                      class="card-image-preview"
                      src={cardPreview(item, 480)}
                      alt={item.title || item.document_id || "Image"}
                      loading="lazy"
                      on:error={() => markCardImageFailed(item)}
                    />
                  </div>
                {/if}
              {/if}
              {#if item.title || (item.document_id && !item.document_id.startsWith("user:note:"))}
                <h4 class="card-title" title={item.title || formatDocPath(item.document_id) || "Memory Card"}>
                  {item.title || (item.document_id ? formatDocPath(item.document_id).split("/").pop() : "")}
                </h4>
              {/if}

              {#if item.document_id && !item.document_id.startsWith("user:note:")}
                {#if sourceClickable}
                  <!-- svelte-ignore a11y-click-events-have-key-events -->
                  <!-- svelte-ignore a11y-no-static-element-interactions -->
                  <div class="card-source-bar clickable" role="button" tabindex="0" on:click|stopPropagation={() => handleCardOpenFile(item)} on:keydown={(e) => (e.key === "Enter") && handleCardOpenFile(item)} title={formatDocPath(item.document_id)}>
                    {#if isFolderItem}
                      <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                        <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
                      </svg>
                    {:else if isImageItem}
                      <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                        <rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
                        <circle cx="8.5" cy="8.5" r="1.5" />
                        <polyline points="21 15 16 10 5 21" />
                      </svg>
                    {:else if isLink}
                      <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                        <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71" />
                        <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71" />
                      </svg>
                    {:else}
                      <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                        <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" />
                        <polyline points="14 2 14 8 20 8" />
                      </svg>
                    {/if}
                    <span class="source-text-link">{formatDocPath(item.document_id)}</span>
                    <span class="source-arrow-icon">↗</span>
                  </div>
                {:else}
                  <div class="card-source-bar static" title={$_("personal_doc_hint", {}, "Imported file — source not linked to your storage")}>
                    <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
                      <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" />
                      <polyline points="14 2 14 8 20 8" />
                    </svg>
                    <span class="source-text-link">{formatDocPath(item.document_id)}</span>
                  </div>
                {/if}
              {/if}

              {#if item.text || item.content}
                {@const rawText = String(item.text || item.content || "").trim()}
                {@const stripped = rawText.replace(/^#+\s+[^\n]+\n*/, "").trim()}
                {@const preview = stripped || (rawText.replace(/^#+\s*/, "").trim().toLowerCase() !== (item.title || "").trim().toLowerCase() ? rawText.replace(/^#+\s*/, "").trim() : "")}
                {#if preview && !/^\d+\s+chunks$/.test(preview)}
                  <p class="card-summary-preview">{preview}</p>
                {/if}
              {:else if item.document_id && !item.document_id.startsWith("user:note:")}
                <p class="card-summary-preview card-preview-path">{formatDocPath(item.document_id)}</p>
              {/if}

              {#if (item.tags && item.tags.length) || item.chunks}
                <div class="card-footer-row">
                  <div class="card-tags">
                    {#each (item.tags || []) as tag}
                      <span class="card-tag-chip">#{String(tag)}</span>
                    {/each}
                  </div>
                  {#if item.chunks}
                    <span class="card-chunks" title={item.chunks}>{$_("chunks_count", { count: item.chunks }, `${item.chunks} chunks`)}</span>
                  {/if}
                </div>
              {/if}
            </div>
          </div>
        {/each}
      </div>
    {/if}

    {#if showScrollTop}
      <button class="scroll-fab top" title={$_("scroll_top", {}, "Scroll to top")} on:click={() => scrollToTop(true)}>
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
          <polyline points="18 15 12 9 6 15" />
        </svg>
      </button>
    {/if}
    {#if showScrollBottom}
      <button class="scroll-fab bottom" title={$_("scroll_bottom", {}, "Scroll to bottom")} on:click={() => scrollToBottom(true)}>
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
          <polyline points="6 9 12 15 18 9" />
        </svg>
      </button>
    {/if}
  </div>
</div>

<style>
  .memory-app {
    display: flex;
    flex-direction: column;
    height: 100%;
    width: 100%;
    overflow: hidden;
    position: relative;
    background-color: var(--bg-secondary, #1A121D);
    color: var(--text-primary, #FCF7FA);
  }

  .memory-app.overlay-header .memory-header {
    border-bottom: none;
    pointer-events: none;
    z-index: 20;
  }

  .memory-app.overlay-header .memory-header .header-left,
  .memory-app.overlay-header .memory-header .header-right {
    pointer-events: auto;
  }

  /* 1. Header (toolbar) */
  .memory-header {
    position: relative;
    display: flex;
    flex-direction: column;
    padding: env(safe-area-inset-top, 0px) 14px 0 14px;
    background-color: var(--bg-secondary, rgba(26, 18, 29, 0.85));
    border-bottom: none;
    z-index: 10;
    flex-shrink: 0;
  }

  /* Gradient edge where the toolbar meets the sidebar — same shadow style as Creations */
  .memory-header::before {
    content: "";
    position: absolute;
    top: 0;
    bottom: 0;
    left: 0;
    width: 24px;
    background: linear-gradient(90deg, rgba(0, 0, 0, 0.25), transparent);
    pointer-events: none;
  }

  .memory-app.light-theme .memory-header::before {
    background: linear-gradient(90deg, rgba(0, 0, 0, 0.05), transparent);
  }

  .header-main-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    width: 100%;
    height: 56px;
    min-height: 56px;
    gap: 12px;
  }

  /* 1b. Filter zone above the cards (same background as the main area) */
  .memory-toolbar {
    position: relative;
    z-index: 9;
    width: 100%;
    padding: 8px 16px 10px;
    box-sizing: border-box;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    background-color: var(--bg-primary, #0F0A10);
  }

  .category-pills-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: center;
    gap: 6px;
    max-width: 100%;
    padding-bottom: 2px;
  }

  .tags-menu-wrap {
    position: relative;
    display: inline-flex;
  }

  .header-left {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .header-title-box {
    display: flex;
    flex-direction: column;
    justify-content: center;
  }

  .header-title-row {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .memory-icon {
    font-size: 16px;
  }

  .header-title {
    font-size: 16px;
    font-weight: 700;
    margin: 0;
    line-height: 1.2;
    color: var(--text-primary, #ffffff);
  }

  .count-pill {
    background: rgba(236, 72, 153, 0.2);
    color: #F472B6;
    border: 1px solid rgba(236, 72, 153, 0.3);
    border-radius: 999px;
    font-size: 11px;
    font-weight: 600;
    padding: 1px 7px;
  }

  .header-right {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .wx-modes {
    display: flex;
    align-items: center;
  }

  .wx-modes .wx-segmented {
    display: inline-flex;
    background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.05));
    border: 1px solid var(--border-color, var(--wx-border, rgba(255, 255, 255, 0.1)));
    border-radius: 6px;
    padding: 2px;
    gap: 2px;
    height: 36px;
    box-sizing: border-box;
  }

  .wx-modes .wx-segmented button {
    width: 32px;
    height: 30px;
    min-width: 32px;
    padding: 0;
    margin: 0;
    border-radius: 4px;
    border: none;
    outline: none;
    background: transparent;
    color: var(--wx-color-font-alt, #998094);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
    box-sizing: border-box;
  }

  .wx-modes .wx-segmented button:hover {
    background-color: var(--wx-background-hover, rgba(255, 255, 255, 0.08));
    color: var(--wx-color-primary, #F472B6);
  }

  .wx-modes .wx-segmented button.wx-selected {
    background-color: var(--wx-color-primary, #F472B6);
    color: #ffffff;
    box-shadow: none;
  }

  .wx-modes .wx-segmented button .wx-icon {
    font-size: 16px;
    line-height: 1;
    height: 16px;
    color: inherit;
    display: inline-block;
  }

  .header-icon-btn {
    width: 34px;
    height: 34px;
    border-radius: 8px;
    background: transparent;
    border: 1px solid transparent;
    color: var(--text-muted, #998094);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .header-icon-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-primary, #ffffff);
  }

  .header-icon-btn.new-card-btn:hover {
    background: rgba(236, 72, 153, 0.18);
    color: #F472B6;
  }

  .new-card-menu-wrap {
    position: relative;
    display: flex;
    align-items: center;
  }

  .new-card-dropdown {
    position: absolute;
    top: 42px;
    right: 0;
    z-index: 60;
    min-width: 220px;
    background: var(--bg-secondary, #1A121D);
    border: 1px solid var(--border-color, #352339);
    border-radius: 12px;
    box-shadow: 0 12px 40px rgba(0, 0, 0, 0.45);
    padding: 6px;
    display: flex;
    flex-direction: column;
    gap: 2px;
    animation: dropIn 0.16s ease;
  }

  @keyframes dropIn {
    from { opacity: 0; transform: translateY(-6px); }
    to { opacity: 1; transform: translateY(0); }
  }

  .menu-item {
    display: flex;
    align-items: center;
    gap: 9px;
    padding: 9px 12px;
    background: transparent;
    border: none;
    border-radius: 8px;
    color: var(--text-primary, #ffffff);
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    text-align: left;
    transition: all 0.12s ease;
  }

  .menu-item:hover {
    background: var(--bg-card-hover, #271B2C);
  }

  .menu-icon {
    font-size: 15px;
    width: 16px;
    text-align: center;
  }

  .menu-svg {
    flex-shrink: 0;
    color: var(--text-muted, #998094);
  }

  .menu-separator {
    height: 1px;
    background: var(--border-color, #352339);
    margin: 4px 8px;
  }

  /* 2. Feed & Scroll Container */
  .memory-feed {
    flex: 1;
    overflow-y: auto;
    overflow-x: hidden;
    padding: 12px 16px 40px;
    display: flex;
    flex-direction: column;
    align-items: center;
    scroll-behavior: smooth;
    background-color: var(--bg-primary, #0F0A10);
    box-sizing: border-box;
    width: 100%;
  }

  /* 3. Search */
  .memory-search-wrap {
    position: relative;
    width: 220px;
    max-width: 220px;
    flex: 0 1 220px;
    min-width: 120px;
    display: flex;
    align-items: center;
  }

  .search-icon-inside {
    position: absolute;
    left: 12px;
    color: var(--text-muted, #998094);
    pointer-events: none;
  }

  .memory-search-input {
    width: 100%;
    background: var(--bg-secondary, #1A121D);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    border-radius: 10px;
    padding: 9px 32px 9px 34px;
    font-size: 13px;
    color: var(--text-primary, #ffffff);
    outline: none;
    box-sizing: border-box;
    transition: border-color 0.15s ease;
  }

  .memory-search-input:focus {
    border-color: var(--accent-blue, #F472B6);
  }

  .clear-search-btn {
    position: absolute;
    right: 10px;
    background: transparent;
    border: none;
    color: var(--text-muted, #998094);
    cursor: pointer;
    font-size: 15px;
    padding: 2px 4px;
  }

  .knowledge-loading-toast {
    position: fixed;
    top: 12px;
    left: 50%;
    transform: translateX(-50%);
    z-index: 999999;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 0 16px;
    height: 32px;
    box-sizing: border-box;
    border-radius: 16px;
    background: var(--bg-secondary, #ffffff);
    border: 1px solid var(--border-color, #E6D3DF);
    box-shadow: 0 4px 16px rgba(0, 0, 0, 0.12);
    backdrop-filter: blur(12px);
    -webkit-backdrop-filter: blur(12px);
    color: var(--wx-color-font, #0F0A10);
    font-size: 12.5px;
    font-weight: 500;
    white-space: nowrap;
    max-width: calc(100vw - 32px);
    pointer-events: none;
    animation: klToastIn 0.25s cubic-bezier(0.16, 1, 0.3, 1);
  }

  :global(html.omd-dark) .knowledge-loading-toast {
    background: rgba(26, 18, 29, 0.94);
    border: 1px solid rgba(26, 18, 29, 0.9);
    box-shadow: 0 6px 20px rgba(0, 0, 0, 0.45);
    color: #F2E5ED;
  }

  .knowledge-loading-toast span {
    display: inline-flex;
    align-items: center;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    line-height: 1;
    margin: 0;
    padding: 0;
  }

  .kl-spinner {
    width: 14px;
    height: 14px;
    flex-shrink: 0;
    border: 2px solid rgba(153, 128, 148, 0.3);
    border-top-color: var(--accent-blue, #EC4899);
    border-radius: 50%;
    animation: kl-spin 0.8s linear infinite;
  }

  @keyframes kl-spin {
    to {
      transform: rotate(360deg);
    }
  }

  @keyframes klToastIn {
    from {
      opacity: 0;
      transform: translate(-50%, -10px);
    }
    to {
      opacity: 1;
      transform: translate(-50%, 0);
    }
  }

  .cat-pill {
    padding: 5px 12px;
    background: var(--bg-secondary, #1A121D);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    border-radius: 999px;
    color: var(--text-secondary, #998094);
    font-size: 12.5px;
    font-weight: 500;
    cursor: pointer;
    white-space: nowrap;
    transition: all 0.15s ease;
  }

  .cat-pill:hover {
    color: var(--text-primary, #ffffff);
    border-color: var(--text-muted, #7D6376);
  }

  .cat-pill.active {
    background: rgba(236, 72, 153, 0.18);
    border-color: rgba(236, 72, 153, 0.45);
    color: #F472B6;
    font-weight: 600;
  }

  .memory-tags-cloud {
    display: none;
  }

  .tags-pill {
    display: inline-flex;
    align-items: center;
    gap: 5px;
  }

  .tags-pill-icon {
    color: var(--text-muted, #998094);
    font-weight: 600;
  }

  .tags-pill-selected {
    background: rgba(236, 72, 153, 0.2);
    border: 1px solid #F472B6;
    color: #F472B6;
    border-radius: 999px;
    font-size: 11px;
    font-weight: 600;
    padding: 0 7px;
    line-height: 16px;
  }

  .tags-chevron {
    transition: transform 0.15s ease;
  }

  .tags-chevron.rotated {
    transform: rotate(180deg);
  }

  .tags-dropdown {
    position: absolute;
    top: calc(100% + 8px);
    right: 0;
    z-index: 50;
    min-width: 230px;
    max-width: min(320px, calc(100vw - 32px));
    box-sizing: border-box;
    background: var(--bg-secondary, #1A121D);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    border-radius: 12px;
    box-shadow: 0 14px 36px rgba(0, 0, 0, 0.4);
    padding: 10px 12px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .tags-dropdown-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }

  .tags-dropdown-cloud {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    max-height: 210px;
    overflow-y: auto;
  }

  .tags-cloud-label {
    font-size: 12px;
    color: var(--text-muted, #998094);
    font-weight: 500;
    margin-right: 2px;
  }

  .tag-cloud-chip {
    padding: 2px 9px;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.1));
    border-radius: 10px;
    color: var(--text-secondary, #998094);
    font-size: 12px;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    gap: 3px;
    transition: all 0.15s ease;
  }

  .tag-cloud-chip:hover {
    border-color: #F472B6;
    color: #F472B6;
  }

  .tag-cloud-chip.person-tag-chip {
    background: rgba(236, 72, 153, 0.16);
    border-color: rgba(236, 72, 153, 0.45);
    color: var(--accent-blue, #EC4899);
    font-weight: 600;
  }
  .tag-cloud-chip.person-tag-chip:hover {
    background: rgba(236, 72, 153, 0.28);
    border-color: var(--accent-blue, #EC4899);
  }
  .tag-person-icon {
    font-size: 0.82rem;
    margin-right: 2px;
  }
  .tag-count-badge {
    font-size: 0.68rem;
    padding: 1px 4px;
    border-radius: 6px;
    background: rgba(255, 255, 255, 0.08);
    margin-left: 3px;
    opacity: 0.85;
  }
  .tag-cloud-chip.active {
    background: rgba(236, 72, 153, 0.2);
    border-color: #F472B6;
    color: #F472B6;
    font-weight: 600;
  }

  .clear-tag-x {
    font-size: 11px;
    font-weight: bold;
  }

  .tag-clear-all {
    background: transparent;
    border: none;
    color: var(--text-muted, #998094);
    font-size: 12px;
    cursor: pointer;
    text-decoration: underline;
    margin-left: auto;
  }

  /* 4. Cards Container */
  .memory-container {
    width: 100%;
    display: flex;
    flex-direction: column;
    gap: 24px;
  }

  .memory-container.roll {
    max-width: 580px;
    margin: 0 auto;
  }

  .memory-container.grid {
    max-width: 1200px;
    margin: 0 auto;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
    gap: 20px;
  }

  /* 5. Memory Card */
  .memory-card {
    background: var(--bg-secondary, #18141D);
    border-radius: 16px;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.08));
    overflow: hidden;
    display: flex;
    flex-direction: column;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.25);
    cursor: pointer;
    outline: none;
    user-select: none;
    transition: transform 0.2s ease, box-shadow 0.2s ease, border-color 0.2s ease;
  }

  .memory-card:hover {
    box-shadow: 0 8px 30px rgba(0, 0, 0, 0.35);
    transform: translateY(-2px);
    border-color: rgba(236, 72, 153, 0.35);
  }

  .memory-app.light-theme .memory-card {
    background: #ffffff;
    border-color: var(--border-color, #E6E2ED);
    box-shadow: 0 2px 10px rgba(24, 18, 32, 0.05);
  }

  .memory-app.light-theme .memory-card:hover {
    box-shadow: 0 8px 24px rgba(24, 18, 32, 0.09);
    border-color: rgba(219, 39, 119, 0.28);
  }

  .memory-card:focus-visible {
    border-color: #F472B6;
    box-shadow: 0 0 0 2px rgba(236, 72, 153, 0.4);
  }

  /* Card Header */
  .memory-card-header {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 10px 14px;
    font-size: 11.5px;
    color: var(--text-muted, #93889E);
    background: rgba(255, 255, 255, 0.025);
    border-bottom: 1px solid var(--border-subtle, rgba(255, 255, 255, 0.05));
  }

  .memory-app.light-theme .memory-card-header {
    background: #FAFAFC;
    border-bottom: 1px solid var(--border-subtle, #F0EDF5);
    color: var(--text-secondary, #5E5468);
  }

  .card-kind-label {
    padding: 2px 8px;
    border-radius: 999px;
    font-size: 11px;
    font-weight: 600;
    flex-shrink: 0;
  }

  .card-device-badge {
    font-size: 12px;
    line-height: 1;
    flex-shrink: 0;
  }

  .memory-card.kind-folder .card-kind-label {
    background: rgba(168, 85, 247, 0.16);
    color: #c084fc;
    border: 1px solid rgba(168, 85, 247, 0.3);
  }

  .memory-card.kind-image .card-kind-label {
    background: rgba(16, 185, 129, 0.16);
    color: #34d399;
    border: 1px solid rgba(16, 185, 129, 0.3);
  }

  .memory-card.kind-doc .card-kind-label {
    background: rgba(236, 72, 153, 0.16);
    color: #F472B6;
    border: 1px solid rgba(236, 72, 153, 0.3);
  }

  .memory-card.kind-link .card-kind-label {
    background: rgba(34, 197, 94, 0.14);
    color: #4ade80;
    border: 1px solid rgba(34, 197, 94, 0.3);
  }

  .memory-card.kind-note .card-kind-label {
    background: rgba(245, 158, 11, 0.14);
    color: #fbbf24;
    border: 1px solid rgba(245, 158, 11, 0.3);
  }

  .memory-app.light-theme .memory-card.kind-folder .card-kind-label {
    background: rgba(126, 34, 206, 0.08);
    color: #7E22CE;
    border-color: rgba(126, 34, 206, 0.18);
  }

  .memory-app.light-theme .memory-card.kind-image .card-kind-label {
    background: rgba(5, 150, 105, 0.08);
    color: #059669;
    border-color: rgba(5, 150, 105, 0.18);
  }

  .memory-app.light-theme .memory-card.kind-doc .card-kind-label {
    background: rgba(190, 24, 93, 0.08);
    color: #BE185D;
    border-color: rgba(190, 24, 93, 0.18);
  }

  .memory-app.light-theme .memory-card.kind-link .card-kind-label {
    background: rgba(22, 163, 74, 0.08);
    color: #15803D;
    border-color: rgba(22, 163, 74, 0.18);
  }

  .memory-app.light-theme .memory-card.kind-note .card-kind-label {
    background: rgba(180, 83, 9, 0.08);
    color: #B45309;
    border-color: rgba(180, 83, 9, 0.18);
  }

  .card-time {
    margin-left: auto;
    font-size: 11px;
    color: var(--text-muted, #998094);
    flex-shrink: 0;
  }

  .card-top-actions {
    display: flex;
    align-items: center;
    gap: 2px;
    opacity: 0.5;
    transition: opacity 0.15s ease;
  }

  .memory-card:hover .card-top-actions {
    opacity: 1;
  }

  .card-top-btn {
    background: transparent;
    border: none;
    color: var(--text-muted, #998094);
    width: 26px;
    height: 26px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .card-top-btn:hover {
    color: #F472B6;
    background: rgba(236, 72, 153, 0.15);
  }

  .card-top-btn.delete-btn:hover {
    color: #ef4444;
    background: rgba(239, 68, 68, 0.15);
  }

  /* Card Body */
  .memory-card-body {
    padding: 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
    flex: 1;
  }

  .card-title {
    margin: 0;
    font-size: 15px;
    font-weight: 600;
    color: var(--text-primary, #ffffff);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    line-height: 1.35;
  }

  .card-image-preview-wrap {
    border-radius: 10px;
    overflow: hidden;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.08));
    max-height: 180px;
  }

  .card-image-preview-wrap.image-on-another-device {
    padding: 14px 12px;
    background: var(--bg-hover, rgba(255, 255, 255, 0.03));
    border: 1px dashed var(--border-color, rgba(255, 255, 255, 0.15));
    text-align: center;
    font-size: 0.82rem;
    color: var(--text-secondary, #998094);
  }

  .card-image-preview-wrap.card-folder-wrap {
    display: flex;
    align-items: center;
    justify-content: center;
    height: 120px;
    background: var(--bg-hover, rgba(255, 255, 255, 0.04));
    border-color: var(--border-subtle, rgba(255, 255, 255, 0.07));
    color: #B8A6CA;
    transition: color 0.18s ease, background 0.18s ease;
  }

  .memory-app.light-theme .card-image-preview-wrap.card-folder-wrap {
    background: #F4F3F7;
    border-color: #E8E4F0;
    color: #7D6D90;
  }

  .memory-card:hover .card-image-preview-wrap.card-folder-wrap {
    color: var(--accent, #DB2777);
  }

  .card-folder-icon {
    width: 48px;
    height: 48px;
  }

  .card-image-preview {
    display: block;
    width: 100%;
    height: 180px;
    object-fit: cover;
  }

  .card-source-bar {
    display: flex;
    align-items: center;
    gap: 5px;
    background: var(--bg-card-hover, rgba(255, 255, 255, 0.04));
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.06));
    padding: 4px 9px;
    border-radius: 7px;
    font-size: 11.5px;
    color: var(--text-secondary, #CEC6D6);
    transition: all 0.12s ease;
    overflow: hidden;
  }

  .card-source-bar.clickable {
    cursor: pointer;
  }

  .card-source-bar.clickable:hover {
    background: var(--accent-blue-active, rgba(236, 72, 153, 0.10));
    border-color: var(--accent-blue, #EC4899);
    color: var(--accent-blue, #EC4899);
  }

  .card-source-bar.static {
    color: var(--text-secondary, #93889E);
    cursor: default;
  }

  .memory-app.light-theme .card-source-bar {
    background: #F4F3F7;
    border-color: #E8E4F0;
    color: #4E4459;
  }

  .memory-app.light-theme .card-source-bar.clickable:hover {
    background: rgba(219, 39, 119, 0.07);
    border-color: rgba(219, 39, 119, 0.32);
    color: var(--accent, #DB2777);
  }

  .memory-app.light-theme .card-source-bar.static {
    color: var(--text-secondary, #5E5468);
  }

  .kind-icon-svg {
    margin-right: 3px;
    vertical-align: -1px;
  }

  .source-text-link {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    flex: 1;
    font-family: monospace;
  }

  .source-arrow-icon {
    font-size: 11px;
    opacity: 0.7;
  }

  .card-summary-preview {
    margin: 0;
    font-size: 13px;
    color: var(--text-secondary, #B59CAF);
    line-height: 1.5;
    word-break: break-word;
    display: -webkit-box;
    -webkit-line-clamp: 3;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }

  .card-summary-preview.card-preview-path {
    font-family: monospace;
    font-size: 11px;
    opacity: 0.7;
    -webkit-line-clamp: 1;
  }

  .card-footer-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    margin-top: 8px;
  }

  .card-tags {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
    min-width: 0;
  }

  .card-tag-chip {
    font-size: 10px;
    font-weight: 600;
    color: #F472B6;
    background: rgba(236, 72, 153, 0.15);
    border: 1px solid rgba(236, 72, 153, 0.25);
    border-radius: 999px;
    padding: 1px 7px;
    white-space: nowrap;
  }

  .memory-app.light-theme .card-tag-chip {
    color: #BE185D;
    background: rgba(219, 39, 119, 0.07);
    border-color: rgba(219, 39, 119, 0.18);
  }

  .memory-app.light-theme .count-pill {
    background: rgba(219, 39, 119, 0.08);
    color: #BE185D;
    border-color: rgba(219, 39, 119, 0.22);
  }

  .memory-app.light-theme .cat-pill.active {
    background: rgba(219, 39, 119, 0.08);
    border-color: rgba(219, 39, 119, 0.30);
    color: #BE185D;
  }

  .memory-app.light-theme .header-icon-btn:hover {
    background: rgba(29, 24, 35, 0.05);
    color: var(--text-primary, #1D1823);
  }

  .card-chunks {
    flex-shrink: 0;
    font-size: 10px;
    font-weight: 600;
    color: var(--text-secondary, #B59CAF);
    background: rgba(153, 128, 148, 0.12);
    border-radius: 999px;
    padding: 1px 7px;
  }

  /* Empty State */
  .empty-memory-state {
    flex: 1;
    width: 100%;
    max-width: 480px;
    margin: 0 auto;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    text-align: center;
    padding: 48px 24px;
    gap: 12px;
  }

  .empty-memory-glow {
    width: 84px;
    height: 84px;
    border-radius: 24px;
    display: flex;
    align-items: center;
    justify-content: center;
    color: #F472B6;
    background: rgba(236, 72, 153, 0.1);
    border: 1px solid rgba(236, 72, 153, 0.2);
    box-shadow: 0 0 40px rgba(236, 72, 153, 0.15);
  }

  .empty-title {
    margin: 0;
    font-size: 18px;
    font-weight: 700;
    color: var(--text-primary, #ffffff);
  }

  .empty-desc {
    margin: 0;
    font-size: 13.5px;
    line-height: 1.5;
    color: var(--text-muted, #998094);
  }

  .empty-actions {
    margin-top: 12px;
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    justify-content: center;
    align-items: center;
    width: 100%;
    max-width: 520px;
  }

  .empty-action-btn {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 9px 18px;
    border-radius: 999px;
    font-size: 13.5px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s cubic-bezier(0.4, 0, 0.2, 1);
    user-select: none;
    white-space: nowrap;
    border: 1px solid transparent;
    font-family: inherit;
  }

  .empty-action-btn.primary {
    background: linear-gradient(135deg, #EC4899, #F472B6);
    color: #ffffff;
    box-shadow: 0 4px 16px rgba(236, 72, 153, 0.3);
  }

  .empty-action-btn.primary:hover {
    transform: translateY(-1px);
    box-shadow: 0 6px 22px rgba(236, 72, 153, 0.45);
    filter: brightness(1.05);
  }

  .empty-action-btn.secondary {
    background: var(--bg-card, rgba(255, 255, 255, 0.06));
    border-color: var(--border-color, rgba(255, 255, 255, 0.14));
    color: var(--text-primary, #ffffff);
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.12);
  }

  .empty-action-btn.secondary:hover {
    transform: translateY(-1px);
    background: var(--bg-card-hover, rgba(255, 255, 255, 0.12));
    border-color: #F472B6;
    color: #F472B6;
    box-shadow: 0 4px 14px rgba(236, 72, 153, 0.2);
  }

  .empty-action-btn .btn-emoji {
    font-size: 15px;
    line-height: 1;
    display: inline-flex;
    align-items: center;
  }

  .empty-action-btn .btn-svg {
    flex-shrink: 0;
    color: currentColor;
    transition: transform 0.15s ease;
  }

  .empty-action-btn:hover .btn-svg {
    transform: scale(1.08);
  }

  .memory-app.light-theme .empty-action-btn.secondary {
    background: #ffffff;
    border-color: #E6E2ED;
    color: #2F2738;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.05);
  }

  .memory-app.light-theme .empty-action-btn.secondary:hover {
    background: #FAF9FB;
    border-color: #DB2777;
    color: #DB2777;
    box-shadow: 0 4px 14px rgba(236, 72, 153, 0.15);
  }

  @media (max-width: 520px) {
    .empty-actions {
      flex-direction: column;
      width: 100%;
    }

    .empty-action-btn {
      width: 100%;
      justify-content: center;
    }
  }

  .ghost-reset-btn {
    margin-top: 8px;
    background: transparent;
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.15));
    color: var(--text-secondary, #998094);
    padding: 8px 18px;
    border-radius: 999px;
    font-size: 13px;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .ghost-reset-btn:hover {
    color: var(--text-primary, #ffffff);
    border-color: #F472B6;
  }

  /* Scroll FABs */
  .scroll-fab {
    position: fixed;
    right: 24px;
    width: 38px;
    height: 38px;
    border-radius: 50%;
    background: var(--bg-secondary, #1A121D);
    border: 1px solid var(--border-color, rgba(255, 255, 255, 0.12));
    color: var(--text-secondary, #998094);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    z-index: 30;
    box-shadow: 0 4px 16px rgba(0, 0, 0, 0.3);
    transition: all 0.15s ease;
  }

  .scroll-fab.top {
    bottom: 72px;
  }

  .scroll-fab.bottom {
    bottom: 20px;
  }

  .scroll-fab:hover {
    color: #F472B6;
    border-color: #F472B6;
  }

  /* Responsive */
  @media (max-width: 768px) {
    .memory-header {
      padding: env(safe-area-inset-top, 0px) 12px 0 12px;
    }

    .header-main-row {
      height: 48px;
      min-height: 48px;
    }

    /* Заголовок убираем — экономим место под поиск */
    .header-title {
      display: none;
    }

    /* Поиск растягивается на всё свободное место */
    .memory-search-wrap {
      flex: 1 1 auto;
      width: auto;
      max-width: none;
      min-width: 100px;
    }

    .memory-search-input {
      padding: 8px 26px 8px 28px;
    }

    .wx-modes {
      display: none;
    }

    .memory-toolbar {
      padding: 6px 10px 8px;
    }

    .category-pills-row {
      justify-content: center;
      gap: 5px;
    }

    .cat-pill {
      font-size: 12px;
      padding: 4px 10px;
    }

    .memory-feed {
      padding: 10px 12px 30px;
    }

    .people-grid {
      grid-template-columns: repeat(auto-fill, minmax(160px, 1fr)) !important;
      gap: 10px !important;
    }
  }

  /* People Gallery View */
  .people-gallery-wrap {
    padding: 4px 0 32px;
    max-width: 1200px;
    margin: 0 auto;
    width: 100%;
    box-sizing: border-box;
  }

  .scan-progress-banner {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 14px;
    padding: 12px 18px;
    background: rgba(236, 72, 153, 0.1);
    border: 1px solid rgba(236, 72, 153, 0.3);
    border-radius: 12px;
    margin-bottom: 20px;
  }
  .scan-circular-progress {
    position: relative;
    width: 38px;
    height: 38px;
    flex-shrink: 0;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .scan-progress-svg {
    width: 100%;
    height: 100%;
    transform: rotate(-90deg);
  }
  .scan-circle-bg {
    fill: none;
    stroke: rgba(236, 72, 153, 0.2);
    stroke-width: 3.2;
  }
  .scan-circle-fill {
    fill: none;
    stroke: var(--accent-blue, #EC4899);
    stroke-width: 3.2;
    stroke-linecap: round;
    transition: stroke-dasharray 0.3s ease;
  }
  .scan-pct-label {
    position: absolute;
    font-size: 0.65rem;
    font-weight: 700;
    color: var(--accent-blue, #EC4899);
  }
  .scan-matched-count {
    color: #10b981;
    font-weight: 600;
  }
  .scan-text-wrap {
    display: flex;
    flex-direction: column;
    gap: 2px;
    flex: 1;
    min-width: 200px;
  }
  .scan-title {
    font-size: 0.86rem;
    font-weight: 600;
    color: var(--text-primary);
  }
  .scan-sub {
    font-size: 0.74rem;
    color: var(--text-muted);
  }
  .scan-progress-bar {
    width: 100%;
    height: 5px;
    background: rgba(255, 255, 255, 0.08);
    border-radius: 3px;
    overflow: hidden;
  }
  .scan-bar-fill {
    height: 100%;
    background: var(--accent-blue, #EC4899);
    transition: width 0.3s ease;
  }

  .people-loading-state {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 40px 0;
    color: var(--text-muted);
    font-size: 0.9rem;
  }

  .people-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
    gap: 16px;
    width: 100%;
    box-sizing: border-box;
  }

  .person-card {
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 14px;
    padding: 16px;
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.08);
    transition: transform 0.18s ease, box-shadow 0.18s ease, border-color 0.18s ease;
    box-sizing: border-box;
    overflow: hidden;
  }
  .person-card:hover {
    transform: translateY(-2px);
    border-color: var(--accent-blue, #EC4899);
    box-shadow: 0 6px 20px rgba(0, 0, 0, 0.15);
  }

  .person-avatar-wrap {
    width: 88px;
    height: 88px;
    border-radius: 50%;
    overflow: hidden;
    margin-bottom: 12px;
    border: 3px solid var(--border-color);
    background: var(--bg-secondary);
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .person-avatar-img {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }
  .person-avatar-fallback {
    font-size: 2.2rem;
    color: var(--text-muted);
  }

  .person-info {
    margin-bottom: 14px;
  }
  .person-name {
    margin: 0 0 4px;
    font-size: 1rem;
    font-weight: 600;
    color: var(--text-primary);
    word-break: break-word;
  }
  .person-photos-count {
    font-size: 0.76rem;
    color: var(--text-muted);
  }

  .person-actions {
    display: flex;
    flex-direction: column;
    gap: 8px;
    width: 100%;
    margin-top: auto;
    box-sizing: border-box;
  }
  .person-sub-actions {
    display: flex;
    align-items: center;
    gap: 6px;
    width: 100%;
    box-sizing: border-box;
  }

  .btn-person-action {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    padding: 7px 10px;
    border-radius: 8px;
    font-size: 0.78rem;
    font-weight: 500;
    background: var(--bg-secondary, rgba(125, 125, 125, 0.08));
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    cursor: pointer;
    transition: all 0.15s ease;
    white-space: nowrap;
    box-sizing: border-box;
  }
  .btn-person-action:hover {
    background: rgba(125, 125, 125, 0.16);
  }
  .btn-person-action.primary {
    width: 100%;
    background: rgba(236, 72, 153, 0.15);
    color: var(--accent-blue, #EC4899);
    border-color: rgba(236, 72, 153, 0.35);
  }
  .btn-person-action.primary:hover {
    background: var(--accent-blue, #EC4899);
    color: #fff;
  }
  .btn-person-action.secondary {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .btn-person-action.running {
    opacity: 0.6;
    cursor: wait;
  }
  .btn-person-delete {
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 7px 9px;
    border-radius: 8px;
    font-size: 0.78rem;
    background: var(--bg-secondary, rgba(125, 125, 125, 0.08));
    border: 1px solid var(--border-color);
    cursor: pointer;
    color: var(--text-muted);
    transition: all 0.15s ease;
    flex-shrink: 0;
    box-sizing: border-box;
  }
  .btn-person-delete:hover {
    background: rgba(239, 68, 68, 0.15);
    border-color: rgba(239, 68, 68, 0.4);
    color: #ef4444;
  }
</style>