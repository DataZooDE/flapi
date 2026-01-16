import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import { ProjectInitOptions } from '../../lib/types';
import { ProjectGenerator, resolveProjectDirectory, validateProjectName } from './generators';
import chalk from 'chalk';
import * as path from 'node:path';

export async function registerInitCommand(program: Command, ctx: CliContext) {
  program
    .command('init [project-name]')
    .description('Initialize a new flapi project with directory structure and examples')
    .option('-f, --force', 'Overwrite existing files without prompting')
    .option('--skip-validation', 'Skip configuration validation after setup')
    .option('--no-examples', 'Only create directories and flapi.yaml')
    .option('--advanced', 'Include additional template files (advanced users)')
    .option('--ai', 'Use AI assistance to configure the project (requires Gemini API key)')
    .action(async (projectName: string | undefined, options: ProjectInitOptions) => {
      try {
        // Resolve project directory
        const projectDir = resolveProjectDirectory(projectName);

        // Validate project name if creating new directory
        if (projectDir !== '.') {
          const validation = validateProjectName(projectDir);
          if (!validation.valid) {
            Console.error(validation.error || 'Invalid project name');
            process.exitCode = 1;
            return;
          }
        }

        // Display start message
        if (projectDir === '.') {
          console.log(chalk.cyan('Setting up flapi project in current directory...'));
        } else {
          console.log(chalk.cyan(`Setting up flapi project: ${projectDir}`));
        }

        // Check if AI mode is requested
        if (options.ai) {
          // AI mode would be implemented in phase 3
          Console.warn('AI-assisted project initialization coming soon!');
          Console.info('Using standard project template instead');
        }

        // Generate standard project structure
        const result = await ProjectGenerator.generateProject(projectDir, options);

        // Optionally validate configuration
        if (!options.skipValidation) {
          const spinner = Console.spinner('Validating configuration...');
          try {
            const validation = await ProjectGenerator.validateProjectConfig(
              projectDir,
              'flapi.yaml'
            );

            if (validation.valid) {
              spinner.succeed(chalk.green('✓ Configuration is valid'));
            } else {
              spinner.fail(chalk.yellow('⚠️  Configuration validation failed'));
              if (validation.errors.length > 0) {
                validation.errors.forEach((err) => {
                  Console.warn(`  • ${err}`);
                });
              }
              result.warnings.push('Configuration validation failed');
            }
          } catch (error) {
            spinner.fail(chalk.yellow('⚠️  Could not validate configuration'));
          }
        }

        // Display summary
        ProjectGenerator.displaySummary(projectDir, result);

        // Exit successfully
        process.exitCode = 0;
      } catch (error) {
        if (error instanceof Error && error.message.includes('Permission denied')) {
          Console.error('Permission denied: Check directory permissions');
          process.exitCode = 2;
        } else if (error instanceof Error && error.message.includes('Invalid')) {
          Console.error(error.message);
          process.exitCode = 3;
        } else {
          handleError(error, ctx.config);
          process.exitCode = 1;
        }
      }
    });
}
