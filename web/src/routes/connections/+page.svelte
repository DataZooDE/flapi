<script lang="ts">
  import { goto } from '$app/navigation';
  import { Network } from 'lucide-svelte';
  import Plus from 'lucide-svelte/icons/plus';
  import type { ConnectionConfig } from '$lib/types';
  import { onMount } from 'svelte';

  export let data: { connections: Record<string, ConnectionConfig> };
  let connections: [string, ConnectionConfig][] = [];
  let error: string | null = null;

  onMount(() => {
    try {
      if (!data?.connections) {
        throw new Error('No connections data available');
      }
      connections = Object.entries(data.connections);
    } catch (e) {
      console.error('Failed to load connections:', e);
      error = e instanceof Error ? e.message : 'An unknown error occurred';
    }
  });

  function handleRowClick(name: string) {
    goto(`/connections/${encodeURIComponent(name)}`);
  }
</script>

<div class="container mx-auto py-6 space-y-6">
  <div class="flex justify-between items-center">
    <h1 class="text-2xl font-bold">Connections</h1>
    <a href="/connections/new" class="btn btn-primary flex items-center">
      <Plus class="h-4 w-4 mr-2" />
      <span>New Connection</span>
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
            <th class="text-left p-4">Init</th>
            <th class="text-left p-4">Logging</th>
            <th class="text-left p-4">Allow</th>
          </tr>
        </thead>
        <tbody>
          {#each connections as [name, connection]}
            <tr 
              class="border-t hover:bg-gray-50 cursor-pointer"
              on:click={() => handleRowClick(name)}
            >
              <td class="p-4">{name}</td>
              <td class="p-4">
                <span class="px-2 py-1 rounded text-sm bg-gray-100">
                  {connection.init}
                </span>
              </td>
              <td class="p-4">
                <div class="space-x-2">
                  {#if connection.log_queries}
                    <span class="px-2 py-1 rounded text-sm bg-green-100 text-green-800">
                      Queries
                    </span>
                  {/if}
                  {#if connection.log_parameters}
                    <span class="px-2 py-1 rounded text-sm bg-green-100 text-green-800">
                      Parameters
                    </span>
                  {/if}
                </div>
              </td>
              <td class="p-4">
                <span class="px-2 py-1 rounded text-sm bg-gray-100">
                  {connection.allow}
                </span>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    {/if}
  </div>
</div> 