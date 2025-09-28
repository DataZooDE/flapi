import { describe, it, expect } from 'vitest';
import path from 'node:path';
import { execa } from 'execa';
import { baseUrl, configPath } from './utils';

const cliPath = path.resolve('dist', 'index.js');

describe('cli templates command (integration)', () => {
  it('lists templates', async () => {
    const result = await execa('node', [cliPath, 'templates', 'list', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });

    expect(result.exitCode).toBe(0);
    // Should return an object with template paths
    expect(result.stdout.length).toBeGreaterThan(0);
  });

  it('gets a template', async () => {
    // First get the list to find an available template
    const listResult = await execa('node', [cliPath, 'templates', 'list', '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });

    const templates = JSON.parse(listResult.stdout);
    const templatePath = Object.keys(templates)[0];

    if (!templatePath) {
      // Skip if no templates available
      return;
    }

    const result = await execa('node', [cliPath, 'templates', 'get', templatePath, '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });

    expect(result.exitCode).toBe(0);
    const template = JSON.parse(result.stdout);
    expect(typeof template).toBe('string');
  });

  it('tests template syntax', async () => {
    // First get the list to find an available template
    const listResult = await execa('node', [cliPath, 'templates', 'list', '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });

    const templates = JSON.parse(listResult.stdout);
    const templatePath = Object.keys(templates)[0];

    if (!templatePath) {
      // Skip if no templates available
      return;
    }

    const result = await execa('node', [cliPath, 'templates', 'test', templatePath, '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });

    expect(result.exitCode).toBe(0);
    const testResult = JSON.parse(result.stdout);
    expect(testResult).toHaveProperty('valid');
  });
});
