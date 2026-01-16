# Config Service REST API Structure

## Overview

The Config Service provides a REST API for managing Flapi configurations at runtime. All endpoints require token-based authentication.

## Base URL

```
http://localhost:8080/api/v1/_config
```

## Authentication

All endpoints (except `/doc.yaml`) require a Bearer token:

```bash
Authorization: Bearer <token>
```

Token is configured via:
- CLI: `--config-service-token <token>`
- ENV: `FLAPI_CONFIG_SERVICE_TOKEN=<token>`
- Auto-generated: 32 alphanumeric characters if not provided

## Centralized Slug Convention

**Critical:** All `{slug}` parameters must follow the centralized slugging logic:

### REST Endpoints
- Path → Slug conversion using `PathUtils::pathToSlug()`
- Examples:
  - `/customers/` → `customers-slash`
  - `/api/v1/data/` → `api-v1-data-slash`
  - `/sap/functions` → `sap-functions`

### MCP Entities (Tools, Resources, Prompts)
- Use the entity name as-is (no conversion)
- Examples:
  - `customer_lookup` → `customer_lookup`
  - `get_sales_data` → `get_sales_data`
  - `list_products` → `list_products`

## API Endpoints

### 1. Endpoint Management

#### List All Endpoints
```
GET /api/v1/_config/endpoints
```

**Response:**
```json
{
  "endpoints": [
    {
      "url-path": "/customers/",
      "method": "GET",
      "config-file-path": "/path/to/customers-rest.yaml",
      "template-source": "/path/to/customers.sql"
    },
    {
      "mcp-tool": {
        "name": "customer_lookup",
        "description": "Look up customer data"
      },
      "template-source": "/path/to/customers.sql"
    }
  ]
}
```

#### Create Endpoint
```
POST /api/v1/_config/endpoints
Content-Type: application/json

{
  "url-path": "/new-endpoint/",
  "method": "POST",
  "template-source": "/path/to/template.sql"
}
```

#### Get Endpoint by Slug
```
GET /api/v1/_config/endpoints/{slug}
```

**Examples:**
```bash
# REST endpoint
GET /api/v1/_config/endpoints/customers-slash

# MCP tool
GET /api/v1/_config/endpoints/customer_lookup
```

**Response:**
```json
{
  "url-path": "/customers/",
  "method": "GET",
  "config-file-path": "/path/to/customers-rest.yaml",
  "template-source": "/path/to/customers.sql",
  "request": [
    {
      "field-name": "id",
      "in": "query",
      "required": false,
      "default": "1"
    }
  ],
  "cache": {
    "enabled": true,
    "ttl": "5m"
  }
}
```

#### Update Endpoint by Slug
```
PUT /api/v1/_config/endpoints/{slug}
Content-Type: application/json

{
  "url-path": "/customers/",
  "method": "GET",
  "template-source": "/path/to/customers.sql"
}
```

#### Delete Endpoint by Slug
```
DELETE /api/v1/_config/endpoints/{slug}
```

### 2. Endpoint Operations

#### Validate Endpoint Config
```
POST /api/v1/_config/endpoints/{slug}/validate
Content-Type: text/plain

<YAML content to validate>
```

**Response:**
```json
{
  "valid": true,
  "errors": [],
  "warnings": ["Template file not found"]
}
```

#### Reload Endpoint from Disk
```
POST /api/v1/_config/endpoints/{slug}/reload
```

**Response:**
```json
{
  "success": true,
  "message": "Endpoint reloaded successfully"
}
```

#### Get Endpoint Parameters
```
GET /api/v1/_config/endpoints/{slug}/parameters
```

**Response:**
```json
{
  "parameters": [
    {
      "name": "id",
      "in": "query",
      "required": false,
      "default": "1",
      "description": "Customer ID"
    }
  ]
}
```

#### Test Endpoint with Parameters
```
POST /api/v1/_config/endpoints/{slug}/test
Content-Type: application/json

{
  "params": {
    "id": "123",
    "limit": "10"
  }
}
```

