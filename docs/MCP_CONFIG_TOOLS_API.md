# MCP Configuration Tools API Reference

Complete reference for all 20 MCP configuration management tools in flAPI. These tools enable AI agents to programmatically manage REST API endpoints, SQL templates, and cache configurations at runtime.

## Overview

The Configuration Service exposes 20 MCP tools organized into 4 functional categories:

- **Discovery Tools (5)**: Read-only introspection of project configuration
- **Template Tools (4)**: SQL template management and testing
- **Endpoint Tools (6)**: CRUD operations for REST endpoints
- **Cache Tools (4)**: Cache status and management operations

All tools use JSON-RPC 2.0 protocol with MCP error codes for standardized error handling.

---

## Error Codes

All tools use standard MCP error codes:

| Code | Name | Meaning |
|------|------|---------|
| -32601 | Method Not Found | Tool or handler not found |
| -32602 | Invalid Params | Missing or invalid parameters |
| -32603 | Internal Error | Server error during execution |
| -32001 | Authentication Required | Auth token missing or invalid |

---

## Phase 1: Discovery Tools

Read-only tools for introspecting project configuration and database schema.

### flapi_get_project_config

Get project metadata and configuration information.

**Authentication:** Not required

**Parameters:** None

**Response (Success):**
```json
{
  "project_name": "my-api-project",
  "project_description": "REST API for customer data",
  "base_path": "/home/user/projects/my-api",
  "version": "1.0.0"
}
```

**Error Response:**
```json
{
  "error": "Configuration service unavailable"
}
```

---

### flapi_get_environment

Get whitelisted environment variables available to templates.

**Authentication:** Not required

**Parameters:** None

**Response (Success):**
```json
{
  "variables": [
    {
      "name": "DB_HOST",
      "value": "localhost"
    },
    {
      "name": "DB_PORT",
      "value": "5432"
    }
  ]
}
```

---

### flapi_get_filesystem

Get directory structure of template and configuration files.

**Authentication:** Not required

**Parameters:** None

**Response (Success):**
```json
{
  "base_path": "/home/user/projects/my-api",
  "template_path": "/home/user/projects/my-api/sqls",
  "tree": [
    {
      "name": "customers.sql",
      "type": "file",
      "size": 1024
    },
    {
      "name": "orders.sql",
      "type": "file",
      "size": 2048
    }
  ]
}
```

---

### flapi_get_schema

Get database schema information for all configured connections.

**Authentication:** Not required

**Parameters:** None

**Response (Success):**
```json
{
  "tables": [
    {
      "name": "customers",
      "schema": "public",
      "columns": [
        {
          "name": "id",
          "type": "INTEGER",
          "nullable": false
        },
        {
          "name": "name",
          "type": "VARCHAR",
          "nullable": false
        }
      ]
    }
  ]
}
```

---

### flapi_refresh_schema

Refresh database schema cache by querying all connections.

**Authentication:** Not required

**Parameters:** None

**Response (Success):**
```json
{
  "status": "schema_refreshed",
  "timestamp": "1704067200",
  "message": "Database schema cache has been refreshed"
}
```

---

## Phase 2: Template Tools

Tools for managing and testing SQL templates with Mustache syntax.

### flapi_get_template

Retrieve the SQL template content for an endpoint.

**Authentication:** Not required

**Parameters:**
- `endpoint` (string, required): Endpoint identifier or filename (e.g., "customers")

**Response (Success):**
```json
{
  "endpoint": "customers",
  "content": "SELECT * FROM customers\nWHERE 1=1\n{{#params.id}}\n  AND id = {{{ params.id }}}\n{{/params.id}}",
  "message": "Template retrieved"
}
```

**Error Response (Endpoint not found):**
```json
{
  "error": "Endpoint not found",
  "endpoint": "customers",
  "hint": "Use flapi_list_endpoints to see available endpoints or flapi_create_endpoint to create new one"
}
```

---

### flapi_update_template

Update the SQL template content for an endpoint (requires authentication).

**Authentication:** Required

**Parameters:**
- `endpoint` (string, required): Endpoint identifier
- `content` (string, required): New template SQL content with Mustache syntax

**Response (Success):**
```json
{
  "endpoint": "customers",
  "message": "Template updated successfully",
  "content_length": 256
}
```

**Error Response (Endpoint not found):**
```json
{
  "error": "Endpoint not found",
  "endpoint": "customers",
  "hint": "Use flapi_list_endpoints to see available endpoints"
}
```

---

### flapi_expand_template

Test Mustache template expansion with sample parameters.

**Authentication:** Not required

**Parameters:**
- `endpoint` (string, required): Endpoint identifier
- `params` (object, optional): Sample parameters for template expansion

**Response (Success):**
```json
{
  "endpoint": "customers",
  "expanded_sql": "SELECT * FROM customers\nWHERE 1=1\n  AND id = 123",
  "status": "Template expanded successfully"
}
```

