import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson } from '../../lib/render';
import { registerInitCommand } from './init';
import chalk from 'chalk';

export function registerProjectCommands(program: Command, ctx: CliContext) {
  const project = program
    .command('project')
    .description('Project configuration and initialization commands');

  // Register get command
  project
    .command('get')
    .description('Get project configuration')
    .action(async () => {
      const spinner = Console.spinner('Fetching project configuration...');
      try {
        const result = await ctx.client.get('/api/v1/_config/project');
        spinner.succeed(chalk.green('✓ Project configuration retrieved'));
        const projectData = result.data;

        if (ctx.config.output === 'json') {
          renderJson(projectData, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan('\n📊 Project Information'));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.bold.blue('Name: ') + chalk.white(projectData?.name || 'N/A'));
          Console.info(chalk.bold.blue('Description: ') + chalk.white(projectData?.description || 'N/A'));
          Console.info(chalk.bold.blue('Base URL: ') + chalk.white(ctx.config.baseUrl));
        }
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch project configuration'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  // Register init command
  registerInitCommand(project, ctx);
}

