# flAPI Configuration UI API Documentation

## API Response Formats

### Project Configuration

#### GET /api/v1/_config/project
Returns the project configuration.

#### PUT /api/v1/_config/project
Updates the project configuration.

### Endpoint Configuration

#### GET /api/v1/_config/endpoints
Lists all endpoints.

#### POST /api/v1/_config/endpoints
Creates a new endpoint.

#### GET /api/v1/_config/endpoints/{path}
Returns the configuration for a specific endpoint.

#### PUT /api/v1/_config/endpoints/{path}
Updates an endpoint's configuration.

#### DELETE /api/v1/_config/endpoints/{path}
Deletes an endpoint.

### Template Management

#### GET /api/v1/_config/endpoints/{path}/template
Returns the SQL template for an endpoint.

Response format:
```json
{
  "template": "SELECT * FROM table WHERE id = {{id}}"
}
```

#### PUT /api/v1/_config/endpoints/{path}/template
Updates an endpoint's SQL template.

Request format: Same as GET response format.

#### POST /api/v1/_config/endpoints/{path}/template/expand
Tests template expansion with provided parameters.

### Cache Management

#### GET /api/v1/_config/endpoints/{path}/cache
Returns the cache configuration for an endpoint.

Response format:
```json
{
  "enabled": true,
  "refresh-time": "1h",
  "cache-table": "cache_table",
  "cache-source": "path/to/cache.sql"
}
```

#### PUT /api/v1/_config/endpoints/{path}/cache
Updates an endpoint's cache configuration.

Request format: Same as GET response format.

#### GET /api/v1/_config/endpoints/{path}/cache/template
Returns the cache SQL template.

#### PUT /api/v1/_config/endpoints/{path}/cache/template
Updates the cache SQL template.

#### POST /api/v1/_config/endpoints/{path}/cache/refresh
Manually triggers a cache refresh for the endpoint.

### Schema Management

#### GET /api/v1/_config/schema
Returns the database schema information.

Response format:
```json
{
  "schema_name": {
    "tables": {
      "table_name": {
        "is_view": false,
        "columns": {
          "column_name": {
            "type": "INTEGER",
            "nullable": true
          }
        }
      }
    }
  }
}
```

#### POST /api/v1/_config/schema/refresh
Refreshes the schema information.

## Error Codes and Meanings

| Code | Meaning | Example Scenarios |
|------|---------|------------------|
| 200 | Success | Operation completed successfully |
| 400 | Bad Request | Invalid JSON, missing required fields |
| 404 | Not Found | Endpoint or template not found |
| 405 | Method Not Allowed | Invalid HTTP method for route |
| 500 | Internal Server Error | Database error, file system error |

Common error response format:
```json
{
  "message": "Error description"
}
```

## Template Expansion Syntax

### Variable Substitution
- Basic syntax: `{{variable_name}}`
- Optional variables: `{{variable_name?}}`
- Default values: `{{variable_name:default}}`

### Special Variables
- `{{now}}` - Current timestamp
- `{{today}}` - Current date
- `{{user}}` - Current user

### SQL Injection Prevention
All variables are automatically escaped to prevent SQL injection. Use the following rules:
1. String values are properly quoted
2. Numeric values are validated
3. Special characters are escaped

## Cache Configuration Options

### Refresh Time Format
- Format: `<number><unit>`
- Units: `s` (seconds), `m` (minutes), `h` (hours), `d` (days)
- Examples: `30s`, `15m`, `1h`, `7d`

### Cache Table Configuration
- Table name must be unique per endpoint
- Schema is automatically created if it doesn't exist
- Tables are automatically created with appropriate columns

### Cache Refresh Strategies
1. **Time-based**: Refresh at specified intervals
2. **Manual**: Using the refresh endpoint
3. **On-demand**: First request after expiration

### Cache Template Variables
Special variables available in cache templates:
- `{{cache_table}}` - Name of the cache table
- `{{last_refresh}}` - Timestamp of last refresh 