<script lang="ts">
  import { onMount } from 'svelte';
  import type { Cache } from '$lib/types';
  import SqlTemplateEditor from './SqlTemplateEditor.svelte';

  export let cache: Cache;

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
  <!-- Cache Settings -->
  <div class="card p-4">
    <div class="grid grid-cols-2 gap-6">
      <div class="space-y-2">
        <label for="cacheTableName" class="label font-medium">Cache Table Name</label>
        <input
          id="cacheTableName"
          type="text"
          bind:value={cache.cacheTableName}
          class="input w-full"
          placeholder="e.g., my_data_cache"
        />
        <p class="text-sm text-gray-600">The name of the table where cached data will be stored</p>
      </div>

      <div class="space-y-2">
        <label for="refreshTime" class="label font-medium">Refresh Interval</label>
        <input
          id="refreshTime"
          type="text"
          bind:value={cache.refreshTime}
          class="input w-full"
          placeholder="e.g., 1h, 30m, 24h"
        />
        <p class="text-sm text-gray-600">How often the cache should be refreshed</p>
      </div>
    </div>
  </div>

  <!-- SQL Editor -->
  <div class="card p-4">
    <div class="space-y-4">
      <div class="flex justify-between items-center">
        <div>
          <h3 class="text-lg font-medium">Cache Source SQL</h3>
          <p class="text-sm text-gray-600">Write the SQL query that will populate the cache table</p>
        </div>
      </div>

      <SqlTemplateEditor
        bind:source={cache.cacheSource}
        variables={[
          { name: 'timestamp', type: 'datetime', description: 'Current timestamp when cache is being refreshed' },
          { name: 'previous_table', type: 'string', description: 'Name of the previous cache table (for incremental updates)' }
        ]}
        onExecute={handleExecute}
        onExpand={handleExpand}
      />
    </div>
  </div>
</div>
