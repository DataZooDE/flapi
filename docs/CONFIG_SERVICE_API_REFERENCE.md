# flAPI Config Service API Reference

**Version:** 1.0.0
**flAPI Version:** >= 1.0.0

This document provides a complete reference for the flAPI Config Service REST API and the `flapii` CLI client for runtime configuration management.

---

## Table of Contents

1. [Overview](#1-overview)
   - [Architecture](#architecture)
   - [Enabling Config Service](#enabling-config-service)
   - [Authentication](#authentication)
2. [REST API Reference](#2-rest-api-reference)
   - [2.1 Project Configuration](#21-project-configuration)
   - [2.2 Endpoint Management](#22-endpoint-management)
   - [2.3 Template Management](#23-template-management)
   - [2.4 Cache Management](#24-cache-management)
   - [2.5 Schema Introspection](#25-schema-introspection)
   - [2.6 Server Administration](#26-server-administration)
   - [2.7 Filesystem Operations](#27-filesystem-operations)
3. [CLI Client (flapii)](#3-cli-client-flapii)
   - [3.1 Installation](#31-installation)
   - [3.2 Global Options](#32-global-options)
   - [3.3 Environment Variables](#33-environment-variables)
   - [3.4 Commands Reference](#34-commands-reference)
   - [3.5 Output Formats](#35-output-formats)
4. [Common Patterns](#4-common-patterns)
   - [Error Handling](#error-handling)
   - [Authentication Flow](#authentication-flow)
- [Appendix A: Endpoint Slug Format](#appendix-a-endpoint-slug-format)
- [Appendix B: HTTP Status Codes](#appendix-b-http-status-codes)
- [Related Documentation](#related-documentation)

---

## 1. Overview

### Architecture

The Config Service provides runtime configuration management without server restart:

```
┌─────────────────────────────────────────────────────────┐
│                    flAPI Server                         │
├─────────────────────────────────────────────────────────┤
│  /api/v1/_config/*  ──→  Config Service REST API        │
│  /api/v1/*          ──→  User-defined REST endpoints    │
│  /mcp/*             ──→  MCP Protocol endpoints         │
└─────────────────────────────────────────────────────────┘
                          ↑
                          │ HTTP/HTTPS
                          ↓
┌─────────────────────────────────────────────────────────┐
│                   flapii CLI Client                     │
│  Commands: ping, config, project, endpoints,            │
│            templates, cache, schema, mcp                │
└─────────────────────────────────────────────────────────┘
```

### Enabling Config Service

Start the flAPI server with the `--config-service` flag:

```bash
# With auto-generated token (printed to stdout)
./flapi --config-service

# With custom token
./flapi --config-service --config-service-token "your-secret-token"

# Via environment variable
export FLAPI_CONFIG_SERVICE_TOKEN="your-secret-token"
./flapi --config-service
```

### Authentication

All Config Service endpoints require authentication via bearer token.

**Header Options:**

```http
Authorization: Bearer <token>
```

or:

```http
X-Config-Token: <token>
```

**Unauthenticated Response:**

```http
HTTP/1.1 401 Unauthorized
Content-Type: text/plain

Unauthorized: Invalid or missing token
```

---

## 2. REST API Reference

### 2.1 Project Configuration

#### GET /api/v1/_config/project

Retrieve the main project configuration.

**Request:**
```http
GET /api/v1/_config/project HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "project-name": "my-api",
  "project-description": "Customer data API",
  "server-name": "localhost",
  "http-port": 8080,
  "template": {
    "path": "./sqls"
  }
}
```

**CLI:** `flapii project get`

> **Implementation:** `src/config_service.cpp` | **Tests:** `test/cpp/config_service_test.cpp`

---

#### GET /api/v1/_config/environment-variables

List whitelisted environment variables and their availability.

**Request:**
```http
GET /api/v1/_config/environment-variables HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "variables": [
    {
      "name": "DB_PASSWORD",
      "value": "***",
      "available": true
    },
    {
      "name": "API_KEY",
      "value": null,
      "available": false
    }
  ]
}
```

---

### 2.2 Endpoint Management

#### GET /api/v1/_config/endpoints

List all endpoint configurations.

**Request:**
```http
GET /api/v1/_config/endpoints HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
[
  {
    "url-path": "/customers",
    "method": "GET",
    "template-source": "customers.sql",
    "connection": ["customers-db"],
    "cache": {
      "enabled": true,
      "table": "customers_cache"
    }
  },
  {
    "mcp-tool": {
      "name": "get_customers",
      "description": "Retrieve customer data"
    },
    "template-source": "customers.sql",
    "connection": ["customers-db"]
  }
]
```

**CLI:** `flapii endpoints list`

---

#### POST /api/v1/_config/endpoints

Create a new endpoint.

**Request:**
```http
POST /api/v1/_config/endpoints HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "url-path": "/orders",
  "method": "GET",
  "template-source": "orders.sql",
  "connection": ["orders-db"],
  "request": [
    {
      "field-name": "customer_id",
      "field-in": "query",
      "required": false,
      "validators": [
        { "type": "int", "min": 1 }
      ]
    }
  ]
}
```

**Response (201):**
```json
{
  "success": true,
  "message": "Endpoint created",
  "slug": "GET-orders"
}
```

**CLI:** `flapii endpoints create -f endpoint.yaml`

---

#### GET /api/v1/_config/endpoints/{slug}

Get a specific endpoint configuration.

**Path Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `slug` | string | Endpoint identifier (see [Appendix A](#appendix-a-endpoint-slug-format)) |

**Request:**
```http
GET /api/v1/_config/endpoints/GET-customers HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "url-path": "/customers",
  "method": "GET",
  "template-source": "customers.sql",
  "connection": ["customers-db"],
  "request": [...],
  "cache": {...},
  "auth": {...}
}
```

**CLI:** `flapii endpoints get /customers`

---

#### PUT /api/v1/_config/endpoints/{slug}

Update an endpoint configuration.

**Request:**
```http
PUT /api/v1/_config/endpoints/GET-customers HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "url-path": "/customers",
  "method": "GET",
  "template-source": "customers-v2.sql",
  "connection": ["customers-db"]
}
```

**Response (200):**
```json
{
  "success": true,
  "message": "Endpoint updated"
}
```

**CLI:** `flapii endpoints update /customers -f endpoint.yaml`

---

#### DELETE /api/v1/_config/endpoints/{slug}

Delete an endpoint.

**Request:**
```http
DELETE /api/v1/_config/endpoints/GET-customers HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "success": true,
  "message": "Endpoint deleted"
}
```

**CLI:** `flapii endpoints delete /customers --force`

---

#### POST /api/v1/_config/endpoints/{slug}/validate

Validate endpoint configuration.

**Request:**
```http
POST /api/v1/_config/endpoints/GET-customers/validate HTTP/1.1
Authorization: Bearer <token>
Content-Type: text/plain

url-path: /customers
method: GET
template-source: customers.sql
connection: [customers-db]
```

**Response (200):**
```json
{
  "valid": true,
  "errors": [],
  "warnings": []
}
```

**CLI:** `flapii endpoints validate /customers -f endpoint.yaml`

---

#### POST /api/v1/_config/endpoints/{slug}/reload

Reload endpoint configuration from disk.

**Request:**
```http
POST /api/v1/_config/endpoints/GET-customers/reload HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "success": true,
  "message": "Endpoint reloaded from disk"
}
```

**CLI:** `flapii endpoints reload /customers`

---

#### GET /api/v1/_config/endpoints/{slug}/parameters

Get parameter definitions for an endpoint.

**Request:**
```http
GET /api/v1/_config/endpoints/GET-customers/parameters HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "endpoint": "/customers",
  "method": "GET",
  "parameters": [
    {
      "name": "id",
      "in": "query",
      "description": "Customer ID",
      "required": false,
      "default": null,
      "validators": [
        { "type": "int", "min": 1 }
      ]
    }
  ]
}
```

---

#### POST /api/v1/_config/endpoints/by-template

Find endpoints using a specific template file.

**Request:**
```http
POST /api/v1/_config/endpoints/by-template HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "template_path": "customers.sql"
}
```

**Response (200):**
```json
{
  "template_path": "customers.sql",
  "count": 2,
  "endpoints": [
    {
      "url_path": "/customers",
      "method": "GET",
      "type": "REST"
    },
    {
      "mcp_name": "get_customers",
      "type": "MCP_Tool"
    }
  ]
}
```

> **Implementation:** `src/config_service.cpp`, `src/endpoint_repository.cpp` | **Tests:** `test/cpp/config_service_test.cpp`, `test/integration/test_config_service_endpoints.tavern.yaml`

---

### 2.3 Template Management

#### GET /api/v1/_config/endpoints/{slug}/template

Get the SQL template content.

**Request:**
```http
GET /api/v1/_config/endpoints/GET-customers/template HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "template": "SELECT * FROM customers\nWHERE 1=1\n{{#params.id}}\n  AND id = {{ params.id }}\n{{/params.id}}"
}
```

**CLI:** `flapii templates get /customers`

---

#### PUT /api/v1/_config/endpoints/{slug}/template

Update the SQL template.

**Request:**
```http
PUT /api/v1/_config/endpoints/GET-customers/template HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "template": "SELECT * FROM customers WHERE status = 'active'"
}
```

**Response (200):**
```json
{
  "success": true,
  "message": "Template updated"
}
```

**CLI:** `flapii templates update /customers -f template.sql`

---

#### POST /api/v1/_config/endpoints/{slug}/template/expand

Expand a Mustache template with parameters.

**Query Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `include_variables` | boolean | Include variable metadata in response |
| `validate_only` | boolean | Only validate, don't expand |

**Request:**
```http
POST /api/v1/_config/endpoints/GET-customers/template/expand HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "parameters": {
    "id": "123",
    "status": "active"
  }
}
```

**Response (200):**
```json
{
  "expanded": "SELECT * FROM customers\nWHERE 1=1\n  AND id = 123\n  AND status = 'active'"
}
```

**CLI:** `flapii templates expand /customers -p '{"id":"123"}'`

---

#### POST /api/v1/_config/endpoints/{slug}/template/test

Execute a template with sample parameters (limited to 10 rows).

**Request:**
```http
POST /api/v1/_config/endpoints/GET-customers/template/test HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "parameters": {
    "id": "123"
  }
}
```

**Response (200):**
```json
{
  "success": true,
  "columns": ["id", "name", "email"],
  "rows": [
    {"id": 123, "name": "John Doe", "email": "john@example.com"}
  ]
}
```

**CLI:** `flapii templates test /customers -p '{"id":"123"}'`

> **Implementation:** `src/config_service.cpp` | **Tests:** `test/cpp/config_service_template_lookup_test.cpp`

---

### 2.4 Cache Management

#### GET /api/v1/_config/endpoints/{slug}/cache

Get cache configuration.

**Request:**
```http
GET /api/v1/_config/endpoints/GET-customers/cache HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "enabled": true,
  "table": "customers_cache",
  "schema": "cache",
  "schedule": "5m",
  "primary-key": ["id"],
  "cursor": {
    "column": "updated_at",
    "type": "timestamp"
  },
  "retention": {
    "keep-last-snapshots": 5,
    "max-snapshot-age": "30d"
  }
}
```

**CLI:** `flapii cache get /customers`

---

#### PUT /api/v1/_config/endpoints/{slug}/cache

Update cache configuration.

**Request:**
```http
PUT /api/v1/_config/endpoints/GET-customers/cache HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "enabled": true,
  "table": "customers_cache",
  "schedule": "10m"
}
```

**Response (200):**
```json
{
  "success": true,
  "message": "Cache configuration updated"
}
```

**CLI:** `flapii cache update /customers --enabled true --ttl 600`

---

#### GET /api/v1/_config/endpoints/{slug}/cache/template

Get the cache populate template.

**Request:**
```http
GET /api/v1/_config/endpoints/GET-customers/cache/template HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "template": "INSERT INTO {{cache.table}} SELECT * FROM source"
}
```

**CLI:** `flapii cache template /customers`

---

#### PUT /api/v1/_config/endpoints/{slug}/cache/template

Update the cache populate template.

**Request:**
```http
PUT /api/v1/_config/endpoints/GET-customers/cache/template HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "template": "REPLACE INTO {{cache.table}} SELECT * FROM customers"
}
```

**Response (200):**
```json
{
  "success": true,
  "message": "Cache template updated"
}
```

**CLI:** `flapii cache update-template /customers -f cache.sql`

---

#### POST /api/v1/_config/endpoints/{slug}/cache/refresh

Trigger a cache refresh.

**Request:**
```http
POST /api/v1/_config/endpoints/GET-customers/cache/refresh HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "success": true,
  "message": "Cache refresh triggered"
}
```

**CLI:** `flapii cache refresh /customers`

---

#### POST /api/v1/_config/endpoints/{slug}/cache/gc

Run garbage collection on cache snapshots.

**Request:**
```http
POST /api/v1/_config/endpoints/GET-customers/cache/gc HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
"Garbage collection completed"
```

---

#### GET /api/v1/_config/endpoints/{slug}/cache/audit

Get cache synchronization audit log for an endpoint.

**Request:**
```http
GET /api/v1/_config/endpoints/GET-customers/cache/audit HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
[
  {
    "event_id": 1,
    "endpoint_path": "/customers",
    "cache_table": "customers_cache",
    "sync_type": "incremental",
    "status": "success",
    "rows_affected": 42,
    "sync_started_at": "2025-01-10T14:30:00Z",
    "sync_completed_at": "2025-01-10T14:30:05Z",
    "duration_ms": 5234
  }
]
```

---

#### GET /api/v1/_config/cache/audit

Get all cache synchronization audit logs.

**Request:**
```http
GET /api/v1/_config/cache/audit HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):** Array of audit log entries for all endpoints.

> **Implementation:** `src/config_service.cpp`, `src/cache_manager.cpp` | **Tests:** `test/integration/test_ducklake_cache.tavern.yaml`, `test/integration/test_ducklake_comprehensive.tavern.yaml`

---

### 2.5 Schema Introspection

#### GET /api/v1/_config/schema

Retrieve database schema information.

**Query Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `tables` | boolean | Return only table list |
| `connections` | boolean | Return only connection info |
| `format` | string | `completion` for flattened format |
| `connection` | string | Filter by connection name |

**Request:**
```http
GET /api/v1/_config/schema HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "main": {
    "tables": {
      "customers": {
        "is_view": false,
        "columns": {
          "id": { "type": "INTEGER", "nullable": false },
          "name": { "type": "VARCHAR", "nullable": false },
          "email": { "type": "VARCHAR", "nullable": true }
        }
      }
    }
  }
}
```

**CLI:** `flapii schema get`

---

#### POST /api/v1/_config/schema/refresh

Refresh the cached schema information.

**Request:**
```http
POST /api/v1/_config/schema/refresh HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "success": true,
  "message": "Schema refreshed"
}
```

**CLI:** `flapii schema refresh`

> **Implementation:** `src/config_service.cpp`, `src/database_manager.cpp` | **Tests:** `test/cpp/config_service_schema_test.cpp`, `test/integration/test_config_service_schema.py`

---

### 2.6 Server Administration

#### GET /api/v1/_config/log-level

Get the current log level.

**Request:**
```http
GET /api/v1/_config/log-level HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "level": "info"
}
```

**CLI:** `flapii config log-level get`

---

#### PUT /api/v1/_config/log-level

Set the server log level.

**Request:**
```http
PUT /api/v1/_config/log-level HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "level": "debug"
}
```

**Valid Levels:** `debug`, `info`, `warning`, `error`

**Response (200):**
```json
{
  "level": "debug",
  "message": "Log level updated successfully"
}
```

**CLI:** `flapii config log-level set debug`

> **Implementation:** `src/config_service.cpp` | **Tests:** `test/integration/test_mcp_methods.py` (GET only), *PUT not tested - see [TEST_TODO.md](./TEST_TODO.md)*

---

### 2.7 Filesystem Operations

#### GET /api/v1/_config/filesystem

Get the project filesystem structure.

**Request:**
```http
GET /api/v1/_config/filesystem HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "base_path": "/home/user/project",
  "config_file": "flapi.yaml",
  "config_file_exists": true,
  "template_path": "/home/user/project/sqls",
  "tree": [
    {
      "name": "customers.yaml",
      "type": "file",
      "path": "customers.yaml",
      "extension": ".yaml",
      "yaml_type": "endpoint",
      "url_path": "/customers"
    },
    {
      "name": "customers.sql",
      "type": "file",
      "path": "customers.sql",
      "extension": ".sql"
    }
  ]
}
```

---

#### GET /api/v1/doc.yaml

Get OpenAPI/Swagger documentation.

**Request:**
```http
GET /api/v1/doc.yaml HTTP/1.1
```

**Response (200):** OpenAPI YAML specification

**Note:** This endpoint does not require authentication.

> **Implementation:** `src/config_service.cpp` | **Tests:** `test/cpp/config_service_filesystem_test.cpp`, *OpenAPI endpoint not tested - see [TEST_TODO.md](./TEST_TODO.md)*

---

## 3. CLI Client (flapii)

### 3.1 Installation

```bash
# From the cli directory
cd cli
npm install
npm link

# Verify installation
flapii --version
```

### 3.2 Global Options

| Option | Short | Type | Default | Description |
|--------|-------|------|---------|-------------|
| `--config` | `-c` | string | - | Path to flapi.yaml configuration file |
| `--base-url` | `-u` | string | - | Base URL of the flapi server |
| `--auth-token` | `-t` | string | - | Bearer token for authentication |
| `--config-service-token` | - | string | - | Config service authentication token |
| `--timeout` | - | number | - | Request timeout in seconds |
| `--retries` | - | number | - | Number of retries for failed requests |
| `--insecure` | - | boolean | `false` | Disable TLS certificate verification |
| `--output` | `-o` | string | `json` | Output format: `json` or `table` |
| `--json-style` | - | string | `camel` | JSON key casing: `camel` or `hyphen` |
| `--debug-http` | - | boolean | `false` | Enable HTTP debugging |
| `--quiet` | `-q` | boolean | `false` | Suppress non-error output |
| `--yes` | `-y` | boolean | `false` | Assume yes for prompts |

### 3.3 Environment Variables

| Variable | Description |
|----------|-------------|
| `FLAPI_BASE_URL` | Base URL of the flapi server |
| `FLAPI_TOKEN` | Bearer authentication token |
| `FLAPI_CONFIG_SERVICE_TOKEN` | Config service authentication token |
| `FLAPI_TIMEOUT` | Request timeout in seconds |
| `FLAPI_RETRIES` | Number of retries |
| `FLAPI_GEMINI_KEY` | Google Gemini API key for AI-assisted endpoint creation |

### 3.4 Commands Reference

#### ping

Check server connectivity.

```bash
flapii ping
```

**API:** `GET /api/v1/_config/project`

---

#### config show

Display current CLI configuration.

```bash
flapii config show
```

---

#### config validate

Validate endpoint YAML files locally.

```bash
flapii config validate
flapii config validate -f endpoint.yaml
flapii config validate -d ./sqls
```

| Option | Short | Description |
|--------|-------|-------------|
| `--file` | `-f` | Specific YAML file to validate |
| `--dir` | `-d` | Directory containing YAML files |

---

#### config log-level

Manage server log level.

```bash
flapii config log-level get
flapii config log-level set <level>
flapii config log-level list
```

**Levels:** `debug`, `info`, `warning`, `error`

---

#### project get

Get project configuration from server.

```bash
flapii project get
flapii project get --output table
```

---

#### project init

Initialize a new flapi project.

```bash
flapii project init
flapii project init my-api
flapii project init --force
```

| Option | Description |
|--------|-------------|
| `--force` | Overwrite existing files |
| `--skip-validation` | Skip config validation after setup |
| `--no-examples` | Create minimal structure |
| `--advanced` | Include additional templates |
| `--ai` | Use AI assistance (requires Gemini API key) |

**Creates:**
```
project/
├── flapi.yaml
├── sqls/
│   ├── sample.yaml
│   └── sample.sql
├── data/
├── common/
└── .gitignore
```

---

#### endpoints list

List all endpoints.

```bash
flapii endpoints list
flapii endpoints list --output table
```

---

#### endpoints get

Get specific endpoint configuration.

```bash
flapii endpoints get /customers
flapii endpoints get /users/:id
```

---

#### endpoints create

Create a new endpoint.

```bash
flapii endpoints create -f endpoint.yaml
cat endpoint.json | flapii endpoints create --stdin
```

| Option | Short | Description |
|--------|-------|-------------|
| `--file` | `-f` | JSON/YAML file with endpoint config |
| `--stdin` | - | Read config from stdin |

---

#### endpoints update

Update an existing endpoint.

```bash
flapii endpoints update /customers -f endpoint.yaml
```

---

#### endpoints delete

Delete an endpoint.

```bash
flapii endpoints delete /customers
flapii endpoints delete /customers --force
```

---

#### endpoints validate

Validate endpoint configuration.

```bash
flapii endpoints validate /customers -f endpoint.yaml
```

---

#### endpoints reload

Reload endpoint from disk.

```bash
flapii endpoints reload /customers
```

---

#### endpoints wizard

Interactive endpoint creation with optional AI assistance.

```bash
flapii endpoints wizard
flapii endpoints wizard --ai
flapii endpoints wizard --output-file endpoint.yaml
flapii endpoints wizard --dry-run
```

| Option | Description |
|--------|-------------|
| `--ai` | Use AI generation (requires Gemini API key) |
| `--output-file` | Save to file instead of creating |
| `--skip-validation` | Skip validation before save |
| `--dry-run` | Preview without creating |

---

#### templates list

List all templates.

```bash
flapii templates list
```

---

#### templates get

Get template content.

```bash
flapii templates get /customers
```

---

#### templates update

Update template content.

```bash
flapii templates update /customers -f template.sql
cat template.sql | flapii templates update /customers --stdin
```

---

#### templates expand

Expand template with parameters.

```bash
flapii templates expand /customers -p '{"id":"123"}'
flapii templates expand /customers --params-file params.json
```

| Option | Short | Description |
|--------|-------|-------------|
| `--params` | `-p` | JSON parameters inline |
| `--params-file` | - | Read parameters from file |

---

#### templates test

Test template with parameters.

```bash
flapii templates test /customers -p '{"id":"123"}'
```

---

#### cache list

List all cache configurations.

```bash
flapii cache list
```

---

#### cache get

Get cache configuration.

```bash
flapii cache get /customers
```

---

#### cache update

Update cache configuration.

```bash
flapii cache update /customers --enabled true --ttl 3600
flapii cache update /customers -f cache.json
```

| Option | Short | Description |
|--------|-------|-------------|
| `--enabled` | `-e` | Enable/disable caching |
| `--ttl` | `-t` | Cache TTL in seconds |
| `--max-size` | `-s` | Maximum cache size |
| `--strategy` | - | Cache strategy |
| `--file` | `-f` | JSON config file |
| `--stdin` | - | Read from stdin |

---

#### cache template

Get cache populate template.

```bash
flapii cache template /customers
```

---

#### cache update-template

Update cache populate template.

```bash
flapii cache update-template /customers -f cache.sql
```

---

#### cache refresh

Trigger cache refresh.

```bash
flapii cache refresh /customers
flapii cache refresh /customers --force
```

---

#### schema get

Get database schema.

```bash
flapii schema get
flapii schema get -c postgres-db
flapii schema get -t customers
```

| Option | Short | Description |
|--------|-------|-------------|
| `--connection` | `-c` | Specific connection |
| `--table` | `-t` | Specific table |
| `--database` | `-d` | Specific database |

---

#### schema refresh

Refresh schema cache.

```bash
flapii schema refresh
flapii schema refresh -c postgres-db --force
```

---

#### schema connections

List database connections.

```bash
flapii schema connections
```

---

#### schema tables

List tables.

```bash
flapii schema tables
flapii schema tables -c postgres-db
```

---

#### mcp tools list

List MCP tools.

```bash
flapii mcp tools list
```

---

#### mcp tools get

Get MCP tool configuration.

```bash
flapii mcp tools get get_customers
```

---

#### mcp resources list

List MCP resources.

```bash
flapii mcp resources list
```

---

#### mcp resources get

Get MCP resource configuration.

```bash
flapii mcp resources get customer_schema
```

---

#### mcp prompts list

List MCP prompts.

```bash
flapii mcp prompts list
```

---

#### mcp prompts get

Get MCP prompt configuration.

```bash
flapii mcp prompts get analyze_customer
```

---

### 3.5 Output Formats

#### JSON Output (Default)

```bash
flapii endpoints list --output json
```

**Key Casing:**
- `--json-style camel` (default): `urlPath`, `templateSource`
- `--json-style hyphen`: `url-path`, `template-source`

#### Table Output

```bash
flapii endpoints list --output table
```

Features colored columns and formatted borders.

> **Implementation:** `cli/src/` | **Tests:** `cli/test/`

---

## 4. Common Patterns

### Error Handling

**HTTP Status Codes:**

| Code | Description | CLI Behavior |
|------|-------------|--------------|
| 200 | Success | Display result |
| 201 | Created | Display success message |
| 400 | Bad Request | Display validation error |
| 401 | Unauthorized | Display auth error, exit 1 |
| 404 | Not Found | Display not found error |
| 429, 500-504 | Server Error | Auto-retry with backoff |

**Error Response Format:**

```json
{
  "error": "Validation failed",
  "details": ["field-name is required"]
}
```

### Authentication Flow

```bash
# Option 1: Environment variable
export FLAPI_CONFIG_SERVICE_TOKEN="your-token"
flapii endpoints list

# Option 2: Command-line flag
flapii -t "your-token" endpoints list

# Option 3: Config service token flag
flapii --config-service-token "your-token" endpoints list
```

---

## Appendix A: Endpoint Slug Format

Slugs identify endpoints in API paths. The format depends on endpoint type:

| Type | Format | Example |
|------|--------|---------|
| REST | `{METHOD}-{path}` | `GET-customers`, `POST-orders` |
| MCP Tool | `{tool_name}` | `get_customers` |
| MCP Resource | `resource-{name}` | `resource-schema` |
| MCP Prompt | `prompt-{name}` | `prompt-analysis` |

**Path Conversion Rules:**
- Leading `/` removed
- `/` becomes `-slash-`
- Special characters become `-`
- Empty path becomes `empty`

**Examples:**

| URL Path | Slug |
|----------|------|
| `/customers` | `GET-customers` |
| `/customers/` | `GET-customers-slash` |
| `/api/v1/users` | `GET-api-slash-v1-slash-users` |

---

## Appendix B: HTTP Status Codes

| Code | Description |
|------|-------------|
| 200 | OK - Request successful |
| 201 | Created - Resource created |
| 400 | Bad Request - Invalid input |
| 401 | Unauthorized - Invalid or missing token |
| 404 | Not Found - Resource doesn't exist |
| 500 | Internal Server Error |
| 501 | Not Implemented |
| 503 | Service Unavailable |

---

## Related Documentation

- [Configuration Reference](./CONFIG_REFERENCE.md) - Configuration file options
- [CLI Reference](./CLI_REFERENCE.md) - Server executable options
- [MCP Reference](./MCP_REFERENCE.md) - Model Context Protocol implementation
