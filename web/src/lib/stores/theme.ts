import { writable } from 'svelte/store';
import type { Theme } from '$lib/config/theme';

function createThemeStore() {
  const { subscribe, set } = writable<Theme>('system');

  const getSystemTheme = (): 'light' | 'dark' => {
    if (typeof window === 'undefined') return 'light';
    return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  };

  const applyTheme = (value: Theme) => {
    if (typeof document === 'undefined') return;
    if (value === 'system') {
      document.documentElement.classList.toggle('dark', getSystemTheme() === 'dark');
    } else {
      document.documentElement.classList.toggle('dark', value === 'dark');
    }
  };

  // Initialize theme from localStorage or system preference
  const savedTheme = typeof localStorage !== 'undefined' 
    ? (localStorage.getItem('theme') as Theme) 
    : 'system';
  set(savedTheme || 'system');
  applyTheme(savedTheme || 'system');

  return {
    subscribe,
    set: (value: Theme) => {
      set(value);
      if (typeof localStorage !== 'undefined') {
        localStorage.setItem('theme', value);
      }
      applyTheme(value);
    },
    initialize: () => {
      const savedTheme = localStorage.getItem('theme') as Theme || 'system';
      set(savedTheme);
      applyTheme(savedTheme);
    }
  };
}

export const theme = createThemeStore();