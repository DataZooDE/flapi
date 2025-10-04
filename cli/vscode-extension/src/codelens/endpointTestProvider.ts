import * as vscode from 'vscode';
import * as yaml from 'yaml';
import type { AxiosInstance } from 'axios';

/**
 * CodeLens provider to show "Test Endpoint" button above url-path in YAML files
 */
export class EndpointTestCodeLensProvider implements vscode.CodeLensProvider {
  private _onDidChangeCodeLenses: vscode.EventEmitter<void> = new vscode.EventEmitter<void>();
  public readonly onDidChangeCodeLenses: vscode.Event<void> = this._onDidChangeCodeLenses.event;
  private client: AxiosInstance;

  constructor(client: AxiosInstance) {
    this.client = client;
  }

  /**
   * Update the API client (e.g., when token changes)
   */
  updateClient(client: AxiosInstance): void {
    this.client = client;
  }

  /**
   * Simple check if YAML contains extended syntax ({{include}} directives)
   */
  private hasExtendedSyntax(text: string): boolean {
    return text.includes('{{include');
  }

  /**
   * Strip extended YAML syntax for basic parsing
   * This allows us to detect url-path even with {{include}} directives
   */
  private stripExtendedSyntax(text: string): string {
    // Remove {{include...}} directives but keep comments
    return text.replace(/\{\{include[^}]*\}\}/g, '');
  }

  /**
   * Trigger a refresh of all code lenses
   */
  refresh(): void {
    this._onDidChangeCodeLenses.fire();
  }

  /**
   * Provide code lenses for a document
   */
  async provideCodeLenses(
    document: vscode.TextDocument,
    token: vscode.CancellationToken
  ): Promise<vscode.CodeLens[]> {
    const codeLenses: vscode.CodeLens[] = [];

    // Only process YAML files
    if (document.languageId !== 'yaml') {
      return codeLenses;
    }

    try {
      const text = document.getText();
      
      // Strip extended syntax for detection purposes
      const strippedText = this.stripExtendedSyntax(text);
      
      let parsed;
      try {
        parsed = yaml.parse(strippedText);
      } catch (parseError) {
        // If parsing still fails, just look for url-path in the raw text
        const hasUrlPath = text.includes('url-path:') || text.includes('urlPath:');
        if (hasUrlPath) {
          const urlPathLine = this.findYamlKeyLine(document, 'url-path') || 
                              this.findYamlKeyLine(document, 'urlPath');
          
          if (urlPathLine !== undefined) {
            const range = new vscode.Range(urlPathLine, 0, urlPathLine, 0);
            const testCommand: vscode.Command = {
              title: '▶️ Test Endpoint',
              command: 'flapi.testEndpoint',
              arguments: [document.uri, urlPathLine]
            };
            codeLenses.push(new vscode.CodeLens(range, testCommand));
          }
        }
        return codeLenses;
      }

      // Check if this is an endpoint configuration
      if (!parsed || typeof parsed !== 'object') {
        return codeLenses;
      }

      const isRestEndpoint = 'url-path' in parsed || 'urlPath' in parsed;
      const isMcpTool = 'mcp-tool' in parsed || 'mcpTool' in parsed;
      const isMcpResource = 'mcp-resource' in parsed || 'mcpResource' in parsed;

      // Only show CodeLens for REST endpoints
      if (!isRestEndpoint) {
        return codeLenses;
      }

      // Find the line with url-path
      const urlPathLine = this.findYamlKeyLine(document, 'url-path') || 
                          this.findYamlKeyLine(document, 'urlPath');

      if (urlPathLine !== undefined) {
        const range = new vscode.Range(urlPathLine, 0, urlPathLine, 0);
        
        // Create "Test Endpoint" CodeLens
        const testCommand: vscode.Command = {
          title: '▶️ Test Endpoint',
          command: 'flapi.testEndpoint',
          arguments: [document.uri, urlPathLine]
        };

        codeLenses.push(new vscode.CodeLens(range, testCommand));
      }
    } catch (error) {
      // Silently ignore parse errors - file might be invalid/incomplete
      console.error('Failed to parse YAML for CodeLens:', error);
    }

    return codeLenses;
  }

  /**
   * Find the line number of a specific YAML key
   */
  private findYamlKeyLine(document: vscode.TextDocument, key: string): number | undefined {
    const text = document.getText();
    const lines = text.split('\n');

    for (let i = 0; i < lines.length; i++) {
      const line = lines[i].trim();
      if (line.startsWith(`${key}:`)) {
        return i;
      }
    }

    return undefined;
  }

  /**
   * Extract endpoint configuration from document
   * For files with extended syntax, we need to use the backend to parse them
   */
  async getEndpointConfig(document: vscode.TextDocument): Promise<any> {
    try {
      const text = document.getText();
      
      // Check if this file uses extended YAML syntax
      if (this.hasExtendedSyntax(text)) {
        // For extended YAML, we need to get the parsed config from the backend
        // We'll fetch it via the filesystem API which uses ExtendedYamlParser
        return await this.getEndpointConfigFromBackend(document);
      }
      
      // For standard YAML, parse directly
      const parsed = yaml.parse(text);
      return parsed;
    } catch (error) {
      throw new Error(`Failed to parse YAML: ${error}`);
    }
  }

  /**
   * Get endpoint configuration from backend (for extended YAML files)
   */
  private async getEndpointConfigFromBackend(document: vscode.TextDocument): Promise<any> {
    try {
      // Get the file path relative to workspace
      const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
      if (!workspaceFolder) {
        throw new Error('No workspace folder found');
      }

      const relativePath = vscode.workspace.asRelativePath(document.uri, false);
      
      // Use the authenticated client to get filesystem data
      const response = await this.client.get('/api/v1/_config/filesystem');
      
      // Find this file in the tree and return its parsed content
      // For now, we'll parse the stripped version locally
      // TODO: Add a dedicated backend endpoint for parsing single YAML files
      
      const text = document.getText();
      const strippedText = this.stripExtendedSyntax(text);
      return yaml.parse(strippedText);
      
    } catch (error) {
      // Fallback to stripped parsing
      console.warn('Failed to get config from backend, using local parsing:', error);
      const text = document.getText();
      const strippedText = this.stripExtendedSyntax(text);
      return yaml.parse(strippedText);
    }
  }
}

