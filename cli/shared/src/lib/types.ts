export interface CliOptions {
  config?: string;
  baseUrl?: string;
  authToken?: string;
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



export interface OperationConfig {
  type?: 'read' | 'write';
  validate_before_write?: boolean;
  returns_data?: boolean;
  transaction?: boolean;
}

export interface EndpointConfig {
  urlPath: string;
  method: string;
  templateSource: string;
  connection: string[];
  operation?: OperationConfig;
  cache?: {
    enabled?: boolean;
    refreshTime?: string;
    cacheTable?: string;
    cacheSource?: string;
  };
}

export interface WriteOperationResponse {
  rows_affected?: number;
  returned_data?: any[];
  last_insert_id?: number;
  data?: any[]; // For read operations or write operations that return data
  next?: string;
  total_count?: number;
  error?: {
    field?: string;
    message?: string;
  };
  errors?: Array<{
    field: string;
    message: string;
  }>;
}

export interface ValidationResult {
  valid: boolean;
  errors: string[];
  warnings: string[];
}

export interface ReloadResult {
  success: boolean;
  message: string;
}
