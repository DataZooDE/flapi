import * as vscode from 'vscode';
import * as path from 'path';
import type { AxiosInstance } from 'axios';
import { pathToSlug } from '@flapi/shared';
import { TestStateService, TestState, TestHistoryEntry, TestResponse } from '../services/testStateService';
import { 
  getParameterEditorCSS, 
  getParameterEditorHTML, 
  getParameterEditorJS,
  type ParameterDefinition as SharedParameterDefinition
} from './shared/parameterEditor';

/**
 * Message types for SQL template testing
 */
interface SqlTemplateMessage {
  command: 'test' | 'preview' | 'updateParams' | 'loadHistory' | 'clearHistory' | 'loadVariables' | 'loadDefaults';
  data?: any;
}

/**
 * Environment variable info
 */
interface EnvironmentVariable {
  name: string;
  value: string;
  available: boolean;
}

/**
 * Parameter definition from backend
 */
interface ParameterDefinition {
  name: string;
  in: string;
  description?: string;
  required: boolean;
  default?: string;
}

/**
 * Manages the webview panel for SQL template testing
 */
export class SqlTemplateTesterPanel {
  public static currentPanel: SqlTemplateTesterPanel | undefined;
  private readonly _panel: vscode.WebviewPanel;
  private readonly _extensionUri: vscode.Uri;
  private _disposables: vscode.Disposable[] = [];
  private _sqlFilePath: string;
  private _yamlConfigPath: string;
  private _state: TestState;
  private _endpointConfig: any;

  constructor(
    panel: vscode.WebviewPanel,
    extensionUri: vscode.Uri,
    private readonly testStateService: TestStateService,
    private readonly client: AxiosInstance,
    sqlFilePath: string,
    yamlConfigPath: string,
    state: TestState,
    endpointConfig: any
  ) {
    this._panel = panel;
    this._extensionUri = extensionUri;
    this._sqlFilePath = sqlFilePath;
    this._yamlConfigPath = yamlConfigPath;
    this._state = state;
    this._endpointConfig = endpointConfig;

    // Set the webview's initial html content
    this._update();

    // Listen for when the panel is disposed
    this._panel.onDidDispose(() => this.dispose(), null, this._disposables);

    // Handle messages from the webview
    this._panel.webview.onDidReceiveMessage(
      async (message: SqlTemplateMessage) => {
        await this._handleMessage(message);
      },
      null,
      this._disposables
    );
  }

  /**
   * Create or show the panel
   */
  public static async createOrShow(
    extensionUri: vscode.Uri,
    testStateService: TestStateService,
    client: AxiosInstance,
    sqlUri: vscode.Uri,
    yamlConfigPath: string
  ): Promise<SqlTemplateTesterPanel> {
    const column = vscode.ViewColumn.Two;
    const sqlFilePath = sqlUri.fsPath;

    // If we already have a panel, show it
    if (SqlTemplateTesterPanel.currentPanel) {
      SqlTemplateTesterPanel.currentPanel._panel.reveal(column);
      // Update with new file
      SqlTemplateTesterPanel.currentPanel._sqlFilePath = sqlFilePath;
      SqlTemplateTesterPanel.currentPanel._yamlConfigPath = yamlConfigPath;
      SqlTemplateTesterPanel.currentPanel._update();
      return SqlTemplateTesterPanel.currentPanel;
    }

    // Load endpoint configs from backend (there may be multiple)
    const endpointConfigs = await SqlTemplateTesterPanel.loadEndpointConfigsForTemplate(client, sqlFilePath);
    
    if (endpointConfigs.length === 0) {
      vscode.window.showErrorMessage(`No endpoints found that reference template: ${path.basename(sqlFilePath)}`);
      throw new Error('No endpoints found for template');
    }
    
    // Use the first endpoint by default (TODO: allow user to select if multiple)
    const endpointConfig = endpointConfigs[0];
    
    if (endpointConfigs.length > 1) {
      vscode.window.showInformationMessage(
        `Found ${endpointConfigs.length} endpoints for this template. Using: ${endpointConfig['url-path'] || endpointConfig.urlPath}`
      );
    }

    // Otherwise, create a new panel
    const panel = vscode.window.createWebviewPanel(
      'flapiSqlTemplateTester',
      `Test SQL: ${path.basename(sqlFilePath)}`,
      column,
      {
        enableScripts: true,
        retainContextWhenHidden: true,
        localResourceRoots: [extensionUri]
      }
    );

    // Initialize state
    const state = await testStateService.initializeSqlState(sqlFilePath, yamlConfigPath);

    SqlTemplateTesterPanel.currentPanel = new SqlTemplateTesterPanel(
      panel,
      extensionUri,
      testStateService,
      client,
      sqlFilePath,
      yamlConfigPath,
      state,
      endpointConfig
    );

    return SqlTemplateTesterPanel.currentPanel;
  }


