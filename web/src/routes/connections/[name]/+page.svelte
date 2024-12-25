<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import { goto } from '$app/navigation';
  import { EditorState } from "@codemirror/state";
  import { EditorView, keymap } from '@codemirror/view';
  import { sql } from '@codemirror/lang-sql';
  import { oneDark } from '@codemirror/theme-one-dark';
  import { defaultKeymap } from '@codemirror/commands';
  import { syntaxHighlighting } from '@codemirror/language';
  import type { ConnectionConfig } from '$lib/types';
  import { saveConnection, validateSql, deleteConnection } from '$lib/api';
  import X from 'lucide-svelte/icons/x';
  import Save from 'lucide-svelte/icons/save';
  import Trash2 from 'lucide-svelte/icons/trash-2';

  export let data: { connection?: ConnectionConfig & { name: string }; isNew: boolean };

  let connection: ConnectionConfig & { name: string } = data.isNew
    ? {
        name: '',
        init: '',
        properties: {},
        log_queries: false,
        log_parameters: false,
        allow: ''
      }
    : { ...data.connection! };

  let editor: any = null;
  let editorContainer: HTMLElement;
  let newPropertyKey = '';
  let newPropertyValue = '';

  onMount(() => {
    if (editorContainer) {
      try {
        const state = EditorState.create({
          doc: connection.init,
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
            EditorView.updateListener.of((update: { docChanged: boolean; state: { doc: { toString(): string } } }) => {
              if (update.docChanged && editor) {
                connection.init = update.state.doc.toString();
              }
            })
          ]
        });

        editor = new EditorView({
          state,
          parent: editorContainer
        });
      } catch (e) {
        console.error('Failed to initialize editor:', e);
      }
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

  async function handleSubmit() {
    try {
      // Update init SQL from editor
      if (editor) {
        connection.init = editor.state.doc.toString();
      }

      // Validate init SQL
      const sqlValidation = await validateSql(connection.init);
      if (!sqlValidation.valid) {
        alert('Invalid initialization SQL: ' + sqlValidation.error);
        return;
      }

      // Save connection
      await saveConnection(connection.name, connection);
      goto('/connections');
    } catch (error) {
      alert('Failed to save connection: ' + (error instanceof Error ? error.message : String(error)));
    }
  }

  function addProperty() {
    if (newPropertyKey && newPropertyValue) {
      connection.properties = {
        ...connection.properties,
        [newPropertyKey]: newPropertyValue
      };
      newPropertyKey = '';
      newPropertyValue = '';
    }
  }

  function removeProperty(key: string) {
    const { [key]: _, ...rest } = connection.properties;
    connection.properties = rest;
  }

  async function handleDelete() {
    if (!confirm('Are you sure you want to delete this connection?')) return;
    try {
      await deleteConnection(connection.name);
      goto('/connections');
    } catch (error) {
      alert('Failed to delete connection: ' + (error instanceof Error ? error.message : String(error)));
    }
  }
</script>

<div class="container mx-auto py-6 space-y-6">
  <div class="flex justify-between items-center">
    <h1 class="text-2xl font-bold">{data.isNew ? 'New Connection' : 'Edit Connection'}</h1>
    <div class="flex space-x-2">
      <button 
        type="button"
        class="btn btn-ghost flex items-center" 
        on:click={() => goto('/connections')}
      >
        <X class="h-4 w-4 mr-2" />
        <span>Cancel</span>
      </button>
      {#if !data.isNew}
        <button 
          type="button"
          class="btn btn-error flex items-center" 
          on:click={handleDelete}
        >
          <Trash2 class="h-4 w-4 mr-2" />
          <span>Delete</span>
        </button>
      {/if}
      <button 
        type="submit"
        form="connection-form"
        class="btn btn-primary flex items-center"
      >
        <Save class="h-4 w-4 mr-2" />
        <span>Save Changes</span>
      </button>
    </div>
  </div>

  <form 
    id="connection-form"
    on:submit|preventDefault={handleSubmit} 
    class="space-y-6"
  >
    <!-- Basic Information -->
    <div class="card p-4">
      <h2 class="text-lg font-semibold mb-4">Basic Information</h2>
      
      <div class="space-y-4">
        <div class="space-y-2">
          <label for="name" class="label">Connection Name</label>
          <input
            id="name"
            type="text"
            bind:value={connection.name}
            class="input"
            required
            disabled={!data.isNew}
          />
        </div>

        <div class="space-y-2">
          <label for="allow" class="label">Allow Pattern</label>
          <input
            id="allow"
            type="text"
            bind:value={connection.allow}
            class="input"
            placeholder="e.g., *.parquet"
          />
        </div>

        <div class="flex items-center space-x-4">
          <label class="flex items-center space-x-2">
            <input
              type="checkbox"
              bind:checked={connection.log_queries}
              class="checkbox"
              id="log_queries"
            />
            <span>Log Queries</span>
          </label>

          <label class="flex items-center space-x-2">
            <input
              type="checkbox"
              bind:checked={connection.log_parameters}
              class="checkbox"
              id="log_parameters"
            />
            <span>Log Parameters</span>
          </label>
        </div>
      </div>
    </div>

    <!-- Properties -->
    <div class="card p-4 space-y-4">
      <h2 class="text-xl font-semibold">Connection Properties</h2>
      
      <div class="space-y-4">
        <div class="grid grid-cols-2 gap-4">
          <div class="space-y-2">
            <label for="property_key" class="label">Property Key</label>
            <input
              id="property_key"
              type="text"
              bind:value={newPropertyKey}
              class="input"
              placeholder="e.g., db_file"
            />
          </div>

          <div class="space-y-2">
            <label for="property_value" class="label">Property Value</label>
            <div class="flex space-x-2">
              <input
                id="property_value"
                type="text"
                bind:value={newPropertyValue}
                class="input flex-1"
                placeholder="e.g., ./data/db.duckdb"
              />
              <button
                type="button"
                class="btn variant-filled-primary"
                on:click={addProperty}
              >
                Add
              </button>
            </div>
          </div>
        </div>

        <div class="space-y-2">
          {#each Object.entries(connection.properties) as [key, value]}
            <div class="flex items-center space-x-2 p-2 bg-secondary rounded">
              <div class="flex-1">
                <span class="font-semibold">{key}:</span> {value}
              </div>
              <button
                type="button"
                class="btn btn-sm variant-soft-error"
                on:click={() => removeProperty(key)}
              >
                Remove
              </button>
            </div>
          {/each}
        </div>
      </div>
    </div>

    <!-- Initialization SQL -->
    <div class="card p-4 space-y-4">
      <h2 class="text-xl font-semibold">Initialization SQL</h2>
      <div bind:this={editorContainer} class="h-64 border rounded" />
    </div>
  </form>
</div>

<style>
  :global(.cm-editor) {
    height: 100%;
  }

  :global(.cm-editor .cm-scroller) {
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
    font-size: 14px;
    line-height: 1.5;
  }
</style> 