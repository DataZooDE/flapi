import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import chalk from 'chalk';

/**
 * Valid log levels for flapi backend
 */
const VALID_LOG_LEVELS = ['debug', 'info', 'warning', 'error'] as const;
type LogLevel = typeof VALID_LOG_LEVELS[number];

/**
 * Accepted spellings that are not canonical level names. `warn` is what the
 * --log-level flag, FLAPI_LOG_LEVEL and `log-level:` in flapi.yaml accept.
 */
const LOG_LEVEL_ALIASES: Record<string, LogLevel> = { warn: 'warning' };

/** The canonical level for `input` (case-insensitive, aliases resolved), or undefined. */
export function normalizeLogLevel(input: string): LogLevel | undefined {
  const lower = input.trim().toLowerCase();
  if ((VALID_LOG_LEVELS as readonly string[]).includes(lower)) {
    return lower as LogLevel;
  }
  return LOG_LEVEL_ALIASES[lower];
}

/**
 * Register log-level subcommands under config command
 */
export function registerLogLevelCommands(configCmd: Command, ctx: CliContext) {
  const logLevel = configCmd
    .command('log-level')
    .description('Manage backend runtime log level');

  // Get current log level
  logLevel
    .command('get')
    .description('Get current backend log level')
    .action(async () => {
      try {
        const response = await ctx.client.get('/api/v1/_config/log-level');
        const currentLevel = response.data.level;
        
        Console.info(chalk.cyan('\n📊 Current Log Level'));
        Console.info(chalk.gray('═'.repeat(60)));
        Console.info(`${chalk.bold('Level:')} ${chalk.yellow(currentLevel)}`);
        Console.info('');
      } catch (error: any) {
        if (error.response?.status === 401) {
          Console.error('Authentication failed. Config service token required.');
          Console.info('Set token with --config-service-token or FLAPI_CONFIG_SERVICE_TOKEN env var');
        } else {
          Console.error('Failed to get log level:', error.message);
        }
        process.exit(1);
      }
    });

  // Set log level
  logLevel
    .command('set <level>')
    .description('Set backend log level (debug, info, warning|warn, error)')
    .action(async (input: string) => {
      // Validate log level
      const level = normalizeLogLevel(input);
      if (!level) {
        Console.error(`Invalid log level: ${input}`);
        Console.info(`Valid levels: ${VALID_LOG_LEVELS.join(', ')} (alias: warn = warning)`);
        process.exit(1);
      }

      try {
        const response = await ctx.client.put('/api/v1/_config/log-level', {
          level: level
        });

        Console.success(chalk.green('\n✓ Log level updated'));
        Console.info(chalk.gray('═'.repeat(60)));
        Console.info(`${chalk.bold('New level:')} ${chalk.yellow(response.data.level)}`);
        Console.info(`${chalk.bold('Message:')} ${response.data.message}`);
        Console.info('');
        Console.info(chalk.dim('Note: Log level will reset on server restart'));
      } catch (error: any) {
        if (error.response?.status === 401) {
          Console.error('Authentication failed. Config service token required.');
          Console.info('Set token with --config-service-token or FLAPI_CONFIG_SERVICE_TOKEN env var');
        } else if (error.response?.status === 400) {
          Console.error('Invalid log level:', error.response.data);
        } else {
          Console.error('Failed to set log level:', error.message);
        }
        process.exit(1);
      }
    });

  // Alias for convenience
  logLevel
    .command('list')
    .description('List valid log levels')
    .action(() => {
      Console.info(chalk.cyan('\n📋 Valid Log Levels'));
      Console.info(chalk.gray('═'.repeat(60)));
      VALID_LOG_LEVELS.forEach(level => {
        let description = '';
        switch (level) {
          case 'debug':
            description = 'Verbose output for debugging';
            break;
          case 'info':
            description = 'General informational messages';
            break;
          case 'warning':
            description = 'Warning messages only (alias: warn)';
            break;
          case 'error':
            description = 'Error messages only';
            break;
        }
        Console.info(`  ${chalk.yellow(level.padEnd(10))} ${chalk.dim(description)}`);
      });
      Console.info('');
      Console.info(chalk.dim('Usage: flapii config log-level set <level>'));
    });
}

