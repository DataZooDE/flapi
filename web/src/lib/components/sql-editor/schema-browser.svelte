<script lang="ts">
  import { schemaStore } from '$lib/stores/schema';
  import type { SchemaInfo, SchemaState, SchemaTable } from '$lib/types';
  import { onMount } from 'svelte';

  // Props
  export let onTableClick: (tableName: string, schemaName: string) => void;

  let schemas: SchemaInfo | null = null;
  
  // Load schema data on mount
  onMount(async () => {
    await schemaStore.load();
    const unsubscribe = schemaStore.subscribe((state: SchemaState) => {
      schemas = state.data;
    });
    
    return unsubscribe;
  });
</script>

<div class="w-full h-full overflow-auto p-2">
  {#if schemas}
    {#each Object.entries(schemas) as [schemaName, schema]}
      <div class="mb-4">
        <div class="font-medium text-sm mb-1">{schemaName}</div>
        <div class="pl-4">
          {#each Object.entries(schema.tables) as [tableName, table]}
            {@const typedTable = table as SchemaTable}
            <button
              class="w-full text-left py-1 px-2 text-sm hover:bg-secondary/10 rounded-sm flex items-center gap-2 group"
              on:click={() => onTableClick(tableName, schemaName)}
            >
              <span class="opacity-50 group-hover:opacity-100">
                {typedTable.is_view ? '(view)' : '(table)'}
              </span>
              <span>{tableName}</span>
            </button>
            <div class="pl-4 text-xs opacity-70">
              {#each Object.entries(typedTable.columns) as [columnName, column]}
                <div class="py-0.5">
                  {columnName}: {column.type}{column.nullable ? '?' : ''}
                </div>
              {/each}
            </div>
          {/each}
        </div>
      </div>
    {/each}
  {:else}
    <div class="text-sm opacity-70">Loading schema information...</div>
  {/if}
</div> 