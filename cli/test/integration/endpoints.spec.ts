import { describe, it, expect } from 'vitest';
import path from 'node:path';
import { execa } from 'execa';
import { baseUrl, configPath } from './utils';

const cliPath = path.resolve('dist', 'index.js');

describe('cli endpoints command (integration)', () => {
  it('can list existing endpoints', async () => {
    const result = await execa('node', [cliPath, 'endpoints', 'list', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });
    expect(result.exitCode).toBe(0);
    // Should show existing endpoints from the config - just check it runs successfully
    expect(result.stdout.length).toBeGreaterThan(0);
  });

  it('can get project configuration', async () => {
    const result = await execa('node', [cliPath, 'project', 'get', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });
    expect(result.exitCode).toBe(0);
    // Should show project configuration - just check it runs successfully
    expect(result.stdout.length).toBeGreaterThan(0);
  });

  it('can ping the server', async () => {
    const result = await execa('node', [cliPath, 'ping'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });
    expect(result.exitCode).toBe(0);
    // Should show connection success and project info
    expect(result.stdout).toContain('📊 Project Information');
    expect(result.stdout).toContain('Name: example-flapi-project');
  });
});

