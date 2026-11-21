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
- Written in modern C++20 with zero runtime dependencies (single-binary deployment)
- Supports Linux (x86/ARM64), macOS (Intel/Apple Silicon), and Windows
- Declarative API philosophy: logic lives in YAML/SQL, not compiled code
- Single binary deployment with built-in DuckDB 1.5.5

## Architecture Documentation

**Start at [docs/README.md](docs/README.md)** — the task-first documentation index.

Architecture and design (the "how it works" tree):

- **[docs/spec/ARCHITECTURE.md](docs/spec/ARCHITECTURE.md)** - System architecture overview with component diagrams
- **[docs/spec/DESIGN_DECISIONS.md](docs/spec/DESIGN_DECISIONS.md)** - Rationale for key design choices
- **[docs/spec/REQUEST_LIFECYCLE.md](docs/spec/REQUEST_LIFECYCLE.md)** - End-to-end request flow with sequence diagrams
- **[docs/spec/components/](docs/spec/components/)** - Component-level documentation:
  - [config-system.md](docs/spec/components/config-system.md) - Configuration, copy-on-write endpoints
  - [query-execution.md](docs/spec/components/query-execution.md) - SQL templates and DuckDB
  - [caching.md](docs/spec/components/caching.md) - DuckLake caching system
  - [mcp-protocol.md](docs/spec/components/mcp-protocol.md) - MCP server, SEP-414 trace context
  - [security.md](docs/spec/components/security.md) - Auth, validation, telemetry egress
  - [observability.md](docs/spec/components/observability.md) - Request identity, spans, capture tiers

Task-first guides (`docs/guides/`): getting-started, rest-endpoints, mcp-tools,
caching, authentication, cloud-storage, yaml-includes, self-packaging,
serverless, claude-code.

### Claude Code plugin (`plugins/flapi/`)

flAPI ships a Claude Code plugin from this repo (`.claude-plugin/marketplace.json`
-> `plugins/flapi/`): 10 skills (`skills/flapi-*/SKILL.md`), 7 slash commands
(`commands/`), an MCP server entry (`.mcp.json`) and a validate-and-reload hook
(`hooks/`). Load it in-repo with `claude --plugin-dir ./plugins/flapi`.

- **Update the skill in the same PR as the feature.** A new config key, CLI flag,
  template variable or behaviour change is not done until the skill that teaches it
  says so.
- `make check-skills` (CI: `cli-integration` job) checks every YAML key against the
  parser, every `flapi`/`flapii` command and option against `--help`, every
  `{{cache.*}}` variable, the version stamp, and runs every tagged example through
  `--validate-config` and a live server. Tag runnable examples
  (`# flapi: endpoint group=g file=x.yaml`, `-- flapi: template ...`,
  `# flapi: check GET /x -> 200`); see `scripts/check_skills.py`.
- Version: `plugin.json`, every SKILL.md `version:` and `**flAPI:** v...` banner
  equal the newest `## vX.Y.Z` heading in CHANGELOG.md. Bump them in the release-prep
  commit.
- Deprecated or wrong facts the skills must not teach are listed in
  `scripts/skill_snippets/forbidden.txt`.

Reference:
- [docs/CONFIG_REFERENCE.md](docs/CONFIG_REFERENCE.md) - Configuration file format
- [docs/CLI_REFERENCE.md](docs/CLI_REFERENCE.md) - CLI commands
- [docs/MCP_REFERENCE.md](docs/MCP_REFERENCE.md) - MCP protocol details
- [docs/CONFIG_SERVICE_API_REFERENCE.md](docs/CONFIG_SERVICE_API_REFERENCE.md) - Runtime configuration API
- [docs/OBSERVABILITY.md](docs/OBSERVABILITY.md) - Tracing, audit, log correlation

`docs/archive/` holds completed plans and shipped design notes. **Not maintained
— never cite it as current behaviour.**

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
      password: '{{env.DB_PASSWORD}}'       # Environment variable (whitelisted under template:)
