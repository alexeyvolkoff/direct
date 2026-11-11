<script>
	import { getContext, onMount, onDestroy } from "svelte";

	import { Button } from "@svar-ui/svelte-core";
	import { DropDownMenu, registerMenuItem } from "@svar-ui/svelte-menu";

	import Tree from "./Tree/Tree.svelte";
	import Drive from "./Drive.svelte";
	import UploadButton from "./UploadButton.svelte";
	import { appVersion, otaState, applyOtaUpdateNow } from "../../ota-updater.js";
	import { translate } from "../../i18n/index.js";

	let { readonly, menuOptions, onnavigate = null } = $props();

	const _ = getContext("wx-i18n").getGroup("filemanager");
	const { showPrompt } = getContext("filemanager-modals");

	registerMenuItem("upload", UploadButton);

	let profile = $state(typeof window !== "undefined" ? window.__omdProfile : null);
	let imgFailed = $state(false);
	let curVersion = $state(
		(typeof import.meta !== "undefined" && import.meta.env && import.meta.env.DEV)
			? ((import.meta.env && import.meta.env.VITE_APP_VERSION) || "3.0.0")
			: (typeof window !== "undefined" && window.__omdAppVersion && window.__omdAppVersion !== "{VERSION}")
				? window.__omdAppVersion
				: ((typeof import.meta !== "undefined" && import.meta.env && import.meta.env.VITE_APP_VERSION) || "3.0.0")
	);
	let ota = $state({ updateAvailable: false, isReady: false, newVersion: "", isDownloading: false });

	let unsubVersion = null;
	let unsubOta = null;

	function onProfileUpdate(e) {
		profile = e.detail;
		imgFailed = false;
	}

	onMount(() => {
		if (typeof window !== "undefined") {
			profile = window.__omdProfile;
			window.addEventListener("omd-profile-update", onProfileUpdate);
			if (import.meta.env && import.meta.env.DEV) {
				curVersion = (import.meta.env && import.meta.env.VITE_APP_VERSION) || "3.0.0";
			} else if (window.__omdAppVersion && window.__omdAppVersion !== "{VERSION}") {
				curVersion = window.__omdAppVersion;
			}
			unsubVersion = appVersion.subscribe(v => {
				if (v && v !== "1.0.4553" && v !== "1.0.0" && !(import.meta.env && import.meta.env.DEV)) {
					curVersion = v;
				}
			});
			unsubOta = otaState.subscribe(s => { if (s) ota = s; });
		}
	});

	onDestroy(() => {
		if (typeof window !== "undefined") {
			window.removeEventListener("omd-profile-update", onProfileUpdate);
			if (unsubVersion) unsubVersion();
			if (unsubOta) unsubOta();
		}
	});

	const api = getContext("filemanager-store");
	const { panels, activePanel } = api.getReactiveState();
	const currentPath = $derived($panels ? $panels[$activePanel].path : "/");

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
			} else if (action.id === "add-file")
				showPrompt({
					item: {
						type: "file",
						size: 0,
						date: new Date(),
					},
					parent: currentPath,
					add: true,
				});
			else if (action.id === "add-folder")
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

	let options = $derived(
		menuOptions("add", { path: currentPath }).map(option => {
			option.text = _(option.text);
			return option;
		})
	);

	function fireAddAction() {
		const opt = options.find(o => typeof o.handler === "function");
		if (opt) {
			opt.handler({ action: opt, path: currentPath });
		} else if (options.length) {
			handleClick({ action: options[0] });
		}
	}

	function openSettings(section = "profile") {
		if (onnavigate) onnavigate();
		if (typeof window !== "undefined") {
			if (window.__omdOpenSettings) {
				window.__omdOpenSettings(section);
			} else {
				window.location.hash = "#/settings/" + section;
				window.dispatchEvent(new CustomEvent("omd-open-settings", { detail: { section } }));
			}
		}
	}

	function openApplet(view = "chat") {
		if (onnavigate) onnavigate();
		if (typeof window !== "undefined") {
			if (window.__omdOpenApplet) {
				window.__omdOpenApplet(view);
			} else {
				const hash = view === "creations" ? "#/creations" : view === "memory" ? "#/knowledge" : "#/chat";
				window.location.hash = hash;
				window.dispatchEvent(new CustomEvent("omd-open-applet", { detail: { view } }));
			}
		}
	}
	const hasSingleAction = $derived(options.length === 1 && typeof options[0].handler === "function");
	const isDeviceRoot = $derived(
		typeof currentPath === "string" && currentPath !== "/" && !currentPath.slice(1).includes("/") && !currentPath.startsWith("/shared-")
	);
	const sidebarButtonTitle = $derived(
		isDeviceRoot
			? (_("Add folder") || (options[0]?.text ?? _("Add New")))
			: (hasSingleAction && options[0]?.text ? options[0].text : _("Add New"))
	);
