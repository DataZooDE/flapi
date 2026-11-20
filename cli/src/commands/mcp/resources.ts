import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson, renderConfigJson, renderMcpResourcesTable } from '../../lib/render';
import chalk from 'chalk';

export function registerMcpResourcesCommand(parent: Command, ctx: CliContext) {
  const resources = parent.command('resources').description('MCP resources commands');

  resources
    .command('list')
    .description('List MCP resources')
    .action(async () => {
      const spinner = Console.spinner('Fetching MCP resources...');
      try {
        const response = await ctx.client.get('/api/v1/_config/endpoints');
        spinner.succeed(chalk.green('✓ MCP resources retrieved'));
        const payload = response.data;
        const resources = Object.entries(payload)
          .map(([path, config]) => ({ path, config: config as Record<string, unknown> }))
          .filter((entry) => entry.config?.['mcp-resource'] ?? entry.config?.mcpResource);

        if (ctx.config.output === 'json') {
          renderConfigJson(resources.map((entry) => entry.config), ctx.config.jsonStyle, 'document');
        } else {
          renderMcpResourcesTable(resources);
        }
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch MCP resources'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  resources
    .command('get <name>')
    .description('Get MCP resource configuration')
    .action(async (name: string) => {
      const spinner = Console.spinner(`Fetching MCP resource ${name}...`);
      try {
        const response = await ctx.client.get('/api/v1/_config/endpoints');
        spinner.succeed(chalk.green(`✓ MCP resource ${name} retrieved`));
        const resources = Object.values(response.data ?? {}) as Record<string, unknown>[];
        const resource = resources.find((candidate) => {
          const config = candidate['mcp-resource'] ?? candidate['mcpResource'];
          return config && typeof config === 'object' && (config as Record<string, unknown>).name === name;
        });

        if (!resource) {
          Console.warn(chalk.yellow(`⚠ Resource '${name}' not found`));
          process.exitCode = 1;
          return;
        }

        if (ctx.config.output === 'json') {
          renderConfigJson(resource, ctx.config.jsonStyle, 'document');
        } else {
          const resourceConfig = resource['mcp-resource'] ?? resource['mcpResource'] ?? resource;
          Console.info(chalk.cyan(`\n📁 MCP Resource: ${name}`));
          Console.info(chalk.gray('═'.repeat(60)));
          renderConfigJson(resourceConfig, ctx.config.jsonStyle, 'document');
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to fetch MCP resource ${name}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
