import * as vscode from 'vscode';
import { createApiClient } from '@flapi/shared';
import { buildEndpointUrl, pathToSlug } from '@flapi/shared';
import { VirtualDocumentManager } from '../providers/virtualDocuments';

/**
 * Manages the multi-document workspace for endpoint editing
 */
export class EndpointWorkspace {
  private diagnostics: vscode.DiagnosticCollection;
  private outputChannel: vscode.OutputChannel;

  constructor(
    private client: ReturnType<typeof createApiClient>,
    private virtualDocuments: VirtualDocumentManager
  ) {
    this.diagnostics = vscode.languages.createDiagnosticCollection('flapi');
    this.outputChannel = vscode.window.createOutputChannel('Flapi Results');
  }

  public updateClient(client: ReturnType<typeof createApiClient>): void {
    this.client = client;
  }

  private resolveFsPath(sourcePath: string): vscode.Uri {
    if (sourcePath.startsWith('/') || /^[a-zA-Z]:\\/.test(sourcePath)) {
      return vscode.Uri.file(sourcePath);
    }
    const root = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? process.cwd();
    const path = require('path').join(root, sourcePath);
    return vscode.Uri.file(path);
  }

  private async resolveTemplateSource(endpointPath: string): Promise<string | undefined> {
    try {
      const cfgResp = await this.client.get(buildEndpointUrl(endpointPath));
      const ts = cfgResp.data?.templateSource as string | undefined;
      return ts && typeof ts === 'string' && ts.length > 0 ? ts : undefined;
    } catch {
      return undefined;
    }
  }

  private async loadAutoParameters(endpointPath: string): Promise<Record<string, unknown>> {
    // Try inline comment in template file: /* flapi:params { ... } */ or -- flapi:params { ... }
    try {
      const templateSource = await this.resolveTemplateSource(endpointPath);
      if (templateSource) {
        const uri = this.resolveFsPath(templateSource);
        const contentBytes = await vscode.workspace.fs.readFile(uri);
        const content = Buffer.from(contentBytes).toString('utf8');
        const head = content.slice(0, 4096);

        // SQL-compatible inline line comment only: -- flapi:params { ... }
        const lineMatch = head.match(/^[\t ]*--\s*flapi:params\s*({.*})/m);
        const jsonText = lineMatch?.[1]?.trim();
        if (jsonText) {
          try {
            const obj = JSON.parse(jsonText);
            if (obj && typeof obj === 'object') {
              return obj as Record<string, unknown>;
            }
          } catch {
            // ignore parse errors
          }
        }

        // Sidecar <template>.vars.json
        const sidecar = vscode.Uri.file(uri.fsPath + '.vars.json');
        try {
          const scBytes = await vscode.workspace.fs.readFile(sidecar);
          const scText = Buffer.from(scBytes).toString('utf8');
          const obj = JSON.parse(scText);
          if (obj && typeof obj === 'object') {
            return obj as Record<string, unknown>;
          }
        } catch {
          // ignore missing/parse errors
        }
      }
    } catch {
      // ignore
    }
    return {};
  }

  private async getEffectiveParameters(endpointPath: string, slug: string): Promise<Record<string, unknown>> {
    const manual = this.virtualDocuments.getParametersProvider().getParameters(slug) ?? {};
    if (manual && Object.keys(manual).length > 0) {
      return manual;
    }
    const autoParams = await this.loadAutoParameters(endpointPath);
    return autoParams;
  }

