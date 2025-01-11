<script lang="ts">
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { endpointsStore } from "$lib/stores/endpoints";
  import { onMount } from "svelte";

  let loading = true;

  onMount(async () => {
    await endpointsStore.loadAll();
    loading = false;
  });
</script>

<div class="p-6">
  <Card>
    <CardHeader>
      <CardTitle>Endpoints</CardTitle>
    </CardHeader>
    <CardContent>
      {#if loading}
        <p>Loading...</p>
      {:else}
        <p class="leading-7">
          Endpoint management coming soon...
        </p>
      {/if}
    </CardContent>
  </Card>
</div> 