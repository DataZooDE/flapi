// Common types used across the application
export interface RequestParameter {
    'field-name': string;
    'field-in': 'query' | 'path' | 'header' | 'body';
    description?: string;
    required?: boolean;
    type?: string;
    format?: string;
} 

export interface SchemaColumn {
  type: string;
  nullable: boolean;
}

export interface SchemaTable {
  is_view: boolean;
  columns: Record<string, SchemaColumn>;
}

export interface SchemaInfo {
  [schema: string]: {
    tables: Record<string, SchemaTable>;
  };
}

export interface SchemaState {
  data: SchemaInfo | null;
  isLoading: boolean;
  error: string | null;
} 

export interface ServerConfig {
  hostname: string;
  http_port: number;
}

export interface DuckDBSettings {
    settings: Record<string, string>;
    cache_schema: string;
    db_path: string;
}

export interface AuthConfig {
    enabled: boolean;
    type: 'basic' | 'none';
    users?: { username: string; password: string }[];
}

export interface HeartbeatConfig {
    enabled: boolean;
    worker_interval: number;
}

export interface EnforceHTTPSConfig {
    enabled: boolean;
    ssl_cert_file: string;
    ssl_key_file: string;
}

export interface ProjectConfig {
  connections: Record<string, ConnectionConfig>;
  server: ServerConfig;
  duckdb: DuckDBSettings;
  heartbeat: HeartbeatConfig;
  enforce_https: EnforceHTTPSConfig;
  template_path: string;
  description: string;
  name: string;
} 

export interface AwsCredentials {
  secret_id: string;
  secret_key: string;
}

export interface AwsSecretsConfig {
  secret_name: string;
  region: string;
  credentials: AwsCredentials;
  init_sql: string;
  secret_table: string;
} 

export interface ConnectionConfig {
  type: string;
  init_sql: string;
  properties: Record<string, string>;
  log_queries: boolean;
  log_parameters: boolean;
} 