</script>

<div class="wx-wrapper">
	{#if !readonly && options.length > 0}
		<div class="wx-button">
		{#key currentPath}
			<button type="button" class="wx-button wx-primary wx-block" style="cursor: pointer;" title={sidebarButtonTitle} onclick={fireAddAction}>{sidebarButtonTitle}</button>
		{/key}
		</div>
	{/if}
	<div class="wx-tree">
		<Tree />
	</div>
	<Drive />
	<div class="wx-sidebar-nav-wrap">
		{#if !readonly}
			<button
				type="button"
				class="wx-sidebar-nav-btn"
				title={translate("generated", {}, "Creations")}
				onclick={() => openApplet("creations")}
			>
				<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
					<rect x="3" y="3" width="18" height="18" rx="2" ry="2" />
					<circle cx="8.5" cy="8.5" r="1.5" />
					<polyline points="21 15 16 10 5 21" />
				</svg>
				<span>{translate("generated", {}, "Creations")}</span>
			</button>
			<button
				type="button"
				class="wx-sidebar-nav-btn"
				title={translate("knowledge", {}, "Knowledge")}
				onclick={() => openApplet("memory")}
			>
				<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
					<path d="M2 3h6a4 4 0 0 1 4 4v14a3 3 0 0 0-3-3H2z" />
					<path d="M22 3h-6a4 4 0 0 0-4 4v14a3 3 0 0 1 3-3h7z" />
				</svg>
				<span>{translate("knowledge", {}, "Knowledge")}</span>
			</button>
		{/if}
		<button
			type="button"
			class="wx-sidebar-nav-btn"
			title={translate("settings", {}, "Settings")}
			onclick={() => openSettings("devices")}
		>
			<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
				<circle cx="12" cy="12" r="3" />
				<path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z" />
			</svg>
			<span>{translate("settings", {}, "Settings")}</span>
			{#if (!import.meta.env || !import.meta.env.DEV) && ota.updateAvailable && ota.isReady}
				<span class="wx-sidebar-update-badge" title={translate("update_ready_reload", {}, "Update downloaded. Click to reload.")}>
					{translate("update_badge", { version: ota.newVersion }, `v${ota.newVersion} ⚡`)}
				</span>
			{:else if (!import.meta.env || !import.meta.env.DEV) && ota.isDownloading}
				<span class="wx-sidebar-downloading-badge">{translate("updating", {}, "Updating...")}</span>
			{/if}
		</button>
	</div>
	{#if profile && profile.authorized}
		<div class="wx-profile-card">
			<button
				class="wx-profile-btn"
				type="button"
				title={profile.isEphemeral ? "Guest Session - Open Settings" : "Open Settings"}
				onclick={() => openSettings("profile")}
			>
				<div class="wx-profile-avatar-wrap">
					<div class="wx-profile-avatar {profile.isEphemeral ? 'ephemeral' : ''}">
						{#if profile.image && !imgFailed}
							<img
								src={profile.image}
								alt={profile.name || "Avatar"}
								referrerpolicy="no-referrer"
								onerror={() => {
									imgFailed = true;
								}}
							/>
						{:else if profile.isEphemeral}
							<svg viewBox="0 0 16 16" width="18" height="18" fill="currentColor"><path d="M8 8a3 3 0 1 0 0-6 3 3 0 0 0 0 6m2-3a2 2 0 1 1-4 0 2 2 0 0 1 4 0m4 8c0 1-1 1-1 1H3s-1 0-1-1 1-4 6-4 6 3 6 4m-1-.004c-.001-.246-.154-.986-.832-1.664C11.516 10.68 10.289 10 8 10s-3.516.68-4.168 1.332c-.678.678-.83 1.418-.832 1.664z"/></svg>
						{:else}
							<span class="wx-avatar-text">{(profile.name || profile.user || "U").charAt(0).toUpperCase()}</span>
						{/if}
					</div>
					<span class="wx-status-indicator {profile.isEphemeral ? 'ephemeral' : 'online'}"></span>
				</div>
				<div class="wx-profile-info">
					<span class="wx-profile-name">{profile.name}</span>
					<span class="wx-profile-sub">{profile.isEphemeral ? 'Direct P2P Link' : (profile.user ? ('@' + profile.user) : (profile.email || (profile.did ? profile.did.slice(0, 12) + '...' : '')))}</span>
				</div>
			</button>
			<button
				class="wx-signout-btn"
				type="button"
				title={profile.isEphemeral ? "Disconnect session" : "Sign out"}
				onclick={() => window.__omdLogout && window.__omdLogout()}
			>
				<svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor"><path fill-rule="evenodd" d="M6 8a.5.5 0 0 0 .5.5h5.793l-2.147 2.146a.5.5 0 0 0 .708.708l3-3a.5.5 0 0 0 0-.708l-3-3a.5.5 0 0 0-.708.708L12.293 7.5H6.5A.5.5 0 0 0 6 8m-2.5 7a.5.5 0 0 1-.5-.5v-13a.5.5 0 0 1 1 0v13a.5.5 0 0 1-.5.5"/></svg>
			</button>
		</div>
	{/if}
</div>

<style>
	.wx-wrapper {
		display: flex;
		flex-direction: column;
		height: 100%;
		background-color: var(--bg-secondary, var(--wx-background));
		border-radius: 0;
		box-shadow: none;
	}

	.wx-button {
		padding: 10px 8px 0;
	}

	.wx-tree {
		padding-top: 8px;
		flex-grow: 1;
		overflow-y: auto;
		overflow-x: hidden;
		scrollbar-width: thin;
		scrollbar-color: transparent transparent;
		transition: scrollbar-color 0.2s ease;
	}

	.wx-tree::-webkit-scrollbar {
		width: 6px;
		height: 6px;
	}

	.wx-tree::-webkit-scrollbar-track {
		background: transparent;
	}

	.wx-tree::-webkit-scrollbar-thumb {
		background: transparent;
		border-radius: 3px;
		transition: background-color 0.2s ease;
	}

	.wx-wrapper:hover .wx-tree,
	.wx-wrapper:focus-within .wx-tree {
		scrollbar-color: var(--scrollbar-thumb, rgba(84, 107, 117, 0.45)) transparent;
	}

	.wx-wrapper:hover .wx-tree::-webkit-scrollbar-thumb,
	.wx-wrapper:focus-within .wx-tree::-webkit-scrollbar-thumb {
		background: var(--scrollbar-thumb, rgba(84, 107, 117, 0.45));
	}

	.wx-wrapper:hover .wx-tree::-webkit-scrollbar-thumb:hover,
	.wx-wrapper:focus-within .wx-tree::-webkit-scrollbar-thumb:hover {
		background: var(--scrollbar-thumb-hover, rgba(84, 107, 117, 0.75));
	}

	.wx-profile-card {
		display: flex;
		align-items: center;
		justify-content: space-between;
		padding: 8px 10px;
		margin: 4px 8px 8px;
		background-color: var(--wx-background-alt);
		border-radius: 8px;
		border: 1px solid var(--wx-border);
		gap: 6px;
		flex-shrink: 0;
	}

	.wx-profile-btn {
		display: flex;
		align-items: center;
		gap: 10px;
		background: transparent;
		border: none;
		padding: 2px 4px;
		border-radius: 6px;
		cursor: pointer;
		text-align: left;
		flex-grow: 1;
		min-width: 0;
		transition: background-color 0.15s ease;
	}

	.wx-profile-btn:hover {
		background-color: rgba(125, 133, 150, 0.12);
	}

	.wx-profile-avatar-wrap {
		position: relative;
		width: 32px;
		height: 32px;
		flex-shrink: 0;
	}

	.wx-profile-avatar {
		width: 32px;
		height: 32px;
		border-radius: 50%;
		background: linear-gradient(135deg, #5863A6, #7B86C4);
		display: flex;
		align-items: center;
		justify-content: center;
		color: #ffffff;
		font-weight: 700;
		font-size: 13px;
		overflow: hidden;
	}

	.wx-profile-avatar img {
		width: 100%;
		height: 100%;
		border-radius: 50%;
		object-fit: cover;
		display: block;
	}

	.wx-profile-avatar.ephemeral {
		background: linear-gradient(135deg, #5A6278, #32394B);
		color: #DCE0F5;
	}

	.wx-avatar-text {
		font-size: 14px;
		font-weight: 700;
		color: #ffffff;
		line-height: 1;
		user-select: none;
	}

	.wx-status-indicator {
		position: absolute;
		bottom: 0;
		right: 0;
		width: 8px;
		height: 8px;
		background-color: #10b981;
		border: 1.5px solid var(--wx-background, #ffffff);
		border-radius: 50%;
		z-index: 2;
	}

	.wx-status-indicator.ephemeral {
		background-color: #f59e0b;
	}

	.wx-profile-info {
		display: flex;
		flex-direction: column;
		min-width: 0;
		overflow: hidden;
	}

	.wx-profile-name {
		font-size: 13px;
		font-weight: 600;
		color: var(--wx-color-font, #161925);
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
		line-height: 1.2;
	}

	.wx-profile-sub {
		font-size: 11px;
		color: var(--wx-color-font-alt, #5A6278);
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
		line-height: 1.2;
		margin-top: 2px;
	}

	.wx-signout-btn {
		display: flex;
		align-items: center;
		justify-content: center;
		background: transparent;
		border: none;
		color: var(--wx-color-font-alt, #5A6278);
		padding: 6px;
		border-radius: 6px;
		cursor: pointer;
		flex-shrink: 0;
		transition: all 0.15s ease;
	}

	.wx-signout-btn:hover {
		color: #ef4444;
		background-color: rgba(239, 68, 68, 0.1);
	}

	.wx-sidebar-nav-wrap {
		padding: 6px 8px 0;
		margin-top: 4px;
		border-top: 1px solid var(--wx-border, rgba(255, 255, 255, 0.06));
		flex-shrink: 0;
	}

	.wx-sidebar-nav-btn {
		display: flex;
		align-items: center;
		gap: 10px;
		width: 100%;
		background: transparent;
		border: none;
		padding: 8px 10px;
		border-radius: 8px;
		color: var(--text-secondary, var(--wx-color-font-alt, #5A6278));
		font-size: 0.88rem;
		font-weight: 500;
		cursor: pointer;
		transition: all 0.15s ease;
		text-align: left;
		box-sizing: border-box;
	}

	.wx-sidebar-nav-btn:hover {
		background: var(--bg-card-hover, rgba(125, 133, 150, 0.12));
		color: var(--text-primary, var(--wx-color-font, #161925));
	}

	.wx-sidebar-nav-btn svg {
		flex-shrink: 0;
	}

	.wx-sidebar-update-badge {
		margin-left: auto;
		background: linear-gradient(135deg, #5863A6, #3D467A);
		color: #ffffff;
		border: none;
		border-radius: 9999px;
		font-size: 10px;
		font-weight: 600;
		padding: 2px 7px;
		cursor: pointer;
		display: inline-flex;
		align-items: center;
		gap: 3px;
		box-shadow: 0 1px 4px rgba(88, 99, 166, 0.35);
		transition: all 0.15s ease;
		animation: pulseUpdate 2s infinite ease-in-out;
	}

	.wx-sidebar-update-badge:hover {
		transform: scale(1.05);
		box-shadow: 0 2px 8px rgba(88, 99, 166, 0.5);
	}

	.wx-sidebar-downloading-badge {
		margin-left: auto;
		font-size: 10px;
		color: #929CD4;
		font-style: italic;
	}

	@keyframes pulseUpdate {
		0%, 100% { opacity: 1; transform: scale(1); }
		50% { opacity: 0.85; transform: scale(0.97); }
	}
	
	:global(.wx-tree i[class^="wxi-"]:before),
	:global(.wx-tree i[class*=" wxi-"]:before) {
		margin-top: -4px !important;
	}
	</style>
