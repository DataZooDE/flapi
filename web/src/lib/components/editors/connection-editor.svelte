<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { Separator } from "$lib/components/ui/separator";
  import { Switch } from "$lib/components/ui/switch";
  import { connections } from '$lib/state';
  import { Plus, Trash } from "lucide-svelte";
  import type { ConnectionConfig } from "$lib/types";

  export let name: string;

  let loading = $state(false);
  let error = $state<string | null>(null);
  let testingConnection = $state(false);
  let newPropertyKey = $state('');
  let newPropertyValue = $state('');

  $: connection = connections[name] || {
    type: '',
    init_sql: '',
    properties: {},
    log_queries: false,
    log_parameters: false
  };

  async function handleSave() {
    loading = true;
    error = null;
    try {
      // Save logic here
      connections[name] = connection;
      loading = false;
    } catch (err) {
      error = err instanceof Error ? err.message : 'An unknown error occurred';
      loading = false;
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
</script>

<div class="space-y-4">
  <Card>
    <CardHeader>
      <CardTitle>Connection: {name}</CardTitle>
    </CardHeader>
    <CardContent class="space-y-4">
      <!-- Connection Type -->
      <div class="space-y-2">
        <Label for="type">Type</Label>
        <Input 
          id="type"
          bind:value={connection.type}
          placeholder="postgres"
        />
      </div>

      <!-- Init SQL -->
      <div class="space-y-2">
        <Label for="init-sql">Init SQL</Label>
        <textarea
          id="init-sql"
          class="w-full min-h-[100px] p-2 rounded-md border"
          bind:value={connection.init_sql}
          placeholder="SELECT 1"
        ></textarea>
      </div>

      <!-- Properties -->
      <div class="space-y-2">
        <Label>Properties</Label>
        <div class="flex space-x-2">
          <Input
            placeholder="Key"
            bind:value={newPropertyKey}
          />
          <Input
            placeholder="Value"
            bind:value={newPropertyValue}
          />
          <Button size="icon" onclick={addProperty}>
            <Plus />
          </Button>
        </div>

        {#if Object.keys(connection.properties).length > 0}
          <div class="mt-2 space-y-2">
            {#each Object.entries(connection.properties) as [key, value]}
              <div class="flex items-center justify-between">
                <span class="text-sm">{key}: {value}</span>
                <Button 
                  size="icon"
                  variant="ghost"
                  onclick={() => removeProperty(key)}
                >
                  <Trash />
                </Button>
              </div>
            {/each}
          </div>
        {/if}
      </div>

      <Separator />

      <!-- Logging Options -->
      <div class="space-y-4">
        <div class="flex items-center justify-between">
          <Label for="log-queries">Log Queries</Label>
          <Switch
            id="log-queries"
            checked={connection.log_queries}
            onchange={(e) => connection.log_queries = e.detail}
          />
        </div>

        <div class="flex items-center justify-between">
          <Label for="log-parameters">Log Parameters</Label>
          <Switch
            id="log-parameters"
            checked={connection.log_parameters}
            onchange={(e) => connection.log_parameters = e.detail}
          />
        </div>
      </div>

      <!-- Actions -->
      <div class="flex justify-end space-x-2">
        <Button
          variant="outline"
          onclick={() => testingConnection = true}
          disabled={loading || testingConnection}
        >
          {testingConnection ? 'Testing...' : 'Test Connection'}
        </Button>

        <Button
          onclick={handleSave}
          disabled={loading || testingConnection}
        >
          {loading ? 'Saving...' : 'Save Changes'}
        </Button>
      </div>

      {#if error}
        <p class="text-destructive text-sm">{error}</p>
      {/if}
    </CardContent>
  </Card>
</div> 