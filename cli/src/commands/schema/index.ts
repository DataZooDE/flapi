import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson } from '../../lib/render';
import chalk from 'chalk';

export function registerSchemaCommands(program: Command, ctx: CliContext) {
  const schema = program
    .command('schema')
    .description('Schema introspection commands');

  schema
    .command('get')
    .description('Get database schema information')
    .option('--connection <connection>', 'Specific connection to introspect')
    .option('--table <table>', 'Specific table to introspect')
    .option('-d, --database <database>', 'Specific database to introspect')
    .action(async (options: {
      connection?: string;
      table?: string;
      database?: string;
    }) => {
      let spinner;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Fetching schema information...');
      }

      try {
        let url = '/api/v1/_config/schema';
        const params = new URLSearchParams();

        if (options.connection) {
          params.append('connection', options.connection);
        }
        if (options.table) {
          params.append('table', options.table);
        }
        if (options.database) {
          params.append('database', options.database);
        }

        if (params.toString()) {
          url += '?' + params.toString();
        }

        const response = await ctx.client.get(url);

        if (ctx.config.output !== 'json' && spinner) {
          spinner.succeed(chalk.green('✓ Schema information retrieved'));
        }
        const schemaData = response.data;

        if (ctx.config.output === 'json') {
          renderJson(schemaData);
        } else {
          Console.info(chalk.cyan('\n🗃️  Database Schema'));
          Console.info(chalk.gray('═'.repeat(60)));

          if (schemaData.connections) {
            Console.info(chalk.bold.blue('Connections:'));
            schemaData.connections.forEach((conn: any) => {
              Console.info(chalk.white(`  • ${conn.name} (${conn.type})`));
              if (conn.tables && conn.tables.length > 0) {
                Console.info(chalk.gray(`    Tables: ${conn.tables.length}`));
                conn.tables.forEach((table: string) => {
                  Console.info(chalk.gray(`      - ${table}`));
                });
              }
            });
          } else if (schemaData.tables) {
            Console.info(chalk.bold.blue('Tables:'));
            schemaData.tables.forEach((table: any) => {
              Console.info(chalk.white(`  • ${table.name}`));
              if (table.columns) {
                table.columns.forEach((col: any) => {
                  Console.info(chalk.gray(`    - ${col.name}: ${col.type}`));
                });
              }
            });
          } else {
            Console.info(chalk.gray('No schema information available'));
          }
        }
      } catch (error) {
        if (spinner) {
          spinner.fail(chalk.red('✗ Failed to fetch schema information'));
        }
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  schema
    .command('refresh')
    .description('Refresh database schema information')
    .option('--connection <connection>', 'Specific connection to refresh')
    .option('-f, --force', 'Force refresh even if not needed')
    .action(async (options: {
      connection?: string;
      force?: boolean;
    }) => {
      let spinner;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Refreshing schema information...');
      }

      try {
        const payload: any = {};
        if (options.connection) {
          payload.connection = options.connection;
        }
        if (options.force) {
          payload.force = true;
        }

        const response = await ctx.client.post('/api/v1/_config/schema/refresh', payload);

        if (ctx.config.output !== 'json' && spinner) {
          spinner.succeed(chalk.green('✓ Schema information refreshed'));
        }
        const result = response.data;

        if (ctx.config.output === 'json') {
          renderJson(result);
        } else {
          Console.info(chalk.cyan('\n🔄 Schema Refresh'));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.green('✓ Schema refresh completed'));

          if (result.connections_refreshed !== undefined) {
            Console.info(chalk.blue(`Connections refreshed: ${result.connections_refreshed}`));
          }
          if (result.tables_discovered !== undefined) {
            Console.info(chalk.blue(`Tables discovered: ${result.tables_discovered}`));
          }
          if (result.last_refresh) {
            Console.info(chalk.blue(`Last refresh: ${new Date(result.last_refresh).toLocaleString()}`));
          }
        }
      } catch (error) {
        if (spinner) {
          spinner.fail(chalk.red('✗ Failed to refresh schema information'));
        }
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  schema
    .command('connections')
    .description('List available database connections')
    .action(async () => {
      let spinner;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Fetching connection information...');
      }

      try {
        const response = await ctx.client.get('/api/v1/_config/schema?connections=true');

        if (ctx.config.output !== 'json' && spinner) {
          spinner.succeed(chalk.green('✓ Connection information retrieved'));
        }
        const schemaData = response.data;

        if (ctx.config.output === 'json') {
          renderJson(schemaData);
        } else {
          Console.info(chalk.cyan('\n🔗 Database Connections'));
          Console.info(chalk.gray('═'.repeat(60)));

          if (schemaData.connections && Array.isArray(schemaData.connections)) {
            schemaData.connections.forEach((conn: any) => {
              Console.info(chalk.bold.blue(`${conn.name}`));
              Console.info(chalk.gray(`  Type: ${conn.type}`));
              Console.info(chalk.gray(`  Status: ${conn.status || 'unknown'}`));
              if (conn.tables) {
                Console.info(chalk.gray(`  Tables: ${conn.tables.length}`));
              }
              Console.info('');
            });
          } else {
            Console.info(chalk.gray('No connections found'));
          }
        }
      } catch (error) {
        if (spinner) {
          spinner.fail(chalk.red('✗ Failed to fetch connection information'));
        }
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  schema
    .command('tables')
    .description('List tables in database connections')
    .option('--connection <connection>', 'Specific connection to list tables for')
    .action(async (options: { connection?: string }) => {
      let spinner;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Fetching table information...');
      }

      try {
        let url = '/api/v1/_config/schema?tables=true';
        if (options.connection) {
          url += `&connection=${encodeURIComponent(options.connection)}`;
        }

        const response = await ctx.client.get(url);

        if (ctx.config.output !== 'json' && spinner) {
          spinner.succeed(chalk.green('✓ Table information retrieved'));
        }
        const schemaData = response.data;

        if (ctx.config.output === 'json') {
          renderJson(schemaData);
        } else {
          Console.info(chalk.cyan('\n📋 Database Tables'));
          Console.info(chalk.gray('═'.repeat(60)));

          if (schemaData.tables && Array.isArray(schemaData.tables)) {
            schemaData.tables.forEach((table: any) => {
              Console.info(chalk.bold.blue(`${table.name}`));
              Console.info(chalk.gray(`  Connection: ${table.connection}`));
              Console.info(chalk.gray(`  Columns: ${table.columns?.length || 0}`));
              if (table.columns) {
                table.columns.forEach((col: any) => {
                  Console.info(chalk.gray(`    - ${col.name}: ${col.type}`));
                });
              }
              Console.info('');
            });
          } else {
            Console.info(chalk.gray('No tables found'));
          }
        }
      } catch (error) {
        if (spinner) {
          spinner.fail(chalk.red('✗ Failed to fetch table information'));
        }
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
