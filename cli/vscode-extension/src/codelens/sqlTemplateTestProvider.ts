import * as vscode from 'vscode';
import * as path from 'path';
import type { AxiosInstance } from 'axios';

/**
 * CodeLens provider for SQL template files
 * 
 * Adds a "Test SQL Template" button to SQL files that have associated YAML configs
 */
export class SqlTemplateTestCodeLensProvider implements vscode.CodeLensProvider {
  private _onDidChangeCodeLenses = new vscode.EventEmitter<void>();
  public readonly onDidChangeCodeLenses = this._onDidChangeCodeLenses.event;
  
  constructor(private client: AxiosInstance) {}

  /**
   * Update the Axios client (e.g., when token changes)
   */
  updateClient(client: AxiosInstance): void {
    this.client = client;
    this._onDidChangeCodeLenses.fire();
  }

  async provideCodeLenses(
    document: vscode.TextDocument,
    token: vscode.CancellationToken
  ): Promise<vscode.CodeLens[]> {
    // Only provide CodeLens for SQL files
    if (document.languageId !== 'sql') {
      return [];
    }

    // Check if this SQL file has an associated YAML config
    const yamlConfigPath = await this.findAssociatedYamlConfig(document.uri);
    
    if (!yamlConfigPath) {
      return [];
    }

    // Add a CodeLens at the top of the file
    const topOfDocument = new vscode.Range(0, 0, 0, 0);
    
    const testLens = new vscode.CodeLens(topOfDocument, {
      title: '$(beaker) Test SQL Template',
      command: 'flapi.testSqlTemplate',
      arguments: [document.uri, yamlConfigPath]
    });

    return [testLens];
  }

  /**
   * Find the associated YAML config file for a SQL template
   * 
   * Strategy:
   * 1. Check if there's a YAML file with the same basename in the same directory
   * 2. Query the backend to see if this SQL file is referenced by any endpoint
   */
  private async findAssociatedYamlConfig(sqlUri: vscode.Uri): Promise<string | undefined> {
    const sqlPath = sqlUri.fsPath;
    const sqlDir = path.dirname(sqlPath);
    const sqlBasename = path.basename(sqlPath, '.sql');

    // Strategy 1: Look for a YAML file with the same basename
    const yamlCandidates = [
      path.join(sqlDir, `${sqlBasename}.yaml`),
      path.join(sqlDir, `${sqlBasename}-rest.yaml`),
      path.join(sqlDir, `${sqlBasename}-mcp-tool.yaml`),
      path.join(sqlDir, `${sqlBasename}-mcp-resource.yaml`),
    ];

    for (const candidate of yamlCandidates) {
      try {
        await vscode.workspace.fs.stat(vscode.Uri.file(candidate));
        return candidate;
      } catch {
        // File doesn't exist, try next
      }
    }

    // Strategy 2: Query the backend
    try {
      const response = await this.client.get('/api/v1/_config/filesystem');
      const endpoints = response.data.endpoints || [];
      
      for (const endpoint of endpoints) {
        const templatePath = endpoint.template_source || endpoint.templateSource;
        if (templatePath && this.normalizePath(templatePath) === this.normalizePath(sqlPath)) {
          const configPath = endpoint.config_file_path || endpoint.configFilePath;
          if (configPath) {
            return configPath;
          }
        }
      }
    } catch (error) {
      console.error('[SqlTemplateTestCodeLensProvider] Failed to query backend:', error);
    }

    return undefined;
  }

  /**
   * Normalize path for comparison
   */
  private normalizePath(p: string): string {
    return path.normalize(p).toLowerCase();
  }
}

