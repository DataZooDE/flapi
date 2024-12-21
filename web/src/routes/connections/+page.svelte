<script lang="ts">
  import type { ConnectionConfig } from '$lib/types';
  import { PlusIcon, PencilIcon, TrashIcon, PlayIcon } from 'lucide-svelte';
  import { goto } from '$app/navigation';
  import { deleteConnection, testConnection } from '$lib/api';

  export let data;

  let connections: (ConnectionConfig & { name: string })[] = data.connections;
  let searchQuery = '';

  $: filteredConnections = connections.filter(connection => 
    connection.name.toLowerCase().includes(searchQuery.toLowerCase())
  );

  async function handleDelete(connection: ConnectionConfig & { name: string }) {
    if (confirm(`Are you sure you want to delete the connection ${connection.name}?`)) {
      await deleteConnection(connection.name);
      connections = connections.filter(c => c.name !== connection.name);
    }
  }

  async function handleTest(connection: ConnectionConfig & { name: string }) {
    try {
      await testConnection(connection.name);
      alert('Connection test successful!');
    } catch (error) {
      alert('Connection test failed: ' + error.message);
    }
  }
</script>

<div class="space-y-4">
  <div class="flex justify-between items-center">
    <h1 class="text-2xl font-bold">Database Connections</h1>
    <button
      class="btn variant-filled-primary flex items-center space-x-2"
      on:click={() => goto('/connections/new')}
    >
      <PlusIcon class="w-4 h-4" />
      <span>New Connection</span>
    </button>
  </div>

  <div class="flex items-center space-x-4">
    <input
      type="text"
      placeholder="Search connections..."
      bind:value={searchQuery}
      class="input"
    />
  </div>

  <div class="card p-4">
    <table class="table table-hover">
      <thead>
        <tr>
          <th>Name</th>
          <th>Properties</th>
          <th>Logging</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody>
        {#each filteredConnections as connection}
          <tr>
            <td>{connection.name}</td>
            <td>
              <div class="space-y-1">
                {#each Object.entries(connection.properties) as [key, value]}
                  <div class="text-sm">
                    <span class="font-semibold">{key}:</span> {value}
                  </div>
                {/each}
              </div>
            </td>
            <td>
              <div class="space-y-1">
                <div class="flex items-center space-x-2">
                  <span class="badge variant-filled-{connection.log_queries ? 'success' : 'error'}">
                    Query Logging
                  </span>
                </div>
                <div class="flex items-center space-x-2">
                  <span class="badge variant-filled-{connection.log_parameters ? 'success' : 'error'}">
                    Parameter Logging
                  </span>
                </div>
              </div>
            </td>
            <td>
              <div class="flex items-center space-x-2">
                <button
                  class="btn btn-sm variant-soft"
                  on:click={() => goto(`/connections/${encodeURIComponent(connection.name)}`)}
                >
                  <PencilIcon class="w-4 h-4" />
                </button>
                <button
                  class="btn btn-sm variant-soft-success"
                  on:click={() => handleTest(connection)}
                >
                  <PlayIcon class="w-4 h-4" />
                </button>
                <button
                  class="btn btn-sm variant-soft-error"
                  on:click={() => handleDelete(connection)}
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