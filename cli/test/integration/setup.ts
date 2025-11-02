import { beforeAll, afterAll } from 'vitest';
import { execa, type ExecaChildProcess } from 'execa';
import net from 'node:net';
import path from 'node:path';
import fs from 'node:fs';

let serverProcess: ExecaChildProcess | undefined;

beforeAll(async () => {
  const projectRoot = path.resolve(__dirname, '../../..');
  const flapiBin = path.resolve(projectRoot, 'build', 'release', 'flapi');
  if (!fs.existsSync(flapiBin)) {
    throw new Error(`flapi binary not found at ${flapiBin}. Build the C++ server first.`);
  }

  // Generate a test config in the CLI integration folder
  const originalConfig = fs.readFileSync(path.resolve(projectRoot, 'examples', 'flapi.yaml'), 'utf-8');
  const testConfigPath = path.resolve(__dirname, 'test-flapi.yaml');
  let testConfig = originalConfig
    .replace('db_path: ./flapi_cache.db', '# db_path: ./flapi_cache.db  # in-memory for tests')
    .replace("path: './sqls'", `path: '${path.resolve(projectRoot, 'examples', 'sqls')}'`);
  
  // Disable DuckLake for integration tests to avoid file path issues
  // Replace ducklake enabled: true with enabled: false (must be directly under ducklake:)
  testConfig = testConfig.replace(
    /^(ducklake:\s*\n)\s+enabled:\s*true(\s*$|\s*#)/m,
    '$1  enabled: false  # Disabled for integration tests$2'
  );
  
  // Fix parquet file paths to be absolute (relative to project root)
  const examplesDataDir = path.resolve(projectRoot, 'examples', 'data');
  testConfig = testConfig.replace(
    /path:\s*['"]\.\/data\/([^'"]+)['"]/g,
    `path: '${examplesDataDir}/$1'`
  );
  
  // Fix northwind.sqlite path
  testConfig = testConfig.replace(
    /ATTACH IF NOT EXISTS ['"]\.\/examples\/data\/(northwind\.(?:sqlite|db))['"]/g,
    `ATTACH IF NOT EXISTS '${examplesDataDir}/$1'`
  );
  
  fs.writeFileSync(testConfigPath, testConfig);

  const port = await findFreePort();
  const baseUrl = `http://localhost:${port}`;

  // Export env for all tests
  process.env.FLAPI_BASE_URL = baseUrl;
  process.env.FLAPI_CONFIG = testConfigPath;

  serverProcess = execa(flapiBin, ['--port', String(port), '--config', testConfigPath], {
    stdout: 'inherit',
    stderr: 'inherit',
  });

  await waitForServer(baseUrl);
});

afterAll(async () => {
  if (serverProcess) {
    serverProcess.kill('SIGINT');
    await serverProcess.catch(() => undefined);
    serverProcess = undefined;
  }
});

async function findFreePort(): Promise<number> {
  return await new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.unref();
    srv.on('error', reject);
    srv.listen(0, () => {
      const address = srv.address();
      if (typeof address === 'object' && address?.port) {
        resolve(address.port);
      } else {
        reject(new Error('Failed to determine free port'));
      }
      srv.close();
    });
  });
}

async function waitForServer(target: string, timeout = 60000) {
  const start = Date.now();
  while (Date.now() - start < timeout) {
    try {
      const res = await fetch(`${target}/api/v1/_config/project`);
      if (res.ok) return;
    } catch {}
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`flapi server did not become ready within ${timeout}ms`);
}


