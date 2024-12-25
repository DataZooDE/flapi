<script lang="ts">
  import { goto } from '$app/navigation';
  import Save from 'lucide-svelte/icons/save';
  import type { ProjectConfig } from '$lib/types';
  import { saveProjectConfig } from '$lib/api';
  import DuckDBPropertySelect from '$lib/components/DuckDBPropertySelect.svelte';

  export let data: { config: ProjectConfig };
  
  // Initialize DuckDB properties
  let newPropertyKey = '';
  let newPropertyValue = '';
  let duckdbProperties: Record<string, string> = {
    access_mode: data?.config?.duckdb?.access_mode ?? 'READ_WRITE',
    threads: String(data?.config?.duckdb?.threads ?? 8),
    max_memory: data?.config?.duckdb?.max_memory ?? '8GB',
    default_order: data?.config?.duckdb?.default_order ?? 'DESC'
  };

  // Create the config object with computed DuckDB properties
  $: config = {
    project_name: data?.config?.project_name ?? '',
    project_description: data?.config?.project_description ?? '',
    template: {
      path: data?.config?.template?.path ?? './sqls',
      'environment-whitelist': data?.config?.template?.['environment-whitelist'] ?? []
    },
    duckdb: {
      db_path: data?.config?.duckdb?.db_path,
      access_mode: duckdbProperties.access_mode as 'READ_WRITE' | 'READ_ONLY',
      threads: Number(duckdbProperties.threads),
      max_memory: duckdbProperties.max_memory,
      default_order: duckdbProperties.default_order as 'ASC' | 'DESC'
    },
    'enforce-https': {
      enabled: data?.config?.['enforce-https']?.enabled ?? false,
      'ssl-cert-file': data?.config?.['enforce-https']?.['ssl-cert-file'],
      'ssl-key-file': data?.config?.['enforce-https']?.['ssl-key-file']
    },
    heartbeat: {
      enabled: data?.config?.heartbeat?.enabled ?? false,
      'worker-interval': data?.config?.heartbeat?.['worker-interval'] ?? 10
    }
  };

  function addDuckDBProperty() {
    if (newPropertyKey && newPropertyValue) {
      duckdbProperties = {
        ...duckdbProperties,
        [newPropertyKey]: newPropertyValue
      };
      newPropertyKey = '';
      newPropertyValue = '';
    }
  }

  function removeDuckDBProperty(key: string) {
    const { [key]: _, ...rest } = duckdbProperties;
    duckdbProperties = {
      ...rest,
      [key]: getDefaultValue(key) // Always keep a default value
    };
  }

  function getDefaultValue(key: string): string {
    switch (key) {
      case 'access_mode': return 'READ_WRITE';
      case 'threads': return '8';
      case 'max_memory': return '8GB';
      case 'default_order': return 'DESC';
      default: return '';
    }
  }

  async function handleSubmit() {
    try {
      await saveProjectConfig(config);
      goto('/');
    } catch (e) {
      alert('Failed to save project configuration: ' + (e instanceof Error ? e.message : String(e)));
    }
  }
</script>

