<script lang="ts">
  import { goto } from '$app/navigation';
  import Save from 'lucide-svelte/icons/save';
  import X from 'lucide-svelte/icons/x';
  import type { ProjectConfig } from '$lib/types';
  import { saveProjectConfig } from '$lib/api';

  export let data: { config: ProjectConfig };
  let config = { ...data.config };
  let errors: Record<string, string> = {};

  function validateConfig(): boolean {
    errors = {};
    
    // Project name validation
    if (!config.project_name?.trim()) {
      errors.project_name = 'Project name is required';
    }

    // Template validation
    if (!config.template?.path?.trim()) {
      errors.template_path = 'Template path is required';
    }

    // Environment whitelist validation
    if (config.template['environment-whitelist']) {
      config.template['environment-whitelist'].forEach((pattern, index) => {
        if (!pattern.trim()) {
          errors[`env_pattern_${index}`] = 'Pattern cannot be empty';
          return;
        }
        if (!/^[A-Z_][A-Z0-9_]*$/.test(pattern)) {
          errors[`env_pattern_${index}`] = 'Pattern must match ^[A-Z_][A-Z0-9_]*$';
        }
      });
    }

    // DuckDB validation
    if (config.duckdb) {
      if (config.duckdb.threads !== undefined && config.duckdb.threads < 1) {
        errors.duckdb_threads = 'Threads must be at least 1';
      }
      if (config.duckdb.max_memory && !/^[0-9]+[KMGT]B$/.test(config.duckdb.max_memory)) {
        errors.duckdb_memory = 'Invalid memory format (e.g., 8GB)';
      }
      if (config.duckdb.access_mode && !['READ_WRITE', 'READ_ONLY'].includes(config.duckdb.access_mode)) {
        errors.duckdb_access_mode = 'Invalid access mode';
      }
      if (config.duckdb.default_order && !['ASC', 'DESC'].includes(config.duckdb.default_order)) {
        errors.duckdb_default_order = 'Invalid default order';
      }
    }

    // Connections validation
    if (config.connections) {
      Object.entries(config.connections).forEach(([name, conn]) => {
        if (!conn.allow?.trim()) {
          errors[`connection_${name}_allow`] = 'Access control must be specified';
        }
      });
    }

    // HTTPS validation
    if (config['enforce-https']?.enabled) {
      if (!config['enforce-https']['ssl-cert-file']?.trim()) {
        errors.ssl_cert = 'SSL certificate file is required when HTTPS is enabled';
      }
      if (!config['enforce-https']['ssl-key-file']?.trim()) {
        errors.ssl_key = 'SSL key file is required when HTTPS is enabled';
      }
    }

    // Heartbeat validation
    if (config.heartbeat?.enabled) {
      if (!config.heartbeat['worker-interval'] || config.heartbeat['worker-interval'] < 1) {
        errors.heartbeat_interval = 'Worker interval must be at least 1 second';
      }
    }

    return Object.keys(errors).length === 0;
  }

  async function handleSubmit() {
    if (!validateConfig()) {
      alert('Please fix the validation errors before saving');
      return;
    }

    try {
      await saveProjectConfig(config);
      goto('/');
    } catch (error) {
      alert('Failed to save project configuration: ' + (error instanceof Error ? error.message : String(error)));
    }
  }
</script>

