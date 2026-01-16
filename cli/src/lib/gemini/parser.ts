import { GeminiGeneratedConfig, WizardConfig, ParameterDefinition } from '../types';

/**
 * Validates and converts Gemini-generated config to WizardConfig
 */
export class ConfigParser {
  static validateAndConvert(
    aiConfig: GeminiGeneratedConfig
  ): WizardConfig {
    // Validate required fields
    this.validateRequired(aiConfig);

    // Validate method
    this.validateMethod(aiConfig.method);

    // Validate URL path
    this.validateUrlPath(aiConfig.url_path);

    // Validate parameters
    this.validateParameters(aiConfig.parameters);

    // Convert to WizardConfig format
    const wizardConfig: WizardConfig = {
      endpoint_name: aiConfig.endpoint_name.trim(),
      url_path: aiConfig.url_path.trim(),
      description: aiConfig.description?.trim(),
      connection: aiConfig.connection.trim(),
      table: aiConfig.table.trim(),
      method: aiConfig.method,
      parameters: aiConfig.parameters.map((p) => this.normalizeParameter(p)),
      enable_cache: aiConfig.enable_cache === true,
      cache_ttl: aiConfig.cache_ttl,
    };

    return wizardConfig;
  }

  private static validateRequired(config: any): void {
    const required = [
      'endpoint_name',
      'url_path',
      'connection',
      'table',
      'method',
      'parameters',
      'enable_cache',
    ];

    for (const field of required) {
      if (!(field in config) || config[field] === null || config[field] === undefined) {
        throw new Error(
          `Validation error: Missing or invalid required field '${field}'`
        );
      }
    }
  }

  private static validateMethod(
    method: string
  ): asserts method is 'GET' | 'POST' | 'PUT' | 'DELETE' {
    if (!['GET', 'POST', 'PUT', 'DELETE'].includes(method)) {
      throw new Error(
        `Validation error: Invalid HTTP method '${method}'. Must be GET, POST, PUT, or DELETE.`
      );
    }
  }

  private static validateUrlPath(path: string): void {
    if (typeof path !== 'string' || !path.startsWith('/')) {
      throw new Error(
        `Validation error: URL path must be a string starting with '/'. Got: '${path}'`
      );
    }

    // Check for valid path characters
    if (!/^\/[a-zA-Z0-9\/_\-{}]*$/.test(path)) {
      throw new Error(
        `Validation error: URL path contains invalid characters. Path: '${path}'`
      );
    }
  }

  private static validateParameters(params: any): void {
    if (!Array.isArray(params)) {
      throw new Error('Validation error: Parameters must be an array');
    }

    for (let i = 0; i < params.length; i++) {
      const param = params[i];

      if (!param.name) {
        throw new Error(
          `Validation error: Parameter ${i} is missing 'name' field`
        );
      }

      if (!param.type) {
        throw new Error(
          `Validation error: Parameter '${param.name}' is missing 'type' field`
        );
      }

      const validTypes = ['string', 'integer', 'number', 'boolean'];
      if (!validTypes.includes(param.type)) {
        throw new Error(
          `Validation error: Parameter '${param.name}' has invalid type '${param.type}'. Must be one of: ${validTypes.join(', ')}`
        );
      }

      if (typeof param.required !== 'boolean') {
        throw new Error(
          `Validation error: Parameter '${param.name}' missing 'required' boolean field`
        );
      }

      const validLocations = ['query', 'path', 'body'];
      if (!validLocations.includes(param.location)) {
        throw new Error(
          `Validation error: Parameter '${param.name}' has invalid location '${param.location}'. Must be one of: ${validLocations.join(', ')}`
        );
      }
    }
  }

  private static normalizeParameter(param: any): ParameterDefinition {
    return {
      name: String(param.name).trim(),
      type: param.type as
        | 'string'
        | 'integer'
        | 'number'
        | 'boolean',
      required: Boolean(param.required),
      location: param.location as 'query' | 'path' | 'body',
      description: param.description ? String(param.description).trim() : undefined,
      defaultValue: param.defaultValue,
      validators: param.validators || [],
    };
  }

  /**
   * Merges user edits with AI-generated config
   */
  static mergeEdits(
    original: GeminiGeneratedConfig,
    edits: Partial<WizardConfig>
  ): WizardConfig {
    return {
      endpoint_name: edits.endpoint_name || original.endpoint_name,
      url_path: edits.url_path || original.url_path,
      description: edits.description || original.description,
      connection: edits.connection || original.connection,
      table: edits.table || original.table,
      method: edits.method || original.method,
      parameters: edits.parameters || original.parameters,
      enable_cache: edits.enable_cache !== undefined ? edits.enable_cache : original.enable_cache,
      cache_ttl: edits.cache_ttl !== undefined ? edits.cache_ttl : original.cache_ttl,
    };
  }
}

/**
 * Validates that a generated config can be sent to the flAPI server
 */
export class FlApiConfigValidator {
  static validateForServer(config: WizardConfig): string[] {
    const errors: string[] = [];

    // Validate endpoint name format
    if (!/^[a-z0-9_]+$/.test(config.endpoint_name)) {
      errors.push(
        `Invalid endpoint name '${config.endpoint_name}'. Must contain only lowercase letters, numbers, and underscores.`
      );
    }

    // Validate URL path format
    if (!/^\/[a-zA-Z0-9\/_\-{}]*$/.test(config.url_path)) {
      errors.push(
        `Invalid URL path '${config.url_path}'. Must start with / and contain only alphanumeric characters, /, -, _, or {}.`
      );
    }

    // Check for path parameter consistency
    const pathParams = this.extractPathParams(config.url_path);
    const definedPathParams = config.parameters
      .filter((p) => p.location === 'path')
      .map((p) => p.name);

    for (const pathParam of pathParams) {
      if (!definedPathParams.includes(pathParam)) {
        errors.push(
          `Path parameter '{${pathParam}}' in URL is not defined in parameters list.`
        );
      }
    }

    for (const definedParam of definedPathParams) {
      if (!pathParams.includes(definedParam)) {
        errors.push(
          `Parameter '${definedParam}' is marked as path parameter but not found in URL path.`
        );
      }
    }

    // Validate cache TTL if caching is enabled
    if (config.enable_cache && config.cache_ttl !== undefined) {
      if (typeof config.cache_ttl !== 'number' || config.cache_ttl < 0) {
        errors.push(
          `Invalid cache TTL '${config.cache_ttl}'. Must be a non-negative number.`
        );
      }
    }

    return errors;
  }

  private static extractPathParams(path: string): string[] {
    const regex = /\{([^}]+)\}/g;
    const params: string[] = [];
    let match;

    while ((match = regex.exec(path)) !== null) {
      params.push(match[1]);
    }

    return params;
  }
}
