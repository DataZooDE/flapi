import { describe, it, expect } from 'vitest';
import path from 'node:path';
import { execa } from 'execa';
import { baseUrl, configPath } from './utils';

const cliPath = path.resolve('dist', 'index.js');

describe('cli ping command (integration)', () => {
  it('succeeds when server is running', async () => {
    expect(baseUrl).toBeDefined();

    const result = await execa('node', [cliPath, 'ping'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });

    expect(result.exitCode).toBe(0);
    expect(result.stdout).toContain('Name: example-flapi-project');
  });
});

