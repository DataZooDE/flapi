<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { EndpointEditor } from "$lib/components/endpoint-editor";
  import { endpointsStore } from "$lib/stores/endpoints";
  import type { EndpointConfig } from "$lib/types/config";
  import { get } from 'svelte/store';

  let selectedEndpoint: string | null = null;
  let endpoints: Record<string, EndpointConfig> | null = null;

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

  // Load endpoints on mount
  endpointsStore.subscribe(value => {
    endpoints = value;
  });

  function handleEndpointUpdate(config: EndpointConfig) {
    if (selectedEndpoint) {
      endpointsStore.save(selectedEndpoint, config);
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
      config={selectedEndpoint === 'new' ? defaultConfig : endpoints?.[selectedEndpoint]}
      onUpdate={handleEndpointUpdate}
    />
  {/if}
</div> 