  /**
   * Opens an endpoint in multi-document split view
   */
  async openEndpoint(path: string): Promise<void> {
    try {
      const slug = pathToSlug(path);
      // Fetch endpoint to resolve file sources
      let templateSource: string | undefined;
      let cacheTemplateSource: string | undefined;
      try {
        const cfg = await this.client.get(buildEndpointUrl(path));
        templateSource = (cfg.data?.templateSource as string | undefined) ?? undefined;
        const cacheConfig = cfg.data?.cache ?? {};
        cacheTemplateSource =
          cacheConfig?.templateFile ||
          cacheConfig?.['template-file'] ||
          cacheConfig?.templateSource ||
          undefined;
      } catch {
        // ignore; fall back to virtual docs
      }
      
      // Create URIs for all endpoint documents (prefer file-based if available)
      const configUri = vscode.Uri.parse(`flapi://endpoint/${slug}/config.json`);
      const templateUri = templateSource ? this.resolveFsPath(templateSource) : vscode.Uri.parse(`flapi://endpoint/${slug}/template.sql`);
      const cacheOrCacheTemplateUri = cacheTemplateSource ? this.resolveFsPath(cacheTemplateSource) : vscode.Uri.parse(`flapi://endpoint/${slug}/cache.json`);
      // Parameters: use sidecar to be writable
      const templateForParams = templateSource ?? '';
      let parametersFileUri: vscode.Uri | undefined;
      if (templateForParams) {
        parametersFileUri = vscode.Uri.file(this.resolveFsPath(templateForParams).fsPath + '.vars.json');
      } else {
        const root = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? process.cwd();
        const sidecarFsPath = require('path').join(root, '.flapi', 'params', `${slug}.json`);
        parametersFileUri = vscode.Uri.file(sidecarFsPath);
      }
      const resultsUri = vscode.Uri.parse(`flapi://endpoint/${slug}/results.sql`);

      // Seed parameters from auto sources if empty
      const defaults = await this.loadAutoParameters(path);
      if (defaults && Object.keys(defaults).length > 0) {
        this.virtualDocuments.getParametersProvider().setParameters(slug, defaults);
      }

      // Ensure params file exists if using sidecar
      if (parametersFileUri?.scheme === 'file') {
        try {
          await vscode.workspace.fs.stat(parametersFileUri);
        } catch {
          await vscode.workspace.fs.createDirectory(vscode.Uri.file(require('path').dirname(parametersFileUri.fsPath)));
          await vscode.workspace.fs.writeFile(parametersFileUri, Buffer.from(JSON.stringify(defaults ?? {}, null, 2), 'utf8'));
        }
      }

      // Open documents in organized split view (prefer file editors)
      await this.openDocumentsInSplitView([
        { uri: configUri, column: vscode.ViewColumn.One, label: 'Config' },
        { uri: templateUri, column: vscode.ViewColumn.Two, label: 'SQL Template' },
        { uri: cacheOrCacheTemplateUri, column: vscode.ViewColumn.Three, label: 'Cache' },
        { uri: parametersFileUri ?? vscode.Uri.parse(`flapi://endpoint/${slug}/parameters.json`), column: vscode.ViewColumn.Beside, label: 'Parameters' },
        { uri: resultsUri, column: vscode.ViewColumn.Beside, label: 'Results' }
      ]);

      // Show success message
      vscode.window.showInformationMessage(`Opened endpoint: ${path}`);
      
    } catch (error) {
      vscode.window.showErrorMessage(`Failed to open endpoint: ${error}`);
    }
  }

  /**
   * Opens multiple documents in a coordinated split view layout
   */
  private async openDocumentsInSplitView(
    documents: Array<{ uri: vscode.Uri; column: vscode.ViewColumn; label: string }>
  ): Promise<void> {
    for (const doc of documents) {
      try {
        await vscode.window.showTextDocument(doc.uri, {
          viewColumn: doc.column,
          preview: false,
          preserveFocus: true
        });
      } catch (error) {
        console.error(`Failed to open ${doc.label}:`, error);
      }
    }
  }

