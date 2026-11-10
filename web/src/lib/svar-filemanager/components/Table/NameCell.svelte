<script>
	import { getContext } from "svelte";

	let { row } = $props();

	const _ = getContext("wx-i18n").getGroup("filemanager");
	const templates = getContext("filemanager-store").templates;
	const fmApi = getContext("filemanager-store");
	const { panels, activePanel } = fmApi ? fmApi.getReactiveState() : {};

	const icon = templates.icon(row, "small");
	let preview = $derived(templates.preview(row, 24, 24));
	const displayName = $derived.by(() => {
		if (row.id === "/shared-to-me" || row.name === "shared-to-me") return _("shared-to-me");
		if (row.id === "/shared-by-me" || row.name === "shared-by-me") return _("shared-by-me");
		if (row.mimeType === "application/x-chat" || row.name === "Chats" || row.name === "chats" || row.id === "/chats") return _("chats");
		if (
			row.mimeType === "application/x-terminal" ||
			row.name === "Terminal" ||
			row.name === "terminal" ||
			row.id === "/terminal" ||
			row.id === "subapp:terminal"
		)
			return _("terminal");
		if (row.mimeType === "application/x-web-app" && row.customAttributes?.appName) {
			return row.customAttributes.appName;
		}
		return row.name;
	});

	function backToParent() {
		if (!fmApi) return;
		const panel = $activePanel !== undefined && $activePanel !== null ? $activePanel : 0;
		const currentPanel = $panels ? $panels[panel] : null;
		const crumbs = currentPanel ? currentPanel._crumbs : null;
		const path = currentPanel ? currentPanel.path : null;
		if (crumbs && crumbs.length > 1) {
			fmApi.exec("set-path", {
				id: crumbs[crumbs.length - 2].id,
				panel: panel,
				selected: [crumbs[crumbs.length - 1].id],
			});
		} else if (path && path !== "/") {
			const parentPath = path.substring(0, path.lastIndexOf("/")) || "/";
			fmApi.exec("set-path", {
				id: parentPath,
				panel: panel,
			});
		}
	}

	function handleCellClick(e) {
		if (e) e.stopPropagation();
		if (row.id === "/wx-filemanager-parent-link") {
			return backToParent();
		}
		if (e && (e.ctrlKey || e.metaKey || e.shiftKey)) return;
		if (e && e.target && (e.target.closest(".wx-more") || e.target.closest(".wx-checkbox")))
			return;

		if (!fmApi) return;
		const panel = $activePanel !== undefined && $activePanel !== null ? $activePanel : 0;
		if (row && (row.type === "folder" || row.isDir || row.isDevice)) {
			fmApi.exec("set-path", {
				id: row.id,
				panel,
			});
		} else {
			fmApi.exec("open-file", {
				id: row.id,
			});
		}
	}
</script>

<!-- svelte-ignore a11y_click_events_have_key_events -->
<!-- svelte-ignore a11y_no_static_element_interactions -->
<div
	class="wx-name-cell"
	class:wx-back-cell={row.id === "/wx-filemanager-parent-link"}
	onclick={handleCellClick}
>
	{#if row.id === "/wx-filemanager-parent-link"}
		<i class="wxi-arrow-left"></i>
		<span class="wx-name"> {_("Back to parent folder")}</span>
	{:else}
		{#if preview}
			<img
				class="wx-icon wx-preview-img"
				alt=""
				loading="lazy"
				src={preview}
				height="24px"
				width="24px"
				style={icon
					? `object-fit: cover; border-radius: 2px; background-image: url('${icon}'); background-size: cover; background-position: center;`
					: `object-fit: cover; border-radius: 2px;`}
			/>
		{:else if icon}
			<img class="wx-icon" alt="" loading="lazy" src={icon} height="24px" width="24px" />
		{:else}<i class="wxi-{row.icon || row.type || 'folder'}"></i>{/if}
		<span class="wx-name"> {displayName} </span>
	{/if}
</div>

<style>
	.wx-name-cell {
		padding: 0 4px;
		display: flex;
		align-items: center;
		height: 100%;
		overflow: hidden;
		flex-shrink: 0;
		text-overflow: clip;
		cursor: pointer;
	}
	.wx-back-cell {
		cursor: pointer;
	}
	i,
	.wx-icon {
		margin-right: 10px;
		display: flex;
		align-items: center;
	}

	i {
		font-size: 24px;
		color: var(--wx-color-primary);
	}
	.wx-name {
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
	}
</style>
