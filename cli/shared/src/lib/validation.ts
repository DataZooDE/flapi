import type { AxiosInstance } from 'axios';
import type { ValidationResult, ReloadResult } from './types';
import { buildEndpointUrl } from './url';

/**
 * Validate endpoint configuration YAML
 */
export async function validateEndpointConfig(
  client: AxiosInstance,
  slug: string,
  yamlContent: string
): Promise<ValidationResult> {
  const url = buildEndpointUrl(slug, 'validate');
  
  const response = await client.post(url, yamlContent, {
    headers: {
      'Content-Type': 'application/x-yaml',
    },
    validateStatus: (status) => status === 200 || status === 400,
  });

  return {
    valid: response.data.valid ?? false,
    errors: response.data.errors ?? [],
    warnings: response.data.warnings ?? [],
  };
}

/**
 * Reload endpoint configuration from disk
 */
export async function reloadEndpointConfig(
  client: AxiosInstance,
  slug: string
): Promise<ReloadResult> {
  const url = buildEndpointUrl(slug, 'reload');
  
  // 400 = the endpoint exists but its YAML does not load (the reason says
  // why); 404 = no such endpoint, which is an error the caller reports.
  const response = await client.post(url, undefined, {
    validateStatus: (status) => status === 200 || status === 400,
  });

  const reason = typeof response.data?.reason === 'string' ? response.data.reason : undefined;
  const message = response.data?.message ?? '';
  return {
    success: response.data?.success ?? false,
    message: reason ? `${message}: ${reason}` : message,
    reason,
  };
}

/**
 * Validate a YAML file
 */
export async function validateYamlFile(
  client: AxiosInstance,
  filePath: string,
  content: string
): Promise<ValidationResult> {
  // Extract slug from file path
  // e.g., /path/to/users.yaml -> users
  const match = filePath.match(/([^/\\]+)\.(yaml|yml)$/);
  if (!match) {
    return {
      valid: false,
      errors: ['Could not extract endpoint name from file path'],
      warnings: [],
    };
  }

  const slug = match[1];
  return validateEndpointConfig(client, slug, content);
}

