import * as vscode from 'vscode';

type VariableNamespace = 'params' | 'env' | 'conn' | 'cache';

export interface DiscoveredVariablesPayload {
  params?: Record<string, unknown>;
  env?: Record<string, unknown>;
  conn?: Record<string, unknown>;
  cache?: Record<string, unknown>;
}

export class VariablesProvider implements vscode.TreeDataProvider<vscode.TreeItem> {
  private _onDidChangeTreeData = new vscode.EventEmitter<void>();
  readonly onDidChangeTreeData = this._onDidChangeTreeData.event;

  private variables: Record<VariableNamespace, string[]> = {
    params: [],
    env: [],
    conn: [],
    cache: [],
  };
  private currentSlug: string | undefined;

  constructor() {}

  setVariables(slug: string, payload: DiscoveredVariablesPayload | undefined) {
    this.currentSlug = slug;
    const collect = (obj?: Record<string, unknown>): string[] =>
      obj ? Object.keys(obj).sort() : [];

    this.variables = {
      params: collect(payload?.params),
      env: collect(payload?.env),
      conn: collect(payload?.conn),
      cache: collect(payload?.cache),
    };
    this._onDidChangeTreeData.fire();
  }

  clear() {
    this.currentSlug = undefined;
    this.variables = { params: [], env: [], conn: [], cache: [] };
    this._onDidChangeTreeData.fire();
  }

  getTreeItem(element: vscode.TreeItem): vscode.TreeItem {
    return element;
  }

  async getChildren(element?: vscode.TreeItem): Promise<vscode.TreeItem[]> {
    if (!element) {
      const roots: Array<{ ns: VariableNamespace; label: string }> = [
        { ns: 'params', label: 'params' },
        { ns: 'conn', label: 'conn' },
        { ns: 'env', label: 'env' },
        { ns: 'cache', label: 'cache' },
      ];
      return roots.map((r) => {
        const item = new vscode.TreeItem(r.label, vscode.TreeItemCollapsibleState.Collapsed);
        item.contextValue = `flapi:variables:${r.ns}`;
        return item;
      });
    }

    // Children of a namespace node: variables
    const ns = element.label?.toString() as VariableNamespace;
    const vars = this.variables[ns] ?? [];
    return vars.map((name) => {
      const full = `${ns}.${name}`;
      const child = new vscode.TreeItem(full, vscode.TreeItemCollapsibleState.None);
      child.contextValue = 'flapi:variables:item';
      child.command = {
        command: 'flapi.variables.insert',
        title: 'Insert variable',
        arguments: [full],
      };
      return child;
    });
  }

  dispose() {
    this._onDidChangeTreeData.dispose();
  }
}


