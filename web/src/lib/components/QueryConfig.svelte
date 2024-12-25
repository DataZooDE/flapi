<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import type { EndpointConfig } from '$lib/types';
  import SqlTemplateEditor from './SqlTemplateEditor.svelte';

  export let endpoint: EndpointConfig;

  const handleExecute = async (sql: string): Promise<any> => {
    // TODO: Implement query execution
    return { message: 'Query execution not implemented yet' };
  };

  const handleExpand = async (template: string): Promise<string> => {
    // TODO: Implement template expansion
    return template.replace(/\{\{(\w+)\}\}/g, "'$1'");
  };
</script>

<div class="space-y-6">
  <!-- SQL Editor -->
  <div class="card p-4">
    <div class="space-y-4">
      <div class="flex justify-between items-center">
        <div>
          <h3 class="text-lg font-medium">Query Template</h3>
          <p class="text-sm text-gray-600">Write the SQL query template with variables from request fields</p>
        </div>
      </div>

      <SqlTemplateEditor
        bind:source={endpoint.templateSource}
        variables={endpoint.requestFields.map(field => ({
          name: field.fieldName,
          type: field.validators[0]?.type || 'string',
          description: field.description
        }))}
        onExecute={handleExecute}
        onExpand={handleExpand}
      />
    </div>
  </div>
</div>
