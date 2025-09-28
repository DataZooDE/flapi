import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['test/integration/**/*.spec.ts'],
    env: {
      NODE_ENV: 'test',
    },
    hookTimeout: 60000,
    testTimeout: 60000,
    threads: false,
    setupFiles: ['test/integration/setup.ts'],
  },
});

