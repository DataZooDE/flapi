<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { page } from '$app/stores';
  import Save from 'lucide-svelte/icons/save';
  import ArrowLeft from 'lucide-svelte/icons/arrow-left';
  import { renderYamlTemplate, renderSqlTemplate, validateYaml, validateSql } from '$lib/utils/templateRenderer';
  import type { EndpointConfig } from '$lib/types';

  export let data: { endpoint: EndpointConfig };
  
  let yamlContent = '';
  let sqlContent = '';
  let yamlError: string | null = null;
  let sqlError: string | null = null;

  onMount(() => {
    try {
      if (data?.endpoint) {
        // Convert endpoint config to YAML
        yamlContent = renderYamlTemplate(JSON.stringify(data.endpoint));
        // Get SQL template if available
        sqlContent = data.endpoint.template || '';
      }
    } catch (error) {
      console.error('Error initializing templates:', error);
      yamlError = error instanceof Error ? error.message : 'Failed to load YAML template';
    }
  });

  function handleSave() {
    try {
      // Validate both templates
      if (!validateYaml(yamlContent)) {
        yamlError = 'Invalid YAML syntax';
        return;
      }
      if (!validateSql(sqlContent)) {
        sqlError = 'Invalid SQL template syntax';
        return;
      }

      // Clear any previous errors
      yamlError = null;
      sqlError = null;

      // Create a sample context for SQL template preview
      const sampleContext = {
        conn: {
          project_id: 'sample-project',
          properties: {
            path: '/sample/path'
          }
        },
        params: {
          id: 123,
          name: 'sample'
        }
      };

      // Render templates and log to console
      console.log('Rendered YAML Template:', yamlContent);
      console.log('Rendered SQL Template:', renderSqlTemplate(sqlContent, sampleContext));

      // TODO: Add actual save functionality here
      
    } catch (error) {
      console.error('Error saving templates:', error);
      if (error instanceof Error) {
        yamlError = error.message;
      }
    }
  }
</script>

<div class="container mx-auto py-6 space-y-6">
  <div class="flex justify-between items-center">
    <div class="flex items-center space-x-4">
      <button class="btn btn-ghost" on:click={() => goto('/endpoints')}>
        <ArrowLeft class="h-4 w-4 mr-2" />
        Back
      </button>
      <h1 class="text-2xl font-bold">Edit Endpoint: {$page.params.path}</h1>
    </div>
    <button class="btn btn-primary flex items-center" on:click={handleSave}>
      <Save class="h-4 w-4 mr-2" />
      <span>Save Changes</span>
    </button>
  </div>

  <div class="grid grid-cols-1 md:grid-cols-2 gap-6">
    <!-- YAML Template Editor -->
    <div class="card">
      <div class="card-header">
        <h2 class="text-xl font-semibold">YAML Configuration</h2>
      </div>
      <div class="p-4">
        {#if yamlError}
          <div class="text-red-600 mb-2">{yamlError}</div>
        {/if}
        <textarea
          class="w-full h-96 font-mono text-sm p-4 border rounded"
          bind:value={yamlContent}
          placeholder="Enter YAML configuration..."
        ></textarea>
      </div>
    </div>

    <!-- SQL Template Editor -->
    <div class="card">
      <div class="card-header">
        <h2 class="text-xl font-semibold">SQL Template</h2>
      </div>
      <div class="p-4">
        {#if sqlError}
          <div class="text-red-600 mb-2">{sqlError}</div>
        {/if}
        <textarea
          class="w-full h-96 font-mono text-sm p-4 border rounded"
          bind:value={sqlContent}
          placeholder="Enter SQL template with Mustache syntax..."
        ></textarea>
      </div>
    </div>
  </div>
</div>

<style>
  .card {
    @apply bg-white shadow rounded-lg overflow-hidden;
  }
  
  .card-header {
    @apply p-4 bg-gray-50 border-b;
  }
  
  textarea {
    @apply bg-white dark:bg-gray-800 resize-none focus:ring-2 focus:ring-blue-500 focus:outline-none;
  }
</style> 