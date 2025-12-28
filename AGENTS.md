# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**flAPI** is a **SQL-to-API framework** that automatically generates REST APIs and AI-compatible tools from SQL templates and YAML configurations. Instead of writing backend code to expose data, analysts write SQL queries and endpoint configurations, and flAPI handles all boilerplate (HTTP server, parameter validation, caching, auth, rate-limiting, MCP protocol).

**Core Value Proposition:**
- **No backend coding**: REST API emerges from SQL + YAML configuration
- **Data analyst-friendly**: Uses SQL (familiar skill) instead of requiring backend developers
- **Multi-protocol**: REST and MCP (Model Context Protocol) equally supported
- **Built-in intelligence**: Validators, caching (DuckLake), authentication, rate-limiting
- **DuckDB-powered**: Access 50+ data sources (BigQuery, Postgres, S3, Snowflake, etc.)

**Key Characteristics:**
- Written in modern C++17 with zero runtime dependencies (single-binary deployment)
- ~13,400 lines of C++ code across 54 files
- Supports Linux (x86/ARM64), macOS (Intel/Apple Silicon), and Windows
- Declarative API philosophy: logic lives in YAML/SQL, not compiled code
- Single binary deployment with built-in DuckDB 1.4.3

## Building and Development

### Build Commands

```bash
# Build both debug and release versions
make all

# Build specific configuration
make debug          # Debug version (faster compilation, more debugging info)
make release        # Optimized release build

# Clean all build artifacts
make clean

# Run with example configuration
make run-debug      # Run debug binary with examples/flapi.yaml
make run-release    # Run release binary with examples/flapi.yaml
```

### Testing

```bash
# Run all C++ unit tests
make test

# Run integration tests (requires Python + tavern/pytest)
make integration-test                # All integration tests
make integration-test-rest           # REST API tests (Tavern)
make integration-test-mcp            # MCP protocol tests
make integration-test-ducklake       # Cache/DuckLake tests
make integration-test-ci             # Integration tests with server management

# Run all tests (unit + integration)
make test-all

# Setup Python integration test environment
make integration-test-setup          # Install Python dependencies
```

### Useful Single Test Commands

```bash
# Run a single C++ test using ctest
cd build/release && ctest -V -R "test_name_pattern"

# Run a single Python/Tavern test
cd test/integration && pytest test_name.tavern.yaml -v

# Run a specific Python test function
cd test/integration && pytest test_mcp_integration.py::test_function_name -v
```

### Development Workflow

```bash
# Full development cycle
make debug                           # Fast iterative builds
make run-debug                       # Start server with examples
# Edit code and rebuild: make debug

# Before committing
make test-all                        # Run full test suite
make clean                           # Clean artifacts
```

## Architecture Overview

### Component Interaction Flow

**Request Processing Pipeline:**
```
HTTP Request → APIServer (Crow)
  ↓
[Middleware: CORS, RateLimit, Auth]
  ↓
RequestHandler: Parse params → Validate → Check cache
  ↓
QueryExecutor: Render SQL template (Mustache) → Execute on DuckDB
  ↓
CacheManager: Check/update cache (if configured)
  ↓
Response: JSON serialization → HTTP response
```

**MCP Protocol Integration:**
```
MCP Client → MCPRouteHandlers (JSON-RPC)
  ├→ initialize, list_tools, call_tool, list_resources, read_resource
  └→ MCPToolHandler: Convert REST endpoints to MCP tools
```

**Configuration Loading:**
```
flapi.yaml (main config)
  ├→ connections: Define data sources
  ├→ duckdb: Engine settings
  ├→ endpoints: Directory with endpoint configs
  └→ sqls/endpoint_name.yaml
      ├→ url-path, request validators
      ├→ template-source: SQL file with Mustache syntax
      ├→ cache configuration (TTL, refresh strategy)
      └→ auth/mcp tool definitions
```

### Layered Design

**Layer 1: User/Config Layer**
- YAML endpoint configurations
- SQL templates with Mustache syntax
- User HTTP/MCP requests

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

### Key Components

| Component | Location | Purpose |
|-----------|----------|---------|
| **APIServer** | `src/api_server.cpp` | HTTP server (Crow framework), routing, middleware |
| **ConfigManager** | `src/config_*.cpp` | YAML parsing, endpoint discovery, validation |
| **DatabaseManager** | `src/database_manager.cpp` | DuckDB connection pooling, extension management |
| **RequestHandler** | `src/request_handler.cpp` | HTTP request processing, parameter validation |
| **QueryExecutor** | `src/query_executor.cpp` | SQL template rendering and execution |
| **CacheManager** | `src/cache_manager.cpp` | DuckLake caching: TTL, refresh, materialization |
| **AuthMiddleware** | `src/auth_middleware.cpp` | JWT/Basic auth validation |
| **RateLimitMiddleware** | `src/rate_limit_middleware.cpp` | Request rate limiting |
| **MCPToolHandler** | `src/mcp_*.cpp` | Model Context Protocol server implementation |

