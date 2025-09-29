import type * as monaco from 'monaco-editor';
import type { CompletionContext } from './index';

interface SqlContext {
  kind: 'table' | 'column' | 'none';
  table?: string;
}

export function registerSqlCompletion(m: typeof monaco, context: CompletionContext) {
  m.languages.registerCompletionItemProvider('mustache-sql', {
    triggerCharacters: [' ', '.', ','],
    provideCompletionItems(model, position) {
      const suggestions: monaco.languages.CompletionItem[] = [];
      const range = new m.Range(position.lineNumber, position.column, position.lineNumber, position.column);
      const ctx = inferContext(model, position);
      const schema = context.getSchema();
      if (!schema) {
        return { suggestions };
      }

      if (ctx.kind === 'table') {
        // Use the flattened tables array from completion format
        const tables = schema.tables || [];
        for (const table of tables) {
          suggestions.push({
            label: table.qualified_name || `${table.schema}.${table.name}`,
            kind: m.languages.CompletionItemKind.Class,
            insertText: table.qualified_name || `${table.schema}.${table.name}`,
            detail: `${table.type} in ${table.schema}`,
            range,
          });
        }
      }

      if (ctx.kind === 'column' && ctx.table) {
        // Use the flattened columns array from completion format
        const columns = schema.columns || [];
        const { catalog, table } = splitCatalogTable(ctx.table);
        
        // Filter columns for the specific table
        const tableColumns = columns.filter((col: any) => 
          col.table === table && col.schema === catalog
        );
        
        for (const col of tableColumns) {
          suggestions.push({
            label: col.name,
            kind: m.languages.CompletionItemKind.Field,
            insertText: col.name,
            detail: `${col.type}${col.nullable ? ' (nullable)' : ' (not null)'}`,
            range,
          });
        }
      }

      return { suggestions };
    },
  });
}

function inferContext(model: monaco.editor.ITextModel, position: monaco.Position): SqlContext {
  const textUntilPosition = model.getValueInRange({
    startLineNumber: 1,
    startColumn: 1,
    endLineNumber: position.lineNumber,
    endColumn: position.column,
  }).toUpperCase();

  if (/\bFROM\s+[A-Z0-9_\.]*$/i.test(textUntilPosition) || /\bJOIN\s+[A-Z0-9_\.]*$/i.test(textUntilPosition)) {
    return { kind: 'table' };
  }

  const tableMatch = /([A-Z0-9_\.]+)\.([A-Z0-9_]*)$/i.exec(textUntilPosition);
  if (tableMatch) {
    return { kind: 'column', table: tableMatch[1] };
  }

  return { kind: 'none' };
}

function splitCatalogTable(name: string): { catalog: string; table: string } {
  if (name.includes('.')) {
    const [catalog, table] = name.split('.', 2);
    return { catalog, table };
  }
  return { catalog: 'main', table: name };
}
