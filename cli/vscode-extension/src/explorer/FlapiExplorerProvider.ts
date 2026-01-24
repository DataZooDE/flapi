import * as vscode from 'vscode';
import type { AxiosInstance } from 'axios';
import { pathToSlug } from '@flapi/shared';

export type FlapiNode = FlapiNodeMetadata;

interface FlapiNodeMetadata {
  kind:
    | 'file:flapi-yaml'
    | 'directory'
    | 'file:yaml:endpoint'
    | 'file:yaml:mcp-tool'
    | 'file:yaml:mcp-resource'
    | 'file:yaml:mcp-prompt'
    | 'file:yaml:shared'
    | 'file:sql:template'
    | 'file:sql:cache'
    | 'file:other';
  id: string;
  label: string;
  description?: string;
  parentId?: string;
  extra?: Record<string, unknown>;
}

interface FilesystemNode {
  name: string;
  path: string;
  type: 'file' | 'directory';
  extension?: string;
  yaml_type?: 'endpoint' | 'mcp-tool' | 'mcp-resource' | 'mcp-prompt' | 'shared';
  url_path?: string;
  mcp_name?: string;
  template_source?: string;
  cache_template_source?: string;
  file_type?: string;
  children?: FilesystemNode[];
}

interface FilesystemResponse {
  base_path: string;
  template_path: string;
  config_file?: string;
  config_file_exists?: boolean;
  tree: FilesystemNode[];
}

export class FlapiExplorerProvider implements vscode.TreeDataProvider<FlapiNodeMetadata> {
  private readonly _onDidChangeTreeData = new vscode.EventEmitter<FlapiNodeMetadata | undefined>();
  readonly onDidChangeTreeData = this._onDidChangeTreeData.event;
  
  private filesystemCache?: FilesystemResponse;
  private client: AxiosInstance;

  constructor(client: AxiosInstance) {
    this.client = client;
  }

  updateClient(client: AxiosInstance): void {
    console.log('[Flapi] FlapiExplorerProvider.updateClient called');
    console.log('[Flapi] Client headers:', JSON.stringify(client.defaults.headers, null, 2));
    this.client = client;
    console.log('[Flapi] FlapiExplorerProvider calling refresh()');
    this.refresh(); // Refresh with new client
  }

  refresh(node?: FlapiNodeMetadata): void {
    this.filesystemCache = undefined; // Clear cache
    this._onDidChangeTreeData.fire(node);
  }

