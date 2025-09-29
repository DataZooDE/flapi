import type * as monaco from 'monaco-editor';
import type { CompletionContext } from './index';

export function registerMustacheCompletion(m: typeof monaco, context: CompletionContext) {
  m.languages.registerCompletionItemProvider('mustache-sql', {
    triggerCharacters: ['{', '.', '_'],
    provideCompletionItems(model, position) {
      const lineContent = model.getLineContent(position.lineNumber);
      const textBeforeCursor = lineContent.substring(0, position.column - 1);
      if (!textBeforeCursor.includes('{{')) {
        return { suggestions: [] };
      }

      const variables = context.getMustacheVariables();
      const range = new m.Range(position.lineNumber, position.column, position.lineNumber, position.column);
      const suggestions = variables.map((name) => ({
        label: name,
        kind: m.languages.CompletionItemKind.Variable,
        insertText: name,
        range,
      }));
      return { suggestions };
    },
  });
}
