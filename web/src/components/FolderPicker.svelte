<script>
  import { createEventDispatcher } from "svelte";
  import { _ } from "../lib/i18n/index.js";

  // Universal folder/file picker. Browses the real filesystem of a device
  // (via FileInfo + home flag) and returns the full absolute system path.
  //
  // Props:
  //   device       – device object exposing `.drive` (WebRTCDataChannelDrive)
  //   mode         – "folder" (only folders selectable) | "file" (only files)
  //   accept       – optional. For files: array of mime-type prefixes and/or
  //                  extensions (e.g. ["image/", ".png"]) to filter the tree.
  //   title        – modal title
  //   allowUpload  – show "Upload file" button (file pickers only)
  //   uploadAccept – accept attribute for the upload input (e.g. "image/*,.png")
  //   uploadInFolder – in folder mode allow uploading files INTO the current
  //   folder. Needed on mobile: there is no File System Access API, so a share
  //   is an empty OPFS dir and documents arrive via upload.
  //   onAddFolder – callback() for a "Add folder on this device" action in the
  //   header. Shown only when passed (local device with no shares yet).
  // Events:
  //   select  – { path } full absolute path of the chosen folder/file
  //   cancel  – picker dismissed

  let { device = null, mode = "folder", accept = null, title = "", allowUpload = false, uploadAccept = "", uploadInFolder = false, onAddFolder = null } = $props();

  const dispatch = createEventDispatcher();

  let loading = $state(false);
  let error = $state("");
  let busy = $state(false);
  let creatingFolder = $state(false);
  let newFolderName = $state("");
  let folderError = $state("");
  let fileInput = $state(null);
  // Загрузка доступна: в режиме файла (по флагу) и в режиме папки (uploadInFolder)
  const canUpload = $derived(allowUpload === true && mode === "file" || uploadInFolder === true && mode === "folder");
  // Virtual path shown in the picker: "/" = device home, "/Docs" = home/Docs
  let dir = $state("/");
  let realDir = $state("");
  let entries = $state([]);
  let selected = $state(null);
  let history = $state([]);
  // Тумбнейлы изображений (keyed by entry id) — для превью при выборе картинок
  let thumbs = $state(new Map());

  const IMAGE_EXTS = ["png", "jpg", "jpeg", "gif", "webp", "bmp", "ico", "tiff", "tif", "heic", "heif", "avif", "svg"];

  // Служебные файлы скрыты, как в обычной вьюхе файлового менеджера:
  // обложки и companion-заметки не являются документами для выбора.
  function isServiceFile(item) {
    const name = String(item.name || "");
    if (name === "folder.jpg" || name === "folder.png") return true;
    if (name.endsWith(".cover.jpg") || name.endsWith(".cover.png") || name.endsWith(".cover.jpeg")) return true;
    if (name.endsWith("Readme.md")) return true; // Readme.md и <file>.Readme.md
    return false;
  }

  function isImageItem(item) {
    const mime = String(item.mimeType || "").toLowerCase();
    if (mime.startsWith("image/")) return true;
    const ext = (item.name || "").includes(".") ? item.name.split(".").pop().toLowerCase() : "";
    return IMAGE_EXTS.includes(ext);
  }

  // Ленивый загрузчик тумбнейла изображения с устройства (WebRTC/Native).
  async function loadThumb(item) {
    if (!device || !device.drive || !isImageItem(item) || thumbs.has(item.id || item.name)) return;
    const path = item.id || childVirtualPath(item.name);
    const drive = device.drive;
    let url = "";
    try {
      if (typeof drive.getThumbnail === "function") {
        const dataUrl = await drive.getThumbnail(path, 96, 96);
        if (dataUrl) url = dataUrl;
      }
    } catch (_) {}
    if (!url && typeof drive.sendRPC === "function" && typeof drive.getFileContent === "function") {
      try {
        const query = "resize=true&width=96&height=96";
        const openRes = await drive.sendRPC("Open", { path, query });
        if (openRes && openRes.status === "ok" && openRes.result) {
          const totalSize = openRes.result.size || 0;
          const mimeType = openRes.result.mimeType || "image/jpeg";
          if (totalSize > 0) {
            const rawBlob = await drive.getFileContent(path, totalSize, null, 0, query);
            if (rawBlob && rawBlob.size > 0) {
              url = URL.createObjectURL(new Blob([rawBlob], { type: mimeType }));
            }
          }
        }
      } catch (_) {}
    }
    if (url) {
      thumbs = new Map(thumbs).set(item.id || item.name, url);
    }
  }

  async function loadFolder(path) {
    if (!device || !device.drive) return;
    loading = true;
    error = "";
    selected = null;
    try {
      if (!device.drive.isConnected || !device.drive.dc || device.drive.dc.readyState !== "open") {
        await device.drive.reconnect();
      }
      const items = await device.drive.getFiles(path, { home: true });
      dir = path;
      realDir = items.realPath || (path === "/" ? "/" : path);
      entries = items.filter(i => {
        if (isServiceFile(i)) return false;
        if (i.type === "folder") return true;
        if (mode === "folder") return false;
        return accepts(i);
      });
      // Лениво подгружаем превью изображений (только в file-режиме)
      if (mode === "file") {
        for (const it of entries) {
          if (it.type !== "folder") loadThumb(it);
        }
      }
    } catch (e) {
      console.error("[FolderPicker] loadFolder error:", e);
      error = e && e.message ? e.message : "Failed to load folder";
    } finally {
      loading = false;
    }
  }

  function accepts(item) {
    if (!accept) return true;
    const mime = item.mimeType || "";
    const name = item.name || "";
    const ext = name.includes(".") ? "." + name.split(".").pop().toLowerCase() : "";
    for (const a of accept) {
      const s = String(a).toLowerCase();
      if (mime.toLowerCase().startsWith(s)) return true;
      if (s.startsWith(".") && ext === s) return true;
      if (ext === s) return true;
    }
    return false;
  }

  function open(item) {
    if (item.type !== "folder") return;
    if (!item.realPath && item.id) {
      // fallback: derive virtual child path from current dir
      const childPath = dir.endsWith("/") ? dir + item.name : dir + "/" + item.name;
      history = [...history, { dir, realDir }];
      loadFolder(childPath);
      return;
    }
    history = [...history, { dir, realDir }];
    loadFolder(dir.endsWith("/") ? dir + item.name : dir + "/" + item.name);
  }

  function goBack() {
    const prev = history.pop();
    if (prev) {
      dir = prev.dir;
      realDir = prev.realDir;
      loadFolder(prev.dir);
    }
  }

  function selectItem(item) {
    if (mode === "folder" && item.type !== "folder") return;
    if (mode === "file" && item.type === "folder") return;
    selected = item;
  }

  // Папка выбирается не кликом по строке (он разворачивает папку), а
  // кнопкой «Select this folder» — выбирается текущая открытая папка.
  function selectCurrentFolder() {
    if (mode !== "folder") return;
    const name = dir.split("/").filter(Boolean).pop() || "";
    dispatch("select", {
      path: dir,
      virtualPath: dir,
      realPath: realDir || dir,
      item: { type: "folder", name, id: dir, realPath: realDir || dir },
    });
  }

  function confirm() {
    if (selected) {
      const virtualPath = selected.id || childVirtualPath(selected.name);
      const realPath = selected.realPath || (realDir.endsWith("/") ? realDir + selected.name : realDir + "/" + selected.name);
      dispatch("select", { path: virtualPath, virtualPath, realPath, item: selected });
      return;
    }
    if (mode === "folder" && dir !== "/") {
      selectCurrentFolder();
    }
  }

  function childVirtualPath(name) {
    return dir.endsWith("/") ? dir + name : dir + "/" + name;
  }

  function startCreateFolder() {
    creatingFolder = true;
    newFolderName = "";
    folderError = "";
  }

  function cancelCreateFolder() {
    creatingFolder = false;
    newFolderName = "";
    folderError = "";
  }

  async function submitCreateFolder() {
    const name = newFolderName.trim();
    if (!name) return;
    if (name.includes("/") || name.includes("..")) {
      folderError = "Invalid folder name";
      return;
    }
    busy = true;
    folderError = "";
    try {
      await device.drive.homeMkDir(childVirtualPath(name));
      creatingFolder = false;
      newFolderName = "";
      await loadFolder(dir);
    } catch (e) {
      console.error("[FolderPicker] create folder error:", e);
      folderError = e && e.message ? e.message : "Failed to create folder";
    } finally {
      busy = false;
    }
  }

  function triggerUpload() {
    if (fileInput) fileInput.click();
  }

  function readFileAsBase64(file) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => {
        const dataUrl = String(reader.result || "");
        const commaIdx = dataUrl.indexOf(",");
        resolve(commaIdx >= 0 ? dataUrl.substring(commaIdx + 1) : dataUrl);
      };
      reader.onerror = () => reject(new Error("Failed to read file"));
      reader.readAsDataURL(file);
    });
  }

  async function handleFileUpload(event) {
    const input = event.currentTarget;
    const files = Array.from((input.files && input.files) || []);
    input.value = "";
    if (!files.length) return;
    busy = true;
    folderError = "";
    try {
      // В корень устройства писать нельзя (у локального устройства корень —
      // список папок-шаров): сначала открыть папку, потом загружать в неё.
      if (dir === "/") {
        folderError = $_("upload_open_folder_first", {}, "Open a folder first, then upload files into it");
        return;
      }
      // Несколько файлов: base64 читаем последовательно, чтобы не сожрать
      // память на мобильных (FileReader по одному).
      let uploadedName = "";
      for (const file of files) {
        const base64 = await readFileAsBase64(file);
        await device.drive.homeWrite(childVirtualPath(file.name), base64);
        uploadedName = file.name;
      }
      await loadFolder(dir);
      // В режиме файла выделим загруженный файл, в режиме папки — нет.
      if (mode === "file") {
        const uploaded = entries.find(i => i.name === uploadedName);
        if (uploaded) selectItem(uploaded);
      }
    } catch (e) {
      console.error("[FolderPicker] upload error:", e);
      folderError = e && e.message ? e.message : "Upload failed";
    } finally {
      busy = false;
    }
  }

  let prevDevice = $state(null);
  $effect(() => {
    if (device && device !== prevDevice) {
      prevDevice = device;
      loadFolder("/");
    }
  });

  let pathLabel = $derived(dir === "/" ? "Home" : dir);
