import { defineConfig } from 'tsup';

// A local config so tsup does not search upward and pick up cli/tsup.config.ts
// (which builds the CLI, not this package, and fails to resolve tsup in CI where
// cli/node_modules isn't installed).
export default defineConfig({
  entry: { index: 'src/index.ts' },
  format: ['esm', 'cjs'],
  dts: true,
  sourcemap: true,
  clean: true,
  target: 'node20',
  splitting: false,
  minify: false,
});
