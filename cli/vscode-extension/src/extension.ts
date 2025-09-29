import * as vscode from 'vscode';
import { createApiClient, buildEndpointUrl, pathToSlug, slugToPath } from '@flapi/shared';
import type { ApiClientConfig } from '@flapi/shared';
import { FlapiExplorerProvider, type FlapiNode } from './explorer/FlapiExplorerProvider';
import { registerContentProviders, getSlugFromNode } from './providers/contentProviders';
import { VirtualDocumentManager } from './providers/virtualDocuments';
import { EndpointWorkspace } from './workspace/EndpointWorkspace';
import { registerEndpointCommands } from './commands/endpointCommands';
import { LanguageSupport } from './language/languageSupport';
import { VariablesProvider } from './views/VariablesProvider';

let explorerProvider: FlapiExplorerProvider | undefined;

// Tracks which endpoint a given opened document belongs to
type DocContext = { slug: string; path: string; kind: 'template' | 'cache' };
const docContextByUri = new Map<string, DocContext>();
function setDocContext(uri: vscode.Uri, ctx: DocContext) {
  docContextByUri.set(uri.toString(), ctx);
}
function getDocContext(uri: vscode.Uri): DocContext | undefined {
  return docContextByUri.get(uri.toString());
}
function clearDocContext(uri: vscode.Uri) {
  docContextByUri.delete(uri.toString());
}

function createClient(): ReturnType<typeof createApiClient> {
  const config = vscode.workspace.getConfiguration('flapi');
  const serverUrl = config.get<string>('serverUrl', 'http://localhost:8080');

  return createApiClient({
    baseUrl: serverUrl,
    timeout: config.get<number>('timeout', 30),
    retries: config.get<number>('retries', 3),
    verifyTls: !config.get<boolean>('insecure', false),
  } as ApiClientConfig);
}

function refreshExplorer(context: vscode.ExtensionContext, client: ReturnType<typeof createClient>) {
  explorerProvider = new FlapiExplorerProvider(client);
  context.subscriptions.push(
    vscode.window.createTreeView('flapiExplorer', {
      treeDataProvider: explorerProvider,
      showCollapseAll: true,
    })
  );
}

