import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson, renderEndpointsTable, renderEndpointTable } from '../../lib/render';
import { buildEndpointUrl } from '../../lib/url';
import { applyOutputOverride } from '../../lib/options';
import { resolvePayload, withPayloadOptions } from '../payload';
import chalk from 'chalk';

export function registerEndpointCommands(program: Command, ctx: CliContext) {
  const endpoints = program
    .command('endpoints')
    .description('Endpoint management commands');

  endpoints
    .command('list')
    .description('List all endpoints')
    .option('--output <format>', 'Output format: json or table')
    .action(async (options: { output?: 'json' | 'table' }) => {
      const spinner = Console.spinner('Fetching endpoints...');
      try {
        const response = await ctx.client.get('/api/v1/_config/endpoints');
        spinner.succeed(chalk.green('✓ Endpoints retrieved'));
        const payload = response.data;
        const resolved = applyOutputOverride(ctx.config, options.output);
        if (resolved.output === 'json') {
          renderJson(payload, resolved.jsonStyle);
        } else {
          renderEndpointsTable(payload);
        }
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch endpoints'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  endpoints
    .command('get <path>')
    .description('Get endpoint configuration')
    .option('--output <format>', 'Output format: json or table')
    .action(async (path: string, options: { output?: 'json' | 'table' }) => {
      const spinner = Console.spinner(`Fetching endpoint ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path);
        const response = await ctx.client.get(endpointUrl);
        spinner.succeed(chalk.green(`✓ Endpoint ${path} retrieved`));
        const payload = response.data;
        const resolved = applyOutputOverride(ctx.config, options.output);
        if (resolved.output === 'json') {
          renderJson(payload, resolved.jsonStyle);
        } else {
          renderEndpointTable(payload);
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to fetch endpoint ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  withPayloadOptions(
    endpoints
      .command('create')
      .description('Create a new endpoint')
  )
    .action(async (options: { file?: string; stdin?: boolean }) => {
      const spinner = Console.spinner('Creating endpoint...');
      try {
        const payload = await resolvePayload(options);
        await ctx.client.post('/api/v1/_config/endpoints', payload);
        spinner.succeed(chalk.green('✓ Endpoint created successfully'));
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to create endpoint'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  withPayloadOptions(
    endpoints
      .command('update <path>')
      .description('Update an existing endpoint')
  )
    .action(async (path: string, options: { file?: string; stdin?: boolean }) => {
      const spinner = Console.spinner(`Updating endpoint ${path}...`);
      try {
        const payload = await resolvePayload(options);
        const endpointUrl = buildEndpointUrl(path);
        await ctx.client.put(endpointUrl, payload);
        spinner.succeed(chalk.green(`✓ Endpoint ${path} updated successfully`));
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to update endpoint ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  endpoints
    .command('delete <path>')
    .description('Delete an endpoint')
    .option('--force', 'Skip confirmation prompt')
    .action(async (path: string, options: { force?: boolean }) => {
      if (!options.force && !ctx.config.yes) {
        Console.warn(`Use --force or --yes to delete endpoint ${path}`);
        process.exitCode = 1;
        return;
      }

      const spinner = Console.spinner(`Deleting endpoint ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path);
        await ctx.client.delete(endpointUrl);
        spinner.succeed(chalk.green(`✓ Endpoint ${path} deleted successfully`));
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to delete endpoint ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}

function ensurePayloadOptions() {
  // placeholder for shared payload validation hook if needed later
}

