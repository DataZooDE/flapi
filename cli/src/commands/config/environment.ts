import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson } from '../../lib/render';
import chalk from 'chalk';

export function registerEnvironmentCommand(config: Command, ctx: CliContext) {
  config
    .command('env')
    .description('Show environment variables exposed by the server')
    .action(async () => {
      const spinner = Console.spinner('Fetching environment variables...');
      try {
        const response = await ctx.client.get('/api/v1/_config/environment-variables');
        spinner.succeed(chalk.green('✓ Environment variables retrieved'));
        const payload = response.data;

        if (ctx.config.output === 'json') {
          renderJson(payload, ctx.config.jsonStyle);
          return;
        }

        Console.info(chalk.cyan('\n🌿 Environment Variables'));
        Console.info(chalk.gray('═'.repeat(60)));

        const variables = Array.isArray(payload?.variables) ? payload.variables : [];
        if (variables.length === 0) {
          Console.info(chalk.gray('No environment variables are configured.'));
          return;
        }

        variables.forEach((variable: any) => {
          const available = variable.available ? chalk.green('available') : chalk.red('missing');
          const value = variable.value ?? '';
          Console.info(`${chalk.bold(variable.name)} - ${available}`);
          if (value) {
            Console.info(chalk.gray(`  Value: ${value}`));
          }
        });
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch environment variables'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
