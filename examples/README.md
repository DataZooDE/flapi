# flAPI Examples

This directory contains working examples that demonstrate flAPI's capabilities. The examples are organized as a progressive tutorial - start with the basics and work your way up to advanced patterns.

## Quick Start

```bash
# From the project root
./build/release/flapi --config examples/flapi.yaml

# Server starts at http://localhost:8080
# MCP endpoint at http://localhost:8080/mcp/jsonrpc
```

## Directory Structure

```
examples/
├── flapi.yaml              # Main configuration (connections, DuckDB settings, MCP config)
├── data/                   # Data files used by the examples
│   ├── customers.parquet   # TPC-H customer data (1500 rows)
│   └── northwind.sqlite    # Northwind sample database
└── sqls/                   # Endpoint configurations organized by topic
    ├── customers/          # Tutorial 2-3: Config reuse, validators, MCP
    ├── northwind/          # Tutorial 1: Basic REST CRUD
    ├── publicis/           # Tutorial 4: BigQuery + caching
    ├── sap/                # Tutorial 5: Enterprise integration
    ├── taxi/               # Advanced: AWS Secrets Manager (inactive)
    └── recommendations/    # Advanced: Composite keys (inactive)
```

---

## Tutorial 1: Your First REST API (Northwind)

**Goal:** Understand basic endpoint configuration and SQL templates.

**Files to examine:**
- `sqls/northwind/products-list.yaml` - Endpoint configuration
- `sqls/northwind/products.sql` - SQL template

### Understanding the Config Structure

Open `sqls/northwind/products-list.yaml`:

```yaml
url-path: /northwind/products/     # The REST endpoint path
method: GET                         # HTTP method (default: GET)

request:                            # Input parameters
  - field-name: product_name
    field-in: query                 # Where to find it: query, path, body, header
    required: false
    validators:
      - type: string
        min: 1
        max: 100

template-source: products.sql       # SQL file to execute
connection:
  - northwind-sqlite               # Which data source to use
```

### The SQL Template

Open `sqls/northwind/products.sql`:

```sql
SELECT ProductID, ProductName, UnitPrice, UnitsInStock
FROM nw.Products
WHERE 1=1
{{#params.product_name}}
    AND ProductName LIKE '%{{{ params.product_name }}}%'
{{/params.product_name}}
ORDER BY ProductName
```

**Key concepts:**
- `WHERE 1=1` allows optional filters to be appended with `AND`
- `{{#params.field}}...{{/params.field}}` - Mustache conditional (only renders if parameter exists)
- `{{{ }}}` triple braces - String values (auto-escaped for SQL injection protection)
- `{{ }}` double braces - Numeric values (no quotes)

### Try It Yourself

```bash
# List all products
curl http://localhost:8080/northwind/products/

# Filter by name
curl "http://localhost:8080/northwind/products/?product_name=chai"

# Get single product
curl http://localhost:8080/northwind/products/1

# Create a product
curl -X POST http://localhost:8080/northwind/products/ \
  -H "Content-Type: application/json" \
  -d '{"product_name": "Test Product", "supplier_id": 1, "category_id": 1}'

# Update a product
curl -X PUT http://localhost:8080/northwind/products/78 \
  -H "Content-Type: application/json" \
  -d '{"product_name": "Updated Name", "unit_price": "29.99"}'

# Delete a product
curl -X DELETE http://localhost:8080/northwind/products/78
```

**Key Takeaways:**
- Endpoint config = YAML file defining URL, parameters, and SQL reference
- SQL templates use Mustache syntax for dynamic query building
- Full CRUD operations with a few config files

See [sqls/northwind/README.md](sqls/northwind/README.md) for detailed Northwind documentation.

---

## Tutorial 2: Configuration Reuse & Validation (Customers)

**Goal:** Learn how to reuse configuration across endpoints and validate input.

**Files to examine:**
- `sqls/customers/customer-common.yaml` - Shared configuration
- `sqls/customers/customers-rest.yaml` - REST endpoint using includes

### YAML Includes Pattern

Instead of duplicating configuration, define it once and include it:

**customer-common.yaml** (shared config):
```yaml
# Request parameters - reusable across endpoints
request:
  - field-name: id
    field-in: query
    validators:
      - type: int
        min: 1
        max: 1000000

  - field-name: segment
    field-in: query
    validators:
      - type: enum
        allowedValues: [AUTOMOBILE, BUILDING, FURNITURE, HOUSEHOLD, MACHINERY]

# Authentication config
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: secret
      roles: [admin, read, write]
```

