<script lang="ts">
	import { cn } from "../../../utils";
	import { buttonVariants, type ButtonVariants } from ".";

	let className: string | undefined = undefined;
	export { className as class };
	export let variant: ButtonVariants["variant"] = "default";
	export let size: ButtonVariants["size"] = "default";
	export let href: string | undefined = undefined;
</script>

{#if href}
	<a
		{href}
		class={cn(buttonVariants({ variant, size }), className)}
		{...$$restProps}
	>
		<slot />
	</a>
{:else}
	<button
		class={cn(buttonVariants({ variant, size }), className)}
		{...$$restProps}
		on:click
	>
		<slot />
	</button>
{/if}
