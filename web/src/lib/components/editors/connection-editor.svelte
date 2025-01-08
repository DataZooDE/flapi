<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { Separator } from "$lib/components/ui/separator";
  import { Switch } from "$lib/components/ui/switch";
  import { connectionsStore } from "$lib/stores/connections";
  import type { ConnectionConfig } from "$lib/types";
  import { onMount } from "svelte";
  
  export let connectionName: string;

  let loading = false;
  let error: string | null = null;
  let testingConnection = false;
  let newPropertyKey = '';
  let newPropertyValue = '';

  $: connection = $connectionsStore.data?.[connectionName] || {
    type: '',
    init_sql: '',
    properties: {},
    log_queries: false,
    log_parameters: false
  };

  onMount(async () => {
    if (connectionName) {
      await connectionsStore.load(connectionName);
    }
  });

  async function handleSave() {
    loading = true;
    error = null;
    try {
      await connectionsStore.save(connectionName, connection);
    } catch (err) {
      error = err instanceof Error ? err.message : 'An unknown error occurred';
    } finally {
      loading = false;
    }
  }

  async function testConnection() {
    testingConnection = true;
    error = null;
    try {
      await connectionsStore.testConnection(connectionName);
      // Show success message
    } catch (err) {
      error = err instanceof Error ? err.message : 'Failed to test connection';
    } finally {
      testingConnection = false;
    }
  }

  function addProperty() {
    if (newPropertyKey && newPropertyValue) {
      connection.properties = {
        ...connection.properties,
        [newPropertyKey]: newPropertyValue
      };
      newPropertyKey = '';
      newPropertyValue = '';
    }
  }

  function removeProperty(key: string) {
    const { [key]: _, ...rest } = connection.properties;
    connection.properties = rest;
  }

  // Add type for event handlers
  type SwitchEvent = Event & { currentTarget: HTMLInputElement };

  function handleSwitchChange(field: 'log_queries' | 'log_parameters') {
    return (e: SwitchEvent) => {
      connection[field] = e.currentTarget.checked;
    };
  }
</script>

<div class="container mx-auto p-4 space-y-4">
  <!-- Basic Connection Settings -->
  <Card>
    <CardHeader>
      <CardTitle>Connection Settings</CardTitle>
    </CardHeader>
    <CardContent>
      <form class="space-y-4">
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="connection-type">Connection Type</Label>
            <Input 
              id="connection-type" 
              bind:value={connection.type} 
              placeholder="duckdb"
            />
          </div>

          <div class="space-y-2">
            <Label for="init-sql">Initialization SQL</Label>
            <textarea
              id="init-sql"
              class="w-full min-h-[100px] p-2 rounded-md border"
              bind:value={connection.init_sql}
              placeholder="-- SQL to run when connection is established"
            />
          </div>
        </div>
      </form>
    </CardContent>
  </Card>

  <!-- Connection Properties -->
  <Card>
    <CardHeader>
      <CardTitle>Connection Properties</CardTitle>
    </CardHeader>
    <CardContent>
      <div class="space-y-4">
        <!-- Existing properties -->
        <div class="space-y-2">
          {#if Object.keys(connection.properties).length > 0}
            {#each Object.entries(connection.properties) as [key, value]}
              <div class="flex items-center gap-2">
                <Input readonly value={key} class="flex-1" />
                <Input readonly value={value} class="flex-1" />
                <Button 
                  variant="destructive" 
                  size="icon"
                  on:click={() => removeProperty(key)}
                >
                  ×
                </Button>
              </div>
            {/each}
          {:else}
            <p class="text-sm text-muted-foreground">No properties configured</p>
          {/if}
        </div>

        <!-- Add new property -->
        <div class="flex items-end gap-2">
          <div class="flex-1">
            <Label for="property-key">Key</Label>
            <Input 
              id="property-key" 
              bind:value={newPropertyKey} 
              placeholder="e.g., database"
            />
          </div>
          <div class="flex-1">
            <Label for="property-value">Value</Label>
            <Input 
              id="property-value" 
              bind:value={newPropertyValue} 
              placeholder="e.g., mydb"
            />
          </div>
          <Button 
            type="button"
            on:click={addProperty}
            disabled={!newPropertyKey || !newPropertyValue}
          >
            Add
          </Button>
        </div>
      </div>
    </CardContent>
  </Card>

  <!-- Logging Configuration -->
  <Card>
    <CardHeader>
      <CardTitle>Logging Configuration</CardTitle>
    </CardHeader>
    <CardContent>
      <div class="space-y-4">
        <div class="flex items-center justify-between">
          <Label for="log-queries">Log SQL Queries</Label>
          <Switch 
            id="log-queries"
            checked={connection.log_queries}
            on:change={handleSwitchChange('log_queries')}
          />
        </div>

        <div class="flex items-center justify-between">
          <Label for="log-parameters">Log Query Parameters</Label>
          <Switch 
            id="log-parameters"
            checked={connection.log_parameters}
            on:change={handleSwitchChange('log_parameters')}
          />
        </div>
      </div>
    </CardContent>
  </Card>

  <!-- Action Buttons -->
  <div class="flex justify-end gap-2">
    <Button 
      variant="outline"
      on:click={testConnection}
      disabled={testingConnection || loading}
    >
      {testingConnection ? 'Testing...' : 'Test Connection'}
    </Button>

    <Button 
      on:click={handleSave} 
      disabled={loading || testingConnection}
    >
      {loading ? 'Saving...' : 'Save Changes'}
    </Button>
  </div>

  {#if error}
    <p class="text-red-500 text-sm mt-2">{error}</p>
  {/if}
</div> 