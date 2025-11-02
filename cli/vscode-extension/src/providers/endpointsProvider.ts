import * as vscode from 'vscode';
import type { AxiosInstance } from 'axios';
import type { EndpointConfig } from '@flapi/shared';

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
        (endpoint) => {
          const operation = this.getOperationType(endpoint);
          const label = `${endpoint.urlPath} ${operation.icon}`;
          return new EndpointItem(
            label,
            endpoint.method,
            vscode.TreeItemCollapsibleState.Collapsed,
            'endpoint',
            operation.type,
          );
        },
      );
    }

    const endpoint = this.endpoints.find((e) => e.urlPath === element.resourcePath);
    if (!endpoint) {
      return [];
    }

    const operation = this.getOperationType(endpoint);
    const details: EndpointItem[] = [
      new EndpointItem('Method', endpoint.method, vscode.TreeItemCollapsibleState.None, 'detail'),
      new EndpointItem('Operation', operation.type, vscode.TreeItemCollapsibleState.None, 'detail'),
      new EndpointItem('Template', endpoint.templateSource, vscode.TreeItemCollapsibleState.None, 'detail'),
      new EndpointItem(
        'Connections',
        endpoint.connection?.join(', ') ?? 'None',
        vscode.TreeItemCollapsibleState.None,
        'detail',
      ),
    ];

    // Add operation configuration details if present
    if (endpoint.operation) {
      const op = endpoint.operation as any;
      const opDetails: string[] = [];
      if (op.validate_before_write === true) opDetails.push('validate-before-write');
      if (op.returns_data === true) opDetails.push('returns-data');
      if (op.transaction === true) opDetails.push('transaction');
      if (opDetails.length > 0) {
        details.splice(2, 0, new EndpointItem('Operation Config', opDetails.join(', '), vscode.TreeItemCollapsibleState.None, 'detail'));
      }
    }

    return details;
  }

  private getOperationType(endpoint: EndpointConfig): { type: string; icon: string } {
    const method = (endpoint.method || 'GET').toUpperCase();
    let opType = 'Read';
    let icon = '👁️';
    
    if (endpoint.operation && typeof endpoint.operation === 'object') {
      const op = endpoint.operation as any;
      if (op.type === 'write') {
        opType = 'Write';
        icon = '✏️';
      }
    } else if (['POST', 'PUT', 'PATCH', 'DELETE'].includes(method)) {
      opType = 'Write';
      icon = '✏️';
    }
    
    return { type: opType, icon };
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
    public readonly operationType?: string,
  ) {
    super(label, collapsibleState);
    if (contextValue === 'endpoint') {
      this.description = resourcePath;
    } else {
      this.description = resourcePath;
    }
    this.tooltip = operationType ? `${label} (${operationType})` : label;
  }
}
