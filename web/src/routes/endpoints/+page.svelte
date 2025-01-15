<script lang="ts">
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { endpoints } from "$lib/state";
  import { Button } from "$lib/components/ui/button";
  import { goto } from '$app/navigation';

  function createNewEndpoint() {
    goto('/config/endpoints/new');
  }
</script>

<div class="p-6">
  <Card>
    <CardHeader class="flex justify-between items-center">
      <CardTitle>Endpoints</CardTitle>
      <Button onclick={createNewEndpoint}>New Endpoint</Button>
    </CardHeader>
    <CardContent>
      {#if Object.keys(endpoints).length === 0}
        <p class="text-muted-foreground">No endpoints configured yet.</p>
      {:else}
        <div class="space-y-4">
          {#each Object.entries(endpoints) as [name, endpoint]}
            <div class="flex justify-between items-center p-4 border rounded-lg">
              <div>
                <h3 class="font-medium">{name}</h3>
                <p class="text-sm text-muted-foreground">
                  {endpoint.method} {endpoint.path}
                </p>
              </div>
              <Button 
                variant="outline"
                onclick={() => goto(`/config/endpoints/${name}`)}
              >
                Edit
              </Button>
            </div>
          {/each}
        </div>
      {/if}
    </CardContent>
  </Card>
</div> 