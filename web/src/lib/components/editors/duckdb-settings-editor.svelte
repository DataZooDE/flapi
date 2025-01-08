<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { Separator } from "$lib/components/ui/separator";
  import { duckdbStore } from "$lib/stores/duckdb";
  import { onMount } from "svelte";
  
  let loading = false;
  let error: string | null = null;
  let newSettingKey = '';
  let newSettingValue = '';

  $: settings = $duckdbStore.data || {
    settings: {},
    db_path: ''
  };

  onMount(async () => {
    await duckdbStore.load();
  });

  async function handleSave() {
    loading = true;
    error = null;
    try {
      await duckdbStore.save(settings);
    } catch (err) {
      error = err instanceof Error ? err.message : 'An unknown error occurred';
    } finally {
      loading = false;
    }
  }

  function addSetting() {
    if (newSettingKey && newSettingValue) {
      settings.settings = {
        ...settings.settings,
        [newSettingKey]: newSettingValue
      };
      newSettingKey = '';
      newSettingValue = '';
    }
  }

  function removeSetting(key: string) {
    const { [key]: _, ...rest } = settings.settings;
    settings.settings = rest;
  }

  const commonSettings = [
    { key: 'memory_limit', description: 'Maximum memory usage (e.g., 4GB)' },
    { key: 'threads', description: 'Number of threads to use' },
    { key: 'access_mode', description: 'READ_ONLY or READ_WRITE' },
    { key: 'allow_unsigned_extensions', description: 'true or false' }
  ];

  function applyCommonSetting(key: string) {
    newSettingKey = key;
  }
</script>

<div class="container mx-auto p-4 space-y-4">
  <Card>
    <CardHeader>
      <CardTitle>DuckDB Settings</CardTitle>
    </CardHeader>
    <CardContent>
      <form class="space-y-6">
        <!-- Database Path -->
        <div class="space-y-2">
          <Label for="db-path">Database Path</Label>
          <Input 
            id="db-path" 
            bind:value={settings.db_path} 
            placeholder="./flapi_cache.db"
          />
          <p class="text-sm text-muted-foreground">
            Leave empty for in-memory database
          </p>
        </div>

        <Separator />

        <!-- Settings -->
        <div class="space-y-4">
          <div class="flex justify-between items-center">
            <Label>Settings</Label>
            <div class="flex gap-2">
              {#each commonSettings as setting}
                <Button 
                  variant="outline" 
                  size="sm"
                  on:click={() => applyCommonSetting(setting.key)}
                  title={setting.description}
                >
                  {setting.key}
                </Button>
              {/each}
            </div>
          </div>

          <!-- Existing settings -->
          <div class="space-y-2">
            {#if Object.keys(settings.settings).length > 0}
              {#each Object.entries(settings.settings) as [key, value]}
                <div class="flex items-center gap-2">
                  <Input readonly value={key} class="flex-1" />
                  <Input readonly value={value} class="flex-1" />
                  <Button 
                    variant="destructive" 
                    size="icon"
                    on:click={() => removeSetting(key)}
                  >
                    ×
                  </Button>
                </div>
              {/each}
            {:else}
              <p class="text-sm text-muted-foreground">No settings configured</p>
            {/if}
          </div>

          <!-- Add new setting -->
          <div class="flex items-end gap-2">
            <div class="flex-1">
              <Label for="setting-key">Key</Label>
              <Input 
                id="setting-key" 
                bind:value={newSettingKey} 
                placeholder="e.g., memory_limit"
              />
            </div>
            <div class="flex-1">
              <Label for="setting-value">Value</Label>
              <Input 
                id="setting-value" 
                bind:value={newSettingValue} 
                placeholder="e.g., 4GB"
              />
            </div>
            <Button 
              type="button"
              on:click={addSetting}
              disabled={!newSettingKey || !newSettingValue}
            >
              Add
            </Button>
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