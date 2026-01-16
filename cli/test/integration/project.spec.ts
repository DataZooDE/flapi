import { describe, it, expect } from 'vitest';
import path from 'node:path';
import { execa } from 'execa';
import { baseUrl, configPath } from './utils';

const cliPath = path.resolve('dist', 'index.js');

describe('cli project command (integration)', () => {
  it('returns project configuration', async () => {
    const result = await execa('node', [cliPath, 'project', 'get', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });

    expect(result.exitCode).toBe(0);
    const payload = JSON.parse(result.stdout.trim());
    expect((payload.name ?? payload['project-name'])).toBe('example-flapi-project');
  });
});

