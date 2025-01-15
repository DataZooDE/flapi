<script lang="ts">
  import "../app.css";
  import { NavigationMenu } from "$lib/components/navigation";
  import { ThemeProvider } from "$lib/components/theme";
</script>

<ThemeProvider>
  <div class="min-h-screen">
    <NavigationMenu>
      <slot />
    </NavigationMenu>
  </div>
</ThemeProvider> 