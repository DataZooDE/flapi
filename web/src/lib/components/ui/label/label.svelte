<script lang="ts">
	import { cn } from "$lib/utils";

	let className: string | undefined = undefined;
	export { className as class };
	export let for_id: string | undefined = undefined;
</script>

<label
	class={cn(
		"text-sm font-medium leading-none peer-disabled:cursor-not-allowed peer-disabled:opacity-70",
		className
	)}
	for={for_id}
	{...$$restProps}
>
	<slot />
</label>
