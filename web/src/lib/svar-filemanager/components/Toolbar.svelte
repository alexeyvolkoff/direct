<script>
	import { getContext } from "svelte";
	import { Segmented } from "@svar-ui/svelte-core";
	import { setID } from "@svar-ui/lib-dom";
	import Search from "./ui/Search.svelte";
	import { translate } from "../../i18n/index.js";
	import { unreadCount, openNotificationsModal } from "../../notification-inbox.js";

	let { narrowMode = false, onshowtree, toolbarActions = null } = $props();

	const api = getContext("filemanager-store");

	const { mode, search: searchValue, searchViewMode } = api.getReactiveState();

	const _ = getContext("wx-i18n").getGroup("filemanager");

	const options = [
		{ icon: "wxi-view-sequential", id: "table" },
		{ icon: "wxi-view-grid", id: "cards" },
	];

	function changeMode({ value }) {
		if ($mode === "search") {
			api.exec("set-search-mode", { mode: value });
		} else {
			api.exec("set-mode", { mode: value });
		}
	}
	function search(e) {
		api.exec("filter-files", { text: e.value });
	}
</script>

<div class="wx-toolbar" class:wx-search-active={$mode === "search"}>
	<div class="wx-left {narrowMode ? 'wx-left-narrow' : ''}" class:wx-left-search={$mode === "search"}>
		{#if !narrowMode}
			<div class="wx-name">{_("Files")}</div>
		{:else if !($mode == "panels" || $mode == "search")}
			<div class="wx-sidebar-icon" data-id={setID("toggle-tree")}>
				<button
					type="button"
					class="wx-sidebar-toggle-btn"
					onclick={() => onshowtree && onshowtree()}
					aria-label="Toggle Sidebar"
					title="Toggle Sidebar"
				>
					<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" fill="currentColor" viewBox="0 0 16 16">
						<path d="M0 3a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2zm5-1v12h9a1 1 0 0 0 1-1V3a1 1 0 0 0-1-1zM4 2H2a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h2z"/>
					</svg>
				</button>
			</div>
		{/if}
		<Search onsearch={search} value={$searchValue} />
	</div>

	<div class="wx-right">
		{#if $unreadCount > 0}
			<button
				type="button"
				class="wx-toolbar-bell-btn"
				title={translate("notifications", {}, "Notifications")}
				aria-label={translate("notifications", {}, "Notifications")}
				onclick={openNotificationsModal}
			>
				<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
					<path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9" />
					<path d="M13.73 21a2 2 0 0 1-3.46 0" />
				</svg>
				<span class="wx-toolbar-bell-badge">{$unreadCount}</span>
			</button>
		{/if}
		<div class="wx-modes">
			<Segmented
				value={$mode === "search" ? ($searchViewMode || "cards") : $mode}
				{options}
				onchange={changeMode}
			/>
			{#if toolbarActions}
				<div class="wx-toolbar-actions">
					{@render toolbarActions()}
				</div>
			{/if}
		</div>
	</div>
</div>

<style>
	.wx-toolbar {
		height: var(--wx-fm-toolbar-height);
		display: flex;
		justify-content: space-between;
		align-items: center;
		padding: 0 12px;
		max-width: 100%;
		background-color: var(--wx-background);
		box-shadow: var(--wx-fm-box-shadow);
		gap: 8px;
	}

	.wx-left,
	.wx-right {
		display: flex;
	}

	.wx-left {
		align-items: center;
		width: 35%;
		justify-content: space-between;
	}
	.wx-left-narrow {
		width: 70%;
	}
	.wx-toolbar.wx-search-active .wx-left {
		flex: 1;
		width: auto;
		max-width: none;
	}
	.wx-toolbar.wx-search-active :global(.wx-search-input) {
		width: 440px !important;
		max-width: min(600px, 60vw) !important;
		transition: width 0.2s ease, max-width 0.2s ease !important;
	}
	@media (max-width: 600px) {
		.wx-toolbar.wx-search-active :global(.wx-search-input) {
			width: 100% !important;
			max-width: 100% !important;
		}
	}
	.wx-name {
		margin-right: 20px;
		font-size: 16px;
		font-weight: var(--wx-font-weight-md);
	}

	.wx-sidebar-icon {
		margin-right: 12px;
		display: flex;
		align-items: center;
	}

	.wx-sidebar-toggle-btn {
		background: transparent;
		border: none;
		color: var(--wx-color-font, #AEB5C7);
		display: flex;
		align-items: center;
		justify-content: center;
		padding: 6px;
		border-radius: 6px;
		cursor: pointer;
		transition: all 0.15s ease;
	}

	.wx-sidebar-toggle-btn:hover {
		background: var(--wx-background-alt, rgba(255, 255, 255, 0.08));
		color: var(--wx-color-primary, #929CD4);
	}

	.wx-preview-icon {
		display: flex;
		justify-content: center;
		align-items: center;
		margin-right: 20px;

		--wx-button-icon-size: 22px;
		--wx-button-line-height: 25px;
		--wx-button-padding: 4px 10px;
		--wx-button-border-radius: 6px;
	}

	.wx-preview-icon i {
		position: relative;
		display: inline-block;
		vertical-align: top;
		font-size: var(--wx-button-icon-size);
		line-height: 1;
		height: var(--wx-button-line-height);
	}

	.wx-preview-icon i:before {
		display: block;
		position: relative;
		top: 50%;
		transform: translateY(-50%);
		color: var(--wx-fm-button-font-color);
	}

	.wx-modes {
		display: flex;
		align-items: center;
		gap: 6px;
		--wx-button-font-color: var(--wx-fm-button-font-color);
		--wx-segmented-padding: 1.5px;
		--wx-segmented-background-hover: linear-gradient(
			rgba(0, 0, 0, 0.1) 0%,
			rgba(0, 0, 0, 0.1) 100%
		);
		--wx-button-icon-size: 22px;
		--wx-segmented-border-radius: 6px;
		--wx-segmented-border: none;
	}

	.wx-toolbar-actions {
		display: flex;
		align-items: center;
		margin-left: 6px;
	}

	.wx-toolbar-bell-btn {
		position: relative;
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 34px;
		height: 34px;
		margin-right: 8px;
		border-radius: 8px;
		border: 1px solid transparent;
		background: transparent;
		color: var(--wx-fm-button-font-color, var(--text-secondary));
		cursor: pointer;
		transition: background 0.15s ease, color 0.15s ease;
	}

	.wx-toolbar-bell-btn:hover {
		background: var(--wx-background-hover, rgba(0, 0, 0, 0.06));
		color: var(--wx-color-font, var(--text-primary));
	}

	.wx-toolbar-bell-badge {
		position: absolute;
		top: 2px;
		right: 2px;
		min-width: 16px;
		height: 16px;
		padding: 0 4px;
		border-radius: 9999px;
		background: var(--accent, #5863A6);
		color: var(--accent-on-brand, #ffffff);
		font-size: 10px;
		font-weight: 700;
		line-height: 16px;
		text-align: center;
	}
</style>