</script>

<!-- svelte-ignore a11y-click-events-have-key-events -->
<!-- svelte-ignore a11y-no-static-element-interactions -->
<div class="picker-backdrop" on:click|self={() => dispatch("cancel")}>
  <div class="picker-card">
    <div class="picker-header">
      <div class="header-info">
        <div class="header-icon">
          <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>
        </div>
        <div>
          <h3 class="picker-title">{title || (mode === "folder" ? $_("select_folder", {}, "Select Folder") : $_("select_file", {}, "Select File"))}</h3>
          <p class="picker-sub">{device?.name || $_("device", {}, "Device")} • {pathLabel}</p>
        </div>
      </div>
      <div class="header-actions">
        {#if onAddFolder}
          <button class="header-action-btn add-folder-btn" on:click={() => onAddFolder()} disabled={busy || loading} title={$_("add_folder_this_device", {}, "Add folder on this device")}>
            <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/><line x1="12" y1="11" x2="12" y2="17"/><line x1="9" y1="14" x2="15" y2="14"/></svg>
          </button>
        {/if}
        {#if canUpload}
          <button class="header-action-btn" on:click={triggerUpload} disabled={busy || loading} title={mode === "folder" ? $_("upload_files_here", {}, "Upload files here") : $_("upload_file", {}, "Upload file")}>
            <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>
          </button>
        {/if}
        <button class="header-action-btn" on:click={startCreateFolder} disabled={busy || loading} title={$_("new_folder", {}, "New folder")}>
          <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/><line x1="12" y1="11" x2="12" y2="17"/><line x1="9" y1="14" x2="15" y2="14"/></svg>
        </button>
        <button class="close-btn" on:click={() => dispatch("cancel")} aria-label={$_("close", {}, "Close")}>
          <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
        </button>
      </div>
    </div>

    <div class="picker-nav">
      <button class="nav-btn" on:click={goBack} disabled={history.length === 0}>
        <svg viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><polyline points="10 4 6 8 10 12"/></svg>
        {$_("back", {}, "Back")}
      </button>
      <span class="nav-path">{pathLabel}</span>
    </div>

    {#if creatingFolder}
      <div class="create-folder-bar">
        <input
          type="text"
          bind:value={newFolderName}
          placeholder={$_("folder_name_placeholder", {}, "Folder name")}
          class="create-folder-input"
          on:keydown={(e) => { if (e.key === "Enter") submitCreateFolder(); if (e.key === "Escape") cancelCreateFolder(); }}
        />
        <button class="nav-btn" on:click={submitCreateFolder} disabled={busy || !newFolderName.trim()}>{$_("create", {}, "Create")}</button>
        <button class="nav-btn" on:click={cancelCreateFolder} disabled={busy}>{$_("cancel", {}, "Cancel")}</button>
      </div>
    {/if}
    {#if folderError}
      <div class="alert error">{folderError}</div>
    {/if}

    {#if canUpload}
      <input type="file" bind:this={fileInput} accept={uploadAccept} multiple={mode === "folder"} style="display:none" on:change={handleFileUpload} />
    {/if}

    <div class="picker-body">
      {#if loading}
        <div class="loading-wrap">
          <div class="spinner"></div>
          <p>{$_("loading", {}, "Loading...")}</p>
        </div>
      {:else if error}
        <div class="alert error">{error}</div>
      {:else if entries.length === 0}
        <p class="empty">{$_("no_items_here", {}, "No items here")}</p>
      {:else}
        <ul class="tree">
          {#each entries as item}
            <li
              class="tree-item {selected === item ? 'selected' : ''}"
              role="button"
              tabindex="0"
              title={item.type === "folder" ? $_("select_folder", {}, "Select folder") : ""}
              on:click={() => (mode === "file" && item.type === "folder" ? open(item) : selectItem(item))}
              on:dblclick={() => (item.type === "folder" ? open(item) : confirm())}
              on:keydown={(e) => {
                if (e.key === "Enter") {
                  if (item.type === "folder") open(item);
                  else confirm();
                }
              }}
            >
              {#if item.type === "folder"}
                <span class="row-btn">
                  <span class="icon folder-icon">
                    <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>
                  </span>
                  <span class="name">{item.name}</span>
                </span>
                <button
                  type="button"
                  class="unfold-btn"
                  title={$_("open_folder", {}, "Open folder")}
                  aria-label={$_("open_folder", {}, "Open folder")}
                  on:click|stopPropagation={() => open(item)}
                >
                  <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2.5"><polyline points="9 6 15 12 9 18"/></svg>
                </button>
              {:else}
                {#if thumbs.get(item.id || item.name)}
                  <img class="item-thumb" src={thumbs.get(item.id || item.name)} alt="" loading="lazy" />
                {:else}
                  <span class="icon file-icon">
                    <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
                  </span>
                {/if}
                <span class="name">{item.name}</span>
              {/if}
            </li>
          {/each}
        </ul>
      {/if}
    </div>

    <div class="picker-footer">
      <span class="selection">
        {selected ? selected.name : (mode === "folder" ? $_("no_folder_selected", {}, "No folder selected") : $_("no_file_selected", {}, "No file selected"))}
      </span>
      {#if mode === "folder" && dir !== "/" && !selected && history.length > 0}
        <button class="btn-cancel" on:click={selectCurrentFolder} title={dir}>
          {$_("select_this_folder", {}, "Select this folder")}
        </button>
      {/if}
      <button class="btn-cancel" on:click={() => dispatch("cancel")}>{$_("cancel", {}, "Cancel")}</button>
      <button class="btn-primary" on:click={confirm} disabled={!selected && (mode !== "folder" || dir === "/")}>
        {$_("select", {}, "Select")}
      </button>
    </div>
  </div>
</div>

<style>
  .picker-backdrop {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(0, 0, 0, 0.6);
    backdrop-filter: blur(4px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 9999;
    padding: 16px;
  }

  .picker-card {
    width: 100%;
    max-width: 480px;
    height: 480px;
    max-height: 80vh;
    border-radius: 16px;
    overflow: hidden;
    display: flex;
    flex-direction: column;
    background: var(--bg-secondary, #ffffff);
    color: var(--wx-color-font, #101c22);
    border: 1px solid var(--border-color, #d6dde1);
    box-shadow: 0 20px 48px rgba(0, 0, 0, 0.2);
    animation: pickerAppear 0.2s cubic-bezier(0.16, 1, 0.3, 1);
  }

  :global(html.omd-dark) .picker-card {
    background: var(--bg-secondary, #202a2f);
    color: var(--wx-color-font, #ffffff);
    border: 1px solid var(--border-color, #3d4f57);
    box-shadow: 0 20px 48px rgba(0, 0, 0, 0.35);
  }

  @keyframes pickerAppear {
    from { opacity: 0; transform: scale(0.96) translateY(8px); }
    to { opacity: 1; transform: scale(1) translateY(0); }
  }

  .picker-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 18px 20px;
    border-bottom: 1px solid var(--border-color, #d6dde1);
  }

  :global(html.omd-dark) .picker-header {
    border-bottom-color: var(--border-color, #3d4f57);
  }

  .header-info {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .header-icon {
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: var(--wx-color-primary-selected, rgba(41, 150, 204, 0.15));
    color: var(--wx-color-primary, #184562);
    display: flex;
    align-items: center;
    justify-content: center;
  }

  :global(html.omd-dark) .header-icon {
    color: var(--wx-color-primary, #2996cc);
  }

  .picker-title {
    font-size: 16px;
    font-weight: 600;
    color: var(--wx-color-font, #101c22);
    margin: 0;
  }

  :global(html.omd-dark) .picker-title {
    color: var(--wx-color-font, #ffffff);
  }

  .picker-sub {
    font-size: 12px;
    color: var(--wx-color-font-alt, #546b75);
    margin: 2px 0 0 0;
    max-width: 320px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  :global(html.omd-dark) .picker-sub {
    color: var(--wx-color-font-alt, #94a3b8);
  }

  .close-btn {
    background: transparent;
    border: none;
    cursor: pointer;
    color: var(--wx-color-font-alt, #546b75);
    padding: 6px;
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.15s ease;
  }

  .close-btn:hover {
    background: var(--bg-card-hover, rgba(0, 0, 0, 0.06));
    color: var(--wx-color-font, #101c22);
  }

  :global(html.omd-dark) .close-btn {
    color: var(--wx-color-font-alt, #94a3b8);
  }

  :global(html.omd-dark) .close-btn:hover {
    background: rgba(148, 163, 184, 0.15);
    color: #ffffff;
  }

  .header-actions {
    display: flex;
    align-items: center;
    gap: 4px;
  }

  .header-action-btn {
    background: transparent;
    border: none;
    cursor: pointer;
    color: var(--wx-color-font-alt, #546b75);
    padding: 6px;
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.15s ease;
  }

  .header-action-btn:hover:not(:disabled) {
    background: var(--bg-card-hover, rgba(0, 0, 0, 0.06));
    color: var(--wx-color-font, #101c22);
  }

  :global(html.omd-dark) .header-action-btn {
    color: var(--wx-color-font-alt, #94a3b8);
  }

  :global(html.omd-dark) .header-action-btn:hover:not(:disabled) {
    background: rgba(148, 163, 184, 0.15);
    color: #ffffff;
  }

  .header-action-btn:disabled {
    opacity: 0.4;
    cursor: default;
  }

  .create-folder-bar {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 8px 20px;
    border-bottom: 1px solid var(--border-color, #d6dde1);
    background: var(--bg-primary, #f6f8f9);
  }

  :global(html.omd-dark) .create-folder-bar {
    border-bottom: 1px solid var(--border-color, #3d4f57);
    background: var(--bg-primary, #101c22);
  }

  .create-folder-input {
    flex: 1;
    height: 32px;
    padding: 0 10px;
    background: var(--bg-secondary, #ffffff);
    border: 1px solid var(--border-color, #d6dde1);
    border-radius: 6px;
    color: var(--wx-color-font, #101c22);
    font-family: inherit;
    font-size: 13px;
    outline: none;
  }

  :global(html.omd-dark) .create-folder-input {
    background: var(--bg-secondary, #202a2f);
    border: 1px solid var(--border-color, #3d4f57);
    color: var(--wx-color-font, #ffffff);
  }

  .create-folder-input:focus {
    border-color: var(--wx-color-primary, #184562);
  }

  .picker-nav {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 10px 20px;
    border-bottom: 1px solid var(--border-color, #d6dde1);
  }

  :global(html.omd-dark) .picker-nav {
    border-bottom: 1px solid var(--border-color, #3d4f57);
  }

  .nav-btn {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    background: var(--bg-primary, #f6f8f9);
    border: 1px solid var(--border-color, #d6dde1);
    color: var(--wx-color-font, #101c22);
    border-radius: 6px;
    font-size: 12px;
    padding: 4px 8px;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .nav-btn:hover:not(:disabled) {
    background: var(--bg-card-hover, #ecf0f2);
    border-color: var(--wx-color-primary, #184562);
    color: var(--wx-color-primary, #184562);
  }

  :global(html.omd-dark) .nav-btn {
    background: transparent;
    border: 1px solid rgba(148, 163, 184, 0.25);
    color: inherit;
  }

  :global(html.omd-dark) .nav-btn:hover:not(:disabled) {
    background: rgba(148, 163, 184, 0.15);
    color: #ffffff;
  }

  .nav-btn:disabled {
    opacity: 0.4;
    cursor: default;
  }

  .nav-path {
    font-size: 12px;
    color: var(--wx-color-font-alt, #546b75);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  :global(html.omd-dark) .nav-path {
    color: var(--wx-color-font-alt, #94a3b8);
  }

  .picker-body {
    flex: 1;
    overflow-y: auto;
    padding: 10px;
    background: var(--bg-secondary, #ffffff);
  }

  :global(html.omd-dark) .picker-body {
    background: var(--bg-secondary, #202a2f);
  }

  .loading-wrap {
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 30px;
    gap: 12px;
    color: var(--wx-color-font-alt, #546b75);
  }

  :global(html.omd-dark) .loading-wrap {
    color: var(--wx-color-font-alt, #94a3b8);
  }

  .spinner {
    width: 28px;
    height: 28px;
    border: 3px solid rgba(0, 0, 0, 0.12);
    border-top-color: var(--wx-color-primary, #184562);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  :global(html.omd-dark) .spinner {
    border: 3px solid rgba(148, 163, 184, 0.2);
    border-top-color: var(--wx-color-primary, #2996cc);
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  .empty {
    text-align: center;
    color: var(--wx-color-font-alt, #546b75);
    padding: 30px;
    font-size: 13px;
  }

  :global(html.omd-dark) .empty {
    color: var(--wx-color-font-alt, #94a3b8);
  }

  .alert.error {
    padding: 10px 14px;
    border-radius: 8px;
    font-size: 13px;
    background: rgba(248, 59, 66, 0.12);
    color: #f83b42;
    border: 1px solid rgba(248, 59, 66, 0.25);
    margin: 8px;
  }

  .tree {
    list-style: none;
    margin: 0;
    padding: 0;
  }

  .tree-item {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 6px 8px;
    border-radius: 8px;
    cursor: pointer;
    transition: background 0.15s ease;
  }

  .tree-item:hover {
    background: var(--bg-card-hover, rgba(0, 0, 0, 0.05));
  }

  :global(html.omd-dark) .tree-item:hover {
    background: rgba(148, 163, 184, 0.12);
  }

  .tree-item.selected {
    background: var(--wx-color-primary-selected, #e4f1fa);
  }

  :global(html.omd-dark) .tree-item.selected {
    background: var(--wx-color-primary-selected, rgba(41, 150, 204, 0.25));
  }

  .row-btn {
    flex: 1;
    display: flex;
    align-items: center;
    gap: 8px;
    background: transparent;
    border: none;
    color: var(--wx-color-font, #101c22);
    font-size: 14px;
    cursor: pointer;
    padding: 2px 0;
    min-width: 0;
    text-align: left;
  }

  :global(html.omd-dark) .row-btn {
    color: var(--wx-color-font, #ffffff);
  }

  .icon {
    display: inline-flex;
    flex-shrink: 0;
  }

  .item-thumb {
    width: 34px;
    height: 34px;
    flex-shrink: 0;
    border-radius: 6px;
    object-fit: cover;
    background: var(--bg-card-hover, rgba(0, 0, 0, 0.06));
    border: 1px solid var(--border-color, #d6dde1);
  }

  :global(html.omd-dark) .item-thumb {
    background: rgba(148, 163, 184, 0.15);
    border: 1px solid var(--border-color, #3d4f57);
  }

  .folder-icon {
    color: #f59e0b;
  }

  :global(html.omd-dark) .folder-icon {
    color: #fbbf24;
  }

  .file-icon {
    color: var(--wx-color-font-alt, #546b75);
  }

  :global(html.omd-dark) .file-icon {
    color: var(--wx-color-font-alt, #94a3b8);
  }

  .name {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .unfold-btn {
    width: 34px;
    height: 34px;
    min-width: 34px;
    border-radius: 8px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    background: transparent;
    border: 1px solid transparent;
    color: var(--wx-color-font-alt, #546b75);
    cursor: pointer;
    padding: 0;
    margin-left: auto;
    flex-shrink: 0;
    transition: all 0.15s ease;
  }

  .unfold-btn:hover {
    background: var(--bg-card-hover, rgba(0, 0, 0, 0.08));
    border-color: var(--border-color, rgba(0, 0, 0, 0.15));
    color: var(--wx-color-primary, #2996cc);
    transform: scale(1.12);
  }

  :global(html.omd-dark) .unfold-btn {
    color: var(--wx-color-font-alt, #94a3b8);
  }

  :global(html.omd-dark) .unfold-btn:hover {
    background: rgba(41, 150, 204, 0.2);
    border-color: rgba(41, 150, 204, 0.4);
    color: #ffffff;
    transform: scale(1.12);
  }

  :global(html.omd-dark) .unfold-btn:active {
    transform: scale(0.96);
  }

  .picker-footer {
    display: flex;
    align-items: center;
    padding: 14px 20px;
    border-top: 1px solid var(--border-color, #d6dde1);
    background: var(--bg-secondary, #ffffff);
    gap: 10px;
  }

  :global(html.omd-dark) .picker-footer {
    border-top: 1px solid var(--border-color, #3d4f57);
    background: var(--bg-secondary, #202a2f);
  }

  .selection {
    flex: 1;
    font-size: 12px;
    color: var(--wx-color-font-alt, #546b75);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  :global(html.omd-dark) .selection {
    color: var(--wx-color-font-alt, #94a3b8);
  }

  .btn-primary {
    padding: 9px 18px;
    background: var(--wx-color-primary, #184562);
    color: #ffffff;
    border: none;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: background 0.15s ease;
  }

  .btn-primary:hover:not(:disabled) {
    background: var(--wx-color-primary-hover, #2996cc);
  }

  .btn-primary:disabled {
    opacity: 0.45;
    cursor: default;
  }

  .btn-cancel {
    padding: 9px 14px;
    background: var(--bg-primary, #f6f8f9);
    border: 1px solid var(--border-color, #d6dde1);
    color: var(--wx-color-font, #101c22);
    border-radius: 8px;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .btn-cancel:hover {
    background: var(--bg-card-hover, #ecf0f2);
    border-color: var(--wx-color-primary, #184562);
    color: var(--wx-color-primary, #184562);
  }

  :global(html.omd-dark) .btn-cancel {
    background: transparent;
    border: 1px solid rgba(148, 163, 184, 0.25);
    color: #ffffff;
  }

  :global(html.omd-dark) .btn-cancel:hover {
    background: rgba(148, 163, 184, 0.15);
    border-color: rgba(148, 163, 184, 0.4);
    color: #ffffff;
  }
</style>
