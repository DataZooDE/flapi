<script lang="ts">
  import { 
    NetworkIcon,
    ListIcon,
    LayoutListIcon,
    SettingsIcon,
    DatabaseIcon,
    KeyRoundIcon,
  } from "lucide-svelte";
  import { TreeItem } from "$lib/components/ui/tree";
  import { goto } from '$app/navigation';
  import { 
    projectConfig,
    connections,
    endpoints 
  } from '$lib/state';

  function handleConnectionClick(name: string) {
    goto(`/config/connections/${name}`);
  }
</script>

<div class="w-[220px] fixed top-0 left-0 pt-[60px] h-full overflow-y-auto border-r border-secondary-foreground/10">
  <TreeItem label="General" icon={SettingsIcon} on:click={() => goto('/config')} />
  <TreeItem label="Connections" icon={NetworkIcon} expanded={true}>
    {#each Object.keys(connections) as name}
      <TreeItem label={name} on:click={() => handleConnectionClick(name)} />
    {/each}
  </TreeItem>
  <TreeItem label="Endpoints" icon={ListIcon} expanded={true}>
    {#each Object.keys(endpoints) as name}
      <TreeItem label={name} on:click={() => goto(`/config/endpoints/${name}`)} />
    {/each}
  </TreeItem>
  <TreeItem label="Server" icon={DatabaseIcon} on:click={() => goto('/config/server')} />
  <TreeItem label="DuckDB" icon={LayoutListIcon} on:click={() => goto('/config/duckdb')} />
  <TreeItem label="AWS Secrets" icon={KeyRoundIcon} on:click={() => goto('/config/aws-secrets')} />
</div> 