```

**Key Points:**
- Properties are custom per connection type (file path vs. database credentials)
- Environment variables supported via `{{env.VAR_NAME}}` (must match `template.environment-whitelist` in flapi.yaml; an unlisted one stops startup; empty whitelist = none; `${VAR}` is NOT supported - only `{{env.VAR}}`, with optional spaces inside the braces)
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
- **Triple braces `{{{ }}}`** for strings: Renders the raw value (no HTML entity escaping). Use inside single-quoted SQL string literals.
- **Double braces `{{ }}`** for numbers/identifiers: HTML-escapes the value (turns `<` into `&lt;` etc.), which is harmless but not SQL-aware — use only where the value is a number or known-safe identifier.

> **Security note:** Neither form performs SQL-specific escaping. Mustache does not understand SQL string literals, quote-doubling, or comment syntax. Defense against injection comes from the `RequestValidator` (typed fields, regex/range/enum checks) and disciplined template authoring (quote string params, parameterise numerics). When in doubt, add a stricter validator rather than relying on rendering.

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
```

> **Pagination owns `limit`/`offset`.** GET endpoints are paginated by default (`with-pagination: true`, 100 rows per page, body `{next, total_count, data}`), and `?limit=`/`?offset=` are reserved for it. Never write `LIMIT {{ params.limit }}` in a template: it applies before flAPI's offset, so `?limit=2&offset=2` returns `data: []`.

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
    regex: "^[a-zA-Z0-9_]+$"

  - type: email
  - type: uuid
  - type: enum
    allowedValues: ["active", "inactive", "pending"]
  - type: date
    min: "2020-01-01"
    max: "2025-12-31"
```

**Security Strategy (Defense in Depth):**
1. **Validators**: First line (whitelist validation)
2. **Quoting discipline in the template**: triple braces inside single-quoted
   SQL literals. Note this is NOT escaping — see the security note in §3.
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
- `{{cache.snapshotTimestamp}}` - This refresh time (`{{cache.currentSnapshotTimestamp}}` does not exist and renders empty)

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
    required: false               # the type comes from the validator; `field-type` has no effect (startup warns)
    validators:
      - type: int
        min: 1

# SQL execution
template-source: customers.sql     # Relative to sqls/ dir
connection: [data-source-name]     # From connections in flapi.yaml

# Caching (optional)
cache:
  enabled: true
  schedule: 6h                     # how often to refresh
  # Mode is INFERRED: no primary-key/cursor = full refresh;
  # cursor only = append; primary-key + cursor = merge.
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

# Authorization (optional). Without this block the top-level `auth:` of
# flapi.yaml applies; this block REPLACES it (no merge); `auth: {enabled: false}`
# opts this endpoint out.
auth:
  enabled: true
  type: basic                      # basic | bearer (alias: jwt) | oidc
  roles: [admin, user]             # caller needs one of them, else 403; empty = any authenticated caller
```

**SQL Template with Mustache** (`sqls/endpoint_name.sql`):
```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND customer_id = {{ params.id }}
{{/params.id}}
{{#params.status}}
  AND status = '{{{ params.status }}}'
{{/params.status}}
```

Template variables available:
- `params.*`: Query/path/body parameters from request
- `conn.*`: Connection properties (paths, credentials) of the endpoint's first connection
- `auth.*`: Authentication context (`auth.username`, `auth.roles`, `auth.email`, `auth.type`, `auth.authenticated`)
- `context.conn.*` / `context.auth.*` do **not** exist and render as empty strings

### 6. Self-Packaging (single-binary deploy)

The same `flapi` binary that serves the API can also fold an entire
config tree into itself, producing a self-contained executable
deployable via `scp`.

```bash
# Pack a config tree into a new bundled binary
flapi pack --in ./examples --out flapi-prod

# Inspect what's bundled
./flapi-prod info

# Extract the bundle for debugging
./flapi-prod unpack --to /tmp/extracted

# Run it -- serves the bundled config from any cwd
cd /tmp && ./flapi-prod
```