## Core Concepts

### 1. Connections

A **connection** defines access to a data source (file, database, cloud storage, API).

**Structure:**
```yaml
connections:
  my-data:
    properties:
      path: './data/customers.parquet'      # File-based
      # OR
      host: 'db.example.com'                # Database
      port: '5432'
      database: 'mydb'
      user: 'read_user'
      password: '${DB_PASSWORD}'            # Environment variable
```

**Key Points:**
- Properties are custom per connection type (file path vs. database credentials)
- Environment variables supported via `${VAR_NAME}` (must be whitelisted in flapi.yaml)
- Loaded once at startup, reused for all requests
- Available in templates as `conn.property_name`

### 2. Endpoints

An **endpoint** is a callable API operation (REST or MCP).

**Structure:**
```yaml
url-path: /customers
method: GET              # GET, POST, PUT, DELETE, PATCH

request:
  - field-name: id
    field-in: query      # or path, body, header
    required: false
    validators:
      - type: int
        min: 1

template-source: customers.sql     # SQL file name
connection: [my-data]              # Connection(s) to use
```

**Request Lifecycle:**
1. HTTP request arrives with parameters
2. Parameters extracted per `field-in` (query/path/body/header)
3. Validators applied (fail fast with 400 if invalid)
4. Template expanded with validated parameters
5. SQL executed against DuckDB
6. Results formatted as JSON
7. Response sent

### 3. SQL Templates (Mustache)

Templates are **Mustache files** that generate SQL from request parameters.

**Available Variables:**
- `params.*` - Request parameters (query, path, body, header)
- `conn.*` - Connection properties
- `cache.*` - Cache metadata (if cache enabled)
- `env.*` - Whitelisted environment variables

**Key Rule: Triple vs. Double Braces**
- **Triple braces `{{{ }}}`** for strings: Auto-escapes quotes, prevents SQL injection
- **Double braces `{{ }}`** for numbers/identifiers: No quotes, safe for numeric types

**Example Template:**
```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND customer_id = {{{ params.id }}}
{{/params.id}}
{{#params.min_price}}
  AND price >= {{ params.min_price }}
{{/params.min_price}}
ORDER BY created_at DESC
LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}100{{/params.limit}}
```

### 4. Validators

**Validators** enforce input constraints before SQL execution.

**Common Types:**
```yaml
validators:
  - type: int
    min: 1
    max: 999999

  - type: string
    min-length: 1
    max-length: 200
    pattern: "^[a-zA-Z0-9_]+$"

  - type: email
  - type: uuid
  - type: enum
    values: ["active", "inactive", "pending"]
  - type: date
    min: "2020-01-01"
    max: "2025-12-31"
```

**Security Strategy (Defense in Depth):**
1. **Validators**: First line (whitelist validation)
2. **Triple braces**: Second line (string escaping)
3. Never trust user input even with both layers

### 5. DuckLake Caching

**DuckLake** is a table versioning system for snapshot-based caching.

**Cache Modes:**
- **Full Refresh**: Recreate entire table each refresh (simple, slow for large tables)
- **Incremental Append**: Only add new rows (fast for append-only data)
- **Incremental Merge**: Insert/update/delete handling (complex, handles all changes)

**Configuration:**
```yaml
cache:
  enabled: true
  table: customers_cache
  schedule: "6h"                    # How often to refresh
  primary-key: [id]                 # For merge mode
  cursor:
    column: updated_at              # Tracks changes
    type: timestamp
```

**Cache Template Variables:**
- `{{cache.table}}` - Cache table name
- `{{cache.schema}}` - Schema name
- `{{cache.previousSnapshotTimestamp}}` - Last refresh time
- `{{cache.currentSnapshotTimestamp}}` - This refresh time

**Snapshot Lifecycle:**
```
Time 0: Cache initialized with full refresh
  → Run populate SQL
  → Store all rows, create snapshot v1

Time 6h: Schedule triggers
  → Run populate SQL (incremental)
  → MERGE changed rows into cache
  → Create snapshot v2 (only changes stored)

Time-travel: Query any previous snapshot
  → SELECT * FROM cache AS OF TIMESTAMP '...'
```

### Configuration System

