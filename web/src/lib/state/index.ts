import { writable } from 'svelte/store';
import type { 
  ProjectConfig, 
  ServerConfig, 
  DuckDBSettings,
  AwsSecretsConfig,
  ConnectionConfig,
  EndpointConfig
} from '$lib/types';

// Create stores with initial values
export const projectConfig = writable<ProjectConfig>({
  project_name: '',
  version: '1.0.0',
  server: {
    name: '',
    http_port: 8080,
    cache_schema: 'cache'
  },
  duckdb_settings: {}
});

export const serverConfig = writable<ServerConfig>({
  name: '',
  port: 8080,
  host: 'localhost'
});

export const duckdbConfig = writable<DuckDBSettings>({
  database: ':memory:',
  options: {}
});

export const awsSecretsConfig = writable<AwsSecretsConfig>({
  region: '',
  accessKeyId: '',
  secretAccessKey: '',
  sessionToken: undefined
});

export const connections = writable<Record<string, ConnectionConfig>>({});

export const endpoints = writable<Record<string, EndpointConfig>>({}); 