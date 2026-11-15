// #211 cluster C against the real server: create keeps mcp-tool, --full,
// audit table, error bodies, schema commands, data keys left alone.
import { describe, it, expect } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { flapii } from './helpers';

const TOOL_YAML = `url-path: /cli-cluster-c
method: GET
template-source: customers/customers.sql
connection: [customers-parquet]
mcp-tool:
  name: cli_cluster_c_tool
  description: created by flapii
`;

function writeTemp(name: string, content: string): string {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'flapii-cc-'));
  const file = path.join(dir, name);
  fs.writeFileSync(file, content);
  return file;
}

describe('cluster C (integration)', () => {
  it('endpoints create -f keeps the mcp-tool block', async () => {
    const file = writeTemp('ep.yaml', TOOL_YAML);
    const created = await flapii(['endpoints', 'create', '-f', file]);
    expect(created.exitCode, created.stderr + created.stdout).toBe(0);

    const tools = await flapii(['mcp', 'tools', 'list', '--output', 'json']);
    expect(tools.exitCode, tools.stderr).toBe(0);
    expect(tools.stdout).toContain('cli_cluster_c_tool');

    const deleted = await flapii(['endpoints', 'delete', '/cli-cluster-c', '--force']);
    expect(deleted.exitCode, deleted.stderr + deleted.stdout).toBe(0);
  });

  it('endpoints list keeps endpoint paths as keys under the default camel style', async () => {
    const r = await flapii(['endpoints', 'list', '--output', 'json']);
    expect(r.exitCode).toBe(0);
    const keys = Object.keys(JSON.parse(r.stdout));
    expect(keys.some((k) => k.startsWith('/'))).toBe(true);
  });

  it('cache refresh --full is accepted and prints a result, not ""', async () => {
    const r = await flapii(['cache', 'refresh', '/customers/', '--full', '--output', 'json']);
    expect(r.exitCode, r.stderr + r.stdout).toBe(0);
    expect(r.stdout.trim()).not.toBe('""');
  });

  it('cache audit -o table prints a table', async () => {
    const r = await flapii(['cache', 'audit', '/customers/', '--output', 'table']);
    expect(r.exitCode, r.stderr + r.stdout).toBe(0);
    expect(r.stdout.trim().startsWith('[')).toBe(false);
    expect(r.stdout).toMatch(/Mode|No data to display/);
  });

  it('a failing call prints the server reason', async () => {
    // /customers/ has a cache; an endpoint without one makes refresh fail with a reason.
    const list = JSON.parse((await flapii(['endpoints', 'list', '--output', 'json'])).stdout) as Record<string, any>;
    const uncached = Object.entries(list).find(([p, cfg]) => p.startsWith('/') && !cfg?.cache?.enabled && !p.includes(':'));
    expect(uncached, 'examples have an endpoint without cache').toBeDefined();
    const r = await flapii(['cache', 'refresh', uncached![0]]);
    expect(r.exitCode).not.toBe(0);
    expect(r.stderr).toMatch(/\[4\d\d\] [A-Za-z ]+: \S/);
  });

  it('schema tables and connections work, --connection no longer collides with -c', async () => {
    const tables = await flapii(['schema', 'tables', '--output', 'json']);
    expect(tables.exitCode, tables.stderr + tables.stdout).toBe(0);
    expect(Array.isArray(JSON.parse(tables.stdout).tables)).toBe(true);

    const conns = await flapii(['schema', 'connections', '--output', 'json']);
    expect(conns.exitCode).toBe(0);
    expect(JSON.parse(conns.stdout).connections.length).toBeGreaterThan(0);

    const filtered = await flapii(['schema', 'tables', '--connection', 'nw', '--output', 'json']);
    expect(filtered.exitCode, filtered.stderr + filtered.stdout).toBe(0);
  });

  it('endpoints validate with no YAML says so', async () => {
    const file = writeTemp('empty.yaml', '');
    const r = await flapii(['endpoints', 'validate', '/customers/', '-f', file]);
    expect(r.exitCode).not.toBe(0);
    expect(r.stderr).toContain('No YAML given');
  });
});
