import * as fs from 'node:fs/promises';
import * as path from 'node:path';
import { Console } from '../../lib/console';
import {
  ProjectInitOptions,
  ProjectInitResult,
  GeneratedProjectConfig,
} from '../../lib/types';
import { PROJECT_TEMPLATES, getDirectoriesToCreate } from './templates';
import chalk from 'chalk';

/**
 * Generate standard project structure and files
 */
export class ProjectGenerator {
  /**
   * Create project directory structure and files
   */
  static async generateProject(
    projectDir: string,
    options: ProjectInitOptions
  ): Promise<ProjectInitResult> {
    const result: ProjectInitResult = {
      projectDir,
      filesCreated: [],
      directoriesCreated: [],
      validationPassed: true,
      warnings: [],
    };

    try {
      // Create main project directory if needed
      if (projectDir !== '.') {
        try {
          await fs.mkdir(projectDir, { recursive: true });
          result.directoriesCreated.push(projectDir);
        } catch (error) {
          throw new Error(
            `Failed to create project directory: ${error instanceof Error ? error.message : String(error)}`
          );
        }
      }

      // Create subdirectories
      for (const dir of getDirectoriesToCreate()) {
        const dirPath = path.join(projectDir, dir);
        try {
          await fs.mkdir(dirPath, { recursive: true });
          result.directoriesCreated.push(dir);
        } catch (error) {
          throw new Error(`Failed to create directory ${dir}: ${error}`);
        }
      }

      if (result.directoriesCreated.length > 0) {
        Console.success('✅ Created directories');
      }

      // Create files from templates
      const filesToCreate = options.noExamples
        ? ['flapi.yaml', '.gitignore']
        : Object.keys(PROJECT_TEMPLATES);

      for (const filePath of filesToCreate) {
        const fullPath = path.join(projectDir, filePath);
        const content = PROJECT_TEMPLATES[filePath as keyof typeof PROJECT_TEMPLATES];

        if (!content) {
          result.warnings.push(`Unknown template: ${filePath}`);
          continue;
        }

        // Check if file exists
        try {
          await fs.access(fullPath);

          if (options.force) {
            // Overwrite with --force flag
            await fs.writeFile(fullPath, content, 'utf-8');
            Console.success(`✅ Created ${path.basename(filePath)} (overwritten)`);
            result.filesCreated.push(filePath);
          } else {
            // Skip existing files
            Console.warn(`⏭️  ${path.basename(filePath)} already exists, skipping`);
            result.warnings.push(`${filePath} already exists`);
          }
        } catch {
          // File doesn't exist, create it
          try {
            await fs.writeFile(fullPath, content, 'utf-8');
            Console.success(`✅ Created ${path.basename(filePath)}`);
            result.filesCreated.push(filePath);
          } catch (error) {
            throw new Error(
              `Failed to create ${filePath}: ${error instanceof Error ? error.message : String(error)}`
            );
          }
        }
      }

      return result;
    } catch (error) {
      throw error;
    }
  }

  /**
   * Generate summary output with next steps
   */
  static displaySummary(
    projectDir: string,
    result: ProjectInitResult,
    config?: GeneratedProjectConfig
  ): void {
    const displayDir = projectDir === '.' ? 'current directory' : projectDir;

    let summary = `Directory structure created:
${displayDir === 'current directory' ? '  (current directory)' : `  ${projectDir}/`}
  ├── flapi.yaml          (main configuration)
  ├── sqls/               (endpoint definitions)
  │  ├── sample.yaml
  │  └── sample.sql
  ├── data/               (data files)
  ├── common/             (shared configs)
  │  ├── auth.yaml
  │  └── rate-limit.yaml
  └── .gitignore`;

    if (config) {
      summary += `

Generated Configuration:
  Project: ${config.name}
  Description: ${config.description}`;

      if (config.connections.length > 0) {
        summary += `
  Connections: ${config.connections.map((c) => c.name).join(', ')}`;
      }

      if (config.examples.length > 0) {
        summary += `
  Example Endpoints: ${config.examples.length}`;
      }
    }

    summary += `

Next steps:
  1. Edit flapi.yaml to add your database connections
  2. Create endpoint YAML files in sqls/
  3. Write SQL templates in sqls/
  4. Run: flapii config validate --config ${path.join(displayDir === 'current directory' ? '.' : projectDir, 'flapi.yaml')}
  5. Start server: ./flapi -c ${path.join(displayDir === 'current directory' ? '.' : projectDir, 'flapi.yaml')}

Documentation:
  • Project guide: https://docs.datazoo.dev/flapi/project-init
  • SQL templates: https://docs.datazoo.dev/flapi/mustache-templates
  • Configuration: https://docs.datazoo.dev/flapi/configuration`;

    console.log(Console.box(summary, '✨ Project Setup Complete!'));
  }

  /**
   * Validate project YAML file
   */
  static async validateProjectConfig(
    projectDir: string,
    configPath: string
  ): Promise<{ valid: boolean; errors: string[] }> {
    try {
      const filePath = path.join(projectDir, configPath);

      // Check file exists
      await fs.access(filePath);

      // Basic YAML validation (would call validateYamlFile in real implementation)
      const content = await fs.readFile(filePath, 'utf-8');

      // Check for required fields
      const hasProjectName = content.includes('project-name:');
      const hasConnections = content.includes('connections:');
      const hasTemplateSource = content.includes('template-source:');

      if (!hasProjectName || !hasConnections || !hasTemplateSource) {
        return {
          valid: false,
          errors: [
            !hasProjectName ? 'Missing required field: project-name' : '',
            !hasConnections ? 'Missing required field: connections' : '',
            !hasTemplateSource ? 'Missing required field: template-source' : '',
          ].filter((e) => e),
        };
      }

      return { valid: true, errors: [] };
    } catch (error) {
      return {
        valid: false,
        errors: [
          `Validation error: ${error instanceof Error ? error.message : String(error)}`,
        ],
      };
    }
  }
}

/**
 * Helper to determine project directory
 */
export function resolveProjectDirectory(projectName?: string): string {
  if (!projectName || projectName === '.') {
    return '.';
  }
  return projectName;
}

/**
 * Helper to validate project name
 */
export function validateProjectName(name: string): { valid: boolean; error?: string } {
  if (!name || name.trim().length === 0) {
    return { valid: false, error: 'Project name cannot be empty' };
  }

  // Allow dots and hyphens, alphanumeric characters
  if (!/^[a-zA-Z0-9._-]+$/.test(name)) {
    return {
      valid: false,
      error: 'Project name must contain only letters, numbers, hyphens, underscores, or dots',
    };
  }

  // Check for special cases
  if (name === '.' || name === '..') {
    return { valid: true }; // These are valid directory references
  }

  // Check for invalid names
  if (name.includes(' ')) {
    return { valid: false, error: 'Project name cannot contain spaces' };
  }

  return { valid: true };
}
