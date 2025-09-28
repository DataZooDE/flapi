import { describe, it, expect } from 'vitest';
import path from 'node:path';
import { execa } from 'execa';
import { baseUrl, configPath } from './utils';

const cliPath = path.resolve('dist', 'index.js');

describe('cli cache command (integration)', () => {
  it('lists cache configurations', async () => {
    const result = await execa('node', [cliPath, 'cache', 'list', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });

    expect(result.exitCode).toBe(0);
    // Should return an object with cache configurations
    expect(result.stdout.length).toBeGreaterThan(0);
  });

  it('gets cache configuration for an endpoint', async () => {
    // First get the list to find an available endpoint
    const listResult = await execa('node', [cliPath, 'endpoints', 'list', '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });

    const endpoints = JSON.parse(listResult.stdout);
    const endpointPath = Object.keys(endpoints)[0];

    if (!endpointPath) {
      // Skip if no endpoints available
      return;
    }

    const result = await execa('node', [cliPath, 'cache', 'get', endpointPath, '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });

    expect(result.exitCode).toBe(0);
    // Should return cache configuration or empty object
    expect(result.stdout.length).toBeGreaterThan(0);
  });

  it('refreshes cache for an endpoint', async () => {
    // First get the list to find an available endpoint
    const listResult = await execa('node', [cliPath, 'endpoints', 'list', '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });

    const endpoints = JSON.parse(listResult.stdout);
    const endpointPath = Object.keys(endpoints)[0];

    if (!endpointPath) {
      // Skip if no endpoints available
      return;
    }

    const result = await execa('node', [cliPath, 'cache', 'refresh', endpointPath, '--force', '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });

    expect(result.exitCode).toBe(0);
    // Should return refresh result
    expect(result.stdout.length).toBeGreaterThan(0);
  });
});
