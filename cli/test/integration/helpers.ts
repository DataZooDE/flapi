import path from 'node:path';
import { execa } from 'execa';

export const cliPath = path.resolve('dist', 'index.js');

/** Run the built `flapii` against the real server started by global-setup. */
export async function flapii(args: string[], env: Record<string, string | undefined> = {}) {
  return await execa('node', [cliPath, ...args], {
    reject: false,
    env: {
      ...process.env,
      FLAPI_BASE_URL: process.env.FLAPI_BASE_URL,
      FLAPI_CONFIG: process.env.FLAPI_CONFIG,
      FLAPI_CONFIG_SERVICE_TOKEN: process.env.FLAPI_CONFIG_SERVICE_TOKEN,
      ...env,
    },
  });
}

export const TOKEN = process.env.FLAPI_CONFIG_SERVICE_TOKEN ?? '';

import { spawn, type ChildProcess } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';

async function freePort(): Promise<number> {
  return await new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.unref();
    srv.on('error', reject);
    srv.listen(0, () => {
      const address = srv.address();
      srv.close(() => (typeof address === 'object' && address?.port ? resolve(address.port) : reject(new Error('no port'))));
    });
  });
}

/** A minimal REAL flapi server, with or without the config service. */
export async function startFlapi(opts: { configService: boolean; token?: string }) {
  const repoRoot = path.resolve(__dirname, '../../..');
  const bin = process.env.FLAPI_BIN ?? path.join(repoRoot, 'build', process.env.FLAPI_BUILD_TYPE ?? 'release', 'flapi');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'flapii-min-'));
  fs.mkdirSync(path.join(dir, 'sqls'));
  fs.writeFileSync(
    path.join(dir, 'flapi.yaml'),
    "project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n" +
      "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n",
  );
  const port = await freePort();
  const args = ['-c', path.join(dir, 'flapi.yaml'), '--port', String(port), '--log-level', 'warning'];
  if (opts.configService) args.push('--config-service', '--config-service-token', opts.token ?? 'min-token');
  const proc: ChildProcess = spawn(bin, args, { cwd: dir, stdio: 'ignore', env: { ...process.env, DATAZOO_DISABLE_TELEMETRY: '1' } });
  const url = `http://127.0.0.1:${port}`;
  for (let i = 0; i < 300; i++) {
    try {
      if ((await fetch(`${url}/health/live`)).ok) break;
    } catch {
      /* starting */
    }
    await new Promise((r) => setTimeout(r, 200));
  }
  return {
    url,
    stop: async () => {
      proc.kill('SIGTERM');
      await new Promise((r) => proc.once('exit', r));
      fs.rmSync(dir, { recursive: true, force: true });
    },
  };
}
