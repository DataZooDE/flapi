import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { createApiClient, buildEndpointUrl, pathToSlug, slugToPath } from '@flapi/shared';
import type { ApiClientConfig } from '@flapi/shared';
import { FlapiExplorerProvider, type FlapiNode } from './explorer/FlapiExplorerProvider';
import { registerContentProviders, getSlugFromNode } from './providers/contentProviders';
import { VirtualDocumentManager } from './providers/virtualDocuments';
import { EndpointWorkspace } from './workspace/EndpointWorkspace';
import { registerEndpointCommands } from './commands/endpointCommands';
import { LanguageSupport } from './language/languageSupport';
import { SchemaProvider, type SchemaNode } from './providers/SchemaProvider';
import { YamlValidator } from './validation/yamlValidator';
import { readTransportSettings } from './services/transportSettings';
import { FlapiApiClient } from '@flapi/shared';
import { TokenStorageService } from './services/tokenStorage';
import { resolveInside, templatesRoot } from './workspace/safePath';
import { createEndpointOnServer, listConnectionNames } from './services/endpointCreator';
import { describeValidation, validateTemplateText } from './validation/templateValidator';
import { endpointPathFromYamlText } from './validation/endpointPath';
import { ParameterStorageService } from './services/parameterStorage';
import { TestStateService } from './services/testStateService';
import { EndpointTestService } from './services/endpointTestService';
import { EndpointTesterPanel } from './webview/endpointTesterPanel';
import { SqlTemplateTesterPanel } from './webview/sqlTemplateTesterPanel';
import { EndpointTestCodeLensProvider } from './codelens/endpointTestProvider';
import { SqlTemplateTestCodeLensProvider } from './codelens/sqlTemplateTestProvider';
import { IncludeNavigationProvider, navigateToInclude } from './codelens/includeNavigationProvider';

let explorerProvider: FlapiExplorerProvider | undefined;
let explorerView: vscode.TreeView<any> | undefined;
let schemaProvider: SchemaProvider | undefined;
let schemaView: vscode.TreeView<any> | undefined;
let tokenStorage: TokenStorageService;
let configApiClient: FlapiApiClient;
let virtualDocuments: VirtualDocumentManager | undefined;
let endpointWorkspace: EndpointWorkspace | undefined;
let languageSupport: LanguageSupport | undefined;
let endpointTestCodeLensProvider: EndpointTestCodeLensProvider | undefined;
let sqlTemplateTestCodeLensProvider: SqlTemplateTestCodeLensProvider | undefined;
let testStateService: TestStateService;

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

function createClient(token?: string): ReturnType<typeof createApiClient> {
  const settings = readTransportSettings();

  return createApiClient({
    baseUrl: settings.serverUrl,
    timeout: settings.timeout,
    retries: settings.retries,
    verifyTls: settings.verifyTls,
    authToken: token,
  } as ApiClientConfig);
}

/** Options for FlapiApiClient built from the same settings as createClient. */
function configClientOptions(token?: string) {
  const settings = readTransportSettings();
  return {
    baseUrl: settings.serverUrl,
    timeout: settings.timeout,
    retries: settings.retries,
    verifyTls: settings.verifyTls,
    authToken: token,
  };
}

function initializeExplorer(context: vscode.ExtensionContext, token?: string) {
  const client = createClient(token);
  explorerProvider = new FlapiExplorerProvider(client);
  explorerView = vscode.window.createTreeView('flapiExplorer', {
    treeDataProvider: explorerProvider,
    showCollapseAll: true,
  });
  context.subscriptions.push(explorerView);
}

function updateExplorerWithToken(token?: string) {
  console.log('[Flapi] updateExplorerWithToken called, token', token ? 'set' : 'not set');
  const client = createClient(token);
  if (explorerProvider) {
    console.log('[Flapi] Calling explorerProvider.updateClient');
    explorerProvider.updateClient(client);
  } else {
    console.log('[Flapi] WARNING: explorerProvider is undefined!');
  }
}

function updateSchemaWithToken(token?: string) {
  console.log('[Flapi] updateSchemaWithToken called, token', token ? 'set' : 'not set');
  const client = createClient(token);
  if (schemaProvider) {
    console.log('[Flapi] Calling schemaProvider.updateClient');
    schemaProvider.updateClient(client);
  } else {
    console.log('[Flapi] WARNING: schemaProvider is undefined!');
  }
}

