<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { EndpointEditor } from "$lib/components/endpoint-editor";
  import { endpoints } from '$lib/state';
  import type { EndpointConfig } from "$lib/types";

  let selectedEndpoint = $state<string | null>(null);

  function handleEndpointUpdate(path: string, config: EndpointConfig) {
    endpoints[path] = config;
  }
</script>

<div class="space-y-4">
  <div class="flex justify-between items-center">
    <h2 class="text-2xl font-bold">Endpoints</h2>
    <Button onclick={() => selectedEndpoint = 'new'}>
      New Endpoint
    </Button>
  </div>

  {#if selectedEndpoint}
    <EndpointEditor
      config={selectedEndpoint === 'new' ? {
        path: '',
        method: 'GET',
        version: 'v1',
        description: '',
        parameters: [],
        query: {
          sql: '',
          parameters: []
        }
      } : endpoints[selectedEndpoint]}
      onUpdate={(config) => handleEndpointUpdate(selectedEndpoint === 'new' ? config.path : selectedEndpoint, config)}
    />
  {/if}
</div> 