**Endpoint Configuration Structure** (`sqls/endpoint_name.yaml`):
```yaml
# REST endpoint definition
url-path: /customers              # URL path
method: GET                        # HTTP method

# Request parameters
request:
  - field-name: id
    field-in: query               # query, path, header, body
    field-type: int
    required: false
    validators:
      - type: int
        min: 1

# SQL execution
template-source: customers.sql     # Relative to sqls/ dir
connection: [data-source-name]     # From connections in flapi.yaml

# Caching (optional)
cache:
  enabled: true
  ttl: 3600                        # Seconds
  refresh: full                    # full or incremental
  table: customers_cache           # Cache table name

# MCP tool definition (optional)
mcp-tool:
  description: Get customer information
  input_schema:
    type: object
    properties:
      id:
        type: integer
        description: Customer ID

# Authorization (optional)
auth:
  required: true
  roles: [admin, user]
```

**SQL Template with Mustache** (`sqls/endpoint_name.sql`):
```sql
SELECT * FROM read_parquet('{{ context.conn.path }}')
WHERE 1=1
{{#if params.id}}
  AND customer_id = {{ params.id }}
{{/if}}
{{#if params.status}}
  AND status = '{{ params.status }}'
{{/if}}
```

Template variables available:
- `params.*`: Query/path/body parameters from request
- `context.conn.*`: Connection properties (paths, credentials)
- `context.auth.*`: Authentication context

## Key Patterns

### Safe Query Building Pattern

Use `WHERE 1=1` with conditional sections to safely build dynamic queries:

```sql
SELECT * FROM table
WHERE 1=1
{{#params.filter1}}
  AND column1 = '{{{ params.filter1 }}}'
{{/params.filter1}}
{{#params.filter2}}
  AND column2 = {{{ params.filter2 }}}
{{/params.filter2}}
ORDER BY created_at DESC
LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}100{{/params.limit}}
```

**Why This Works:**
- `WHERE 1=1` allows adding any number of AND conditions
- Each condition wrapped in conditional section (only rendered if parameter exists)
- Default applied if parameter not specified (see LIMIT)
- Triple braces for strings, double for numbers
- No SQL injection risk (validators + escaping)

### Template Variable Types

**Conditional Rendering:**
```sql
{{#params.name}}
  -- Only rendered if params.name exists and is truthy
  AND name = '{{{ params.name }}}'
{{/params.name}}

{{^params.name}}
  -- Rendered if params.name does NOT exist
  AND name IS NULL
{{/params.name}}
```

**Default Values:**
```sql
LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}100{{/params.limit}}
-- Use params.limit if provided, otherwise default to 100
```

**Connection Properties:**
```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
-- Access connection properties defined in flapi.yaml
```

**Cache Metadata (Incremental Refresh):**
```sql
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT * FROM source
{{#cache.previousSnapshotTimestamp}}
WHERE updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}'
{{/cache.previousSnapshotTimestamp}}
-- Only refresh rows changed since last snapshot
```

## Code Style and Conventions

### C++ (Backend)

**File and Naming:**
- Files: `lowercase_with_underscores.cpp` and `.hpp`
- Classes: `PascalCase`
- Functions: `PascalCase`
- Variables: `snake_case`
- Constants: `SCREAMING_SNAKE_CASE`
- Member variables: prefix with `_` (e.g., `_userId`)

**Modern C++ Practices:**
- Use `std::unique_ptr` over `shared_ptr`; use `new`/`delete` only in exceptional cases
- Always use `const` and references where appropriate
- Use `const` references for non-trivial objects: `const std::vector<T>&`
- Avoid namespace imports: `std::string`, not `using std`
- Use C++11 range-based for loops: `for (const auto& item : items)`
- Use `std::optional`, `std::variant` for type-safe alternatives

**Class Layout:**
```cpp
class MyClass {
public:
    MyClass();
    int public_variable;

public:
    void MyMethod();

private:
    void PrivateMethod();

private:
    int _private_variable;
};
```

**Important Rules:**
- All functions in `src/` directory should be in the `duckdb` namespace
- Use `override` when overriding virtual methods
- Use `[u]int(8|16|32|64)_t` instead of `int`, `long`, `uint`
- Use `idx_t` instead of `size_t` for offsets/indices/counts
- Always use braces for `if` statements and loops (no single-line statements)
- Use exceptions only for exceptional situations that terminate query execution
- Validate inputs at function boundaries
- Use `D_ASSERT` for programmer errors, never for user input

### TypeScript (CLI/VSCode Extension)

**Location:** `cli/` directory

**Important Rules:**
- Both CLI and VSCode extension should share a common API client
- Both should communicate with flAPI server via **ConfigService** (never directly via files)
- All interactions use the same authentication tokens and URL resolution logic

**CLI Commands** (`cli/src/commands/`):
- `config/`: Configuration management
- `endpoint/`: Endpoint testing and introspection
- `template/`: SQL template validation
- `cache/`: Cache inspection and management
- `serve/`: Local development server

## Testing

### C++ Unit Tests

**Framework:** Catch2 (`test/cpp/`)

Test files: `src/components/*_test.cpp`

**Running tests:**
```bash
make test                          # Run all unit tests
cd build/release && ctest -V -R pattern  # Run specific test
```

