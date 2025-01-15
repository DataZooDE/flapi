<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { Select } from "$lib/components/ui/select";
  import { cn } from "$lib/utils";
  import { Plus, Trash } from "lucide-svelte";

  export let className = "";

  interface ConnectionConfig {
    init?: string;
    properties: Record<string, any>;
    "log-queries"?: boolean;
    "log-parameters"?: boolean;
    allow?: string;
  }

  export let name = "";
  export let config: ConnectionConfig = {
    init: "",
    properties: {},
    "log-queries": false,
    "log-parameters": false,
    allow: "*"
  };
  export let onUpdate: (name: string, config: ConnectionConfig) => void = () => {};

  let activeTab = 0;
  let newPropertyKey = "";
  let newPropertyValue = "";

  function validateConfig(): string[] {
    const errors: string[] = [];
    if (!config.allow) {
      errors.push('Allow field is required');
    }
    return errors;
  }

  function handleChange() {
    const errors = validateConfig();
    validationErrors = errors;
    onUpdate(name, config);
  }

  function addProperty() {
    if (newPropertyKey && newPropertyValue) {
      config.properties = { ...config.properties, [newPropertyKey]: newPropertyValue };
      newPropertyKey = "";
      newPropertyValue = "";
      handleChange();
    }
  }

  function removeProperty(key: string) {
    const { [key]: _, ...rest } = config.properties;
    config.properties = rest;
    handleChange();
  }

  let validationErrors: string[] = [];
</script>

<div class={cn("space-y-6", className)}>
  <div class="space-y-2">
    <Label for="init">Init SQL</Label>
    <Input
      id="init"
      bind:value={config.init}
      on:change={handleChange}
      placeholder="CREATE TABLE IF NOT EXISTS..."
    />
  </div>

  <div class="space-y-2">
    <Label>Properties</Label>
    <div class="flex space-x-2">
      <Input
        placeholder="Property Key"
        bind:value={newPropertyKey}
      />
      <Input
        placeholder="Property Value"
        bind:value={newPropertyValue}
      />
      <Button size="icon" on:click={addProperty}>
        <Plus />
      </Button>
    </div>
    {#if Object.keys(config.properties).length > 0}
      <ul class="mt-2 space-y-1">
        {#each Object.entries(config.properties) as [key, value]}
          <li class="flex items-center justify-between">
            <span class="text-sm">{key}: {value}</span>
            <Button size="icon" variant="ghost" on:click={() => removeProperty(key)}>
              <Trash />
            </Button>
          </li>
        {/each}
      </ul>
    {/if}
  </div>

  <div class="space-y-2">
    <Label for="allow">Allow</Label>
    <Input
      id="allow"
      bind:value={config.allow}
      on:change={handleChange}
      placeholder="*"
    />
    <p class="text-sm text-muted-foreground">
      Specify which tables/views can be accessed through this connection
    </p>
  </div>

  <div class="flex items-center space-x-2">
    <input
      type="checkbox"
      id="log-queries"
      bind:checked={config["log-queries"]}
      on:change={handleChange}
    />
    <Label for="log-queries">Log Queries</Label>
  </div>

  <div class="flex items-center space-x-2">
    <input
      type="checkbox"
      id="log-parameters"
      bind:checked={config["log-parameters"]}
      on:change={handleChange}
    />
    <Label for="log-parameters">Log Parameters</Label>
  </div>

  {#if validationErrors.length > 0}
    <div class="text-destructive text-sm space-y-1">
      {#each validationErrors as error}
        <p>{error}</p>
      {/each}
    </div>
  {/if}
</div> 