import type { EndpointConfig, ConnectionConfig, FlapiConfig } from './types';

const API_BASE = '/api';

// Mock data
const mockConfig: FlapiConfig = {
  project_name: 'Customer API',
  project_description: 'API for managing customer data',
  template: {
    path: './sqls'
  },
  connections: {
    'duckdb': {
      name: 'duckdb',
      init: 'CREATE TABLE IF NOT EXISTS users (id INTEGER, name TEXT, email TEXT);',
      properties: {
        'db_file': './data/db.duckdb'
      },
      log_queries: true,
      log_parameters: false,
      allow: '*.parquet'
    },
    'sqlite': {
      name: 'sqlite',
      init: 'PRAGMA foreign_keys = ON;',
      properties: {
        'db_file': './data/app.db'
      },
      log_queries: false,
      log_parameters: false,
      allow: '*.db'
    }
  }
};

const mockEndpoints: Record<string, EndpointConfig> = {
  'users': {
    urlPath: '/users',
    method: 'GET',
    requestFields: [
      {
        fieldName: 'id',
        fieldIn: 'query',
        description: 'User ID',
        required: false,
        validators: [
          { type: 'int', min: 1, preventSqlInjection: true }
        ]
      }
    ],
    templateSource: 'SELECT * FROM users WHERE {% if id %}id = :id{% else %}1=1{% endif %}',
    connection: ['duckdb'],
    rate_limit: { enabled: true, max: 100, interval: 60 },
    auth: { enabled: false, type: 'basic', users: [] },
    cache: {
      cacheTableName: '',
      cacheSource: '',
      refreshTime: '',
      refreshEndpoint: false,
      maxPreviousTables: 5
    },
    heartbeat: { enabled: false, params: {} }
  }
};

export async function fetchConfig(): Promise<{ flapi: FlapiConfig; endpoints: Record<string, EndpointConfig> }> {
  // Simulate API delay
  await new Promise(resolve => setTimeout(resolve, 100));
  return {
    flapi: mockConfig,
    endpoints: mockEndpoints
  };
}

export async function refreshConfig(): Promise<void> {
  const response = await fetch(`${API_BASE}/config`, { method: 'DELETE' });
  if (!response.ok) {
    throw new Error('Failed to refresh configuration');
  }
}

export async function testEndpoint(endpoint: EndpointConfig, params: Record<string, string>): Promise<any> {
  const queryString = new URLSearchParams(params).toString();
  const response = await fetch(`${API_BASE}${endpoint.urlPath}?${queryString}`);
  if (!response.ok) {
    throw new Error('Failed to test endpoint');
  }
  return response.json();
}

export async function testConnection(connectionName: string): Promise<void> {
  const response = await fetch(`${API_BASE}/connections/${connectionName}/test`);
  if (!response.ok) {
    throw new Error('Failed to test connection');
  }
}

export async function saveEndpoint(endpoint: EndpointConfig): Promise<void> {
  // Simulate API delay
  await new Promise(resolve => setTimeout(resolve, 100));
  mockEndpoints[endpoint.urlPath] = endpoint;
}

export async function deleteEndpoint(urlPath: string): Promise<void> {
  // Simulate API delay
  await new Promise(resolve => setTimeout(resolve, 100));
  delete mockEndpoints[urlPath];
}

export async function saveConnection(name: string, config: ConnectionConfig): Promise<void> {
  // Simulate API delay
  await new Promise(resolve => setTimeout(resolve, 100));
  mockConfig.connections[name] = config;
}

export async function deleteConnection(name: string): Promise<void> {
  // Simulate API delay
  await new Promise(resolve => setTimeout(resolve, 100));
  delete mockConfig.connections[name];
}

export async function validateSql(sql: string): Promise<{ valid: boolean; error?: string }> {
  // Simple mock validation
  if (!sql.trim()) {
    return { valid: false, error: 'SQL cannot be empty' };
  }
  if (!sql.toLowerCase().includes('select') && !sql.toLowerCase().includes('create')) {
    return { valid: false, error: 'SQL must be a SELECT or CREATE statement' };
  }
  return { valid: true };
} 