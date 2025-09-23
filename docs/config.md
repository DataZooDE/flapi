# FLAPI Configuration Guide

FLAPI uses YAML configuration files to define your data APIs, database connections, and server settings. This guide provides comprehensive documentation for configuring FLAPI from a practitioner's perspective.

## Table of Contents

1. [Quick Start](#quick-start)
2. [Configuration Architecture](#configuration-architecture)
3. [Main Configuration File](#main-configuration-file)
4. [Endpoint Configuration](#endpoint-configuration)
5. [Advanced YAML Features](#advanced-yaml-features)
6. [Configuration Reference](#configuration-reference)
7. [Scenario-Based Examples](#scenario-based-examples)
8. [Best Practices](#best-practices)
9. [Troubleshooting](#troubleshooting)

---

## Quick Start

The simplest FLAPI configuration consists of two parts:

### 1. Main Configuration (`flapi.yaml`)
```yaml
# Basic project setup
project_name: my-api-project
project_description: My first FLAPI project

# SQL templates directory
template:
  path: './sqls'

# Database connection
connections:
  my-data:
    properties:
      path: './data/customers.parquet'

# DuckDB settings
duckdb:
  db_path: ./flapi_cache.db
  threads: 4
  max_memory: 2GB
```

### 2. Endpoint Configuration (`sqls/customers.yaml`)
```yaml
# REST endpoint
url-path: /customers

# Request parameters
request:
  - field-name: id
    field-in: query
    description: Customer ID
    required: false
    validators:
      - type: int
        min: 1

# SQL template and connection
template-source: customers.sql
connection: [my-data]
```

### 3. SQL Template (`sqls/customers.sql`)
```sql
SELECT * FROM read_parquet('{{ context.conn.path }}')
WHERE 1=1
{{#if params.id}}
  AND customer_id = {{ params.id }}
{{/if}}
```

---

## Configuration Architecture

FLAPI uses a **hierarchical configuration system**:

```
flapi.yaml                 # Main configuration
├── sqls/                  # Endpoint configurations
│   ├── customers.yaml     # REST endpoint + MCP tool
│   ├── orders.yaml        # Another endpoint
│   └── common/            # Shared configuration pieces
│       ├── auth.yaml      # Authentication config
│       ├── rate-limit.yaml# Rate limiting config
│       └── connections.yaml# Connection definitions
└── data/                  # Data files
    └── customers.parquet
```

### Configuration Types

FLAPI automatically detects configuration types based on the presence of specific keys:

- **REST Endpoint**: Contains `url-path`
- **MCP Tool**: Contains `mcp-tool` section
- **MCP Resource**: Contains `mcp-resource` section
- **MCP Prompt**: Contains `mcp-prompt` section
- **Unified**: Can combine REST + MCP in a single file

---

## Main Configuration File

The main `flapi.yaml` file contains global settings and server configuration.

### Complete Example

```yaml
# Project Information
project_name: enterprise-api
project_description: Enterprise data API with multiple sources
server_name: production-server

# Template Configuration
template:
  path: './sqls'
  environment-whitelist:
    - '^FLAPI_.*'
    - '^DB_.*'

# Database Connections
connections:
  # BigQuery connection
  bigquery-warehouse:
    init: |
      FORCE INSTALL 'bigquery' FROM 'http://storage.googleapis.com/hafenkran';
      LOAD 'bigquery';
    properties:
      project_id: 'my-gcp-project'
      dataset: 'analytics'
    log-queries: true
    log-parameters: false
    allow: '*'

  # Parquet files
  customer-data:
    properties:
      path: './data/customers.parquet'
    log-queries: false

  # PostgreSQL connection
  postgres-orders:
    init: |
      INSTALL postgres_scanner;
      LOAD postgres_scanner;
    properties:
      host: 'localhost'
      port: '5432'
      database: 'orders'
      user: 'flapi_user'
      password: '${DB_PASSWORD}'

# DuckDB Configuration
duckdb:
  db_path: ./enterprise_cache.db
  access_mode: READ_WRITE
  threads: 8
  max_memory: 8GB
  default_order: DESC

# Global Rate Limiting
rate-limit:
  enabled: true
  max: 1000
  interval: 3600  # 1 hour

# HTTPS Configuration
enforce-https:
  enabled: true
  ssl-cert-file: './ssl/cert.pem'
  ssl-key-file: './ssl/key.pem'

# Global Heartbeat
heartbeat:
  enabled: true
  worker-interval: 30
```

### Key Sections Explained

#### Project Information
- `project_name`: Identifies your API project
- `project_description`: Human-readable description
- `server_name`: Server identifier (optional)

#### Template Configuration
- `path`: Directory containing SQL templates and endpoint configurations
- `environment-whitelist`: Regex patterns for allowed environment variables

#### Connections
Define database connections that endpoints can use:
- `init`: SQL commands to run when connection is established
- `properties`: Connection-specific properties accessible in templates
- `log-queries`: Enable query logging for this connection
- `log-parameters`: Enable parameter logging
- `allow`: Access control pattern (default: '*')

---

## Endpoint Configuration

Endpoints are configured in YAML files within your `template.path` directory. FLAPI supports four types of endpoints:

### 1. REST Endpoint

```yaml
# Basic REST endpoint
url-path: /customers
method: GET  # Optional, defaults to GET

request:
  - field-name: customer_id
    field-in: query
    description: Unique customer identifier
    required: true
    validators:
      - type: int
        min: 1
        max: 999999

template-source: customers.sql
connection: [customer-data]

# Optional features
with-pagination: true
rate-limit:
  enabled: true
  max: 100
  interval: 60

auth:
  enabled: true
  type: basic
  users:
    - username: api_user
      password: secure_password
      roles: [read]

cache:
  enabled: true
  cache-table-name: customers_cache
  cache-source: customers_cache.sql
  refresh-time: 1h
```

### 2. MCP Tool

```yaml
# MCP Tool for AI interactions
mcp-tool:
  name: get_customer_data
  description: Retrieve customer information by ID or segment
  result_mime_type: application/json

request:
  - field-name: customer_id
    field-in: query
    description: Customer ID to lookup
    required: false
    validators:
      - type: int
        min: 1

  - field-name: segment
    field-in: query
    description: Customer segment filter
    required: false
    validators:
      - type: enum
        allowedValues: [premium, standard, basic]

template-source: customers.sql
connection: [customer-data]

# Shared configuration with REST endpoints
rate-limit:
  enabled: true
  max: 50
  interval: 60

auth:
  enabled: true
  type: basic
  users:
    - username: ai_agent
      password: agent_password
      roles: [ai_tools]
```

### 3. MCP Resource

```yaml
# MCP Resource - static or dynamic data
mcp-resource:
  name: customer_schema
  description: Customer database schema and metadata
  mime_type: application/json

# Can use SQL template for dynamic resources
template-source: customer_schema.sql
connection: [customer-data]

# Or provide static content (not shown here)
```

### 4. MCP Prompt

```yaml
# MCP Prompt - AI prompt templates
mcp-prompt:
  name: customer_analysis
  description: Generate customer analysis report
  template: |
    Analyze the customer data for segment: {{segment}}
    
    Focus on:
    1. Customer behavior patterns
    2. Revenue trends for {{time_period}}
    3. Recommendations for {{analysis_type}}
    
    Use the available customer data to provide insights.
  
  arguments:
    - segment
    - time_period
    - analysis_type
```

### 5. Unified Configuration

You can combine REST and MCP in a single file:

```yaml
# Serves as BOTH REST endpoint AND MCP tool
url-path: /customers          # Makes it a REST endpoint
mcp-tool:                     # Also makes it an MCP tool
  name: customer_lookup
  description: Customer data lookup
  result_mime_type: application/json

request:
  - field-name: id
    field-in: query
    description: Customer ID
    required: false
    validators:
      - type: int
        min: 1

template-source: customers.sql
connection: [customer-data]

# Shared configuration applies to both REST and MCP
rate-limit:
  enabled: true
  max: 100
  interval: 60

auth:
  enabled: true
  type: basic
  users:
    - username: api_user
      password: password
      roles: [read, ai_tools]
```

---

## Advanced YAML Features

FLAPI's Extended YAML Parser provides powerful features for modular configuration.

### YAML Includes

Modularize your configuration by including other YAML files:

#### File Include
```yaml
# Include entire file
{{include from common/auth.yaml}}
{{include from common/rate-limit.yaml}}

url-path: /customers
template-source: customers.sql
```

#### Section Include
```yaml
# Include specific sections
url-path: /customers
{{include:request from common/customer-request.yaml}}
{{include:auth from common/basic-auth.yaml}}
template-source: customers.sql
```

#### Relative Path Resolution
```yaml
# Includes resolve relative to the current file
{{include from ../shared/database-config.yaml}}
{{include from ./local-overrides.yaml}}
```

### Environment Variable Substitution

Use environment variables in your configuration:

```yaml
connections:
  production-db:
    properties:
      host: '{{env.DB_HOST}}'
      password: '{{env.DB_PASSWORD}}'
      api_key: '{{env.BIGQUERY_API_KEY}}'

# Environment variable in include paths
{{include from common/{{env.ENVIRONMENT}}-config.yaml}}
```

### Conditional Includes

Include files based on conditions:

```yaml
# Include based on environment variable
{{include from common/auth.yaml if env.ENABLE_AUTH}}
{{include from common/debug-config.yaml if env.DEBUG_MODE}}

# Include based on boolean conditions
{{include from common/ssl-config.yaml if true}}
{{include from common/dev-tools.yaml if false}}
```

### Complex Example

```yaml
# Main endpoint configuration
url-path: /analytics/{{env.API_VERSION}}/customers

# Conditional authentication
{{include:auth from common/{{env.AUTH_TYPE}}-auth.yaml if env.ENABLE_AUTH}}

# Environment-specific request validation
{{include:request from common/{{env.ENVIRONMENT}}-validation.yaml}}

# Shared components
{{include:rate-limit from common/rate-limit.yaml}}
{{include:cache from common/{{env.CACHE_STRATEGY}}-cache.yaml}}

template-source: analytics/customers-{{env.ENVIRONMENT}}.sql
connection: 
  - {{env.PRIMARY_CONNECTION}}
  - {{env.BACKUP_CONNECTION}}
```

---

## Configuration Reference

### Request Field Configuration

Request fields define API parameters and validation:

```yaml
request:
  - field-name: customer_id      # Parameter name
    field-in: query             # Location: query, path, header, body
    description: Customer ID    # Human-readable description
    required: true              # Whether parameter is required
    default-value: "1"          # Default value if not provided
    validators:                 # Validation rules
      - type: int               # Validator type
        min: 1                  # Type-specific options
        max: 999999
        preventSqlInjection: true
```

#### Validator Types

**String Validator**
```yaml
validators:
  - type: string
    regex: "^[A-Za-z0-9_-]+$"
    min: 3                    # Minimum length
    max: 50                   # Maximum length
    preventSqlInjection: true
```

**Integer Validator**
```yaml
validators:
  - type: int
    min: 1
    max: 1000000
    preventSqlInjection: true
```

**Float Validator**
```yaml
validators:
  - type: float
    min: 0.0
    max: 999.99
```

**Email Validator**
```yaml
validators:
  - type: email
```

**UUID Validator**
```yaml
validators:
  - type: uuid
```

**Enum Validator**
```yaml
validators:
  - type: enum
    allowedValues: [active, inactive, pending]
```

**Date Validator**
```yaml
validators:
  - type: date
    min: "2020-01-01"
    max: "2025-12-31"
```

**Time Validator**
```yaml
validators:
  - type: time
    min: "09:00:00"
    max: "17:00:00"
```

### Authentication Configuration

#### Basic Authentication
```yaml
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: admin_password
      roles: [admin, read, write]
    - username: readonly
      password: readonly_password
      roles: [read]
```

#### JWT Authentication
```yaml
auth:
  enabled: true
  type: jwt
  jwt_secret: your-secret-key
  jwt_issuer: your-issuer
```

#### AWS Secrets Manager Authentication
```yaml
auth:
  enabled: true
  type: basic
  from_aws_secretmanager:
    secret_name: flapi-users
    region: us-east-1
    secret_table: user_auth
    init: |
      CREATE TABLE user_auth AS 
      SELECT * FROM read_json_auto('{{ secret_json }}')
```

### Rate Limiting Configuration

```yaml
rate-limit:
  enabled: true
  max: 1000              # Maximum requests
  interval: 3600         # Time window in seconds (1 hour)
```

### Caching Configuration

```yaml
cache:
  enabled: true
  cache-table-name: my_cache_table
  cache-source: cache_query.sql      # SQL file for cache population
  refresh-time: 1h                   # Refresh interval (1h, 30m, 300s)
  refresh-endpoint: true             # Enable cache refresh endpoint
  max-previous-tables: 5             # Keep N previous cache versions
```

### Heartbeat Configuration

```yaml
heartbeat:
  enabled: true
  worker-interval: 30    # Check interval in seconds
  params:                # Parameters passed to heartbeat queries
    service_id: "api-1"
    region: "us-east-1"
```

---

## Scenario-Based Examples

### Scenario 1: Multi-Source Analytics API

**Goal**: Create an API that combines data from BigQuery, PostgreSQL, and local Parquet files.

```yaml
# flapi.yaml
project_name: analytics-hub
project_description: Multi-source analytics API

template:
  path: './endpoints'

connections:
  bigquery-warehouse:
    init: |
      FORCE INSTALL 'bigquery' FROM 'http://storage.googleapis.com/hafenkran';
      LOAD 'bigquery';
    properties:
      project_id: 'analytics-project'
      dataset: 'warehouse'

  postgres-transactions:
    init: |
      INSTALL postgres_scanner;
      LOAD postgres_scanner;
    properties:
      host: '{{env.POSTGRES_HOST}}'
      database: 'transactions'
      user: '{{env.POSTGRES_USER}}'
      password: '{{env.POSTGRES_PASSWORD}}'

  local-customers:
    properties:
      path: './data/customers.parquet'

duckdb:
  db_path: ./analytics_cache.db
  threads: 8
  max_memory: 16GB
```

```yaml
# endpoints/customer-analytics.yaml
url-path: /analytics/customers
mcp-tool:
  name: customer_analytics
  description: Customer analytics combining multiple data sources
  result_mime_type: application/json

request:
  - field-name: customer_segment
    field-in: query
    description: Customer segment to analyze
    required: false
    validators:
      - type: enum
        allowedValues: [premium, standard, basic]

  - field-name: date_range
    field-in: query
    description: Analysis date range in days
    required: false
    default-value: "30"
    validators:
      - type: int
        min: 1
        max: 365

template-source: customer-analytics.sql
connection: 
  - bigquery-warehouse
  - postgres-transactions
  - local-customers

cache:
  enabled: true
  cache-table-name: customer_analytics_cache
  cache-source: customer-analytics-cache.sql
  refresh-time: 2h

rate-limit:
  enabled: true
  max: 50
  interval: 3600

auth:
  enabled: true
  type: basic
  users:
    - username: analyst
      password: '{{env.ANALYST_PASSWORD}}'
      roles: [analytics]
```

```sql
-- endpoints/customer-analytics.sql
WITH customer_base AS (
  SELECT * FROM read_parquet('{{ context.conn.local-customers.path }}')
  {{#if params.customer_segment}}
  WHERE segment = '{{ params.customer_segment }}'
  {{/if}}
),
transaction_data AS (
  SELECT customer_id, SUM(amount) as total_spent, COUNT(*) as transaction_count
  FROM postgres_scan('{{ context.conn.postgres-transactions.host }}', 
                      '{{ context.conn.postgres-transactions.database }}', 
                      'public', 'transactions')
  WHERE transaction_date >= CURRENT_DATE - INTERVAL '{{ params.date_range }}' DAY
  GROUP BY customer_id
),
warehouse_metrics AS (
  SELECT customer_id, engagement_score, lifetime_value
  FROM bigquery_scan('{{ context.conn.bigquery-warehouse.project_id }}.{{ context.conn.bigquery-warehouse.dataset }}.customer_metrics')
)
SELECT 
  c.customer_id,
  c.segment,
  c.registration_date,
  t.total_spent,
  t.transaction_count,
  w.engagement_score,
  w.lifetime_value
FROM customer_base c
LEFT JOIN transaction_data t ON c.customer_id = t.customer_id
LEFT JOIN warehouse_metrics w ON c.customer_id = w.customer_id
ORDER BY w.lifetime_value DESC
```

### Scenario 2: Modular Configuration with Includes

**Goal**: Create reusable configuration components for consistent setup across multiple endpoints.

```yaml
# common/auth-config.yaml
auth:
  enabled: true
  type: basic
  users:
    - username: api_user
      password: '{{env.API_PASSWORD}}'
      roles: [read]
    - username: admin
      password: '{{env.ADMIN_PASSWORD}}'
      roles: [admin, read, write]
```

```yaml
# common/rate-limit-config.yaml
rate-limit:
  enabled: true
  max: 100
  interval: 60
```

```yaml
# common/customer-request.yaml
request:
  - field-name: customer_id
    field-in: query
    description: Customer ID
    required: false
    validators:
      - type: int
        min: 1
        max: 999999
  - field-name: segment
    field-in: query
    description: Customer segment
    required: false
    validators:
      - type: enum
        allowedValues: [premium, standard, basic]
```

```yaml
# endpoints/customers-rest.yaml
url-path: /customers

# Include shared components
{{include:request from ../common/customer-request.yaml}}
{{include:auth from ../common/auth-config.yaml}}
{{include:rate-limit from ../common/rate-limit-config.yaml}}

template-source: customers.sql
connection: [customer-data]
```

```yaml
# endpoints/customers-mcp.yaml
mcp-tool:
  name: customer_lookup
  description: Customer data lookup tool
  result_mime_type: application/json

# Reuse the same components
{{include:request from ../common/customer-request.yaml}}
{{include:auth from ../common/auth-config.yaml}}
{{include:rate-limit from ../common/rate-limit-config.yaml}}

template-source: customers.sql
connection: [customer-data]
```

### Scenario 3: Environment-Specific Configuration

**Goal**: Support different configurations for development, staging, and production.

```yaml
# flapi.yaml
project_name: multi-env-api
project_description: Environment-aware API configuration

template:
  path: './endpoints'
  environment-whitelist:
    - '^FLAPI_.*'
    - '^ENV_.*'

# Environment-specific includes
{{include from config/{{env.ENVIRONMENT}}-connections.yaml}}
{{include from config/{{env.ENVIRONMENT}}-settings.yaml}}

duckdb:
  db_path: ./{{env.ENVIRONMENT}}_cache.db
  threads: '{{env.FLAPI_THREADS}}'
  max_memory: '{{env.FLAPI_MAX_MEMORY}}'
```

```yaml
# config/development-connections.yaml
connections:
  dev-data:
    properties:
      path: './dev-data/customers.parquet'
    log-queries: true
    log-parameters: true
```

```yaml
# config/production-connections.yaml
connections:
  prod-bigquery:
    init: |
      FORCE INSTALL 'bigquery' FROM 'http://storage.googleapis.com/hafenkran';
      LOAD 'bigquery';
    properties:
      project_id: 'production-project'
      dataset: 'analytics'
    log-queries: false
    log-parameters: false
```

```yaml
# endpoints/customers.yaml
url-path: /customers

# Environment-specific authentication
{{include:auth from ../config/{{env.ENVIRONMENT}}-auth.yaml if env.ENABLE_AUTH}}

# Environment-specific validation
{{include:request from ../config/{{env.ENVIRONMENT}}-validation.yaml}}

template-source: customers-{{env.ENVIRONMENT}}.sql
connection: 
  - '{{env.PRIMARY_CONNECTION}}'
```

---

## Best Practices

### 1. Configuration Organization

**Use a clear directory structure:**
```
flapi.yaml
├── endpoints/
│   ├── customers/
│   │   ├── customers.yaml
│   │   ├── customer-analytics.yaml
│   │   └── customer-segments.yaml
│   └── orders/
│       ├── orders.yaml
│       └── order-history.yaml
├── common/
│   ├── auth.yaml
│   ├── rate-limits.yaml
│   └── connections.yaml
└── config/
    ├── development.yaml
    ├── staging.yaml
    └── production.yaml
```

### 2. Security Best Practices

**Never hardcode secrets:**
```yaml
# ❌ Bad
connections:
  db:
    properties:
      password: 'hardcoded_password'

# ✅ Good
connections:
  db:
    properties:
      password: '{{env.DB_PASSWORD}}'
```

**Use environment variable whitelisting:**
```yaml
template:
  environment-whitelist:
    - '^FLAPI_.*'    # Only FLAPI_* variables
    - '^DB_.*'       # Only DB_* variables
```

**Enable authentication by default:**
```yaml
# Default auth config
auth:
  enabled: true
  type: basic
  users:
    - username: '{{env.API_USERNAME}}'
      password: '{{env.API_PASSWORD}}'
      roles: [read]
```

### 3. Performance Optimization

**Use caching for expensive queries:**
```yaml
cache:
  enabled: true
  cache-table-name: expensive_query_cache
  cache-source: expensive-query-cache.sql
  refresh-time: 1h
```

**Configure appropriate rate limits:**
```yaml
rate-limit:
  enabled: true
  max: 1000        # Adjust based on your capacity
  interval: 3600   # 1 hour window
```

**Optimize DuckDB settings:**
```yaml
duckdb:
  threads: 8           # Match your CPU cores
  max_memory: 8GB      # Leave room for other processes
  access_mode: READ_WRITE
```

### 4. Configuration Validation

**Use comprehensive request validation:**
```yaml
request:
  - field-name: customer_id
    field-in: query
    description: Customer ID
    required: true
    validators:
      - type: int
        min: 1
        max: 999999
        preventSqlInjection: true  # Always enable
```

**Validate configuration on startup:**
- FLAPI automatically validates configuration on startup
- Use descriptive field names and descriptions
- Test your configuration with sample requests

### 5. Modular Configuration

**Create reusable components:**
```yaml
# common/standard-auth.yaml
auth:
  enabled: true
  type: basic
  users:
    - username: '{{env.API_USER}}'
      password: '{{env.API_PASSWORD}}'
      roles: [read]

# common/standard-rate-limit.yaml
rate-limit:
  enabled: true
  max: 100
  interval: 60
```

**Use includes consistently:**
```yaml
# All endpoints use the same pattern
{{include:auth from ../common/standard-auth.yaml}}
{{include:rate-limit from ../common/standard-rate-limit.yaml}}
```

### 6. Documentation and Comments

**Document your configuration:**
```yaml
# Customer data endpoint
# Provides access to customer information with optional filtering
url-path: /customers

request:
  - field-name: segment
    field-in: query
    description: Filter customers by segment (premium, standard, basic)
    required: false
    validators:
      - type: enum
        allowedValues: [premium, standard, basic]

# Use customer data connection (Parquet files)
connection: [customer-data]

# Cache results for 30 minutes to improve performance
cache:
  enabled: true
  cache-table-name: customers_cache
  refresh-time: 30m
```

---

## Troubleshooting

### Common Configuration Errors

#### 1. YAML Syntax Errors

**Error**: `yaml-cpp: error at line X, column Y`

**Solution**: Check YAML indentation and syntax
```yaml
# ❌ Bad - inconsistent indentation
request:
  - field-name: id
    field-in: query
  description: Customer ID  # Wrong indentation

# ✅ Good - consistent indentation
request:
  - field-name: id
    field-in: query
    description: Customer ID
```

#### 2. Include Path Resolution

**Error**: `Could not resolve include path: common/auth.yaml`

**Solutions**:
- Check file exists: `ls common/auth.yaml`
- Use correct relative paths
- Verify include search paths in configuration

```yaml
# If your structure is:
# endpoints/customers.yaml
# common/auth.yaml

# Use:
{{include from ../common/auth.yaml}}
```

#### 3. Environment Variable Issues

**Error**: Environment variable not found

**Solutions**:
- Check variable is set: `echo $MY_VAR`
- Verify whitelist includes the variable:
```yaml
template:
  environment-whitelist:
    - '^MY_.*'  # Allows MY_VAR
```

#### 4. Connection Configuration

**Error**: `Connection 'my-connection' not found`

**Solutions**:
- Verify connection is defined in `flapi.yaml`:
```yaml
connections:
  my-connection:
    properties:
      path: './data/file.parquet'
```
- Check connection name matches exactly (case-sensitive)

#### 5. SQL Template Issues

**Error**: `Template file not found: customers.sql`

**Solutions**:
- Verify file exists in template path
- Check `template.path` in `flapi.yaml`
- Ensure file has correct extension (`.sql`)

### Debugging Configuration

#### 1. Enable Logging

```yaml
connections:
  debug-connection:
    log-queries: true
    log-parameters: true
    properties:
      path: './data/debug.parquet'
```

#### 2. Use Configuration Validation

FLAPI validates configuration on startup. Check the logs for:
- Missing required fields
- Invalid validator configurations
- Connection issues

#### 3. Test Endpoints

Use curl to test your endpoints:
```bash
# Test basic endpoint
curl "http://localhost:8080/customers?id=123"

# Test with authentication
curl -u "username:password" "http://localhost:8080/customers?id=123"

# Test MCP tools
curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc": "2.0", "id": 1, "method": "tools/list"}'
```

### Performance Troubleshooting

#### 1. Slow Query Performance

- Enable query logging to identify slow queries
- Consider adding caching for expensive operations
- Optimize SQL templates
- Check DuckDB memory and thread settings

#### 2. High Memory Usage

```yaml
duckdb:
  max_memory: 4GB    # Reduce if needed
  threads: 4         # Reduce for memory-constrained systems
```

#### 3. Rate Limiting Issues

Adjust rate limits based on your requirements:
```yaml
rate-limit:
  enabled: true
  max: 10000     # Increase for high-traffic APIs
  interval: 3600 # Adjust time window
```

### Getting Help

1. **Check FLAPI logs** for detailed error messages
2. **Validate YAML syntax** using online YAML validators
3. **Test SQL templates** independently in DuckDB
4. **Review example configurations** in the FLAPI repository
5. **Check environment variables** are properly set and whitelisted

---

## Configuration Options Reference

This section provides a complete reference for all configuration options available in FLAPI.

### Main Configuration File (`flapi.yaml`)

#### Project Settings
```yaml
project_name: string              # Required. Project identifier
project_description: string       # Optional. Human-readable description  
server_name: string              # Optional. Server identifier (default: "flapi-server")
```

#### HTTP Server Settings
```yaml
http_port: integer               # Optional. HTTP port (default: 8080)
enforce-https:
  enabled: boolean               # Optional. Force HTTPS (default: false)
  ssl-cert-file: string         # Required if HTTPS enabled. SSL certificate path
  ssl-key-file: string          # Required if HTTPS enabled. SSL private key path
```

#### Template Configuration
```yaml
template:
  path: string                   # Required. Directory containing SQL templates and endpoint configs
  environment-whitelist:         # Optional. Regex patterns for allowed environment variables
    - string                     # Regex pattern (e.g., '^FLAPI_.*')
```

#### Database Connections
```yaml
connections:
  connection_name:               # Connection identifier
    init: string                 # Optional. SQL commands to run on connection initialization
    properties:                  # Optional. Key-value pairs accessible in templates
      key: string               # Custom properties for this connection
    log-queries: boolean        # Optional. Enable query logging (default: false)
    log-parameters: boolean     # Optional. Enable parameter logging (default: false)
    allow: string              # Optional. Access control pattern (default: '*')
```

#### DuckDB Configuration
```yaml
duckdb:
  db_path: string               # Optional. Database file path (default: in-memory)
  access_mode: string           # Optional. READ_ONLY, READ_WRITE (default: READ_WRITE)
  threads: integer              # Optional. Number of threads (default: system cores)
  max_memory: string            # Optional. Memory limit (e.g., "8GB", default: 80% of system)
  default_order: string         # Optional. ASC, DESC (default: ASC)
```

#### Global Rate Limiting
```yaml
rate-limit:
  enabled: boolean              # Optional. Enable global rate limiting (default: false)
  max: integer                  # Required if enabled. Maximum requests per interval
  interval: integer             # Required if enabled. Time window in seconds
```

#### Global Heartbeat
```yaml
heartbeat:
  enabled: boolean              # Optional. Enable heartbeat worker (default: false)
  worker-interval: integer      # Required if enabled. Check interval in seconds
```

### Endpoint Configuration Files

#### Endpoint Types
Endpoints are automatically detected based on the presence of specific keys:

- **REST Endpoint**: Contains `url-path`
- **MCP Tool**: Contains `mcp-tool` section
- **MCP Resource**: Contains `mcp-resource` section  
- **MCP Prompt**: Contains `mcp-prompt` section
- **Unified**: Can combine multiple types in one file

#### REST Endpoint Configuration
```yaml
url-path: string                # Required. API endpoint path (e.g., "/customers")
method: string                  # Optional. HTTP method (default: "GET")
with-pagination: boolean        # Optional. Enable pagination (default: true)
```

#### MCP Tool Configuration
```yaml
mcp-tool:
  name: string                  # Required. Tool name for MCP protocol
  description: string           # Required. Human-readable tool description
  result_mime_type: string      # Optional. Result MIME type (default: "application/json")
```

#### MCP Resource Configuration
```yaml
mcp-resource:
  name: string                  # Required. Resource name for MCP protocol
  description: string           # Required. Human-readable resource description
  mime_type: string             # Optional. Resource MIME type (default: "application/json")
```

#### MCP Prompt Configuration
```yaml
mcp-prompt:
  name: string                  # Required. Prompt name for MCP protocol
  description: string           # Required. Human-readable prompt description
  template: string              # Required. Prompt template with placeholders
  arguments:                    # Required. List of template arguments
    - string                    # Argument name
```

#### Request Parameters
```yaml
request:
  - field-name: string          # Required. Parameter name
    field-in: string            # Required. Parameter location: query, path, header, body
    description: string         # Required. Human-readable description
    required: boolean           # Optional. Whether parameter is required (default: false)
    default-value: string       # Optional. Default value if not provided
    validators:                 # Optional. List of validation rules
      - type: string            # Required. Validator type
        # ... type-specific options
```

#### Template and Connection
```yaml
template-source: string         # Required. SQL template file name
connection:                     # Required. List of connection names to use
  - string                      # Connection name from main config
```

#### Authentication
```yaml
auth:
  enabled: boolean              # Required. Enable authentication
  type: string                  # Required if enabled. "basic" or "jwt"
  
  # Basic Authentication
  users:                        # Required for basic auth
    - username: string          # Required. Username
      password: string          # Required. Password (use env vars for security)
      roles:                    # Required. List of user roles
        - string                # Role name
  
  # JWT Authentication  
  jwt_secret: string            # Required for JWT. Secret key for token verification
  jwt_issuer: string            # Optional for JWT. Expected token issuer
  
  # AWS Secrets Manager Authentication
  from_aws_secretmanager:       # Optional. Load users from AWS Secrets Manager
    secret_name: string         # Required. Secret name in AWS
    region: string              # Required. AWS region
    secret_table: string        # Required. Table name for user data
    init: string                # Required. SQL to create table from secret JSON
```

#### Rate Limiting (Per-Endpoint)
```yaml
rate-limit:
  enabled: boolean              # Required. Enable rate limiting for this endpoint
  max: integer                  # Required if enabled. Maximum requests per interval
  interval: integer             # Required if enabled. Time window in seconds
```

#### Caching
```yaml
cache:
  enabled: boolean              # Required. Enable caching
  cache-table-name: string      # Required if enabled. Name for cache table
  cache-source: string          # Optional. SQL file for cache population
  refresh-time: string          # Required if enabled. Refresh interval (e.g., "1h", "30m", "300s")
  refresh-endpoint: boolean     # Optional. Enable cache refresh endpoint (default: false)
  max-previous-tables: integer  # Optional. Keep N previous cache versions (default: 5)
```

#### Heartbeat (Per-Endpoint)
```yaml
heartbeat:
  enabled: boolean              # Required. Enable heartbeat for this endpoint
  params:                       # Optional. Parameters passed to heartbeat queries
    key: string                 # Custom key-value pairs
```

### Request Validators Reference

#### String Validator
```yaml
validators:
  - type: string
    regex: string               # Optional. Regular expression pattern
    min: integer                # Optional. Minimum string length
    max: integer                # Optional. Maximum string length
    preventSqlInjection: boolean # Optional. Enable SQL injection prevention (default: true)
```

#### Integer Validator
```yaml
validators:
  - type: int
    min: integer                # Optional. Minimum value (inclusive)
    max: integer                # Optional. Maximum value (inclusive)
    preventSqlInjection: boolean # Optional. Enable SQL injection prevention (default: true)
```

#### Float Validator
```yaml
validators:
  - type: float
    min: float                  # Optional. Minimum value (inclusive)
    max: float                  # Optional. Maximum value (inclusive)
```

#### Email Validator
```yaml
validators:
  - type: email
    # No additional options - validates RFC 5322 email format
```

#### UUID Validator
```yaml
validators:
  - type: uuid
    # No additional options - validates UUID v1-v5 format
```

#### Enum Validator
```yaml
validators:
  - type: enum
    allowedValues:              # Required. List of allowed values
      - string                  # Allowed value
```

#### Date Validator
```yaml
validators:
  - type: date
    min: string                 # Optional. Minimum date in YYYY-MM-DD format
    max: string                 # Optional. Maximum date in YYYY-MM-DD format
```

#### Time Validator
```yaml
validators:
  - type: time
    min: string                 # Optional. Minimum time in HH:MM:SS format
    max: string                 # Optional. Maximum time in HH:MM:SS format
```

### Environment Variables

Environment variables can be used in any string value using the syntax `{{env.VARIABLE_NAME}}`.

#### Configuration
```yaml
template:
  environment-whitelist:        # Required to use environment variables
    - '^FLAPI_.*'              # Allow all variables starting with FLAPI_
    - '^DB_.*'                 # Allow all variables starting with DB_
    - 'SPECIFIC_VAR'           # Allow specific variable
```

#### Usage Examples
```yaml
# In any string value
database_url: '{{env.DATABASE_URL}}'
password: '{{env.DB_PASSWORD}}'
api_key: '{{env.API_KEY}}'

# In connection properties
connections:
  my-db:
    properties:
      host: '{{env.DB_HOST}}'
      port: '{{env.DB_PORT}}'
      user: '{{env.DB_USER}}'
      password: '{{env.DB_PASSWORD}}'

# In file paths
template-source: 'queries-{{env.ENVIRONMENT}}.sql'

# In include paths
{{include from config/{{env.ENVIRONMENT}}-settings.yaml}}
```

---

## YAML Include Syntax Reference

FLAPI's Extended YAML Parser provides powerful include functionality for modular configuration.

### Include Syntax Overview

FLAPI supports several types of include directives using Mustache-inspired syntax:

```yaml
# Basic file include - includes entire file content
{{include from path/to/file.yaml}}

# Section include - includes specific section from file
{{include:section_name from path/to/file.yaml}}

# Conditional include - includes file only if condition is met
{{include from path/to/file.yaml if condition}}

# Section + conditional include
{{include:section_name from path/to/file.yaml if condition}}

# Environment variables in paths
{{include from config/{{env.ENVIRONMENT}}-settings.yaml}}
```

### File Include Directives

#### Basic File Include
Includes the entire content of another YAML file.

**Syntax:**
```yaml
{{include from file_path}}
```

**Example:**
```yaml
# main.yaml
project_name: my-project
{{include from common/database-config.yaml}}
{{include from common/auth-config.yaml}}

# common/database-config.yaml
connections:
  main-db:
    properties:
      path: './data/main.parquet'

# common/auth-config.yaml  
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: secret
```

**Result:**
```yaml
project_name: my-project
connections:
  main-db:
    properties:
      path: './data/main.parquet'
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: secret
```

#### Section Include
Includes only a specific section from another YAML file.

**Syntax:**
```yaml
{{include:section_name from file_path}}
```

**Example:**
```yaml
# endpoint.yaml
url-path: /customers
{{include:request from common/customer-fields.yaml}}
{{include:auth from common/security-config.yaml}}
template-source: customers.sql

# common/customer-fields.yaml
request:
  - field-name: customer_id
    field-in: query
    description: Customer ID
    validators:
      - type: int
        min: 1

other_section:
  not_included: true

# common/security-config.yaml
auth:
  enabled: true
  type: basic
  users:
    - username: api_user
      password: secure_password

rate_limit:
  enabled: true
  max: 100
```

**Result:**
```yaml
url-path: /customers
request:
  - field-name: customer_id
    field-in: query
    description: Customer ID
    validators:
      - type: int
        min: 1
auth:
  enabled: true
  type: basic
  users:
    - username: api_user
      password: secure_password
template-source: customers.sql
```

### Path Resolution

#### Relative Paths
Include paths are resolved relative to the file containing the include directive.

**Directory Structure:**
```
flapi.yaml
├── endpoints/
│   ├── customers.yaml
│   └── orders.yaml
├── common/
│   ├── auth.yaml
│   └── database.yaml
└── config/
    ├── dev.yaml
    └── prod.yaml
```

**Path Examples:**
```yaml
# From endpoints/customers.yaml
{{include from ../common/auth.yaml}}        # Go up one level, then to common/
{{include from ../config/dev.yaml}}        # Go up one level, then to config/
{{include from ./local-overrides.yaml}}    # Same directory as customers.yaml

# From flapi.yaml (root)
{{include from common/auth.yaml}}           # Direct path to common/
{{include from endpoints/shared.yaml}}     # Direct path to endpoints/
```

#### Absolute Paths
You can use absolute paths for includes outside your project:

```yaml
{{include from /etc/flapi/global-config.yaml}}
{{include from /home/user/shared-configs/auth.yaml}}
```

#### Search Paths
Configure additional search paths for includes:

```yaml
# This feature is configured in the ExtendedYamlParser
# Currently used internally, may be exposed in future versions
```

### Environment Variables in Includes

#### Variable Substitution in Paths
Use environment variables to make include paths dynamic:

**Syntax:**
```yaml
{{include from path/{{env.VARIABLE_NAME}}/file.yaml}}
```

**Examples:**
```yaml
# Environment-specific configuration
{{include from config/{{env.ENVIRONMENT}}-database.yaml}}
{{include from config/{{env.ENVIRONMENT}}-auth.yaml}}

# Dynamic file selection
{{include from templates/{{env.TEMPLATE_TYPE}}.yaml}}
{{include from connections/{{env.DATA_SOURCE}}-connection.yaml}}

# Combined with section includes
{{include:auth from security/{{env.AUTH_PROVIDER}}-config.yaml}}
```

**Environment Setup:**
```bash
export ENVIRONMENT=production
export TEMPLATE_TYPE=advanced
export DATA_SOURCE=bigquery
export AUTH_PROVIDER=oauth2
```

#### Variable Substitution in File Content
Environment variables are also substituted in the content of included files:

```yaml
# config/production-database.yaml
connections:
  prod-db:
    properties:
      host: '{{env.DB_HOST}}'
      password: '{{env.DB_PASSWORD}}'
      database: '{{env.DB_NAME}}'
```

### Conditional Includes

#### Basic Conditions
Include files only when certain conditions are met:

**Syntax:**
```yaml
{{include from file_path if condition}}
{{include:section from file_path if condition}}
```

#### Environment Variable Conditions
```yaml
# Include if environment variable is set and non-empty
{{include from debug-config.yaml if env.DEBUG_MODE}}
{{include from ssl-config.yaml if env.ENABLE_SSL}}

# Include based on environment
{{include from dev-tools.yaml if env.ENVIRONMENT}}
```

#### Boolean Conditions
```yaml
# Always include (useful for temporary enabling/disabling)
{{include from optional-config.yaml if true}}

# Never include (useful for temporary disabling)
{{include from experimental-config.yaml if false}}
```

#### Condition Evaluation Rules
- **Environment variables**: 
  - `true` if variable exists and is non-empty
  - `false` if variable doesn't exist or is empty string
- **Boolean literals**: `true` and `false` are supported
- **Missing conditions**: If condition cannot be evaluated, include is skipped

**Examples:**
```bash
# These will include the file
export ENABLE_AUTH=1
export ENABLE_AUTH=true  
export ENABLE_AUTH=yes

# These will NOT include the file
export ENABLE_AUTH=
export ENABLE_AUTH=false
unset ENABLE_AUTH
```

### Advanced Include Patterns

#### Nested Includes
Included files can contain their own include directives:

```yaml
# main.yaml
{{include from config/base.yaml}}

# config/base.yaml
project_name: my-project
{{include from database/{{env.DB_TYPE}}.yaml}}
{{include from auth/{{env.AUTH_TYPE}}.yaml}}

# database/postgres.yaml
connections:
  main:
    {{include from ../connection-templates/postgres-template.yaml}}

# connection-templates/postgres-template.yaml
init: |
  INSTALL postgres_scanner;
  LOAD postgres_scanner;
properties:
  host: '{{env.POSTGRES_HOST}}'
  database: '{{env.POSTGRES_DB}}'
```

#### Complex Conditional Logic
```yaml
# Include different configurations based on environment
{{include from config/development.yaml if env.ENVIRONMENT}}
{{include from config/staging.yaml if env.ENVIRONMENT}}  
{{include from config/production.yaml if env.ENVIRONMENT}}

# Include optional features
{{include from features/caching.yaml if env.ENABLE_CACHE}}
{{include from features/monitoring.yaml if env.ENABLE_MONITORING}}
{{include from features/debug.yaml if env.DEBUG_MODE}}

# Include security configurations
{{include:auth from security/basic-auth.yaml if env.AUTH_TYPE}}
{{include:auth from security/jwt-auth.yaml if env.AUTH_TYPE}}
{{include:auth from security/oauth2-auth.yaml if env.AUTH_TYPE}}
```

#### Modular Endpoint Configuration
```yaml
# endpoints/customer-api.yaml
url-path: /customers

# Include common request parameters
{{include:request from ../common/customer-request-fields.yaml}}

# Include environment-specific authentication
{{include:auth from ../auth/{{env.ENVIRONMENT}}-auth.yaml if env.ENABLE_AUTH}}

# Include optional features
{{include:rate-limit from ../features/rate-limiting.yaml if env.ENABLE_RATE_LIMIT}}
{{include:cache from ../features/caching.yaml if env.ENABLE_CACHE}}

# Include connection configuration
{{include:connection from ../connections/{{env.DATA_SOURCE}}-connection.yaml}}

template-source: customers.sql
```

### Include Configuration Options

The Extended YAML Parser can be configured with various options:

#### Security Settings
```yaml
# These are internal configuration options
# (not directly configurable in YAML files)

allow_recursive_includes: true      # Allow nested includes (default: true)
allow_environment_variables: true   # Allow {{env.VAR}} syntax (default: true)  
allow_conditional_includes: true    # Allow if conditions (default: true)
max_include_depth: 10              # Maximum nesting level (default: 10)
```

#### Environment Variable Whitelist
Control which environment variables can be used:

```yaml
template:
  environment-whitelist:
    - '^FLAPI_.*'      # Allow FLAPI_* variables
    - '^DB_.*'         # Allow DB_* variables
    - 'ENVIRONMENT'    # Allow specific variable
    - 'DEBUG_MODE'     # Allow specific variable
```

### Error Handling

#### Common Include Errors

**File Not Found:**
```
Error: Could not resolve include path: common/missing-file.yaml
```
**Solution:** Check file exists and path is correct relative to including file.

**Circular Dependency:**
```
Error: Circular dependency detected including file: /path/to/file.yaml
```
**Solution:** Remove circular references between include files.

**Invalid Syntax:**
```
Error: Invalid include directive: {{include from file.yaml
```
**Solution:** Ensure include directives are properly closed with `}}`.

**Environment Variable Not Allowed:**
```
Error: Environment variable 'RESTRICTED_VAR' not in whitelist
```
**Solution:** Add variable pattern to `environment-whitelist` in template configuration.

#### Best Practices for Includes

1. **Use Relative Paths**: Keep includes relative to maintain portability
2. **Organize by Function**: Group related configurations in directories
3. **Avoid Deep Nesting**: Limit include depth for maintainability  
4. **Document Dependencies**: Comment which files depend on includes
5. **Test Configurations**: Validate final merged configuration
6. **Use Environment Variables Sparingly**: Only for truly dynamic content

#### Debugging Includes

Enable debug logging to trace include processing:
```bash
# FLAPI will log include processing when debug logging is enabled
export FLAPI_LOG_LEVEL=debug
./flapi -c flapi.yaml
```

Look for log messages like:
```
[DEBUG] Processing include directive: {{include from common/auth.yaml}}
[DEBUG] Resolved include path: /path/to/common/auth.yaml  
[DEBUG] Successfully included file: common/auth.yaml
```

---

This comprehensive guide covers all aspects of FLAPI configuration. For additional examples and updates, refer to the FLAPI repository and documentation.
