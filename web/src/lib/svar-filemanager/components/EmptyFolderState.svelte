<script>
	import { getContext } from "svelte";
	import { apiKey } from "@svar-ui/svelte-uploader";
	import { _ } from "../../i18n/index.js";

	let { path, currentDevice, isDeviceRoot = false, isLocalWebDevice = false } = $props();

	const api = getContext("filemanager-store");
	const menuOptions = getContext("filemanager-menu-options");
	const { showPrompt } = getContext("filemanager-modals") || {};
	const uploaderApi = getContext(apiKey);

	function fireDeviceRootAction() {
		if (menuOptions) {
			const opts = menuOptions("add", { path });
			const opt = opts?.find(o => typeof o.handler === "function");
			if (opt) {
				opt.handler({ action: opt, path });
				return;
			}
		}
		if (isLocalWebDevice) {
			api.exec("add-local-folder", {});
		}
	}

	function handleAddFolder() {
		if (showPrompt) {
			showPrompt({
				item: {
					type: "folder",
					date: new Date(),
					lazy: true,
				},
				parent: path,
				add: true,
			});
		} else {
			api.exec("create-file", {
				file: { name: $_("new_folder", {}, "New folder"), type: "folder" },
				parent: path,
			});
		}
	}

	function handleAddFile() {
		if (showPrompt) {
			showPrompt({
				item: {
					type: "file",
					size: 0,
					date: new Date(),
				},
				parent: path,
				add: true,
			});
		} else {
			api.exec("create-file", {
				file: { name: $_("new_file", {}, "New file"), type: "file" },
				parent: path,
			});
		}
	}

	function handleUpload() {
		const uploadInput = document.querySelector(
			".wx-sidebar input[type=file], .wx-filemanager input[type=file]"
		);
		if (uploadInput) {
			uploadInput.click();
		} else if (uploaderApi && typeof uploaderApi.open === "function") {
			uploaderApi.open({});
		} else {
			api.exec("upload-file");
		}
	}
</script>

