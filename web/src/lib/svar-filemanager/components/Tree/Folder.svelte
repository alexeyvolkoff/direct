<script>
	import { setID } from "@svar-ui/lib-dom";
	import Folder from "./Folder.svelte";
	import { getContext } from "svelte";

	const api = getContext("filemanager-store");
	const { panels, activePanel } = api.getReactiveState();

	let path = $derived($panels[$activePanel].path);

	let { folderIcon = false, folder = {} } = $props();

	const _ = getContext("wx-i18n").getGroup("filemanager");

	const file = $derived(api.getFile(folder.id) || folder);
	const hasFolders = $derived(!!folder.data?.find(i => i?.type === "folder"));
	const padding = $derived(folder.$level > 2 ? (folder.$level - 2) * 16 : 0);
	const name = $derived.by(() => {
		if (folder.id === "/" || folder.name === "My files") return _(folder.name);
		if (folder.id === "/chats" || folder.name === "chats" || folder.name === "Chats" || folder.mimeType === "application/x-chat") return _("chats");
		if (
			folder.id === "/terminal" ||
			folder.name === "terminal" ||
			folder.name === "Terminal" ||
			folder.mimeType === "application/x-terminal" ||
			folder.id === "subapp:terminal"
		)
			return _("terminal");
		if (folder.id === "/shared-to-me" || folder.name === "shared-to-me") return _("shared-to-me");
		if (folder.id === "/shared-by-me" || folder.name === "shared-by-me") return _("shared-by-me");
		return folder.name;
	});
	const open = $derived(folder.open);
	const isDevice = $derived(
		(
			file?.itemType === "device" ||
			file?.isDevice ||
			folder.itemType === "device" ||
			folder.isDevice ||
			(typeof folder.id === "string" &&
				folder.id.split("/").filter(Boolean).length === 1 &&
				folder.id !== "/")
		) &&
			!folder.isRootSection &&
			!file?.isRootSection
	);
	const customIcon = $derived.by(() => {
		const isDesktop =
			file?.deviceType === "desktop" ||
			folder.deviceType === "desktop" ||
			folder.name?.toLowerCase().includes("brave") ||
			folder.name?.toLowerCase().includes("laptop") ||
			folder.name?.toLowerCase().includes("desktop") ||
			folder.name?.toLowerCase().includes("linux") ||
			folder.name?.toLowerCase().includes("windows") ||
			folder.name?.toLowerCase().includes("mac") ||
			folder.name?.toLowerCase().includes("pc");
		const isPhone =
			file?.deviceType === "phone" ||
			folder.name?.toLowerCase().includes("phone") ||
			folder.name?.toLowerCase().includes("android") ||
			folder.name?.toLowerCase().includes("iphone");
		const isTablet =
			file?.deviceType === "tablet" ||
			folder.name?.toLowerCase().includes("tablet") ||
			folder.name?.toLowerCase().includes("ipad");
		const isShared =
			file?.itemType === "shared" ||
			file?.isShared ||
			folder.itemType === "shared" ||
			folder.isShared ||
			folder.name?.toLowerCase().includes("shared");
		const isLinks =
			folder.name?.toLowerCase().includes("links") || file?.icon === "wxi-folder-links";
		const isSites =
			folder.name?.toLowerCase().includes("sites") || file?.icon === "wxi-folder-sites";

		if (folder.icon) {
			return folder.icon;
		} else if (file?.icon) {
			return file.icon;
		} else if (isDevice && folder.id !== "/") {
			return isPhone ? "wxi-phone" : isTablet ? "wxi-tablet" : isDesktop ? "wxi-desktop" : "wxi-embedded";
		} else if (isShared) {
			return "wxi-folder-shared";
		} else if (isLinks) {
			return "wxi-folder-links";
		} else if (isSites) {
			return "wxi-folder-sites";
		} else if (folder.id === "/") {
			return "wxi-devices";
		}
		return "wxi-folder";
	});

	const childFolders = $derived.by(() => {
		if (!open || !folder.data || !folder.data.length || !hasFolders) return [];
		const seen = new Set();
		return folder.data.filter(item => {
			if (!item || item.type !== "folder") return false;
			const id = item.id;
			if (!id || seen.has(id)) return false;
			seen.add(id);
			return true;
		});
	});
</script>

<li
	data-id={setID(folder.id)}
	class="wx-folder"
	class:wx-selected={path === folder.id}
	style="padding-left: {padding}px"
>
	{#if hasFolders}
		<i
			class="wx-toggle wxi-{open ? 'angle-down' : 'angle-right'}"
			data-action="toggle"
		></i>
	{:else}<span class="wx-toggle-placeholder"></span>{/if}
	<i class={customIcon || folderIcon || "wxi-folder"}></i>
	<span class="wx-name"> {name} </span>
	{#if (isDevice || folder.id === "/chats" || folder.name === "chats") && folder.id !== "/"}
		{@const isOnline = (folder?.online !== undefined ? folder.online : file?.online) !== false}
		<span
			class="wx-status-dot {isOnline ? 'online' : 'offline'}"
			title={isOnline ? 'Online' : 'Offline'}
		></span>
	{/if}
</li>

{#if childFolders.length}
	{#each childFolders as child (child.id)}
		<Folder folder={child} />
	{/each}
{/if}

<style>
	.wx-folder {
		display: flex;
		align-items: center;
		cursor: default;
		letter-spacing: 0.2px;
		width: 100%;
		height: 32px;
		vertical-align: top;
		white-space: nowrap;
		position: relative;
	}

	.wx-selected {
		background-color: var(--wx-fm-select-background);
	}

	.wx-folder:hover {
		background-color: var(--wx-hover-background, rgba(128, 128, 128, 0.1));
	}
	i {
		font-size: 22px;
		margin-right: 8px;
		max-height: 100%;
		color: var(--wx-color-primary);
	}
	.wx-toggle {
		cursor: pointer;
		color: var(--wx-icon-color);
		font-size: 24px;
		margin-right: -2px;
	}
	.wx-toggle-placeholder {
		width: 23px;
		flex-shrink: 0;
	}
	.wx-name {
		padding-right: 8px;
	}
	.wx-status-dot {
		width: 7px;
		height: 7px;
		border-radius: 50%;
		margin-left: auto;
		margin-right: 8px;
		flex-shrink: 0;
	}
	.wx-status-dot.online {
		background-color: #22c55e;
		box-shadow: 0 0 6px rgba(34, 197, 94, 0.6);
	}
	.wx-status-dot.offline {
		background-color: #73688A;
	}
</style>
