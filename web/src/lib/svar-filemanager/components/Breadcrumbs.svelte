<script>
	import { getContext } from "svelte";
	import { delegateClick, setID } from "@svar-ui/lib-dom";
	import { getSelectionOnNavigation } from "../store/index.js";
	import { DropDownMenu } from "@svar-ui/svelte-menu";
	import Icon from "./ui/Icon.svelte";
	import SortMenu from "./SortMenu.svelte";

	let { panel, isCameraRoll = false } = $props();

	const api = getContext("filemanager-store");
	const _ = getContext("wx-i18n").getGroup("filemanager");
	const menuOptions = getContext("filemanager-menu-options") || api.menuOptions;
	const { showPrompt } = getContext("filemanager-modals") || {};

	const { panels, mode, activePanel } = api.getReactiveState();
	const files = $derived($panels[panel]._files);
	const crumbs = $derived($panels[panel]._crumbs);
	const currentPath = $derived($panels[panel].path);
	const selected = $derived($panels[panel].selected || []);
	const isPanels = $derived($mode === "panels");
	const opposite = panel === 0 ? 1 : 0;
	const oppositePath = $derived($panels[opposite]?.path || "/");
	const hasSelection = $derived(selected.length > 0);
	const arrow = panel === 0 ? "→" : "←";

	function ensureActive() {
		if (isPanels && $activePanel !== panel) api.exec("set-active-panel", { panel });
	}

	function copyFiles(e) {
		if (e) e.stopPropagation();
		if (!hasSelection) return;
		ensureActive();
		api.exec("copy-files", { ids: [...selected], target: oppositePath });
	}
	function moveFiles(e) {
		if (e) e.stopPropagation();
		if (!hasSelection) return;
		ensureActive();
		api.exec("move-files", { ids: [...selected], target: oppositePath });
	}

	function click(id) {
		if (!id || typeof id !== "string" || !id.startsWith("/")) return;
		ensureActive();
		const selectedId = getSelectionOnNavigation(id, crumbs);
		api.exec("set-path", {
			id,
			panel,
			...(selectedId && { selected: [selectedId] }),
		});
	}

	let loading = $state(null);
	let _refreshTimeout = null;

	$effect(() => {
		if (files) {
			loading = null;
			if (_refreshTimeout) {
				clearTimeout(_refreshTimeout);
				_refreshTimeout = null;
			}
		}
		return () => {
			if (_refreshTimeout) clearTimeout(_refreshTimeout);
		};
	});

	function refresh(ev) {
		if (ev && ev.stopPropagation) ev.stopPropagation();
		ensureActive();
		const id = crumbs && crumbs.length ? crumbs[crumbs.length - 1].id : "/";
		loading = true;
		if (_refreshTimeout) clearTimeout(_refreshTimeout);
		_refreshTimeout = setTimeout(() => {
			loading = false;
		}, 3500);

		api.exec("request-data", { id, refresh: true });
	}

	function backToParent() {
		ensureActive();
		if (crumbs && crumbs.length > 1) {
			const parentCrumb = crumbs[crumbs.length - 2];
			const currentCrumb = crumbs[crumbs.length - 1];
			api.exec("set-path", {
				id: parentCrumb.id,
				panel,
				selected: [currentCrumb.id],
			});
		} else if (currentPath && currentPath !== "/") {
			const parentPath = currentPath.substring(0, currentPath.lastIndexOf("/")) || "/";
			api.exec("set-path", {
				id: parentPath,
				panel,
			});
		}
	}

	function handleClick(pack) {
		const action = pack && pack.action ? pack.action : pack && pack.detail ? pack.detail.action : null;
		if (action) {
			if (typeof action.handler === "function") {
				action.handler({ action, path: currentPath });
				return;
			}
			if (action.id === "connect-device") {
				api.exec("connect-device");
				return;
			}
			if (action.id === "upload") {
				const uploadInput = document.querySelector(
					".wx-sidebar input[type=file], .wx-filemanager input[type=file]"
				);
				if (uploadInput) uploadInput.click();
				else api.exec("upload-file");
			} else if (action.id === "add-file" && showPrompt) {
				showPrompt({
					item: {
						type: "file",
						size: 0,
						date: new Date(),
					},
					parent: currentPath,
					add: true,
				});
			} else if (action.id === "add-folder" && showPrompt) {
				showPrompt({
					item: {
						type: "folder",
						date: new Date(),
						lazy: true,
					},
					parent: currentPath,
					add: true,
				});
			}
		}
	}

	let options = $derived.by(() => {
		const fn = typeof menuOptions === "function" ? menuOptions : () => [];
		return fn("add", { path: currentPath }).map(option => {
			option.text = _(option.text);
			return option;
		});
	});

	const hasSingleAction = $derived(options.length === 1 && typeof options[0].handler === "function");
	const isDeviceRoot = $derived(
		typeof currentPath === "string" && currentPath !== "/" && !currentPath.slice(1).includes("/") && !currentPath.startsWith("/shared-")
	);
	const addButtonTitle = $derived(
		isDeviceRoot
			? (_("Add folder") || (options[0]?.text ?? _("Add New")))
			: (hasSingleAction && options[0]?.text ? options[0].text : _("Add New"))
	);
	function getCrumbName(crumb) {
		if (crumb.id === "/shared-to-me" || crumb.name === "shared-to-me") return _("shared-to-me");
		if (crumb.id === "/shared-by-me" || crumb.name === "shared-by-me") return _("shared-by-me");
		if (crumb.id === "/chats" || crumb.name === "chats" || crumb.name === "Chats") return _("chats");
		if (crumb.id === "/terminal" || crumb.name === "terminal" || crumb.name === "Terminal") return _("terminal");
		return crumb.name;
	}
