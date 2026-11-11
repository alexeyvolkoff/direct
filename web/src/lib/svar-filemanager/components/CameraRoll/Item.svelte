<script>
	import { getContext } from "svelte";
	import { setID } from "@svar-ui/lib-dom";
	import { isVideoFile, formatDuration } from "./media-utils.js";

	let { item } = $props();

	const api = getContext("filemanager-store");
	const _ = getContext("wx-i18n").getGroup("filemanager");
	const templates = getContext("filemanager-store").templates;

	let { panels, activePanel } = api.getReactiveState();

	const selection = $derived($panels[$activePanel].selected || []);
	const isSelected = $derived(selection.indexOf(item.id) >= 0);
	const hasAnySelection = $derived(selection.length > 0);

	let preview = $derived(templates.preview(item, 214, 163));
	let icon = $derived(templates.icon(item, "big"));

	const isVideo = $derived(isVideoFile(item));
	const duration = $derived.by(() => {
		const val = item.customAttributes?.videoDuration || item.duration;
		return formatDuration(val);
	});

	let previewError = $state(false);
	let previewLoaded = $state(false);
	let lastPreview = null;

	$effect(() => {
		if (preview !== lastPreview) {
			lastPreview = preview;
			previewError = false;
			previewLoaded = false;
		}
	});

	function initImg(node) {
		if (node.complete && node.naturalWidth > 0) {
			previewLoaded = true;
		}
		return {
			update() {
				if (node.complete && node.naturalWidth > 0) {
					previewLoaded = true;
				}
			}
		};
	}

	const displayName = $derived(item.name || item.value || "");
	const isSharedToMe = $derived(item.id === "/shared-to-me" || item.name === "shared-to-me");
	const isSharedByMe = $derived(item.id === "/shared-by-me" || item.name === "shared-by-me");
</script>

<div
	class="wx-cr-item"
	class:wx-selected={isSelected}
	data-id={setID(item.id)}
	title={displayName}
