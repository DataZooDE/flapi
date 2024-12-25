<script lang="ts">
  import type { EndpointConfig } from '$lib/types';
  import { fetchConfig } from '$lib/api';

  export let endpoint: EndpointConfig;

  let availableConnections: string[] = [];

  async function loadConnections() {
    try {
      const config = await fetchConfig();
      availableConnections = Object.keys(config.flapi.connections);
    } catch (error) {
      console.error('Failed to load connections:', error);
    }
  }

  loadConnections();
</script>

<div class="space-y-6">
  <div class="flex justify-between items-center">
    <h2 class="text-lg font-semibold">Connection Selection</h2>
  </div>

  <div class="card p-4 space-y-4">
    <div class="space-y-4">
      <div class="space-y-2">
        <label for="connections" class="label">Selected Connections</label>
        <div class="flex flex-wrap gap-2">
          {#each availableConnections as conn}
            <label class="flex items-center space-x-2 p-2 border rounded-lg hover:bg-gray-50">
              <input
                type="checkbox"
                class="checkbox"
                checked={endpoint.connection.includes(conn)}
                on:change={(e) => {
                  if (e.currentTarget.checked) {
                    endpoint.connection = [...endpoint.connection, conn];
                  } else {
                    endpoint.connection = endpoint.connection.filter(c => c !== conn);
                  }
                }}
              />
              <span>{conn}</span>
            </label>
          {/each}
        </div>
      </div>

      {#if endpoint.connection.length === 0}
        <div class="p-4 bg-yellow-50 text-yellow-700 rounded-lg">
          <p class="text-sm">
            <strong>Warning:</strong> At least one connection must be selected for the endpoint to function.
          </p>
        </div>
      {/if}
    </div>
  </div>
</div>
