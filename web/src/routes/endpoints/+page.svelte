<script lang="ts">
  import { goto } from '$app/navigation';
  import { Network } from 'lucide-svelte';
  import Plus from 'lucide-svelte/icons/plus';
  import type { EndpointConfig } from '$lib/types';

  export let data: { endpoints: Record<string, EndpointConfig> };
  let loading = true;
  let error: string | null = null;

  $: {
    if (data) {
      loading = false;
    }
  }
</script>

<div class="container mx-auto py-6 space-y-6">
  <div class="flex justify-between items-center">
    <h1 class="text-2xl font-bold">Endpoints</h1>
    <a href="/endpoints/new" class="btn btn-primary flex items-center">
      <Plus class="h-4 w-4 mr-2" />
      <span>New Endpoint</span>
    </a>
  </div>

  <div class="card">
    {#if loading}
      <div class="p-4 text-center">
        <div class="loading loading-spinner loading-lg"></div>
      </div>
    {:else if error}
      <div class="p-4 text-red-800 bg-red-100 rounded">
        <p class="font-medium">Error loading endpoints</p>
        <p class="text-sm">{error}</p>
        <button 
          class="btn btn-sm mt-2"
          on:click={() => window.location.reload()}
        >
          Retry
        </button>
      </div>
    {:else}
      <table class="min-w-full">
        <thead>
          <tr>
            <th class="text-left p-4">Name</th>
            <th class="text-left p-4">Method</th>
            <th class="text-left p-4">Authentication</th>
            <th class="text-left p-4">Cache</th>
            <th class="text-left p-4">Rate Limit</th>
            <th class="text-left p-4">Status</th>
          </tr>
        </thead>
        <tbody>
          {#each data.endpoints as [path, endpoint]}
            <tr 
              class="border-t hover:bg-gray-50 cursor-pointer"
              on:click={() => handleRowClick(path)}
            >
              <td class="p-4">{path}</td>
              <td class="p-4">
                <span class="px-2 py-1 rounded text-sm {endpoint.method === 'GET' ? 'bg-green-100 text-green-800' : 'bg-blue-100 text-blue-800'}">
                  {endpoint.method}
                </span>
              </td>
              <td class="p-4">
                <span class="px-2 py-1 rounded text-sm bg-gray-100">
                  {endpoint.auth.type}
                </span>
              </td>
              <td class="p-4">
                <span class="px-2 py-1 rounded text-sm {endpoint.cache.refreshEndpoint ? 'bg-green-100 text-green-800' : 'bg-red-100 text-red-800'}">
                  {formatCache(endpoint.cache)}
                </span>
              </td>
              <td class="p-4">
                <span class="px-2 py-1 rounded text-sm {endpoint.rate_limit.enabled ? 'bg-green-100 text-green-800' : 'bg-red-100 text-red-800'}">
                  {formatRateLimit(endpoint.rate_limit)}
                </span>
              </td>
              <td class="p-4">
                <span class="px-2 py-1 rounded text-sm bg-green-100 text-green-800">
                  active
                </span>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    {/if}
  </div>
</div>
