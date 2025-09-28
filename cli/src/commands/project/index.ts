import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson } from '../../lib/render';
import chalk from 'chalk';

export function registerProjectCommands(program: Command, ctx: CliContext) {
  program
    .command('project')
    .description('Project configuration commands')
    .command('get')
    .description('Get project configuration')
    .action(async () => {
      const spinner = Console.spinner('Fetching project configuration...');
      try {
        const result = await ctx.client.get('/api/v1/_config/project');
        spinner.succeed(chalk.green('✓ Project configuration retrieved'));
        const project = result.data;

        if (ctx.config.output === 'json') {
          renderJson(project, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan('\n📊 Project Information'));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.bold.blue('Name: ') + chalk.white(project?.name || 'N/A'));
          Console.info(chalk.bold.blue('Description: ') + chalk.white(project?.description || 'N/A'));
          Console.info(chalk.bold.blue('Base URL: ') + chalk.white(ctx.config.baseUrl));
        }
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch project configuration'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}