<div class="container mx-auto py-6 space-y-6">
  <div class="flex justify-between items-center">
    <h1 class="text-2xl font-bold">Project Settings</h1>
    <button 
      type="submit"
      form="project-form"
      class="btn btn-primary flex items-center"
    >
      <Save class="h-4 w-4 mr-2" />
      <span>Save Changes</span>
    </button>
  </div>

  <form 
    id="project-form"
    on:submit|preventDefault={handleSubmit} 
    class="space-y-6"
  >
    <!-- Basic Information -->
    <div class="card p-4">
      <h2 class="text-lg font-semibold mb-4">Basic Information</h2>
      <div class="space-y-4">
        <div class="space-y-2">
          <label for="project_name" class="label">Project Name</label>
          <input
            id="project_name"
            type="text"
            bind:value={config.project_name}
            class="input"
            required
          />
        </div>

        <div class="space-y-2">
          <label for="project_description" class="label">Project Description</label>
          <textarea
            id="project_description"
            bind:value={config.project_description}
            class="textarea"
            rows="3"
          />
        </div>
      </div>
    </div>

    <!-- Template Configuration -->
    <div class="card p-4">
      <h2 class="text-lg font-semibold mb-4">Template Configuration</h2>
      <div class="space-y-4">
        <div class="space-y-2">
          <label for="template_path" class="label">Template Path</label>
          <input
            id="template_path"
            type="text"
            bind:value={config.template.path}
            class="input"
            placeholder="./sqls"
          />
        </div>

        <div class="space-y-2">
          <h3 class="text-sm font-medium text-gray-600">Environment Whitelist</h3>
          <div class="space-y-2">
            {#each config.template['environment-whitelist'] || [] as pattern, i}
              <div class="flex items-center space-x-2">
                <input
                  type="text"
                  id="env_pattern_{i}"
                  aria-label="Environment pattern {i + 1}"
                  bind:value={config.template['environment-whitelist'][i]}
                  class="input flex-1"
                  placeholder="^FLAPI_.*"
                />
                <button
                  type="button"
                  class="btn btn-ghost text-red-600"
                  on:click={() => {
                    config.template['environment-whitelist'].splice(i, 1);
                    config.template['environment-whitelist'] = [...config.template['environment-whitelist']];
                  }}
                >
                  Remove
                </button>
              </div>
            {/each}
            <button
              type="button"
              class="btn btn-secondary"
              on:click={() => {
                config.template['environment-whitelist'] = [...(config.template['environment-whitelist'] || []), ''];
              }}
            >
              Add Pattern
            </button>
          </div>
        </div>
      </div>
    </div>

    <!-- DuckDB Configuration -->
    <div class="card p-4">
      <h2 class="text-lg font-semibold mb-4">DuckDB Configuration</h2>
      <div class="space-y-4">
        <div class="space-y-2">
          <label for="db_path" class="label">Database Path</label>
          <input
            id="db_path"
            type="text"
            bind:value={config.duckdb.db_path}
            class="input"
            placeholder="./flapi_cache.db"
          />
          <p class="text-sm text-gray-600">Leave empty for in-memory database</p>
        </div>

        <!-- Current Properties -->
        <div class="space-y-2">
          <h3 class="text-sm font-medium text-gray-600">Current Properties</h3>
          <div class="space-y-2">
            {#each Object.entries(duckdbProperties) as [key, value]}
              <div class="flex items-center space-x-2 p-2 bg-secondary rounded">
                <div class="flex-1">
                  <span class="font-mono text-sm">{key}:</span> 
                  <span class="font-medium">{value}</span>
                </div>
                <button
                  type="button"
                  class="btn btn-sm btn-ghost text-red-600"
                  on:click={() => removeDuckDBProperty(key)}
                >
                  Remove
                </button>
              </div>
            {/each}
          </div>
        </div>

        <!-- Add New Property -->
        <div class="space-y-4 border-t pt-4">
          <h3 class="text-sm font-medium text-gray-600">Add New Property</h3>
          <div class="grid grid-cols-2 gap-4">
            <div class="space-y-2">
              <label for="property_key" class="label">Property Key</label>
              <DuckDBPropertySelect
                bind:value={newPropertyKey}
                on:select={({ detail }) => {
                  // Set default value when property is selected
                  newPropertyValue = detail.option.defaultValue;
                }}
              />
            </div>

            <div class="space-y-2">
              <label for="property_value" class="label">Property Value</label>
              <div class="flex space-x-2">
                {#if newPropertyKey === 'access_mode'}
                  <select
                    id="property_value"
                    bind:value={newPropertyValue}
                    class="select flex-1"
                  >
                    <option value="READ_WRITE">READ_WRITE</option>
                    <option value="READ_ONLY">READ_ONLY</option>
                  </select>
                {:else if newPropertyKey === 'default_order' || newPropertyKey === 'default_null_order' || newPropertyKey === 'null_order'}
                  <select
                    id="property_value"
                    bind:value={newPropertyValue}
                    class="select flex-1"
                  >
                    <option value="ASC">ASC</option>
                    <option value="DESC">DESC</option>
                  </select>
                {:else if newPropertyKey === 'threads' || newPropertyKey === 'external_threads' || newPropertyKey === 'max_expression_depth' || newPropertyKey === 'max_line_length' || newPropertyKey === 'max_string_length' || newPropertyKey === 'ordered_aggregate_threshold' || newPropertyKey === 'perfect_ht_threshold' || newPropertyKey === 'checkpoint_threshold'}
                  <input
                    id="property_value"
                    type="number"
                    bind:value={newPropertyValue}
                    class="input flex-1"
                    min="1"
                    placeholder="8"
                  />
                {:else if newPropertyKey === 'enable_external_access' || newPropertyKey === 'enable_fsst_vectors' || newPropertyKey === 'enable_object_cache' || newPropertyKey === 'enable_profiling' || newPropertyKey === 'enable_progress_bar' || newPropertyKey === 'preserve_identifier_case' || newPropertyKey === 'preserve_insertion_order' || newPropertyKey === 'enable_optimizer' || newPropertyKey === 'force_compression' || newPropertyKey === 'force_index_join' || newPropertyKey === 'lock_configuration' || newPropertyKey === 'prefer_range_joins'}
                  <select
                    id="property_value"
                    bind:value={newPropertyValue}
                    class="select flex-1"
                  >
                    <option value="true">true</option>
                    <option value="false">false</option>
                  </select>
                {:else if newPropertyKey === 'profiling_mode'}
                  <select
                    id="property_value"
                    bind:value={newPropertyValue}
                    class="select flex-1"
                  >
                    <option value="detailed">detailed</option>
                    <option value="standard">standard</option>
                  </select>
                {:else if newPropertyKey === 'window_mode'}
                  <select
                    id="property_value"
                    bind:value={newPropertyValue}
                    class="select flex-1"
                  >
                    <option value="window">window</option>
                    <option value="combine">combine</option>
                  </select>
                {:else}
                  <input
                    id="property_value"
                    type="text"
                    bind:value={newPropertyValue}
                    class="input flex-1"
                    placeholder={newPropertyKey === 'max_memory' ? '8GB' : ''}
                  />
                {/if}
                <button
                  type="button"
                  class="btn btn-secondary"
                  disabled={!newPropertyKey || !newPropertyValue}
                  on:click={addDuckDBProperty}
                >
                  Add
                </button>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- HTTPS Configuration -->
    <div class="card p-4">
      <h2 class="text-lg font-semibold mb-4">HTTPS Configuration</h2>
      <div class="space-y-4">
        <div class="flex items-center space-x-2">
          <input
            type="checkbox"
            id="enable_https"
            bind:checked={config['enforce-https'].enabled}
            class="checkbox"
          />
          <label for="enable_https" class="cursor-pointer">Enable HTTPS</label>
        </div>

        {#if config['enforce-https'].enabled}
          <div class="space-y-2">
            <label for="ssl_cert" class="label">SSL Certificate File</label>
            <input
              id="ssl_cert"
              type="text"
              bind:value={config['enforce-https']['ssl-cert-file']}
              class="input"
              placeholder="./ssl/cert.pem"
            />
          </div>

          <div class="space-y-2">
            <label for="ssl_key" class="label">SSL Key File</label>
            <input
              id="ssl_key"
              type="text"
              bind:value={config['enforce-https']['ssl-key-file']}
              class="input"
              placeholder="./ssl/key.pem"
            />
          </div>
        {/if}
      </div>
    </div>

    <!-- Heartbeat Configuration -->
    <div class="card p-4">
      <h2 class="text-lg font-semibold mb-4">Heartbeat Configuration</h2>
      <div class="space-y-4">
        <div class="flex items-center space-x-2">
          <input
            type="checkbox"
            id="enable_heartbeat"
            bind:checked={config.heartbeat.enabled}
            class="checkbox"
          />
          <label for="enable_heartbeat" class="cursor-pointer">Enable Heartbeat</label>
        </div>

        {#if config.heartbeat.enabled}
          <div class="space-y-2">
            <label for="worker_interval" class="label">Worker Interval (seconds)</label>
            <input
              id="worker_interval"
              type="number"
              bind:value={config.heartbeat['worker-interval']}
              class="input"
              min="1"
            />
          </div>
        {/if}
      </div>
    </div>
  </form>
</div> 