  /**
   * Load endpoint configurations from backend that reference the SQL template
   * Uses /by-template to find endpoints, then fetches full config using PathUtils
   */
  private static async loadEndpointConfigsForTemplate(client: AxiosInstance, sqlFilePath: string): Promise<any[]> {
    try {
      // Query the backend to find all endpoints that use this SQL template
      const response = await client.post('/api/v1/_config/endpoints/by-template', {
        template_path: sqlFilePath
      });
      
      const endpoints = response.data.endpoints || [];
      
      if (endpoints.length === 0) {
        console.warn('[SqlTemplateTesterPanel] No endpoints found for template:', sqlFilePath);
        return [];
      }
      
      console.log(`[SqlTemplateTesterPanel] Found ${endpoints.length} endpoints for template`);
      
      // Fetch full configuration for each endpoint using the slug derived from url_path or mcp_name
      const fullConfigs = await Promise.all(
        endpoints.map(async (ep: any) => {
          try {
            let slug: string;
            if (ep.type === 'REST' && ep.url_path) {
              // Convert path to slug using centralized PathUtils from @flapi/shared
              // /customers/ → customers-slash
              // /publicis → publicis
              // /sap/functions → sap-functions
              slug = pathToSlug(ep.url_path);
            } else if (ep.mcp_name) {
              slug = ep.mcp_name;
            } else {
              console.warn('[SqlTemplateTesterPanel] Cannot determine slug for endpoint:', ep);
              return null;
            }
            
            console.log(`[SqlTemplateTesterPanel] Fetching config for slug: ${slug}`);
            const configResponse = await client.get(`/api/v1/_config/endpoints/${slug}`);
            return configResponse.data;
          } catch (error: any) {
            console.error('[SqlTemplateTesterPanel] Failed to fetch config for endpoint:', ep, error);
            return null;
          }
        })
      );
      
      const validConfigs = fullConfigs.filter(c => c !== null);
      console.log(`[SqlTemplateTesterPanel] Successfully loaded ${validConfigs.length} full configs`);
      
      return validConfigs;
    } catch (error: any) {
      console.error('[SqlTemplateTesterPanel] Failed to load endpoint configs:', error);
      return [];
    }
  }

  public dispose() {
    SqlTemplateTesterPanel.currentPanel = undefined;

    // Clean up our resources
    this._panel.dispose();

    while (this._disposables.length) {
      const disposable = this._disposables.pop();
      if (disposable) {
        disposable.dispose();
      }
    }
  }

  private async _handleMessage(message: SqlTemplateMessage) {
    switch (message.command) {
      case 'preview':
        await this._previewTemplate(message.data);
        break;
      case 'test':
        await this._executeTest(message.data);
        break;
      case 'updateParams':
        await this._updateParameters(message.data);
        break;
      case 'loadHistory':
        await this._loadHistory();
        break;
      case 'clearHistory':
        await this._clearHistory();
        break;
      case 'loadVariables':
        await this._loadVariables();
        break;
      case 'loadDefaults':
        await this._loadDefaults();
        break;
    }
  }

  /**
   * Preview the Mustache-expanded SQL template
   */
  private async _previewTemplate(data: any) {
    try {
      const { parameters } = data;
      
      if (!this._endpointConfig) {
        throw new Error('No endpoint configuration available');
      }
      
      // Get the endpoint slug using centralized logic
      const slug = this._getEndpointSlug();
      if (!slug) {
        throw new Error('Could not determine endpoint slug');
      }
      
      console.log(`[SqlTemplateTesterPanel] Previewing template for slug: ${slug}`);
      
      // Call backend to render the template
      const response = await this.client.post(`/api/v1/_config/endpoints/${slug}/template/expand`, {
        parameters
      });

      this._panel.webview.postMessage({
        command: 'previewResult',
        preview: response.data.expanded || ''
      });
    } catch (error: any) {
      console.error('[SqlTemplateTesterPanel] Preview error:', error);
      this._panel.webview.postMessage({
        command: 'previewError',
        error: error.message || 'Failed to preview template'
      });
    }
  }

