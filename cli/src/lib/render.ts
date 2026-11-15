import cliTable3 from 'cli-table3';
import { Console } from './console';
import type { FlapiiConfig } from './types';
import { toCamelCaseKeys, toHyphenCaseKeys, renameKey } from './casing';
import chalk from 'chalk';

type JsonStyle = 'camel' | 'hyphen';

/** The client's own settings include the config-service token; never print it. */
function redactSecrets(config: FlapiiConfig): Record<string, unknown> {
  const out: Record<string, unknown> = { ...(config as unknown as Record<string, unknown>) };
  for (const key of Object.keys(out)) {
    if (/token|secret|password|api[-_]?key/i.test(key) && out[key] !== undefined && out[key] !== '') {
      out[key] = '<redacted>';
    }
  }
  return out;
}

export function renderConfig(config: FlapiiConfig) {
  if (config.output === 'json') {
    const processed = applyJsonStyle(redactSecrets(config), config.jsonStyle);
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

  const configDict = redactSecrets(config);
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

/**
 * Print a DATA payload as JSON, exactly as the server sent it: metrics, audit
 * rows, schema, expand/test results, health, query results. Never re-cased.
 *
 * `--json-style` used to be applied to everything, and the keys of a data
 * payload are data: endpoint paths ("/customers" became "customers"), table
 * and column names, status classes ("2xx" became "2Xx").
 */
export function renderJson(value: unknown) {
  Console.info(JSON.stringify(value, null, 2));
}

/**
 * How a config document is laid out:
 *  - 'document': one config object (an endpoint, a cache block, the project),
 *    or a list of them;
 *  - 'keyed':    an object keyed by an identifier (endpoint path) whose VALUES
 *    are config objects - the keys are kept verbatim.
 */
export type ConfigShape = 'document' | 'keyed';

// Inside a config document, the children of these keys are named by the user
// (parameter names, connection properties, schema properties), not by flAPI.
const VERBATIM_CHILD_KEYS = new Set([
  'properties', 'params', 'parameters', 'variables', 'env', 'headers', 'defaults',
  'inputSchema', 'input_schema', 'input-schema', 'outputSchema', 'output_schema', 'output-schema',
]);

/** Re-case the FIELD NAMES of a config document (`--json-style`), never its data keys. */
export function applyConfigJsonStyle(value: unknown, style: JsonStyle, shape: ConfigShape = 'document'): unknown {
  if (shape === 'keyed' && value && typeof value === 'object' && !Array.isArray(value)) {
    const out: Record<string, unknown> = {};
    for (const [key, nested] of Object.entries(value as Record<string, unknown>)) {
      out[key] = applyConfigJsonStyle(nested, style, 'document');
    }
    return out;
  }
  return restyleDocument(value, style);
}

export function renderConfigJson(value: unknown, style: JsonStyle, shape: ConfigShape = 'document') {
  Console.info(JSON.stringify(applyConfigJsonStyle(value, style, shape), null, 2));
}

function restyleDocument(value: unknown, style: JsonStyle): unknown {
  if (Array.isArray(value)) {
    return value.map((item) => restyleDocument(item, style));
  }
  if (!value || typeof value !== 'object') {
    return value;
  }
  const out: Record<string, unknown> = {};
  for (const [key, nested] of Object.entries(value as Record<string, unknown>)) {
    out[renameKey(key, style)] = VERBATIM_CHILD_KEYS.has(key) ? nested : restyleDocument(nested, style);
  }
  return out;
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
    colWidths: [25, 8, 15, 12, 15, 8],
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

export function determineEndpointType(config: Record<string, unknown>): string {
  // A file with a url-path AND an mcp-* block serves both; it showed as
  // "MCP Tool" only, hiding that it is a REST route as well.
  const rest = Boolean(config['url-path'] ?? config.urlPath);
  const kind =
    'mcp-tool' in config || 'mcpTool' in config
      ? 'Tool'
      : 'mcp-resource' in config || 'mcpResource' in config
        ? 'Resource'
        : 'mcp-prompt' in config || 'mcpPrompt' in config
          ? 'Prompt'
          : undefined;
  if (kind && rest) return `REST+${kind}`;
  if (kind) return `MCP ${kind}`;
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

export function renderTable(data: Record<string, unknown>[]): void {
  if (data.length === 0) {
    Console.info(chalk.dim('No data to display'));
    return;
  }

  // Get all keys from first object
  const keys = Object.keys(data[0]);
  const headers = keys.map((key) =>
    chalk.bold.cyan(key.replace(/_/g, ' ').replace(/\b\w/g, (l) => l.toUpperCase()))
  );

  // Calculate column widths
  const colWidths = keys.map((key) => {
    const maxLength = Math.max(
      key.length,
      ...data.map((row) => String(row[key] || '').length)
    );
    return Math.min(maxLength + 2, 30);
  });

  const table = new cliTable3({
    head: headers,
    colWidths,
    style: {
      head: [],
      border: [],
      compact: true,
    },
  });

  for (const row of data) {
    const values = keys.map((key) => {
      const value = row[key];
      const formatted = formatValue(value);
      return formatted.length > 30 ? formatted.slice(0, 27) + '...' : formatted;
    });
    table.push(values);
  }

  Console.info(table.toString());
}

