import * as vscode from 'vscode';
import * as path from 'path';
import YAML from 'yaml';
import { pathToSlug } from '@flapi/shared';
import { FlapiApiClient } from '../shared/apiClient';

export interface ValidationError {
    line?: number;
    column?: number;
    message: string;
    severity: 'error' | 'warning';
}

export interface ValidationResult {
    valid: boolean;
    errors: string[];
    warnings: string[];
}

export class YamlValidator {
    private diagnosticCollection: vscode.DiagnosticCollection;
    private apiClient: FlapiApiClient;
    private outputChannel: vscode.OutputChannel;

    constructor(apiClient: FlapiApiClient, outputChannel: vscode.OutputChannel) {
        this.apiClient = apiClient;
        this.outputChannel = outputChannel;
        this.diagnosticCollection = vscode.languages.createDiagnosticCollection('flapi-yaml');
    }

    /**
     * Validate a YAML file and display diagnostics
     */
    async validateYamlFile(document: vscode.TextDocument): Promise<boolean> {
        // Only validate YAML files in the Flapi workspace
        if (!this.isFlapiYamlFile(document)) {
            return true;
        }

        try {
            const content = document.getText();
            const slug = this.getSlugForDocument(document);
            
            if (!slug) {
                this.outputChannel.appendLine(`Could not determine slug for ${document.uri.fsPath}`);
                return true; // Don't block save if we can't determine slug
            }

            this.outputChannel.appendLine(`Validating ${document.fileName}...`);

            // Call backend validation API
            const result = await this.apiClient.validateEndpointConfig(slug, content);

            // Clear previous diagnostics
            this.diagnosticCollection.delete(document.uri);

            if (!result.valid) {
                // Convert validation errors to VSCode diagnostics
                const diagnostics = this.convertToDiagnostics(result, document);
                this.diagnosticCollection.set(document.uri, diagnostics);

                this.outputChannel.appendLine(`❌ Validation failed for ${document.fileName}:`);
                result.errors.forEach(err => this.outputChannel.appendLine(`  - ERROR: ${err}`));
                result.warnings.forEach(warn => this.outputChannel.appendLine(`  - WARNING: ${warn}`));

                // Show validation failed message
                const action = await vscode.window.showErrorMessage(
                    `Validation failed for ${document.fileName}. See Problems panel for details.`,
                    'View Problems',
                    'Save Anyway'
                );

                if (action === 'View Problems') {
                    vscode.commands.executeCommand('workbench.actions.view.problems');
                }

                return action === 'Save Anyway';
            }

            // Log warnings even if valid
            if (result.warnings && result.warnings.length > 0) {
                const diagnostics = this.convertToDiagnostics(result, document);
                this.diagnosticCollection.set(document.uri, diagnostics);

                this.outputChannel.appendLine(`⚠️  Validation succeeded with warnings for ${document.fileName}:`);
                result.warnings.forEach(warn => this.outputChannel.appendLine(`  - ${warn}`));
            } else {
                this.outputChannel.appendLine(`✅ Validation passed for ${document.fileName}`);
            }

            return true;

        } catch (error) {
            this.outputChannel.appendLine(`Error during validation: ${error}`);
            const action = await vscode.window.showWarningMessage(
                `Could not validate ${document.fileName}: ${error}`,
                'Save Anyway',
                'Cancel'
            );
            return action === 'Save Anyway';
        }
    }

    /**
     * Reload endpoint configuration in backend after successful save
     */
    async reloadEndpointConfig(document: vscode.TextDocument): Promise<void> {
            const slug = this.getSlugForDocument(document);
            if (!slug) {
                return;
            }

        try {
            this.outputChannel.appendLine(`Reloading endpoint configuration for ${slug}...`);
            await this.apiClient.reloadEndpointConfig(slug);
            this.outputChannel.appendLine(`✅ Endpoint configuration reloaded successfully`);
        } catch (error) {
            this.outputChannel.appendLine(`⚠️  Failed to reload configuration: ${error}`);
            vscode.window.showWarningMessage(`Failed to reload configuration: ${error}`);
        }
    }