**Test patterns:**
- Use Catch2 `TEST_CASE` and `SECTION` macros
- Mock dependencies with test fixtures
- Test both success and error cases

### Python Integration Tests

**Framework:** pytest + Tavern (`test/integration/`)

**Test types:**
- `*.tavern.yaml`: REST API tests using Tavern specification
- `*.py`: Custom Python tests (MCP, async, complex scenarios)

**Running tests:**
```bash
make integration-test-setup        # Install dependencies
make integration-test-rest         # Tavern tests
make integration-test-mcp          # MCP protocol tests
make integration-test-ci           # Full suite with server management
```

**Test server lifecycle:**
- Tests assume server running on `http://localhost:8080`
- Use `integration-test-ci` target for automatic server startup/shutdown
- See `test/integration/conftest.py` for fixtures

## Database and Caching (DuckLake)

### DuckDB Integration

- Embedded in-process OLAP database (v1.4.3)
- Extensions for external data sources: BigQuery, Postgres, Iceberg, Parquet, CSV
- Query execution with result caching

**Database Operations:**
- Defined in `DatabaseManager` (src/database_manager.cpp)
- Connection pooling to external sources
- Extension loading and configuration
- Transaction support with ACID compliance

### Caching Strategy (DuckLake)

**Cache Configuration in Endpoint YAML:**
```yaml
cache:
  enabled: true
  ttl: 3600                        # Cache validity in seconds
  refresh: full                    # full = REPLACE, incremental = APPEND/MERGE
  table: cache_table_name          # Where to store cached results
  refresh_query: |                 # Optional: custom refresh query
    SELECT * FROM external_source
```

**Cache Refresh Lifecycle:**
1. HeartbeatWorker monitors cache expiration schedules
2. On refresh trigger: execute refresh query
3. Full refresh: `REPLACE INTO cache_table SELECT ...`
4. Incremental: `INSERT INTO cache_table SELECT ...`
5. Subsequent requests serve from cache until TTL expires

**Materialized Results:**
- Cache tables stored in DuckDB local database
- Results materialized as Parquet internally
- Fast serving from local storage (millisecond latency)

## Configuration Reference

### Main Configuration (`flapi.yaml`)

```yaml
# Project metadata
project_name: my-project
project_description: Description
version: 1.0.0

# SQL templates location
template:
  path: ./sqls

# Data source connections
connections:
  data-source-name:
    type: postgres | snowflake | bigquery | local  # Optional, auto-detected
    properties:
      # Type-specific: path, host, database, api_key, etc.
      path: ./data/customers.parquet

# DuckDB engine settings
duckdb:
  db_path: ./flapi_cache.db        # Local cache database
  threads: 4                        # Query parallelism
  max_memory: 2GB                   # Memory limit
  extensions:                       # Optional: explicit extension config
    - name: json
    - name: postgres

# Server settings
server:
  port: 8080                        # REST API port
  mcp_port: 8081                    # MCP server port
  host: 0.0.0.0
  log_level: info                   # debug, info, warn, error

# Global auth configuration (optional)
auth:
  default_required: true
  jwt_secret: ${JWT_SECRET}         # Environment variable substitution
  allowed_roles: [admin, user]

# Global rate limiting (optional)
rate_limit:
  enabled: true
  requests_per_minute: 100
  burst_size: 10
```

### Environment Variables

```bash
# Configuration
FLAPI_CONFIG=path/to/flapi.yaml     # Config file path
FLAPI_LOG_LEVEL=debug|info|warn|error

# Authentication
JWT_SECRET=your-secret-key          # JWT signing key
AWS_REGION=us-east-1                # For AWS Secrets Manager

# Development
FLAPI_CROSS_COMPILE=arm64            # Cross-compile target (Linux only)
```

## CLI Management API

The `flapii` CLI tool communicates with the running flAPI server via REST APIs. These management APIs allow runtime configuration changes without restarting.

### Key Management Endpoints

**Endpoint Management:**
```bash
GET /api/v1/_config/endpoints         # List all endpoints
GET /api/v1/_config/endpoints/{path}  # Get one endpoint
POST /api/v1/_config/endpoints        # Create new endpoint
PUT /api/v1/_config/endpoints/{path}  # Update endpoint
DELETE /api/v1/_config/endpoints/{path}  # Delete endpoint
```

**Template Management:**
```bash
GET /api/v1/_config/endpoints/{path}/template     # Get template content
PUT /api/v1/_config/endpoints/{path}/template     # Update template
POST /api/v1/_config/endpoints/{path}/template/expand  # Expand template
```

**Cache Management:**
```bash
GET /api/v1/_config/endpoints/{path}/cache        # Get cache config
PUT /api/v1/_config/endpoints/{path}/cache        # Update cache
POST /api/v1/_config/endpoints/{path}/cache/refresh  # Force refresh
```

