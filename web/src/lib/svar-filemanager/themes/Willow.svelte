<script>
	import { Willow } from "@svar-ui/svelte-core";
	let { fonts = true, children } = $props();
</script>

{#if children}
	<Willow {fonts}>{@render children()}</Willow>
{:else}
	<Willow {fonts} />
{/if}

<style>
	:global(.wx-willow-theme) {
		--wx-theme-name: willow;

		--wx-fm-background: var(--bg-primary, #f8f7fc);
		--wx-fm-box-shadow: 0px 1px 2px rgba(24, 19, 38, 0.06),
			0px 3px 10px rgba(24, 19, 38, 0.10);
		--wx-fm-select-background: var(--wx-color-primary-selected, #f3e8ff);
		--wx-fm-grid-border: 1px solid var(--border-color, #ddd6eb);
		--wx-fm-grid-header-color: #f8f7fc;
		--wx-fm-button-font-color: #8b80a3;
		--wx-fm-toolbar-height: 56px;
	}
</style>