export function activate(context: vscode.ExtensionContext) {
  const client = createClient();
  refreshExplorer(context, client);

  // Initialize virtual document manager and workspace
  const virtualDocuments = new VirtualDocumentManager(client);
  virtualDocuments.register(context);
  
  const endpointWorkspace = new EndpointWorkspace(client, virtualDocuments);
  context.subscriptions.push(endpointWorkspace);

  // Initialize language support
  const languageSupport = new LanguageSupport(client);
  languageSupport.register(context);
  context.subscriptions.push(languageSupport);

  // Variables view (toggle-able)
  const variablesProvider = new VariablesProvider();
  const variablesView = vscode.window.createTreeView('flapiVariables', {
    treeDataProvider: variablesProvider,
    showCollapseAll: true,
  });
  context.subscriptions.push(variablesView);

  // Register endpoint commands
  registerEndpointCommands(context, endpointWorkspace);

  context.subscriptions.push(
    vscode.commands.registerCommand('flapi.explorer.refresh', () => explorerProvider?.refresh()),
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (event.affectsConfiguration('flapi')) {
        refreshExplorer(context, createClient());
      }
    }),
    registerContentProviders(context, client),
    vscode.commands.registerCommand('flapi.openProject', () => {
      vscode.commands.executeCommand('workbench.action.openSettings', 'flapi');
    }),
    // Tree view endpoint command - context aware
    vscode.commands.registerCommand('flapi.openEndpointFromTree', async (node: FlapiNode) => {
      try {
        const slug = getSlugFromNode(node);
        if (!slug || typeof slug !== 'string') {
          vscode.window.showErrorMessage(`Invalid slug: ${JSON.stringify(slug)} (type: ${typeof slug})`);
          return;
        }
        const endpointPath = slugToPath(slug);

        // Route based on node kind
        switch (node.kind) {
          case 'endpoint:template':
          case 'endpoint':
            await vscode.commands.executeCommand('flapi.openTemplate', node);
            return;
          case 'endpoint:cache':
            await vscode.commands.executeCommand('flapi.openCache', node);
            return;
          case 'endpoint:details':
            await vscode.commands.executeCommand('flapi.openEndpointDetails', node);
            return;
          default:
            // Fallback: open endpoint (template)
            await vscode.commands.executeCommand('flapi.openTemplate', node);
            return;
        }
      } catch (error) {
        console.error('Error in openEndpointFromTree:', error);
        vscode.window.showErrorMessage(`Failed to open endpoint: ${error}`);
      }
    }),
    vscode.commands.registerCommand('flapi.openTemplate', async (node: FlapiNode) => {
      const slug = getSlugFromNode(node);
      if (!slug || typeof slug !== 'string') {
        vscode.window.showErrorMessage(`Invalid slug for template: ${slug}`);
        return;
      }
      try {
        const endpointPath = slugToPath(slug);
        // Fetch endpoint to resolve templateSource
        const cfg = await client.get(buildEndpointUrl(endpointPath));
        const templateSource = (cfg.data?.templateSource as string | undefined) ?? '';
        if (templateSource) {
          const fsPath = resolveFsPath(templateSource);
          const doc = await vscode.workspace.openTextDocument(fsPath);
          const editor = await vscode.window.showTextDocument(doc, { preview: false });
          setDocContext(editor.document.uri, { slug, path: endpointPath, kind: 'template' });
          return;
        }
        // Fallback to virtual provider
        const uri = vscode.Uri.parse(`flapi://endpoint/${slug}/template.sql`);
        const doc = await vscode.workspace.openTextDocument(uri);
        const editor = await vscode.window.showTextDocument(doc, { preview: false });
        setDocContext(editor.document.uri, { slug, path: endpointPath, kind: 'template' });
      } catch (error) {
        vscode.window.showErrorMessage(`Failed to open template: ${String(error)}`);
      }
    }),
    vscode.commands.registerCommand('flapi.openCache', async (node: FlapiNode) => {
      const slug = getSlugFromNode(node);
      if (!slug || typeof slug !== 'string') {
        vscode.window.showErrorMessage(`Invalid slug for cache: ${slug}`);
        return;
      }
      try {
        const endpointPath = slugToPath(slug);
        const cfg = await client.get(buildEndpointUrl(endpointPath));
        const cacheSource = (cfg.data?.cache?.templateSource as string | undefined) ?? '';
        if (cacheSource) {
          const fsPath = resolveFsPath(cacheSource);
          const doc = await vscode.workspace.openTextDocument(fsPath);
          const editor = await vscode.window.showTextDocument(doc, { preview: false });
          setDocContext(editor.document.uri, { slug, path: endpointPath, kind: 'cache' });
          return;
        }
        // Fallback to virtual provider
        const uri = vscode.Uri.parse(`flapi://endpoint/${slug}/cache.json`);
        const doc = await vscode.workspace.openTextDocument(uri);
        const editor = await vscode.window.showTextDocument(doc, { preview: false });
        setDocContext(editor.document.uri, { slug, path: endpointPath, kind: 'cache' });
      } catch (error) {
        vscode.window.showErrorMessage(`Failed to open cache: ${String(error)}`);
      }
    }),

    // Details view: open JSON preview of endpoint config
    vscode.commands.registerCommand('flapi.openEndpointDetails', async (node: FlapiNode) => {
      const slug = getSlugFromNode(node);
      if (!slug || typeof slug !== 'string') {
        vscode.window.showErrorMessage(`Invalid slug for details: ${slug}`);
        return;
      }
      try {
        const endpointPath = slugToPath(slug);
        const cfg = await client.get(buildEndpointUrl(endpointPath));
        const doc = await vscode.workspace.openTextDocument({
          content: JSON.stringify(cfg.data ?? {}, null, 2),
          language: 'json',
        });
        await vscode.window.showTextDocument(doc, { preview: false });
      } catch (error) {
        vscode.window.showErrorMessage(`Failed to open endpoint details: ${String(error)}`);
      }
    })
    ,
    // Quick-open parameters seeded from template comments/sidecar
    vscode.commands.registerCommand('flapi.openParameters', async (node: FlapiNode) => {
      const slug = getSlugFromNode(node);
      if (!slug || typeof slug !== 'string') return;
      const endpointPath = slugToPath(slug);
      await endpointWorkspace.openParameters(endpointPath);
    })
  );

  // Editor title actions for SQL template files
  context.subscriptions.push(
    vscode.commands.registerCommand('flapi.template.expand', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;
      const ctx = getDocContext(editor.document.uri);
      if (!ctx || ctx.kind !== 'template') {
        void vscode.window.showWarningMessage('No template context bound to this editor. Open via flapi explorer.');
        return;
      }
      await endpointWorkspace.expandTemplate(ctx.path);
      // Open/refresh results doc beside
      const resUri = vscode.Uri.parse(`flapi://endpoint/${ctx.slug}/results.sql`);
      const resDoc = await vscode.workspace.openTextDocument(resUri);
      await vscode.window.showTextDocument(resDoc, { preview: false, viewColumn: vscode.ViewColumn.Beside });
    }),
    vscode.commands.registerCommand('flapi.template.test', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;
      const ctx = getDocContext(editor.document.uri);
      if (!ctx || ctx.kind !== 'template') {
        void vscode.window.showWarningMessage('No template context bound to this editor. Open via flapi explorer.');
        return;
      }
      await endpointWorkspace.testTemplate(ctx.path);
      const resUri = vscode.Uri.parse(`flapi://endpoint/${ctx.slug}/results.sql`);
      const resDoc = await vscode.workspace.openTextDocument(resUri);
      await vscode.window.showTextDocument(resDoc, { preview: false, viewColumn: vscode.ViewColumn.Beside });
    })
  );

  // Clear context when documents close
  context.subscriptions.push(
    vscode.workspace.onDidCloseTextDocument((doc) => clearDocContext(doc.uri)),
    vscode.workspace.onDidSaveTextDocument(async (doc) => {
      const ctx = getDocContext(doc.uri);
      if (!ctx) return;
      try {
        if (ctx.kind === 'template') {
          await endpointWorkspace.saveTemplate(ctx.path, doc.getText());
        } else if (ctx.kind === 'cache') {
          await endpointWorkspace.saveCacheTemplate(ctx.path, doc.getText());
        }
      } catch {
        // surfaced inside save helpers
      }
    }),
    vscode.window.onDidChangeActiveTextEditor(async (ed) => {
      if (!ed) return;
      const ctx = getDocContext(ed.document.uri);
      if (!ctx) return;
      // Refresh variable list for current endpoint
      try {
        const paramsProvider = virtualDocuments.getParametersProvider();
        const slug = ctx.slug;
        const path = ctx.path;
        const params = paramsProvider.getParameters(slug) ?? {};
        const url = buildEndpointUrl(path, 'template/expand') + '?include_variables=true&validate_only=true';
        const resp = await client.post(url, { parameters: params });
        variablesProvider.setVariables(slug, resp.data?.variables);
      } catch {
        // ignore
      }
    })
  );

  // Toggle variables view
  context.subscriptions.push(
    vscode.commands.registerCommand('flapi.variables.toggle', async () => {
      await vscode.commands.executeCommand('workbench.view.extension.flapi');
    }),
    vscode.commands.registerCommand('flapi.variables.insert', async (fullName: string) => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;
      const snippet = new vscode.SnippetString(`{{${fullName}}}`);
      await editor.insertSnippet(snippet, editor.selection.active);
    }),
    vscode.commands.registerCommand('flapi.cli.copyCommand', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;
      const ctx = getDocContext(editor.document.uri);
      if (!ctx) {
        void vscode.window.showWarningMessage('No flapi context for this editor.');
        return;
      }
      const action = await vscode.window.showQuickPick([
        { label: 'Expand Template', value: 'expand' },
        { label: 'Test Template', value: 'test' }
      ], { placeHolder: 'Choose flapii action' });
      if (!action) return;

      const cmd = `flapii templates ${action.value} --endpoint ${ctx.path} --params @${ctx.slug}.json`;
      await vscode.env.clipboard.writeText(cmd);
      void vscode.window.showInformationMessage('Copied flapii command to clipboard');
    })
  );
}

export function deactivate() {
  explorerProvider = undefined;
}

function resolveFsPath(sourcePath: string): vscode.Uri {
  if (sourcePath.startsWith('/') || /^[a-zA-Z]:\\/.test(sourcePath)) {
    return vscode.Uri.file(sourcePath);
  }
  const root = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? process.cwd();
  return vscode.Uri.file(require('path').join(root, sourcePath));
}