<div class="wx-empty-folder-container">
	<div class="wx-empty-folder-card">
		{#if path === "/shared-to-me"}
			<div class="wx-empty-folder-glow">
				<svg viewBox="0 0 24 24" width="40" height="40" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
					<circle cx="6" cy="8" r="2" />
					<circle cx="16" cy="16" r="2" />
					<path d="M12 12v-2a2 2 0 0 0-2-2H4a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h8a2 2 0 0 0 2-2v-2" />
					<path d="M22 12a10 10 0 0 0-8-8" />
				</svg>
			</div>
			<h2 class="wx-empty-title">{$_("shared_to_me_empty", {}, "Nothing shared with you yet")}</h2>
			<p class="wx-empty-desc">
				{$_("shared_to_me_empty_desc", {}, "Paste a share link to access files from other devices.")}
			</p>
			<div class="wx-empty-actions">
				<button type="button" class="wx-empty-btn wx-primary-btn" onclick={fireDeviceRootAction}>
					<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
						<path d="M16 3h5v5" />
						<path d="M8 3H3v18h18v-8" />
						<path d="M21 3 10 14" />
					</svg>
					<span>{$_("paste_share_link", {}, "Paste link")}</span>
				</button>
			</div>
		{:else if path === "/shared-by-me"}
			<div class="wx-empty-folder-glow">
				<svg viewBox="0 0 24 24" width="40" height="40" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
					<circle cx="18" cy="5" r="3" />
					<circle cx="6" cy="12" r="3" />
					<circle cx="18" cy="19" r="3" />
					<line x1="8.59" y1="13.51" x2="15.42" y2="17.49" />
					<line x1="15.41" y1="6.51" x2="8.59" y2="10.49" />
				</svg>
			</div>
			<h2 class="wx-empty-title">{$_("shared_by_me_empty", {}, "Nothing shared by you yet")}</h2>
			<p class="wx-empty-desc">
				{$_("shared_by_me_empty_desc", {}, "Links to files and folders you share will appear here.")}
			</p>
		{:else if isDeviceRoot}
			<div class="wx-empty-folder-glow">
				<svg viewBox="0 0 24 24" width="40" height="40" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
					<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
					<line x1="12" y1="11" x2="12" y2="17" />
					<line x1="9" y1="14" x2="15" y2="14" />
				</svg>
			</div>
			<h2 class="wx-empty-title">{$_("device_empty_title", {}, "No shared folders yet")}</h2>
			<p class="wx-empty-desc">
				{$_("device_empty_desc", {}, "Connect or share a folder on this device to access and synchronize files.")}
			</p>
			<div class="wx-empty-actions">
				<button type="button" class="wx-empty-btn wx-primary-btn" onclick={fireDeviceRootAction}>
					<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
						<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
						<line x1="12" y1="11" x2="12" y2="17" />
						<line x1="9" y1="14" x2="15" y2="14" />
					</svg>
					<span>{$_("add_folder", {}, "Add folder")}</span>
				</button>
			</div>
		{:else}
			<div class="wx-empty-folder-glow">
				<svg viewBox="0 0 24 24" width="40" height="40" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
					<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
				</svg>
			</div>
			<h2 class="wx-empty-title">{$_("folder_empty", {}, "This folder is empty")}</h2>
			<p class="wx-empty-desc">
				{$_("folder_empty_desc", {}, "Create a new folder, file, or upload files from your device.")}
			</p>
			<div class="wx-empty-actions">
				<button type="button" class="wx-empty-btn wx-primary-btn" onclick={handleAddFolder}>
					<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
						<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
						<line x1="12" y1="11" x2="12" y2="17" />
						<line x1="9" y1="14" x2="15" y2="14" />
					</svg>
					<span>{$_("add_folder", {}, "Add folder")}</span>
				</button>

				<button type="button" class="wx-empty-btn wx-secondary-btn" onclick={handleAddFile}>
					<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
						<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" />
						<polyline points="14 2 14 8 20 8" />
						<line x1="12" y1="18" x2="12" y2="12" />
						<line x1="9" y1="15" x2="15" y2="15" />
					</svg>
					<span>{$_("add_file", {}, "Add file")}</span>
				</button>

				<button type="button" class="wx-empty-btn wx-secondary-btn" onclick={handleUpload}>
					<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
						<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
						<polyline points="17 8 12 3 7 8" />
						<line x1="12" y1="3" x2="12" y2="15" />
					</svg>
					<span>{$_("upload", {}, "Upload file")}</span>
				</button>
			</div>
		{/if}
	</div>
</div>

<style>
	.wx-empty-folder-container {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 100%;
		height: 100%;
		min-height: 360px;
		padding: 32px 16px;
		box-sizing: border-box;
	}

	.wx-empty-folder-card {
		max-width: 440px;
		width: 100%;
		padding: 36px 28px;
		border-radius: 18px;
		background: var(--wx-background-alt, rgba(127, 127, 127, 0.04));
		border: 1px solid var(--wx-border-color, rgba(127, 127, 127, 0.16));
		box-shadow: 0 10px 30px -10px rgba(0, 0, 0, 0.08);
		display: flex;
		flex-direction: column;
		align-items: center;
		text-align: center;
		box-sizing: border-box;
		animation: wx-empty-fade-in 0.25s cubic-bezier(0.16, 1, 0.3, 1);
	}

	@keyframes wx-empty-fade-in {
		from {
			opacity: 0;
			transform: translateY(6px);
		}
		to {
			opacity: 1;
			transform: translateY(0);
		}
	}

	.wx-empty-folder-glow {
		width: 76px;
		height: 76px;
		border-radius: 50%;
		background: radial-gradient(circle, rgba(142, 116, 189, 0.22) 0%, rgba(142, 116, 189, 0.03) 70%);
		display: flex;
		align-items: center;
		justify-content: center;
		color: var(--wx-color-primary, #563F7A);
		margin-bottom: 18px;
		box-shadow: 0 0 24px rgba(142, 116, 189, 0.12);
	}

	.wx-empty-title {
		font-size: 19px;
		font-weight: 600;
		letter-spacing: -0.01em;
		margin: 0 0 8px;
		color: var(--wx-color-font, inherit);
	}

	.wx-empty-desc {
		font-size: 13.5px;
		line-height: 1.5;
		color: var(--wx-color-font-alt, #73688A);
		margin: 0 0 24px;
		max-width: 360px;
	}

	.wx-empty-actions {
		display: flex;
		flex-wrap: wrap;
		gap: 10px;
		justify-content: center;
		align-items: center;
		width: 100%;
	}

	.wx-empty-btn {
		display: inline-flex;
		align-items: center;
		gap: 8px;
		padding: 9px 18px;
		border-radius: 10px;
		font-size: 13.5px;
		font-weight: 500;
		cursor: pointer;
		font-family: inherit;
		transition: all 0.2s cubic-bezier(0.4, 0, 0.2, 1);
		user-select: none;
		white-space: nowrap;
	}

	.wx-primary-btn {
		background: var(--wx-color-primary, #563F7A);
		color: #ffffff;
		border: 1px solid transparent;
		box-shadow: 0 4px 14px rgba(142, 116, 189, 0.25);
	}

	.wx-primary-btn:hover {
		transform: translateY(-1px);
		filter: brightness(1.08);
		box-shadow: 0 6px 18px rgba(142, 116, 189, 0.35);
	}

	.wx-secondary-btn {
		background: var(--wx-background, #ffffff);
		border: 1px solid var(--wx-border-color, rgba(127, 127, 127, 0.25));
		color: var(--wx-color-font, inherit);
		box-shadow: 0 2px 6px rgba(0, 0, 0, 0.03);
	}

	.wx-secondary-btn:hover {
		background: var(--wx-background-alt, rgba(127, 127, 127, 0.08));
		border-color: var(--wx-color-primary, #563F7A);
		color: var(--wx-color-primary, #563F7A);
		transform: translateY(-1px);
	}

	@media (max-width: 540px) {
		.wx-empty-folder-card {
			padding: 24px 16px;
		}

		.wx-empty-actions {
			flex-direction: column;
		}

		.wx-empty-btn {
			width: 100%;
			justify-content: center;
		}
	}
</style>
