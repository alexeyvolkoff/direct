<script>
  import { onMount, onDestroy, createEventDispatcher } from 'svelte';
  import { deviceRegistry } from '../lib/device-registry.js';
  import { unifiedDiff } from '../lib/unified-diff.js';
  import { getLanguageFromFilename } from '../lib/lang.js';
  import { getDomainBrandTitle } from '../lib/app-mode.js';
  import Terminal from './Terminal.svelte';
  import Editor from './Editor.svelte';
  import ReviewDiff from './ReviewDiff.svelte';

  export let type = 'terminal'; // 'terminal' | 'editor'
  export let targetLinkId = '';
  export let path = '';
  export let filePath = '';
  export let fileName = '';
  export let port = 22;
  export let service = 'terminal';
  export let fileContent = '';

  const dispatch = createEventDispatcher();

  let showSidebar = true;
  let tabs = [];
  let activeTabId = '';
  let terminalCounter = 0;

  let isMobile = typeof window !== 'undefined' ? window.innerWidth <= 768 : false;
  function updateMobileState() {
    isMobile = window.innerWidth <= 768;
  }

  // --- Sidebar resize ---
  const SIDEBAR_DEFAULT = 180;
  const SIDEBAR_MIN = 120;
  const SIDEBAR_MAX = 400;
  let sidebarWidth = (() => {
    try { return parseInt(localStorage.getItem('omd_sidebar_width'), 10) || SIDEBAR_DEFAULT; } catch { return SIDEBAR_DEFAULT; }
  })();
  let _sidebarDrag = null;
  function onSidebarDragStart(e) {
    e.preventDefault();
    _sidebarDrag = { startX: e.clientX, startW: sidebarWidth };
    document.addEventListener('mousemove', onSidebarDragMove);
    document.addEventListener('mouseup', onSidebarDragEnd);
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  }
  function onSidebarDragMove(e) {
    if (!_sidebarDrag) return;
    sidebarWidth = Math.min(SIDEBAR_MAX, Math.max(SIDEBAR_MIN, _sidebarDrag.startW + (e.clientX - _sidebarDrag.startX)));
  }
  function onSidebarDragEnd() {
    _sidebarDrag = null;
    document.removeEventListener('mousemove', onSidebarDragMove);
    document.removeEventListener('mouseup', onSidebarDragEnd);
    document.body.style.cursor = '';
    document.body.style.userSelect = '';
    try { localStorage.setItem('omd_sidebar_width', String(sidebarWidth)); } catch {}
  }

  // --- Review panel resize ---
  const REVIEW_DEFAULT = 500;
  const REVIEW_MIN = 250;
  let reviewWidth = (() => {
    try { return parseInt(localStorage.getItem('omd_review_width'), 10) || REVIEW_DEFAULT; } catch { return REVIEW_DEFAULT; }
  })();
  let _reviewDrag = null;
  function onReviewDragStart(e) {
    e.preventDefault();
    _reviewDrag = { startX: e.clientX, startW: reviewWidth };
    document.addEventListener('mousemove', onReviewDragMove);
    document.addEventListener('mouseup', onReviewDragEnd);
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  }
  function onReviewDragMove(e) {
    if (!_reviewDrag) return;
    const maxW = Math.round(window.innerWidth * 0.6);
    reviewWidth = Math.min(maxW, Math.max(REVIEW_MIN, _reviewDrag.startW - (e.clientX - _reviewDrag.startX)));
  }
  function onReviewDragEnd() {
    _reviewDrag = null;
    document.removeEventListener('mousemove', onReviewDragMove);
    document.removeEventListener('mouseup', onReviewDragEnd);
    document.body.style.cursor = '';
    document.body.style.userSelect = '';
    try { localStorage.setItem('omd_review_width', String(reviewWidth)); } catch {}
  }

  // --- File save ---
  let saving = false;
  async function saveFile() {
    if (saving || !activeTab || activeTab.type !== 'editor') return;
    const tab = activeTab;
    const filePath = cleanDevicePath(tab.path);
    if (!filePath || filePath === '/') return;
    saving = true;
    // Flush DOM so the banner is painted before the async write.
    await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    try {
      const data = btoa(unescape(encodeURIComponent(tab.content || '')));
      await activeDevice.drive.writeFileChunked(filePath, data);
      tab.original = tab.content;
    } catch (err) {
      console.error("[Workspace] save failed:", err);
    } finally {
      saving = false;
    }
  }

  function handleGlobalKeydown(e) {
    if ((e.ctrlKey || e.metaKey) && e.key === 's') {
      e.preventDefault();
      saveFile();
    }
  }

  function cleanDevicePath(p) {
    if (!p || p === '~') return '/';
    let devPath = p;
    if (targetLinkId && devPath.startsWith('/' + targetLinkId + '/')) {
      devPath = devPath.slice(targetLinkId.length + 1);
    } else if (targetLinkId && devPath === '/' + targetLinkId) {
      devPath = '/';
    }
    if (devPath === '~') devPath = '/';
    else if (devPath.startsWith('~/')) devPath = '/' + devPath.slice(2);
    else if (!devPath.startsWith('/')) devPath = '/' + devPath;
    return devPath;
  }

  function getTabDir(tab) {
    if (!tab) return '/';
    if (tab.type === 'terminal') {
      return cleanDevicePath(tab.cwd || tab.path);
    }
    const devPath = cleanDevicePath(tab.path);
    return devPath.substring(0, devPath.lastIndexOf('/')) || '/';
  }

  // Determine initial directory to show in explorer
  let initialDirPath = '';
  $: {
    if (type === 'editor' && (filePath || path)) {
      const devPath = cleanDevicePath(filePath || path);
      initialDirPath = devPath.substring(0, devPath.lastIndexOf('/')) || '/';
    } else if (path) {
      initialDirPath = cleanDevicePath(path);
    } else {
      initialDirPath = '/';
    }
  }

  let currentPath = filePath || path || '~';

  let rootNode = {
    name: 'root',
    path: '/',
    type: 'folder',
    open: true,
    children: [],
    loaded: false
  };

  let visibleNodes = [];

  function splitDeviceId(fullPath) {
    if (!fullPath || fullPath === "/") return null;
    const clean = fullPath.startsWith("/") ? fullPath.slice(1) : fullPath;
    const slashIndex = clean.indexOf("/");
    if (slashIndex === -1) {
      return { prefix: clean, path: "/" };
    }
    return {
      prefix: clean.slice(0, slashIndex),
      path: "/" + clean.slice(slashIndex + 1),
    };
  }

  // Flatten the tree for rendering
  $: {
    const arr = [];
    function traverse(node, depth) {
      if (node !== rootNode) {
        arr.push({ ...node, depth, ref: node });
      }
      if (node.open && node.children) {
        for (const child of node.children) {
          traverse(child, depth + 1);
        }
      }
    }
    traverse(rootNode, -1);
    visibleNodes = arr;
  }

  async function loadFolderNode(node) {
    if (!targetLinkId) return;
    const device = deviceRegistry.get(targetLinkId);
    if (!device || !device.drive || device.status !== 'connected') return;

    let fInfoPath = cleanDevicePath(node.path);

    try {
      const textExts = ["txt", "md", "js", "ts", "json", "html", "css", "scss", "cpp", "c", "h", "py", "sh", "bash", "xml", "log", "yml", "yaml", "svelte", "vue", "ini", "conf", "gitignore", "svg", "sql", "graphql", "env"];
      
      const files = await device.drive.getFiles(fInfoPath);
      node.children = files
        .filter(f => {
          if (f.mimeType && f.mimeType.startsWith('application/x-')) return false;
          //if (f.name === 'Terminal' || f.name === 'Chats') return false;
          if (f.type === 'app') return false;
          if (f.type === 'folder' || f.lazy || f.isDir) return true;
          const ext = f.name.includes('.') ? f.name.split('.').pop().toLowerCase() : '';
          return textExts.includes(ext);
        })
        .map(f => ({
          name: f.name,
          type: (f.type === 'folder' || f.lazy || f.isDir) ? 'folder' : 'file',
          path: f.id,
          open: false,
          loaded: false,
          children: []
        }))
        .sort((a, b) => {
          if (a.type === b.type) return a.name.localeCompare(b.name);
          return a.type === 'folder' ? -1 : 1;
        });
      node.loaded = true;
      rootNode = rootNode;
    } catch (e) {
      console.warn("Failed to load directory for workspace tree:", e);
    }
  }

  async function updateTreeForTab(tab) {
    if (!tab) return;
    const targetDir = getTabDir(tab);

    if (rootNode.path !== targetDir || !rootNode.loaded) {
      rootNode.path = targetDir;
      rootNode.loaded = false;
      await loadFolderNode(rootNode);
    }
    currentPath = tab.path;
  }

  async function handleNodeClick(flatNode) {
    const node = flatNode.ref;
    if (node.type === 'folder') {
      node.open = !node.open;
      if (node.open && !node.loaded) {
        await loadFolderNode(node);
      }
      rootNode = rootNode;
    } else {
      await openFileTab(node);
    }
  }

  async function openFileTab(node) {
    const existing = tabs.find(t => cleanDevicePath(t.path) === cleanDevicePath(node.path));
    if (existing) {
      await selectTab(existing.id);
      return;
    }

    let content = '';
    try {
      const devPath = cleanDevicePath(node.path);
      const p2pUrl = `/p2p/${targetLinkId}` + encodeURI(devPath);
      const res = await fetch(p2pUrl);
      content = await res.text();
    } catch (e) {
      console.error("Failed to load file content:", e);
      content = "Error loading file content: " + e.message;
    }

    const newTab = {
      id: 'file-' + node.path,
      title: node.name,
      type: 'editor',
      path: node.path,
      original: content,
      content
    };

    tabs = [...tabs, newTab];
    activeTabId = newTab.id;
    currentPath = node.path;
    if (isMobile) {
      showSidebar = false;
    }
  }

  function addTerminalTab() {
    terminalCounter++;
    const activeT = tabs.find(t => t.id === activeTabId);
    const startPath = activeT ? getTabDir(activeT) : (initialDirPath || '/');
    const newTab = {
      id: 'terminal-' + terminalCounter,
      title: 'Terminal ' + (tabs.filter(t => t.type === 'terminal').length + 1),
      type: 'terminal',
      path: startPath,
      cwd: startPath
    };
    tabs = [...tabs, newTab];
    activeTabId = newTab.id;
    updateTreeForTab(newTab);
  }

  // Receives live session metadata from a Terminal (cwd/title/branch/procTitle).
  // Callback instead of bind:: child writes into props bound inside a keyed
  // {#each} permanently wedge parent updates under Svelte 5 legacy mode.
  function applyTabMeta(tab, m) {
    if (!tab || !m) return;
    let changed = false;
    if (m.cwd != null && tab.cwd !== m.cwd) { tab.cwd = m.cwd; changed = true; }
    if (tab.title !== m.title) { tab.title = m.title; changed = true; }
    if (tab.branch !== m.branch) { tab.branch = m.branch; changed = true; }
    if (tab.procTitle !== m.procTitle) { tab.procTitle = m.procTitle; changed = true; }
    if (changed) tabs = tabs;
  }

  async function selectTab(id) {
    activeTabId = id;
    const tab = tabs.find(t => t.id === id);
    if (tab) {
      await updateTreeForTab(tab);
    }
  }

  function closeTab(id) {
    const index = tabs.findIndex(t => t.id === id);
    if (index === -1) return;

    tabs = tabs.filter(t => t.id !== id);

    if (tabs.length === 0) {
      if (typeof document !== 'undefined') {
        try { document.title = getDomainBrandTitle(); } catch (e) {}
      }
      dispatch('close');
      return;
    }

    if (activeTabId === id) {
      const nextIndex = Math.min(index, tabs.length - 1);
      const nextTab = tabs[nextIndex];
      activeTabId = nextTab.id;
      updateTreeForTab(nextTab);
    }
  }

  onMount(async () => {
    let initialTab;
    if (type === 'editor') {
      initialTab = {
        id: 'file-' + (filePath || path),
        title: fileName || (filePath || path).split('/').pop() || 'Untitled',
        type: 'editor',
        path: filePath || path,
        original: fileContent,
        content: fileContent
      };
      tabs = [initialTab];
      activeTabId = initialTab.id;
    } else {
      terminalCounter = 1;
      initialTab = {
        id: 'terminal-1',
        title: 'Terminal',
        type: 'terminal',
        path: path || '~',
        cwd: '~'
      };
      tabs = [initialTab];
      activeTabId = initialTab.id;
    }

    await updateTreeForTab(initialTab);
    updateMobileState();
    window.addEventListener('resize', updateMobileState);
    if (isMobile) {
      showSidebar = false;
    }
    document.addEventListener('keydown', handleGlobalKeydown);
  });

  onDestroy(() => {
    window.removeEventListener('resize', updateMobileState);
    document.removeEventListener('keydown', handleGlobalKeydown);
  });

  $: activeTab = tabs.find(t => t.id === activeTabId) || null;
  $: activeDevice = deviceRegistry.get(targetLinkId);

  let reviewOpen = false;
  let reviewLoading = false;
  let reviewDiff = "";
  let reviewError = "";
  let reviewTabId = "";
  let reviewCwd = "";
  let reviewBranch = "";

  function closeReview() {
    reviewOpen = false;
  }

  async function requestReview() {
    if (!activeTab) return;
    reviewOpen = true;
    reviewTabId = activeTab.id;
    reviewCwd = activeTab.cwd || "";
    reviewBranch = activeTab.branch || "";
    reviewError = "";
    if (activeTab.type === 'editor') {
      const original = activeTab.original != null ? activeTab.original : activeTab.content;
      reviewDiff = unifiedDiff(original, activeTab.content, activeTab.title);
      reviewLoading = false;
    } else {
      reviewLoading = true;
      reviewDiff = "";
      try {
        const device = deviceRegistry.get(targetLinkId);
        const drive = device && device.drive;
        if (!drive || !drive.isConnected) throw new Error("P2P drive not connected");
        const dir = activeTab.cwd && activeTab.cwd !== "~" ? activeTab.cwd : "~";
        const branch = (activeTab.branch || "").replace(/[^A-Za-z0-9_@.\/-]/g, "");
        let input;
        if (branch) {
          const base = "origin/" + branch;
          input = 'if git rev-parse --verify -q "' + base + '" >/dev/null 2>&1; then git diff --no-color "' + base + '"; elif git rev-parse -q --verify HEAD >/dev/null 2>&1; then git diff --no-color HEAD; fi';
        } else {
          input = 'if git rev-parse -q --verify HEAD >/dev/null 2>&1; then git diff --no-color HEAD; fi';
        }
        const result = await drive.exec(dir, input);
        let text = "";
        if (result && result.stdout) {
          const bin = atob(result.stdout);
          const bytes = Uint8Array.from(bin, (m) => m.charCodeAt(0));
          text = new TextDecoder().decode(bytes);
        }
        reviewDiff = text;
        reviewLoading = false;
      } catch (e) {
        reviewLoading = false;
        reviewDiff = "";
        reviewError = String((e && e.message) || e);
      }
    }
  }

  onDestroy(() => {
    if (typeof document !== 'undefined') {
      try { document.title = getDomainBrandTitle(); } catch (e) {}
    }
  });

  $: if (reviewOpen && activeTabId !== reviewTabId) {
    reviewOpen = false;
  }

  $: {
    if (activeTab && activeTab.type === 'terminal' && activeTab.cwd) {
      const devPath = cleanDevicePath(activeTab.cwd);
      if (rootNode.path !== devPath) {
        rootNode.path = devPath;
        rootNode.loaded = false;
        loadFolderNode(rootNode);
      }
      if (reviewOpen && activeTab.id === reviewTabId && (activeTab.cwd !== reviewCwd || activeTab.branch !== reviewBranch)) {
        requestReview();
      }
    }
  }