  /**
   * Execute the SQL template
   */
  private async _executeTest(data: any) {
    try {
      const { parameters, limit } = data;
      
      if (!this._endpointConfig) {
        throw new Error('No endpoint configuration available');
      }
      
      // Get the endpoint slug using centralized logic
      const slug = this._getEndpointSlug();
      if (!slug) {
        throw new Error('Could not determine endpoint slug');
      }
      
      console.log(`[SqlTemplateTesterPanel] Executing test for slug: ${slug} with params:`, parameters);
      
      const startTime = Date.now();
      const response = await this.client.post(`/api/v1/_config/endpoints/${slug}/template/test`, {
        parameters,
        limit: limit || 1000
      });
      const endTime = Date.now();

      const result = response.data;
      const responseInfo: TestResponse = {
        time: endTime - startTime,
        size: JSON.stringify(result).length,
        body: JSON.stringify(result, null, 2),
        contentType: 'application/json'
      };

      // Save to history
      const historyEntry: TestHistoryEntry = {
        timestamp: new Date(),
        parameters,
        response: responseInfo
      };

      await this.testStateService.addToHistory(this._sqlFilePath, historyEntry);

      // Send result to webview
      this._panel.webview.postMessage({
        command: 'testResult',
        response: responseInfo,
        data: result
      });
    } catch (error: any) {
      console.error('[SqlTemplateTesterPanel] Execute error:', error);
      const errorResponse: TestResponse = {
        time: 0,
        size: 0,
        body: error.response?.data || error.message,
        error: error.message
      };

      this._panel.webview.postMessage({
        command: 'testError',
        error: error.message || 'Failed to execute SQL',
        response: errorResponse
      });
    }
  }

  /**
   * Update parameters
   */
  private async _updateParameters(data: any) {
    const { parameters } = data;
    await this.testStateService.updateParameters(this._sqlFilePath, parameters);
  }

  /**
   * Load history
   */
  private async _loadHistory() {
    const state = this.testStateService.getState(this._sqlFilePath);
    if (state) {
      this._panel.webview.postMessage({
        command: 'historyLoaded',
        history: state.history
      });
    }
  }

  /**
   * Clear history
   */
  private async _clearHistory() {
    await this.testStateService.clearHistory(this._sqlFilePath);
    this._panel.webview.postMessage({
      command: 'historyCleared'
    });
  }

  /**
   * Load available Mustache variables from backend
   */
  private async _loadVariables() {
    try {
      const response = await this.client.get('/api/v1/_config/environment-variables');
      const variables: EnvironmentVariable[] = response.data.variables || [];
      
      this._panel.webview.postMessage({
        command: 'variablesLoaded',
        variables
      });
    } catch (error: any) {
      console.error('[SqlTemplateTesterPanel] Failed to load variables:', error);
      this._panel.webview.postMessage({
        command: 'variablesError',
        error: error.message || 'Failed to load variables'
      });
    }
  }

  /**
   * Load default parameters from YAML config
   */
  private async _loadDefaults() {
    try {
      // Get the endpoint slug using centralized logic
      const slug = this._getEndpointSlug();
      if (!slug) {
        console.error('[SqlTemplateTesterPanel] Could not determine endpoint slug');
        return;
      }
      
      console.log(`[SqlTemplateTesterPanel] Loading defaults for slug: ${slug}`);
      const response = await this.client.get(`/api/v1/_config/endpoints/${slug}/parameters`);
      const parameters: ParameterDefinition[] = response.data.parameters || [];
      
      // Build default parameters object
      const defaults: Record<string, string> = {};
      for (const param of parameters) {
        if (param.default) {
          defaults[param.name] = param.default;
        }
      }
      
      this._panel.webview.postMessage({
        command: 'defaultsLoaded',
        defaults,
        parameters
      });
    } catch (error: any) {
      console.error('[SqlTemplateTesterPanel] Failed to load defaults:', error);
    }
  }

  /**
   * Get the endpoint slug using centralized logic
   * (Extracted to avoid duplication across _previewTemplate, _executeTest, _loadDefaults)
   */
  private _getEndpointSlug(): string | null {
    if (!this._endpointConfig) {
      return null;
    }
    
    const urlPath = this._endpointConfig['url-path'] || this._endpointConfig.urlPath;
    const mcpTool = this._endpointConfig['mcp-tool'] || this._endpointConfig.mcpTool;
    const mcpResource = this._endpointConfig['mcp-resource'] || this._endpointConfig.mcpResource;
    const mcpPrompt = this._endpointConfig['mcp-prompt'] || this._endpointConfig.mcpPrompt;
    
    if (urlPath) {
      // REST endpoint - use centralized pathToSlug
      return pathToSlug(urlPath);
    } else if (mcpTool?.name) {
      // MCP tool - use name as-is
      return mcpTool.name;
    } else if (mcpResource?.name) {
      // MCP resource - use name as-is
      return mcpResource.name;
    } else if (mcpPrompt?.name) {
      // MCP prompt - use name as-is
      return mcpPrompt.name;
    }
    
    return null;
  }