  /**
   * Expands template with current parameters and shows results
   */
  async expandTemplate(path: string): Promise<void> {
    try {
      const slug = pathToSlug(path);
      const parameters = await this.getEffectiveParameters(path, slug);
      
      const url = buildEndpointUrl(path, 'template/expand');
      const response = await this.client.post(url, { parameters });

      const expandedSql =
        (response.data && (response.data.expanded_sql ?? response.data.expandedSql ?? response.data.sql)) || '';
      const resultData =
        (response.data && (response.data.data ?? response.data.rows ?? response.data.result ?? response.data.resultSet)) ||
        undefined;
      
      // Update results document
      this.virtualDocuments.getResultsProvider().setResults(slug, {
        expandedSql,
        data: resultData,
        variables: response.data?.variables
      });

      // Show results in output channel
      this.outputChannel.clear();
      this.outputChannel.appendLine(`=== Template Expansion Results for ${path} ===`);
      this.outputChannel.appendLine('');
      this.outputChannel.appendLine('Expanded SQL:');
      this.outputChannel.appendLine(expandedSql || '-- (empty)');
      this.outputChannel.appendLine('');
      
      if (resultData !== undefined) {
        this.outputChannel.appendLine('Query Results:');
        this.outputChannel.appendLine(JSON.stringify(resultData, null, 2));
      }
      
      this.outputChannel.show(true);
      
      vscode.window.showInformationMessage('Template expanded successfully!');
      
    } catch (error) {
      this.showError('Template expansion failed', error);
    }
  }

  /**
   * Tests template with current parameters
   */
  async testTemplate(path: string): Promise<void> {
    try {
      const slug = pathToSlug(path);
      const parameters = await this.getEffectiveParameters(path, slug);
      
      const url = buildEndpointUrl(path, 'template/test');
      const response = await this.client.post(url, { parameters });
      const expandedSql =
        (response.data && (response.data.expanded_sql ?? response.data.expandedSql ?? response.data.sql)) || '';
      
      // Show test results in output channel
      this.outputChannel.clear();
      this.outputChannel.appendLine(`=== Template Test Results for ${path} ===`);
      this.outputChannel.appendLine('');
      this.outputChannel.appendLine('Test Status: SUCCESS');
      this.outputChannel.appendLine('');
      this.outputChannel.appendLine('Expanded SQL:');
      this.outputChannel.appendLine(expandedSql || '-- (empty)');
      this.outputChannel.show(true);
      
      vscode.window.showInformationMessage('Template test passed!');
      
    } catch (error) {
      this.showError('Template test failed', error);
    }
  }

  /**
   * Validates template and shows diagnostics
   */
  async validateTemplate(path: string): Promise<void> {
    try {
      const slug = pathToSlug(path);
      const parameters = this.virtualDocuments.getParametersProvider().getParameters(slug);
      
      const url = buildEndpointUrl(path, 'template/expand') + '?validate_only=true';
      const response = await this.client.post(url, { parameters });
      
      // Clear previous diagnostics
      this.diagnostics.clear();
      
      if (!response.data.valid) {
        const templateUri = vscode.Uri.parse(`flapi://endpoint/${slug}/template.sql`);
        const diagnosticsArray: vscode.Diagnostic[] = [];
        
        // Add errors as diagnostics
        if (response.data.errors) {
          for (const error of response.data.errors) {
            const diagnostic = new vscode.Diagnostic(
              new vscode.Range(0, 0, 0, 1000), // Full first line for now
              error.message,
              vscode.DiagnosticSeverity.Error
            );
            diagnostic.source = 'flapi';
            diagnosticsArray.push(diagnostic);
          }
        }
        
        // Add warnings as diagnostics
        if (response.data.warnings) {
          for (const warning of response.data.warnings) {
            const diagnostic = new vscode.Diagnostic(
              new vscode.Range(0, 0, 0, 1000),
              warning.message,
              vscode.DiagnosticSeverity.Warning
            );
            diagnostic.source = 'flapi';
            diagnosticsArray.push(diagnostic);
          }
        }
        
        this.diagnostics.set(templateUri, diagnosticsArray);
      }
      
    } catch (error) {
      console.error('Validation failed:', error);
    }
  }

