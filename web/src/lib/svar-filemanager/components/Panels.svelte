<script>
	import TableView from "./Table/View.svelte";
	import { getContext } from "svelte";
	import { setID } from "@svar-ui/lib-dom";

	const api = getContext("filemanager-store");
	const { activePanel, mode, panels } = api.getReactiveState();

	function toggleActive(panel) {
		api.exec("set-active-panel", { panel });
	}

	function handleKeydown(e) {
		if ($mode !== "panels") return;
		const key = e.key;
		const lower = key.toLowerCase();

		// F5 = Copy, F6 = Move (classic twin-panel)
		if (key === "F5" || key === "F6") {
			e.preventDefault();
			const active = $activePanel;
			const sel = $panels[active]?.selected || [];
			if (!sel.length) return;
			const target = $panels[active === 0 ? 1 : 0]?.path || "/";
			const action = key === "F5" ? "copy-files" : "move-files";
			api.exec(action, { ids: [...sel], target });
			return;
		}
		// Ctrl/Cmd+A = select all in active panel
		if ((e.ctrlKey || e.metaKey) && lower === "a") {
			const active = $activePanel;
			const files = $panels[active]?._files || [];
			if (!files.length) return;
			e.preventDefault();
			api.exec("select-file", { panel: active });
			for (const f of files) {
				api.exec("select-file", { id: f.id, toggle: true, panel: active });
			}
		}
	}
</script>

<svelte:window onkeydown={handleKeydown} />

<div class="wx-panels" data-id={setID("body")}>
	<div class="wx-item" data-panel={0}>
		<TableView
			panel={0}
			isPanels={true}
			active={$activePanel == 0}
			onclick={() => toggleActive(0)}
			oncontextmenu={() => toggleActive(0)}
		/>
	</div>

	<div class="wx-item" data-panel={1}>
		<TableView
			panel={1}
			isPanels={true}
			active={$activePanel == 1}
			onclick={() => toggleActive(1)}
			oncontextmenu={() => toggleActive(1)}
		/>
	</div>
</div>

<style>
	.wx-panels {
		display: flex;
		width: 100%;
		max-width: 100%;
		height: 100%;
		align-items: stretch;
		gap: 10px;
	}
	.wx-item {
		flex: 1 1 0;
		min-width: 0;
	}
</style>
