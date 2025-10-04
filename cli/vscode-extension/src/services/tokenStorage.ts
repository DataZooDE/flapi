import * as vscode from 'vscode';

/**
 * Service for managing config service token storage
 */
export class TokenStorageService {
    private static readonly TOKEN_KEY = 'flapi.configServiceToken';
    private context: vscode.ExtensionContext;

    constructor(context: vscode.ExtensionContext) {
        this.context = context;
    }

    /**
     * Get the stored config service token
     */
    getToken(): string | undefined {
        return this.context.workspaceState.get<string>(TokenStorageService.TOKEN_KEY);
    }

    /**
     * Set the config service token
     */
    async setToken(token: string): Promise<void> {
        await this.context.workspaceState.update(TokenStorageService.TOKEN_KEY, token);
    }

    /**
     * Clear the stored token
     */
    async clearToken(): Promise<void> {
        await this.context.workspaceState.update(TokenStorageService.TOKEN_KEY, undefined);
    }

    /**
     * Check if a token is set
     */
    hasToken(): boolean {
        const token = this.getToken();
        return token !== undefined && token !== '';
    }
}

