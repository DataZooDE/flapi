<script lang="ts">
  import { createEventDispatcher } from 'svelte';

  export let value: string = '';
  export let showAddProperty: boolean = true;

  const dispatch = createEventDispatcher();

  // Configuration options with descriptions and default values
  const configOptions = [
    {
      key: 'access_mode',
      name: 'Access Mode',
      description: 'Access mode of the database (AUTOMATIC, READ_ONLY or READ_WRITE)',
      defaultValue: 'automatic',
      type: 'select',
      options: ['automatic', 'READ_ONLY', 'READ_WRITE']
    },
    {
      key: 'allow_unsigned_extensions',
      name: 'Allow Unsigned Extensions',
      description: 'Allow to load community built extensions',
      defaultValue: 'true',
      type: 'boolean'
    },
    {
      key: 'custom_extension_repository',
      name: 'Custom Extension Repository',
      description: 'URL to custom extension repository',
      defaultValue: '',
      type: 'text'
    },
    {
      key: 'default_null_order',
      name: 'Default NULL Order',
      description: 'Default NULL sorting order',
      defaultValue: 'nulls_first',
      type: 'select',
      options: ['nulls_first', 'nulls_last']
    },
    {
      key: 'default_order',
      name: 'Default Order',
      description: 'Default sorting order',
      defaultValue: 'ASC',
      type: 'select',
      options: ['ASC', 'DESC']
    },
    {
      key: 'enable_external_access',
      name: 'Enable External Access',
      description: 'Allow access to external state',
      defaultValue: 'true',
      type: 'boolean'
    },
    {
      key: 'enable_object_cache',
      name: 'Enable Object Cache',
      description: 'Whether to enable the object cache',
      defaultValue: 'true',
      type: 'boolean'
    },
    {
      key: 'enable_progress_bar',
      name: 'Enable Progress Bar',
      description: 'Enables the progress bar, printing progress to the terminal for long queries',
      defaultValue: 'true',
      type: 'boolean'
    },
    {
      key: 'explain_output',
      name: 'Explain Output',
      description: 'Output of EXPLAIN statements',
      defaultValue: 'physical_only',
      type: 'select',
      options: ['all', 'optimized_only', 'physical_only']
    },
    {
      key: 'extension_directory',
      name: 'Extension Directory',
      description: 'Directory to store extensions',
      defaultValue: '',
      type: 'text'
    },
    {
      key: 'external_threads',
      name: 'External Threads',
      description: 'Number of external threads',
      defaultValue: '4',
      type: 'number'
    },
    {
      key: 'file_search_path',
      name: 'File Search Path',
      description: 'A comma separated list of directories to search for input files',
      defaultValue: '',
      type: 'text'
    },
    {
      key: 'home_directory',
      name: 'Home Directory',
      description: 'Sets the home directory used by the system',
      defaultValue: '',
      type: 'text'
    },
    {
      key: 'log_query_path',
      name: 'Log Query Path',
      description: 'Path to log queries (NULL for no logging)',
      defaultValue: '',
      type: 'text'
    },
    {
      key: 'max_expression_depth',
      name: 'Max Expression Depth',
      description: 'Maximum expression depth in the parser',
      defaultValue: '1000',
      type: 'number'
    },
    {
      key: 'max_memory',
      name: 'Max Memory',
      description: 'Maximum memory usage limit',
      defaultValue: '8GB',
      type: 'text'
    },
    {
      key: 'memory_limit',
      name: 'Memory Limit',
      description: 'Memory limit for the system',
      defaultValue: '8GB',
      type: 'text'
    },
    {
      key: 'ordered_aggregate_threshold',
      name: 'Ordered Aggregate Threshold',
      description: 'Number of rows to accumulate before sorting',
      defaultValue: '262144',
      type: 'number'
    },
    {
      key: 'perfect_ht_threshold',
      name: 'Perfect Hash Table Threshold',
      description: 'Threshold in bytes for when to use a perfect hash table',
      defaultValue: '12',
      type: 'number'
    },
    {
      key: 'preserve_identifier_case',
      name: 'Preserve Identifier Case',
      description: 'Whether to preserve identifier case',
      defaultValue: 'true',
      type: 'boolean'
    },
    {
      key: 'profile_output',
      name: 'Profile Output',
      description: 'File to save profile output (empty for terminal)',
      defaultValue: '',
      type: 'text'
    },
    {
      key: 'profiling_mode',
      name: 'Profiling Mode',
      description: 'The profiling mode to use',
      defaultValue: 'standard',
      type: 'select',
      options: ['detailed', 'standard']
    },
    {
      key: 'schema',
      name: 'Schema',
      description: 'Default search schema',
      defaultValue: 'main',
      type: 'text'
    },
    {
      key: 'threads',
      name: 'Threads',
      description: 'Number of threads to use',
      defaultValue: '8',
      type: 'number'
    },
    {
      key: 'window_mode',
      name: 'Window Mode',
      description: 'The window mode to use',
      defaultValue: 'window',
      type: 'select',
      options: ['window', 'combine']
    }
  ].sort((a, b) => a.name.localeCompare(b.name));

  function handleSelect(option: typeof configOptions[0]) {
    value = option.key;
    dispatch('select', { option });
  }

  let searchTerm = '';
</script>

<div class="relative">
  <div class="card bg-base-100 shadow-lg max-h-96 overflow-y-auto">
    <div class="p-2 sticky top-0 bg-base-100 z-10 border-b">
      <input
        type="text"
        placeholder="Search properties..."
        class="input input-sm w-full"
        bind:value={searchTerm}
        on:input={(e) => {
          const term = e.currentTarget.value.toLowerCase();
          configOptions.forEach((option) => {
            const element = document.getElementById(`option-${option.key}`);
            if (element) {
              const matches = 
                option.name.toLowerCase().includes(term) ||
                option.description.toLowerCase().includes(term) ||
                option.key.toLowerCase().includes(term);
              element.style.display = matches ? 'block' : 'none';
            }
          });
        }}
      />
    </div>
    <div class="divide-y">
      {#each configOptions as option}
        <button
          id="option-{option.key}"
          class="w-full text-left p-3 hover:bg-gray-50 transition-colors"
          class:bg-primary-50={value === option.key}
          on:click={() => handleSelect(option)}
        >
          <div class="flex items-center justify-between">
            <div class="flex flex-col">
              <span class="font-medium text-sm">{option.name}</span>
              <span class="text-xs text-gray-500 font-mono">{option.key}</span>
            </div>
            <span class="text-xs px-2 py-1 bg-gray-100 rounded">{option.type}</span>
          </div>
          <p class="text-sm text-gray-600 mt-1">{option.description}</p>
          <p class="text-xs text-gray-500 mt-1">Default: <span class="font-mono">{option.defaultValue}</span></p>
        </button>
      {/each}
    </div>
  </div>
</div>
