import { defineConfig } from 'vitest/config';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { svelteTesting } from '@testing-library/svelte/vite';
import path from 'path';

export default defineConfig(({ mode }) => ({
  plugins: [
    svelte({ hot: !process.env.VITEST }), 
    svelteTesting()
  ],
  test: {
    include: ['src/**/*.{test,spec}.{js,ts}'],
    globals: true,
    environment: 'jsdom',
    setupFiles: ['./src/test/setup.ts'],
    alias: {
      '$lib': path.resolve(__dirname, './src/lib')
    },
    deps: {
      inline: [/^svelte/]
    }
  },
  resolve: {
    conditions: mode === 'test' ? ['browser'] : [],
  }
})); 