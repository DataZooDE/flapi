# flAPI Configuration Reference

**Version:** 1.0.0
**flAPI Version:** >= 1.0.0
**DuckDB Version:** >= 1.4.3

This document provides a complete reference for all configuration options in flAPI, the SQL-to-API framework that generates REST APIs and MCP tools from SQL templates.

---

## Table of Contents

1. [Overview](#1-overview)
   - [Configuration Philosophy](#configuration-philosophy)
   - [File Organization](#file-organization)
   - [Quick Start](#quick-start)
2. [Main Configuration (flapi.yaml)](#2-main-configuration-flapiyaml)
   - [2.1 Project Metadata](#21-project-metadata)
   - [2.2 Template Configuration](#22-template-configuration)
   - [2.3 Connections](#23-connections)
   - [2.4 DuckDB Configuration](#24-duckdb-configuration)
   - [2.5 DuckLake Configuration](#25-ducklake-configuration)
   - [2.6 MCP Configuration](#26-mcp-configuration)
   - [2.7 Authentication (Global)](#27-authentication-global)
   - [2.8 Rate Limiting (Global)](#28-rate-limiting-global)
   - [2.9 HTTPS Configuration](#29-https-configuration)
   - [2.10 Heartbeat Configuration](#210-heartbeat-configuration)
   - [2.11 Storage Configuration (VFS)](#211-storage-configuration-vfs)
3. [Endpoint Configuration](#3-endpoint-configuration)
   - [3.1 REST Endpoints](#31-rest-endpoints)
   - [3.2 MCP Tools](#32-mcp-tools)
   - [3.3 MCP Resources](#33-mcp-resources)
   - [3.4 MCP Prompts](#34-mcp-prompts)
   - [3.5 Configuration Includes](#35-configuration-includes)
4. [Request Parameters](#4-request-parameters)
   - [4.1 Field Definition](#41-field-definition)
   - [4.2 Parameter Locations](#42-parameter-locations)
   - [4.3 Default Values](#43-default-values)
5. [Validators](#5-validators)
   - [5.1 Integer Validator](#51-integer-validator)
   - [5.2 String Validator](#52-string-validator)
   - [5.3 Enum Validator](#53-enum-validator)
   - [5.4 Email Validator](#54-email-validator)
   - [5.5 UUID Validator](#55-uuid-validator)
   - [5.6 Date Validator](#56-date-validator)
   - [5.7 Time Validator](#57-time-validator)
6. [Cache Configuration](#6-cache-configuration)
   - [6.1 Basic Cache Settings](#61-basic-cache-settings)
   - [6.2 Refresh Modes](#62-refresh-modes)
   - [6.3 Retention Policies](#63-retention-policies)
   - [6.4 Cache Template Variables](#64-cache-template-variables)
7. [Authentication](#7-authentication)
   - [7.1 Basic Authentication](#71-basic-authentication)
   - [7.2 JWT Authentication](#72-jwt-authentication)
   - [7.3 Bearer Authentication](#73-bearer-authentication)
   - [7.4 OIDC Authentication](#74-oidc-authentication)
   - [7.5 AWS Secrets Manager](#75-aws-secrets-manager)
8. [Operation Configuration](#8-operation-configuration)
   - [8.1 Read Operations](#81-read-operations)
   - [8.2 Write Operations](#82-write-operations)
9. [SQL Templates (Mustache)](#9-sql-templates-mustache)
   - [9.1 Variable Syntax](#91-variable-syntax)
   - [9.2 Available Contexts](#92-available-contexts)
   - [9.3 Conditional Sections](#93-conditional-sections)
   - [9.4 Best Practices](#94-best-practices)
10. [Environment Variables](#10-environment-variables)
    - [10.1 Substitution Syntax](#101-substitution-syntax)
    - [10.2 Whitelist Configuration](#102-whitelist-configuration)
- [Appendix A: Complete Example Configuration](#appendix-a-complete-example-configuration)
- [Appendix B: Naming Conventions](#appendix-b-naming-conventions)
- [Appendix C: Default Values Reference](#appendix-c-default-values-reference)

---

## 1. Overview

### Configuration Philosophy

flAPI follows a **declarative configuration philosophy**:

- **YAML-based**: All configuration uses human-readable YAML format
- **SQL-first**: Business logic lives in SQL templates, not compiled code
- **Separation of concerns**: Main config, endpoint definitions, and SQL templates are separate files
- **Convention over configuration**: Sensible defaults reduce boilerplate
- **Environment-aware**: Support for environment variables enables dev/staging/prod configurations

### File Organization

A typical flAPI project has the following structure:

```
project/
├── flapi.yaml              # Main configuration file
├── sqls/                   # Endpoint definitions and SQL templates
│   ├── customers.yaml      # Endpoint configuration
│   ├── customers.sql       # SQL template
│   ├── orders.yaml
│   ├── orders.sql
│   └── common/             # Shared configuration fragments
│       └── auth.yaml
├── data/                   # Local data files (Parquet, CSV, etc.)
└── cache/                  # DuckLake cache storage (auto-created)
```

### Quick Start

Minimal configuration to get started:

```yaml
# flapi.yaml
project-name: my-api

template:
  path: ./sqls

connections:
  local-data:
    properties:
      path: ./data/customers.parquet

duckdb:
  access_mode: READ_WRITE
```

```yaml
# sqls/customers.yaml
url-path: /customers
method: GET
template-source: customers.sql
connection:
  - local-data
```

```sql
-- sqls/customers.sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
{{#params.id}}
WHERE id = {{ params.id }}
{{/params.id}}
LIMIT 100
```

---

## 2. Main Configuration (flapi.yaml)

The main configuration file defines global settings, connections, and server behavior.

### 2.1 Project Metadata

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `project-name` | string | - | Human-readable project name |
| `project-description` | string | - | Project description |
| `server-name` | string | `"localhost"` | Server hostname for generated URLs |
| `http-port` | integer | `8080` | HTTP server port |

**Example:**

```yaml
project-name: Customer API
project-description: REST API for customer data access
server-name: api.example.com
http-port: 8080
```

> **Implementation:** `src/config_manager.cpp` | **Tests:** `test/cpp/config_manager_test.cpp`

### 2.2 Template Configuration

Defines where endpoint configurations and SQL templates are located.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `template.path` | string | **required** | Directory containing endpoint YAML and SQL files |
| `template.environment-whitelist` | array[string] | `[]` | Regex patterns for allowed environment variables |

**Example:**

```yaml
template:
  path: ./sqls
  environment-whitelist:
    - '^FLAPI_.*'
    - '^DB_.*'
    - '^API_KEY$'
```

**Notes:**
- The `path` is relative to the main configuration file location
- Environment variables must match at least one whitelist pattern to be substituted
- Use `^` and `$` anchors for exact matches

> **Implementation:** `src/config_manager.cpp`, `src/sql_template_processor.cpp` | **Tests:** `test/cpp/sql_template_processor_test.cpp`

### 2.3 Connections

Connections define data sources accessible in SQL templates.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `connections.<name>` | object | - | Connection configuration keyed by unique name |
| `connections.<name>.init` | string | - | SQL initialization commands (load extensions, create secrets) |
| `connections.<name>.log-queries` | boolean | `false` | Log SQL queries to this connection |
| `connections.<name>.log-parameters` | boolean | `false` | Log parameter values |
| `connections.<name>.allow` | string | - | Access control list |
| `connections.<name>.properties` | object | - | Custom key-value properties accessible in templates |

**Example:**

```yaml
connections:
  # Local file connection
  customers-parquet:
    properties:
      path: ./data/customers.parquet

  # PostgreSQL connection
  postgres-db:
    init: |
      INSTALL postgres;
      LOAD postgres;
    properties:
      host: db.example.com
      port: '5432'
      database: mydb
      user: '${DB_USER}'
      password: '${DB_PASSWORD}'

  # BigQuery connection
  bigquery-data:
    init: |
      INSTALL 'bigquery' FROM community;
      LOAD 'bigquery';
    log-queries: true
    properties:
      project_id: my-gcp-project
```

**Notes:**
- Connection properties are accessible in templates as `{{ conn.property_name }}`
- The `init` SQL runs once when the connection is first used
- Use environment variable substitution for sensitive values

> **Implementation:** `src/config_manager.cpp`, `src/database_manager.cpp` | **Tests:** `test/cpp/config_manager_test.cpp`

### 2.4 DuckDB Configuration

Configures the embedded DuckDB database engine.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `duckdb.db_path` | string | `:memory:` | Path to persistent database file |
| `duckdb.access_mode` | string | `READ_WRITE` | Database access mode: `READ_WRITE` or `READ_ONLY` |
| `duckdb.threads` | integer | auto | Number of threads for query parallelism |
| `duckdb.max_memory` | string | auto | Memory limit (e.g., `4GB`, `512MB`) |
| `duckdb.default_order` | string | - | Default sort order: `ASC` or `DESC` |
| `duckdb.<setting>` | any | - | Any DuckDB configuration setting |

**Example:**

```yaml
duckdb:
  db_path: ./flapi_cache.db
  access_mode: READ_WRITE
  threads: 8
  max_memory: 8GB
  default_order: DESC
```

**Notes:**
- Omit `db_path` or set to `:memory:` for an in-memory database
- Any key-value pairs are passed directly as DuckDB settings

> **Implementation:** `src/database_manager.cpp` | **Tests:** `test/cpp/database_manager_test.cpp`

### 2.5 DuckLake Configuration

Configures DuckLake for snapshot-based caching with time-travel capabilities.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ducklake.enabled` | boolean | `false` | Enable DuckLake caching system |
| `ducklake.alias` | string | `"cache"` | Catalog alias for DuckLake tables |
| `ducklake.metadata-path` | string | - | Directory for DuckLake metadata |
| `ducklake.data-path` | string | - | Directory for DuckLake data files |
| `ducklake.data-inlining-row-limit` | integer | - | Maximum rows to inline in metadata |

**Retention Configuration:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ducklake.retention.keep-last-snapshots` | integer | - | Number of snapshots to retain per table |
| `ducklake.retention.max-snapshot-age` | string | - | Delete snapshots older than this (e.g., `30d`, `7d`) |

**Compaction Configuration:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ducklake.compaction.enabled` | boolean | `false` | Enable automatic compaction |
| `ducklake.compaction.schedule` | string | - | Cron schedule (e.g., `@daily`, `@hourly`) |

**Scheduler Configuration:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ducklake.scheduler.enabled` | boolean | `false` | Enable cache refresh scheduler |
| `ducklake.scheduler.scan-interval` | string | - | Check interval (e.g., `5m`, `1h`) |

**Example:**

```yaml
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./data/cache.ducklake
  data-path: ./data/cache
  retention:
    keep-last-snapshots: 10
    max-snapshot-age: 30d
  compaction:
    enabled: true
    schedule: '@daily'
  scheduler:
    enabled: true
    scan-interval: 5m
```

> **Implementation:** `src/cache_manager.cpp` | **Tests:** `test/cpp/database_manager_ducklake_test.cpp`, `test/integration/test_ducklake_cache.tavern.yaml`

### 2.6 MCP Configuration

Configures the Model Context Protocol (MCP) server.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp.enabled` | boolean | `true` | Enable MCP server |
| `mcp.port` | integer | `8081` | MCP server port |
| `mcp.host` | string | - | MCP server host |
| `mcp.allow-list-changed-notifications` | boolean | - | Enable list change notifications |
| `mcp.instructions` | string | - | Inline instructions for LLM clients |
| `mcp.instructions-file` | string | - | Path to instructions markdown file |

**MCP Authentication:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp.auth.enabled` | boolean | `false` | Enable MCP authentication |
| `mcp.auth.type` | string | - | Auth type: `basic`, `bearer`, `oidc` |
| `mcp.auth.methods.<method>.required` | boolean | `true` | Per-method authentication requirement |

**Example:**

```yaml
mcp:
  enabled: true
  port: 8081
  host: localhost
  allow-list-changed-notifications: true
  instructions-file: ./mcp_instructions.md
  auth:
    enabled: true
    type: bearer
    jwt-secret: '${MCP_JWT_SECRET}'
```

**Notes:**
- MCP server is auto-enabled when endpoints define `mcp-tool`, `mcp-resource`, or `mcp-prompt`
- Use `instructions-file` for large instruction sets, `instructions` for inline content

> **Implementation:** `src/mcp_server.cpp`, `src/mcp_route_handlers.cpp` | **Tests:** `test/cpp/mcp_server_test.cpp`, `test/integration/test_mcp_methods.py`

### 2.7 Authentication (Global)

Global authentication settings that apply to all endpoints unless overridden.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `auth.enabled` | boolean | `false` | Enable authentication globally |
| `auth.type` | string | - | Default auth type: `basic`, `jwt`, `bearer`, `oidc` |
| `auth.jwt-secret` | string | - | JWT signing key |
| `auth.jwt-issuer` | string | - | Expected JWT issuer claim |
| `auth.users` | array | - | User list for basic auth |

See [Section 7: Authentication](#7-authentication) for detailed configuration options.

> **Implementation:** `src/auth_middleware.cpp` | **Tests:** `test/cpp/auth_middleware_test.cpp`, `test/integration/test_customers.tavern.yaml`

### 2.8 Rate Limiting (Global)

Global rate limiting settings.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `rate_limit.enabled` | boolean | `false` | Enable rate limiting globally |
| `rate_limit.max` | integer | `100` | Maximum requests per interval |
| `rate_limit.interval` | integer | `60` | Time window in seconds |

**Example:**

```yaml
rate_limit:
  enabled: true
  max: 100
  interval: 60
```

> **Implementation:** `src/rate_limit_middleware.cpp` | **Tests:** *None - see [TEST_TODO.md](./TEST_TODO.md)*

### 2.9 HTTPS Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enforce-https.enabled` | boolean | `false` | Force HTTPS for all connections |
| `enforce-https.ssl-cert-file` | string | - | Path to SSL certificate |
| `enforce-https.ssl-key-file` | string | - | Path to SSL private key |

**Example:**

```yaml
enforce-https:
  enabled: true
  ssl-cert-file: ./ssl/cert.pem
  ssl-key-file: ./ssl/key.pem
```

> **Implementation:** `src/api_server.cpp` | **Tests:** *None - see [TEST_TODO.md](./TEST_TODO.md)*

### 2.10 Heartbeat Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `heartbeat.enabled` | boolean | `false` | Enable global heartbeat worker |
| `heartbeat.worker-interval` | integer | `60` | Check interval in seconds |

**Example:**

```yaml
heartbeat:
  enabled: true
  worker-interval: 10
```

> **Implementation:** `src/heartbeat_worker.cpp` | **Tests:** *None - see [TEST_TODO.md](./TEST_TODO.md)*

### 2.11 Storage Configuration (VFS)

flAPI supports loading configuration files and SQL templates from remote storage (S3, HTTPS, GCS, Azure) via DuckDB's Virtual File System (VFS) integration.

#### Remote Configuration Loading

Load the main configuration file from a remote URL:

```bash
# Load config from HTTPS
flapi --config https://example.com/configs/flapi.yaml

# Load config from S3 (requires AWS credentials)
flapi --config s3://my-bucket/configs/flapi.yaml

# Load config from GCS
flapi --config gs://my-bucket/configs/flapi.yaml
```

#### Remote Template Paths

SQL templates can be loaded from remote URLs by specifying a full URI in `template-source`:

```yaml
# Endpoint configuration with remote template
url-path: /customers
method: GET
template-source: https://example.com/templates/customers.sql
connection:
  - local-data
```

Or configure the template base path as a remote URL:

```yaml
# flapi.yaml - templates from HTTPS
template:
  path: https://example.com/templates/
```

#### Supported URI Schemes

| Scheme | Description | Credential Source |
|--------|-------------|-------------------|
| `https://` | HTTPS URLs | None required |
| `http://` | HTTP URLs (not recommended) | None required |
| `s3://` | Amazon S3 | `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION` |
| `s3a://`, `s3n://` | S3-compatible storage | Same as S3 |
| `gs://` | Google Cloud Storage | `GOOGLE_APPLICATION_CREDENTIALS` |
| `az://`, `abfs://` | Azure Blob Storage | `AZURE_STORAGE_ACCOUNT`, `AZURE_STORAGE_KEY` |
| `file://` | Local filesystem | None required |

#### Path Security

flAPI includes security features to prevent path traversal attacks:

- **Path traversal detection**: Blocks `..` sequences in paths
- **URL-encoded traversal detection**: Detects `%2e%2e` and other encoded traversal attempts
- **Scheme whitelisting**: Only configured schemes are allowed (default: `file`, `https`)

**Default Allowed Schemes:**

```yaml
# Built-in defaults (no configuration needed)
# Allowed: file://, https://
# Blocked by default: http://, s3://, gs://, az://
```

To enable additional schemes, configure them in the connection's `init` block:

```yaml
connections:
  s3-data:
    init: |
      INSTALL httpfs;
      LOAD httpfs;
      SET s3_region='us-east-1';
    properties:
      bucket: my-bucket
      prefix: data/
```

#### Environment Variables for Cloud Storage

**Amazon S3:**

```bash
export AWS_ACCESS_KEY_ID=your-access-key
export AWS_SECRET_ACCESS_KEY=your-secret-key
export AWS_REGION=us-east-1
# Optional: for assumed roles
export AWS_SESSION_TOKEN=your-session-token
```

**Google Cloud Storage:**

```bash
export GOOGLE_APPLICATION_CREDENTIALS=/path/to/service-account.json
# Or use application default credentials
gcloud auth application-default login
```

**Azure Blob Storage:**

```bash
export AZURE_STORAGE_ACCOUNT=your-storage-account
export AZURE_STORAGE_KEY=your-storage-key
# Or use connection string
export AZURE_STORAGE_CONNECTION_STRING=your-connection-string
```

#### Example: Full Remote Configuration

```yaml
# flapi.yaml (can itself be hosted remotely)
project-name: Cloud-Native API
project-description: API with remote configuration

template:
  path: s3://my-bucket/templates/

connections:
  cloud-data:
    init: |
      INSTALL httpfs;
      LOAD httpfs;
      SET s3_region='us-east-1';
    properties:
      bucket: my-data-bucket
      path: s3://my-data-bucket/data/

duckdb:
  access_mode: READ_ONLY
```

```yaml
# s3://my-bucket/templates/customers.yaml
url-path: /customers
method: GET
template-source: customers.sql  # Relative to template.path (s3://my-bucket/templates/)
connection:
  - cloud-data
```

> **Implementation:** `src/vfs_adapter.cpp`, `src/config_loader.cpp`, `src/path_validator.cpp` | **Tests:** `test/cpp/test_vfs_adapter.cpp`, `test/cpp/test_path_validator.cpp`, `test/integration/test_vfs_e2e.py`

---

## 3. Endpoint Configuration

Endpoints are defined in YAML files within the `template.path` directory. Each file can define one endpoint type: REST, MCP Tool, MCP Resource, or MCP Prompt.

### 3.1 REST Endpoints

REST endpoints expose SQL queries as HTTP APIs.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `url-path` | string | **required** | HTTP path (e.g., `/customers`, `/customers/:id`) |
| `method` | string | `GET` | HTTP method: `GET`, `POST`, `PUT`, `PATCH`, `DELETE` |
| `template-source` | string | **required** | Path to SQL template file |
| `connection` | array[string] | **required** | Connection name(s) from flapi.yaml |
| `with-pagination` | boolean | `true` | Enable pagination (adds `limit`/`offset`) |
| `request-fields-validation` | boolean | `false` | Enable strict field validation |

**Example:**

```yaml
url-path: /customers/:customer_id
method: GET

request:
  - field-name: customer_id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1

template-source: customers-get.sql
connection:
  - customers-db

with-pagination: true
```

**Path Parameters:**

URL path parameters use the `:param` syntax and are automatically extracted:

```yaml
url-path: /customers/:id/orders/:order_id
# Available as: params.id, params.order_id
```

### 3.2 MCP Tools

MCP tools expose SQL queries as callable tools for AI models.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp-tool.name` | string | **required** | Unique tool name (alphanumeric, underscores) |
| `mcp-tool.description` | string | **required** | Tool description for AI models |
| `mcp-tool.result-mime-type` | string | `application/json` | Result MIME type |

**Example:**

```yaml
mcp-tool:
  name: customer_lookup
  description: Retrieve customer information by ID or filter criteria
  result-mime-type: application/json

request:
  - field-name: id
    field-in: query
    description: Customer ID
    required: false
    validators:
      - type: int
        min: 1

template-source: customers.sql
connection:
  - customers-parquet
```

### 3.3 MCP Resources

MCP resources expose data as readable resources for AI models.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp-resource.name` | string | **required** | Unique resource name |
| `mcp-resource.description` | string | **required** | Resource description |
| `mcp-resource.mime-type` | string | `application/json` | Content MIME type |

**Example:**

```yaml
mcp-resource:
  name: customer_schema
  description: Customer database schema and field definitions
  mime-type: application/json

template-source: schema-query.sql
connection:
  - customers-db
```

### 3.4 MCP Prompts

MCP prompts provide template-based prompts for AI models.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp-prompt.name` | string | **required** | Unique prompt name |
| `mcp-prompt.description` | string | **required** | Prompt description |
| `mcp-prompt.template` | string | **required** | Mustache template content (inline) |
| `mcp-prompt.arguments` | array[string] | `[]` | Template argument names |

**Example:**

```yaml
mcp-prompt:
  name: customer_analysis
  description: Generate customer analysis prompt
  template: |
    You are a data analyst. Analyze this customer:
    {{#customer_id}}Customer ID: {{customer_id}}{{/customer_id}}
    {{#segment}}Segment: {{segment}}{{/segment}}

    Provide insights on purchasing patterns and recommendations.
  arguments:
    - customer_id
    - segment
```

**Notes:**
- MCP prompts use **inline templates**, not file-based `template-source`
- Prompts do not require `connection` (they don't execute SQL)

### 3.5 Configuration Includes

Reuse common configuration across endpoints using the include syntax.

**Syntax:**

```yaml
{{include:section_name from filename.yaml}}
```

**Example:**

```yaml
# common/customer-common.yaml
request:
  - field-name: id
    field-in: query
    validators:
      - type: int
        min: 1

auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: secret

# customers-rest.yaml
url-path: /customers
method: GET

{{include:request from common/customer-common.yaml}}
{{include:auth from common/customer-common.yaml}}

template-source: customers.sql
connection:
  - customers-parquet
```

**Includable Sections:**
- `request`
- `auth`
- `rate-limit`
- `connection`
- `template-source`
- `cache`
- `heartbeat`

> **Implementation:** `src/endpoint_config_parser.cpp`, `src/request_handler.cpp` | **Tests:** `test/cpp/endpoint_config_parser_test.cpp`, `test/cpp/extended_yaml_parser_test.cpp`

---

## 4. Request Parameters

### 4.1 Field Definition

Each request parameter is defined with the following properties:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `field-name` | string | **required** | Parameter name (used as `params.<name>` in templates) |
| `field-in` | string | **required** | Parameter location |
| `description` | string | `""` | Human-readable description |
| `required` | boolean | `false` | Whether parameter is mandatory |
| `default` | string | - | Default value if not provided |
| `validators` | array | - | Array of validator configurations |

**Example:**

```yaml
request:
  - field-name: customer_id
    field-in: path
    description: Unique customer identifier
    required: true
    validators:
      - type: int
        min: 1

  - field-name: status
    field-in: query
    description: Filter by status
    required: false
    default: "active"
    validators:
      - type: enum
        allowedValues: ["active", "inactive", "pending"]
```

### 4.2 Parameter Locations

The `field-in` property specifies where the parameter is extracted from:

| Location | Description | Example |
|----------|-------------|---------|
| `query` | URL query string | `GET /customers?id=123` |
| `path` | URL path parameter | `GET /customers/:id` → `GET /customers/123` |
| `body` | Request body (JSON) | `POST /customers` with JSON body |
| `header` | HTTP header | `X-Customer-ID: 123` |

### 4.3 Default Values

Default values are used when a parameter is not provided:

```yaml
request:
  - field-name: limit
    field-in: query
    default: "100"
    validators:
      - type: int
        min: 1
        max: 1000

  - field-name: sort_order
    field-in: query
    default: "DESC"
    validators:
      - type: enum
        allowedValues: ["ASC", "DESC"]
```

**Notes:**
- Defaults are always strings in YAML; validators handle type conversion
- Defaults are applied before validation

> **Implementation:** `src/request_handler.cpp` | **Tests:** `test/cpp/request_handler_test.cpp`

---

## 5. Validators

Validators ensure request parameters meet constraints before SQL execution.

### 5.1 Integer Validator

Validates integer values with optional range constraints.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `int` |
| `min` | integer | `INT_MIN` | Minimum allowed value |
| `max` | integer | `INT_MAX` | Maximum allowed value |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: int
    min: 1
    max: 1000000
    preventSqlInjection: true
```

### 5.2 String Validator

Validates string values with optional pattern and length constraints.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `string` |
| `regex` | string | - | Regex pattern (full match required) |
| `min` | integer | - | Minimum string length |
| `max` | integer | - | Maximum string length |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: string
    regex: "^[A-Za-z0-9_-]{3,50}$"
    min: 3
    max: 50
    preventSqlInjection: true
```

### 5.3 Enum Validator

Validates against a list of allowed values.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `enum` |
| `allowedValues` | array[string] | **required** | List of valid values |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: enum
    allowedValues: ["retail", "corporate", "vip", "enterprise"]
```

### 5.4 Email Validator

Validates email address format.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `email` |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: email
```

### 5.5 UUID Validator

Validates UUID format (v1-v5).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `uuid` |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: uuid
```

### 5.6 Date Validator

Validates date format (YYYY-MM-DD) with optional range.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `date` |
| `min` | string | - | Minimum date (YYYY-MM-DD) |
| `max` | string | - | Maximum date (YYYY-MM-DD) |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: date
    min: "2020-01-01"
    max: "2025-12-31"
```

### 5.7 Time Validator

Validates time format (HH:MM:SS) with optional range.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `time` |
| `min` | string | - | Minimum time (HH:MM:SS) |
| `max` | string | - | Maximum time (HH:MM:SS) |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: time
    min: "08:00:00"
    max: "18:00:00"
```

> **Implementation:** `src/request_validator.cpp` | **Tests:** `test/cpp/request_validator_test.cpp`, `test/integration/test_customers.tavern.yaml`

---

## 6. Cache Configuration

DuckLake caching enables snapshot-based query result caching with automatic refresh.

### 6.1 Basic Cache Settings

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cache.enabled` | boolean | `false` | Enable caching for this endpoint |
| `cache.table` | string | **required** | Cache table name |
| `cache.schema` | string | `"cache"` | DuckDB schema name |
| `cache.schedule` | string | - | Refresh schedule (e.g., `5m`, `1h`, `30s`) |
| `cache.template-file` | string | - | Custom cache refresh SQL template |

**Schedule Format:**
- `30s` = 30 seconds
- `5m` = 5 minutes
- `1h` = 1 hour
- `2d` = 2 days

**Example:**

```yaml
cache:
  enabled: true
  table: customers_cache
  schema: analytics
  schedule: 5m
```

### 6.2 Refresh Modes

**Full Refresh (Default):**

Recreates the entire cache table on each refresh.

```yaml
cache:
  enabled: true
  table: customers_cache
  schedule: 1h
```

**Incremental Append:**

Appends only new rows using a cursor column.

| Parameter | Type | Description |
|-----------|------|-------------|
| `cache.cursor.column` | string | Column tracking new rows |
| `cache.cursor.type` | string | Column type: `date`, `timestamp`, `integer` |

```yaml
cache:
  enabled: true
  table: orders_cache
  schedule: 5m
  cursor:
    column: created_at
    type: timestamp
```

**Incremental Merge:**

Handles inserts, updates, and deletes using cursor + primary key.

| Parameter | Type | Description |
|-----------|------|-------------|
| `cache.primary-key` | array[string] | Column(s) for identifying rows |
| `cache.cursor.column` | string | Column tracking all changes |
| `cache.cursor.type` | string | Column type |

```yaml
cache:
  enabled: true
  table: customers_cache
  schedule: 1m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
```

### 6.3 Retention Policies

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cache.retention.keep-last-snapshots` | integer | - | Number of snapshots to keep |
| `cache.retention.max-snapshot-age` | string | - | Maximum snapshot age (e.g., `7d`, `2h`) |
| `cache.rollback-window` | string | - | Time window for rollback capability |
| `cache.delete-handling` | string | - | Delete mode: `soft` or `hard` |

**Example:**

```yaml
cache:
  enabled: true
  table: customers_cache
  schedule: 5m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  retention:
    keep-last-snapshots: 5
    max-snapshot-age: 14d
  rollback-window: 2d
  delete-handling: soft
```

### 6.4 Cache Template Variables

Special variables available in cache-enabled SQL templates:

| Variable | Description |
|----------|-------------|
| `{{cache.table}}` | Cache table name |
| `{{cache.schema}}` | Cache schema name |
| `{{cache.catalog}}` | DuckLake catalog alias |
| `{{cache.previousSnapshotTimestamp}}` | Last refresh timestamp |
| `{{cache.currentSnapshotTimestamp}}` | Current refresh timestamp |

**Example Template:**

```sql
-- Incremental refresh template
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT * FROM source_table
{{#cache.previousSnapshotTimestamp}}
WHERE updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}'
{{/cache.previousSnapshotTimestamp}}
```

> **Implementation:** `src/cache_manager.cpp` | **Tests:** `test/integration/test_ducklake_comprehensive.tavern.yaml`, `test/integration/test_ducklake_advanced.py`

---

## 7. Authentication

Authentication can be configured globally (in flapi.yaml) or per-endpoint (in endpoint YAML).

### 7.1 Basic Authentication

Username/password authentication with HTTP Basic auth.

| Parameter | Type | Description |
|-----------|------|-------------|
| `auth.enabled` | boolean | Enable authentication |
| `auth.type` | string | Must be `basic` |
| `auth.users` | array | List of authorized users |
| `auth.users[].username` | string | Username (supports env vars) |
| `auth.users[].password` | string | Password (supports env vars) |
| `auth.users[].roles` | array[string] | Assigned roles |

**Example:**

```yaml
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: '${ADMIN_PASSWORD}'
      roles: [admin, read, write]
    - username: reader
      password: '${READER_PASSWORD}'
      roles: [read]
```

### 7.2 JWT Authentication

JSON Web Token authentication.

| Parameter | Type | Description |
|-----------|------|-------------|
| `auth.enabled` | boolean | Enable authentication |
| `auth.type` | string | Must be `jwt` |
| `auth.jwt-secret` | string | Secret key for JWT validation |
| `auth.jwt-issuer` | string | Expected issuer claim |

**Example:**

```yaml
auth:
  enabled: true
  type: jwt
  jwt-secret: '${JWT_SECRET}'
  jwt-issuer: my-auth-server
```

### 7.3 Bearer Authentication

Bearer token authentication (similar to JWT).

| Parameter | Type | Description |
|-----------|------|-------------|
| `auth.enabled` | boolean | Enable authentication |
| `auth.type` | string | Must be `bearer` |
| `auth.jwt-secret` | string | Secret key for token validation |
| `auth.jwt-issuer` | string | Expected issuer claim |

**Example:**

```yaml
auth:
  enabled: true
  type: bearer
  jwt-secret: '${API_TOKEN_SECRET}'
  jwt-issuer: api-gateway
```

### 7.4 OIDC Authentication

OpenID Connect authentication with external identity providers.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `auth.oidc.issuer-url` | string | **required** | OIDC provider URL |
| `auth.oidc.client-id` | string | **required** | OAuth client ID |
| `auth.oidc.client-secret` | string | - | OAuth client secret |
| `auth.oidc.provider-type` | string | `generic` | Provider: `google`, `microsoft`, `keycloak`, `generic` |
| `auth.oidc.allowed-audiences` | array[string] | - | Acceptable `aud` claims |
| `auth.oidc.verify-expiration` | boolean | `true` | Verify token expiration |
| `auth.oidc.clock-skew-seconds` | integer | `300` | Allowed clock skew |
| `auth.oidc.username-claim` | string | `sub` | Claim for username |
| `auth.oidc.email-claim` | string | `email` | Claim for email |
| `auth.oidc.roles-claim` | string | `roles` | Claim for roles |
| `auth.oidc.groups-claim` | string | `groups` | Claim for groups |
| `auth.oidc.role-claim-path` | string | - | Path for nested claims |
| `auth.oidc.enable-client-credentials` | boolean | `false` | Enable client credentials flow |
| `auth.oidc.enable-refresh-tokens` | boolean | `false` | Enable refresh tokens |
| `auth.oidc.scopes` | array[string] | - | OAuth scopes |
| `auth.oidc.jwks-cache-hours` | integer | `24` | JWKS cache duration |

**Example:**

```yaml
auth:
  enabled: true
  type: oidc
  oidc:
    issuer-url: https://login.microsoftonline.com/tenant-id/v2.0
    client-id: '${AZURE_CLIENT_ID}'
    client-secret: '${AZURE_CLIENT_SECRET}'
    provider-type: microsoft
    allowed-audiences:
      - api://my-api
    roles-claim: roles
    role-claim-path: realm_access.roles
```

### 7.5 AWS Secrets Manager

Load credentials from AWS Secrets Manager.

| Parameter | Type | Description |
|-----------|------|-------------|
| `auth.from-aws-secretmanager.secret-name` | string | AWS secret name |
| `auth.from-aws-secretmanager.region` | string | AWS region |
| `auth.from-aws-secretmanager.secret-id` | string | AWS access key ID |
| `auth.from-aws-secretmanager.secret-key` | string | AWS secret access key |
| `auth.from-aws-secretmanager.secret-table` | string | Table name for cached credentials |
| `auth.from-aws-secretmanager.init` | string | Custom initialization SQL |

**Example:**

```yaml
auth:
  enabled: true
  type: basic
  from-aws-secretmanager:
    secret-name: prod/api/credentials
    region: us-east-1
    secret-id: '${AWS_ACCESS_KEY}'
    secret-key: '${AWS_SECRET_KEY}'
```

> **Implementation:** `src/auth_middleware.cpp`, `src/oidc_auth_handler.cpp` | **Tests:** `test/cpp/auth_middleware_test.cpp`, `test/integration/test_oidc_authentication.py`

---

## 8. Operation Configuration

Specifies how database operations are executed.

### 8.1 Read Operations

Read operations (queries) are the default for `GET`, `HEAD`, and `OPTIONS` methods.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `operation.type` | string | `Read` | Operation type (auto-detected) |

### 8.2 Write Operations

Write operations are auto-detected for `POST`, `PUT`, `PATCH`, and `DELETE` methods.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `operation.type` | string | `Write` | Operation type |
| `operation.returns-data` | boolean | `false` | Whether operation returns results (RETURNING clause) |
| `operation.transaction` | boolean | `true` | Wrap in transaction |
| `operation.validate-before-write` | boolean | `true` | Apply strict validation |

**Example:**

```yaml
url-path: /customers/:id
method: PATCH

operation:
  type: Write
  returns-data: true
  transaction: true
  validate-before-write: true

# Additional cache settings for write endpoints
cache:
  invalidate-on-write: true   # Invalidate cache after write
  refresh-on-write: false     # Immediately refresh cache after write
```

> **Implementation:** `src/request_handler.cpp`, `src/query_executor.cpp` | **Tests:** `test/integration/test_write_operations.tavern.yaml`

---

## 9. SQL Templates (Mustache)

SQL templates use Mustache syntax for dynamic query generation.

### 9.1 Variable Syntax

**Double Braces `{{ }}`** - For numeric values and identifiers (no escaping):

```sql
SELECT * FROM table
WHERE id = {{ params.id }}
LIMIT {{ params.limit }}
```

**Triple Braces `{{{ }}}`** - For string values (escapes quotes, prevents SQL injection):

```sql
SELECT * FROM table
WHERE name = '{{{ params.name }}}'
AND status = '{{{ params.status }}}'
```

### 9.2 Available Contexts

| Context | Description | Example |
|---------|-------------|---------|
| `params.*` | Request parameters | `{{ params.customer_id }}` |
| `conn.*` | Connection properties | `{{{ conn.path }}}` |
| `cache.*` | Cache metadata | `{{ cache.table }}` |
| `env.*` | Whitelisted environment variables | `{{{ env.API_KEY }}}` |
| `context.auth.*` | Authentication context | `{{ context.auth.username }}` |

### 9.3 Conditional Sections

**Conditional Rendering (if exists and truthy):**

```sql
SELECT * FROM customers
WHERE 1=1
{{#params.id}}
  AND id = {{ params.id }}
{{/params.id}}
{{#params.name}}
  AND name LIKE '%{{{ params.name }}}%'
{{/params.name}}
```

**Inverted Sections (if not exists or falsy):**

```sql
LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}100{{/params.limit}}
```

### 9.4 Best Practices

**1. Use `WHERE 1=1` Pattern:**

```sql
SELECT * FROM table
WHERE 1=1
{{#params.filter1}}
  AND column1 = {{{ params.filter1 }}}
{{/params.filter1}}
```

**2. Always Use Triple Braces for Strings:**

```sql
-- CORRECT: Prevents SQL injection
WHERE name = '{{{ params.name }}}'

-- INCORRECT: Vulnerable to injection
WHERE name = '{{ params.name }}'
```

**3. Provide Default Values:**

```sql
ORDER BY created_at {{#params.order}}{{ params.order }}{{/params.order}}{{^params.order}}DESC{{/params.order}}
LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}100{{/params.limit}}
```

**4. Use Connection Properties for File Paths:**

```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
```

> **Implementation:** `src/sql_template_processor.cpp` | **Tests:** `test/cpp/sql_template_processor_test.cpp`

---

## 10. Environment Variables

### 10.1 Substitution Syntax

Environment variables can be substituted in configuration files:

**Standard Syntax:**

```yaml
password: '${DB_PASSWORD}'
```

**Template Syntax (in endpoint YAML):**

```yaml
username: '{{env.API_USER}}'
```

### 10.2 Whitelist Configuration

Environment variables must be whitelisted to be substituted:

```yaml
# flapi.yaml
template:
  environment-whitelist:
    - '^FLAPI_.*'       # All FLAPI_ prefixed variables
    - '^DB_.*'          # All DB_ prefixed variables
    - '^API_KEY$'       # Exact match for API_KEY
    - '^JWT_SECRET$'    # Exact match for JWT_SECRET
```

**Whitelist Pattern Examples:**

| Pattern | Matches |
|---------|---------|
| `^FLAPI_.*` | `FLAPI_PORT`, `FLAPI_DEBUG`, etc. |
| `^DB_` | `DB_HOST`, `DB_PASSWORD`, etc. |
| `^API_KEY$` | Only `API_KEY` |
| `.*_SECRET$` | `JWT_SECRET`, `API_SECRET`, etc. |

> **Implementation:** `src/config_manager.cpp` | **Tests:** `test/cpp/config_manager_test.cpp`

---

## Appendix A: Complete Example Configuration

### Main Configuration (flapi.yaml)

```yaml
# Project metadata
project-name: Customer API
project-description: REST and MCP API for customer data
server-name: api.example.com
http-port: 8080

# Template configuration
template:
  path: ./sqls
  environment-whitelist:
    - '^FLAPI_.*'
    - '^DB_.*'
    - '^JWT_SECRET$'
    - '^API_.*'

# Database connections
connections:
  customers-db:
    init: |
      INSTALL postgres;
      LOAD postgres;
    log-queries: false
    properties:
      host: '${DB_HOST}'
      port: '5432'
      database: customers
      user: '${DB_USER}'
      password: '${DB_PASSWORD}'

  local-parquet:
    properties:
      path: ./data/customers.parquet

# DuckDB settings
duckdb:
  db_path: ./cache.db
  access_mode: READ_WRITE
  threads: 4
  max_memory: 4GB

# DuckLake caching
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache/metadata
  data-path: ./cache/data
  retention:
    keep-last-snapshots: 10
    max-snapshot-age: 30d
  scheduler:
    enabled: true
    scan-interval: 5m

# MCP server
mcp:
  enabled: true
  port: 8081
  instructions-file: ./mcp_instructions.md

# Global authentication
auth:
  enabled: false

# Global rate limiting
rate_limit:
  enabled: true
  max: 100
  interval: 60

# Heartbeat
heartbeat:
  enabled: true
  worker-interval: 30
```

### Endpoint Configuration (sqls/customers.yaml)

```yaml
# REST endpoint
url-path: /customers/:id
method: GET

# Also expose as MCP tool
mcp-tool:
  name: get_customer
  description: Retrieve customer by ID
  result-mime-type: application/json

# Request parameters
request:
  - field-name: id
    field-in: path
    description: Customer ID
    required: true
    validators:
      - type: int
        min: 1
        preventSqlInjection: true

  - field-name: include_orders
    field-in: query
    description: Include recent orders
    required: false
    default: "false"
    validators:
      - type: enum
        allowedValues: ["true", "false"]

# SQL template
template-source: customers-get.sql

# Connection
connection:
  - customers-db

# Pagination
with-pagination: false

# Caching
cache:
  enabled: true
  table: customers_cache
  schedule: 5m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  retention:
    keep-last-snapshots: 5

# Authentication
auth:
  enabled: true
  type: jwt
  jwt-secret: '${JWT_SECRET}'

# Rate limiting
rate-limit:
  enabled: true
  max: 50
  interval: 60
```

### SQL Template (sqls/customers-get.sql)

```sql
SELECT
    c.id,
    c.name,
    c.email,
    c.segment,
    c.created_at,
    c.updated_at
    {{#params.include_orders}}
    , (
        SELECT json_agg(o.*)
        FROM orders o
        WHERE o.customer_id = c.id
        ORDER BY o.created_at DESC
        LIMIT 5
    ) as recent_orders
    {{/params.include_orders}}
FROM customers c
WHERE c.id = {{ params.id }}
```

---

## Appendix B: Naming Conventions

flAPI supports both hyphenated and camelCase naming for backward compatibility:

| Preferred (Hyphenated) | Alternative (CamelCase) |
|------------------------|-------------------------|
| `field-name` | - |
| `field-in` | - |
| `url-path` | - |
| `template-source` | - |
| `with-pagination` | - |
| `primary-key` | `primaryKey` |
| `jwt-secret` | - |
| `jwt-issuer` | - |
| `rate-limit` | - |
| `prevent-sql-injection` | `preventSqlInjection` |
| `allowed-values` | `allowedValues` |
| `returns-data` | `returnsData` |
| `rollback-window` | `rollbackWindow` |
| `delete-handling` | `deleteHandling` |

**Recommendation:** Use hyphenated naming for consistency.

---

## Appendix C: Default Values Reference

| Configuration | Default Value |
|---------------|---------------|
| `http-port` | `8080` |
| `server-name` | `"localhost"` |
| `duckdb.db_path` | `:memory:` |
| `duckdb.access_mode` | `READ_WRITE` |
| `ducklake.alias` | `"cache"` |
| `ducklake.enabled` | `false` |
| `mcp.enabled` | `true` |
| `mcp.port` | `8081` |
| `mcp.auth.enabled` | `false` |
| `auth.enabled` | `false` |
| `rate_limit.enabled` | `false` |
| `rate_limit.max` | `100` |
| `rate_limit.interval` | `60` |
| `heartbeat.enabled` | `false` |
| `heartbeat.worker-interval` | `60` |
| `enforce-https.enabled` | `false` |
| `method` | `GET` |
| `with-pagination` | `true` |
| `request-fields-validation` | `false` |
| `required` | `false` |
| `cache.enabled` | `false` |
| `cache.schema` | `"cache"` |
| `operation.type` | Auto-detected from method |
| `operation.returns-data` | `false` |
| `operation.transaction` | `true` |
| `operation.validate-before-write` | `true` |
| `preventSqlInjection` | `true` |
| `mcp-tool.result-mime-type` | `application/json` |
| `mcp-resource.mime-type` | `application/json` |
| `auth.oidc.verify-expiration` | `true` |
| `auth.oidc.clock-skew-seconds` | `300` |
| `auth.oidc.username-claim` | `sub` |
| `auth.oidc.email-claim` | `email` |
| `auth.oidc.roles-claim` | `roles` |
| `auth.oidc.groups-claim` | `groups` |
| `auth.oidc.jwks-cache-hours` | `24` |

---

## Related Documentation

- [CLI Reference](./CLI_REFERENCE.md) - Command-line interface documentation
- [Config Service API Reference](./CONFIG_SERVICE_API_REFERENCE.md) - Runtime configuration REST API
- [MCP Reference](./MCP_REFERENCE.md) - Model Context Protocol implementation details
