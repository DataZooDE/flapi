import * as vscode from 'vscode';
import type { AxiosInstance } from 'axios';
import type { EndpointConfig } from '@flapi/shared';
import { pathToSlug } from '@flapi/shared';

export type FlapiNode = FlapiNodeMetadata;

interface FlapiNodeMetadata {
  kind:
    | 'root:project'
    | 'root:endpoints'
    | 'root:schema'
    | 'root:mcp'
    | 'project'
    | 'endpoint'
    | 'endpoint:details'
    | 'endpoint:template'
    | 'endpoint:cache'
    | 'schema:connection'
    | 'schema:table'
    | 'schema:column'
    | 'mcp:group'
    | 'mcp:item';
  id: string;
  label: string;
  description?: string;
  parentId?: string;
  extra?: Record<string, unknown>;
}

export class FlapiExplorerProvider implements vscode.TreeDataProvider<FlapiNodeMetadata> {
  private readonly _onDidChangeTreeData = new vscode.EventEmitter<FlapiNodeMetadata | undefined>();
  readonly onDidChangeTreeData = this._onDidChangeTreeData.event;

  constructor(private readonly client: AxiosInstance) {}

  refresh(node?: FlapiNodeMetadata) {
    this._onDidChangeTreeData.fire(node);
  }

