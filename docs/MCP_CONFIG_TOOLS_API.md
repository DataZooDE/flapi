# MCP Configuration Tools API Reference

Complete reference for all 20 MCP configuration management tools in flAPI. These tools enable AI agents to programmatically manage REST API endpoints, SQL templates, and cache configurations at runtime.

## Overview

The Configuration Service exposes 20 MCP tools organized into 5 functional categories:

- **Discovery Tools (5)**: Read-only introspection of project configuration
- **Template Tools (4)**: SQL template management and testing
- **Endpoint Tools (6)**: CRUD operations for REST endpoints
- **Cache Tools (4)**: Cache status and management operations
- **Metrics Tool (1)**: `flapi_get_metrics`, runtime request and cache metrics

All tools use JSON-RPC 2.0 protocol with MCP error codes for standardized error handling.

### Activation Requirement

⚠️ **Important:** The MCP configuration tools (`flapi_*`) are **only available when the server is started with the `--config-service` flag**. Without this flag, only declarative MCP tools from YAML configurations are accessible.

To enable config MCP tools:
```bash
./flapi --config-service
./flapi --config-service --config-service-token "your-token"  # With authentication
```

When `--config-service` is NOT enabled:
- Config tools (`flapi_get_*`, `flapi_create_*`, etc.) will return "Tool not found" errors
- Only endpoint-based MCP tools from your YAML configurations are available
- REST configuration API (`/api/v1/_config/*`) is also disabled

**Error Codes:** See [MCP Reference § Appendix B](./MCP_REFERENCE.md#appendix-b-error-reference) for complete MCP error reference.

**Authentication:** every `flapi_*` tool, read or write, needs the config-service token
(`--config-service-token` / `FLAPI_CONFIG_SERVICE_TOKEN`), sent as `X-Config-Token: <token>` (read
first) or `Authorization: Bearer <token>`. Any non-empty token works as the configured value; startup
warns below 16 characters. Without it a call fails with `Authentication required for tool: <name>`;
with a wrong one, `Authentication validation failed: Invalid authentication token`. Endpoint-backed
MCP tools use `mcp.auth` in `flapi.yaml` instead.

Endpoint changes made with these tools (create, update, delete, reload) rebuild `tools/list`,
`resources/list` and `prompts/list` immediately; no restart and no notification.

---

## Discovery Tools

Read-only tools for introspecting project configuration and database schema.

### flapi_get_project_config

Get project metadata and configuration information.

**Authentication:** Required (config-service token)

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

**Authentication:** Required (config-service token)

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

**Authentication:** Required (config-service token)

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

**Authentication:** Required (config-service token)

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

**Authentication:** Required (config-service token)

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

## Template Tools

Tools for managing and testing SQL templates with Mustache syntax.

### flapi_get_template

Retrieve the SQL template content for an endpoint.

**Authentication:** Required (config-service token)

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

**Authentication:** Required (config-service token)

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

**Authentication:** Required (config-service token)

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

## Endpoint Tools

CRUD operations for REST endpoint configuration (path, method, template, cache).

### flapi_list_endpoints

List all configured REST and MCP endpoints.

**Authentication:** Required (config-service token)

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

**Authentication:** Required (config-service token)

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

Modify an existing endpoint configuration (requires authentication). The change goes through the same
merge-and-persist path as `PUT /api/v1/_config/endpoints/{slug}`: it is deep-merged onto the
endpoint's YAML file, re-parsed, written and swapped in, so it survives a reload and a restart.

**Authentication:** Required

**Parameters:**
- `path` (string, required): Endpoint path to update
- `config` (object, optional): endpoint keys to change, in the YAML spelling, e.g.
  `{"template-source": "customers_v2.sql", "cache": {"schedule": "10m"}}`
- any other top-level key is merged the same way (`template-source`, `connection`, ...);
  `template_source` is accepted for `template-source`

Changing `url-path`, `method` or the name is refused (delete and re-create); an endpoint built from
`{{include}}` files is refused too.

**Response (Success):**
```json
{
  "status": "success",
  "path": "/customers",
  "message": "Endpoint updated successfully",
  "file": "/srv/api/sqls/customers.yaml",
  "persisted": true,
  "method": "GET",
  "template_source": "/srv/api/sqls/customers_v2.sql"
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

## Cache Tools

Monitor and manage DuckLake cache for endpoints.

### flapi_get_cache_status

Get cache status and configuration for an endpoint.

**Authentication:** Required (config-service token)

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

The refresh runs before the call returns. There is no full-rebuild argument; use
`POST /api/v1/_config/endpoints/{slug}/cache/refresh?full=true` or `flapii cache refresh <path> --full`.

**Response (Success):**
```json
{"tool": "flapi_refresh_cache", "status": "ok"}
```

Read the outcome with `flapi_get_cache_audit` or `flapi_get_metrics`.

---

### flapi_get_cache_audit

Read an endpoint's cache sync log from `<alias>.audit.sync_events`, newest first
(at most 100 rows). Requires authentication.

**Parameters:**
- `path` (string, required): endpoint path, e.g. `/customers`

**Response (Success):** the audit rows, as returned by
`GET /api/v1/_config/endpoints/{path}/cache/audit`:
```json
[
  {
    "event_id": "evt_1763114400_1804289383",
    "endpoint_path": "/customers",
    "cache_table": "customers_cache",
    "cache_schema": "analytics",
    "sync_type": "merge",
    "status": "success",
    "message": "Cache refreshed successfully",
    "snapshot_id": "118",
    "rows_affected": 37,
    "sync_started_at": "2026-11-14 10:00:00.000",
    "sync_completed_at": "2026-11-14 10:00:00.812",
    "duration_ms": 812
  }
]
```

`sync_type` is `full`, `append`, `merge` or `garbage_collection`. `duration_ms`
times the populate statement (or the snapshot expiry for GC). `rows_affected` is
DuckDB's row count for the last statement of the cache template, or `null` when
that statement reports none. `snapshot_id` is the DuckLake snapshot the refresh
produced. Times are UTC.

---

### flapi_run_cache_gc

Expire one endpoint's old cache snapshots according to its own `retention`
policy. Requires authentication.

**Parameters:**
- `path` (string, **required**): endpoint path. There is no all-endpoints form;
  call it once per cached endpoint.

**Response (Success):** the handler's JSON status. A GC run is also recorded in
the audit log as `sync_type: garbage_collection`.

---

## Metrics Tool

### flapi_get_metrics

Runtime metrics since server start: per-endpoint and per-MCP-tool request counts,
error counts and latency, 401/403/429 rejections, and per-cache readiness, last
refresh mode/status/duration/rows and data age. Requires authentication.

**Parameters:**
- `endpoint` (string, optional): an endpoint path or MCP tool name to narrow
  `requests.series` and `caches` to

**Response (Success):** the same document as
[`GET /api/v1/_config/metrics`](CONFIG_SERVICE_API_REFERENCE.md#get-apiv1_configmetrics).

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

5. **Send the config-service token**: `X-Config-Token: <token>` or `Authorization: Bearer <token>` on every call

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

---

## Related Documentation

- **[Configuration Reference](./CONFIG_REFERENCE.md)** - Complete configuration file format and options
- **[MCP Reference](./MCP_REFERENCE.md)** - MCP protocol details and error codes
- **[MCP Configuration Integration Guide](./MCP_CONFIG_INTEGRATION.md)** - Integration architecture and flows
- **[Config Service API Reference](./CONFIG_SERVICE_API_REFERENCE.md)** - REST API and CLI client
- **[Documentation index](./README.md)** - Start here; organised by what you are trying to do
