import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson, renderCacheTable } from '../../lib/render';
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
      let spinner;
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
          spinner.succeed(chalk.green('✓ Cache configurations retrieved'));
        }

        if (ctx.config.output === 'json') {
          renderJson(caches, ctx.config.jsonStyle);
        } else {
          renderCacheTable(caches);
        }
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch cache configurations'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('get <path>')
    .description('Get cache configuration for endpoint')
    .action(async (path: string) => {
      const spinner = Console.spinner(`Fetching cache config for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache');
        const response = await ctx.client.get(endpointUrl);
        spinner.succeed(chalk.green(`✓ Cache config for ${path} retrieved`));
        const cacheConfig = response.data;

        if (ctx.config.output === 'json') {
          renderJson(cacheConfig, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan(`\n💾 Cache Configuration: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));

          if (typeof cacheConfig === 'object' && cacheConfig !== null) {
            const config = cacheConfig as Record<string, any>;
            Console.info(chalk.bold.blue('Enabled: ') + chalk.white(config.enabled ? 'Yes' : 'No'));
            Console.info(chalk.bold.blue('Table: ') + chalk.white(config.table || 'N/A'));
            Console.info(chalk.bold.blue('Schema: ') + chalk.white(config.schema || 'N/A'));
            Console.info(chalk.bold.blue('Schedule: ') + chalk.white(config.schedule || 'N/A'));
            if (Array.isArray(config['primary-key'])) {
              Console.info(chalk.bold.blue('Primary Key: ') + chalk.white(config['primary-key'].join(', ')));
            }
            if (config.cursor) {
              Console.info(
                chalk.bold.blue('Cursor: ') +
                  chalk.white(`${config.cursor.column || 'N/A'} (${config.cursor.type || 'unknown'})`)
              );
            }
            if (config['rollback-window']) {
              Console.info(chalk.bold.blue('Rollback Window: ') + chalk.white(config['rollback-window']));
            }
            if (config.retention) {
              Console.info(chalk.bold.blue('Retention: ') + chalk.white(JSON.stringify(config.retention)));
            }
            if (config['delete-handling']) {
              Console.info(chalk.bold.blue('Delete Handling: ') + chalk.white(config['delete-handling']));
            }
            if (config['template-file']) {
              Console.info(chalk.bold.blue('Template File: ') + chalk.white(config['template-file']));
            }
          } else {
            Console.info(chalk.gray('No cache configuration found'));
          }
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to fetch cache config for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('update <path>')
    .description('Update cache configuration')
    .option('-e, --enabled <enabled>', 'Enable/disable caching (true/false)')
    .option('--table <table>', 'Cache table name')
    .option('--schema <schema>', 'Cache schema name')
    .option('--schedule <cron>', 'Cache refresh schedule or interval string')
    .option('--primary-key <columns...>', 'Primary key columns (space separated)')
    .option('--cursor-column <column>', 'Cursor column name')
    .option('--cursor-type <type>', 'Cursor data type')
    .option('--rollback-window <duration>', 'Rollback window (e.g. 6h)')
    .option('--retention-keep <count>', 'Number of snapshots to keep')
    .option('--retention-age <duration>', 'Max snapshot age (e.g. 7d)')
    .option('--delete-handling <mode>', 'Delete handling strategy')
    .option('--template-file <path>', 'Cache template SQL file')
    .option('-f, --file <file>', 'JSON file containing cache configuration')
    .option('--stdin', 'Read cache configuration from stdin')
    .action(async (path: string, options: {
      enabled?: string;
      table?: string;
      schema?: string;
      schedule?: string;
      primaryKey?: string[];
      cursorColumn?: string;
      cursorType?: string;
      rollbackWindow?: string;
      retentionKeep?: string;
      retentionAge?: string;
      deleteHandling?: string;
      templateFile?: string;
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
        if (options.table) {
          cacheConfig.table = options.table;
        }
        if (options.schema) {
          cacheConfig.schema = options.schema;
        }
        if (options.schedule) {
          cacheConfig.schedule = options.schedule;
        }
        if (options.primaryKey && options.primaryKey.length > 0) {
          cacheConfig['primary-key'] = options.primaryKey;
        }
        if ((options.cursorColumn && !options.cursorType) || (!options.cursorColumn && options.cursorType)) {
          Console.error('Both --cursor-column and --cursor-type must be provided together');
          process.exitCode = 1;
          return;
        }
        if (options.cursorColumn && options.cursorType) {
          cacheConfig.cursor = {
            column: options.cursorColumn,
            type: options.cursorType,
          };
        }
        if (options.rollbackWindow) {
          cacheConfig['rollback-window'] = options.rollbackWindow;
        }
        if (options.retentionKeep || options.retentionAge) {
          cacheConfig.retention = {};
          if (options.retentionKeep) {
            cacheConfig.retention['keep-last-snapshots'] = Number(options.retentionKeep);
          }
          if (options.retentionAge) {
            cacheConfig.retention['max-snapshot-age'] = options.retentionAge;
          }
        }
        if (options.deleteHandling) {
          cacheConfig['delete-handling'] = options.deleteHandling;
        }
        if (options.templateFile) {
          cacheConfig['template-file'] = options.templateFile;
        }

        if (Object.keys(cacheConfig).length === 0) {
          Console.error('No cache configuration provided. Use --file, --stdin, or individual options.');
          process.exitCode = 1;
          return;
        }
      }

      const spinner = Console.spinner(`Updating cache config for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache');
        await ctx.client.put(endpointUrl, cacheConfig);
        spinner.succeed(chalk.green(`✓ Cache config for ${path} updated successfully`));
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to update cache config for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('template <path>')
    .description('Get cache template for endpoint')
    .action(async (path: string) => {
      const spinner = Console.spinner(`Fetching cache template for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache/template');
        const response = await ctx.client.get(endpointUrl);
        spinner.succeed(chalk.green(`✓ Cache template for ${path} retrieved`));
        const template = typeof response.data === 'string' ? response.data : response.data?.template;

        if (ctx.config.output === 'json') {
          renderJson({ template: template ?? '' }, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan(`\n📄 Cache Template: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.white(template ?? ''));
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to fetch cache template for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('update-template <path>')
    .description('Update cache template')
    .option('-f, --file <file>', 'File containing cache template')
    .option('--stdin', 'Read cache template from stdin')
    .action(async (path: string, options: { file?: string; stdin?: boolean }) => {
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
        const endpointUrl = buildEndpointUrl(path, 'cache/template');
        await ctx.client.put(endpointUrl, {
          template: templateContent
        });
        spinner.succeed(chalk.green(`✓ Cache template for ${path} updated successfully`));
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to update cache template for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('refresh <path>')
    .description('Refresh cache for endpoint')
    .option('-f, --force', 'Force refresh even if not expired')
    .action(async (path: string, options: { force?: boolean }) => {
      const spinner = Console.spinner(`Refreshing cache for ${path}...`);
      try {
        const payload: any = {};
        if (options.force) {
          payload.force = true;
        }

        const endpointUrl = buildEndpointUrl(path, 'cache/refresh');
        const response = await ctx.client.post(endpointUrl, payload);
        spinner.succeed(chalk.green(`✓ Cache for ${path} refreshed successfully`));
        const result = response.data;

        if (ctx.config.output === 'json') {
          renderJson(result || { success: true }, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan(`\n🔄 Cache Refresh: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.green('✓ Cache refreshed successfully'));
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to refresh cache for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('gc <path>')
    .description('Run DuckLake garbage collection for endpoint cache')
    .action(async (path: string) => {
      const spinner = Console.spinner(`Running cache GC for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache/gc');
        await ctx.client.post(endpointUrl);
        spinner.succeed(chalk.green(`✓ Cache GC triggered for ${path}`));
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to run cache GC for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('audit <path>')
    .description('Show DuckLake audit log entries for an endpoint')
    .action(async (path: string) => {
      const spinner = Console.spinner(`Fetching cache audit for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache/audit');
        const response = await ctx.client.get(endpointUrl);
        spinner.succeed(chalk.green(`✓ Cache audit for ${path} retrieved`));
        renderJson(response.data, ctx.config.jsonStyle);
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to fetch cache audit for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('audit-all')
    .description('Show DuckLake audit log across all caches')
    .action(async () => {
      const spinner = Console.spinner('Fetching cache audit log...');
      try {
        const response = await ctx.client.get('/api/v1/_config/cache/audit');
        spinner.succeed(chalk.green('✓ Cache audit log retrieved'));
        renderJson(response.data, ctx.config.jsonStyle);
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch cache audit log'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
