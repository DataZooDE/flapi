import * as vscode from 'vscode';
import { createApiClient } from '@flapi/shared';
import { buildEndpointUrl, slugToPath } from '@flapi/shared';

/**
 * Base class for virtual document providers that handle flapi:// URIs
 */
abstract class BaseVirtualDocumentProvider implements vscode.TextDocumentContentProvider {
  protected _onDidChange = new vscode.EventEmitter<vscode.Uri>();
  readonly onDidChange = this._onDidChange.event;

  constructor(protected client: ReturnType<typeof createApiClient>) {}

  abstract provideTextDocumentContent(uri: vscode.Uri): Promise<string>;

  protected parseUri(uri: vscode.Uri): { slug: string; path: string } {
    // URI format uses authority for the resource kind:
    //   flapi://endpoint/{slug}/{component}
    // So uri.authority === 'endpoint' and uri.path === '/{slug}/{component}'
    const normalized = uri.path.replace(/^\/+/, ''); // remove all leading slashes
    const parts = normalized.split('/'); // [slug, component]
    const slug = parts[0] ?? '';
    const path = slugToPath(slug);
    return { slug, path };
  }

  public refresh(uri: vscode.Uri) {
    this._onDidChange.fire(uri);
  }
}

/**
 * Provides endpoint configuration as JSON
 */
export class EndpointConfigProvider extends BaseVirtualDocumentProvider {
  async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
    try {
      const { path } = this.parseUri(uri);
      const url = buildEndpointUrl(path, '');
      const response = await this.client.get(url);
      return JSON.stringify(response.data, null, 2);
    } catch (error) {
      return JSON.stringify({ error: `Failed to load endpoint config: ${error}` }, null, 2);
    }
  }
}

/**
 * Provides SQL template content
 */
export class TemplateProvider extends BaseVirtualDocumentProvider {
  async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
    try {
      const { path } = this.parseUri(uri);
      const url = buildEndpointUrl(path, 'template');
      const response = await this.client.get(url);
      return response.data.template || '-- No template content';
    } catch (error) {
      return `-- Failed to load template: ${error}`;
    }
  }
}

/**
 * Provides cache configuration as JSON
 */
export class CacheConfigProvider extends BaseVirtualDocumentProvider {
  async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
    try {
      const { path } = this.parseUri(uri);
      const url = buildEndpointUrl(path, 'cache');
      const response = await this.client.get(url);
      return JSON.stringify(response.data, null, 2);
    } catch (error) {
      return JSON.stringify({ error: `Failed to load cache config: ${error}` }, null, 2);
    }
  }
}

/**
 * Provides request parameters as JSON (editable)
 */
export class ParametersProvider extends BaseVirtualDocumentProvider {
  private parametersCache = new Map<string, any>();

  async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
    const { slug } = this.parseUri(uri);
    
    // Return cached parameters or default empty object
    const params = this.parametersCache.get(slug) || {};
    return JSON.stringify(params, null, 2);
  }

  public setParameters(slug: string, parameters: any) {
    this.parametersCache.set(slug, parameters);
    const uri = vscode.Uri.parse(`flapi://endpoint/${slug}/parameters.json`);
    this.refresh(uri);
  }

  public getParameters(slug: string): any {
    return this.parametersCache.get(slug) || {};
  }
}

/**
 * Provides expanded SQL results (read-only)
 */
export class ResultsProvider extends BaseVirtualDocumentProvider {
  private resultsCache = new Map<string, any>();

  async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
    const { slug } = this.parseUri(uri);
    const results = this.resultsCache.get(slug);
    
    if (!results) {
      return '-- No results yet. Use "Flapi: Expand Template" to see expanded SQL and results.';
    }

    let content = '';
    
    if (results.expandedSql) {
      content += '-- Expanded SQL:\n';
      content += results.expandedSql + '\n\n';
    }

