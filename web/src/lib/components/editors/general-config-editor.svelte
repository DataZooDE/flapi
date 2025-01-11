<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { Separator } from "$lib/components/ui/separator";
  import { projectConfigStore } from "$lib/stores/project-config";
  import { onMount } from "svelte";
  import type { ProjectConfig } from '$lib/types';
  import { Checkbox } from "$lib/components/ui/checkbox";
  import DuckDBSettingsEditor from "./duckdb-settings-editor.svelte";

  let loading = false;
  let error: string | null = null;
  let config: ProjectConfig | null = null;

  let projectDescription: string = '';
  let hostname: string = '';
  let httpPort: number = 8080;
  let cacheSchema: string = 'cache';
  let dbPath: string = ':memory:';
  let templatePath: string = '';
  let projectName: string = '';
  let heartbeatEnabled: boolean = false;
  let heartbeatInterval: number = 10;
  let enforceHttpsEnabled: boolean = false;
  let sslCertFile: string = '';
  let sslKeyFile: string = '';
  let duckdbSettings: Record<string, string> = {};

  projectConfigStore.subscribe(value => {
    config = value;
    if (config) {
      projectName = config.name || '';
      projectDescription = config.description || '';
      hostname = config.server?.hostname || '';
      httpPort = config.server?.http_port || 8080;
      cacheSchema = config.duckdb?.cache_schema || 'cache';
      dbPath = config.duckdb?.db_path || ':memory:';
      templatePath = config.template_path || '';
      heartbeatEnabled = config.heartbeat?.enabled || false;
      heartbeatInterval = config.heartbeat?.worker_interval || 10;
      enforceHttpsEnabled = config.enforce_https?.enabled || false;
      sslCertFile = config.enforce_https?.ssl_cert_file || '';
      sslKeyFile = config.enforce_https?.ssl_key_file || '';
      duckdbSettings = config.duckdb?.settings || {};
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
          name: projectName,
          description: projectDescription,
          server: {
            ...config.server,
            hostname: hostname,
            http_port: httpPort,
          },
          duckdb: {
            ...config.duckdb,
            db_path: dbPath,
            settings: duckdbSettings,
            cache_schema: cacheSchema
          },
          template_path: templatePath,
          heartbeat: {
            enabled: heartbeatEnabled,
            worker_interval: heartbeatInterval
          },
          enforce_https: {
            enabled: enforceHttpsEnabled,
            ssl_cert_file: sslCertFile,
            ssl_key_file: sslKeyFile
          }
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

  async function handleReset() {
    loading = true;
    error = null;
    try {
      await projectConfigStore.load();
    } catch (err) {
      error = err instanceof Error ? err.message : 'An unknown error occurred';
    } finally {
      loading = false;
    }
  }

  function handleDuckdbSettingsUpdate(updatedSettings: Record<string, string>) {
    duckdbSettings = updatedSettings;
  }
</script>

<div class="container mx-auto p-4 space-y-4">
  <Card>
    <CardHeader>
      <CardTitle>General Configuration</CardTitle>
    </CardHeader>
    <CardContent class="space-y-4">
      <form class="space-y-6">
        <div class="space-y-2">
          <Label for="project-name">Project Name</Label>
          <Input
            id="project-name"
            bind:value={projectName}
            placeholder="My Project"
            disabled
          />
          <p class="text-sm text-muted-foreground">
            The name of the project. This is read-only and cannot be changed here.
          </p>
        </div>

        <div class="space-y-2">
          <Label for="project-description">Project Description</Label>
          <Input
            id="project-description"
            bind:value={projectDescription}
            placeholder="A brief description of the project."
          />
          <p class="text-sm text-muted-foreground">
            A brief description of the project.
          </p>
        </div>

        <div class="space-y-2">
          <Label for="template-path">Template Path</Label>
          <Input
            id="template-path"
            bind:value={templatePath}
            placeholder="./path/to/templates"
          />
          <p class="text-sm text-muted-foreground">
            The path to the directory containing SQL templates. Ideally relative to the current working directory.
          </p>
        </div>

        <Separator />

        <Card>
          <CardHeader>
            <CardTitle>Server Configuration</CardTitle>
          </CardHeader>
          <CardContent class="space-y-4">
            <div class="space-y-2">
              <Label for="hostname">Hostname</Label>
              <Input
                id="hostname"
                bind:value={hostname}
                placeholder="localhost"
              />
              <p class="text-sm text-muted-foreground">
                The hostname or IP address the server will listen on.
              </p>
            </div>
            <div class="space-y-2">
              <Label for="http-port">HTTP Port</Label>
              <Input
                id="http-port"
                type="number"
                bind:value={httpPort}
                placeholder="8080"
              />
              <p class="text-sm text-muted-foreground">
                The port number to listen on (1-65535).
              </p>
            </div>
            <div class="space-y-2">
              <Label for="enforce-https-enabled">Enforce HTTPS</Label>
              <Checkbox
                id="enforce-https-enabled"
                bind:checked={enforceHttpsEnabled}
              />
              <p class="text-sm text-muted-foreground">
                Enable or disable HTTPS enforcement.
              </p>
            </div>
            {#if enforceHttpsEnabled}
              <div class="space-y-2">
                <Label for="ssl-cert-file">SSL Certificate File</Label>
                <Input
                  id="ssl-cert-file"
                  bind:value={sslCertFile}
                  placeholder="/path/to/cert.pem"
                />
                <p class="text-sm text-muted-foreground">
                  The path to the SSL certificate file.
                </p>
              </div>
              <div class="space-y-2">
                <Label for="ssl-key-file">SSL Key File</Label>
                <Input
                  id="ssl-key-file"
                  bind:value={sslKeyFile}
                  placeholder="/path/to/key.pem"
                />
                <p class="text-sm text-muted-foreground">
                  The path to the SSL key file.
                </p>
              </div>
            {/if}
          </CardContent>
        </Card>

        <Separator />

        <Card>
          <CardHeader>
            <CardTitle>DuckDB Settings</CardTitle>
          </CardHeader>
          <CardContent class="space-y-4">
            <div class="space-y-2">
              <Label for="db-path">Database Path</Label>
              <Input
                id="db-path"
                bind:value={dbPath}
                placeholder=":memory:"
              />
              <p class="text-sm text-muted-foreground">
                The path to the DuckDB database file. Leave empty for an in-memory database.
              </p>
            </div>
            <div class="space-y-2">
              <Label for="cache-schema">Cache Schema</Label>
              <Input
                id="cache-schema"
                bind:value={cacheSchema}
                placeholder="cache"
              />
              <p class="text-sm text-muted-foreground">
                The schema name for storing cached query results.
              </p>
            </div>
            <DuckDBSettingsEditor
              settings={duckdbSettings}
              onUpdate={handleDuckdbSettingsUpdate}
            />
          </CardContent>
        </Card>

        <Separator />

        <Card>
          <CardHeader>
            <CardTitle>Heartbeat Settings</CardTitle>
          </CardHeader>
          <CardContent class="space-y-4">
            <div class="space-y-2">
              <Label for="heartbeat-enabled">Heartbeat Enabled</Label>
              <Checkbox
                id="heartbeat-enabled"
                bind:checked={heartbeatEnabled}
              />
              <p class="text-sm text-muted-foreground">
                Enable or disable the heartbeat worker.
              </p>
            </div>
            {#if heartbeatEnabled}
              <div class="space-y-2">
                <Label for="heartbeat-interval">Heartbeat Interval</Label>
                <Input
                  id="heartbeat-interval"
                  type="number"
                  bind:value={heartbeatInterval}
                  placeholder="10"
                />
                <p class="text-sm text-muted-foreground">
                  The interval (in seconds) at which the heartbeat worker runs.
                </p>
              </div>
            {/if}
          </CardContent>
        </Card>

        <!-- Action Buttons -->
        <div class="flex justify-end gap-2">
          <Button
            type="button"
            on:click={handleReset}
            disabled={loading}
            variant="outline"
          >
            Reset
          </Button>
          <Button
            type="button"
            on:click={handleSave}
            disabled={loading}
            variant="default"
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