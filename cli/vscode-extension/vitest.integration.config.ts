import path from 'node:path';
import { defineConfig } from 'vitest/config';

// Extension code against a REAL flAPI server. The only fake is the `vscode` module
// itself (it exists only inside the VS Code host); everything on the server side
// is the real binary, started by the same global setup the CLI suite uses.
export default defineConfig({
  resolve: {
    alias: { vscode: path.resolve(__dirname, 'test/stubs/vscode.ts') },
  },
  test: {
    include: ['test/integration/**/*.spec.ts'],
    environment: 'node',
    hookTimeout: 120000,
    testTimeout: 60000,
    pool: 'forks',
    poolOptions: { forks: { singleFork: true } },
    globalSetup: [path.resolve(__dirname, '../test/integration/global-setup.ts')],
  },
});
