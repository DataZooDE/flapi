import enquirer from 'enquirer';
import chalk from 'chalk';
import boxen from 'boxen';
import { WizardConfig, ParameterDefinition } from '../types';
import { renderTable } from '../render';

const { prompt } = enquirer;

export class ConfigReviewer {
  /**
   * Display a config summary with box formatting
   */
  static displaySummary(config: WizardConfig): void {
    const summary = {
      'Endpoint Name': config.endpoint_name,
      'URL Path': config.url_path,
      'HTTP Method': config.method,
      Connection: config.connection,
      Table: config.table,
      Parameters: config.parameters.length > 0 ? config.parameters.length : 'None',
      Cache: config.enable_cache ? `Enabled (${config.cache_ttl || 300}s)` : 'Disabled',
      Description: config.description || '(none)',
    };

    const content = Object.entries(summary)
      .map(([key, value]) => `${chalk.bold(key)}: ${value}`)
      .join('\n');

    console.log(boxen(content, {
      title: chalk.bold.cyan(' Generated Endpoint Configuration '),
      titleAlignment: 'center',
      padding: 1,
      margin: 1,
    }));
  }

  /**
   * Display parameters in table format
   */
  static displayParameters(parameters: ParameterDefinition[]): void {
    if (parameters.length === 0) {
      console.log(chalk.dim('No parameters defined'));
      return;
    }

    const tableData = parameters.map((p) => ({
      Name: p.name,
      Type: p.type,
      Required: p.required ? '✓' : '✗',
      Location: p.location,
      Description: p.description || '–',
    }));

    renderTable(tableData);
  }

  /**
   * Allow user to edit specific config fields
   */
  static async editConfig(config: WizardConfig): Promise<WizardConfig> {
    const editOptions = [
      { name: 'Endpoint name', value: 'name' },
      { name: 'URL path', value: 'path' },
      { name: 'Description', value: 'description' },
      { name: 'HTTP method', value: 'method' },
      { name: 'Parameters', value: 'parameters' },
      { name: 'Cache settings', value: 'cache' },
      { name: 'Done editing', value: 'done' },
    ];

    let edited = { ...config };
    let continueEditing = true;

    while (continueEditing) {
      const answers = await prompt<{ field: string }>([
        {
          type: 'select',
          name: 'field',
          message: 'What would you like to edit?',
          choices: editOptions,
        },
      ]);

      switch (answers.field) {
        case 'name':
          edited = await this.editName(edited);
          break;
        case 'path':
          edited = await this.editPath(edited);
          break;
        case 'description':
          edited = await this.editDescription(edited);
          break;
        case 'method':
          edited = await this.editMethod(edited);
          break;
        case 'parameters':
          edited = await this.editParameters(edited);
          break;
        case 'cache':
          edited = await this.editCache(edited);
          break;
        case 'done':
          continueEditing = false;
          break;
      }
    }

    return edited;
  }

  private static async editName(config: WizardConfig): Promise<WizardConfig> {
    const answers = await prompt<{ name: string }>([
      {
        type: 'input',
        name: 'name',
        message: 'New endpoint name',
        initial: config.endpoint_name,
        validate: (value: string) => {
          if (!value || value.trim().length === 0) {
            return 'Name is required';
          }
          if (!/^[a-z0-9_]+$/.test(value)) {
            return 'Name must contain only lowercase letters, numbers, and underscores';
          }
          return true;
        },
      },
    ]);

    return { ...config, endpoint_name: answers.name };
  }

  private static async editPath(config: WizardConfig): Promise<WizardConfig> {
    const answers = await prompt<{ path: string }>([
      {
        type: 'input',
        name: 'path',
        message: 'New URL path',
        initial: config.url_path,
        validate: (value: string) => {
          if (!value.startsWith('/')) {
            return 'URL path must start with /';
          }
          return true;
        },
      },
    ]);

    return { ...config, url_path: answers.path };
  }

