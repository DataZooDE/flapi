<script lang="ts">
  import { ChevronRight } from "lucide-svelte";
  import { cn } from "$lib/utils";
  import { Button } from "$lib/components/ui/button";

  export let label: string;
  export let icon: any = undefined;
  export let expanded = false;
  export let selected = false;
  export let level = 0;
  export let hasChildren = false;

  $: indentClass = `pl-${level * 4}`;
</script>

<div class={cn("flex items-center", indentClass)}>
  {#if hasChildren}
    <Button 
      variant="ghost" 
      size="icon" 
      class="w-4 h-4 p-0 mr-1"
      on:click={() => expanded = !expanded}
    >
      <ChevronRight class={cn("w-4 h-4 transition-transform", expanded && "rotate-90")} />
    </Button>
  {:else}
    <div class="w-5" ></div>
  {/if}

  <button type="button" 
    class={cn(
      "flex items-center py-1 px-2 rounded-sm cursor-pointer hover:bg-muted/50 flex-1",
      selected && "bg-muted"
    )}
    on:click
  >
    {#if icon}
      <svelte:component this={icon} class="w-4 h-4 mr-2" />
    {/if}
    <span class="text-sm">{label}</span>
  </button>
</div>

{#if expanded && hasChildren}
  <slot />
{/if} 