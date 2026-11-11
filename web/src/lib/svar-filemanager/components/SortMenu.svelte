<script>
	// Sort dropdown shown at the right side of the breadcrumbs bar.
	// Works for all views (Table, Cards, Camera Roll): writes to the store's
	// per-panel `_sorts[path]` state and persists the choice per folder.
	import { getContext } from "svelte";
	import { DropDownMenu } from "@svar-ui/svelte-menu";
	import { getFolderSort, setFolderSort, SORT_DEFAULTS, CAMERAROLL_DEFAULTS } from "../store/sort-prefs.js";

	let { panel, isCameraRoll = false } = $props();

	const api = getContext("filemanager-store");
	const _ = getContext("wx-i18n").getGroup("filemanager");

	const { panels } = api.getReactiveState();

	const path = $derived($panels[panel].path);
	const defaults = $derived(isCameraRoll ? CAMERAROLL_DEFAULTS : SORT_DEFAULTS);
	const current = $derived(getFolderSort(path, defaults));

	const FIELD_LABELS = {
		name: () => _("Name"),
		size: () => _("Size"),
		date: () => _("Date"),
	};

	const COMBO_LABELS = {
		"name-asc": () => _("Name (A-Z)"),
		"name-desc": () => _("Name (Z-A)"),
		"date-asc": () => _("Date (oldest first)"),
		"date-desc": () => _("Date (newest first)"),
		"size-asc": () => _("Size (smallest first)"),
		"size-desc": () => _("Size (largest first)"),
	};

	function fieldLabel(key) {
		const f = FIELD_LABELS[key];
		return f ? f() : key;
	}

	function comboLabel(key, order) {
		const f = COMBO_LABELS[`${key}-${order}`];
		return f ? f() : `${fieldLabel(key)} — ${orderLabel(order)}`;
	}

	function orderLabel(order) {
		return order === "desc" ? _("Descending") : _("Ascending");
	}

	// Menu options: 3 fields, each toggling asc/desc; a checkmark marks the
	// active field. Clicking the active field flips its order.
	function sortOption(key) {
		const isActive = current.key === key;
		const order = isActive ? (current.order === "asc" ? "desc" : "asc") : "asc";
		return {
			id: `sort-${key}-${order}`,
			text: comboLabel(key, order),
			icon: isActive ? "wxi-check" : "",
			handler: () => apply(key, order),
		};
	}

	let options = $derived.by(() => {
		// touch `current` so options rebuild when the sort changes
		void current;
		void path;
		return [sortOption("name"), sortOption("date"), sortOption("size")];
	});

	function apply(key, order) {
		setFolderSort(path, key, order);
		api.exec("sort-files", { key, order, panel, path });
	}

	// Apply the stored per-folder sort whenever the folder changes.
	// When the folder has no custom choice, fall back to the view defaults
	// (Camera Roll folders default to newest-first, others to name asc).
	let lastAppliedPath = null;
	$effect(() => {
		const p = path;
		if (p === lastAppliedPath) return;
		lastAppliedPath = p;
		const pref = getFolderSort(p, defaults);
		if (pref.custom || isCameraRoll) {
			api.exec("sort-files", { key: pref.key, order: pref.order, panel, path: p });
		}
	});
</script>

<div class="wx-sort-menu" data-menu-ignore="true">
	<DropDownMenu {options} at="bottom-end" css="wx-sort-dropdown">
		<button
			type="button"
			class="wx-sort-btn"
			title={`${_("Sort by")}: ${comboLabel(current.key, current.order)}`}
			aria-label={`${_("Sort by")}: ${comboLabel(current.key, current.order)}`}
		>
			<i class="wxi-sort"></i>
			<span class="wx-sort-label">{fieldLabel(current.key)}</span>
			<i class="wxi-angle-{current.order === 'asc' ? 'up' : 'down'} wx-sort-dir"></i>
		</button>
	</DropDownMenu>
</div>

<style>
	.wx-sort-menu {
		display: flex;
		align-items: center;
		flex-shrink: 0;
		margin-left: 8px;
	}

	.wx-sort-btn {
		display: inline-flex;
		align-items: center;
		gap: 4px;
		height: 32px;
		padding: 0 10px;
		border-radius: 6px;
		background: transparent;
		border: none;
		color: var(--wx-color-font-alt, #7E869B);
		font-family: var(--wx-font-family);
		font-size: 13px;
		font-weight: 500;
		cursor: pointer;
		white-space: nowrap;
		transition: background-color 0.15s ease, color 0.15s ease;
	}

	.wx-sort-btn:hover {
		background: var(--wx-background-alt, rgba(255, 255, 255, 0.08));
		color: var(--wx-color-primary, #929CD4);
	}

	.wx-sort-btn .wxi-sort {
		font-size: 16px;
		line-height: 1;
	}

	.wx-sort-dir {
		font-size: 14px;
		line-height: 1;
	}

	.wx-sort-label {
		max-width: 90px;
		overflow: hidden;
		text-overflow: ellipsis;
	}

	@media (max-width: 600px) {
		.wx-sort-label,
		.wx-sort-dir {
			display: none;
		}
		.wx-sort-btn {
			padding: 0 8px;
		}
	}
</style>