</script>

<div
	class="wx-breadcrumbs"
	use:delegateClick={{ click }}
	data-menu-ignore="true"
	onclick={ensureActive}
>
	<div class="wx-breadcrumbs-nav">
		<!-- svelte-ignore a11y_click_events_have_key_events -->
		<!-- svelte-ignore a11y_no_static_element_interactions -->
		<div class="wx-refresh-icon" onclick={refresh} role="button" tabindex="-1">
			<Icon name="refresh" spin={!!loading} />
		</div>
		<!-- svelte-ignore a11y_click_events_have_key_events -->
		<!-- svelte-ignore a11y_no_static_element_interactions -->
		<div class="wx-item wx-home-icon" data-id="/" data-menu-ignore="true" title={_("Home")}>
			<svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" style="vertical-align: -2px; display: inline-block;">
				<path d="M8.707 1.5a1 1 0 0 0-1.414 0L.646 8.146a.5.5 0 0 0 .708.708L2 8.207V13.5A1.5 1.5 0 0 0 3.5 15h9a1.5 1.5 0 0 0 1.5-1.5V8.207l.646.647a.5.5 0 0 0 .708-.708L13 5.793V2.5a.5.5 0 0 0-.5-.5h-1a.5.5 0 0 0-.5.5v1.293zM13 7.207V13.5a.5.5 0 0 1-.5.5h-9a.5.5 0 0 1-.5-.5V7.207l5-5z"/>
			</svg>
		</div>
		{#if currentPath && currentPath !== "/"}
			<!-- svelte-ignore a11y_click_events_have_key_events -->
			<!-- svelte-ignore a11y_no_static_element_interactions -->
			<div
				class="wx-back-icon"
				onclick={backToParent}
				title={_("Back to parent folder")}
				role="button"
				tabindex="-1"
			>
				<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align: -2px; display: inline-block;">
					<line x1="12" y1="8" x2="4" y2="8" />
					<polyline points="8 4 4 8 8 12" />
				</svg>
			</div>
		{/if}
	</div>

	<div class="wx-crumbs-list">
		{#each crumbs || [] as crumb, i}
			{#if i && crumb.id !== "/"}
				<div class="wx-crumb-sep">
					<Icon name="angle-right" />
				</div>
			{/if}
			{#if crumb.id !== "/"}
				<div class="wx-item wx-crumb-item" data-id={setID(crumb.id)} data-menu-ignore="true" title={getCrumbName(crumb)}>
					{getCrumbName(crumb)}
				</div>
			{/if}
		{/each}
	</div>

	{#if isPanels && hasSelection}
		<div class="wx-panel-actions" data-menu-ignore="true">
			<button type="button" class="wx-panel-btn wx-panel-copy" onclick={copyFiles} title="Copy to {oppositePath}">
				Copy {arrow}
			</button>
			<button type="button" class="wx-panel-btn wx-panel-move" onclick={moveFiles} title="Move to {oppositePath}">
				Move {arrow}
			</button>
		</div>
	{/if}

	<div class="wx-breadcrumbs-add" data-menu-ignore="true">
		<SortMenu {panel} {isCameraRoll} />
		{#if hasSingleAction}
			<button type="button" class="wx-breadcrumb-add-btn" title={addButtonTitle} onclick={() => options[0].handler({ action: options[0], path: currentPath })}>
				<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round">
					<line x1="8" y1="3" x2="8" y2="13" />
					<line x1="3" y1="8" x2="13" y2="8" />
				</svg>
			</button>
		{:else if options.length > 0}
			<DropDownMenu {options} at="bottom-end" onclick={handleClick}>
				<button type="button" class="wx-breadcrumb-add-btn" title={addButtonTitle}>
					<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round">
						<line x1="8" y1="3" x2="8" y2="13" />
						<line x1="3" y1="8" x2="13" y2="8" />
					</svg>
				</button>
			</DropDownMenu>
		{/if}
	</div>
</div>

<style>
	.wx-breadcrumbs {
		flex: 0 0 48px;
		display: flex;
		justify-content: space-between;
		align-items: center;
		padding: 0 12px 0 6px;
		max-width: 100%;
		border-radius: 6px 6px 0 0;
		background-color: var(--wx-background);
		font-size: 16px;
		overflow: hidden;
		box-sizing: border-box;
	}

	.wx-breadcrumbs-nav {
		display: flex;
		align-items: center;
		gap: 6px;
		flex-shrink: 0;
	}

	.wx-home-icon,
	.wx-back-icon {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 28px;
		height: 28px;
		cursor: pointer;
		border-radius: 4px;
		color: var(--wx-color-font-alt, #94a3b8);
		transition: color 0.15s ease, background-color 0.15s ease;
	}
	.wx-home-icon:hover,
	.wx-back-icon:hover,
	.wx-refresh-icon:hover {
		color: var(--wx-color-primary, #38bdf8);
		background-color: var(--wx-background-alt, rgba(255, 255, 255, 0.08));
	}
	.wx-refresh-icon:hover i,
	.wx-refresh-icon:hover :global(i),
	.wx-refresh-icon:hover :global(.wx-icon) {
		color: var(--wx-color-primary, #38bdf8) !important;
	}

	.wx-crumbs-list {
		display: flex;
		align-items: center;
		flex-grow: 1;
		min-width: 0;
		overflow: hidden;
	}

	.wx-crumb-sep {
		display: flex;
		align-items: center;
		color: var(--wx-color-font-alt, #64748b);
	}

	.wx-refresh-icon {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 28px;
		height: 28px;
		margin-right: 2px;
		flex-shrink: 0;
		cursor: pointer;
		border-radius: 4px;
		color: var(--wx-color-font-alt, #94a3b8);
		transition: color 0.15s ease, background-color 0.15s ease;
	}

	.wx-item {
		cursor: pointer;
		font-size: 15px;
		font-weight: 400;
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
	}
	.wx-item:hover {
		color: var(--wx-color-primary);
	}

	.wx-breadcrumbs-add {
		display: flex;
		align-items: center;
		margin-left: auto;
		flex-shrink: 0;
	}

	.wx-breadcrumb-add-btn {
		width: 32px;
		height: 32px;
		border-radius: 6px;
		background-color: var(--wx-color-primary, #2996CC);
		border: 1px solid var(--wx-color-primary, #2996CC);
		color: #ffffff;
		display: flex;
		align-items: center;
		justify-content: center;
		padding: 0;
		cursor: pointer;
		transition: background-color 0.15s ease, border-color 0.15s ease, transform 0.1s ease;
		box-sizing: border-box;
	}

	.wx-breadcrumb-add-btn:hover {
		background-color: var(--wx-color-primary-hover, #37A9EF);
		border-color: var(--wx-color-primary-hover, #37A9EF);
		transform: scale(1.05);
	}

	.wx-panel-actions {
		display: flex;
		align-items: center;
		gap: 6px;
		margin-left: 8px;
		flex-shrink: 0;
	}
	.wx-panel-btn {
		padding: 5px 10px;
		border-radius: 6px;
		font-size: 12px;
		font-weight: 600;
		line-height: 1;
		cursor: pointer;
		border: 1px solid transparent;
		white-space: nowrap;
		transition: all 0.15s;
	}
	.wx-panel-copy {
		background: rgba(59,130,246,0.12);
		border-color: rgba(59,130,246,0.35);
		color: #3b82f6;
	}
	.wx-panel-copy:hover {
		background: rgba(59,130,246,0.22);
		border-color: rgba(59,130,246,0.55);
	}
	.wx-panel-move {
		background: rgba(245,158,11,0.12);
		border-color: rgba(245,158,11,0.35);
		color: #d97706;
	}
	.wx-panel-move:hover {
		background: rgba(245,158,11,0.22);
		border-color: rgba(245,158,11,0.55);
	}
	:global(.wx-willow-dark-theme) .wx-panel-copy { color: #60a5fa; }
	:global(.wx-willow-dark-theme) .wx-panel-move { color: #fbbf24; }

	@media (max-width: 600px) {
		.wx-crumbs-list {
			margin-left: 10px;
		}
		.wx-crumbs-list .wx-crumb-item:not(:last-child),
		.wx-crumbs-list .wx-crumb-sep {
			display: none !important;
		}
		.wx-crumbs-list .wx-crumb-item:last-child {
			font-weight: 600 !important;
			font-size: 15px !important;
		}
	}
</style>
