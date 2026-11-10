<script>
	import { getContext } from "svelte";

	import { delegateClick } from "@svar-ui/lib-dom";
	import Folder from "./Folder.svelte";
	import { getSelectionOnNavigation } from "../../store/index.js";

	const api = getContext("filemanager-store");
	const { data, panels, activePanel } = api.getReactiveState();

	const crumbs = $derived($panels[$activePanel]._crumbs);

	function toggle(id) {
		api.exec("open-tree-folder", {
			id,
			mode: !$data.byId(id).open,
		});
	}

	function click(id) {
		const selectedId = getSelectionOnNavigation(id, crumbs);
		api.exec("set-path", {
			id,
			panel: $activePanel,
			...(selectedId && { selected: [selectedId] }),
		});
	}

	let folders = $state([]);
	$effect(() => {
		const root = $data.byId("/");
		const raw = root?.data?.length ? root.data : $data.byId(0)?.data || [];
		const seen = new Set();
		folders = raw.filter(item => {
			if (!item || !item.id) return false;
			if (seen.has(item.id)) return false;
			seen.add(item.id);
			return true;
		});
	});
</script>

<ul use:delegateClick={{ click, toggle }}>
	{#each folders as folder (folder ? folder.id : Math.random())}
		{#if folder && (folder.type === "folder" || folder.isDir || folder.isDevice || folder.id === "/chats")}
			<Folder {folder} />
		{/if}
	{/each}
</ul>

<style>
	ul {
		padding: 0;
		margin: 0;
		height: 100%;
		min-width: 100%;
		width: fit-content;
	}
</style>
