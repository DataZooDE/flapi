import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson, renderMcpPromptsTable } from '../../lib/render';
import chalk from 'chalk';

export function registerMcpPromptsCommand(parent: Command, ctx: CliContext) {
  const prompts = parent.command('prompts').description('MCP prompts commands');

  prompts
    .command('list')
    .description('List MCP prompts')
    .action(async () => {
      const spinner = Console.spinner('Fetching MCP prompts...');
      try {
        const response = await ctx.client.get('/api/v1/_config/endpoints');
        spinner.succeed(chalk.green('✓ MCP prompts retrieved'));
        const payload = response.data;
        const prompts = Object.entries(payload)
          .map(([path, config]) => ({ path, config }))
          .filter((entry) => entry.config?.['mcp-prompt'] ?? entry.config?.mcpPrompt);

        if (ctx.config.output === 'json') {
          renderJson(prompts.map((entry) => entry.config), ctx.config.jsonStyle);
        } else {
          renderMcpPromptsTable(prompts);
        }
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch MCP prompts'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  prompts
    .command('get <name>')
    .description('Get MCP prompt configuration')
    .action(async (name: string) => {
      const spinner = Console.spinner(`Fetching MCP prompt ${name}...`);
      try {
        const response = await ctx.client.get('/api/v1/_config/endpoints');
        spinner.succeed(chalk.green(`✓ MCP prompt ${name} retrieved`));
        const prompts = Object.values(response.data ?? {}) as Record<string, unknown>[];
        const prompt = prompts.find((candidate) => {
          const config = candidate['mcp-prompt'] ?? candidate['mcpPrompt'];
          return config && typeof config === 'object' && (config as Record<string, unknown>).name === name;
        });

        if (!prompt) {
          Console.warn(chalk.yellow(`⚠ Prompt '${name}' not found`));
          process.exitCode = 1;
          return;
        }

        if (ctx.config.output === 'json') {
          renderJson(prompt, ctx.config.jsonStyle);
        } else {
          const promptConfig = prompt['mcp-prompt'] ?? prompt['mcpPrompt'] ?? prompt;
          Console.info(chalk.cyan(`\n💬 MCP Prompt: ${name}`));
          Console.info(chalk.gray('═'.repeat(60)));
          renderJson(promptConfig, ctx.config.jsonStyle);
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to fetch MCP prompt ${name}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