    /**
     * Convert validation result to VSCode diagnostics
     */
    private convertToDiagnostics(result: ValidationResult, document: vscode.TextDocument): vscode.Diagnostic[] {
        const diagnostics: vscode.Diagnostic[] = [];

        // Add errors
        if (result.errors) {
            for (const error of result.errors) {
                const diagnostic = this.createDiagnostic(error, vscode.DiagnosticSeverity.Error, document);
                diagnostics.push(diagnostic);
            }
        }

        // Add warnings
        if (result.warnings) {
            for (const warning of result.warnings) {
                const diagnostic = this.createDiagnostic(warning, vscode.DiagnosticSeverity.Warning, document);
                diagnostics.push(diagnostic);
            }
        }

        return diagnostics;
    }

    /**
     * Create a diagnostic from an error/warning message
     */
    private createDiagnostic(
        message: string,
        severity: vscode.DiagnosticSeverity,
        document: vscode.TextDocument
    ): vscode.Diagnostic {
        // Try to parse line/column from message if present
        // Format: "Error at line 5: message" or just "message"
        const lineMatch = message.match(/line (\d+)/i);
        const line = lineMatch ? parseInt(lineMatch[1], 10) - 1 : 0; // Convert to 0-based

        // Try to find the relevant line in the document for better highlighting
        let range: vscode.Range;
        if (line >= 0 && line < document.lineCount) {
            const lineText = document.lineAt(line);
            // Highlight the entire line, or just the non-whitespace part
            const firstNonWhitespace = lineText.firstNonWhitespaceCharacterIndex;
            range = new vscode.Range(line, firstNonWhitespace, line, lineText.range.end.character);
        } else {
            // Default to first line if we can't determine the line
            range = new vscode.Range(0, 0, 0, 0);
        }

        const diagnostic = new vscode.Diagnostic(range, message, severity);
        diagnostic.source = 'flapi';
        return diagnostic;
    }

    /**
     * Check if a document is a Flapi YAML file
     */
    private isFlapiYamlFile(document: vscode.TextDocument): boolean {
        // Check if it's a YAML file
        if (document.languageId !== 'yaml') {
            return false;
        }

        // Check if it's in the template directory
        const config = vscode.workspace.getConfiguration('flapi');
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (!workspaceFolder) {
            return false;
        }

        // Simple heuristic: YAML files in workspace that contain endpoint config keywords
        const fileName = document.fileName.toLowerCase();
        return fileName.endsWith('.yaml') || fileName.endsWith('.yml');
    }

    /**
     * Extract slug from file path
     */
    private getSlugForDocument(document: vscode.TextDocument): string | null {
        try {
            const parsed = YAML.parse(document.getText());
            if (parsed && typeof parsed === 'object') {
                const urlPath = parsed['url-path'] || parsed['urlPath'];
                if (typeof urlPath === 'string' && urlPath.length > 0) {
                    return pathToSlug(urlPath);
                }

                const mcpToolName = parsed['mcp-tool']?.name || parsed['mcpTool']?.name;
                if (typeof mcpToolName === 'string' && mcpToolName.length > 0) {
                    return mcpToolName;
                }

                const mcpResourceName = parsed['mcp-resource']?.name || parsed['mcpResource']?.name;
                if (typeof mcpResourceName === 'string' && mcpResourceName.length > 0) {
                    return mcpResourceName;
                }

                const mcpPromptName = parsed['mcp-prompt']?.name || parsed['mcpPrompt']?.name;
                if (typeof mcpPromptName === 'string' && mcpPromptName.length > 0) {
                    return mcpPromptName;
                }
            }
        } catch {
            // ignore YAML parse errors
        }

        const relative = vscode.workspace.asRelativePath(document.uri);
        const match = relative.match(/([^/]+)\.(yaml|yml)$/i);
        if (match) {
            const base = match[1];
            return pathToSlug(`/${base}`);
        }

        const basename = path.basename(document.uri.fsPath, path.extname(document.uri.fsPath));
        return basename ? pathToSlug(`/${basename}`) : null;
    }

    /**
     * Clear diagnostics for a document
     */
    clearDiagnostics(document: vscode.TextDocument): void {
        this.diagnosticCollection.delete(document.uri);
    }

    /**
     * Dispose of resources
     */
    dispose(): void {
        this.diagnosticCollection.dispose();
    }
}