**customers-rest.yaml** (uses the shared config):
```yaml
url-path: /customers/

# Include sections from common file
{{include:request from customer-common.yaml}}
{{include:auth from customer-common.yaml}}
{{include:connection from customer-common.yaml}}
{{include:template-source from customer-common.yaml}}
```

### Input Validators

flAPI validates all input before executing SQL:

| Validator | Options | Example |
|-----------|---------|---------|
| `int` | min, max | `type: int, min: 1, max: 1000` |
| `string` | min, max, regex | `type: string, regex: "^[A-Za-z]+$"` |
| `enum` | allowedValues | `type: enum, allowedValues: [a, b, c]` |
| `email` | - | `type: email` |
| `uuid` | - | `type: uuid` |
| `date` | min, max | `type: date, min: "2020-01-01"` |

### Try It Yourself

```bash
# Without auth - returns 401
curl http://localhost:8080/customers/

# With basic auth
curl -u admin:secret http://localhost:8080/customers/

# Filter by segment (validates against enum)
curl -u admin:secret "http://localhost:8080/customers/?segment=AUTOMOBILE"

# Invalid segment - returns 400
curl -u admin:secret "http://localhost:8080/customers/?segment=invalid"
```

**Key Takeaways:**
- `{{include:section from file.yaml}}` eliminates config duplication
- Validators protect against invalid input and SQL injection
- Auth can be configured per-endpoint or shared via includes

---

## Tutorial 3: MCP Integration (AI Tools)

**Goal:** Expose your data as AI-compatible tools using the Model Context Protocol.

**Files to examine:**
- `sqls/customers/customers-mcp-tool.yaml` - MCP tool definition
- `sqls/customers/customers-mcp-resource.yaml` - MCP resource definition
- `sqls/customers/customers-mcp-prompt.yaml` - MCP prompt definition

### MCP Tools

Tools are callable functions that AI models can invoke:

```yaml
mcp-tool:
  name: customer_lookup
  description: Retrieve customer information by ID, segment, or other criteria
  result-mime-type: application/json

# Same request/connection/template as REST
{{include:request from customer-common.yaml}}
{{include:connection from customer-common.yaml}}
{{include:template-source from customer-common.yaml}}
```

### MCP Resources

Resources provide read-only data like schemas or documentation:

```yaml
mcp-resource:
  name: customer_schema
  description: Customer database schema definition
  mime-type: application/json

template-source: customer-mcp-resource-schema.sql
{{include:connection from customer-common.yaml}}
```

### MCP Prompts

Prompts are templates that guide AI behavior:

```yaml
mcp-prompt:
  name: customer_data_analysis
  description: Generate comprehensive customer analysis
  template: |
    You are a customer data analyst. Analyze the customer data.

    {{#customer_id}}
    **Customer ID**: {{customer_id}}
    {{/customer_id}}

    Use the `customer_lookup` MCP tool to retrieve data.
  arguments:
    - customer_id
    - segment
```

### Testing MCP

The MCP endpoint is available at `http://localhost:8080/mcp/jsonrpc`

```bash
# List available tools
curl -X POST http://localhost:8080/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc": "2.0", "id": 1, "method": "tools/list"}'

# Call a tool
curl -X POST http://localhost:8080/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/call",
    "params": {
      "name": "customer_lookup",
      "arguments": {"segment": "AUTOMOBILE"}
    }
  }'
```

**Key Takeaways:**
- Same endpoint can be both REST and MCP tool
- MCP tools make your data accessible to AI assistants
- Resources provide metadata, prompts guide AI behavior

---

## Tutorial 4: Caching with DuckLake

**Goal:** Understand how to cache query results for performance.

**Files to examine:**
- `sqls/publicis/publicis.yaml` - Endpoint with cache config
- `sqls/publicis/publicis_cache.sql` - Cache population template
- `flapi.yaml` - DuckLake configuration

### Cache Configuration

```yaml
url-path: /publicis
template-source: publicis.sql
connection:
  - bigquery-lakehouse

cache:
  enabled: true
  table: publicis_cache           # Where to store cached data
  schema: analytics
  schedule: 60m                   # Refresh every 60 minutes
  retention:
    keep-last-snapshots: 5        # Keep 5 historical versions
    max-snapshot-age: 14d         # Delete snapshots older than 14 days
  template-file: publicis_cache.sql  # Custom cache population query
```

### How DuckLake Caching Works

