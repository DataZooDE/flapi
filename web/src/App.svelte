<script lang="ts">
  import "./app.css";
  import { Sidebar } from "./lib/components/ui/sidebar";
  import NavMenu from "./lib/components/navigation/nav-menu.svelte";
</script>

<div class="h-screen w-screen flex overflow-hidden bg-background text-foreground">
  <Sidebar class="w-64">
    <div class="p-4 border-b">
      <h1 class="text-lg font-semibold">flAPI Configuration</h1>
    </div>
    <div class="p-2">
      <NavMenu />
    </div>
  </Sidebar>

  <main class="flex-1 overflow-y-auto">
    <slot />
  </main>
</div>
