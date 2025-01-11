<script lang="ts">
  import "../app.css";
  import { NavigationMenu } from "$lib/components/navigation";
  import { ThemeProvider } from "$lib/components/theme";
</script>

<ThemeProvider>
  <div class="min-h-screen bg-background text-foreground flex">
    <!-- Primary Left Sidebar Panel -->
    <NavigationMenu />

    <!-- Secondary Right Editor Area -->
    <main class="flex-1 pl-[220px] p-6">
      <slot />
    </main>
  </div>
</ThemeProvider> 