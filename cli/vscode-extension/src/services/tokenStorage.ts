import * as vscode from 'vscode';

/**
 * The config-service token is a credential: it lives in VS Code's SecretStorage,
 * never in workspaceState (an ordinary state file that sits beside the workspace).
 */
export class TokenStorageService {
    private static readonly TOKEN_KEY = 'flapi.configServiceToken';
    private context: vscode.ExtensionContext;

    constructor(context: vscode.ExtensionContext) {
        this.context = context;
    }

    /**
     * Get the stored config service token. A token an older version kept in
     * workspaceState is moved to SecretStorage and removed from workspaceState.
     */
    async getToken(): Promise<string | undefined> {
        const secret = await this.context.secrets.get(TokenStorageService.TOKEN_KEY);
        if (secret) {
            return secret;
        }
        const legacy = this.context.workspaceState.get<string>(TokenStorageService.TOKEN_KEY);
        if (legacy) {
            await this.context.secrets.store(TokenStorageService.TOKEN_KEY, legacy);
            await this.context.workspaceState.update(TokenStorageService.TOKEN_KEY, undefined);
            return legacy;
        }
        return undefined;
    }

    /**
     * Set the config service token
     */
    async setToken(token: string): Promise<void> {
        await this.context.secrets.store(TokenStorageService.TOKEN_KEY, token);
        // Never leave a second copy behind.
        await this.context.workspaceState.update(TokenStorageService.TOKEN_KEY, undefined);
    }

    /**
     * Clear the stored token
     */
    async clearToken(): Promise<void> {
        await this.context.secrets.delete(TokenStorageService.TOKEN_KEY);
        await this.context.workspaceState.update(TokenStorageService.TOKEN_KEY, undefined);
    }

    /**
     * Check if a token is set
     */
    async hasToken(): Promise<boolean> {
        const token = await this.getToken();
        return token !== undefined && token !== '';
    }
}
