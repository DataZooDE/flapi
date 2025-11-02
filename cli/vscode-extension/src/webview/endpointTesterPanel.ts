import * as vscode from 'vscode';
import { EndpointTestState, RequestFieldDefinition, ResponseInfo, TestEndpointMessage } from '../types/endpointTest';
import { ParameterStorageService } from '../services/parameterStorage';
import { EndpointTestService } from '../services/endpointTestService';
import { 
  getParameterEditorCSS, 
  getParameterEditorJS
} from './shared/parameterEditor';

/**
 * Manages the webview panel for endpoint testing
 */
export class EndpointTesterPanel {
  public static currentPanel: EndpointTesterPanel | undefined;
  private readonly _panel: vscode.WebviewPanel;
  private readonly _extensionUri: vscode.Uri;
  private _disposables: vscode.Disposable[] = [];
  private _endpointConfig: any;
  private _state: EndpointTestState;

  constructor(
    panel: vscode.WebviewPanel,
    extensionUri: vscode.Uri,
    private readonly parameterStorage: ParameterStorageService,
    private readonly testService: EndpointTestService,
    endpointConfig: any,
    state: EndpointTestState
  ) {
    this._panel = panel;
    this._extensionUri = extensionUri;
    this._endpointConfig = endpointConfig;
    this._state = state;

    // Set the webview's initial html content
    this._update();

    // Listen for when the panel is disposed
    this._panel.onDidDispose(() => this.dispose(), null, this._disposables);

    // Handle messages from the webview
    this._panel.webview.onDidReceiveMessage(
      async (message: TestEndpointMessage) => {
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
    parameterStorage: ParameterStorageService,
    testService: EndpointTestService,
    endpointConfig: any,
    baseUrl: string
  ): Promise<EndpointTesterPanel> {
    const column = vscode.ViewColumn.Two;

    // If we already have a panel, show it
    if (EndpointTesterPanel.currentPanel) {
      EndpointTesterPanel.currentPanel._panel.reveal(column);
      // Update with new config
      EndpointTesterPanel.currentPanel._endpointConfig = endpointConfig;
      EndpointTesterPanel.currentPanel._update();
      return EndpointTesterPanel.currentPanel;
    }

    // Otherwise, create a new panel
    const panel = vscode.window.createWebviewPanel(
      'flapiEndpointTester',
      `Test: ${endpointConfig['url-path'] || endpointConfig.urlPath}`,
      column,
      {
        enableScripts: true,
        retainContextWhenHidden: true,
        localResourceRoots: [extensionUri]
      }
    );

    // Initialize state
    const urlPath = endpointConfig['url-path'] || endpointConfig.urlPath;
    const method = endpointConfig.method || 'GET';
    const state = await parameterStorage.initializeEndpointState(urlPath, baseUrl, method);

    EndpointTesterPanel.currentPanel = new EndpointTesterPanel(
      panel,
      extensionUri,
      parameterStorage,
      testService,
      endpointConfig,
      state
    );

    return EndpointTesterPanel.currentPanel;
  }

  public dispose() {
    EndpointTesterPanel.currentPanel = undefined;

    // Clean up our resources
    this._panel.dispose();

    while (this._disposables.length) {
      const disposable = this._disposables.pop();
      if (disposable) {
        disposable.dispose();
      }
    }
  }

  private async _handleMessage(message: TestEndpointMessage) {
    switch (message.command) {
      case 'test':
        await this._executeTest(message.data);
        break;
      case 'updateParams':
        await this._updateParameters(message.data);
        break;
      case 'updateHeaders':
        await this._updateHeaders(message.data);
        break;
      case 'loadHistory':
        await this._loadHistory();
        break;
      case 'clearHistory':
        await this._clearHistory();
        break;
    }
  }

  private async _executeTest(data: any) {
    try {
      const { parameters, headers, body, authConfig } = data;
      
      // Execute the request
      const response = await this.testService.executeRequest(
        this._state.baseUrl,
        this._state.slug,
        this._state.method,
        parameters,
        headers,
        body,
        authConfig
      );

      // Save to history
      const historyEntry = this.testService.createHistoryEntry(
        parameters,
        headers,
        response,
        body
      );
      await this.parameterStorage.addToHistory(this._state.slug, historyEntry);

      // Send response back to webview
      this._panel.webview.postMessage({
        command: 'testResponse',
        response: response
      });
    } catch (error: any) {
      this._panel.webview.postMessage({
        command: 'testError',
        error: error.message
      });
    }
  }

  private async _updateParameters(parameters: Record<string, string>) {
    await this.parameterStorage.updateParameters(this._state.slug, parameters);
    this._state.parameters = parameters;
  }

  private async _updateHeaders(headers: Record<string, string>) {
    await this.parameterStorage.updateHeaders(this._state.slug, headers);
    this._state.headers = headers;
  }

  private async _loadHistory() {
    const state = this.parameterStorage.getEndpointState(this._state.slug);
    if (state) {
      this._panel.webview.postMessage({
        command: 'historyLoaded',
        history: state.history
      });
    }
  }

  private async _clearHistory() {
    await this.parameterStorage.clearHistory(this._state.slug);
    this._panel.webview.postMessage({
      command: 'historyCleared'
    });
  }

  private _update() {
    const webview = this._panel.webview;
    this._panel.title = `Test: ${this._state.slug}`;
    this._panel.webview.html = this._getHtmlForWebview(webview);
    
    // Send initial state to webview
    setTimeout(() => {
      webview.postMessage({
        command: 'init',
        config: this._endpointConfig,
        state: this._state
      });
    }, 100);
  }

  private _getOperationInfoHtml(): string {
    const operation = this._endpointConfig.operation || this._endpointConfig['operation'];
    const method = this._endpointConfig.method || 'GET';
    
    // Determine operation type
    let opType = 'Read';
    let opDetails: string[] = [];
    let showWarning = false;
    
    if (operation && typeof operation === 'object') {
      const op = operation as Record<string, unknown>;
      opType = op.type === 'write' ? 'Write' : 'Read';
      
      if (op.validate_before_write === true) {
        opDetails.push('validate-before-write');
        showWarning = true;
      }
      if (op.returns_data === true) {
        opDetails.push('returns-data');
      }
      if (op.transaction === true) {
        opDetails.push('transaction');
      }
    } else {
      // Infer from HTTP method
      if (['POST', 'PUT', 'PATCH', 'DELETE'].includes(method.toUpperCase())) {
        opType = 'Write';
      }
    }
    
    let html = `<div>Operation: <code>${opType}</code>`;
    if (opDetails.length > 0) {
      html += ` <span style="color: var(--vscode-descriptionForeground); font-size: 0.9em;">(${opDetails.join(', ')})</span>`;
    }
    html += '</div>';
    
    if (showWarning) {
      html += `<div style="margin-top: 4px; padding: 4px 8px; background: var(--vscode-inputValidation-warningBackground); border-radius: 3px; font-size: 0.85em; color: var(--vscode-warningForeground);">
        ⚠️ Validation enabled: Unknown parameters will be rejected
      </div>`;
    }
    
    return html;
  }

  private _getHtmlForWebview(webview: vscode.Webview): string {
    // Get the request fields from the config
    const requestFields: RequestFieldDefinition[] = this._getRequestFields();
    const urlPath = this._endpointConfig['url-path'] || this._endpointConfig.urlPath || '';
    const method = this._endpointConfig.method || 'GET';
    
    return `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Flapi Endpoint Tester</title>
    <link href="https://cdn.jsdelivr.net/npm/@vscode/codicons@0.0.32/dist/codicon.css" rel="stylesheet" />
    <style>
        ${this._getStyles()}
    </style>
</head>
<body>
    <div class="container">
        <!-- Header Section -->
        <div class="header">
            <h2>REST Endpoint Tester</h2>
            <div class="header-info">
                <div>Endpoint: <code>${urlPath}</code></div>
                <div>Method: <code>${method}</code></div>
                ${this._getOperationInfoHtml()}
            </div>
        </div>

        <!-- Main Content (Request) -->
        <div class="main-content">
            <!-- URL Bar -->
            <div class="url-section">
                <select id="method" class="method-select">
                    <option value="GET">GET</option>
                    <option value="POST">POST</option>
                    <option value="PUT">PUT</option>
                    <option value="DELETE">DELETE</option>
                    <option value="PATCH">PATCH</option>
                </select>
                <input type="text" id="url" class="url-input" placeholder="http://localhost:8080/endpoint" />
                <button id="sendBtn" class="send-btn">
                    <span class="codicon codicon-play"></span>
                    <span class="btn-text">Send</span>
                </button>
            </div>

            <!-- Query Parameters (Collapsible) -->
            <div class="collapsible-section">
                <div class="collapsible-header" onclick="toggleSection('params')">
                    <div>
                        <span class="expand-icon" id="params-icon">▼</span>
                        <strong>Query Parameters</strong>
                    </div>
                </div>
                <div class="collapsible-content expanded" id="params-content">
                    <div id="paramsContainer" class="params-container"></div>
                    <button id="addParamBtn" class="add-btn">+ Add Parameter</button>
                    
                    <!-- Datalist for parameter name autocomplete -->
                    <datalist id="paramNamesList">
                    </datalist>
                </div>
            </div>

            <!-- Authentication (Collapsible) -->
            <div class="collapsible-section">
                <div class="collapsible-header" onclick="toggleSection('auth')">
                    <div>
                        <span class="expand-icon" id="auth-icon">▶</span>
                        <strong>Authentication</strong>
                    </div>
                </div>
                <div class="collapsible-content" id="auth-content">
                    <div class="auth-container">
                        <div class="auth-type-row">
                            <label>Type:</label>
                            <select id="authType" class="auth-select">
                                <option value="none">No Auth</option>
                                <option value="basic">Basic Auth</option>
                                <option value="bearer">Bearer Token</option>
                                <option value="apikey">API Key</option>
                            </select>
                        </div>
                        <div id="authFields" class="auth-fields"></div>
                    </div>
                </div>
            </div>

            <!-- Headers (Collapsible) -->
            <div class="collapsible-section">
                <div class="collapsible-header" onclick="toggleSection('headers')">
                    <div>
                        <span class="expand-icon" id="headers-icon">▶</span>
                        <strong>Headers (optional)</strong>
                    </div>
                </div>
                <div class="collapsible-content" id="headers-content">
                    <div id="headersContainer" class="params-container"></div>
                    <button id="addHeaderBtn" class="add-btn">+ Add Header</button>
                </div>
            </div>

            <!-- Request Body (Collapsible, for POST/PUT) -->
            <div class="collapsible-section" id="bodySection" style="display: none;">
                <div class="collapsible-header" onclick="toggleSection('body')">
                    <div>
                        <span class="expand-icon" id="body-icon">▶</span>
                        <strong>Request Body</strong>
                    </div>
                </div>
                <div class="collapsible-content" id="body-content">
                    <textarea id="requestBody" class="body-textarea" placeholder="Enter JSON body..."></textarea>
                </div>
            </div>
        </div>

        <!-- Resize Handle -->
        <div class="resize-handle" id="resizeHandle"></div>

        <!-- Response Section -->
        <div class="response-section">
            <div class="section-header">Response</div>
            
            <!-- Status Bar -->
            <div id="statusBar" class="status-bar">
                <span id="statusText" class="status-text">Ready</span>
                <span id="timeText" class="time-text"></span>
                <span id="sizeText" class="size-text"></span>
            </div>

            <!-- Response Tabs -->
            <div class="response-tabs">
                <button class="response-tab active" data-tab="body">Body</button>
                <button class="response-tab" data-tab="headers">Headers</button>
            </div>

            <!-- Response Tab Content -->
            <div class="response-tab-content">
                <div id="tabBody" class="tab-pane active">
                    <!-- JSON View -->
                    <div id="jsonView" class="response-body-container">
                        <div class="json-toolbar">
                            <button id="expandAllBtn" class="json-tool-btn" title="Expand all">▼ Expand All</button>
                            <button id="collapseAllBtn" class="json-tool-btn" title="Collapse all">▶ Collapse All</button>
                        </div>
                        <div id="jsonRenderer" class="json-renderer">
                            <pre id="responseBody" class="response-body">Click "Send" to execute the request...</pre>
                        </div>
                    </div>
                    
                    <!-- Table View -->
                    <div id="tableView" class="response-table-container" style="display: none;">
                        <div id="dataTable" class="data-table-wrapper">
                            <p style="text-align: center; color: var(--vscode-descriptionForeground);">No data to display</p>
                        </div>
                    </div>
                    
                    <!-- View Toggle (at bottom) -->
                    <div class="view-toggle-bottom">
                        <button id="jsonViewBtn" class="view-toggle-btn active">JSON</button>
                        <button id="tableViewBtn" class="view-toggle-btn">Table</button>
                    </div>
                    
                    <!-- Pagination Controls -->
                    <div id="paginationControls" class="pagination-controls" style="display: none;">
                        <button id="prevPageBtn" class="pagination-btn" disabled>← Previous</button>
                        <span id="pageInfo" class="page-info">Showing 0 records</span>
                        <button id="nextPageBtn" class="pagination-btn" disabled>Next →</button>
                        <span id="totalInfo" class="total-info"></span>
                    </div>
                </div>
                <div id="tabHeaders" class="tab-pane">
                    <div class="headers-table-container">
                        <table id="headersTable" class="headers-table">
                            <thead>
                                <tr>
                                    <th>Header</th>
                                    <th>Value</th>
                                </tr>
                            </thead>
                            <tbody id="headersTableBody">
                                <tr><td colspan="2" style="text-align: center; color: var(--vscode-descriptionForeground);">No response yet</td></tr>
                            </tbody>
                        </table>
                    </div>
                </div>
            </div>

            <!-- Action Buttons -->
            <div class="action-buttons">
                <button id="copyBtn" class="action-btn" title="Copy response">
                    <span class="codicon codicon-copy"></span>
                </button>
                <button id="formatBtn" class="action-btn" title="Format JSON">
                    <span class="codicon codicon-symbol-field"></span>
                </button>
                <button id="historyBtn" class="action-btn" title="Request history">
                    <span class="codicon codicon-history"></span>
                </button>
            </div>
        </div>

        <!-- History Panel (initially hidden) -->
        <div id="historyPanel" class="history-panel" style="display: none;">
            <div class="history-header">
                <h3>Request History</h3>
                <button id="closeHistoryBtn" class="close-btn">✕</button>
            </div>
            <div id="historyList" class="history-list"></div>
            <button id="clearHistoryBtn" class="clear-history-btn">Clear History</button>
        </div>
    </div>

    <script>
        ${this._getScript()}
    </script>
</body>
</html>`;
  }

  private _getRequestFields(): RequestFieldDefinition[] {
    const fields: RequestFieldDefinition[] = [];
    const request = this._endpointConfig.request || [];

    for (const field of request) {
      fields.push({
        fieldName: field['field-name'] || field.fieldName,
        fieldIn: field['field-in'] || field.fieldIn || 'query',
        description: field.description || '',
        required: field.required || false,
        defaultValue: field.default || field.defaultValue || ''
      });
    }

    return fields;
  }

  private _getStyles(): string {
    return `
      * {
        box-sizing: border-box;
        margin: 0;
        padding: 0;
      }

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

      /* Header Section */
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
        display: flex;
        gap: 16px;
      }

      .header-info code {
        background: var(--vscode-textCodeBlock-background);
        padding: 2px 6px;
        border-radius: 3px;
        font-family: var(--vscode-editor-font-family);
      }

      /* Main Content Area */
      .main-content {
        flex: 1;
        overflow-y: auto;
        padding: 16px;
        min-height: 200px;
      }

      /* URL Section */
      .url-section {
        display: flex;
        gap: 8px;
        margin-bottom: 16px;
        padding-bottom: 16px;
        border-bottom: 1px solid var(--vscode-panel-border);
      }

      .method-select {
        padding: 8px 12px;
        background-color: var(--vscode-input-background);
        color: var(--vscode-input-foreground);
        border: 1px solid var(--vscode-input-border);
        border-radius: 4px;
        font-size: 13px;
        cursor: pointer;
        min-width: 90px;
      }

      .url-input {
        flex: 1;
        padding: 8px 12px;
        background-color: var(--vscode-input-background);
        color: var(--vscode-input-foreground);
        border: 1px solid var(--vscode-input-border);
        border-radius: 4px;
        font-size: 13px;
        font-family: var(--vscode-editor-font-family);
      }

      .send-btn {
        padding: 8px 16px;
        background-color: var(--vscode-button-background);
        color: var(--vscode-button-foreground);
        border: none;
        border-radius: 4px;
        font-size: 13px;
        font-weight: 600;
        cursor: pointer;
        white-space: nowrap;
        display: flex;
        align-items: center;
        gap: 6px;
      }

      .send-btn:hover {
        background-color: var(--vscode-button-hoverBackground);
      }

      .send-btn .codicon {
        font-size: 14px;
      }

      /* Collapsible Sections */
      .collapsible-section {
        border-bottom: 1px solid var(--vscode-panel-border);
        margin-bottom: 12px;
      }

      .collapsible-header {
        padding: 8px 16px 8px 0;
        display: flex;
        justify-content: space-between;
        align-items: center;
        cursor: pointer;
        user-select: none;
      }

      .collapsible-header:hover {
        background-color: var(--vscode-list-hoverBackground);
      }

      .collapsible-header > div {
        display: flex;
        align-items: center;
        gap: 8px;
      }

      .expand-icon {
        display: inline-block;
        transition: transform 0.2s;
        font-size: 12px;
        width: 16px;
        text-align: center;
      }

      .collapsible-content {
        padding: 0 0 12px 24px;
        display: none;
      }

      .collapsible-content.expanded {
        display: block;
      }

      /* Resize Handle */
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

      /* Parameters Section (Shared Component CSS) */
      .params-section, .headers-section, .body-section {
        margin-bottom: 16px;
      }

      .params-container {
        display: flex;
        flex-direction: column;
        gap: 8px;
        margin-bottom: 8px;
      }

      /* Shared parameter editor styles */
      ${getParameterEditorCSS()}

      .add-btn {
        padding: 6px 12px;
        background-color: var(--vscode-button-secondaryBackground);
        color: var(--vscode-button-secondaryForeground);
        border: none;
        border-radius: 4px;
        font-size: 13px;
        cursor: pointer;
      }

      .add-btn:hover {
        background-color: var(--vscode-button-secondaryHoverBackground);
      }

      .body-textarea {
        width: 100%;
        min-height: 120px;
        padding: 10px;
        background-color: var(--vscode-input-background);
        color: var(--vscode-input-foreground);
        border: 1px solid var(--vscode-input-border);
        border-radius: 4px;
        font-family: var(--vscode-editor-font-family);
        font-size: 13px;
        resize: vertical;
      }

      /* Response Section */
      .response-section {
        flex: 0 0 auto;
        min-height: 100px;
        max-height: 60vh;
        overflow: hidden;
        display: flex;
        flex-direction: column;
        border-top: 1px solid var(--vscode-panel-border);
      }

      .section-header {
        padding: 8px 16px;
        background-color: var(--vscode-sideBar-background);
        border-bottom: 1px solid var(--vscode-panel-border);
        font-weight: 600;
        font-size: 13px;
      }

      .status-bar {
        display: flex;
        gap: 16px;
        padding: 8px 12px;
        background-color: var(--vscode-sideBar-background);
        border-bottom: 1px solid var(--vscode-panel-border);
        font-size: 12px;
      }

      .status-text {
        font-weight: 600;
      }

      .status-text.success {
        color: #4ec9b0;
      }

      .status-text.error {
        color: #f48771;
      }

      .status-text.client-error {
        color: #ce9178;
      }

      .time-text, .size-text {
        color: var(--vscode-descriptionForeground);
      }

      .response-body-container {
        display: flex;
        flex-direction: column;
        border: 1px solid var(--vscode-panel-border);
        border-radius: 4px;
        margin-bottom: 12px;
        max-height: 500px;
        overflow: hidden;
      }

      .json-toolbar {
        display: flex;
        gap: 4px;
        padding: 6px 8px;
        background-color: var(--vscode-editorWidget-background);
        border-bottom: 1px solid var(--vscode-panel-border);
      }

      .json-tool-btn {
        padding: 4px 8px;
        background: var(--vscode-button-secondaryBackground);
        color: var(--vscode-button-secondaryForeground);
        border: none;
        border-radius: 3px;
        font-size: 11px;
        cursor: pointer;
      }

      .json-tool-btn:hover {
        background: var(--vscode-button-secondaryHoverBackground);
      }

      .json-renderer {
        flex: 1;
        overflow: auto;
        background-color: var(--vscode-editor-background);
      }

      .response-body {
        padding: 12px;
        margin: 0;
        font-family: var(--vscode-editor-font-family);
        font-size: 13px;
        line-height: 1.6;
        color: var(--vscode-editor-foreground);
        white-space: pre-wrap;
        word-wrap: break-word;
      }

      /* JSON syntax highlighting */
      .json-key {
        color: #9CDCFE;
        font-weight: 500;
      }

      .json-string {
        color: #CE9178;
      }

      .json-number {
        color: #B5CEA8;
      }

      .json-boolean {
        color: #569CD6;
        font-weight: 600;
      }

      .json-null {
        color: #569CD6;
        font-style: italic;
      }

      .json-collapsible {
        cursor: pointer;
        user-select: none;
      }

      .json-collapsible::before {
        content: '▼ ';
        display: inline-block;
        width: 12px;
        color: var(--vscode-descriptionForeground);
      }

      .json-collapsed::before {
        content: '▶ ';
      }

      .json-collapsed + .json-content {
        display: none;
      }

      .action-buttons {
        display: flex;
        gap: 8px;
        padding: 8px 12px;
        border-top: 1px solid var(--vscode-panel-border);
        background: var(--vscode-sideBar-background);
      }

      .action-btn {
        padding: 8px 12px;
        border: none;
        border-radius: 4px;
        cursor: pointer;
        display: flex;
        align-items: center;
        justify-content: center;
        min-width: 36px;
        height: 32px;
        background: var(--vscode-button-secondaryBackground);
        color: var(--vscode-button-secondaryForeground);
      }

      .action-btn .codicon {
        font-size: 16px;
      }

      .action-btn:hover {
        background: var(--vscode-button-secondaryHoverBackground);
      }

      /* History Panel */
      .history-panel {
        position: fixed;
        top: 50%;
        left: 50%;
        transform: translate(-50%, -50%);
        width: 600px;
        max-height: 70vh;
        background-color: var(--vscode-editor-background);
        border: 1px solid var(--vscode-panel-border);
        border-radius: 8px;
        padding: 16px;
        box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
        z-index: 1000;
        overflow-y: auto;
      }

      .history-header {
        display: flex;
        justify-content: space-between;
        align-items: center;
        margin-bottom: 16px;
      }

      .close-btn {
        padding: 4px 8px;
        background: none;
        border: none;
        color: var(--vscode-foreground);
        font-size: 18px;
        cursor: pointer;
      }

      .history-list {
        max-height: 400px;
        overflow-y: auto;
        margin-bottom: 12px;
      }

      .history-item {
        padding: 12px;
        border: 1px solid var(--vscode-panel-border);
        border-radius: 4px;
        margin-bottom: 8px;
        cursor: pointer;
      }

      .history-item:hover {
        background-color: var(--vscode-list-hoverBackground);
      }

      .history-item-header {
        display: flex;
        justify-content: space-between;
        margin-bottom: 6px;
        font-size: 12px;
      }

      .history-item-params {
        font-size: 11px;
        color: var(--vscode-descriptionForeground);
      }

      .clear-history-btn {
        width: 100%;
        padding: 8px;
        background-color: var(--vscode-button-secondaryBackground);
        color: var(--vscode-button-secondaryForeground);
        border: none;
        border-radius: 4px;
        cursor: pointer;
      }

      /* Authentication Section */
      .auth-section, .headers-section, .body-section {
        margin-bottom: 16px;
      }

      .auth-container {
        margin-top: 12px;
      }

      .auth-type-row {
        display: flex;
        align-items: center;
        gap: 12px;
        margin-bottom: 12px;
      }

      .auth-type-row label {
        font-size: 13px;
        min-width: 50px;
      }

      .auth-select {
        flex: 1;
        padding: 6px 10px;
        background-color: var(--vscode-input-background);
        color: var(--vscode-input-foreground);
        border: 1px solid var(--vscode-input-border);
        border-radius: 4px;
        font-size: 13px;
        cursor: pointer;
      }

      .auth-fields {
        display: flex;
        flex-direction: column;
        gap: 8px;
      }

      .auth-field-row {
        display: flex;
        align-items: center;
        gap: 12px;
      }

      .auth-field-row label {
        font-size: 13px;
        min-width: 100px;
      }

      .auth-field-row input {
        flex: 1;
        padding: 6px 10px;
        background-color: var(--vscode-input-background);
        color: var(--vscode-input-foreground);
        border: 1px solid var(--vscode-input-border);
        border-radius: 4px;
        font-size: 13px;
      }

      /* Response Tabs */
      .response-tabs {
        display: flex;
        gap: 4px;
        margin-bottom: 12px;
        border-bottom: 1px solid var(--vscode-panel-border);
      }

      .response-tab {
        padding: 8px 16px;
        background: none;
        border: none;
        border-bottom: 2px solid transparent;
        color: var(--vscode-descriptionForeground);
        font-size: 13px;
        cursor: pointer;
        transition: all 0.2s;
      }

      .response-tab:hover {
        color: var(--vscode-foreground);
        background-color: var(--vscode-list-hoverBackground);
      }

      .response-tab.active {
        color: var(--vscode-foreground);
        border-bottom-color: var(--vscode-button-background);
        font-weight: 600;
      }

      .response-tab-content {
        margin-bottom: 12px;
      }

      .tab-pane {
        display: none;
      }

      .tab-pane.active {
        display: block;
      }

      /* Tables for Headers and Cookies */
      .headers-table-container, .cookies-table-container {
        background-color: var(--vscode-editor-background);
        border: 1px solid var(--vscode-panel-border);
        border-radius: 4px;
        max-height: 400px;
        overflow: auto;
      }

      .headers-table, .cookies-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 13px;
      }

      .headers-table th, .cookies-table th {
        background-color: var(--vscode-editorWidget-background);
        color: var(--vscode-foreground);
        font-weight: 600;
        text-align: left;
        padding: 10px 12px;
        border-bottom: 1px solid var(--vscode-panel-border);
        position: sticky;
        top: 0;
        z-index: 1;
      }

      .headers-table td, .cookies-table td {
        padding: 8px 12px;
        border-bottom: 1px solid var(--vscode-panel-border);
        font-family: var(--vscode-editor-font-family);
      }

      .headers-table tr:last-child td, .cookies-table tr:last-child td {
        border-bottom: none;
      }

      .headers-table tbody tr:hover, .cookies-table tbody tr:hover {
        background-color: var(--vscode-list-hoverBackground);
      }

      /* View Toggle (at bottom) */
      .view-toggle-bottom {
        display: flex;
        gap: 4px;
        justify-content: center;
        margin-top: 12px;
        padding: 8px;
        border-top: 1px solid var(--vscode-panel-border);
        background-color: var(--vscode-editorWidget-background);
      }

      .view-toggle-btn {
        padding: 6px 16px;
        background: none;
        border: none;
        border-bottom: 2px solid transparent;
        color: var(--vscode-descriptionForeground);
        font-size: 13px;
        cursor: pointer;
        transition: all 0.2s;
      }

      .view-toggle-btn:hover {
        color: var(--vscode-foreground);
        background-color: var(--vscode-list-hoverBackground);
      }

      .view-toggle-btn.active {
        color: var(--vscode-foreground);
        border-bottom-color: var(--vscode-button-background);
        font-weight: 600;
      }

      /* Table View */
      .response-table-container {
        max-height: 600px;
        overflow: auto;
        background-color: var(--vscode-editor-background);
        border: 1px solid var(--vscode-panel-border);
        border-radius: 4px;
      }

      .data-table-wrapper table {
        width: 100%;
        border-collapse: collapse;
        font-size: 13px;
      }

      .data-table-wrapper th {
        background-color: var(--vscode-editorWidget-background);
        color: var(--vscode-foreground);
        font-weight: 600;
        text-align: left;
        padding: 10px 12px;
        border-bottom: 1px solid var(--vscode-panel-border);
        position: sticky;
        top: 0;
        z-index: 1;
      }

      .data-table-wrapper td {
        padding: 8px 12px;
        border-bottom: 1px solid var(--vscode-panel-border);
        font-family: var(--vscode-editor-font-family);
        max-width: 300px;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
      }

      .data-table-wrapper tbody tr:hover {
        background-color: var(--vscode-list-hoverBackground);
      }

      .nested-object {
        color: var(--vscode-descriptionForeground);
        font-style: italic;
      }

      .json-object {
        color: var(--vscode-textPreformat-foreground);
        font-family: var(--vscode-editor-font-family);
        font-size: 12px;
        word-break: break-word;
      }

      /* Pagination */
      .pagination-controls {
        display: flex;
        align-items: center;
        justify-content: center;
        gap: 12px;
        margin-top: 16px;
        padding: 12px;
        background-color: var(--vscode-editorWidget-background);
        border: 1px solid var(--vscode-panel-border);
        border-radius: 4px;
      }

      .pagination-btn {
        padding: 6px 12px;
        background-color: var(--vscode-button-secondaryBackground);
        color: var(--vscode-button-secondaryForeground);
        border: none;
        border-radius: 4px;
        font-size: 13px;
        cursor: pointer;
      }

      .pagination-btn:hover:not(:disabled) {
        background-color: var(--vscode-button-secondaryHoverBackground);
      }

      .pagination-btn:disabled {
        opacity: 0.5;
        cursor: not-allowed;
      }

      .page-info {
        font-size: 13px;
        color: var(--vscode-foreground);
        font-weight: 600;
      }

      .total-info {
        font-size: 12px;
        color: var(--vscode-descriptionForeground);
        margin-left: 8px;
      }
    `;
  }

  private _getScript(): string {
    return `
      (function() {
        const vscode = acquireVsCodeApi();
        let currentState = null;
        let currentConfig = null;

        // Initialize
        window.addEventListener('message', event => {
          const message = event.data;
          
          switch (message.command) {
            case 'init':
              currentState = message.state;
              currentConfig = message.config;
              initializeUI();
              break;
            case 'testResponse':
              displayResponse(message.response);
              break;
            case 'testError':
              displayError(message.error);
              break;
            case 'historyLoaded':
              displayHistory(message.history);
              break;
            case 'historyCleared':
              document.getElementById('historyList').innerHTML = '<p style="text-align: center; color: var(--vscode-descriptionForeground);">No history yet</p>';
              break;
          }
        });

        // Initialize collapsible sections
        function initializeCollapsible() {
          // Restore collapsed/expanded state from localStorage
          ['params', 'auth', 'headers', 'body'].forEach(sectionId => {
            const isExpanded = localStorage.getItem(\`\${sectionId}-expanded\`) !== 'false';
            const content = document.getElementById(\`\${sectionId}-content\`);
            const icon = document.getElementById(\`\${sectionId}-icon\`);
            if (content && icon) {
              if (isExpanded) {
                content.classList.add('expanded');
                icon.textContent = '▼';
              } else {
                content.classList.remove('expanded');
                icon.textContent = '▶';
              }
            }
          });
        }

        // Toggle collapsible section
        window.toggleSection = function(sectionId) {
          const content = document.getElementById(\`\${sectionId}-content\`);
          const icon = document.getElementById(\`\${sectionId}-icon\`);
          if (!content || !icon) return;
          
          const isExpanded = content.classList.contains('expanded');
          
          if (isExpanded) {
            content.classList.remove('expanded');
            icon.textContent = '▶';
            localStorage.setItem(\`\${sectionId}-expanded\`, 'false');
          } else {
            content.classList.add('expanded');
            icon.textContent = '▼';
            localStorage.setItem(\`\${sectionId}-expanded\`, 'true');
          }
        };

        // Initialize resize handle
        function initializeResize() {
          const handle = document.getElementById('resizeHandle');
          const mainContent = document.querySelector('.main-content');
          const responseSection = document.querySelector('.response-section');
          
          if (!handle || !mainContent || !responseSection) return;
          
          let startY, startHeight;
          
          handle.addEventListener('mousedown', (e) => {
            handle.classList.add('dragging');
            startY = e.clientY;
            startHeight = mainContent.offsetHeight;
            
            document.addEventListener('mousemove', onMouseMove);
            document.addEventListener('mouseup', onMouseUp);
            e.preventDefault();
          });
          
          function onMouseMove(e) {
            const delta = e.clientY - startY;
            const newHeight = Math.max(200, startHeight + delta);
            mainContent.style.height = \`\${newHeight}px\`;
            mainContent.style.flex = '0 0 auto';
          }
          
          function onMouseUp() {
            handle.classList.remove('dragging');
            document.removeEventListener('mousemove', onMouseMove);
            document.removeEventListener('mouseup', onMouseUp);
          }
        }

        function initializeUI() {
          // Initialize collapsible sections and resize
          initializeCollapsible();
          initializeResize();
          
          // Set method and URL
          document.getElementById('method').value = currentState.method;
          document.getElementById('url').value = currentState.baseUrl + currentState.slug;

          // Initialize parameters from config
          const requestFields = currentConfig.request || [];
          const paramsContainer = document.getElementById('paramsContainer');
          paramsContainer.innerHTML = '';
          
          // Populate datalist with all available parameter names
          populateParamNamesList(requestFields);

          requestFields.forEach(field => {
            const fieldName = field['field-name'] || field.fieldName;
            const savedValue = currentState.parameters[fieldName] || field.default || field.defaultValue || '';
            addParameterRow(fieldName, savedValue, field.description, field.required);
          });

          // Initialize headers
          const headersContainer = document.getElementById('headersContainer');
          headersContainer.innerHTML = '';
          Object.entries(currentState.headers).forEach(([key, value]) => {
            addHeaderRow(key, value);
          });

          // Show/hide body section based on method
          updateBodySection();

          // Initialize authentication
          initializeAuth();

          // Show/hide body section based on method
          updateBodySection();

          // Initialize response tabs
          initializeTabs();
          
          // Initialize view toggle
          initializeViewToggle();
          
          // Initialize pagination
          initializePagination();

          // Event listeners
          document.getElementById('sendBtn').addEventListener('click', sendRequest);
          document.getElementById('addParamBtn').addEventListener('click', () => addParameterRow('', '', '', false));
          document.getElementById('addHeaderBtn').addEventListener('click', () => addHeaderRow('', ''));
          document.getElementById('method').addEventListener('change', updateBodySection);
          document.getElementById('copyBtn').addEventListener('click', copyResponse);
          document.getElementById('formatBtn').addEventListener('click', formatResponse);
          document.getElementById('historyBtn').addEventListener('click', showHistory);
          document.getElementById('closeHistoryBtn').addEventListener('click', hideHistory);
          document.getElementById('clearHistoryBtn').addEventListener('click', clearHistory);
          document.getElementById('authType').addEventListener('change', updateAuthFields);
        }
        
        function initializeViewToggle() {
          document.getElementById('jsonViewBtn').addEventListener('click', () => {
            document.getElementById('jsonViewBtn').classList.add('active');
            document.getElementById('tableViewBtn').classList.remove('active');
            document.getElementById('jsonView').style.display = 'block';
            document.getElementById('tableView').style.display = 'none';
          });
          
          document.getElementById('tableViewBtn').addEventListener('click', () => {
            document.getElementById('tableViewBtn').classList.add('active');
            document.getElementById('jsonViewBtn').classList.remove('active');
            document.getElementById('tableView').style.display = 'block';
            document.getElementById('jsonView').style.display = 'none';
          });
        }
        
        let nextUrl = null;
        let previousUrls = []; // Stack to track previous pages
        
        function initializePagination() {
          document.getElementById('prevPageBtn').addEventListener('click', () => {
            if (previousUrls.length > 0) {
              goToPreviousPage();
            }
          });
          
          document.getElementById('nextPageBtn').addEventListener('click', () => {
            if (nextUrl) {
              goToNextPage();
            }
          });
        }
        
        function goToNextPage() {
          if (!nextUrl) return;
          
          // Save current path (without base URL) to history
          const currentUrl = document.getElementById('url').value;
          const baseUrl = currentState.baseUrl;
          const currentPath = currentUrl.replace(baseUrl, '');
          previousUrls.push(currentPath);
          
          // Parse the next URL and update parameters
          updateUrlAndParams(nextUrl);
          
          // Trigger request
          sendRequest();
        }
        
        function goToPreviousPage() {
          if (previousUrls.length === 0) return;
          
          // Pop the previous path
          const prevPath = previousUrls.pop();
          
          // Update URL and parameters
          updateUrlAndParams(prevPath);
          
          // Trigger request
          sendRequest();
        }
        
        function updateUrlAndParams(urlPath) {
          // Parse URL to extract path and parameters
          const url = new URL('http://dummy' + urlPath);
          
          // Update URL field (only if it's a path, not a full URL)
          const baseUrl = currentState.baseUrl;
          let fullUrl;
          if (urlPath.startsWith('http://') || urlPath.startsWith('https://')) {
            // It's already a full URL (shouldn't happen, but handle it)
            fullUrl = urlPath;
          } else {
            // It's a path, prepend base URL
            fullUrl = baseUrl + urlPath;
          }
          document.getElementById('url').value = fullUrl;
          
          // Update parameter fields
          const paramsContainer = document.getElementById('paramsContainer');
          const existingParams = {};
          
          // Collect existing parameter rows
          const paramRows = paramsContainer.querySelectorAll('.param-row');
          paramRows.forEach(row => {
            const inputs = row.querySelectorAll('input');
            const name = inputs[0].value.trim();
            if (name) {
              existingParams[name] = row;
            }
          });
          
          // Update or add parameters from URL
          url.searchParams.forEach((value, name) => {
            if (existingParams[name]) {
              // Update existing parameter
              const inputs = existingParams[name].querySelectorAll('input');
              inputs[1].value = value;
            } else {
              // Add new parameter
              addParameterRow(name, value, '', false);
            }
          });
        }

        function initializeAuth() {
          // Check if endpoint has auth configured
          if (currentConfig.auth && currentConfig.auth.enabled) {
            const authSection = document.getElementById('authSection');
            authSection.open = true;
            
            // Detect auth type from config
            const authType = currentConfig.auth.type || 'none';
            if (authType === 'basic' && currentConfig.auth.users && currentConfig.auth.users.length > 0) {
              document.getElementById('authType').value = 'basic';
              updateAuthFields();
              // Pre-fill first user
              const firstUser = currentConfig.auth.users[0];
              document.getElementById('authUsername').value = firstUser.username || '';
              document.getElementById('authPassword').value = firstUser.password || '';
            }
          }
          
          updateAuthFields();
        }

        function updateAuthFields() {
          const authType = document.getElementById('authType').value;
          const authFields = document.getElementById('authFields');
          authFields.innerHTML = '';

          if (authType === 'basic') {
            authFields.innerHTML = \`
              <div class="auth-field-row">
                <label>Username:</label>
                <input type="text" id="authUsername" placeholder="Enter username" />
              </div>
              <div class="auth-field-row">
                <label>Password:</label>
                <input type="password" id="authPassword" placeholder="Enter password" />
              </div>
            \`;
          } else if (authType === 'bearer') {
            authFields.innerHTML = \`
              <div class="auth-field-row">
                <label>Token:</label>
                <input type="text" id="authToken" placeholder="Enter bearer token" />
              </div>
            \`;
          } else if (authType === 'apikey') {
            authFields.innerHTML = \`
              <div class="auth-field-row">
                <label>Header Name:</label>
                <input type="text" id="authHeaderName" placeholder="e.g., X-API-Key" />
              </div>
              <div class="auth-field-row">
                <label>API Key:</label>
                <input type="text" id="authApiKey" placeholder="Enter API key" />
              </div>
            \`;
          }
        }

        function getAuthConfig() {
          const authType = document.getElementById('authType').value;
          
          if (authType === 'none') {
            return { type: 'none' };
          }

          if (authType === 'basic') {
            return {
              type: 'basic',
              username: document.getElementById('authUsername')?.value || '',
              password: document.getElementById('authPassword')?.value || ''
            };
          }

          if (authType === 'bearer') {
            return {
              type: 'bearer',
              token: document.getElementById('authToken')?.value || ''
            };
          }

          if (authType === 'apikey') {
            return {
              type: 'apikey',
              headerName: document.getElementById('authHeaderName')?.value || '',
              apiKey: document.getElementById('authApiKey')?.value || ''
            };
          }

          return { type: 'none' };
        }

        function initializeTabs() {
          const tabs = document.querySelectorAll('.response-tab');
          tabs.forEach(tab => {
            tab.addEventListener('click', () => {
              // Remove active class from all tabs and panes
              document.querySelectorAll('.response-tab').forEach(t => t.classList.remove('active'));
              document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
              
              // Add active class to clicked tab and corresponding pane
              tab.classList.add('active');
              const tabName = tab.getAttribute('data-tab');
              document.getElementById('tab' + tabName.charAt(0).toUpperCase() + tabName.slice(1)).classList.add('active');
            });
          });
        }

        function populateParamNamesList(requestFields) {
          const datalist = document.getElementById('paramNamesList');
          datalist.innerHTML = '';
          
          requestFields.forEach(field => {
            const option = document.createElement('option');
            const fieldName = field['field-name'] || field.fieldName;
            option.value = fieldName;
            
            // Add description as label if available
            if (field.description) {
              option.label = field.description;
            }
            
            datalist.appendChild(option);
          });
        }
        
        function addParameterRow(name, value, description, required) {
          const container = document.getElementById('paramsContainer');
          const row = document.createElement('div');
          row.className = 'param-row';
          
          const nameInput = document.createElement('input');
          nameInput.type = 'text';
          nameInput.placeholder = required ? 'Parameter name *' : 'Parameter name';
          nameInput.value = name;
          nameInput.title = description || '';
          nameInput.setAttribute('list', 'paramNamesList');
          
          // Auto-fill default value when parameter name is selected from dropdown
          nameInput.addEventListener('input', (e) => {
            const selectedName = e.target.value;
            const requestFields = currentConfig.request || [];
            const matchingField = requestFields.find(f => 
              (f['field-name'] || f.fieldName) === selectedName
            );
            
            if (matchingField && !valueInput.value) {
              // Auto-fill with default value if exists
              const defaultVal = matchingField.default || matchingField.defaultValue || '';
              if (defaultVal) {
                valueInput.value = defaultVal;
              }
              
              // Update title with description
              if (matchingField.description) {
                nameInput.title = matchingField.description;
              }
              
              // Mark as required if applicable
              if (matchingField.required) {
                nameInput.placeholder = 'Parameter name *';
              }
            }
          });
          
          const valueInput = document.createElement('input');
          valueInput.type = 'text';
          valueInput.placeholder = 'Value';
          valueInput.value = value;
          
          const removeBtn = document.createElement('button');
          removeBtn.className = 'remove-btn';
          removeBtn.textContent = '✕';
          removeBtn.addEventListener('click', () => row.remove());
          
          row.appendChild(nameInput);
          row.appendChild(valueInput);
          row.appendChild(removeBtn);
          container.appendChild(row);
        }

        function addHeaderRow(name, value) {
          const container = document.getElementById('headersContainer');
          const row = document.createElement('div');
          row.className = 'param-row';
          
          const nameInput = document.createElement('input');
          nameInput.type = 'text';
          nameInput.placeholder = 'Header name';
          nameInput.value = name;
          
          const valueInput = document.createElement('input');
          valueInput.type = 'text';
          valueInput.placeholder = 'Value';
          valueInput.value = value;
          
          const removeBtn = document.createElement('button');
          removeBtn.className = 'remove-btn';
          removeBtn.textContent = '✕';
          removeBtn.addEventListener('click', () => row.remove());
          
          row.appendChild(nameInput);
          row.appendChild(valueInput);
          row.appendChild(removeBtn);
          container.appendChild(row);
        }

        function updateBodySection() {
          const method = document.getElementById('method').value;
          const bodySection = document.getElementById('bodySection');
          
          if (['POST', 'PUT', 'PATCH'].includes(method)) {
            bodySection.style.display = 'block';
          } else {
            bodySection.style.display = 'none';
          }
        }

        function sendRequest() {
          // Collect parameters
          const parameters = {};
          const paramRows = document.getElementById('paramsContainer').querySelectorAll('.param-row');
          paramRows.forEach(row => {
            const inputs = row.querySelectorAll('input');
            const name = inputs[0].value.trim();
            const value = inputs[1].value.trim();
            if (name) {
              parameters[name] = value;
            }
          });

          // Collect headers
          const headers = {};
          const headerRows = document.getElementById('headersContainer').querySelectorAll('.param-row');
          headerRows.forEach(row => {
            const inputs = row.querySelectorAll('input');
            const name = inputs[0].value.trim();
            const value = inputs[1].value.trim();
            if (name) {
              headers[name] = value;
            }
          });

          // Get body and validate JSON
          const bodyElement = document.getElementById('requestBody');
          const body = bodyElement.value.trim();
          let bodyJson = null;
          
          // Validate JSON body for write operations
          if (body && ['POST', 'PUT', 'PATCH'].includes(method)) {
            try {
              bodyJson = JSON.parse(body);
              // Clear any previous validation error
              const bodyError = document.getElementById('bodyError');
              if (bodyError) {
                bodyError.style.display = 'none';
                bodyError.textContent = '';
              }
            } catch (e) {
              // Show JSON syntax error
              const bodyError = document.getElementById('bodyError');
              if (!bodyError) {
                const errorDiv = document.createElement('div');
                errorDiv.id = 'bodyError';
                errorDiv.style.cssText = 'margin-top: 8px; padding: 8px; background: var(--vscode-inputValidation-errorBackground); color: var(--vscode-errorForeground); border-radius: 4px; font-size: 0.9em;';
                bodyElement.parentElement.appendChild(errorDiv);
              } else {
                bodyError.style.display = 'block';
              }
              const errorMsg = e instanceof Error ? e.message : String(e);
              document.getElementById('bodyError').textContent = '⚠️ Invalid JSON: ' + errorMsg;
              
              // Don't send request if JSON is invalid
              return;
            }
          }

          // Get auth config
          const authConfig = getAuthConfig();

          // Show loading state
          document.getElementById('statusText').textContent = 'Sending...';
          document.getElementById('statusText').className = 'status-text';
          const jsonRenderer = document.getElementById('jsonRenderer');
          if (jsonRenderer) {
            jsonRenderer.innerHTML = '<pre class="response-body">Loading...</pre>';
          }

          // Send test request
          vscode.postMessage({
            command: 'test',
            data: { parameters, headers, body, authConfig }
          });

          // Save parameters and headers
          vscode.postMessage({
            command: 'updateParams',
            data: parameters
          });
          vscode.postMessage({
            command: 'updateHeaders',
            data: headers
          });
        }

        function displayResponse(response) {
          const statusBar = document.getElementById('statusBar');
          const statusText = document.getElementById('statusText');
          const timeText = document.getElementById('timeText');
          const sizeText = document.getElementById('sizeText');
          const responseBody = document.getElementById('responseBody');

          // Update status
          statusText.textContent = response.status + ' ' + response.statusText;
          statusText.className = 'status-text ' + getStatusClass(response.status);
          
          // Update time and size
          timeText.textContent = 'Time: ' + formatTime(response.time);
          sizeText.textContent = 'Size: ' + formatBytes(response.size);

          // Parse response body
          let parsedData = null;
          try {
            parsedData = JSON.parse(response.body);
          } catch (e) {
            // Not JSON
          }

          // Detect write operation response (has rows_affected)
          const isWriteOp = parsedData && ('rows_affected' in parsedData || 'returned_data' in parsedData || 'last_insert_id' in parsedData);
          
          if (isWriteOp) {
            // Handle write operation response
            renderWriteOperationResponse(response, parsedData);
          } else {
            // Handle read operation response (standard)
            renderResponseBody(response, parsedData);

            // Update table view if data is paginated
            if (parsedData && parsedData.data && Array.isArray(parsedData.data)) {
              renderTableView(parsedData.data);
              updatePagination(parsedData);
            }
          }

          // Update headers tab
          renderResponseHeaders(response.headers);
        }
        
        function updatePagination(responseData) {
          const paginationControls = document.getElementById('paginationControls');
          const prevBtn = document.getElementById('prevPageBtn');
          const nextBtn = document.getElementById('nextPageBtn');
          const pageInfo = document.getElementById('pageInfo');
          const totalInfo = document.getElementById('totalInfo');
          
          // Update next URL from response
          nextUrl = responseData.next || null;
          
          // Show pagination controls if there's data
          paginationControls.style.display = 'flex';
          
          // Update button states
          prevBtn.disabled = previousUrls.length === 0;
          nextBtn.disabled = !nextUrl;
          
          // Show simple page info
          const dataCount = responseData.data ? responseData.data.length : 0;
          pageInfo.textContent = \`Showing \${dataCount} records\`;
          
          // Show total if available
          if (responseData.total_count !== undefined) {
            totalInfo.textContent = \`(\${responseData.total_count} total)\`;
          } else {
            totalInfo.textContent = '';
          }
        }
        
        function renderTableView(dataArray) {
          const tableWrapper = document.getElementById('dataTable');
          
          if (!dataArray || dataArray.length === 0) {
            tableWrapper.innerHTML = '<p style="text-align: center; color: var(--vscode-descriptionForeground);">No data to display</p>';
            return;
          }
          
          // Extract columns from first row
          const firstRow = dataArray[0];
          const columns = Object.keys(firstRow);
          
          // Build table HTML
          let tableHTML = '<table><thead><tr>';
          columns.forEach(col => {
            tableHTML += \`<th>\${col}</th>\`;
          });
          tableHTML += '</tr></thead><tbody>';
          
          // Add rows
          dataArray.forEach(row => {
            tableHTML += '<tr>';
            columns.forEach(col => {
              const value = row[col];
              let displayValue;
              
              if (value === null || value === undefined) {
                displayValue = '<span class="nested-object">null</span>';
              } else if (typeof value === 'object') {
                // Show JSON string for objects instead of {object}
                const jsonStr = JSON.stringify(value);
                const escaped = jsonStr.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
                displayValue = '<span class="json-object">' + escaped + '</span>';
              } else {
                const escaped = String(value).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
                displayValue = escaped;
              }
              
              tableHTML += \`<td>\${displayValue}</td>\`;
            });
            tableHTML += '</tr>';
          });
          
          tableHTML += '</tbody></table>';
          tableWrapper.innerHTML = tableHTML;
        }

        let currentJsonData = null;

        function renderWriteOperationResponse(response, parsedData) {
          const jsonRenderer = document.getElementById('jsonRenderer');
          
          // Store for copy/expand/collapse
          currentJsonData = parsedData;
          
          // Create write operation summary
          const summaryDiv = document.createElement('div');
          summaryDiv.className = 'write-operation-summary';
          summaryDiv.style.cssText = 'background: var(--vscode-editor-inactiveSelectionBackground); padding: 12px; margin-bottom: 12px; border-radius: 4px; border-left: 3px solid var(--vscode-activityBarBadge-background);';
          
          let summaryHTML = '<div style="font-weight: bold; margin-bottom: 8px; color: var(--vscode-activityBarBadge-background);">📝 Write Operation Result</div>';
          
          if (parsedData.rows_affected !== undefined) {
            summaryHTML += `<div style="margin: 4px 0;"><strong>Rows Affected:</strong> <span style="color: var(--vscode-textLink-foreground);">${parsedData.rows_affected}</span></div>`;
          }
          
          if (parsedData.last_insert_id !== undefined) {
            summaryHTML += `<div style="margin: 4px 0;"><strong>Last Insert ID:</strong> <span style="color: var(--vscode-textLink-foreground);">${parsedData.last_insert_id}</span></div>`;
          }
          
          if (parsedData.returned_data && Array.isArray(parsedData.returned_data) && parsedData.returned_data.length > 0) {
            summaryHTML += `<div style="margin: 4px 0;"><strong>Returned Data:</strong> ${parsedData.returned_data.length} record(s)</div>`;
          }
          
          if (parsedData.errors && Array.isArray(parsedData.errors) && parsedData.errors.length > 0) {
            summaryHTML += `<div style="margin: 8px 0; padding: 8px; background: var(--vscode-inputValidation-errorBackground); border-radius: 4px;">`;
            summaryHTML += '<div style="font-weight: bold; color: var(--vscode-errorForeground); margin-bottom: 4px;">⚠️ Validation Errors:</div>';
            parsedData.errors.forEach(error => {
              summaryHTML += `<div style="margin: 2px 0; color: var(--vscode-errorForeground);">• ${error.field || 'Unknown'}: ${error.message || 'Error'}</div>`;
            });
            summaryHTML += '</div>';
          } else if (parsedData.error) {
            summaryHTML += `<div style="margin: 8px 0; padding: 8px; background: var(--vscode-inputValidation-errorBackground); border-radius: 4px;">`;
            summaryHTML += `<div style="color: var(--vscode-errorForeground);">⚠️ Error: ${parsedData.error.field || 'Unknown'}: ${parsedData.error.message || 'Error'}</div>`;
            summaryHTML += '</div>';
          }
          
          summaryDiv.innerHTML = summaryHTML;
          
          // Render the full JSON with syntax highlighting
          jsonRenderer.innerHTML = '';
          jsonRenderer.appendChild(summaryDiv);
          
          const jsonElement = renderJsonWithFolding(currentJsonData, 0);
          jsonRenderer.appendChild(jsonElement);
          
          // Show table view if returned_data exists
          if (parsedData.returned_data && Array.isArray(parsedData.returned_data) && parsedData.returned_data.length > 0) {
            renderTableView(parsedData.returned_data);
          } else if (parsedData.data && Array.isArray(parsedData.data) && parsedData.data.length > 0) {
            // Some write operations might return data in the 'data' field
            renderTableView(parsedData.data);
          }
        }
        
        function renderResponseBody(response, parsedData) {
          const responseBody = document.getElementById('responseBody');
          const jsonRenderer = document.getElementById('jsonRenderer');
          const contentType = response.contentType || '';
          
          if (contentType.includes('json') || parsedData) {
            // Store for copy/expand/collapse
            currentJsonData = parsedData || JSON.parse(response.body);
            
            // Render with syntax highlighting and folding
            jsonRenderer.innerHTML = '';
            const jsonElement = renderJsonWithFolding(currentJsonData, 0);
            jsonRenderer.appendChild(jsonElement);
          } else if (contentType.includes('html')) {
            // Show HTML (escaped)
            jsonRenderer.innerHTML = '<pre class="response-body"></pre>';
            jsonRenderer.querySelector('pre').textContent = response.body;
          } else if (contentType.includes('xml')) {
            // Show XML
            jsonRenderer.innerHTML = '<pre class="response-body"></pre>';
            jsonRenderer.querySelector('pre').textContent = response.body;
          } else if (contentType.includes('image')) {
            // Show image preview message
            jsonRenderer.innerHTML = '<pre class="response-body">[Image content - ' + contentType + ']\\n\\nNote: Image preview not yet supported in this view.</pre>';
          } else {
            // Plain text or unknown
            jsonRenderer.innerHTML = '<pre class="response-body"></pre>';
            jsonRenderer.querySelector('pre').textContent = response.body;
          }
        }
        
        function renderJsonWithFolding(obj, indent) {
          const container = document.createElement('pre');
          container.className = 'response-body';
          container.appendChild(buildJsonNode(obj, indent));
          return container;
        }
        
        function buildJsonNode(obj, indent) {
          const node = document.createElement('span');
          const indentStr = '  '.repeat(indent);
          
          if (obj === null) {
            node.innerHTML = \`<span class="json-null">null</span>\`;
          } else if (typeof obj === 'boolean') {
            node.innerHTML = \`<span class="json-boolean">\${obj}</span>\`;
          } else if (typeof obj === 'number') {
            node.innerHTML = \`<span class="json-number">\${obj}</span>\`;
          } else if (typeof obj === 'string') {
            const escaped = obj.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
            node.innerHTML = \`<span class="json-string">"\${escaped}"</span>\`;
          } else if (Array.isArray(obj)) {
            if (obj.length === 0) {
              node.textContent = '[]';
            } else {
              const toggle = document.createElement('span');
              toggle.className = 'json-collapsible';
              toggle.textContent = '[';
              
              const content = document.createElement('span');
              content.className = 'json-content';
              content.textContent = '\\n';
              
              obj.forEach((item, i) => {
                content.appendChild(document.createTextNode(indentStr + '  '));
                content.appendChild(buildJsonNode(item, indent + 1));
                if (i < obj.length - 1) {
                  content.appendChild(document.createTextNode(','));
                }
                content.appendChild(document.createTextNode('\\n'));
              });
              
              content.appendChild(document.createTextNode(indentStr + ']'));
              
              toggle.addEventListener('click', (e) => {
                e.stopPropagation();
                toggle.classList.toggle('json-collapsed');
              });
              
              node.appendChild(toggle);
              node.appendChild(content);
            }
          } else if (typeof obj === 'object') {
            const keys = Object.keys(obj);
            if (keys.length === 0) {
              node.textContent = '{}';
            } else {
              const toggle = document.createElement('span');
              toggle.className = 'json-collapsible';
              toggle.textContent = '{';
              
              const content = document.createElement('span');
              content.className = 'json-content';
              content.textContent = '\\n';
              
              keys.forEach((key, i) => {
                content.appendChild(document.createTextNode(indentStr + '  '));
                const keySpan = document.createElement('span');
                keySpan.className = 'json-key';
                keySpan.textContent = \`"\${key}"\`;
                content.appendChild(keySpan);
                content.appendChild(document.createTextNode(': '));
                content.appendChild(buildJsonNode(obj[key], indent + 1));
                if (i < keys.length - 1) {
                  content.appendChild(document.createTextNode(','));
                }
                content.appendChild(document.createTextNode('\\n'));
              });
              
              content.appendChild(document.createTextNode(indentStr + '}'));
              
              toggle.addEventListener('click', (e) => {
                e.stopPropagation();
                toggle.classList.toggle('json-collapsed');
              });
              
              node.appendChild(toggle);
              node.appendChild(content);
            }
          }
          
          return node;
        }
        
        // JSON toolbar handlers
        document.getElementById('expandAllBtn').addEventListener('click', () => {
          document.querySelectorAll('.json-collapsible').forEach(el => {
            el.classList.remove('json-collapsed');
          });
        });
        
        document.getElementById('collapseAllBtn').addEventListener('click', () => {
          document.querySelectorAll('.json-collapsible').forEach(el => {
            el.classList.add('json-collapsed');
          });
        });

        function renderResponseHeaders(headers) {
          const headersTableBody = document.getElementById('headersTableBody');
          headersTableBody.innerHTML = '';

          if (!headers || Object.keys(headers).length === 0) {
            headersTableBody.innerHTML = '<tr><td colspan="2" style="text-align: center; color: var(--vscode-descriptionForeground);">No headers</td></tr>';
            return;
          }

          Object.entries(headers).forEach(([name, value]) => {
            const row = document.createElement('tr');
            const nameCell = document.createElement('td');
            const valueCell = document.createElement('td');
            
            nameCell.textContent = name;
            nameCell.style.fontWeight = '600';
            valueCell.textContent = value;
            
            row.appendChild(nameCell);
            row.appendChild(valueCell);
            headersTableBody.appendChild(row);
          });
        }


        function displayError(error) {
          const statusText = document.getElementById('statusText');
          const responseBody = document.getElementById('responseBody');

          statusText.textContent = 'Error';
          statusText.className = 'status-text error';
          responseBody.textContent = 'Error: ' + error;
        }

        function copyResponse() {
          const responseBody = document.getElementById('responseBody').textContent;
          navigator.clipboard.writeText(responseBody);
        }

        function formatResponse() {
          const responseBody = document.getElementById('responseBody');
          try {
            const parsed = JSON.parse(responseBody.textContent);
            responseBody.textContent = JSON.stringify(parsed, null, 2);
          } catch (e) {
            // Already formatted or not JSON
          }
        }

        function showHistory() {
          vscode.postMessage({ command: 'loadHistory' });
          document.getElementById('historyPanel').style.display = 'block';
        }

        function hideHistory() {
          document.getElementById('historyPanel').style.display = 'none';
        }

        function displayHistory(history) {
          const historyList = document.getElementById('historyList');
          historyList.innerHTML = '';

          if (!history || history.length === 0) {
            historyList.innerHTML = '<p style="text-align: center; color: var(--vscode-descriptionForeground);">No history yet</p>';
            return;
          }

          history.forEach((item, index) => {
            const historyItem = document.createElement('div');
            historyItem.className = 'history-item';
            
            const header = document.createElement('div');
            header.className = 'history-item-header';
            
            const timestamp = new Date(item.timestamp).toLocaleString();
            const status = item.response.status + ' ' + item.response.statusText;
            
            header.innerHTML = '<span>' + timestamp + '</span><span class="' + getStatusClass(item.response.status) + '">' + status + '</span>';
            
            const params = document.createElement('div');
            params.className = 'history-item-params';
            params.textContent = 'Params: ' + JSON.stringify(item.parameters);
            
            historyItem.appendChild(header);
            historyItem.appendChild(params);
            
            historyItem.addEventListener('click', () => {
              loadHistoryItem(item);
              hideHistory();
            });
            
            historyList.appendChild(historyItem);
          });
        }

        function loadHistoryItem(item) {
          // Load parameters
          const paramRows = document.getElementById('paramsContainer').querySelectorAll('.param-row');
          paramRows.forEach(row => {
            const inputs = row.querySelectorAll('input');
            const name = inputs[0].value.trim();
            if (item.parameters[name] !== undefined) {
              inputs[1].value = item.parameters[name];
            }
          });

          // Load headers
          const headersContainer = document.getElementById('headersContainer');
          headersContainer.innerHTML = '';
          Object.entries(item.headers).forEach(([key, value]) => {
            addHeaderRow(key, value);
          });

          // Display the response
          displayResponse(item.response);
        }

        function clearHistory() {
          vscode.postMessage({ command: 'clearHistory' });
        }

        function getStatusClass(status) {
          if (status === 0) return 'error';
          if (status >= 200 && status < 300) return 'success';
          if (status >= 400) return 'client-error';
          return '';
        }

        function formatTime(ms) {
          if (ms < 1000) return ms + 'ms';
          return (ms / 1000).toFixed(2) + 's';
        }

        function formatBytes(bytes) {
          if (bytes === 0) return '0 B';
          const k = 1024;
          const sizes = ['B', 'KB', 'MB', 'GB'];
          const i = Math.floor(Math.log(bytes) / Math.log(k));
          return (bytes / Math.pow(k, i)).toFixed(2) + ' ' + sizes[i];
        }
      })();
    `;
  }
}

