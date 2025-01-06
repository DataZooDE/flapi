import type { ProjectConfig, EndpointConfig, AuthConfig, ConnectionConfig } from './config';

// Test data based on OpenAPI spec
export const validProjectConfig: ProjectConfig = {
  project_name: "test-project",
  project_description: "Test project configuration",
  endpoints: {},
  connections: {
    "main-db": {
      type: "duckdb",
      init_sql: "CREATE TABLE IF NOT EXISTS test (id INTEGER)",
      properties: {
        "db_path": "/tmp/test.db"
      },
      log_queries: true,
      log_parameters: false
    }
  },
  server: {
    server_name: "localhost",
    http_port: 8080,
    cache_schema: "flapi"
  },
  duckdb_settings: {
    "memory_limit": "4GB",
    "threads": "4"
  }
};

export const validEndpointConfig: EndpointConfig = {
  url_path: "/api/test",
  method: "GET",
  version: "v1",
  template_source: "SELECT * FROM test",
  request: [
    {
      field_name: "id",
      field_in: "query",
      description: "Record ID",
      required: true,
      type: "integer"
    }
  ],
  cache: {
    enabled: true,
    refresh_time: "1h",
    cache_table_name: "test_cache",
    cache_source: "SELECT * FROM test"
  },
  auth: {
    type: "aws_secrets",
    properties: {
      secret_name: "my-secret",
      region: "us-east-1"
    }
  },
  rate_limit: {
    requests_per_second: 10,
    burst_size: 20
  }
};

export const validAwsAuthConfig: AuthConfig = {
  type: "aws_secrets",
  properties: {
    secret_name: "my-secret",
    region: "us-east-1",
    secret_id: "AKIAXXXXXXXX",
    secret_key: "XXXXXXXX",
    secret_table: "auth_users",
    init_sql: "SELECT * FROM auth_users"
  }
};

export const validConnectionConfig: ConnectionConfig = {
  type: "duckdb",
  init_sql: "CREATE TABLE IF NOT EXISTS test (id INTEGER)",
  properties: {
    "db_path": "/tmp/test.db"
  },
  log_queries: true,
  log_parameters: false
};

export const testData = {
  validProjectConfig,
  validEndpointConfig,
  validAwsAuthConfig,
  validConnectionConfig
}; 