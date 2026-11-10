<script>
	import { getContext } from "svelte";
	import { delegateClick, setID } from "@svar-ui/lib-dom";
	import { deviceRegistry } from "../../../device-registry.js";
	import { isFolderFetchingStore } from "../../../folder-loading.js";
	import Item from "./Item.svelte";
	import EmptyFolderState from "../EmptyFolderState.svelte";

	const api = getContext("filemanager-store");
	const _ = getContext("filemanager-locale") || getContext("wx-i18n").getGroup("filemanager");
	const menuOptions = getContext("filemanager-menu-options");

	let { panels, activePanel, mode } = api.getReactiveState();

	const files = $derived($panels[$activePanel]._files);
	const selected = $derived($panels[$activePanel].selected);
	const path = $derived($panels[$activePanel].path);
	const crumbs = $derived($panels[$activePanel]._crumbs);
	const selectNavigation = $derived($panels[$activePanel]._selectNavigation);

	// First path segment is the device prefix ("/<linkId>"); "" at the root
	// that lists devices. An empty folder inside a device gets a centered CTA.
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
		$mode !== "search" && (isServiceSection || !!currentDevice) && isDataLoaded && (!files || files.length === 0)
	);

	function click(id, e) {
		if (id === "/wx-filemanager-parent-link") {
			return backToParent();
		}
		if (!id || id === "body") return;

		const target = e && e.target;
		const actionClick =
			(
				target &&
				(target.closest(".wx-more") !== null ||
					(typeof target.className === "string" &&
						(target.className.indexOf("wx-more") !== -1 ||
							target.className.indexOf("wxi-dots-v") !== -1)))
			) ||
			(e && (e.ctrlKey || e.metaKey || e.shiftKey));

		if (actionClick) {
			const ctrl = e && (e.ctrlKey || e.metaKey);
			const shift = e && e.shiftKey;
			api.exec("select-file", {
				id,
				toggle: ctrl,
				range: shift,
				panel: $activePanel,
			});
			return;
		}

		if ($mode === "search") {
			api.exec("filter-files", {
				text: "",
			});
		}

		const item = files ? files.find(a => a.id === id) : null;
		if (item) {
			if (item.type == "folder" || item.isDir) {
				api.exec("set-path", {
					id: item.id,
					panel: $activePanel,
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
				panel: $activePanel,
				selected: [crumbs[crumbs.length - 1].id],
			});
		} else if (path && path !== "/") {
			const parentPath = path.substring(0, path.lastIndexOf("/")) || "/";
			api.exec("set-path", {
				id: parentPath,
				panel: $activePanel,
			});
		}
	}
	function dblclick(id) {
	}

	function applySelection(id, ev) {
		if (
			!selected?.length ||
			!selected.filter(i => i?.id === id).length > 0
		) {
			click(id, ev);
		}
	}

	let renderedFiles = $derived(files || []);
</script>

{#if $mode == "search" && !renderedFiles.length}
	<div class="wx-not-found">
		<div class="wx-not-found-text">{_("Looks like nothing is here")}</div>
	</div>
{:else if renderedFiles.length}
	<!-- svelte-ignore a11y_no_noninteractive_tabindex -->
	<div
		tabindex="0"
		class="wx-cards"
		data-id={setID("body")}
		use:delegateClick={{ click, dblclick, context: applySelection }}
	>
		{#each renderedFiles as child (child.id)}
			<Item item={child} />
		{/each}
	</div>
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
	<!-- svelte-ignore a11y_no_noninteractive_tabindex -->
	<div
		tabindex="0"
		class="wx-cards"
		data-id={setID("body")}
		use:delegateClick={{ click, dblclick, context: applySelection }}
	></div>
{/if}

<style>
	.wx-cards {
		flex-grow: 1;
		flex-wrap: wrap;
		height: 100%;
		border-top: none;
		padding: 4px 20px 10px;
		display: flex;
		flex-direction: row;
		align-items: flex-start;
		justify-content: flex-start;
		overflow-y: auto;
		align-content: flex-start;
		outline: none;
		/* Uniform cursor on the surface; items set their own pointer */
		cursor: default;
	}
	@media (max-width: 600px) {
		.wx-cards {
			padding: 4px 10px 10px;
		}
	}
	.wx-not-found-text {
		text-align: center;
		color: var(--wx-color-font-alt);
	}
	.wx-not-found {
		display: flex;
		flex-direction: column;
		justify-content: center;
		align-items: center;
		gap: 5px;
		height: 100%;
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
		border-top-color: var(--wx-color-primary, #184562);
		border-radius: 50%;
		animation: wx-spin 0.8s linear infinite;
	}
	@keyframes wx-spin {
		to { transform: rotate(360deg); }
	}
</style>
