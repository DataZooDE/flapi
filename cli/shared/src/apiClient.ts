import axios from 'axios';
import type { AxiosInstance, AxiosRequestConfig, InternalAxiosRequestConfig } from 'axios';
import { pathToSlug, slugToPath } from './lib/url';

/**
 * Configuration options for FlapiApiClient
 */
export interface FlapiApiClientOptions {
  baseURL: string;
  token?: string;
  debug?: boolean;
  timeout?: number;
}

/**
 * Response from /by-template endpoint
 */
export interface EndpointTemplateMatch {
  url_path?: string;
  method?: string;
  config_file_path: string;
  template_source: string;
  type: 'REST' | 'MCP_Tool' | 'MCP_Resource' | 'MCP_Prompt';
  mcp_name?: string;
  full_config?: any; // Full endpoint configuration
}

export interface FindByTemplateResponse {
  template_path: string;
  count: number;
  endpoints: EndpointTemplateMatch[];
}

export interface ParameterDefinition {
  name: string;
  in: string;
  description?: string;
  required: boolean;
  default?: string;
  validators?: Array<{
    type: string;
    min?: number;
    max?: number;
    regex?: string;
    allowedValues?: string[];
  }>;
}

export interface GetParametersResponse {
  endpoint: string;
  method: string;
  parameters: ParameterDefinition[];
}

export interface EnvironmentVariable {
  name: string;
  value?: string;
  available: boolean;
}

export interface GetEnvironmentVariablesResponse {
  variables: EnvironmentVariable[];
}

export interface LogLevelResponse {
  level: 'debug' | 'info' | 'warning' | 'error';
  message?: string;
}

/**
 * Centralized API client for Flapi config service
 * 
 * Features:
 * - Automatic authentication
 * - Request/response logging
 * - Typed API methods
 * - Consistent error handling
 * - Path/slug conversion utilities
 */
export class FlapiApiClient {
  private client: AxiosInstance;
  private debug: boolean;

  constructor(options: FlapiApiClientOptions) {
    this.debug = options.debug ?? false;
    
    const headers: Record<string, string> = {
      'Accept': 'application/json',
      'Content-Type': 'application/json',
    };
    
    // For config service endpoints, send both X-Config-Token and Authorization headers
    if (options.token) {
      headers['X-Config-Token'] = options.token;
      headers['Authorization'] = `Bearer ${options.token}`;
    }
    
    this.client = axios.create({
      baseURL: options.baseURL,
      timeout: options.timeout ?? 30000,
      headers
    });

    this.setupInterceptors();
  }

  /**
   * Setup request/response interceptors for logging and error handling
   */
  private setupInterceptors() {
    // Request interceptor
    this.client.interceptors.request.use(
      (config) => {
        if (this.debug) {
          const timestamp = new Date().toISOString();
          console.log(`[FlapiAPI] ${timestamp} → ${config.method?.toUpperCase()} ${config.url}`);
          
          if (config.headers) {
            const sanitized = this.sanitizeHeaders(config.headers);
            console.log('[FlapiAPI]   Headers:', JSON.stringify(sanitized, null, 2));
          }
          
          if (config.data) {
            const dataStr = JSON.stringify(config.data);
            const preview = dataStr.length > 500 ? dataStr.substring(0, 500) + '...' : dataStr;
            console.log('[FlapiAPI]   Body:', preview);
          }
        }
        return config;
      },
      (error) => {
        if (this.debug) {
          console.error('[FlapiAPI] ✗ Request error:', error.message);
        }
        return Promise.reject(error);
      }
    );

    // Response interceptor
    this.client.interceptors.response.use(
      (response) => {
        if (this.debug) {
          const timestamp = new Date().toISOString();
          const dataSize = JSON.stringify(response.data).length;
          console.log(`[FlapiAPI] ${timestamp} ← ${response.status} ${response.config.url}`);
          console.log(`[FlapiAPI]   Size: ${dataSize} bytes`);
          
          if (dataSize < 1000) {
            console.log('[FlapiAPI]   Data:', JSON.stringify(response.data, null, 2));
          }
        }
        return response;
      },
      (error) => {
        if (this.debug) {
          const timestamp = new Date().toISOString();
          console.error(`[FlapiAPI] ${timestamp} ✗ ${error.response?.status || 'ERROR'} ${error.config?.url}`);
          console.error('[FlapiAPI]   Error:', error.message);
          
          if (error.response?.data) {
            console.error('[FlapiAPI]   Response:', JSON.stringify(error.response.data, null, 2));
          }
        }
        return Promise.reject(error);
      }
    );
  }

  /**
   * Sanitize headers for logging (hide sensitive data)
   */
  private sanitizeHeaders(headers: any): any {
    const copy = { ...headers };
    if (copy.Authorization) {
      const parts = copy.Authorization.split(' ');
      if (parts.length === 2) {
        const token = parts[1];
        copy.Authorization = `${parts[0]} ${token.substring(0, 3)}***${token.substring(token.length - 4)}`;
      }
    }
    if (copy['X-Config-Token']) {
      const token = copy['X-Config-Token'];
      copy['X-Config-Token'] = `${token.substring(0, 3)}***${token.substring(token.length - 4)}`;
    }
    return copy;
  }

