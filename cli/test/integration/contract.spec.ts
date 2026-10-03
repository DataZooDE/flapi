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

describe('cache update sends what the server reads', () => {
  it('--schedule changes the schedule and keeps caching enabled', async () => {
    const upd = await flapii(['cache', 'update', '/customers/', '--schedule', '10m']);
    expect(upd.exitCode, upd.stderr + upd.stdout).toBe(0);
    const got = JSON.parse((await flapii(['cache', 'get', '/customers/', '--output', 'json'])).stdout);
    expect(got.enabled).toBe(true);
    expect(got.schedule).toBe('10m');
  });

  it('--ttl is not a server setting: it fails and says what to use, changing nothing', async () => {
    const before = JSON.parse((await flapii(['cache', 'get', '/customers/', '--output', 'json'])).stdout);
    const upd = await flapii(['cache', 'update', '/customers/', '--ttl', '300']);
    expect(upd.exitCode).not.toBe(0);
    expect(upd.stderr + upd.stdout).toContain('--schedule');
    const after = JSON.parse((await flapii(['cache', 'get', '/customers/', '--output', 'json'])).stdout);
    expect(after).toEqual(before);
  });
});

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { lossyProxy } from './proxy';

describe('a lost response to a write is not retried', () => {
  it('endpoints create sends the POST once even when the answer is lost', async () => {
    const proxy = await lossyProxy(process.env.FLAPI_BASE_URL!);
    try {
      const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'flapii-retry-')), 'ep.json');
      fs.writeFileSync(file, JSON.stringify({
        'url-path': '/retry-probe', method: 'GET', 'template-source': 'retry-probe.sql',
        connection: ['customers-parquet'],
      }));
      const r = await flapii(['endpoints', 'create', '--file', file], { FLAPI_BASE_URL: proxy.url });
      expect(r.exitCode).not.toBe(0); // the answer was lost: the CLI must say so, not guess
      expect(proxy.count('POST'), 'the CLI repeated a non-idempotent request').toBe(1);
    } finally {
      // The server committed the write; leave the shared server as it was found.
      await flapii(['endpoints', 'delete', '/retry-probe']);
      await proxy.close();
    }
  });

  it('a safe GET is still retried after a lost response', async () => {
    const proxy = await lossyProxy(process.env.FLAPI_BASE_URL!);
    try {
      const r = await flapii(['endpoints', 'list', '--output', 'json'], { FLAPI_BASE_URL: proxy.url });
      expect(r.exitCode).toBe(0);
    } finally {
      await proxy.close();
    }
  });
});

import { startFlapi } from './helpers';

describe('a first-time user is told what to do', () => {
  it('a missing token says how to supply one', async () => {
    const r = await flapii(['endpoints', 'list'], { FLAPI_CONFIG_SERVICE_TOKEN: '' });
    expect(r.exitCode).not.toBe(0);
    expect(r.stderr + r.stdout).toContain('FLAPI_CONFIG_SERVICE_TOKEN');
  });

  it('a wrong token says the token was rejected', async () => {
    const r = await flapii(['endpoints', 'list'], { FLAPI_CONFIG_SERVICE_TOKEN: 'not-the-token' });
    expect(r.exitCode).not.toBe(0);
    expect(r.stderr + r.stdout).toMatch(/token/i);
  });

  it('an unreachable server says where it looked', async () => {
    const r = await flapii(['endpoints', 'list'], { FLAPI_BASE_URL: 'http://127.0.0.1:1' });
    expect(r.exitCode).not.toBe(0);
    expect(r.stderr + r.stdout).toContain('http://127.0.0.1:1');
    expect(r.stderr + r.stdout).toMatch(/running|reach/i);
  });

  it('a server with the config service OFF says how to turn it on', async () => {
    const server = await startFlapi({ configService: false });
    try {
      const r = await flapii(['endpoints', 'list'], { FLAPI_BASE_URL: server.url });
      expect(r.exitCode).not.toBe(0);
      expect(r.stderr + r.stdout).toContain('--config-service');
    } finally {
      await server.stop();
    }
  });
});
