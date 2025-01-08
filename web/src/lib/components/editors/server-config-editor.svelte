<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { serverStore } from "$lib/stores/server";
  import { onMount } from "svelte";
  
  let loading = false;
  let error: string | null = null;

  $: config = $serverStore.data || {
    name: '',
    http_port: 8080,
    cache_schema: 'cache'
  };

  onMount(async () => {
    await serverStore.load();
  });

  async function handleSave() {
    loading = true;
    error = null;
    try {
      await serverStore.save(config);
    } catch (err) {
      error = err instanceof Error ? err.message : 'An unknown error occurred';
    } finally {
      loading = false;
    }
  }

  function validatePort(event: Event) {
    const input = event.target as HTMLInputElement;
    const value = parseInt(input.value);
    if (isNaN(value) || value < 1 || value > 65535) {
      input.setCustomValidity('Port must be between 1 and 65535');
    } else {
      input.setCustomValidity('');
    }
  }
</script>

<div class="container mx-auto p-4 space-y-4">
  <Card>
    <CardHeader>
      <CardTitle>Server Configuration</CardTitle>
    </CardHeader>
    <CardContent>
      <form class="space-y-6">
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="server-name">Server Name</Label>
            <Input 
              id="server-name" 
              bind:value={config.name} 
              placeholder="flapi-server"
            />
            <p class="text-sm text-muted-foreground">
              A unique name for this server instance
            </p>
          </div>

          <div class="space-y-2">
            <Label for="http-port">HTTP Port</Label>
            <Input 
              id="http-port" 
              type="number"
              min="1"
              max="65535"
              bind:value={config.http_port} 
              placeholder="8080"
              on:input={validatePort}
            />
            <p class="text-sm text-muted-foreground">
              The port number to listen on (1-65535)
            </p>
          </div>

          <div class="space-y-2">
            <Label for="cache-schema">Cache Schema</Label>
            <Input 
              id="cache-schema" 
              bind:value={config.cache_schema} 
              placeholder="cache"
            />
            <p class="text-sm text-muted-foreground">
              Schema name for storing cached query results
            </p>
          </div>
        </div>

        <!-- Save Button -->
        <div class="flex justify-end">
          <Button 
            type="button"
            on:click={handleSave}
            disabled={loading}
          >
            {loading ? 'Saving...' : 'Save Changes'}
          </Button>
        </div>

        {#if error}
          <p class="text-red-500 text-sm mt-2">{error}</p>
        {/if}
      </form>
    </CardContent>
  </Card>
</div> 