  /**
   * Update authentication token
   */
  setToken(token: string) {
    this.client.defaults.headers.common['Authorization'] = `Bearer ${token}`;
    if (this.debug) {
      console.log('[FlapiAPI] Token updated');
    }
  }

  /**
   * Enable/disable debug logging
   */
  enableDebug(enable = true) {
    this.debug = enable;
    if (this.debug) {
      console.log('[FlapiAPI] Debug logging enabled');
    }
  }

  /**
   * Get raw axios client for custom requests
   */
  getRawClient(): AxiosInstance {
    return this.client;
  }

  // ============================================================================
  // Typed API Methods
  // ============================================================================

  /**
   * Find all endpoints that reference a specific SQL template file
   */
  async findEndpointsByTemplate(templatePath: string): Promise<FindByTemplateResponse> {
    const response = await this.client.post<FindByTemplateResponse>(
      '/api/v1/_config/endpoints/by-template',
      { template_path: templatePath }
    );
    return response.data;
  }

  /**
   * Get endpoint configuration by URL path
   * Automatically converts path to slug
   */
  async getEndpointByPath(path: string): Promise<any> {
    const slug = pathToSlug(path);
    const response = await this.client.get(`/api/v1/_config/endpoints/${slug}`);
    return response.data;
  }

  /**
   * Get endpoint configuration by MCP name
   */
  async getEndpointByMcpName(name: string): Promise<any> {
    const response = await this.client.get(`/api/v1/_config/endpoints/${name}`);
    return response.data;
  }

  /**
   * Get parameter definitions for an endpoint
   */
  async getEndpointParameters(pathOrName: string): Promise<GetParametersResponse> {
    const slug = pathToSlug(pathOrName);
    const response = await this.client.get<GetParametersResponse>(
      `/api/v1/_config/endpoints/${slug}/parameters`
    );
    return response.data;
  }

  /**
   * Test an endpoint with given parameters
   */
  async testEndpoint(pathOrName: string, parameters: Record<string, any>): Promise<any> {
    const slug = pathToSlug(pathOrName);
    const response = await this.client.post(
      `/api/v1/_config/endpoints/${slug}/test`,
      { parameters }
    );
    return response.data;
  }

  /**
   * Get available environment variables
   */
  async getEnvironmentVariables(): Promise<GetEnvironmentVariablesResponse> {
    const response = await this.client.get<GetEnvironmentVariablesResponse>(
      '/api/v1/_config/environment-variables'
    );
    return response.data;
  }

  /**
   * Get filesystem structure
   */
  async getFilesystem(): Promise<any> {
    const response = await this.client.get('/api/v1/_config/filesystem');
    return response.data;
  }

  /**
   * Get schema information
   */
  async getSchema(format?: 'completion'): Promise<any> {
    const url = format 
      ? `/api/v1/_config/schema?format=${format}`
      : '/api/v1/_config/schema';
    const response = await this.client.get(url);
    return response.data;
  }

  /**
   * Get current log level
   */
  async getLogLevel(): Promise<LogLevelResponse> {
    const response = await this.client.get<LogLevelResponse>('/api/v1/_config/log-level');
    return response.data;
  }

  /**
   * Set log level at runtime
   */
  async setLogLevel(level: 'debug' | 'info' | 'warning' | 'error'): Promise<LogLevelResponse> {
    const response = await this.client.put<LogLevelResponse>(
      '/api/v1/_config/log-level',
      { level }
    );
    return response.data;
  }

  /**
   * Validate endpoint configuration
   */
  async validateEndpoint(pathOrName: string, yamlContent: string): Promise<any> {
    const slug = pathToSlug(pathOrName);
    const response = await this.client.post(
      `/api/v1/_config/endpoints/${slug}/validate`,
      yamlContent,
      { headers: { 'Content-Type': 'text/plain' } }
    );
    return response.data;
  }

  /**
   * Reload endpoint configuration from disk
   */
  async reloadEndpoint(pathOrName: string): Promise<any> {
    const slug = pathToSlug(pathOrName);
    const response = await this.client.post(`/api/v1/_config/endpoints/${slug}/reload`);
    return response.data;
  }

  /**
   * List all endpoints
   */
  async listEndpoints(): Promise<any> {
    const response = await this.client.get('/api/v1/_config/endpoints');
    return response.data;
  }
}

/**
 * Create a pre-configured Flapi API client
 */
export function createFlapiClient(options: FlapiApiClientOptions): FlapiApiClient {
  return new FlapiApiClient(options);
}

// Export types for convenience
export type { AxiosInstance, AxiosRequestConfig, InternalAxiosRequestConfig };

