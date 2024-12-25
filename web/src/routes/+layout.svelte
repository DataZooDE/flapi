<script lang="ts">
  import { page } from '$app/stores';
  import { Terminal, Settings, Database, Network } from 'lucide-svelte';
  import type { EndpointConfig, ConnectionConfig } from '$lib/types';
  import "../app.css";

  export let data: {
    endpoints: Record<string, EndpointConfig>;
    connections: Record<string, ConnectionConfig>;
  };
</script>

<div class="app-container">
  <aside class="sidebar">
    <div class="flex h-16 items-center border-b px-6">
      <img src="/logo.svg" alt="flAPI" class="h-8 w-8" />
      <h2 class="ml-3 text-lg font-semibold">flAPI</h2>
    </div>

    <nav class="p-4 space-y-2">
      <a 
        href="/" 
        class="flex items-center space-x-2 p-2 hover:bg-gray-100 rounded {$page.url.pathname === '/' ? 'bg-gray-100' : ''}"
      >
        <Terminal class="h-5 w-5" />
        <span>Project</span>
      </a>

      <div class="space-y-1">
        <a 
          href="/connections" 
          class="flex items-center space-x-2 p-2 hover:bg-gray-100 rounded {$page.url.pathname.includes('/connections') ? 'bg-gray-100' : ''}"
        >
          <Network class="h-5 w-5" />
          <span>Connections</span>
        </a>

        {#if Object.keys(data.connections).length > 0}
          <div class="pl-6 space-y-1">
            {#each Object.entries(data.connections) as [name, connection]}
              <a
                href="/connections/{name}"
                class="flex flex-col p-2 hover:bg-gray-100 rounded {$page.url.pathname === `/connections/${name}` ? 'text-primary-600 bg-primary-50' : ''}"
              >
                <span class="text-sm">{name}</span>
                <span class="text-xs text-gray-500">{connection.init}</span>
              </a>
            {/each}
          </div>
        {/if}
      </div>

      <div class="space-y-1">
        <a 
          href="/endpoints" 
          class="flex items-center space-x-2 p-2 hover:bg-gray-100 rounded {$page.url.pathname.includes('/endpoints') ? 'bg-gray-100' : ''}"
        >
          <Settings class="h-5 w-5" />
          <span>Endpoints</span>
        </a>

        {#if Object.keys(data.endpoints).length > 0}
          <div class="pl-6 space-y-1">
            {#each Object.entries(data.endpoints) as [path, endpoint]}
              <a
                href="/endpoints/{encodeURIComponent(path)}"
                class="flex flex-col p-2 hover:bg-gray-100 rounded {$page.url.pathname === `/endpoints/${encodeURIComponent(path)}` ? 'text-primary-600 bg-primary-50' : ''}"
              >
                <span class="text-sm">{path}</span>
                <span class="text-xs text-gray-500">{endpoint.urlPath}</span>
              </a>
            {/each}
          </div>
        {/if}
      </div>

      <a 
        href="/doc" 
        class="flex items-center space-x-2 p-2 hover:bg-gray-100 rounded {$page.url.pathname === '/doc' ? 'bg-gray-100' : ''}"
      >
        <Database class="h-5 w-5" />
        <span>Documentation</span>
      </a>
    </nav>
  </aside>

  <main class="main-content">
    <slot />
  </main>
</div>

<style>
  .app-container {
    display: grid;
    grid-template-columns: 250px 1fr;
    min-height: 100vh;
  }

  .sidebar {
    @apply bg-white border-r;
    height: 100vh;
    overflow-y: auto;
    position: sticky;
    top: 0;
  }

  .main-content {
    @apply p-6 bg-gray-50;
    min-height: 100vh;
  }
</style> 