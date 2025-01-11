<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { EndpointEditor } from "$lib/components/endpoint-editor";
  import { endpoints } from "$lib/stores/endpoints";
  import type { EndpointConfig } from "$lib/types/config";

  let selectedEndpoint: string | null = null;

  const defaultConfig: EndpointConfig = {
    path: '',
    method: 'GET',
    version: 'v1',
    description: '',
    parameters: [],
    query: {
      sql: '',
      parameters: []
    }
  };

  function handleEndpointUpdate(config: EndpointConfig) {
    if (selectedEndpoint) {
      endpoints.update(config);
    }
  }
</script>

<div class="container mx-auto p-6">
  <div class="flex justify-between items-center mb-6">
    <h1 class="text-2xl font-bold">Endpoints</h1>
    <Button variant="outline" on:click={() => selectedEndpoint = 'new'}>
      Create New Endpoint
    </Button>
  </div>

  {#if selectedEndpoint}
    <EndpointEditor 
      config={selectedEndpoint === 'new' ? defaultConfig : $endpoints[selectedEndpoint]}
      onUpdate={handleEndpointUpdate}
    />
  {/if}
</div> 