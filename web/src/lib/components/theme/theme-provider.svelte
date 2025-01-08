<script lang="ts">
  import { onMount } from 'svelte';
  import { theme } from '$lib/stores/theme';

  onMount(() => {
    theme.initialize();

    // Listen for system theme changes
    const mediaQuery = window.matchMedia('(prefers-color-scheme: dark)');
    const handleChange = () => {
      // Only update if we're using system theme
      const currentTheme = localStorage.getItem('theme') || 'system';
      if (currentTheme === 'system') {
        document.documentElement.classList.toggle('dark', mediaQuery.matches);
      }
    };

    mediaQuery.addEventListener('change', handleChange);
    return () => mediaQuery.removeEventListener('change', handleChange);
  });
</script>

<slot /> 