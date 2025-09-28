import { describe, it, expect } from 'vitest';
import path from 'node:path';
import { execa } from 'execa';
import { baseUrl, configPath } from './utils';

const cliPath = path.resolve('dist', 'index.js');

describe('cli mcp commands (integration)', () => {
  it('lists MCP tools', async () => {
    const result = await execa('node', [cliPath, 'mcp', 'tools', 'list', '--output', 'json'], {
      env: {
        ...process.env,
        FLAPI_BASE_URL: baseUrl,
        FLAPI_CONFIG: configPath,
      },
    });
    expect(result.exitCode).toBe(0);
    const payload = JSON.parse(result.stdout);
    expect(Array.isArray(payload)).toBe(true);
  });

  it('gets MCP resource', async () => {
    const list = await execa('node', [cliPath, 'mcp', 'resources', 'list', '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });
    const resources = JSON.parse(list.stdout);
    const resourceName = resources[0]?.['mcp-resource']?.name ?? resources[0]?.mcpResource?.name;
    if (!resourceName) {
      throw new Error('No MCP resource available for testing');
    }

    const result = await execa('node', [cliPath, 'mcp', 'resources', 'get', resourceName, '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });
    expect(result.exitCode).toBe(0);
  });

  it('gets MCP prompt', async () => {
    const list = await execa('node', [cliPath, 'mcp', 'prompts', 'list', '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });
    const prompts = JSON.parse(list.stdout);
    const promptName = prompts[0]?.['mcp-prompt']?.name ?? prompts[0]?.mcpPrompt?.name;
    if (!promptName) {
      throw new Error('No MCP prompt available for testing');
    }

    const result = await execa('node', [cliPath, 'mcp', 'prompts', 'get', promptName, '--output', 'json'], {
      env: { ...process.env, FLAPI_BASE_URL: baseUrl, FLAPI_CONFIG: configPath },
    });
    expect(result.exitCode).toBe(0);
  });
});

