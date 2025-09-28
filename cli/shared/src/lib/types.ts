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
