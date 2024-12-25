<script lang="ts">
  import { goto } from '$app/navigation';
  import Save from 'lucide-svelte/icons/save';
  import X from 'lucide-svelte/icons/x';
  import type { ProjectConfig } from '$lib/types';
  import { saveProjectConfig } from '$lib/api';

  export let data: { config: ProjectConfig };
  let config = { ...data.config };

  async function handleSubmit() {
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
          <label class="label">Environment Whitelist</label>
          <div class="space-y-2">
            {#each config.template['environment-whitelist'] || [] as pattern, i}
              <div class="flex items-center space-x-2">
                <input
                  type="text"
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
