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
    AwsSecretsConfig
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
    private authState: AuthState = { type: 'none' };

    constructor() {
        // Load auth state from local storage
        const storedAuth = localStorage.getItem('authState');
        if (storedAuth) {
            this.authState = JSON.parse(storedAuth);
        }
    }

    // Add request interceptor
    addRequestInterceptor(interceptor: RequestInterceptor) {
        this.requestInterceptors.push(interceptor);
    }

    // Add response interceptor
    addResponseInterceptor(interceptor: ResponseInterceptor) {
        this.responseInterceptors.push(interceptor);
    }

    // Set authentication state
    setAuthState(state: AuthState) {
        this.authState = state;
        localStorage.setItem('authState', JSON.stringify(state));
    }

    // Clear authentication state
    clearAuthState() {
        this.authState = { type: 'none' };
        localStorage.removeItem('authState');
    }

    // Generic request method
    private async request<T>(
        url: string,
        options: RequestInit = {}
    ): Promise<T> {
        let requestOptions = { ...options };

        // Apply request interceptors
        for (const interceptor of this.requestInterceptors) {
            requestOptions = await interceptor(requestOptions);
        }

        // Add authentication headers
        if (this.authState.type === 'basic' && this.authState.credentials?.username && this.authState.credentials?.password) {
            const encodedCredentials = btoa(`${this.authState.credentials.username}:${this.authState.credentials.password}`);
            requestOptions.headers = {
                ...requestOptions.headers,
                'Authorization': `Basic ${encodedCredentials}`
            };
        } else if (this.authState.type === 'bearer' && this.authState.credentials?.token) {
            requestOptions.headers = {
                ...requestOptions.headers,
                'Authorization': `Bearer ${this.authState.credentials.token}`
            };
        }

        const response = await fetch(API_BASE_URL + url, requestOptions);

        // Apply response interceptors
        let interceptedResponse = response;
        for (const interceptor of this.responseInterceptors) {
            interceptedResponse = await interceptor(interceptedResponse);
        }

        if (!interceptedResponse.ok) {
            let errorDetails;
            try {
                errorDetails = await interceptedResponse.json();
            } catch (e) {
                errorDetails = await interceptedResponse.text();
            }
            throw new APIError(
                `API request failed: ${interceptedResponse.status} ${interceptedResponse.statusText}`,
                interceptedResponse.status,
                errorDetails
            );
        }

        if (interceptedResponse.status === 204) {
            return {} as T;
        }

        return await interceptedResponse.json() as T;
    }

    // Project config methods
    async getProjectConfig(): Promise<ProjectConfig> {
        return this.request<ProjectConfig>('/project');
    }

    async updateProjectConfig(config: ProjectConfig): Promise<void> {
        await this.request('/project', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // Endpoint config methods
    async getEndpointConfig(path: string): Promise<EndpointConfig> {
        return this.request<EndpointConfig>(`/endpoints/${path}`);
    }

    async updateEndpointConfig(path: string, config: EndpointConfig): Promise<void> {
        await this.request(`/endpoints/${path}`, {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    async createEndpoint(config: EndpointConfig): Promise<void> {
        await this.request(`/endpoints`, {
            method: 'POST',
            body: JSON.stringify(config),
        });
    }

    async deleteEndpoint(path: string): Promise<void> {
        await this.request(`/endpoints/${path}`, {
            method: 'DELETE'
        });
    }

    async listEndpoints(): Promise<Record<string, EndpointConfig>> {
        return this.request<Record<string, EndpointConfig>>('/endpoints');
    }

    // Template methods
    async getEndpointTemplate(path: string): Promise<{ template: string }> {
        return this.request<{ template: string }>(`/endpoints/${path}/template`);
    }

    async updateEndpointTemplate(path: string, template: string): Promise<void> {
        await this.request(`/endpoints/${path}/template`, {
            method: 'PUT',
            body: JSON.stringify({ template }),
        });
    }

    async expandEndpointTemplate(
        path: string,
        request: PreviewTemplateRequest
    ): Promise<PreviewTemplateResponse> {
        return this.request<PreviewTemplateResponse>(
            `/endpoints/${path}/template/expand`,
            {
                method: 'POST',
                body: JSON.stringify(request),
            }
        );
    }

    // Cache methods
    async getCacheConfig(path: string): Promise<CacheConfig> {
        return this.request<CacheConfig>(`/endpoints/${path}/cache`);
    }

    async updateCacheConfig(path: string, config: CacheConfig): Promise<void> {
        await this.request(`/endpoints/${path}/cache`, {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    async getCacheTemplate(path: string): Promise<{ template: string }> {
        return this.request<{ template: string }>(`/endpoints/${path}/cache/template`);
    }

    async updateCacheTemplate(path: string, template: string): Promise<void> {
        await this.request(`/endpoints/${path}/cache/template`, {
            method: 'PUT',
            body: JSON.stringify({ template }),
        });
    }

    async refreshCache(path: string): Promise<void> {
        await this.request(`/endpoints/${path}/cache/refresh`, {
            method: 'POST'
        });
    }

    // Schema methods
    async getSchema(): Promise<SchemaInfo> {
        return this.request<SchemaInfo>('/schema');
    }

    async refreshSchema(): Promise<void> {
        await this.request('/schema/refresh', {
            method: 'POST'
        });
    }

    // Auth methods
    async getAuthConfig(): Promise<AuthConfig> {
        return this.request<AuthConfig>('/auth');
    }

    async updateAuthConfig(config: AuthConfig): Promise<void> {
        await this.request('/auth', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // DuckDB settings methods
    async getDuckDBSettings(): Promise<DuckDBSettings> {
        return this.request<DuckDBSettings>('/duckdb');
    }

    async updateDuckDBSettings(settings: DuckDBSettings): Promise<void> {
        await this.request('/duckdb', {
            method: 'PUT',
            body: JSON.stringify(settings),
        });
    }

    // Server config methods
    async getServerConfig(): Promise<ServerConfig> {
        return this.request<ServerConfig>('/server');
    }

    async updateServerConfig(config: ServerConfig): Promise<void> {
        await this.request('/server', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // Connection methods
    async getConnection(name: string): Promise<ConnectionConfig> {
        return this.request<ConnectionConfig>(`/connections/${name}`);
    }

    async updateConnection(name: string, config: ConnectionConfig): Promise<void> {
        await this.request(`/connections/${name}`, {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // Add the preview method
    async previewEndpointTemplate(
        path: string,
        request: PreviewTemplateRequest
    ): Promise<PreviewTemplateResponse> {
        return this.request<PreviewTemplateResponse>(
            `/endpoints/${path}/preview`,
            {
                method: 'POST',
                body: JSON.stringify(request),
            }
        );
    }

    // Add these methods to the APIClient class
    async getAwsSecretsConfig(): Promise<AwsSecretsConfig> {
        return this.request<AwsSecretsConfig>('/aws-secrets');
    }

    async updateAwsSecretsConfig(config: AwsSecretsConfig): Promise<void> {
        await this.request('/aws-secrets', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // Add to APIClient class
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