<script lang="ts">
  import "./app.css";
  import { Sidebar } from "./lib/components/ui/sidebar";
  import NavMenu from "./lib/components/navigation/nav-menu.svelte";
  import ThemeToggle from "./lib/components/theme/theme-toggle.svelte";
  import ThemeProvider from "./lib/components/theme/theme-provider.svelte";
</script>

<ThemeProvider>
  <div class="h-screen w-screen flex overflow-hidden bg-background text-foreground">
    <Sidebar class="w-64">
      <div class="p-4 border-b">
        <h1 class="text-lg font-semibold">flAPI Configuration</h1>
        <div class="absolute right-4 top-4">
          <ThemeToggle />
        </div>
      </div>
      <div class="p-2">
        <NavMenu />
      </div>
    </Sidebar>

    <main class="flex-1 overflow-y-auto">
      <slot />
    </main>
  </div>
</ThemeProvider>
