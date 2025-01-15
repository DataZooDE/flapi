import adapter from '@sveltejs/adapter-static';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

/** @type {import('@sveltejs/kit').Config} */
const config = {
  kit: {
    adapter: adapter({
      fallback: 'index.html',
      pages: 'build',
      assets: 'build',
      precompress: false,
      strict: true
    }),
    alias: {
      $lib: 'src/lib',
      $components: 'src/lib/components'
    }
  },
  preprocess: vitePreprocess()
};

export default config;
