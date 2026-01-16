import type { Command } from 'commander';
import type { CliContext } from '../../lib/types';
import { Console } from '../../lib/console';
import { handleError } from '../../lib/errors';
import chalk from 'chalk';
import * as fs from 'node:fs/promises';
import * as yaml from 'js-yaml';
import {
  GeminiClient,
  GeminiAuthManager,
  promptCreationMethod,
  promptNaturalDescription,
  promptAuthMethod,
  promptApiKey,
  promptEditAction,
  promptConfirmSave,
  promptEndpointMetadata,
  promptHttpMethod,
  promptAddParameter,
  promptCacheOptions,
} from '../../lib/gemini';
import { ConfigParser, FlApiConfigValidator } from '../../lib/gemini/parser';
import { ConfigReviewer } from '../../lib/gemini/reviewer';
import { buildEndpointUrl } from '../../lib/url';
import { WizardConfig } from '../../lib/types';

export async function registerWizardCommand(program: Command, ctx: CliContext) {
  program
    .command('wizard')
    .description('Interactive endpoint creation wizard (with optional AI assistance)')
    .option('--ai', 'Use AI assistance (skip creation method prompt)')
    .option('--output-file <path>', 'Save configuration to file instead of creating endpoint')
    .option('--skip-validation', 'Skip validating configuration before saving')
    .option('--dry-run', 'Show what would be done without actually creating the endpoint')
    .action(async (options: {
      ai?: boolean;
      outputFile?: string;
      skipValidation?: boolean;
      dryRun?: boolean;
    }) => {
      try {
        let creationMethod: 'ai' | 'manual' = 'manual';

        if (options.ai) {
          creationMethod = 'ai';
        } else {
          creationMethod = await promptCreationMethod();
        }

        let wizardConfig: WizardConfig;

        if (creationMethod === 'ai') {
          wizardConfig = await runAiWizard(ctx);
        } else {
          wizardConfig = await runManualWizard(ctx);
        }

        // Validate configuration against server requirements
        if (!options.skipValidation) {
          const errors = FlApiConfigValidator.validateForServer(wizardConfig);
          if (errors.length > 0) {
            Console.error('Configuration validation failed:');
            errors.forEach((err) => {
              console.log(chalk.red(`  • ${err}`));
            });
            process.exitCode = 1;
            return;
          }
        }

        // Show what would be done
        if (options.dryRun) {
          console.log(
            chalk.yellow('\n[DRY RUN] Configuration would be saved as:')
          );
          ConfigReviewer.displaySummary(wizardConfig);
          console.log(chalk.dim('\nNo actual changes were made.'));
          return;
        }

        // Handle file output or API creation
        if (options.outputFile) {
          await saveToFile(wizardConfig, options.outputFile);
          Console.success(`Configuration saved to ${options.outputFile}`);
        } else {
          await createEndpointViaApi(ctx, wizardConfig);
        }
      } catch (error) {
        if (error instanceof Error && error.message === 'Wizard cancelled') {
          console.log(chalk.yellow('\nWizard cancelled.'));
          return;
        }
        handleError(error, ctx.config);
        process.exitCode = 1;
      }
    });
}

async function runAiWizard(ctx: CliContext): Promise<WizardConfig> {
  // Check authentication
  const authManager = new GeminiAuthManager();
  let authConfig = await authManager.getAuthConfig();

  if (!authConfig) {
    console.log(
      chalk.cyan(
        '\nTo use AI-assisted endpoint creation, you need a Google Gemini API key.'
      )
    );

    const authMethod = await promptAuthMethod();

    if (authMethod === 'api_key') {
      const apiKey = await promptApiKey();
      await authManager.storeApiKey(apiKey);
      authConfig = await authManager.getAuthConfig();

      if (!authConfig) {
        throw new Error('Failed to save API key');
      }
    } else {
      throw new Error('OAuth authentication not yet implemented');
    }
  }

  // Get connections and tables for context
  const spinner = Console.spinner('Fetching available connections...');
  try {
    const response = await ctx.client.get('/api/v1/_config/connections');
    spinner.succeed();
    const connections = Object.keys(response.data);

    // Get natural language description
    const description = await promptNaturalDescription();

    // Generate configuration with AI
    const client = new GeminiClient(authConfig);
    const aiConfig = await client.generateEndpointConfig(
      description,
      connections,
      ['example_table'] // In real scenario, would fetch actual tables
    );

    // Validate and convert
    const wizardConfig = ConfigParser.validateAndConvert(aiConfig);

    // Show summary and allow editing
    ConfigReviewer.displaySummary(wizardConfig);

    if (aiConfig.reasoning) {
      console.log(chalk.dim(`\nAI Reasoning: ${aiConfig.reasoning}`));
    }

    const editAction = await promptEditAction();

    switch (editAction) {
      case 'accept':
        return wizardConfig;
      case 'edit':
        return await ConfigReviewer.editConfig(wizardConfig);
      case 'regenerate':
        console.log(chalk.yellow('Regenerating configuration...'));
        return await runAiWizard(ctx);
      case 'manual':
        return await runManualWizard(ctx);
      case 'cancel':
        throw new Error('Wizard cancelled');
      default:
        throw new Error('Invalid action');
    }
  } catch (error) {
    spinner.fail();
    throw error;
  }
}

