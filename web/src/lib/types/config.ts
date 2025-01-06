export interface ProjectConfig {
  project_name: string;
  project_description: string;
  connections: Record<string, ConnectionConfig>;
  endpoints: Record<string, EndpointConfig>;
  server?: ServerConfig;
  duckdb_settings?: Record<string, string>;
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
  method?: string;
  version?: string;
  template_source: string;
  request?: RequestParameter[];
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

// Type guards for runtime validation
export function isProjectConfig(obj: any): obj is ProjectConfig {
  return (
    obj &&
    typeof obj === 'object' &&
    (!obj.server || typeof obj.server === 'object') &&
    (!obj.duckdb_settings || typeof obj.duckdb_settings === 'object')
  );
}

export function isEndpointConfig(obj: any): obj is EndpointConfig {
  return (
    obj &&
    typeof obj === 'object' &&
    typeof obj.url_path === 'string' &&
    typeof obj.template_source === 'string' &&
    (!obj.method || typeof obj.method === 'string') &&
    (!obj.version || typeof obj.version === 'string') &&
    (!obj.request || Array.isArray(obj.request))
  );
}

export function isAuthConfig(obj: any): obj is AuthConfig {
  if (!obj || typeof obj !== 'object') return false;
  
  if (!['basic', 'bearer', 'aws_secrets'].includes(obj.type)) return false;
  
  if (typeof obj.properties !== 'object' || !obj.properties) return false;

  if (obj.type === 'aws_secrets') {
    const props = obj.properties;
    return !!(props.secret_name && props.region);
  }

  return true;
}

// Validation helpers
export function validateProjectConfig(config: any): string[] {
  const errors: string[] = [];
  
  // First check if it's a valid object structure
  if (!config || typeof config !== 'object') {
    errors.push('Invalid project configuration structure');
    return errors;
  }

  // Check required fields
  if (!config.project_name || typeof config.project_name !== 'string') {
    errors.push('Missing project name');
  }
  if (!config.project_description || typeof config.project_description !== 'string') {
    errors.push('Missing project description');
  }
  if (!config.connections || typeof config.connections !== 'object') {
    errors.push('Missing connections configuration');
  }
  if (!config.endpoints || typeof config.endpoints !== 'object') {
    errors.push('Missing endpoints configuration');
  }

  // Validate server configuration if present
  if (config.server) {
    if (config.server.http_port && typeof config.server.http_port !== 'number') {
      errors.push('Server http_port must be a number');
    }
    if (config.server.server_name && typeof config.server.server_name !== 'string') {
      errors.push('Server name must be a string');
    }
  }

  return errors;
}

export function validateEndpointConfig(config: any): string[] {
  const errors: string[] = [];

  if (!isEndpointConfig(config)) {
    errors.push('Invalid endpoint configuration structure');
    return errors;
  }

  if (!config.url_path.startsWith('/')) {
    errors.push('URL path must start with /');
  }

  if (config.cache) {
    if (typeof config.cache.enabled !== 'boolean') {
      errors.push('Cache enabled must be a boolean');
    }
    if (config.cache.enabled && !config.cache.cache_table_name) {
      errors.push('Cache table name is required when cache is enabled');
    }
  }

  if (config.request) {
    config.request.forEach((param: any, index: number) => {
      if (!param.field_name) {
        errors.push(`Request parameter at index ${index} missing field_name`);
      }
      if (!['query', 'path', 'body'].includes(param.field_in)) {
        errors.push(`Invalid field_in value for parameter ${param.field_name}`);
      }
    });
  }

  return errors;
}

export function validateConnectionConfig(config: any): string[] {
  const errors: string[] = [];

  if (!config || typeof config !== 'object') {
    errors.push('Invalid connection configuration structure');
    return errors;
  }

  if (!config.type || typeof config.type !== 'string') {
    errors.push('Connection type is required');
  }

  if (!config.properties || typeof config.properties !== 'object') {
    errors.push('Connection properties must be an object');
  }

  if (config.log_queries !== undefined && typeof config.log_queries !== 'boolean') {
    errors.push('log_queries must be a boolean');
  }

  if (config.log_parameters !== undefined && typeof config.log_parameters !== 'boolean') {
    errors.push('log_parameters must be a boolean');
  }

  if (config.init_sql !== undefined && typeof config.init_sql !== 'string') {
    errors.push('init_sql must be a string');
  }

  return errors;
}

export function validateCacheConfig(config: any): string[] {
  const errors: string[] = [];

  if (!config || typeof config !== 'object') {
    errors.push('Invalid cache configuration structure');
    return errors;
  }

  if (typeof config.enabled !== 'boolean') {
    errors.push('Cache enabled must be a boolean');
  }

  if (config.enabled && !config.cache_table_name) {
    errors.push('Cache table name is required when cache is enabled');
  }

  if (config.refresh_time && !isValidRefreshTime(config.refresh_time)) {
    errors.push('Invalid refresh time format');
  }

  return errors;
}

function isValidRefreshTime(time: string): boolean {
  // Format: <number><unit> where unit is s, m, h, or d
  const regex = /^\d+[smhd]$/;
  return regex.test(time);
}

export function validateAwsSecretsConfig(config: any): string[] {
  const errors: string[] = [];

  if (!config || typeof config !== 'object') {
    errors.push('Invalid AWS secrets configuration structure');
    return errors;
  }

  if (!config.secret_name || typeof config.secret_name !== 'string') {
    errors.push('Missing secret name');
  }

  if (!config.region || typeof config.region !== 'string') {
    errors.push('Missing region');
  }

  if (config.credentials) {
    if (!config.credentials.secret_id || !config.credentials.secret_id.startsWith('AKIA')) {
      errors.push('Invalid AWS access key format');
    }
    if (!config.credentials.secret_key || config.credentials.secret_key.length === 0) {
      errors.push('Secret key cannot be empty');
    }
  }

  return errors;
} 