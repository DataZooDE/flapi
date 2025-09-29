import * as vscode from 'vscode';
import type { AxiosInstance } from 'axios';
import { slugToPath, buildEndpointUrl } from '@flapi/shared';

class EndpointDocument implements vscode.CustomDocument {
  constructor(
    public readonly uri: vscode.Uri,
    public readonly path: string,
    public readonly slug: string,
  ) {}

  dispose(): void {}
}

interface WebviewMessage {
  type: string;
  requestId?: string;
  payload?: any;
}

export class EndpointEditorProvider implements vscode.CustomEditorProvider<EndpointDocument> {
  public static readonly viewType = 'flapi.endpointEditor';

  private readonly webviews = new Map<string, vscode.WebviewPanel>();
  private readonly onDidChangeEmitter = new vscode.EventEmitter<vscode.CustomDocumentEditEvent<EndpointDocument>>();
  public readonly onDidChangeCustomDocument = this.onDidChangeEmitter.event;

  constructor(private readonly client: AxiosInstance, private readonly extensionUri: vscode.Uri) {}

  async openCustomDocument(uri: vscode.Uri, _openContext: vscode.CustomDocumentOpenContext, _token: vscode.CancellationToken): Promise<EndpointDocument> {
    const slug = uri.path.replace(/^\//, '').replace(/\.json$/i, '');
    const path = slugToPath(slug);
    return new EndpointDocument(uri, path, slug);
  }

  async resolveCustomEditor(document: EndpointDocument, webviewPanel: vscode.WebviewPanel, _token: vscode.CancellationToken): Promise<void> {
    this.webviews.set(document.uri.toString(), webviewPanel);
    webviewPanel.onDidDispose(() => {
      this.webviews.delete(document.uri.toString());
    });

    webviewPanel.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.joinPath(this.extensionUri, 'webview')],
    };

    webviewPanel.webview.html = await this.getHtmlForWebview(webviewPanel.webview, document);

