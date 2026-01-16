# flapi Developer Guide - AGENT.md

This document provides comprehensive technical information about flapi for developers working on the project. It covers architecture, design patterns, key components, and how everything fits together.

---

## Table of Contents

1. [Project Overview](#project-overview)
2. [Architecture](#architecture)
3. [Core Concepts](#core-concepts)
4. [Directory Structure](#directory-structure)
5. [Key Components](#key-components)
6. [Data Structures and Schemas](#data-structures-and-schemas)
7. [Configuration System](#configuration-system)
8. [CLI Architecture](#cli-architecture)
9. [Server Implementation](#server-implementation)
10. [SQL Template Engine](#sql-template-engine)
11. [Caching System (DuckLake)](#caching-system-ducklake)
12. [Validators and Input Validation](#validators-and-input-validation)
13. [API Protocols](#api-protocols)
14. [Build System](#build-system)
15. [Testing Infrastructure](#testing-infrastructure)
16. [Development Workflows](#development-workflows)
17. [Extension Points](#extension-points)
18. [Common Issues and Solutions](#common-issues-and-solutions)

---

## Project Overview

### What is flapi?

**flapi** (Fast & Flexible API) is a **SQL-to-API framework** that automatically generates REST APIs and AI-compatible tools (MCP - Model Context Protocol) from SQL templates and YAML configurations.

**Key Innovation**: Instead of writing backend code to expose data, analysts write SQL queries and YAML configurations, and flapi handles:
- HTTP server implementation
- Parameter validation
- SQL template rendering (Mustache)
- Caching with DuckLake
- Authentication and rate limiting
- MCP protocol implementation

### Core Value Proposition

- **No backend coding**: REST API emerges from SQL + YAML
- **Data analyst friendly**: Uses SQL (familiar skill)
- **Multi-protocol**: REST + MCP + streaming (future)
- **Built-in intelligence**: Validators, caching, auth, rate-limiting
- **DuckDB powered**: Access 50+ data sources (BigQuery, Postgres, S3, etc.)

### Target Users

- Data analysts who know SQL
- Companies wanting to expose data quickly
- Teams combining analytics with AI (MCP)

---

## Architecture

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     HTTP Client / AI Agent                   │
└────────────────────────────┬────────────────────────────────┘
                             │ (REST or MCP)
┌────────────────────────────▼────────────────────────────────┐
│              flapi Server (C++ Binary)                        │
├─────────────────────────────────────────────────────────────┤
│  REST API Handler    │   MCP Handler    │   CLI Management   │
│  ├─ Request parsing  │   ├─ Tool mgmt   │   ├─ Endpoints    │
│  ├─ Parameter valid. │   ├─ Resources   │   ├─ Templates    │
│  └─ Response format  │   └─ Prompts     │   └─ Cache ctrl   │
├─────────────────────────────────────────────────────────────┤
│                   Core Engine Layer                           │
│  ├─ Template Engine (Mustache)                              │
│  ├─ Parameter Validator                                      │
│  ├─ Request Router                                           │
│  └─ Response Formatter                                       │
├─────────────────────────────────────────────────────────────┤
│                Cache Management (DuckLake)                    │
│  ├─ Snapshot Manager                                         │
│  ├─ Refresh Scheduler                                        │
│  ├─ Time-travel Query Support                               │
│  └─ Garbage Collection                                       │
├─────────────────────────────────────────────────────────────┤
│                 DuckDB Query Execution                        │
│  ├─ Extension Manager (postgres_scanner, httpfs, etc.)      │
│  ├─ Query Optimizer                                          │
│  ├─ Result Streaming                                         │
│  └─ Transaction Support                                      │
├─────────────────────────────────────────────────────────────┤
│            Data Sources (via DuckDB extensions)               │
│  ├─ Local Files (Parquet, CSV, JSON)                        │
│  ├─ Cloud Storage (S3, GCS, Azure)                          │
│  ├─ Databases (BigQuery, Postgres, MySQL)                   │
│  └─ APIs (HTTP, etc.)                                        │
└─────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│                    Configuration Files (YAML)                 │
│  ├─ flapi.yaml (main config, connections, global settings)  │
│  ├─ sqls/*.yaml (endpoint definitions)                       │
│  └─ sqls/*.sql (SQL templates with Mustache)                │
└──────────────────────────────────────────────────────────────┘
```

### Layered Design

**Layer 1: User/Config Layer**
- YAML endpoint configurations
- SQL templates with Mustache syntax
- User HTTP requests

**Layer 2: Request Processing**
- Parameter extraction and validation
- Request routing to endpoint handler
- Template expansion with user parameters

**Layer 3: Query Execution**
- Mustache template to SQL rendering
- DuckDB query execution
- Result formatting

**Layer 4: Response Handling**
- JSON serialization
- HTTP response formatting
- Optional caching (DuckLake)

### Key Design Principles

1. **Configuration Over Code**: Logic lives in YAML/SQL, not compiled code
2. **Declarative Endpoints**: Define what, not how
3. **SQL as API Language**: Analysts work in familiar SQL
4. **Zero Backend Coding**: Framework handles all boilerplate
5. **Multi-Protocol First**: REST and MCP equally supported
6. **Performance by Default**: Caching built-in, not added
7. **Extensible**: Via DuckDB extensions and custom validators

---

## Core Concepts

### 1. Connections

A connection defines access to a data source.

**Structure**:
```yaml
connections:
  connection-name:
    properties:
      # Connection-specific properties
      path: './data/file.parquet'
      # OR
      host: 'db.example.com'
      port: '5432'
      database: 'mydb'
      user: 'read_user'
      password: '${POSTGRES_PASSWORD}'  # Environment variable
```

**Types**:
- **File-based**: path property (Parquet, CSV, SQLite)
- **Database**: host, port, database, user, password
- **Cloud Storage**: bucket, region, credentials
- **API**: URL, auth token

**Implementation Notes**:
- Properties are custom per connection type
- Environment variables supported via `${VAR_NAME}`
- Environment whitelist in flapi.yaml restricts which vars accessible
- Connection loaded once at startup, reused for all requests

### 2. Endpoints

An endpoint is a callable API operation.

**Structure**:
```yaml
url-path: /customers
method: GET  # or POST, PUT, DELETE, PATCH (default GET)

request:
  - field-name: id
    field-in: query  # or path, body, header
    description: "Customer ID"
    required: true
    validators:
      - type: int
        min: 1
        max: 999999

template-source: customers.sql
connection: [conn-name]

operation:  # Only for write operations (POST, PUT, DELETE)
  type: write
  returns-data: true
  transaction: true
  validate-before-write: true

auth:  # Optional
  type: basic
  users:
    - username: admin
      password: ${ADMIN_PASSWORD}

rate-limit:  # Optional
  enabled: true
  requests: 100
  window: 60  # seconds

cache:  # Optional, DuckLake
  enabled: true
  table: customers_cache
  schema: analytics
  schedule: "6h"
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  template_file: customers-cache-populate.sql
```

**Request Parameter Fields**:
- `field-name`: Parameter identifier
- `field-in`: Where parameter comes from (query/path/body/header)
- `description`: Human-readable, used in MCP tool docs
- `required`: true/false (default false)
- `validators`: List of validation rules

**Types**:
- **GET**: Read-only, parameters from query/path
- **POST**: Create, parameters from body
- **PUT**: Update, parameters from body/path
- **DELETE**: Delete, often just id in path

**Lifecycle**:
1. Request arrives with parameters
2. Parameters extracted per `field-in`
3. Validators applied (fail fast)
4. Template expanded with validated parameters
5. SQL executed
6. Results formatted as JSON
7. Response sent

### 3. SQL Templates (Mustache)

Templates are Mustache files that generate SQL from parameters.

**Syntax**:

```sql
-- Parameters (provided by request)
SELECT * FROM table WHERE id = {{ params.id }}

-- Conditional sections (optional parameters)
{{#params.name}}
  AND name LIKE '{{{ params.name }}}%'
{{/params.name}}

-- Connection properties
SELECT * FROM read_parquet('{{{ conn.path }}}')

-- Cache metadata
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM source
{{#cache.previousSnapshotTimestamp}}
WHERE updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}'
{{/cache.previousSnapshotTimestamp}}

-- Environment variables (whitelisted)
SELECT * FROM postgres_scan('password={{env.DB_PASSWORD}}')
```

**Key Rules**:

1. **Triple braces `{{{ }}}` for strings**: Auto-escapes quotes, prevents SQL injection
2. **Double braces `{{ }}` for numbers**: No quotes, safe for numeric types
3. **Conditional sections `{{#field}}...{{/field}}`**: Only rendered if field exists/true
4. **Inverted sections `{{^field}}...{{/field}}`**: Only rendered if field doesn't exist
5. **Access properties**: `params.name`, `conn.property`, `cache.table`, `env.VAR`

**Processing Pipeline**:
1. Load template from disk
2. Extract parameters from request (validates)
3. Build Mustache context: `{ params: {...}, conn: {...}, cache: {...} }`
4. Render template (expand Mustache)
5. Execute resulting SQL
6. Format results

**Design Pattern - Safe Query Building**:
```sql
SELECT * FROM table WHERE 1=1
{{#params.filter1}}
  AND column1 = '{{{ params.filter1 }}}'
{{/params.filter1}}
{{#params.filter2}}
  AND column2 = {{{ params.filter2 }}}
{{/params.filter2}}
ORDER BY created_at DESC
LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}100{{/params.limit}}
```

This pattern:
- `WHERE 1=1` allows adding any number of AND conditions
- Each condition wrapped in conditional section
- Default limit applied if not specified

### 4. Validators

Validators enforce input constraints before SQL execution.

**Types**:

```yaml
validators:
  # Integer with range
  - type: int
    min: 1
    max: 999999

  # String with length
  - type: string
    min-length: 1
    max-length: 200
    pattern: "^[a-zA-Z0-9_]+$"  # Optional regex

  # Floating point
  - type: float
    min: 0.01
    max: 99999.99

  # Email format
  - type: email

  # UUID v4 format
  - type: uuid

  # Enumeration (whitelist)
  - type: enum
    values: ["active", "inactive", "pending"]

  # Date (ISO 8601)
  - type: date
    min: "2020-01-01"
    max: "2025-12-31"

  # Time (ISO 8601)
  - type: time
    min: "09:00:00"
    max: "17:00:00"
```

**Validation Pipeline**:
1. Request arrives with parameters
2. For each parameter:
   - Check if required (must be present)
   - Apply all validators in order
   - If any fails, return 400 Bad Request
3. If all pass, continue to template expansion

**Error Response**:
```json
{
  "error": "Invalid parameter: product_id - must be an integer with min: 1"
}
```

**Purpose**:
- **Security**: Prevent SQL injection (whitelist validation)
- **Data integrity**: Ensure correct types and ranges
- **User feedback**: Clear error messages

**Triple Braces Protection**:
Even with triple braces, validators add defense in depth:
- Validators: First line (prevent bad input)
- Triple braces: Second line (escape if somehow reached)

### 5. DuckLake Caching

Snapshot-based table caching for performance.

**Concept**: Instead of caching query results, cache entire tables with versions.

**Modes**:

1. **Full Refresh**: Recreate entire table each refresh
   ```yaml
   cache:
     enabled: true
     table: products_cache
     schedule: "1d"
     # No primary-key or cursor
   ```
   - Simple but slow for large tables
   - Good for small reference data

2. **Incremental Append**: Only add new rows
   ```yaml
   cache:
     enabled: true
     table: events_cache
     schedule: "15m"
     cursor:
       column: created_at
       type: timestamp
   ```
   - Fast for append-only data
   - Won't catch deletes

3. **Incremental Merge**: Upsert (insert/update/delete)
   ```yaml
   cache:
     enabled: true
     table: customers_cache
     schedule: "6h"
     primary-key: [id]
     cursor:
       column: updated_at
       type: timestamp
   ```
   - Complex but handles all changes
   - Best for mutable data

**Snapshot Lifecycle**:

```
Time 0: Cache doesn't exist
  → Run cache populate template (full)
  → Create snapshot v1 (all rows)

Time 6h: Schedule triggers refresh
  → Run cache populate template (incremental)
  → Fetch rows changed since v1.timestamp
  → MERGE into cache table
  → Create snapshot v2 (only changed rows stored)

Time 12h: Schedule triggers refresh
  → Run cache populate template (incremental)
  → Fetch rows changed since v2.timestamp
  → MERGE into cache table
  → Create snapshot v3

Time-travel query:
  → SELECT * FROM cache AS OF TIMESTAMP '...' (any snapshot)
```

**Configuration**:
```yaml
cache:
  table: table_name           # Cache table name
  schema: analytics           # Cache schema
  schedule: "6h"              # How often to refresh
  primary-key: [id]           # For merge mode
  cursor:
    column: updated_at        # Tracks changes
    type: timestamp           # Column type
  template-file: cache-populate.sql  # Population SQL
  retention:
    keep-last-snapshots: 10   # How many versions
    max-snapshot-age: "30d"   # When to delete old
```

**Cache Template Variables**:
```sql
{{cache.catalog}}                    # DuckDB catalog name
{{cache.schema}}                     # Schema name
{{cache.table}}                      # Table name
{{cache.previousSnapshotTimestamp}}  # Last refresh time
{{cache.currentSnapshotTimestamp}}   # This refresh time
```

**Management APIs**:
- `flapii cache list`: Show all caches
- `flapii cache get {endpoint}`: Get config
- `flapii cache refresh {endpoint}`: Force refresh
- `flapii cache gc`: Garbage collection

### 6. Validators and Input Validation

**Where Validation Happens**:
1. **Request arrives** → Extract parameters
2. **Validate each parameter** → Check type and constraints
3. **Fail fast** → 400 Bad Request if any validation fails
4. **Template expansion** → Only if all valid
5. **SQL execution** → Parameters already clean

**Validator Processing**:
```cpp
for each parameter {
    for each validator in parameter.validators {
        if !validator.check(parameter_value) {
            return HTTP 400 with error message
        }
    }
}
```

**Security Implications**:
- Validators are FIRST line of defense
- Triple braces are SECOND line
- Together: defense in depth
- Never trust user input even with validators

---

## Directory Structure

### Project Layout

```
flapi/
├── src/
│   └── include/
│       ├── api_server.hpp           # REST API handler
│       ├── mcp_server.hpp           # MCP protocol handler
│       ├── config_service.hpp       # Config management API
│       ├── request_handler.hpp      # Request processing
│       ├── template_engine.hpp      # Mustache rendering
│       ├── extended_yaml_parser.hpp # YAML with includes
│       ├── validators.hpp           # Input validators
│       ├── cache_manager.hpp        # DuckLake integration
│       └── ... other components
│
├── cli/
│   ├── src/
│   │   ├── commands/
│   │   │   ├── config/
│   │   │   │   ├── show.ts
│   │   │   │   ├── validate.ts
│   │   │   │   └── log-level.ts
│   │   │   ├── endpoints/
│   │   │   │   ├── list.ts
│   │   │   │   ├── get.ts
│   │   │   │   ├── create.ts
│   │   │   │   ├── update.ts
│   │   │   │   ├── delete.ts
│   │   │   │   ├── validate.ts
│   │   │   │   └── reload.ts
│   │   │   ├── templates/
│   │   │   │   ├── list.ts
│   │   │   │   ├── get.ts
│   │   │   │   ├── update.ts
│   │   │   │   ├── expand.ts
│   │   │   │   └── test.ts
│   │   │   ├── cache/
│   │   │   │   ├── list.ts
│   │   │   │   ├── get.ts
│   │   │   │   ├── update.ts
│   │   │   │   ├── refresh.ts
│   │   │   │   └── gc.ts
│   │   │   ├── schema/
│   │   │   │   ├── get.ts
│   │   │   │   ├── refresh.ts
│   │   │   │   └── connections.ts
│   │   │   ├── mcp/
│   │   │   │   ├── tools.ts
│   │   │   │   ├── resources.ts
│   │   │   │   └── prompts.ts
│   │   │   ├── ping.ts
│   │   │   ├── project/
│   │   │   │   └── init.ts       # Project scaffolding
│   │   │   └── endpoint/
│   │   │       └── create.ts     # Interactive endpoint wizard
│   │   ├── base.ts               # Command base class
│   │   └── index.ts              # Entry point
│   ├── test/
│   │   ├── unit/
│   │   └── integration/
│   ├── vscode-extension/
│   │   └── ... VSCode integration
│   ├── package.json
│   ├── tsconfig.json
│   └── README.md
│
├── test/
│   ├── integration/
│   │   ├── rest-endpoints/
│   │   │   └── *.tavern.yaml      # Tavern (REST) tests
│   │   ├── mcp-tools/
│   │   │   └── *.py               # pytest (MCP) tests
│   │   ├── ducklake/
│   │   │   └── *.py               # Cache tests
│   │   └── conftest.py
│   └── unit/
│       └── ... C++ unit tests
│
├── examples/
│   ├── simple-api/
│   ├── cached-api/
│   ├── mcp-tools/
│   └── ... example projects
│
├── flapi.yaml                    # Example main config
├── Makefile                      # Build and test commands
├── CMakeLists.txt               # C++ build config
├── Dockerfile                   # Docker image
├── docker-compose.yml           # Local development
├── .gitignore
├── README.md
└── CHANGELOG.md
```

### Key Directories Explained

**`src/include/`**: C++ header files for server implementation
- Not a .cpp files directory - headers contain implementation (modern C++)
- Each component is mostly self-contained
- No circular dependencies (DAG)

**`cli/src/commands/`**: Node.js/TypeScript CLI commands
- Organized by domain (config, endpoints, templates, cache, schema, mcp)
- Each command implements single responsibility
- Commands call REST APIs on running flapi server

**`test/integration/`**: End-to-end tests
- Tavern format for REST endpoint testing (YAML-based, human-readable)
- pytest for MCP testing (Python)
- Tests run against actual server

**`examples/`**: Sample projects
- Minimal working examples
- Different use cases (read, write, cache, MCP)
- Good starting point for users

---

## Key Components

### 1. API Server (`api_server.hpp`)

Handles HTTP requests and REST responses.

**Responsibilities**:
- Listen on port 8080 (configurable)
- Parse incoming HTTP requests
- Route to appropriate endpoint handler
- Format responses as JSON
- Handle CORS, auth, rate-limiting

**Flow**:
```
HTTP Request arrives
  ↓
Parse headers, body, query params
  ↓
Extract parameters per endpoint config
  ↓
Validate parameters (validators)
  ↓
Expand template with parameters
  ↓
Execute SQL against DuckDB
  ↓
Format results as JSON
  ↓
Return HTTP 200 + JSON body
```

**Key Methods**:
- `handleRequest()`: Main request handler
- `parseParameters()`: Extract from query/body/path/header
- `formatResponse()`: JSON serialization
- `handleError()`: Error response formatting

### 2. MCP Server (`mcp_server.hpp`)

Implements Model Context Protocol for AI agent integration.

**Responsibilities**:
- Listen on port 8081 (configurable) for MCP
- Implement JSON-RPC 2.0 protocol
- Register endpoints as MCP tools/resources/prompts
- Handle tool invocation requests
- Return structured results

**MCP Concepts**:
- **Tools**: Callable functions (endpoints with POST/GET)
- **Resources**: Queryable data (read-only endpoints)
- **Prompts**: Template text with variables (for AI context)

**Flow**:
```
AI Agent sends: {"method": "tools/call", "params": {"name": "customer_lookup", "params": {"id": 123}}}
  ↓
MCP Server finds tool definition
  ↓
Calls corresponding REST endpoint
  ↓
Formats response as MCP result
  ↓
Returns: {"result": {...result data...}}
```

**Key Methods**:
- `registerTools()`: Register endpoints as tools
- `handleToolCall()`: Execute tool invocation
- `formatMCPResponse()`: JSON-RPC response formatting

### 3. Template Engine (`template_engine.hpp`)

Renders Mustache templates to SQL.

**Responsibilities**:
- Load template from disk
- Parse Mustache syntax
- Build context (params, conn, cache, env)
- Render to final SQL
- Handle conditionals and iterations

**Template Processing**:
```
Input: "SELECT * FROM {{table}} WHERE id = {{{params.id}}}"
Context: {
  params: {id: 123},
  table: "customers",
  conn: {path: "./data/customers.parquet"}
}
  ↓
Parse and replace variables
  ↓
Output: "SELECT * FROM customers WHERE id = '123'"
```

**Key Methods**:
- `load()`: Load template from file
- `render()`: Render with context
- `parseConditional()`: Handle {{#field}}...{{/field}}
- `escapeString()`: Triple braces escaping

**Security Features**:
- Triple braces: Escape quotes and special chars
- Double braces: No quotes, numbers only
- Strict template syntax: Errors fail fast

### 4. YAML Configuration Parser (`extended_yaml_parser.hpp`)

Parses flapi.yaml and endpoint configs with extensions.

**Features**:
- Standard YAML parsing
- Include support: `{{include from file.yaml}}`
- Section includes: `{{include:section from file.yaml}}`
- Environment variable substitution: `${VAR}`
- Validation of config structure

**Parsing Pipeline**:
```
Read flapi.yaml
  ↓
Process includes (recursive)
  ↓
Substitute environment variables
  ↓
Validate against schema
  ↓
Return parsed config object
```

**Key Methods**:
- `parse()`: Parse YAML file
- `processIncludes()`: Handle {{include}} directives
- `substituteEnv()`: Replace ${VAR} with env values
- `validate()`: Check config against schema

### 5. Request Handler (`request_handler.hpp`)

Orchestrates request processing.

**Workflow**:
```
create_endpoint_interactive() runs
  ↓
Handler.extractParameters(request, endpoint_config)
  ↓
Handler.validateParameters(params, endpoint_config.validators)
  ↓
Handler.expandTemplate(template, params, connection)
  ↓
Handler.executeQuery(sql, connection)
  ↓
Handler.formatResponse(results)
  ↓
HTTP 200 + JSON
```

### 6. Cache Manager (`cache_manager.hpp`)

Manages DuckLake cache lifecycle.

**Responsibilities**:
- Initialize DuckLake (if enabled)
- Schedule cache refreshes
- Run cache population SQL
- Manage snapshots
- Handle garbage collection
- Support time-travel queries

**Cache Lifecycle**:
```
Endpoint configured with cache: true
  ↓
On server startup: Initialize cache table
  ↓
Schedule fires (e.g., every 6 hours)
  ↓
Load cache populate template
  ↓
Expand with cache variables
  ↓
Execute refresh SQL (MERGE or INSERT)
  ↓
Create snapshot v2
  ↓
Clean up old snapshots (retention)
```

---

## Data Structures and Schemas

### Endpoint Configuration Schema

```yaml
# Required
url-path: string              # e.g., "/customers"

# Optional, defaults
method?: "GET" | "POST" | "PUT" | "DELETE" | "PATCH"  # default: GET
description?: string          # For help/MCP docs

# Required if parameters expected
request?:
  - field-name: string
    field-in: "query" | "path" | "body" | "header"
    description: string
    required?: boolean         # default: false
    validators?:
      - type: "int" | "string" | "float" | "email" | "uuid" | "enum" | "date" | "time"
        # Type-specific fields
        min?: number           # int, float, date, time
        max?: number           # int, float, date, time
        min-length?: number    # string
        max-length?: number    # string
        pattern?: string       # string (regex)
        values?: array         # enum

# Required
template-source: string       # e.g., "customers.sql"
connection: [string]          # Array of connection names (usually 1)

# Optional: Write operations only
operation?:
  type: "write"
  returns-data?: boolean       # default: true
  transaction?: boolean        # default: true
  validate-before-write?: boolean  # default: true

# Optional: Authentication
auth?:
  type: "basic" | "jwt"
  users?:                      # For basic auth
    - username: string
      password: string
  token-key?: string           # For JWT
  issuer?: string              # For JWT

# Optional: Rate limiting
rate-limit?:
  enabled: boolean
  requests: number             # e.g., 100
  window: number               # seconds, e.g., 60

# Optional: Caching
cache?:
  enabled: boolean
  table: string
  schema: string               # default: "analytics"
  schedule: string             # e.g., "6h", "1d", "5m"
  primary-key?: [string]       # For merge mode
  cursor?:
    column: string
    type: "timestamp" | "date" | "integer"
  template_file: string        # e.g., "cache-populate.sql"
  retention?:
    keep_last_snapshots?: number
    max_snapshot_age?: string
```

### Main Configuration Schema (flapi.yaml)

```yaml
project-name: string
description?: string

# Data sources
connections:
  [connection-name]:
    properties:
      # File-based
      path?: string
      # Database
      host?: string
      port?: string | number
      database?: string
      user?: string
      password?: string
      # Cloud
      bucket?: string
      region?: string
      # Generic key-value

# Where to find endpoint configs
template-source: string        # e.g., "sqls" (directory)

# DuckDB settings
duckdb?:
  threads?: number
  memory_limit?: string
  database_path?: string

# DuckLake caching
ducklake?:
  enabled: boolean
  alias: string                # default: "cache"
  metadata_path: string
  data_path: string
  scheduler?:
    enabled: boolean
    scan-interval: string      # e.g., "5m"
  retention?:
    keep-last-snapshots: number
    max-snapshot-age: string

# Environment variable whitelisting
environment-whitelist?: [string]

# Global auth (applies to all endpoints)
auth?: ...

# Global rate limiting
rate-limit?: ...
```

### Request/Response Format

**Request Parameters** (vary by field-in):

Query: `GET /customers?id=123&name=John`
```json
{
  "id": "123",
  "name": "John"
}
```

Body: `POST /customers` with body
```json
{
  "name": "John",
  "email": "john@example.com"
}
```

Path: `GET /customers/123`
```json
{
  "id": "123"
}
```

Header: `GET /customers` with header
```json
{
  "X-Custom-Header": "value"
}
```

**Successful Response** (HTTP 200):

```json
{
  "data": [
    {
      "id": 1,
      "name": "John",
      "email": "john@example.com"
    }
  ]
}
```

**Error Response** (HTTP 400, 404, 500):

```json
{
  "error": "Invalid parameter: id - must be an integer"
}
```

---

## Configuration System

### Configuration Loading Order

1. **Default configuration** - Built-in defaults
2. **flapi.yaml** - User config (discovered in CWD or parent dirs)
3. **Environment variables** - Override config values
4. **CLI flags** - Highest priority (for server or CLI tools)

### Environment Variables

**For flapi server**:
- `FLAPI_PORT`: Server port (default 8080)
- `FLAPI_MCP_PORT`: MCP port (default 8081)
- `FLAPI_THREADS`: DuckDB threads
- `FLAPI_MEMORY`: DuckDB memory limit
- `FLAPI_LOG_LEVEL`: debug, info, warn, error (default: info)

**For CLI tool**:
- `FLAPI_BASE_URL`: Server URL (default: http://localhost:8080)
- `FLAPI_CONFIG`: Path to flapi.yaml
- `FLAPI_TOKEN`: Bearer token for auth

**Used in config**:
- `${VAR_NAME}` in connection properties
- Only whitelisted vars (from environment-whitelist)

### Configuration Validation

The `flapii config validate` command checks:
1. **YAML syntax** - Valid YAML structure
2. **Required fields** - url-path, template-source, connection
3. **Template files** - Referenced .sql files exist
4. **Connections** - Reference existing connections
5. **Validators** - Valid validator types and options
6. **Cross-references** - No broken links

---

## CLI Architecture

### CLI Layers

```
User Command Line
  ↓
CLI Entry Point (index.ts)
  ↓
Command Router (finds matching command)
  ↓
Command Implementation (e.g., endpoints/create.ts)
  ↓
API Client (calls REST API on running server)
  ↓
REST API Server (/api/v1/...)
  ↓
Core Engine (validation, template expansion, etc.)
  ↓
Database (DuckDB + DuckLake)
```

### Command Structure

Each command follows this pattern:

```typescript
// commands/endpoints/list.ts
import { Command, Flags } from '@oclif/core'

export default class EndpointsList extends Command {
  static description = 'List all endpoints'
  static flags = {
    output: Flags.enum({
      options: ['json', 'table'],
      default: 'table'
    })
  }

  async run(): Promise<void> {
    // 1. Parse flags
    const { flags } = await this.parse(EndpointsList)

    // 2. Make API call
    const response = await fetch(
      `${baseUrl}/api/v1/_config/endpoints`,
      { headers: { 'Authorization': `Bearer ${token}` } }
    )

    // 3. Format output
    if (flags.output === 'json') {
      console.log(JSON.stringify(await response.json(), null, 2))
    } else {
      // Table format with nice columns
    }
  }
}
```

### Key CLI Commands

```bash
# Configuration management
flapii config show              # Show effective config
flapii config validate          # Validate config files
flapii config log-level [get|set] [level]

# Endpoint management
flapii endpoints list           # List all endpoints
flapii endpoints get /path      # Get endpoint config
flapii endpoints create <file>  # Create from YAML file
flapii endpoints update /path <file>  # Update endpoint
flapii endpoints delete /path   # Delete endpoint
flapii endpoints validate <file>  # Validate endpoint YAML
flapii endpoints reload /path   # Hot-reload endpoint

# Template management
flapii templates list           # List templates
flapii templates get /path      # Get template content
flapii templates expand /path --params '{...}'  # Expand template
flapii templates test /path     # Test template syntax
flapii templates update /path <file>  # Update template

# Cache management
flapii cache list               # Show cache status
flapii cache get /path          # Get cache config
flapii cache update /path --schedule "10m"  # Update cache
flapii cache refresh /path      # Force refresh
flapii cache gc                 # Garbage collection

# Schema introspection
flapii schema get               # Get database schema
flapii schema connections       # List connections
flapii schema tables --connection db  # List tables
flapii schema refresh           # Refresh schema cache

# MCP management
flapii mcp tools list           # List MCP tools
flapii mcp tools get tool_name  # Get tool details
flapii mcp resources list       # List resources
flapii mcp prompts list         # List prompts

# Server management
flapii ping                     # Check server status
flapii project init             # Initialize new project
flapii endpoint create          # Interactive wizard
```

### REST API Endpoints

The server exposes management APIs:

```
GET /api/v1/_config/endpoints         # List all
GET /api/v1/_config/endpoints/{path}  # Get one
POST /api/v1/_config/endpoints        # Create
PUT /api/v1/_config/endpoints/{path}  # Update
DELETE /api/v1/_config/endpoints/{path}  # Delete

GET /api/v1/_config/endpoints/{path}/template
PUT /api/v1/_config/endpoints/{path}/template
POST /api/v1/_config/endpoints/{path}/template/expand

GET /api/v1/_config/endpoints/{path}/cache
PUT /api/v1/_config/endpoints/{path}/cache
POST /api/v1/_config/endpoints/{path}/cache/refresh
POST /api/v1/_config/endpoints/{path}/cache/gc

GET /api/v1/_schema               # Get schema
GET /api/v1/_schema/connections   # List connections
POST /api/v1/_schema/refresh      # Refresh schema

POST /api/v1/_ping                # Server health
```

---

## Server Implementation

### Server Lifecycle

```
1. Parse CLI args (port, config path, log level)
  ↓
2. Load flapi.yaml
  ↓
3. Process includes and env vars
  ↓
4. Validate configuration
  ↓
5. Initialize DuckDB
  ↓
6. Initialize DuckLake (if enabled)
  ↓
7. Load all endpoints from sqls/ directory
  ↓
8. Initialize extension handlers (auth, rate-limiting, caching)
  ↓
9. Start HTTP server (port 8080)
  ↓
10. Start MCP server (port 8081) if enabled
  ↓
11. Start cache scheduler (if enabled)
  ↓
12. Listen for requests
```

### Request Handling

For each incoming HTTP request:

```cpp
1. Parse HTTP method, path, headers
2. Route to matching endpoint by url-path
3. If no endpoint found: 404 Not Found

4. Extract parameters per endpoint config
   - Query params from URL
   - Path params from URL pattern
   - Body params from JSON body
   - Header params from headers

5. Apply authentication if configured
   - Basic auth: Check username/password
   - JWT: Verify token signature
   - Return 401 if auth fails

6. Check rate limiting
   - Increment counter for IP/user
   - Return 429 if limit exceeded

7. Validate parameters
   - For each parameter: run all validators
   - Return 400 if any fails

8. Build context for template
   - params: {field: value, ...}
   - conn: connection properties
   - cache: cache metadata (if enabled)
   - env: whitelisted environment variables

9. Expand template
   - Load template from disk
   - Render Mustache with context
   - Return SQL string

10. Execute SQL
    - If cache enabled and not modified endpoint:
      - Query cache first (faster)
    - Otherwise: Query underlying data sources
    - Stream results

11. Format response
    - Convert results to JSON
    - Return HTTP 200 + JSON body

12. Log request (method, path, params, duration, status)
```

### Error Handling

| Error | HTTP Code | Response |
|-------|-----------|----------|
| Invalid parameter (validation) | 400 | `{"error": "Invalid parameter: field - reason"}` |
| Missing required parameter | 400 | `{"error": "Missing required parameter: field"}` |
| Authentication failed | 401 | `{"error": "Unauthorized"}` |
| Rate limit exceeded | 429 | `{"error": "Rate limit exceeded"}` |
| Endpoint not found | 404 | `{"error": "Endpoint not found: /path"}` |
| Template expansion failed | 500 | `{"error": "Template error: ..."}` |
| SQL execution failed | 500 | `{"error": "Database error: ..."}` |
| Internal error | 500 | `{"error": "Internal server error"}` |

---

## SQL Template Engine

### Template Loading

```cpp
std::string load_template(const std::string& endpoint_name) {
    // Look for sqls/{endpoint_name}.sql
    fs::path template_path = config["template-source"] /
                            (endpoint_name + ".sql");

    if (!fs::exists(template_path)) {
        throw std::runtime_error("Template not found: " + template_path.string());
    }

    std::ifstream file(template_path);
    return std::string((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
}
```

### Context Building

```cpp
json context = {
    {"params", extracted_parameters},
    {"conn", connection_properties},
    {"cache", cache_metadata},  // if cache enabled
    {"env", whitelisted_env_vars}
};

// Example for GET /customers?id=123
// context = {
//   "params": {"id": "123"},
//   "conn": {"path": "./data/customers.parquet"},
//   "cache": {"table": "customers_cache", "schema": "analytics"},
//   "env": {"DB_PASSWORD": "***"}
// }
```

### Template Rendering

```cpp
std::string expand_template(
    const std::string& template_content,
    const json& context
) {
    // Use Mustache library (e.g., mstch)
    // Key rules:
    // - {{{ }}} for strings (auto-escape)
    // - {{ }} for numbers/identifiers
    // - {{#field}}...{{/field}} for conditionals
    // - {{^field}}...{{/field}} for negation

    mstch::mustache tmpl(template_content);
    std::string expanded = mstch::render(tmpl, context);

    return expanded;
}
```

### Example: Three Templates

**Simple Read**:
```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
{{#params.id}}
WHERE id = {{ params.id }}
{{/params.id}}
LIMIT 100
```

**Insert with Multiple Parameters**:
```sql
INSERT INTO {{ conn.table }} (name, email, status)
VALUES ('{{{ params.name }}}', '{{{ params.email }}}', '{{{ params.status }}}')
RETURNING *
```

**Dynamic Filtering**:
```sql
SELECT * FROM table
WHERE 1=1
{{#params.name}}
  AND name LIKE '%{{{ params.name }}}%'
{{/params.name}}
{{#params.min_price}}
  AND price >= {{ params.min_price }}
{{/params.min_price}}
{{#params.max_price}}
  AND price <= {{ params.max_price }}
{{/params.max_price}}
ORDER BY created_at DESC
LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}100{{/params.limit}}
```

---

## Caching System (DuckLake)

### DuckLake Overview

**DuckLake** is a table versioning system built on DuckDB.

**Key Features**:
- **Snapshots**: Point-in-time versions of tables
- **Time-travel**: Query any snapshot (e.g., data from 6 hours ago)
- **Incremental**: Only store changed rows (save space)
- **Merge mode**: Automatic insert/update/delete handling

### Cache Refresh Flow

```
Cache enabled: true, schedule: "6h"
  ↓
Server starts, initializes DuckLake catalog
  ↓
Schedule fires (every 6 hours)
  ↓
Load cache populate SQL
  ↓
Build context:
  - params: (empty, no user input)
  - conn: connection properties
  - cache: {
      table: "customers_cache",
      schema: "analytics",
      catalog: "cache",
      previousSnapshotTimestamp: "2025-01-15 00:00:00",
      currentSnapshotTimestamp: "2025-01-15 06:00:00"
    }
  ↓
Expand template (Mustache)
  ↓
Example expanded SQL (merge mode):
  MERGE INTO cache.analytics.customers_cache t
  USING (
    SELECT * FROM postgres_scan(...)
    WHERE updated_at > TIMESTAMP '2025-01-15 00:00:00'
  ) s
  ON t.id = s.id
  WHEN MATCHED THEN UPDATE SET *
  WHEN NOT MATCHED THEN INSERT *
  ↓
Execute SQL against DuckDB
  ↓
Create new snapshot v2 (differences only)
  ↓
Update previous snapshot timestamp to current
  ↓
Run garbage collection (delete old snapshots)
  ↓
Next refresh in 6 hours
```

### Cache Templates

**Full Refresh**:
```sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_parquet('{{{ conn.path }}}')
```

**Incremental Append**:
```sql
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT * FROM postgres_scan(...)
{{#cache.previousSnapshotTimestamp}}
WHERE created_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}'
{{/cache.previousSnapshotTimestamp}}
```

**Incremental Merge**:
```sql
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} t
USING (
  SELECT * FROM postgres_scan(...)
  {{#cache.previousSnapshotTimestamp}}
  WHERE updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}'
  {{/cache.previousSnapshotTimestamp}}
) s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET *
WHEN NOT MATCHED THEN INSERT *
```

### Cache Query

When endpoint has cache enabled:

```cpp
// Check if cache is fresh
if (cache_last_refresh < now - cache_max_age) {
    // Cache too old, query underlying source
    results = query_source();
} else {
    // Cache is fresh, query cache table
    results = query_cache_table();
}
```

---

## Validators and Input Validation

### Validator Implementation

```cpp
struct Validator {
    std::string type;           // "int", "string", etc.
    std::map<std::string, variant> options;  // min, max, pattern, etc.
};

bool validate_parameter(
    const std::string& value,
    const Validator& validator
) {
    if (validator.type == "int") {
        int int_val = std::stoi(value);
        if (validator.options.count("min")) {
            if (int_val < std::get<int>(validator.options["min"])) {
                return false;
            }
        }
        if (validator.options.count("max")) {
            if (int_val > std::get<int>(validator.options["max"])) {
                return false;
            }
        }
        return true;
    }
    else if (validator.type == "string") {
        if (validator.options.count("max-length")) {
            if (value.length() > std::get<int>(validator.options["max-length"])) {
                return false;
            }
        }
        if (validator.options.count("pattern")) {
            std::regex pattern(std::get<std::string>(validator.options["pattern"]));
            if (!std::regex_match(value, pattern)) {
                return false;
            }
        }
        return true;
    }
    // ... other types
}
```

### Security Implications

Validators are **first line of defense**:

```
Untrusted Input
  ↓
Validator checks
  ├─ Range: 1-999999 ✓
  ├─ Type: integer ✓
  ├─ Pattern: [a-zA-Z0-9]+ ✓
  ↓
Parameter passes validation
  ↓
Template expansion (triple braces escape)
  ↓
SQL: WHERE id = '123'  (safe)
```

---

## API Protocols

### REST API

**Design**:
- RESTful endpoints (GET, POST, PUT, DELETE)
- JSON request/response bodies
- Standard HTTP status codes
- Parameter extraction from query/path/body/header

**Endpoints**:
```
GET  /customers                      # List
GET  /customers?id=123               # Filter
GET  /customers/123                  # Get by path param
POST /customers -d '{"name":"John"}' # Create
PUT  /customers/123 -d '{...}        # Update
DELETE /customers/123                # Delete
```

**Status Codes**:
- 200: Success
- 400: Bad request (validation failed)
- 401: Unauthorized
- 404: Not found
- 429: Rate limited
- 500: Server error

### MCP (Model Context Protocol)

**Design**:
- JSON-RPC 2.0 over stdio or HTTP
- Tools: Callable functions (like REST endpoints)
- Resources: Queryable reference data
- Prompts: Template text for AI context

**Tool Schema**:
```json
{
  "name": "customer_lookup",
  "description": "Look up customer by ID",
  "inputSchema": {
    "type": "object",
    "properties": {
      "customer_id": {
        "type": "integer",
        "description": "Customer ID"
      }
    },
    "required": ["customer_id"]
  }
}
```

**Tool Invocation**:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "customer_lookup",
    "arguments": {
      "customer_id": 123
    }
  }
}
```

**Response**:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    {
      "type": "text",
      "text": "{"id": 123, "name": "John", "email": "john@example.com"}"
    }
  ]
}
```

---

## Build System

### Build Tools

- **C++ Compiler**: GCC/Clang (C++17+)
- **Build Generator**: CMake 3.15+
- **Package Manager**: Conan (C++ dependencies)
- **CLI Build**: npm/TypeScript
- **Testing**: pytest (Python) + Tavern (YAML)

### Build Commands

```bash
# Full build
make release        # Optimized binary
make debug          # Debug symbols
make test          # Build + run tests
make test-all      # All tests (unit + integration)

# Components
make cli-build     # Build CLI
make cli-test      # Test CLI

# Docker
make docker-build  # Build Docker image
make docker-test   # Run tests in container

# Development
make watch        # Watch files, rebuild on change
make serve        # Start development server
```

### Build Output

```
flapi/
└── build/
    ├── Release/bin/
    │   └── flapi              # Main binary
    ├── Release/lib/
    │   └── libflapi.a         # Static library
    └── cli/
        └── dist/
            └── index.js       # CLI JavaScript
```

### Key Dependencies

**C++**:
- DuckDB (database engine)
- nlohmann/json (JSON handling)
- mstch (Mustache templates)
- httplib (HTTP server)
- yaml-cpp (YAML parsing)

**Node.js/TypeScript**:
- @oclif/core (CLI framework)
- axios (HTTP client)
- chalk (colored output)
- inquirer (interactive prompts)

---

## Testing Infrastructure

### Test Organization

```
test/
├── integration/
│   ├── rest-endpoints/
│   │   ├── read-operations.tavern.yaml
│   │   ├── write-operations.tavern.yaml
│   │   ├── parameters.tavern.yaml
│   │   ├── validators.tavern.yaml
│   │   ├── caching.tavern.yaml
│   │   └── mcp-tools.tavern.yaml
│   ├── mcp-tools/
│   │   ├── test_mcp_tools.py
│   │   └── test_mcp_resources.py
│   ├── ducklake/
│   │   └── test_caching.py
│   ├── conftest.py           # Pytest fixtures
│   └── docker-compose.yml    # Test environment
│
└── unit/
    ├── validators/
    │   └── test_validators.cpp
    ├── template_engine/
    │   └── test_mustache.cpp
    └── config_parser/
        └── test_yaml.cpp
```

### Tavern Format (REST Testing)

```yaml
test_name: GET endpoint with optional parameter

stages:
  - name: Get all customers
    request:
      url: http://localhost:8080/customers
      method: GET
    response:
      status_code: 200
      json:
        data:
          - name: John

  - name: Filter by ID
    request:
      url: http://localhost:8080/customers?id=1
      method: GET
    response:
      status_code: 200
      json:
        data:
          - id: 1
            name: John

  - name: Validation failure
    request:
      url: http://localhost:8080/customers?id=invalid
      method: GET
    response:
      status_code: 400
      json:
        error: !re "Invalid parameter"
```

### pytest Format (Python Testing)

```python
import pytest
import requests

class TestEndpoints:
    @pytest.fixture
    def server_url(self):
        return "http://localhost:8080"

    def test_create_endpoint(self, server_url):
        # Create endpoint
        response = requests.post(
            f"{server_url}/api/v1/_config/endpoints",
            json={
                "url-path": "/test",
                "template-source": "test.sql",
                "connection": ["default"]
            }
        )
        assert response.status_code == 200

    def test_endpoint_validation(self, server_url):
        # Test parameter validation
        response = requests.get(
            f"{server_url}/test",
            params={"id": "invalid"}
        )
        assert response.status_code == 400
```

### Running Tests

```bash
# All tests
make test-all

# Specific test suite
make test-rest      # REST endpoints only
make test-mcp       # MCP tools only
make test-cache     # Caching tests

# Single test
pytest test/integration/rest-endpoints/validators.tavern.yaml

# With coverage
pytest --cov=src test/
```

---

## Development Workflows

### Adding a New Endpoint Type

1. **Define YAML config** (sqls/new-endpoint.yaml)
2. **Create SQL template** (sqls/new-endpoint.sql)
3. **Add validators** if needed
4. **Write integration test** (Tavern or pytest)
5. **Test via CLI**: `flapii endpoints list`, `curl http://localhost:8080/path`
6. **Test via MCP** (if applicable)

### Adding a New Validator

1. **Add type to validator enum** (src/include/validators.hpp)
2. **Implement validation logic** (src/validators.cpp)
3. **Add YAML schema** (documentation)
4. **Write unit tests** (test/unit/validators/)
5. **Write integration tests** (test/integration/)
6. **Update CLI help**

### Adding a New CLI Command

1. **Create command file** (cli/src/commands/group/command.ts)
2. **Extend Command class** (use @oclif/core)
3. **Implement run() method**
4. **Call server REST API**
5. **Format output (JSON/table)**
6. **Add tests** (cli/test/)
7. **Update CLI help**

### Performance Optimization Workflow

1. **Identify bottleneck** (via profiling/logs)
2. **Check query plan** (`EXPLAIN SELECT...`)
3. **Add index** (in source database)
4. **Enable caching** (if appropriate)
5. **Benchmark** before/after
6. **Document** decision

---

## Extension Points

### 1. Custom Validators

The validator system is extensible:

```cpp
// Implement custom validator
struct EmailValidator : public Validator {
    bool validate(const std::string& value) override {
        // Custom logic here
        return value.find('@') != std::string::npos;
    }
};

// Register in validator factory
registry.register("email", std::make_unique<EmailValidator>());
```

### 2. DuckDB Extensions

flapi can use any DuckDB extension:

```yaml
# In connection properties
connections:
  bigquery-data:
    properties:
      project_id: 'my-project'
      dataset: 'my_dataset'
```

Extensions are auto-loaded when needed (postgres_scanner, httpfs, bigquery, json, etc.)

### 3. Custom Authentication

The auth system supports different types:

```yaml
auth:
  type: jwt
  token-key: ${JWT_SECRET}
  issuer: "example.com"
```

Can be extended for OAuth, API keys, etc.

### 4. Custom Output Formatters

Can add new response formats beyond JSON:

```cpp
// Add CSV formatter
formatter_registry.register("csv", std::make_unique<CSVFormatter>());

// Use: ?format=csv
GET /customers?format=csv
```

### 5. Scheduled Tasks

Can extend scheduler for background jobs:

```yaml
# Beyond cache refreshes
scheduled_jobs:
  - name: daily-sync
    cron: "0 2 * * *"      # 2am daily
    action: refresh_all_caches
```

---

## Common Issues and Solutions

### Issue 1: Template Expansion Fails

**Symptom**: `400 Bad Request: Template error`

**Causes**:
- Mustache syntax error (unmatched braces)
- Variable not in context (typo in variable name)
- Invalid SQL generated

**Solution**:
```bash
# Test template expansion
flapii templates expand /endpoint --params '{"id":"123"}'

# Check syntax
flapii endpoints validate sqls/endpoint.yaml

# Check logs
flapii config log-level debug
./flapi -c flapi.yaml  # Look for error messages
```

### Issue 2: Validator Always Rejects Valid Input

**Symptom**: `400 Bad Request: Invalid parameter`

**Causes**:
- Wrong validator type
- Too strict constraints
- Type mismatch (string passed for int)

**Solution**:
```bash
# Remove/loosen validators
# Test with curl
curl "http://localhost:8080/endpoint?id=123"

# Check validator config
flapii endpoints get /endpoint | jq '.request[0].validators'
```

### Issue 3: Cache Not Refreshing

**Symptom**: Cache data is stale

**Causes**:
- Schedule not firing
- Template error in cache populate
- DuckLake not initialized

**Solution**:
```bash
# Force refresh
flapii cache refresh /endpoint

# Check config
flapii cache get /endpoint

# Check logs
flapii config log-level debug

# Validate cache template
cat sqls/cache-populate.sql
```

### Issue 4: Memory Usage High

**Symptom**: DuckDB using lots of memory

**Causes**:
- Large query result set
- No LIMIT clause
- Cache accumulating snapshots

**Solution**:
```yaml
# In flapi.yaml
duckdb:
  memory_limit: "4GB"

# In endpoint config
request:
  - field-name: limit
    validators:
      - type: int
        max: 10000
```

```bash
# Trigger garbage collection
flapii cache gc

# Check retention
flapii cache get /endpoint | jq '.retention'
```

### Issue 5: Connection Fails

**Symptom**: `500 Internal Server Error` or connection refused

**Causes**:
- Wrong connection credentials
- Database not accessible
- Network issues

**Solution**:
```bash
# Check connection config
flapii schema connections

# Test connectivity
flapii schema refresh --connection connection-name

# Check environment variables
echo $DB_PASSWORD  # Should not be empty

# Check flapi.yaml
cat flapi.yaml | grep -A 10 "connections:"
```

---

## Contribution Guidelines

### Code Style

- **C++**: Use clang-format (config in repo)
- **TypeScript**: Use ESLint (config in repo)
- **Commit messages**: Conventional commits (feat:, fix:, docs:)

### Testing Requirements

- Unit tests for all new functions
- Integration tests for all new endpoints
- No regressions in existing tests

### Documentation

- Update CHANGELOG.md
- Add examples if new feature
- Update CLI help text
- Document breaking changes

### Pull Request Process

1. Fork repository
2. Create feature branch: `git checkout -b feat/description`
3. Make changes + tests
4. Pass all tests: `make test-all`
5. Submit PR with description
6. Address review feedback
7. Merge when approved

---

## Performance Characteristics

### Request Latency (typical)

| Operation | Time |
|-----------|------|
| Parameter validation | 1-5ms |
| Template expansion | 2-10ms |
| Simple SELECT (cached) | 5-50ms |
| Complex query (not cached) | 100-1000ms |
| Network roundtrip | 1-10ms |
| **Total (simple GET)** | **10-70ms** |

### Memory Usage

- Base server: ~50MB
- Per connection: ~5-20MB
- Cache table (1M rows): ~100-500MB
- DuckDB buffer pool: Configurable, default 1GB

### Scalability

- Concurrent connections: 1000+
- Endpoints: 1000+
- Parameters per endpoint: No limit (practical ~20)
- Request rate: 1000+ req/sec (single-threaded DuckDB)

---

## Future Roadmap

**Planned Features**:
- Streaming responses (Server-Sent Events)
- GraphQL protocol support
- Real-time subscriptions (WebSocket)
- More sophisticated caching (TTL, conditional)
- Built-in metrics/monitoring
- Multi-tenancy support
- Advanced auth (OAuth, SAML)
- SQL query optimization hints

**Under Consideration**:
- Direct file upload handling
- Custom SQL functions
- Data transformation pipelines
- API versioning
- Request/response middleware

---

## References and Resources

### Official Documentation
- GitHub: https://github.com/DataZooDE/flapi
- DuckDB: https://duckdb.org/docs/
- MCP Protocol: https://modelcontextprotocol.io/

### Key Articles
- Mustache: https://mustache.github.io/
- YAML: https://yaml.org/
- REST API best practices: https://restfulapi.net/

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2025-11-10 | Initial agent guide documentation |

---

## Conclusion

flapi is a well-architected system that removes backend coding from the critical path of data exposure. Key strengths:

1. **Clean separation of concerns**: Config, SQL, validation, caching, auth are all independent
2. **Security first**: Multiple validation layers, SQL injection prevention
3. **Performance-optimized**: Caching built-in, streaming support
4. **Developer-friendly**: CLI, clear error messages, good defaults
5. **Extensible**: DuckDB ecosystem, custom validators, modular design

For developers contributing to flapi:
- Understand the request pipeline
- Test changes with integration tests
- Keep validator/auth/cache systems decoupled
- Document configuration changes
- Profile performance-sensitive code paths

This guide should give you a solid understanding of flapi's internals and how to extend it effectively.
