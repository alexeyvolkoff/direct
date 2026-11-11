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

		--wx-fm-background: var(--bg-primary, #151821);
		--wx-fm-box-shadow: none;
		--wx-fm-select-background: var(--wx-color-primary-selected, rgba(123, 134, 196, 0.18));
		--wx-fm-grid-border: 1px solid var(--border-color, #32394b);
		--wx-fm-grid-header-color: var(--wx-background);
		--wx-fm-button-font-color: #aeb5c7;
		--wx-fm-toolbar-height: 56px;
	}
</style>
