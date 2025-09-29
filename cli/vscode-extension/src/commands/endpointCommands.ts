import * as vscode from 'vscode';
import { EndpointWorkspace } from '../workspace/EndpointWorkspace';
import { pathToSlug } from '@flapi/shared';

/**
 * Registers all endpoint-related commands
 */
export function registerEndpointCommands(
  context: vscode.ExtensionContext,
  workspace: EndpointWorkspace
): void {

  // Command: Open endpoint in multi-document view
  const openEndpointCommand = vscode.commands.registerCommand(
    'flapi.openEndpoint',
    async (arg?: string | any) => {
      let path: string | undefined;
      
      // Allow being called with a string path or a tree node
      if (typeof arg === 'string') {
        path = arg;
      } else if (arg && typeof arg === 'object') {
        try {
          const { slugToPath } = await import('@flapi/shared');
          const slug = (arg?.extra?.slug as string | undefined) ?? (arg?.id as string | undefined);
          if (slug && typeof slug === 'string') {
            path = slugToPath(slug);
          }
        } catch {
          // ignore
        }
      }

      if (!path) {
        // Show quick pick if no path resolved
        path = await showEndpointQuickPick();
        if (!path) return;
      }
      
      await workspace.openEndpoint(path);
    }
  );

  // Command: Expand template with current parameters
  const expandTemplateCommand = vscode.commands.registerCommand(
    'flapi.expandTemplate',
    async (path?: string) => {
      path = path || await getActiveEndpointPath();
      if (!path) {
        vscode.window.showWarningMessage('No endpoint selected. Please open an endpoint first.');
        return;
      }
      
      await workspace.expandTemplate(path);
    }
  );

  // Command: Test template
  const testTemplateCommand = vscode.commands.registerCommand(
    'flapi.testTemplate',
    async (path?: string) => {
      path = path || await getActiveEndpointPath();
      if (!path) {
        vscode.window.showWarningMessage('No endpoint selected. Please open an endpoint first.');
        return;
      }
      
      await workspace.testTemplate(path);
    }
  );

  // Command: Validate template
  const validateTemplateCommand = vscode.commands.registerCommand(
    'flapi.validateTemplate',
    async (path?: string) => {
      path = path || await getActiveEndpointPath();
      if (!path) return;
      
      await workspace.validateTemplate(path);
    }
  );

  // Command: Save endpoint configuration
  const saveEndpointCommand = vscode.commands.registerCommand(
    'flapi.saveEndpoint',
    async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.uri.scheme !== 'flapi') {
        vscode.window.showWarningMessage('Please open a flapi endpoint document first.');
        return;
      }

      const path = await getEndpointPathFromUri(editor.document.uri);
      if (!path) return;

      try {
        const content = editor.document.getText();
        const config = JSON.parse(content);
        await workspace.saveEndpointConfig(path, config);
      } catch (error) {
        vscode.window.showErrorMessage(`Invalid JSON: ${error}`);
      }
    }
  );

  // Command: Save template
  const saveTemplateCommand = vscode.commands.registerCommand(
    'flapi.saveTemplate',
    async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.uri.scheme !== 'flapi' || !editor.document.uri.path.endsWith('template.sql')) {
        vscode.window.showWarningMessage('Please open a flapi template document first.');
        return;
      }

      const path = await getEndpointPathFromUri(editor.document.uri);
      if (!path) return;

      const template = editor.document.getText();
      await workspace.saveTemplate(path, template);
    }
  );

  // Command: Save cache configuration
  const saveCacheCommand = vscode.commands.registerCommand(
    'flapi.saveCacheConfig',
    async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.uri.scheme !== 'flapi' || !editor.document.uri.path.endsWith('cache.json')) {
        vscode.window.showWarningMessage('Please open a flapi cache configuration document first.');
        return;
      }

      const path = await getEndpointPathFromUri(editor.document.uri);
      if (!path) return;

      try {
        const content = editor.document.getText();
        const cacheConfig = JSON.parse(content);
        await workspace.saveCacheConfig(path, cacheConfig);
      } catch (error) {
        vscode.window.showErrorMessage(`Invalid JSON: ${error}`);
      }
    }
  );

  // Command: Update parameters
  const updateParametersCommand = vscode.commands.registerCommand(
    'flapi.updateParameters',
    async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.uri.scheme !== 'flapi' || !editor.document.uri.path.endsWith('parameters.json')) {
        vscode.window.showWarningMessage('Please open a flapi parameters document first.');
        return;
      }

      const path = await getEndpointPathFromUri(editor.document.uri);
      if (!path) return;

      try {
        const content = editor.document.getText();
        const parameters = JSON.parse(content);
        workspace.setParameters(path, parameters);
        vscode.window.showInformationMessage('Parameters updated!');
      } catch (error) {
        vscode.window.showErrorMessage(`Invalid JSON: ${error}`);
      }
    }
  );

  // Register all commands
  context.subscriptions.push(
    openEndpointCommand,
    expandTemplateCommand,
    testTemplateCommand,
    validateTemplateCommand,
    saveEndpointCommand,
    saveTemplateCommand,
    saveCacheCommand,
    updateParametersCommand
  );

  // Auto-validate on template changes
  const onDidChangeTextDocument = vscode.workspace.onDidChangeTextDocument(async (event) => {
    if (event.document.uri.scheme === 'flapi' && event.document.uri.path.endsWith('template.sql')) {
      const path = await getEndpointPathFromUri(event.document.uri);
      if (path) {
        // Debounced validation
        setTimeout(() => workspace.validateTemplate(path), 1000);
      }
    }
  });

  context.subscriptions.push(onDidChangeTextDocument);
}

/**
 * Shows a quick pick to select an endpoint
 */
async function showEndpointQuickPick(): Promise<string | undefined> {
  // This would ideally fetch from the API, but for now we'll use a simple input
  const input = await vscode.window.showInputBox({
    prompt: 'Enter endpoint path (e.g., /customers/, /publicis)',
    placeHolder: '/customers/'
  });
  
  return input;
}

/**
 * Gets the endpoint path from the currently active document
 */
async function getActiveEndpointPath(): Promise<string | undefined> {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.uri.scheme !== 'flapi') {
    return undefined;
  }
  
  return getEndpointPathFromUri(editor.document.uri);
}

/**
 * Extracts endpoint path from a flapi:// URI
 */
async function getEndpointPathFromUri(uri: vscode.Uri): Promise<string | undefined> {
  // URI format: flapi://endpoint/{slug}/{component}
  const pathParts = uri.path.split('/');
  if (pathParts.length < 3 || pathParts[1] !== 'endpoint') {
    return undefined;
  }
  
  const slug = pathParts[2];
  // Convert slug back to path (this should use slugToPath from shared lib)
  const { slugToPath } = await import('@flapi/shared');
  return slugToPath(slug);
}
