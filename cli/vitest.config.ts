import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['test/unit/**/*.spec.ts'],
    env: {
      NODE_ENV: 'test',
    },
    coverage: {
      enabled: false,
    },
    deps: {
      inline: ['cli-table3'],
    },
  },
  resolve: {
    alias: {
      '~': '/src',
    },
  },
});

