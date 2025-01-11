<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import { Separator } from "$lib/components/ui/separator";
  import { Plus, Trash } from "lucide-svelte";
  import { cn } from "$lib/utils";
  import { Select, SelectContent, SelectItem, SelectTrigger } from "$lib/components/ui/select";

  export let settings: Record<string, string> = {};
  export let onUpdate: (settings: Record<string, string>) => void;

  let newSettingKey = '';
  let newSettingValue = '';
  let validationErrors: Record<string, string> = {};
  let searchQuery = '';

  const duckdbSettings = [
    {
      name: 'access_mode',
      description: 'Access mode of the database (AUTOMATIC, READ_ONLY or READ_WRITE)',
      type: 'VARCHAR',
      default: 'automatic',
      scope: 'GLOBAL'
    },
    {
      name: 'allocator_background_threads',
      description: 'Whether to enable the allocator background thread.',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'allocator_bulk_deallocation_flush_threshold',
      description: 'If a bulk deallocation larger than this occurs, flush outstanding allocations.',
      type: 'VARCHAR',
      default: '512.0 MiB',
      scope: 'GLOBAL'
    },
    {
      name: 'allocator_flush_threshold',
      description: 'Peak allocation threshold at which to flush the allocator after completing a task.',
      type: 'VARCHAR',
      default: '128.0 MiB',
      scope: 'GLOBAL'
    },
    {
      name: 'allow_community_extensions',
      description: 'Allow to load community built extensions',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'GLOBAL'
    },
    {
      name: 'allow_extensions_metadata_mismatch',
      description: 'Allow to load extensions with not compatible metadata',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'allow_persistent_secrets',
      description: 'Allow the creation of persistent secrets, that are stored and loaded on restarts',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'GLOBAL'
    },
    {
      name: 'allow_unredacted_secrets',
      description: 'Allow printing unredacted secrets',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'allow_unsigned_extensions',
      description: 'Allow to load extensions with invalid or missing signatures',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'arrow_large_buffer_size',
      description: 'Whether arrow buffers for strings, blobs, uuids and bits should be exported using large buffers',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'arrow_lossless_conversion',
      description: 'Whenever a DuckDB type does not have a clear native or canonical extension match in Arrow, export the types with a duckdb.type_name extension name.',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'arrow_output_list_view',
      description: 'Whether export to arrow format should use ListView as the physical layout for LIST columns',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'autoinstall_extension_repository',
      description: 'Overrides the custom endpoint for extension installation on autoloading',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'autoinstall_known_extensions',
      description: 'Whether known extensions are allowed to be automatically installed when a query depends on them',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'GLOBAL'
    },
    {
      name: 'autoload_known_extensions',
      description: 'Whether known extensions are allowed to be automatically loaded when a query depends on them',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'GLOBAL'
    },
    {
      name: 'binary_as_string',
      description: 'In Parquet files, interpret binary data as a string.',
      type: 'BOOLEAN',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'ca_cert_file',
      description: 'Path to a custom certificate file for self-signed certificates.',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'catalog_error_max_schemas',
      description: 'The maximum number of schemas the system will scan for "did you mean…" style errors in the catalog',
      type: 'UBIGINT',
      default: '100',
      scope: 'GLOBAL'
    },
    {
      name: 'checkpoint_threshold',
      description: 'The WAL size threshold at which to automatically trigger a checkpoint (e.g., 1GB)',
      type: 'VARCHAR',
      default: '16.0 MiB',
      scope: 'GLOBAL'
    },
    {
      name: 'custom_extension_repository',
      description: 'Overrides the custom endpoint for remote extension installation',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'custom_user_agent',
      description: 'Metadata from DuckDB callers',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'default_block_size',
      description: 'The default block size for new duckdb database files (new as-in, they do not yet exist).',
      type: 'UBIGINT',
      default: '262144',
      scope: 'GLOBAL'
    },
    {
      name: 'default_collation',
      description: 'The collation setting used when none is specified',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'default_null_order',
      description: 'Null ordering used when none is specified (NULLS_FIRST, NULLS_LAST, NULLS_FIRST_ON_ASC_LAST_ON_DESC or NULLS_LAST_ON_ASC_FIRST_ON_DESC)',
      type: 'VARCHAR',
      default: 'NULLS_LAST',
      scope: 'GLOBAL'
    },
    {
      name: 'default_order',
      description: 'The order type used when none is specified (ASC or DESC)',
      type: 'VARCHAR',
      default: 'ASC',
      scope: 'GLOBAL'
    },
    {
      name: 'default_secret_storage',
      description: 'Allows switching the default storage for secrets',
      type: 'VARCHAR',
      default: 'local_file',
      scope: 'GLOBAL'
    },
    {
      name: 'disabled_filesystems',
      description: 'Disable specific file systems preventing access (e.g., LocalFileSystem)',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'duckdb_api',
      description: 'DuckDB API surface',
      type: 'VARCHAR',
      default: 'cli',
      scope: 'GLOBAL'
    },
    {
      name: 'enable_external_access',
      description: 'Allow the database to access external state (through e.g., loading/installing modules, COPY TO/FROM, CSV readers, pandas replacement scans, etc)',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'GLOBAL'
    },
    {
      name: 'enable_fsst_vectors',
      description: 'Allow scans on FSST compressed segments to emit compressed vectors to utilize late decompression',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'enable_http_metadata_cache',
      description: 'Whether or not the global http metadata is used to cache HTTP metadata',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'enable_macro_dependencies',
      description: 'Enable created MACROs to create dependencies on the referenced objects (such as tables)',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'enable_object_cache',
      description: 'Whether or not object cache is used to cache e.g., Parquet metadata',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'enable_server_cert_verification',
      description: 'Enable server side certificate verification.',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'enable_view_dependencies',
      description: 'Enable created VIEWs to create dependencies on the referenced objects (such as tables)',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'extension_directory',
      description: 'Set the directory to store extensions in',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'external_threads',
      description: 'The number of external threads that work on DuckDB tasks.',
      type: 'BIGINT',
      default: '1',
      scope: 'GLOBAL'
    },
    {
      name: 'force_download',
      description: 'Forces upfront download of file',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'http_keep_alive',
      description: 'Keep alive connections. Setting this to false can help when running into connection failures',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'GLOBAL'
    },
    {
      name: 'http_proxy_password',
      description: 'Password for HTTP proxy',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'http_proxy_username',
      description: 'Username for HTTP proxy',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'http_proxy',
      description: 'HTTP proxy host',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 'http_retries',
      description: 'HTTP retries on I/O error',
      type: 'UBIGINT',
      default: '3',
      scope: 'GLOBAL'
    },
    {
      name: 'http_retry_backoff',
      description: 'Backoff factor for exponentially increasing retry wait time',
      type: 'FLOAT',
      default: '4',
      scope: 'GLOBAL'
    },
    {
      name: 'http_retry_wait_ms',
      description: 'Time between retries',
      type: 'UBIGINT',
      default: '100',
      scope: 'GLOBAL'
    },
    {
      name: 'http_timeout',
      description: 'HTTP timeout read/write/connection/retry',
      type: 'UBIGINT',
      default: '30000',
      scope: 'GLOBAL'
    },
    {
      name: 'immediate_transaction_mode',
      description: 'Whether transactions should be started lazily when needed, or immediately when BEGIN TRANSACTION is called',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'index_scan_max_count',
      description: 'The maximum index scan count sets a threshold for index scans. If fewer than MAX(index_scan_max_count, index_scan_percentage * total_row_count) rows match, we perform an index scan instead of a table scan.',
      type: 'UBIGINT',
      default: '2048',
      scope: 'GLOBAL'
    },
    {
      name: 'index_scan_percentage',
      description: 'The index scan percentage sets a threshold for index scans. If fewer than MAX(index_scan_max_count, index_scan_percentage * total_row_count) rows match, we perform an index scan instead of a table scan.',
      type: 'DOUBLE',
      default: '0.001',
      scope: 'GLOBAL'
    },
    {
      name: 'lock_configuration',
      description: 'Whether or not the configuration can be altered',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'max_memory',
      description: 'The maximum memory of the system (e.g., 1GB)',
      type: 'VARCHAR',
      default: '80% of RAM',
      scope: 'GLOBAL'
    },
    {
      name: 'max_temp_directory_size',
      description: 'The maximum amount of data stored inside the \'temp_directory\'. The default value of 0 bytes is a placeholder to mean that the entire available disk space on that drive may be used. To actually limit the temp_directory  to 0 bytes, set temp_directory to NULL or the empty string.',
      type: 'VARCHAR',
      default: '0 bytes',
      scope: 'GLOBAL'
    },
    {
      name: 'max_vacuum_tasks',
      description: 'The maximum vacuum tasks to schedule during a checkpoint',
      type: 'UBIGINT',
      default: '100',
      scope: 'GLOBAL'
    },
    {
      name: 'old_implicit_casting',
      description: 'Allow implicit casting to/from VARCHAR',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 'password',
      description: 'The password to use. Ignored for legacy compatibility.',
      type: 'VARCHAR',
      default: 'NULL',
      scope: 'GLOBAL'
    },
    {
      name: 'preserve_insertion_order',
      description: 'Whether or not to preserve insertion order. If set to false the system is allowed to re-order any results that do not contain ORDER BY clauses.',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'GLOBAL'
    },
    {
      name: 'produce_arrow_string_view',
      description: 'Whether strings should be produced by DuckDB in Utf8View format instead of Utf8',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 's3_access_key_id',
      description: 'S3 Access Key ID',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 's3_endpoint',
      description: 'S3 Endpoint',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 's3_region',
      description: 'S3 Region',
      type: 'VARCHAR',
      default: 'us-east-1',
      scope: 'GLOBAL'
    },
    {
      name: 's3_secret_access_key',
      description: 'S3 Access Key',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 's3_session_token',
      description: 'S3 Session Token',
      type: 'VARCHAR',
      default: '',
      scope: 'GLOBAL'
    },
    {
      name: 's3_uploader_max_filesize',
      description: 'S3 Uploader max filesize (between 50GB and 5TB)',
      type: 'VARCHAR',
      default: '800GB',
      scope: 'GLOBAL'
    },
    {
      name: 's3_uploader_max_parts_per_file',
      description: 'S3 Uploader max parts per file (between 1 and 10000)',
      type: 'UBIGINT',
      default: '10000',
      scope: 'GLOBAL'
    },
    {
      name: 's3_uploader_thread_limit',
      description: 'S3 Uploader global thread limit',
      type: 'UBIGINT',
      default: '50',
      scope: 'GLOBAL'
    },
    {
      name: 's3_url_compatibility_mode',
      description: 'Disable Globs and Query Parameters on S3 URLs',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'GLOBAL'
    },
    {
      name: 's3_url_style',
      description: 'S3 URL style',
      type: 'VARCHAR',
      default: 'vhost',
      scope: 'GLOBAL'
    },
    {
      name: 's3_use_ssl',
      description: 'S3 use SSL',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'GLOBAL'
    },
    {
      name: 'secret_directory',
      description: 'Set the directory to which persistent secrets are stored',
      type: 'VARCHAR',
      default: '~/.duckdb/stored_secrets',
      scope: 'GLOBAL'
    },
    {
      name: 'storage_compatibility_version',
      description: 'Serialize on checkpoint with compatibility for a given duckdb version',
      type: 'VARCHAR',
      default: 'v0.10.2',
      scope: 'GLOBAL'
    },
    {
      name: 'temp_directory',
      description: 'Set the directory to which to write temp files. Set to NULL or empty string to disable.',
      type: 'VARCHAR',
      default: '⟨database_name⟩.tmp or .tmp (in in-memory mode)',
      scope: 'GLOBAL'
    },
    {
      name: 'threads',
      description: 'The number of total threads used by the system.',
      type: 'BIGINT',
      default: '# CPU cores',
      scope: 'GLOBAL'
    },
    {
      name: 'username',
      description: 'The username to use. Ignored for legacy compatibility.',
      type: 'VARCHAR',
      default: 'NULL',
      scope: 'GLOBAL'
    },
    {
      name: 'custom_profiling_settings',
      description: 'Accepts a JSON enabling custom metrics',
      type: 'VARCHAR',
      default: '{"RESULT_SET_SIZE": "true", "OPERATOR_TIMING": "true", "OPERATOR_ROWS_SCANNED": "true", "CUMULATIVE_ROWS_SCANNED": "true", "OPERATOR_CARDINALITY": "true", "OPERATOR_TYPE": "true", "CUMULATIVE_CARDINALITY": "true", "EXTRA_INFO": "true", "CPU_TIME": "true", "BLOCKED_THREAD_TIME": "true", "QUERY_NAME": "true"}',
      scope: 'LOCAL'
    },
    {
      name: 'enable_http_logging',
      description: 'Enables HTTP logging',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'LOCAL'
    },
    {
      name: 'enable_profiling',
      description: 'Enables profiling, and sets the output format (JSON, QUERY_TREE, QUERY_TREE_OPTIMIZER)',
      type: 'VARCHAR',
      default: 'NULL',
      scope: 'LOCAL'
    },
    {
      name: 'enable_progress_bar_print',
      description: 'Controls the printing of the progress bar, when \'enable_progress_bar\' is true',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'LOCAL'
    },
    {
      name: 'enable_progress_bar',
      description: 'Enables the progress bar, printing progress to the terminal for long queries',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'LOCAL'
    },
    {
      name: 'errors_as_json',
      description: 'Output error messages as structured JSON instead of as a raw string',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'LOCAL'
    },
    {
      name: 'explain_output',
      description: 'Output of EXPLAIN statements (ALL, OPTIMIZED_ONLY, PHYSICAL_ONLY)',
      type: 'VARCHAR',
      default: 'physical_only',
      scope: 'LOCAL'
    },
    {
      name: 'file_search_path',
      description: 'A comma separated list of directories to search for input files',
      type: 'VARCHAR',
      default: '',
      scope: 'LOCAL'
    },
    {
      name: 'home_directory',
      description: 'Sets the home directory used by the system',
      type: 'VARCHAR',
      default: '',
      scope: 'LOCAL'
    },
    {
      name: 'http_logging_output',
      description: 'The file to which HTTP logging output should be saved, or empty to print to the terminal',
      type: 'VARCHAR',
      default: '',
      scope: 'LOCAL'
    },
    {
      name: 'ieee_floating_point_ops',
      description: 'Use IEE754-compliant floating point operations (returning NAN instead of errors/NULL)',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'LOCAL'
    },
    {
      name: 'integer_division',
      description: 'Whether or not the / operator defaults to integer division, or to floating point division',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'LOCAL'
    },
    {
      name: 'log_query_path',
      description: 'Specifies the path to which queries should be logged (default: NULL, queries are not logged)',
      type: 'VARCHAR',
      default: 'NULL',
      scope: 'LOCAL'
    },
    {
      name: 'max_expression_depth',
      description: 'The maximum expression depth limit in the parser. WARNING: increasing this setting and using very deep expressions might lead to stack overflow errors.',
      type: 'UBIGINT',
      default: '1000',
      scope: 'LOCAL'
    },
    {
      name: 'merge_join_threshold',
      description: 'The number of rows we need on either table to choose a merge join',
      type: 'UBIGINT',
      default: '1000',
      scope: 'LOCAL'
    },
    {
      name: 'nested_loop_join_threshold',
      description: 'The number of rows we need on either table to choose a nested loop join',
      type: 'UBIGINT',
      default: '5',
      scope: 'LOCAL'
    },
    {
      name: 'order_by_non_integer_literal',
      description: 'Allow ordering by non-integer literals - ordering by such literals has no effect',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'LOCAL'
    },
    {
      name: 'ordered_aggregate_threshold',
      description: 'The number of rows to accumulate before sorting, used for tuning',
      type: 'UBIGINT',
      default: '262144',
      scope: 'LOCAL'
    },
    {
      name: 'partitioned_write_flush_threshold',
      description: 'The threshold in number of rows after which we flush a thread state when writing using PARTITION_BY',
      type: 'UBIGINT',
      default: '524288',
      scope: 'LOCAL'
    },
    {
      name: 'partitioned_write_max_open_files',
      description: 'The maximum amount of files the system can keep open before flushing to disk when writing using PARTITION_BY',
      type: 'UBIGINT',
      default: '100',
      scope: 'LOCAL'
    },
    {
      name: 'perfect_ht_threshold',
      description: 'Threshold in bytes for when to use a perfect hash table',
      type: 'BIGINT',
      default: '12',
      scope: 'LOCAL'
    },
    {
      name: 'pivot_filter_threshold',
      description: 'The threshold to switch from using filtered aggregates to LIST with a dedicated pivot operator',
      type: 'BIGINT',
      default: '20',
      scope: 'LOCAL'
    },
    {
      name: 'pivot_limit',
      description: 'The maximum number of pivot columns in a pivot statement',
      type: 'BIGINT',
      default: '100000',
      scope: 'LOCAL'
    },
    {
      name: 'prefer_range_joins',
      description: 'Force use of range joins with mixed predicates',
      type: 'BOOLEAN',
      default: 'false',
      scope: 'LOCAL'
    },
    {
      name: 'preserve_identifier_case',
      description: 'Whether or not to preserve the identifier case, instead of always lowercasing all non-quoted identifiers',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'LOCAL'
    },
    {
      name: 'profile_output',
      description: 'The file to which profile output should be saved, or empty to print to the terminal',
      type: 'VARCHAR',
      default: '',
      scope: 'LOCAL'
    },
    {
      name: 'profiling_mode',
      description: 'The profiling mode (STANDARD or DETAILED)',
      type: 'VARCHAR',
      default: 'NULL',
      scope: 'LOCAL'
    },
    {
      name: 'progress_bar_time',
      description: 'Sets the time (in milliseconds) how long a query needs to take before we start printing a progress bar',
      type: 'BIGINT',
      default: '2000',
      scope: 'LOCAL'
    },
    {
      name: 'scalar_subquery_error_on_multiple_rows',
      description: 'When a scalar subquery returns multiple rows - return a random row instead of returning an error',
      type: 'BOOLEAN',
      default: 'true',
      scope: 'LOCAL'
    },
    {
      name: 'schema',
      description: 'Sets the default search schema. Equivalent to setting search_path to a single value.',
      type: 'VARCHAR',
      default: 'main',
      scope: 'LOCAL'
    },
    {
      name: 'search_path',
      description: 'Sets the default catalog search path as a comma-separated list of values',
      type: 'VARCHAR',
      default: '',
      scope: 'LOCAL'
    },
    {
      name: 'streaming_buffer_size',
      description: 'The maximum memory to buffer between fetching from a streaming result (e.g., 1GB)',
      type: 'VARCHAR',
      default: '976.5 KiB',
      scope: 'LOCAL'
    }
  ];

  function handleAddSetting() {
    if (!newSettingKey || !newSettingValue) {
      validationErrors = { ...validationErrors, newSetting: 'Key and value are required' };
      return;
    }

    const settingInfo = duckdbSettings.find(s => s.name === newSettingKey);
    if (settingInfo) {
      if (!validateSettingValue(newSettingValue, settingInfo.type)) {
        validationErrors = { ...validationErrors, newSetting: `Invalid value for type ${settingInfo.type}` };
        return;
      }
    }

    settings = { ...settings, [newSettingKey]: newSettingValue };
    newSettingKey = '';
    newSettingValue = '';
    validationErrors = {};
    onUpdate(settings);
  }

  function handleRemoveSetting(key: string) {
    const { [key]: _, ...rest } = settings;
    settings = rest;
    onUpdate(settings);
  }

  function handleSettingChange(key: string, value: string) {
    const settingInfo = duckdbSettings.find(s => s.name === key);
    if (settingInfo) {
      if (!validateSettingValue(value, settingInfo.type)) {
        validationErrors = { ...validationErrors, [key]: `Invalid value for type ${settingInfo.type}` };
        return;
      }
    }
    settings = { ...settings, [key]: value };
    validationErrors = { ...validationErrors, [key]: '' };
    onUpdate(settings);
  }

  function validateSettingValue(value: string, type: string): boolean {
    if (type === 'BOOLEAN') {
      return value === 'true' || value === 'false';
    }
    if (type === 'BIGINT' || type === 'UBIGINT' || type === 'FLOAT' || type === 'DOUBLE') {
      return !isNaN(Number(value));
    }
    return true;
  }

  $: filteredSettings = duckdbSettings.filter(setting =>
    setting.name.toLowerCase().includes(searchQuery.toLowerCase()) ||
    setting.description.toLowerCase().includes(searchQuery.toLowerCase())
  );