</script>

<div class="workspace-container">
  <div class="workspace-body">
    <!-- Sidebar -->
    {#if showSidebar}
      {#if isMobile}
        <div class="sidebar-backdrop" on:click={() => showSidebar = false}></div>
      {/if}
      <div class="sidebar" style="width: {isMobile ? '280px' : sidebarWidth + 'px'}">
        <div class="explorer">
          <span class="explorer_span">Explorer</span>
          <button 
            class="sidebar-toggle-btn" 
            title="Hide Explorer" 
            on:click={() => showSidebar = false}
          >
            <svg width="14" height="14" viewBox="0 0 16 16"><path fill="currentColor" d="M1 3.5A1.5 1.5 0 0 1 2.5 2h11A1.5 1.5 0 0 1 15 3.5v9a1.5 1.5 0 0 1-1.5 1.5h-11A1.5 1.5 0 0 1 1 12.5v-9zM2.5 3a.5.5 0 0 0-.5.5V12.5a.5.5 0 0 0 .5.5H5V3H2.5zm3.5 10h7.5a.5.5 0 0 0 .5-.5V3.5a.5.5 0 0 0-.5-.5H6v10z"/></svg>
          </button>
        </div>
        <div class="file-list">
          {#each visibleNodes as node, i}
            <div class="file-row" 
                 class:active={cleanDevicePath(node.path) === cleanDevicePath(currentPath)}
                 style="padding-left: {node.depth * 16 + 8}px"
                 on:click={() => handleNodeClick(node)}>
              <div class="icon">
                {#if node.type === 'folder'}
                  <svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg">
                    <mask id="folder-mask-1-{i}" fill="white"><rect x="1" y="4" width="12" height="9" rx="1"/></mask>
                    <rect x="1" y="4" width="12" height="9" rx="1" stroke="#8C94A1" stroke-width="2.4" mask="url(#folder-mask-1-{i})"/>
                    <mask id="folder-mask-2-{i}" fill="white"><path d="M1 3C1 2.44772 1.44772 2 2 2H5C5.55228 2 6 2.44772 6 3V5H1V3Z"/></mask>
                    <path d="M1 3C1 2.44772 1.44772 2 2 2H5C5.55228 2 6 2.44772 6 3V5H1V3Z" stroke="#8C94A1" stroke-width="2.4" mask="url(#folder-mask-2-{i})"/>
                  </svg>
                {:else}
                  <svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg">
                    <mask id="file-mask-{i}" fill="white"><rect x="2" y="1" width="10" height="12" rx="1"/></mask>
                    <rect x="2" y="1" width="10" height="12" rx="1" stroke="#8C94A1" stroke-width="2.4" mask="url(#file-mask-{i})"/>
                  </svg>
                {/if}
              </div>
              <div class="file-name" title={node.name}><span class="file-name_span">{node.name}</span></div>
            </div>
          {/each}
        </div>
        <div class="resize-handle" on:mousedown={onSidebarDragStart}></div>
      </div>
    {/if}

    <!-- Main Area -->
    <div class="main-area">
      <!-- Tab Bar -->
      <div class="tab-bar">
        {#if !showSidebar}
          <!-- Toggle Explorer Button (only when sidebar is hidden) -->
          <button 
            class="tab-btn" 
            title="Show Explorer" 
            on:click={() => showSidebar = true}
          >
            <svg width="14" height="14" viewBox="0 0 16 16"><path fill="currentColor" d="M1 3.5A1.5 1.5 0 0 1 2.5 2h11A1.5 1.5 0 0 1 15 3.5v9a1.5 1.5 0 0 1-1.5 1.5h-11A1.5 1.5 0 0 1 1 12.5v-9zM2.5 3a.5.5 0 0 0-.5.5V12.5a.5.5 0 0 0 .5.5H5V3H2.5zm3.5 10h7.5a.5.5 0 0 0 .5-.5V3.5a.5.5 0 0 0-.5-.5H6v10z"/></svg>
          </button>
        {/if}

        {#each tabs as tab (tab.id)}
          <div 
            class="tab" 
            class:active={activeTabId === tab.id}
            on:click={() => selectTab(tab.id)}
          >
            {#if tab.type === 'terminal'}
              <svg class="tab-icon" viewBox="0 0 16 16"><path fill="currentColor" d="M0 3a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2zm9.5 5.5h-3a.5.5 0 0 0 0 1h3a.5.5 0 0 0 0-1m-6.354-.354a.5.5 0 1 0 .708.708l2-2a.5.5 0 0 0 0-.708l-2-2a.5.5 0 1 0-.708.708L4.793 6.5z"/></svg>
            {:else}
              <svg class="tab-icon" viewBox="0 0 24 24"><path fill="currentColor" d="M14 2H6c-1.1 0-1.99.9-1.99 2L4 20c0 1.1.89 2 1.99 2H18c1.1 0 2-.9 2-2V8l-6-6z"/></svg>
            {/if}
            <span class="tab-label">{tab.title}</span>
            <button class="tab-close" on:click|stopPropagation={() => closeTab(tab.id)}>
              <svg viewBox="0 0 24 24"><path fill="currentColor" d="M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12 19 6.41z"/></svg>
            </button>
          </div>
        {/each}

        <!-- Add Terminal Button -->
        <button 
          class="tab-btn add-term-btn" 
          title="New Terminal" 
          on:click={addTerminalTab}
        >
          <svg width="14" height="14" viewBox="0 0 16 16"><path fill="currentColor" d="M0 3a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2zm9.5 5.5h-3a.5.5 0 0 0 0 1h3a.5.5 0 0 0 0-1m-6.354-.354a.5.5 0 1 0 .708.708l2-2a.5.5 0 0 0 0-.708l-2-2a.5.5 0 1 0-.708.708L4.793 6.5z"/></svg>
        </button>

        <!-- Right-side buttons: Save + Review -->
        <div class="tab-bar-right">
          {#if activeTab && activeTab.type === 'editor'}
            <button 
              class="tab-btn save-btn"
              class:saving
              title="Save (Ctrl+S)"
              disabled={saving}
              on:click={saveFile}
            >
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>
            </button>
          {/if}
          <button 
            class="tab-btn review-btn" 
            class:active={reviewOpen}
            title="Review current tab (git diff)" 
            disabled={!activeTab}
            on:click={() => reviewOpen ? closeReview() : requestReview()}
          >
            <svg width="14" height="14" viewBox="0 0 16 16"><path fill="currentColor" d="M7.5 2h1v4.5H13v1H8.5V12h-1V7.5H3v-1h4.5V2z"/><path fill="currentColor" d="M3 12.5h10v1H3v-1z"/></svg>
          </button>
        </div>
      </div>

      <!-- Content Area -->
      <div class="content-area">
        {#if saving}
          <div class="saving-banner">
            <span class="saving-dots"><i></i><i></i><i></i></span> Saving
          </div>
        {/if}
        <div class="panes">
          {#each tabs as tab (tab.id)}
            <div class={tab.type === 'terminal' ? 'terminal-pane' : 'editor-pane'} style="display: {activeTabId === tab.id ? 'block' : 'none'};">
              {#if tab.type === 'terminal'}
                <div class="term-row">
                  <Terminal
                    targetLinkId={targetLinkId}
                    port={port}
                    service={service}
                    initialPath={tab.path}
                    showCloseButton={false}
                    initTag={tab.id}
                    fsDrive={activeDevice ? activeDevice.drive : null}
                    onmeta={(m) => applyTabMeta(tab, m)}
                    bind:this={tab.term}
                  />
                </div>
              {:else if tab.type === 'editor'}
                <Editor 
                  value={tab.content} 
                  filename={tab.title} 
                  showHeader={false}
                  on:close={() => closeTab(tab.id)}
                  on:change={(e) => tab.content = e.detail}
                />
              {/if}
            </div>
          {/each}
        </div>

        {#if reviewOpen}
          {#if isMobile}
            <div class="review-backdrop" on:click={() => reviewOpen = false}></div>
          {/if}
          <div class="review-panel" style="width: {isMobile ? '90vw' : reviewWidth + 'px'}">
            <div class="resize-handle review-resize" on:mousedown={onReviewDragStart}></div>
            <div class="review-header">
              <span>Review — {activeTab ? activeTab.title : ''}</span>
            </div>
            <div class="review-body">
              {#if reviewLoading}
                <div class="review-empty">Fetching diff…</div>
              {:else if reviewDiff}
                {#if activeTab && activeTab.type === 'editor'}
                  <ReviewDiff
                    mode="diff-editor"
                    original={activeTab.original != null ? activeTab.original : activeTab.content}
                    modified={activeTab.content}
                    filename={activeTab.title}
                  />
                {:else}
                  <ReviewDiff mode="unified" diff={reviewDiff} />
                {/if}
              {:else if reviewError}
                <div class="review-empty review-error">{reviewError}</div>
              {:else}
                <div class="review-empty">No changes</div>
              {/if}
            </div>
          </div>
        {/if}
      </div>

      <!-- Status Bar -->
      <div class="status-bar">
        <div class="status-left">
          <span>{activeTab && activeTab.type === 'editor' ? (activeTab.title ? getLanguageFromFilename(activeTab.title) : 'Plain Text') : (activeTab && activeTab.procTitle ? activeTab.procTitle.replace(/^[\s-]+/, '') : 'Shell')}</span>
        </div>
        <div class="status-right">
          <svg class="status-icon" viewBox="0 0 16 16"><path fill="currentColor" d="M11.75 2.5a.75.75 0 1 0 0 1.5.75.75 0 0 0 0-1.5zm-2.25.75a2.25 2.25 0 1 1 3 2.122V6A2.5 2.5 0 0 1 10 8.5H6a1 1 0 0 0-1 1v1.128a2.251 2.251 0 1 1-1.5 0V5.372a2.25 2.25 0 1 1 1.5 0v1.836A2.493 2.493 0 0 1 6 7h4a1 1 0 0 0 1-1v-.628A2.25 2.25 0 0 1 9.5 3.25zM4.25 12a.75.75 0 1 0 0 1.5.75.75 0 0 0 0-1.5zM3.5 3.25a.75.75 0 1 1 1.5 0 .75.75 0 0 1-1.5 0z"/></svg>
          <span>{activeTab && activeTab.branch ? activeTab.branch : 'No git repository'}</span>
        </div>
      </div>
    </div>
  </div>
</div>

<style>
  .workspace-container {
    --wx-color-primary: #5863A6;
    --wx-color-primary-hover: #7B86C4;
    --wx-background: #1F2430;
    --wx-background-alt: #191D28;
    --wx-background-hover: #282E3D;
    --wx-color-font: #E9ECED;
    --wx-color-font-alt: #7E869B;
    --border-color: #32394B;
    --wx-icon-color: #7E869B;
    --top-menu-color: #929CD4;
    --top-menu-hover-color: #7dd3fc;
    color-scheme: dark;

    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    background: var(--wx-background, #1F2430);
    color: var(--wx-color-font, #f1f2f4);
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
    overflow: hidden;
    border-radius: 0;
  }

  .workspace-body {
    display: flex;
    flex: 1;
    overflow: hidden;
    min-height: 0;
  }

  .sidebar {
    align-self: stretch;
    padding: 4px 6px 6px 6px;
    position: relative;
    background: var(--wx-background-alt, #191D28);
    border-right: 1px solid var(--border-color, #32394B);
    flex-direction: column;
    justify-content: flex-start;
    align-items: flex-start;
    gap: 4px;
    display: flex;
    min-height: 0;
    flex-shrink: 0;
  }

  .explorer {
    align-self: stretch;
    padding-left: 12px;
    padding-right: 8px;
    justify-content: space-between;
    align-items: center;
    display: flex;
    flex-shrink: 0;
    box-sizing: border-box;
    height: 24px;
  }

  .sidebar-toggle-btn {
    background: transparent;
    border: none;
    color: #8C94A1;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 20px;
    height: 20px;
    padding: 0;
    border-radius: 4px;
    transition: all 0.12s ease;
  }

  .sidebar-toggle-btn:hover {
    color: #ffffff;
    background: rgba(255, 255, 255, 0.1);
  }

  .explorer_span {
    color: #8C94A1;
    font-size: 11px;
    font-family: Inter, sans-serif;
    font-weight: 700;
    line-height: 14.30px;
    text-transform: uppercase;
    word-wrap: break-word;
  }

  .file-list {
    align-self: stretch;
    padding-top: 8px;
    flex-direction: column;
    justify-content: flex-start;
    align-items: flex-start;
    gap: 2px;
    display: flex;
    flex: 1;
    overflow-y: auto;
    scrollbar-width: thin;
    scrollbar-color: var(--border-color, #32394B) transparent;
  }

  .file-list::-webkit-scrollbar {
    width: 6px;
    height: 6px;
  }

  .file-list::-webkit-scrollbar-track {
    background: transparent;
  }

  .file-list::-webkit-scrollbar-thumb {
    background: var(--border-color, #32394B);
    border-radius: 3px;
  }

  .file-list::-webkit-scrollbar-thumb:hover {
    background: #7E869B;
  }

  .file-row {
    width: 100%;
    height: 24px;
    padding-left: 8px;
    justify-content: flex-start;
    align-items: center;
    gap: 6px;
    display: inline-flex;
    box-sizing: border-box;
    cursor: pointer;
  }
  
  .file-row:hover {
    background: rgba(255,255,255,0.05);
    border-radius: 4px;
  }

  .icon {
    position: relative;
    flex-shrink: 0;
  }

  .file-name {
    flex: 1;
    overflow: hidden;
  }

  .file-name_span {
    color: #E9ECED;
    font-size: 13px;
    font-family: Inter, sans-serif;
    font-weight: 500;
    line-height: 19.50px;
    display: block;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .main-area {
    flex: 1;
    display: flex;
    flex-direction: column;
    min-width: 0;
  }

  .tab-bar {
    display: flex;
    background: var(--wx-background-alt, #191D28);
    height: calc(32px + env(safe-area-inset-top, 0px));
    padding-top: env(safe-area-inset-top, 0px);
    align-items: flex-end;
    border-bottom: 1px solid var(--border-color, #32394B);
    box-sizing: border-box;
  }

  .tab-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    height: 32px;
    width: 36px;
    border: none;
    border-right: 1px solid var(--border-color, #32394B);
    background: transparent;
    color: var(--wx-color-font-alt, #A5B5C1);
    cursor: pointer;
    padding: 0;
    flex-shrink: 0;
  }

  .tab-btn.add-term-btn {
    border-right: none;
  }

  .tab-bar-right {
    display: flex;
    margin-left: auto;
  }

  .tab-btn.review-btn {
    border-right: none;
  }

  .tab-btn.save-btn.saving {
    color: #929CD4;
    background: rgba(146, 156, 212, 0.14);
  }

  .tab-btn:disabled {
    opacity: 0.35;
    cursor: default;
  }

  .tab-btn:disabled:hover {
    color: var(--wx-color-font-alt, #A5B5C1);
    background: transparent;
  }

  .tab-btn:hover {
    color: #ffffff;
    background: rgba(255,255,255,0.05);
  }

  .tab-btn.active {
    color: #00aadd;
    background: rgba(255,255,255,0.05);
  }

  .tab {
    display: flex;
    align-items: center;
    height: 32px;
    padding: 0 16px;
    border-right: 1px solid var(--border-color, #32394B);
    font-size: 12px;
    color: var(--wx-color-font-alt, #A5B5C1);
    cursor: pointer;
    min-width: 120px;
    max-width: 200px;
  }

  .tab.active {
    background: var(--wx-background, #1F2430);
    color: var(--wx-color-font, #ffffff);
    border-bottom: 1px solid var(--wx-background, #1F2430);
    margin-bottom: -1px;
    padding-bottom: 1px;
  }

  .tab-icon {
    width: 14px;
    height: 14px;
    margin-right: 8px;
    flex-shrink: 0;
    color: #00aadd;
  }

  .tab-label {
    flex: 1;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .tab-close {
    background: none;
    border: none;
    color: inherit;
    opacity: 0.5;
    cursor: pointer;
    padding: 2px;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 4px;
    margin-left: 4px;
  }

  .tab-close:hover {
    opacity: 1;
    background-color: rgba(255,255,255,0.1);
  }

  .tab-close svg {
    width: 14px;
    height: 14px;
  }

  .content-area {
    flex: 1;
    background: var(--wx-background, #1F2430);
    position: relative;
    overflow: hidden;
    padding: 0;
    padding-bottom: env(safe-area-inset-bottom, 0px);
    box-sizing: border-box;
    display: flex;
    flex-direction: row;
    align-items: stretch;
    min-height: 0;
  }

  .saving-banner {
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    z-index: 90;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    height: 26px;
    background: rgba(45, 55, 65, 0.92);
    color: #8899aa;
    font-size: 12px;
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
    letter-spacing: 0.02em;
    border-bottom: 1px solid rgba(80, 100, 120, 0.3);
    backdrop-filter: blur(4px);
    animation: saving-banner-in 0.2s ease;
  }

  @keyframes saving-banner-in {
    from { opacity: 0; transform: translateY(-100%); }
    to { opacity: 1; transform: translateY(0); }
  }

  .saving-dots {
    display: inline-flex;
    align-items: center;
    gap: 3px;
    height: 1em;
  }
  .saving-dots i {
    display: block;
    width: 4px;
    height: 4px;
    border-radius: 50%;
    background: #929CD4;
    animation: saving-dot-blink 1.4s infinite;
    font-style: normal;
  }
  .saving-dots i:nth-child(2) { animation-delay: 0.2s; }
  .saving-dots i:nth-child(3) { animation-delay: 0.4s; }

  @keyframes saving-dot-blink {
    0%, 20% { opacity: 0.2; }
    50% { opacity: 1; }
    80%, 100% { opacity: 0.2; }
  }

  .panes {
    flex: 1;
    min-width: 0;
    position: relative;
  }

  .terminal-pane {
    width: 100%;
    height: 100%;
    padding: 16px;
    box-sizing: border-box;
  }

  .term-row {
    display: flex;
    height: 100%;
    min-height: 0;
  }

  .term-row :global(.terminal-app) {
    flex: 1;
    min-width: 0;
    width: auto;
  }

  .review-panel {
    height: auto;
    display: flex;
    flex-direction: column;
    background: var(--wx-background-alt, #191D28);
    border-left: 1px solid var(--border-color, #32394B);
    border-radius: 6px;
    overflow: hidden;
    flex-shrink: 0;
    align-self: stretch;
    position: relative;
  }

  .review-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 6px 12px;
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--wx-color-font-alt, #aab3bb);
    border-bottom: 1px solid var(--border-color, #32394B);
    flex-shrink: 0;
  }

  .review-close {
    background: transparent;
    border: none;
    color: #aab3bb;
    font-size: 16px;
    line-height: 1;
    cursor: pointer;
    padding: 2px 4px;
  }

  .review-close:hover {
    color: #ffffff;
  }

  .review-body {
    flex: 1;
    overflow: hidden;
    min-height: 0;
  }

  .review-body > :global(*) {
    width: 100%;
    height: 100%;
  }

  .resize-handle {
    position: absolute;
    top: 0;
    width: 5px;
    height: 100%;
    cursor: col-resize;
    z-index: 10;
    background: transparent;
    transition: background 0.15s;
  }
  .resize-handle:hover,
  .resize-handle:active {
    background: rgba(100, 160, 220, 0.35);
  }

  /* sidebar handle: right edge */
  .sidebar > .resize-handle {
    right: -3px;
  }

  /* review handle: left edge */
  .review-resize {
    left: -3px;
  }

  .review-empty {
    padding: 18px;
    color: #7c8790;
    font-size: 13px;
    text-align: center;
  }

  .review-error {
    color: #e06c75;
  }

  .editor-pane {
    width: 100%;
    height: 100%;
    padding: 8px 0 0 0;
    box-sizing: border-box;
  }

  .terminal-pane > :global(*),
  .editor-pane > :global(*) {
    width: 100%;
    height: 100%;
  }

  .status-bar {
    height: 24px;
    background: var(--wx-background-alt, #191D28);
    border-top: 1px solid var(--border-color, #32394B);
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 16px;
    font-size: 11px;
    color: var(--wx-color-font-alt, rgba(255, 255, 255, 0.60));
  }

  .status-left, .status-right {
    display: flex;
    align-items: center;
  }

  .status-icon {
    width: 12px;
    height: 12px;
    margin-right: 4px;
    opacity: 0.8;
  }

  /* Mobile Flyout Overlays (<768px) */
  @media (max-width: 768px) {
    .workspace-body {
      position: relative;
    }

    .sidebar-backdrop,
    .review-backdrop {
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background: rgba(0, 0, 0, 0.55);
      backdrop-filter: blur(2px);
      -webkit-backdrop-filter: blur(2px);
      z-index: 99;
    }

    .sidebar {
      position: absolute !important;
      top: 0;
      left: 0;
      bottom: 0;
      z-index: 100;
      max-width: 85vw !important;
      width: 280px !important;
      box-shadow: 4px 0 20px rgba(0, 0, 0, 0.5);
    }

    .review-panel {
      position: absolute !important;
      top: 0;
      right: 0;
      bottom: 0;
      z-index: 100;
      max-width: 90vw !important;
      width: 90vw !important;
      box-shadow: -4px 0 20px rgba(0, 0, 0, 0.5);
    }

    .sidebar > .resize-handle,
    .review-panel > .resize-handle {
      display: none !important;
    }
  }
</style>
