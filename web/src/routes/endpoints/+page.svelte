<script lang="ts">
  import { goto } from '$app/navigation';
  import Terminal from 'lucide-svelte/icons/terminal';
  import Plus from 'lucide-svelte/icons/plus';
  import type { EndpointConfig } from '$lib/types';
  import { onMount } from 'svelte';

  export let data: { endpoints: Record<string, EndpointConfig> };
  let endpoints: [string, EndpointConfig][] = [];
  let error: string | null = null;

  onMount(() => {
    try {
      if (!data?.endpoints) {
        throw new Error('No endpoints data available');
      }
      endpoints = Object.entries(data.endpoints);
    } catch (e) {
      console.error('Failed to load endpoints:', e);
      error = e instanceof Error ? e.message : 'An unknown error occurred';
    }
  });

  function handleRowClick(path: string) {
    goto(`/endpoints/${encodeURIComponent(path)}`);
  }

  function formatRateLimit(rateLimit: { enabled: boolean; max: number; interval: number }) {
    if (!rateLimit?.enabled) return 'disabled';
    return `${rateLimit.max}/${rateLimit.interval}s`;
  }

  function formatCache(cache: { refreshEndpoint: boolean; refreshTime: string }) {
    if (!cache?.refreshEndpoint) return 'disabled';
    return cache.refreshTime || '1h';
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
    {#if error}
      <div class="p-4 text-red-800 bg-red-100 rounded">
        {error}
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
          {#each endpoints as [path, endpoint]}
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
