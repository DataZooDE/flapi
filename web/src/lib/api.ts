import type { 
    EndpointConfig, 
    CacheConfig, 
    SchemaInfo,
    AuthConfig,
    AwsSecretsManagerConfig,
    ProjectConfig,
    ServerConfig,
    DuckDBSettings,
    ConnectionConfig,
    AwsSecretsConfig,
    HeartbeatConfig,
    EnforceHTTPSConfig
} from '$lib/types';

// Types for interceptors
export type RequestInterceptor = (config: RequestInit) => RequestInit | Promise<RequestInit>;
export type ResponseInterceptor = (response: Response) => Response | Promise<Response>;

// Authentication state interface
interface AuthState {
    type: 'basic' | 'bearer' | 'none';
    credentials?: {
        username?: string;
        password?: string;
        token?: string;
    };
}

// Error class for API responses
export class APIError extends Error {
    constructor(
        message: string,
        public status: number,
        public details?: unknown
    ) {
        super(message);
        this.name = 'APIError';
    }
}

// Re-export types for convenience
export type { EndpointConfig, CacheConfig, SchemaInfo, AuthConfig };

// Default API base URL
const API_BASE_URL = '/api/v1/_config';

// API Client class
class APIClient {
    private requestInterceptors: RequestInterceptor[] = [];
    private responseInterceptors: ResponseInterceptor[] = [];

    addRequestInterceptor(interceptor: RequestInterceptor) {
        this.requestInterceptors.push(interceptor);
    }

    addResponseInterceptor(interceptor: ResponseInterceptor) {
        this.responseInterceptors.push(interceptor);
    }

    private async request<T>(url: string, options: RequestInit = {}): Promise<T> {
        const requestUrl = API_BASE_URL + url;
        let modifiedOptions = options;

        // Apply request interceptors
        for (const interceptor of this.requestInterceptors) {
            modifiedOptions = await interceptor(modifiedOptions);
        }

        const response = await fetch(requestUrl, modifiedOptions);

        if (!response.ok) {
            let errorDetails;
            try {
                errorDetails = await response.json();
            } catch (e) {
                errorDetails = await response.text();
            }
            throw new APIError(`API request failed with status ${response.status}`, response.status, errorDetails);
        }

        let responseData = await response.json();

        // Apply response interceptors
        for (const interceptor of this.responseInterceptors) {
            const modifiedResponse = await interceptor(response);
            if (modifiedResponse instanceof Response) {
                responseData = await modifiedResponse.json();
            }
        }

        // Transform response data to use underscores instead of hyphens
        if (typeof responseData === 'object' && responseData !== null) {
            responseData = this.transformKeys(responseData);
        }

        return responseData as T;
    }

    private transformKeys(obj: any): any {
        if (typeof obj !== 'object' || obj === null) {
            return obj;
        }

        if (Array.isArray(obj)) {
            return obj.map(item => this.transformKeys(item));
        }

        const transformedObj: any = {};
        for (const key in obj) {
            if (Object.prototype.hasOwnProperty.call(obj, key)) {
                const transformedKey = key.replace(/-/g, '_');
                transformedObj[transformedKey] = this.transformKeys(obj[key]);
            }
        }
        return transformedObj;
    }
    
    async getProjectConfig(): Promise<ProjectConfig> {
        return this.request<ProjectConfig>('/project');
    }

    async updateProjectConfig(config: ProjectConfig): Promise<void> {
        await this.request('/project', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }
    
    async getEndpointConfig(path: string): Promise<EndpointConfig> {
        return this.request<EndpointConfig>(`/endpoints/${path}`);
    }

    async updateEndpointConfig(path: string, config: EndpointConfig): Promise<void> {
        await this.request(`/endpoints/${path}`, {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    async getConnection(name: string): Promise<ConnectionConfig> {
        return this.request<ConnectionConfig>(`/connections/${name}`);
    }

    async updateConnection(name: string, config: ConnectionConfig): Promise<void> {
        await this.request(`/connections/${name}`, {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    async getDuckDBSettings(): Promise<{ settings: Record<string, string>; db_path: string }> {
        return this.request<{ settings: Record<string, string>; db_path: string }>('/duckdb');
    }

    async updateDuckDBSettings(config: { settings: Record<string, string>; db_path: string }): Promise<void> {
        await this.request('/duckdb', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    async getServerConfig(): Promise<ServerConfig> {
        return this.request<ServerConfig>('/server');
    }

    async updateServerConfig(config: ServerConfig): Promise<void> {
        await this.request('/server', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    async getAwsSecretsConfig(): Promise<AwsSecretsConfig> {
        return this.request<AwsSecretsConfig>('/aws-secrets');
    }

    async updateAwsSecretsConfig(config: AwsSecretsConfig): Promise<void> {
        await this.request('/aws-secrets', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    async testConnection(name: string): Promise<void> {
        await this.request(`/connections/${name}/test`, {
            method: 'POST'
        });
    }

    async testAwsSecretsManagerConnection(config: AwsSecretsConfig): Promise<void> {
        await this.request('/aws-secrets/test', {
            method: 'POST',
            body: JSON.stringify(config)
        });
    }

    // Heartbeat methods
    async getHeartbeatConfig(): Promise<HeartbeatConfig> {
        return this.request<HeartbeatConfig>('/heartbeat');
    }

    async updateHeartbeatConfig(config: HeartbeatConfig): Promise<void> {
        await this.request('/heartbeat', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // Enforce HTTPS methods
    async getEnforceHTTPSConfig(): Promise<EnforceHTTPSConfig> {
        return this.request<EnforceHTTPSConfig>('/enforce-https');
    }

    async updateEnforceHTTPSConfig(config: EnforceHTTPSConfig): Promise<void> {
        await this.request('/enforce-https', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }
}

// Create and export default instance
export const api = new APIClient();

// Define types for preview requests and responses
export interface PreviewTemplateRequest {
    parameters: Record<string, string>;
}

export interface PreviewTemplateResponse {
    result: string;
} 