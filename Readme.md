# flAPI: Instant SQL based APIs

flAPI is a powerful service that automatically generates read-only APIs for datasets by utilizing SQL templates. Built on top of [DuckDB](https://duckdb.org/) and leveraging its SQL engine and extension ecosystem, flAPI offers a seamless way to connect to various data sources and expose them as RESTful APIs.

![overview of flAPI](https://i.imgur.com/m7UVZlR.png)

## ⚡ Features

- **Automatic API Generation**: Create APIs for your datasets without coding
- **MCP (Model Context Protocol) Support**: Declarative creation of AI tools alongside REST endpoints
- **Multiple Data Sources**: Connect to [BigQuery](https://github.com/hafenkran/duckdb-bigquery), SAP ERP & BW (via [ERPL](https://github.com/datazoode/erpl)), Parquet, [Iceberg](https://github.com/duckdb/duckdb_iceberg), [Postgres](https://github.com/duckdb/postgres_scanner), [MySQL](https://github.com/duckdb/duckdb_mysql), and more
- **SQL Templates**: Use Mustache-like syntax for dynamic queries
- **Caching**: DuckLake-backed cache with full refresh and incremental sync
- **Security**: Implement row-level and column-level security with ease
- **Easy deployment**: Deploy flAPI with a single binary file

## 🛠 Quick Start
The easiest way to get started with flAPI is to use the pre-built docker image.

#### 1. Pull the docker image from the Github Container Registry:

```bash
> docker pull ghcr.io/datazoode/flapi:latest
```


The image is pretty small and mainly contains the flAPI binary which is statically linked against [DuckDB v1.4.3](https://github.com/duckdb/duckdb/releases/tag/v1.4.3). Details about the docker image can be found in the [Dockerfile](./docker/development/Dockerfile).

#### 2. Run flAPI:
Once you have downloaded the binary, you can run flAPI by executing the following command:

```
> docker run -it --rm -p 8080:8080 -p 8081:8081 -v $(pwd)/examples/:/config ghcr.io/datazoode/flapi -c /config/flapi.yaml
```

The different arguments in this docker command are:
- `-it --rm`: Run the container in interactive mode and remove it after the process has finished
- `-p 8080:8080`: Exposes port 8080 of the container to the host, this makes the REST API available at `http://localhost:8080`
- `-p 8081:8081`: Exposes port 8081 for the MCP server (when enabled)
- `-v $(pwd)/examples/:/config`: This mounts the local `examples` directory to the `/config` directory in the container, this is where the flAPI configuration file
is expected to be found.
- `ghcr.io/datazoode/flapi`: The docker image to use
- `-c /config/flapi.yaml`: This is an argument to the flAPI application which tells it to use the `flapi.yaml` file in the `/config` directory as the configuration file.

#### 2.1 Enable MCP Support:
To enable MCP support, you can either:

**Option A: Use the command line flag**
```
> docker run -it --rm -p 8080:8080 -p 8081:8081 -v $(pwd)/examples/:/config ghcr.io/datazoode/flapi -c /config/flapi.yaml --enable-mcp
```

**Option B: Configure in flapi.yaml**
```yaml
mcp:
  enabled: true
  port: 8081
  # ... other MCP configuration
```

#### 3.1 Test the API server:
If everything is set up correctly, you should be able to access the API at the URL specified in the configuration file.

```bash
> curl 'http://localhost:8080/'


         ___
     ___( o)>   Welcome to
     \ <_. )    flAPI
      `---'    

    Fast and Flexible API Framework
    powered by DuckDB
```

#### 3.2 Get an overview of the available endpoints:
The flAPI server creates embedded Swagger UI at which provides an overview of the available endpoints and allows you to test them. It can be found at

[`> http://localhost:8080/doc`](http://localhost:8080/doc)

You should see the familiar Swagger UI page:

![flAPI Swagger UI](https://i.imgur.com/HqjHMlA.png)

The raw yaml [Swagger 2.0](https://swagger.io/specification/) is also available at [`http://localhost:8080/doc.yaml`](http://localhost:8080/doc.yaml)

#### 3.3 Test the MCP server:
If MCP is enabled, you can test the MCP server as well:

```bash
# Check MCP server health
> curl 'http://localhost:8081/mcp/health'

{"status":"healthy","server":"flapi-mcp-server","version":"0.3.0","protocol_version":"2024-11-05","tools_count":0}

# Initialize MCP connection
> curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc": "2.0", "id": 1, "method": "initialize"}'

# List available tools
> curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc": "2.0", "id": 2, "method": "tools/list"}'
```

## 🤖 MCP (Model Context Protocol) Support

flAPI now supports the **Model Context Protocol (MCP)** in a **unified configuration approach**. Every flAPI instance automatically runs both a REST API server and an MCP server concurrently, allowing you to create AI tools alongside your REST endpoints using the same configuration files and SQL templates.

### Key Features

- **Unified Configuration**: Single YAML files can define REST endpoints, MCP tools, and MCP resources
- **Automatic Detection**: Configuration type is determined by presence of `url-path` (REST), `mcp-tool` (MCP tool), or `mcp-resource` (MCP resource)
- **Shared Components**: MCP tools and resources use the same SQL templates, parameter validation, authentication, and caching as REST endpoints
- **Concurrent Servers**: REST API (port 8080) and MCP server (port 8081) run simultaneously
- **Declarative Definition**: Define everything using YAML configuration with SQL templatestocol
- **Tool Discovery**: Automatic tool discovery and schema generation
- **Security Integration**: Reuse existing authentication, rate limiting, and caching features

### MCP Endpoints

- `POST /mcp/jsonrpc` - Main JSON-RPC endpoint for tool calls
- `GET /mcp/health` - Health check endpoint

### Unified Configuration

**MCP is now automatically enabled** - no separate configuration needed! Every flAPI instance runs both REST API and MCP servers concurrently.

Configuration files can define multiple entity types:

#### REST Endpoint + MCP Tool (Unified)

```yaml
# Single configuration file serves as BOTH REST endpoint AND MCP tool
url-path: /customers/                    # Makes this a REST endpoint
mcp-tool:                                # Also makes this an MCP tool
  name: get_customers
  description: Retrieve customer information by ID
  result-mime-type: application/json

request:
  - field-name: id
    field-in: query
    description: Customer ID
    required: false
    validators:
      - type: int
        min: 1
        max: 1000000
        preventSqlInjection: true

template-source: customers.sql
connection: [customers-parquet]

rate-limit:
  enabled: true
  max: 100
  interval: 60

auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: secret
      roles: [admin]
```

#### MCP Resource Only

```yaml
# MCP Resource example
mcp-resource:
  name: customer_schema
  description: Customer database schema definition
  mime-type: application/json

template-source: customer-schema.sql
connection: [customers-parquet]
```

### Using MCP Tools

Once MCP is enabled, you can interact with tools using JSON-RPC 2.0:

```bash
# Check MCP server health
curl 'http://localhost:8081/mcp/health'

# Initialize MCP connection
curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc": "2.0", "id": 1, "method": "initialize"}'

# List available tools (discovered from unified configuration)
curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc": "2.0", "id": 2, "method": "tools/list"}'

# Call a tool (same SQL template used for both REST and MCP)
curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "get_customers", "arguments": {"id": "123"}}}'
```

## 🎓 Example

Here's a simple example of how to create an API endpoint using flAPI:

### 1. Create a basic flAPI configuration
flAPI uses the popular [YAML](https://yaml.org/) format to configure the API endpoints. A basic configuration file looks like this:

```yaml
project_name: example-flapi-project
project_description: An example flAPI project demonstrating various configuration options
template:
  path: './sqls'            # The path where SQL templates and API endpoint configurations are stored
  environment-whitelist:    # Optional: List of regular expressions for whitelisting envvars which are available in the templates
    - '^FLAPI_.*'

duckdb:                     # Configuration of the DuckDB embedded into flAPI
  db_path: ./flapi_cache.db # Optional: remove or comment out for in-memory database, we use this store also as cache
  access_mode: READ_WRITE   # See the https://duckdb.org/docs/configuration/overview) for more details
  threads: 8
  max_memory: 8GB
  default_order: DESC

connections:                # A YAML map of database connection configurations, a API endpoint needs to reference one of these connections
   bigquery-lakehouse: 
                            # SQL commands to initialize the connection (e.g., e.g. installing, loading and configuring the BQ a DuckDB extension)
      init: |
         INSTALL 'bigquery' FROM 'http://storage.googleapis.com/hafenkran';
         LOAD 'bigquery';
      properties:           # A YAML map of connection-specific properties (accessible in templates via {{ context.conn.property_name }})
         project_id: 'my-project-id'

   customers-parquet: 
      properties:
         path: './data/customers.parquet'

heartbeat:
  enabled: true            # The eartbeat worker is a background thread which can can be used to periodically trigger endpionts
  worker-interval: 10      # The interval in seconds at which the heartbeat worker will trigger endpoints

enforce-https:
  enabled: false           # Whether to force HTTPS for the API connections, we strongly recommend to use a reverse proxy to do SSL termination
  # ssl-cert-file: './ssl/cert.pem'
  # ssl-key-file: './ssl/key.pem'

```

After that ensure that the template path (`./sqls` in this example) exists.

### 1. Define your API endpoint (`./sqls/customers.yaml`):
Each endpoint is at least defined by a YAML file and a corresponding SQL template in the template path. For our example we will create the file `./sqls/customers.yaml`:

```yaml
url-path: /customers/      # The URL path at which the endpoint will be available

request:                  # The request configuration for the endpoint, this defines the parameters that can be used in the query
  - field-name: id
    field-in: query       # The location of the parameter, other options are 'path', 'query' and 'body'
    description: Customer ID # A description of the parameter, this is used in the auto-generated API documentation
    required: false       # Whether the parameter is required
    validators:           # A list of validators that will be applied to the parameter
      - type: int
        min: 1
        max: 1000000
        preventSqlInjection: true

template-source: customers.sql # The path to the SQL template that will be used to generate the endpoint
connection: 
  - customers-parquet          # The connection that will be used to execute the query

rate-limit:
  enabled: true           # Whether rate limiting is enabled for the endpoint
  max: 100                # The maximum number of requests per interval
  interval: 60            # The interval in seconds
  
auth:
  enabled: true           # Whether authentication is enabled for the endpoint
  type: basic             # The type of authentication, other options are 'basic' and 'bearer'
  users:                  # The users that are allowed to access the endpoint
    - username: admin
      password: secret
      roles: [admin]
    - username: user
      password: password
      roles: [read]

heartbeat:
  enabled: true           # Whether the heartbeat worker if enabled will trigger the endpoint periodically
  params:                 # A YAML map of parameters that will be passed by the heartbeat worker to the endpoint
    id: 123

```
There are many more configuration options available, see the [full documentation](link-to-your-docs) for more details.

### 2. Configure the endpoints SQL template (`./sqls/customers.sql`):
After the creation of the YAML endpoint configuration we need to connect the SQL template which connects the enpoint to the data connection.
The template files use the [Mustache templating language](https://mustache.github.io/) to dynamically generate the SQL query. 

```sql
SELECT * FROM '{{{conn.path}}}'
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{{ params.id }}}
{{/params.id}}
```

The above template uses the `path` parameter defined in the connection configuration to directly query a local parquet file. If the id parameter is 
provided, it will be used to filter the results.

### 3. Send a request:
To test the endpoint and see if everything worked, we can use curl. We should also provide the correct basic auth credentials (`admin:secret` in this case). To make the JSON result easier to read, we pipe the output to [`jq`](https://jqlang.github.io/jq/).

```bash
> curl -X GET -u admin:secret "http://localhost:8080/customers?id=123" | jq .

{
  "next": "",
  "total_count": 1,
  "data": [
    {
      "c_mktsegment": "BUILDING",
      "c_acctbal": 5897.82999999999992724,
      "c_phone": "15-817-151-1168",
      "c_address": "YsOnaaER8MkvK5cpf4VSlq",
      "c_nationkey": 5,
      "c_name": "Customer#000000123",
      "c_comment": "ependencies. regular, ironic requests are fluffily regu",
      "c_custkey": 123
    }
  ]
}

```

## ⁉️ DuckLake-backed caching (current implementation)
flAPI uses the DuckDB DuckLake extension to provide modern, snapshot-based caching. You write the SQL to define the cached table, and flAPI manages schemas, snapshots, retention, scheduling, and audit logs.

### Quick start: Full refresh cache
1) Configure DuckLake globally (alias is `cache` by default):
```yaml
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./examples/data/cache.ducklake
  data-path: ./examples/data/cache.ducklake
  data-inlining-row-limit: 10  # Enable data inlining for small changes (optional)
  retention:
    max-snapshot-age: 14d
  compaction:
    enabled: false
  scheduler:
    enabled: true
```

2) Add cache block to your endpoint (no `primary-key`/`cursor` → full refresh):
```yaml
url-path: /publicis
template-source: publicis.sql
connection: [bigquery-lakehouse]

cache:
  enabled: true
  table: publicis_cache
  schema: analytics
  schedule: 5m
  retention:
    max_snapshot_age: 14d
  template_file: publicis/publicis_cache.sql
```

3) Write the cache SQL template (CTAS):
```sql
-- publicis/publicis_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT
  p.country,
  p.product_category,
  p.campaign_type,
  p.channel,
  sum(p.clicks) AS clicks
FROM bigquery_scan('{{{conn.project_id}}}.landing__publicis.kaercher_union_all') AS p
GROUP BY 1, 2, 3, 4;
```

4) Query from the cache in your main SQL:
```sql
-- publicis.sql
SELECT
  p.country,
  p.product_category,
  p.campaign_type,
  p.channel,
  p.clicks
FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS p
WHERE 1=1
```

Notes:
- The cache schema (`cache.analytics`) is created automatically if missing.
- Regular GET requests never refresh the cache. Refreshes happen on warmup, on schedule, or via the manual API.
- **Data Inlining**: When `data-inlining-row-limit` is configured, small cache changes (≤ specified row limit) are written directly to DuckLake metadata instead of creating separate Parquet files. This improves performance for small incremental updates.

#### Data inlining (optional, for small changes)

DuckLake supports writing very small inserts directly into the metadata catalog instead of creating a Parquet file for every micro-batch. This is called "Data Inlining" and can significantly speed up small, frequent updates.

- **Enable globally**: configure once under the top-level `ducklake` block:

  ```yaml
  ducklake:
    enabled: true
    alias: cache
    metadata_path: ./examples/data/cache.ducklake
    data_path: ./examples/data/cache.ducklake
    data_inlining_row_limit: 10  # inline inserts up to 10 rows
  ```

- **Behavior**:
  - Inserts with rows ≤ `data-inlining-row-limit` are inlined into the catalog metadata.
  - Larger inserts automatically fall back to normal Parquet file writes.
  - Inlining applies to all caches (global setting), no per-endpoint toggle.

- **Manual flush (optional)**: you can flush inlined data to Parquet files at any time using DuckLake’s function. Assuming your DuckLake alias is `cache`:

  ```sql
  -- Flush all inlined data in the catalog
  CALL ducklake_flush_inlined_data('cache');

  -- Flush only a specific schema
  CALL ducklake_flush_inlined_data('cache', schema_name => 'analytics');

  -- Flush only a specific table (default schema "main")
  CALL ducklake_flush_inlined_data('cache', table_name => 'events_cache');

  -- Flush a specific table in a specific schema
  CALL ducklake_flush_inlined_data('cache', schema_name => 'analytics', table_name => 'events_cache');
  ```

- **Notes**:
  - This feature is provided by DuckLake and is currently marked experimental upstream. See the DuckLake docs for details: [Data Inlining](https://ducklake.select/docs/stable/duckdb/advanced_features/data_inlining.html).
  - If you don’t set `data_inlining_row_limit`, flAPI won’t enable inlining and DuckLake will use regular Parquet writes.

### Advanced: Incremental append and merge
The engine infers sync mode from your YAML:
- No `primary-key`, no `cursor` → full refresh (CTAS)
- With `cursor` only → incremental append
- With `primary-key` + `cursor` → incremental merge (upsert)

Example YAMLs:
```yaml
# Incremental append
cache:
  enabled: true
  table: events_cache
  schema: analytics
  schedule: 10m
  cursor:
    column: created_at
    type: timestamp
  template-file: events/events_cache.sql

# Incremental merge (upsert)
cache:
  enabled: true
  table: customers_cache
  schema: analytics
  schedule: 15m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  template_file: customers/customers_cache.sql
```

Cache template variables available to your SQL:
- `{{cache.catalog}}`, `{{cache.schema}}`, `{{cache.table}}`, `{{cache.schedule}}`
- `{{cache.snapshotId}}`, `{{cache.snapshotTimestamp}}` (current)
- `{{cache.previousSnapshotId}}`, `{{cache.previousSnapshotTimestamp}}` (previous)
- `{{cache.cursorColumn}}`, `{{cache.cursorType}}`
- `{{cache.primaryKeys}}`
- `{{params.cacheMode}}` is available with values `full`, `append`, or `merge`

Incremental append example:
```sql
-- events/events_cache.sql
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT *
FROM source_events
WHERE {{#cache.previousSnapshotTimestamp}} event_time > TIMESTAMP '{{cache.previousSnapshotTimestamp}}' {{/cache.previousSnapshotTimestamp}}
```

Incremental merge example:
```sql
-- customers/customers_cache.sql
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT * FROM source_customers
  WHERE {{#cache.previousSnapshotTimestamp}} updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}' {{/cache.previousSnapshotTimestamp}}
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET
  name = s.name,
  email = s.email,
  updated_at = s.updated_at
WHEN NOT MATCHED THEN INSERT (*) VALUES (s.*);
```

### When does the cache refresh?
- Startup warmup: flAPI refreshes caches for endpoints with cache enabled.
- Scheduled refresh: controlled by `cache.schedule` on each endpoint (e.g., `5m`).
- Manual refresh: call the refresh API (see below).
- Regular GET requests do not refresh the cache.

### Audit, retention, compaction, and control APIs
flAPI maintains an audit table inside DuckLake at `cache.audit.sync_events` and provides control endpoints:

- Manual refresh:
```bash
curl -X POST "http://localhost:8080/api/v1/_config/endpoints/publicis/cache/refresh"
```

- Audit logs (endpoint-specific and global):
```bash
curl "http://localhost:8080/api/v1/_config/endpoints/publicis/cache/audit"
curl "http://localhost:8080/api/v1/_config/cache/audit"
```

- Garbage collection (retention):
Retention can be configured per endpoint under `cache.retention`:
```yaml
cache:
  retention:
    max-snapshot-age: 7d     # time-based retention
    # keep-last-snapshots: 3 # version-based retention (subject to DuckLake support)
```
The system applies retention after each refresh and you can also trigger GC manually:
```bash
curl -X POST "http://localhost:8080/api/v1/_config/endpoints/publicis/cache/gc"
```

- Compaction:
If enabled in global `ducklake.scheduler`, periodic file merging is performed via DuckLake `ducklake_merge_adjacent_files`.

### Template authoring guide (reference)
Use these variables inside your cache templates and main queries:

- Identification
  - `{{cache.catalog}}` → usually `cache`
  - `{{cache.schema}}` → e.g., `analytics` (auto-created if missing)
  - `{{cache.table}}` → your cache table name

- Mode and scheduling
  - `{{params.cacheMode}}` → `full` | `append` | `merge`
  - `{{cache.schedule}}` → if set in YAML

- Snapshots
  - `{{cache.snapshotId}}`, `{{cache.snapshotTimestamp}}`
  - `{{cache.previousSnapshotId}}`, `{{cache.previousSnapshotTimestamp}}`

- Incremental hints
  - `{{cache.cursorColumn}}`, `{{cache.cursorType}}`
  - `{{cache.primaryKeys}}` → comma-separated list, e.g., `id,tenant_id`

Authoring tips:
- Full refresh: use `CREATE OR REPLACE TABLE ... AS SELECT ...`.
- Append: `INSERT INTO cache.table SELECT ... WHERE event_time > previousSnapshotTimestamp`.
- Merge: `MERGE INTO cache.table USING (SELECT ...) ON pk ...`.
- Do not create schemas in templates; flAPI does that automatically.

### Troubleshooting
- Cache refresh happens on every request: by design this is disabled. Ensure you’re not calling the manual refresh endpoint from a client and that your logs show scheduled or warmup refreshes only.
- Schema not found: verify `cache.schema` is set; flAPI will auto-create it.
- Retention errors: use time-based `max-snapshot-age` first. Version-based retention depends on DuckLake support.

## 🧩 YAML includes and environment variables
flAPI extends plain YAML with lightweight include and environment-variable features so you can keep configurations modular and environment-aware.

### Environment variables
- Write environment variables as `{{env.VAR_NAME}}` anywhere in your YAML.
- Only variables that match the whitelist in your root config are substituted:
  ```yaml
  template:
    path: './sqls'
    environment-whitelist:
      - '^FLAPI_.*'     # allow all variables starting with FLAPI_
      - '^PROJECT_.*'   # optional additional prefixes
  ```
- If the whitelist is empty or omitted, all environment variables are allowed.

Examples:
```yaml
# Substitute inside strings
project-name: "${{env.PROJECT_NAME}}"

# Build include paths dynamically
template:
  path: "{{env.CONFIG_DIR}}/sqls"
```

### Include syntax
You can splice content from another YAML file directly into the current document.

- Basic include: `{{include from path/to/file.yaml}}`
- Section include: `{{include:top_level_key from path/to/file.yaml}}` includes only that key
- Conditional include: append `if <condition>` to either form

Conditions supported:
- `true` or `false`
- `env.VAR_NAME` (include if the variable exists and is non-empty)
- `!env.VAR_NAME` (include if the variable is missing or empty)

Examples:
```yaml
# Include another YAML file relative to this file
{{include from common/settings.yaml}}

# Include only a section (top-level key) from a file
{{include:connections from shared/connections.yaml}}

# Conditional include based on an environment variable
{{include from overrides/dev.yaml if env.FLAPI_ENV}}

# Use env var in the include path
{{include from {{env.CONFIG_DIR}}/secrets.yaml}}
```

Resolution rules and behavior:
- Paths are resolved relative to the current file first; absolute paths are supported.
- Includes inside YAML comments are ignored (e.g., lines starting with `#`).
- Includes are expanded before the YAML is parsed.
- Includes do not recurse: include directives within included files are not processed further.
- Circular includes are guarded against within a single expansion pass; avoid cycles.

Tips:
- Prefer section includes (`{{include:...}}`) to avoid unintentionally overwriting unrelated keys.
- Keep shared blocks in small files (e.g., `connections.yaml`, `auth.yaml`) and include them where needed.



## 🏭 Building from source
The source code of flAPI is written in C++ and closely resembles the [DuckDB build process](https://duckdb.org/docs/dev/building/overview). A good documentation of the build process is the GitHub action in [`build.yaml`](.github/workflows/build.yaml). In essecence a few prerequisites need to be met:
In essecence a few prerequisites need to be met:

- Install the dependencies: `sudo apt-get install -y build-essential cmake ninja-build`
- Checkout the repository and submodules: `git clone --recurse-submodules https://github.com/datazoode/flapi.git`
- Build the project: `make release`

The build process will download and build DuckDB v1.1.2 and install the vcpkg package manager. We depend on the following vcpkg ports:

- [`argparse`](https://github.com/p-ranav/argparse) - Command line argument parser
- [`crow`](https://github.com/CrowCpp/Crow) - Our REST-Web framework and JSON handling
- [`yaml-cpp`](https://github.com/jbeder/yaml-cpp) - YAML parser
- [`jwt-cpp`](https://github.com/Thalhammer/jwt-cpp) - JSON Web Token library
- [`openssl`](https://github.com/openssl/openssl) - Crypto library
- [`catch2`](https://github.com/catchorg/Catch2) - Testing framework

**Note**: MCP support is built-in and doesn't require additional dependencies beyond what's already included.

## 📚 Documentation

For more detailed information, check out our [full documentation](link-to-your-docs).

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for more details.

## 📄 License

flAPI is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for more details.

## 🙋‍♀️ Support

If you have any questions or need help, please [open an issue](https://github.com/yourusername/flapi/issues) or join our [community chat](link-to-your-chat).

---