**Schema Introspection:**
```bash
GET /api/v1/_schema                   # Get database schema
GET /api/v1/_schema/connections       # List connections
POST /api/v1/_schema/refresh          # Refresh schema cache
```

**Server Health:**
```bash
POST /api/v1/_ping                    # Check server status
```

### CLI Command Examples

```bash
# List all endpoints
flapii endpoints list

# Get specific endpoint config
flapii endpoints get /customers

# Test template expansion
flapii templates expand /customers --params '{"id":"123"}'

# Refresh cache for endpoint
flapii cache refresh /customers

# View cache configuration
flapii cache get /customers

# Validate endpoint YAML before creating
flapii endpoints validate sqls/new_endpoint.yaml

# Create endpoint from YAML file
flapii endpoints create sqls/new_endpoint.yaml
```

### How CLI Interacts with Server

```
User runs: flapii endpoints list
  ↓
CLI Command (cli/src/commands/endpoints/list.ts)
  ↓
Makes HTTP call: GET http://localhost:8080/api/v1/_config/endpoints
  ↓
ConfigService API Handler (src/config_service.cpp)
  ↓
Returns endpoint list as JSON
  ↓
CLI formats as table/JSON and displays
```

## Common Development Tasks

### Initializing a New flapi Project

The CLI provides a project initialization command that scaffolds a new flapi project with all necessary directories and example files.

**Quick Start:**

```bash
# Initialize in current directory
flapii project init

# Create new project directory
flapii project init my-api-project

# Force overwrite existing files
flapii project init my-api --force
```

**What Gets Created:**

```
project-directory/
├── flapi.yaml              # Main configuration file
├── sqls/                   # Endpoint definitions and SQL templates
│   ├── sample.yaml         # Example endpoint config
│   └── sample.sql          # Example SQL template with Mustache
├── data/                   # Data files directory (add your Parquet, CSV, etc.)
├── common/                 # Reusable configuration templates
│   ├── auth.yaml           # Authentication template
│   └── rate-limit.yaml     # Rate limiting template
└── .gitignore              # Git ignore rules for flapi projects
```

**Command Options:**

```bash
# Skip validation after setup
flapii project init my-api --skip-validation

# Only create directories and flapi.yaml (no examples)
flapii project init my-api --no-examples

# Force overwrite if files already exist
flapii project init my-api --force

# Advanced mode (additional templates, Phase 3)
flapii project init my-api --advanced
```

**Detailed Walkthrough:**

1. Run initialization:
   ```bash
   flapii project init my-first-api
   Setting up flapi project: my-first-api
   ✅ Created directories
   ✅ Created flapi.yaml
   ✅ Created sample endpoint
   ✅ Created .gitignore
   ✅ Created reusable configs
   ✓ Configuration is valid
   ```

2. Edit `flapi.yaml` to add your database connections:
   ```yaml
   connections:
     my-database:
       properties:
         host: localhost
         port: 5432
         user: $DB_USER
         password: $DB_PASSWORD
   ```

3. Create endpoint YAML in `sqls/` directory

4. Write SQL templates in `sqls/`

5. Run server:
   ```bash
   ./flapi -c my-first-api/flapi.yaml
   ```

### Adding a New REST Endpoint

1. **Create endpoint YAML** (`sqls/my_endpoint.yaml`):
```yaml
url-path: /my-endpoint
method: GET
request:
  - field-name: param1
    field-in: query
    field-type: string
template-source: my_endpoint.sql
connection: [data-source]
```

2. **Create SQL template** (`sqls/my_endpoint.sql`):
```sql
SELECT * FROM table
WHERE column = '{{ params.param1 }}'
```

3. **Test locally**:
```bash
make run-debug
# Test: curl http://localhost:8080/my-endpoint?param1=value
```

4. **Add integration test** (`test/integration/test_my_endpoint.tavern.yaml`):
```yaml
test_name: Test my endpoint
stages:
  - name: Get data
    request:
      url: http://localhost:8080/my-endpoint?param1=value
      method: GET
    response:
      status_code: 200
```

### AI-Assisted Endpoint Creation

The CLI includes an interactive endpoint creation wizard with optional AI assistance powered by Google Gemini. This lets you generate endpoint configurations from natural language descriptions.

**Quick Start:**

```bash
# Interactive wizard with manual mode (no AI)
flapii endpoints wizard

# AI-assisted endpoint creation
flapii endpoints wizard --ai

# Save configuration to file instead of creating endpoint
flapii endpoints wizard --output-file endpoint-config.yaml

# Preview without making changes
flapii endpoints wizard --dry-run

# Skip validation (useful for batch processing)
flapii endpoints wizard --skip-validation
```

**Using AI Generation:**

