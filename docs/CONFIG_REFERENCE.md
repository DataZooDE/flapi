# flAPI Configuration Reference

**flAPI Version:** v26.11.26 (releases are numbered `vYY.MM.DD`)
**DuckDB Version:** 2.0.1

This document provides a complete reference for all configuration options in flAPI, the SQL-to-API framework that generates REST APIs and MCP tools from SQL templates.

---

## Table of Contents

1. [Overview](#1-overview)
   - [Configuration Philosophy](#configuration-philosophy)
   - [File Organization](#file-organization)
   - [Quick Start](#quick-start)
2. [Main Configuration (flapi.yaml)](#2-main-configuration-flapiyaml)
   - [2.1 Project Metadata](#21-project-metadata)
   - [2.2 Template Configuration](#22-template-configuration)
   - [2.3 Connections](#23-connections)
   - [2.4 DuckDB Configuration](#24-duckdb-configuration)
   - [2.4.1 DuckDB Hardening (`duckdb-hardening`)](#241-duckdb-hardening-duckdb-hardening)
   - [2.5 DuckLake Configuration](#25-ducklake-configuration)
   - [2.6 MCP Configuration](#26-mcp-configuration)
   - [2.7 Authentication (Global)](#27-authentication-global)
   - [2.8 Rate Limiting (Global)](#28-rate-limiting-global)
   - [2.9 HTTPS Configuration](#29-https-configuration)
   - [2.9.1 CORS](#291-cors)
   - [2.9.2 Audit Log](#292-audit-log)
   - [2.10 Heartbeat Configuration](#210-heartbeat-configuration)
   - [2.11 Storage Configuration (VFS)](#211-storage-configuration-vfs)
   - [2.12 Telemetry Configuration](#212-telemetry-configuration)
3. [Endpoint Configuration](#3-endpoint-configuration)
   - [3.1 REST Endpoints](#31-rest-endpoints)
   - [3.2 MCP Tools](#32-mcp-tools)
   - [3.3 MCP Resources](#33-mcp-resources)
   - [3.4 MCP Prompts](#34-mcp-prompts)
   - [3.5 Configuration Includes](#35-configuration-includes)
4. [Request Parameters](#4-request-parameters)
   - [4.1 Field Definition](#41-field-definition)
   - [4.2 Parameter Locations](#42-parameter-locations)
   - [4.3 Default Values](#43-default-values)
5. [Validators](#5-validators)
   - [5.1 Integer Validator](#51-integer-validator)
   - [5.2 String Validator](#52-string-validator)
   - [5.3 Enum Validator](#53-enum-validator)
   - [5.4 Email Validator](#54-email-validator)
   - [5.5 UUID Validator](#55-uuid-validator)
   - [5.6 Date Validator](#56-date-validator)
   - [5.7 Time Validator](#57-time-validator)
6. [Cache Configuration](#6-cache-configuration)
   - [6.1 Basic Cache Settings](#61-basic-cache-settings)
   - [6.2 Cache Readiness During Warmup](#62-cache-readiness-during-warmup)
   - [6.3 Refresh Modes](#63-refresh-modes)
   - [6.4 Retention Policies](#64-retention-policies)
   - [6.5 Cache Template Variables](#65-cache-template-variables)
7. [Authentication](#7-authentication)
   - [7.1 Basic Authentication](#71-basic-authentication)
   - [7.2 JWT Authentication](#72-jwt-authentication)
   - [7.3 Bearer Authentication](#73-bearer-authentication)
   - [7.4 OIDC Authentication](#74-oidc-authentication)
   - [7.5 AWS Secrets Manager](#75-aws-secrets-manager)
8. [Operation Configuration](#8-operation-configuration)
   - [8.1 Read Operations](#81-read-operations)
   - [8.2 Write Operations](#82-write-operations)
9. [SQL Templates (Mustache)](#9-sql-templates-mustache)
   - [9.1 Variable Syntax](#91-variable-syntax)
   - [9.2 Available Contexts](#92-available-contexts)
   - [9.3 Conditional Sections](#93-conditional-sections)
   - [9.4 Best Practices](#94-best-practices)
10. [Environment Variables](#10-environment-variables)
    - [10.1 Substitution Syntax](#101-substitution-syntax)
    - [10.2 Whitelist Configuration](#102-whitelist-configuration)
- [Appendix A: Complete Example Configuration](#appendix-a-complete-example-configuration)
- [Appendix B: Naming Conventions](#appendix-b-naming-conventions)
- [Appendix C: Default Values Reference](#appendix-c-default-values-reference)

---

## 1. Overview

### Configuration Philosophy

flAPI follows a **declarative configuration philosophy**:

- **YAML-based**: All configuration uses human-readable YAML format
- **SQL-first**: Business logic lives in SQL templates, not compiled code
- **Separation of concerns**: Main config, endpoint definitions, and SQL templates are separate files
- **Convention over configuration**: Sensible defaults reduce boilerplate
- **Environment-aware**: Support for environment variables enables dev/staging/prod configurations

### File Organization

A typical flAPI project has the following structure:

```
project/
├── flapi.yaml              # Main configuration file
├── sqls/                   # Endpoint definitions and SQL templates
│   ├── customers.yaml      # Endpoint configuration
│   ├── customers.sql       # SQL template
│   ├── orders.yaml
│   ├── orders.sql
│   └── common/             # Shared configuration fragments
│       └── auth.yaml
├── data/                   # Local data files (Parquet, CSV, etc.)
└── cache/                  # DuckLake cache storage (auto-created)
```

### Quick Start

Minimal configuration to get started:

```yaml
# flapi.yaml
project-name: my-api

template:
  path: ./sqls

connections:
  local-data:
    properties:
      path: ./data/customers.parquet

duckdb:
  access_mode: READ_WRITE
```

```yaml
# sqls/customers.yaml
url-path: /customers
method: GET
template-source: customers.sql
connection:
  - local-data
```

```sql
-- sqls/customers.sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
{{#params.id}}
WHERE id = {{ params.id }}
{{/params.id}}
LIMIT 100
```

---

### 1.4 12-factor checklist (environment variables)

flapi follows the [12-factor app](https://12factor.net/) principle of
configuration via the environment for the bits a deployment artifact
should not bake in.

| Env var | Read at | Effect | Precedence |
|---------|---------|--------|------------|
| `FLAPI_CONFIG` | startup | Path to `flapi.yaml` (fallback for `-c`) | CLI > env > `flapi.yaml` default |
| `FLAPI_PORT` | startup | HTTP server port (fallback for `-p` / `--port`) | CLI > env > `http-port` config > `8080`; invalid values exit non-zero |
| `FLAPI_HOST` | startup | Bind address (fallback for `--host`) | CLI > env > `http-host` config > `0.0.0.0` |
| `FLAPI_LOG_LEVEL` | startup | Log verbosity (fallback for `--log-level`) | CLI > env > `log-level` config > `info` default; invalid values exit non-zero |
| `FLAPI_CONFIG_SERVICE_TOKEN` | startup | Bearer token for the management API (fallback for `--config-service-token`) | CLI > env > auto-generate |
| `FLAPI_NO_TELEMETRY` | startup | Disable PostHog telemetry (fallback for `--no-telemetry`) | CLI > env > config-file > enabled |
| `FLAPI_LOG_HTTP_REQUESTS` | startup | `1`/`true`/`yes`/`on` logs every DuckDB HTTP request (see `log-http-requests`); `0`/`false`/`no`/`off` turns it off. Also covers what is read before the config key: the fetch of a remote `--config` and the YAML includes of a local one | env > `log-http-requests` config > off |
| `FLAPI_NO_CONNECTION_REUSE` | startup | `1`: a fresh DuckDB connection per query instead of per-thread reuse with cached prepared statements | env > reuse on |
| `AWS_ENDPOINT_URL_SECRETS_MANAGER` | startup | Secrets Manager endpoint for `auth.from-aws-secretmanager` (warned about; `AWS_ENDPOINT_URL` is not used for it) | env > AWS; `AWS_IGNORE_CONFIGURED_ENDPOINT_URLS=true` ignores it |
| `FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT` | startup | `1`/`true`/`yes`: accept a plain-`http://` Secrets Manager endpoint that is not loopback | refused without it |
| `DUCKDB_JE_MALLOC_CONF` | startup | DuckDB allocator options (Linux); flAPI sets `narenas:<usable CPUs / 6>` only when this is unset | env > flAPI default |
| `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` / `AWS_REGION` | startup, query time | S3 credentials (DuckDB `httpfs`) | env only |
| `GOOGLE_APPLICATION_CREDENTIALS` / `GOOGLE_CLOUD_PROJECT` | startup | Detected and logged only. flAPI does **not** configure GCS for DuckDB from them, and DuckDB `httpfs` does not read a service-account file: GCS access needs an HMAC key in a DuckDB secret (`CREATE SECRET (TYPE gcs, KEY_ID '…', SECRET '…')`, e.g. in a connection's `init`) | — |
| `AZURE_STORAGE_CONNECTION_STRING` / `AZURE_STORAGE_ACCOUNT` / `AZURE_STORAGE_KEY` | startup, query time | Azure Blob credentials | env only |
| `{{env.VARNAME}}` in YAML | startup (config parse) | Interpolated into any string field | requires `environment-whitelist` entry |

Secrets should always come from the environment, never from a config
file checked into version control. When using the self-packaging
feature (`flapi pack`), the default secret deny list refuses to
bundle `*.env`, `secrets/*`, `*.pem`, and `*.key` files -- enforcing
this same principle at packaging time.

---

## 2. Main Configuration (flapi.yaml)

The main configuration file defines global settings, connections, and server behavior.

A top-level key that flAPI does not read (a typo, a key from another product, your own annotation)
is **ignored with a startup warning** naming it, not silently. `version:` is accepted as metadata.
The list of known keys is `ConfigManager::KnownTopLevelKeys()` in `src/config_manager.cpp`.

### 2.1 Project Metadata

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `project-name` | string | - | Human-readable project name |
| `project-description` | string | `""` | Project description (optional) |
| `server-name` | string | - | **No effect**: nothing reads it, and startup warns. The bind address is `http-host` |
| `http-port` | integer | `8080` | HTTP server port (overridable via `--port` / `FLAPI_PORT`) |
| `log-level` | string | `info` | Log verbosity: `debug`, `info`, `warning` (or `warn`), `error`. Overridable via `--log-level` / `FLAPI_LOG_LEVEL` (both also accept `warn`). Any other value stops startup. |
| `tracing` | map | disabled | OpenTelemetry tracing. Off by default; see [OBSERVABILITY.md](OBSERVABILITY.md). |
| `log-format` | string | `text` | `text` for human-readable lines, `json` for one JSON object per line, from the first startup line on (lines logged while the configuration is read are held until it says which format). Both carry `request_id` (and `trace_id` once tracing is enabled) for lines emitted while serving a request, Crow's `Response: ...` access line included. |
| `log-http-requests` | bool | `false` | **Debugging only.** Log one `info` line per HTTP request DuckDB makes (S3, GCS, HTTP(S); Azure unverified): `duckdb http <METHOD> <url> -> <status\|ERR> <N>ms`, with the `request_id` of the request that caused it, or `-`. The URL keeps scheme, host and path; userinfo is dropped and the whole query becomes `?redacted`, whatever `redact_http_logs` says. Secrets in URL *paths* (Slack webhooks, bot tokens) are not redacted, and request parameters that reach a path land in the log. Every request is a synchronous write under DuckDB's logging lock, so volume grows with glob width. Takes over DuckDB's logging config (`duckdb_logs` stays empty). `FLAPI_LOG_HTTP_REQUESTS` wins when set. Off, DuckDB logging is never enabled. See [OBSERVABILITY.md §8](OBSERVABILITY.md#which-object-store-requests-did-a-request-cause). |
| `stall-timeout-s` | int | `60` | Readiness (`GET /health`) returns `503` when a request has been in flight longer than this. `0` disables it. Guards against a backend that stops answering without failing — a wedged connection leaves the process, DuckDB and every other connection healthy, so a `SELECT 1` probe passes while the endpoint is dead. Raise it if you serve deliberately long synchronous queries; the usual answer to those is the MCP Tasks extension. |
| `http-host` | string | `"0.0.0.0"` | Bind address (overridable via `--host` / `FLAPI_HOST`); use `127.0.0.1` to restrict to loopback |


> **There is no `server:` block.** The HTTP server is configured by the
> top-level `http-port` and `http-host` keys (or `-p/--port`, `--host`,
> `FLAPI_PORT`, `FLAPI_HOST`). A config that contains `server:` — in any form —
> is rejected at startup with an error naming those keys. Earlier examples used
> `server: {port, host}`; nothing ever read it, so the port silently stayed at
> 8080.

> **How many connections flAPI serves in parallel.** flAPI runs a request
> handler on the server thread that owns its connection, and uses
> `max(CPU cores, 8)` threads (capped at 64) — not one per core. The floor
> matters on small instances: a slow *synchronous* query occupies its thread
> for the whole query, and other connections landing on that thread wait
> behind it. Measured with one ~5s query in flight and 40 concurrent
> `GET /health` probes: with a single server thread **all 40 waited out the
> query and none reported the stall**; with seven, 35 of 40 answered
> `503 stalled` immediately. The floor makes that unlikely rather than
> impossible — see
> [#120](https://github.com/DataZooDE/flapi/issues/120). It is not
> configurable, and it is unrelated to `duckdb.threads`, which sets query
> parallelism inside DuckDB.

**Example:**

```yaml
project-name: Customer API
project-description: REST API for customer data access
http-host: 0.0.0.0
http-port: 8080
```

> **Implementation:** `src/config_manager.cpp` | **Tests:** `test/cpp/config_manager_test.cpp`

### 2.2 Template Configuration

Defines where endpoint configurations and SQL templates are located.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `template.path` | string | **required** | Directory containing endpoint YAML and SQL files |
| `template.environment-whitelist` | array[string] | `[]` | Regex patterns (full match, case-sensitive) for the environment variables `{{env.NAME}}` may read. **Empty or missing allows none.** |

**Example:**

```yaml
template:
  path: ./sqls
  environment-whitelist:
    - '^FLAPI_.*'
    - '^DB_.*'
    - '^API_KEY$'
```

**Notes:**
- The `path` is relative to the main configuration file location
- **One whitelist governs every `{{env.NAME}}`**: in SQL templates, in YAML configuration
  files (the main config, endpoint files and files pulled in with `{{include}}`) and in
  `env.NAME` include conditions. It lives here, under `template:`, and nowhere else.
- A user in an `auth.users` list with an **empty** username or password stops startup. An unset
  variable substitutes as an empty string, and an empty credential must never be accepted.
- A variable must match at least one pattern. **An empty or missing whitelist allows no
  variables.** Patterns are regular expressions matched against the whole name and are
  **case-sensitive** (`AWS_REGION` and `aws_region` are different variables), so `API_KEY`
  is an exact match and `^FLAPI_.*` is a prefix.
- In **YAML files**, a `{{env.NAME}}` that does not match **stops startup** with an error naming
  every such variable and this key. It is never left as literal text, since a literal
  `{{env.DB_PASSWORD}}` would quietly become the password. In SQL templates an unlisted
  variable is simply not available.
- A **top-level** `environment-whitelist:` is never read and is rejected at startup; it must be
  under `template:`.
- The whitelist must be in the **main configuration file itself**, not in a file it includes: it is
  read before anything is substituted or included.
- A full-line comment can mention `{{env.NAME}}` freely; a comment after a value is still checked.
  A line starting with `#` *inside* a block scalar (`key: |`) is text, not a comment, so a variable
  on it is substituted or refused like any other.
- The `path` is relative to the main configuration file location.
- `${NAME}` is **not** a supported syntax; write `{{env.NAME}}`. In YAML files, spaces inside the braces are accepted (`{{ env.NAME }}`); the triple-brace `{{{ env.NAME }}}` form is SQL-template syntax and is left for the template pass.

> **Implementation:** `src/config_manager.cpp` (`applyEnvironmentPolicy`), `src/extended_yaml_parser.cpp`, `src/sql_template_processor.cpp` | **Tests:** `test/integration/test_environment_whitelist.py`, `test/cpp/extended_yaml_parser_test.cpp`, `test/cpp/sql_template_processor_test.cpp`

### 2.3 Connections

Connections define data sources accessible in SQL templates.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `connections.<name>` | object | - | Connection configuration keyed by unique name |
| `connections.<name>.init` | string | - | SQL initialization commands (load extensions, create secrets) |
| `connections.<name>.log-queries` | boolean | - | **No effect**: nothing reads it; startup warns |
| `connections.<name>.log-parameters` | boolean | - | **No effect**: nothing reads it; startup warns |
| `connections.<name>.allow` | string | - | **No effect**: nothing reads it; startup warns |
| `connections.<name>.serialize-access` | boolean | auto | Run one query at a time on this connection. Auto-detected for SQLite attachments; see below |
| `connections.<name>.properties` | object | - | Custom key-value properties accessible in templates |

**Example:**

```yaml
connections:
  # Local file connection
  customers-parquet:
    properties:
      path: ./data/customers.parquet

  # PostgreSQL connection
  postgres-db:
    init: |
      INSTALL postgres;
      LOAD postgres;
    properties:
      host: db.example.com
      port: '5432'
      database: mydb
      user: '{{env.DB_USER}}'
      password: '{{env.DB_PASSWORD}}'

  # BigQuery connection
  bigquery-data:
    init: |
      INSTALL 'bigquery' FROM community;
      LOAD 'bigquery';
    properties:
      project_id: my-gcp-project
```

**Notes:**
- Connection properties are accessible in templates as `{{{ conn.property_name }}}` (triple braces: `{{ }}`
  HTML-escapes `/` and `'`), for the endpoint's first listed connection only
- The `init` SQL runs once at startup, in the shared DuckDB, before the external-access policy applies; it is
  not a template (`{{ conn.x }}` inside it stays literal text). Relative paths in `init` resolve against the
  server's working directory, while `./` paths in `properties` resolve against the config file's directory

**Serialised access (`serialize-access`)**

A read and a write issued at the same moment against a DuckDB **SQLite
attachment** deadlock each other. Two requests are enough, and the attachment
can stop answering for good while the process, DuckDB, the health endpoint and
every other connection stay perfectly healthy — so the instance keeps accepting
traffic it can no longer serve (#116).

flAPI therefore runs **one query at a time** on any connection whose `init`
contains an `ATTACH ... (TYPE sqlite)`. Nothing else is affected: parquet,
BigQuery, Postgres and DuckLake traffic still runs fully in parallel.

The cost is real — concurrent requests to a SQLite-backed endpoint queue rather
than overlap — so set it yourself when the detection is wrong in either
direction:

```yaml
connections:
  # An exotic backend with the same problem
  my-backend:
    serialize-access: true

  # A SQLite attachment you have measured and want to run in parallel anyway
  read-only-snapshot:
    serialize-access: false
    init: |
      ATTACH '/data/snapshot.sqlite' AS snap (TYPE sqlite);
```

A **read served from the DuckLake cache** (an endpoint with `cache.enabled` and a `cache.template-file`)
does not take the lock: its serving template reads the cache table, not the source. The populate
template and every uncached read still do. A cached endpoint without a `template-file` serves its source
query, so it keeps the lock. Do not join the source into a cached endpoint's serving template on a
serialised connection: that read would run unserialised.

An explicit value always wins over the detection.

A **cache populate** that reads the source takes the same lock for the duration of the populate script, so a
write to a SQLite source issued during a slow populate waits for it (and the populate is never run against a
concurrent writer). Lock order is always the DuckLake catalog write lock first, the source lock second; a
write releases its source lock before any `refresh-on-write` / `invalidate-on-write` refresh starts.
- Use environment variable substitution for sensitive values

> **Implementation:** `src/config_manager.cpp`, `src/database_manager.cpp` | **Tests:** `test/cpp/config_manager_test.cpp`

### 2.4 DuckDB Configuration

Configures the embedded DuckDB database engine.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `duckdb.db_path` | string | `:memory:` | Path to persistent database file |
| `duckdb.access_mode` | string | `READ_WRITE` | Database access mode: `READ_WRITE` or `READ_ONLY` |
| `duckdb.threads` | integer | auto | Number of threads for query parallelism |
| `duckdb.max_memory` | string | auto | Memory limit (e.g., `4GB`, `512MB`) |
| `duckdb.default_order` | string | - | Default sort order: `ASC` or `DESC`. With `ducklake.enabled` it is set per flAPI connection, not globally, because DuckLake's own catalog queries need the ascending default |
| `duckdb.default-extensions` | list | `[httpfs, ducklake, fts, json, postgres, sqlite, parquet]` | Extensions installed and loaded at startup. Extensions built into the binary (`core_functions`, `parquet`, `json`, `icu`) are skipped: nothing is downloaded for them. `[]` loads none. A scalar is refused. |
| `duckdb.TimeZone` | string | `UTC` | The session time zone (IANA name). Defaults to `UTC` whatever the host `TZ` is; the key is matched case-insensitively (`timezone:` works). Applied with `SET GLOBAL` once the engine is open (the setting belongs to ICU, which is not loaded when the config is read) and logged at startup as `DuckDB TimeZone: UTC`. If that fails, each connection retries with its own `SET` and a warning says so |
| `duckdb.<setting>` | any | - | Any DuckDB configuration setting |

**Example:**

```yaml
duckdb:
  db_path: ./flapi_cache.db
  access_mode: READ_WRITE
  threads: 8
  max_memory: 8GB
  default_order: DESC
```

**Notes:**
- Omit `db_path` or set to `:memory:` for an in-memory database
- Any other key-value pairs are passed directly as DuckDB settings
- **Persistent DuckDB secrets are visible to templates.** DuckDB loads the secrets in
  `~/.duckdb/stored_secrets` of the user flAPI runs as, and any template can list them (values redacted) with
  `duckdb_secrets()` and use them. `duckdb: {allow_persistent_secrets: false}` stops that; secrets flAPI or a
  connection `init:` creates are unaffected.
- `FLAPI_EXTENSION_DIRECTORY` sets `extension_directory` when the config does not (see the precedence below).
- `FLAPI_EXTENSIONS_OFFLINE=1` (also `true`/`yes`) never downloads: startup skips `INSTALL`, sets
  `autoinstall_known_extensions=false`, and **fails** if a default extension cannot be loaded, naming it and
  `flapi extensions install`. Without it a missing extension is only a warning.
- Startup `LOAD`s each default extension and `INSTALL`s (downloads) only one that no search directory has.

**Where DuckDB extensions come from** (first match wins; `src/config_manager.cpp` `parseDuckDBConfig`,
`src/extension_installer.cpp` `ShippedExtensionDirectory`):

| # | Source | Used as |
|---|--------|---------|
| 1 | `duckdb.extension_directory` (or `extension_directories`) in `flapi.yaml` | the only directory |
| 2 | `FLAPI_EXTENSION_DIRECTORY` | the only directory |
| 3 | extensions embedded by `flapi pack --with-extensions` (extracted to `$TMPDIR/flapi-extensions/ext-<hash>/`) | the only directory; offline on |
| 4 | `extensions/` **next to the flapi executable** (release archives, Docker `/app/extensions`) | searched **after** the user directory below |
| 5 | DuckDB's default, `~/.duckdb/extensions` | install target, searched first |

- Releases ship the signed default extensions (`httpfs`, `ducklake`, `fts`, `postgres_scanner`,
  `sqlite_scanner`; `core_functions`, `parquet`, `json` and `icu` are built in) in DuckDB's own layout,
  `extensions/<duckdb version>/<platform>/`. The executable's real path is used: a symlink to the binary
  (`/usr/local/bin/flapi`) works. The `flapi-io` wheels carry no `extensions/`: with it a wheel would be
  ~155 MB, above PyPI's 100 MB file limit, so a pip-installed flapi downloads on first start.
- With 4 present, DuckDB gets `extension_directories = ['~/.duckdb/extensions', '<bin>/extensions']`. DuckDB
  installs into the first directory and loads from the first that has the file, so the shipped directory is
  **only ever read** (it may be read-only), an extension outside the default set still installs into the user
  directory, and a copy you install there yourself (`FORCE INSTALL httpfs`, e.g. a fix before the next flAPI
  release) **wins** over the shipped one. A user directory copy for the same DuckDB version is the same build
  unless you installed another on purpose.
- An `extensions/` holding no files for this binary's DuckDB version and platform (left over from an older
  release) is ignored with one warning naming what it holds; startup goes on with 5.
- 1-3 replace the shipped directory instead of adding to it: an operator who points flAPI at a directory gets
  exactly that directory, and a packed binary never loads a second copy of an extension.
- Offline (`FLAPI_EXTENSIONS_OFFLINE=1`) works with the shipped directory alone.
- **macOS:** an archive downloaded with a browser gets the `com.apple.quarantine` attribute, and its unpacked
  files keep it; Gatekeeper then refuses the binary and the extensions it loads. Clear it once for the whole
  unpacked directory, binary and `extensions/` together: `xattr -dr com.apple.quarantine <unpacked dir>`
  (`curl` / `gh release download` set no quarantine).
- `core_functions`, `parquet`, `json` and `icu` are built into flAPI (DuckDB 2.0): they are never installed,
  loaded from the extension directory or packed, and need no network.
- Defaults flAPI sets unless the key is given here: `threads` = the CPUs the process may use (affinity mask and
  container CPU quota, not every host CPU); `async_threads` = 0 when no connection, `template.path`, config or
  DuckLake path names remote storage (`s3://`, `gs://`, `az://`, `http(s)://`, ...), else 2x `threads`. A template
  that reads `s3://` only inside its SQL should set `async_threads` itself.
- DuckDB's allocator (Linux) gets one arena per 6 usable CPUs (`DUCKDB_JE_MALLOC_CONF=narenas:N`); an
  operator's own `DUCKDB_JE_MALLOC_CONF` wins.
- Each request thread keeps its DuckDB connection and up to 64 prepared SELECTs between requests (a non-SELECT
  statement retires the connection). `FLAPI_NO_CONNECTION_REUSE=1` opens a fresh connection per query instead.
- Pre-install everything a config needs with `flapi extensions install --config flapi.yaml --dir <path>`
  ([CLI_REFERENCE § 3](./CLI_REFERENCE.md)).

> **Implementation:** `src/database_manager.cpp` | **Tests:** `test/cpp/database_manager_test.cpp`

### 2.4.1 DuckDB Hardening (`duckdb-hardening`)

What SQL templates may reach outside the database (#253). A **top-level** block, not under `duckdb:` (every
scalar there is handed to DuckDB as a setting).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `duckdb-hardening.external-access` | string | `auto` | `auto`, `full` or `restricted`. `auto` is `restricted` when the main config file (`--config`) is remote (`s3://`, `gs://`, `az://`, `http(s)://`, ...) and `full` otherwise; a packed bundle counts as local. Anything else stops startup naming the key |
| `duckdb-hardening.allowed-directories` | list | `[]` | Extra prefixes templates may read and write under `restricted`. Relative entries resolve against the working directory; each gets a trailing `/`; URLs (`s3://other-bucket/data/`) are kept as written |
| `duckdb-hardening.allowed-paths` | list | `[]` | Extra single files, same rules without the trailing `/` |

An unknown key inside the block, a scalar where a list belongs, or an empty entry stops startup.

```yaml
duckdb-hardening:
  external-access: restricted
  allowed-directories:
    - s3://shared-reference-data/          # another bucket the templates read
    - /srv/flapi/data
```

**What `restricted` does**, once, at the very end of startup (after the DuckLake ATTACH, connection inits and
the cloud credentials), in this order:

1. Loads the extensions flAPI needs: `httpfs`, `ducklake` (cache on), `azure` (when something names `az://`),
   every extension a connection `init` LOADs/INSTALLs, and `aws` for its `CREDENTIAL_CHAIN` (an AWS Secrets
   Manager `init` runs in a private DuckDB and is not affected). Turns
   `autoinstall_known_extensions` off.
2. `SET allowed_directories` to: the config's directory (for a remote config its URL prefix, query string
   stripped), `template.path`, the DuckLake `data-path`, `embed://` in a packed bundle, and the lists above.
   `allowed_paths` to `allowed-paths`. DuckDB adds every attached database file (main database, DuckLake
   catalog, their WALs) and its temp directory by itself.
3. `SET enable_external_access = false`: any other file, URL, bucket, ATTACH target or extension load is refused
   (`Permission Error: ... disabled by configuration`).
4. `SET lock_configuration = true` with `allowed_configs` = `TimeZone`, `default_order`, `enable_profiling`,
   `tracked_metrics` (what flAPI itself sets per connection). A template can change no other setting
   (`secret_directory`, `http_proxy`, `allowed_directories`, ...).

While restricted, every rendered template (endpoint, cache populate, write, MCP tool, config-service expand) is
parsed and checked against an **allowlist of statement types** before DuckDB sees it
(`src/external_access_policy.cpp`). Allowed: `SELECT`/`WITH`, `INSERT`, `UPDATE`, `DELETE`, `MERGE`, `CREATE`/`DROP`/
`ALTER` of tables, views, macros, schemas, sequences, indexes and types (`OR REPLACE`, `AS SELECT`), `COPY` (within the
allowed directories), transactions, `CALL` of table functions (`CHECKPOINT` is one), `VACUUM`/`ANALYZE`,
`PREPARE`/`EXECUTE` and `EXPLAIN` of an allowed statement, `SET SESSION`/`RESET SESSION`, `SET VARIABLE`, and a bare
`SET` of a setting that defaults to the session (`USE`, `schema`, `search_path`). Everything else is refused with
`... is not allowed in a template: external access is restricted (duckdb-hardening.external-access)`:
`PRAGMA` (including `IMPORT DATABASE`, which runs `<dir>/schema.sql` verbatim), `EXPORT DATABASE`, `ATTACH`/`DETACH`,
`COPY FROM DATABASE`, `INSTALL`/`LOAD`/`UPDATE EXTENSIONS`, `CREATE`/`DROP SECRET`, `SET GLOBAL`/`RESET GLOBAL`, and a
bare `SET` of a global-default setting (`SET default_order`, `SET TimeZone`: write `SET SESSION`). Wrapping does not
help: `EXPLAIN ANALYZE` and `PREPARE` are checked by their inner statement. SQL flAPI cannot parse is refused.
Why: DuckDB still lets `INSTALL ... FROM core` reach the official repository with external access off, and a
temporary secret is instance-wide (a template's `CREATE SECRET (TYPE s3, ENDPOINT ..., SCOPE 's3://your-bucket/')`
redirects every endpoint's reads of that bucket). Load extensions and create secrets at startup instead: `LOAD` in a
connection `init`, or `duckdb.default-extensions` - which **replaces** the defaults (`httpfs, ducklake, fts, json,
postgres, sqlite, parquet`), so list those too. Extensions only templates use (`excel`, `spatial`, `iceberg`,
`delta`, `aws`, ...) must be listed: nothing can load after startup. `duckdb_extensions()` fails in a template: the extension directory is
not allowed, on purpose (allowed directories are writable). `aws` is preloaded for you when a connection
`init` uses `credential_chain`; `azure` when anything names `az://`. An endpoint's `auth.from-aws-secretmanager.init`
runs in a private DuckDB with external access off and its configuration locked: it can create its secret (and
`LOAD` httpfs/aws, loaded for it beforehand), but cannot read or write files, reach the network or `INSTALL`.

Startup also loads the persistent secrets (`secret_directory` is never allowed) and allows the CA bundle httpfs needs
for https: whichever of `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem`,
`/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem`, `/etc/ssl/cert.pem` exist, plus `SSL_CERT_FILE` and
`ca_cert_file` when set (`SSL_CERT_DIR` is not allowed: use a bundle file). The DuckLake data path stored in the
catalog is allowed as well as the configured one. `flapi cache refresh` (the one-shot) applies the same policy.

The policy is logged once at startup:
`external access: restricted (auto: the configuration is remote; allowed: s3://bucket/, ...)` or
`external access: full (auto: the configuration is local)`.

**Limits:**
- Allowed directories and paths are **writable** too: a template can `COPY ... TO` a file under the config
  directory, `template.path` or the DuckLake `data-path` (with a remote config, into the config's own bucket prefix
  with flAPI's credentials). That includes **`flapi.yaml` itself** when the config directory is local: a template can
  overwrite it with `external-access: full` for the next start. With a process running as root, the allowed CA
  bundle is writable as well. Keep flAPI's credentials and files read-only where the deployment allows it.
- **HTTP redirects are followed without re-checking the allow list** (DuckDB checks the first URL only). Do not
  allow an http(s) origin that redirects, or one whose redirects you do not control.
- An endpoint YAML `{{include}}` outside the config/template prefix fails on a hot reload under `restricted` (the
  reload reads through the restricted database).
- With a **local** `flapi.yaml` and a remote `template.path`, `auto` is `full`; set `restricted` yourself.
- Whoever writes `flapi.yaml` chooses the mode: with the config in a bucket, the bucket writer can set `full`.
- Connection `init` SQL and the `duckdb:` settings run at startup, before the policy, with full access.

> **Implementation:** `src/database_manager.cpp` (`applyExternalAccessPolicy`), `src/external_access_policy.cpp` |
> **Tests:** `test/integration/test_external_access.py`

### 2.5 DuckLake Configuration

Configures DuckLake for snapshot-based caching with time-travel capabilities.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ducklake.enabled` | boolean | `false` | Enable DuckLake caching system |
| `ducklake.alias` | string | `"cache"` | Catalog alias for DuckLake tables |
| `ducklake.metadata-path` | string | - | DuckLake metadata catalog: a DuckDB file path, or `sqlite:`/`postgres:`/`mysql:` connection string. A missing parent directory of a local file is created (a read-only replica refuses to start instead). Shown redacted (`password=<redacted>`) in config API answers and logs |
| `ducklake.data-path` | string | - | Directory (or object-store prefix) for DuckLake data files; a missing local directory is created |
| `ducklake.data-inlining-row-limit` | integer | - | Maximum rows to inline in metadata. Keep it to a few thousand: at or above the size of a large merge delta (~112k rows at 200000) refreshes were 60-100x slower |
| `ducklake.override-data-path` | boolean | `false` | Attach with `OVERRIDE_DATA_PATH`: use this `data-path` although the catalog recorded another one. A catalog stores its data path as an absolute path, so a catalog prebuilt elsewhere (CI, another directory, another image path) otherwise fails with `DATA_PATH parameter ... does not match existing data path in the catalog` |
| `ducklake.warmup` | string | `always` | When caches are built at startup: `always` (re-run every populate query), `if-empty` (only tables that do not exist yet), `never` (none). See below. |
| `ducklake.read-only` | boolean | `false` | Attach the catalog `READ_ONLY`, for replicas serving a catalog another process writes. Implies `warmup: never`, no scheduled refresh, retention or compaction, and `409` from the config service's cache mutations. A cache not yet built is checked again (at most every 5 s on requests, and on every scheduler scan) and served once the writer has built it; that needs a shared Postgres/SQLite/MySQL catalog while the writer runs: several replicas can share a DuckDB-file catalog, but no writer can open it while one of them holds it, and a replica does not start while a writer server holds it. A replica needs the writer's `connections:` names (placeholder properties are fine) and logs that its scheduler is off. |

**Warm-up.** `if-empty` suits a persistent `db_path` or a shared catalog: a restart serves what is already
there. With `never`, a cache whose table does not exist is **unbuilt**: its endpoint answers `503`
`cache_not_built`, and `/health` answers `200` with `status: degraded` and an `unbuilt` list, so a startup
probe or CI smoke test passes while a monitor still sees it. The scheduler, `POST …/cache/refresh` and
`flapi cache refresh` build it later. Precedence: `read-only` > `--cache-warmup` / `FLAPI_CACHE_WARMUP` >
endpoint `cache.warmup` > `ducklake.warmup`.

**Retention Configuration:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ducklake.retention.keep-last-snapshots` | integer | - | Number of snapshots to retain per table |
| `ducklake.retention.max-snapshot-age` | string | - | Delete snapshots older than this (e.g., `30d`, `7d`) |

The global block is the default for every cache that has **no `retention:` block of its own**. An
endpoint's `cache.retention` replaces it as a whole (the two are not merged). Retention runs after each
refresh and on `…/cache/gc`; once snapshots are expired, flAPI also runs
`ducklake_cleanup_old_files(<catalog>, cleanup_all => true)`, so the data files they referenced are
deleted from disk (or the bucket) rather than left behind. Expiry is per table and covers every
incarnation of it: a CTAS full refresh or a `--full` rebuild creates a new table id, and the old ones'
snapshots expire too. A file is deleted only once no remaining snapshot of any table can see it, so
after a `--full` the old copy stays until the last snapshot before the rebuild leaves the window
(`keep-last-snapshots: N`: N-1 more refreshes that write).
To remove a table no endpoint uses any more (for example after changing `cache.schema`), call
`POST /api/v1/_config/cache/drop-table` (`flapii cache drop-table schema.table`).

**Compaction Configuration:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ducklake.compaction.enabled` | boolean | `false` | Enable automatic compaction. Needs a `schedule` as well |
| `ducklake.compaction.schedule` | string | - | How often: an interval (`30m`, `6h`, `1d`) or `@hourly`, `@daily`, `@midnight`, `@weekly`. Anything else (including a cron expression) stops startup with an error naming the key. Without `compaction.enabled: true` it has no effect, and startup warns |
| `ducklake.compaction.delete-threshold` | number | DuckLake's `0.95` | Fraction of deleted rows (`0 < x <= 1`) above which `ducklake_rewrite_data_files` rewrites a data file. A merge cache's delete files rarely reach DuckLake's default, so refreshes slow down as they pile up. Set about refreshes between compactions × the share of rows a refresh changes (1% x 5 = `0.05`); `0.2` never triggered with 1% deltas. Out of range stops startup |

Compaction runs once per catalog per period: `ducklake_merge_adjacent_files`, `ducklake_rewrite_data_files`
and a file cleanup. Each run writes an audit row with `sync_type` `compaction`, and when it last ran
survives a restart. A one-shot `flapi cache refresh` also compacts when compaction is due. Startup logs
`DuckLake compaction every <schedule>; last run ...; next due ...`, and each run logs
`DuckLake compaction is due (schedule ...); running it now`. Its history is
`GET /api/v1/_config/cache/audit?sync_type=compaction`; a catalog-wide run's row has an empty
`cache_table` and `cache_schema`. To compact now, whatever the schedule (and even with
`compaction.enabled: false`): `flapii cache compact [path]` / `POST /api/v1/_config/cache/compact`.

**Scheduler Configuration:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ducklake.scheduler.enabled` | boolean | `true` | Run `cache.schedule` refreshes. On by default; `false` turns scheduled refreshes off (startup logs a warning naming how many caches are affected) |
| `ducklake.scheduler.scan-interval` | string | see below | How often the scheduler checks which caches are due (`1s`, `30s`, `5m`). An invalid value stops startup |

Without `scan-interval`, schedules are checked every `min(heartbeat.worker-interval, shortest cache.schedule)`,
never more often than once a second. Startup logs `Cache scheduler on: N scheduled cache(s), checked every X s`,
and warns about any `cache.schedule` shorter than the scan interval (it then refreshes at most once per
scan). Endpoints that share one cache table share one schedule.

**No refresh at boot.** Startup warm-up builds the caches (see `warmup` above); the scheduler does not
refresh them again straight away. A cache's first scheduled refresh is one `schedule` after its last real
refresh — this run's warm-up, else the newest snapshot in the catalog, else process start. A refresh through
the config service counts as a real refresh.

**Example:**

```yaml
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./data/cache.ducklake
  data-path: ./data/cache
  retention:
    keep-last-snapshots: 10
    max-snapshot-age: 30d
  compaction:
    enabled: true
    schedule: '@daily'
  scheduler:
    scan-interval: 30s
```

**Audit log.** Every refresh and compaction writes a row to the catalog's `audit.sync_events` table
(`event_id` is a UUIDv4). Its `full` column (BOOLEAN) is `true` for a refresh that loaded the whole
source: a forced `--full`, a full-mode cache, or an incremental cache's first build. It is bounded: the newest 1000 rows per cache table are kept, and older audit
snapshots are expired together with the cache's own.

> **Implementation:** `src/cache_manager.cpp` | **Tests:** `test/cpp/database_manager_ducklake_test.cpp`, `test/integration/test_ducklake_cache.tavern.yaml`

### 2.6 MCP Configuration

Configures the Model Context Protocol (MCP) server.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp.enabled` | boolean | `true` | Enable MCP server |
| `mcp.instructions` | string | - | Inline instructions for LLM clients |
| `mcp.instructions-file` | string | - | Path to instructions markdown file |
| `mcp.strict-descriptions` | boolean | `false` | Reject tool descriptions containing control characters, JSON-breakout patterns, or role-override phrases ("ignore previous instructions") at config-load time |

> **MCP has no port or host of its own.** It is served by the same server as the
> REST API, on the HTTP port and host (`http-port`, `-p/--port`/`FLAPI_PORT`,
> `--host`/`FLAPI_HOST`), at `/mcp/jsonrpc`. `mcp.port`, `mcp.host` and
> `mcp.allow-list-changed-notifications` were documented in earlier versions but
> never took effect; flAPI now logs a warning when any of them is set.

**MCP Authentication:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp.auth.enabled` | boolean | `false` | Enable MCP authentication |
| `mcp.auth.type` | string | - | Auth type: `basic`, `bearer` (alias `jwt`), `oidc`. Anything else fails to load when `mcp.auth.enabled` (`AUTH_UNKNOWN_TYPE`) |
| `mcp.auth.methods.<method>.required` | boolean | `true` | Per-method authentication requirement |

> **Deny-by-default RBAC:** when `mcp.auth.enabled: true`, every MCP tool MUST declare `mcp-tool.allowed-roles`. A tool without that list refuses every call with `Permission denied`. This prevents a freshly-added tool from being callable by any authenticated user. Endpoints without `mcp.auth.enabled` keep working without role config — the `flapii project init` demo path stays simple.

**Example:**

```yaml
mcp:
  enabled: true
  instructions-file: ./mcp_instructions.md
  strict-descriptions: true
  auth:
    enabled: true
    type: bearer
    jwt-secret: '{{env.MCP_JWT_SECRET}}'
```

**Per-tool hardening keys** (in each endpoint's `mcp-tool:` block — see Section 3.2):

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp-tool.allowed-roles` | array | - | Required JWT/OIDC roles to call this tool. Required when `mcp.auth.enabled: true` |
| `mcp-tool.response.max-rows` | integer | - | Cap result-set size |
| `mcp-tool.response.redact-columns` | array | - | Replace listed columns with the redaction sentinel before returning rows |
| `mcp-tool.response.sample` | boolean | `false` | Return only summary metadata (`row_count`, `columns`, `sampled: true`), never row data |
| `mcp-tool.rate-limit.enabled` | boolean | `true` when the block is present | Per-tool rate limit (keyed on authenticated principal, falling back to anonymous) |
| `mcp-tool.rate-limit.max` | integer | `100` | Maximum tool invocations per interval |
| `mcp-tool.rate-limit.interval` | integer | `60` | Window in seconds |

**Dry-run / shadow mode:** any `tools/call` request may include `"arguments": { ..., "_dryRun": true }`. flAPI runs validators and template expansion (`?` placeholders rendered) and returns `rendered_sql`, `bindings` and `parameters` as a JSON payload; it never executes the query and runs no `EXPLAIN`. Maps 1:1 onto the audit/shadow-mode use case for batch validation before promoting an endpoint to production.

**Notes:**
- MCP server is auto-enabled when endpoints define `mcp-tool`, `mcp-resource`, or `mcp-prompt`
- Use `instructions-file` for large instruction sets, `instructions` for inline content
- `tools/list`, `resources/list` and `prompts/list` are rebuilt whenever an endpoint is created, updated, deleted or reloaded (over REST or MCP); no restart is needed. Cursors from an older list are rejected. The server does not send a `list_changed` notification, so clients re-list on their own
- The top-level REST `auth:` block does not protect MCP; MCP uses `mcp.auth`

> **Implementation:** `src/mcp_server.cpp`, `src/mcp_route_handlers.cpp`, `src/mcp_authorization_policy.cpp`, `src/mcp_description_scanner.cpp`, `src/mcp_dry_run.cpp`, `src/mcp_response_shaper.cpp`, `src/mcp_tool_rate_limiter.cpp` | **Tests:** `test/integration/test_mcp_rbac.py`, `test_mcp_dry_run.py`, `test_mcp_description_hygiene.py`, `test_mcp_response_shaping.py`, `test_mcp_per_tool_rate_limit.py`

### 2.7 Authentication (Global)

Global authentication settings that apply to all endpoints unless overridden.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `auth.enabled` | boolean | `true` if the block sets `type`, `roles`, `users`, `jwt-secret`, `jwt-issuer`, `oidc` or `from-aws-secretmanager`, else `false` | Enable authentication for every REST endpoint without its own `auth:` block. The same default applies inside an endpoint's `auth:` block; only `enabled: false` turns it off |
| `auth.type` | string | - | Default auth type: `basic`, `bearer` (alias `jwt`), `oidc`. Anything else fails to load (`AUTH_UNKNOWN_TYPE`) |
| `auth.jwt-secret` | string | - | JWT signing key |
| `auth.jwt-issuer` | string | - | Expected JWT issuer claim; unset = `iss` not checked (`AUTH_JWT_ISSUER_UNCHECKED`) |
| `auth.jwt-audience` | string or array[string] | - | The token's `aud` must contain one of these; unset = `aud` not checked |
| `auth.jwt-require-exp` | boolean | `true` | Refuse a bearer token without `exp` |
| `auth.users` | array | - | User list for basic auth |
| `auth.roles` | array[string] | `[]` | Roles a caller must hold (any one of them). Empty = any authenticated caller |

**Inheritance.** A top-level `auth:` block with `enabled: true` applies to every REST endpoint that has
**no `auth:` block of its own** — at startup, on reload, and to endpoints created through the config
service without one. An endpoint's own block **replaces** the global one (the two are not merged), and
`auth: {enabled: false}` on an endpoint opts it out. An endpoint block that leaves out `enabled` but
configures authentication (`type`, `roles`, `users`, `jwt-*`, ...) is ON, not an opt-out. Startup logs
`Global auth (type '<type>') applies to N endpoint(s) without their own auth: block; M endpoint(s) opt out with auth: {enabled: false}`.

An enabled endpoint block that names **no scheme and no credentials** (no `type`, `users`, `jwt-secret`,
`jwt-issuer`, `oidc` or `from-aws-secretmanager` — typically `roles:` only) takes the global block's type
and credentials and applies its own `roles` on top. Without an enabled global block it fails to load with
`AUTH_NO_TYPE`; so does a block that has credentials but no `type`. Keys the chosen type never reads
(`users:` under `bearer`, `jwt-secret:` under `basic`, ...) produce a load warning naming them — one scheme
applies per block, and the same goes for the global block and `mcp.auth`.

An enabled block whose `type` is not a scheme flAPI has (a typo such as `basci`) fails to load, and is
refused on reload, with `AUTH_UNKNOWN_TYPE: Endpoint GET /x has auth type 'basci', which is not a scheme
flAPI supports. Valid types: basic, bearer, jwt (alias of bearer), oidc.` The same applies to the global
block and to `mcp.auth.type` (which also accepts `jwt`). Such a block used to answer 401 to every request
and log nothing.

**Basic-auth cost.** A PBKDF2 hash at 600 000 iterations costs ~165 ms of CPU to check, and Basic auth
sends the password with every request. flAPI remembers a **successful** check for 5 minutes (at most
10 000 entries, keyed by an HMAC of user, password and stored hash under a per-process random key; no
password is stored), so only the first request with a given password pays it. A wrong password is
checked in full every time and never remembered; a changed hash never matches an old entry; every load
or reload of an `auth:` block empties the cache. At most half the cores derive PBKDF2 at once; a request
that waits more than 2 s for a slot is refused (401) and the log says why, so a flood of wrong passwords
cannot occupy every worker.

**401 responses** carry the configured scheme's challenge — `WWW-Authenticate: Basic realm="flAPI"` for
basic, `Bearer realm="flAPI"` for bearer and oidc, with `error="invalid_token"` when a bearer token was sent
and refused — and a JSON body `{"error":"Unauthorized","message":"..."}`, like the 403.
The config service reports each endpoint's `auth.roles` and `auth.inherited`. MCP tools are not covered:
they use `mcp.auth` (§2.6).

**Password hashing for `auth.users[*].password`:**
flAPI detects the format by prefix:

- `$pbkdf2-sha256$<iter>$<salt>$<hash>` — **recommended.** Modular Crypt Format (MCF) string, as written by Python `passlib` (its ab64 encoding, standard and URL-safe base64 are all accepted). Generate one with `python3 -c "from passlib.hash import pbkdf2_sha256; import getpass; print(pbkdf2_sha256.using(rounds=600000).hash(getpass.getpass()))"`. flAPI's own hashes use OpenSSL `PKCS5_PBKDF2_HMAC` with 600 000 iterations (OWASP 2023 minimum), 16-byte random salt, 32-byte derived key.
- `$2a$` / `$2b$` / `$2y$` (bcrypt) — **not supported.** Every login as that user is refused; the startup auditor reports `AUTH_BCRYPT_UNSUPPORTED`.
- 32-character lowercase hex — interpreted as MD5. Still accepted, but the startup auditor emits a **deprecation warning** (MD5 has no salt and is fast to brute-force).
- Anything else — interpreted as plaintext. Accepted for local demos; the startup auditor emits `AUTH_PLAINTEXT_PASSWORD`.

The auditor also warns `CORS_WILDCARD_WITH_AUTH` when any authentication is enabled while
`cors.allow-origins` is empty or contains `*`: any web page could then call the server cross-origin with a
credential the browser holds and read the answer. List your origins in `cors.allow-origins` (§2.9.1).

See [Section 7: Authentication](#7-authentication) for detailed configuration options.

> **Implementation:** `src/auth_middleware.cpp`, `src/password_hasher.cpp` | **Tests:** `test/cpp/auth_middleware_test.cpp`, `test/cpp/password_hasher_test.cpp`, `test/integration/test_password_hashing.py`

### 2.8 Rate Limiting (Global)

Global rate limiting settings.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `rate_limit.enabled` | boolean | `false` | Enable rate limiting globally |
| `rate_limit.max` | integer | `100` | Maximum requests per interval |
| `rate_limit.interval` | integer | `60` | Time window in seconds |
| `rate_limit.key` | string | `ip` | Bucket key: `ip` (legacy, per-IP), `user` (authenticated principal only), or `user-or-ip` (principal when present, IP fallback). With `user`/`user-or-ip` every request ALSO counts against its client IP with a ceiling of 10x the limit, so rotating invalid credentials cannot mint fresh buckets; many legitimate users behind one address share that ceiling. Buckets are per endpoint **route** and HTTP method (`/items/:id` is one bucket, `GET` and `POST` on one path are limited separately). |

**Example:**

```yaml
rate_limit:
  enabled: true
  max: 100
  interval: 60
  key: user-or-ip   # share-NAT-friendly: each authenticated user gets its own bucket
```

> **Implementation:** `src/rate_limit_middleware.cpp`, `src/rate_limit_key_builder.cpp` | **Tests:** `test/cpp/rate_limit_key_builder_test.cpp`, `test/integration/test_per_user_rate_limit.py`

### 2.9 HTTPS Configuration

flAPI's embedded server can terminate TLS directly. Reverse-proxy termination is still recommended for production, but direct TLS is supported for self-contained deployments.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enforce-https.enabled` | boolean | `false` | Serve over TLS: the listener is bound via OpenSSL using the cert+key pair below |
| `enforce-https.ssl-cert-file` | string | - | PEM-encoded certificate (required when enabled) |
| `enforce-https.ssl-key-file` | string | - | PEM-encoded private key (required when enabled) |

The block is named `enforce-https` and its keys are kebab-case. There is **no** top-level `https:`
block: it is not read, so a config that uses one serves plain HTTP (flAPI warns about the unknown
key at startup). There is no plain-HTTP redirect listener.

**Example:**

```yaml
enforce-https:
  enabled: true
  ssl-cert-file: ./ssl/cert.pem
  ssl-key-file: ./ssl/key.pem
```

> **Implementation:** `src/api_server.cpp` | **Tests:** `test/integration/test_tls_wireup.py`

### 2.9.1 CORS

Cross-Origin Resource Sharing. With no `cors` block, or an empty `allow-origins` list, every response carries `Access-Control-Allow-Origin: *` (`src/cors_policy.cpp`, `CorsPolicy::resolveAllowedOrigin`). Set an allow-list to restrict it: a request whose `Origin` is in the list gets that origin echoed back, any other origin (and a request without `Origin`) gets no `Access-Control-Allow-Origin` header. A `*` entry anywhere in the list means wildcard again.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cors.allow-origins` | array | `[]` (= `*`) | List of allowed `Origin:` values. Empty or absent sends `Access-Control-Allow-Origin: *` |
| `cors.allow-headers` | array | `[]` | `Access-Control-Allow-Headers` on preflights. Empty (or `*`) echoes the headers the browser asked for in `Access-Control-Request-Headers` |
| `cors.allow-methods` | array | `[]` (= GET, POST, PUT, PATCH, DELETE) | `Access-Control-Allow-Methods` on preflights |

A response whose `Access-Control-Allow-Origin` echoes an allow-listed origin also carries `Vary: Origin`.

**Preflights.** An `OPTIONS` request with `Origin` and `Access-Control-Request-Method` (REST paths and `/mcp/jsonrpc` alike) answers `204` with, for an allowed origin: `Access-Control-Allow-Origin`, `Access-Control-Allow-Methods`, `Access-Control-Allow-Headers`, `Access-Control-Max-Age: 600` and `Vary: Origin`. Without `cors.allow-headers`, the requested headers are echoed; a preflight that requests none gets `*`, plus `Authorization, Content-Type, X-Config-Token, Mcp-Session-Id` by name when any authentication is enabled (`*` does not cover `Authorization` in browsers). A disallowed origin gets no `Access-Control-Allow-*` header.

**Example:**

```yaml
cors:
  allow-origins:
    - https://app.example.com
    - https://staging.example.com
```

`flapii project init` does not generate a `cors` block, so a new project answers every origin with `*`. Set an explicit allow-list before exposing an authenticated API to browsers.

> **Implementation:** `src/cors_middleware.cpp`, `src/cors_policy.cpp` | **Tests:** `test/cpp/cors_policy_test.cpp`, `test/cpp/auth_cors_ergonomics_test.cpp`, `test/integration/test_cors_allowlist.py`, `test/integration/test_cors_preflight_and_auth_challenge.py`

### 2.9.2 Audit Log

Per-request JSONL audit log. Records the principal, method, target (REST path or MCP tool name), parameters (with operator-defined redaction), latency, status, and row count for both REST and MCP traffic.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `audit.enabled` | boolean | `false` | Off by default — operators opt in |
| `audit.sink` | string | `stdout` | `stdout` for container log collectors or `file` for a JSONL file |
| `audit.path` | string | - | Required when `sink: file`. Created if missing |
| `audit.redact` | array | `[]` | Parameter names to replace with `<redacted>` (e.g. `[ssn, jwt]`) |

**Example:**

```yaml
audit:
  enabled: true
  sink: file
  path: ./logs/audit.jsonl
  redact:
    - password
    - api_key
```

Each event is a single JSON line of the shape:
```json
{"timestamp":"2026-05-17T05:32:11Z","request_id":"…","trace_id":"…","span_id":"…","principal":"alice","method":"tools/call","target":"customer_lookup","status":"success","row_count":1,"latency_ms":12,"params":{"id":"42"}}
```

`status` is `success`, `denied` (REST 401/403), `rate_limited` (429), `dry_run` (MCP `_dryRun`),
`error:<HTTP status>` (other REST failures) or `error:<reason>` (MCP, e.g. `error:invalid_arguments`).
`trace_id` / `span_id` appear when the request has a
trace. `row_count` is `-1` when the row count is not known (REST requests today).

> **Implementation:** `src/audit_logger.cpp` | **Tests:** `test/cpp/audit_logger_test.cpp`, `test/integration/test_audit_log.py`

### 2.10 Heartbeat Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `heartbeat.enabled` | boolean | `false` | Enable global heartbeat worker |
| `heartbeat.worker-interval` | integer | `60` | Check interval in seconds. `60` with or without a `heartbeat:` block; `0` or a negative value stops startup |

**Example:**

```yaml
heartbeat:
  enabled: true
  worker-interval: 30
```

`worker-interval` also bounds how often cache schedules are checked when
`ducklake.scheduler.scan-interval` is not set (see [§2.5](#25-ducklake-configuration)).

Both keys are global. An endpoint opts in with its own `heartbeat:` block (see
[§3 Endpoint Configuration](#3-endpoint-configuration)); the global block
only decides whether the worker runs at all, and how often it sweeps.

#### What a heartbeat tick does

For every endpoint with `heartbeat.enabled: true`, the worker runs that
endpoint's **entire read path** — connection, DuckDB extensions, template
render, query — and throws the result away. It is a warm-up, not a
cache refresh: an endpoint with no `cache:` block is warmed too, and that is
deliberate, so the first real caller does not pay the cold cost.

A tick is always a **GET**, whatever the endpoint's `method:` is. Warming must
never mutate anything.

It does **not** go through the HTTP router: no routing, no CORS, no
rate-limiting, no auth middleware, and no `rest_endpoint_served` telemetry —
a warm-up is not traffic anybody generated. `APIServer::warmEndpoint()` calls
`RequestHandler::handleRequest()` directly. Cache-backed endpoints are
refreshed by the same call, through the ordinary cache path.

Because there is no HTTP request, there is no authenticated principal, so
`{{params.__auth_*}}` template variables are absent during a warm-up. A
template that requires them should guard on them (`{{#params.__auth_username}}`)
or the endpoint should not enable the heartbeat.

#### `heartbeat.params` (endpoint-level)

```yaml
# sqls/customers.yaml
heartbeat:
  enabled: true
  params:
    status: active       # arrives in the template as {{params.status}}
```

`params` is a flat string→string map injected into the warm-up's template
context exactly where a path or query parameter would land, so
`{{params.status}}` renders during a heartbeat tick. Use it when the template
needs a value to produce valid SQL — a required filter, a partition key.

> **Behaviour change.** Up to and including v26.09.23, `heartbeat.params` was parsed and
> then silently ignored: the warm-up was always issued with an empty parameter
> map. Templates that referenced a heartbeat param rendered as if it were
> absent. It is now honoured. If you have an endpoint whose `heartbeat.params`
> names something the template also reads from a request — or whose endpoint
> sets `request-fields-validation: true` and does not declare that name as a
> request field — review it: the value now actually arrives.

> **Implementation:** `src/heartbeat_worker.cpp` (`performHeartbeat`),
> `src/api_server.cpp` (`APIServer::warmEndpoint` — reads
> `endpoint.heartbeat.params`), `src/config_manager.cpp`
> (`parseEndpointHeartbeat`) |
> **Tests:** `test/integration/test_heartbeat_warming.py`,
> `test/integration/test_handler_offload.py`
> (`TestHeartbeatRequestsAreNotOffloaded`)

### 2.11 Storage Configuration (VFS)

flAPI supports loading configuration files and SQL templates from remote storage (S3, HTTPS, GCS, Azure) via DuckDB's Virtual File System (VFS) integration.

#### Remote Configuration Loading

Load the main configuration file from a remote URL:

```bash
# Load config from HTTPS
flapi --config https://example.com/configs/flapi.yaml

# Load config from S3 (requires AWS credentials)
flapi --config s3://my-bucket/configs/flapi.yaml

# Load config from GCS
flapi --config gs://my-bucket/configs/flapi.yaml
```

#### Remote Template Paths

SQL templates can be loaded from remote URLs by specifying a full URI in `template-source`:

```yaml
# Endpoint configuration with remote template
url-path: /customers
method: GET
template-source: https://example.com/templates/customers.sql
connection:
  - local-data
```

A remote `template.path` (an `s3://`, `gs://`, `az://` or `http(s)://` directory) loads the endpoint YAML and SQL
files from there:

```yaml
# flapi.yaml
template:
  path: s3://my-bucket/api-templates/sqls   # or gs://, az://, https://host/dir
storage:
  cache:                                      # optional: cache remote template files
    enabled: true
    ttl: 300
```

- `s3://`, `gs://` and `az://` are listed with a recursive glob (`**/*.yaml`, `**/*.yml`).
- Plain `http(s)://` has no listing. Put a `manifest.txt` at the root of `template.path`: one endpoint YAML per
  line, relative to it (`#` starts a comment; `..` is refused). The manifest is also the fallback when a glob
  finds nothing. If neither works, startup fails and names the manifest it looked for.
- `template-source:` in each endpoint resolves against the YAML's own location, as it does locally.
- Remote files are read through DuckDB's `httpfs` with the credentials from the environment (`AWS_*`, ...),
  before the main database exists, so `--config s3://...` and a remote `template.path` work at startup.
  `httpfs` must be installable (or already in the extension directory) on the host.
- `--validate-config` checks that each remote template exists, stays under `template.path`, and parses.
- **Credentials** come from the environment and are applied before the first remote read (a value you set
  yourself in `duckdb:` settings or a connection `init` always wins over the environment for `s3_*`):

  | Scheme | Environment | Notes |
  |---|---|---|
  | `s3://` | `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`, `AWS_REGION` | A custom `AWS_ENDPOINT_URL` selects SSL from its `http(s)://` scheme and uses path-style URLs unless it is an `amazonaws.com` host. |
  | `gs://` | `GCS_ACCESS_KEY_ID`, `GCS_SECRET_ACCESS_KEY` (or the `GS_*` spelling) | An HMAC key pair for Google's S3-interoperability API; a service-account key file (`GOOGLE_APPLICATION_CREDENTIALS`) cannot be used for `gs://` reads. Optional `GCS_ENDPOINT_URL` for an S3-compatible gateway. |
  | `az://`, `azure://` | `AZURE_STORAGE_CONNECTION_STRING`, or `AZURE_STORAGE_ACCOUNT` + `AZURE_STORAGE_KEY`, or `AZURE_STORAGE_ACCOUNT` + `AZURE_TENANT_ID`/`AZURE_CLIENT_ID` | Loads DuckDB's `azure` extension (installed on first use, only when Azure credentials are set). |

  Because the credentials are created as DuckDB secrets, give your own values as `duckdb:` settings or as a
  `CREATE SECRET` in a connection `init` (a secret you create wins over the environment's); a plain
  `SET s3_access_key_id` in an `init` does not override a secret.

  Offline hosts must have `httpfs` (and `azure`) pre-installed in `FLAPI_EXTENSION_DIRECTORY` (see below).
- **Limits:** each remote file is read whole, up to 10 MB. A manifest entry must be relative to `template.path`
  (or a full URL under it); `..` segments are refused. A local absolute `template-source` is refused when
  `template.path` is remote.
- **Changes:** remote files are cached by `storage.cache` (on by default, `ttl` 300 s, `max_size_mb` 50): an
  edited remote `.sql` shows up within one TTL; set `storage.cache.enabled: false` (or a small `ttl`) to see edits
  on the next request, at the cost of a round trip to the store per render. Endpoint YAMLs are read at startup:
  restart to pick up changes (`POST /api/v1/_config/endpoints/reload-file` answers 501 for remote paths). The
  config service reads a remote template (`GET .../template`) but refuses to write one (501): edit it in storage.
- A relative `template.path` inside a remote `--config` resolves against the config's URL (keeping its `?query`);
  relative *local* paths in such a config (`ducklake.metadata-path`, connection paths) resolve against the server's
  working directory.
- Offline hosts: set `FLAPI_EXTENSION_DIRECTORY` (httpfs pre-installed) and `FLAPI_EXTENSIONS_OFFLINE=1`; the
  bootstrap then neither installs nor auto-loads anything.

#### Supported URI Schemes

| Scheme | Description | Credential Source |
|--------|-------------|-------------------|
| `https://` | HTTPS URLs | None required |
| `http://` | HTTP URLs (not recommended) | None required |
| `s3://` | Amazon S3 | `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION` |
| `s3a://`, `s3n://` | S3-compatible storage (data only) | Same as S3 |
| `gs://` | Google Cloud Storage | HMAC key in a DuckDB secret (`TYPE gcs`); `GOOGLE_*` variables are not used |
| `az://`, `azure://` | Azure Blob Storage | `AZURE_STORAGE_ACCOUNT`, `AZURE_STORAGE_KEY` |
| `abfs://` | Azure Data Lake (data only) | as Azure |
| `file://` | Local filesystem | None required |

#### Path Security

- **Template sources stay inside `template.path`.** A `template-source` that
  escapes the templates directory (`..`, an absolute path, a symlink out, another
  bucket or host) is a `--validate-config` warning for a local config, and stops
  startup for a remote config or `template.path`. Remote paths are checked after
  percent-decoding (`%2e%2e`); in a packed binary the check is against the bundle.
- **Schemes:** the config and templates can be read from `s3://`, `gs://`, `az://`,
  `azure://` and `http(s)://` (and local paths). Schemes marked *data only* are
  passed to DuckDB in SQL but are not recognised as a config or template location.
  There is no scheme allowlist; what a *template* may read is
  governed by DuckDB external access instead: see
  [§2.4.1 DuckDB Hardening](#241-duckdb-hardening-duckdb-hardening). Under
  `auto` (the default), the templates of a remote config (`s3://`, `gs://`,
  `az://`, `https://`) run with external access restricted to the directories
  flAPI needs.
- Credentials: the environment variables below, or a DuckDB secret in a
  connection's `init`; see [Reading from cloud storage](guides/cloud-storage.md).

#### Environment Variables for Cloud Storage

**Amazon S3:**

```bash
export AWS_ACCESS_KEY_ID=your-access-key
export AWS_SECRET_ACCESS_KEY=your-secret-key
export AWS_REGION=us-east-1
# Optional: for assumed roles
export AWS_SESSION_TOKEN=your-session-token
```

**Google Cloud Storage:**

flAPI reads `GOOGLE_APPLICATION_CREDENTIALS` / `GOOGLE_CLOUD_PROJECT` only to log
that they are set (`src/credential_manager.cpp`, `CredentialManager::configureDuckDB`
applies nothing for GCS). DuckDB's `httpfs` authenticates to GCS with an **HMAC
key**, not a service-account JSON file or application-default credentials. Create
the key in the Cloud Console (Cloud Storage → Settings → Interoperability) and
register it as a DuckDB secret, for example in the connection's `init`, reading
the values from environment variables (whitelist them, e.g. `'^GCS_HMAC_.*'` under
`template.environment-whitelist`):

```yaml
connections:
  gcs-data:
    init: |
      INSTALL httpfs;
      LOAD httpfs;
      CREATE OR REPLACE SECRET gcs_hmac (
        TYPE gcs,
        KEY_ID '{{env.GCS_HMAC_KEY_ID}}',
        SECRET '{{env.GCS_HMAC_SECRET}}'
      );
    properties:
      path: 'gs://my-bucket/data/*.parquet'
```

**Azure Blob Storage:**

```bash
export AZURE_STORAGE_ACCOUNT=your-storage-account
export AZURE_STORAGE_KEY=your-storage-key
# Or use connection string
export AZURE_STORAGE_CONNECTION_STRING=your-connection-string
```

#### Example: Full Remote Configuration

```yaml
# flapi.yaml (can itself be hosted remotely)
project-name: Cloud-Native API
project-description: API with remote configuration

template:
  path: s3://my-bucket/templates/

connections:
  cloud-data:
    init: |
      INSTALL httpfs;
      LOAD httpfs;
      SET s3_region='us-east-1';
    properties:
      bucket: my-data-bucket
      path: s3://my-data-bucket/data/

duckdb:
  access_mode: READ_ONLY
```

```yaml
# s3://my-bucket/templates/customers.yaml
url-path: /customers
method: GET
template-source: customers.sql  # Relative to template.path (s3://my-bucket/templates/)
connection:
  - cloud-data
```

> **Implementation:** `src/vfs_adapter.cpp`, `src/config_loader.cpp`, `src/path_validator.cpp` | **Tests:** `test/cpp/test_vfs_adapter.cpp`, `test/cpp/test_path_validator.cpp`, `test/integration/test_vfs_e2e.py`

---

### 2.12 Telemetry Configuration

Controls the anonymous, aggregate usage telemetry flAPI sends to PostHog (EU):
`server_started` at boot, `$exception` on a failed request, and at most once a
minute per route and status a `feature_used` event (`feature` =
`rest_endpoint_served`, `mcp_tool_called` or `auth_enforced`) with a `count` and
the mean and max duration. [TELEMETRY.md](../TELEMETRY.md)
lists every property and what is never sent.

```yaml
telemetry:
  enabled: true      # false opts out
  sample_rate: 1.0   # values in (0,1) down-sample the per-request events
```

| Field | Default | Description |
|-------|---------|-------------|
| `telemetry.enabled` | `true` | Enable/disable telemetry events |
| `telemetry.sample_rate` | `1.0` | A value in (0,1) keeps about one in `round(1/rate)` per-request and per-tool events; anything else (including `0`) sends all, so use `enabled: false` to stop. Lifecycle events are always sent; sampled events carry `sample_rate` |

**Opt-out precedence (highest wins):**

1. `--no-telemetry` CLI flag
2. `FLAPI_NO_TELEMETRY=1` environment variable
3. `telemetry.enabled: false` in `flapi.yaml`
4. `DATAZOO_DISABLE_TELEMETRY=1` (cross-product env var, handled at the PostHog layer)

> **Implementation:** `src/flapi_telemetry.cpp`, `src/include/flapi_telemetry.hpp` | **Tests:** `test/cpp/test_flapi_telemetry.cpp`

---

## 3. Endpoint Configuration

Endpoints are defined in YAML files within the `template.path` directory. Each file can define one endpoint type: REST, MCP Tool, MCP Resource, or MCP Prompt.

### 3.1 REST Endpoints

REST endpoints expose SQL queries as HTTP APIs.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `url-path` | string | **required** | HTTP path (e.g., `/customers`, `/customers/:id`). Rules below |
| `method` | string | `GET` | HTTP method: `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, in any case (trimmed and stored upper-case; empty means `GET`). Anything else is a load error naming the endpoint |
| `allow-shadowing` | boolean | `false` | Let this endpoint answer the URLs it shares with a less specific, overlapping route of the same method whose effective auth differs in any way (looser or stricter). Without it that combination is refused (see *Route precedence*) |
| `description` | string | - | The operation's description in `/doc.yaml`. Without it, `mcp-tool.description` is used; without either, the operation has none |
| `template-source` | string | **required** | Path to SQL template file |
| `connection` | array[string] | **required** | Connection name(s) from flapi.yaml |
| `with-pagination` | boolean | `true` | Wrap the query in `?limit=` (default `100`) / `?offset=` paging; the body is `{"next", "total_count", "data"}` plus `X-Total-Count`/`X-Limit`/`X-Offset`/`X-Next` headers. `limit`/`offset` are reserved; do not use them in the template. `limit=0` returns no rows and an empty `next` (the count only). `false` returns a bare JSON array |
| `total-count` | boolean | `true` | `false`: paginated responses carry no `total_count` and no `X-Total-Count`, and flAPI never runs the `COUNT` query for them; `next` is found by reading one row past the page. For endpoints whose clients only page forward. With `true`, the `COUNT` runs only for a full page or an empty page past the end: a shorter page gives the total itself. On a DuckLake-cached endpoint whose query reads only DuckLake tables, the count is reused until the catalog's next snapshot (exact; see [caching.md](spec/components/caching.md#total_count-of-a-cached-endpoint-242)) |
| `request-fields-validation` | boolean | `false` | Reject (400) a query parameter that is not a declared `field-name`. `limit`, `offset` and `format` are always allowed |

A query parameter given more than once (`?x=1&x=2`) is rejected with 400
`{"error": "Duplicate query parameter: x"}`; it used to take the first value silently.

**Example:**

```yaml
url-path: /customers/:customer_id
method: GET

request:
  - field-name: customer_id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1

template-source: customers-get.sql
connection:
  - customers-db

with-pagination: true
```

**Path Parameters:**

URL path parameters use the `:param` syntax and are automatically extracted:

```yaml
url-path: /customers/:id/orders/:order_id
# Available as: params.id, params.order_id
```

**url-path rules** (checked at load, by `--validate-config`, and by create/update; a
violation is a load error / 400 naming the endpoint):

- starts with `/`; `/` alone is allowed, and so is one trailing `/` (`/customers/`),
  which is then part of the route (`/customers` is a different route);
- every segment is either a literal of `A-Z a-z 0-9 . _ ~ -` or a parameter `:name`
  (`name` = letters, digits, `_`, not starting with a digit); each parameter name
  appears once per path (`/a/:id/b/:id` is refused);
- no `.` or `..` segment (`/a/../b`): clients and proxies resolve them before the
  request arrives, so the route would never match what was written;
- no empty segments (`//`), no `{id}` (write `:id`), no `*`, `(`, `[`, `+`, `?`, `|`, `^`,
  `$`, `\`, `<`, `>`, `%` or spaces. Routes are matched exactly and case-sensitively.

**Route precedence.** Two endpoints of the same method may have *overlapping* routes:
`/customers/7` and `/customers/:id`, or `/a/:x/b` and `/:z/:x/b`, both match some
URLs. Such a URL is answered by the **most specific** route: compare segments left to
right; at the first position where one route has a literal and the other a `:param`,
the literal wins. This never depends on file order or reloads, and rate limiting, auth
and the request handler always resolve the same endpoint. Routes with the same shape
(`/customers/:id` and `/customers/:cid`, same method in any case) are duplicates and
stop startup.

When overlapping endpoints have different **effective** auth (after inheriting the
top-level `auth:` block), startup, reload, reload-file, create and update refuse the
pair, **in either direction** - an open literal over a protected `:param` route and a
protected literal over an open one alike. Auth is compared on `enabled`, `type`
(`jwt` = `bearer`), `roles` **and the identity source**: `users` (username, password,
per-user roles), `jwt-secret` (compared as a digest), `jwt-issuer`, the `oidc` issuer,
client, audiences, claims, `scopes`, `verify-expiration` and `clock-skew-seconds`, and
the `from-aws-secretmanager` secret and region. Secret values never
appear in the message:

```
Overlapping routes with different auth: GET /customers/7 (basic, roles [admin]) takes
precedence over GET /customers/:id (open) for /customers/7 (auth differs in: enabled);
/customers/:id still serves every other URL its pattern matches, including other
spellings of the same value such as /customers/07 - a literal route protects that
exact path string, not a record (declared in ... and ...).
```

**A literal route protects only that exact path string.** Path parameters are matched
as text and the SQL usually casts them, so `/customers/07`, `/customers/+7` (and
`/customers/7.0` for a float column) reach `/customers/:id` and select the same row
as the protected `/customers/7`. To protect a record, either give both routes the same
auth, or protect the general endpoint and filter rows by the caller inside its
template:

```sql
-- /customers/:id, auth: basic with users admin (roles [admin]) and guest (roles [guest])
SELECT * FROM customers
WHERE id = {{{ params.id }}}
  AND (id <> 7 OR list_contains(string_split('{{{ auth.roles }}}', ','), 'admin'))
```

Set `allow-shadowing: true` on the **more specific** endpoint only when the split is
intended; the overlap is then logged as a WARNING (which the plugin hook passes on to
Claude). Overlaps with the same auth log an INFO line.

**What counts as "the same auth"** is who gets in, not how the block is spelled:

- `users`: the same username and per-user roles, and a password that admits the same
  secret. Equal strings do. A **plaintext** password and the MD5 or PBKDF2 hash of it are
  the same user (the migration case), because flapi can verify the plaintext against the
  hash. Two PBKDF2 hashes of one password with **different salts cannot be proven equal**
  (the hash is one-way) and count as different: give both endpoints the same hash string
  (a YAML include or anchor), or keep one of them in plaintext while migrating. bcrypt
  hashes are not verifiable by flapi and only match when byte-identical.
- `oidc`: compared as the provider requests see it, with the `provider-type` preset
  applied. `provider-type: keycloak` plus `domain`/`realm` is the same provider as the
  spelled-out `issuer-url` with the claim defaults the preset fills in
  (`username-claim: preferred_username`, `role-claim-path: realm_access.roles`, ...).
  Identity keys: the resolved `issuer-url`, `client-id`, `client-secret` (digest),
  `allowed-audiences`, `username-claim`, `roles-claim`, `groups-claim`, `role-claim-path`,
  `verify-expiration`, `clock-skew-seconds` and `scopes`. **Not identity:** `email-claim`
  (a display claim), `jwks-cache-hours` (a key-cache lifetime), `enable-client-credentials`
  and `enable-refresh-tokens` (token-endpoint flows, not request authentication), and
  `provider-type`/`tenant-id`/`realm`/`domain` on their own (they only feed the preset).
- Anything else in the list above (`type`, `roles`, `jwt-secret`, `jwt-issuer`,
  `from-aws-secretmanager`) must be equal.

**Three levels.** An overlap is judged against the endpoint that actually answers the
shared URLs. With a protected `/a/:x/:y`, an open `/a/b/:y` (`allow-shadowing: true`) and
an open `/a/b/c`, the literal `/a/b/c` adds no exposure: `/a/b/:y` already answers it open.
It loads without its own flag when another endpoint of the same method covers it, beats
the less specific route, loses to it, and has exactly its auth. The covering route has
to be loaded before the literal (files load in path order; create it first at runtime).

**`allow-shadowing` over MCP.** The `flapi_create_endpoint` MCP tool never opts into
shadowing: it has no such argument (`allow-shadowing` in any spelling is ignored), so an
AI client cannot create an open route over a protected one. Set the flag in the YAML file,
or `POST /api/v1/_config/endpoints`, if the split is intended.

### 3.2 MCP Tools

MCP tools expose SQL queries as callable tools for AI models.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp-tool.name` | string | **required** | Unique tool name (alphanumeric, underscores) |
| `mcp-tool.description` | string | **required** | Tool description for AI models |
| `mcp-tool.result-mime-type` | string | `application/json` | Result MIME type |

**Example:**

```yaml
mcp-tool:
  name: customer_lookup
  description: Retrieve customer information by ID or filter criteria
  result-mime-type: application/json

request:
  - field-name: id
    field-in: query
    description: Customer ID
    required: false
    validators:
      - type: int
        min: 1

template-source: customers.sql
connection:
  - customers-parquet
```

### 3.3 MCP Resources

MCP resources expose data as readable resources for AI models.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp-resource.name` | string | **required** | Unique resource name (two files with the same name fail the load) |
| `mcp-resource.description` | string | **required** | Resource description |
| `mcp-resource.mime-type` | string | `application/json` | Content MIME type |
| `mcp-resource.uri-template` | string | - | `flapi://customers/{id}`: a parameterised resource, listed in `resources/templates/list` only. Each `{var}` must be a `request:` field; its validators run on `resources/read` |
| `mcp-resource.response.max-rows` | integer | the file's `mcp-tool.response.max-rows`, else `1000` | Cap the rows a read returns; a capped read carries `_meta.truncated` |
| `mcp-resource.response.redact-columns` | array[string] | the file's `mcp-tool.response.redact-columns` | Columns replaced by `"<redacted>"` |

**Example:**

```yaml
mcp-resource:
  name: customer_schema
  description: Customer database schema and field definitions
  mime-type: application/json
  response:
    max-rows: 200          # default 1000
    redact-columns: [email]

template-source: schema-query.sql
connection:
  - customers-db
```

### 3.4 MCP Prompts

MCP prompts provide template-based prompts for AI models.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mcp-prompt.name` | string | **required** | Unique prompt name |
| `mcp-prompt.description` | string | **required** | Prompt description |
| `mcp-prompt.template` | string | **required** | Mustache template content (inline) |
| `mcp-prompt.arguments` | array[string] | `[]` | Template argument names |

**Example:**

```yaml
mcp-prompt:
  name: customer_analysis
  description: Generate customer analysis prompt
  template: |
    You are a data analyst. Analyze this customer:
    {{#customer_id}}Customer ID: {{customer_id}}{{/customer_id}}
    {{#segment}}Segment: {{segment}}{{/segment}}

    Provide insights on purchasing patterns and recommendations.
  arguments:
    - customer_id
    - segment
```

**Notes:**
- MCP prompts use **inline templates**, not file-based `template-source`
- Prompts do not require `connection` (they don't execute SQL)

### 3.5 Configuration Includes

Reuse common configuration across endpoints using the include syntax.

**Syntax:**

```yaml
{{include:section_name from filename.yaml}}
```

**Example:**

```yaml
# common/customer-common.yaml
request:
  - field-name: id
    field-in: query
    validators:
      - type: int
        min: 1

auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: secret

# customers-rest.yaml
url-path: /customers
method: GET

{{include:request from common/customer-common.yaml}}
{{include:auth from common/customer-common.yaml}}

template-source: customers.sql
connection:
  - customers-parquet
```

**Server errors.** A request that fails inside flAPI (a SQL error, a missing table, a bind failure)
returns `500` with a fixed JSON body and no SQL, file paths or database detail:

```json
{"error": "Internal Server Error",
 "message": "The request failed on the server. The details are in the server log under this request id.",
 "request_id": "req-46788500aef28a89"}
```

The real error is logged with the same `request_id` (also sent as the `X-Request-Id` header); grep the
server log for it.

**Includable Sections:**
- `request`
- `auth`
- `rate-limit`
- `connection`
- `template-source`
- `cache`
- `heartbeat`

> **Implementation:** `src/endpoint_config_parser.cpp`, `src/request_handler.cpp` | **Tests:** `test/cpp/endpoint_config_parser_test.cpp`, `test/cpp/extended_yaml_parser_test.cpp`

---

## 4. Request Parameters

### 4.1 Field Definition

Each request parameter is defined with the following properties:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `field-name` | string | **required** | Parameter name (used as `params.<name>` in templates) |
| `field-in` | string | **required** | Parameter location |
| `description` | string | `""` | Human-readable description |
| `required` | boolean | `false` | Whether parameter is mandatory |
| `default` | string | - | Default value if not provided |
| `validators` | array | - | Array of validator configurations |
| `field-type` | string | - | **No effect**: nothing reads it, and loading warns. Constrain the value with a validator (`- type: int`) instead |

**Example:**

```yaml
request:
  - field-name: customer_id
    field-in: path
    description: Unique customer identifier
    required: true
    validators:
      - type: int
        min: 1

  - field-name: status
    field-in: query
    description: Filter by status
    required: false
    default: "active"
    validators:
      - type: enum
        allowedValues: ["active", "inactive", "pending"]
```

### 4.2 Parameter Locations

The `field-in` property specifies where the parameter is extracted from:

| Location | Description | Example |
|----------|-------------|---------|
| `query` | URL query string | `GET /customers?id=123` |
| `path` | URL path parameter | `GET /customers/:id` → `GET /customers/123` |
| `body` | Request body (JSON) | `POST /customers` with JSON body |
| `header` | HTTP header | `X-Customer-ID: 123` |

Each declared field is read **only** from its `field-in` source, so a query string cannot satisfy a
`body` field (a required one is `400 Required field is missing`) and a body key cannot replace a `query`
field. A route parameter (`/customers/:id`) is path-only: `?id=2` or a body `{"id": 99}` never replaces
it, whether or not the endpoint declares the field. Names no field declares are still accepted from the
query string or body (validation can reject unknown ones). Path values are percent-decoded once before
validation (`/customers/%37` is `7`, `/names/John%20Doe` is `John Doe`, `%2F` becomes `/` inside the same
parameter, `+` stays a plus); a malformed escape, `%00` or invalid UTF-8 is `400`.

A `header` field is read from the request's headers, matched case-insensitively (`field-name: X-Tenant`
accepts `x-tenant: acme`), on GET and on write methods alike. Only the header counts: a query-string or
body parameter of the same name does **not** satisfy a `header` field, and a missing required header is a
`400 Required field is missing`. In the template the value is `params.<field-name>` as written in the
config.

### 4.3 Default Values

Default values are used when a parameter is not provided:

```yaml
request:
  - field-name: days
    field-in: query
    default: "30"
    validators:
      - type: int
        min: 1
        max: 365

  - field-name: sort_order
    field-in: query
    default: "DESC"
    validators:
      - type: enum
        allowedValues: ["ASC", "DESC"]
```

**Notes:**
- Do not declare `limit` or `offset`: on a paginated endpoint (the default) they are reserved for pagination (§3)
- Defaults are always strings in YAML; validators handle type conversion
- Defaults are applied before validation

> **Implementation:** `src/request_handler.cpp` | **Tests:** `test/cpp/request_handler_test.cpp`

---

## 5. Validators

Validators ensure request parameters meet constraints before SQL execution.

### 5.1 Integer Validator

Validates integer values with optional range constraints.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `int` |
| `min` | integer | `INT_MIN` | Minimum allowed value |
| `max` | integer | `INT_MAX` | Maximum allowed value |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: int
    min: 1
    max: 1000000
    preventSqlInjection: true
```

### 5.2 String Validator

Validates string values with optional pattern and length constraints.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `string` |
| `regex` | string | - | Regex pattern (full match required). `pattern` is accepted as an alias; if both are given, `regex` wins |
| `min` | integer | - | Minimum string length |
| `max` | integer | - | Maximum string length |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: string
    regex: "^[A-Za-z0-9_-]{3,50}$"
    min: 3
    max: 50
    preventSqlInjection: true
```

### 5.3 Enum Validator

Validates against a list of allowed values.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `enum` |
| `allowedValues` | array[string] | **required** | List of valid values. `values` is accepted as an alias; if both are given, `allowedValues` wins |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: enum
    allowedValues: ["retail", "corporate", "vip", "enterprise"]
```

### 5.4 Email Validator

Validates email address format.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `email` |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: email
```

### 5.5 UUID Validator

Validates UUID format (v1-v5).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `uuid` |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: uuid
```

### 5.6 Date Validator

Validates date format (YYYY-MM-DD) with optional range.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `date` |
| `min` | string | - | Minimum date (YYYY-MM-DD) |
| `max` | string | - | Maximum date (YYYY-MM-DD) |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: date
    min: "2020-01-01"
    max: "2025-12-31"
```

### 5.7 Time Validator

Validates time format (HH:MM:SS) with optional range.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | - | Must be `time` |
| `min` | string | - | Minimum time (HH:MM:SS) |
| `max` | string | - | Maximum time (HH:MM:SS) |
| `preventSqlInjection` | boolean | `true` | Enable SQL injection prevention |

**Example:**

```yaml
validators:
  - type: time
    min: "08:00:00"
    max: "18:00:00"
```

### 5.8 Number and Boolean Validators

`type: number` (aliases `float`, `double`) requires the **whole** value to be one plain decimal number
(`-3`, `1.5`, `.5`, `1e3`; no spaces, hex or `NaN`). `type: boolean` (alias `bool`) accepts `true`, `false`,
`1`, `0` (any case). `type: integer` is an alias of `int`. A value that does not match is rejected with
HTTP 400 before any SQL is rendered.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `min` | number | - (no bound) | Smallest allowed value, inclusive (`0.5` is a valid bound) |
| `max` | number | - (no bound) | Largest allowed value, inclusive |

```yaml
validators:
  - type: number
    min: 0
    max: 99.99
```

Out of range: `Number is less than the minimum allowed value (min 0)` /
`Number is greater than the maximum allowed value (max 99.99)`. MCP `tools/list` and `/doc.yaml` emit
`minimum`/`maximum` only for a bound that is set.

A JSON body number reaches the validators as the text the client sent: `9.99` stays `9.99`, and an
`int` validator rejects `301.7` (`Invalid integer value`) and `5.0`. Before, the fraction was dropped.

Range errors name the limit: `Integer is greater than the maximum allowed value (max 1000)`,
`String is shorter than the minimum allowed length (min 3)`, `Date is after the maximum allowed date
(max 2025-12-31)`, and likewise for the other bounds.

### 5.9 The SQL-injection screen is a safety net, not the defence

Every string-like field (and any field without a bound type) is screened for SQL keywords, comment markers
and, when the value contains a single quote, SQL-shaped tokens next to it (operators, `or`/`and`/`like`, a
parenthesis, ...) after collapsing all whitespace. That is a heuristic: it can be evaded by a payload it
does not know. **A bare `{{ params.x }}` (double braces, no quotes) is bound as a prepared-statement
parameter, so the value can never change the SQL — but only when all of these hold**
(`src/prepared_template_rewriter.cpp`, `src/sql_parameter_classifier.cpp`):

- the reference is **outside** every `{{#...}}` / `{{^...}}` section. Inside a section it is rendered as
  text (HTML-escaped, unquoted), exactly like any other Mustache value;
- the field is declared under `request:` and has a validator whose `type` is one of `int`/`integer`,
  `number`/`float`/`double`, `boolean`/`bool`, `date`, `time`, `string`, `uuid`, `email` or `enum`. A
  field without such a validator is rendered as text;
- the reference is **not** inside a SQL string literal. flAPI replaces the tag with `?` wherever it
  stands, so `'{{ params.x }}'` becomes the literal `'?'`, not a placeholder, and the request fails with
  `500` (the server log, under the response's `request_id`, says `Failed to bind parameter`). Write `WHERE name = {{ params.name }}`, without quotes; the bound value
  is typed (`VARCHAR` for string-like validators), so no quotes are needed.

```sql
-- bound: outside a section, typed field, no quotes
WHERE name = {{ params.name }}
-- NOT bound: inside a section; quote it with triple braces and rely on the validator
{{#params.city}} AND city = '{{{ params.city }}}' {{/params.city}}
```

`{{{ params.x }}}` is bound the same way (#192) when it is a plain value site of a typed field outside
every section: `n = {{{ params.x }}}` and `n = '{{{ params.x }}}'` both become `n = ?` (a quoted
site binds the text, and an absent value binds `''` as it used to render), and
`n LIKE '%{{{ params.x }}}%'` becomes `n LIKE ('%' || ? || '%')`. It stays raw text
(validated by the field's validator and the screen above) inside a section, inside a SQL comment, inside
`AT (VERSION|TIMESTAMP => ...)` (DuckLake resolves it before parameters are bound), when the field is an
`enum` and the tag is unquoted (a whitelisted identifier such as `ORDER BY {{{ params.sort }}}`), when it
follows a keyword that expects a name or makes a typed literal (`FROM`, `JOIN`, `INTO`, `TABLE`, `BY`,
`DATE '...'`, `INTERVAL '...'`, `ATTACH '...'`, `E'...'`), when it is glued to an identifier
(`t_{{{ params.x }}}`), or when a quoted literal mixes it with a non-triple-brace tag. A tag in a
quoted literal with surrounding text is concatenated, so `'{{{a}}}-{{{b}}}'` becomes `(? || '-' || ?)`.
Reserve raw `{{{ }}}` text for identifiers and fragments you validate with an `enum` or a strict
`regex`, and for optional filters inside sections. Set
`preventSqlInjection: false` on a field only when every reference to it is a bound `{{ }}` and it
legitimately contains such characters (e.g. a free-text name with an apostrophe and parentheses).

> **Implementation:** `src/request_validator.cpp` | **Tests:** `test/cpp/request_validator_test.cpp`, `test/integration/test_customers.tavern.yaml`, `test/integration/test_validator_hardening.py`

---

## 6. Cache Configuration

DuckLake caching enables snapshot-based query result caching with automatic refresh.

### 6.1 Basic Cache Settings

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cache.enabled` | boolean | `false` | Enable caching for this endpoint |
| `cache.table` | string | **required** | Cache table name |
| `cache.schema` | string | `"cache"` | DuckDB schema name |
| `cache.schedule` | string | - | Refresh schedule (e.g., `5m`, `1h`, `30s`) |
| `cache.template-file` | string | - | Cache populate SQL template. Without it the endpoint's `template-source` is materialised with a full refresh (startup warns) |
| `cache.warmup` | string | - | Overrides `ducklake.warmup` for this endpoint: `always`, `if-empty` or `never` |

**Schedule Format:**
- `30s` = 30 seconds
- `5m` = 5 minutes
- `1h` = 1 hour
- `2d` = 2 days

**Example:**

```yaml
cache:
  enabled: true
  table: customers_cache
  schema: analytics
  schedule: 5m
```

### 6.2 Cache Readiness During Warmup

At startup, flAPI opens the HTTP listener before cache warmup finishes. Cache-enabled endpoints are
not allowed to serve until their configured cache table is fully built.

While a cache is still building, requests to that endpoint return:

```http
503 Service Unavailable
Retry-After: 5
Content-Type: application/json
```

```json
{
  "error": "cache_warming",
  "message": "Cache for this endpoint is still being built",
  "table": "customers_cache"
}
```

If the **first build** fails, the endpoint keeps returning `503` with `Retry-After: 5`, now saying so:

```json
{
  "error": "cache_build_failed",
  "message": "Cache for this endpoint failed to build",
  "table": "customers_cache"
}
```

The failure detail is only in the server log, logged once. This is intentional: a cached endpoint must
never return `200` with empty or partial results from a half-built cache. A failing **refresh** of a cache
that was already built does not take the endpoint offline: it keeps serving the last snapshot (see
`/health`: `stale`). That includes a restart with `warmup: always` whose rebuild fails while the table
from the previous run still exists.

**The populate template must build the table.** A cache template whose statement is a bare `SELECT` is
refused before it runs (`cache template must write {{cache.catalog}}.{{cache.schema}}.{{cache.table}} …
statement N of the template is a bare SELECT, which builds nothing`; a SELECT before the last statement,
e.g. a guard, is refused with `statement N of M is a bare SELECT; its result is discarded. Wrap a check in
CREATE OR REPLACE TEMP TABLE _chk AS SELECT ...`). After the script has run, the
cache table must exist; if it does not, the refresh fails and is rolled back. Write a
`CREATE OR REPLACE TABLE … AS SELECT …`, an `INSERT INTO …` or a `MERGE INTO …`.

Without `cache.template-file` the endpoint's `template-source` is the populate SQL. If it renders to a single
`SELECT` it is materialised as `CREATE OR REPLACE TABLE <catalog>.<schema>.<table> AS <select>` - a full
refresh every time, whatever `primary-key`/`cursor` imply (audited as `full`) - and startup warns
`cache on <endpoint> has no template-file: ...`. The bare-`SELECT` refusal applies only to an explicit
`template-file`.

Shared tables (several endpoints with the same `cache.table`) are warmed once, not once per endpoint.

**Full rebuild.** `POST /api/v1/_config/endpoints/{slug}/cache/refresh?full=true`,
`flapi cache refresh --full` and `flapii cache refresh <path> --full` erase the incremental watermark
**and** drop the cache table (`DROP TABLE IF EXISTS`) in the same transaction, so the template's full-load
branch rebuilds it from scratch — an append cache is rebuilt, not appended to twice. A template that
manages its own `BEGIN`/`COMMIT` only gets the watermark cleared (a warning says so).

When warm-up is skipped (`warmup: never`, or `read-only`) and the cache table does not exist, the
endpoint returns `503` with `"error": "cache_not_built"` until a refresh builds it. On a read-only
replica, the writer's refresh counts: the replica notices the new table within about 5 s.

`cache.schedule` must be `<positive number>[s|m|h|d]` (at most 10 years); anything else stops startup.

Endpoints without a `cache:` block are not gated by cache readiness.

### 6.3 Refresh Modes

**Full Refresh (Default):**

Recreates the entire cache table on each refresh.

```yaml
cache:
  enabled: true
  table: customers_cache
  schedule: 1h
```

**Incremental Append:**

Appends only new rows using a cursor column.

| Parameter | Type | Description |
|-----------|------|-------------|
| `cache.cursor.column` | string | Column tracking new rows |
| `cache.cursor.type` | string | Column type: `date`, `timestamp`, `integer` |

```yaml
cache:
  enabled: true
  table: orders_cache
  schedule: 5m
  cursor:
    column: created_at
    type: timestamp
```

**Incremental Merge:**

Handles inserts, updates, and deletes using cursor + primary key.

| Parameter | Type | Description |
|-----------|------|-------------|
| `cache.primary-key` | array[string] | Column(s) for identifying rows |
| `cache.cursor.column` | string | Column tracking all changes |
| `cache.cursor.type` | string | Column type |

```yaml
cache:
  enabled: true
  table: customers_cache
  schedule: 1m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
```

### 6.4 Retention Policies

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cache.retention.keep-last-snapshots` | integer | - | Number of snapshots to keep |
| `cache.retention.max-snapshot-age` | string | - | Maximum snapshot age (e.g., `7d`, `2h`) |

Without its own `retention:` block a cache uses `ducklake.retention`; with one, the endpoint's block
replaces the global one entirely. Expired snapshots' data files are deleted from storage afterwards.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cache.rollback-window` | string | - | **Deprecated, no effect.** Parsed but never read; startup logs a warning. Use DuckLake time travel over retained snapshots instead. |
| `cache.delete-handling` | string | - | **Deprecated, no effect.** Parsed but never read; startup logs a warning. Handle deletes in the cache template (see [Deletes in merge mode](guides/caching.md#deletes-in-merge-mode)). |

**Example:**

```yaml
cache:
  enabled: true
  table: customers_cache
  schedule: 5m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  retention:
    keep-last-snapshots: 5
    max-snapshot-age: 14d
```

### 6.5 Cache Template Variables

Special variables available in cache-enabled SQL templates:

| Variable | Description |
|----------|-------------|
| `{{cache.table}}` | Cache table name |
| `{{cache.schema}}` | Cache schema name |
| `{{cache.catalog}}` | DuckLake catalog alias |
| `{{cache.previousSnapshotTimestamp}}` | The watermark for an incremental refresh. With a `cursor:` configured it is `max(<cursor column>)` over the rows actually cached; without one it falls back to the commit timestamp of this table's last completed refresh. |
| `{{cache.snapshotTimestamp}}` / `{{cache.snapshotId}}` | Commit time (UTC) and id of the newest snapshot that wrote this table before this refresh, i.e. its last refresh that changed rows (a 0-row refresh writes none). Read before this refresh writes anything, so it is never this refresh's own snapshot. **Not** the same value as `previousSnapshotTimestamp` when a `cursor:` is configured — see below. Do not use it as an incremental watermark. Kept with `--full`. |
| `{{cache.previousSnapshotId}}` | The same snapshot id. Absent for a table no snapshot has touched yet, and with `--full`. It is **kept** when a `cursor:` is configured but no watermark can be read (an empty cache table, a value that does not match `cursor.type`): there only `previousSnapshotTimestamp` is erased, so guard the full-load branch on `{{^cache.previousSnapshotTimestamp}}`. |

> `{{cache.currentSnapshotTimestamp}}` appeared in an earlier version of this
> table and has never existed — the implemented name is
> `{{cache.snapshotTimestamp}}`. A template using the old name renders an empty
> string, which inside a `WHERE ... > TIMESTAMP '...'` is a SQL error rather
> than a silent one.
>
> `previousSnapshotTimestamp` also used to carry the refresh *before* last, so
> an append template re-read rows the previous refresh had already appended.
> It is now the last completed refresh, as documented here.
>
> **With a `cursor:` it is derived from the data, not from snapshot metadata.**
> A snapshot's timestamp is the instant a refresh *committed*, but that refresh
> read the source at some earlier instant. A source row written in between was
> never read by that refresh, and a `WHERE updated_at > '<commit time>'` filter
> excludes it from the next one too — so on a continuously-written source every
> refresh permanently dropped the rows written while it ran, and reported
> success. `max(<cursor>)` over what is actually cached has no such window:
> rows above it are exactly the rows not yet loaded. A row exactly at the
> boundary may be re-read, which is idempotent under merge and a bounded,
> visible duplicate under append — losing it is neither.

> **Validated against `cursor.type` before it is used.** The watermark comes
> from cached data, which comes from your source, and it is rendered into the
> refresh template and executed — so a value that does not look like the
> declared type is dropped and that refresh loads the full source (with a
> warning). Declare the cursor's real type: `timestamp`/`date`/`time` and the
> numeric types accept only the characters they are made of, and anything else
> rejects quotes, semicolons, comment markers and newlines.
>
> **Note:** the value is rendered raw, so quote or cast it in the template as
> its type requires — the `TIMESTAMP '...'` form in the example below is right
> for a timestamp cursor only. Use the **triple**-brace form
> `{{{cache.previousSnapshotTimestamp}}}` inside a quoted SQL literal: the
> double-brace form HTML-escapes what it renders, which corrupts any value
> containing a quote.
>
> **`{{cache.snapshotTimestamp}}` is a different value.** It is still the
> snapshot commit time, so a template using it as a watermark keeps the
> row-dropping behaviour described above. Use `previousSnapshotTimestamp`.
>
> **The comparison should match your mode.** The templates here use `>`, so a
> row arriving later that carries *exactly* `max(<cursor>)` is not re-read —
> it is skipped. That residual window is far narrower than the one closed
> above (it needs a second row at the same cursor value, written after the
> refresh read), but it is real. With `primary-key` set (merge mode) `>=` is
> safe and closes it, because a re-read row collapses on the key. In append
> mode `>=` duplicates instead, so either accept the window or give the cursor
> a strictly increasing unique value.

**Example Template:**

```sql
-- Incremental refresh template
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT * FROM source_table
{{#cache.previousSnapshotTimestamp}}
WHERE updated_at > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'
{{/cache.previousSnapshotTimestamp}}
```

> **Implementation:** `src/cache_manager.cpp` | **Tests:** `test/integration/test_ducklake_comprehensive.tavern.yaml`, `test/integration/test_ducklake_advanced.py`

---

## 7. Authentication

Authentication can be configured globally (in flapi.yaml) or per-endpoint (in endpoint YAML). A global
block applies to every endpoint without its own `auth:` block; an endpoint's block replaces it, and
`auth: {enabled: false}` opts an endpoint out (§2.7).

**Roles.** Every method below accepts `auth.roles`, a list of role names. A caller who authenticates but
holds none of them gets **`403`** `{"error":"Forbidden","message":"The authenticated principal does not hold any role this endpoint requires."}`;
an unauthenticated caller gets `401`. An empty or missing `roles` list admits any authenticated caller.
Basic-auth users get their roles from `users[].roles`; JWT/OIDC callers from the token's roles claim.

```yaml
auth:
  enabled: true
  type: basic
  roles: [admin]          # reader below authenticates, then gets 403
  users:
    - username: admin
      password: '{{env.ADMIN_PASSWORD}}'
      roles: [admin]
    - username: reader
      password: '{{env.READER_PASSWORD}}'
      roles: [read]
```

### 7.1 Basic Authentication

Username/password authentication with HTTP Basic auth.

| Parameter | Type | Description |
|-----------|------|-------------|
| `auth.enabled` | boolean | Enable authentication |
| `auth.type` | string | Must be `basic` |
| `auth.users` | array | List of authorized users |
| `auth.users[].username` | string | Username (supports env vars) |
| `auth.users[].password` | string | Password (supports env vars) |
| `auth.users[].roles` | array[string] | Assigned roles |

**Example:**

```yaml
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: '{{env.ADMIN_PASSWORD}}'
      roles: [admin, read, write]
    - username: reader
      password: '{{env.READER_PASSWORD}}'
      roles: [read]
```

### 7.2 JWT Authentication

JSON Web Token authentication.

| Parameter | Type | Description |
|-----------|------|-------------|
| `auth.enabled` | boolean | Enable authentication |
| `auth.type` | string | `jwt` — an alias of `bearer` (§7.3); both behave identically |
| `auth.jwt-secret` | string | Secret key for JWT validation |
| `auth.jwt-issuer` | string | Expected issuer claim (§7.3) |
| `auth.jwt-audience` | string or array[string] | Accepted audiences (§7.3) |
| `auth.jwt-require-exp` | boolean | Refuse tokens without `exp` (default `true`, §7.3) |

**Example:**

```yaml
auth:
  enabled: true
  type: jwt
  jwt-secret: '{{env.JWT_SECRET}}'
  jwt-issuer: my-auth-server
```

### 7.3 Bearer Authentication

Bearer token authentication with an HS256-signed JWT. `type: jwt` is the same method under another name.

| Parameter | Type | Description |
|-----------|------|-------------|
| `auth.enabled` | boolean | Enable authentication |
| `auth.type` | string | Must be `bearer` |
| `auth.jwt-secret` | string | Secret key for token validation |
| `auth.jwt-issuer` | string | Expected `iss`. Unset: `iss` is not checked, and startup reports `AUTH_JWT_ISSUER_UNCHECKED` |
| `auth.jwt-audience` | string or array[string] | The token's `aud` (a string or an array) must contain one of these. Unset: not checked. An empty entry stops startup |
| `auth.jwt-require-exp` | boolean | Default `true`: a token without `exp` is refused. `false` accepts one (an expired `exp` is still refused) |

**Example:**

```yaml
auth:
  enabled: true
  type: bearer
  jwt-secret: '{{env.API_TOKEN_SECRET}}'
  jwt-issuer: api-gateway
  jwt-audience: orders-api
```

A token is accepted when it is signed with HS256 and `jwt-secret` (any other `alg`, `none` included, is
refused), carries `exp` in the future (unless `jwt-require-exp: false`), has no `nbf` or `iat` in the future,
matches `jwt-issuer` and `jwt-audience` where they are set, and has a string `sub`. `sub` becomes
`auth.username`, a string `email` claim `auth.email`, and `roles` (an array of strings, or one string)
`auth.roles`; a token without `roles` authenticates with no roles (so an endpoint `roles:` list answers 403,
not 401). REST and MCP (`mcp.auth.type: bearer`) run the same check (`src/bearer_token_verifier.cpp`).

### 7.4 OIDC Authentication

OpenID Connect authentication with external identity providers.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `auth.oidc.issuer-url` | string | preset / **required** for `generic` | OIDC provider URL. May contain `{tenant}`, `{realm}`, `{domain}` placeholders |
| `auth.oidc.client-id` | string | **required** for presets | OAuth client ID; the expected `aud` when `allowed-audiences` is empty |
| `auth.oidc.client-secret` | string | - | Parsed but has no effect (flAPI never calls the token endpoint); startup warns |
| `auth.oidc.provider-type` | string | `generic` | Provider: `google`, `microsoft` (aliases `azure`, `azure-ad`, `entra`), `keycloak`, `auth0`, `okta`, `generic`. `github` stops startup: GitHub OAuth apps issue no OIDC tokens; for GitHub Actions use `generic` with `issuer-url: https://token.actions.githubusercontent.com` |
| `auth.oidc.tenant-id` | string | - | Substituted for `{tenant}` in `issuer-url` (alias key `tenant`). Microsoft's default issuer is `https://login.microsoftonline.com/{tenant}/v2.0` |
| `auth.oidc.realm` | string | - | Substituted for `{realm}`. Keycloak's default issuer is `https://{domain}/realms/{realm}` |
| `auth.oidc.domain` | string | - | Substituted for `{domain}` (Keycloak host, Auth0/Okta tenant name). Auth0: a domain with a dot is the full host, and the issuer always ends with `/` |
| `auth.oidc.allowed-audiences` | array[string] | `[client-id]` | Acceptable `aud` claims; one must be in the token. Empty: `aud` must contain `client-id` (no check only when that is empty too) |
| `auth.oidc.verify-expiration` | boolean | `true` | Verify token expiration |
| `auth.oidc.clock-skew-seconds` | integer | `300` | Allowed clock skew for `exp`, `nbf` and `iat` (the latter two may not be in the future) |
| `auth.oidc.username-claim` | string | `sub` | Claim for username (`auth.username`). Written here, it wins over the preset's default |
| `auth.oidc.email-claim` | string | `email` | Claim for email (`auth.email`) |
| `auth.oidc.roles-claim` | string | `roles` | Claim for roles (`auth.roles`) |
| `auth.oidc.groups-claim` | string | `groups` | Claim for groups: `auth.groups` (comma-joined); grants nothing |
| `auth.oidc.role-claim-path` | string | - | Roles claim that wins over `roles-claim` (Keycloak preset: `realm_access.roles`) |
| `auth.oidc.enable-client-credentials` | boolean | `false` | Parsed but has no effect; startup warns |
| `auth.oidc.enable-refresh-tokens` | boolean | `false` | Parsed but has no effect; startup warns |
| `auth.oidc.scopes` | array[string] | - | Parsed but has no effect; startup warns |
| `auth.oidc.jwks-cache-hours` | integer | `24` | JWKS cache duration |

**Example:**

```yaml
auth:
  enabled: true
  type: oidc
  oidc:
    provider-type: microsoft        # or azure / entra
    tenant-id: '{{env.AZURE_TENANT_ID}}'
    client-id: '{{env.AZURE_CLIENT_ID}}'
    client-secret: '{{env.AZURE_CLIENT_SECRET}}'
    allowed-audiences:
      - api://my-api
    roles-claim: roles
```

```yaml
# Keycloak: issuer becomes https://kc.example.com/realms/acme
auth:
  enabled: true
  type: oidc
  oidc:
    provider-type: keycloak
    domain: kc.example.com
    realm: acme
    client-id: my-api
    role-claim-path: realm_access.roles
```

Token signatures: RS256/384/512 and PS256/384/512 with RSA keys, ES256/384/512 with EC keys on
P-256/P-384/P-521. The token's `alg` must fit the key its `kid` names: the key type, the EC curve, and the
JWK's own `alg` when it has one. `none`, HS* and anything else are refused.

Claim names (`username-claim`, `roles-claim`, ...) are looked up as written first, so
`https://example.com/roles` is one claim; only when no claim has that name is a dot a path into nested
objects (`realm_access.roles`). A role or group containing a comma or a control character refuses the
token (401).

A placeholder left in `issuer-url` after substitution stops startup with an error naming the key to set,
e.g. `'issuer-url' (https://login.microsoftonline.com/{tenant}/v2.0) still contains the {tenant} placeholder. Set tenant-id in the oidc block, or write the provider's real issuer URL into issuer-url.`

### 7.5 AWS Secrets Manager

Basic-auth users from an AWS Secrets Manager secret, in an endpoint's `auth:` block or the top-level one.
The secret's `SecretString` is `{"auth": [{"username": "...", "password": "...", "roles": ["..."]}]}`
(passwords plaintext, MD5 or `$pbkdf2-sha256$...`, as for `users:`); the
[authentication guide](guides/authentication.md#users-from-aws-secrets-manager) has the details.

| Parameter | Type | Description |
|-----------|------|-------------|
| `auth.from-aws-secretmanager.secret-name` | string | AWS secret name or ARN (required) |
| `auth.from-aws-secretmanager.region` | string | AWS region; else `AWS_REGION`, else `AWS_DEFAULT_REGION`, else the active AWS profile's region, else a startup error |
| `auth.from-aws-secretmanager.secret-id` | string | AWS access key ID. Set together with `secret-key`, or neither (the AWS SDK's default credential chain) |
| `auth.from-aws-secretmanager.secret-key` | string | AWS secret access key |
| `auth.from-aws-secretmanager.secret-table` | string | **Ignored** (a startup warning says so). The user list is held in flAPI's memory, not in a table |
| `auth.from-aws-secretmanager.init` | string | Deprecated: SQL creating a DuckDB S3 secret named after `secret-name` (non-alphanumerics → `_`). Runs in a private DuckDB, never the one templates use, so it has no side effects there |

- Read by `type: basic` blocks only. The secret is fetched **once, at startup**, per `secret-name`, however many
  endpoints share it; blocks sharing a name must agree on region, credentials and `init`. To rotate users, update
  the secret and restart flAPI.
- `AWS_ENDPOINT_URL_SECRETS_MANAGER` replaces AWS's endpoint unless `AWS_IGNORE_CONFIGURED_ENDPOINT_URLS=true`
  (startup warns while it is active). `AWS_ENDPOINT_URL` is not used for it. Plain `http://` only to a loopback
  host, or with `FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT=1`.
- Tables `auth_<name>` / `secret-table` an older version left in the main database are dropped at startup.
- Neither the credentials nor the user list reach the DuckDB database SQL templates run on, and neither is logged.

**Example:**

```yaml
auth:
  enabled: true
  type: basic
  from-aws-secretmanager:
    secret-name: prod/api/credentials
    region: us-east-1
    secret-id: '{{env.AWS_ACCESS_KEY}}'
    secret-key: '{{env.AWS_SECRET_KEY}}'
```

> **Implementation:** `src/auth_middleware.cpp`, `src/oidc_auth_handler.cpp` | **Tests:** `test/cpp/auth_middleware_test.cpp`, `test/integration/test_oidc_authentication.py`

---

## 8. Operation Configuration

Specifies how database operations are executed.

### 8.1 Read Operations

Read operations (queries) are the default for `GET`, `HEAD`, and `OPTIONS` methods.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `operation.type` | string | `Read` | Operation type (auto-detected) |

### 8.2 Write Operations

Write operations are auto-detected for `POST`, `PUT`, `PATCH`, and `DELETE` methods. All four run the
write path: parameters are extracted and validated, the template is rendered and executed, and the
response is `{"rows_affected": n}` with status `201` for `POST` and `200` for `PUT`/`PATCH`/`DELETE`; rows a
`RETURNING` sends back come under `data` (`returns-data: true` makes `data` present, `[]`, even when there are none). `n` is the number
of rows changed - DuckDB's count, summed over the template's statements; for a `RETURNING` statement, the rows
it returned - so an `UPDATE` of five rows answers 5 and a `DELETE` that matched nothing answers 0. A `DELETE`
endpoint is no different — its template is the `DELETE FROM …` statement, and as an MCP tool it is a write
too (cache maintenance after it, write result). With `returns-data: true` the body always has `data`:
`[]` when the `RETURNING` matched no row.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `operation.type` | string | `Write` | Operation type |
| `operation.returns-data` | boolean | `false` | Whether operation returns results (RETURNING clause) |
| `operation.transaction` | boolean | `true` | Wrap in transaction |
| `operation.validate-before-write` | boolean | `true` | Apply strict validation |

**Example:**

```yaml
url-path: /customers/:id
method: PATCH

operation:
  type: Write
  returns-data: true
  transaction: true
  validate-before-write: true

# The cache this write refreshes: name the read endpoint's table and populate template.
# A write refreshes only the cache its OWN block names; without `table` nothing is refreshed.
cache:
  enabled: true
  table: products_cache
  template-file: products_cache.sql
  invalidate-on-write: true   # refresh the cache synchronously, before the response
```

| Key | When the cache is refreshed |
|-----|------------------------------|
| `cache.invalidate-on-write: true` | **Synchronously**, after the write and before the response is sent. The caller's next read sees the change |
| `cache.refresh-on-write: true` | **Asynchronously**, after the response is sent. Bursts of writes are coalesced into one refresh per endpoint |
| both | Synchronous only (as `invalidate-on-write`) |

MCP tool calls that write follow the same rules.

> **Implementation:** `src/request_handler.cpp`, `src/query_executor.cpp` | **Tests:** `test/integration/test_write_operations.tavern.yaml`

---

## 9. SQL Templates (Mustache)

SQL templates use Mustache syntax for dynamic query generation.

### 9.1 Variable Syntax

**Double Braces `{{ }}`** - For numeric values and identifiers. **When the field has a typed validator that flAPI recognises (`int`, `double`, `boolean`, `date`, `time`, `uuid`, `enum`, `email`, `string`), the renderer replaces this site with a DuckDB `?` placeholder and binds the value via `duckdb_bind_*`** — the rendered SQL is then executed as a prepared statement, and the value can never alter the SQL's structure.

```sql
SELECT * FROM table
WHERE id = {{ params.id }}        -- with `validators: [{type: int}]` → bound as int64
```

The placeholder is only used **outside** `{{#...}}`/`{{^...}}` sections and **outside** SQL string
literals: inside a section the value is rendered as text, and `'{{ params.x }}'` turns into the literal
`'?'` (the request then fails with `500`; the server log says `Failed to bind parameter`). See
[§5.9](#59-the-sql-injection-screen-is-a-safety-net-not-the-defence).

**Triple Braces `{{{ }}}`** - Renders the raw value with no HTML entity escaping. For a typed field at a plain value site outside every section (`= {{{ params.x }}}`, `= '{{{ params.x }}}'`, `LIKE '%{{{ params.x }}}%'`) the rewriter binds the value just like `{{ }}` does (see [§5.9](#59-the-sql-injection-screen-is-a-safety-net-not-the-defence)); everywhere else (inside sections, identifiers, typed literals such as `DATE '...'`, `AT (VERSION|TIMESTAMP => ...)`) it is raw text:

```sql
SELECT * FROM table
WHERE name LIKE '%{{{ params.name }}}%'
```

> **Security note:** Neither brace form performs SQL-specific escaping; Mustache doesn't understand SQL string literals, quote-doubling, or comment syntax. flAPI's SQL-injection defense is **layered**:
>
> 1. `RequestValidator` rejects obviously-bad input before SQL is rendered (typed fields, regex/range/enum/format checks). Strict integer/date/time parsing rejects trailing garbage so `1; DROP TABLE` cannot slip through as `1` and `2024-03-15' OR 1=1` cannot slip through as `2024-03-15`.
> 2. For double-brace references on typed fields, the **DuckDB prepared-statement bind** is the hard boundary — the value travels as a primitive, not as text, and cannot smuggle SQL. This applies uniformly to **GET, POST, PUT, PATCH, and Arrow-streaming endpoints** (multi-statement write templates have their binding plan sliced across statements based on `?` count).
> 3. For triple-brace references that stay raw text (inside sections, identifiers, typed literals) and for non-typed fields, the legacy keyword regex still rejects obvious injection patterns. (It is demoted to a debug-level log only for fields the prepared-statement path already protects — Integer / Double / Boolean / Date / Time — because the regex is a known false-positive source for clean numeric and temporal values.)
>
> The end-to-end injection corpus at `test/integration/test_sql_injection_corpus.py` (99 GET-path payloads spanning all 9 validator types + pagination wraps) and `test/integration/test_sql_injection_write_corpus.py` (19 POST-path payloads including multi-statement RETURNING) exercise these layers. Bind-conversion failures on typed params return HTTP 400 (client error) rather than 500.

### 9.2 Available Contexts

| Context | Description | Example |
|---------|-------------|---------|
| `params.*` | Request parameters | `{{ params.customer_id }}` |
| `conn.*` | Connection properties | `{{{ conn.path }}}` |
| `cache.*` | Cache metadata | `{{ cache.table }}` |
| `env.*` | Whitelisted environment variables | `{{{ env.API_KEY }}}` |
| `auth.*` | Authentication context | `'{{{ auth.username }}}'`, `'{{{ auth.roles }}}'`, `'{{{ auth.email }}}'`, `'{{{ auth.type }}}'` |

**`auth.*` values are SQL-quoted for a plain top-level `'...'` literal of the outer query
only.** `auth.username`, `auth.email`, `auth.roles` and `auth.type` come from the identity
provider (Basic username, JWT `sub` and `roles`, OIDC claims) and are rendered, never bound as
prepared parameters, so every single quote in them is doubled (`O'Brien` renders `O''Brien`).
Write them inside a plain single-quoted literal, e.g. `WHERE owner = '{{{ auth.username }}}'`;
that matches the stored `O'Brien`. `{{ }}` and `{{{ }}}` render the same value here.

Plain top-level `'...'` of the outer query only. Refused:

- unquoted, `E'...'` (a backslash escapes the quote there), a `--` or `/* */` comment, a
  `"..."` identifier or a `$$` string;
- **SQL text that another function executes.** A literal passed, at any depth, to
  `query()`, `query_table()`, `bigquery_query()`, `postgres_query()`, `mysql_query()`,
  `snowflake_query()`, `odbc_query()`, `sqlite_query()` or any other `*_query` / `*_execute`
  function, any literal that itself holds a doubled quote `''`, a `$$`/`$tag$` or a `"`,
  and any literal in a call whose other arguments hold one (a macro handed SQL text), is SQL
  that is parsed again, where the doubling no longer protects. The function name is found
  across comments and in its `"quoted"` form. Push the query down without `auth.*` and filter
  in the outer query: `SELECT * FROM bigquery_query('proj', 'SELECT ...') WHERE owner =
  '{{{ auth.username }}}'`.

The check runs on the **rendered** SQL of every request (with a random placeholder for each
value, before the real values are substituted), so no Mustache comment or section can hide
where a value lands. The template source is also checked at startup, reload,
`--validate-config` and by the config service, which refuse such a template:

```
Unsafe use of auth.* in sqls/docs.sql:1:52: 'auth.username' is inside a -- comment. ...
```

A quoted value still carries meaning inside some functions: `%` and `_` in `LIKE`, regular
expression metacharacters in `regexp_matches`, glob characters (`*`, `?`, `[`) in `GLOB` and
in file paths such as `read_csv('data/{{{ auth.username }}}.csv')`, and path segments in a
file URL. Compare with `=`, and do not build file paths from `auth.*`.

An identity containing a control character (newline, carriage return, NUL, ...), or a role
containing a comma (roles are comma-joined in `auth.roles`), is refused with 401 on REST and
MCP, never cleaned up. `auth.authenticated` is `true` or empty and may be
used anywhere.

### 9.3 Conditional Sections

**Conditional Rendering (if exists and truthy):**

```sql
SELECT * FROM customers
WHERE 1=1
{{#params.id}}
  AND id = {{ params.id }}
{{/params.id}}
{{#params.name}}
  AND name LIKE '%{{{ params.name }}}%'
{{/params.name}}
```

**Inverted Sections (if not exists or falsy):**

```sql
{{^params.status}}
  AND status <> 'archived'
{{/params.status}}
```

### 9.4 Best Practices

**1. Use `WHERE 1=1` Pattern:**

```sql
SELECT * FROM table
WHERE 1=1
{{#params.filter1}}
  AND column1 = {{{ params.filter1 }}}
{{/params.filter1}}
```

**2. Prefer Double Braces for Typed Equality Lookups:**

When the field has a typed validator (`int`, `double`, `boolean`, `date`, `time`, `uuid`, `enum`, `email`, `string`), use the double-brace form **without** wrapping quotes. The renderer emits a `?` and binds the value via DuckDB's prepared-statement API — no quotes are needed and SQL injection is structurally impossible.

```sql
-- RECOMMENDED for typed equality: the value is bound via duckdb_bind_*
WHERE id = {{ params.id }}              -- field has `validators: [{type: int}]`
WHERE email = {{ params.email }}        -- field has `validators: [{type: email}]`

-- LIKE patterns and quoted sites are bound too (rewritten to ('%' || ? || '%') / ?)
WHERE name LIKE '%{{{ params.name }}}%'
WHERE name = '{{{ params.name }}}'
```

Use triple braces (`{{{ params.X }}}`) inside sections and when the value is part of the SQL text — an identifier (validate it with `enum`), a typed literal (`DATE '{{{ params.d }}}'`), or an `AT (VERSION|TIMESTAMP => ...)` argument; those sites are rendered raw. A bare double-brace `{{ params.x }}` inside `AT (...)` stays bound: that works for `int` and `date` fields, but DuckLake cannot take a bound string there, so write `AT (TIMESTAMP => CAST('{{{ params.ts }}}' AS TIMESTAMP))` with a strict `pattern:` validator on `ts`.

**3. Provide Default Values:**

```sql
ORDER BY created_at {{#params.order}}{{{ params.order }}}{{/params.order}}{{^params.order}}DESC{{/params.order}}
```

Do **not** do this for `limit`/`offset`. With `with-pagination: true` (the default) they are reserved:
flAPI wraps the query and applies `?limit=`/`?offset=` itself (100 rows per page by default). A template
`LIMIT {{ params.limit }}` runs before flAPI's offset, so `?limit=2&offset=2` returns `"data": []`.

**4. Use Connection Properties for File Paths:**

```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
```

> **Implementation:** `src/sql_template_processor.cpp` | **Tests:** `test/cpp/sql_template_processor_test.cpp`

---

## 10. Environment Variables

### 10.1 Substitution Syntax

Environment variables are substituted into configuration files — the main config,
endpoint files and included files — with one syntax:

```yaml
password: '{{env.DB_PASSWORD}}'
username: '{{env.API_USER}}'
```

The shell form `${DB_PASSWORD}` is **not** supported: it is left as literal text, so a
password written that way would be the string `${DB_PASSWORD}`. Some examples elsewhere in
these docs still show it; the working form is `{{env.NAME}}`.

### 10.2 Whitelist Configuration

Environment variables must be whitelisted to be read. A `{{env.NAME}}` whose name matches no
pattern stops startup with an error that names it and this key; **an empty or missing
whitelist allows no variables**:

```yaml
# flapi.yaml
template:
  environment-whitelist:
    - '^FLAPI_.*'       # All FLAPI_ prefixed variables
    - '^DB_.*'          # All DB_ prefixed variables
    - '^API_KEY$'       # Exact match for API_KEY
    - '^JWT_SECRET$'    # Exact match for JWT_SECRET
```

**Whitelist Pattern Examples:**

| Pattern | Matches |
|---------|---------|
| `^FLAPI_.*` | `FLAPI_PORT`, `FLAPI_DEBUG`, etc. |
| `^DB_.*` | `DB_HOST`, `DB_PASSWORD`, etc. (patterns match the *whole* name, so `^DB_` alone would match only a variable called `DB_`) |
| `^API_KEY$` | Only `API_KEY` |
| `.*_SECRET$` | `JWT_SECRET`, `API_SECRET`, etc. |

> **Implementation:** `src/config_manager.cpp` | **Tests:** `test/cpp/config_manager_test.cpp`

---

## Appendix A: Complete Example Configuration

### Main Configuration (flapi.yaml)

```yaml
# Project metadata
project-name: Customer API
project-description: REST and MCP API for customer data
http-port: 8080

# Template configuration
template:
  path: ./sqls
  environment-whitelist:
    - '^FLAPI_.*'
    - '^DB_.*'
    - '^JWT_SECRET$'
    - '^API_.*'

# Database connections
connections:
  customers-db:
    init: |
      INSTALL postgres;
      LOAD postgres;
    properties:
      host: '{{env.DB_HOST}}'
      port: '5432'
      database: customers
      user: '{{env.DB_USER}}'
      password: '{{env.DB_PASSWORD}}'

  local-parquet:
    properties:
      path: ./data/customers.parquet

# DuckDB settings
duckdb:
  db_path: ./cache.db
  access_mode: READ_WRITE
  threads: 4
  max_memory: 4GB

# DuckLake caching
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache/metadata
  data-path: ./cache/data
  retention:
    keep-last-snapshots: 10
    max-snapshot-age: 30d
  # The scheduler is on by default; scan-interval only overrides how often it checks.
  scheduler:
    scan-interval: 1m

# MCP server (shares the HTTP port)
mcp:
  enabled: true
  instructions-file: ./mcp_instructions.md

# Global authentication
auth:
  enabled: false

# Global rate limiting
rate_limit:
  enabled: true
  max: 100
  interval: 60

# Heartbeat
heartbeat:
  enabled: true
  worker-interval: 30
```

### Endpoint Configuration (sqls/customers.yaml)

```yaml
# REST endpoint
url-path: /customers/:id
method: GET

# Also expose as MCP tool
mcp-tool:
  name: get_customer
  description: Retrieve customer by ID
  result-mime-type: application/json

# Request parameters
request:
  - field-name: id
    field-in: path
    description: Customer ID
    required: true
    validators:
      - type: int
        min: 1
        preventSqlInjection: true

  - field-name: include_orders
    field-in: query
    description: Include recent orders
    required: false
    default: "false"
    validators:
      - type: enum
        allowedValues: ["true", "false"]

# SQL template
template-source: customers-get.sql

# Connection
connection:
  - customers-db

# Pagination
with-pagination: false

# Caching
cache:
  enabled: true
  table: customers_cache
  schedule: 5m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  retention:
    keep-last-snapshots: 5

# Authentication
auth:
  enabled: true
  type: jwt
  jwt-secret: '{{env.JWT_SECRET}}'

# Rate limiting
rate-limit:
  enabled: true
  max: 50
  interval: 60
```

### SQL Template (sqls/customers-get.sql)

```sql
SELECT
    c.id,
    c.name,
    c.email,
    c.segment,
    c.created_at,
    c.updated_at
    {{#params.include_orders}}
    , (
        SELECT json_agg(o.*)
        FROM orders o
        WHERE o.customer_id = c.id
        ORDER BY o.created_at DESC
        LIMIT 5
    ) as recent_orders
    {{/params.include_orders}}
FROM customers c
WHERE c.id = {{ params.id }}
```

---

## Appendix B: Naming Conventions

flAPI supports both hyphenated and camelCase naming for backward compatibility:

| Preferred (Hyphenated) | Alternative (CamelCase) |
|------------------------|-------------------------|
| `field-name` | - |
| `field-in` | - |
| `url-path` | - |
| `template-source` | - |
| `with-pagination` | - |
| `primary-key` | `primaryKey` |
| `jwt-secret` | - |
| `jwt-issuer` | - |
| `rate-limit` | - |
| `prevent-sql-injection` | `preventSqlInjection` |
| `allowed-values` | `allowedValues` |
| `returns-data` | `returnsData` |
| `rollback-window` (deprecated, no effect) | `rollbackWindow` |
| `delete-handling` (deprecated, no effect) | `deleteHandling` |

**Recommendation:** Use hyphenated naming for consistency.

---

## Appendix C: Default Values Reference

| Configuration | Default Value |
|---------------|---------------|
| `http-port` | `8080` |
| `http-host` | `"0.0.0.0"` |
| `duckdb.db_path` | `:memory:` |
| `duckdb.access_mode` | `READ_WRITE` |
| `ducklake.alias` | `"cache"` |
| `ducklake.enabled` | `false` |
| `ducklake.scheduler.enabled` | `true` |
| `ducklake.scheduler.scan-interval` | `min(heartbeat.worker-interval, shortest cache.schedule)`, at least 1 s |
| `ducklake.compaction.enabled` | `false` |
| `mcp.enabled` | `true` |
| `mcp.auth.enabled` | `false` |
| `auth.enabled` | `true` when the `auth:` block configures authentication, else `false` |
| `auth.roles` | `[]` (any authenticated caller) |
| `project-description` | `""` |
| `rate_limit.enabled` | `false` |
| `rate_limit.max` | `100` |
| `rate_limit.interval` | `60` |
| `heartbeat.enabled` | `false` |
| `heartbeat.worker-interval` | `60` |
| `enforce-https.enabled` | `false` |
| `method` | `GET` |
| `with-pagination` | `true` |
| `request-fields-validation` | `false` |
| `required` | `false` |
| `cache.enabled` | `false` |
| `cache.schema` | `"cache"` |
| `operation.type` | Auto-detected from method |
| `operation.returns-data` | `false` |
| `operation.transaction` | `true` |
| `operation.validate-before-write` | `true` |
| `preventSqlInjection` | `true` |
| `mcp-tool.result-mime-type` | `application/json` |
| `mcp-resource.mime-type` | `application/json` |
| `auth.oidc.verify-expiration` | `true` |
| `auth.oidc.clock-skew-seconds` | `300` |
| `auth.oidc.username-claim` | `sub` |
| `auth.oidc.email-claim` | `email` |
| `auth.oidc.roles-claim` | `roles` |
| `auth.oidc.groups-claim` | `groups` |
| `auth.oidc.jwks-cache-hours` | `24` |

---

## Related Documentation

- **[Documentation index](./README.md)** - Start here; organised by what you are trying to do
- **[CLI Reference](./CLI_REFERENCE.md)** - Server executable command-line options
- **[Config Service API Reference](./CONFIG_SERVICE_API_REFERENCE.md)** - Runtime configuration REST API and CLI client
- **[MCP Reference](./MCP_REFERENCE.md)** - Model Context Protocol specification and implementation
- **[MCP Configuration Tools API](./MCP_CONFIG_TOOLS_API.md)** - 20 MCP tools for configuration management
- **[MCP Configuration Integration Guide](./MCP_CONFIG_INTEGRATION.md)** - Integration architecture and request/response flows
- **[Cloud Storage Guide](./CLOUD_STORAGE_GUIDE.md)** - Using cloud storage backends (S3, GCS, Azure)
