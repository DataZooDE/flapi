<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { cn } from "$lib/utils";
  import { EndpointEditor } from "$lib/components/endpoint-editor";
  import { ConnectionEditor } from "$lib/components/connection-editor";
  import { endpoints } from "$lib/stores/endpoints";
  import { connections } from "$lib/stores/connections";
  import type { EndpointConfig } from "$lib/types/config";

  const sections = [
    { id: 'general', label: 'General Config' },
    { id: 'endpoints', label: 'Endpoints' },
    { id: 'connections', label: 'Connections' }
  ];

  let activeSection = 'general';
  let selectedEndpoint: string | null = null;
  let selectedConnection: string | null = null;

  function handleEndpointUpdate(config: EndpointConfig) {
    if (selectedEndpoint) {
      endpoints.update(config);
    }
  }

  function handleConnectionUpdate(name: string, config: any) {
    connections.update(name, config);
  }
</script>

<div class="grid grid-cols-[220px_1fr] h-full">
  <!-- Left sidebar -->
  <div class="border-r bg-muted/40 p-4 space-y-2">
    {#each sections as section}
      <Button 
        variant={activeSection === section.id ? "secondary" : "ghost"}
        class="w-full justify-start"
        on:click={() => activeSection = section.id}
      >
        {section.label}
      </Button>
    {/each}
  </div>

  <!-- Main content area -->
  <div class="p-4">
    {#if activeSection === 'general'}
      <h2 class="text-lg font-medium mb-4">General Configuration</h2>
      <!-- General config component will go here -->

    {:else if activeSection === 'endpoints'}
      <div class="space-y-4">
        <h2 class="text-lg font-medium">Endpoints</h2>
        <Button variant="outline" on:click={() => selectedEndpoint = 'new'}>
          Create New Endpoint
        </Button>
        
        {#if selectedEndpoint}
          <EndpointEditor 
            config={selectedEndpoint === 'new' ? {} : $endpoints[selectedEndpoint]}
            onUpdate={handleEndpointUpdate}
          />
        {/if}
      </div>

    {:else if activeSection === 'connections'}
      <div class="space-y-4">
        <h2 class="text-lg font-medium">Connections</h2>
        <Button variant="outline" on:click={() => selectedConnection = 'new'}>
          Create New Connection
        </Button>
        
        {#if selectedConnection}
          <ConnectionEditor
            name={selectedConnection === 'new' ? '' : selectedConnection}
            config={selectedConnection === 'new' ? undefined : $connections[selectedConnection]}
            onUpdate={handleConnectionUpdate}
          />
        {/if}
      </div>
    {/if}
  </div>
</div> 