<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { Separator } from "$lib/components/ui/separator";
  import { projectConfigStore } from "$lib/stores/project-config";
  import { onMount } from "svelte";
  import type { ProjectConfig } from '$lib/types';

  let loading = false;
  let error: string | null = null;
  let config: ProjectConfig | null = null;

  let projectName: string = '';
  let projectDescription: string = '';
  let serverName: string = '';
  let httpPort: number = 8080;
  let cacheSchema: string = 'cache';
  let dbPath: string = ':memory:';
  let templatePath: string = '';
  let authEnabled: boolean = false;
  let projectNameFromApi: string = '';

  projectConfigStore.subscribe(value => {
    config = value;
    if (config) {
      projectName = config.project_name || '';
      projectDescription = config.project_description || '';
      serverName = config.server?.name || '';
      httpPort = config.server?.http_port || 8080;
      cacheSchema = config.server?.cache_schema || 'cache';
      dbPath = config.duckdb?.db_path || ':memory:';
      templatePath = config.template_path || '';
      authEnabled = config.auth?.enabled || false;
      projectNameFromApi = config.name || '';
    }
  });

  onMount(async () => {
    await projectConfigStore.load();
  });

  async function handleSave() {
    loading = true;
    error = null;
    try {
      if (config) {
        await projectConfigStore.save({
          ...config,
          project_name: projectName,
          project_description: projectDescription,
          server: {
            ...config.server,
            name: serverName,
            http_port: httpPort,
            cache_schema: cacheSchema
          },
          duckdb: {
            ...config.duckdb,
            db_path: dbPath
          },
          template_path: templatePath,
          auth: {
            enabled: authEnabled
          },
          name: projectNameFromApi
        });
      } else {
        error = 'No config to save';
      }
    } catch (err) {
      error = err instanceof Error ? err.message : 'An unknown error occurred';
    } finally {
      loading = false;
    }
  }
</script>

<div class="container mx-auto p-4 space-y-4">
  <Card>
    <CardHeader>
      <CardTitle>General Configuration</CardTitle>
    </CardHeader>
    <CardContent class="space-y-4">
      <form>
        <!-- Project Settings -->
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="project-name">Project Name</Label>
            <Input
              id="project-name"
              bind:value={projectName}
              placeholder="my-project"
            />
          </div>
          <div class="space-y-2">
            <Label for="project-description">Project Description</Label>
            <Input
              id="project-description"
              bind:value={projectDescription}
              placeholder="A short description of the project"
            />
          </div>
          <div class="space-y-2">
            <Label for="project-name-from-api">Project Name from API</Label>
            <Input
              id="project-name-from-api"
              bind:value={projectNameFromApi}
              placeholder="my-project"
              disabled
            />
          </div>
        </div>

        <Separator />

        <!-- Server Settings -->
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="server-name">Server Name</Label>
            <Input
              id="server-name"
              bind:value={serverName}
              placeholder="my-server"
            />
          </div>
          <div class="space-y-2">
            <Label for="http-port">HTTP Port</Label>
            <Input
              id="http-port"
              type="number"
              bind:value={httpPort}
              placeholder="8080"
            />
          </div>
          <div class="space-y-2">
            <Label for="cache-schema">Cache Schema</Label>
            <Input
              id="cache-schema"
              bind:value={cacheSchema}
              placeholder="cache"
            />
          </div>
        </div>

        <Separator />

        <!-- DuckDB Settings -->
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="db-path">Database Path</Label>
            <Input
              id="db-path"
              bind:value={dbPath}
              placeholder=":memory:"
            />
            <p class="text-sm text-muted-foreground">
              Leave empty for in-memory database
            </p>
          </div>
        </div>

        <Separator />

        <!-- Template Path -->
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="template-path">Template Path</Label>
            <Input
              id="template-path"
              bind:value={templatePath}
              placeholder="/path/to/templates"
            />
          </div>
        </div>

        <Separator />

        <!-- Auth Settings -->
        <div class="grid gap-4">
          <div class="space-y-2">
            <Label for="auth-enabled">Auth Enabled</Label>
            <!--
            <Input
              id="auth-enabled"
              type="checkbox"
              bind:checked={authEnabled}
            />
            -->
          </div>
        </div>

        <!-- Action Buttons -->
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