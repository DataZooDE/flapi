<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { Separator } from "$lib/components/ui/separator";
  import { awsSecretsConfig } from '$lib/state';

  let loading = $state(false);
  let error = $state<string | null>(null);
  let testingConnection = $state(false);

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

  async function testConnection() {
    testingConnection = true;
    error = null;
    try {
      // Test connection logic here using API
      testingConnection = false;
    } catch (err) {
      error = err instanceof Error ? err.message : 'An unknown error occurred';
      testingConnection = false;
    }
  }
</script>

<div class="space-y-4">
  <Card>
    <CardHeader>
      <CardTitle>AWS Secrets Manager Configuration</CardTitle>
    </CardHeader>
    <CardContent class="space-y-4">
      <div class="space-y-2">
        <Label for="region">AWS Region</Label>
        <Input 
          id="region"
          bind:value={awsSecretsConfig.region}
          placeholder="us-east-1"
        />
      </div>

      <Separator />

      <div class="space-y-4">
        <h3 class="text-sm font-medium">AWS Credentials</h3>
        <div class="space-y-2">
          <Label for="access-key">Access Key ID</Label>
          <Input 
            id="access-key"
            type="password"
            bind:value={awsSecretsConfig.accessKeyId}
            placeholder="AKIA..."
          />
        </div>

        <div class="space-y-2">
          <Label for="secret-key">Secret Access Key</Label>
          <Input 
            id="secret-key"
            type="password"
            bind:value={awsSecretsConfig.secretAccessKey}
            placeholder="wJalrXUtnFEMI..."
          />
        </div>

        <div class="space-y-2">
          <Label for="session-token">Session Token (Optional)</Label>
          <Input 
            id="session-token"
            type="password"
            bind:value={awsSecretsConfig.sessionToken}
            placeholder="IQoJb3JpZ2lu..."
          />
        </div>
      </div>

      <div class="flex justify-end space-x-2">
        <Button
          variant="outline"
          onclick={testConnection}
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