  getTreeItem(element: FlapiNodeMetadata): vscode.TreeItem {
    switch (element.kind) {
      case 'file:flapi-yaml':
        return {
          label: element.label,
          description: 'Project Config',
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:config-file',
          iconPath: new vscode.ThemeIcon('settings-gear'),
          command: {
            command: 'flapi.openProjectConfig',
            title: 'Open Project Config',
            arguments: [element],
          },
        };
      
      case 'directory':
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.Collapsed,
          contextValue: 'flapi:directory',
          iconPath: new vscode.ThemeIcon('folder'),
        };
      
      case 'file:yaml:endpoint':
        const urlPath = element.extra?.url_path as string ?? '';
        const hasTemplateOrCache = element.extra?.template_source || element.extra?.cache_template_source;
        return {
          label: element.label,
          description: `📡 ${urlPath}`,
          collapsibleState: hasTemplateOrCache 
            ? vscode.TreeItemCollapsibleState.Collapsed 
            : vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:yaml-endpoint',
          iconPath: new vscode.ThemeIcon('file-code'),
          command: {
            command: 'flapi.openYamlFile',
            title: 'Open YAML',
            arguments: [element],
          },
        };
      
      case 'file:yaml:mcp-tool':
        const toolName = element.extra?.mcp_name as string ?? '';
        const hasMcpToolTemplate = element.extra?.template_source;
        return {
          label: element.label,
          description: `🔧 ${toolName}`,
          collapsibleState: hasMcpToolTemplate 
            ? vscode.TreeItemCollapsibleState.Collapsed 
            : vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:yaml-mcp-tool',
          iconPath: new vscode.ThemeIcon('tools'),
          command: {
            command: 'flapi.openYamlFile',
            title: 'Open MCP Tool',
            arguments: [element],
          },
        };
      
      case 'file:yaml:mcp-resource':
        const resourceName = element.extra?.mcp_name as string ?? '';
        return {
          label: element.label,
          description: `📦 ${resourceName}`,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:yaml-mcp-resource',
          iconPath: new vscode.ThemeIcon('package'),
          command: {
            command: 'flapi.openYamlFile',
            title: 'Open MCP Resource',
            arguments: [element],
          },
        };
      
      case 'file:yaml:mcp-prompt':
        const promptName = element.extra?.mcp_name as string ?? '';
        return {
          label: element.label,
          description: `💬 ${promptName}`,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:yaml-mcp-prompt',
          iconPath: new vscode.ThemeIcon('comment'),
          command: {
            command: 'flapi.openYamlFile',
            title: 'Open MCP Prompt',
            arguments: [element],
          },
        };
      
      case 'file:yaml:shared':
        return {
          label: element.label,
          description: '🔗 shared',
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:yaml-shared',
          iconPath: new vscode.ThemeIcon('file-code'),
          command: {
            command: 'flapi.openYamlFile',
            title: 'Open Shared Config',
            arguments: [element],
          },
        };
      
      case 'file:sql:template':
      case 'file:sql:cache':
        const isCacheSql = element.kind === 'file:sql:cache';
        return {
          label: element.label,
          description: isCacheSql ? '(cache)' : '(template)',
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: isCacheSql ? 'flapi:sql-cache' : 'flapi:sql-template',
          iconPath: new vscode.ThemeIcon('database'),
          command: {
            command: 'flapi.openSqlFile',
            title: 'Open SQL',
            arguments: [element],
          },
        };
      
      case 'file:other':
        return {
          label: element.label,
          collapsibleState: vscode.TreeItemCollapsibleState.None,
          contextValue: 'flapi:file-other',
          iconPath: new vscode.ThemeIcon('file'),
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
      // Root level: show flapi.yaml + filesystem tree
      return this.loadRootNodes();
    }

    // For directories, load their children from the cached filesystem
    if (element.kind === 'directory') {
      return this.loadDirectoryChildren(element);
    }

    // For YAML files with templates, show their referenced SQL files
    if (element.kind === 'file:yaml:endpoint' || element.kind === 'file:yaml:mcp-tool' || 
        element.kind === 'file:yaml:mcp-resource' || element.kind === 'file:yaml:mcp-prompt') {
      return this.loadYamlChildren(element);
    }

    return [];
  }

  private async loadRootNodes(): Promise<FlapiNodeMetadata[]> {
    try {
      const response = await this.client.get<FilesystemResponse>('/api/v1/_config/filesystem');
      this.filesystemCache = response.data;
      
      const nodes: FlapiNodeMetadata[] = [];
      
      // Add flapi.yaml at the top
      if (this.filesystemCache.config_file_exists) {
        nodes.push({
          kind: 'file:flapi-yaml',
          id: 'flapi.yaml',
          label: this.filesystemCache.config_file ?? 'flapi.yaml',
          extra: {
            base_path: this.filesystemCache.base_path,
          },
        });
      }
      
      // Add filesystem tree nodes
      if (this.filesystemCache.tree) {
        for (const node of this.filesystemCache.tree) {
          nodes.push(this.convertFilesystemNode(node));
        }
      }
      
      return nodes;
    } catch (error: any) {
      // Handle specific error cases
      if (error?.response?.status === 404) {
        // Config service is not enabled
        const action = await vscode.window.showWarningMessage(
          'Config Service is not enabled on the server. Start the server with --config-service flag.',
          'OK'
        );
        return [];
      } else if (error?.response?.status === 401) {
        // Config service is enabled but no token provided
        const action = await vscode.window.showWarningMessage(
          'Config Service requires authentication. Please set your config service token.',
          'Set Token'
        );
        if (action === 'Set Token') {
          await vscode.commands.executeCommand('flapi.setConfigToken');
        }
        return [];
      } else {
        // Other errors
        void vscode.window.showErrorMessage(`Failed to load filesystem: ${error?.message || String(error)}`);
        return [];
      }
    }
  }