function updateAllClientsWithToken(token?: string) {
  console.log('[Flapi] updateAllClientsWithToken called');
  const client = createClient(token);
  
  // Update all services that use the client
  if (virtualDocuments) {
    console.log('[Flapi] Updating virtualDocuments client');
    virtualDocuments.updateClient(client);
  }
  
  if (endpointWorkspace) {
    console.log('[Flapi] Updating endpointWorkspace client');
    endpointWorkspace.updateClient(client);
  }
  
  if (languageSupport) {
    console.log('[Flapi] Updating languageSupport client');
    languageSupport.updateClient(client);
  }
  
  if (endpointTestCodeLensProvider) {
    console.log('[Flapi] Updating endpointTestCodeLensProvider client');
    endpointTestCodeLensProvider.updateClient(client);
  }
  
  if (sqlTemplateTestCodeLensProvider) {
    console.log('[Flapi] Updating sqlTemplateTestCodeLensProvider client');
    sqlTemplateTestCodeLensProvider.updateClient(client);
  }
  
  // Update configApiClient (FlapiApiClient)
  console.log('[Flapi] Updating configApiClient');
  configApiClient.reconfigure(configClientOptions(token));
}

export async function activate(context: vscode.ExtensionContext) {
  // Initialize token storage first
  tokenStorage = new TokenStorageService(context);
  const token = await tokenStorage.getToken();
  
  // Create clients with token
  const client = createClient(token);
  configApiClient = new FlapiApiClient(configClientOptions(token));
  
  // Initialize explorer with token
  initializeExplorer(context, token);

  // Initialize schema provider
  schemaProvider = new SchemaProvider(client);
  schemaView = vscode.window.createTreeView('flapiSchema', {
    treeDataProvider: schemaProvider,
    showCollapseAll: true,
  });
  context.subscriptions.push(schemaView);

  // Initialize virtual document manager and workspace
  virtualDocuments = new VirtualDocumentManager(client);
  virtualDocuments.register(context);
  
  endpointWorkspace = new EndpointWorkspace(client, virtualDocuments);
  context.subscriptions.push(endpointWorkspace);

  // Initialize language support
  languageSupport = new LanguageSupport(client);
  languageSupport.register(context);
  context.subscriptions.push(languageSupport);
  
  // Initialize YAML validator
  const outputChannel = vscode.window.createOutputChannel('Flapi');
  const yamlValidator = new YamlValidator(configApiClient, outputChannel);
  context.subscriptions.push(yamlValidator, outputChannel);

  // Initialize endpoint testing services
  const parameterStorage = new ParameterStorageService(context);
  testStateService = new TestStateService(context);
  const testService = new EndpointTestService(outputChannel);

  // Register CodeLens provider for endpoint testing
  endpointTestCodeLensProvider = new EndpointTestCodeLensProvider(client);
  context.subscriptions.push(
    vscode.languages.registerCodeLensProvider(
      { language: 'yaml', scheme: 'file' },
      endpointTestCodeLensProvider
    )
  );

  // Register CodeLens provider for SQL template testing
  sqlTemplateTestCodeLensProvider = new SqlTemplateTestCodeLensProvider(client);
  context.subscriptions.push(
    vscode.languages.registerCodeLensProvider(
      { language: 'sql', scheme: 'file' },
      sqlTemplateTestCodeLensProvider
    )
  );

  // Register CodeLens provider for include navigation
  const includeNavigationProvider = new IncludeNavigationProvider();
  context.subscriptions.push(
    vscode.languages.registerCodeLensProvider(
      { language: 'yaml', scheme: 'file' },
      includeNavigationProvider
    )
  );

  // Register endpoint commands
  registerEndpointCommands(context, endpointWorkspace);

  context.subscriptions.push(
    vscode.commands.registerCommand('flapi.explorer.refresh', () => explorerProvider?.refresh()),
    vscode.commands.registerCommand('flapi.schema.refresh', () => schemaProvider?.refresh()),
    
    // Config service token command
    vscode.commands.registerCommand('flapi.setConfigToken', async () => {
      const token = await vscode.window.showInputBox({
        prompt: 'Enter config service token',
        password: true,
        placeHolder: 'Token from server startup',
        ignoreFocusOut: true,
      });
      
      if (token) {
        await tokenStorage.setToken(token);
        
        // Update all clients with new token
        updateExplorerWithToken(token);
        updateSchemaWithToken(token);
        updateAllClientsWithToken(token);
        
        vscode.window.showInformationMessage('Config service token saved. Refreshing...');
      }
    }),
    
    vscode.commands.registerCommand('flapi.clearConfigToken', async () => {
      await tokenStorage.clearToken();
      
      // Update all clients without token
      updateExplorerWithToken(undefined);
      updateSchemaWithToken(undefined);
      updateAllClientsWithToken(undefined);
      
      vscode.window.showInformationMessage('Config service token cleared');
    }),
    
    // Validation commands
    vscode.commands.registerCommand('flapi.validateYaml', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        vscode.window.showWarningMessage('No editor active');
        return;
      }
      
      if (editor.document.languageId !== 'yaml') {
        vscode.window.showWarningMessage('Current file is not a YAML file');
        return;
      }
      
      outputChannel.show(true);
      outputChannel.appendLine('='.repeat(60));
      outputChannel.appendLine(`Validating ${editor.document.fileName}...`);
      
      const isValid = await yamlValidator.validateYamlFile(editor.document);
      if (isValid) {
        vscode.window.showInformationMessage(`✓ ${editor.document.fileName} is valid`);
      }
    }),
    
    vscode.commands.registerCommand('flapi.reloadEndpoint', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        vscode.window.showWarningMessage('No editor active');
        return;
      }
      
      if (editor.document.languageId !== 'yaml') {
        vscode.window.showWarningMessage('Current file is not a YAML file');
        return;
      }
      
      outputChannel.show(true);
      await yamlValidator.reloadEndpointConfig(editor.document);
    }),

    // SQL template validation and reload commands
    vscode.commands.registerCommand('flapi.validateSqlTemplate', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        vscode.window.showWarningMessage('No editor active');
        return;
      }
      
      if (editor.document.languageId !== 'sql') {
        vscode.window.showWarningMessage('Current file is not a SQL file');
        return;
      }
      
      try {
        // Find associated YAML config
        const sqlPath = editor.document.uri.fsPath;
        const sqlDir = path.dirname(sqlPath);
        const sqlBasename = path.basename(sqlPath, '.sql');
        
        // Try to find YAML file with same basename
        const yamlPath = path.join(sqlDir, `${sqlBasename}.yaml`);
        const ymlPath = path.join(sqlDir, `${sqlBasename}.yml`);
        
        let yamlUri: vscode.Uri | null = null;
        if (fs.existsSync(yamlPath)) {
          yamlUri = vscode.Uri.file(yamlPath);
        } else if (fs.existsSync(ymlPath)) {
          yamlUri = vscode.Uri.file(ymlPath);
        }
        
        if (!yamlUri) {
          vscode.window.showWarningMessage('Could not find associated YAML configuration file');
          return;
        }
        
        outputChannel.show(true);
        outputChannel.appendLine('='.repeat(60));
        outputChannel.appendLine(`Validating SQL template: ${sqlPath}...`);
        outputChannel.appendLine(`Using config: ${yamlUri.fsPath}`);
        
        // Validate the editor's own (possibly unsaved) text on the server, against the
        // endpoint this file belongs to. The YAML is validated too, but it is not what
        // "SQL template is valid" is about.
        const yamlDoc = await vscode.workspace.openTextDocument(yamlUri);
        const endpointPath = endpointPathFromYamlText(yamlDoc.getText());
        if (!endpointPath) {
          vscode.window.showWarningMessage('Could not tell which endpoint this SQL file belongs to (no url-path in its YAML)');
          return;
        }
        const token = await tokenStorage.getToken();
        const result = await validateTemplateText(createClient(token), endpointPath, editor.document.getText());
        result.errors.forEach((e) => outputChannel.appendLine(`  - ERROR (${e.type}): ${e.message}`));
        result.warnings.forEach((w) => outputChannel.appendLine(`  - WARNING (${w.type}): ${w.message}`));
        const shown = describeValidation(result);
        if (shown.ok) {
          vscode.window.showInformationMessage(shown.message);
        } else {
          vscode.window.showErrorMessage(shown.message);
        }
      } catch (error: any) {
        vscode.window.showErrorMessage(`Validation failed: ${error.message}`);
      }
    }),
    
    vscode.commands.registerCommand('flapi.reloadSqlTemplate', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        vscode.window.showWarningMessage('No editor active');
        return;
      }
      
      if (editor.document.languageId !== 'sql') {
        vscode.window.showWarningMessage('Current file is not a SQL file');
        return;
      }
      
      try {
        // Find associated YAML config
        const sqlPath = editor.document.uri.fsPath;
        const sqlDir = path.dirname(sqlPath);
        const sqlBasename = path.basename(sqlPath, '.sql');
        
        // Try to find YAML file with same basename
        const yamlPath = path.join(sqlDir, `${sqlBasename}.yaml`);
        const ymlPath = path.join(sqlDir, `${sqlBasename}.yml`);
        
        let yamlUri: vscode.Uri | null = null;
        if (fs.existsSync(yamlPath)) {
          yamlUri = vscode.Uri.file(yamlPath);
        } else if (fs.existsSync(ymlPath)) {
          yamlUri = vscode.Uri.file(ymlPath);
        }
        
        if (!yamlUri) {
          vscode.window.showWarningMessage('Could not find associated YAML configuration file');
          return;
        }
        
        outputChannel.show(true);
        outputChannel.appendLine('='.repeat(60));
        outputChannel.appendLine(`Reloading SQL template: ${sqlPath}...`);
        outputChannel.appendLine(`Using config: ${yamlUri.fsPath}`);
        
        const yamlDoc = await vscode.workspace.openTextDocument(yamlUri);
        // Say so only if the server actually reloaded it.
        if (await yamlValidator.reloadEndpointConfig(yamlDoc)) {
          vscode.window.showInformationMessage('✓ SQL template reloaded');
        }
      } catch (error: any) {
        vscode.window.showErrorMessage(`Reload failed: ${error.message}`);
      }
    }),

    // Include navigation command
    vscode.commands.registerCommand('flapi.navigateToInclude', async (sourceUri: vscode.Uri, includeInfo: { file: string; section: string | null }) => {
      await navigateToInclude(sourceUri, includeInfo);
    }),

    // Endpoint testing command
    vscode.commands.registerCommand('flapi.testEndpoint', async (documentUri?: vscode.Uri) => {
      try {
        // Get the document - either from the URI or active editor
        let document: vscode.TextDocument;
        
        if (documentUri) {
          document = await vscode.workspace.openTextDocument(documentUri);
        } else {
          const editor = vscode.window.activeTextEditor;
          if (!editor) {
            vscode.window.showWarningMessage('No editor active');
            return;
          }
          document = editor.document;
        }
        
        // Ensure it's a YAML file
        if (document.languageId !== 'yaml') {
          vscode.window.showWarningMessage('Current file is not a YAML file');
          return;
        }
        
        // Check if the code lens provider is initialized
        if (!endpointTestCodeLensProvider) {
          vscode.window.showErrorMessage('Endpoint test provider not initialized');
          return;
        }
        
        // Parse the YAML to get endpoint config
        const endpointConfig = await endpointTestCodeLensProvider.getEndpointConfig(document);
        
        // Check if this is a REST endpoint
        const urlPath = endpointConfig['url-path'] || endpointConfig.urlPath;
        if (!urlPath) {
          vscode.window.showWarningMessage('This YAML file does not define a REST endpoint (no url-path)');
          return;
        }
        
        // Get base URL from configuration
        const config = vscode.workspace.getConfiguration('flapi');
        const baseUrl = config.get<string>('serverUrl', 'http://localhost:8080');
        
        // Create or show the endpoint tester panel
        await EndpointTesterPanel.createOrShow(
          context.extensionUri,
          parameterStorage,
          testService,
          endpointConfig,
          baseUrl
        );
      } catch (error: any) {
        vscode.window.showErrorMessage(`Failed to open endpoint tester: ${error.message}`);
      }
    }),

    // SQL template testing command
    vscode.commands.registerCommand('flapi.testSqlTemplate', async (sqlUri?: vscode.Uri, yamlConfigPath?: string) => {
      try {
        // Get the SQL file - either from the URI or active editor
        let document: vscode.TextDocument;
        
        if (sqlUri) {
          document = await vscode.workspace.openTextDocument(sqlUri);
        } else {
          const editor = vscode.window.activeTextEditor;
          if (!editor) {
            vscode.window.showWarningMessage('No editor active');
            return;
          }
          document = editor.document;
        }
        
        // Ensure it's a SQL file
        if (document.languageId !== 'sql') {
          vscode.window.showWarningMessage('Current file is not a SQL file');
          return;
        }

        // If no YAML config path provided, try to find it
        if (!yamlConfigPath && sqlTemplateTestCodeLensProvider) {
          // The CodeLens provider should have already found it, but we can look again
          vscode.window.showWarningMessage('This SQL file does not have an associated YAML config');
          return;
        }
        
        // Get authenticated client
        const config = vscode.workspace.getConfiguration('flapi');
        const token = await tokenStorage.getToken();
        const authClient = createClient(token);
        
        // Create or show the SQL template tester panel
        await SqlTemplateTesterPanel.createOrShow(
          context.extensionUri,
          testStateService,
          authClient,
          document.uri,
          yamlConfigPath!
        );
      } catch (error: any) {
        vscode.window.showErrorMessage(`Failed to open SQL template tester: ${error.message}`);
      }
    }),
    
    // Filesystem-based commands
    vscode.commands.registerCommand('flapi.openProjectConfig', async () => {
      const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
      if (!workspaceRoot) {
        void vscode.window.showErrorMessage('No workspace folder open');
        return;
      }
      const configPath = vscode.Uri.file(require('path').join(workspaceRoot, 'flapi.yaml'));
      try {
        const doc = await vscode.workspace.openTextDocument(configPath);
        await vscode.window.showTextDocument(doc);
      } catch (error) {
        void vscode.window.showErrorMessage(`Failed to open flapi.yaml: ${error}`);
      }
    }),
    
    vscode.commands.registerCommand('flapi.openYamlFile', async (node: FlapiNode) => {
      const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
      if (!workspaceRoot) return;
      
      const relativePath = node.extra?.path as string;
      if (!relativePath) return;
      
      const fullPath = vscode.Uri.file(resolveInside(templatesRoot(workspaceRoot), relativePath));
      try {
        const doc = await vscode.workspace.openTextDocument(fullPath);
        await vscode.window.showTextDocument(doc);
        
        // IMPORTANT: Do NOT set docContext for YAML files!
        // docContext should only be used for virtual SQL template documents,
        // not for actual filesystem YAML endpoint configuration files.
        // YAML files are saved directly to disk and then trigger a reload via yamlValidator.
      } catch (error) {
        void vscode.window.showErrorMessage(`Failed to open YAML file: ${error}`);
      }
    }),
    
    vscode.commands.registerCommand('flapi.openSqlFile', async (node: FlapiNode) => {
      const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
      if (!workspaceRoot) return;
      
      const relativePath = node.extra?.path as string;
      if (!relativePath) return;
      
      const fullPath = vscode.Uri.file(resolveInside(templatesRoot(workspaceRoot), relativePath));
      try {
        const doc = await vscode.workspace.openTextDocument(fullPath);
        await vscode.window.showTextDocument(doc);
      } catch (error) {
        void vscode.window.showErrorMessage(`Failed to open SQL file: ${error}`);
      }
    }),
    
    vscode.commands.registerCommand('flapi.deleteFile', async (node: FlapiNode) => {
      const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
      if (!workspaceRoot) return;
      
      const relativePath = node.extra?.path as string;
      if (!relativePath) return;
      
      const fileName = node.label;
      const confirm = await vscode.window.showWarningMessage(
        `Are you sure you want to delete "${fileName}"?`,
        { modal: true },
        'Delete'
      );
      
      if (confirm !== 'Delete') return;
      
      const fullPath = vscode.Uri.file(resolveInside(templatesRoot(workspaceRoot), relativePath));
      try {
        await vscode.workspace.fs.delete(fullPath);
        void vscode.window.showInformationMessage(`Deleted ${fileName}`);
        explorerProvider?.refresh();
      } catch (error) {
        void vscode.window.showErrorMessage(`Failed to delete file: ${error}`);
      }
    }),
    
    vscode.commands.registerCommand('flapi.renameFile', async (node: FlapiNode) => {
      const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
      if (!workspaceRoot) return;
      
      const relativePath = node.extra?.path as string;
      if (!relativePath) return;
      
      const oldName = node.label;
      const newName = await vscode.window.showInputBox({
        prompt: 'Enter new file name',
        value: oldName,
        validateInput: (value) => {
          if (!value || value.trim().length === 0) {
            return 'File name cannot be empty';
          }
          if (value.includes('/') || value.includes('\\')) {
            return 'File name cannot contain path separators';
          }
          return null;
        }
      });
      
      if (!newName || newName === oldName) return;
      
      const path = require('path');
      const oldFullPath = vscode.Uri.file(resolveInside(templatesRoot(workspaceRoot), relativePath));
      const dirPath = path.dirname(relativePath);
      const newRelativePath = path.join(dirPath, newName);
      const newFullPath = vscode.Uri.file(resolveInside(templatesRoot(workspaceRoot), newRelativePath));
      
      try {
        await vscode.workspace.fs.rename(oldFullPath, newFullPath);
        void vscode.window.showInformationMessage(`Renamed to ${newName}`);
        explorerProvider?.refresh();
      } catch (error) {
        void vscode.window.showErrorMessage(`Failed to rename file: ${error}`);
      }
    }),
    
    vscode.commands.registerCommand('flapi.newEndpoint', async () => {
      // The endpoint is created ON THE SERVER through the ConfigService; nothing is
      // written to the local workspace (the server's templates directory is not
      // necessarily this workspace's).
      const token = await tokenStorage.getToken();
      const client = createClient(token);

      let connections: string[];
      try {
        connections = await listConnectionNames(client);
      } catch (error) {
        void vscode.window.showErrorMessage(`Could not read connections from the flAPI server: ${error}`);
        return;
      }
      if (connections.length === 0) {
        void vscode.window.showErrorMessage('The flAPI server has no connections configured');
        return;
      }

      // Ask for endpoint name
      const endpointName = await vscode.window.showInputBox({
        prompt: 'Enter endpoint name (e.g., "my-endpoint")',
        validateInput: (value) => {
          if (!value || value.trim().length === 0) {
            return 'Endpoint name cannot be empty';
          }
          if (!/^[a-z0-9-_]+$/.test(value)) {
            return 'Endpoint name can only contain lowercase letters, numbers, hyphens, and underscores';
          }
          return null;
        }
      });
      
      if (!endpointName) return;
      
      // Ask for URL path
      const urlPath = await vscode.window.showInputBox({
        prompt: 'Enter URL path (e.g., "/api/my-endpoint")',
        value: `/${endpointName}`,
        validateInput: (value) => {
          if (!value || value.trim().length === 0) {
            return 'URL path cannot be empty';
          }
          if (!value.startsWith('/')) {
            return 'URL path must start with /';
          }
          return null;
        }
      });
      
      if (!urlPath) return;
      
      const connection = connections.length === 1
        ? connections[0]
        : await vscode.window.showQuickPick(connections, { placeHolder: 'Connection for the new endpoint' });
      if (!connection) return;

      try {
        const created = await createEndpointOnServer(client, {
          name: endpointName,
          urlPath,
          connection,
          sql: `SELECT 'Hello from ${endpointName}' AS message;\n`,
        });
        void vscode.window.showInformationMessage(`Created endpoint ${created.urlPath} on the server`);
        explorerProvider?.refresh();
      } catch (error) {
        void vscode.window.showErrorMessage(`Failed to create endpoint: ${error instanceof Error ? error.message : error}`);
      }
    }),
    vscode.commands.registerCommand('flapi.schema.copyTableName', async (node: SchemaNode) => {
      if (node.kind === 'table') {
        const tableName = node.label;
        await vscode.env.clipboard.writeText(tableName);
        void vscode.window.showInformationMessage(`Copied table name: ${tableName}`);
      }
    }),
    vscode.commands.registerCommand('flapi.schema.copyColumnName', async (node: SchemaNode) => {
      if (node.kind === 'column') {
        const columnName = node.label;
        await vscode.env.clipboard.writeText(columnName);
        void vscode.window.showInformationMessage(`Copied column name: ${columnName}`);
      }
    }),
    vscode.workspace.onDidChangeConfiguration(async (event) => {
      if (event.affectsConfiguration('flapi')) {
        const currentToken = await tokenStorage.getToken();
        // Server URL / timeout / retries / insecure apply to every client, not
        // only the two tree views.
        updateExplorerWithToken(currentToken);
        updateSchemaWithToken(currentToken);
        updateAllClientsWithToken(currentToken);
      }
    }),
    registerContentProviders(context, client),
    vscode.commands.registerCommand('flapi.openProject', () => {
      vscode.commands.executeCommand('workbench.action.openSettings', 'flapi');
    }),
    // Legacy tree view endpoint command - kept for backward compat with old providers
    vscode.commands.registerCommand('flapi.openEndpointFromTree', async (node: FlapiNode) => {
      // For new filesystem-based nodes, delegate to appropriate command
      if (node.kind === 'file:yaml:endpoint' || node.kind === 'file:yaml:mcp-tool' || 
          node.kind === 'file:yaml:mcp-resource' || node.kind === 'file:yaml:mcp-prompt' ||
          node.kind === 'file:yaml:shared') {
        await vscode.commands.executeCommand('flapi.openYamlFile', node);
        return;
      }
      if (node.kind === 'file:sql:template' || node.kind === 'file:sql:cache') {
        await vscode.commands.executeCommand('flapi.openSqlFile', node);
        return;
      }
      
      // Fallback: try old logic (may not work with new tree structure)
      void vscode.window.showWarningMessage('Node type not supported in filesystem-based explorer');
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
      if (!slug || typeof slug !== 'string' || !endpointWorkspace) return;
      const endpointPath = slugToPath(slug);
      await endpointWorkspace.openParameters(endpointPath);
    })
  );

  // Editor title actions for SQL template files
  context.subscriptions.push(
    vscode.commands.registerCommand('flapi.template.expand', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || !endpointWorkspace) return;
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
      if (!editor || !endpointWorkspace) return;
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

  // YAML validation on save (intercepts before save)
  context.subscriptions.push(
    vscode.workspace.onWillSaveTextDocument(async (event) => {
      const document = event.document;
      
      // Only validate YAML files
      if (document.languageId !== 'yaml') {
        return;
      }

      // Check if it's a Flapi endpoint YAML file
      const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
      if (!workspaceFolder) {
        return;
      }

      // Validate before save
      event.waitUntil(
        (async () => {
          const isValid = await yamlValidator.validateYamlFile(document);
          if (!isValid) {
            // Validation failed - cancel the save
            throw new Error('Validation failed');
          }
        })()
      );
    })
  );

  // Clear context when documents close, and reload config after YAML saves
  context.subscriptions.push(
    vscode.workspace.onDidCloseTextDocument((doc) => {
      clearDocContext(doc.uri);
      yamlValidator.clearDiagnostics(doc);
    }),
    vscode.workspace.onDidSaveTextDocument(async (doc) => {
      // Handle SQL template saves
      const ctx = getDocContext(doc.uri);
      if (ctx && endpointWorkspace) {
        try {
          if (ctx.kind === 'template') {
            await endpointWorkspace.saveTemplate(ctx.path, doc.getText());
          } else if (ctx.kind === 'cache') {
            await endpointWorkspace.saveCacheTemplate(ctx.path, doc.getText());
          }
        } catch {
          // surfaced inside save helpers
        }
        return;
      }

      // Handle YAML endpoint config saves - trigger reload
      if (doc.languageId === 'yaml') {
        await yamlValidator.reloadEndpointConfig(doc);
      }
    })
  );

  context.subscriptions.push(
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
  schemaProvider = undefined;
}

function resolveFsPath(sourcePath: string): vscode.Uri {
  if (sourcePath.startsWith('/') || /^[a-zA-Z]:\\/.test(sourcePath)) {
    return vscode.Uri.file(sourcePath);
  }
  const root = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? process.cwd();
  return vscode.Uri.file(require('path').join(root, sourcePath));
}
