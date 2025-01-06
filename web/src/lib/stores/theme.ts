import { writable } from 'svelte/store';
import type { Theme } from '$lib/config/theme';

function createThemeStore() {
  const { subscribe, set } = writable<Theme>('system');

  return {
    subscribe,
    set: (theme: Theme) => {
      set(theme);
      if (theme === 'system') {
        const systemTheme = window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
        document.documentElement.classList.toggle('dark', systemTheme === 'dark');
      } else {
        document.documentElement.classList.toggle('dark', theme === 'dark');
      }
    },
    initialize: () => {
      const savedTheme = (localStorage.getItem('theme') as Theme) || 'system';
      set(savedTheme);
    }
  };
}

export const theme = createThemeStore(); 