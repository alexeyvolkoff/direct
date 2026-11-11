<script>
	import { getContext } from "svelte";

	import Breadcrumbs from "../Breadcrumbs.svelte";
	import Panel from "./Panel.svelte";
	import UploadDropArea from "../UploadDropArea.svelte";

	let { isCameraRoll = false } = $props();

	const api = getContext("filemanager-store");
	const { activePanel } = api.getReactiveState();
</script>

<div class="wx-wrapper wx-cameraroll-wrapper">
	<Breadcrumbs panel={$activePanel} isCameraRoll={isCameraRoll} />
	<UploadDropArea>
		<Panel />
	</UploadDropArea>
</div>

<style>
	.wx-cameraroll-wrapper {
		display: flex;
		flex-direction: column;
		height: 100%;
		max-height: 100%;
		max-width: 100%;
		flex-shrink: 1;
		min-height: 0;
	}

	.wx-cameraroll-wrapper > :global(.wx-upload-area) {
		flex: 1 1 0;
		min-height: 0;
		height: calc(100% - 48px);
		overflow: hidden;
		border: 1px solid transparent;
		border-radius: 0 0 6px 6px;
	}
	.wx-cameraroll-wrapper > :global(.wx-upload-area.wx-active) {
		border: 1px solid var(--wx-color-primary, #5863A6);
	}
</style>
