<script lang="ts">
  import { 
    Network, 
    Database, 
    Plus, 
    Folder 
  } from "lucide-svelte";
  import TreeItem from "$lib/components/ui/tree/tree-item.svelte";
  import { Button } from "$lib/components/ui/button";
  import { navigate } from "$lib/router";
  import { endpoints } from "$lib/stores/endpoints";
  import { connections } from "$lib/stores/connections";

  export let selectedEndpoint: string | null = null;
  export let selectedConnection: string | null = null;

  let endpointsExpanded = true;
  let connectionsExpanded = true;

  // Initialize empty stores if they don't exist
  $: endpointsList = $endpoints || {};
  $: connectionsList = $connections || {};

  function handleEndpointSelect(path: string) {
    selectedEndpoint = path;
    navigate(`/config/endpoints?path=${encodeURIComponent(path)}`);
  }

  function handleConnectionSelect(name: string) {
    selectedConnection = name;
    navigate(`/config/connections?name=${encodeURIComponent(name)}`);
  }

  function handleNewEndpoint() {
    selectedEndpoint = 'new';
    navigate('/config/endpoints?new=true');
  }

  function handleNewConnection() {
    selectedConnection = 'new';
    navigate('/config/connections?new=true');
  }
</script>

<div class="space-y-1">
  <!-- Endpoints Section -->
  <TreeItem
    label="Endpoints"
    icon={Folder}
    hasChildren={true}
    bind:expanded={endpointsExpanded}
  >
    {#if Object.keys(endpointsList).length > 0}
      {#each Object.entries(endpointsList) as [path, config]}
        <TreeItem
          label={path}
          icon={Network}
          level={1}
          selected={selectedEndpoint === path}
          on:click={() => handleEndpointSelect(path)}
        />
      {/each}
    {/if}
    <div class="pl-4 pr-2 py-1">
      <Button 
        variant="ghost" 
        size="sm"
        class="w-full justify-start"
        on:click={handleNewEndpoint}
      >
        <Plus class="w-4 h-4 mr-2" />
        New Endpoint
      </Button>
    </div>
  </TreeItem>

  <!-- Connections Section -->
  <TreeItem
    label="Connections"
    icon={Folder}
    hasChildren={true}
    bind:expanded={connectionsExpanded}
  >
    {#if Object.keys(connectionsList).length > 0}
      {#each Object.entries(connectionsList) as [name, config]}
        <TreeItem
          label={name}
          icon={Database}
          level={1}
          selected={selectedConnection === name}
          on:click={() => handleConnectionSelect(name)}
        />
      {/each}
    {/if}
    <div class="pl-4 pr-2 py-1">
      <Button 
        variant="ghost" 
        size="sm"
        class="w-full justify-start"
        on:click={handleNewConnection}
      >
        <Plus class="w-4 h-4 mr-2" />
        New Connection
      </Button>
    </div>
  </TreeItem>
</div> 