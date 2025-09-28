import * as vscode from 'vscode';
import { createApiClient } from '@flapi/shared';
import type { ApiClientConfig } from '@flapi/shared';
import { EndpointsProvider } from './providers/endpointsProvider';

let endpointsProvider: EndpointsProvider | undefined;

export function activate(context: vscode.ExtensionContext) {
  const config = vscode.workspace.getConfiguration('flapi');
  const serverUrl = config.get<string>('serverUrl', 'http://localhost:8080');

  const apiClient = createApiClient({
    baseUrl: serverUrl,
    timeout: 30,
    retries: 3,
    verifyTls: true,
  } as ApiClientConfig);

  endpointsProvider = new EndpointsProvider(apiClient);

  vscode.window.createTreeView('flapiEndpoints', {
    treeDataProvider: endpointsProvider,
    showCollapseAll: true,
  });

  context.subscriptions.push(
    vscode.commands.registerCommand('flapi.refreshEndpoints', () => {
      endpointsProvider?.refresh();
    }),
  );
}

export function deactivate() {
  endpointsProvider = undefined;
}
