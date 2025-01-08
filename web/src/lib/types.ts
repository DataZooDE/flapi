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
  http_port: number;
  cache_schema: string;
}

export interface ProjectConfig {
  project_name: string;
  version: string;
  server: ServerConfig;
  duckdb_settings: Record<string, any>;
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