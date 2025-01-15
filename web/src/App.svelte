<script lang="ts">
  import "./app.css";
  import { NavigationMenu } from "$lib/components/navigation";
  import { ThemeProvider } from "$lib/components/theme";
  import { Router } from "$lib/router/index";
</script>

<ThemeProvider>
  <div class="min-h-screen bg-background text-foreground">
    <!-- Primary Left Sidebar Panel -->
    <NavigationMenu />

    <!-- Secondary Right Editor Area -->
    <main class="md:ml-[220px] p-6">
      <Router />
    </main>
  </div>
</ThemeProvider>