---

### flapi_test_template

Validate template syntax and Mustache syntax.

**Authentication:** Not required

**Parameters:**
- `endpoint` (string, required): Endpoint identifier

**Response (Success):**
```json
{
  "endpoint": "customers",
  "valid": true,
  "message": "Template syntax is valid"
}
```

**Error Response (Invalid syntax):**
```json
{
  "error": "Template syntax error",
  "endpoint": "customers",
  "reason": "Unclosed Mustache block at line 3"
}
```

---

## Phase 3: Endpoint Tools

CRUD operations for REST endpoint configuration (path, method, template, cache).

### flapi_list_endpoints

List all configured REST and MCP endpoints.

**Authentication:** Not required

**Parameters:** None

**Response (Success):**
```json
{
  "count": 2,
  "endpoints": [
    {
      "name": "GetCustomers",
      "path": "customers",
      "method": "GET",
      "type": "rest"
    },
    {
      "name": "ListOrders",
      "path": "orders",
      "method": "GET",
      "type": "rest"
    }
  ]
}
```

---

### flapi_get_endpoint

Get complete configuration for a specific endpoint.

**Authentication:** Not required

**Parameters:**
- `path` (string, required): Endpoint path

**Response (Success):**
```json
{
  "name": "GetCustomers",
  "path": "customers",
  "method": "GET",
  "template_source": "customers.sql",
  "connections": ["db-primary"],
  "auth_required": false,
  "cache_enabled": true
}
```

**Error Response (Endpoint not found):**
```json
{
  "error": "Endpoint not found",
  "path": "customers",
  "hint": "Use flapi_list_endpoints to see available endpoints"
}
```

---

### flapi_create_endpoint

Create a new REST endpoint (requires authentication).

**Authentication:** Required

**Parameters:**
- `path` (string, required): Endpoint URL path (e.g., "customers")
- `method` (string, optional): HTTP method - GET, POST, PUT, DELETE (defaults to GET)
- `template_source` (string, optional): SQL template filename

**Response (Success):**
```json
{
  "status": "success",
  "path": "customers",
  "method": "GET",
  "template_source": "customers.sql",
  "message": "Endpoint created successfully"
}
```

**Error Response (Endpoint exists):**
```json
{
  "error": "Endpoint already exists",
  "path": "customers",
  "hint": "Use flapi_update_endpoint to modify existing endpoint, or delete it first with flapi_delete_endpoint"
}
```

---

### flapi_update_endpoint

Modify an existing endpoint configuration (requires authentication).

**Authentication:** Required

**Parameters:**
- `path` (string, required): Endpoint path to update
- `method` (string, optional): New HTTP method
- `template_source` (string, optional): New template filename

**Response (Success):**
```json
{
  "status": "success",
  "path": "customers",
  "message": "Endpoint updated successfully",
  "previous_method": "GET",
  "new_method": "POST",
  "previous_template": "customers.sql",
  "new_template": "customers_create.sql"
}
```

---

### flapi_delete_endpoint

Remove an endpoint from configuration (requires authentication).

**Authentication:** Required

**Parameters:**
- `path` (string, required): Endpoint path to delete

**Response (Success):**
```json
{
  "status": "success",
  "path": "customers",
  "message": "Endpoint deleted successfully",
  "deleted_method": "GET",
  "deleted_template": "customers.sql"
}
```

---

### flapi_reload_endpoint

Hot-reload endpoint configuration from disk without restarting server (requires authentication).

**Authentication:** Required

**Parameters:**
- `path` (string, required): Endpoint path to reload

**Response (Success):**
```json
{
  "status": "success",
  "path": "customers",
  "message": "Endpoint configuration reloaded from disk",
  "original_method": "GET",
  "original_template": "customers.sql"
}
```

**Error Response (Load fails):**
```json
{
  "error": "Failed to reload endpoint configuration from disk",
  "path": "customers",
  "hint": "Verify that endpoint YAML file exists and is valid"
}
```

---

## Phase 4: Cache Tools

Monitor and manage DuckLake cache for endpoints.

### flapi_get_cache_status

Get cache status and configuration for an endpoint.

**Authentication:** Not required

**Parameters:**
- `path` (string, required): Endpoint path

**Response (Success):**
```json
{
  "status": "success",
  "path": "customers",
  "cache_enabled": true,
  "cache_table": "customers_cache",
  "cache_schema": "cache_v1"
}
```

**Error Response (Cache not enabled):**
```json
{
  "error": "Cache not enabled for this endpoint",
  "path": "customers",
  "method": "GET",
  "hint": "Enable cache in endpoint YAML configuration and reload endpoint"
}
```

---

### flapi_refresh_cache

Trigger manual cache refresh for an endpoint (requires authentication).

**Authentication:** Required

