import enquirer from 'enquirer';
import chalk from 'chalk';

const { prompt } = enquirer;

export interface PromptAnswers {
  creationMethod: 'ai' | 'manual';
  description?: string;
  useAi?: boolean;
  authMethod?: 'api_key' | 'oauth';
  apiKey?: string;
  endpointName?: string;
  urlPath?: string;
  connection?: string;
  table?: string;
  method?: 'GET' | 'POST' | 'PUT' | 'DELETE';
  enableCache?: boolean;
  cacheTtl?: number;
  confirmSave?: boolean;
  editOption?: 'accept' | 'edit' | 'regenerate' | 'manual' | 'cancel';
}

export async function promptCreationMethod(): Promise<'ai' | 'manual'> {
  const answers = await prompt<{ method: 'ai' | 'manual' }>([
    {
      type: 'select',
      name: 'method',
      message: 'How would you like to create the endpoint?',
      choices: [
        { name: 'AI-Assisted (using Google Gemini)', value: 'ai' },
        { name: 'Manual (step-by-step)', value: 'manual' },
      ],
    },
  ]);

  return answers.method;
}

export async function promptNaturalDescription(): Promise<string> {
  console.log(chalk.cyan('\nDescribe the endpoint you want to create:'));
  console.log(chalk.dim('(Enter a description, then press Enter twice to finish)\n'));

  let description = '';
  let emptyLineCount = 0;

  return new Promise((resolve) => {
    const stdin = process.stdin;
    stdin.setEncoding('utf8');

    const onData = (chunk: string) => {
      description += chunk;
      const lines = chunk.split('\n');

      for (const line of lines) {
        if (line.trim() === '') {
          emptyLineCount++;
          if (emptyLineCount >= 2) {
            stdin.removeListener('data', onData);
            stdin.pause();
            resolve(description.trim());
          }
        } else {
          emptyLineCount = 0;
        }
      }
    };

    stdin.on('data', onData);
  });
}

export async function promptAuthMethod(): Promise<'api_key' | 'oauth'> {
  const answers = await prompt<{ method: 'api_key' | 'oauth' }>([
    {
      type: 'select',
      name: 'method',
      message: 'How would you like to authenticate with Gemini?',
      choices: [
        { name: 'API Key', value: 'api_key' },
        { name: 'OAuth (Google Account)', value: 'oauth' },
      ],
    },
  ]);

  return answers.method;
}

export async function promptApiKey(): Promise<string> {
  const answers = await prompt<{ key: string }>([
    {
      type: 'password',
      name: 'key',
      message: 'Enter your Google Gemini API key',
      validate: (value: string) => {
        if (!value || value.trim().length === 0) {
          return 'API key is required';
        }
        return true;
      },
    },
  ]);

  return answers.key;
}

export async function promptEditAction(): Promise<
  'accept' | 'edit' | 'regenerate' | 'manual' | 'cancel'
> {
  const answers = await prompt<{
    action: 'accept' | 'edit' | 'regenerate' | 'manual' | 'cancel';
  }>([
    {
      type: 'select',
      name: 'action',
      message: 'What would you like to do?',
      choices: [
        { name: 'Accept and save', value: 'accept' },
        { name: 'Edit configuration', value: 'edit' },
        { name: 'Regenerate with different description', value: 'regenerate' },
        { name: 'Switch to manual mode', value: 'manual' },
        { name: 'Cancel', value: 'cancel' },
      ],
    },
  ]);

  return answers.action;
}

export async function promptConfirmSave(): Promise<boolean> {
  const answers = await prompt<{ confirm: boolean }>([
    {
      type: 'confirm',
      name: 'confirm',
      message: 'Save endpoint configuration?',
      initial: true,
    },
  ]);

  return answers.confirm;
}

// Manual wizard flow prompts
export async function promptEndpointMetadata(connections: string[]): Promise<{
  name: string;
  urlPath: string;
  description?: string;
  connection: string;
  table: string;
}> {
  const answers = await prompt<{
    name: string;
    urlPath: string;
    description?: string;
    connection: string;
    table: string;
  }>([
    {
      type: 'input',
      name: 'name',
      message: 'Endpoint name (e.g., get_customers)',
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
    {
      type: 'input',
      name: 'urlPath',
      message: 'URL path (e.g., /customers)',
      validate: (value: string) => {
        if (!value || value.trim().length === 0) {
          return 'URL path is required';
        }
        if (!value.startsWith('/')) {
          return 'URL path must start with /';
        }
        return true;
      },
    },
    {
      type: 'input',
      name: 'description',
      message: 'Description (optional)',
    },
    {
      type: 'select',
      name: 'connection',
      message: 'Select connection',
      choices: connections,
    },
    {
      type: 'input',
      name: 'table',
      message: 'Table name',
      validate: (value: string) => {
        if (!value || value.trim().length === 0) {
          return 'Table name is required';
        }
        return true;
      },
    },
  ]);

  return answers;
}

export async function promptHttpMethod(): Promise<
  'GET' | 'POST' | 'PUT' | 'DELETE'
> {
  const answers = await prompt<{
    method: 'GET' | 'POST' | 'PUT' | 'DELETE';
  }>([
    {
      type: 'select',
      name: 'method',
      message: 'HTTP method',
      choices: [
        { name: 'GET (read)', value: 'GET' },
        { name: 'POST (create)', value: 'POST' },
        { name: 'PUT (update)', value: 'PUT' },
        { name: 'DELETE (delete)', value: 'DELETE' },
      ],
    },
  ]);

  return answers.method;
}

export async function promptAddParameter(): Promise<{
  name: string;
  type: 'string' | 'integer' | 'number' | 'boolean';
  required: boolean;
  location: 'query' | 'path' | 'body';
  description?: string;
  addMore: boolean;
}> {
  const answers = await prompt<{
    name: string;
    type: 'string' | 'integer' | 'number' | 'boolean';
    required: boolean;
    location: 'query' | 'path' | 'body';
    description?: string;
    addMore: boolean;
  }>([
    {
      type: 'input',
      name: 'name',
      message: 'Parameter name',
      validate: (value: string) => {
        if (!value || value.trim().length === 0) {
          return 'Parameter name is required';
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
      message: 'Is it required?',
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
    {
      type: 'confirm',
      name: 'addMore',
      message: 'Add another parameter?',
      initial: false,
    },
  ]);

  return answers;
}

export async function promptCacheOptions(): Promise<{
  enabled: boolean;
  ttl?: number;
}> {
  const answers = await prompt<{
    enabled: boolean;
    ttl?: number;
  }>([
    {
      type: 'confirm',
      name: 'enabled',
      message: 'Enable caching?',
      initial: false,
    },
  ]);

  if (answers.enabled) {
    const ttlAnswer = await prompt<{ ttl: number }>([
      {
        type: 'number',
        name: 'ttl',
        message: 'Cache TTL in seconds',
        initial: 300,
        validate: (value: number) => {
          if (value < 0) {
            return 'TTL must be positive';
          }
          return true;
        },
      },
    ]);
    answers.ttl = ttlAnswer.ttl;
  }

  return answers;
}