1. Run `flapii endpoints wizard --ai`
2. Provide API key when prompted (stored in `~/.flapi/config.json`)
3. Describe your endpoint in natural language:
   ```
   "Create a GET endpoint to fetch active customers with pagination.
    Should filter by status and region. Cache for 5 minutes."
   ```
4. AI generates endpoint configuration with parameters, validators, and cache settings
5. Review the generated config and choose to:
   - Accept and save immediately
   - Edit configuration (modify name, path, parameters, cache settings)
   - Regenerate with a different description
   - Fall back to manual mode
   - Cancel

**AI Authentication:**

- Set `FLAPI_GEMINI_KEY` environment variable, OR
- Provide API key when first prompted (saved securely to `~/.flapi/config.json`)
- Get a free API key: https://aistudio.google.com/app/apikey

**Manual Mode:**

If you prefer not to use AI or it's not available:
```bash
flapii endpoints wizard  # No --ai flag

# Wizard prompts for:
# - Endpoint name and URL path
# - HTTP method (GET/POST/PUT/DELETE)
# - Parameters (name, type, location: query/path/body)
# - Cache settings (TTL)
```

**Saving to File:**

Generate configuration files without creating endpoints:
```bash
# Output as YAML (for version control/review)
flapii endpoints wizard --output-file my-endpoint.yaml

# Review file before creating
cat my-endpoint.yaml
flapii endpoints create --file my-endpoint.yaml
```

### Adding Cache to an Endpoint

1. Add cache section to endpoint YAML:
```yaml
cache:
  enabled: true
  ttl: 3600
  refresh: full
  table: my_endpoint_cache
```

2. Create materialized view or cache table definition in DuckDB

3. Test cache refresh:
```bash
make run-debug
# Monitor DuckDB cache: SELECT * FROM my_endpoint_cache;
```

### Modifying SQL Templates

- Templates use Mustache syntax: `{{ variable_name }}`
- Available variables:
  - `params.*` - Request parameters
  - `context.conn.*` - Connection properties
  - `context.auth.*` - Auth context
- Test template rendering locally before deployment

### Debugging a Failed Request

1. **Check server logs**:
```bash
make run-debug
# Look for error messages in output
```

2. **Enable verbose logging**:
```bash
./build/debug/flapi --config examples/flapi.yaml --log-level debug
```

3. **Test endpoint directly**:
```bash
curl -v http://localhost:8080/endpoint?param=value
```

4. **Check configuration validation**:
```bash
# Review endpoint YAML for required fields
# Check SQL template for Mustache syntax errors
```

### Testing Template Expansion

Test how Mustache templates render before deploying:

```bash
# Expand template with parameters
flapii templates expand /customers --params '{"id":"123"}'

# See generated SQL output
# Check for SQL syntax errors
```

### Debugging Validation Errors

When parameter validation is rejecting valid input:

```bash
# List endpoint config with validators
flapii endpoints get /endpoint | jq '.request[0].validators'

# Test request
curl -v "http://localhost:8080/endpoint?param=value"

# Check response for validation error details
# Adjust validators in endpoint config
```

### Testing Cache Behavior

Debug cache refresh and hit rates:

```bash
# Force cache refresh
flapii cache refresh /endpoint

# Check cache status
flapii cache get /endpoint

# View cache configuration
flapii cache get /endpoint | jq '.cache'

# Monitor cache tables
# SELECT * FROM endpoint_cache_table;
```

### Adding a New Validator Type

1. Add type to validator enum in `src/include/validators.hpp`
2. Implement validation logic in `src/validators.cpp`
3. Add YAML schema documentation
4. Write unit tests in `test/unit/validators/`
5. Write integration tests in `test/integration/`
6. Update CLI help text

### Extending with DuckDB Extensions

flAPI can use any DuckDB extension. Extensions are auto-loaded when needed:

```yaml
# In connection config, use DuckDB-specific syntax
connections:
  bigquery-data:
    properties:
      project_id: 'my-project'
      dataset: 'my_dataset'

# Extensions automatically loaded: postgres_scanner, httpfs, bigquery, json, etc.
```

Common extensions:
- `postgres_scanner` - Query Postgres
- `httpfs` - Read S3, GCS, Azure
- `json` - JSON functions
- `iceberg` - Apache Iceberg tables
- `delta` - Delta Lake tables

## Dependencies and Build System

### CMake Build Configuration

**Key Features:**
- C++17 standard requirement
- vcpkg integration for consistent dependency management
- Platform-specific configurations (Windows, macOS, Linux/ARM64)
- Cross-compilation support
- Sanitizer support for debug builds (ASAN, UBSAN)

**Build Targets:**
- `flapi-lib`: Static library with core functionality
- `flapi`: Executable binary
- `integration_tests`: Python integration test target
- Catch2 tests: C++ unit tests

### Dependencies

