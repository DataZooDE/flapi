import axios, { AxiosRequestConfig, AxiosResponse } from 'axios';
import * as vscode from 'vscode';
import { ResponseInfo, RequestHistory, AuthConfig } from '../types/endpointTest';

/**
 * Service for executing HTTP requests to test endpoints
 */
export class EndpointTestService {
  private outputChannel: vscode.OutputChannel;

  constructor(outputChannel: vscode.OutputChannel) {
    this.outputChannel = outputChannel;
  }

  /**
   * Build authentication headers from auth config
   */
  buildAuthHeaders(authConfig?: AuthConfig): Record<string, string> {
    if (!authConfig || authConfig.type === 'none') {
      return {};
    }

    const headers: Record<string, string> = {};

    switch (authConfig.type) {
      case 'basic':
        if (authConfig.username && authConfig.password) {
          const credentials = Buffer.from(`${authConfig.username}:${authConfig.password}`).toString('base64');
          headers['Authorization'] = `Basic ${credentials}`;
        }
        break;
      
      case 'bearer':
        if (authConfig.token) {
          headers['Authorization'] = `Bearer ${authConfig.token}`;
        }
        break;
      
      case 'apikey':
        if (authConfig.apiKey && authConfig.headerName) {
          headers[authConfig.headerName] = authConfig.apiKey;
        }
        break;
    }

    return headers;
  }

  /**
   * Execute a test request to an endpoint
   */
  async executeRequest(
    baseUrl: string,
    urlPath: string,
    method: string,
    parameters: Record<string, string>,
    headers: Record<string, string>,
    body?: string,
    authConfig?: AuthConfig
  ): Promise<ResponseInfo> {
    const startTime = Date.now();
    
    try {
      // Build the full URL
      const url = this.buildUrl(baseUrl, urlPath);
      
      // Build auth headers
      const authHeaders = this.buildAuthHeaders(authConfig);
      
      // Prepare request configuration
      const config: AxiosRequestConfig = {
        method: method.toLowerCase() as any,
        url,
        headers: {
          'Content-Type': 'application/json',
          ...authHeaders,
          ...headers
        },
        params: parameters,
        validateStatus: () => true // Accept all status codes
      };

      // Add body for POST/PUT/PATCH requests
      if (body && ['POST', 'PUT', 'PATCH'].includes(method.toUpperCase())) {
        config.data = body;
      }

      this.outputChannel.appendLine(`[${new Date().toISOString()}] ${method} ${url}`);
      this.outputChannel.appendLine(`Parameters: ${JSON.stringify(parameters)}`);
      if (Object.keys(headers).length > 0) {
        this.outputChannel.appendLine(`Headers: ${JSON.stringify(headers)}`);
      }

      // Execute the request
      const response: AxiosResponse = await axios(config);
      
      const endTime = Date.now();
      const time = endTime - startTime;

      // Calculate response size
      const responseBody = typeof response.data === 'string' 
        ? response.data 
        : JSON.stringify(response.data, null, 2);
      const size = new Blob([responseBody]).size;

      // Extract content type
      const contentType = response.headers['content-type'] || response.headers['Content-Type'] || 'text/plain';

      // Parse cookies from Set-Cookie header
      const cookies = this.parseCookies(response.headers['set-cookie'] || response.headers['Set-Cookie']);

      const responseInfo: ResponseInfo = {
        status: response.status,
        statusText: response.statusText,
        time,
        size,
        headers: response.headers as Record<string, string>,
        body: responseBody,
        contentType,
        cookies
      };

      this.outputChannel.appendLine(`Response: ${response.status} ${response.statusText} (${time}ms, ${size} bytes)`);
      
      return responseInfo;
    } catch (error: any) {
      const endTime = Date.now();
      const time = endTime - startTime;
      
      this.outputChannel.appendLine(`Error: ${error.message}`);
      
      // Return error as response
      return {
        status: 0,
        statusText: error.message,
        time,
        size: 0,
        headers: {},
        body: JSON.stringify({
          error: error.message,
          details: error.response?.data || error.toString()
        }, null, 2)
      };
    }
  }