>
	<!-- Main Thumbnail -->
	<div class="wx-cr-preview-wrapper">
		{#if preview && !previewError}
			<div class="wx-cr-preview">
				{#if !previewLoaded}
					<div class="wx-cr-placeholder">
						{#if icon}
							<img class="wx-cr-placeholder-icon" alt="" src={icon} />
						{:else}
							<i class="wxi-{item.icon || (isVideo ? 'video' : 'image') || 'file'}"></i>
						{/if}
					</div>
				{/if}
				<img
					use:initImg
					class="wx-cr-img"
					class:wx-cr-img-hidden={!previewLoaded}
					alt={displayName}
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
			<div class="wx-cr-fallback">
				<img class="wx-cr-placeholder-icon" alt="" src={icon} />
			</div>
		{:else}
			<div class="wx-cr-fallback">
				<i class="wxi-{item.icon || (isVideo ? 'video' : 'image') || 'file'}"></i>
			</div>
		{/if}
	</div>

	<!-- Selected Tint Overlay -->
	{#if isSelected}
		<div class="wx-cr-selected-overlay"></div>
	{/if}

	<!-- Selection Checkmark Circle -->
	<div
		class="wx-cr-check"
		class:selected={isSelected}
		class:force-show={hasAnySelection}
		data-select-id={setID(item.id)}
		title={isSelected ? _("Deselect") : _("Select")}
	>
		{#if isSelected}
			<svg viewBox="0 0 24 24" width="16" height="16" fill="white">
				<path d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z" />
			</svg>
		{:else}
			<div class="wx-cr-check-inner"></div>
		{/if}
	</div>

	<!-- Video Indicator Badge -->
	{#if isVideo}
		<div class="wx-cr-video-badge">
			<svg viewBox="0 0 24 24" width="12" height="12" fill="currentColor">
				<path d="M8 5v14l11-7z" />
			</svg>
			{#if duration}
				<span class="wx-cr-duration">{duration}</span>
			{/if}
		</div>
	{/if}

	<!-- More Actions Button -->
	{#if !isSharedToMe && !isSharedByMe && item.id !== "/shared-to-me" && item.id !== "/shared-by-me" && !item.isRootSection}
		<div
			data-action-id={setID(item.id)}
			class="wx-more wx-cr-more"
			title={_("Actions")}
		>
			<i class="wxi-dots-v"></i>
		</div>
	{/if}
</div>

<style>
	.wx-cr-item {
		position: relative;
		aspect-ratio: 1 / 1;
		width: 100%;
		height: 100%;
		overflow: hidden;
		border-radius: 3px;
		background-color: var(--wx-background-alt, rgba(125, 125, 125, 0.12));
		cursor: pointer;
		user-select: none;
		box-sizing: border-box;
		display: block;
	}

	.wx-cr-item:hover {
		filter: brightness(1.05);
	}

	.wx-cr-item.wx-selected {
		box-shadow: inset 0 0 0 3px var(--wx-color-primary, #7D60AC);
	}

	.wx-cr-selected-overlay {
		position: absolute;
		inset: 0;
		background-color: rgba(142, 116, 189, 0.28);
		pointer-events: none;
		z-index: 1;
	}

	.wx-cr-preview-wrapper {
		width: 100%;
		height: 100%;
		position: relative;
		overflow: hidden;
		background: rgba(0, 0, 0, 0.05);
	}

	.wx-cr-preview {
		width: 100%;
		height: 100%;
		position: relative;
		display: flex;
		align-items: center;
		justify-content: center;
	}

	.wx-cr-placeholder {
		position: absolute;
		inset: 0;
		display: flex;
		align-items: center;
		justify-content: center;
		z-index: 1;
		pointer-events: none;
	}

	.wx-cr-placeholder-icon {
		width: 48px;
		height: 48px;
		max-width: 48px;
		max-height: 48px;
		object-fit: contain;
		opacity: 0.85;
	}

	.wx-cr-placeholder i {
		font-size: 44px;
		color: var(--wx-color-primary, #A28AD0);
		display: flex;
		align-items: center;
		justify-content: center;
		line-height: 1;
	}

	.wx-cr-img {
		width: 100%;
		height: 100%;
		object-fit: cover;
		display: block;
		position: relative;
		z-index: 2;
		transition: opacity 0.2s ease-in, transform 0.25s ease;
	}

	.wx-cr-img-hidden {
		opacity: 0 !important;
	}

	.wx-cr-item:hover .wx-cr-img:not(.wx-cr-img-hidden) {
		transform: scale(1.025);
	}

	.wx-cr-fallback {
		width: 100%;
		height: 100%;
		display: flex;
		align-items: center;
		justify-content: center;
		color: var(--wx-color-primary, #A28AD0);
		font-size: 44px;
	}

	.wx-cr-icon {
		width: 48px;
		height: 48px;
		object-fit: contain;
	}

	/* Selection Checkmark */
	.wx-cr-check {
		position: absolute;
		top: 6px;
		left: 6px;
		width: 24px;
		height: 24px;
		border-radius: 50%;
		display: flex;
		align-items: center;
		justify-content: center;
		z-index: 2;
		opacity: 0;
		transform: scale(0.85);
		transition: opacity 0.15s ease, transform 0.15s ease, background-color 0.15s ease;
		cursor: pointer;
	}

	.wx-cr-item:hover .wx-cr-check,
	.wx-cr-check.force-show,
	.wx-cr-check.selected {
		opacity: 1;
		transform: scale(1);
	}

	.wx-cr-check.selected {
		background: var(--wx-color-primary, #7D60AC);
		box-shadow: 0 1px 4px rgba(0, 0, 0, 0.4);
	}

	.wx-cr-check:not(.selected) {
		background: rgba(0, 0, 0, 0.35);
		backdrop-filter: blur(4px);
		border: 1.5px solid rgba(255, 255, 255, 0.75);
	}

	.wx-cr-check-inner {
		width: 10px;
		height: 10px;
		border-radius: 50%;
	}

	/* Video Indicator */
	.wx-cr-video-badge {
		position: absolute;
		bottom: 6px;
		right: 6px;
		background: rgba(0, 0, 0, 0.65);
		backdrop-filter: blur(4px);
		color: #ffffff;
		font-size: 11px;
		font-weight: 500;
		padding: 2px 6px;
		border-radius: 4px;
		display: flex;
		align-items: center;
		gap: 4px;
		pointer-events: none;
		z-index: 2;
		line-height: 1;
	}

	.wx-cr-duration {
		font-size: 10px;
		letter-spacing: 0.2px;
	}

	/* More actions button ("...") */
	.wx-cr-more {
		position: absolute;
		top: 6px;
		right: 6px;
		width: 24px;
		height: 24px;
		border-radius: 50%;
		background: rgba(0, 0, 0, 0.45);
		backdrop-filter: blur(4px);
		color: #ffffff;
		display: flex;
		align-items: center;
		justify-content: center;
		opacity: 0;
		transform: scale(0.85);
		transition: opacity 0.15s ease, transform 0.15s ease, background 0.15s ease;
		z-index: 2;
		font-size: 14px;
		cursor: pointer;
	}

	.wx-cr-item:hover .wx-cr-more {
		opacity: 1;
		transform: scale(1);
	}

	.wx-cr-more:hover {
		background: rgba(0, 0, 0, 0.75);
	}

	@media (max-width: 768px) {
		.wx-cr-more,
		.wx-cr-check.force-show {
			opacity: 1;
			transform: scale(1);
		}
	}
</style>