</script>

<Card>
  <CardHeader>
    <CardTitle>DuckDB Settings</CardTitle>
  </CardHeader>
  <CardContent class="space-y-4">
    {#each Object.entries(settings) as [key, value]}
      <div class="flex items-center space-x-2">
        <div class="flex-1 space-y-1">
          <Label for={key}>{key}</Label>
          <Input
            id={key}
            bind:value={value}
            on:change={() => handleSettingChange(key, value)}
          />
          {#if validationErrors[key]}
            <p class="text-destructive text-sm">{validationErrors[key]}</p>
          {/if}
          {#if duckdbSettings.find(s => s.name === key)}
            <p class="text-sm text-muted-foreground">
              {duckdbSettings.find(s => s.name === key)?.description}
            </p>
            <p class="text-sm text-muted-foreground">
              Default: {duckdbSettings.find(s => s.name === key)?.default}
            </p>
            <p class="text-sm text-muted-foreground">
              Type: {duckdbSettings.find(s => s.name === key)?.type}
            </p>
          {/if}
        </div>
        <Button
          variant="ghost"
          size="icon"
          on:click={() => handleRemoveSetting(key)}
        >
          <Trash class="w-4 h-4" />
        </Button>
      </div>
    {/each}
    <Separator />
    <div class="flex space-x-2">
      <div class="flex-1 space-y-1">
        <Label for="new-setting-key">New Setting Key</Label>
        <Select bind:value={newSettingKey}>
          <SelectTrigger class="w-full">
            {newSettingKey ? newSettingKey : "Select a setting"}
          </SelectTrigger>
          <SelectContent>
            <Input
              type="text"
              placeholder="Search settings..."
              class="p-2 border-b"
              bind:value={searchQuery}
            />
            {#each filteredSettings as setting}
              <SelectItem value={setting.name}>
                <div class="flex flex-col">
                  <span class="font-medium">{setting.name}</span>
                  <span class="text-sm text-muted-foreground">{setting.description}</span>
                  <span class="text-sm text-muted-foreground">Default: {setting.default}</span>
                </div>
              </SelectItem>
            {/each}
          </SelectContent>
        </Select>
      </div>
      <div class="flex-1 space-y-1">
        <Label for="new-setting-value">New Setting Value</Label>
        <Input
          id="new-setting-value"
          bind:value={newSettingValue}
          placeholder="setting_value"
        />
      </div>
      <Button
        variant="ghost"
        size="icon"
        on:click={handleAddSetting}
      >
        <Plus class="w-4 h-4" />
      </Button>
    </div>
    {#if validationErrors.newSetting}
      <p class="text-destructive text-sm">{validationErrors.newSetting}</p>
    {/if}
  </CardContent>
</Card> 