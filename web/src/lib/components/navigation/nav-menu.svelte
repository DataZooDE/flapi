<script lang="ts">
  import { cn } from "$lib/utils";
  import { Button } from "$lib/components/ui/button";
  import { 
    ChevronRight,
    Cog,
    Database,
    Network
  } from "lucide-svelte";
  import ConfigTree from "./config-tree.svelte";
  import { currentPath, navigate } from "$lib/router";

  const navItems = [
    {
      title: "General",
      href: "/config",
      icon: Cog
    }
  ];

  // Get query params from URL
  const urlParams = new URLSearchParams(window.location.search);
  let selectedEndpoint = urlParams.get('path');
  let selectedConnection = urlParams.get('name');

  function handleClick(href: string) {
    navigate(href);
  }
</script>

<nav class="border-r bg-muted/40 h-screen w-[220px] fixed left-0 top-0 flex flex-col">
  <!-- Header -->
  <div class="p-4 border-b bg-muted/60">
    <h2 class="text-lg font-semibold">flAPI Configuration</h2>
  </div>

  <!-- Navigation Content -->
  <div class="flex-1 overflow-y-auto">
    <div class="p-4 space-y-4">
      <!-- General Config Button -->
      {#each navItems as item}
        <Button
          variant={$currentPath === item.href ? "secondary" : "ghost"}
          class="w-full justify-start"
          on:click={() => handleClick(item.href)}
        >
          <svelte:component this={item.icon} class="w-4 h-4 mr-2" />
          {item.title}
        </Button>
      {/each}

      <!-- Tree View -->
      <div class="pt-2">
        <ConfigTree
          bind:selectedEndpoint
          bind:selectedConnection
        />
      </div>
    </div>
  </div>
</nav>

<div class="pl-[220px]">
  <slot />
</div> 