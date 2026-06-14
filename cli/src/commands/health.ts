import type { Command } from 'commander';
import type { CliContext } from '../lib/types';
import { Console } from '../lib/console';
import { handleError } from '../lib/errors';
import { renderJson } from '../lib/render';
import chalk from 'chalk';

export function registerHealthCommand(program: Command, ctx: CliContext) {
  program
    .command('health')
    .description('Get server health (database, endpoints, Arrow IPC, VFS, credential status)')
    .action(async () => {
      let spinner;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Fetching server health...');
      }
      try {
        const response = await ctx.client.get('/api/v1/_config/health');
        if (spinner) {
          spinner.succeed(chalk.green('✓ Server health retrieved'));
        }
        const data = response.data;

        if (ctx.config.output === 'json') {
          renderJson(data, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan('\n❤️  Server Health'));
          Console.info(chalk.gray('═'.repeat(60)));
          const status = data?.status ?? 'unknown';
          const healthy = ['ok', 'healthy', 'up'].includes(String(status).toLowerCase());
          Console.info(
            chalk.bold.blue('Status: ') + (healthy ? chalk.green(status) : chalk.yellow(status)),
          );
          renderJson(data, ctx.config.jsonStyle);
        }
      } catch (error) {
        if (spinner) {
          spinner.fail(chalk.red('✗ Failed to fetch server health'));
        }
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
