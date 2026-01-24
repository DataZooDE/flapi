import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson } from '../../lib/render';
import chalk from 'chalk';

interface FilesystemNode {
  name: string;
  type: 'file' | 'directory';
  path: string;
  children?: FilesystemNode[];
  extension?: string;
  yaml_type?: string;
}

function renderTree(nodes: FilesystemNode[], prefix = '') {
  nodes.forEach((node, index) => {
    const isLast = index === nodes.length - 1;
    const branch = isLast ? '└─' : '├─';
    const nextPrefix = prefix + (isLast ? '  ' : '│ ');

    const label = node.type === 'directory'
      ? chalk.blue(`[dir] ${node.name}`)
      : chalk.white(node.name);

    Console.info(`${prefix}${branch} ${label}`);

    if (node.type === 'file' && node.yaml_type) {
      Console.info(`${nextPrefix}${chalk.gray(`(${node.yaml_type})`)}`);
    }

    if (node.children && node.children.length > 0) {
      renderTree(node.children, nextPrefix);
    }
  });
}

export function registerFilesystemCommand(config: Command, ctx: CliContext) {
  config
    .command('filesystem')
    .description('Inspect server-side filesystem tree for templates')
    .action(async () => {
      const spinner = Console.spinner('Fetching filesystem structure...');
      try {
        const response = await ctx.client.get('/api/v1/_config/filesystem');
        spinner.succeed(chalk.green('✓ Filesystem data retrieved'));
        const payload = response.data;

        if (ctx.config.output === 'json') {
          renderJson(payload, ctx.config.jsonStyle);
          return;
        }

        Console.info(chalk.cyan('\n📁 flapi Filesystem'));
        Console.info(chalk.gray('═'.repeat(60)));
        Console.info(`${chalk.bold('Base Path:')} ${payload?.base_path || 'N/A'}`);
        Console.info(`${chalk.bold('Templates Path:')} ${payload?.template_path || 'N/A'}`);
        if (payload?.config_file) {
          Console.info(`${chalk.bold('Config File:')} ${payload.config_file} (${payload?.config_file_exists ? 'found' : 'missing'})`);
        }
        Console.info('');

        if (Array.isArray(payload?.tree) && payload.tree.length > 0) {
          renderTree(payload.tree);
        } else {
          Console.info(chalk.gray('No files discovered under the template path.'));
        }
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch filesystem data'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
