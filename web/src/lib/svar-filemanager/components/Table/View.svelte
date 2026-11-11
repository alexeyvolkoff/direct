<script>
	import { getContext } from "svelte";
	import { formatSize } from "../../store/index.js";
	import { deviceRegistry } from "../../../device-registry.js";
	import { isFolderFetchingStore } from "../../../folder-loading.js";
	import { delegateClick, dateToString, setID } from "@svar-ui/lib-dom";
	import { Grid } from "@svar-ui/svelte-grid";
	import { setFolderSort } from "../../store/sort-prefs.js";

	import Breadcrumbs from "../Breadcrumbs.svelte";
	import NameCell from "./NameCell.svelte";
	import UploadDropArea from "../UploadDropArea.svelte";
	import EmptyFolderState from "../EmptyFolderState.svelte";

	let { panel, active = false, onclick, oncontextmenu, isPanels = false } = $props();

	const api = getContext("filemanager-store");
	const menuOptions = getContext("filemanager-menu-options");

	const locale = getContext("wx-i18n");
	const _ = locale.getGroup("filemanager");
	const format = dateToString("%d %M %Y", locale.getRaw().calendar);

	let { panels, activePanel } = api.getReactiveState();

	const selection = $derived($panels[panel].selected);
	const path = $derived($panels[panel].path);
	const crumbs = $derived($panels[panel]._crumbs);
	const sorts = $derived($panels[panel]._sorts);
	const selectNavigation = $derived($panels[panel]._selectNavigation);
	const files = $derived($panels[panel]._files);

	// Empty folder inside a device gets a centered CTA (same as cards view).
	const devicePrefix = $derived(path && path !== "/" ? path.split("/")[1] || "" : "");
	const currentDevice = $derived(devicePrefix ? deviceRegistry.getByPrefix(devicePrefix) : null);
	const isLocalWebDevice = $derived(currentDevice?.local === true);
	const isDeviceRoot = $derived(path !== "/" && !path.slice(1).includes("/") && !path.startsWith("/shared-"));
	// Service sections (Shared to me / Shared by me) are virtual folders fed
	// through provide-data, not device folders tracked in _folderItems.
	const isServiceSection = $derived(
		path === "/shared-to-me" ||
		path === "/shared-by-me" ||
		(path && (path.startsWith("/shared-to-me/") || path.startsWith("/shared-by-me/")))
	);

	const isDataLoaded = $derived.by(() => {
		if ($isFolderFetchingStore) return false;
		if (currentDevice && !currentDevice.local) {
			if (!currentDevice.drive || !currentDevice.drive.isConnected || currentDevice.drive.dc?.readyState !== "open") {
				return false;
			}
		}
		if (isDeviceRoot && currentDevice) {
			const prefix = currentDevice.prefix || ("/" + devicePrefix);
			if (typeof window !== "undefined" && window._deviceItems && !(prefix in window._deviceItems)) {
				return false;
			}
		} else if (path && path !== "/" && !isServiceSection) {
			if (typeof window !== "undefined" && window._folderItems && !(path in window._folderItems)) {
				return false;
			}
		}
		const node = api.getFile(path);
		if (node && node.lazy) return false;
		return true;
	});

	const isEmptyFolder = $derived(
		(isServiceSection || !!currentDevice) && isDataLoaded && (!files || files.length === 0)
	);

	const columns = [
		{
			id: "name",
			header: _("Name"),
			flexgrow: 3,
			sort: true,
			resize: true,
			cell: NameCell,
		},
		{
			id: "size",
			header: _("Size"),
			width: 100,
			sort: true,
			resize: true,
			template: (v, row) => {
				if (!row || row.type === "folder" || row.isDevice || row.itemType === "device" || row.isDir || row.id === "/shared-to-me" || row.id === "/shared-by-me" || row.itemType === "shared-to-me" || row.itemType === "shared-by-me") return "";
				return typeof v === "number" && v > 0 ? formatSize(v) : "";
			},
		},
		{
			id: "date",
			header: _("Date"),
			width: 120,
			sort: true,
			resize: true,
			template: v => (v ? format(v) : ""),
		},
	];

	let sortClick = null;
	let resizeClick = null;

	const tableSelection = $derived(
		selectNavigation ? ["/wx-filemanager-parent-link"] : [...selection]
	);

	function click(id, e) {
		//[FIXME] grip is wx-table classname which may change
		if (
			e &&
			typeof e.target?.className === "string" &&
			e.target.className.indexOf("grip") !== -1
		)
			return;
		if (id === "/wx-filemanager-parent-link") {
			return backToParent();
		}
		if (!id || id === "body") {
			if (isPanels) {
				try { if ($activePanel !== panel) api.exec("set-active-panel", { panel }); } catch(_){}
			}
			return;
		}

		if (sortClick || resizeClick) {
			sortClick = resizeClick = null;
			return;
		}

		const ctrl = e && (e.ctrlKey || e.metaKey);
		const shift = e && e.shiftKey;
		const target = e && e.target;
		const isCheckboxOrMore =
			target &&
			(target.closest(".wx-checkbox") ||
				target.closest(".wx-more") ||
				target.closest(".wx-actions"));

		// In dual-panel mode click = select (classic Norton/MC), dblclick = open
		if (isPanels) {
			try { if ($activePanel !== panel) api.exec("set-active-panel", { panel }); } catch(_){}
			api.exec("select-file", {
				id,
				toggle: ctrl || isCheckboxOrMore,
				range: shift,
				panel: panel,
			});
			if (e) { e.preventDefault(); e.stopPropagation(); }
			return;
		}

		if (ctrl || shift || isCheckboxOrMore) {
			api.exec("select-file", {
				id,
				toggle: ctrl || isCheckboxOrMore,
				range: shift,
				panel: panel,
			});
			return;
		}

		const item = files ? files.find(a => a.id === id) : null;
		if (item) {
			if (item.type === "folder" || item.isDir || item.isDevice) {
				api.exec("set-path", {
					id: item.id,
					panel: panel,
				});
			} else {
				api.exec("open-file", {
					id: item.id,
				});
			}
		}
	}

	function backToParent() {
		if (crumbs && crumbs.length > 1) {
			api.exec("set-path", {
				id: crumbs[crumbs.length - 2].id,
				panel: panel,
				selected: [crumbs[crumbs.length - 1].id],
			});
		} else if (path && path !== "/") {
			const parentPath = path.substring(0, path.lastIndexOf("/")) || "/";
			api.exec("set-path", {
				id: parentPath,
				panel: panel,
			});
		}
	}

	function dblclick(id) {
		if (!id || id === "body" || id === "/wx-filemanager-parent-link") return;
		if (!isPanels) return;
		try { if ($activePanel !== panel) api.exec("set-active-panel", { panel }); } catch(_){}
		const item = files ? files.find(a => a.id === id) : null;
		if (!item) return;
		if (item.type === "folder" || item.isDir || item.isDevice) {
			api.exec("set-path", { id: item.id, panel: panel });
		} else {
			api.exec("open-file", { id: item.id });
		}
	}

	function handleSort(e) {
		const col = e.key;
		const prevSort = sorts[path];
		let order = !prevSort ? "desc" : "asc";

		if (prevSort && prevSort.key === col) {
			order = prevSort.order === "asc" ? "desc" : "asc";
		}

		api.exec("sort-files", {
			key: col,
			order,
			panel,
			path,
		});
		setFolderSort(path, col, order);
	}

	function initTable(api) {
		api.intercept("sort-rows", e => {
			sortClick = true;
			handleSort(e);
			return false;
		});

		api.intercept("select-row", () => false);

		api.on("resize-column", () => (resizeClick = true));

		api.intercept("hotkey", () => false);
	}

	let renderedFiles = $derived(files || []);

	let sortMarks = $derived.by(() => {
		if (renderedFiles && sorts[path]) {
			const { key, order } = sorts[path];
			return { [key]: { order } };
		}
	});
