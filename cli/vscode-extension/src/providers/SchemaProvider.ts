import * as vscode from 'vscode';
import type { AxiosInstance } from 'axios';

export type SchemaNode = SchemaNodeMetadata;

interface SchemaNodeMetadata {
  kind: 'catalog' | 'table' | 'column';
  id: string;
  label: string;
  description?: string;
  parentId?: string;
  extra?: Record<string, unknown>;
}

export class SchemaProvider implements vscode.TreeDataProvider<SchemaNodeMetadata> {
  private readonly _onDidChangeTreeData = new vscode.EventEmitter<SchemaNodeMetadata | undefined>();
  readonly onDidChangeTreeData = this._onDidChangeTreeData.event;
  private client: AxiosInstance;

  constructor(client: AxiosInstance) {
    this.client = client;
  }

  updateClient(client: AxiosInstance): void {
    console.log('[Flapi] SchemaProvider.updateClient called, has headers:', !!client.defaults.headers);
    this.client = client;
    console.log('[Flapi] SchemaProvider calling refresh()');
    this.refresh(); // Refresh with new client
  }

  refresh(node?: SchemaNodeMetadata): void {
    this._onDidChangeTreeData.fire(node);
  }

  getTreeItem(element: SchemaNodeMetadata): vscode.TreeItem {
    switch (element.kind) {
      case 'catalog':
        const isEmptyCatalog = element.extra?.isEmpty === true;
        return {
          label: element.label,
          description: element.description,
          collapsibleState: isEmptyCatalog 
            ? vscode.TreeItemCollapsibleState.None 
            : vscode.TreeItemCollapsibleState.Collapsed,
          contextValue: isEmptyCatalog ? 'flapi:schema-catalog-empty' : 'flapi:schema-catalog',
          iconPath: new vscode.ThemeIcon('database'),
        };
      
      case 'table':
        const isEmptyTable = element.extra?.isEmpty === true;
        const isView = element.extra?.isView === true;
        return {
          label: element.label,
          description: element.description,
          collapsibleState: isEmptyTable 
            ? vscode.TreeItemCollapsibleState.None 
            : vscode.TreeItemCollapsibleState.Collapsed,
          contextValue: isEmptyTable ? 'flapi:schema-table-empty' : 'flapi:schema-table',
          iconPath: new vscode.ThemeIcon(isView ? 'symbol-interface' : 'table'),
        };
      
      case 'column':
        const isEmptyColumn = element.extra?.isEmpty === true;
        return {
          label: element.label,
          description: element.description,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: isEmptyColumn ? 'flapi:schema-column-empty' : 'flapi:schema-column',
          iconPath: new vscode.ThemeIcon('symbol-field'),
        };
      
      default:
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
        };
    }
  }

  async getChildren(element?: SchemaNodeMetadata): Promise<SchemaNodeMetadata[]> {
    if (!element) {
      return this.loadCatalogs();
    }

    switch (element.kind) {
      case 'catalog':
        return this.loadTables(element.id);
      
      case 'table':
        return this.loadColumns(element.parentId ?? '', element.id);
      
      default:
        return [];
    }
  }

  private async loadCatalogs(): Promise<SchemaNodeMetadata[]> {
    try {
      const response = await this.client.get('/api/v1/_config/schema');
      const data = response.data;
      
      const catalogs = Object.keys(data || {});
      
      if (catalogs.length === 0) {
        return [{
          kind: 'catalog',
          id: 'no-catalogs',
          label: 'No database catalogs available',
          extra: { isEmpty: true },
        }];
      }
      
      return catalogs.map((catalog) => ({
        kind: 'catalog',
        id: catalog,
        label: catalog,
        description: `${Object.keys(data[catalog]?.tables || {}).length} tables`,
        extra: { catalog, connection: data[catalog] },
      }));
    } catch (error: any) {
      // Handle specific error cases
      if (error?.response?.status === 404) {
        // Config service is not enabled - show once only (already shown by explorer)
        return [];
      } else if (error?.response?.status === 401) {
        // Config service requires token - show once only (already shown by explorer)
        return [];
      } else {
        // Other errors
        void vscode.window.showErrorMessage(`Failed to load schema: ${error?.message || String(error)}`);
        return [];
      }
    }
  }

  private async loadTables(catalogId: string): Promise<SchemaNodeMetadata[]> {
    try {
      const response = await this.client.get('/api/v1/_config/schema');
      const data = response.data;
      const catalog = data?.[catalogId];
      
      if (!catalog?.tables) {
        return [{
          kind: 'table',
          id: 'no-tables',
          parentId: catalogId,
          label: 'No tables available',
          extra: { isEmpty: true },
        }];
      }
      
      const tables = Object.entries(catalog.tables);
      return tables.map(([tableName, tableInfo]: [string, any]) => ({
        kind: 'table',
        id: tableName,
        parentId: catalogId,
        label: tableName,
        description: tableInfo.is_view ? 'VIEW' : 'TABLE',
        extra: { 
          table: tableInfo, 
          tableName, 
          catalogId,
          isView: tableInfo.is_view === true,
        },
      }));
    } catch (error) {
      void vscode.window.showErrorMessage(`Failed to load tables for ${catalogId}: ${String(error)}`);
      return [];
    }
  }

  private async loadColumns(catalogId: string, tableId: string): Promise<SchemaNodeMetadata[]> {
    try {
      const response = await this.client.get('/api/v1/_config/schema');
      const data = response.data;
      const catalog = data?.[catalogId];
      const table = catalog?.tables?.[tableId];
      
      if (!table?.columns) {
        return [{
          kind: 'column',
          id: 'no-columns',
          parentId: tableId,
          label: 'No columns available',
          extra: { isEmpty: true },
        }];
      }
      
      const columns = Object.entries(table.columns);
      return columns.map(([columnName, columnInfo]: [string, any]) => ({
        kind: 'column',
        id: `${catalogId}:${tableId}:${columnName}`,
        parentId: tableId,
        label: columnName,
        description: `${columnInfo.type || 'unknown'}${columnInfo.nullable ? ' (nullable)' : ' (not null)'}`,
        extra: { 
          column: columnInfo, 
          columnName, 
          tableId, 
          catalogId,
        },
      }));
    } catch (error) {
      void vscode.window.showErrorMessage(`Failed to load columns for ${tableId}: ${String(error)}`);
      return [];
    }
  }
}