**How it works:**

- A ZIP archive is appended after the executable (or, on macOS,
  written into a reserved `__FLAPI/__bundle` Mach-O segment that
  was allocated at link time -- 16 MiB default, knob
  `FLAPI_RESERVED_BUNDLE_MIB`). Mach-O is re-codesigned so the
  output is notarisable.
- At startup, `bundle_locator` either reverse-scans the EOCD
  signature from EOF (Linux / Windows) or probes the reserved
  section (macOS). On hit, entries are decompressed once into a
  shared `ArchiveEntries`.
- `EmbeddedArchiveFileProvider` (implements `IFileProvider`) serves
  config / SQL templates from that map. `FileProviderFactory`
  dispatches non-remote paths to it when a bundle is present.
- For SQL templates that use `read_csv()` / `read_parquet()`, an
  `EmbeddedFileSystem` is registered with DuckDB on the `embed://`
  scheme, so `read_csv('embed://data/cities.csv')` resolves to the
  same in-memory bytes.

**Extensions can go in the bundle.** `flapi pack --with-extensions` installs
the config's DuckDB extensions (host platform) and embeds them under
`.flapi/extensions/`. At startup they are extracted once to
`$TMPDIR/flapi-extensions/ext-<hash>/`, used as `extension_directory`, and
offline extensions are on (`FLAPI_EXTENSIONS_OFFLINE=0` overrides).

