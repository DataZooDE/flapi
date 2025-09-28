import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { renderConfig } from '../../lib/render';
import chalk from 'chalk';

export function registerConfigCommand(program: Command, ctx: CliContext) {
  program
    .command('config')
    .description('Show current configuration')
    .action(() => {
      Console.info(chalk.cyan('\n⚙️  Configuration'));
      Console.info(chalk.gray('═'.repeat(60)));
      const config = ctx.config;
      renderConfig(config);
    });
}

