<script lang="ts">
  import '../app.css';
  import { page } from '$app/stores';
  import { Home, Database, Network } from 'lucide-svelte';
</script>

<div class="min-h-screen bg-primary-50">
  <!-- Header -->
  <header class="bg-primary-400 text-white shadow-lg">
    <div class="container mx-auto px-4 py-3 flex items-center space-x-4">
      <img src="/logo.svg" alt="Flapi" class="h-8 w-8" />
      <h1 class="text-xl font-bold">Flapi Documentation</h1>
    </div>
  </header>

  <!-- Main Content -->
  <div class="container mx-auto px-4 py-8 flex gap-8">
    <!-- Sidebar -->
    <aside class="w-64 flex-shrink-0">
      <nav class="space-y-1">
        <a
          href="/"
          class="flex items-center space-x-3 px-4 py-2 rounded-lg text-gray-700 hover:bg-primary-100 {$page.url.pathname === '/' ? 'bg-primary-100 font-medium' : ''}"
        >
          <Home class="w-5 h-5" />
          <span>Start Here</span>
        </a>
        <a
          href="/connections"
          class="flex items-center space-x-3 px-4 py-2 rounded-lg text-gray-700 hover:bg-primary-100 {$page.url.pathname.startsWith('/connections') ? 'bg-primary-100 font-medium' : ''}"
        >
          <Database class="w-5 h-5" />
          <span>Connections</span>
        </a>
        <a
          href="/endpoints"
          class="flex items-center space-x-3 px-4 py-2 rounded-lg text-gray-700 hover:bg-primary-100 {$page.url.pathname.startsWith('/endpoints') ? 'bg-primary-100 font-medium' : ''}"
        >
          <Network class="w-5 h-5" />
          <span>Endpoints</span>
        </a>
      </nav>
    </aside>

    <!-- Content -->
    <main class="flex-1 bg-white rounded-lg shadow-sm p-6">
      <slot />
    </main>
  </div>
</div>

<style>
  :global(html) {
    font-family: 'Inter', system-ui, sans-serif;
  }
</style> 