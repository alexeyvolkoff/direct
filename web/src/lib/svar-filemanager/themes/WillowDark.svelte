<script>
	import { WillowDark } from "@svar-ui/svelte-core";
	let { fonts = true, children } = $props();
</script>

{#if children}
	<WillowDark {fonts}>{@render children()}</WillowDark>
{:else}
	<WillowDark {fonts} />
{/if}

<style>
	:global(.wx-willow-dark-theme) {
		--wx-theme-name: willow-dark;
		color-scheme: dark;

		--wx-fm-background: var(--bg-primary, #101c22);
		--wx-fm-box-shadow: none;
		--wx-fm-select-background: rgba(41, 150, 204, 0.25);
		--wx-fm-grid-border: 1px solid #3d4f57;
		--wx-fm-grid-header-color: var(--wx-background);
		--wx-fm-button-font-color: #9fa1ae;
		--wx-fm-toolbar-height: 56px;
	}
</style>
