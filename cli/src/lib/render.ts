import cliTable3 from 'cli-table3';
import { Console } from './console';
import type { FlapiiConfig } from './types';
import { toCamelCaseKeys, toHyphenCaseKeys } from './casing';
import chalk from 'chalk';

type JsonStyle = 'camel' | 'hyphen';

export function renderConfig(config: FlapiiConfig) {
  if (config.output === 'json') {
    const processed = applyJsonStyle(config, config.jsonStyle);
    Console.info(JSON.stringify(processed, null, 2));
    return;
  }

  Console.info(chalk.cyan('\n📋 Current Configuration'));
  Console.info(chalk.gray('═'.repeat(60)));

  const table = new cliTable3({
    head: [
      chalk.bold.cyan('Setting'),
      chalk.bold.cyan('Value'),
      chalk.bold.yellow('Source')
    ],
    colWidths: [20, 30, 15],
    style: {
      head: [],
      border: [],
      compact: true,
    },
  });

  const configDict = config as unknown as Record<string, unknown>; // shared version
  Object.entries(configDict).forEach(([key, value]) => {
    const source = getConfigSource(key, config);
    table.push([
      chalk.cyan(key.replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase())),
      chalk.white(String(value)),
      chalk.yellow(source)
    ]);
  });

  Console.info(table.toString());
}

export function renderJson(value: unknown, style: JsonStyle = 'camel') {
  const processed = applyJsonStyle(value, style);
  Console.info(JSON.stringify(processed, null, 2));
}

export function renderEndpointsTable(endpoints: Record<string, unknown>) {
  Console.info(chalk.cyan('\n🌐 API Endpoints'));
  Console.info(chalk.gray('═'.repeat(60)));

  const table = new cliTable3({
    head: [
      chalk.bold.cyan('Path'),
      chalk.bold.green('Method'),
      chalk.bold.yellow('Type'),
      chalk.bold.magenta('Operation'),
      chalk.bold.blue('Connection'),
      chalk.bold.red('Cache')
    ],
    colWidths: [25, 8, 10, 12, 15, 8],
    style: {
      head: [],
      border: [],
      compact: true,
    },
  });

  for (const [path, config] of Object.entries(endpoints)) {
    if (!config || typeof config !== 'object') continue;
    const endpoint = config as Record<string, unknown>;
    const method = endpoint.method ?? 'GET';
    const type = determineEndpointType(endpoint);
    const operation = determineOperationType(endpoint, method);
    const connection = Array.isArray(endpoint.connection)
      ? endpoint.connection.join(', ')
      : String(endpoint.connection || 'N/A');
    const cacheEnabled = endpoint.cache && typeof endpoint.cache === 'object'
      ? (endpoint.cache as Record<string, unknown>).enabled === true ? '✓' : '✗'
      : '✗';

    table.push([
      chalk.cyan(path),
      chalk.green(String(method).toUpperCase()),
      chalk.yellow(type),
      chalk.magenta(operation),
      chalk.blue(connection),
      chalk.red(cacheEnabled)
    ]);
  }

  Console.info(table.toString());
}

export function renderEndpointTable(endpoint: Record<string, unknown>) {
  Console.info(chalk.cyan('\n🔍 Endpoint Details'));
  Console.info(chalk.gray('═'.repeat(60)));

  const table = new cliTable3({
    head: [
      chalk.bold.cyan('Property'),
      chalk.bold.white('Value')
    ],
    colWidths: [20, 40],
    style: {
      head: [],
      border: [],
      compact: true,
    },
  });

  const formatValue = (value: unknown): string => {
    if (Array.isArray(value)) {
      return value.join(', ');
    }
    if (value && typeof value === 'object') {
      return JSON.stringify(value, null, 2);
    }
    return String(value || 'N/A');
  };

  // Show operation configuration prominently if present
  if (endpoint.operation && typeof endpoint.operation === 'object') {
    const op = endpoint.operation as Record<string, unknown>;
    const opType = op.type || (isWriteMethod(endpoint.method as string) ? 'write' : 'read');
    const opDetails: string[] = [];
    if (op.validate_before_write === true) opDetails.push('validate');
    if (op.returns_data === true) opDetails.push('returns-data');
    if (op.transaction === true) opDetails.push('transaction');
    
    const opDisplay = opDetails.length > 0 
      ? `${opType} (${opDetails.join(', ')})`
      : opType;
    
    table.push([
      chalk.bold.magenta('Operation'),
      chalk.magenta(opDisplay)
    ]);
  } else {
    // Infer operation type from method
    const inferredOp = isWriteMethod(endpoint.method as string) ? 'write' : 'read';
    table.push([
      chalk.bold.magenta('Operation'),
      chalk.gray(inferredOp + ' (inferred)')
    ]);
  }

  for (const [key, value] of Object.entries(endpoint)) {
    // Skip operation field as we already displayed it
    if (key === 'operation') continue;
    
    const displayKey = key.replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
    table.push([
      chalk.cyan(displayKey),
      chalk.white(formatValue(value))
    ]);
  }

  Console.info(table.toString());
}

