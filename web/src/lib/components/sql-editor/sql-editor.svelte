<script lang="ts">
  import { onMount } from 'svelte';
  import { EditorState, Extension } from '@codemirror/state';
  import { EditorView, ViewUpdate } from '@codemirror/view';
  import { sql } from '@codemirror/lang-sql';
  import { defaultKeymap } from '@codemirror/commands';
  import { syntaxHighlighting, defaultHighlightStyle } from '@codemirror/language';
  import SchemaBrowser from './schema-browser.svelte';
  import { Button } from '$lib/components/ui/button';
  import { api } from '$lib/api';

  // Props with types
  export let value: string = '';
  export let placeholder: string = 'Enter SQL query...';
  export let readonly: boolean = false;
  export let variables: Array<{ name: string; description?: string }> = [];
  export let output: string = '';
  export let showOutput: boolean = false;
  export let showSchema: boolean = false;
  export let path: string = ''; // Path to the endpoint for preview

  // Local state
  let element: HTMLDivElement;
  let view: EditorView | undefined;
  let previewParams: Record<string, string> = {};
  let previewError: string | null = null;
  let isPreviewLoading = false;

  // Function to execute preview
  async function executePreview() {
    if (!path) return;
    
    previewError = null;
    isPreviewLoading = true;
    
    try {
      const response = await api.previewEndpointTemplate(path, {
        template: value,
        parameters: previewParams
      });
      output = response.sql;
      showOutput = true;
    } catch (error) {
      previewError = error instanceof Error ? error.message : 'Preview failed';
      showOutput = true;
      output = `Error: ${previewError}`;
    } finally {
      isPreviewLoading = false;
    }
  }

  // Update preview parameters when variables change
  $: {
    const newParams: Record<string, string> = {};
    variables.forEach(v => {
      if (!(v.name in previewParams)) {
        newParams[v.name] = '';
      }
    });
    previewParams = { ...previewParams, ...newParams };
  }

  // Function to insert variable at cursor
  function insertVariable(varName: string) {
    if (!view) return;
    
    const selection = view.state.selection.main;
    const transaction = view.state.update({
      changes: {
        from: selection.from,
        to: selection.to,
        insert: `{{${varName}}}`
      }
    });
    view.dispatch(transaction);
  }

  // Function to insert table reference
  function insertTable(tableName: string, schemaName: string) {
    if (!view) return;
    
    const selection = view.state.selection.main;
    const tableRef = schemaName === 'public' ? tableName : `${schemaName}.${tableName}`;
    
    const transaction = view.state.update({
      changes: {
        from: selection.from,
        to: selection.to,
        insert: tableRef
      }
    });
    view.dispatch(transaction);
  }

  const baseExtensions: Extension[] = [
    defaultKeymap,
    sql(),
    syntaxHighlighting(defaultHighlightStyle),
    EditorView.lineWrapping,
    EditorView.theme({
      '&': { height: '100%' },
      '.cm-scroller': { overflow: 'auto' },
      '.cm-content': { padding: '10px' }
    })
  ];

  // Setup editor on mount
  onMount(() => {
    const startState = EditorState.create({
      doc: value,
      extensions: [
        ...baseExtensions,
        EditorView.updateListener.of((update: ViewUpdate) => {
          if (update.docChanged) {
            value = update.state.doc.toString();
          }
        }),
        EditorState.readOnly.of(readonly)
      ]
    });

    view = new EditorView({
      state: startState,
      parent: element
    });

    return () => {
      if (view) {
        view.destroy();
      }
    };
  });

  // Update editor when value prop changes
  $: if (view && value !== view.state.doc.toString()) {
    view.dispatch({
      changes: {
        from: 0,
        to: view.state.doc.length,
        insert: value
      }
    });
  }
</script>

<div class="flex h-full gap-2">
  {#if showSchema}
    <div class="w-64 border-r">
      <SchemaBrowser onTableClick={insertTable} />
    </div>
  {/if}
  
  <div class="flex-1 flex flex-col gap-2">
    <!-- Variables Panel -->
    {#if variables.length > 0}
      <div class="flex flex-col gap-2 p-2 bg-secondary/10 rounded-md">
        <div class="flex flex-wrap gap-2">
          {#each variables as variable}
            <button
              class="px-2 py-1 text-sm bg-secondary/20 rounded hover:bg-secondary/30 transition-colors"
              title={variable.description || variable.name}
              on:click={() => insertVariable(variable.name)}
            >
              {variable.name}
            </button>
          {/each}
        </div>
        
        <!-- Preview Parameters -->
        <div class="flex flex-col gap-2">
          <div class="text-sm font-medium">Preview Parameters:</div>
          <div class="grid grid-cols-2 gap-2">
            {#each variables as variable}
              <div class="flex items-center gap-2">
                <span class="text-sm">{variable.name}:</span>
                <input
                  type="text"
                  class="flex-1 px-2 py-1 text-sm border rounded"
                  bind:value={previewParams[variable.name]}
                  placeholder="Value..."
                />
              </div>
            {/each}
          </div>
          <Button 
            variant="outline" 
            size="sm"
            disabled={isPreviewLoading} 
            on:click={executePreview}
          >
            {isPreviewLoading ? 'Previewing...' : 'Preview SQL'}
          </Button>
        </div>
      </div>
    {/if}

    <!-- Editor -->
    <div 
      class="flex-1 w-full border rounded-md overflow-hidden" 
      bind:this={element}
    />

    <!-- Output Panel -->
    {#if showOutput && (output || previewError)}
      <div class="border rounded-md p-2 bg-secondary/5">
        <div class="text-sm font-medium mb-1">
          {previewError ? 'Error:' : 'Expanded SQL:'}
        </div>
        <pre class="text-sm whitespace-pre-wrap {previewError ? 'text-destructive' : ''}">{output}</pre>
      </div>
    {/if}
  </div>
</div>

<style>
  :global(.cm-editor) {
    height: 100%;
    width: 100%;
  }
  
  :global(.cm-scroller) {
    overflow: auto;
  }

  :global(.cm-content) {
    white-space: pre-wrap;
  }
</style> 