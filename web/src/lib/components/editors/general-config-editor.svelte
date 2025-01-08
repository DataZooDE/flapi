<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { configStore } from "$lib/stores/config";
  import { Separator } from "$lib/components/ui/separator";
  import { duckdbStore } from "$lib/stores/duckdb";
  import { onMount } from "svelte";
  import { awsSecretsStore } from "$lib/stores/aws-secrets";
  
  let loading = false;
  let error: string | null = null;
  let newSettingKey = '';
  let newSettingValue = '';
  let testingConnection = false;
  
  $: config = $configStore;
  $: duckdbSettings = $duckdbStore.data || { settings: {}, db_path: '' };
  $: awsSecrets = $awsSecretsStore.data || {
    secret_name: '',
    region: '',
    credentials: { secret_id: '', secret_key: '' },
    init_sql: '',
    secret_table: ''
  };

  onMount(async () => {
    await duckdbStore.load();
    await awsSecretsStore.load();
  });

  async function handleSave() {
    loading = true;
    error = null;
    try {
      await configStore.save();
      await duckdbStore.save(duckdbSettings);
      await awsSecretsStore.save(awsSecrets);
    } catch (err) {
      error = err instanceof Error ? err.message : 'An unknown error occurred';
    } finally {
      loading = false;
    }
  }

  async function testAwsConnection() {
    testingConnection = true;
    error = null;
    try {
      await awsSecretsStore.testConnection(awsSecrets);
      // Show success message
    } catch (err) {
      error = err instanceof Error ? err.message : 'Failed to test AWS connection';
    } finally {
      testingConnection = false;
    }
  }

  function addSetting() {
    if (newSettingKey && newSettingValue) {
      duckdbSettings.settings = {
        ...duckdbSettings.settings,
        [newSettingKey]: newSettingValue
      };
      newSettingKey = '';
      newSettingValue = '';
    }
  }

  function removeSetting(key: string) {
    const { [key]: _, ...rest } = duckdbSettings.settings || {};
    duckdbSettings.settings = rest;
  }
</script>

<div class="container mx-auto p-4 space-y-4">
  <!-- Project Settings -->
  <Card>
    <CardHeader>
      <CardTitle>Project Settings</CardTitle>
    </CardHeader>
    <CardContent>
      <form class="space-y-4">
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="project-name">Project Name</Label>
            <Input 
              id="project-name" 
              bind:value={$configStore.project_name} 
              placeholder="My API Project"
            />
          </div>
          
          <div class="space-y-2">
            <Label for="version">Version</Label>
            <Input 
              id="version" 
              bind:value={$configStore.version} 
              placeholder="1.0.0"
            />
          </div>
        </div>
      </form>
    </CardContent>
  </Card>

  <!-- Server Configuration -->
  <Card>
    <CardHeader>
      <CardTitle>Server Configuration</CardTitle>
    </CardHeader>
    <CardContent>
      <form class="space-y-4">
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="server-name">Server Name</Label>
            <Input 
              id="server-name" 
              bind:value={$configStore.server.name} 
              placeholder="flapi-server"
            />
          </div>
          
          <div class="space-y-2">
            <Label for="http-port">HTTP Port</Label>
            <Input 
              id="http-port" 
              type="number" 
              bind:value={$configStore.server.http_port} 
              placeholder="8080"
            />
          </div>

          <div class="space-y-2">
            <Label for="cache-schema">Cache Schema</Label>
            <Input 
              id="cache-schema" 
              bind:value={$configStore.server.cache_schema} 
              placeholder="cache"
            />
          </div>
        </div>
      </form>
    </CardContent>
  </Card>

  <!-- DuckDB Settings -->
  <Card>
    <CardHeader>
      <CardTitle>DuckDB Settings</CardTitle>
    </CardHeader>
    <CardContent>
      <form class="space-y-6">
        <div class="space-y-2">
          <Label for="db-path">Database Path</Label>
          <Input 
            id="db-path" 
            bind:value={duckdbSettings.db_path} 
            placeholder="./flapi_cache.db"
          />
          <p class="text-sm text-muted-foreground">
            Leave empty for in-memory database
          </p>
        </div>

        <Separator class="my-4" />

        <div class="space-y-4">
          <Label>Settings</Label>
          
          <!-- Existing settings -->
          <div class="space-y-2">
            {#if duckdbSettings.settings && Object.keys(duckdbSettings.settings).length > 0}
              {#each Object.entries(duckdbSettings.settings) as [key, value]}
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
                placeholder="e.g., access_mode"
              />
            </div>
            <div class="flex-1">
              <Label for="setting-value">Value</Label>
              <Input 
                id="setting-value" 
                bind:value={newSettingValue} 
                placeholder="e.g., READ_WRITE"
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
      </form>
    </CardContent>
  </Card>

  <!-- AWS Secrets Manager Configuration -->
  <Card>
    <CardHeader>
      <CardTitle>AWS Secrets Manager Configuration</CardTitle>
    </CardHeader>
    <CardContent>
      <form class="space-y-6">
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="secret-name">Secret Name</Label>
            <Input 
              id="secret-name" 
              bind:value={awsSecrets.secret_name} 
              placeholder="my-api-secret"
            />
          </div>

          <div class="space-y-2">
            <Label for="region">AWS Region</Label>
            <Input 
              id="region" 
              bind:value={awsSecrets.region} 
              placeholder="us-east-1"
            />
          </div>

          <div class="space-y-2">
            <Label for="secret-table">Secret Table Name</Label>
            <Input 
              id="secret-table" 
              bind:value={awsSecrets.secret_table} 
              placeholder="auth_users"
            />
          </div>

          <Separator class="my-2" />

          <div class="space-y-2">
            <Label>AWS Credentials (Optional)</Label>
            <div class="grid gap-2">
              <Input 
                id="secret-id" 
                bind:value={awsSecrets.credentials.secret_id} 
                placeholder="AWS Access Key ID"
              />
              <Input 
                id="secret-key" 
                type="password"
                bind:value={awsSecrets.credentials.secret_key} 
                placeholder="AWS Secret Access Key"
              />
            </div>
            <p class="text-sm text-muted-foreground">
              Leave empty to use environment credentials
            </p>
          </div>

          <div class="space-y-2">
            <Label for="init-sql">Initialization SQL</Label>
            <textarea
              id="init-sql"
              class="w-full min-h-[100px] p-2 rounded-md border"
              bind:value={awsSecrets.init_sql}
              placeholder="SELECT username, password FROM auth_users"
            />
          </div>

          <div class="flex justify-end">
            <Button 
              type="button"
              variant="outline"
              on:click={testAwsConnection}
              disabled={testingConnection || !awsSecrets.secret_name || !awsSecrets.region}
            >
              {testingConnection ? 'Testing...' : 'Test Connection'}
            </Button>
          </div>
        </div>
      </form>
    </CardContent>
  </Card>

  <!-- Save Button -->
  <div class="flex justify-end">
    <Button 
      on:click={handleSave} 
      disabled={loading}
    >
      {loading ? 'Saving...' : 'Save Changes'}
    </Button>
  </div>

  {#if error}
    <p class="text-red-500 text-sm mt-2">{error}</p>
  {/if}
</div> 