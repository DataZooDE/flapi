import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['test/**/*.spec.ts'],
    // Real-server tests need a flapi binary: `npm run test:integration`.
    exclude: ['test/integration/**', 'node_modules/**'],
    environment: 'node',
  },
});
