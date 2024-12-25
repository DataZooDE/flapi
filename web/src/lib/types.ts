export interface ConnectionConfig {
  name: string;
  init: string;
  properties: Record<string, string>;
  log_queries: boolean;
  log_parameters: boolean;
  allow: string;
}

export interface Validator {
  type: 'string' | 'int' | 'enum' | 'date' | 'time';
  regex?: string;
  min?: number;
  max?: number;
  values?: string[];
  preventSqlInjection: boolean;
}

export interface RequestField {
  fieldName: string;
  fieldIn: 'query' | 'path' | 'header' | 'body';
  description: string;
  required: boolean;
  validators: Validator[];
}

export interface RateLimit {
  enabled: boolean;
  max: number;
  interval: number;
}

export interface Auth {
  enabled: boolean;
  type: 'basic' | 'bearer';
  users: string[];
}

export interface Cache {
  cacheTableName: string;
  cacheSource: string;
  refreshTime: string;
  refreshEndpoint: boolean;
  maxPreviousTables: number;
}

export interface Heartbeat {
  enabled: boolean;
  params: Record<string, any>;
}

export interface EndpointConfig {
  urlPath: string;
  method: 'GET' | 'POST' | 'PUT' | 'DELETE';
  requestFields: RequestField[];
  templateSource: string;
  template?: string;
  connection: string[];
  rate_limit: RateLimit;
  auth: Auth;
  cache: Cache;
  heartbeat: Heartbeat;
}

export interface SecurityConfig {
  cors: {
    allowed_origins: string[];
  };
}

export interface FlapiConfig {
  project_name: string;
  project_description: string;
  template: {
    path: string;
  };
  connections: Record<string, ConnectionConfig>;
  security?: SecurityConfig;
}

export interface ProjectConfig {
  project_name: string;
  project_description: string;
  template: {
    path: string;
    'environment-whitelist': string[];
  };
  duckdb: {
    db_path?: string;
    access_mode: 'READ_WRITE' | 'READ_ONLY';
    threads: number;
    max_memory: string;
    default_order: 'ASC' | 'DESC';
  };
  'enforce-https': {
    enabled: boolean;
    'ssl-cert-file'?: string;
    'ssl-key-file'?: string;
  };
  heartbeat: {
    enabled: boolean;
    'worker-interval': number;
  };
} 