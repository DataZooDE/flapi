<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { projectConfig } from "$lib/state";
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";

  let loading = $state(false);
  let error = $state<string | null>(null);

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

<div class="container mx-auto p-6">
  <Card>
    <CardHeader>
      <CardTitle>General Configuration</CardTitle>
    </CardHeader>
    <CardContent class="space-y-4">
      <div class="space-y-2">
        <Label for="project-name">Project Name</Label>
        <Input 
          id="project-name"
          bind:value={projectConfig.project_name}
          placeholder="My API Project"
        />
      </div>

      <div class="space-y-2">
        <Label for="version">Version</Label>
        <Input 
          id="version"
          bind:value={projectConfig.version}
          placeholder="1.0.0"
        />
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