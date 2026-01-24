import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['test/unit/**/*.spec.ts'],
    env: {
      NODE_ENV: 'test',
    },
    setupFiles: ['test/setup/resetExitCode.ts'],
    coverage: {
      enabled: false,
    },
    server: {
      deps: {
        inline: ['cli-table3'],
      },
    },
  },
  resolve: {
    alias: {
      '~': '/src',
    },
  },
});
