export interface ConnectionConfig {
  init: string;
  properties: Record<string, string>;
  log_queries: boolean;
  log_parameters: boolean;
  allow: string;
}

export interface FlapiConfig {
  project_name: string;
  project_description?: string;
  template: {
    path: string;
    'environment-whitelist'?: string[];
  };
  duckdb?: {
    db_path?: string;
    access_mode?: 'READ_WRITE' | 'READ_ONLY';
    threads?: number;
    max_memory?: string;
    default_order?: 'ASC' | 'DESC';
  };
  'enforce-https'?: {
    enabled: boolean;
    'ssl-cert-file'?: string;
    'ssl-key-file'?: string;
  };
  heartbeat?: {
    enabled: boolean;
    'worker-interval'?: number;
  };
  connections?: Record<string, ConnectionConfig>;
}

export interface CacheConfig {
  enabled: boolean;
  refreshInterval?: number;
  maxAge?: number;
  template?: string;
}

export interface EndpointConfig {
  urlPath: string;
  templateSource: string;
  method?: string;
  auth?: {
    type: string;
  };
  cache?: CacheConfig;
  rate_limit?: {
    enabled: boolean;
  };
  requestFields: RequestFieldConfig[];
}

export interface RequestFieldConfig {
  fieldName: string;
  fieldIn: string;
  description: string;
  required: boolean;
} 