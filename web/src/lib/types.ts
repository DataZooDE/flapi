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
  name: string;
  port: number;
  host: string;
}

export interface DuckDBSettings {
  database: string;
  options: Record<string, any>;
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
  project_name: string;
  version: string;
  server: {
    name: string;
    http_port: number;
    cache_schema: string;
  };
  duckdb_settings: Record<string, any>;
} 

export interface AwsCredentials {
  secret_id: string;
  secret_key: string;
}

export interface AwsSecretsConfig {
  region: string;
  accessKeyId: string;
  secretAccessKey: string;
  sessionToken?: string;
} 

export interface ConnectionConfig {
  type: string;
  init_sql?: string;
  properties: Record<string, any>;
  log_queries?: boolean;
  log_parameters?: boolean;
} 

export interface EndpointConfig {
  path: string;
  method: 'GET' | 'POST' | 'PUT' | 'DELETE' | 'PATCH';
  version: 'v1';
  description?: string;
  parameters: Array<{
    name: string;
    type: 'string' | 'number' | 'boolean';
    in: 'path' | 'query' | 'header';
    required: boolean;
    description?: string;
  }>;
  query: {
    sql: string;
    parameters: Array<{
      name: string;
      type: 'string' | 'number' | 'boolean';
      required: boolean;
    }>;
  };
} 