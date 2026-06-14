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
            Console.info(chalk.bold.blue('TTL: ') + chalk.white(config.ttl || 'N/A'));
            Console.info(chalk.bold.blue('Max Size: ') + chalk.white(config.max_size || 'N/A'));
            Console.info(chalk.bold.blue('Strategy: ') + chalk.white(config.strategy || 'N/A'));
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
    .option('-t, --ttl <ttl>', 'Cache TTL in seconds')
    .option('-s, --max-size <size>', 'Maximum cache size')
    .option('--strategy <strategy>', 'Cache strategy (lru, ttl, etc.)')
    .option('-f, --file <file>', 'JSON file containing cache configuration')
    .option('--stdin', 'Read cache configuration from stdin')
    .action(async (path: string, options: {
      enabled?: string;
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
        if (options.ttl !== undefined) {
          cacheConfig.ttl = parseInt(options.ttl);
        }
        if (options.maxSize !== undefined) {
          cacheConfig.max_size = parseInt(options.maxSize);
        }
        if (options.strategy !== undefined) {
          cacheConfig.strategy = options.strategy;
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
        const template = response.data;

        if (ctx.config.output === 'json') {
          renderJson(template, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan(`\n📄 Cache Template: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.white(template));
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
          renderJson(result, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan(`\n🔄 Cache Refresh: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.green('✓ Cache refreshed successfully'));

          if (result.entries_cleared !== undefined) {
            Console.info(chalk.blue(`Entries cleared: ${result.entries_cleared}`));
          }
          if (result.cache_size !== undefined) {
            Console.info(chalk.blue(`New cache size: ${result.cache_size}`));
          }
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to refresh cache for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('gc <path>')
    .description('Run garbage collection on cache snapshots (DuckLake)')
    .action(async (path: string) => {
      const spinner = Console.spinner(`Running cache GC for ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'cache/gc');
        const response = await ctx.client.post(endpointUrl, {});
        spinner.succeed(chalk.green(`✓ Cache GC for ${path} completed`));

        if (ctx.config.output !== 'json') {
          Console.info(chalk.cyan(`\n🧹 Cache GC: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
        }
        renderJson(response.data, ctx.config.jsonStyle);
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to run cache GC for ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  cache
    .command('audit [path]')
    .description('Get cache sync audit log (for one endpoint, or all endpoints if no path given)')
    .action(async (path?: string) => {
      const target = path ? `for ${path}` : '(all endpoints)';
      const spinner = Console.spinner(`Fetching cache audit log ${target}...`);
      try {
        const url = path
          ? buildEndpointUrl(path, 'cache/audit')
          : '/api/v1/_config/cache/audit';
        const response = await ctx.client.get(url);
        spinner.succeed(chalk.green(`✓ Cache audit log ${target} retrieved`));

        if (ctx.config.output !== 'json') {
          Console.info(chalk.cyan(`\n📋 Cache Audit ${target}`));
          Console.info(chalk.gray('═'.repeat(60)));
        }
        renderJson(response.data, ctx.config.jsonStyle);
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to fetch cache audit log ${target}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
