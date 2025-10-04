import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import chalk from 'chalk';
import * as fs from 'node:fs/promises';
import * as path from 'node:path';
import YAML from 'js-yaml';

interface YamlEndpoint {
  'url-path'?: string;
  'template-source'?: string;
  'mcp-tool'?: { name: string };
  'mcp-resource'?: { name: string };
  'mcp-prompt'?: { name: string };
  connection?: string[];
}

interface ValidationIssue {
  endpoint: string;
  type: 'error' | 'warning';
  message: string;
}

async function findYamlFiles(dir: string): Promise<string[]> {
  const files: string[] = [];
  try {
    const entries = await fs.readdir(dir, { withFileTypes: true });
    
    for (const entry of entries) {
      const fullPath = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        const subFiles = await findYamlFiles(fullPath);
        files.push(...subFiles);
      } else if (entry.isFile() && (entry.name.endsWith('.yaml') || entry.name.endsWith('.yml'))) {
        files.push(fullPath);
      }
    }
  } catch (error) {
    // Ignore errors for directories we can't read
  }
  return files;
}

async function validateEndpointFile(filePath: string): Promise<ValidationIssue[]> {
  const issues: ValidationIssue[] = [];
  
  try {
    const content = await fs.readFile(filePath, 'utf-8');
    const doc = YAML.load(content) as YamlEndpoint;
    
    if (!doc) {
      return issues;
    }
    
    // Determine endpoint name
    const endpointName = doc['url-path'] || 
                        doc['mcp-tool']?.name || 
                        doc['mcp-resource']?.name || 
                        doc['mcp-prompt']?.name || 
                        path.basename(filePath);
    
    // Check if it's an endpoint file (has url-path or mcp-* fields)
    const isEndpoint = !!(doc['url-path'] || doc['mcp-tool'] || doc['mcp-resource'] || doc['mcp-prompt']);
    
    if (!isEndpoint) {
      return issues; // Skip non-endpoint YAML files
    }
    
    // Validate template-source
    if (!doc['template-source']) {
      issues.push({
        endpoint: endpointName,
        type: 'error',
        message: 'template-source cannot be empty'
      });
    } else {
      // Check if template file exists
      const templatePath = path.join(path.dirname(filePath), doc['template-source']);
      try {
        await fs.access(templatePath);
      } catch {
        issues.push({
          endpoint: endpointName,
          type: 'warning',
          message: `Template file does not exist: ${doc['template-source']}`
        });
      }
    }
    
    // Validate connection
    if (!doc.connection || doc.connection.length === 0) {
      issues.push({
        endpoint: endpointName,
        type: 'warning',
        message: 'No database connection specified'
      });
    }
    
    // Validate URL path format
    if (doc['url-path'] && !doc['url-path'].startsWith('/')) {
      issues.push({
        endpoint: endpointName,
        type: 'error',
        message: `url-path must start with '/' but got '${doc['url-path']}'`
      });
    }
    
  } catch (error) {
    const endpointName = path.basename(filePath);
    if (error instanceof Error) {
      issues.push({
        endpoint: endpointName,
        type: 'error',
        message: `YAML parsing error: ${error.message}`
      });
    }
  }
  
  return issues;
}

export function registerValidateCommand(config: Command, ctx: CliContext) {
  config
    .command('validate')
    .description('Validate flapi configuration and endpoint files')
    .option('-f, --file <path>', 'Specific YAML file to validate')
    .option('-d, --dir <path>', 'Directory containing endpoint YAML files', 'examples/sqls')
    .action(async (options: { file?: string; dir?: string }) => {
      const spinner = Console.spinner('Validating configuration...');
      
      try {
        let yamlFiles: string[] = [];
        
        if (options.file) {
          // Validate single file
          yamlFiles = [options.file];
        } else {
          // Validate all files in directory
          yamlFiles = await findYamlFiles(options.dir || 'examples/sqls');
          
          if (yamlFiles.length === 0) {
            spinner.fail(chalk.red('No YAML files found'));
            Console.warn(`Directory: ${options.dir || 'examples/sqls'}`);
            process.exitCode = 1;
            return;
          }
        }
        
        spinner.text = `Validating ${yamlFiles.length} file(s)...`;
        
        const allIssues: ValidationIssue[] = [];
        let validatedCount = 0;
        
        for (const file of yamlFiles) {
          const issues = await validateEndpointFile(file);
          allIssues.push(...issues);
          if (issues.length === 0) {
            validatedCount++;
          }
        }
        
        spinner.stop();
        
        // Group issues by endpoint
        const endpointIssues = new Map<string, ValidationIssue[]>();
        for (const issue of allIssues) {
          if (!endpointIssues.has(issue.endpoint)) {
            endpointIssues.set(issue.endpoint, []);
          }
          endpointIssues.get(issue.endpoint)!.push(issue);
        }
        
        // Display results
        console.log(chalk.green(`✓ Validated ${yamlFiles.length} file(s)`));
        
        if (endpointIssues.size > 0) {
          console.log('');
          for (const [endpoint, issues] of endpointIssues) {
            const errors = issues.filter(i => i.type === 'error');
            const warnings = issues.filter(i => i.type === 'warning');
            
            if (errors.length > 0) {
              console.log(chalk.red(`✗ Endpoint: ${endpoint}`));
              for (const issue of errors) {
                console.log(chalk.red(`  ERROR: ${issue.message}`));
              }
            }
            
            if (warnings.length > 0) {
              console.log(chalk.yellow(`⚠ Endpoint: ${endpoint}`));
              for (const issue of warnings) {
                console.log(chalk.yellow(`  WARNING: ${issue.message}`));
              }
            }
          }
        }
        
        // Summary
        console.log('');
        console.log('='.repeat(60));
        
        const errorCount = allIssues.filter(i => i.type === 'error').length;
        const warningCount = allIssues.filter(i => i.type === 'warning').length;
        
        if (errorCount === 0) {
          console.log(chalk.green('✓ Validation PASSED'));
          if (warningCount > 0) {
            console.log(chalk.yellow(`  ${warningCount} warning(s)`));
          }
        } else {
          console.log(chalk.red('✗ Validation FAILED'));
          console.log(chalk.red(`  ${errorCount} error(s)`));
          if (warningCount > 0) {
            console.log(chalk.yellow(`  ${warningCount} warning(s)`));
          }
          process.exitCode = 1;
        }
        
      } catch (error) {
        spinner.fail(chalk.red('✗ Validation failed'));
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}

