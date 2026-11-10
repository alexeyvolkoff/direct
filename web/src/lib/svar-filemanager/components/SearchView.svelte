<script>
	import { getContext, onMount, onDestroy } from "svelte";
	import Icon from "./ui/Icon.svelte";
	import { whitelist } from "../icons.js";
	import { _, translate } from "../../i18n/index.js";

	let { httpDrive = null, searchDrivesProvider = null } = $props();

	const api = getContext("filemanager-store");
	const driveContext = getContext("filemanager-http-drive");

	const { panels, activePanel, search: searchQuery, searchViewMode } = api.getReactiveState();
	let crumbs = $derived($panels[$activePanel]?._crumbs || []);
	let currentPath = $derived($panels[$activePanel]?.path || "/");

	// State
	let folderMatches = $state([]);
	let semanticResults = $state([]);
	let searching = $state(false);
	let error = $state(null);
	let searchDurationMs = $state(0);
	let viewMode = $derived(searchViewMode && $searchViewMode === "cards" ? "grid" : "list");
	let filterTab = $state("all"); // "all" | "folder" | "semantic"
	let copiedId = $state(null);
	let currentQuery = $state("");

	// Computed combined results
	let combinedResults = $derived.by(() => {
		if (filterTab === "folder") return folderMatches;
		if (filterTab === "semantic") return semanticResults;
		// "all": show in-folder matches first, then semantic results (deduplicating by itemPath)
		const seen = new Set(folderMatches.map((m) => m.itemPath || m.id));
		const uniqueSemantic = semanticResults.filter((s) => !seen.has(s.itemPath || s.id));
		return [...folderMatches, ...uniqueSemantic];
	});

	let totalCount = $derived(folderMatches.length + semanticResults.length);

	function clearSearch() {
		api.exec("filter-files", {
			text: "",
		});
	}

	function getTargetDrives(path) {
		if (searchDrivesProvider && typeof searchDrivesProvider === "function") {
			try {
				const targets = searchDrivesProvider(path);
				if (Array.isArray(targets) && targets.length > 0) return targets;
			} catch (e) {
				console.warn("[SearchView] searchDrivesProvider error:", e);
			}
		}

		if (typeof window !== "undefined" && window.__omdGetSearchDrives && typeof window.__omdGetSearchDrives === "function") {
			try {
				const targets = window.__omdGetSearchDrives(path);
				if (Array.isArray(targets) && targets.length > 0) return targets;
			} catch (_) {}
		}

		// Fallback to single drive
		let singleDrive = null;
		if (httpDrive && typeof httpDrive.fetch === "function") singleDrive = httpDrive;
		else if (driveContext && typeof driveContext === "function") singleDrive = driveContext();
		else if (typeof window !== "undefined" && window.__omdHttpDrive) singleDrive = window.__omdHttpDrive;

		return singleDrive ? [{ drive: singleDrive, device: null }] : [];
	}

	function getFileIcon(fileName, type = "file", item = null) {
		const isFolder = type === "folder" || item?.isFolder || item?.contentType === "folder";
		const ext = fileName && fileName.includes(".") ? fileName.split(".").pop().toLowerCase() : "";

		// 1. Check if Filemanager templates provide custom icon (e.g. OMD branded SVG Data URIs)
		if (api?.templates?.icon) {
			try {
				const fileObj = {
					id: item?.itemPath || item?.id || fileName,
					name: fileName,
					ext,
					type: isFolder ? "folder" : "file",
					...(item || {}),
				};
				const custom = api.templates.icon(fileObj, "big");
				if (custom) return custom;
			} catch (_) {}
		}

		// 2. Direct Svar Vivid icon set fallback (big size 64px)
		if (isFolder) {
			return "https://cdn.svar.dev/icons/filemanager/vivid/big/folder.svg";
		}
		const icon = whitelist[ext] ? ext : "file";
		return `https://cdn.svar.dev/icons/filemanager/vivid/big/${icon}.svg`;
	}

	function fixMojibake(str) {
		if (!str || typeof str !== "string") return str || "";
		try {
			if (/[\u00C2-\u00DF][\u0080-\u00BF]/.test(str)) {
				return decodeURIComponent(escape(str));
			}
		} catch (_) {}
		return str;
	}

	function getScoreBadgeClass(score) {
		const num = typeof score === "number" ? score : parseFloat(score) || 0;
		if (num >= 80) return "score-high";
		if (num >= 60) return "score-medium";
		return "score-low";
	}

	function formatDate(val) {
		if (!val) return "";
		try {
			const d = new Date(val);
			if (isNaN(d.getTime())) return String(val);
			return d.toLocaleDateString(undefined, {
				year: "numeric",
				month: "short",
				day: "numeric",
				hour: "2-digit",
				minute: "2-digit",
			});
		} catch (_) {
			return String(val);
		}
	}

	function highlightMatch(text, query) {
		if (!text) return "";
		const clean = fixMojibake(text);
		if (!query || !query.trim()) return escapeHtml(clean);

		const words = query
			.trim()
			.split(/\s+/)
			.filter((w) => w.length > 1)
			.map((w) => escapeRegExp(w));

		if (words.length === 0) return escapeHtml(clean);

		const regex = new RegExp(`(${words.join("|")})`, "gi");
		const escaped = escapeHtml(clean);
		return escaped.replace(regex, '<mark class="wx-search-hl">$1</mark>');
	}

	function escapeHtml(str) {
		return str
			.replace(/&/g, "&amp;")
			.replace(/</g, "&lt;")
			.replace(/>/g, "&gt;")
			.replace(/"/g, "&quot;")
			.replace(/'/g, "&#039;");
	}

	function escapeRegExp(str) {
		return str.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
	}

	function openFile(item) {
		const targetPath = item.itemPath || item.id;
		if (!targetPath) return;

		// 1. If folder: navigate to it
		if (item.isFolder || item.contentType === "folder") {
			api.exec("filter-files", { text: "" });
			setTimeout(() => {
				api.exec("set-path", { id: targetPath });
			}, 50);
			return;
		}

		// 2. If in FM store, trigger open-file
		const existing = api.getFile(targetPath);
		if (existing && existing.type !== "folder") {
			api.exec("open-file", { id: targetPath, file: existing });
			return;
		}

		// 3. Dispatch global open event for App.svelte
		if (typeof window !== "undefined") {
			const fileName = item.title || (targetPath.includes("/") ? targetPath.split("/").pop() : targetPath);
			const devPrefix = item.device?.prefix || (item.linkId ? "/" + item.linkId : "");
			let realPath = targetPath;
			if (devPrefix && realPath.startsWith(devPrefix + "/")) {
				realPath = realPath.substring(devPrefix.length);
			} else if (devPrefix && realPath === devPrefix) {
				realPath = "/";
			}
			if (!realPath.startsWith("/")) realPath = "/" + realPath;

			const fileObj = {
				id: targetPath,
				path: targetPath,
				realPath: realPath,
				fullPath: targetPath,
				itemPath: targetPath,
				name: fileName,
				title: fileName,
				value: fileName,
				type: "file",
				mimeType: item.contentType || item.mimeType || "",
				contentType: item.contentType || item.mimeType || "",
				...item,
			};
			fileObj.realPath = realPath;
			window.dispatchEvent(
				new CustomEvent("omd-open-file", {
					detail: {
						source: fileObj,
						file: fileObj,
					},
				})
			);
		}
	}

	function showInFolder(item, e) {
		if (e) e.stopPropagation();
		const targetPath = item.itemPath || item.id;
		if (!targetPath) return;

		const lastSlash = targetPath.lastIndexOf("/");
		const parentFolder = lastSlash > 0 ? targetPath.substring(0, lastSlash) : "/";

		api.exec("filter-files", { text: "" });

		setTimeout(() => {
			api.exec("set-path", { id: parentFolder });
			api.exec("select-file", { id: targetPath });
		}, 60);
	}

	function copyPath(item, e) {
		if (e) e.stopPropagation();
		const targetPath = item.itemPath || item.id;
		if (!targetPath) return;

		if (typeof navigator !== "undefined" && navigator.clipboard) {
			navigator.clipboard.writeText(targetPath);
			copiedId = targetPath;
			setTimeout(() => {
				if (copiedId === targetPath) copiedId = null;
			}, 2000);
		}
	}

	function downloadFile(item, e) {
		if (e) e.stopPropagation();
		const targetPath = item.itemPath || item.id;
		if (!targetPath) return;

		const existing = api.getFile(targetPath);
		if (existing) {
			api.exec("download-file", { id: targetPath });
		} else {
			const fileName = item.title || targetPath.split("/").pop();
			const link = document.createElement("a");
			link.href = `/p2p${targetPath.startsWith("/") ? "" : "/"}${encodeURI(targetPath)}?download=true`;
			link.download = fileName;
			link.click();
		}
	}

	// 1. Instant local filter for current folder
	function updateCurrentFolderMatches(q) {
		if (!q || !q.trim() || currentPath === "/") {
			folderMatches = [];
			return;
		}

		try {
			const store = api.getState();
			const curFolder = store.data.byId(currentPath);
			const directChildren = curFolder?.data || [];
			const qLower = q.trim().toLowerCase();

			const matches = directChildren
				.filter((f) => f && (f.name || f.value) && (f.name || f.value).toLowerCase().includes(qLower))
				.map((f) => ({
					id: f.id,
					itemPath: f.id,
					title: f.name || f.value || f.id.split("/").pop(),
					snippet: f.type === "folder" ? translate("folder_in_current_location", {}, "Folder in current location") : translate("file_in_current_location", {}, "File in current location"),
					relevance: 100,
					owner: f.owner || "",
					contentType: f.type || "file",
					last_modified: f.date || "",
					inCurrentFolder: true,
					isFolder: f.type === "folder",
				}));

			folderMatches = matches;
		} catch (err) {
			console.warn("[SearchView] Error checking current folder matches:", err);
			folderMatches = [];
		}
	}

	// 2. Semantic search across target node(s)
	let debounceTimer = null;

	const withTimeout = (promise, ms, label) =>
		Promise.race([
			promise,
			new Promise((_, rej) => setTimeout(() => rej(new Error(`timeout ${label}`)), ms)),
		]);

	async function runMultiNodeSearch(query) {
		if (!query || !query.trim()) {
			semanticResults = [];
			searching = false;
			error = null;
			return;
		}

		searching = true;
		error = null;
		const startTime = performance.now();
		const trimmedQuery = query.trim();

		try {
			const targets = getTargetDrives(currentPath);
			const ep = `/search?q=${encodeURIComponent(trimmedQuery)}&limit=50`;

			if (!targets || targets.length === 0) {
				// No targets via P2P drive, try direct fetch
				const directRes = await withTimeout(fetch(ep), 8000, "direct");
				if (!directRes.ok) throw new Error(`HTTP ${directRes.status}`);
				const data = await directRes.json();
				semanticResults = parseResponseItems(data, null);
				searchDurationMs = Math.round(performance.now() - startTime);
				return;
			}

			// Fire parallel requests to all candidate nodes
			const fetchPromises = targets.map(async ({ drive, device }) => {
				try {
					const res = await withTimeout(drive.fetch(ep), 8000, `node ${device?.name || device?.linkId || "default"}`);
					if (!res || !res.ok) return [];
					const data = await res.json();
					return parseResponseItems(data, device);
				} catch (err) {
					console.warn(`[SearchView] Search error on node ${device?.name || device?.linkId || "default"}:`, err);
					return [];
				}
			});

			const resultsArray = await Promise.allSettled(fetchPromises);
			const merged = [];

			for (const outcome of resultsArray) {
				if (outcome.status === "fulfilled" && Array.isArray(outcome.value)) {
					merged.push(...outcome.value);
				}
			}

			// Deduplicate by itemPath across nodes (keeping highest relevance)
			const docMap = new Map();
			for (const item of merged) {
				const key = item.itemPath || item.id;
				if (!docMap.has(key) || (item.relevance || 0) > (docMap.get(key).relevance || 0)) {
					docMap.set(key, item);
				}
			}

			const finalSemantic = Array.from(docMap.values());
			finalSemantic.sort((a, b) => (b.relevance || 0) - (a.relevance || 0));

			semanticResults = finalSemantic;
			searchDurationMs = Math.round(performance.now() - startTime);
		} catch (err) {
			console.warn("[SearchView] Multi-node search error:", err);
			error = err.message || translate("search_error_nodes", {}, "Failed to complete search across nodes");

			// Local fallback if semantic search failed
			if (semanticResults.length === 0) {
				try {
					const localFiles = api.getState().data.findFiles(trimmedQuery);
					semanticResults = (localFiles || []).map((f) => ({
						id: f.id,
						itemPath: f.id,
						title: f.name || f.value || f.id.split("/").pop(),
						snippet: f.name || "",
						relevance: 90,
						owner: "",
						contentType: f.type || "file",
						last_modified: f.date || "",
						isLocalFallback: true,
					}));
				} catch (_) {}
			}
			searchDurationMs = Math.round(performance.now() - startTime);
		} finally {
			searching = false;
		}
	}

	function parseResponseItems(data, device) {
		const items = [];
		if (!data || typeof data !== "object") return items;

		const devPrefix = device?.prefix || "";
		const devName = device?.name || device?.linkId || "";

		function cleanRawPath(p, owner) {
			if (!p) return "";
			let res = p;
			if (res.startsWith("http://") || res.startsWith("https://")) {
				try {
					res = new URL(res).pathname;
				} catch (_) {}
			}
			if (owner && res.startsWith(`/${owner}/`)) {
				res = res.substring(owner.length + 1);
			}
			return res;
		}

		if (Array.isArray(data)) {
			for (const it of data) {
				if (it && (it.itemPath || it.id)) {
					let fullPath = cleanRawPath(it.itemPath || it.id, it.owner);
					if (devPrefix && !fullPath.startsWith(devPrefix)) {
						fullPath = devPrefix + (fullPath.startsWith("/") ? fullPath : "/" + fullPath);
					}
					if (!fullPath.startsWith("/")) fullPath = "/" + fullPath;
					items.push({
						...it,
						id: fullPath,
						itemPath: fullPath,
						deviceName: devName,
						deviceIcon: device?.icon || null,
						linkId: device?.linkId || "",
						device: device,
					});
				}
			}
		} else {
			for (const [k, v] of Object.entries(data)) {
				if (k === "system_info" || !v || typeof v !== "object") continue;
				let path = cleanRawPath(v.itemPath || k, v.owner);
				if (devPrefix && !path.startsWith(devPrefix)) {
					path = devPrefix + (path.startsWith("/") ? path : "/" + path);
				}
				if (!path.startsWith("/")) path = "/" + path;
				items.push({
					id: path,
					itemPath: path,
					title: v.title || path.split("/").pop(),
					snippet: v.snippet || "",
					relevance: typeof v.relevance === "number" ? v.relevance : parseFloat(v.relevance) || 0,
					owner: v.owner || "",
					contentType: v.contentType || "",
					last_modified: v.last_modified || "",
					image_preview: v.image_preview || "",
					description: v.description || "",
					isFolder: !!v.isFolder || !!v.is_folder || v.contentType === "folder",
					deviceName: devName,
					deviceIcon: device?.icon || null,
					linkId: device?.linkId || "",
					device: device,
				});
			}
		}
		return items;
	}

	$effect(() => {
		const q = $searchQuery || "";
		if (q !== currentQuery) {
			currentQuery = q;
			// 1. Instant local folder match
			updateCurrentFolderMatches(q);

			// 2. Debounced multi-node semantic search
			if (debounceTimer) clearTimeout(debounceTimer);
			if (q.trim()) {
				searching = true;
				debounceTimer = setTimeout(() => {
					runMultiNodeSearch(q);
				}, 3000);
			} else {
				folderMatches = [];
				semanticResults = [];
				searching = false;
				error = null;
			}
		}
	});

	// Enter in the search input runs the semantic search immediately
	function runNowHandler(e) {
		const v = e && e.detail && e.detail.value;
		if (debounceTimer) {
			clearTimeout(debounceTimer);
			debounceTimer = null;
		}
		if (v !== undefined) currentQuery = v;
		runMultiNodeSearch(v || currentQuery || "");
	}

	onMount(() => {
		if (typeof window !== "undefined") {
			window.addEventListener("omd-search-run-now", runNowHandler);
		}
	});

	onDestroy(() => {
		if (typeof window !== "undefined") {
			window.removeEventListener("omd-search-run-now", runNowHandler);
		}
		if (debounceTimer) {
			clearTimeout(debounceTimer);
			debounceTimer = null;
		}
	});
</script>

<div class="wx-search-view">
	<!-- Top Bar -->
	<header class="wx-search-header">
		<div class="wx-header-spacer"></div>

		<div class="wx-header-center">
			{#if currentQuery}
				<span class="wx-stat-query">"{currentQuery}"</span>
			{/if}
			{#if searching}
				<span class="wx-stat-item searching">
					<svg class="wx-stat-spinner" viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2.5">
						<circle cx="12" cy="12" r="10" stroke-dasharray="32" stroke-dashoffset="12" />
					</svg>
					<span>{$_("searching", {}, "Searching…")}</span>
				</span>
			{:else}
				<span class="wx-stat-item count">
					{totalCount} {totalCount === 1 ? $_("search_match", {}, "match") : $_("search_matches", {}, "matches")}
				</span>
				{#if searchDurationMs > 0}
					<span class="wx-stat-item time">{searchDurationMs}ms</span>
				{/if}
			{/if}
		</div>

		<div class="wx-header-right">
			<button
				type="button"
				class="wx-action-icon-btn close-btn"
				onclick={clearSearch}
				title={$_("close_search", {}, "Close search")}
				aria-label={$_("close_search", {}, "Close search")}
			>
				<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.2">
					<line x1="18" y1="6" x2="6" y2="18" />
					<line x1="6" y1="6" x2="18" y2="18" />
				</svg>
			</button>
		</div>
	</header>

	<!-- Content Area -->
	<main class="wx-search-body">
		<div class="wx-search-content-lane">
			<!-- Section 1: In Current Folder (if user is inside a folder and filterTab !== 'semantic') -->
			{#if folderMatches.length > 0 && filterTab !== "semantic"}
			<div class="wx-section-block">
				<div class="wx-section-header">
					<div class="wx-section-title">
						<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
							<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
						</svg>
						<span>{$_("files_in_current_folder", {}, "Files in current folder")}</span>
						<span class="wx-badge-section-count">{folderMatches.length}</span>
					</div>
					<span class="wx-section-path">{currentPath}</span>
				</div>

				<div class="wx-results-container {viewMode === 'grid' ? 'grid-view' : 'list-view'}">
					{#each folderMatches as item (item.id)}
						<!-- svelte-ignore a11y_click_events_have_key_events -->
						<!-- svelte-ignore a11y_no_static_element_interactions -->
						<div
							class="wx-result-card local-folder-card"
							onclick={() => openFile(item)}
						>
							<div class="wx-card-header">
								<div class="wx-file-icon-wrap">
									<img
										src={getFileIcon(item.title, item.contentType, item)}
										alt=""
										class="wx-file-icon"
										loading="lazy"
										onerror={(e) => {
											e.currentTarget.onerror = null;
											e.currentTarget.src = "https://cdn.svar.dev/icons/filemanager/vivid/big/file.svg";
										}}
									/>
								</div>

								<div class="wx-file-meta-col">
									<div class="wx-file-title-row">
										<h2 class="wx-file-title" title={item.title}>
											{@html highlightMatch(item.title, currentQuery)}
										</h2>
									</div>
									<div class="wx-file-path-row">
										<span class="wx-path-badge-folder">
											{$_("this_folder", {}, "This folder")}
										</span>
									</div>
								</div>

								<div class="wx-card-header-right">
									<span class="wx-folder-match-pill">{$_("exact_name_match", {}, "Exact name match")}</span>
								</div>
							</div>

							<div class="wx-card-footer">
								<div class="wx-meta-badges">
									{#if item.last_modified}
										<span class="wx-meta-chip">
											<svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
												<circle cx="12" cy="12" r="10" />
												<polyline points="12 6 12 12 16 14" />
											</svg>
											<span>{formatDate(item.last_modified)}</span>
										</span>
									{/if}
								</div>

								<div class="wx-card-actions">
									<button
										type="button"
										class="wx-action-btn primary"
										onclick={(e) => {
											e.stopPropagation();
											openFile(item);
										}}
									>
										<span>{item.isFolder ? $_("open_folder", {}, "Open Folder") : $_("open", {}, "Open")}</span>
									</button>
								</div>
							</div>
						</div>
					{/each}
				</div>
			</div>
		{/if}

		<!-- Section 2: Semantic Multi-Node Vector Search -->
		{#if filterTab !== "folder"}
			{#if folderMatches.length > 0 && semanticResults.length > 0}
				<div class="wx-section-divider">
					<div class="wx-section-header">
						<div class="wx-section-title">
							<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
								<path d="M12 2L15.09 8.26L22 9.27L17 14.14L18.18 21.02L12 17.77L5.82 21.02L7 14.14L2 9.27L8.91 8.26L12 2Z" />
							</svg>
							<span>{$_("semantic_search_results", {}, "Semantic vector search results")}</span>
							<span class="wx-badge-section-count">{semanticResults.length}</span>
						</div>
					</div>
				</div>
			{/if}

			{#if searching}
				<!-- Loading Skeleton -->
				<div class="wx-search-loading">
					<div class="wx-loading-spinner-wrap">
						<div class="wx-spinner"></div>
						<div class="wx-loading-text">
							<strong>{$_("searching_nodes", {}, "Searching ChromaDB across connected nodes…")}</strong>
							<span>{$_("querying_vector_index", {}, "Querying vector index for semantic context")}</span>
						</div>
					</div>

					<div class="wx-skeletons-list">
						{#each [1, 2, 3] as _}
							<div class="wx-skeleton-card">
								<div class="wx-skeleton-header">
									<div class="wx-sk-icon"></div>
									<div class="wx-sk-title-wrap">
										<div class="wx-sk-line sk-title"></div>
										<div class="wx-sk-line sk-path"></div>
									</div>
									<div class="wx-sk-badge"></div>
								</div>
								<div class="wx-sk-line sk-snippet1"></div>
								<div class="wx-sk-line sk-snippet2"></div>
							</div>
						{/each}
					</div>
				</div>
			{:else if error && semanticResults.length === 0 && folderMatches.length === 0}
				<!-- Error State -->
				<div class="wx-search-status-card error-card">
					<div class="wx-status-icon error-icon">
						<svg viewBox="0 0 24 24" width="32" height="32" fill="none" stroke="currentColor" stroke-width="2">
							<circle cx="12" cy="12" r="10" />
							<line x1="12" y1="8" x2="12" y2="12" />
							<line x1="12" y1="16" x2="12.01" y2="16" />
						</svg>
					</div>
					<h3>{$_("search_service_warning", {}, "Search service warning")}</h3>
					<p>{error}</p>
					<button class="wx-retry-btn" onclick={() => runMultiNodeSearch(currentQuery)}>
						{$_("try_again", {}, "Try Again")}
					</button>
				</div>
			{:else if totalCount === 0 && currentQuery}
				<!-- Empty State -->
				<div class="wx-search-empty">
					<div class="wx-empty-visual">
						<svg viewBox="0 0 24 24" width="56" height="56" fill="none" stroke="currentColor" stroke-width="1.5">
							<circle cx="11" cy="11" r="8" />
							<line x1="21" y1="21" x2="16.65" y2="16.65" />
							<line x1="8" y1="11" x2="14" y2="11" />
						</svg>
					</div>
					<h2 class="wx-empty-title">{$_("no_documents_found", {}, "No documents found")}</h2>
					<p class="wx-empty-desc">
						{$_("no_files_matched", { query: currentQuery }, `No files matched "${currentQuery}" in current folder or indexed nodes.`)}
					</p>
					<div class="wx-empty-hints">
						<span class="wx-hint-title">{$_("search_tips_title", {}, "Tips for search:")}</span>
						<ul>
							<li>{$_("search_tip_natural_language", {}, 'Use natural language questions or concept words (e.g. "invoice 2026")')}</li>
							<li>{$_("search_tip_indexed_nodes", {}, 'Check if the folders on the storage nodes are indexed')}</li>
							<li>{$_("search_tip_broader_terms", {}, 'Try broader or alternative terms')}</li>
						</ul>
					</div>
				</div>
			{:else if semanticResults.length > 0}
				<!-- Semantic Results Container -->
				<div class="wx-results-container {viewMode === 'grid' ? 'grid-view' : 'list-view'}">
					{#each semanticResults as item (item.id)}
						<!-- svelte-ignore a11y_click_events_have_key_events -->
						<!-- svelte-ignore a11y_no_static_element_interactions -->
						<div
							class="wx-result-card"
							onclick={() => openFile(item)}
						>
							<div class="wx-card-header">
								<div class="wx-file-icon-wrap">
									{#if item.image_preview}
										<img
											src={item.image_preview}
											alt={item.title}
											class="wx-thumb-preview"
											onerror={(e) => {
												e.currentTarget.onerror = null;
												e.currentTarget.src = getFileIcon(item.title, item.contentType, item);
												e.currentTarget.className = "wx-file-icon";
											}}
										/>
									{:else}
										<img
											src={getFileIcon(item.title, item.contentType, item)}
											alt=""
											class="wx-file-icon"
											loading="lazy"
											onerror={(e) => {
												e.currentTarget.onerror = null;
												e.currentTarget.src = "https://cdn.svar.dev/icons/filemanager/vivid/big/file.svg";
											}}
										/>
									{/if}
								</div>

								<div class="wx-file-meta-col">
									<div class="wx-file-title-row">
										<h2 class="wx-file-title" title={item.title}>
											{@html highlightMatch(item.title, currentQuery)}
										</h2>
									</div>

									<div class="wx-file-path-row">
										<span
											class="wx-path-link"
											title={$_("open_containing_folder", {}, "Open containing folder")}
											onclick={(e) => showInFolder(item, e)}
										>
											<svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2">
												<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
											</svg>
											<span>{item.itemPath}</span>
										</span>
									</div>
								</div>

								<div class="wx-card-header-right">
									{#if item.deviceName}
										<span class="wx-node-badge" title={$_("source_node", {}, "Source node")}>
											💻 {item.deviceName}
										</span>
									{/if}

									{#if item.relevance !== undefined}
										<div class="wx-relevance-pill {getScoreBadgeClass(item.relevance)}">
											<svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2.5">
												<polyline points="20 6 9 17 4 12" />
											</svg>
											<span>{item.relevance}%</span>
										</div>
									{/if}
								</div>
							</div>

							{#if item.snippet}
								<div class="wx-snippet-box">
									<p class="wx-snippet-text">
										{@html highlightMatch(item.snippet, currentQuery)}
									</p>
								</div>
							{/if}

							<div class="wx-card-footer">
								<div class="wx-meta-badges">
									{#if item.last_modified}
										<span class="wx-meta-chip">
											<svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2">
												<circle cx="12" cy="12" r="10" />
												<polyline points="12 6 12 12 16 14" />
											</svg>
											<span>{formatDate(item.last_modified)}</span>
										</span>
									{/if}
								</div>

								<div class="wx-card-actions">
									<button
										type="button"
										class="wx-action-btn primary"
										onclick={(e) => {
											e.stopPropagation();
											openFile(item);
										}}
										title={$_("open_file", {}, "Open file")}
									>
										<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
											<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z" />
											<circle cx="12" cy="12" r="3" />
										</svg>
										<span>{$_("open", {}, "Open")}</span>
									</button>

									<button
										type="button"
										class="wx-action-btn secondary"
										onclick={(e) => showInFolder(item, e)}
										title={$_("show_in_folder", {}, "Show in folder")}
									>
										<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
											<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
										</svg>
										<span>{$_("folder", {}, "Folder")}</span>
									</button>

									<button
										type="button"
										class="wx-action-btn-icon"
										onclick={(e) => copyPath(item, e)}
										title={copiedId === item.itemPath ? $_("copied", {}, "Copied!") : $_("copy_path", {}, "Copy path")}
										aria-label={$_("copy_path", {}, "Copy path")}
									>
										{#if copiedId === item.itemPath}
											<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="#10b981" stroke-width="2.5">
												<polyline points="20 6 9 17 4 12" />
											</svg>
										{:else}
											<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
												<rect x="9" y="9" width="13" height="13" rx="2" ry="2" />
												<path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1" />
											</svg>
										{/if}
									</button>

									<button
										type="button"
										class="wx-action-btn-icon"
										onclick={(e) => downloadFile(item, e)}
										title={$_("download_file", {}, "Download file")}
										aria-label={$_("download", {}, "Download")}
									>
										<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
											<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
											<polyline points="7 10 12 15 17 10" />
											<line x1="12" y1="15" x2="12" y2="3" />
										</svg>
									</button>
								</div>
							</div>
						</div>
					{/each}
				</div>
			{/if}
		{/if}
		</div>
	</main>
</div>

<style>
	.wx-search-view {
		display: flex;
		flex-direction: column;
		height: 100%;
		max-height: 100%;
		width: 100%;
		background-color: transparent;
		color: var(--wx-color-font, #e0e6ed);
		overflow: hidden;
		font-family: inherit;
	}

	/* Top Bar (Breadcrumb style) */
	.wx-search-header {
		flex: 0 0 48px;
		height: 48px;
		display: flex;
		justify-content: space-between;
		align-items: center;
		padding: 0 16px;
		background-color: transparent !important;
		border: none !important;
		box-shadow: none !important;
		box-sizing: border-box;
		z-index: 5;
		gap: 8px;
	}

	.wx-header-spacer,
	.wx-header-right {
		display: flex;
		align-items: center;
		flex-shrink: 0;
		width: 32px;
		height: 32px;
	}

	.wx-header-right {
		justify-content: flex-end;
	}

	.wx-action-icon-btn {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 28px;
		height: 28px;
		border-radius: 6px;
		background: transparent;
		border: none;
		color: var(--wx-color-font-alt, #94a3b8);
		cursor: pointer;
		transition: all 0.15s ease;
	}

	.wx-action-icon-btn:hover {
		background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.08));
		color: var(--wx-color-primary-hover, #37a9ef);
	}

	.wx-action-icon-btn.close-btn:hover {
		background-color: rgba(239, 68, 68, 0.15);
		color: #f87171;
	}

	.wx-header-center {
		display: flex;
		align-items: center;
		justify-content: center;
		gap: 12px;
		flex: 1;
		min-width: 0;
		text-align: center;
	}

	.wx-stat-query {
		font-size: 14px;
		font-weight: 600;
		color: var(--wx-color-primary, #38bdf8);
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
		max-width: 320px;
	}

	.wx-stat-item {
		display: inline-flex;
		align-items: center;
		gap: 4px;
		font-size: 13px;
		font-weight: 500;
		color: var(--wx-color-font, #e2e8f0);
	}

	.wx-stat-item.time {
		font-size: 12px;
		color: var(--wx-color-font-alt, #94a3b8);
	}

	.wx-stat-spinner {
		animation: wx-spin 0.8s linear infinite;
	}

	@keyframes wx-spin {
		100% {
			transform: rotate(360deg);
		}
	}

	/* Content Area */
	.wx-search-body {
		flex: 1;
		overflow-y: auto;
		overflow-x: hidden;
		padding: 0;
		width: 100%;
		box-sizing: border-box;
	}

	/* Middle content lane (like in Chat) */
	.wx-search-content-lane {
		max-width: 75%;
		width: 100%;
		margin: 0 auto;
		box-sizing: border-box;
		padding: 16px 20px 48px;
		display: flex;
		flex-direction: column;
	}

	@media (max-width: 900px) {
		.wx-search-content-lane {
			max-width: 100% !important;
			padding: 8px 8px 32px !important;
		}
	}

	/* Section Blocks */
	.wx-section-block {
		width: 100%;
		box-sizing: border-box;
		margin: 0 0 16px 0;
		padding: 0;
	}

	.wx-section-divider {
		width: 100%;
		box-sizing: border-box;
		margin: 8px 0;
		border-top: 1px solid var(--wx-border, rgba(255, 255, 255, 0.08));
	}

	.wx-section-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		margin-bottom: 10px;
		gap: 8px;
	}

	.wx-section-title {
		display: flex;
		align-items: center;
		gap: 6px;
		font-size: 13px;
		font-weight: 700;
		text-transform: uppercase;
		letter-spacing: 0.5px;
		color: var(--wx-color-font, #cbd5e1);
	}

	.wx-badge-section-count {
		font-size: 11px;
		padding: 1px 6px;
		border-radius: 10px;
		background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.08));
		color: var(--wx-color-primary, #38bdf8);
	}

	.wx-section-path {
		font-size: 11px;
		color: var(--wx-color-font-alt, #64748b);
	}

	/* Results Container */
	.wx-results-container.list-view {
		display: flex;
		flex-direction: column;
		gap: 8px;
		width: 100%;
		box-sizing: border-box;
		padding: 4px 0 16px 0;
	}

	.wx-results-container.grid-view {
		display: grid;
		grid-template-columns: repeat(auto-fill, minmax(260px, 1fr));
		gap: 12px;
		width: 100%;
		box-sizing: border-box;
		padding: 4px 0 16px 0;
	}

	/* Result Card */
	.wx-result-card {
		display: flex;
		flex-direction: column;
		background-color: var(--wx-background, #202a2f);
		border: 1px solid var(--border-color, var(--wx-border, rgba(255, 255, 255, 0.08)));
		border-radius: 12px;
		padding: 12px 14px;
		cursor: pointer;
		transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
		box-shadow: 0 2px 8px rgba(0, 0, 0, 0.15);
		box-sizing: border-box;
		width: 100%;
		max-width: 100%;
		min-width: 0;
		overflow: hidden;
	}

	.wx-result-card:hover {
		transform: translateY(-2px);
		border-color: var(--wx-color-primary-hover, #37a9ef);
		box-shadow: 0 6px 18px rgba(0, 0, 0, 0.18);
	}

	.wx-result-card.local-folder-card {
		border-left: 3px solid var(--wx-color-primary, #2996cc);
	}

	/* Card Header */
	.wx-card-header {
		display: flex;
		align-items: flex-start;
		gap: 12px;
		width: 100%;
		min-width: 0;
	}

	.wx-file-icon-wrap {
		width: 40px;
		height: 40px;
		flex-shrink: 0;
		display: flex;
		align-items: center;
		justify-content: center;
		border-radius: 8px;
		overflow: hidden;
		background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.05));
	}

	.wx-file-icon {
		width: 32px;
		height: 32px;
		object-fit: contain;
	}

	.wx-thumb-preview {
		width: 100%;
		height: 100%;
		object-fit: cover;
	}

	.wx-file-meta-col {
		flex: 1;
		min-width: 0;
	}

	.wx-file-title-row {
		display: flex;
		align-items: baseline;
		gap: 8px;
	}

	.wx-file-title {
		font-size: 15px;
		font-weight: 600;
		margin: 0;
		color: var(--wx-color-font, #f1f5f9);
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
	}

	.wx-file-title:hover {
		color: var(--wx-color-primary, #38bdf8);
	}

	.wx-file-path-row {
		margin-top: 3px;
	}

	.wx-path-badge-folder {
		display: inline-block;
		font-size: 11px;
		font-weight: 600;
		color: #10b981;
		background-color: rgba(16, 185, 129, 0.12);
		padding: 1px 6px;
		border-radius: 4px;
	}

	.wx-path-link {
		display: inline-flex;
		align-items: center;
		gap: 5px;
		font-size: 12px;
		color: var(--wx-color-font-alt, #94a3b8);
		border-radius: 4px;
		padding: 1px 4px;
		margin-left: -4px;
		transition: all 0.15s ease;
		max-width: 100%;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.wx-path-link:hover {
		color: var(--wx-color-primary, #38bdf8);
		background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.08));
	}

	.wx-card-header-right {
		display: flex;
		align-items: center;
		gap: 8px;
		flex-shrink: 0;
	}

	.wx-node-badge {
		font-size: 11px;
		font-weight: 500;
		padding: 2px 7px;
		border-radius: 8px;
		background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.08));
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.1));
		color: var(--wx-color-font, #e2e8f0);
	}

	.wx-folder-match-pill {
		font-size: 11px;
		font-weight: 600;
		color: #10b981;
		padding: 2px 8px;
		border-radius: 10px;
		background-color: rgba(16, 185, 129, 0.12);
		border: 1px solid rgba(16, 185, 129, 0.25);
	}

	/* Relevance Pill */
	.wx-relevance-pill {
		display: inline-flex;
		align-items: center;
		gap: 4px;
		padding: 3px 8px;
		border-radius: 12px;
		font-size: 12px;
		font-weight: 700;
		letter-spacing: 0.2px;
		border: 1px solid transparent;
	}

	.wx-relevance-pill.score-high {
		background-color: rgba(16, 185, 129, 0.15);
		color: #10b981;
		border-color: rgba(16, 185, 129, 0.3);
	}

	.wx-relevance-pill.score-medium {
		background-color: rgba(14, 165, 233, 0.15);
		color: #0ea5e9;
		border-color: rgba(14, 165, 233, 0.3);
	}

	.wx-relevance-pill.score-low {
		background-color: rgba(245, 158, 11, 0.15);
		color: #f59e0b;
		border-color: rgba(245, 158, 11, 0.3);
	}

	/* Semantic Snippet */
	.wx-snippet-box {
		margin-top: 10px;
		padding: 8px 12px;
		border-radius: 6px;
		background-color: var(--wx-background-alt, rgba(0, 0, 0, 0.2));
		border-left: 3px solid var(--wx-color-primary, #2996cc);
		font-size: 13px;
		line-height: 1.5;
		box-sizing: border-box;
		width: 100%;
		max-width: 100%;
		min-width: 0;
		overflow: hidden;
	}

	.wx-snippet-text {
		margin: 0;
		color: var(--wx-color-font-alt, #cbd5e1);
		display: -webkit-box;
		-webkit-line-clamp: 3;
		-webkit-box-orient: vertical;
		overflow: hidden;
		word-break: break-word;
		overflow-wrap: anywhere;
	}

	/* Подсветка найденной фразы: читаема и на светлой, и на тёмной теме.
	   Светлая тема — плотный жёлтый с тёмным текстом; тёмная — полупрозрачный
	   жёлтый со светлым текстом (наследует фон карточки через html.dark-theme). */
	:global(.wx-search-hl) {
		background-color: #ffd54a;
		color: #1f2937;
		padding: 1px 3px;
		border-radius: 3px;
		font-weight: 600;
	}

	:global(html.dark-theme .wx-search-hl) {
		background-color: rgba(253, 224, 71, 0.28);
		color: #fef3c7;
	}

	/* Footer & Action Buttons */
	.wx-card-footer {
		display: flex;
		justify-content: space-between;
		align-items: center;
		margin-top: 12px;
		padding-top: 10px;
		border-top: 1px solid var(--wx-border, rgba(255, 255, 255, 0.06));
		gap: 8px;
		flex-wrap: wrap;
	}

	.wx-meta-badges {
		display: flex;
		align-items: center;
		gap: 8px;
	}

	.wx-meta-chip {
		display: inline-flex;
		align-items: center;
		gap: 4px;
		font-size: 11px;
		color: var(--wx-color-font-alt, #94a3b8);
	}

	.wx-card-actions {
		display: flex;
		align-items: center;
		gap: 6px;
		margin-left: auto;
	}

	.wx-action-btn {
		display: inline-flex;
		align-items: center;
		gap: 5px;
		height: 28px;
		padding: 0 10px;
		border-radius: 6px;
		font-size: 12px;
		font-weight: 500;
		cursor: pointer;
		border: 1px solid transparent;
		transition: all 0.15s ease;
	}

	.wx-action-btn.primary {
		background-color: var(--wx-color-primary, #2996cc);
		color: #ffffff;
	}

	.wx-action-btn.primary:hover {
		background-color: var(--wx-color-primary-hover, #37a9ef);
		box-shadow: 0 2px 8px rgba(41, 150, 204, 0.35);
	}

	.wx-action-btn.secondary {
		background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.08));
		border-color: var(--wx-border, rgba(255, 255, 255, 0.1));
		color: var(--wx-color-font, #e2e8f0);
	}

	.wx-action-btn.secondary:hover {
		background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.15));
		color: #ffffff;
	}

	.wx-action-btn-icon {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 28px;
		height: 28px;
		border-radius: 6px;
		background: transparent;
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.1));
		color: var(--wx-color-font-alt, #94a3b8);
		cursor: pointer;
		transition: all 0.15s ease;
	}

	.wx-action-btn-icon:hover {
		background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.1));
		color: var(--wx-color-font, #ffffff);
	}

	/* Loading Skeleton */
	.wx-search-loading {
		width: 100%;
		box-sizing: border-box;
		margin: 0;
		display: flex;
		flex-direction: column;
		gap: 16px;
	}

	.wx-loading-spinner-wrap {
		display: flex;
		align-items: center;
		gap: 12px;
		padding: 12px 16px;
		border-radius: 8px;
		background-color: var(--wx-background, #202a2f);
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.08));
	}

	.wx-spinner {
		width: 20px;
		height: 20px;
		border: 2px solid var(--wx-border, rgba(255, 255, 255, 0.15));
		border-top-color: var(--wx-color-primary, #2996cc);
		border-radius: 50%;
		animation: spin 0.8s linear infinite;
	}

	.wx-loading-text {
		display: flex;
		flex-direction: column;
		font-size: 13px;
	}

	.wx-loading-text strong {
		color: var(--wx-color-font, #f1f5f9);
	}

	.wx-loading-text span {
		font-size: 12px;
		color: var(--wx-color-font-alt, #94a3b8);
	}

	.wx-skeletons-list {
		display: flex;
		flex-direction: column;
		gap: 12px;
	}

	.wx-skeleton-card {
		background-color: var(--wx-background, #202a2f);
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.05));
		border-radius: 10px;
		padding: 14px 16px;
		display: flex;
		flex-direction: column;
		gap: 10px;
	}

	.wx-skeleton-header {
		display: flex;
		align-items: center;
		gap: 12px;
	}

	.wx-sk-icon {
		width: 36px;
		height: 36px;
		border-radius: 6px;
		background: linear-gradient(90deg, rgba(255, 255, 255, 0.05) 25%, rgba(255, 255, 255, 0.1) 50%, rgba(255, 255, 255, 0.05) 75%);
		background-size: 200% 100%;
		animation: shimmer 1.5s infinite;
	}

	.wx-sk-title-wrap {
		flex: 1;
		display: flex;
		flex-direction: column;
		gap: 6px;
	}

	.wx-sk-line {
		border-radius: 4px;
		background: linear-gradient(90deg, rgba(255, 255, 255, 0.05) 25%, rgba(255, 255, 255, 0.1) 50%, rgba(255, 255, 255, 0.05) 75%);
		background-size: 200% 100%;
		animation: shimmer 1.5s infinite;
	}

	.sk-title {
		height: 14px;
		width: 40%;
	}

	.sk-path {
		height: 10px;
		width: 60%;
	}

	.sk-snippet1 {
		height: 12px;
		width: 90%;
		margin-top: 4px;
	}

	.sk-snippet2 {
		height: 12px;
		width: 75%;
	}

	.wx-sk-badge {
		width: 50px;
		height: 22px;
		border-radius: 11px;
		background: linear-gradient(90deg, rgba(255, 255, 255, 0.05) 25%, rgba(255, 255, 255, 0.1) 50%, rgba(255, 255, 255, 0.05) 75%);
		background-size: 200% 100%;
		animation: shimmer 1.5s infinite;
	}

	/* Empty State */
	.wx-search-empty {
		display: flex;
		flex-direction: column;
		align-items: center;
		justify-content: center;
		padding: 48px 16px;
		text-align: center;
		max-width: 540px;
		margin: 0 auto;
	}

	.wx-empty-visual {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 96px;
		height: 96px;
		border-radius: 50%;
		background: radial-gradient(circle, rgba(41, 150, 204, 0.15) 0%, transparent 70%);
		color: var(--wx-color-font-alt, #64748b);
		margin-bottom: 16px;
	}

	.wx-empty-title {
		font-size: 20px;
		font-weight: 700;
		margin: 0 0 8px 0;
		color: var(--wx-color-font, #f1f5f9);
	}

	.wx-empty-desc {
		font-size: 14px;
		color: var(--wx-color-font-alt, #94a3b8);
		margin: 0 0 24px 0;
		line-height: 1.5;
	}

	.wx-empty-hints {
		background-color: var(--wx-background, #202a2f);
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.08));
		border-radius: 8px;
		padding: 14px 18px;
		text-align: left;
		font-size: 13px;
		color: var(--wx-color-font-alt, #cbd5e1);
		line-height: 1.6;
	}

	.wx-hint-title {
		font-weight: 600;
		color: var(--wx-color-font, #f1f5f9);
		display: block;
		margin-bottom: 6px;
	}

	.wx-empty-hints ul {
		margin: 0;
		padding-left: 18px;
	}

	/* Error State */
	.wx-search-status-card {
		display: flex;
		flex-direction: column;
		align-items: center;
		justify-content: center;
		padding: 32px 16px;
		text-align: center;
		max-width: 460px;
		margin: 40px auto;
		background-color: var(--wx-background, #202a2f);
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.08));
		border-radius: 12px;
	}

	.wx-status-icon.error-icon {
		color: #ef4444;
		margin-bottom: 12px;
	}

	.wx-retry-btn {
		margin-top: 14px;
		height: 32px;
		padding: 0 16px;
		border-radius: 6px;
		background-color: var(--wx-color-primary, #2996cc);
		color: #ffffff;
		border: none;
		font-size: 13px;
		font-weight: 500;
		cursor: pointer;
	}

	@keyframes spin {
		to {
			transform: rotate(360deg);
		}
	}

	@keyframes shimmer {
		0% {
			background-position: 200% 0;
		}
		100% {
			background-position: -200% 0;
		}
	}

	/* Responsive */
	@media (max-width: 900px) {
		.wx-search-content-lane {
			max-width: 100% !important;
			width: 100% !important;
			padding: 8px 8px 32px !important;
		}

		.wx-results-container.grid-view {
			grid-template-columns: 1fr !important;
			width: 100% !important;
		}

		.wx-results-container.list-view {
			width: 100% !important;
		}

		.wx-result-card {
			width: 100% !important;
			max-width: 100% !important;
			box-sizing: border-box !important;
		}
	}

	@media (max-width: 768px) {
		.wx-search-header {
			padding: 0 10px;
			flex-wrap: wrap;
			height: auto;
			min-height: 48px;
			padding-top: 4px;
			padding-bottom: 4px;
		}

		.wx-card-header {
			flex-wrap: wrap;
			gap: 8px;
		}

		.wx-card-header-right {
			width: 100%;
			justify-content: flex-start;
			margin-top: 4px;
			flex-wrap: wrap;
			gap: 6px;
		}

		.wx-file-title {
			font-size: 14px;
		}

		.wx-card-footer {
			flex-direction: column;
			align-items: flex-start;
			gap: 8px;
		}

		.wx-meta-badges {
			width: 100%;
			flex-wrap: wrap;
			gap: 6px;
		}

		.wx-card-actions {
			width: 100%;
			justify-content: flex-start;
			flex-wrap: wrap;
			gap: 6px;
			margin-left: 0;
		}
	}
</style>
