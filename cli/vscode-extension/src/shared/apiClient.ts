import * as vscode from 'vscode';

export interface ValidationResult {
    valid: boolean;
    errors: string[];
    warnings: string[];
}

export interface ReloadResult {
    success: boolean;
    message: string;
}

export class FlapiApiClient {
    private baseUrl: string;
    private token: string | undefined;

    constructor(token?: string) {
        this.baseUrl = this.getBaseUrl();
        this.token = token;
    }

    private getBaseUrl(): string {
        const config = vscode.workspace.getConfiguration('flapi');
        const host = config.get<string>('host') || 'localhost';
        const port = config.get<number>('port') || 8080;
        return `http://${host}:${port}`;
    }

    /**
     * Set the authentication token
     */
    setToken(token: string | undefined) {
        this.token = token;
    }

    /**
     * Get authorization headers
     */
    private getAuthHeaders(): Record<string, string> {
        if (this.token) {
            return {
                'Authorization': `Bearer ${this.token}`,
            };
        }
        return {};
    }

    /**
     * Validate endpoint configuration YAML
     */
    async validateEndpointConfig(slug: string, yamlContent: string): Promise<ValidationResult> {
        const url = `${this.baseUrl}/api/v1/_config/endpoints/${encodeURIComponent(slug)}/validate`;
        
        const response = await fetch(url, {
            method: 'POST',
            headers: {
                ...this.getAuthHeaders(),
                'Content-Type': 'application/x-yaml',
            },
            body: yamlContent,
        });

        if (!response.ok && response.status !== 400) {
            throw new Error(`Validation request failed: ${response.statusText}`);
        }

        const result = await response.json();
        return {
            valid: result.valid ?? false,
            errors: result.errors ?? [],
            warnings: result.warnings ?? [],
        };
    }

    /**
     * Reload endpoint configuration from disk
     */
    async reloadEndpointConfig(slug: string): Promise<ReloadResult> {
        const url = `${this.baseUrl}/api/v1/_config/endpoints/${encodeURIComponent(slug)}/reload`;
        
        const response = await fetch(url, {
            method: 'POST',
            headers: this.getAuthHeaders(),
        });

        if (!response.ok) {
            const errorText = await response.text();
            throw new Error(`Reload request failed: ${errorText}`);
        }

        const result = await response.json();
        return {
            success: result.success ?? false,
            message: result.message ?? '',
        };
    }

    /**
     * Get list of endpoints
     */
    async getEndpoints(): Promise<any[]> {
        const url = `${this.baseUrl}/api/v1/_config/endpoints`;
        const response = await fetch(url, {
            headers: this.getAuthHeaders(),
        });

        if (!response.ok) {
            throw new Error(`Failed to fetch endpoints: ${response.statusText}`);
        }

        const result = await response.json();
        return result.endpoints ?? [];
    }

    /**
     * Get endpoint template content
     */
    async getTemplate(slug: string): Promise<string> {
        const url = `${this.baseUrl}/api/v1/_config/endpoints/${encodeURIComponent(slug)}/template`;
        const response = await fetch(url, {
            headers: this.getAuthHeaders(),
        });

        if (!response.ok) {
            throw new Error(`Failed to fetch template: ${response.statusText}`);
        }

        return await response.text();
    }

    /**
     * Expand template with parameters
     */
    async expandTemplate(slug: string, params: Record<string, any>): Promise<any> {
        const url = `${this.baseUrl}/api/v1/_config/endpoints/${encodeURIComponent(slug)}/template/expand`;
        
        const response = await fetch(url, {
            method: 'POST',
            headers: {
                ...this.getAuthHeaders(),
                'Content-Type': 'application/json',
            },
            body: JSON.stringify(params),
        });

        if (!response.ok) {
            throw new Error(`Failed to expand template: ${response.statusText}`);
        }

        return await response.json();
    }

    /**
     * Test template execution
     */
    async testTemplate(slug: string, params: Record<string, any>): Promise<any> {
        const url = `${this.baseUrl}/api/v1/_config/endpoints/${encodeURIComponent(slug)}/template/test`;
        
        const response = await fetch(url, {
            method: 'POST',
            headers: {
                ...this.getAuthHeaders(),
                'Content-Type': 'application/json',
            },
            body: JSON.stringify(params),
        });

        if (!response.ok) {
            throw new Error(`Failed to test template: ${response.statusText}`);
        }

        return await response.json();
    }

    /**
     * Get database schema
     */
    async getSchema(): Promise<any> {
        const url = `${this.baseUrl}/api/v1/_config/schema`;
        const response = await fetch(url, {
            headers: this.getAuthHeaders(),
        });

        if (!response.ok) {
            throw new Error(`Failed to fetch schema: ${response.statusText}`);
        }

        return await response.json();
    }

    /**
     * Get filesystem structure
     */
    async getFilesystemStructure(): Promise<any> {
        const url = `${this.baseUrl}/api/v1/_config/filesystem`;
        const response = await fetch(url, {
            headers: this.getAuthHeaders(),
        });

        if (!response.ok) {
            throw new Error(`Failed to fetch filesystem structure: ${response.statusText}`);
        }

        return await response.json();
    }
}

