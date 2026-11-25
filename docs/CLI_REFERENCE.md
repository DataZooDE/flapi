# flAPI Server - CLI Reference

**flAPI Version:** v26.11.25 (releases are numbered `vYY.MM.DD`)

This document provides a complete reference for the `flapi` server executable's command-line options and runtime behavior.

---

## Table of Contents

1. [Overview](#1-overview)
   - [Server Architecture](#server-architecture)
   - [Quick Start](#quick-start)
2. [Command-Line Options](#2-command-line-options)
   - [Configuration File](#configuration-file--c---config)
   - [Server Port](#server-port--p---port)
   - [Bind Host](#bind-host---host)
   - [Log Level](#log-level---log-level)
   - [Validate Configuration](#validate-configuration---validate-config)
   - [Configuration Service](#configuration-service---config-service)
   - [Configuration Service Token](#configuration-service-token---config-service-token)
   - [Cache Warm-up](#cache-warm-up---cache-warmup---no-cache-warmup)
3. [Self-Packaging Subcommands](#3-self-packaging-subcommands)
   - [`flapi pack`](#flapi-pack----create-a-self-contained-binary)
   - [`flapi info`](#flapi-info----inspect-the-running-binarys-bundle)
   - [`flapi unpack`](#flapi-unpack---to-dir----dump-the-bundle-for-debugging)
   - [macOS notarisation specifics](#macos-notarisation-specifics)
   - [`flapi cache refresh`](#flapi-cache-refresh----refresh-caches-once-and-exit)
   - [`flapi extensions install`](#flapi-extensions-install----pre-install-duckdb-extensions)
4. [Environment Variables](#4-environment-variables)
5. [Usage Examples](#5-usage-examples)
   - [Basic Startup](#basic-startup)
   - [Development Mode](#development-mode)
   - [Production Mode](#production-mode)
   - [CI/CD Validation](#cicd-validation)
6. [Runtime Health Endpoints](#6-runtime-health-endpoints)
7. [Signal Handling](#7-signal-handling)
8. [Exit Codes](#8-exit-codes)
- [Related Documentation](#related-documentation)

---

## 1. Overview

### Server Architecture

flAPI runs as a unified server providing both REST API and MCP (Model Context Protocol) endpoints on a single port:

```
┌─────────────────────────────────────────┐
│           flAPI Server                  │
│         (Single Binary)                 │
├─────────────────────────────────────────┤
│  Port 8080 (default)                    │
│  ├── REST API endpoints (/)             │
│  ├── MCP endpoints (/mcp)               │
│  └── Config Service API (/_config)      │
└─────────────────────────────────────────┘
```

**Key characteristics:**
- Single binary deployment
- Embedded DuckDB database engine
- REST and MCP on the same port
- Optional runtime configuration API

### Quick Start

```bash
# Start with default configuration (flapi.yaml in current directory)
./flapi

# Start with custom configuration file
./flapi -c /path/to/config.yaml

# Start on a specific port
./flapi -p 9000

# Validate configuration without starting server
./flapi --validate-config

# Print the build version
./flapi --version
```

Once the port is bound the server logs
`flAPI server listening on http://<host>:<port> (REST API; MCP at http://<host>:<port>/mcp/jsonrpc)`.

---

## 2. Command-Line Options

### Configuration File (`-c`, `--config`)

Specifies the path to the flAPI YAML configuration file.

| Property | Value |
|----------|-------|
| Short form | `-c` |
| Long form | `--config` |
| Type | string (file path) |
| Default | `flapi.yaml` |
| Required | No |
| Environment variable | `FLAPI_CONFIG` |

**Description:**

The configuration file defines connections, endpoint directories, DuckDB settings, authentication, caching, and other server options. The path can be absolute or relative to the current working directory.

**Precedence (highest wins):**
1. `-c` / `--config` CLI flag
2. `FLAPI_CONFIG` environment variable
3. Built-in default (`flapi.yaml` in the current working directory)

**Example:**

```bash
# Use default flapi.yaml in current directory
./flapi

# Specify a custom configuration file
./flapi -c production.yaml
./flapi --config /etc/flapi/config.yaml

# Point at a config via environment variable (12-factor style)
export FLAPI_CONFIG=/etc/flapi/production.yaml
./flapi
```

**See also:** [Configuration Reference](./CONFIG_REFERENCE.md) for configuration file options.

> **Implementation:** `src/main.cpp`, `src/config_manager.cpp` | **Tests:** `test/cpp/config_manager_test.cpp`, `test/integration/test_env_overrides.py`

---

### Server Port (`-p`, `--port`)

Overrides the HTTP server port defined in the configuration file.

| Property | Value |
|----------|-------|
| Short form | `-p` |
| Long form | `--port` |
| Type | integer |
| Default | From config file (typically `8080`) |
| Required | No |
| Environment variable | `FLAPI_PORT` |

**Description:**

When specified, this option overrides the `http-port` value in the configuration file. Useful for running multiple instances or when port configuration needs to be dynamic.

**Precedence (highest wins):**
1. `-p` / `--port` CLI flag
2. `FLAPI_PORT` environment variable
3. `http-port` from `flapi.yaml`
4. Built-in default (`8080`)

Invalid `FLAPI_PORT` values (non-integer, `<1`, `>65535`) cause flapi to
exit with a single-line error -- a typo like `FLAPI_PORT=abc` surfaces
immediately rather than silently falling through to the config-file value.

**Example:**

```bash
# Start on port 9000
./flapi -p 9000

# Override config file port
./flapi -c production.yaml --port 80

# 12-factor: pick up the port from the environment
export FLAPI_PORT=9000
./flapi
```

> **Implementation:** `src/main.cpp`, `src/api_server.cpp` | **Tests:** `test/integration/test_env_overrides.py`, `test/integration/conftest.py`

---

### Bind Host (`--host`)

Overrides the bind address (`http-host`) defined in the configuration file.

| Property | Value |
|----------|-------|
| Long form | `--host` |
| Type | string |
| Default | From config file (`0.0.0.0` if unset) |
| Required | No |
| Environment variable | `FLAPI_HOST` |

**Description:**

Controls which network interface the HTTP server binds on. Use
`127.0.0.1` to restrict access to the loopback interface only, or
`0.0.0.0` to accept connections on all interfaces.

**Precedence (highest wins):**
1. `--host` CLI flag
2. `FLAPI_HOST` environment variable
3. `http-host` from `flapi.yaml`
4. Built-in default (`0.0.0.0`)

**Example:**

```bash
# Loopback only (useful behind a reverse proxy)
./flapi --host 127.0.0.1

# 12-factor: pick up the host from the environment
export FLAPI_HOST=127.0.0.1
./flapi
```

> **Implementation:** `src/main.cpp`, `src/api_server.cpp`, `src/config_manager.cpp` | **Tests:** `test/integration/test_env_overrides.py`

---

### Log Level (`--log-level`)

Sets the logging verbosity level.

| Property | Value |
|----------|-------|
| Long form | `--log-level` |
| Type | string (enum) |
| Default | `info` |
| Required | No |
| Valid values | `debug`, `info`, `warning` (or `warn`), `error` |
| Environment variable | `FLAPI_LOG_LEVEL` |

**Description:**

Controls the amount of log output. More verbose levels include all messages from less verbose levels.

**Precedence (highest wins):**
1. `--log-level` CLI flag
2. `FLAPI_LOG_LEVEL` environment variable
3. Built-in default (`info`)

`warn` is accepted as an alias of `warning` everywhere (flag,
`FLAPI_LOG_LEVEL`, and `log-level:` in `flapi.yaml`). Invalid values cause
flapi to exit with a single-line error -- typos like `FLAPI_LOG_LEVEL=DEBUG`
surface immediately rather than silently defaulting to `info`. An unknown
`log-level:` in the config file stops startup the same way.

| Level | Description |
|-------|-------------|
| `debug` | Detailed debugging information, SQL queries, request/response details |
| `info` | General operational information, startup messages, request summaries |
| `warning` | Warning conditions that may require attention |
| `error` | Error conditions that affect operation |

**Example:**

```bash
# Enable verbose debugging
./flapi --log-level debug

# Production mode with minimal logging
./flapi --log-level error

# Default info level
./flapi --log-level info

# Set verbosity via environment variable (12-factor style)
export FLAPI_LOG_LEVEL=debug
./flapi
```

> **Implementation:** `src/main.cpp` | **Tests:** `test/integration/test_mcp_methods.py` (logging/setLevel)

---

### Validate Configuration (`--validate-config`)

Validates the configuration file and exits without starting the server.

| Property | Value |
|----------|-------|
| Long form | `--validate-config` |
| Type | boolean flag |
| Default | `false` |
| Required | No |

**Description:**

Loads and validates the configuration file, checking for:
- YAML syntax errors
- Required field presence
- File references (template files exist)
- Mustache syntax of every serving template and cache `template-file`: unbalanced or crossed sections
  (`{{#x}}` / `{{^x}}` without `{{/x}}`, a stray `{{/x}}`), `{{{` not closed by `}}}`, an unterminated
  `{{`, empty tags, partials (`{{> x}}`) and set-delimiter tags (`{{= =}}`). The error names the file,
  line and column, e.g. `Invalid template sqls/orders.sql:12:3: '{{/region}}' closes a section that was
  never opened`. The same check runs at startup (exit 1) and on reload. The SQL itself is not run.
- Template variable names: every `{{ params.x }}` / `{{#params.x}}` in an endpoint's template must name a
  request field declared in its `request:` list (or a `:name` of its `url-path`; `limit` and `offset`
  are always set). It is a **warning**, not an error (templates are often shared by endpoints that declare different
  subsets of their parameters) - such a variable is either empty or an unvalidated raw query parameter. `conn.*`, `env.*`, `cache.*`, `context.*`, comments and names inside a section
  (`{{#params.items}}{{name}}{{/params.items}}`) are not checked.
- **Every** invalid endpoint or template file is reported in one run, not just the first: with
  `--validate-config` a file that fails to load (a Mustache syntax error, a malformed endpoint) is listed
  and the remaining files are still checked. A normal start still stops at the first.
- Connection configuration validity
- URL path format compliance

After validation, the server exits immediately with an appropriate exit code.

**Example:**

```bash
# Validate default configuration
./flapi --validate-config

# Validate specific configuration file
./flapi -c production.yaml --validate-config

# Use in CI/CD pipeline
./flapi --validate-config && echo "Config valid" || echo "Config invalid"
```

**Output:**

```
Loading configuration from: flapi.yaml
Configuration validated successfully.
Endpoints loaded: 12
Exit code: 0
```

> **Implementation:** `src/config_manager.cpp` | **Tests:** `test/cpp/config_manager_yaml_validation_test.cpp`

---

### Configuration Service (`--config-service`)

Enables the runtime configuration management API.

| Property | Value |
|----------|-------|
| Long form | `--config-service` |
| Type | boolean flag |
| Default | `false` |
| Required | No |

**Description:**

When enabled, exposes REST API endpoints and MCP tools for managing endpoints, templates, and cache at runtime without server restart:

**REST API Endpoints:**

| Endpoint | Description |
|----------|-------------|
| `GET /api/v1/_config/endpoints` | List all endpoints |
| `GET /api/v1/_config/endpoints/{path}` | Get endpoint configuration |
| `POST /api/v1/_config/endpoints` | Create new endpoint |
| `PUT /api/v1/_config/endpoints/{path}` | Update endpoint |
| `DELETE /api/v1/_config/endpoints/{path}` | Delete endpoint |
| `GET /api/v1/_config/endpoints/{path}/template` | Get SQL template |
| `PUT /api/v1/_config/endpoints/{path}/template` | Update SQL template |
| `POST /api/v1/_config/endpoints/{path}/cache/refresh` | Refresh a cache now (`?full=true`: drop and rebuild) |
| `GET /api/v1/_config/schema` | Database schema introspection |
| `GET /api/v1/_config/health` | Health and server version |
| `GET /api/v1/_config/metrics` | Runtime metrics |

The full list is in the [Config Service API Reference](./CONFIG_SERVICE_API_REFERENCE.md).

**MCP Tools (20 configuration management tools):**

When config-service is enabled, 20 MCP configuration tools become available:
- **Discovery Tools (5):** `flapi_get_project_config`, `flapi_get_environment`, `flapi_get_filesystem`, `flapi_get_schema`, `flapi_refresh_schema`
- **Template Tools (4):** `flapi_get_template`, `flapi_update_template`, `flapi_expand_template`, `flapi_test_template`
- **Endpoint Tools (6):** `flapi_list_endpoints`, `flapi_get_endpoint`, `flapi_create_endpoint`, `flapi_update_endpoint`, `flapi_delete_endpoint`, `flapi_reload_endpoint`
- **Cache Tools (4):** `flapi_get_cache_status`, `flapi_refresh_cache`, `flapi_get_cache_audit`, `flapi_run_cache_gc`
- **Metrics (1):** `flapi_get_metrics`

See [MCP Configuration Tools API Reference](./MCP_CONFIG_TOOLS_API.md) for complete tool documentation.

**Important:** Without `--config-service`, these MCP tools will NOT be available. They do not appear in `tools/list` responses.

**Security:** All configuration service endpoints and MCP tools require authentication via bearer token.

**Example:**

```bash
# Enable configuration service with auto-generated token
./flapi --config-service

# Enable with custom token
./flapi --config-service --config-service-token "my-secure-token-0123456789"
```

---

### Configuration Service Token (`--config-service-token`)

Sets the authentication token for the configuration service API.

| Property | Value |
|----------|-------|
| Long form | `--config-service-token` |
| Type | string |
| Default | Auto-generated (if `--config-service` enabled) |
| Required | No (auto-generated if omitted) |
| Environment variable | `FLAPI_CONFIG_SERVICE_TOKEN` |

**Description:**

The bearer token required for all configuration service API requests. Can be provided via:
1. Command-line option
2. Environment variable `FLAPI_CONFIG_SERVICE_TOKEN`
3. Auto-generated (printed to stdout on startup)

**Token Usage:**

Include in API requests as:
```
Authorization: Bearer <token>
```
or:
```
X-Config-Token: <token>
```

**Example:**

```bash
# Use custom token via CLI
./flapi --config-service --config-service-token "secret-token-12345"

# Use token from environment variable
export FLAPI_CONFIG_SERVICE_TOKEN="secret-token-12345"
./flapi --config-service

# Auto-generate token (printed to stdout)
./flapi --config-service
# Output includes: Token: <generated-token>
```

> **Implementation:** `src/config_service.cpp` | **Tests:** `test/cpp/config_service_test.cpp`, `test/integration/test_config_service_endpoints.tavern.yaml`

---

### Cache Warm-up (`--cache-warmup`, `--no-cache-warmup`)

Overrides `ducklake.warmup` and every endpoint's `cache.warmup` for this run.

| Property | Value |
|----------|-------|
| Long form | `--cache-warmup <always\|if-empty\|never>`, `--no-cache-warmup` (= `never`) |
| Default | the configured mode (`always` unless configured) |
| Environment variable | `FLAPI_CACHE_WARMUP` |

`never` starts the server without running a single populate query, which is what a CI smoke test
wants: `./flapi -c flapi.yaml --no-cache-warmup` and probe `/health` (it answers `200`, `degraded`, listing
the unbuilt caches). An invalid value exits with code 1. `ducklake.read-only: true` always means `never`.

### Disable Telemetry (`--no-telemetry`)

Disables all telemetry events.

| Property | Value |
|----------|-------|
| Long form | `--no-telemetry` |
| Type | flag (boolean) |
| Default | `false` (telemetry enabled) |
| Required | No |
| Environment variable | `FLAPI_NO_TELEMETRY` |

**Description:**

When set, flapi sends no telemetry at all (no `server_started`, no per-route `feature_used` counts; see [TELEMETRY.md](../TELEMETRY.md)). Telemetry can also be disabled via the `FLAPI_NO_TELEMETRY` environment variable or via `telemetry.enabled: false` in `flapi.yaml`.

**Precedence (highest wins):**
1. `--no-telemetry` CLI flag
2. `FLAPI_NO_TELEMETRY=1` (or `true` / `yes`)
3. `telemetry.enabled: false` in `flapi.yaml`

**Example:**

```bash
# One-off: disable telemetry for this run
./flapi --no-telemetry

# Disable via environment variable
export FLAPI_NO_TELEMETRY=1
./flapi

# Disable permanently via config file
# In flapi.yaml:
#   telemetry:
#     enabled: false
```

> **Implementation:** `src/main.cpp`, `src/flapi_telemetry.cpp` | **Tests:** `test/cpp/test_flapi_telemetry.cpp`

---

## 3. Self-Packaging Subcommands

flapi can fold its config tree (flapi.yaml + endpoint YAMLs + SQL
templates + small data files) into the binary itself, producing a
single self-contained artifact deployable via `scp`. The same binary
that serves the API also produces new bundled artifacts -- there is
no separate packager.

See [DESIGN_DECISIONS.md §9](./spec/DESIGN_DECISIONS.md#9-self-packaging-via-appended-zip)
for the architectural rationale.

### `flapi pack` -- create a self-contained binary

```
flapi pack --in <config-dir> --out <new-binary> [--with-extensions | --extensions-dir <dir>]
           [--exclude <glob>]... [--allow-secrets] [--macos-append] [--skip-validation]
```

| Option | Required | Description |
|--------|----------|-------------|
| `--in` | yes | Directory containing `flapi.yaml` and friends. Walked recursively. |
| `--out` | yes | Path for the bundled output binary. Overwritten if it exists. |
| `--allow-secrets` | no | Bypass the default secret deny list. Testing only -- production users must never set this. |
| `--exclude` | no | Do not bundle files matching this glob, relative to `--in`; repeatable. `*` stays within a path segment, `**` crosses segments, `?` matches one character. A pattern without `/` matches a file or directory name at any depth, and a matching directory excludes everything below it (`--exclude scratch --exclude 'data/raw/**'`). |
| `--skip-validation` | no | Skip the config validation `pack` otherwise runs first (the same checks as `--validate-config`). Without it, an invalid `<in>/flapi.yaml` is refused and no output file is written. |
| `--macos-append` | no | macOS only: append the archive after `__LINKEDIT` instead of overwriting the reserved `__FLAPI/__bundle` segment. **Not notarisable.** |
| `--with-extensions` | no | Install the DuckDB extensions the config needs (`duckdb.default-extensions`, every `INSTALL` in `connections.*.init`) **for the platform `pack` runs on**, and embed them. Fails without writing output if any cannot be installed. |
| `--extensions-dir` | no | Embed an already filled extension directory instead, e.g. one built by `flapi extensions install` for another platform. Exclusive with `--with-extensions`. |

**Embedded extensions.** They are stored under the reserved `.flapi/extensions/` prefix (an input tree
may not use it). At startup the binary extracts them once to
`$TMPDIR/flapi-extensions/ext-<content hash>/`, reusing that directory on the next start, and uses it as
`extension_directory` with **offline extensions on**. A data source works from a single `scp`-ed file
with no network access. Precedence: `duckdb.extension_directory` in the config >
`FLAPI_EXTENSION_DIRECTORY` > the bundle. Set `FLAPI_EXTENSIONS_OFFLINE=0` to allow downloads anyway.
Extensions are platform-specific, so pack on (or `--extensions-dir` for) the target platform. On macOS
they usually exceed the 16 MiB reserved segment; raise `FLAPI_RESERVED_BUNDLE_MIB` at build time or use
`--macos-append`.

**Never bundled: the DuckLake catalog.** The configured `ducklake.metadata-path` (and its `.wal`) and
`data-path`, when they lie inside `--in`, and any `*.ducklake`, `*.ducklake.wal` or `*.ducklake.files`
are left out. They are runtime state that a bundle never opens. `pack` prints
`Excluded N file(s) (DuckLake catalog / --exclude), e.g. <path>`.

**Default secret deny list** (refusal with non-zero exit, unless
`--allow-secrets`):

- `*.env` and `.env.*` (e.g. `.env.local`)  at any depth
- `secrets/` segment  at any depth
- `*.pem`             at any depth
- `*.key`             at any depth

Matching is **case-insensitive** (`.ENV`, `SECRETS/token`, `server.PEM` are refused too).
Symlinks are never followed: a symlink in the input tree is refused with an error.

**Bundle limits.** Reading a bundle (startup, `info`, `unpack`) refuses entries whose name would
leave the extraction directory (`../x`, absolute paths, backslashes), more than 100000 entries
(`FLAPI_BUNDLE_MAX_ENTRIES`) and more than 1 GiB decompressed (`FLAPI_BUNDLE_MAX_MIB`). A binary whose
bundle is present but damaged or unsafe **refuses to start** instead of falling back to a
`flapi.yaml` beside it.

**Reproducible builds.** Set `SOURCE_DATE_EPOCH` to stamp every
archive entry with a deterministic mtime; the produced binary is
then bit-identical across runs given the same input.

**Re-pack idempotence.** If the host binary already has a trailing
bundle, `pack` strips it from the _copy_ (not the running binary)
before appending the new one, so repeated invocations don't grow the
output.

**Example:**

```bash
SOURCE_DATE_EPOCH=1700000000 flapi pack --in ./examples --out flapi-prod
chmod +x flapi-prod                            # exec bits already preserved
scp flapi-prod user@host:/opt/flapi/           # one-file deploy
ssh user@host '/opt/flapi/flapi-prod'          # serves bundled config from any cwd
```

> **Implementation:** `src/pack.cpp`, `src/archive_io.cpp` | **Tests:** `test/cpp/pack_test.cpp`, `test/integration/test_self_packaging.py`

### `flapi info` -- inspect the running binary's bundle

```
flapi info
```

Prints the EOCD offset, bundle size, and entry list (with byte
counts). Exits non-zero with `"Bundle: none (filesystem mode)"` if
the binary has no appended (or in-section, on macOS) bundle.

**Example:**

```bash
$ ./flapi-prod info
Binary: /opt/flapi/flapi-prod
Bundle offset: 70123456
Bundle size:   12534 bytes
Entries (17):
  flapi.yaml (1024 bytes)
  sqls/customers.yaml (412 bytes)
  ...
```

### `flapi unpack --to <dir>` -- dump the bundle for debugging

```
flapi unpack --to <dir>
```

Writes every bundle entry to `<dir>` (creating intermediate
directories as needed), preserving paths. Useful for diffing a
deployed bundle against a development tree.

**Example:**

```bash
$ ./flapi-prod unpack --to /tmp/extracted
Unpacked 17 entries to /tmp/extracted

$ diff -ru ./examples /tmp/extracted
# (empty -- bundle matches source tree)
```

### macOS notarisation specifics

On Darwin, `flapi` is linked with a reserved `__FLAPI/__bundle`
Mach-O section (default 16 MiB, knob `FLAPI_RESERVED_BUNDLE_MIB` at
CMake configure time). `flapi pack` overwrites this section in
place and re-invokes `codesign --force --sign $CODESIGN_IDENTITY`
(defaulting to `-` for ad-hoc) so the freshly bundled binary has a
fresh valid signature. The output is suitable for `notarytool
submit`.

The `--macos-append` flag falls back to the Linux/Windows-style
trailing-bytes layout. Use it only for local debugging -- the
signature is intentionally invalid and the binary will fail
notarisation.

> **Implementation:** `src/macho_bundle.cpp` | **Tests:** `test/cpp/macho_bundle_test.cpp`, `test/integration/test_self_packaging_macos.py`

### `flapi cache refresh` -- refresh caches once and exit

```bash
flapi cache refresh --config flapi.yaml --endpoint /orders --endpoint /customers
flapi cache refresh --config flapi.yaml --all
flapi cache refresh --config flapi.yaml --endpoint /orders --full   # drop and rebuild
flapi cache refresh --config flapi.yaml --endpoint /orders --method GET
```

`--endpoint` picks the endpoint on that path **that has a cache**: a cached `GET /orders` beside an
uncached `POST /orders` refreshes the GET (it used to pick whichever loaded first and fail with
`has no cache`, exit `2`). Several cached endpoints on one path and table are one cache. `--method`
narrows the choice; it is needed only when cached endpoints on the path use different tables
(`ambiguous: ... pass --method GET|POST`, exit `2`).

Opens DuckDB and the DuckLake catalog read-write, runs each selected cache's populate template exactly
as the server would (same watermark and merge semantics, same retention), prints a JSON summary and
exits. It opens no port and runs no warm-up, so it fits a Cloud Run Job or an Airflow task right after
`dbt build`, replacing an always-on writer. Readers attach the same catalog with
`ducklake.read-only: true`.

```json
{"failed":0,"refreshed":[{"endpoint":"/orders","schema":"analytics","table":"orders_cache","status":"ok","rows_affected":3,"duration_ms":192}]}
```

Each item carries `duration_ms` and `rows_affected` (the sum over the template's
statements). `--full` erases the incremental watermark and drops the cache table
before the template runs, in one transaction, so an append cache is rebuilt
rather than re-appended; the item then also says `"full": true`. The run also
compacts the catalog when `ducklake.compaction` is due.

If a running server holds the DuckLake catalog (a DuckDB-file catalog is locked
by the process that has it open), the command prints
`{"error":"a running flapi holds the DuckLake catalog lock; refresh through the server: flapii cache refresh <path> (if the holders are read-only replicas, stop the replicas, or use a Postgres/SQLite/MySQL DuckLake catalog, which a writer and replicas can share)"}`
and exits `1`.

In a packed binary the command reads the **bundled** config: `--config` defaults to the bundled
`flapi.yaml`, and `embed://` paths in the populate SQL read the bundle. Outside a packed binary,
`embed://<path>` resolves to `<config directory>/<path>`, so a config written for `flapi pack` also
refreshes from its source tree.

| Exit code | Meaning |
|-----------|---------|
| `0` | every selected cache refreshed |
| `1` | at least one refresh failed (the others still ran; see `"status": "failed"`), or the catalog is locked by a running server |
| `2` | usage: an argument the command does not know, nothing selected, an unknown endpoint, a non-cached endpoint, or a `read-only` catalog |

`--config` falls back to `FLAPI_CONFIG`, then `flapi.yaml`. `--log-level` (default `warning`) and
`--no-telemetry` are accepted after the subcommand too (`flapi cache refresh --all --no-telemetry`); the
one-shot sends no telemetry either way.

A refresh whose cursor watermark does not look like `cursor.type` (e.g. `type: integer` over a VARCHAR
column) drops the watermark and loads the full source; the audit row's `message` says so
(`Cache refreshed successfully; warning: ...`), as does the server's refresh answer (`warning`).

### `flapi extensions install` -- pre-install DuckDB extensions

```bash
# Everything a config needs (core_functions, parquet, json and icu are built in): duckdb.default-extensions,
# and every INSTALL in connections.*.init
flapi extensions install --config flapi.yaml --dir /opt/flapi/extensions

# Or name them; @repo for non-core repositories
flapi extensions install --dir /opt/flapi/extensions httpfs parquet ducklake bigquery@community

# flAPI's default set (httpfs ducklake fts postgres sqlite): what a release ships in extensions/
flapi extensions install --defaults --dir ./extensions
```

Installs into `--dir` (default: `FLAPI_EXTENSION_DIRECTORY`, then DuckDB's own default) and exits `1` if
any extension failed, after reporting every failure. Run it at image build time; at runtime set
`FLAPI_EXTENSION_DIRECTORY` to the same directory and, to make a missing extension a startup error
rather than a download, `FLAPI_EXTENSIONS_OFFLINE=1`. The extension directory is keyed by DuckDB version and
platform, so install with the same `flapi` binary you ship. In a packed binary, `flapi extensions install` with
no names and no `--config` installs what the bundled `flapi.yaml` needs.

A release binary finds the `extensions/` directory shipped next to it (see
[CONFIG_REFERENCE: where DuckDB extensions come from](CONFIG_REFERENCE.md)). An extension it carries is
**copied** from there into `--dir` (no download, so `flapi pack --with-extensions` works offline too); with no
`--dir` and no `FLAPI_EXTENSION_DIRECTORY` it is reported as shipped and not installed at all.

Release binaries (archives, wheels, Docker) are linked with `-DFLAPI_STRIP_BINARY=ON`: no static symbol table
(Linux 122 -> 102 MB; macOS keeps global symbols), the dynamic symbols loadable extensions need are kept. Local
builds keep their symbols unless you pass the option.

---

## 4. Environment Variables

| Variable | Description | Used By |
|----------|-------------|---------|
| `FLAPI_CONFIG` | Path to `flapi.yaml` (fallback for `-c`) | `--config` fallback |
| `FLAPI_PORT` | HTTP server port (fallback for `-p` / `--port`); invalid values exit 1 | `--port` fallback |
| `FLAPI_HOST` | Bind address (fallback for `--host`) | `--host` fallback |
| `FLAPI_LOG_LEVEL` | Log verbosity (fallback for `--log-level`); invalid values exit 1 | `--log-level` fallback |
| `FLAPI_CONFIG_SERVICE_TOKEN` | Authentication token for configuration service API | `--config-service-token` fallback |
| `FLAPI_NO_TELEMETRY` | Disable telemetry when set to `1`, `true`, or `yes` | `--no-telemetry` fallback |
| `FLAPI_CACHE_WARMUP` | `always`, `if-empty` or `never`; invalid values exit 1 | `--cache-warmup` fallback |
| `FLAPI_EXTENSION_DIRECTORY` | Default `extension_directory` when the config sets none; replaces the bundled and shipped directories | DuckDB startup, `flapi extensions install` |
| `FLAPI_EXTENSIONS_OFFLINE` | `1`/`true`/`yes`: never download extensions; a missing one stops startup | DuckDB startup |
| `AWS_ENDPOINT_URL_SECRETS_MANAGER` | Secrets Manager endpoint for `auth.from-aws-secretmanager` (startup warns; `AWS_ENDPOINT_URL` is not used for it) | startup |
| `FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT` | `1`/`true`/`yes`: accept a plain-`http://`, non-loopback Secrets Manager endpoint | startup |
| `SOURCE_DATE_EPOCH` | Mtime stamped on every entry by `flapi pack` (reproducible builds) | `flapi pack` |
| `CODESIGN_IDENTITY` | macOS only: identity passed to `codesign --sign` after `flapi pack`. Defaults to `-` (ad-hoc). | `flapi pack` |

**Configuration File Variables:**

Environment variables can be used in configuration files via `{{env.VAR_NAME}}` syntax. Variables must be whitelisted under `template:` in the main configuration; an unlisted variable stops startup, and an empty whitelist allows none:

```yaml
template:
  environment-whitelist:
    - '^DB_.*'
    - '^API_KEY$'
```

See [Configuration Reference - Environment Variables](./CONFIG_REFERENCE.md#10-environment-variables) for details.

---

## 5. Usage Examples

### Basic Startup

```bash
# Default startup
./flapi

# With explicit config file
./flapi -c flapi.yaml
```

### Development Mode

```bash
# Verbose logging for debugging
./flapi --log-level debug

# Enable config service for runtime changes
./flapi --config-service --log-level debug

# Custom port for development
./flapi -p 3000 --log-level debug --config-service
```

### Production Mode

```bash
# Minimal logging, specific config
./flapi -c /etc/flapi/production.yaml --log-level error

# Production with config service (secure token)
./flapi -c /etc/flapi/production.yaml \
  --config-service \
  --config-service-token "${CONFIG_SERVICE_SECRET}" \
  --log-level warn
```

For platforms with short startup health-check windows, point the platform liveness check at:

```text
/health/live
```

Use `/health` when the platform should wait until cache warmup is complete before marking the
deployment ready for traffic.

### CI/CD Validation

```bash
# Validate configuration in CI pipeline (does not open the database)
./flapi -c flapi.yaml --validate-config

# Start the real server without touching any data source
./flapi -c flapi.yaml --no-cache-warmup &
curl -fsS http://localhost:8080/health   # 200, "degraded" until caches are built

# Shell script example
if ./flapi --validate-config; then
  echo "Configuration is valid"
  exit 0
else
  echo "Configuration validation failed"
  exit 1
fi
```

---

## 6. Runtime Health Endpoints

flAPI always registers two unauthenticated health endpoints. They do not require
`--config-service`.

| Endpoint | Purpose | Response |
|----------|---------|----------|
| `GET /health/live` | Liveness: process is up and the listener is accepting connections | `200 {"status":"live"}`; `503 {"status":"fatal"}` when the database was invalidated |
| `GET /health` | Readiness: cache-enabled endpoints are ready to serve | `200` with `status` `ready`, or `degraded` (a `stale` or `unbuilt` cache); `503` with `status` `starting`, `degraded` (a `failed` first build) or `stalled` |

**An invalidated database is fatal.** An internal DuckDB error can invalidate the whole database; every
later query then fails with `database has been invalidated` until the process restarts. flAPI detects it
on the first such error, logs it once at `critical` level with the cause, and from then on BOTH
`/health` and `/health/live` answer

```json
503 {"status": "fatal", "reason": "<the DuckDB error, at most 300 characters>", "message": "The database has been invalidated ... flapi must be restarted; GET /health shows the cause."}
```

so a supervisor or liveness probe restarts it. Data and config-service requests answer `503`
`{"error":"database_invalidated","message":...}` (`Retry-After: 30`) instead of a `500` each. The one
known trigger, two refreshes attaching the same alias in parallel, cannot happen any more: refreshes
are serialised per catalog.

During cache warmup, `/health/live` returns `200` as soon as the server is listening, while
`/health` returns:

```json
{
  "status": "starting",
  "caches": {"total": 4, "ready": 1, "stale": 0, "failed": 0, "unbuilt": 0},
  "pending": [{"schema": "cache", "table": "customers_cache"}],
  "uptime_s": 7
}
```

If a cache has **never built** (the warm-up failed), `/health` returns `503` with `"status":
"degraded"` and a `failed` list naming the table and schema. If a later **refresh** fails over a cache
that was already built, the endpoint keeps serving the last snapshot and `/health` returns `200` with
`"status": "degraded"` and a `stale` list. This includes a failed warm-up over a table that an earlier
run built: with `warmup: always`, restarting while the source is unreachable keeps serving the existing
table and reports it `stale`. If warm-up was skipped (`--no-cache-warmup`, `warmup: never`,
`read-only`) and a cache table does not exist, `/health` returns `200` with `"status": "degraded"` and an
`unbuilt` list, and that endpoint answers `503` with `"error": "cache_not_built"` (on a read-only
replica the message says the writer has not built it yet). The readiness list follows endpoint
changes: a reload, create, update or delete that names a new cache table adds it as `unbuilt` (or
`ready` when the table exists), and a table no endpoint names any more is dropped from `/health` and
`caches.total` (a stale `failed` entry used to stay until restart). The error text (SQL, file paths) is written to the server log
only: `/health` is unauthenticated and never returns it.

A cache counts as **failed** only when no table exists to serve: its first build failed. The failure is
logged once, with the reason.

`caches.ready` counts caches that are ready and current; a `stale` cache (serving its last snapshot
while refreshes fail) is counted in `caches.stale` only, no longer in `ready`.

If a data request has been in flight for `stall-timeout-s` seconds or longer (top-level key, default
`60`, `0` disables), `/health` returns `503` with `"status": "stalled"`, `"stalled_after_s"` and
`requests.oldest_ms`. This check runs before the cache checks.

Requests to a cached data endpoint while its cache is starting return `503 Service
Unavailable` with `Retry-After: 5` and a JSON body containing `"error": "cache_warming"`; after a failed
first build the body says `"error": "cache_build_failed"`. flAPI does not serve partial or empty results
from a half-built cache.

**AWS App Runner example:**

```yaml
HealthCheckConfiguration:
  Protocol: HTTP
  Path: /health/live
  Interval: 5
  Timeout: 2
  HealthyThreshold: 1
  UnhealthyThreshold: 5
```

---

## 7. Signal Handling

| Signal | Behavior |
|--------|----------|
| `SIGINT` (Ctrl+C) | Graceful shutdown - closes connections, stops server |
| `SIGTERM` | Graceful shutdown - same as SIGINT |
| Unhandled exception | Logs error and exits with code 1 |

**Graceful Shutdown:**

On receiving a shutdown signal, the server:
1. Stops accepting new connections
2. Completes in-flight requests (with timeout); running cache populate scripts are interrupted
   (within about 10 s) and no new refresh starts
3. Closes database connections
4. Exits cleanly

**Crash Handling:**

- **Windows:** Creates minidump file (`crash_dump_<PID>.dmp`) for debugging
- **Unix/Linux:** Logs error details before exit

> **Implementation:** `src/main.cpp` | **Tests:** *None*

---

## 8. Exit Codes

| Code | Description |
|------|-------------|
| `0` | Success (normal exit or validation passed) |
| `1` | General error (configuration error, startup failure, validation failed) |

**Validation Exit Codes:**

```bash
# Check exit code after validation
./flapi --validate-config
echo $?  # 0 = valid, 1 = invalid
```

> **Implementation:** `src/main.cpp` | **Tests:** *None*

---

## Related Documentation

- **[Documentation index](./README.md)** - Start here; organised by what you are trying to do
- **[Configuration Reference](./CONFIG_REFERENCE.md)** - Configuration file options and format
- **[Config Service API Reference](./CONFIG_SERVICE_API_REFERENCE.md)** - Runtime configuration REST API and CLI client
- **[MCP Reference](./MCP_REFERENCE.md)** - Model Context Protocol specification
- **[Cloud Storage Guide](./CLOUD_STORAGE_GUIDE.md)** - Using cloud storage backends with configuration files
