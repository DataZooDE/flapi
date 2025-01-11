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

    if (!name.trim()) {
      errors.push("Connection name is required");
    }

    if (!/^[a-zA-Z][a-zA-Z0-9_]*$/.test(name)) {
      errors.push("Connection name must start with a letter and contain only letters, numbers, and underscores");
    }

    return errors;
  }

  let validationErrors: string[] = [];

  function validateAndUpdate() {
    validationErrors = validateConfig();
    if (validationErrors.length === 0) {
      onUpdate(name, config);
    }
  }

  function addProperty() {
    if (newPropertyKey && newPropertyValue) {
      config.properties = {
        ...config.properties,
        [newPropertyKey]: newPropertyValue
      };
      newPropertyKey = "";
      newPropertyValue = "";
      validateAndUpdate();
    }
  }

  function removeProperty(key: string) {
    const { [key]: _, ...rest } = config.properties;
    config.properties = rest;
    validateAndUpdate();
  }

  function handleChange() {
    validateAndUpdate();
  }
</script>

<div class={cn("space-y-6", className)}>
  <div class="flex justify-between items-center">
    <h3 class="text-lg font-medium">Connection Configuration</h3>
  </div>

  <div class="space-y-4">
    <!-- Basic Configuration -->
    <div class="space-y-4">
      <div class="space-y-2">
        <Label for="connection-name">Connection Name</Label>
        <Input
          id="connection-name"
          bind:value={name}
          on:change={handleChange}
          placeholder="e.g. main_db"
        />
      </div>

      <div class="space-y-2">
        <Label for="connection-init">Initialization SQL</Label>
        <textarea
          id="connection-init"
          bind:value={config.init}
          on:change={handleChange}
          class="w-full h-32 px-3 py-2 border rounded-md font-mono"
          placeholder="-- SQL to run when connection is established"
        />
      </div>
    </div>

    <!-- Properties -->
    <div class="space-y-4">
      <div class="flex justify-between items-center">
        <h4 class="font-medium">Connection Properties</h4>
      </div>

      <div class="grid grid-cols-[1fr_1fr_auto] gap-2 items-end">
        <div class="space-y-2">
          <Label for="new-property-key">Property Name</Label>
          <Input
            id="new-property-key"
            bind:value={newPropertyKey}
            placeholder="e.g. host"
          />
        </div>
        <div class="space-y-2">
          <Label for="new-property-value">Value</Label>
          <Input
            id="new-property-value"
            bind:value={newPropertyValue}
            placeholder="e.g. localhost"
          />
        </div>
        <Button variant="outline" on:click={addProperty}>
          <Plus class="w-4 h-4 mr-2" />
          Add
        </Button>
      </div>

      {#if Object.keys(config.properties).length === 0}
        <div class="text-center py-8 text-muted-foreground">
          No properties defined. Add properties above.
        </div>
      {:else}
        <div class="space-y-2">
          {#each Object.entries(config.properties) as [key, value]}
            <div class="flex justify-between items-center p-2 border rounded-md">
              <div>
                <span class="font-medium">{key}</span>
                <span class="text-muted-foreground">: {value}</span>
              </div>
              <Button
                variant="ghost"
                size="icon"
                class="text-destructive hover:text-destructive"
                on:click={() => removeProperty(key)}
                title="Remove property"
                aria-label="Remove property"
              >
                <Trash class="w-4 h-4" />
              </Button>
            </div>
          {/each}
        </div>
      {/if}
    </div>

    <!-- Advanced Settings -->
    <div class="space-y-4">
      <div class="space-y-2">
        <Label for="connection-allow">Allow Pattern</Label>
        <Input
          id="connection-allow"
          bind:value={config.allow}
          on:change={handleChange}
          placeholder="e.g. * or specific.table.*"
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
    </div>
  </div>

  {#if validationErrors.length > 0}
    <div class="text-destructive text-sm space-y-1">
      {#each validationErrors as error}
        <p>{error}</p>
      {/each}
    </div>
  {/if}
</div> 