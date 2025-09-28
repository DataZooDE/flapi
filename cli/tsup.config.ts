import { defineConfig } from 'tsup';

export default defineConfig({
  entry: {
    index: 'src/index.ts',
  },
  format: ['esm'],
  sourcemap: true,
  clean: true,
  dts: false,
  target: 'node20',
  banner: {
    js: '#!/usr/bin/env node',
  },
  splitting: false,
  minify: false,
  ignoreWatch: ['test', 'dist'],
});

