import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson } from '../../lib/render';
import chalk from 'chalk';

export function registerInfoCommands(config: Command, ctx: CliContext) {
  config
    .command('env')
    .description('List whitelisted environment variables and their availability')
    .action(async () => {
      let spinner;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Fetching environment variables...');
      }
      try {
        const response = await ctx.client.get('/api/v1/_config/environment-variables');
        if (spinner) {
          spinner.succeed(chalk.green('✓ Environment variables retrieved'));
        }
        const data = response.data;

        if (ctx.config.output === 'json') {
          renderJson(data, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan('\n🔐 Environment Variables'));
          Console.info(chalk.gray('═'.repeat(60)));
          const vars = Array.isArray(data?.variables) ? data.variables : [];
          if (vars.length === 0) {
            Console.info(chalk.gray('No environment variables configured'));
          } else {
            vars.forEach((v: any) => {
              const status = v.available ? chalk.green('available') : chalk.yellow('not set');
              Console.info(chalk.bold.blue(v.name) + ' ' + status);
            });
          }
        }
      } catch (error) {
        if (spinner) {
          spinner.fail(chalk.red('✗ Failed to fetch environment variables'));
        }
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  config
    .command('filesystem')
    .description('Show the project filesystem structure')
    .action(async () => {
      let spinner;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Fetching filesystem structure...');
      }
      try {
        const response = await ctx.client.get('/api/v1/_config/filesystem');
        if (spinner) {
          spinner.succeed(chalk.green('✓ Filesystem structure retrieved'));
        }
        const data = response.data;

        if (ctx.config.output !== 'json') {
          Console.info(chalk.cyan('\n📁 Project Filesystem'));
          Console.info(chalk.gray('═'.repeat(60)));
        }
        renderJson(data, ctx.config.jsonStyle);
      } catch (error) {
        if (spinner) {
          spinner.fail(chalk.red('✗ Failed to fetch filesystem structure'));
        }
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
