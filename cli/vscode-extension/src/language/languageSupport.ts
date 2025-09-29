import * as vscode from 'vscode';
import { createApiClient } from '@flapi/shared';

/**
 * Provides language support for flapi documents
 */
export class LanguageSupport {
  private disposables: vscode.Disposable[] = [];

  constructor(private client: ReturnType<typeof createApiClient>) {}

  public register(context: vscode.ExtensionContext): void {
    // Register completion providers for SQL+Mustache templates (DuckDB dialect)
    const sqlCompletionProvider = vscode.languages.registerCompletionItemProvider(
      [{ scheme: 'flapi', pattern: '**/*.sql' }, { scheme: 'file', language: 'sql' }],
      {
        provideCompletionItems: (document, position, token, context) => {
          return this.provideSqlCompletions(document, position);
        }
      },
      '.' // Trigger on dot for table.column completion
    );

    // Register completion providers for JSON configurations
    const jsonCompletionProvider = vscode.languages.registerCompletionItemProvider(
      { scheme: 'flapi', pattern: '**/*.json' },
      {
        provideCompletionItems: (document, position, token, context) => {
          return this.provideJsonCompletions(document, position);
        }
      }
    );

    // Register hover providers for variables and schema elements
    const hoverProvider = vscode.languages.registerHoverProvider(
      [{ scheme: 'flapi' }, { scheme: 'file' }],
      {
        provideHover: (document, position, token) => {
          return this.provideHover(document, position);
        }
      }
    );

    // Register document symbol provider for better navigation
    const symbolProvider = vscode.languages.registerDocumentSymbolProvider(
      [{ scheme: 'flapi', pattern: '**/*.sql' }, { scheme: 'file', language: 'sql' }],
      {
        provideDocumentSymbols: (document, token) => {
          return this.provideSqlSymbols(document);
        }
      }
    );

    this.disposables.push(
      sqlCompletionProvider,
      jsonCompletionProvider,
      hoverProvider,
      symbolProvider
    );

    context.subscriptions.push(...this.disposables);
  }

  /**
   * Provides SQL completions for table names, columns, and variables
   */
  private async provideSqlCompletions(
    document: vscode.TextDocument,
    position: vscode.Position
  ): Promise<vscode.CompletionItem[]> {
    const items: vscode.CompletionItem[] = [];
    const lineText = document.lineAt(position).text;
    const wordRange = document.getWordRangeAtPosition(position);
    const word = wordRange ? document.getText(wordRange) : '';

    // Basic DuckDB keywords
    const duckKeywords = ['SELECT', 'FROM', 'WHERE', 'GROUP BY', 'ORDER BY', 'LIMIT', 'OFFSET', 'WITH', 'UNION ALL'];
    duckKeywords.forEach((kw) => items.push(new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword)));

    // Mustache helpers
    const mustacheSnippets: Array<[string, string]> = [
      ['Mustache Variable', '{{${1:params.name}}}'],
      ['Mustache Section', '{{#${1:section}}}\n  ${2:content}\n{{/${1:section}}}'],
      ['Mustache Inverted', '{{^${1:section}}}\n  ${2:content}\n{{/${1:section}}}'],
    ];
    mustacheSnippets.forEach(([label, snippet]) => {
      const item = new vscode.CompletionItem(label, vscode.CompletionItemKind.Snippet);
      item.insertText = new vscode.SnippetString(snippet);
      items.push(item);
    });

