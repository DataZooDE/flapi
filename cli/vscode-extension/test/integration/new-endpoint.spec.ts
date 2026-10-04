/**
 * "New endpoint" creates the endpoint ON THE SERVER (ConfigService), never in the
 * local workspace. Everything below talks to the REAL flAPI the global setup started.
 */
import { describe, it, expect, afterAll } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { parse } from 'yaml';
import { createApiClient } from '@flapi/shared';
import { createEndpointOnServer, deleteEndpointOnServer, listConnectionNames } from '../../src/services/endpointCreator';

const base = process.env.FLAPI_BASE_URL!;
const token = process.env.FLAPI_CONFIG_SERVICE_TOKEN!;
const workdir = process.env.FLAPI_IT_WORKDIR!;
const sqlsDir = path.join(workdir, 'examples', 'sqls');
const client = createApiClient({ baseUrl: base, authToken: token, timeout: 30, retries: 0, verifyTls: true } as never);

const created: string[] = [];

function listTree(root: string): string[] {
  const out: string[] = [];
  const walk = (dir: string) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      if (entry.name === 'node_modules' || entry.name === '.git') continue;
      const full = path.join(dir, entry.name);
      out.push(full);
      if (entry.isDirectory()) walk(full);
    }
  };
  walk(root);
  return out.sort();
}

async function serverPaths(): Promise<string[]> {
  const res = await client.get('/api/v1/_config/endpoints');
  const data = res.data;
  const entries = Array.isArray(data) ? data : Object.values<any>(data);
  return entries.map((e: any) => e['url-path'] ?? e.urlPath ?? e.path ?? String(e)).sort();
}

afterAll(async () => {
  for (const urlPath of created) {
    try {
      await deleteEndpointOnServer(client, urlPath);
    } catch {
      /* already gone */
    }
  }
  // flapi_delete_endpoint removes the YAML but leaves the SQL template behind.
  for (const f of fs.readdirSync(sqlsDir)) {
    if (f.startsWith('newep') && f.endsWith('.sql')) fs.rmSync(path.join(sqlsDir, f));
  }
});

describe('listConnectionNames', () => {
  it('returns the connections the real server has configured', async () => {
    const names = await listConnectionNames(client);
    expect(names).toContain('customers-parquet');
  });
});

