import { Option, type Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson, renderConfigJson, renderCacheTable, renderTable } from '../../lib/render';
import { cacheAuditRows, describeRefreshResult, refreshQuery, skippedRefresh } from '../../lib/cacheRender';
import { applyOutputOverride } from '../../lib/options';
import { buildEndpointUrl } from '../../lib/url';
import chalk from 'chalk';

export function registerCacheCommands(program: Command, ctx: CliContext) {
  const cache = program
    .command('cache')
    .description('Cache management commands');

  cache
    .command('list')
    .description('List all cache configurations')
    .action(async () => {
      let spinner: ReturnType<typeof Console.spinner> | undefined;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Fetching cache configurations...');
      }

      try {
        // List cache configs by getting all endpoints and extracting cache info
        const endpointsResponse = await ctx.client.get('/api/v1/_config/endpoints');
        const endpoints = endpointsResponse.data;
        const caches: Record<string, any> = {};

        for (const [path, endpoint] of Object.entries(endpoints)) {
          if (typeof endpoint === 'object' && endpoint !== null) {
            const config = endpoint as Record<string, any>;
            if (config.cache) {
              caches[path] = config.cache;
            }
          }
        }

        if (ctx.config.output !== 'json') {
          spinner?.succeed(chalk.green('✓ Cache configurations retrieved'));
        }

        if (ctx.config.output === 'json') {
          renderConfigJson(caches, ctx.config.jsonStyle, 'keyed');
        } else {
          renderCacheTable(caches);
        }
      } catch (error) {
        spinner?.fail(chalk.red('✗ Failed to fetch cache configurations'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('get <path>')
    .option('--method <method>', 'HTTP method, when several endpoints share the path (GET + POST /orders)')
    .description('Get cache configuration for endpoint')
    .action(async (path: string, options: { method?: string }) => {
      const spinner = Console.spinner(`Fetching cache config for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache', options.method);
        const response = await ctx.client.get(endpointUrl);
        spinner?.succeed(chalk.green(`✓ Cache config for ${path} retrieved`));
        const cacheConfig = response.data;

        if (ctx.config.output === 'json') {
          renderConfigJson(cacheConfig, ctx.config.jsonStyle, 'document');
        } else {
          Console.info(chalk.cyan(`\n💾 Cache Configuration: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));

          if (typeof cacheConfig === 'object' && cacheConfig !== null) {
            const config = cacheConfig as Record<string, any>;
            Console.info(chalk.bold.blue('Enabled: ') + chalk.white(config.enabled ? 'Yes' : 'No'));
            Console.info(chalk.bold.blue('TTL: ') + chalk.white(config.ttl || 'N/A'));
            Console.info(chalk.bold.blue('Max Size: ') + chalk.white(config.max_size || 'N/A'));
            Console.info(chalk.bold.blue('Strategy: ') + chalk.white(config.strategy || 'N/A'));
          } else {
            Console.info(chalk.gray('No cache configuration found'));
          }
        }
      } catch (error) {
        spinner?.fail(chalk.red(`✗ Failed to fetch cache config for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('update <path>')
    .option('--method <method>', 'HTTP method, when several endpoints share the path (GET + POST /orders)')
    .description('Update cache configuration')
    .option('-e, --enabled <enabled>', 'Enable/disable caching (true/false)')
    .option('-s, --schedule <duration>', 'Refresh schedule, e.g. 5m, 6h')
    .option('--table <table>', 'Cache table name')
    .option('--schema <schema>', 'Cache schema name')
    .option('--ttl <ttl>', 'Not supported: flAPI has no TTL; use --schedule', undefined)
    .option('--max-size <size>', 'Not supported by the server')
    .option('--strategy <strategy>', 'Not supported by the server')
    .option('-f, --file <file>', 'JSON file containing cache configuration')
    .option('--stdin', 'Read cache configuration from stdin')
    .action(async (path: string, options: { method?: string;
      enabled?: string;
      schedule?: string;
      table?: string;
      schema?: string;
      ttl?: string;
      maxSize?: string;
      strategy?: string;
      file?: string;
      stdin?: boolean;
    }) => {
      let cacheConfig: any = {};

      if (options.file && options.stdin) {
        Console.error('Cannot specify both --file and --stdin');
        process.exitCode = 1;
        return;
      }

      if (options.file) {
        try {
          const fs = await import('node:fs');
          const configContent = fs.readFileSync(options.file, 'utf-8');
          cacheConfig = JSON.parse(configContent);
        } catch (error) {
          Console.error('Invalid JSON in --file');
          process.exitCode = 1;
          return;
        }
      } else if (options.stdin) {
        try {
          const chunks: string[] = [];
          process.stdin.setEncoding('utf-8');
          for await (const chunk of process.stdin) {
            chunks.push(chunk);
          }
          cacheConfig = JSON.parse(chunks.join(''));
        } catch (error) {
          Console.error('Invalid JSON from stdin');
          process.exitCode = 1;
          return;
        }
      } else {
        // Build config from individual options
        if (options.enabled !== undefined) {
          cacheConfig.enabled = options.enabled === 'true';
        }
        // The server reads enabled, table, schema, schedule, primary-key, cursor,
        // retention, ... It has no ttl / max_size / strategy: sending them was a
        // silent no-op (and, before the server was fixed, a 500). Refuse them.
        const unsupported = [
          options.ttl !== undefined ? '--ttl (use --schedule, e.g. --schedule 5m)' : '',
          options.maxSize !== undefined ? '--max-size' : '',
          options.strategy !== undefined ? '--strategy' : '',
        ].filter(Boolean);
        if (unsupported.length > 0) {
          Console.error(`Unsupported option(s): ${unsupported.join(', ')}. flAPI caches are configured with --schedule, --table, --schema, --enabled or --file.`);
          process.exitCode = 1;
          return;
        }
        if (options.schedule !== undefined) {
          cacheConfig.schedule = options.schedule;
        }
        if (options.table !== undefined) {
          cacheConfig.table = options.table;
        }
        if (options.schema !== undefined) {
          cacheConfig.schema = options.schema;
        }

        if (Object.keys(cacheConfig).length === 0) {
          Console.error('No cache configuration provided. Use --file, --stdin, or individual options.');
          process.exitCode = 1;
          return;
        }
      }

      const spinner = Console.spinner(`Updating cache config for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache', options.method);
        await ctx.client.put(endpointUrl, cacheConfig);
        spinner?.succeed(chalk.green(`✓ Cache config for ${path} updated successfully`));
      } catch (error) {
        spinner?.fail(chalk.red(`✗ Failed to update cache config for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('template <path>')
    .option('--method <method>', 'HTTP method, when several endpoints share the path (GET + POST /orders)')
    .description('Get cache template for endpoint')
    .action(async (path: string, options: { method?: string }) => {
      const spinner = Console.spinner(`Fetching cache template for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache/template', options.method);
        const response = await ctx.client.get(endpointUrl);
        spinner?.succeed(chalk.green(`✓ Cache template for ${path} retrieved`));
        const template = response.data;

        if (ctx.config.output === 'json') {
          renderJson(template);
        } else {
          Console.info(chalk.cyan(`\n📄 Cache Template: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.white(template));
        }
      } catch (error) {
        spinner?.fail(chalk.red(`✗ Failed to fetch cache template for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('update-template <path>')
    .option('--method <method>', 'HTTP method, when several endpoints share the path (GET + POST /orders)')
    .description('Update cache template')
    .option('-f, --file <file>', 'File containing cache template')
    .option('--stdin', 'Read cache template from stdin')
    .action(async (path: string, options: { method?: string; file?: string; stdin?: boolean }) => {
      if (options.file && options.stdin) {
        Console.error('Cannot specify both --file and --stdin');
        process.exitCode = 1;
        return;
      }

      if (!options.file && !options.stdin) {
        Console.error('Must specify either --file or --stdin');
        process.exitCode = 1;
        return;
      }

      let templateContent: string;

      if (options.file) {
        const fs = await import('node:fs');
        templateContent = fs.readFileSync(options.file, 'utf-8');
      } else {
        const chunks: string[] = [];
        process.stdin.setEncoding('utf-8');
        for await (const chunk of process.stdin) {
          chunks.push(chunk);
        }
        templateContent = chunks.join('');
      }

      const spinner = Console.spinner(`Updating cache template for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache/template', options.method);
        await ctx.client.put(endpointUrl, {
          template: templateContent
        });
        spinner?.succeed(chalk.green(`✓ Cache template for ${path} updated successfully`));
      } catch (error) {
        spinner?.fail(chalk.red(`✗ Failed to update cache template for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('refresh <path>')
    .option('--method <method>', 'HTTP method, when several endpoints share the path (GET + POST /orders)')
    .description('Refresh cache for endpoint (--full: rebuild from scratch, erasing the watermark)')
    .option('--full', 'Full rebuild: ignore the incremental watermark and repopulate the whole table')
    // Kept so existing scripts do not break, but hidden: the server never had a
    // "force" flag (it sent {force:true}, which was ignored). It is a normal
    // refresh; --full is the one that changes what happens.
    .addOption(new Option('-f, --force', 'Deprecated: same as a normal refresh').hideHelp())
    .action(async (path: string, options: { method?: string; full?: boolean; force?: boolean }) => {
      if (options.force && !options.full) {
        Console.warn('--force is deprecated and does nothing extra; use --full for a full rebuild');
      }
      const spinner = Console.spinner(`Refreshing cache for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache/refresh', options.method);
        const query = refreshQuery(options);
        const response = await ctx.client.post(endpointUrl, {}, query ? { params: query } : undefined);
        const result = response.data;
        spinner?.succeed(chalk.green(`✓ Cache for ${path} refreshed successfully`));
        const lines = describeRefreshResult(result);

        if (ctx.config.output === 'json') {
          // An empty body is not data; say what happened instead of printing "".
          renderJson(result && typeof result === 'object' ? result : { path, refreshed: true, full: Boolean(options.full) });
        } else {
          Console.info(chalk.cyan(`\n🔄 Cache Refresh: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.green(options.full ? '✓ Cache fully rebuilt' : '✓ Cache refreshed successfully'));
          lines.forEach((line) => Console.info(chalk.blue(line)));
        }
      } catch (error) {
        // Another refresh of the same table (warm-up, the scheduler) is in
        // flight: the server answers 409 + Retry-After and starts nothing.
        const skipped = skippedRefresh(error);
        if (skipped) {
          const wait = skipped.retryAfter ? `; retry in ${skipped.retryAfter}s` : '';
          spinner?.warn(chalk.yellow(`A refresh of ${path} is already running; not started again${wait}`));
          if (ctx.config.output === 'json') {
            renderJson(skipped.body);
          }
          process.exitCode = 1;
          return;
        }
        spinner?.fail(chalk.red(`✗ Failed to refresh cache for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('gc <path>')
    .option('--method <method>', 'HTTP method, when several endpoints share the path (GET + POST /orders)')
    .description('Run garbage collection on cache snapshots (DuckLake)')
    .action(async (path: string, options: { method?: string }) => {
      const spinner = Console.spinner(`Running cache GC for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache/gc', options.method);
        const response = await ctx.client.post(endpointUrl, {});
        spinner?.succeed(chalk.green(`✓ Cache GC for ${path} completed`));

        if (ctx.config.output !== 'json') {
          Console.info(chalk.cyan(`\n🧹 Cache GC: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
        }
        renderJson(response.data);
      } catch (error) {
        spinner?.fail(chalk.red(`✗ Failed to run cache GC for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('drop-table <table>')
    .description(
      'Drop an obsolete cache table ([schema.]table) that no endpoint uses any more, ' +
        'e.g. after changing cache.schema; its snapshots are expired and its files deleted',
    )
    .action(async (table: string) => {
      const dot = table.indexOf('.');
      const body = dot < 0 ? { table } : { schema: table.slice(0, dot), table: table.slice(dot + 1) };
      const spinner = Console.spinner(`Dropping cache table ${table}...`);
      try {
        const response = await ctx.client.post('/api/v1/_config/cache/drop-table', body);
        spinner?.succeed(chalk.green(`✓ Dropped cache table ${table}`));
        renderJson(response.data);
      } catch (error) {
        spinner?.fail(chalk.red(`✗ Failed to drop cache table ${table}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('compact [path]')
    .option('--method <method>', 'HTTP method, when several endpoints on the path have a cache')
    .description(
      'Run DuckLake compaction now (merge small files, rewrite delete-heavy ones, delete the replaced files): ' +
        'the whole catalog, or only the cache table of [path]',
    )
    .action(async (path: string | undefined, options: { method?: string }) => {
      const target = path ? `for ${path}` : 'for the catalog';
      const body: Record<string, string> = {};
      if (path) {
        body.endpoint = path.startsWith('/') ? path : `/${path}`;
        if (options.method) {
          body.method = options.method.toUpperCase();
        }
      }
      const spinner = Console.spinner(`Compacting ${target}...`);
      try {
        const response = await ctx.client.post('/api/v1/_config/cache/compact', body);
        spinner?.succeed(chalk.green(`✓ Compacted ${target}`));
        renderJson(response.data);
      } catch (error) {
        spinner?.fail(chalk.red(`✗ Failed to compact ${target}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('audit [path]')
    .option('--method <method>', 'HTTP method, when several endpoints share the path (GET + POST /orders)')
    .description('Get cache sync audit log (for one endpoint, one cache table with --table, or all)')
    .option('--table <table>', 'One cache table ([schema.]table), whichever endpoint sharing it refreshed it')
    .option('--output <format>', 'Output format: json or table')
    .action(async (path: string | undefined, options: { method?: string; table?: string; output?: 'json' | 'table' }) => {
      const target = path ? `for ${path}` : options.table ? `for table ${options.table}` : '(all endpoints)';
      const spinner = Console.spinner(`Fetching cache audit log ${target}...`);
      try {
        const url = path
          ? buildEndpointUrl(path, 'cache/audit', options.method)
          : options.table
            ? `/api/v1/_config/cache/audit?table=${encodeURIComponent(options.table)}`
            : '/api/v1/_config/cache/audit';
        const response = await ctx.client.get(url);
        spinner?.succeed(chalk.green(`✓ Cache audit log ${target} retrieved`));

        const resolved = applyOutputOverride(ctx.config, options.output);
        if (resolved.output === 'json') {
          renderJson(response.data);
          return;
        }
        // `-o table` printed the JSON too; it is a table of the refreshes now.
        Console.info(chalk.cyan(`\n📋 Cache Audit ${target}`));
        Console.info(chalk.gray('═'.repeat(60)));
        renderTable(cacheAuditRows(response.data, !path));
      } catch (error) {
        spinner?.fail(chalk.red(`✗ Failed to fetch cache audit log ${target}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