**Response:**
```json
{
  "sql": "SELECT * FROM customers WHERE id = ? LIMIT ?",
  "results": [
    {"id": 123, "name": "John Doe"}
  ],
  "row_count": 1,
  "execution_time_ms": 15
}
```

#### Get SQL Template
```
GET /api/v1/_config/endpoints/{slug}/template
```

**Response:**
```
SELECT * FROM customers
WHERE id = {{params.id}}
{{#params.limit}}
LIMIT {{params.limit}}
{{/params.limit}}
```

#### Update SQL Template
```
PUT /api/v1/_config/endpoints/{slug}/template
Content-Type: text/plain

SELECT * FROM customers WHERE id = {{params.id}}
```

#### Get Cache Configuration
```
GET /api/v1/_config/endpoints/{slug}/cache
```

**Response:**
```json
{
  "enabled": true,
  "ttl": "5m",
  "template-file": "/path/to/cache-key.sql"
}
```

#### Update Cache Configuration
```
PUT /api/v1/_config/endpoints/{slug}/cache
Content-Type: application/json

{
  "enabled": true,
  "ttl": "10m"
}
```

### 3. Template Lookup

#### Find Endpoints by SQL Template
```
POST /api/v1/_config/endpoints/by-template
Content-Type: application/json

{
  "template_path": "/path/to/customers.sql"
}
```

**Response:**
```json
{
  "endpoints": [
    {
      "url_path": "/customers/",
      "method": "GET",
      "config_file_path": "/path/to/customers-rest.yaml",
      "template_source": "/path/to/customers.sql",
      "type": "REST"
    },
    {
      "mcp_name": "customer_lookup",
      "config_file_path": "/path/to/customers-mcp-tool.yaml",
      "template_source": "/path/to/customers.sql",
      "type": "MCP"
    }
  ]
}
```

### 4. Environment Variables

#### Get Environment Variables
```
GET /api/v1/_config/environment-variables
```

**Response:**
```json
{
  "variables": [
    {
      "name": "DB_HOST",
      "value": "localhost"
    },
    {
      "name": "API_KEY",
      "value": "***" // Sensitive values masked
    }
  ]
}
```

### 5. Database Schema

#### Get Database Schema
```
GET /api/v1/_config/schema
```

**Response:**
```json
{
  "tables": [
    {
      "name": "customers",
      "columns": [
        {
          "name": "id",
          "type": "INTEGER",
          "nullable": false,
          "primary_key": true
        },
        {
          "name": "name",
          "type": "TEXT",
          "nullable": false
        }
      ]
    }
  ]
}
```

### 6. Filesystem

#### Get Filesystem Structure
```
GET /api/v1/_config/filesystem
```

**Response:**
```json
{
  "root": "/path/to/project",
  "files": [
    {
      "path": "/path/to/customers.sql",
      "type": "template",
      "size": 1234
    },
    {
      "path": "/path/to/customers-rest.yaml",
      "type": "config",
      "size": 567
    }
  ]
}
```

### 7. Log Level Control

#### Get Current Log Level
```
GET /api/v1/_config/log-level
```

**Response:**
```json
{
  "level": "info"
}
```

#### Set Log Level
```
PUT /api/v1/_config/log-level
Content-Type: application/json

{
  "level": "debug"
}
```

**Valid levels:** `debug`, `info`, `warning`, `error`, `critical`

**Response:**
```json
{
  "level": "debug",
  "message": "Log level updated successfully"
}
```

### 8. OpenAPI Documentation

#### Get OpenAPI Spec (Public)
```
GET /api/v1/doc.yaml
```

**Response:** YAML document with OpenAPI 3.0 specification

**Note:** This is the ONLY endpoint that does NOT require authentication.

## Error Responses

All endpoints follow consistent error format:

### 400 Bad Request
```json
{
  "error": "Invalid JSON",
  "message": "Missing required field: url-path"
}
```

### 401 Unauthorized
```
Unauthorized: Invalid or missing token
```

### 404 Not Found
```
Endpoint not found
```

### 500 Internal Server Error
```json
{
  "error": "Internal server error",
  "message": "Database connection failed"
}
```