describe('createEndpointOnServer', () => {
  it('creates a persistent, serving endpoint on the server and writes nothing locally', async () => {
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'ext-workspace-'));
    fs.mkdirSync(path.join(workspace, 'examples', 'sqls'), { recursive: true });
    const localBefore = [...listTree(workspace), ...listTree(process.cwd())];

    const urlPath = '/newep-basic';
    const result = await createEndpointOnServer(client, {
      name: 'newep-basic',
      urlPath,
      connection: 'customers-parquet',
      sql: "SELECT 'hello' AS message",
    });
    created.push(urlPath);
    expect(result.urlPath).toBe(urlPath);
    expect(result.slug).toBeTruthy();

    // exists on the server
    expect(await serverPaths()).toContain(urlPath);
    // serves
    const served = await fetch(`${base}${urlPath}`);
    expect(served.status).toBe(200);
    expect(JSON.stringify(await served.json())).toContain('hello');
    // persisted under the SERVER's templates directory
    const yamlFile = fs
      .readdirSync(sqlsDir)
      .filter((f) => f.endsWith('.yaml'))
      .find((f) => parse(fs.readFileSync(path.join(sqlsDir, f), 'utf8'))?.['url-path'] === urlPath);
    expect(yamlFile, 'no YAML with this url-path under the server templates dir').toBeTruthy();
    expect(fs.readFileSync(path.join(sqlsDir, 'newep-basic.sql'), 'utf8')).toContain("'hello' AS message");
    const stored = parse(fs.readFileSync(path.join(sqlsDir, yamlFile!), 'utf8'));
    expect(stored['url-path']).toBe(urlPath);

    // nothing written under the extension's local workspace / cwd
    expect([...listTree(workspace), ...listTree(process.cwd())]).toEqual(localBefore);
    fs.rmSync(workspace, { recursive: true, force: true });
  });

  it('rejects a connection the server does not know, with the server message, and creates nothing', async () => {
    const before = await serverPaths();
    const filesBefore = fs.readdirSync(sqlsDir).sort();
    await expect(
      createEndpointOnServer(client, { name: 'newep-noconn', urlPath: '/newep-noconn', connection: 'no-such-connection' }),
    ).rejects.toThrow(/no-such-connection.*not found/i);
    expect(await serverPaths()).toEqual(before);
    expect(fs.readdirSync(sqlsDir).sort()).toEqual(filesBefore);
  });

  const hostile: Array<[string, { name: string; urlPath: string }]> = [
    ['newline + yaml key in url-path', { name: 'newep-h1', urlPath: '/newep-h1\nurl-path: /evil' }],
    ['colon-space and quotes in url-path', { name: 'newep-h2', urlPath: '/newep-h2: "x" \'y\' # c' }],
    ['dot-dot in url-path', { name: 'newep-h3', urlPath: '/../../newep-h3' }],
    ['dot-dot in name (template-source)', { name: '../../newep-h4', urlPath: '/newep-h4' }],
    ['absolute name', { name: '/tmp/newep-h5', urlPath: '/newep-h5' }],
  ];

  for (const [label, input] of hostile) {
    it(`cannot inject YAML or escape the templates directory: ${label}`, async () => {
      const outsideBefore = listTree(path.join(workdir)).filter((f) => !f.startsWith(sqlsDir + path.sep));
      const sqlsBefore = new Set(fs.readdirSync(sqlsDir));
      let accepted = false;
      try {
        await createEndpointOnServer(client, { ...input, connection: 'customers-parquet', sql: 'SELECT 1 AS n' });
        accepted = true;
        created.push(input.urlPath);
      } catch {
        /* rejection is the expected outcome */
      }

      // no endpoint other than the one literally requested may exist
      const paths = await serverPaths();
      expect(paths).not.toContain('/evil');
      // the server never wrote anything outside its templates directory (cache db etc. excluded)
      const outsideAfter = listTree(path.join(workdir)).filter(
        (f) => !f.startsWith(sqlsDir + path.sep) && !f.includes(`${path.sep}data${path.sep}`),
      );
      expect(outsideAfter).toEqual(outsideBefore.filter((f) => !f.includes(`${path.sep}data${path.sep}`)));

      for (const f of fs.readdirSync(sqlsDir).filter((n) => !sqlsBefore.has(n))) {
        if (!f.endsWith('.yaml')) continue;
        const doc = parse(fs.readFileSync(path.join(sqlsDir, f), 'utf8'));
        // exactly the values we sent, as DATA - no extra keys smuggled in
        expect(doc['url-path']).toBe(input.urlPath);
        expect(Object.keys(doc).sort()).not.toContain('evil');
        expect(doc['template-source']).toBe(`${input.name}.sql`);
      }
      if (!accepted) {
        expect(new Set(fs.readdirSync(sqlsDir))).toEqual(sqlsBefore);
      }
    });
  }
});

describe('the command no longer writes local files', () => {
  it('extension.ts newEndpoint does not use workspace.fs.writeFile', () => {
    const src = fs.readFileSync(path.resolve(__dirname, '../../src/extension.ts'), 'utf8');
    const start = src.indexOf("registerCommand('flapi.newEndpoint'");
    const end = src.indexOf("registerCommand('flapi.schema.copyTableName'");
    expect(start).toBeGreaterThan(0);
    const body = src.slice(start, end);
    expect(body).not.toMatch(/fs\.writeFile|writeFileSync/);
    expect(body).not.toMatch(/examples\/sqls/);
  });
});
