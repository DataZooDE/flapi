/**
 * Explorer file commands map a server-supplied relative path onto a local
 * directory. Positive cases use the REAL server's filesystem listing; the hostile
 * ones are what a compromised server could send.
 */
import { describe, it, expect } from 'vitest';
import fs from 'node:fs';
import path from 'node:path';
import { createApiClient } from '@flapi/shared';
import { resolveInside } from '../../src/workspace/safePath';

const base = process.env.FLAPI_BASE_URL!;
const token = process.env.FLAPI_CONFIG_SERVICE_TOKEN!;
const root = path.join(process.env.FLAPI_IT_WORKDIR!, 'examples', 'sqls');

describe('resolveInside', () => {
  it('maps every path the real server lists to a file inside the templates directory', async () => {
    const client = createApiClient({ baseUrl: base, authToken: token, timeout: 30, retries: 0, verifyTls: true } as never);
    const res = await client.get('/api/v1/_config/filesystem');
    const paths: string[] = [];
    const walk = (node: any) => {
      if (node?.path && node.type === 'file') paths.push(node.path);
      (node?.children ?? []).forEach(walk);
    };
    (res.data.tree ?? []).forEach(walk);
    expect(paths.length, 'the server listed no files').toBeGreaterThan(0);
    for (const rel of paths) {
      const full = resolveInside(root, rel);
      expect(full.startsWith(path.resolve(root) + path.sep)).toBe(true);
    }
    expect(fs.existsSync(resolveInside(root, 'customers/customers-rest.yaml'))).toBe(true);
  });

  it.each(['../../etc/passwd', '../x', 'a/../../x', '/etc/passwd', 'customers/../../..', '', '..', '.'])(
    'refuses %j',
    (rel) => {
      expect(() => resolveInside(root, rel)).toThrow(/outside the templates directory/);
    },
  );
});
