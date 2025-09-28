import * as vscode from 'vscode';
import type { AxiosInstance } from 'axios';
import type { EndpointConfig } from '../../shared/src/lib/types';

export class EndpointsProvider implements vscode.TreeDataProvider<EndpointItem> {
  private _onDidChangeTreeData = new vscode.EventEmitter<EndpointItem | void>();
  readonly onDidChangeTreeData = this._onDidChangeTreeData.event;

  private endpoints: EndpointConfig[] = [];

  constructor(private readonly client: AxiosInstance) {
    this.refresh();
  }

  refresh() {
    this.loadEndpoints();
    this._onDidChangeTreeData.fire();
  }

  getTreeItem(element: EndpointItem): vscode.TreeItem {
    return element;
  }

  async getChildren(element?: EndpointItem): Promise<EndpointItem[]> {
    if (!element) {
      return this.endpoints.map(
        (endpoint) =>
          new EndpointItem(
            endpoint.urlPath,
            endpoint.method,
            vscode.TreeItemCollapsibleState.Collapsed,
            'endpoint',
          ),
      );
    }

    const endpoint = this.endpoints.find((e) => e.urlPath === element.resourcePath);
    if (!endpoint) {
      return [];
    }

    return [
      new EndpointItem('Method', endpoint.method, vscode.TreeItemCollapsibleState.None, 'detail'),
      new EndpointItem('Template', endpoint.templateSource, vscode.TreeItemCollapsibleState.None, 'detail'),
      new EndpointItem(
        'Connections',
        endpoint.connection?.join(', ') ?? 'None',
        vscode.TreeItemCollapsibleState.None,
        'detail',
      ),
    ];
  }

  private async loadEndpoints() {
    try {
      const response = await this.client.get('/api/v1/_config/endpoints');
      const data = response.data as Record<string, EndpointConfig>;
      this.endpoints = Object.values(data);
    } catch (error) {
      vscode.window.showErrorMessage(`Failed to load endpoints: ${error}`);
      this.endpoints = [];
    }
  }
}

class EndpointItem extends vscode.TreeItem {
  constructor(
    public readonly label: string,
    public readonly resourcePath: string,
    public readonly collapsibleState: vscode.TreeItemCollapsibleState,
    public readonly contextValue: string,
  ) {
    super(label, collapsibleState);
    this.description = resourcePath;
  }
}