    webviewPanel.webview.onDidReceiveMessage(async (message: WebviewMessage) => {
      switch (message.type) {
        case 'load':
          await this.handleLoadRequest(webviewPanel, document, message.requestId);
          break;
        case 'save-template':
          await this.handleSaveTemplate(webviewPanel, document, message.payload, message.requestId);
          break;
        case 'expand-template':
          await this.handleExpandTemplate(webviewPanel, document, message.payload, message.requestId);
          break;
        case 'test-template':
          await this.handleTestTemplate(webviewPanel, document, message.payload, message.requestId);
          break;
        case 'save-cache':
          await this.handleSaveCache(webviewPanel, document, message.payload, message.requestId);
          break;
        case 'save-request':
          await this.handleSaveRequest(webviewPanel, document, message.payload, message.requestId);
          break;
        case 'refresh':
          await this.handleLoadRequest(webviewPanel, document, message.requestId, true);
          break;
            case 'request-schema':
              await this.handleSchemaRequest(webviewPanel, message.requestId);
              break;
            case 'validate-template':
              await this.handleValidateTemplate(webviewPanel, document, message.payload, message.requestId);
              break;
            default:
              console.warn('[flapi] Unknown message type from webview', message.type);
              break;
      }
    });
  }

  async saveCustomDocument(_document: EndpointDocument, _cancellation: vscode.CancellationToken): Promise<void> {
    return Promise.resolve();
  }

  async saveCustomDocumentAs(_document: EndpointDocument, _destination: vscode.Uri, _cancellation: vscode.CancellationToken): Promise<void> {
    return Promise.resolve();
  }

  async revertCustomDocument(document: EndpointDocument, _cancellation: vscode.CancellationToken): Promise<void> {
    const panel = this.webviews.get(document.uri.toString());
    panel?.webview.postMessage({ type: 'refresh' });
  }

  async backupCustomDocument(document: EndpointDocument, context: vscode.CustomDocumentBackupContext, _cancellation: vscode.CancellationToken): Promise<vscode.CustomDocumentBackup> {
    try {
      const response = await this.client.get(buildEndpointUrl(document.path));
      const payload = JSON.stringify(response.data ?? {}, null, 2);
      await vscode.workspace.fs.writeFile(context.destination, new TextEncoder().encode(payload));
    } catch {
      await vscode.workspace.fs.writeFile(context.destination, new Uint8Array());
    }

    return {
      id: context.destination.toString(),
      delete: async () => {
        try {
          await vscode.workspace.fs.delete(context.destination);
        } catch {
          // ignore
        }
      },
    };
  }

  private async getHtmlForWebview(webview: vscode.Webview, document: EndpointDocument): Promise<string> {
    const scriptUri = webview.asWebviewUri(vscode.Uri.joinPath(this.extensionUri, 'webview', 'endpointEditor.bundle.js'));
    const styleUri = webview.asWebviewUri(vscode.Uri.joinPath(this.extensionUri, 'webview', 'endpointEditor.css'));
    const workerBase = webview.asWebviewUri(vscode.Uri.joinPath(this.extensionUri, 'webview', 'monaco'));
    const nonce = getNonce();

    const csp = [
      "default-src 'none'",
      `img-src ${webview.cspSource} data: blob:`,
      `style-src ${webview.cspSource} 'unsafe-inline'`,
      `font-src ${webview.cspSource}`,
      `script-src 'nonce-${nonce}' ${webview.cspSource}`,
      `connect-src ${webview.cspSource} http: https:`,
      `worker-src blob: ${webview.cspSource}`,
    ].join('; ');

    return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta http-equiv="Content-Security-Policy" content="${csp}">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <link href="${styleUri}" rel="stylesheet" />
  <title>Endpoint ${document.path}</title>
</head>
<body>
  <div id="root" data-worker-base="${workerBase}"></div>
  <script nonce="${nonce}" src="${scriptUri}"></script>
</body>
</html>`;
  }

  private async handleLoadRequest(panel: vscode.WebviewPanel, document: EndpointDocument, requestId?: string, force = false) {
    try {
      console.log(`[Extension] Loading data for endpoint: ${document.path}`);
      const [config, template, cache, cacheTemplate] = await Promise.all([
        this.client.get(buildEndpointUrl(document.path)).then((res) => res.data),
        this.client.get(buildEndpointUrl(document.path, 'template')).then((res) => res.data?.template ?? res.data ?? ''),
        this.client.get(buildEndpointUrl(document.path, 'cache')).then((res) => res.data ?? {}),
        this.client.get(buildEndpointUrl(document.path, 'cache/template')).then((res) => res.data?.template ?? res.data ?? '').catch(() => ''),
      ]);

      const normalizedConfig = normalizeEndpointConfig(config);
      const normalizedCache = normalizeCache(cache);

      const payload = {
        config: normalizedConfig,
        rawConfig: config,
        template,
        cache: normalizedCache,
        cacheTemplate,
        path: document.path,
        slug: document.slug,
        version: 1,
        force,
      };

      console.log(`[Extension] Sending load-result to webview:`, payload);
      panel.webview.postMessage({
        type: 'load-result',
        requestId,
        payload,
      });
    } catch (error) {
      console.error(`[Extension] Error loading endpoint data:`, error);
      panel.webview.postMessage({
        type: 'error',
        requestId,
        payload: serializeError(error),
      });
    }
  }

  private async handleSchemaRequest(panel: vscode.WebviewPanel, requestId?: string) {
    try {
      const response = await this.client.get('/api/v1/_config/schema?format=completion');
      panel.webview.postMessage({
        type: 'schema-load',
        requestId,
        payload: response.data,
      });
    } catch (error) {
      panel.webview.postMessage({
        type: 'error',
        requestId,
        payload: serializeError(error),
      });
    }
  }

  private async handleSaveTemplate(panel: vscode.WebviewPanel, document: EndpointDocument, payload: { template: string }, requestId?: string) {
    try {
      await this.client.put(buildEndpointUrl(document.path, 'template'), { template: payload.template });
      panel.webview.postMessage({ type: 'save-template-success', requestId });
    } catch (error) {
      panel.webview.postMessage({ type: 'error', requestId, payload: serializeError(error) });
    }
  }

  private async handleExpandTemplate(panel: vscode.WebviewPanel, document: EndpointDocument, payload: { parameters: unknown; includeVariables?: boolean }, requestId?: string) {
    try {
      const url = buildEndpointUrl(document.path, 'template/expand') + (payload.includeVariables ? '?include_variables=true' : '');
      const response = await this.client.post(url, {
        parameters: payload.parameters,
      });
      panel.webview.postMessage({ type: 'expand-template-success', requestId, payload: response.data });
    } catch (error) {
      panel.webview.postMessage({ type: 'error', requestId, payload: serializeError(error) });
    }
  }

  private async handleTestTemplate(panel: vscode.WebviewPanel, document: EndpointDocument, payload: { parameters: unknown }, requestId?: string) {
    try {
      const response = await this.client.post(buildEndpointUrl(document.path, 'template/test'), {
        parameters: payload.parameters,
      });
      panel.webview.postMessage({ type: 'test-template-success', requestId, payload: response.data });
    } catch (error) {
      panel.webview.postMessage({ type: 'error', requestId, payload: serializeError(error) });
    }
  }

  private async handleSaveCache(panel: vscode.WebviewPanel, document: EndpointDocument, payload: { cache: any; cacheTemplate?: string }, requestId?: string) {
    try {
      if (payload.cacheTemplate !== undefined) {
        await this.client.put(buildEndpointUrl(document.path, 'cache/template'), { template: payload.cacheTemplate });
      }
      await this.client.put(buildEndpointUrl(document.path, 'cache'), payload.cache);
      panel.webview.postMessage({ type: 'save-cache-success', requestId });
    } catch (error) {
      panel.webview.postMessage({ type: 'error', requestId, payload: serializeError(error) });
    }
  }

  private async handleSaveRequest(panel: vscode.WebviewPanel, document: EndpointDocument, payload: { config: any }, requestId?: string) {
    try {
      const body = payload.config ?? {};
      body.urlPath = document.path;

      const response = await this.client.get(buildEndpointUrl(document.path));
      const existing = normalizeEndpointConfig(response.data);
      const merged = { ...existing, ...body };

      await this.client.put(buildEndpointUrl(document.path), merged);
      panel.webview.postMessage({ type: 'save-request-success', requestId });
    } catch (error) {
      panel.webview.postMessage({ type: 'error', requestId, payload: serializeError(error) });
    }
  }

  private async handleValidateTemplate(panel: vscode.WebviewPanel, document: EndpointDocument, payload: { parameters: unknown }, requestId?: string) {
    try {
      const url = buildEndpointUrl(document.path, 'template/expand') + '?validate_only=true';
      const response = await this.client.post(url, {
        parameters: payload.parameters,
      });
      panel.webview.postMessage({ type: 'validate-template-success', requestId, payload: response.data });
    } catch (error) {
      panel.webview.postMessage({ type: 'error', requestId, payload: serializeError(error) });
    }
  }
}

function normalizeEndpointConfig(config: any) {
  if (!config || typeof config !== 'object') {
    return {};
  }
  const normalized = { ...config };
  if ('url-path' in normalized && !('urlPath' in normalized)) {
    normalized.urlPath = normalized['url-path'];
  }
  if ('template-source' in normalized && !('templateSource' in normalized)) {
    normalized.templateSource = normalized['template-source'];
  }
  if ('rate-limit' in normalized && !('rateLimit' in normalized)) {
    normalized.rateLimit = normalized['rate-limit'];
  }
  if ('cache' in normalized && normalized.cache && typeof normalized.cache === 'object') {
    normalized.cache = normalizeCache(normalized.cache);
  }
  return normalized;
}

function normalizeCache(cache: any) {
  if (!cache || typeof cache !== 'object') {
    return {};
  }
  const normalized = { ...cache };
  if ('template-file' in normalized && !('templateFile' in normalized)) {
    normalized.templateFile = normalized['template-file'];
  }
  if ('refresh-time' in normalized && !('refreshTime' in normalized)) {
    normalized.refreshTime = normalized['refresh-time'];
  }
  return normalized;
}

function serializeError(error: unknown) {
  if (error instanceof Error) {
    return { message: error.message, stack: error.stack };
  }
  if (typeof error === 'object' && error) {
    return { message: JSON.stringify(error) };
  }
  return { message: String(error) };
}

function getNonce() {
  let text = '';
  const possible = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
  for (let i = 0; i < 32; i++) {
    text += possible.charAt(Math.floor(Math.random() * possible.length));
  }
  return text;
}
