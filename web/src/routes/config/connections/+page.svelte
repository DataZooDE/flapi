<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { ConnectionEditor } from "$lib/components/connection-editor";
  import { connections } from '$lib/state';
  import type { ConnectionConfig } from "$lib/types";

  let selectedConnection = $state<string | null>(null);

  function handleConnectionUpdate(name: string, config: ConnectionConfig) {
    connections[name] = config;
  }
</script>

<div class="space-y-4">
  <div class="flex justify-between items-center">
    <h2 class="text-2xl font-bold">Connections</h2>
    <Button onclick={() => selectedConnection = 'new'}>
      New Connection
    </Button>
  </div>

  {#if selectedConnection}
    <ConnectionEditor
      name={selectedConnection === 'new' ? '' : selectedConnection}
      config={selectedConnection === 'new' ? undefined : connections[selectedConnection]}
      onUpdate={handleConnectionUpdate}
    />
  {/if}
</div> 