| Library | Purpose |
|---------|---------|
| **DuckDB 1.4.3** | In-process OLAP database |
| **Crow** | C++ web framework |
| **OpenSSL** | Encryption/security |
| **jwt-cpp** | JWT authentication |
| **yaml-cpp** | YAML configuration parsing |
| **argparse** | CLI argument parsing |
| **fmt** | String formatting |
| **AWS SDK** | AWS Secrets Manager integration |

### Dependency Management

**Linux/macOS:** vcpkg (automatic setup via CMake)
**Windows:** vcpkg with x64-windows-static-md triplet

## Extension Points

### 1. Custom Validators

Add new validator types beyond the built-in ones (int, string, email, uuid, enum, date, time).

**How to Add:**
1. Add type to validator enum in `src/include/validators.hpp`
2. Implement validation logic in `src/validators.cpp`
3. Use in endpoint config:
   ```yaml
   validators:
     - type: custom_type
       option1: value1
   ```

### 2. DuckDB Extensions

flAPI supports all DuckDB extensions. They auto-load when needed.

**Supported Extensions:**
- `postgres_scanner` - Query Postgres databases
- `httpfs` - Read from S3, GCS, Azure, HTTP
- `json` - JSON functions and operators
- `iceberg` - Apache Iceberg tables
- `delta` - Delta Lake tables
- `csv` - CSV file scanning
- `httpfs` - Cloud storage access
- And 50+ more

**Usage:** Just reference the data source in your template, extension loads automatically.

### 3. Custom Authentication

Currently supports Basic and JWT auth. Can be extended for:
- OAuth2/OIDC
- API keys
- Custom token validation

Configure in endpoint:
```yaml
auth:
  type: jwt
  token-key: ${JWT_SECRET}
  issuer: "example.com"
```

### 4. Output Formatters

Default format is JSON. Can extend to support:
- CSV
- Parquet
- Arrow
- XML

Reference in endpoint or query parameter.

### 5. Scheduled Tasks

Beyond cache refresh, can extend scheduler for:
- Custom background jobs
- Data synchronization
- Reporting
- Maintenance tasks

Configure with cron expressions or interval schedules.

## Performance Characteristics

### Request Latency (Typical)

| Operation | Time |
|-----------|------|
| Parameter validation | 1-5ms |
| Template expansion (Mustache) | 2-10ms |
| Simple SELECT (cached) | 5-50ms |
| Complex query (not cached) | 100-1000ms |
| Network roundtrip | 1-10ms |
| **Total for simple GET** | **10-70ms** |

### Memory Usage

- Base server: ~50MB
- Per connection: ~5-20MB
- Cache table (1M rows): ~100-500MB
- DuckDB buffer pool: Configurable, default 1GB

### Scalability

- Concurrent connections: 1000+
- Endpoints: 1000+
- Parameters per endpoint: No practical limit (~20 recommended)
- Request rate: 1000+ req/sec (single-threaded DuckDB)

## Performance Considerations

### Query Optimization

- DuckDB performs aggressive optimization on queries
- Use `EXPLAIN` to analyze query plans: `EXPLAIN SELECT ...`
- Filters in WHERE clauses are pushed down to source systems
- Joins between parquet/CSV and external sources are optimized

### Cache Strategy

- Use full refresh (`refresh: full`) for small, frequently-accessed datasets
- Use incremental refresh (`refresh: incremental`) for large append-only data
- Set appropriate TTL based on data freshness requirements
- Monitor cache hit rates in logs

### Memory Management

- Configure `duckdb.max_memory` based on available system memory
- DuckDB spillsover to disk when memory limit reached
- Use `duckdb.threads` to control parallelism

## Deployment

### Docker

```bash
make docker                         # Build Docker image
# Image includes pre-built flAPI binary
# Ports: 8080 (REST), 8081 (MCP)
```

### Single Binary

```bash
make release                        # Build optimized release binary
./build/release/flapi --config flapi.yaml
```

The binary is statically linked with all dependencies and can be deployed to any Linux/macOS/Windows system without additional runtime requirements.

## Troubleshooting

### Build Issues

**Problem:** CMake finds wrong DuckDB version
- Solution: `rm -rf build/` and rebuild

**Problem:** vcpkg dependencies not found (macOS/Windows)
- Solution: Ensure `VCPKG_ROOT` environment variable is set

**Problem:** Cross-compilation failure (Linux ARM64)
- Solution: Set `FLAPI_CROSS_COMPILE=arm64` and use proper toolchain

### Runtime Issues

#### Endpoint returns 404

**Problem:** `GET /my-endpoint` returns `404 Not Found`

**Causes:**
- Endpoint YAML not found in sqls/ directory
- `url-path` doesn't match request path
- Endpoint not loaded during startup

