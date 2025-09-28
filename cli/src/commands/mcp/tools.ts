import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson, renderMcpToolsTable } from '../../lib/render';
import chalk from 'chalk';

export function registerMcpToolsCommand(parent: Command, ctx: CliContext) {
  const tools = parent.command('tools').description('MCP tools commands');

  tools
    .command('list')
    .description('List MCP tools')
    .action(async () => {
      const spinner = Console.spinner('Fetching MCP tools...');
      try {
        const response = await ctx.client.get('/api/v1/_config/endpoints');
        spinner.succeed(chalk.green('✓ MCP tools retrieved'));
        const payload = response.data;
        const tools = Object.entries(payload)
          .map(([path, config]) => ({ path, config }))
          .filter((entry) => entry.config?.['mcp-tool'] ?? entry.config?.mcpTool);

        if (ctx.config.output === 'json') {
          renderJson(tools.map((entry) => entry.config), ctx.config.jsonStyle);
        } else {
          renderMcpToolsTable(tools);
        }
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch MCP tools'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  tools
    .command('get <name>')
    .description('Get MCP tool configuration')
    .action(async (name: string) => {
      const spinner = Console.spinner(`Fetching MCP tool ${name}...`);
      try {
        const response = await ctx.client.get('/api/v1/_config/endpoints');
        spinner.succeed(chalk.green(`✓ MCP tool ${name} retrieved`));
        const tools = Object.values(response.data ?? {}) as Record<string, unknown>[];
        const tool = tools.find((candidate) => {
          const config = candidate['mcp-tool'] ?? candidate['mcpTool'];
          return config && typeof config === 'object' && (config as Record<string, unknown>).name === name;
        });

        if (!tool) {
          Console.warn(chalk.yellow(`⚠ Tool '${name}' not found`));
          process.exitCode = 1;
          return;
        }

        if (ctx.config.output === 'json') {
          renderJson(tool, ctx.config.jsonStyle);
        } else {
          const toolConfig = tool['mcp-tool'] ?? tool['mcpTool'] ?? tool;
          Console.info(chalk.cyan(`\n🔧 MCP Tool: ${name}`));
          Console.info(chalk.gray('═'.repeat(60)));
          renderJson(toolConfig, ctx.config.jsonStyle);
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to fetch MCP tool ${name}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
