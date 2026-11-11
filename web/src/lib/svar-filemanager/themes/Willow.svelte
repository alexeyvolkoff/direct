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

		--wx-fm-background: var(--bg-primary, #f6f7fa);
		--wx-fm-box-shadow: 0px 1px 2px rgba(22, 25, 37, 0.05),
			0px 3px 10px rgba(22, 25, 37, 0.08);
		--wx-fm-select-background: var(--wx-color-primary-selected, #e9ecf7);
		--wx-fm-grid-border: 1px solid var(--border-color, #d8dce8);
		--wx-fm-grid-header-color: #f6f7fa;
		--wx-fm-button-font-color: #5a6278;
		--wx-fm-toolbar-height: 56px;
	}
</style>
