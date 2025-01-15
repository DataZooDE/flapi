<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { serverConfig } from '$lib/state';

  let loading = $state(false);
  let error = $state<string | null>(null);

  function validatePort(event: Event) {
    const input = event.target as HTMLInputElement;
    const value = input.value;
    if (value && !/^\d+$/.test(value)) {
      input.value = value.replace(/\D/g, '');
    }
  }

  async function handleSave() {
    loading = true;
    error = null;
    try {
      // Save logic here using API
      loading = false;
    } catch (err) {
      error = err instanceof Error ? err.message : 'An unknown error occurred';
      loading = false;
    }
  }
</script>

<div class="space-y-4">
  <Card>
    <CardHeader>
      <CardTitle>Server Configuration</CardTitle>
    </CardHeader>
    <CardContent class="space-y-4">
      <div class="space-y-2">
        <Label for="name">Server Name</Label>
        <Input 
          id="name"
          bind:value={serverConfig.name}
          placeholder="My API Server"
        />
        <p class="text-sm text-muted-foreground">
          A friendly name for this server instance
        </p>
      </div>

      <div class="space-y-2">
        <Label for="host">Host</Label>
        <Input 
          id="host"
          bind:value={serverConfig.host}
          placeholder="localhost"
        />
        <p class="text-sm text-muted-foreground">
          The host address to bind to
        </p>
      </div>

      <div class="space-y-2">
        <Label for="port">Port</Label>
        <Input 
          id="port"
          type="number"
          bind:value={serverConfig.port}
          on:input={validatePort}
          placeholder="8080"
        />
        <p class="text-sm text-muted-foreground">
          The port number to listen on
        </p>
      </div>

      <div class="flex justify-end">
        <Button 
          onclick={handleSave}
          disabled={loading}
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