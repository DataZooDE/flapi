<script lang="ts">
	import { Tabs as TabsPrimitive } from "bits-ui";
	import { cn } from "$lib/utils";

	type $$Props = TabsPrimitive.ContentProps;

	let className: $$Props["class"] = undefined;
	export let value: string;
	export { className as class };
</script>

<TabsPrimitive.Content
	{value}
	class={cn(
		"mt-2 ring-offset-background focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2",
		className
	)}
	{...$$restProps}
>
	<slot />
</TabsPrimitive.Content>
