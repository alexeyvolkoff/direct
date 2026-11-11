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

		--wx-fm-background: var(--bg-primary, #0d0b12);
		--wx-fm-box-shadow: none;
		--wx-fm-select-background: var(--wx-color-primary-selected, rgba(168, 85, 247, 0.22));
		--wx-fm-grid-border: 1px solid var(--border-color, #2d2640);
		--wx-fm-grid-header-color: var(--wx-background);
		--wx-fm-button-font-color: #a99ec2;
		--wx-fm-toolbar-height: 56px;
	}
</style>
