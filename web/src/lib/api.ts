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

// Add new interfaces
interface PreviewTemplateRequest {
    template: string;
    parameters: Record<string, string>;
}

interface PreviewTemplateResponse {
    sql: string;
}

// API Client class
export class APIClient {
    private baseUrl: string;
    private requestInterceptors: RequestInterceptor[] = [];
    private responseInterceptors: ResponseInterceptor[] = [];
    private authState: AuthState = { type: 'none' };

    constructor(baseUrl = '/api') {
        this.baseUrl = baseUrl;
        // Add authentication interceptor by default
        this.addRequestInterceptor(this.authInterceptor.bind(this));
    }

    // Authentication methods
    setBasicAuth(username: string, password: string) {
        this.authState = {
            type: 'basic',
            credentials: { username, password }
        };
    }

    setBearerAuth(token: string) {
        this.authState = {
            type: 'bearer',
            credentials: { token }
        };
    }

    clearAuth() {
        this.authState = { type: 'none' };
    }

    private authInterceptor(config: RequestInit): RequestInit {
        const headers = { ...(config.headers as Record<string, string> || {}) };

        switch (this.authState.type) {
            case 'basic': {
                const { username = '', password = '' } = this.authState.credentials || {};
                const base64Credentials = btoa(`${username}:${password}`);
                headers['Authorization'] = `Basic ${base64Credentials}`;
                break;
            }
            case 'bearer': {
                const { token = '' } = this.authState.credentials || {};
                headers['Authorization'] = `Bearer ${token}`;
                break;
            }
        }

        return {
            ...config,
            headers
        };
    }

    // Interceptor management
    addRequestInterceptor(interceptor: RequestInterceptor) {
        this.requestInterceptors.push(interceptor);
        return () => {
            const index = this.requestInterceptors.indexOf(interceptor);
            if (index >= 0) {
                this.requestInterceptors.splice(index, 1);
            }
        };
    }

    addResponseInterceptor(interceptor: ResponseInterceptor) {
        this.responseInterceptors.push(interceptor);
        return () => {
            const index = this.responseInterceptors.indexOf(interceptor);
            if (index >= 0) {
                this.responseInterceptors.splice(index, 1);
            }
        };
    }

    private async request<T>(
        endpoint: string,
        options: RequestInit = {}
    ): Promise<T> {
        // Initialize headers as a plain object
        const initialHeaders: Record<string, string> = {
            'Content-Type': 'application/json',
            ...(options.headers as Record<string, string> || {})
        };

        // Apply request interceptors
        let finalOptions: RequestInit = {
            ...options,
            headers: initialHeaders
        };

        for (const interceptor of this.requestInterceptors) {
            finalOptions = await interceptor(finalOptions);
            // Ensure headers remain a plain object
            if (finalOptions.headers instanceof Headers) {
                const headers: Record<string, string> = {};
                finalOptions.headers.forEach((value, key) => {
                    headers[key] = value;
                });
                finalOptions.headers = headers;
            }
        }

        let response = await fetch(`${this.baseUrl}${endpoint}`, finalOptions);

        // Apply response interceptors
        for (const interceptor of this.responseInterceptors) {
            response = await interceptor(response);
        }

        if (!response.ok) {
            let errorDetails;
            try {
                errorDetails = await response.json();
            } catch {
                errorDetails = await response.text();
            }
            throw new APIError(
                `API request failed: ${response.statusText}`,
                response.status,
                errorDetails
            );
        }

        return response.json() as Promise<T>;
    }

    // Endpoint Configuration Methods
    async getEndpointConfig(path: string): Promise<EndpointConfig> {
        return this.request<EndpointConfig>(`/config/endpoints/${path}`);
    }

    async updateEndpointConfig(
        path: string,
        config: EndpointConfig
    ): Promise<void> {
        await this.request(`/config/endpoints/${path}`, {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // Template Management Methods
    async getEndpointTemplate(path: string): Promise<{ template: string }> {
        return this.request<{ template: string }>(
            `/config/endpoints/${path}/template`
        );
    }

    async updateEndpointTemplate(
        path: string,
        template: string
    ): Promise<void> {
        await this.request(`/config/endpoints/${path}/template`, {
            method: 'PUT',
            body: JSON.stringify({ template }),
        });
    }

    // Cache Management Methods
    async getEndpointCache(path: string): Promise<CacheConfig> {
        return this.request<CacheConfig>(`/config/endpoints/${path}/cache`);
    }

    async updateEndpointCache(
        path: string,
        config: CacheConfig
    ): Promise<void> {
        await this.request(`/config/endpoints/${path}/cache`, {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // Schema Management Methods
    async getSchema(): Promise<SchemaInfo> {
        return this.request<SchemaInfo>('/config/schema');
    }

    // Authentication configuration methods
    async getAuthConfig(path: string): Promise<AuthConfig> {
        return this.request<AuthConfig>(`/config/endpoints/${path}/auth`);
    }

    async updateAuthConfig(
        path: string,
        config: AuthConfig
    ): Promise<void> {
        await this.request(`/config/endpoints/${path}/auth`, {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // AWS Secrets Manager specific methods
    async testAwsSecretsManagerConnection(
        config: AwsSecretsManagerConfig
    ): Promise<void> {
        await this.request('/config/test/aws-secrets-manager', {
            method: 'POST',
            body: JSON.stringify(config),
        });
    }

    // Project Configuration Methods
    async getProjectConfig(): Promise<ProjectConfig> {
        return this.request<ProjectConfig>('/config/project');
    }

    async updateProjectConfig(config: ProjectConfig): Promise<void> {
        await this.request('/config/project', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // Server Configuration Methods
    async getServerConfig(): Promise<ServerConfig> {
        return this.request<ServerConfig>('/config/server');
    }

    async updateServerConfig(config: ServerConfig): Promise<void> {
        await this.request('/config/server', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // DuckDB Settings Methods
    async getDuckDBSettings(): Promise<DuckDBSettings> {
        return this.request<DuckDBSettings>('/config/duckdb');
    }

    async updateDuckDBSettings(settings: DuckDBSettings): Promise<void> {
        await this.request('/config/duckdb', {
            method: 'PUT',
            body: JSON.stringify(settings),
        });
    }

    // Connection Methods with logging config
    async getConnection(name: string): Promise<ConnectionConfig> {
        return this.request<ConnectionConfig>(`/config/connections/${name}`);
    }

    async updateConnection(
        name: string, 
        config: ConnectionConfig
    ): Promise<void> {
        await this.request(`/config/connections/${name}`, {
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
            `/config/endpoints/${path}/preview`,
            {
                method: 'POST',
                body: JSON.stringify(request),
            }
        );
    }

    // Add these methods to the APIClient class
    async getAwsSecretsConfig(): Promise<AwsSecretsConfig> {
        return this.request<AwsSecretsConfig>('/config/aws-secrets');
    }

    async updateAwsSecretsConfig(config: AwsSecretsConfig): Promise<void> {
        await this.request('/config/aws-secrets', {
            method: 'PUT',
            body: JSON.stringify(config),
        });
    }

    // Add to APIClient class
    async testConnection(name: string): Promise<void> {
        await this.request(`/config/connections/${name}/test`, {
            method: 'POST'
        });
    }
}

// Create and export default instance
export const api = new APIClient(); 