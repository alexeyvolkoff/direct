<script>
	import { getContext } from "svelte";
	import { delegateClick, setID } from "@svar-ui/lib-dom";
	import Item from "./Item.svelte";
	import { isImageOrVideo, isVideoFile, isIgnoredFile } from "./media-utils.js";

	const api = getContext("filemanager-store");
	const _ = getContext("wx-i18n").getGroup("filemanager");

	let { panels, activePanel, mode } = api.getReactiveState();

	const files = $derived($panels[$activePanel]._files);
	const selected = $derived($panels[$activePanel].selected);
	const path = $derived($panels[$activePanel].path);
	const crumbs = $derived($panels[$activePanel]._crumbs);

	const realFiles = $derived((files || []).filter(f => !isIgnoredFile(f)));
	const subfolders = $derived(realFiles.filter(f => f.type === "folder" || f.isDir));
	const mediaFiles = $derived(realFiles.filter(f => isImageOrVideo(f)));

	function click(id, e) {
		if (id === "/wx-filemanager-parent-link") {
			return backToParent();
		}
		if (!id || id === "body") return;

		const target = e && e.target;

		// Check if click was on checkmark circle
		const checkTarget = target && target.closest(".wx-cr-check");
		if (checkTarget) {
			api.exec("select-file", {
				id,
				toggle: true,
				panel: $activePanel,
			});
			return;
		}

		// Check if click was on more actions button ("...")
		const moreTarget = target && (
			target.closest(".wx-more") !== null ||
			(typeof target.className === "string" &&
				(target.className.indexOf("wx-more") !== -1 ||
					target.className.indexOf("wxi-dots-v") !== -1))
		);
		if (moreTarget) {
			return;
		}

		// Keyboard modifier clicks (Ctrl, Cmd, Shift)
		if (e && (e.ctrlKey || e.metaKey || e.shiftKey)) {
			const ctrl = e.ctrlKey || e.metaKey;
			const shift = e.shiftKey;
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
			if (item.type === "folder" || item.isDir) {
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

	function openFolder(folderId) {
		if (!folderId) return;
		api.exec("set-path", {
			id: folderId,
			panel: $activePanel,
		});
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
</script>

{#if $mode === "search" && !mediaFiles.length && !subfolders.length}
	<div class="wx-not-found">
		<div class="wx-not-found-text">{_("Looks like nothing is here")}</div>
	</div>
{:else}
	<!-- svelte-ignore a11y_no_noninteractive_tabindex -->
	<div
		tabindex="0"
		class="wx-cameraroll-container"
		data-id={setID("body")}
		use:delegateClick={{ click, dblclick, context: applySelection }}
	>
		{#if subfolders.length}
			<div class="wx-cr-folders-row">
				{#each subfolders as folder (folder.id)}
					<div
						class="wx-cr-folder-chip"
						data-id={setID(folder.id)}
						title={folder.name || folder.value}
						onclick={(e) => {
							e.stopPropagation();
							openFolder(folder.id);
						}}
					>
						<i class="wxi-folder"></i>
						<span class="wx-cr-folder-name">{folder.name || folder.value}</span>
					</div>
				{/each}
			</div>
		{/if}

		{#if mediaFiles.length}
			<div class="wx-cameraroll-grid">
				{#each mediaFiles as child (child.id)}
					<Item item={child} />
				{/each}
			</div>
		{/if}
	</div>
{/if}

<style>
	.wx-cameraroll-container {
		flex-grow: 1;
		height: 100%;
		overflow-y: auto;
		display: flex;
		flex-direction: column;
		outline: none;
		box-sizing: border-box;
		-webkit-overflow-scrolling: touch;
		/* Uniform cursor on the surface; items/controls set their own pointer
		   (prevents arrow/hand flicker when crossing gaps) */
		cursor: default;
	}

	.wx-cr-folders-row {
		display: flex;
		flex-wrap: wrap;
		gap: 8px;
		padding: 10px 10px 8px;
		flex-shrink: 0;
		background: transparent;
		cursor: default;
	}

	.wx-cr-folder-chip {
		display: inline-flex;
		align-items: center;
		gap: 6px;
		padding: 6px 12px;
		border-radius: 8px;
		background: var(--wx-background, rgba(255, 255, 255, 0.06));
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.12));
		color: var(--wx-color-font, #ECE7F6);
		font-size: 13px;
		font-weight: 500;
		cursor: pointer;
		white-space: nowrap;
		user-select: none;
		transition: background-color 0.15s ease, border-color 0.15s ease, transform 0.1s ease;
	}

	.wx-cr-folder-chip:hover {
		background: var(--wx-background-hover, rgba(255, 255, 255, 0.12));
		border-color: var(--wx-color-primary, #A28AD0);
		transform: translateY(-1px);
	}

	.wx-cr-folder-chip i {
		font-size: 16px;
		color: var(--wx-color-primary, #7D60AC) !important;
	}

	.wx-cameraroll-grid {
		display: grid;
		grid-template-columns: repeat(auto-fill, minmax(175px, 1fr));
		gap: 4px;
		padding: 4px 8px;
		align-content: flex-start;
		box-sizing: border-box;
		width: 100%;
		cursor: default;
	}

	@media (max-width: 600px) {
		.wx-cameraroll-grid {
			grid-template-columns: repeat(3, 1fr);
			gap: 2px;
			padding: 2px;
		}
	}

	@media (min-width: 601px) and (max-width: 960px) {
		.wx-cameraroll-grid {
			grid-template-columns: repeat(auto-fill, minmax(130px, 1fr));
			gap: 3px;
			padding: 3px;
		}
	}

	.wx-not-found {
		display: flex;
		flex-direction: column;
		justify-content: center;
		align-items: center;
		gap: 5px;
		height: 100%;
	}

	.wx-not-found-text {
		text-align: center;
		color: var(--wx-color-font-alt);
	}
</style>
