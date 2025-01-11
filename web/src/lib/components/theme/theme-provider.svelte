<script lang="ts">
  import { onMount } from "svelte";
  
  let theme: "light" | "dark" = "light";
  
  onMount(() => {
    // Check system preference
    if (window.matchMedia("(prefers-color-scheme: dark)").matches) {
      theme = "dark";
      document.documentElement.classList.add("dark");
    }
  });
</script>

<div class={theme}>
  <slot />
</div> 