  private async loadDirectoryChildren(element: FlapiNodeMetadata): Promise<FlapiNodeMetadata[]> {
    // Use cached filesystem data to find children
    if (!this.filesystemCache) {
      return [];
    }
    
    const nodePath = element.extra?.path as string;
    const node = this.findNodeByPath(this.filesystemCache.tree, nodePath);
    
    if (!node || !node.children) {
      return [];
    }
    
    // Build a set of SQL files that are referenced by YAMLs
    const referencedSqlFiles = new Set<string>();
    for (const child of node.children) {
      if ((child.extension === '.yaml' || child.extension === '.yml') && child.yaml_type) {
        if (child.template_source) {
          referencedSqlFiles.add(child.template_source);
        }
        if (child.cache_template_source) {
          // Handle both relative and absolute paths
          const cacheFile = child.cache_template_source.split('/').pop() ?? '';
          referencedSqlFiles.add(cacheFile);
        }
      }
    }
    
    // Filter out SQL files that are referenced by YAMLs
    // They will be shown as children of the YAML files instead
    const filteredChildren = node.children.filter(child => {
      if (child.extension === '.sql') {
        return !referencedSqlFiles.has(child.name);
      }
      return true;
    });
    
    return filteredChildren.map(child => this.convertFilesystemNode(child, nodePath));
  }

  private async loadYamlChildren(element: FlapiNodeMetadata): Promise<FlapiNodeMetadata[]> {
    const children: FlapiNodeMetadata[] = [];
    const parentPath = element.extra?.path as string;
    if (!parentPath) {
      return [];
    }
    
    const parentDir = parentPath.substring(0, parentPath.lastIndexOf('/'));
    
    // Add template SQL file if referenced
    const templateSource = element.extra?.template_source as string | undefined;
    if (templateSource) {
      const templatePath = parentDir ? `${parentDir}/${templateSource}` : templateSource;
      children.push({
        kind: 'file:sql:template',
        id: `${element.id}:template`,
        label: templateSource,
        description: '(template)',
        parentId: element.id,
        extra: {
          path: templatePath,
          extension: '.sql',
        },
      });
    }
    
    // Add cache SQL file if referenced
    const cacheTemplateSource = element.extra?.cache_template_source as string | undefined;
    if (cacheTemplateSource) {
      const cachePath = parentDir && !cacheTemplateSource.startsWith('/')
        ? `${parentDir}/${cacheTemplateSource}`
        : cacheTemplateSource;
      children.push({
        kind: 'file:sql:cache',
        id: `${element.id}:cache`,
        label: cacheTemplateSource,
        description: '(cache)',
        parentId: element.id,
        extra: {
          path: cachePath,
          extension: '.sql',
        },
      });
    }
    
    return children;
  }

  private findNodeByPath(nodes: FilesystemNode[], targetPath: string): FilesystemNode | undefined {
    for (const node of nodes) {
      if (node.path === targetPath) {
        return node;
      }
      if (node.children) {
        const found = this.findNodeByPath(node.children, targetPath);
        if (found) {
          return found;
        }
      }
    }
    return undefined;
  }

  private convertFilesystemNode(node: FilesystemNode, parentPath?: string): FlapiNodeMetadata {
    if (node.type === 'directory') {
      return {
        kind: 'directory',
        id: node.path,
        label: node.name,
        parentId: parentPath,
        extra: {
          path: node.path,
        },
      };
    }
    
    // File node
    const ext = node.extension;
    let kind: FlapiNodeMetadata['kind'] = 'file:other';
    const extra: Record<string, unknown> = {
      path: node.path,
      extension: ext,
    };
    
    if (ext === '.yaml' || ext === '.yml') {
      // Determine YAML type
      if (node.yaml_type === 'endpoint') {
        kind = 'file:yaml:endpoint';
        extra.url_path = node.url_path;
        extra.template_source = node.template_source;
        extra.cache_template_source = node.cache_template_source;
      } else if (node.yaml_type === 'mcp-tool') {
        kind = 'file:yaml:mcp-tool';
        extra.mcp_name = node.mcp_name;
        extra.template_source = node.template_source;
      } else if (node.yaml_type === 'mcp-resource') {
        kind = 'file:yaml:mcp-resource';
        extra.mcp_name = node.mcp_name;
        extra.template_source = node.template_source;
      } else if (node.yaml_type === 'mcp-prompt') {
        kind = 'file:yaml:mcp-prompt';
        extra.mcp_name = node.mcp_name;
        extra.template_source = node.template_source;
      } else if (node.yaml_type === 'shared') {
        kind = 'file:yaml:shared';
      }
    } else if (ext === '.sql') {
      // Check if it's a cache SQL (heuristic: name contains _cache or -cache)
      if (node.name.includes('_cache') || node.name.includes('-cache')) {
        kind = 'file:sql:cache';
      } else {
        kind = 'file:sql:template';
      }
    }
    
    return {
      kind,
      id: node.path,
      label: node.name,
      parentId: parentPath,
      extra,
    };
  }
}
