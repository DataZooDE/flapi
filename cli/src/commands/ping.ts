import type { Command } from 'commander';
import type { CliContext } from '../lib/types';
import { Console } from '../lib/console';
import { handleError } from '../lib/errors';
import chalk from 'chalk';

export function registerPingCommand(program: Command, ctx: CliContext) {
  program
    .command('ping')
    .description('Ping the flapi server to check connectivity.')
    .action(async () => {
      const spinner = Console.spinner('Pinging flapi server...');
      try {
        const response = await ctx.client.get('/api/v1/_config/project');
        spinner.succeed(chalk.green('✓ Connected to flapi server'));
        const project = response.data;

        // Ping always shows table format for better UX
        Console.info(chalk.cyan('\n📊 Project Information'));
        Console.info(chalk.gray('═'.repeat(60)));
        Console.info(chalk.bold.blue('Name: ') + chalk.white(project?.name || 'N/A'));
        Console.info(chalk.bold.blue('Description: ') + chalk.white(project?.description || 'N/A'));
        Console.info(chalk.bold.blue('Base URL: ') + chalk.white(ctx.config.baseUrl));
      } catch (error) {
        spinner.fail(chalk.red('✗ Ping failed'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}

