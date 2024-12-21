<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { EditorView, keymap } from '@codemirror/view';
  import { sql } from '@codemirror/lang-sql';
  import { oneDark } from '@codemirror/theme-one-dark';
  import { defaultKeymap } from '@codemirror/commands';
  import { syntaxHighlighting } from '@codemirror/language';
  import type { ConnectionConfig } from '$lib/types';
  import { saveConnection, validateSql } from '$lib/api';

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

  let editor: EditorView;
  let editorContainer: HTMLElement;
  let newPropertyKey = '';
  let newPropertyValue = '';

  onMount(() => {
    if (editorContainer) {
      editor = new EditorView({
        doc: connection.init,
        extensions: [
          keymap.of(defaultKeymap),
          sql(),
          oneDark,
          syntaxHighlighting
        ],
        parent: editorContainer
      });
    }
  });

  async function handleSubmit() {
    try {
      // Update init SQL from editor
      connection.init = editor.state.doc.toString();

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
</script>

<div class="space-y-8">
  <div class="flex justify-between items-center">
    <h1 class="text-2xl font-bold">
      {data.isNew ? 'New Connection' : 'Edit Connection'}
    </h1>
  </div>

  <form on:submit|preventDefault={handleSubmit} class="space-y-6">
    <!-- Basic Information -->
    <div class="card p-4 space-y-4">
      <h2 class="text-xl font-semibold">Basic Information</h2>
      
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

    <div class="flex justify-end space-x-4">
      <button
        type="button"
        class="btn variant-soft"
        on:click={() => goto('/connections')}
      >
        Cancel
      </button>
      <button type="submit" class="btn variant-filled-primary">
        {data.isNew ? 'Create Connection' : 'Save Changes'}
      </button>
    </div>
  </form>
</div> 