## Client Usage

### TypeScript/JavaScript (VSCode Extension, CLI)

```typescript
import { FlapiApiClient } from '@flapi/shared';

const client = new FlapiApiClient({
  baseURL: 'http://localhost:8080',
  token: 'your-token-here'
});

// Get endpoint config (works for both REST and MCP)
import { pathToSlug } from '@flapi/shared';

// For REST endpoint
const restSlug = pathToSlug('/customers/'); // 'customers-slash'
const restConfig = await client.get(`/api/v1/_config/endpoints/${restSlug}`);

// For MCP tool
const mcpSlug = 'customer_lookup'; // Use as-is
const mcpConfig = await client.get(`/api/v1/_config/endpoints/${mcpSlug}`);
```

### C++ (Backend)

```cpp
// Finding endpoint by slug
const auto* endpoint = findEndpointBySlug(config_manager, slug);

// Getting slug from endpoint
std::string slug = endpoint->getSlug();
// For REST: pathToSlug(urlPath)
// For MCP: mcp_tool->name (as-is)
```

## Workflow Examples

### 1. VSCode Extension: SQL Template Testing

```typescript
// 1. Find endpoints using this template
const response = await client.post('/api/v1/_config/endpoints/by-template', {
  template_path: '/path/to/customers.sql'
});

// 2. Load full configs for each endpoint
for (const ep of response.data.endpoints) {
  let slug: string;
  if (ep.type === 'REST') {
    slug = pathToSlug(ep.url_path);
  } else {
    slug = ep.mcp_name;
  }
  
  const config = await client.get(`/api/v1/_config/endpoints/${slug}`);
  
  // 3. Get default parameters
  const params = await client.get(`/api/v1/_config/endpoints/${slug}/parameters`);
  
  // 4. Test with parameters
  const result = await client.post(`/api/v1/_config/endpoints/${slug}/test`, {
    params: { id: '123' }
  });
}
```

### 2. CLI: Set Log Level

```bash
flapi config log-level set debug \
  --config-service-token $FLAPI_CONFIG_SERVICE_TOKEN
```

### 3. VSCode Extension: Validate on Save

```typescript
// On save, validate YAML
const yamlContent = await workspace.fs.readFile(document.uri);
const slug = pathToSlug(extractUrlPathFromYaml(yamlContent));

const response = await client.post(
  `/api/v1/_config/endpoints/${slug}/validate`,
  yamlContent.toString()
);

if (!response.data.valid) {
  // Show diagnostics
}

// After save, reload
await client.post(`/api/v1/_config/endpoints/${slug}/reload`);
```

## Design Principles

1. **Centralized Slug Logic** - All slug generation happens via `EndpointConfig::getSlug()` in backend
2. **Type Agnostic** - Same API structure for REST, MCP Tool, MCP Resource, MCP Prompt
3. **Consistent Errors** - All endpoints return structured errors
4. **Token Security** - All endpoints (except docs) require authentication
5. **Real-time Updates** - Reload endpoints dynamically without restart
6. **Developer Friendly** - Consistent naming, clear responses, OpenAPI docs

## Migration Notes

### From Path-Based to Slug-Based

**Old (Deprecated):**
```cpp
const auto* endpoint = config_manager->getEndpointForPath(path);
```

**New (Recommended):**
```cpp
const auto* endpoint = findEndpointBySlug(config_manager, slug);
```

### Frontend Slug Generation

**Old (Wrong):**
```typescript
const slug = urlPath.replace(/\//g, '_').replace(/^_/, '').replace(/_$/, '');
```

**New (Correct):**
```typescript
import { pathToSlug } from '@flapi/shared';
const slug = pathToSlug(urlPath); // For REST
const slug = mcpName; // For MCP (as-is)
```

## Summary

The Config Service API provides a **unified, slug-based interface** for managing all endpoint types (REST and MCP). By using centralized slug logic, all clients (VSCode extension, CLI, backend) can reliably discover, configure, test, and manage endpoints at runtime.

**Key Takeaway:** Always use `pathToSlug()` for REST endpoints and use MCP names as-is!