**Solution:**
```bash
# List all registered endpoints
flapii endpoints list

# Get specific endpoint
flapii endpoints get /my-endpoint

# Check YAML file exists
ls -la sqls/my_endpoint.yaml

# Review url-path in config
flapii endpoints get /my-endpoint | jq '.url-path'

# Reload endpoint if already created
flapii endpoints reload /my-endpoint
```

#### Template Expansion Fails

**Problem:** `400 Bad Request: Template error` or `500 Internal Server Error`

**Causes:**
- Mustache syntax error (unmatched braces)
- Variable not in context (typo in variable name)
- Invalid SQL generated

**Solution:**
```bash
# Test template expansion with parameters
flapii templates expand /endpoint --params '{"id":"123"}'

# Check syntax
flapii endpoints validate sqls/endpoint.yaml

# Enable debug logging
flapii config log-level set debug

# Check error message in logs
./build/debug/flapi --config flapi.yaml --log-level debug
```

#### Validator Always Rejects Valid Input

**Problem:** `400 Bad Request: Invalid parameter: field - validation failed`

**Causes:**
- Validator type mismatch (expecting int, got string)
- Constraints too strict (min/max bounds)
- Regex pattern doesn't match valid input

**Solution:**
```bash
# Check validator configuration
flapii endpoints get /endpoint | jq '.request[0].validators'

# Test with curl (no validators)
curl -v "http://localhost:8080/endpoint?id=test"

# Review validator config - may need to:
# - Change type
# - Loosen min/max bounds
# - Fix regex pattern
# - Make parameter optional (required: false)
```

#### Cache Not Refreshing

**Problem:** Cache data is stale, refresh not triggering

**Causes:**
- Cache schedule not firing (disabled or incorrect syntax)
- Template error in cache populate SQL
- DuckLake not initialized
- Cache table doesn't exist

**Solution:**
```bash
# Check cache configuration
flapii cache get /endpoint

# Manually force refresh
flapii cache refresh /endpoint

# Check cache status
flapii cache list

# Validate cache populate template
cat sqls/cache-populate.sql

# Enable debug logging to see refresh attempts
flapii config log-level debug

# Check if cache table exists
# SELECT * FROM cache_table_name;
```

#### Memory Usage High

**Problem:** DuckDB using lots of memory, slow queries

**Causes:**
- Large query result set without LIMIT
- No LIMIT clause in template
- Cache accumulating snapshots
- Memory limit not configured

**Solution:**
```yaml
# In flapi.yaml, set memory limit
duckdb:
  max_memory: "4GB"

# In endpoint config, set reasonable LIMIT
request:
  - field-name: limit
    validators:
      - type: int
        max: 10000
```

```bash
# Trigger garbage collection
flapii cache gc

# Check retention policy
flapii cache get /endpoint | jq '.cache.retention'

# Monitor memory in debug logs
flapii config log-level debug
```

#### Connection Fails

**Problem:** `500 Internal Server Error` or connection refused

**Causes:**
- Wrong connection credentials
- Database not accessible from network
- Missing environment variables
- Connection not whitelisted

**Solution:**
```bash
# List connections
flapii schema connections

# Test connection
flapii schema refresh --connection connection-name

# Check credentials
echo $DB_PASSWORD  # Should not be empty

# Verify connection config in flapi.yaml
cat flapi.yaml | grep -A 10 "connections:"

# Check environment variable whitelist
cat flapi.yaml | grep -A 5 "environment-whitelist:"

# Enable debug logging
flapii config log-level debug
./build/debug/flapi --config flapi.yaml
```

#### Authentication Failures

**Problem:** `401 Unauthorized` or JWT validation errors

**Causes:**
- Missing or invalid JWT secret
- Token expired or malformed
- Auth not enabled for endpoint
- Wrong authentication type

**Solution:**
```bash
# Check if auth is enabled for endpoint
flapii endpoints get /endpoint | jq '.auth'

# Verify JWT secret is set
echo $JWT_SECRET  # Must not be empty

# Check token format
# Should be: Authorization: Bearer <token>

# Verify auth configuration
cat flapi.yaml | grep -A 10 "auth:"

# Test without auth (temporary)
# Remove or set required: false in endpoint config
```

#### SQL Syntax Errors

**Problem:** `400 Bad Request` or `500 Internal Server Error` with SQL error

**Causes:**
- Invalid SQL generated from template
- Mustache variables containing invalid SQL
- Triple braces not used for strings
- Missing connections

**Solution:**
```bash
# Expand template to see generated SQL
flapii templates expand /endpoint --params '{"id":"123"}'

# Check generated SQL syntax
# Copy output and run in DuckDB

# Fix template:
# - Use triple braces for strings: {{{ }}}
# - Use double braces for numbers: {{ }}
# - Verify variable names match

# Test with EXPLAIN
# EXPLAIN SELECT ... (in generated SQL)
```