<div class="container mx-auto py-6 space-y-6">
  <div class="flex justify-between items-center">
    <h1 class="text-2xl font-bold">Project Settings</h1>
    <div class="flex space-x-2">
      <button 
        type="button"
        class="btn btn-ghost flex items-center" 
        on:click={() => goto('/')}
      >
        <X class="h-4 w-4 mr-2" />
        <span>Cancel</span>
      </button>
      <button 
        type="submit"
        form="project-form"
        class="btn btn-primary flex items-center"
      >
        <Save class="h-4 w-4 mr-2" />
        <span>Save Changes</span>
      </button>
    </div>
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
            class="input {errors.project_name ? 'border-red-500' : ''}"
            required
          />
          {#if errors.project_name}
            <p class="text-red-500 text-sm">{errors.project_name}</p>
          {/if}
        </div>

        <div class="space-y-2">
          <label for="project_description" class="label">Project Description</label>
          <textarea
            id="project_description"
            bind:value={config.project_description}
            class="textarea {errors.project_description ? 'border-red-500' : ''}"
            rows="3"
          />
          {#if errors.project_description}
            <p class="text-red-500 text-sm">{errors.project_description}</p>
          {/if}
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
            class="input {errors.template_path ? 'border-red-500' : ''}"
            placeholder="./sqls"
          />
          {#if errors.template_path}
            <p class="text-red-500 text-sm">{errors.template_path}</p>
          {/if}
        </div>

        <div class="space-y-2">
          <label class="label">Environment Whitelist</label>
          <div class="space-y-2">
            {#each config.template['environment-whitelist'] || [] as pattern, i}
              <div class="flex items-center space-x-2">
                <input
                  type="text"
                  bind:value={config.template['environment-whitelist'][i]}
                  class="input flex-1 {errors[`env_pattern_${i}`] ? 'border-red-500' : ''}"
                  placeholder="^FLAPI_.*"
                />
                {#if errors[`env_pattern_${i}`]}
                  <p class="text-red-500 text-sm">{errors[`env_pattern_${i}`]}</p>
                {/if}
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

        <div class="grid grid-cols-2 gap-4">
          <div class="space-y-2">
            <label for="access_mode" class="label">Access Mode</label>
            <select
              id="access_mode"
              bind:value={config.duckdb.access_mode}
              class="select"
            >
              <option value="READ_WRITE">READ_WRITE</option>
              <option value="READ_ONLY">READ_ONLY</option>
            </select>
          </div>

          <div class="space-y-2">
            <label for="threads" class="label">Threads</label>
            <input
              id="threads"
              type="number"
              bind:value={config.duckdb.threads}
              class="input"
              min="1"
            />
          </div>

          <div class="space-y-2">
            <label for="max_memory" class="label">Max Memory</label>
            <input
              id="max_memory"
              type="text"
              bind:value={config.duckdb.max_memory}
              class="input"
              placeholder="8GB"
            />
          </div>

          <div class="space-y-2">
            <label for="default_order" class="label">Default Order</label>
            <select
              id="default_order"
              bind:value={config.duckdb.default_order}
              class="select"
            >
              <option value="ASC">ASC</option>
              <option value="DESC">DESC</option>
            </select>
          </div>
        </div>
      </div>
    </div>

    <!-- Connections Configuration -->
    <div class="card p-4">
      <h2 class="text-lg font-semibold mb-4">Connections</h2>
      <div class="space-y-4">
        {#each Object.entries(config.connections || {}) as [name, connection]}
          <div class="border p-4 rounded-lg">
            <div class="flex justify-between items-center mb-4">
              <h3 class="font-medium">{name}</h3>
              <button 
                type="button" 
                class="btn btn-ghost text-red-600"
                on:click={() => {
                  delete config.connections[name];
                  config.connections = {...config.connections};
                }}
              >
                Remove
              </button>
            </div>

            <div class="space-y-4">
              <div class="space-y-2">
                <label class="label">Initialization SQL</label>
                <textarea
                  bind:value={connection.init}
                  class="textarea"
                  rows="3"
                  placeholder="INSTALL 'extension';"
                />
              </div>

              <div class="space-y-2">
                <label class="label">Properties</label>
                {#each Object.entries(connection.properties || {}) as [key, value]}
                  <div class="flex space-x-2">
                    <input
                      type="text"
                      bind:value={key}
                      class="input flex-1"
                      placeholder="Key"
                    />
                    <input
                      type="text"
                      bind:value={value}
                      class="input flex-1"
                      placeholder="Value"
                    />
                    <button
                      type="button"
                      class="btn btn-ghost text-red-600"
                      on:click={() => {
                        delete connection.properties[key];
                        connection.properties = {...connection.properties};
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
                    connection.properties = {
                      ...connection.properties,
                      '': ''
                    };
                  }}
                >
                  Add Property
                </button>
              </div>

              <!-- Add missing fields -->
              <div class="space-y-2">
                <label class="flex items-center space-x-2">
                  <input
                    type="checkbox"
                    bind:checked={connection.log_queries}
                    class="checkbox"
                  />
                  <span>Log Queries</span>
                </label>
              </div>

              <div class="space-y-2">
                <label class="flex items-center space-x-2">
                  <input
                    type="checkbox"
                    bind:checked={connection.log_parameters}
                    class="checkbox"
                  />
                  <span>Log Parameters</span>
                </label>
              </div>

              <div class="space-y-2">
                <label class="label">Access Control</label>
                <input
                  type="text"
                  bind:value={connection.allow}
                  class="input"
                  placeholder="* for all users, or specific roles"
                />
              </div>
            </div>
          </div>
        {/each}
        <button
          type="button"
          class="btn btn-primary"
          on:click={() => {
            const name = prompt('Enter connection name:');
            if (name) {
              config.connections = {
                ...config.connections,
                [name]: {
                  init: '',
                  properties: {},
                  log_queries: false,
                  log_parameters: false,
                  allow: '*'
                }
              };
            }
          }}
        >
          Add Connection
        </button>
      </div>
    </div>

    <!-- HTTPS Configuration -->
    <div class="card p-4">
      <h2 class="text-lg font-semibold mb-4">HTTPS Configuration</h2>
      <div class="space-y-4">
        <label class="flex items-center space-x-2">
          <input
            type="checkbox"
            bind:checked={config['enforce-https'].enabled}
            class="checkbox"
          />
          <span>Enable HTTPS</span>
        </label>

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
        <label class="flex items-center space-x-2">
          <input
            type="checkbox"
            bind:checked={config.heartbeat.enabled}
            class="checkbox"
          />
          <span>Enable Heartbeat</span>
        </label>

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