  /**
   * Saves changes to endpoint configuration
   */
  async saveEndpointConfig(path: string, config: any): Promise<void> {
    try {
      const url = buildEndpointUrl(path, '');
      await this.client.put(url, config);
      
      const slug = pathToSlug(path);
      this.virtualDocuments.refreshAll(slug);
      
      vscode.window.showInformationMessage('Endpoint configuration saved!');
      
    } catch (error) {
      this.showError('Failed to save endpoint configuration', error);
    }
  }

  /**
   * Saves changes to template
   */
  async saveTemplate(path: string, template: string): Promise<void> {
    try {
      const url = buildEndpointUrl(path, 'template');
      await this.client.put(url, { template });
      
      const slug = pathToSlug(path);
      this.virtualDocuments.refreshAll(slug);
      
      vscode.window.showInformationMessage('Template saved!');
      
    } catch (error) {
      this.showError('Failed to save template', error);
    }
  }

  /**
   * Saves changes to cache configuration
   */
  async saveCacheConfig(path: string, cacheConfig: any): Promise<void> {
    try {
      const url = buildEndpointUrl(path, 'cache');
      await this.client.put(url, cacheConfig);
      
      const slug = pathToSlug(path);
      this.virtualDocuments.refreshAll(slug);
      
      vscode.window.showInformationMessage('Cache configuration saved!');
      
    } catch (error) {
      this.showError('Failed to save cache configuration', error);
    }
  }

  /**
   * Saves changes to cache template (SQL)
   */
  async saveCacheTemplate(path: string, template: string): Promise<void> {
    try {
      const url = buildEndpointUrl(path, 'cache/template');
      await this.client.put(url, { template });

      const slug = pathToSlug(path);
      this.virtualDocuments.refreshAll(slug);

      vscode.window.showInformationMessage('Cache template saved!');
    } catch (error) {
      this.showError('Failed to save cache template', error);
    }
  }

  /**
   * Updates parameters for template expansion
   */
  setParameters(path: string, parameters: any): void {
    const slug = pathToSlug(path);
    this.virtualDocuments.getParametersProvider().setParameters(slug, parameters);
  }

  async openParameters(path: string): Promise<void> {
    const slug = pathToSlug(path);
    const defaults = await this.loadAutoParameters(path);
    // Resolve sidecar path: prefer next to template source; else workspace .flapi/params
    const templateSource = await this.resolveTemplateSource(path);
    let sidecarUri: vscode.Uri;
    if (templateSource) {
      const tplUri = this.resolveFsPath(templateSource);
      sidecarUri = vscode.Uri.file(tplUri.fsPath + '.vars.json');
    } else {
      const root = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? process.cwd();
      const sidecarFsPath = require('path').join(root, '.flapi', 'params', `${slug}.json`);
      sidecarUri = vscode.Uri.file(sidecarFsPath);
    }

    // Ensure directory exists
    try {
      const dir = require('path').dirname(sidecarUri.fsPath);
      await vscode.workspace.fs.createDirectory(vscode.Uri.file(dir));
    } catch {
      // ignore
    }

    // Write file if missing
    try {
      await vscode.workspace.fs.stat(sidecarUri);
    } catch {
      const initial = JSON.stringify(defaults && Object.keys(defaults).length > 0 ? defaults : {}, null, 2);
      await vscode.workspace.fs.writeFile(sidecarUri, Buffer.from(initial, 'utf8'));
    }

    const doc = await vscode.workspace.openTextDocument(sidecarUri);
    await vscode.window.showTextDocument(doc, { preview: false, viewColumn: vscode.ViewColumn.Beside });
  }

  /**
   * Shows error message and logs to output channel
   */
  private showError(message: string, error: any): void {
    const errorMsg = error?.response?.data?.error || error?.message || String(error);
    
    this.outputChannel.clear();
    this.outputChannel.appendLine(`ERROR: ${message}`);
    this.outputChannel.appendLine(errorMsg);
    this.outputChannel.show(true);
    
    vscode.window.showErrorMessage(`${message}: ${errorMsg}`);
  }

  /**
   * Disposes resources
   */
  dispose(): void {
    this.diagnostics.dispose();
    this.outputChannel.dispose();
  }
}
