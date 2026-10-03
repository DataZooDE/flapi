/**
 * The config-service token is a credential: it belongs in SecretStorage, not in the
 * workspace's ordinary state file, and a stored token must actually authenticate
 * against a real server.
 */
import { describe, it, expect } from 'vitest';
import { createApiClient } from '@flapi/shared';
import { TokenStorageService } from '../../src/services/tokenStorage';
import { fakeContext } from '../stubs/vscode';

const base = process.env.FLAPI_BASE_URL!;
const SERVER_TOKEN = process.env.FLAPI_CONFIG_SERVICE_TOKEN!;

const clientWith = (token?: string) =>
  createApiClient({ baseUrl: base, timeout: 30, retries: 0, verifyTls: true, authToken: token } as never);

describe('TokenStorageService', () => {
  it('keeps the token out of workspaceState', async () => {
    const ctx = fakeContext();
    const storage = new TokenStorageService(ctx as never);
    await storage.setToken(SERVER_TOKEN);
    expect(ctx.workspaceState.dump()).not.toContain(SERVER_TOKEN);
    expect(await storage.getToken()).toBe(SERVER_TOKEN);
  });

  it('migrates a token an older version left in workspaceState, then removes it', async () => {
    const ctx = fakeContext();
    await ctx.workspaceState.update('flapi.configServiceToken', SERVER_TOKEN);
    const storage = new TokenStorageService(ctx as never);
    expect(await storage.getToken()).toBe(SERVER_TOKEN);
    expect(ctx.workspaceState.dump()).not.toContain(SERVER_TOKEN);
    expect(await ctx.secrets.get('flapi.configServiceToken')).toBe(SERVER_TOKEN);
  });

  it('clearing removes it everywhere', async () => {
    const ctx = fakeContext();
    const storage = new TokenStorageService(ctx as never);
    await storage.setToken(SERVER_TOKEN);
    await storage.clearToken();
    expect(await storage.getToken()).toBeUndefined();
    expect(await storage.hasToken()).toBe(false);
  });

  it('a stored token authenticates against the real server; a wrong one is rejected', async () => {
    const ctx = fakeContext();
    const storage = new TokenStorageService(ctx as never);
    await storage.setToken(SERVER_TOKEN);
    const ok = await clientWith(await storage.getToken()).get('/api/v1/_config/project');
    expect(ok.status).toBe(200);
    await expect(clientWith('wrong-token').get('/api/v1/_config/project')).rejects.toThrow();
  });
});

import fs from 'node:fs';
import path from 'node:path';

describe('the extension never writes a credential to its logs', () => {
  // A regression guard over the source: the extension host log is readable by
  // anyone who can read the user's logs, and the token (and the whole header set
  // carrying it) was printed there when the token changed.
  const walk = (dir: string): string[] =>
    fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
      e.isDirectory() ? walk(path.join(dir, e.name)) : e.name.endsWith('.ts') ? [path.join(dir, e.name)] : [],
    );

  it('no console call prints headers, Authorization or a token value', () => {
    const offenders: string[] = [];
    for (const file of walk(path.resolve(__dirname, '../../src'))) {
      const src = fs.readFileSync(file, 'utf8');
      const calls = src.match(/console\.(log|info|debug|warn|error)\([^;]*\);/gs) ?? [];
      for (const call of calls) {
        if (/defaults\.headers|Authorization|X-Config-Token|\.substring\(token\.length/.test(call)) {
          offenders.push(`${path.relative(process.cwd(), file)}: ${call.slice(0, 100)}`);
        }
      }
    }
    expect(offenders).toEqual([]);
  });
});
