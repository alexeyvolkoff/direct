<script>
	import { getContext } from "svelte";
	import { setID } from "@svar-ui/lib-dom";

	let { item } = $props();

	const api = getContext("filemanager-store");
	const _ = getContext("wx-i18n").getGroup("filemanager");
	const templates = getContext("filemanager-store").templates;

	let { panels, activePanel, mode } = api.getReactiveState();

	const selection = $derived($panels[$activePanel].selected);

	let preview = $derived(templates.preview(item, 214, 163));
	let icon = $derived(templates.icon(item, "big"));
	let isSharedToMe = $derived(item.id === "/shared-to-me" || item.name === "shared-to-me");
	let isSharedByMe = $derived(item.id === "/shared-by-me" || item.name === "shared-by-me");
	let displayName = $derived.by(() => {
		if (item.id === "/chats" || item.name === "chats" || item.name === "Chats") return _("chats");
		if (item.mimeType === "application/x-chat") return _("chats");
		if (
			item.mimeType === "application/x-terminal" ||
			item.name === "Terminal" ||
			item.name === "terminal" ||
			item.id === "/terminal" ||
			item.id === "subapp:terminal"
		)
			return _("terminal");
		if (isSharedToMe) return _("shared-to-me");
		if (isSharedByMe) return _("shared-by-me");
		if (item.mimeType === "application/x-web-app" && item.customAttributes?.appName) {
			return item.customAttributes.appName;
		}
		return item.name;
	});
	let [nameStart, nameTail] = $derived(splitFileName(displayName));

	let previewError = $state(false);
	let previewLoaded = $state(false);
	$effect(() => {
		item;
		previewError = false;
		previewLoaded = false;
	});

	let isApp = $derived(
		item.mimeType === "application/x-terminal" ||
			item.mimeType === "application/x-chat" ||
			item.mimeType === "application/x-code" ||
			item.mimeType === "application/x-web-app" ||
			(typeof item.id === "string" && item.id.startsWith("subapp:")) ||
			item.itemType === "app"
	);
	let isDevice = $derived(
		(
			item.isDevice ||
			item.itemType === "device" ||
			(typeof item.id === "string" &&
				item.id.split("/").filter(Boolean).length === 1 &&
				item.id !== "/")
		) &&
			!isSharedToMe &&
			!isSharedByMe &&
			!item.isRootSection
	);
	let isShared = $derived(item.isShared);
	let typeLabel = $derived.by(() => {
		if (item.typeLabel) {
			return _(item.typeLabel);
		} else if (isSharedToMe) {
			return _("SHARED WITH ME");
		} else if (isSharedByMe) {
			return _("SHARED BY ME");
		} else if (isApp) {
			return _("App");
		} else if (isDevice) {
			return _("Device");
		} else if (item.type === "folder" || item.isDir) {
			return _("Folder");
		} else {
			if (item.ext && item.ext.length <= 6) {
				return item.ext.toUpperCase();
			} else if (item.mimeType) {
				const parts = item.mimeType.split("/");
				const sub = parts[1] || parts[0];
				if (sub.startsWith("vnd.") || item.mimeType.length > 18) {
					return item.ext ? item.ext.toUpperCase() : sub.split(".").pop().toUpperCase();
				}
				return item.mimeType;
			}
			return _("File");
		}
	});

	function splitFileName(name) {
		const MAX_TAIL_LENGTH = 10;
		const NO_EXT_TAIL_LENGTH = 5;

		const lastDotIndex = name.lastIndexOf(".");
		const hasExtension = lastDotIndex > 0 && lastDotIndex < name.length - 1;
		const tailLength = hasExtension
			? Math.min(MAX_TAIL_LENGTH, name.length - lastDotIndex)
			: Math.min(NO_EXT_TAIL_LENGTH, name.length);

		const nameStart = name.slice(0, -tailLength);
		const nameTail = name.slice(-tailLength);

		return [nameStart, nameTail];
	}

	function backToParent() {
		const currentPanel = $panels ? $panels[$activePanel] : null;
		const crumbs = currentPanel ? currentPanel._crumbs : null;
		const path = currentPanel ? currentPanel.path : null;
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
</script>

{#if item.id === "/wx-filemanager-parent-link"}
	{#if $mode !== "search"}
		<div class="wx-back-item">
			<div
				class="wx-back"
				data-id={setID("/wx-filemanager-parent-link")}
				class:wx-selected={item.navigation}
				onclick={backToParent}
			>
				<i class="wxi-arrow-left"></i>
				<span> {_("Back to parent folder")}</span>
			</div>
		</div>
	{/if}
{:else}
	<div
		class="wx-item"
		class:wx-selected={selection?.length && selection.indexOf(item.id) >= 0}
		data-id={setID(item.id)}
		title={displayName}
	>
		<div class="wx-preview-wrapper">
			{#if preview && !previewError}
				<div class="wx-preview wx-file-preview">
					{#if !previewLoaded}
						<div class="wx-card-placeholder">
							{#if icon}
								<img class="wx-card-placeholder-icon" alt="" src={icon} />
							{:else}
								<i class="wxi-{item.icon || item.type || 'file'}"></i>
							{/if}
						</div>
					{/if}
					<img
						class="wx-card-preview"
						class:wx-card-preview-hidden={!previewLoaded}
						alt={_("Preview")}
						loading="lazy"
						src={preview}
						onload={() => (previewLoaded = true)}
						onerror={() => {
							previewError = true;
							setTimeout(() => {
								if (previewError && !previewLoaded) {
									previewError = false;
								}
							}, 5000);
						}}
					/>
				</div>
			{:else if icon}
				<div class="wx-preview wx-file-icon">
					<img class="wx-card-preview" alt="" src={icon} />
				</div>
			{:else}
				<div class="wx-preview"><i class="wxi-{item.icon || item.type || 'folder'}"></i></div>
			{/if}

			{#if !isSharedToMe && !isSharedByMe && item.id !== "/shared-to-me" && item.id !== "/shared-by-me" && !item.isRootSection}
				<div data-action-id={setID(item.id)} class="wx-more wx-more-mobile" title={_("Actions")}>
					<i class="wxi-dots-v"></i>
				</div>
			{/if}
		</div>

		<div class="wx-info">
			<div class="wx-folder-name">
				<div class="wx-type-wrapper">
					<span class="wx-type">{typeLabel}</span>
				{#if item.isDevice || item.itemType === "device" || item.id === "/chats" || item.name === "chats"}
					<span class="wx-device-badge {item.online !== false ? 'online' : 'offline'}">
						<span class="wx-device-badge-dot"></span>
						{item.online !== false ? 'Online' : 'Offline'}
					</span>
				{/if}
				</div>
				<span class="wx-name" title={displayName}>
					<span>{nameStart}</span><span>{nameTail}</span>
				</span>
			</div>

			{#if !isSharedToMe && !isSharedByMe && item.id !== "/shared-to-me" && item.id !== "/shared-by-me" && !item.isRootSection}
				<div data-action-id={setID(item.id)} class="wx-more wx-more-desktop" title={_("Actions")}>
					<i class="wxi-dots-v"></i>
				</div>
			{/if}
		</div>
	</div>
{/if}

<style>
	.wx-item {
		display: flex;
		flex-direction: column;
		width: 210px;
		height: 200px;
		box-sizing: border-box;
		margin: 0 20px 20px 0;
		overflow: hidden;
		cursor: pointer;
		background-color: var(--wx-background);
		border-radius: 12px;
		box-shadow: var(--wx-fm-box-shadow);
		user-select: none;
		position: relative;
		transition: transform 0.1s ease, box-shadow 0.15s ease;
	}

	.wx-item:hover {
		box-shadow: 0 4px 12px rgba(0, 0, 0, 0.25);
	}

	.wx-selected {
		outline: none !important;
	}
	.wx-item.wx-selected {
		background-color: var(--wx-fm-select-background, #E4F1FA);
	}
	.wx-item.wx-selected .wx-info {
		background-color: var(--wx-fm-select-background, #E4F1FA);
	}

	.wx-preview-wrapper {
		display: flex;
		align-items: center;
		justify-content: center;
		flex-shrink: 0;
		width: 100%;
		height: 144px;
		overflow: hidden;
		position: relative;
		background: rgba(0, 0, 0, 0.05);
		box-sizing: border-box;
	}

	.wx-preview {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 100%;
		height: 100%;
		position: relative;
	}

	.wx-card-placeholder {
		position: absolute;
		inset: 0;
		display: flex;
		align-items: center;
		justify-content: center;
		z-index: 1;
		pointer-events: none;
	}

	.wx-card-placeholder-icon {
		height: 80px;
		width: 80px;
		max-height: 80px;
		max-width: 80px;
		object-fit: contain;
		opacity: 0.9;
	}

	.wx-card-placeholder i {
		font-size: 80px;
		color: var(--wx-color-primary, #929CD4);
		display: flex;
		align-items: center;
		justify-content: center;
		line-height: 1;
	}

	.wx-preview i {
		font-size: 80px;
		color: var(--wx-color-primary, #929CD4);
		display: flex;
		align-items: center;
		justify-content: center;
		line-height: 1;
	}

	.wx-file-preview .wx-card-preview {
		height: 154px;
		width: 100%;
		object-fit: cover;
		position: relative;
		z-index: 2;
		transition: opacity 0.2s ease-in;
	}

	.wx-card-preview-hidden {
		opacity: 0 !important;
	}

	.wx-file-icon .wx-card-preview {
		height: 80px;
		width: 80px;
		object-fit: contain;
	}

	.wx-info {
		display: flex;
		align-items: center;
		height: auto;
		min-height: 54px;
		flex-grow: 1;
		padding: 6px 8px 6px 12px;
		box-sizing: border-box;
		background-color: var(--wx-background);
		width: 100%;
		border-radius: 0 0 11px 11px;
	}

	.wx-folder-name {
		display: flex;
		flex-direction: column;
		flex-grow: 1;
		min-width: 0;
		overflow: hidden;
		margin-right: 4px;
		text-align: left;
	}

	.wx-type-wrapper {
		display: flex;
		align-items: center;
		gap: 6px;
		height: 16px;
		margin-bottom: 2px;
	}

	.wx-type {
		color: var(--wx-color-font-alt);
		font-size: 11px;
		line-height: 16px;
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
		text-transform: uppercase;
		letter-spacing: 0.3px;
	}

	.wx-name {
		font-size: 13px;
		font-weight: var(--wx-font-weight-md, 500);
		color: var(--wx-color-font, #ECE7F6);
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
		text-align: left;
		display: block;
	}

	.wx-name > span:first-child {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
		min-width: 0;
	}
	.wx-name > span:last-child {
		white-space: nowrap;
		flex-shrink: 0;
	}

	.wx-more-desktop {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 28px;
		height: 28px;
		min-width: 28px;
		min-height: 28px;
		max-width: 28px;
		max-height: 28px;
		aspect-ratio: 1 / 1;
		padding: 0;
		margin-left: auto;
		flex-shrink: 0;
		border-radius: 50%;
		color: var(--wx-icon-color);
		box-sizing: border-box;
		cursor: pointer;
		transition: background-color 0.15s ease, color 0.15s ease;
	}

	.wx-more-desktop:hover {
		background-color: var(--wx-button-background, rgba(255, 255, 255, 0.12));
		color: var(--wx-color-primary, #929CD4);
		border-radius: 50%;
	}

	.wx-more-desktop i {
		font-size: 18px;
		line-height: 1;
		display: flex;
		align-items: center;
		justify-content: center;
		width: 100%;
		height: 100%;
		margin: 0;
		padding: 0;
	}

	.wx-more-mobile i {
		font-size: 18px;
	}

	.wx-more-mobile {
		display: none;
	}

	.wx-device-badge {
		font-size: 9px;
		font-weight: 600;
		padding: 1px 5px;
		border-radius: 4px;
		display: inline-flex;
		align-items: center;
		gap: 3px;
		line-height: 1.2;
		text-transform: uppercase;
		flex-shrink: 0;
	}
	.wx-device-badge.online {
		background-color: rgba(34, 197, 94, 0.15);
		color: #4ade80;
	}
	.wx-device-badge.offline {
		background-color: rgba(139, 128, 163, 0.15);
		color: #7E869B;
	}
	.wx-device-badge-dot {
		width: 4px;
		height: 4px;
		border-radius: 50%;
		background-color: currentColor;
		flex-shrink: 0;
		transform: translateY(-1px);
	}

	.wx-back-item {
		width: 100%;
		margin: 6px 0 12px;
		font-size: 13px;
		line-height: 18px;
		cursor: pointer;
	}

	.wx-back {
		display: flex;
		align-items: center;
		width: fit-content;
		color: var(--top-menu-color, var(--wx-color-primary, #929CD4));
		user-select: none;
		font-weight: 500;
		cursor: pointer;
		transition: color 0.15s ease;
	}

	.wx-back:hover {
		color: var(--top-menu-hover-color, #7dd3fc);
	}

	.wx-back i {
		display: flex;
		align-items: center;
		margin-right: 8px;
		font-size: 18px;
	}

	@media (max-width: 600px) {
		.wx-item {
			width: calc(33.333% - 8px);
			height: auto;
			min-height: 105px;
			margin: 0 4px 14px;
			padding: 6px 2px;
			background-color: transparent;
			box-shadow: none;
			border-radius: 8px;
			align-items: center;
		}

		.wx-item:hover {
			box-shadow: none;
		}

		.wx-selected {
			background-color: rgba(123, 134, 196, 0.18) !important;
			outline: none !important;
		}

		.wx-preview-wrapper {
			width: 56px;
			height: 56px;
			flex-grow: 0;
			margin-bottom: 4px;
			background: transparent;
			overflow: visible !important;
			position: relative;
		}

		.wx-file-preview {
			width: 56px;
			height: 56px;
			border-radius: 6px;
			overflow: hidden !important;
			background: rgba(0, 0, 0, 0.2);
		}

		.wx-file-preview .wx-card-preview {
			height: 100%;
			width: 100%;
			object-fit: cover;
		}

		.wx-file-icon .wx-card-preview {
			width: 48px;
			height: 48px;
		}

		.wx-card-placeholder-icon {
			width: 48px;
			height: 48px;
		}

		.wx-card-placeholder i {
			font-size: 48px;
		}

		.wx-preview i {
			font-size: 48px;
		}

		.wx-info {
			height: auto;
			padding: 0;
			background: transparent;
			flex-direction: column;
			align-items: center;
			text-align: center;
		}

		.wx-folder-name {
			align-items: center;
			margin-right: 0;
			text-align: center;
		}

		.wx-type-wrapper {
			display: none;
		}

		.wx-name {
			font-size: 11.5px;
			line-height: 1.25;
			text-align: center;
			white-space: normal;
			word-break: break-word;
			display: -webkit-box;
			-webkit-line-clamp: 2;
			-webkit-box-orient: vertical;
			overflow: hidden;
			text-overflow: ellipsis;
			max-width: 100%;
		}
		.wx-name > span:first-child,
		.wx-name > span:last-child {
			white-space: normal;
		}

		.wx-more-desktop {
			display: none;
		}

		.wx-more-mobile {
			position: absolute;
			top: -4px;
			right: -6px;
			display: flex;
			align-items: center;
			justify-content: center;
			width: 22px;
			height: 22px;
			border-radius: 50%;
			background: rgba(21, 24, 33, 0.85);
			border: 1px solid rgba(255, 255, 255, 0.15);
			color: var(--wx-icon-color, #AEB5C7);
			opacity: 0.9;
			z-index: 2;
			box-sizing: border-box;
			box-shadow: 0 1px 4px rgba(0, 0, 0, 0.4);
		}

		.wx-more-mobile i {
			font-size: 13px;
			line-height: 1;
			display: flex;
			align-items: center;
			justify-content: center;
			width: 100%;
			height: 100%;
			text-align: center;
			margin: 0;
			padding: 0;
		}

		.wx-more-mobile i:before {
			line-height: 1;
			display: block;
		}
	}
</style>