**Secrets never go in the bundle.** `pack` refuses files matching
`*.env`, `secrets/*`, `*.pem`, `*.key` by default. Credentials come
from environment variables at runtime (`AWS_*`, `GOOGLE_*`, `AZURE_*`,
`FLAPI_CONFIG_SERVICE_TOKEN`, `{{env.VARNAME}}` interpolation in
YAML). See [DESIGN_DECISIONS §9](docs/spec/DESIGN_DECISIONS.md#9-self-packaging-via-appended-zip)
for the rationale and [CLI_REFERENCE §3](docs/CLI_REFERENCE.md#3-self-packaging-subcommands)
for full subcommand options.

**Reproducibility.** Set `SOURCE_DATE_EPOCH` (epoch seconds) before
`flapi pack` and the output is bit-identical across runs.

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
```

**Why This Works:**
- `WHERE 1=1` allows adding any number of AND conditions
- Each condition wrapped in conditional section (only rendered if parameter exists)
- No `LIMIT`: default pagination adds it (see the warning in §3)
- Triple braces for strings, double for numbers
- Injection defence is the validator (plus prepared-statement binding for a bare `{{ params.x }}` outside sections); neither brace form escapes SQL

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
AND region = '{{#params.region}}{{{ params.region }}}{{/params.region}}{{^params.region}}EU{{/params.region}}'
-- Use params.region if provided, otherwise default to EU
```
(Not for `limit`/`offset`: those are reserved for pagination.)

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
WHERE updated_at > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'
{{/cache.previousSnapshotTimestamp}}
-- Only refresh rows changed since last snapshot
```

## Commit Message Guidelines

### Attribution Policy

**Never include Claude Code attribution in commit messages.**

Do NOT add lines like:
```
Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
```

Rationale: Work is owned and authored by the human developer, even when AI assistance is used. AI is a tool, not a co-author.

### Commit Message Format

Commit messages should follow this format:

```
feat|fix|chore|docs|test: Brief description (imperative mood)

- Bullet point explaining what changed
- Why it changed (if not obvious)
- Any relevant issue references (#123)
```

**Examples:**
```
feat: Add VFS support for cloud storage configuration
- Enable S3, GCS, Azure paths in config files
- Implement PathSchemeUtils for scheme detection
- Add LocalFileProvider and DuckDBVFSProvider

chore: Update documentation for reference structure
- Consolidate authentication references
- Add cross-reference sections to all docs
- Create REFERENCE_MAP.md for navigation

fix: Correct template expansion in DuckDB queries
- Use triple braces for string parameters
- Fix null handling in optional fields
- Closes #42
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

**Python Environment:** Always use `uv` for Python virtual environments:
```bash
cd test/integration
uv venv                            # Create virtual environment
uv pip install -e .                # Install dependencies from pyproject.toml
source .venv/bin/activate          # Activate environment
```

**Test types:**
- `*.tavern.yaml`: REST API tests using Tavern specification
- `*.py`: Custom Python tests (MCP, async, complex scenarios)

**Running tests:**
```bash
# Manual test runs (after uv environment setup)
cd test/integration
source .venv/bin/activate
pytest test_customers.tavern.yaml -v           # Single test file
pytest test_mcp_integration.py::test_name -v   # Single test function

# Or use make targets (handles environment automatically)
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

- Embedded in-process OLAP database (v1.5.5)
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
  schedule: 6h                     # how often to refresh
  # Mode is INFERRED from primary-key/cursor - there is no `refresh:` key.
  table: cache_table_name          # Where to store cached results
  template-file: cache_populate.sql  # The populate SQL (there is no refresh_query key)
```

**Cache Refresh Lifecycle:**
1. HeartbeatWorker checks `cache.schedule` (the scheduler is on by default; `ducklake.scheduler.enabled: false` opts out). There is no refresh at boot beyond warm-up: a cache is due one `schedule` after its last refresh
2. On refresh trigger: run the `template-file` **verbatim**, all statements in one transaction (a failure changes nothing)
3. Full refresh: the template must write the table itself, e.g. `CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS SELECT ...` (a bare `SELECT` statement is refused before anything runs, and a script that leaves no cache table fails and rolls back)
4. Incremental: `INSERT INTO ...` (append) or a single-action `MERGE` (merge), behind a `{{^cache.previousSnapshotTimestamp}}` bootstrap that creates the table on the first run
5. Requests always read the cache table; a GET never triggers a refresh. A cache whose first build failed answers `503 {"error":"cache_build_failed"}`
6. Full rebuild on demand: `flapii cache refresh <path> --full` / `?full=true` / `flapi cache refresh --full` drops the table and rebuilds it with the watermark erased

**Materialized Results:**
- Cache tables live in the DuckLake catalog (`ducklake.metadata-path`), not in `duckdb.db_path`
- Data is written as Parquet files under `ducklake.data-path`
- Fast serving from local storage (millisecond latency)

## Configuration Reference

### Main Configuration (`flapi.yaml`)

```yaml
# Project metadata
project-name: my-project
project-description: Description
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

# Logging (top level, not nested under `server:`)
log-level: info                     # debug, info, warning (or warn), error; anything else stops startup
log-format: text                    # text | json

# MCP server (its own top-level block). No port of its own: MCP is served on
# the HTTP port, at /mcp/jsonrpc.
mcp:
  enabled: true

# Global rate limiting (optional). Note the underscore here and the HYPHEN
# in the per-endpoint `rate-limit:` block - they genuinely differ.
rate_limit:
  enabled: true
  max: 100                          # requests per interval
  interval: 60                      # seconds

# Tracing (optional, off by default, read once at startup)
tracing:
  enabled: false
```

> **Do not trust this snippet over the code.** It is illustrative; the
> authoritative key list is [docs/CONFIG_REFERENCE.md](docs/CONFIG_REFERENCE.md),
> and the parser in `src/config_manager.cpp` is the ground truth. Before
> documenting any key, grep for a *reader* of it, not just a parser.

### Environment Variables

```bash
# Configuration
FLAPI_CONFIG=path/to/flapi.yaml     # Config file path
FLAPI_LOG_LEVEL=debug|info|warning|warn|error
FLAPI_PORT=8080                      # HTTP port (fallback for --port)
FLAPI_HOST=0.0.0.0                   # Bind address (fallback for --host)

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
         user: '{{env.DB_USER}}'
         password: '{{env.DB_PASSWORD}}'
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
    validators:
      - type: string
        max-length: 100
template-source: my_endpoint.sql
connection: [data-source]
```

2. **Create SQL template** (`sqls/my_endpoint.sql`):
```sql
SELECT * FROM table
WHERE column = {{ params.param1 }}   -- bound as a prepared parameter: no quotes
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
  schedule: 6h
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
  - `conn.*` - Connection properties
  - `auth.*` - Auth context
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
- **C++20 project-wide, with one deliberate exception**: DuckDB's subdirectory is
  built at C++17. abseil (via opentelemetry-cpp) exports `cxx_std_20`, and mixing
  standards across the link produced a real segfault; but C++20 removed
  `std::uncaught_exception()`, which DuckDB calls behind a `__cplusplus` guard
  that MSVC defeats. `CMakeLists.txt` saves and restores `CMAKE_CXX_STANDARD`
  around `add_subdirectory(duckdb)`. **Do not "simplify" this** — see
  `docs/spec/DESIGN_DECISIONS.md § 10c`. `scripts/check_cxx_standard_uniform.sh`
  guards the rest.
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
| **DuckDB 1.5.5** | In-process OLAP database |
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
  token-key: '{{env.JWT_SECRET}}'
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

- Use full refresh (declare neither `primary-key` nor `cursor`) for small datasets
- Use incremental append (declare a `cursor`) for large append-only data
- Set `cache.schedule` based on data freshness requirements (there is no TTL)
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
# Port: 8080 serves both REST and MCP (/mcp/jsonrpc)
# Published tags: :latest and :v<version> from releases only,
# :main and :sha-<commit> from main; pull requests push nothing.
```

### Single Binary

```bash
make release                        # Build optimized release binary
./build/release/flapi --config flapi.yaml
```

The binary is statically linked with all dependencies and can be deployed to any Linux/macOS/Windows system without additional runtime requirements.

### PyPI Wheels

Both the `flapi` server and the `flapii` CLI ship inside a **single** PyPI
package, `flapi-io`. Installing it provides both console commands:

```bash
pip install flapi-io    # installs both `flapi` (server) and `flapii` (CLI)
```

The `flapi_io` wheel bundles both binaries (`flapi_io/bin/flapi` and
`flapi_io/bin/flapii`) and declares console-script entry points for each
(`flapi = flapi_io:main_flapi`, `flapii = flapi_io:main_flapii`). Wheels are
built automatically during release using
[bin-to-wheel](https://github.com/DataZooDE/bin-to-wheel).

**Package name on PyPI:**
- `flapi-io` — the only published package ("flapi" was taken on PyPI); carries
  both the server and the CLI. There is **no** separate `flapii` PyPI package.

## Release Process

Versioning: `v*` tags (e.g., `v0.5.0`). Tag push triggers the full release pipeline.

### Cross-Platform Targets

| Component | Platform | Build method | Artifact name |
|-----------|----------|-------------|---------------|
| flapi (server) | Linux x86_64 | Docker cross-compile | `flapi-linux-amd64` |
| flapi (server) | Linux ARM64 | Docker cross-compile | `flapi-linux-arm64` |
| flapi (server) | macOS ARM64 | Native (macos-latest) | `flapi-macos-arm64` |
| flapi (server) | Windows x64 | MSVC (windows-latest) | `flapi-windows-amd64` |
| flapii (CLI) | Linux x86_64 | `bun build --compile` | `flapii-linux-amd64` |
| flapii (CLI) | Linux ARM64 | `bun build --compile` | `flapii-linux-arm64` |
| flapii (CLI) | macOS ARM64 | `bun build --compile` | `flapii-macos-arm64` |
| flapii (CLI) | Windows x64 | `bun build --compile` | `flapii-windows-amd64` |

### Release Steps

1. Ensure all CI builds pass on `main` (`gh run list`)
   - In the release-prep commit, after rolling `## Unreleased` into `## v{version}`,
     stamp the Claude Code plugin: `python3 scripts/check_skills.py --set-version {version}`
     (rewrites plugin.json, every SKILL.md `version:` and banner), then
     `python3 scripts/check_skills.py --static-only` must pass.
2. Tag and push: `git tag v{version} && git push origin v{version}`
3. `.github/workflows/build.yaml` triggers on `v*` tag push (there is no separate `release.yaml`):
   - Downloads build artifacts (4 flapi server + 4 flapii CLI binaries)
   - Creates archive assets for the **flapi server only** (`.tar.gz` for Unix, `.zip` for Windows)
   - Builds **4** `flapi_io` Python wheels via `bin-to-wheel`, each bundling both the flapi and flapii binaries for that platform
   - Creates the GitHub Release with the 4 server archives, the wheels and `server.json`. The flapii binaries are **not** attached as standalone release assets; they ship only inside the `flapi-io` wheels (the `flapii-*` names in the table above are CI artifact names)
   - Publishes the `flapi-io` wheels to PyPI via trusted publishing (OIDC)

### PyPI Trusted Publishing Setup

Uses OIDC trusted publishing — no API tokens needed. Configuration:

| Package | GitHub Environment | PyPI Pending Publisher |
|---------|-------------------|----------------------|
| `flapi-io` | `pypi-flapi-io` | DataZooDE/flapi, build.yaml, pypi-flapi-io |

Only `flapi-io` is published (it carries both the server and the CLI), so a single trusted-publisher environment is needed.

### Wheel Output

Each release produces **4** `flapi_io` wheels (one per platform), each bundling both the `flapi` and `flapii` binaries:

| Package | Platform | Wheel platform tag |
|---------|----------|--------------------|
| `flapi-io` | Linux x86_64 | `manylinux_2_17_x86_64` |
| `flapi-io` | Linux ARM64 | `manylinux_2_17_aarch64` |
| `flapi-io` | macOS ARM64 | `macosx_11_0_arm64` |
| `flapi-io` | Windows x64 | `win_amd64` |

### flapii Build Process

The `flapii` CLI is a TypeScript project (Commander.js) compiled to standalone binaries using Bun's `--compile` flag. The `flapii-build` job in `build.yaml` runs a matrix build across 4 targets:

```bash
bun build --compile --target=bun-linux-x64 src/index.ts --outfile flapii
bun build --compile --target=bun-linux-arm64 src/index.ts --outfile flapii
bun build --compile --target=bun-darwin-arm64 src/index.ts --outfile flapii
bun build --compile --target=bun-windows-x64 src/index.ts --outfile flapii.exe
```

Each produces a self-contained binary with no runtime dependencies (Bun runtime is embedded).

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
- Scheduler opted out (`ducklake.scheduler.enabled: false`; startup warns)
- Template error in cache populate SQL (a bare `SELECT` is refused: the template must write the table)
- DuckLake not initialized
- Cache table doesn't exist

**Solution:**
```bash
# Check cache configuration
flapii cache get /endpoint

# Refresh now, or drop and rebuild from scratch
flapii cache refresh /endpoint
flapii cache refresh /endpoint --full

# Refresh history: mode, status, duration, rows, snapshot
flapii cache audit /endpoint -o table

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
# Expire old snapshots per retention and delete their files (path is required)
flapii cache gc /endpoint

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

**Problem:** `401 Unauthorized`, `403 Forbidden` or JWT validation errors

**Causes:**
- Missing or invalid JWT secret
- Token expired or malformed
- Auth not enabled for endpoint (or inherited from the top-level `auth:` when the endpoint has no block)
- Wrong authentication type
- `403`: authenticated, but the principal holds none of the endpoint's `auth.roles`

**Solution:**
```bash
# Check if auth is enabled for endpoint (`inherited: true` = the top-level auth applies)
flapii endpoints get /endpoint | jq '.auth'

# Verify JWT secret is set
echo $JWT_SECRET  # Must not be empty

# Check token format
# Should be: Authorization: Bearer <token>

# Verify auth configuration
cat flapi.yaml | grep -A 10 "auth:"

# Test without auth (temporary)
# Set `auth: {enabled: false}` on the endpoint (removing the block inherits the top-level auth)
```

#### SQL Syntax Errors

**Problem:** `400 Bad Request` or `500 Internal Server Error`. A 500 body carries only a `request_id`; grep the server log for it to see the SQL error

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


## Documentation Maintenance

### When to Update Documentation

After making code changes, update the relevant documentation:

| Change Type | Documents to Update |
|-------------|---------------------|
| New component or major refactor | `docs/spec/ARCHITECTURE.md`, relevant component doc |
| API endpoint changes | `docs/CONFIG_SERVICE_API_REFERENCE.md` |
| Configuration options | `docs/CONFIG_REFERENCE.md` |
| MCP protocol changes | `docs/MCP_REFERENCE.md` |
| CLI command changes | `docs/CLI_REFERENCE.md` |
| Design pattern changes | `docs/spec/DESIGN_DECISIONS.md` |
| Request flow changes | `docs/spec/REQUEST_LIFECYCLE.md` |
| Config system changes | `docs/spec/components/config-system.md` |
| Query/DuckDB changes | `docs/spec/components/query-execution.md` |
| Cache system changes | `docs/spec/components/caching.md` |
| MCP implementation changes | `docs/spec/components/mcp-protocol.md` |
| Auth/security changes | `docs/spec/components/security.md` |
| Tracing/audit/logging changes | `docs/OBSERVABILITY.md`, `docs/spec/components/observability.md` |
| A new user-facing capability | a guide in `docs/guides/`, linked from `docs/README.md` |

### Documentation Checklist

Before completing work that modifies code:

- [ ] If architecture changed → update `docs/spec/ARCHITECTURE.md`
- [ ] If new design decision → add to `docs/spec/DESIGN_DECISIONS.md`
- [ ] **Every config key you document, grep for a *reader* in `src/`, not just a
      parser.** Three keys once shipped documented and parsed but never read;
      operators could set them and nothing happened.
- [ ] If request flow changed → update `docs/spec/REQUEST_LIFECYCLE.md`
- [ ] If component internals changed → update relevant `docs/spec/components/*.md`
- [ ] If user-facing API changed → update relevant reference doc in `docs/`

### Documentation Style

- Use Mermaid diagrams for visual architecture
- Include source file references (e.g., `src/config_manager.cpp:123`)
- Keep explanations concise and code-focused
- Update "Last updated" timestamps when making significant changes

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Open GitHub issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds

## GitHub Issue Workflow

When working on GitHub issues, use feature branches and pull requests to enable code review and maintain a clean main branch history.

**Workflow Steps:**

1. **Create feature branch from main:**
   ```bash
   git checkout main
   git pull origin main
   git checkout -b feature/gh-<issue-number>-<short-description>
   # Example: git checkout -b feature/gh-9-arrow-streaming
   ```

2. **Work and commit on feature branch:**
   - Make changes, run tests
   - Commit with descriptive messages
   - Reference GitHub issue: `git commit -m "feat: Add Arrow streaming support (#9)"`

3. **Create pull request:**
   ```bash
   git push -u origin feature/gh-<issue-number>-<short-description>
   gh pr create --title "..." --body "Closes #<issue-number>..."
   ```

4. **After PR merge:**
   ```bash
   git checkout main
   git pull origin main
   git branch -d feature/gh-<issue-number>-<short-description>
   ```

**Branch Naming Convention:**
- `feature/gh-<N>-<description>` - For GitHub issue work
- `fix/gh-<N>-<description>` - For bug fixes
- `chore/gh-<N>-<description>` - For maintenance tasks

**PR Description Template:**
```markdown
## Summary
<1-3 bullet points>

## Test plan
- [ ] Tests added/updated
- [ ] Manual verification steps

Closes #<issue-number>
```
