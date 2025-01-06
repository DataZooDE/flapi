import { describe, it, expect } from 'vitest';
import { 
  isProjectConfig, 
  validateProjectConfig,
  isEndpointConfig,
  validateEndpointConfig,
  isAuthConfig,
  validateConnectionConfig,
  validateCacheConfig,
  validateAwsSecretsConfig
} from './config';
import { testData } from './config.test.data';

describe('Configuration Type Validation', () => {
  describe('ProjectConfig', () => {
    it('validates a complete project config', () => {
      const result = isProjectConfig(testData.validProjectConfig);
      expect(result).toBe(true);
    });

    it('validates required fields', () => {
      const errors = validateProjectConfig(testData.validProjectConfig);
      expect(errors).toHaveLength(0);
    });

    it('detects missing required fields', () => {
      const invalidConfig = {
        connections: {},
        endpoints: {}
        // Missing project_name and project_description
      };
      const errors = validateProjectConfig(invalidConfig);
      expect(errors).toContain('Missing project name');
      expect(errors).toContain('Missing project description');
    });

    it('validates field types', () => {
      const invalidTypes = {
        project_name: 123, // Should be string
        project_description: true, // Should be string
        connections: {},
        endpoints: {},
      };
      const errors = validateProjectConfig(invalidTypes);
      expect(errors).toContain('Missing project name'); // Because number is not valid
      expect(errors).toContain('Missing project description'); // Because boolean is not valid
    });

    it('validates server configuration types', () => {
      const configWithInvalidPort = {
        ...testData.validProjectConfig,
        server: {
          http_port: "8080", // Should be number
          server_name: "test"
        }
      };
      const errors = validateProjectConfig(configWithInvalidPort);
      expect(errors).toContain('Server http_port must be a number');
    });
  });

  describe('EndpointConfig', () => {
    it('validates a complete endpoint config', () => {
      const result = isEndpointConfig(testData.validEndpointConfig);
      expect(result).toBe(true);
    });

    it('validates URL path format', () => {
      const invalidEndpoint = {
        ...testData.validEndpointConfig,
        url_path: 'invalid-path' // Should start with /
      };
      const errors = validateEndpointConfig(invalidEndpoint);
      expect(errors).toContain('URL path must start with /');
    });

    it('validates cache configuration', () => {
      const invalidCache = {
        ...testData.validEndpointConfig,
        cache: {
          enabled: "true", // Should be boolean
          cache_table_name: "test_cache"
        }
      };
      const errors = validateEndpointConfig(invalidCache);
      expect(errors).toContain('Cache enabled must be a boolean');
    });

    it('validates required endpoint fields', () => {
      const missingFields = {
        url_path: '/test'
        // Missing template_source
      };
      const result = isEndpointConfig(missingFields);
      expect(result).toBe(false);
    });
  });

  describe('CacheConfig', () => {
    it('validates a complete cache config', () => {
      const errors = validateCacheConfig(testData.validEndpointConfig.cache);
      expect(errors).toHaveLength(0);
    });

    it('validates required cache fields', () => {
      const invalidCache = {
        enabled: true
        // Missing cache_table_name
      };
      const errors = validateCacheConfig(invalidCache);
      expect(errors).toContain('Cache table name is required when cache is enabled');
    });

    it('validates refresh time format', () => {
      const invalidRefreshTime = {
        ...testData.validEndpointConfig.cache,
        refresh_time: 'invalid'
      };
      const errors = validateCacheConfig(invalidRefreshTime);
      expect(errors).toContain('Invalid refresh time format');
    });
  });

  describe('ConnectionConfig', () => {
    it('validates a complete connection config', () => {
      const errors = validateConnectionConfig(testData.validConnectionConfig);
      expect(errors).toHaveLength(0);
    });

    it('validates required connection fields', () => {
      const invalidConnection = {
        // Missing type
        properties: {}
      };
      const errors = validateConnectionConfig(invalidConnection);
      expect(errors).toContain('Connection type is required');
    });

    it('validates connection properties', () => {
      const invalidProperties = {
        type: 'duckdb',
        properties: null // Should be object
      };
      const errors = validateConnectionConfig(invalidProperties);
      expect(errors).toContain('Connection properties must be an object');
    });

    it('validates logging options', () => {
      const invalidLogging = {
        ...testData.validConnectionConfig,
        log_queries: "true", // Should be boolean
        log_parameters: 1 // Should be boolean
      };
      const errors = validateConnectionConfig(invalidLogging);
      expect(errors).toContain('log_queries must be a boolean');
      expect(errors).toContain('log_parameters must be a boolean');
    });
  });

  describe('AuthConfig', () => {
    it('validates a complete auth config', () => {
      const result = isAuthConfig(testData.validAwsAuthConfig);
      expect(result).toBe(true);
    });

    it('validates auth type', () => {
      const invalidType = {
        ...testData.validAwsAuthConfig,
        type: 'invalid'
      };
      const result = isAuthConfig(invalidType);
      expect(result).toBe(false);
    });

    it('validates AWS secrets configuration', () => {
      const invalidAws = {
        type: 'aws_secrets',
        properties: {
          // Missing required fields
        }
      };
      const result = isAuthConfig(invalidAws);
      expect(result).toBe(false);
    });
  });

  describe('AWS Secrets Manager', () => {
    it('validates complete AWS secrets configuration', () => {
      const validConfig = {
        secret_name: "my-secret",
        region: "us-east-1",
        credentials: {
          secret_id: "AKIAXXXXXXXX",
          secret_key: "XXXXXXXX"
        },
        init_sql: "SELECT * FROM users",
        secret_table: "auth_users"
      };
      const errors = validateAwsSecretsConfig(validConfig);
      expect(errors).toHaveLength(0);
    });

    it('validates required AWS fields', () => {
      const invalidConfig = {
        // Missing secret_name and region
        credentials: {
          secret_id: "AKIA",
          secret_key: "XXX"
        }
      };
      const errors = validateAwsSecretsConfig(invalidConfig);
      expect(errors).toContain('Missing secret name');
      expect(errors).toContain('Missing region');
    });

    it('validates AWS credentials format', () => {
      const invalidCredentials = {
        secret_name: "my-secret",
        region: "us-east-1",
        credentials: {
          secret_id: "invalid", // Should start with AKIA
          secret_key: ""  // Should not be empty
        }
      };
      const errors = validateAwsSecretsConfig(invalidCredentials);
      expect(errors).toContain('Invalid AWS access key format');
      expect(errors).toContain('Secret key cannot be empty');
    });
  });
}); 