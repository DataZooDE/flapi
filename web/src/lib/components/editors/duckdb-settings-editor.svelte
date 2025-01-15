<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { duckdbConfig } from '$lib/state';
  import { Plus, Trash } from "lucide-svelte";

  let loading = $state(false);
  let error = $state<string | null>(null);
  let newOptionKey = $state('');
  let newOptionValue = $state('');

  function addOption() {
    if (newOptionKey && newOptionValue) {
      duckdbConfig.options = {
        ...duckdbConfig.options,
        [newOptionKey]: newOptionValue
      };
      newOptionKey = '';
      newOptionValue = '';
    }
  }

  function removeOption(key: string) {
    const { [key]: _, ...rest } = duckdbConfig.options;
    duckdbConfig.options = rest;
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
      <CardTitle>DuckDB Settings</CardTitle>
    </CardHeader>
    <CardContent class="space-y-4">
      <div class="space-y-2">
        <Label for="database">Database Path</Label>
        <Input 
          id="database"
          bind:value={duckdbConfig.database}
          placeholder=":memory:"
        />
        <p class="text-sm text-muted-foreground">
          Path to database file or :memory: for in-memory database
        </p>
      </div>

      <div class="space-y-2">
        <Label>DuckDB Options</Label>
        <div class="flex space-x-2">
          <Input
            placeholder="Option name"
            bind:value={newOptionKey}
          />
          <Input
            placeholder="Option value"
            bind:value={newOptionValue}
          />
          <Button size="icon" onclick={addOption}>
            <Plus />
          </Button>
        </div>

        {#if Object.keys(duckdbConfig.options).length > 0}
          <div class="mt-2 space-y-2">
            {#each Object.entries(duckdbConfig.options) as [key, value]}
              <div class="flex items-center justify-between">
                <span class="text-sm">{key}: {value}</span>
                <Button 
                  size="icon"
                  variant="ghost"
                  onclick={() => removeOption(key)}
                >
                  <Trash />
                </Button>
              </div>
            {/each}
          </div>
        {/if}
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