**Parameters:**
- `path` (string, required): Endpoint path

**Response (Success):**
```json
{
  "path": "customers",
  "status": "Cache refresh triggered",
  "cache_table": "customers_cache",
  "timestamp": "1704067200",
  "message": "Cache refresh has been scheduled"
}
```

---

### flapi_get_cache_audit

Get audit log of cache operations and refresh history.

**Authentication:** Not required

**Parameters:**
- `path` (string, required): Endpoint path

**Response (Success):**
```json
{
  "path": "customers",
  "cache_table": "customers_cache",
  "audit_log": [
    {
      "timestamp": "1704067200",
      "event": "cache_refreshed",
      "status": "success"
    },
    {
      "timestamp": "1704063600",
      "event": "cache_status_checked",
      "status": "success"
    }
  ],
  "message": "Cache audit log retrieved successfully"
}
```

---

### flapi_run_cache_gc

Trigger garbage collection to clean up old cache snapshots (requires authentication).

**Authentication:** Required

**Parameters:**
- `path` (string, optional): Endpoint path for targeted GC (if omitted, runs globally)

**Response (Success - Endpoint-specific):**
```json
{
  "status": "Garbage collection triggered",
  "path": "customers",
  "timestamp": "1704067200",
  "message": "Cache garbage collection for endpoint scheduled"
}
```

**Response (Success - Global):**
```json
{
  "status": "Garbage collection triggered",
  "scope": "all_caches",
  "timestamp": "1704067200",
  "message": "Global cache garbage collection has been scheduled"
}
```

---

## Authentication

### Bearer Token Format

Tools requiring authentication expect a Bearer token:

```
Authorization: Bearer <token>
```

### API Key Format

Alternative authentication formats supported:

```
Authorization: Basic <base64-encoded-credentials>
Authorization: Token <api-key>
Authorization: API-Key <key>
```

### Token Validation

Tokens are validated for:
- Non-empty value (minimum 8 characters)
- Valid Base64 characters (for Bearer/Basic schemes)
- Scheme recognition (Bearer, Basic, Token, API-Key)
- Length bounds (maximum 4096 characters)

Invalid tokens return:
```json
{
  "error": "Authentication validation failed: <specific reason>"
}
```

---

## Error Handling Guidelines

### Structured Error Responses

All errors include:
- `error`: Description of what failed
- `path`/`method`/`template`: Resource details
- `reason`: Underlying exception (if applicable)
- `hint`: Actionable guidance for resolution

### Example Error Flow

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "call_tool",
  "params": {
    "name": "flapi_get_endpoint",
    "arguments": {
      "path": "nonexistent"
    }
  }
}
```

**Response (Error):**
```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32603,
    "message": "{\"error\":\"Endpoint not found\",\"path\":\"nonexistent\",\"hint\":\"Use flapi_list_endpoints to see available endpoints\"}"
  }
}
```

---

## Rate Limiting

Configuration tools do not have built-in rate limiting but respect server-level rate limits.

---

## Concurrency Considerations

- Read operations (list, get, read) are safe for concurrent access
- Write operations (create, update, delete, reload) lock the endpoint configuration
- Cache operations are atomic with respect to the cache table
- Multiple concurrent writes to the same endpoint may see conflicts

---

## Best Practices

1. **Always list endpoints first**: Use `flapi_list_endpoints` to verify endpoint exists before operating on it

2. **Validate templates before updating**: Use `flapi_test_template` to validate syntax before `flapi_update_template`

3. **Check cache status before refresh**: Use `flapi_get_cache_status` to verify cache is enabled before refreshing

4. **Handle structured errors**: Parse error responses for `hint` field to guide users

5. **Authenticate with correct scheme**: Use `Bearer` for JWT tokens, `Basic` for credentials, `Token` for API keys

6. **Reload after configuration changes**: Use `flapi_reload_endpoint` to apply changes without server restart

---

## Common Workflows

### Create a New Endpoint

1. Call `flapi_list_endpoints` to see existing endpoints
2. Call `flapi_create_endpoint` with path, method, template_source
3. Call `flapi_get_endpoint` to verify creation
4. Call `flapi_test_template` to validate template

### Update Endpoint Configuration

1. Call `flapi_get_endpoint` to see current config
2. Call `flapi_update_endpoint` with new values
3. Call `flapi_reload_endpoint` to apply changes
4. Verify with `flapi_get_endpoint`

### Manage Cache

1. Call `flapi_get_cache_status` to check if cache is enabled
2. Call `flapi_refresh_cache` to manually refresh if needed
3. Call `flapi_get_cache_audit` to review refresh history
4. Call `flapi_run_cache_gc` periodically to clean up snapshots

---

## API Version

These tools implement MCP Configuration Service v1.0 for flAPI.

For integration guidance, see [MCP Configuration Integration Guide](./MCP_CONFIG_INTEGRATION.md).
