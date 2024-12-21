<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { EditorView, keymap } from '@codemirror/view';
  import { sql } from '@codemirror/lang-sql';
  import { oneDark } from '@codemirror/theme-one-dark';
  import { defaultKeymap } from '@codemirror/commands';
  import { syntaxHighlighting } from '@codemirror/language';
  import type { EndpointConfig } from '$lib/types';
  import { saveEndpoint, validateSql } from '$lib/api';

  export let data: { endpoint?: EndpointConfig; isNew: boolean };

  let endpoint: EndpointConfig = data.isNew
    ? {
        urlPath: '',
        method: 'GET',
        requestFields: [],
        templateSource: '',
        connection: [],
        rate_limit: { enabled: false, max: 100, interval: 60 },
        auth: { enabled: false, type: 'basic', users: [] },
        cache: {
          cacheTableName: '',
          cacheSource: '',
          refreshTime: '',
          refreshEndpoint: false,
          maxPreviousTables: 5
        },
        heartbeat: { enabled: false, params: {} }
      }
    : { ...data.endpoint! };

  let editor: EditorView;
  let editorContainer: HTMLElement;

  onMount(() => {
    if (editorContainer) {
      editor = new EditorView({
        doc: endpoint.templateSource,
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
      // Update SQL template from editor
      endpoint.templateSource = editor.state.doc.toString();

      // Validate SQL
      const sqlValidation = await validateSql(endpoint.templateSource);
      if (!sqlValidation.valid) {
        alert('Invalid SQL: ' + sqlValidation.error);
        return;
      }

      // Save endpoint
      await saveEndpoint(endpoint);
      goto('/');
    } catch (error) {
      alert('Failed to save endpoint: ' + (error instanceof Error ? error.message : String(error)));
    }
  }

  function addRequestField() {
    endpoint.requestFields = [
      ...endpoint.requestFields,
      {
        fieldName: '',
        fieldIn: 'query',
        description: '',
        required: false,
        validators: []
      }
    ];
  }

  function removeRequestField(index: number) {
    endpoint.requestFields = endpoint.requestFields.filter((_, i) => i !== index);
  }

  function addValidator(fieldIndex: number) {
    endpoint.requestFields[fieldIndex].validators = [
      ...endpoint.requestFields[fieldIndex].validators,
      { type: 'string', preventSqlInjection: true }
    ];
  }

  function removeValidator(fieldIndex: number, validatorIndex: number) {
    endpoint.requestFields[fieldIndex].validators = endpoint.requestFields[
      fieldIndex
    ].validators.filter((_, i) => i !== validatorIndex);
  }
</script>

<div class="space-y-8">
  <div class="flex justify-between items-center">
    <h1 class="text-2xl font-bold">
      {data.isNew ? 'New Endpoint' : 'Edit Endpoint'}
    </h1>
  </div>

  <form on:submit|preventDefault={handleSubmit} class="space-y-6">
    <!-- Basic Information -->
    <div class="card p-4 space-y-4">
      <h2 class="text-xl font-semibold">Basic Information</h2>
      
      <div class="grid grid-cols-2 gap-4">
        <div class="space-y-2">
          <label for="urlPath" class="label">URL Path</label>
          <input
            id="urlPath"
            type="text"
            bind:value={endpoint.urlPath}
            class="input"
            required
          />
        </div>

        <div class="space-y-2">
          <label for="method" class="label">HTTP Method</label>
          <select id="method" bind:value={endpoint.method} class="select">
            <option value="GET">GET</option>
            <option value="POST">POST</option>
            <option value="PUT">PUT</option>
            <option value="DELETE">DELETE</option>
          </select>
        </div>
      </div>
    </div>

    <!-- Request Fields -->
    <div class="card p-4 space-y-4">
      <div class="flex justify-between items-center">
        <h2 class="text-xl font-semibold">Request Fields</h2>
        <button
          type="button"
          class="btn variant-soft-primary"
          on:click={addRequestField}
        >
          Add Field
        </button>
      </div>

      {#each endpoint.requestFields as field, fieldIndex}
        <div class="card variant-soft p-4 space-y-4">
          <div class="grid grid-cols-2 gap-4">
            <div class="space-y-2">
              <label for="field_name_{fieldIndex}" class="label">Field Name</label>
              <input
                id="field_name_{fieldIndex}"
                type="text"
                bind:value={field.fieldName}
                class="input"
                required
              />
            </div>

            <div class="space-y-2">
              <label for="field_in_{fieldIndex}" class="label">Field Location</label>
              <select id="field_in_{fieldIndex}" bind:value={field.fieldIn} class="select">
                <option value="query">Query</option>
                <option value="path">Path</option>
                <option value="header">Header</option>
                <option value="body">Body</option>
              </select>
            </div>

            <div class="space-y-2">
              <label for="field_desc_{fieldIndex}" class="label">Description</label>
              <input
                id="field_desc_{fieldIndex}"
                type="text"
                bind:value={field.description}
                class="input"
              />
            </div>

            <div class="flex items-center space-x-4">
              <label class="flex items-center space-x-2">
                <input
                  type="checkbox"
                  bind:checked={field.required}
                  class="checkbox"
                  id="field_required_{fieldIndex}"
                />
                <span>Required</span>
              </label>

              <button
                type="button"
                class="btn variant-soft-error"
                on:click={() => removeRequestField(fieldIndex)}
              >
                Remove Field
              </button>
            </div>
          </div>

          <!-- Validators -->
          <div class="space-y-4">
            <div class="flex justify-between items-center">
              <h3 class="text-lg font-semibold">Validators</h3>
              <button
                type="button"
                class="btn variant-soft-primary"
                on:click={() => addValidator(fieldIndex)}
              >
                Add Validator
              </button>
            </div>

            {#each field.validators as validator, validatorIndex}
              <div class="card variant-ghost p-4 grid grid-cols-2 gap-4">
                <div class="space-y-2">
                  <label for="validator_type_{fieldIndex}_{validatorIndex}" class="label">Type</label>
                  <select id="validator_type_{fieldIndex}_{validatorIndex}" bind:value={validator.type} class="select">
                    <option value="string">String</option>
                    <option value="int">Integer</option>
                    <option value="enum">Enum</option>
                    <option value="date">Date</option>
                    <option value="time">Time</option>
                  </select>
                </div>

                {#if validator.type === 'string'}
                  <div class="space-y-2">
                    <label for="validator_regex_{fieldIndex}_{validatorIndex}" class="label">Regex Pattern</label>
                    <input
                      id="validator_regex_{fieldIndex}_{validatorIndex}"
                      type="text"
                      bind:value={validator.regex}
                      class="input"
                    />
                  </div>
                {:else if validator.type === 'int'}
                  <div class="grid grid-cols-2 gap-4">
                    <div class="space-y-2">
                      <label for="validator_min_{fieldIndex}_{validatorIndex}" class="label">Min</label>
                      <input
                        id="validator_min_{fieldIndex}_{validatorIndex}"
                        type="number"
                        bind:value={validator.min}
                        class="input"
                      />
                    </div>
                    <div class="space-y-2">
                      <label for="validator_max_{fieldIndex}_{validatorIndex}" class="label">Max</label>
                      <input
                        id="validator_max_{fieldIndex}_{validatorIndex}"
                        type="number"
                        bind:value={validator.max}
                        class="input"
                      />
                    </div>
                  </div>
                {/if}

                <div class="col-span-2 flex justify-between items-center">
                  <label class="flex items-center space-x-2">
                    <input
                      type="checkbox"
                      bind:checked={validator.preventSqlInjection}
                      class="checkbox"
                      id="validator_sql_{fieldIndex}_{validatorIndex}"
                    />
                    <span>Prevent SQL Injection</span>
                  </label>

                  <button
                    type="button"
                    class="btn variant-soft-error"
                    on:click={() => removeValidator(fieldIndex, validatorIndex)}
                  >
                    Remove Validator
                  </button>
                </div>
              </div>
            {/each}
          </div>
        </div>
      {/each}
    </div>

    <!-- SQL Template -->
    <div class="card p-4 space-y-4">
      <h2 class="text-xl font-semibold">SQL Template</h2>
      <div bind:this={editorContainer} class="h-64 border rounded" />
    </div>

    <!-- Authentication -->
    <div class="card p-4 space-y-4">
      <h2 class="text-xl font-semibold">Authentication</h2>
      
      <div class="space-y-4">
        <label class="flex items-center space-x-2">
          <input
            type="checkbox"
            bind:checked={endpoint.auth.enabled}
            class="checkbox"
            id="auth_enabled"
          />
          <span>Enable Authentication</span>
        </label>

        {#if endpoint.auth.enabled}
          <div class="grid grid-cols-2 gap-4">
            <div class="space-y-2">
              <label for="auth_type" class="label">Auth Type</label>
              <select id="auth_type" bind:value={endpoint.auth.type} class="select">
                <option value="basic">Basic Auth</option>
                <option value="bearer">Bearer Token</option>
              </select>
            </div>
          </div>
        {/if}
      </div>
    </div>

    <!-- Rate Limiting -->
    <div class="card p-4 space-y-4">
      <h2 class="text-xl font-semibold">Rate Limiting</h2>
      
      <div class="space-y-4">
        <label class="flex items-center space-x-2">
          <input
            type="checkbox"
            bind:checked={endpoint.rate_limit.enabled}
            class="checkbox"
            id="rate_limit_enabled"
          />
          <span>Enable Rate Limiting</span>
        </label>

        {#if endpoint.rate_limit.enabled}
          <div class="grid grid-cols-2 gap-4">
            <div class="space-y-2">
              <label for="rate_limit_max" class="label">Max Requests</label>
              <input
                id="rate_limit_max"
                type="number"
                bind:value={endpoint.rate_limit.max}
                class="input"
                min="1"
              />
            </div>

            <div class="space-y-2">
              <label for="rate_limit_interval" class="label">Interval (seconds)</label>
              <input
                id="rate_limit_interval"
                type="number"
                bind:value={endpoint.rate_limit.interval}
                class="input"
                min="1"
              />
            </div>
          </div>
        {/if}
      </div>
    </div>

    <!-- Caching -->
    <div class="card p-4 space-y-4">
      <h2 class="text-xl font-semibold">Caching</h2>
      
      <div class="space-y-4">
        <label class="flex items-center space-x-2">
          <input
            type="checkbox"
            bind:checked={endpoint.cache.refreshEndpoint}
            class="checkbox"
            id="cache_enabled"
          />
          <span>Enable Caching</span>
        </label>

        {#if endpoint.cache.refreshEndpoint}
          <div class="grid grid-cols-2 gap-4">
            <div class="space-y-2">
              <label for="cache_table" class="label">Cache Table Name</label>
              <input
                id="cache_table"
                type="text"
                bind:value={endpoint.cache.cacheTableName}
                class="input"
                required
              />
            </div>

            <div class="space-y-2">
              <label for="cache_refresh" class="label">Refresh Time</label>
              <input
                id="cache_refresh"
                type="text"
                bind:value={endpoint.cache.refreshTime}
                class="input"
                placeholder="e.g., 1h, 30m"
                required
              />
            </div>
          </div>
        {/if}
      </div>
    </div>

    <div class="flex justify-end space-x-4">
      <button
        type="button"
        class="btn variant-soft"
        on:click={() => goto('/')}
      >
        Cancel
      </button>
      <button type="submit" class="btn variant-filled-primary">
        {data.isNew ? 'Create Endpoint' : 'Save Changes'}
      </button>
    </div>
  </form>
</div> 