import { describe, it, expect } from 'vitest';
import path from 'node:path';
import { execa } from 'execa';
import { baseUrl, configPath } from './utils';

const cliPath = path.resolve('dist', 'index.js');

describe('cli schema command (integration)', () => {
  it('gets schema information', async () => {
    const result = await execa('node', [cliPath, 'schema', 'get', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });

    expect(result.exitCode).toBe(0);
    // Should return schema information
    expect(result.stdout.length).toBeGreaterThan(0);
  });

  it('refreshes schema information', async () => {
    const result = await execa('node', [cliPath, 'schema', 'refresh', '--force', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });

    expect(result.exitCode).toBe(0);
    // Should return refresh result
    expect(result.stdout.length).toBeGreaterThan(0);
  });

  it('lists database connections', async () => {
    const result = await execa('node', [cliPath, 'schema', 'connections', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });

    expect(result.exitCode).toBe(0);
    // Should return connections array or object
    expect(result.stdout.length).toBeGreaterThan(0);
  });

  it('lists tables in connections', async () => {
    const result = await execa('node', [cliPath, 'schema', 'tables', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });

    expect(result.exitCode).toBe(0);
    // Should return tables information
    expect(result.stdout.length).toBeGreaterThan(0);
  });
});
