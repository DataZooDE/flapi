/**
 * The CLI against a REAL flAPI server: what it prints must match what the server
 * actually returns, and must never print the config-service token.
 * (Findings from the CLI/extension review; each was reproduced by hand first.)
 */
import { describe, it, expect } from 'vitest';
import { flapii, TOKEN } from './helpers';

describe('config show never discloses the token', () => {
  it('table output', async () => {
    expect(TOKEN).not.toBe('');
    const r = await flapii(['config', 'show', '--output', 'table']);
    expect(r.exitCode).toBe(0);
    expect(r.stdout + r.stderr).not.toContain(TOKEN);
    expect(r.stdout).toContain('AuthToken');
  });

  it('json output', async () => {
    const r = await flapii(['config', 'show', '--output', 'json']);
    expect(r.exitCode).toBe(0);
    expect(r.stdout + r.stderr).not.toContain(TOKEN);
  });
});

describe('templates', () => {
  it('get prints the SQL text in table output, not [object Object]', async () => {
    const r = await flapii(['templates', 'get', '/customers/', '--output', 'table']);
    expect(r.exitCode).toBe(0);
    expect(r.stdout).not.toContain('[object Object]');
    expect(r.stdout.toUpperCase()).toContain('SELECT');
  });

  it('get --output json is one parseable JSON value on stdout', async () => {
    const r = await flapii(['templates', 'get', '/customers/', '--output', 'json']);
    expect(r.exitCode).toBe(0);
    expect(typeof JSON.parse(r.stdout).template).toBe('string');
  });

  it('test reports success for a template that runs', async () => {
    const r = await flapii(['templates', 'test', '/customers/', '--output', 'table']);
    expect(r.exitCode).toBe(0);
    expect(r.stdout).not.toMatch(/invalid/i);
    expect(r.stdout).toMatch(/succe|valid|rows/i);
  });
});

describe('MCP listings see every entity kind', () => {
  it('lists the tool, the resource and the prompt of the examples', async () => {
    for (const kind of ['tools', 'resources', 'prompts']) {
      const r = await flapii(['mcp', kind, 'list', '--output', 'json']);
      expect(r.exitCode).toBe(0);
      const items = JSON.parse(r.stdout);
      expect(Array.isArray(items) && items.length > 0, `mcp ${kind} list was empty`).toBe(true);
    }
  });
});
