<script lang="ts">
  import { onMount } from 'svelte';

  let swaggerContainer: HTMLElement;

  onMount(() => {
    const script = document.createElement('script');
    script.src = 'https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.9.0/swagger-ui-bundle.js';
    script.onload = () => {
      const ui = (window as any).SwaggerUIBundle({
        url: '/api/doc.yaml',
        dom_id: '#swagger-ui',
        deepLinking: true,
        presets: [
          (window as any).SwaggerUIBundle.presets.apis,
          (window as any).SwaggerUIBundle.SwaggerUIStandalonePreset
        ],
      });
    };
    document.head.appendChild(script);

    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = 'https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.9.0/swagger-ui.css';
    document.head.appendChild(link);
  });
</script>

<div class="space-y-6">
  <div class="prose max-w-none">
    <h1 class="text-2xl font-bold text-gray-900">API Documentation</h1>
    <p class="text-gray-600">
      This page provides interactive documentation for all available API endpoints.
      You can try out the endpoints directly from this interface.
    </p>
  </div>

  <div bind:this={swaggerContainer} id="swagger-ui" class="mt-8" />
</div>

<style>
  :global(.swagger-ui) {
    @apply text-gray-900;
  }

  :global(.swagger-ui .info) {
    @apply mb-8;
  }

  :global(.swagger-ui .scheme-container) {
    @apply bg-gray-50 shadow-sm;
  }

  :global(.swagger-ui .opblock) {
    @apply shadow-sm border border-gray-200;
  }

  :global(.swagger-ui .opblock-tag) {
    @apply text-lg font-semibold text-gray-900;
  }

  :global(.swagger-ui .opblock .opblock-summary-method) {
    @apply text-sm font-medium;
  }

  :global(.swagger-ui .btn) {
    @apply shadow-sm;
  }
</style> 