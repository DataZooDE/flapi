import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { renderJson, renderTemplatesTable } from '../../lib/render';
import { buildEndpointUrl } from '../../lib/url';
import chalk from 'chalk';

export function registerTemplateCommands(program: Command, ctx: CliContext) {
  const templates = program
    .command('templates')
    .description('Template management commands');

  templates
    .command('list')
    .description('List all templates')
    .action(async () => {
      let spinner;
      if (ctx.config.output !== 'json') {
        spinner = Console.spinner('Fetching templates...');
      }

      try {
        // List templates by getting all endpoints and extracting template info
        const endpointsResponse = await ctx.client.get('/api/v1/_config/endpoints');
        const endpoints = endpointsResponse.data;
        const templates: Record<string, any> = {};

        for (const [path, endpoint] of Object.entries(endpoints)) {
          if (typeof endpoint === 'object' && endpoint !== null) {
            const config = endpoint as Record<string, any>;
            if (config['template-source'] || config.templateSource) {
              templates[path] = {
                'template-source': config['template-source'] || config.templateSource,
                'template-path': config['template-path'] || config.templatePath
              };
            }
          }
        }

        if (ctx.config.output !== 'json') {
          spinner.succeed(chalk.green('✓ Templates retrieved'));
        }

        if (ctx.config.output === 'json') {
          renderJson(templates, ctx.config.jsonStyle);
        } else {
          renderTemplatesTable(templates);
        }
      } catch (error) {
        spinner.fail(chalk.red('✗ Failed to fetch templates'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  templates
    .command('get <path>')
    .description('Get template content')
    .action(async (path: string) => {
      const spinner = Console.spinner(`Fetching template ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'template');
        const response = await ctx.client.get(endpointUrl);
        spinner.succeed(chalk.green(`✓ Template ${path} retrieved`));
        const template = response.data;

        if (ctx.config.output === 'json') {
          renderJson(template, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan(`\n📄 Template: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.white(template));
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to fetch template ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  templates
    .command('update <path>')
    .description('Update template content')
    .option('-f, --file <file>', 'File containing template content')
    .option('--stdin', 'Read template content from stdin')
    .action(async (path: string, options: { file?: string; stdin?: boolean }) => {
      if (options.file && options.stdin) {
        Console.error('Cannot specify both --file and --stdin');
        process.exitCode = 1;
        return;
      }

      if (!options.file && !options.stdin) {
        Console.error('Must specify either --file or --stdin');
        process.exitCode = 1;
        return;
      }

      let templateContent: string;

      if (options.file) {
        const fs = await import('node:fs');
        templateContent = fs.readFileSync(options.file, 'utf-8');
      } else {
        const chunks: string[] = [];
        process.stdin.setEncoding('utf-8');
        for await (const chunk of process.stdin) {
          chunks.push(chunk);
        }
        templateContent = chunks.join('');
      }

      const spinner = Console.spinner(`Updating template ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'template');
        await ctx.client.put(endpointUrl, {
          template: templateContent
        });
        spinner.succeed(chalk.green(`✓ Template ${path} updated successfully`));
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to update template ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  templates
    .command('expand <path>')
    .description('Expand template with parameters')
    .option('-p, --params <params>', 'JSON parameters for template expansion')
    .option('--params-file <file>', 'File containing JSON parameters')
    .action(async (path: string, options: { params?: string; paramsFile?: string }) => {
      let params: any = {};

      if (options.params) {
        try {
          params = JSON.parse(options.params);
        } catch (error) {
          Console.error('Invalid JSON in --params');
          process.exitCode = 1;
          return;
        }
      } else if (options.paramsFile) {
        try {
          const fs = await import('node:fs');
          const paramsContent = fs.readFileSync(options.paramsFile, 'utf-8');
          params = JSON.parse(paramsContent);
        } catch (error) {
          Console.error('Invalid JSON in --params-file');
          process.exitCode = 1;
          return;
        }
      }

      const spinner = Console.spinner(`Expanding template ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'template/expand');
        const response = await ctx.client.post(endpointUrl, {
          parameters: params
        });
        spinner.succeed(chalk.green(`✓ Template ${path} expanded successfully`));
        const result = response.data;

        if (ctx.config.output === 'json') {
          renderJson(result, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan(`\n📄 Expanded Template: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));
          Console.info(chalk.white(result.expanded || result));
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to expand template ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });

  templates
    .command('test <path>')
    .description('Test template syntax and parameters')
    .option('-p, --params <params>', 'JSON parameters for template testing')
    .option('--params-file <file>', 'File containing JSON parameters')
    .action(async (path: string, options: { params?: string; paramsFile?: string }) => {
      let params: any = {};

      if (options.params) {
        try {
          params = JSON.parse(options.params);
        } catch (error) {
          Console.error('Invalid JSON in --params');
          process.exitCode = 1;
          return;
        }
      } else if (options.paramsFile) {
        try {
          const fs = await import('node:fs');
          const paramsContent = fs.readFileSync(options.paramsFile, 'utf-8');
          params = JSON.parse(paramsContent);
        } catch (error) {
          Console.error('Invalid JSON in --params-file');
          process.exitCode = 1;
          return;
        }
      }

      const spinner = Console.spinner(`Testing template ${path}...`);
      try {
        const endpointUrl = buildEndpointUrl(path, 'template/test');
        const response = await ctx.client.post(endpointUrl, {
          parameters: params
        });
        spinner.succeed(chalk.green(`✓ Template ${path} test completed`));
        const result = response.data;

        if (ctx.config.output === 'json') {
          renderJson(result, ctx.config.jsonStyle);
        } else {
          Console.info(chalk.cyan(`\n🔍 Template Test: ${path}`));
          Console.info(chalk.gray('═'.repeat(60)));

          if (result.valid) {
            Console.success('Template syntax is valid');
            if (result.parameters) {
              Console.info(chalk.blue('Required parameters:'));
              Object.keys(result.parameters).forEach(param => {
                Console.info(chalk.white(`  - ${param}`));
              });
            }
          } else {
            Console.error('Template syntax is invalid');
            if (result.errors) {
              result.errors.forEach((error: string) => {
                Console.error(chalk.red(`  - ${error}`));
              });
            }
          }
        }
      } catch (error) {
        spinner.fail(chalk.red(`✗ Failed to test template ${path}`));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}
