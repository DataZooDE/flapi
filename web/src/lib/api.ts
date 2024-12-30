import { API_BASE_URL } from './config';
import type { FlapiConfig, EndpointConfig } from './types';

const API_BASE = `${API_BASE_URL}/api/v1/_config`;

const fetchOptions: RequestInit = {
  credentials: 'include',
  headers: {
    'Content-Type': 'application/json',
  }
};

export async function getProjectConfig(customFetch: typeof fetch = fetch): Promise<FlapiConfig> {
  const response = await customFetch(`${API_BASE}/project`, fetchOptions);
  if (!response.ok) throw new Error('Failed to fetch project config');
  return response.json();
}

export async function saveProjectConfig(config: FlapiConfig, customFetch: typeof fetch = fetch): Promise<void> {
  const response = await customFetch(`${API_BASE}/project`, {
    ...fetchOptions,
    method: 'PUT',
    body: JSON.stringify(config)
  });
  if (!response.ok) throw new Error('Failed to save project config');
}

export async function getEndpoints(customFetch: typeof fetch = fetch): Promise<Record<string, EndpointConfig>> {
  const response = await customFetch(`${API_BASE}/endpoints`, fetchOptions);
  if (!response.ok) throw new Error('Failed to fetch endpoints');
  return response.json();
}

export async function getEndpoint(path: string, customFetch: typeof fetch = fetch): Promise<EndpointConfig> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}`, fetchOptions);
  if (!response.ok) throw new Error('Failed to fetch endpoint');
  return response.json();
}

export async function createEndpoint(config: EndpointConfig, customFetch: typeof fetch = fetch): Promise<void> {
  const response = await customFetch(`${API_BASE}/endpoints`, {
    ...fetchOptions,
    method: 'POST',
    body: JSON.stringify(config)
  });
  if (!response.ok) throw new Error('Failed to create endpoint');
}

export async function updateEndpoint(path: string, config: EndpointConfig, customFetch: typeof fetch = fetch): Promise<void> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}`, {
    ...fetchOptions,
    method: 'PUT',
    body: JSON.stringify(config)
  });
  if (!response.ok) throw new Error('Failed to update endpoint');
}

export async function deleteEndpoint(path: string, customFetch: typeof fetch = fetch): Promise<void> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}`, {
    ...fetchOptions,
    method: 'DELETE'
  });
  if (!response.ok) throw new Error('Failed to delete endpoint');
}

// Template-related endpoints
export async function getEndpointTemplate(path: string, customFetch: typeof fetch = fetch): Promise<string> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}/template`, fetchOptions);
  if (!response.ok) throw new Error('Failed to fetch endpoint template');
  return response.text();
}

export async function updateEndpointTemplate(path: string, template: string, customFetch: typeof fetch = fetch): Promise<void> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}/template`, {
    ...fetchOptions,
    method: 'PUT',
    body: template
  });
  if (!response.ok) throw new Error('Failed to update endpoint template');
}

export async function expandTemplate(path: string, params: Record<string, any>, customFetch: typeof fetch = fetch): Promise<string> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}/template/expand`, {
    ...fetchOptions,
    method: 'POST',
    body: JSON.stringify(params)
  });
  if (!response.ok) throw new Error('Failed to expand template');
  return response.text();
}

export async function testTemplate(path: string, params: Record<string, any>, customFetch: typeof fetch = fetch): Promise<any> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}/template/test`, {
    ...fetchOptions,
    method: 'POST',
    body: JSON.stringify(params)
  });
  if (!response.ok) throw new Error('Failed to test template');
  return response.json();
}

// Cache-related endpoints
export async function getCacheConfig(path: string, customFetch: typeof fetch = fetch): Promise<any> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}/cache`, fetchOptions);
  if (!response.ok) throw new Error('Failed to fetch cache config');
  return response.json();
}

export async function updateCacheConfig(path: string, config: any, customFetch: typeof fetch = fetch): Promise<void> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}/cache`, {
    ...fetchOptions,
    method: 'PUT',
    body: JSON.stringify(config)
  });
  if (!response.ok) throw new Error('Failed to update cache config');
}

export async function getCacheTemplate(path: string, customFetch: typeof fetch = fetch): Promise<string> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}/cache/template`, fetchOptions);
  if (!response.ok) throw new Error('Failed to fetch cache template');
  return response.text();
}

export async function updateCacheTemplate(path: string, template: string, customFetch: typeof fetch = fetch): Promise<void> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}/cache/template`, {
    ...fetchOptions,
    method: 'PUT',
    body: template
  });
  if (!response.ok) throw new Error('Failed to update cache template');
}

export async function refreshCache(path: string, customFetch: typeof fetch = fetch): Promise<void> {
  const response = await customFetch(`${API_BASE}/endpoints/${encodeURIComponent(path)}/cache/refresh`, {
    ...fetchOptions,
    method: 'POST'
  });
  if (!response.ok) throw new Error('Failed to refresh cache');
}

// Schema-related endpoints
export async function getSchema(customFetch: typeof fetch = fetch): Promise<any> {
  const response = await customFetch(`${API_BASE}/schema`, fetchOptions);
  if (!response.ok) throw new Error('Failed to fetch schema');
  return response.json();
}

export async function refreshSchema(customFetch: typeof fetch = fetch): Promise<void> {
  const response = await customFetch(`${API_BASE}/schema/refresh`, {
    ...fetchOptions,
    method: 'POST'
  });
  if (!response.ok) throw new Error('Failed to refresh schema');
}

// Connection management
export async function saveConnection(name: string, config: ConnectionConfig, customFetch: typeof fetch = fetch): Promise<void> {
  // TODO: Implement connection save endpoint in backend
  throw new Error('Not implemented: saveConnection');
}

export async function deleteConnection(name: string, customFetch: typeof fetch = fetch): Promise<void> {
  // TODO: Implement connection delete endpoint in backend
  throw new Error('Not implemented: deleteConnection');
}

// SQL validation
export async function validateSql(sql: string, customFetch: typeof fetch = fetch): Promise<{ valid: boolean; error?: string }> {
  // TODO: Implement SQL validation endpoint in backend
  // For now, return valid if SQL is not empty
  return {
    valid: sql.trim().length > 0,
    error: sql.trim().length === 0 ? 'SQL cannot be empty' : undefined
  };
} 