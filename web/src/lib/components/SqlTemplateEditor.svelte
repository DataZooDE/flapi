<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import { EditorState } from "@codemirror/state";
  import { EditorView, keymap } from '@codemirror/view';
  import { sql } from '@codemirror/lang-sql';
  import { validateSql } from '$lib/api';
  import { defaultKeymap } from '@codemirror/commands';
  import { autocompletion } from '@codemirror/autocomplete';
  import type { CompletionContext } from '@codemirror/autocomplete';
  import { oneDark } from '@codemirror/theme-one-dark';
  import { syntaxHighlighting } from '@codemirror/language';
  import CopyIcon from 'lucide-svelte/icons/copy';
  import TrashIcon from 'lucide-svelte/icons/trash-2';
  import TerminalIcon from 'lucide-svelte/icons/terminal';
  import PlayCircleIcon from 'lucide-svelte/icons/play-circle';
  import DatabaseIcon from 'lucide-svelte/icons/database';

  export let source: string;
  export let variables: Array<{ name: string; type: string; description?: string }> = [];
  export let onExecute: (sql: string) => Promise<any>;
  export let onExpand: (template: string) => Promise<string> = async (template) => template;

  let editor: any = null;
  let editorElement: HTMLElement;
  let validationError: string | null = null;
  let previewContent: string = '';
  let isExecuting = false;
  let isExpanding = false;
  let customVariables: Array<{ name: string; type: string; description?: string }> = [];

  // Database schema information
  let tables: Array<{
    name: string;
    columns: Array<{ name: string; type: string; constraints?: string[] }>;
  }> = [];

  // Load schema information
  async function loadSchema() {
    try {
      // TODO: Replace with actual API call
      tables = [
        {
          name: 'actions',
          columns: [
            { name: 'action_id', type: 'integer', constraints: ['PK', 'NOT NULL'] },
            { name: 'service_id', type: 'integer', constraints: ['FK'] },
            { name: 'name', type: 'text' },
            { name: 'reference_url', type: 'text' },
            { name: 'permission_only_flag', type: 'boolean' },
            { name: 'access_level', type: 'text' }
          ]
        }
      ];
    } catch (e) {
      console.error('Failed to load schema:', e);
    }
  }

  function addCustomVariable() {
    customVariables = [...customVariables, { name: '', type: 'string' }];
  }

  function removeCustomVariable(index: number) {
    customVariables = customVariables.filter((_, i) => i !== index);
  }

  function updateCustomVariable(index: number, field: string, value: string) {
    customVariables = customVariables.map((v, i) => 
      i === index ? { ...v, [field]: value } : v
    );
  }

  // Combine predefined and custom variables for completion
  $: allVariables = [...variables, ...customVariables];

  // Mustache completion function
  function mustacheCompletions(context: CompletionContext) {
    let word = context.matchBefore(/\{\{.*\}\}|\{\{.*|\{\{/);
    if (!word) return null;

    return {
      from: word.from,
      options: allVariables.map(v => ({
        label: v.name,
        type: 'variable',
        info: `${v.type}${v.description ? ` - ${v.description}` : ''}`,
        apply: `{{${v.name}}}`
      }))
    };
  }

  function insertVariable(variable: string) {
    if (!editor) return;
    const pos = editor.state.selection.main.head;
    editor.dispatch({
      changes: { from: pos, insert: `{{${variable}}}` }
    });
    editor.focus();
  }

  async function handleExecute() {
    if (isExecuting) return;
    isExecuting = true;
    try {
      const result = await onExecute(source);
      previewContent = JSON.stringify(result, null, 2);
    } catch (e) {
      previewContent = e instanceof Error ? e.message : 'Failed to execute query';
    } finally {
      isExecuting = false;
    }
  }

  async function handleExpand() {
    if (isExpanding) return;
    isExpanding = true;
    try {
      previewContent = await onExpand(source);
    } catch (e) {
      previewContent = e instanceof Error ? e.message : 'Failed to expand template';
    } finally {
      isExpanding = false;
    }
  }

  async function validateTemplate(sql: string) {
    try {
      const result = await validateSql(sql);
      validationError = result.error || null;
    } catch (e) {
      validationError = e instanceof Error ? e.message : 'Failed to validate SQL';
    }
  }

  onMount(() => {
    if (!editorElement) return;
    loadSchema();

    try {
      const state = EditorState.create({
        doc: source,
        extensions: [
          keymap.of(defaultKeymap),
          sql(),
          oneDark,
          EditorView.theme({
            "&": { height: "100%" },
            ".cm-scroller": { overflow: "auto" },
            ".cm-content": { padding: "4px 0" },
            ".cm-line": { padding: "0 8px" }
          }),
          autocompletion({ override: [mustacheCompletions] }),
          EditorView.updateListener.of((update: { docChanged: boolean; state: { doc: { toString(): string } } }) => {
            if (update.docChanged) {
              const newValue = update.state.doc.toString();
              if (source !== newValue) {
                source = newValue;
                validateTemplate(newValue).catch(console.error);
              }
            }
          })
        ]
      });

      editor = new EditorView({
        state,
        parent: editorElement
      });
    } catch (e) {
      console.error('Failed to initialize editor:', e);
    }
  });

  onDestroy(() => {
    if (editor) {
      try {
        editor.destroy();
      } catch (e) {
        console.error('Failed to destroy editor:', e);
      }
      editor = null;
    }
  });
</script>

<div class="grid grid-cols-12 gap-4">
  <!-- Left Sidebar -->
  <div class="col-span-3 space-y-6">
    <!-- Schema Browser -->
    <div class="card p-4">
      <div class="flex items-center space-x-2 mb-3">
        <DatabaseIcon class="h-4 w-4 text-primary-600" />
        <h4 class="font-medium">Database Schema</h4>
      </div>
      <div class="space-y-3">
        {#each tables as table}
          <div class="space-y-2">
            <div class="flex items-center space-x-2 bg-gray-50 p-2 rounded">
              <DatabaseIcon class="h-3 w-3 text-gray-600" />
              <span class="font-medium text-sm">{table.name}</span>
            </div>
            <ul class="ml-4 space-y-1 border-l border-gray-200 pl-4">
              {#each table.columns as column}
                <li class="text-sm">
                  <div class="flex items-center justify-between">
                    <span class="text-gray-700">{column.name}</span>
                    <div class="flex items-center space-x-2">
                      <span class="text-xs px-1.5 py-0.5 bg-gray-100 rounded">{column.type}</span>
                      {#if column.constraints}
                        <span class="text-xs px-1.5 py-0.5 bg-blue-50 text-blue-700 rounded">
                          {column.constraints.join(', ')}
                        </span>
                      {/if}
                    </div>
                  </div>
                </li>
              {/each}
            </ul>
          </div>
        {/each}
      </div>
    </div>

    <!-- Variables -->
    <div class="card p-4">
      <div class="space-y-4">
        <div>
          <h4 class="font-medium mb-2">Template Variables</h4>
          <div class="space-y-2">
            {#each variables as variable}
              <button
                class="w-full text-left p-2 hover:bg-gray-50 rounded border border-gray-200 transition-colors"
                on:click={() => insertVariable(variable.name)}
              >
                <div class="flex items-center justify-between">
                  <code class="text-sm text-primary-600">{`{{${variable.name}}}`}</code>
                  <span class="text-xs px-1.5 py-0.5 bg-gray-100 rounded">{variable.type}</span>
                </div>
                {#if variable.description}
                  <p class="text-xs text-gray-600 mt-1">{variable.description}</p>
                {/if}
              </button>
            {/each}
          </div>
        </div>

        {#if customVariables.length > 0}
          <div>
            <h4 class="font-medium mb-2">Custom Variables</h4>
            <div class="space-y-2">
              {#each customVariables as variable}
                <button
                  class="w-full text-left p-2 hover:bg-gray-50 rounded border border-gray-200 transition-colors"
                  on:click={() => insertVariable(variable.name)}
                >
                  <div class="flex items-center justify-between">
                    <code class="text-sm text-primary-600">{`{{${variable.name}}}`}</code>
                    <span class="text-xs px-1.5 py-0.5 bg-gray-100 rounded">{variable.type}</span>
                  </div>
                  {#if variable.description}
                    <p class="text-xs text-gray-600 mt-1">{variable.description}</p>
                  {/if}
                </button>
              {/each}
            </div>
          </div>
        {/if}
      </div>
    </div>
  </div>

  <!-- Editor Section -->
  <div class="col-span-9">
    <!-- Editor Toolbar -->
    <div class="flex justify-between items-center mb-3">
      <div class="flex space-x-2">
        <button class="btn btn-sm flex items-center" on:click={() => navigator.clipboard.writeText(source)}>
          <CopyIcon class="h-4 w-4 mr-2" />
          <span>Copy</span>
        </button>
        <button class="btn btn-sm flex items-center" on:click={() => editor?.dispatch({ changes: { from: 0, to: source.length, insert: '' } })}>
          <TrashIcon class="h-4 w-4 mr-2" />
          <span>Clear</span>
        </button>
      </div>
      <div class="flex space-x-2">
        <button 
          class="btn btn-sm btn-primary flex items-center" 
          on:click={handleExpand}
          disabled={isExpanding}
        >
          <TerminalIcon class="h-4 w-4 mr-2" />
          <span>Expand Template</span>
        </button>
        <button 
          class="btn btn-sm btn-primary flex items-center" 
          on:click={handleExecute}
          disabled={isExecuting}
        >
          <PlayCircleIcon class="h-4 w-4 mr-2" />
          <span>Execute Query</span>
        </button>
      </div>
    </div>

    <!-- Editor -->
    <div class="card">
      <div 
        bind:this={editorElement} 
        class="border rounded-md overflow-hidden h-[400px]"
      />
    </div>

    <!-- Preview/Output Area -->
    <div class="mt-4">
      <h4 class="font-medium mb-2">Output</h4>
      <pre class="bg-gray-50 p-3 rounded border font-mono text-sm overflow-x-auto whitespace-pre-wrap h-[150px] overflow-y-auto">
        {previewContent}
      </pre>
    </div>

    {#if validationError}
      <div class="mt-2 text-red-800 bg-red-100 p-4 rounded">
        {validationError}
      </div>
    {/if}
  </div>
</div>

<style>
  :global(.cm-editor) {
    height: 100%;
  }

  :global(.cm-editor .cm-scroller) {
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
    font-size: 14px;
    line-height: 1.5;
    overflow: auto;
  }

  :global(.cm-editor .cm-gutters) {
    border-right: 1px solid #e5e7eb;
    background: #f9fafb;
  }

  :global(.cm-editor .cm-activeLineGutter) {
    background: #f3f4f6;
  }

  :global(.cm-editor.cm-focused) {
    outline: none;
  }

  :global(.cm-editor .cm-line) {
    padding: 0 8px;
  }

  :global(.cm-editor .cm-mustache) {
    color: #0550ae;
    font-weight: 500;
  }
</style>
