import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { load } from 'js-yaml';
import type { CliOptions, FlapiiConfig } from './types';

const DEFAULT_BASE_URL = 'http://localhost:8080';
const DEFAULT_TIMEOUT = 10;
const DEFAULT_RETRIES = 2;

interface CliYamlConfig {
  cli?: {
    output?: {
      style?: 'json' | 'table';
      'json-style'?: 'camel' | 'hyphen';
    };
    http?: {
      'timeout-seconds'?: number;
      retries?: number;
      'verify-tls'?: boolean;
    };
  };
}

export function loadConfig(options: CliOptions): FlapiiConfig {
  const yamlConfig = options.config ? readConfigFile(options.config) : discoverConfig();
  const env = process.env;

  const baseUrl =
    options.baseUrl ??
    env.FLAPI_BASE_URL ??
    yamlConfig?.cli?.http?.['base-url'] ??
    DEFAULT_BASE_URL;

  const timeout = parseNumber(
    options.timeout ?? env.FLAPI_TIMEOUT ?? yamlConfig?.cli?.http?.['timeout-seconds'],
    DEFAULT_TIMEOUT,
  );

  const retries = parseNumber(
    options.retries ?? env.FLAPI_RETRIES ?? yamlConfig?.cli?.http?.retries,
    DEFAULT_RETRIES,
  );

  const verifyTls = parseBoolean(
    env.FLAPI_VERIFY_TLS ?? yamlConfig?.cli?.http?.['verify-tls'],
    !options.insecure,
  );

  return {
    configPath: yamlConfig?.configPath,
    baseUrl,
    authToken: options.authToken ?? env.FLAPI_TOKEN,
    timeout,
    retries,
    verifyTls,
    output: options.output ?? yamlConfig?.cli?.output?.style ?? 'json',
    jsonStyle: options.jsonStyle ?? yamlConfig?.cli?.output?.['json-style'] ?? 'camel',
    debugHttp: Boolean(options.debugHttp ?? false),
    quiet: Boolean(options.quiet ?? false),
    yes: Boolean(options.yes ?? false),
  };
}

function readConfigFile(filePath: string): CliYamlConfig & { configPath: string } {
  const absolute = path.resolve(filePath);
  return { configPath: absolute, ...(parseYamlFile(absolute) ?? {}) };
}

function discoverConfig(): (CliYamlConfig & { configPath: string }) | undefined {
  const envPath = process.env.FLAPI_CONFIG;
  if (envPath && fs.existsSync(envPath)) {
    return readConfigFile(envPath);
  }

  const searchDirs = [process.cwd(), ...discoverParents(process.cwd())];
  for (const dir of searchDirs) {
    const candidate = path.join(dir, 'flapi.yaml');
    if (fs.existsSync(candidate)) {
      return readConfigFile(candidate);
    }
  }

  const userConfig = path.join(os.homedir(), '.config', 'flapii', 'flapi.yaml');
  if (fs.existsSync(userConfig)) {
    return readConfigFile(userConfig);
  }

  return undefined;
}

function discoverParents(dir: string) {
  const parents: string[] = [];
  let current = path.resolve(dir);
  let parent = path.dirname(current);
  while (parent !== current) {
    parents.push(parent);
    current = parent;
    parent = path.dirname(current);
  }
  return parents;
}

function parseYamlFile(filePath: string): CliYamlConfig | undefined {
  try {
    const content = fs.readFileSync(filePath, 'utf-8');
    return load(content) as CliYamlConfig;
  } catch (error) {
    throw new Error(`Failed to read config file ${filePath}: ${(error as Error).message}`);
  }
}

function parseNumber(value: unknown, fallback: number): number {
  if (value === undefined || value === null) return fallback;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function parseBoolean(value: unknown, fallback: boolean): boolean {
  if (value === undefined || value === null) return fallback;
  if (typeof value === 'boolean') return value;
  if (typeof value === 'string') {
    const normalized = value.toLowerCase();
    if (['false', '0', 'no', 'n'].includes(normalized)) return false;
    if (['true', '1', 'yes', 'y'].includes(normalized)) return true;
  }
  return fallback;
}

