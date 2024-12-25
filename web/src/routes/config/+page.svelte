<script lang="ts">
  import { onMount } from 'svelte';
  import type { FlapiConfig } from '$lib/types';

  let config: FlapiConfig = {
    project_name: '',
    project_description: '',
    template: {
      path: ''
    },
    connections: {}
  };

  let allowedOrigins = '';

  onMount(() => {
    // Initialize allowedOrigins from config if it exists
    if (config.security?.cors?.allowed_origins) {
      allowedOrigins = config.security.cors.allowed_origins.join(', ');
    }
  });

  function handleAllowedOriginsChange() {
    if (!config.security) config.security = { cors: { allowed_origins: [] } };
    if (!config.security.cors) config.security.cors = { allowed_origins: [] };
    config.security.cors.allowed_origins = allowedOrigins.split(',').map(s => s.trim()).filter(s => s);
  }
</script>

<div class="space-y-8">
  <div>
    <h1 class="text-2xl font-bold">Configuration</h1>
    <p class="text-muted-foreground">Manage your API configuration</p>
  </div>

  <form class="space-y-8">
    <!-- Project Information -->
    <div class="card p-6 space-y-6">
      <h2 class="text-xl font-semibold">Project Information</h2>
      <div class="grid gap-4">
        <div class="space-y-2">
          <label for="project-name" class="text-sm font-medium">Project Name</label>
          <input
            type="text"
            id="project-name"
            class="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm ring-offset-background focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 disabled:cursor-not-allowed disabled:opacity-50"
            bind:value={config.project_name}
          />
        </div>
        <div class="space-y-2">
          <label for="project-description" class="text-sm font-medium">Project Description</label>
          <textarea
            id="project-description"
            class="flex min-h-[80px] w-full rounded-md border border-input bg-background px-3 py-2 text-sm ring-offset-background focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 disabled:cursor-not-allowed disabled:opacity-50"
            bind:value={config.project_description}
          ></textarea>
        </div>
      </div>
    </div>

    <!-- Template Configuration -->
    <div class="card p-6 space-y-6">
      <h2 class="text-xl font-semibold">Template Configuration</h2>
      <div class="space-y-2">
        <label for="template-path" class="text-sm font-medium">Template Path</label>
        <input
          type="text"
          id="template-path"
          class="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm ring-offset-background focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 disabled:cursor-not-allowed disabled:opacity-50"
          bind:value={config.template.path}
        />
      </div>
    </div>

    <!-- Security Settings -->
    <div class="card p-6 space-y-6">
      <h2 class="text-xl font-semibold">Security Settings</h2>
      <div class="space-y-4">
        <!-- CORS Configuration -->
        <div>
          <h3 class="text-lg font-medium mb-4">CORS Configuration</h3>
          <div class="space-y-2">
            <label for="allowed-origins" class="text-sm font-medium">Allowed Origins</label>
            <input
              type="text"
              id="allowed-origins"
              class="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm ring-offset-background focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 disabled:cursor-not-allowed disabled:opacity-50"
              bind:value={allowedOrigins}
              on:input={handleAllowedOriginsChange}
              placeholder="http://localhost:3000, https://example.com"
            />
          </div>
        </div>
      </div>
    </div>

    <!-- Save Button -->
    <div class="flex justify-end">
      <button
        type="submit"
        class="inline-flex items-center justify-center rounded-md text-sm font-medium ring-offset-background transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 disabled:pointer-events-none disabled:opacity-50 bg-primary text-primary-foreground hover:bg-primary/90 h-10 px-4 py-2"
      >
        Save Changes
      </button>
    </div>
  </form>
</div>