    try {
      // Get schema information for table/column completion
      const schemaResponse = await this.client.get('/api/v1/_config/schema?format=completion');
      const schema = schemaResponse.data;

      if (Array.isArray(schema)) {
        for (const item of schema) {
          // Add table completions
          const tableItem = new vscode.CompletionItem(item.name, vscode.CompletionItemKind.Class);
          tableItem.detail = `Table in ${item.schema}`;
          tableItem.documentation = `Type: ${item.type}`;
          items.push(tableItem);

          // Add column completions with qualified names
          if (item.qualified_name) {
            const columnItem = new vscode.CompletionItem(item.qualified_name, vscode.CompletionItemKind.Field);
            columnItem.detail = `Column: ${item.type}${item.nullable ? ' (nullable)' : ''}`;
            columnItem.insertText = item.qualified_name;
            items.push(columnItem);
          }
        }
      }

      // Add variable completions
      const variableCompletions = this.getVariableCompletions();
      items.push(...variableCompletions);

    } catch (error) {
      console.error('Failed to load schema for completion:', error);
    }

    return items;
  }

  /**
   * Provides JSON completions for configuration files
   */
  private async provideJsonCompletions(
    document: vscode.TextDocument,
    position: vscode.Position
  ): Promise<vscode.CompletionItem[]> {
    const items: vscode.CompletionItem[] = [];
    
    // Determine what type of JSON file this is based on the URI
    const uri = document.uri;
    if (uri.path.includes('config.json')) {
      items.push(...this.getEndpointConfigCompletions());
    } else if (uri.path.includes('cache.json')) {
      items.push(...this.getCacheConfigCompletions());
    } else if (uri.path.includes('parameters.json')) {
      items.push(...this.getParametersCompletions());
    }

    return items;
  }

  /**
   * Provides hover information for variables and schema elements
   */
  private async provideHover(
    document: vscode.TextDocument,
    position: vscode.Position
  ): Promise<vscode.Hover | undefined> {
    const wordRange = document.getWordRangeAtPosition(position);
    if (!wordRange) return undefined;

    const word = document.getText(wordRange);
    
    // Check if it's a variable reference
    if (word.startsWith('{{') && word.endsWith('}}')) {
      const varName = word.slice(2, -2).trim();
      return new vscode.Hover(this.getVariableHover(varName));
    }

    // Check if it's a table or column reference
    try {
      const schemaResponse = await this.client.get('/api/v1/_config/schema?format=completion');
      const schema = schemaResponse.data;

      if (Array.isArray(schema)) {
        const schemaItem = schema.find(item => 
          item.name === word || item.qualified_name === word
        );
        
        if (schemaItem) {
          const markdown = new vscode.MarkdownString();
          markdown.appendCodeblock(`${schemaItem.qualified_name}: ${schemaItem.type}`, 'sql');
          if (schemaItem.nullable) {
            markdown.appendText('\n\n*Nullable*');
          }
          return new vscode.Hover(markdown);
        }
      }
    } catch (error) {
      console.error('Failed to load schema for hover:', error);
    }

    return undefined;
  }

  /**
   * Provides document symbols for SQL files
   */
  private provideSqlSymbols(document: vscode.TextDocument): vscode.DocumentSymbol[] {
    const symbols: vscode.DocumentSymbol[] = [];
    const text = document.getText();
    
    // Find variable references
    const variableRegex = /\{\{([^}]+)\}\}/g;
    let match;
    
    while ((match = variableRegex.exec(text)) !== null) {
      const varName = match[1].trim();
      const startPos = document.positionAt(match.index);
      const endPos = document.positionAt(match.index + match[0].length);
      const range = new vscode.Range(startPos, endPos);
      
      const symbol = new vscode.DocumentSymbol(
        varName,
        'Variable',
        vscode.SymbolKind.Variable,
        range,
        range
      );
      
      symbols.push(symbol);
    }

    return symbols;
  }

  /**
   * Gets variable completion items
   */
  private getVariableCompletions(): vscode.CompletionItem[] {
    const variables = [
      { name: 'params.*', description: 'Request parameters' },
      { name: 'conn.*', description: 'Connection configuration' },
      { name: 'env.*', description: 'Environment variables' },
      { name: 'cache.*', description: 'Cache data' }
    ];

    return variables.map(variable => {
      const item = new vscode.CompletionItem(variable.name, vscode.CompletionItemKind.Variable);
      item.detail = variable.description;
      item.insertText = new vscode.SnippetString(`{{${variable.name}}}`);
      return item;
    });
  }

  /**
   * Gets endpoint configuration completion items
   */
  private getEndpointConfigCompletions(): vscode.CompletionItem[] {
    const configs = [
      { key: 'path', type: 'string', description: 'Endpoint path' },
      { key: 'method', type: 'string', description: 'HTTP method (GET, POST, etc.)' },
      { key: 'templateSource', type: 'string', description: 'SQL template source' },
      { key: 'connection', type: 'string', description: 'Database connection name' },
      { key: 'requestFields', type: 'array', description: 'Request field definitions' },
      { key: 'rateLimit', type: 'object', description: 'Rate limiting configuration' },
      { key: 'auth', type: 'object', description: 'Authentication configuration' }
    ];

    return configs.map(config => {
      const item = new vscode.CompletionItem(`"${config.key}"`, vscode.CompletionItemKind.Property);
      item.detail = `${config.type} - ${config.description}`;
      item.insertText = new vscode.SnippetString(`"${config.key}": $0`);
      return item;
    });
  }

  /**
   * Gets cache configuration completion items
   */
  private getCacheConfigCompletions(): vscode.CompletionItem[] {
    const configs = [
      { key: 'enabled', type: 'boolean', description: 'Enable caching' },
      { key: 'ttl', type: 'number', description: 'Time to live in seconds' },
      { key: 'template', type: 'string', description: 'Cache key template' }
    ];

    return configs.map(config => {
      const item = new vscode.CompletionItem(`"${config.key}"`, vscode.CompletionItemKind.Property);
      item.detail = `${config.type} - ${config.description}`;
      item.insertText = new vscode.SnippetString(`"${config.key}": $0`);
      return item;
    });
  }

  /**
   * Gets parameters completion items
   */
  private getParametersCompletions(): vscode.CompletionItem[] {
    const commonParams = [
      { key: 'id', type: 'number', description: 'Numeric identifier' },
      { key: 'name', type: 'string', description: 'Name parameter' },
      { key: 'limit', type: 'number', description: 'Result limit' },
      { key: 'offset', type: 'number', description: 'Result offset' },
      { key: 'filter', type: 'string', description: 'Filter criteria' }
    ];

    return commonParams.map(param => {
      const item = new vscode.CompletionItem(`"${param.key}"`, vscode.CompletionItemKind.Property);
      item.detail = `${param.type} - ${param.description}`;
      item.insertText = new vscode.SnippetString(`"${param.key}": $0`);
      return item;
    });
  }

  /**
   * Gets hover information for variables
   */
  private getVariableHover(varName: string): vscode.MarkdownString {
    const markdown = new vscode.MarkdownString();
    
    if (varName.startsWith('params.')) {
      markdown.appendText('**Request Parameter**\n\n');
      markdown.appendText(`Variable: \`${varName}\`\n\n`);
      markdown.appendText('Populated from HTTP request parameters.');
    } else if (varName.startsWith('conn.')) {
      markdown.appendText('**Connection Variable**\n\n');
      markdown.appendText(`Variable: \`${varName}\`\n\n`);
      markdown.appendText('Populated from database connection configuration.');
    } else if (varName.startsWith('env.')) {
      markdown.appendText('**Environment Variable**\n\n');
      markdown.appendText(`Variable: \`${varName}\`\n\n`);
      markdown.appendText('Populated from environment variables.');
    } else if (varName.startsWith('cache.')) {
      markdown.appendText('**Cache Variable**\n\n');
      markdown.appendText(`Variable: \`${varName}\`\n\n`);
      markdown.appendText('Populated from cached data.');
    } else {
      markdown.appendText('**Template Variable**\n\n');
      markdown.appendText(`Variable: \`${varName}\`\n\n`);
      markdown.appendText('Custom template variable.');
    }

    return markdown;
  }

  public dispose(): void {
    this.disposables.forEach(d => d.dispose());
  }
}
