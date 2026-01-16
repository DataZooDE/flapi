export interface CliOptions {
  config?: string;
  baseUrl?: string;
  authToken?: string;
  configServiceToken?: string;
  timeout?: string;
  retries?: string;
  insecure?: boolean;
  output?: 'json' | 'table';
  jsonStyle?: 'camel' | 'hyphen';
  debugHttp?: boolean;
  quiet?: boolean;
  yes?: boolean;
}

export interface FlapiiConfig {
  configPath?: string;
  baseUrl: string;
  authToken?: string;
  timeout: number;
  retries: number;
  verifyTls: boolean;
  output: 'json' | 'table';
  jsonStyle: 'camel' | 'hyphen';
  debugHttp: boolean;
  quiet: boolean;
  yes: boolean;
}

export interface ApiClientConfig extends FlapiiConfig {
  headers?: Record<string, string>;
}

export interface CliContext {
  get config(): FlapiiConfig;
  get client(): import('axios').AxiosInstance;
}



export interface EndpointConfig {
  urlPath: string;
  method: string;
  templateSource: string;
  connection: string[];
  cache?: {
    enabled?: boolean;
    refreshTime?: string;
    cacheTable?: string;
    cacheSource?: string;
  };
}

// Wizard and AI-related types
export interface ParameterDefinition {
  name: string;
  type: 'string' | 'integer' | 'number' | 'boolean';
  required: boolean;
  location: 'query' | 'path' | 'body';
  description?: string;
  defaultValue?: string | number | boolean;
  validators?: ValidatorConfig[];
}

export interface ValidatorConfig {
  type: 'minLength' | 'maxLength' | 'pattern' | 'enum' | 'min' | 'max';
  value?: string | number | string[];
}

export interface WizardConfig {
  endpoint_name: string;
  url_path: string;
  description?: string;
  connection: string;
  table: string;
  method: 'GET' | 'POST' | 'PUT' | 'DELETE';
  parameters: ParameterDefinition[];
  enable_cache: boolean;
  cache_ttl?: number;
}

export interface GeminiAuthConfig {
  auth_method: 'api_key' | 'oauth';
  api_key?: string;
  oauth?: {
    access_token: string;
    refresh_token?: string;
    expiry?: number;
  };
  model?: string;
  max_tokens?: number;
}

export interface GeminiGeneratedConfig {
  endpoint_name: string;
  url_path: string;
  description: string;
  connection: string;
  table: string;
  method: 'GET' | 'POST' | 'PUT' | 'DELETE';
  parameters: ParameterDefinition[];
  enable_cache: boolean;
  cache_ttl?: number;
  reasoning?: string;
}

export interface ConnectionInfo {
  name: string;
  type: string;
  description?: string;
}

export interface TableInfo {
  name: string;
  columns: {
    name: string;
    type: string;
  }[];
}

// Project initialization types
export interface ProjectInitOptions {
  force?: boolean;
  skipValidation?: boolean;
  noExamples?: boolean;
  advanced?: boolean;
  ai?: boolean;
}

export interface ConnectionConfigDef {
  name: string;
  type: 'bigquery' | 'postgres' | 's3' | 'parquet' | 'sqlite' | 'custom';
  description?: string;
  properties: Record<string, string>;
}

export interface EndpointExampleDef {
  name: string;
  url_path: string;
  description: string;
  template_name: string;
  sql_template?: string;
}

export interface DuckDBSettings {
  max_memory?: string;
  threads?: number;
  default_order?: string;
}

export interface GeneratedProjectConfig {
  name: string;
  description: string;
  connections: ConnectionConfigDef[];
  examples: EndpointExampleDef[];
  duckdb_settings?: DuckDBSettings;
  reasoning?: string;
}

export interface ProjectInitResult {
  projectDir: string;
  filesCreated: string[];
  directoriesCreated: string[];
  validationPassed: boolean;
  warnings: string[];
}
