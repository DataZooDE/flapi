<script lang="ts">
  import { cn } from "../../../utils";

  let className: string | undefined = undefined;
  export { className as class };
</script>

<aside
  class={cn("flex h-full w-full flex-col border-r bg-background", className)}
  {...$$restProps}
>
  <slot />
</aside> 