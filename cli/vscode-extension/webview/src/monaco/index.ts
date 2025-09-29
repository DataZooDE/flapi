import * as monaco from 'monaco-editor';
import { registerMustacheLanguage } from './mustacheLanguage';
import { registerMustacheCompletion } from './mustacheCompletion';
import { registerSqlCompletion } from './sqlCompletion';

export interface EditorInstance {
  editor: monaco.editor.IStandaloneCodeEditor;
  dispose(): void;
  getValue(): string;
  setValue(value: string): void;
  layout(): void;
}

export type CompletionContext = {
  getMustacheVariables(): string[];
  getSchema(): any;
};

registerMustacheLanguage(monaco);

export function initializeMonaco(context: CompletionContext & { workerBaseUrl: string }) {
  // Register completion providers now that we have variable discovery
  registerMustacheCompletion(monaco, context);
  registerSqlCompletion(monaco, context);

  // Disable workers entirely for webview context to avoid CORS issues
  (self as any).MonacoEnvironment = {
    getWorker: function (_moduleId: string, _label: string) {
      // Return null to disable workers and run everything in main thread
      // This is acceptable for the webview use case and avoids CORS issues
      return null;
    },
  };
  return monaco;
}

export function createEditor(container: HTMLElement, options: monaco.editor.IStandaloneEditorConstructionOptions): EditorInstance {
  const editor = monaco.editor.create(container, {
    automaticLayout: true,
    minimap: { enabled: false },
    fontSize: 13,
    theme: getPreferredTheme(),
    // Completely disable language services and problematic features
    links: false,
    colorDecorators: false,
    codeLens: false,
    folding: false,
    foldingStrategy: 'indentation',
    showFoldingControls: 'never',
    // Disable all language features that require workers
    quickSuggestions: false,
    suggestOnTriggerCharacters: false,
    acceptSuggestionOnEnter: 'off',
    tabCompletion: 'off',
    wordBasedSuggestions: 'off',
    // Disable validation by disabling language features
    // validate: false, // Not a valid option
    // Disable other problematic features
    hover: { enabled: false },
    contextmenu: false,
    ...options,
  });

  // Disable language services after creation
  if (editor.getModel()) {
    monaco.languages.setLanguageConfiguration('json', {});
    monaco.languages.setLanguageConfiguration('sql', {});
  }

  return {
    editor,
    dispose: () => editor.dispose(),
    getValue: () => editor.getValue(),
    setValue: (value: string) => editor.setValue(value ?? ''),
    layout: () => editor.layout(),
  };
}

function getPreferredTheme(): string {
  const body = document.body;
  const isDark = body.classList.contains('vscode-dark');
  return isDark ? 'vs-dark' : 'vs';
}