  getTreeItem(element: FlapiNodeMetadata): vscode.TreeItem {
    switch (element.kind) {
      case 'root:project':
      case 'root:endpoints':
      case 'root:schema':
      case 'root:mcp':
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.Collapsed,
          contextValue: element.kind,
        };
      case 'project':
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:project',
          command: {
            command: 'flapi.openProject',
            title: 'Open Project',
            arguments: [element],
          },
        };
      case 'endpoint':
        return {
          label: element.label,
          description: element.description,
          collapsibleState: vscode.TreeItemCollapsibleState.Collapsed,
          contextValue: 'flapi:endpoint',
          command: {
            command: 'flapi.openEndpointFromTree',
            title: 'Open Endpoint',
            arguments: [element],
          },
        };
      case 'endpoint:details':
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:endpoint-details',
          command: {
            command: 'flapi.openEndpointFromTree',
            title: 'Open Endpoint',
            arguments: [element],
          },
        };
      case 'endpoint:template':
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:template',
          command: {
            command: 'flapi.openTemplate',
            title: 'Open Template',
            arguments: [element],
          },
        };
      case 'endpoint:cache':
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:cache',
          command: {
            command: 'flapi.openCache',
            title: 'Open Cache',
            arguments: [element],
          },
        };
      case 'schema:connection':
        const isEmptyCatalog = element.extra?.isEmpty === true;
        return {
          label: element.label,
          description: element.description,
          collapsibleState: isEmptyCatalog ? vscode.TreeItemCollapsibleState.None : vscode.TreeItemCollapsibleState.Collapsed,
          contextValue: isEmptyCatalog ? 'flapi:schema-connection-empty' : 'flapi:schema-connection',
        };
      case 'schema:table':
        const isEmptyTable = element.extra?.isEmpty === true;
        return {
          label: element.label,
          description: element.description,
          collapsibleState: isEmptyTable ? vscode.TreeItemCollapsibleState.None : vscode.TreeItemCollapsibleState.Collapsed,
          contextValue: isEmptyTable ? 'flapi:schema-table-empty' : 'flapi:schema-table',
        };
      case 'schema:column':
        const isEmptyColumn = element.extra?.isEmpty === true;
        return {
          label: element.label,
          description: element.description,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: isEmptyColumn ? 'flapi:schema-column-empty' : 'flapi:schema-column',
        };
      case 'mcp:group':
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.Collapsed,
          contextValue: 'flapi:mcp-group',
        };
      case 'mcp:item':
        const isEmpty = element.extra?.isEmpty === true;
        const hasError = element.extra?.hasError === true;
        return {
          label: element.label,
          description: element.description,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: isEmpty || hasError ? 'flapi:mcp-item-empty' : 'flapi:mcp-item',
          command: isEmpty || hasError ? undefined : {
            command: 'flapi.openMcpItem',
            title: 'Open MCP Item',
            arguments: [element],
          },
        };
      default:
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
        };
    }
  }

  async getChildren(element?: FlapiNodeMetadata): Promise<FlapiNodeMetadata[]> {
    if (!element) {
      return [
        { kind: 'root:project', id: 'project-root', label: 'project' },
        { kind: 'root:endpoints', id: 'endpoints-root', label: 'endpoints' },
        { kind: 'root:schema', id: 'schema-root', label: 'schema' },
        { kind: 'root:mcp', id: 'mcp-root', label: 'mcp' },
      ];
    }

    switch (element.kind) {
      case 'root:project':
        return [
          {
            kind: 'project',
            id: 'project-config',
            label: 'Project Configuration',
          },
        ];

      case 'root:endpoints':
        return this.loadEndpoints();

      case 'endpoint':
        return this.loadEndpointChildren(element);

      case 'root:schema':
        return this.loadSchemaConnections();

      case 'schema:connection':
        return this.loadSchemaTables(element.id);

      case 'schema:table':
        return this.loadSchemaColumns(element.parentId ?? '', element.id);

      case 'root:mcp':
        return this.loadMcpGroups();

      case 'mcp:group':
        return this.loadMcpItems(element.id);

      default:
        return [];
    }
  }

  private async loadEndpoints(): Promise<FlapiNodeMetadata[]> {
    try {
      const response = await this.client.get<Record<string, EndpointConfig>>('/api/v1/_config/endpoints');
      const endpoints = response.data ?? {};
      return Object.entries(endpoints)
        .filter(([path]) => !!path)
        .map(([path, config]) => ({
          kind: 'endpoint',
          id: path,
          label: path,
          description: (config as EndpointConfig).method ?? 'GET',
          extra: { path, slug: pathToSlug(path), config },
        }));
    } catch (error) {
      void vscode.window.showErrorMessage(`Failed to load endpoints: ${String(error)}`);
      return [];
    }
  }

  private loadEndpointChildren(endpoint: FlapiNodeMetadata): FlapiNodeMetadata[] {
    const path = endpoint.extra?.path as string | undefined ?? endpoint.id;
    return [
      {
        kind: 'endpoint:details',
        id: `${path}:details`,
        parentId: path,
        label: 'Details',
        extra: { path, slug: pathToSlug(path) },
      },
      {
        kind: 'endpoint:template',
        id: `${path}:template`,
        parentId: path,
        label: 'Template',
        extra: { path, slug: pathToSlug(path) },
      },
      {
        kind: 'endpoint:cache',
        id: `${path}:cache`,
        parentId: path,
        label: 'Cache',
        extra: { path, slug: pathToSlug(path) },
      },
    ];
  }

  private async loadSchemaConnections(): Promise<FlapiNodeMetadata[]> {
    try {
      const response = await this.client.get('/api/v1/_config/schema');
      const data = response.data;
      
      // The schema is organized by catalogs (main, analytics, audit, etc.)
      const catalogs = Object.keys(data || {});
      
      if (catalogs.length === 0) {
        return [{
          kind: 'schema:connection',
          id: 'no-catalogs',
          label: 'No database catalogs available',
          extra: { isEmpty: true },
        }];
      }
      
      return catalogs.map((catalog) => ({
        kind: 'schema:connection',
        id: catalog,
        label: catalog,
        description: `${Object.keys(data[catalog]?.tables || {}).length} tables`,
        extra: { catalog, connection: data[catalog] },
      }));
    } catch (error) {
      void vscode.window.showErrorMessage(`Failed to load schema connections: ${String(error)}`);
      return [];
    }
  }

  private async loadSchemaTables(connectionId: string): Promise<FlapiNodeMetadata[]> {
    try {
      const response = await this.client.get('/api/v1/_config/schema');
      const data = response.data;
      const catalog = data?.[connectionId];
      
      if (!catalog?.tables) {
        return [{
          kind: 'schema:table',
          id: 'no-tables',
          parentId: connectionId,
          label: 'No tables available',
          extra: { isEmpty: true },
        }];
      }
      
      const tables = Object.entries(catalog.tables);
      return tables.map(([tableName, tableInfo]: [string, any]) => ({
        kind: 'schema:table',
        id: tableName,
        parentId: connectionId,
        label: tableName,
        description: tableInfo.is_view ? 'VIEW' : 'TABLE',
        extra: { table: tableInfo, tableName, connectionId },
      }));
    } catch (error) {
      void vscode.window.showErrorMessage(`Failed to load tables for ${connectionId}: ${String(error)}`);
      return [];
    }
  }

  private async loadSchemaColumns(connectionId: string, tableId: string): Promise<FlapiNodeMetadata[]> {
    try {
      const response = await this.client.get('/api/v1/_config/schema');
      const data = response.data;
      const catalog = data?.[connectionId];
      const table = catalog?.tables?.[tableId];
      
      if (!table?.columns) {
        return [{
          kind: 'schema:column',
          id: 'no-columns',
          parentId: tableId,
          label: 'No columns available',
          extra: { isEmpty: true },
        }];
      }
      
      const columns = Object.entries(table.columns);
      return columns.map(([columnName, columnInfo]: [string, any]) => ({
        kind: 'schema:column',
        id: `${connectionId}:${tableId}:${columnName}`,
        parentId: tableId,
        label: columnName,
        description: `${columnInfo.type || 'unknown'}${columnInfo.nullable ? ' (nullable)' : ' (not null)'}`,
        extra: { column: columnInfo, columnName, tableId, connectionId },
      }));
    } catch (error) {
      void vscode.window.showErrorMessage(`Failed to load columns for ${tableId}: ${String(error)}`);
      return [];
    }
  }

  private async loadMcpGroups(): Promise<FlapiNodeMetadata[]> {
    return [
      { kind: 'mcp:group', id: 'mcp-tools', label: 'Tools', description: 'MCP tools from YAML files', extra: { subtype: 'tools' } },
      { kind: 'mcp:group', id: 'mcp-resources', label: 'Resources', description: 'MCP resources from YAML files', extra: { subtype: 'resources' } },
      { kind: 'mcp:group', id: 'mcp-prompts', label: 'Prompts', description: 'MCP prompts from YAML files', extra: { subtype: 'prompts' } },
    ];
  }

  private async loadMcpItems(groupId: string): Promise<FlapiNodeMetadata[]> {
    const subtype = groupId.replace('mcp-', '');
    
    // First try to get MCP items from endpoints
    try {
      const response = await this.client.get<Record<string, EndpointConfig>>('/api/v1/_config/endpoints');
      const endpoints = response.data ?? {};
      
      const mcpItems: FlapiNodeMetadata[] = [];
      
      for (const [path, config] of Object.entries(endpoints)) {
        const mcpKey = `mcp-${subtype}`;
        const mcpConfig = (config as any)?.[mcpKey];
        
        if (mcpConfig) {
          if (Array.isArray(mcpConfig)) {
            // Multiple items
            mcpConfig.forEach((item: any, index: number) => {
              mcpItems.push({
                kind: 'mcp:item',
                id: `${subtype}:${path}:${index}`,
                label: item.name ?? item.id ?? `${subtype} from ${path}`,
                description: item.description ?? `From endpoint ${path}`,
                extra: { subtype, item, path },
              });
            });
          } else if (typeof mcpConfig === 'object' && mcpConfig !== null) {
            // Single item
            mcpItems.push({
              kind: 'mcp:item',
              id: `${subtype}:${path}`,
              label: mcpConfig.name ?? mcpConfig.id ?? `${subtype} from ${path}`,
              description: mcpConfig.description ?? `From endpoint ${path}`,
              extra: { subtype, item: mcpConfig, path },
            });
          }
        }
      }
      
      if (mcpItems.length > 0) {
        return mcpItems;
      }
      
      // If no MCP items found in endpoints, show a placeholder
      return [{
        kind: 'mcp:item',
        id: `${subtype}:no-items`,
        label: `No ${subtype} available`,
        description: 'MCP items are defined in separate YAML files but not loaded as endpoints. Check examples/sqls/ for MCP files.',
        extra: { subtype, isEmpty: true },
      }];
      
    } catch (error) {
      // Don't show error message for MCP discovery failures
      console.warn(`Failed to load MCP ${subtype}:`, error);
      return [{
        kind: 'mcp:item',
        id: `${subtype}:error`,
        label: `Error loading ${subtype}`,
        description: 'Failed to load MCP items',
        extra: { subtype, hasError: true },
      }];
    }
  }
}