  /**
   * Create a request history entry
   */
  createHistoryEntry(
    parameters: Record<string, string>,
    headers: Record<string, string>,
    response: ResponseInfo,
    body?: string
  ): RequestHistory {
    return {
      timestamp: new Date(),
      parameters,
      headers,
      body,
      response
    };
  }

  /**
   * Build the full URL from base URL and path
   */
  private buildUrl(baseUrl: string, urlPath: string): string {
    // Remove trailing slash from baseUrl
    const base = baseUrl.replace(/\/$/, '');
    
    // Ensure urlPath starts with /
    const path = urlPath.startsWith('/') ? urlPath : `/${urlPath}`;
    
    return `${base}${path}`;
  }

  /**
   * Validate URL format
   */
  isValidUrl(url: string): boolean {
    try {
      new URL(url);
      return true;
    } catch {
      return false;
    }
  }

  /**
   * Format response body with syntax highlighting
   */
  formatResponseBody(body: string, contentType?: string): string {
    try {
      // Try to parse as JSON
      if (!contentType || contentType.includes('json')) {
        const parsed = JSON.parse(body);
        return JSON.stringify(parsed, null, 2);
      }
    } catch {
      // Return as-is if not JSON
    }
    
    return body;
  }

  /**
   * Get status class for UI styling
   */
  getStatusClass(status: number): string {
    if (status === 0) return 'error';
    if (status >= 200 && status < 300) return 'success';
    if (status >= 300 && status < 400) return 'redirect';
    if (status >= 400 && status < 500) return 'client-error';
    if (status >= 500) return 'server-error';
    return 'unknown';
  }

  /**
   * Format bytes to human-readable string
   */
  formatBytes(bytes: number): string {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return `${(bytes / Math.pow(k, i)).toFixed(2)} ${sizes[i]}`;
  }

  /**
   * Format milliseconds to human-readable string
   */
  formatTime(ms: number): string {
    if (ms < 1000) return `${ms}ms`;
    return `${(ms / 1000).toFixed(2)}s`;
  }

  /**
   * Parse cookies from Set-Cookie header
   */
  parseCookies(setCookieHeader?: string | string[]): Array<{ name: string; value: string; attributes?: string }> {
    if (!setCookieHeader) return [];

    const cookieStrings = Array.isArray(setCookieHeader) ? setCookieHeader : [setCookieHeader];
    const cookies: Array<{ name: string; value: string; attributes?: string }> = [];

    for (const cookieString of cookieStrings) {
      const parts = cookieString.split(';');
      const [nameValue, ...attributes] = parts;
      const [name, value] = nameValue.split('=');
      
      if (name && value) {
        cookies.push({
          name: name.trim(),
          value: value.trim(),
          attributes: attributes.map(a => a.trim()).join('; ')
        });
      }
    }

    return cookies;
  }

  /**
   * Detect content type category
   */
  getContentTypeCategory(contentType?: string): 'json' | 'html' | 'xml' | 'text' | 'image' | 'binary' {
    if (!contentType) return 'text';

    const lowerType = contentType.toLowerCase();

    if (lowerType.includes('json')) return 'json';
    if (lowerType.includes('html')) return 'html';
    if (lowerType.includes('xml')) return 'xml';
    if (lowerType.includes('image')) return 'image';
    if (lowerType.includes('text')) return 'text';
    
    return 'binary';
  }

  /**
   * Get appropriate language ID for syntax highlighting
   */
  getLanguageId(contentType?: string): string {
    if (!contentType) return 'plaintext';

    const category = this.getContentTypeCategory(contentType);
    
    switch (category) {
      case 'json': return 'json';
      case 'html': return 'html';
      case 'xml': return 'xml';
      case 'text': return 'plaintext';
      default: return 'plaintext';
    }
  }
}

