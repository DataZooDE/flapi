<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { ConnectionEditor } from "$lib/components/connection-editor";
  import { connectionsStore } from "$lib/stores/connections";
  import type { ConnectionConfig } from "$lib/types";
  import { get } from 'svelte/store';

  let selectedConnection: string | null = null;
  let connections: Record<string, ConnectionConfig> | null = null;

  // Load connections on mount
  connectionsStore.subscribe(value => {
    connections = value;
  });

  function handleConnectionUpdate(name: string, config: ConnectionConfig) {
    connectionsStore.save(name, config);
  }
</script>

<div class="container mx-auto p-6">
  <div class="flex justify-between items-center mb-6">
    <h1 class="text-2xl font-bold">Connections</h1>
    <Button variant="outline" on:click={() => selectedConnection = 'new'}>
      Create New Connection
    </Button>
  </div>

  {#if selectedConnection}
    <ConnectionEditor
      name={selectedConnection === 'new' ? '' : selectedConnection}
      config={selectedConnection === 'new' ? undefined : connections?.[selectedConnection]}
      onUpdate={handleConnectionUpdate}
    />
  {/if}
</div> 