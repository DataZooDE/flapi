# flapi MCP Server Usage Guide

This server provides SQL-based data access through MCP tools. Each tool executes a predefined SQL query with customizable parameters.

## Available Tool Types

- **Query Tools**: Read-only data retrieval (GET-style endpoints)
- **Mutation Tools**: Data modification (POST/PUT/PATCH/DELETE endpoints)
- **Cached Tools**: Pre-computed results for fast access

## Recommended Workflows

### Data Retrieval Pattern

1. Use search/filter tools to narrow down datasets
2. Apply pagination for large result sets (use `limit` and `offset` parameters)
3. Cache-enabled tools provide faster response times (check tool descriptions)

### Data Modification Pattern

1. Validate required parameters before calling mutation tools
2. Use transaction-aware tools for multi-step operations
3. Check response for success/error status

## Performance Tips

- **Pagination**: Always use `limit` parameter for potentially large datasets (default: 100 rows)
- **Caching**: Tools marked as "cached" return pre-computed results (check `cache_ttl` in descriptions)
- **Filtering**: Combine multiple filters in a single query rather than sequential calls
- **Batch Operations**: Use batch-enabled tools when available for bulk updates

## Constraints and Limits

- **Maximum Result Set**: 10,000 rows per query
- **Request Timeout**: 30 seconds per query
- **Rate Limiting**: 100 requests per minute per client
- **Parameter Limits**: String parameters limited to 4KB

## Data Access Patterns

### REST-Style Endpoints

- `GET /resource` → Query tool (list/search)
- `GET /resource/{id}` → Query tool (single item)
- `POST /resource` → Create tool
- `PUT /resource/{id}` → Update tool
- `DELETE /resource/{id}` → Delete tool

### MCP Resources

Resources provide read-only access to configuration, schema, and metadata. Use resources to:
- Discover available database tables and columns
- Check server configuration and capabilities
- Access API documentation

## Error Handling

- 400 errors: Parameter validation failure (check required parameters)
- 404 errors: Resource not found (verify IDs and endpoints)
- 500 errors: Server error (check logs for SQL errors or database issues)

## Authentication

If authentication is enabled, include credentials via MCP client authentication mechanisms. See server configuration for supported auth types (JWT, Basic, OIDC).