</script>

<!-- svelte-ignore a11y_click_events_have_key_events -->
<!-- svelte-ignore a11y_no_static_element_interactions -->
<div {onclick} {oncontextmenu} class="wx-wrapper">
	<Breadcrumbs {panel} />
	<div
		data-id={setID("body")}
		class="wx-list"
		class:wx-active={active}
		use:delegateClick={{ click, dblclick }}
	>
	<UploadDropArea>
		{#if renderedFiles.length}
			<Grid
				init={initTable}
				data={renderedFiles}
				{columns}
				selectedRows={tableSelection}
				columnStyle={() => "wx-each-cell"}
				sizes={{ rowHeight: 42, headerHeight: 42 }}
				{sortMarks}
			/>
		{:else if !isDataLoaded}
			<div class="wx-empty-folder-loading">
				<div class="wx-spinner"></div>
			</div>
		{:else if isEmptyFolder}
			<EmptyFolderState
				{path}
				{currentDevice}
				{isDeviceRoot}
				{isLocalWebDevice}
			/>
		{:else}
			<Grid
				init={initTable}
				data={renderedFiles}
				{columns}
				selectedRows={tableSelection}
				columnStyle={() => "wx-each-cell"}
				sizes={{ rowHeight: 42, headerHeight: 42 }}
				{sortMarks}
			/>
		{/if}
	</UploadDropArea>
	</div>
</div>

<style>
	.wx-wrapper {
		display: flex;
		flex-direction: column;
		height: 100%;
		max-height: 100%;
		max-width: 100%;
		box-shadow: var(--wx-fm-box-shadow);
		border-radius: 6px;
	}

	.wx-list {
		height: calc(100% - 50px);
	}
	.wx-list > :global(.wx-upload-area .wx-grid) {
		--wx-table-cell-border: var(--wx-fm-grid-border);
		--wx-table-header-border: var(--wx-fm-grid-border);
		--wx-table-header-cell-border: var(--wx-fm-grid-border);
	}
	.wx-list > :global(.wx-upload-area .wx-body .wx-each-cell) {
		border-right: none;
	}
	.wx-list
		> :global(.wx-upload-area .wx-header .wx-cell:first-child .wx-text) {
		padding: 0 6px;
	}
	.wx-list > :global(.wx-upload-area .wx-table) {
		border-radius: 0 0 6px 6px;
	}

	.wx-list.wx-active > :global(.wx-upload-area .wx-grid),
	.wx-list > :global(.wx-upload-area.wx-active .wx-grid) {
		--wx-table-cell-border: 1px solid var(--wx-color-primary);
	}
	.wx-list.wx-active > :global(.wx-upload-area .wx-row) {
		--wx-table-cell-border: var(--wx-fm-grid-border);
	}

	.wx-list > :global(.wx-upload-area.wx-active .wx-row) {
		background: var(--wx-color-primary-selected);
		--wx-table-cell-border: 1px solid var(--wx-color-primary-selected);
	}
	/*switch off focus due to filamanager own navigation system*/
	.wx-list > :global(.wx-upload-area .wx-grid .wx-cell) {
		outline: none;
	}

	/* temp hack to align toolbar and table body (with 1.75px full match) */
	.wx-list > :global(.wx-upload-area) {
		border-right: 1px solid transparent;
	}
	.wx-empty-folder-loading {
		display: flex;
		align-items: center;
		justify-content: center;
		height: 100%;
		width: 100%;
	}
	.wx-spinner {
		width: 32px;
		height: 32px;
		border: 3px solid var(--wx-border-color, rgba(127, 127, 127, 0.2));
		border-top-color: var(--wx-color-primary, #7E22CE);
		border-radius: 50%;
		animation: wx-spin 0.8s linear infinite;
	}
	@keyframes wx-spin {
		to { transform: rotate(360deg); }
	}
</style>