async function runManualWizard(ctx: CliContext): Promise<WizardConfig> {
  // Fetch connections
  const spinner = Console.spinner('Fetching connections...');
  let connections: string[] = [];

  try {
    const response = await ctx.client.get('/api/v1/_config/connections');
    connections = Object.keys(response.data);
    spinner.succeed();
  } catch (error) {
    spinner.fail();
    throw new Error(
      `Failed to fetch connections: ${error instanceof Error ? error.message : String(error)}`
    );
  }

  if (connections.length === 0) {
    throw new Error('No connections available. Please create a connection first.');
  }

  // Collect metadata
  const metadata = await promptEndpointMetadata(connections);

  // Select HTTP method
  const method = await promptHttpMethod();

  // Collect parameters
  const parameters = [];
  let addMore = true;

  console.log(chalk.cyan('\nConfigure parameters (or skip if none):'));

  while (addMore) {
    const paramConfig = await promptAddParameter();
    addMore = paramConfig.addMore;

    parameters.push({
      name: paramConfig.name,
      type: paramConfig.type,
      required: paramConfig.required,
      location: paramConfig.location,
      description: paramConfig.description || undefined,
      validators: [],
    });
  }

  // Configure caching
  const cacheConfig = await promptCacheOptions();

  // Build final config
  const wizardConfig: WizardConfig = {
    endpoint_name: metadata.name,
    url_path: metadata.urlPath,
    description: metadata.description || undefined,
    connection: metadata.connection,
    table: metadata.table,
    method,
    parameters,
    enable_cache: cacheConfig.enabled,
    cache_ttl: cacheConfig.ttl,
  };

  // Show summary and confirm
  ConfigReviewer.displaySummary(wizardConfig);

  const confirmed = await promptConfirmSave();

  if (!confirmed) {
    throw new Error('Wizard cancelled');
  }

  return wizardConfig;
}

async function createEndpointViaApi(
  ctx: CliContext,
  config: WizardConfig
): Promise<void> {
  const spinner = Console.spinner(`Creating endpoint ${config.endpoint_name}...`);

  try {
    // Convert WizardConfig to API format
    const apiPayload = {
      endpoint_name: config.endpoint_name,
      url_path: config.url_path,
      description: config.description || '',
      connection: config.connection,
      table: config.table,
      method: config.method,
      parameters: config.parameters,
      enable_cache: config.enable_cache,
      cache_ttl: config.cache_ttl || 300,
    };

    const response = await ctx.client.post('/api/v1/_config/endpoints', apiPayload);

    spinner.succeed(chalk.green(`✓ Endpoint ${config.endpoint_name} created`));

    console.log(chalk.cyan(`\nEndpoint Details:`));
    console.log(`  Name: ${config.endpoint_name}`);
    console.log(`  Path: ${config.url_path}`);
    console.log(`  Method: ${config.method}`);

    if (config.parameters.length > 0) {
      console.log(`  Parameters: ${config.parameters.length}`);
      ConfigReviewer.displayParameters(config.parameters);
    }

    if (config.enable_cache) {
      console.log(`  Cache: Enabled (${config.cache_ttl}s TTL)`);
    }
  } catch (error) {
    spinner.fail(chalk.red(`✗ Failed to create endpoint`));
    throw error;
  }
}

async function saveToFile(config: WizardConfig, filePath: string): Promise<void> {
  // Create endpoint configuration in YAML format
  const endpointConfig = {
    [config.endpoint_name]: {
      url_path: config.url_path,
      method: config.method,
      connection: config.connection,
      table: config.table,
      description: config.description || '',
      parameters: config.parameters.map((p) => ({
        name: p.name,
        type: p.type,
        required: p.required,
        location: p.location,
        description: p.description || undefined,
      })),
      cache: config.enable_cache
        ? {
            enabled: true,
            ttl: config.cache_ttl || 300,
          }
        : {
            enabled: false,
          },
    },
  };

  const yaml_content = yaml.dump(endpointConfig, { indent: 2 });

  try {
    await fs.writeFile(filePath, yaml_content, 'utf-8');
  } catch (error) {
    throw new Error(
      `Failed to write configuration file: ${error instanceof Error ? error.message : String(error)}`
    );
  }
}
