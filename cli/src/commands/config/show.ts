import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { renderConfig } from '../../lib/render';
import chalk from 'chalk';

export function registerConfigCommand(program: Command, ctx: CliContext): Command {
  const config = program
    .command('config')
    .description('Configuration management commands');
  
  config
    .command('show')
    .description('Show current configuration')
    .action(() => {
      Console.info(chalk.cyan('\n⚙️  Configuration'));
      Console.info(chalk.gray('═'.repeat(60)));
      const currentConfig = ctx.config;
      renderConfig(currentConfig);
    });
  
  return config;
}

