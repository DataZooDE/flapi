import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['test/integration/**/*.spec.ts'],
    env: { NODE_ENV: 'test' },
    hookTimeout: 120000,
    testTimeout: 60000,
    pool: 'forks',
    poolOptions: { forks: { singleFork: true } },
    globalSetup: ['test/integration/global-setup.ts'],
  },
});