export function renderMcpToolsTable(entries: { path: string; config: Record<string, unknown> }[]) {
  Console.info(chalk.cyan('\n🔧 MCP Tools'));
  Console.info(chalk.gray('═'.repeat(60)));

  const table = new cliTable3({
    head: [
      chalk.bold.cyan('Path'),
      chalk.bold.green('Name'),
      chalk.bold.magenta('Operation'),
      chalk.bold.yellow('Description')
    ],
    colWidths: [25, 20, 12, 25],
    style: {
      head: [],
      border: [],
      compact: true,
    },
  });

  for (const { path, config } of entries) {
    const tool = (config['mcp-tool'] ?? config['mcpTool']) as Record<string, unknown>;
    const method = config.method as string || 'GET';
    const operation = determineOperationType(config, method);
    table.push([
      chalk.cyan(path),
      chalk.green(String(tool?.name ?? 'n/a')),
      chalk.magenta(operation),
      chalk.yellow(String(tool?.description ?? ''))
    ]);
  }

  Console.info(table.toString());
}

export function renderMcpResourcesTable(entries: { path: string; config: Record<string, unknown> }[]) {
  Console.info(chalk.cyan('\n📁 MCP Resources'));
  Console.info(chalk.gray('═'.repeat(60)));

  const table = new cliTable3({
    head: [
      chalk.bold.cyan('Path'),
      chalk.bold.green('Name'),
      chalk.bold.yellow('Description')
    ],
    colWidths: [25, 20, 35],
    style: {
      head: [],
      border: [],
      compact: true,
    },
  });

  for (const { path, config } of entries) {
    const resource = (config['mcp-resource'] ?? config['mcpResource']) as Record<string, unknown>;
    table.push([
      chalk.cyan(path),
      chalk.green(String(resource?.name ?? 'n/a')),
      chalk.yellow(String(resource?.description ?? ''))
    ]);
  }

  Console.info(table.toString());
}

export function renderMcpPromptsTable(entries: { path: string; config: Record<string, unknown> }[]) {
  Console.info(chalk.cyan('\n💬 MCP Prompts'));
  Console.info(chalk.gray('═'.repeat(60)));

  const table = new cliTable3({
    head: [
      chalk.bold.cyan('Path'),
      chalk.bold.green('Name'),
      chalk.bold.yellow('Description')
    ],
    colWidths: [25, 20, 35],
    style: {
      head: [],
      border: [],
      compact: true,
    },
  });

  for (const { path, config } of entries) {
    const prompt = (config['mcp-prompt'] ?? config['mcpPrompt']) as Record<string, unknown>;
    table.push([
      chalk.cyan(path),
      chalk.green(String(prompt?.name ?? 'n/a')),
      chalk.yellow(String(prompt?.description ?? ''))
    ]);
  }

  Console.info(table.toString());
}

