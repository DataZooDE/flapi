<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { Separator } from "$lib/components/ui/separator";
  import { awsSecretsStore } from "$lib/stores/aws-secrets";
  import { onMount } from "svelte";
  
  let loading = false;
  let error: string | null = null;
  let testingConnection = false;

  $: config = $awsSecretsStore.data || {
    secret_name: '',
    region: '',
    credentials: {
      secret_id: '',
      secret_key: ''
    },
    init_sql: '',
    secret_table: ''
  };

  onMount(async () => {
    await awsSecretsStore.load();
  });

  async function handleSave() {
    loading = true;
    error = null;
    try {
      await awsSecretsStore.save(config);
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
      await awsSecretsStore.testConnection(config);
      // Show success message using toast or similar
    } catch (err) {
      error = err instanceof Error ? err.message : 'Failed to test connection';
    } finally {
      testingConnection = false;
    }
  }

  const regions = [
    'us-east-1', 'us-east-2', 'us-west-1', 'us-west-2',
    'eu-west-1', 'eu-west-2', 'eu-central-1',
    'ap-northeast-1', 'ap-northeast-2', 'ap-southeast-1', 'ap-southeast-2',
    'sa-east-1'
  ];
</script>

<div class="container mx-auto p-4 space-y-4">
  <Card>
    <CardHeader>
      <CardTitle>AWS Secrets Manager Configuration</CardTitle>
    </CardHeader>
    <CardContent>
      <form class="space-y-6">
        <!-- Basic Settings -->
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="secret-name">Secret Name</Label>
            <Input 
              id="secret-name" 
              bind:value={config.secret_name} 
              placeholder="my-api-secret"
            />
            <p class="text-sm text-muted-foreground">
              Name of the secret in AWS Secrets Manager
            </p>
          </div>

          <div class="space-y-2">
            <Label for="region">AWS Region</Label>
            <select
              id="region"
              class="w-full p-2 rounded-md border"
              bind:value={config.region}
            >
              <option value="">Select a region</option>
              {#each regions as region}
                <option value={region}>{region}</option>
              {/each}
            </select>
            <p class="text-sm text-muted-foreground">
              AWS region where the secret is stored
            </p>
          </div>

          <div class="space-y-2">
            <Label for="secret-table">Secret Table Name</Label>
            <Input 
              id="secret-table" 
              bind:value={config.secret_table} 
              placeholder="auth_users"
            />
            <p class="text-sm text-muted-foreground">
              Name of the table to store authentication data
            </p>
          </div>
        </div>

        <Separator />

        <!-- AWS Credentials -->
        <div class="space-y-4">
          <Label>AWS Credentials (Optional)</Label>
          <div class="grid gap-4">
            <div class="space-y-2">
              <Label for="secret-id">Access Key ID</Label>
              <Input 
                id="secret-id" 
                bind:value={config.credentials.secret_id} 
                placeholder="AWS Access Key ID"
              />
            </div>
            <div class="space-y-2">
              <Label for="secret-key">Secret Access Key</Label>
              <Input 
                id="secret-key" 
                type="password"
                bind:value={config.credentials.secret_key} 
                placeholder="AWS Secret Access Key"
              />
            </div>
          </div>
          <p class="text-sm text-muted-foreground">
            Leave empty to use environment credentials or IAM role
          </p>
        </div>

        <Separator />

        <!-- Init SQL -->
        <div class="space-y-2">
          <Label for="init-sql">Initialization SQL</Label>
          <textarea
            id="init-sql"
            class="w-full min-h-[100px] p-2 rounded-md border"
            bind:value={config.init_sql}
            placeholder="SELECT username, password FROM auth_users"
          />
          <p class="text-sm text-muted-foreground">
            SQL query to fetch authentication data from the secret
          </p>
        </div>

        <!-- Action Buttons -->
        <div class="flex justify-end gap-2">
          <Button 
            variant="outline"
            on:click={testConnection}
            disabled={testingConnection || loading || !config.secret_name || !config.region}
          >
            {testingConnection ? 'Testing...' : 'Test Connection'}
          </Button>

          <Button 
            type="button"
            on:click={handleSave}
            disabled={loading || testingConnection}
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