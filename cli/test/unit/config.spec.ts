import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { loadConfig } from '../../src/lib/config';

const originalEnv = { ...process.env };

function withTempDir(callback: (dir: string) => void) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'flapii-config-test-'));
  try {
    callback(dir);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

describe('loadConfig', () => {
  beforeEach(() => {
    process.env = { ...originalEnv };
  });

  afterEach(() => {
    process.env = { ...originalEnv };
  });

  it('uses defaults when no sources provided', () => {
    const config = loadConfig({});
    expect(config.baseUrl).toBe('http://localhost:8080');
    expect(config.timeout).toBe(10);
    expect(config.retries).toBe(2);
    expect(config.verifyTls).toBe(true);
  });

  it('prefers CLI options over env and file', () => {
    process.env.FLAPI_BASE_URL = 'http://env-host';
    process.env.FLAPI_TIMEOUT = '20';

    const config = loadConfig({ baseUrl: 'http://cli-host', timeout: '30' });
    expect(config.baseUrl).toBe('http://cli-host');
    expect(config.timeout).toBe(30);
  });

  it('loads from YAML config file with cli section', () => {
    withTempDir((dir) => {
      const yamlPath = path.join(dir, 'flapi.yaml');
      fs.writeFileSync(
        yamlPath,
        `cli:
  http:
    timeout-seconds: 15
    retries: 4
  output:
    style: table
    json-style: hyphen
`,
      );

      const config = loadConfig({ config: yamlPath });
      expect(config.timeout).toBe(15);
      expect(config.retries).toBe(4);
      expect(config.output).toBe('table');
      expect(config.jsonStyle).toBe('hyphen');
    });
  });

  it('resolves config from FLAPI_CONFIG env var', () => {
    withTempDir((dir) => {
      const yamlPath = path.join(dir, 'custom.yaml');
      fs.writeFileSync(yamlPath, 'cli:\n  http:\n    retries: 7\n');
      process.env.FLAPI_CONFIG = yamlPath;

      const config = loadConfig({});
      expect(config.retries).toBe(7);
      expect(config.configPath).toBe(yamlPath);
    });
  });

  it('interprets verifyTls based on env and flag', () => {
    process.env.FLAPI_VERIFY_TLS = 'false';
    const config = loadConfig({});
    expect(config.verifyTls).toBe(false);

    const insecure = loadConfig({ insecure: true });
    expect(insecure.verifyTls).toBe(false);
  });
});