export function renderTemplatesTable(templates: Record<string, any>) {
  Console.info(chalk.cyan('\n📄 Templates'));
  Console.info(chalk.gray('═'.repeat(60)));

  const table = new cliTable3({
    head: [
      chalk.bold.cyan('Path'),
      chalk.bold.green('Template Source'),
      chalk.bold.yellow('Template Path')
    ],
    colWidths: [25, 35, 20],
    style: {
      head: [],
      border: [],
      compact: true,
    },
  });

  for (const [path, template] of Object.entries(templates)) {
    const templateSource = template['template-source'] || template.templateSource || 'N/A';
    const templatePath = template['template-path'] || template.templatePath || 'N/A';

    table.push([
      chalk.cyan(path),
      chalk.green(String(templateSource)),
      chalk.yellow(String(templatePath))
    ]);
  }

  Console.info(table.toString());
}

export function renderCacheTable(caches: Record<string, any>) {
  Console.info(chalk.cyan('\n💾 Cache Configurations'));
  Console.info(chalk.gray('═'.repeat(60)));

  const table = new cliTable3({
    head: [
      chalk.bold.cyan('Path'),
      chalk.bold.green('Enabled'),
      chalk.bold.yellow('Refresh Time'),
      chalk.bold.blue('Cache Table'),
      chalk.bold.red('Cache Source')
    ],
    colWidths: [20, 10, 15, 20, 35],
    style: {
      head: [],
      border: [],
      compact: true,
    },
  });

  for (const [path, cache] of Object.entries(caches)) {
    // Skip empty paths
    if (!path || path.trim() === '') {
      continue;
    }

    const enabled = cache?.enabled === true ? '✓' : '✗';
    const refreshTime = cache?.refreshTime || 'N/A';
    const cacheTable = cache?.cacheTable || 'N/A';
    const cacheSource = cache?.cacheSource ? 
      (cache.cacheSource.length > 30 ? '...' + cache.cacheSource.slice(-27) : cache.cacheSource) : 
      'N/A';

    table.push([
      chalk.cyan(path),
      cache?.enabled === true ? chalk.green(enabled) : chalk.red(enabled),
      chalk.yellow(String(refreshTime)),
      chalk.blue(String(cacheTable)),
      chalk.gray(String(cacheSource))
    ]);
  }

  Console.info(table.toString());
}

function applyJsonStyle(value: unknown, style: JsonStyle): unknown {
  if (!value || typeof value !== 'object') {
    return value;
  }

  if (Array.isArray(value)) {
    return value.map((item) => applyJsonStyle(item, style));
  }

  const transformer = style === 'camel' ? toCamelCaseKeys : toHyphenCaseKeys;
  const transformed = transformer(value as Record<string, unknown>);
  for (const [key, nested] of Object.entries(transformed)) {
    transformed[key] = applyJsonStyle(nested, style);
  }
  return transformed;
}

function formatValue(value: unknown): string {
  if (Array.isArray(value)) {
    return value.join(', ');
  }
  if (value && typeof value === 'object') {
    return JSON.stringify(value, null, 2);
  }
  return String(value);
}

function determineEndpointType(config: Record<string, unknown>): string {
  if ('mcp-tool' in config || 'mcpTool' in config) return 'MCP Tool';
  if ('mcp-resource' in config || 'mcpResource' in config) return 'MCP Resource';
  if ('mcp-prompt' in config || 'mcpPrompt' in config) return 'MCP Prompt';
  return 'REST';
}

function determineOperationType(config: Record<string, unknown>, method: string): string {
  // Check explicit operation configuration
  if (config.operation && typeof config.operation === 'object') {
    const op = config.operation as Record<string, unknown>;
    const opType = op.type;
    if (opType === 'write') return 'Write';
    if (opType === 'read') return 'Read';
  }
  
  // Infer from HTTP method
  if (isWriteMethod(method)) return 'Write';
  return 'Read';
}

function isWriteMethod(method: string): boolean {
  const upperMethod = String(method).toUpperCase();
  return ['POST', 'PUT', 'PATCH', 'DELETE'].includes(upperMethod);
}

function getConfigSource(key: string, config: FlapiiConfig): string {
  // This is a simplified version - in a real implementation you'd track sources
  if (key === 'baseUrl' && config.baseUrl) return 'flag/env';
  if (key === 'configPath' && config.configPath) return 'flag';
  if (key === 'authToken' && config.authToken) return 'flag/env';
  return 'default';
}