1. **First request**: Executes the SQL, stores result in cache table
2. **Subsequent requests**: Serves from cache (fast)
3. **On schedule**: Background refresh updates the cache
4. **Time-travel**: Query historical snapshots

### DuckLake in flapi.yaml

```yaml
ducklake:
  enabled: true
  alias: cache                    # Access as cache.schema.table
  metadata-path: ./data/cache.ducklake
  data-path: ./data/cache
  retention:
    keep-last-snapshots: 10
    max-snapshot-age: 30d
  scheduler:
    enabled: true
    scan-interval: 5m             # Check for refresh every 5 minutes
```

**Note:** The publicis example requires BigQuery access. The pattern works the same with any data source.

**Key Takeaways:**
- Cache stores query results locally for fast serving
- Scheduled refresh keeps data fresh
- DuckLake provides versioning and time-travel

---

## Tutorial 5: Enterprise Integration (SAP)

**Goal:** Connect to enterprise systems using DuckDB extensions.

**Files to examine:**
- `sqls/sap/flights-rest-read-table.yaml` - SAP endpoint
- `flapi.yaml` - SAP connection configuration

### Connection Init Scripts

Complex data sources need initialization. In `flapi.yaml`:

```yaml
connections:
  sap-abap-trial:
    init: |
      INSTALL 'erpl' FROM 'http://get.erpl.io';
      LOAD 'erpl';

      CREATE OR REPLACE PERSISTENT SECRET abap_trial (
        TYPE sap_rfc,
        ASHOST 'localhost',
        SYSNR '00',
        CLIENT '001',
        USER 'DEVELOPER',
        PASSWD 'password',
        LANG 'EN'
      );
```

### The SAP Endpoint

```yaml
url-path: /sap/flights
template-source: flights.sql
connection: [sap-abap-trial]

{{include:request from flights-common.yaml}}
```

The SQL template uses SAP-specific functions:

```sql
SELECT * FROM sap_read_table('SFLIGHT') AS f
```

**Note:** This example requires a running SAP ABAP trial system.

**Key Takeaways:**
- Connection `init` scripts run SQL to set up extensions and credentials
- DuckDB extensions enable access to 50+ data sources
- Same endpoint patterns work regardless of data source

---

## Advanced Patterns (Inactive Examples)

These examples demonstrate advanced patterns but are marked inactive (`.yaml.inactive`) because they require specific infrastructure.

### AWS Secrets Manager (`sqls/taxi/`)

Demonstrates fetching credentials from AWS Secrets Manager:

```yaml
auth:
  enabled: true
  type: basic
  from-aws-secretmanager:
    secret-name: flapi_endpoint_users
    secret-table: taxi_auth
```

### Composite Primary Keys (`sqls/recommendations/`)

Demonstrates caching with composite keys for incremental refresh:

```yaml
cache:
  primary-key: [material_number, country_code]  # Composite key
  schedule: 10m
```

To activate these examples, rename the file (remove `.inactive`) and ensure the required infrastructure is available.

---

## Configuration Reference

### Main Config (`flapi.yaml`)

| Section | Purpose |
|---------|---------|
| `connections` | Define data sources (databases, files, APIs) |
| `duckdb` | DuckDB engine settings (memory, threads) |
| `ducklake` | Cache configuration and scheduling |
| `mcp` | MCP server settings |
| `template.path` | Where to find endpoint configs |

### Endpoint Config

| Field | Purpose |
|-------|---------|
| `url-path` | REST API path |
| `method` | HTTP method (GET, POST, PUT, DELETE) |
| `request` | Input parameters with validators |
| `template-source` | SQL file to execute |
| `connection` | Data source(s) to use |
| `cache` | Caching configuration |
| `auth` | Authentication settings |
| `mcp-tool` | Expose as MCP tool |
| `mcp-resource` | Expose as MCP resource |

---

## Next Steps

1. **Create your own endpoint**: Copy a northwind example, modify the SQL
2. **Add caching**: Add a `cache` section to improve performance
3. **Enable MCP**: Add `mcp-tool` to make your data AI-accessible
4. **Read the docs**: See the main project documentation for complete reference

## Troubleshooting

**401 Unauthorized**: The endpoint requires authentication. Use `-u admin:secret` or check the auth config.

**400 Bad Request**: Input validation failed. Check the validator constraints.

**500 Internal Server Error**: Check the server logs for SQL errors. Common issues:
- Missing connection
- Invalid SQL syntax
- Template variable typo

**Server won't start**: Check `flapi.yaml` syntax. Use `--log-level debug` for more info.