  private static async editDescription(
    config: WizardConfig
  ): Promise<WizardConfig> {
    const answers = await prompt<{ description: string }>([
      {
        type: 'input',
        name: 'description',
        message: 'New description',
        initial: config.description || '',
      },
    ]);

    return { ...config, description: answers.description || undefined };
  }

  private static async editMethod(
    config: WizardConfig
  ): Promise<WizardConfig> {
    const answers = await prompt<{ method: string }>([
      {
        type: 'select',
        name: 'method',
        message: 'New HTTP method',
        choices: ['GET', 'POST', 'PUT', 'DELETE'],
        initial: config.method,
      },
    ]);

    return {
      ...config,
      method: answers.method as 'GET' | 'POST' | 'PUT' | 'DELETE',
    };
  }

  private static async editParameters(
    config: WizardConfig
  ): Promise<WizardConfig> {
    this.displayParameters(config.parameters);

    const answers = await prompt<{ action: string }>([
      {
        type: 'select',
        name: 'action',
        message: 'What would you like to do?',
        choices: [
          { name: 'Add parameter', value: 'add' },
          { name: 'Remove parameter', value: 'remove' },
          { name: 'Done', value: 'done' },
        ],
      },
    ]);

    if (answers.action === 'add') {
      return await this.addParameter(config);
    } else if (answers.action === 'remove') {
      return await this.removeParameter(config);
    }

    return config;
  }

  private static async addParameter(config: WizardConfig): Promise<WizardConfig> {
    const paramAnswers = await prompt<ParameterDefinition>([
      {
        type: 'input',
        name: 'name',
        message: 'Parameter name',
        validate: (value: string) => {
          if (!value || value.trim().length === 0) {
            return 'Name is required';
          }
          const exists = config.parameters.some((p) => p.name === value);
          if (exists) {
            return 'A parameter with this name already exists';
          }
          return true;
        },
      },
      {
        type: 'select',
        name: 'type',
        message: 'Parameter type',
        choices: ['string', 'integer', 'number', 'boolean'],
      },
      {
        type: 'confirm',
        name: 'required',
        message: 'Is required?',
        initial: false,
      },
      {
        type: 'select',
        name: 'location',
        message: 'Parameter location',
        choices: [
          { name: 'Query string', value: 'query' },
          { name: 'URL path', value: 'path' },
          { name: 'Request body', value: 'body' },
        ],
      },
      {
        type: 'input',
        name: 'description',
        message: 'Description (optional)',
      },
    ]);

    return {
      ...config,
      parameters: [...config.parameters, paramAnswers as ParameterDefinition],
    };
  }

  private static async removeParameter(config: WizardConfig): Promise<WizardConfig> {
    if (config.parameters.length === 0) {
      console.log(chalk.yellow('No parameters to remove'));
      return config;
    }

    const paramNames = config.parameters.map((p) => p.name);
    const answers = await prompt<{ name: string }>([
      {
        type: 'select',
        name: 'name',
        message: 'Select parameter to remove',
        choices: paramNames,
      },
    ]);

    return {
      ...config,
      parameters: config.parameters.filter((p) => p.name !== answers.name),
    };
  }

  private static async editCache(config: WizardConfig): Promise<WizardConfig> {
    const answers = await prompt<{ enabled: boolean; ttl?: number }>([
      {
        type: 'confirm',
        name: 'enabled',
        message: 'Enable caching?',
        initial: config.enable_cache,
      },
    ]);

    if (!answers.enabled) {
      return { ...config, enable_cache: false, cache_ttl: undefined };
    }

    const ttlAnswers = await prompt<{ ttl: number }>([
      {
        type: 'number',
        name: 'ttl',
        message: 'Cache TTL in seconds',
        initial: config.cache_ttl || 300,
        validate: (value: number) => {
          if (value < 0) {
            return 'TTL must be positive';
          }
          return true;
        },
      },
    ]);

    return {
      ...config,
      enable_cache: true,
      cache_ttl: ttlAnswers.ttl,
    };
  }
}
