/**
 * Starts a REAL flAPI server for the CLI integration suite.
 *
 * - ConfigService is OFF by default in flAPI, so it is enabled with a token; the
 *   CLI under test is given the same token through its documented env var.
 * - The server runs against a COPY of examples/ in a temp directory. Several
 *   commands create, update and delete endpoint files; running them against the
 *   repository tree polluted it once already.
 * - Runs once, in the main process, before any test file is imported, so the
 *   env vars below exist when test modules read them (they used to be read at
 *   import time, before a beforeAll could set them).
 */
import { spawn, type ChildProcess } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

export const TEST_TOKEN = 'cli-integration-token';

let server: ChildProcess | undefined;
let workDir: string | undefined;

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

async function waitReady(baseUrl: string, timeoutMs = 90000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      const res = await fetch(`${baseUrl}/api/v1/_config/project`, {
        headers: { Authorization: `Bearer ${TEST_TOKEN}` },
      });
      if (res.ok) return;
    } catch {
      /* not up yet */
    }
    await new Promise((r) => setTimeout(r, 300));
  }
  throw new Error(`flapi did not become ready within ${timeoutMs}ms`);
}

export default async function setup() {
  const repoRoot = path.resolve(__dirname, '../../..');
  const buildType = process.env.FLAPI_BUILD_TYPE ?? 'release';
  const flapiBin = process.env.FLAPI_BIN ?? path.join(repoRoot, 'build', buildType, 'flapi');
  if (!fs.existsSync(flapiBin)) {
    throw new Error(`flapi binary not found at ${flapiBin}. Build the C++ server first (make ${buildType}).`);
  }

  workDir = fs.mkdtempSync(path.join(os.tmpdir(), 'flapii-it-'));
  const examples = path.join(workDir, 'examples');
  fs.cpSync(path.join(repoRoot, 'examples'), examples, { recursive: true });
  // flapi-test.yaml refers to './examples/data/...' relative to the cwd.
  fs.symlinkSync('.', path.join(examples, 'examples'));

  const port = await freePort();
  const baseUrl = `http://127.0.0.1:${port}`;
  const config = path.join(examples, 'flapi-test.yaml');

  server = spawn(
    flapiBin,
    ['-c', config, '--port', String(port), '--config-service', '--config-service-token', TEST_TOKEN, '--log-level', 'warning'],
    { cwd: examples, stdio: ['ignore', 'ignore', 'inherit'], env: { ...process.env, DATAZOO_DISABLE_TELEMETRY: '1' } },
  );
  server.on('exit', (code) => {
    if (code && code !== 0 && server) console.error(`flapi exited early with code ${code}`);
  });

  await waitReady(baseUrl);

  process.env.FLAPI_BASE_URL = baseUrl;
  process.env.FLAPI_CONFIG = config;
  process.env.FLAPI_CONFIG_SERVICE_TOKEN = TEST_TOKEN;
  process.env.FLAPI_IT_WORKDIR = workDir;

  return async () => {
    if (server) {
      server.kill('SIGTERM');
      await new Promise((r) => server!.once('exit', r));
      server = undefined;
    }
    if (workDir) {
      fs.rmSync(workDir, { recursive: true, force: true });
    }
  };
}
