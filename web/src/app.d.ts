/// <reference types="svelte" />
/// <reference types="vite/client" />

// Svelte HTML declarations
declare namespace svelte.JSX {
  interface HTMLAttributes<T> {
    'on:click_outside'?: (e: CustomEvent) => void;
  }
}

declare namespace App {
  interface Locals {}
  interface PageData {}
  interface Error {}
  interface Platform {}
}

// Export interfaces for use in components
export interface ProjectConfig {
  project_name: string;
  project_description: string;
  connections: Record<string, ConnectionConfig>;
  endpoints: Record<string, EndpointConfig>;
  server?: ServerConfig;
  duckdb_settings?: Record<string, string>;
  aws_secrets_manager?: AwsSecretsConfig;
}

export interface AwsSecretsConfig {
  secret_name: string;
  region: string;
  credentials?: {
    secret_id: string;
    secret_key: string;
  };
  init_sql?: string;
  secret_table?: string;
}

export interface ServerConfig {
  server_name?: string;
  http_port?: number;
  cache_schema?: string;
}

export interface ConnectionConfig {
  type: string;
  init_sql?: string;
  properties: Record<string, string>;
  log_queries?: boolean;
  log_parameters?: boolean;
}

export interface EndpointConfig {
  url_path: string;
  template_source: string;
  method?: string;
  version?: string;
  request?: RequestParameter[];
  connection?: string;
  cache?: CacheConfig;
  auth?: AuthConfig;
  rate_limit?: RateLimitConfig;
}

export interface RequestParameter {
  field_name: string;
  field_in: 'query' | 'path' | 'body';
  description?: string;
  required: boolean;
  type?: string;
  default?: string;
}

export interface CacheConfig {
  enabled: boolean;
  refresh_time?: string;
  cache_table_name: string;
  cache_source?: string;
}

export interface AuthConfig {
  type: 'basic' | 'bearer' | 'aws_secrets';
  properties: Record<string, string>;
}

export interface RateLimitConfig {
  requests_per_second: number;
  burst_size?: number;
} 