  /**
   * Update the webview content
   */
  private _update() {
    this._panel.webview.html = this._getHtmlForWebview();
    
    // Send initial state
    this._panel.webview.postMessage({
      command: 'init',
      state: this._state,
      sqlFilePath: this._sqlFilePath,
      yamlConfigPath: this._yamlConfigPath,
      endpointConfig: this._endpointConfig
    });

    // Load defaults and variables
    this._loadDefaults();
    this._loadVariables();
  }

  /**
   * Get HTML content for the webview
   */
  private _getHtmlForWebview(): string {
    return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SQL Template Tester</title>
  <link href="https://cdn.jsdelivr.net/npm/@vscode/codicons@0.0.32/dist/codicon.css" rel="stylesheet" />
  <style>
    body {
      font-family: var(--vscode-font-family);
      font-size: var(--vscode-font-size);
      color: var(--vscode-foreground);
      background-color: var(--vscode-editor-background);
      padding: 0;
      margin: 0;
    }

    .container {
      display: flex;
      flex-direction: column;
      height: 100vh;
    }

    .header {
      padding: 12px 16px;
      background-color: var(--vscode-editor-background);
      border-bottom: 1px solid var(--vscode-panel-border);
    }

    .header h2 {
      margin: 0 0 8px 0;
      font-size: 16px;
      font-weight: 600;
    }

    .header-info {
      font-size: 12px;
      opacity: 0.8;
    }

    .main-content {
      flex: 1;
      overflow: hidden;
      display: flex;
      flex-direction: column;
    }

    .variables-panel {
      border-bottom: 1px solid var(--vscode-panel-border);
      background-color: var(--vscode-sideBar-background);
    }

    .variables-header {
      padding: 8px 16px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      cursor: pointer;
      user-select: none;
    }

    .variables-header:hover {
      background-color: var(--vscode-list-hoverBackground);
    }

    .variables-content {
      padding: 12px 16px;
      max-height: 200px;
      overflow-y: auto;
      display: none;
    }

    .variables-content.expanded {
      display: block;
    }

    .variable-item {
      padding: 4px 0;
      font-size: 12px;
      font-family: var(--vscode-editor-font-family);
    }

    .variable-name {
      color: var(--vscode-symbolIcon-variableForeground);
      font-weight: 500;
    }

    .variable-value {
      color: var(--vscode-descriptionForeground);
      margin-left: 8px;
    }

    .variable-unavailable {
      opacity: 0.5;
    }

    /* Parameter Editor Styles (from shared component) */
    ${getParameterEditorCSS()}

    .actions {
      display: flex;
      gap: 8px;
      margin-top: 12px;
    }

    .actions button {
      padding: 8px 12px;
      border: none;
      border-radius: 4px;
      cursor: pointer;
      font-size: 16px;
      display: flex;
      align-items: center;
      justify-content: center;
      min-width: 40px;
      height: 32px;
    }

    .actions button .codicon {
      font-size: 16px;
    }

    .btn-primary {
      background: var(--vscode-button-background);
      color: var(--vscode-button-foreground);
    }

    .btn-primary:hover {
      background: var(--vscode-button-hoverBackground);
    }

    .btn-secondary {
      background: var(--vscode-button-secondaryBackground);
      color: var(--vscode-button-secondaryForeground);
    }

    .btn-secondary:hover {
      background: var(--vscode-button-secondaryHoverBackground);
    }

    .preview-section, .results-section {
      flex: 1;
      overflow: hidden;
      display: flex;
      flex-direction: column;
    }

    .section-header {
      padding: 8px 16px;
      background-color: var(--vscode-sideBar-background);
      border-bottom: 1px solid var(--vscode-panel-border);
      font-weight: 600;
      font-size: 13px;
    }

    .section-content {
      flex: 1;
      overflow: auto;
      padding: 12px;
    }

    pre {
      margin: 0;
      padding: 12px;
      background: var(--vscode-textCodeBlock-background);
      border-radius: 3px;
      font-family: var(--vscode-editor-font-family);
      font-size: 12px;
      overflow-x: auto;
      white-space: pre-wrap;
      word-wrap: break-word;
    }

    /* SQL Syntax Highlighting */
    .sql-preview {
      line-height: 1.5;
    }

    .sql-keyword {
      color: var(--vscode-symbolIcon-keywordForeground, #569cd6);
      font-weight: bold;
    }

    .sql-string {
      color: var(--vscode-symbolIcon-stringForeground, #ce9178);
    }

    .sql-number {
      color: var(--vscode-symbolIcon-numberForeground, #b5cea8);
    }

    .sql-comment {
      color: var(--vscode-descriptionForeground);
      font-style: italic;
    }

    .sql-function {
      color: var(--vscode-symbolIcon-functionForeground, #dcdcaa);
    }

    /* View Toggle Styles */
    .view-toggle-bottom {
      display: flex;
      gap: 4px;
      padding: 8px;
      border-top: 1px solid var(--vscode-panel-border);
      background: var(--vscode-sideBar-background);
      justify-content: center;
    }

    .view-toggle-btn {
      padding: 6px 16px;
      border: 1px solid var(--vscode-button-border);
      background: var(--vscode-button-secondaryBackground);
      color: var(--vscode-button-secondaryForeground);
      border-radius: 3px;
      cursor: pointer;
      font-size: 12px;
      transition: all 0.2s;
    }

    .view-toggle-btn:hover {
      background: var(--vscode-button-secondaryHoverBackground);
    }

    .view-toggle-btn.active {
      background: var(--vscode-button-background);
      color: var(--vscode-button-foreground);
      border-color: var(--vscode-button-background);
    }

    .response-view {
      flex: 1;
      overflow: auto;
    }

    .data-table-wrapper {
      width: 100%;
      overflow: auto;
    }

    .data-table-wrapper table {
      border-collapse: collapse;
      width: 100%;
      font-size: 12px;
    }

    .data-table-wrapper th,
    .data-table-wrapper td {
      padding: 6px 8px;
      text-align: left;
      border: 1px solid var(--vscode-panel-border);
      max-width: 300px;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .data-table-wrapper th {
      background: var(--vscode-sideBar-background);
      font-weight: 600;
      position: sticky;
      top: 0;
      z-index: 1;
    }

    .data-table-wrapper tr:hover {
      background: var(--vscode-list-hoverBackground);
    }

    .json-formatted {
      white-space: pre-wrap;
      word-wrap: break-word;
      font-family: var(--vscode-editor-font-family);
      font-size: 12px;
    }

    .error {
      color: var(--vscode-errorForeground);
      padding: 12px;
      background: var(--vscode-inputValidation-errorBackground);
      border: 1px solid var(--vscode-inputValidation-errorBorder);
      border-radius: 3px;
      margin-bottom: 12px;
    }

    .response-info {
      display: flex;
      gap: 16px;
      padding: 8px 12px;
      background: var(--vscode-sideBar-background);
      border-bottom: 1px solid var(--vscode-panel-border);
      font-size: 12px;
    }

    .response-info span {
      opacity: 0.8;
    }

    .response-info strong {
      margin-left: 4px;
      opacity: 1;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 12px;
    }

    th, td {
      padding: 8px;
      text-align: left;
      border: 1px solid var(--vscode-panel-border);
    }

    th {
      background: var(--vscode-sideBar-background);
      font-weight: 600;
    }

    tr:hover {
      background: var(--vscode-list-hoverBackground);
    }

    .limit-input {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-top: 12px;
    }

    .limit-input label {
      font-size: 12px;
    }

    .limit-input input {
      width: 100px;
      padding: 4px 8px;
      background: var(--vscode-input-background);
      color: var(--vscode-input-foreground);
      border: 1px solid var(--vscode-input-border);
      border-radius: 2px;
    }

    .expand-icon {
      transition: transform 0.2s;
    }

    .expand-icon.expanded {
      transform: rotate(90deg);
    }

    /* Resize Handles - Style Guide Compliance */
    .resize-handle {
      height: 4px;
      background: transparent;
      cursor: row-resize;
      position: relative;
      flex-shrink: 0;
      border-top: 1px solid var(--vscode-panel-border);
      border-bottom: 1px solid var(--vscode-panel-border);
    }

    .resize-handle:hover,
    .resize-handle.dragging {
      background: var(--vscode-focusBorder);
    }

    .resize-handle::before {
      content: '';
      position: absolute;
      top: -2px;
      bottom: -2px;
      left: 0;
      right: 0;
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h2>SQL Template Tester</h2>
      <div class="header-info">
        <div>Template: <code id="sql-path"></code></div>
        <div>Config: <code id="yaml-path"></code></div>
      </div>
    </div>

    <div class="main-content">
      <!-- Variables Panel (Collapsible) -->
      <div class="variables-panel">
        <div class="variables-header" onclick="toggleVariables()">
          <div>
            <span class="expand-icon" id="variables-expand-icon">▶</span>
            <strong>Available Variables</strong>
            <span style="opacity: 0.7; font-size: 11px; margin-left: 8px;">(Click to expand)</span>
          </div>
        </div>
        <div class="variables-content" id="variables-content">
          <div id="variables-list">Loading...</div>
        </div>
      </div>

      <!-- Parameter Editor (Shared Component) -->
      <div class="param-editor" id="param-editor">
        <h3>Request Parameters</h3>
        <div class="param-list" id="param-editor-list"></div>
        <button class="add-param-btn" onclick="addParameter()">+ Add Parameter</button>
        <button class="add-param-btn" onclick="loadDefaults()">Load Defaults</button>
        
        <div class="limit-input">
          <label for="result-limit">Result Limit:</label>
          <input type="number" id="result-limit" value="1000" min="1" max="100000" />
        </div>

        <div class="actions">
          <button class="btn-primary" onclick="previewTemplate()" title="Preview expanded SQL template">
            <span class="codicon codicon-eye"></span>
          </button>
          <button class="btn-primary" onclick="executeTest()" title="Execute SQL query">
            <span class="codicon codicon-play"></span>
          </button>
        </div>
      </div>

      <!-- Resize Handle 1: Between Parameters and Preview -->
      <div class="resize-handle" id="resizeHandle1"></div>

      <!-- Preview Section -->
      <div class="preview-section" id="previewSection">
        <div class="section-header">SQL Preview</div>
        <div class="section-content">
          <pre id="preview-content" class="sql-preview">Click "Preview SQL" to see the expanded template...</pre>
        </div>
      </div>

      <!-- Resize Handle 2: Between Preview and Results -->
      <div class="resize-handle" id="resizeHandle2"></div>

      <!-- Results Section -->
      <div class="results-section" id="resultsSection">
        <div class="section-header">Results</div>
        <div class="response-info" id="response-info" style="display: none;"></div>
        <div class="section-content">
          <!-- JSON View -->
          <div id="jsonView" class="response-view">
            <div id="results-content">Click "Execute" to run the SQL...</div>
          </div>
          
          <!-- Table View -->
          <div id="tableView" class="response-view" style="display: none;">
            <div id="table-content" class="data-table-wrapper">
              <p style="text-align: center; opacity: 0.6;">No data to display</p>
            </div>
          </div>
          
          <!-- View Toggle (at bottom) -->
          <div class="view-toggle-bottom">
            <button id="jsonViewBtn" class="view-toggle-btn active" onclick="toggleView('json')">JSON</button>
            <button id="tableViewBtn" class="view-toggle-btn" onclick="toggleView('table')">Table</button>
          </div>
        </div>
      </div>
    </div>
  </div>

  <script>
    const vscode = acquireVsCodeApi();
    let state = {};
    let availableVariables = [];
    let parameterDefinitions = [];

    // Message handler
    window.addEventListener('message', event => {
      const message = event.data;
      
      switch (message.command) {
        case 'init':
          handleInit(message);
          break;
        case 'previewResult':
          handlePreviewResult(message);
          break;
        case 'previewError':
          handlePreviewError(message);
          break;
        case 'testResult':
          handleTestResult(message);
          break;
        case 'testError':
          handleTestError(message);
          break;
        case 'variablesLoaded':
          handleVariablesLoaded(message);
          break;
        case 'defaultsLoaded':
          handleDefaultsLoaded(message);
          break;
      }
    });

    function handleInit(message) {
      state = message.state;
      document.getElementById('sql-path').textContent = message.sqlFilePath;
      document.getElementById('yaml-path').textContent = message.yamlConfigPath;
      
      // Load parameters
      renderParameters();
      
      // Initialize resize handles
      initializeResize();
    }
    
    // Initialize resize functionality - Style Guide Compliance
    function initializeResize() {
      // Resize handle 1: Between parameters and preview
      initializeResizeHandle('resizeHandle1', 'param-editor', 'previewSection');
      
      // Resize handle 2: Between preview and results  
      initializeResizeHandle('resizeHandle2', 'previewSection', 'resultsSection');
    }
    
    function initializeResizeHandle(handleId, prevElementId, nextElementId) {
      const handle = document.getElementById(handleId);
      const prevElement = document.getElementById(prevElementId);
      const nextElement = document.getElementById(nextElementId);
      
      if (!handle || !prevElement || !nextElement) return;
      
      let startY, startHeight;
      
      handle.addEventListener('mousedown', (e) => {
        handle.classList.add('dragging');
        startY = e.clientY;
        startHeight = prevElement.offsetHeight;
        
        document.addEventListener('mousemove', onMouseMove);
        document.addEventListener('mouseup', onMouseUp);
        e.preventDefault();
      });
      
      function onMouseMove(e) {
        const delta = e.clientY - startY;
        const newHeight = Math.max(80, startHeight + delta); // Min height 80px per style guide
        prevElement.style.height = \`\${newHeight}px\`;
        prevElement.style.flex = '0 0 auto';
      }
      
      function onMouseUp() {
        handle.classList.remove('dragging');
        document.removeEventListener('mousemove', onMouseMove);
        document.removeEventListener('mouseup', onMouseUp);
      }
    }

    function handlePreviewResult(message) {
      const previewEl = document.getElementById('preview-content');
      const sql = message.preview;
      
      // Apply SQL syntax highlighting
      previewEl.innerHTML = highlightSQL(sql);
    }
    
    function highlightSQL(sql) {
      if (!sql) return '';
      
      // Escape HTML first
      let highlighted = escapeHtml(sql);
      
      // SQL Keywords
      const keywords = /\\b(SELECT|FROM|WHERE|AND|OR|NOT|IN|BETWEEN|LIKE|IS|NULL|JOIN|INNER|LEFT|RIGHT|OUTER|ON|AS|GROUP|BY|HAVING|ORDER|LIMIT|OFFSET|INSERT|INTO|VALUES|UPDATE|SET|DELETE|CREATE|TABLE|ALTER|DROP|INDEX|VIEW|WITH|CASE|WHEN|THEN|ELSE|END|DISTINCT|COUNT|SUM|AVG|MAX|MIN|UNION|ALL|EXCEPT|INTERSECT)\\b/gi;
      highlighted = highlighted.replace(keywords, '<span class="sql-keyword">$&</span>');
      
      // SQL Functions
      const functions = /\\b(COUNT|SUM|AVG|MAX|MIN|ROUND|FLOOR|CEIL|ABS|COALESCE|NULLIF|CAST|CONVERT|SUBSTRING|TRIM|UPPER|LOWER|LENGTH|CONCAT|NOW|CURRENT_DATE|CURRENT_TIME|CURRENT_TIMESTAMP)\\s*\\(/gi;
      highlighted = highlighted.replace(functions, '<span class="sql-function">$&</span>');
      
      // Strings (single quotes)
      highlighted = highlighted.replace(/'([^']*)'/g, '<span class="sql-string">\\'$1\\'</span>');
      
      // Numbers
      highlighted = highlighted.replace(/\\b(\\d+\\.?\\d*)\\b/g, '<span class="sql-number">$1</span>');
      
      // Comments
      highlighted = highlighted.replace(/--([^\\n]*)/g, '<span class="sql-comment">--$1</span>');
      highlighted = highlighted.replace(/\\/\\*([\\s\\S]*?)\\*\\//g, '<span class="sql-comment">/*$1*/</span>');
      
      return highlighted;
    }

    function handlePreviewError(message) {
      document.getElementById('preview-content').innerHTML = 
        '<div class="error">' + escapeHtml(message.error) + '</div>';
    }

    function handleTestResult(message) {
      const { response, data } = message;
      
      // Update response info
      const infoDiv = document.getElementById('response-info');
      infoDiv.style.display = 'flex';
      infoDiv.innerHTML = 
        '<span>Time: <strong>' + response.time + 'ms</strong></span>' +
        '<span>Size: <strong>' + formatBytes(response.size) + '</strong></span>' +
        '<span>Rows: <strong>' + (data.rows?.length || data.data?.length || 0) + '</strong></span>';
      
      // Store the result data for view switching
      window.currentResultData = data;
      
      // Render in the current view (JSON or table)
      renderResultView();
    }
    
    function renderResultView() {
      const data = window.currentResultData;
      if (!data) return;
      
      const jsonView = document.getElementById('jsonView');
      const tableView = document.getElementById('tableView');
      const jsonBtn = document.getElementById('jsonViewBtn');
      const tableBtn = document.getElementById('tableViewBtn');
      
      const isTableView = tableBtn.classList.contains('active');
      
      if (isTableView) {
        // Render table view
        const rows = data.rows || data.data || [];
        const columns = data.columns || [];
        
        if (rows.length > 0) {
          document.getElementById('table-content').innerHTML = renderTable(rows, columns);
        } else {
          document.getElementById('table-content').innerHTML = 
            '<p style="text-align: center; opacity: 0.6;">No data to display</p>';
        }
      } else {
        // Render JSON view
        const resultsDiv = document.getElementById('results-content');
        resultsDiv.innerHTML = '<pre class="json-formatted">' + JSON.stringify(data, null, 2) + '</pre>';
      }
    }
    
    function toggleView(view) {
      const jsonView = document.getElementById('jsonView');
      const tableView = document.getElementById('tableView');
      const jsonBtn = document.getElementById('jsonViewBtn');
      const tableBtn = document.getElementById('tableViewBtn');
      
      if (view === 'json') {
        jsonView.style.display = 'block';
        tableView.style.display = 'none';
        jsonBtn.classList.add('active');
        tableBtn.classList.remove('active');
      } else {
        jsonView.style.display = 'none';
        tableView.style.display = 'block';
        jsonBtn.classList.remove('active');
        tableBtn.classList.add('active');
      }
      
      // Re-render the current result in the new view
      renderResultView();
    }
    
    function renderTable(rows, columns) {
      if (!rows || rows.length === 0) {
        return '<p style="text-align: center; opacity: 0.6;">No rows returned</p>';
      }
      
      // Extract columns if not provided
      let cols = columns;
      if (!cols || cols.length === 0) {
        if (typeof rows[0] === 'object' && rows[0] !== null) {
          cols = Object.keys(rows[0]);
        } else {
          return '<pre>' + JSON.stringify(rows, null, 2) + '</pre>';
        }
      }
      
      // Build table HTML
      let html = '<table><thead><tr>';
      
      // Table headers
      cols.forEach(col => {
        const colName = typeof col === 'object' ? (col.name || String(col)) : String(col);
        html += '<th>' + escapeHtml(colName) + '</th>';
      });
      html += '</tr></thead><tbody>';
      
      // Table rows
      rows.forEach(row => {
        html += '<tr>';
        cols.forEach(col => {
          const colName = typeof col === 'object' ? (col.name || String(col)) : String(col);
          let cellValue = row[colName];
          
          // Handle complex types (objects/arrays) - show as JSON string
          if (cellValue !== null && typeof cellValue === 'object') {
            cellValue = JSON.stringify(cellValue);
          }
          
          html += '<td title="' + escapeHtml(String(cellValue || '')) + '">' + 
                  escapeHtml(String(cellValue !== null && cellValue !== undefined ? cellValue : '')) + 
                  '</td>';
        });
        html += '</tr>';
      });
      html += '</tbody></table>';
      
      return html;
    }

    function handleTestError(message) {
      const resultsDiv = document.getElementById('results-content');
      resultsDiv.innerHTML = '<div class="error">' + escapeHtml(message.error) + '</div>';
      
      document.getElementById('response-info').style.display = 'none';
    }

    function handleVariablesLoaded(message) {
      availableVariables = message.variables || [];
      renderVariables();
    }

    function handleDefaultsLoaded(message) {
      parameterDefinitions = message.parameters || [];
      const defaults = message.defaults || {};
      
      // Merge defaults with existing parameters
      state.parameters = { ...defaults, ...state.parameters };
      renderParameters();
    }

    function renderVariables() {
      const listDiv = document.getElementById('variables-list');
      
      if (availableVariables.length === 0) {
        listDiv.innerHTML = '<div style="opacity: 0.6;">No environment variables configured</div>';
        return;
      }
      
      listDiv.innerHTML = availableVariables.map(v => 
        '<div class="variable-item ' + (v.available ? '' : 'variable-unavailable') + '">' +
          '<span class="variable-name">' + escapeHtml(v.name) + '</span>' +
          '<span class="variable-value">' + (v.available ? escapeHtml(v.value) : '(not set)') + '</span>' +
        '</div>'
      ).join('');
    }

    /* Parameter Editor Functions (from shared component) */
    ${getParameterEditorJS({ showLoadDefaults: true })}

    function previewTemplate() {
      const parameters = collectParameters();
      state.parameters = parameters;
      
      vscode.postMessage({
        command: 'updateParams',
        data: { parameters }
      });
      
      vscode.postMessage({
        command: 'preview',
        data: { parameters }
      });
    }

    function executeTest() {
      const parameters = collectParameters();
      const limit = parseInt(document.getElementById('result-limit').value) || 1000;
      state.parameters = parameters;
      
      vscode.postMessage({
        command: 'updateParams',
        data: { parameters }
      });
      
      vscode.postMessage({
        command: 'test',
        data: { parameters, limit }
      });
    }

    function loadDefaults() {
      vscode.postMessage({
        command: 'loadDefaults'
      });
    }

    function toggleVariables() {
      const content = document.getElementById('variables-content');
      const icon = document.getElementById('variables-expand-icon');
      
      content.classList.toggle('expanded');
      icon.classList.toggle('expanded');
    }

    function renderTable(rows) {
      if (!rows || rows.length === 0) {
        return '<div style="opacity: 0.6; padding: 12px;">No results</div>';
      }
      
      const headers = Object.keys(rows[0]);
      
      let html = '<table>';
      html += '<thead><tr>';
      headers.forEach(h => {
        html += '<th>' + escapeHtml(h) + '</th>';
      });
      html += '</tr></thead>';
      
      html += '<tbody>';
      rows.forEach(row => {
        html += '<tr>';
        headers.forEach(h => {
          const value = row[h];
          const displayValue = typeof value === 'object' ? JSON.stringify(value) : String(value);
          html += '<td>' + escapeHtml(displayValue) + '</td>';
        });
        html += '</tr>';
      });
      html += '</tbody>';
      html += '</table>';
      
      return html;
    }

    function escapeHtml(text) {
      const div = document.createElement('div');
      div.textContent = text;
      return div.innerHTML;
    }

    function formatBytes(bytes) {
      if (bytes < 1024) return bytes + ' B';
      if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
      return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
    }
  </script>
</body>
</html>`;
  }
}

