<script lang="ts">
  import type { EndpointConfig } from '$lib/types';
  import { PlusIcon, PencilIcon, TrashIcon, PlayIcon } from 'lucide-svelte';
  import { goto } from '$app/navigation';
  import { deleteEndpoint, testEndpoint } from '$lib/api';

  export let data;

  let endpoints: EndpointConfig[] = data.endpoints;
  let searchQuery = '';

  $: filteredEndpoints = endpoints.filter(endpoint => 
    endpoint.urlPath.toLowerCase().includes(searchQuery.toLowerCase()) ||
    endpoint.method.toLowerCase().includes(searchQuery.toLowerCase())
  );

  async function handleDelete(endpoint: EndpointConfig) {
    if (confirm(`Are you sure you want to delete the endpoint ${endpoint.urlPath}?`)) {
      await deleteEndpoint(endpoint.urlPath);
      endpoints = endpoints.filter(e => e.urlPath !== endpoint.urlPath);
    }
  }

  async function handleTest(endpoint: EndpointConfig) {
    try {
      const result = await testEndpoint(endpoint, {});
      alert('Test successful! Response: ' + JSON.stringify(result, null, 2));
    } catch (error) {
      alert('Test failed: ' + error.message);
    }
  }
</script>

<div class="space-y-4">
  <div class="flex justify-between items-center">
    <h1 class="text-2xl font-bold">API Endpoints</h1>
    <button
      class="btn variant-filled-primary flex items-center space-x-2"
      on:click={() => goto('/endpoints/new')}
    >
      <PlusIcon class="w-4 h-4" />
      <span>New Endpoint</span>
    </button>
  </div>

  <div class="flex items-center space-x-4">
    <input
      type="text"
      placeholder="Search endpoints..."
      bind:value={searchQuery}
      class="input"
    />
  </div>

  <div class="card p-4">
    <table class="table table-hover">
      <thead>
        <tr>
          <th>Method</th>
          <th>Path</th>
          <th>Authentication</th>
          <th>Cache</th>
          <th>Rate Limit</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        {#each filteredEndpoints as endpoint}
          <tr>
            <td>
              <span class="badge variant-filled">{endpoint.method}</span>
            </td>
            <td>{endpoint.urlPath}</td>
            <td>
              {#if endpoint.auth.enabled}
                <span class="badge variant-filled-success">Enabled</span>
              {:else}
                <span class="badge variant-filled-error">Disabled</span>
              {/if}
            </td>
            <td>
              {#if endpoint.cache.refreshEndpoint}
                <span class="badge variant-filled-success">
                  {endpoint.cache.refreshTime}
                </span>
              {:else}
                <span class="badge variant-filled-error">Disabled</span>
              {/if}
            </td>
            <td>
              {#if endpoint.rate_limit.enabled}
                <span class="badge variant-filled-success">
                  {endpoint.rate_limit.max}/{endpoint.rate_limit.interval}s
                </span>
              {:else}
                <span class="badge variant-filled-error">Disabled</span>
              {/if}
            </td>
            <td>
              <div class="flex items-center space-x-2">
                <button
                  class="btn btn-sm variant-soft"
                  on:click={() => goto(`/endpoints/${encodeURIComponent(endpoint.urlPath)}`)}
                >
                  <PencilIcon class="w-4 h-4" />
                </button>
                <button
                  class="btn btn-sm variant-soft-success"
                  on:click={() => handleTest(endpoint)}
                >
                  <PlayIcon class="w-4 h-4" />
                </button>
                <button
                  class="btn btn-sm variant-soft-error"
                  on:click={() => handleDelete(endpoint)}
                >
                  <TrashIcon class="w-4 h-4" />
                </button>
              </div>
            </td>
          </tr>
        {/each}
      </tbody>
    </table>
  </div>
</div> 