    if (results.data) {
      content += '-- Query Results:\n';
      content += '/*\n';
      content += JSON.stringify(results.data, null, 2);
      content += '\n*/';
    }

    if (results.error) {
      content += '-- Error:\n';
      content += `/* ${results.error} */`;
    }

    return content || '-- No results available';
  }

  public setResults(slug: string, results: any) {
    this.resultsCache.set(slug, results);
    const uri = vscode.Uri.parse(`flapi://endpoint/${slug}/results.sql`);
    this.refresh(uri);
  }
}

/**
 * Manager class to coordinate all virtual document providers
 */
export class VirtualDocumentManager {
  private endpointConfigProvider: EndpointConfigProvider;
  private templateProvider: TemplateProvider;
  private cacheConfigProvider: CacheConfigProvider;
  private parametersProvider: ParametersProvider;
  private resultsProvider: ResultsProvider;

  constructor(client: ReturnType<typeof createApiClient>) {
    this.endpointConfigProvider = new EndpointConfigProvider(client);
    this.templateProvider = new TemplateProvider(client);
    this.cacheConfigProvider = new CacheConfigProvider(client);
    this.parametersProvider = new ParametersProvider(client);
    this.resultsProvider = new ResultsProvider(client);
  }

  public register(context: vscode.ExtensionContext) {
    // Register a single provider that handles all flapi:// URIs
    const provider = {
      provideTextDocumentContent: (uri: vscode.Uri) => {
        // Expect authority 'endpoint'
        if (uri.authority !== 'endpoint') {
          return Promise.resolve('// Invalid flapi URI (authority)');
        }

        const normalized = uri.path.replace(/^\/+/, ''); // '{slug}/{component}'
        const parts = normalized.split('/');
        if (parts.length < 2) {
          return Promise.resolve('// Invalid flapi URI (path)');
        }

        const component = parts[1];
        switch (component) {
          case 'config.json':
            return this.endpointConfigProvider.provideTextDocumentContent(uri);
          case 'template.sql':
            return this.templateProvider.provideTextDocumentContent(uri);
          case 'cache.json':
            return this.cacheConfigProvider.provideTextDocumentContent(uri);
          case 'parameters.json':
            return this.parametersProvider.provideTextDocumentContent(uri);
          case 'results.sql':
            return this.resultsProvider.provideTextDocumentContent(uri);
          default:
            return Promise.resolve('// Unknown document type');
        }
      },

      onDidChange: this.endpointConfigProvider.onDidChange
    };

    context.subscriptions.push(
      vscode.workspace.registerTextDocumentContentProvider('flapi', provider)
    );
  }

  public getParametersProvider(): ParametersProvider {
    return this.parametersProvider;
  }

  public getResultsProvider(): ResultsProvider {
    return this.resultsProvider;
  }

  public updateClient(client: ReturnType<typeof createApiClient>): void {
    // Update all providers with new client
    this.endpointConfigProvider = new EndpointConfigProvider(client);
    this.templateProvider = new TemplateProvider(client);
    this.cacheConfigProvider = new CacheConfigProvider(client);
    this.parametersProvider = new ParametersProvider(client);
    this.resultsProvider = new ResultsProvider(client);
  }

  public refreshAll(slug: string) {
    const uris = [
      vscode.Uri.parse(`flapi://endpoint/${slug}/config.json`),
      vscode.Uri.parse(`flapi://endpoint/${slug}/template.sql`),
      vscode.Uri.parse(`flapi://endpoint/${slug}/cache.json`),
      vscode.Uri.parse(`flapi://endpoint/${slug}/parameters.json`),
      vscode.Uri.parse(`flapi://endpoint/${slug}/results.sql`)
    ];

    uris.forEach(uri => {
      this.endpointConfigProvider.refresh(uri);
      this.templateProvider.refresh(uri);
      this.cacheConfigProvider.refresh(uri);
      this.parametersProvider.refresh(uri);
      this.resultsProvider.refresh(uri);
    });
  }
}
