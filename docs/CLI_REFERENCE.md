# flAPI Server - CLI Reference

**Version:** 1.0.0
**flAPI Version:** >= 1.0.0

This document provides a complete reference for the `flapi` server executable's command-line options and runtime behavior.

---

## Table of Contents

1. [Overview](#1-overview)
   - [Server Architecture](#server-architecture)
   - [Quick Start](#quick-start)
2. [Command-Line Options](#2-command-line-options)
   - [Configuration File](#configuration-file---c---config)
   - [Server Port](#server-port---p---port)
   - [Log Level](#log-level---log-level)
   - [Validate Configuration](#validate-configuration---validate-config)
   - [Configuration Service](#configuration-service---config-service)
   - [Configuration Service Token](#configuration-service-token---config-service-token)
3. [Environment Variables](#3-environment-variables)
4. [Usage Examples](#4-usage-examples)
   - [Basic Startup](#basic-startup)
   - [Development Mode](#development-mode)
   - [Production Mode](#production-mode)
   - [CI/CD Validation](#cicd-validation)
5. [Signal Handling](#5-signal-handling)
6. [Exit Codes](#6-exit-codes)
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
```

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

**Description:**

The configuration file defines connections, endpoint directories, DuckDB settings, authentication, caching, and other server options. The path can be absolute or relative to the current working directory.

**Example:**

```bash
# Use default flapi.yaml in current directory
./flapi

# Specify a custom configuration file
./flapi -c production.yaml
./flapi --config /etc/flapi/config.yaml
```

**See also:** [Configuration Reference](./CONFIG_REFERENCE.md) for configuration file options.

> **Implementation:** `src/main.cpp`, `src/config_manager.cpp` | **Tests:** `test/cpp/config_manager_test.cpp`

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

**Description:**

When specified, this option overrides the `http-port` value in the configuration file. Useful for running multiple instances or when port configuration needs to be dynamic.

**Example:**

```bash
# Start on port 9000
./flapi -p 9000

# Override config file port
./flapi -c production.yaml --port 80
```

> **Implementation:** `src/main.cpp`, `src/api_server.cpp` | **Tests:** `test/integration/conftest.py`

---

### Log Level (`--log-level`)

Sets the logging verbosity level.

| Property | Value |
|----------|-------|
| Long form | `--log-level` |
| Type | string (enum) |
| Default | `info` |
| Required | No |
| Valid values | `debug`, `info`, `warning`, `error` |

**Description:**

Controls the amount of log output. More verbose levels include all messages from less verbose levels.

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
| `POST /api/v1/_config/endpoints/{path}/cache/refresh` | Force cache refresh |
| `GET /api/v1/_schema` | Database schema introspection |
| `POST /api/v1/_ping` | Health check |

**MCP Tools (18 configuration management tools):**

When config-service is enabled, 18 MCP configuration tools become available:
- **Discovery Tools (5):** `flapi_get_project_config`, `flapi_get_environment`, `flapi_get_filesystem`, `flapi_get_schema`, `flapi_refresh_schema`
- **Template Tools (4):** `flapi_get_template`, `flapi_update_template`, `flapi_expand_template`, `flapi_test_template`
- **Endpoint Tools (6):** `flapi_list_endpoints`, `flapi_get_endpoint`, `flapi_create_endpoint`, `flapi_update_endpoint`, `flapi_delete_endpoint`, `flapi_reload_endpoint`
- **Cache Tools (4):** `flapi_get_cache_status`, `flapi_refresh_cache`, `flapi_get_cache_audit`, `flapi_run_cache_gc`

See [MCP Configuration Tools API Reference](./MCP_CONFIG_TOOLS_API.md) for complete tool documentation.

**Important:** Without `--config-service`, these MCP tools will NOT be available. They do not appear in `tools/list` responses.

**Security:** All configuration service endpoints and MCP tools require authentication via bearer token.

**Example:**

```bash
# Enable configuration service with auto-generated token
./flapi --config-service

# Enable with custom token
./flapi --config-service --config-service-token "my-secure-token"
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

## 3. Environment Variables

| Variable | Description | Used By |
|----------|-------------|---------|
| `FLAPI_CONFIG_SERVICE_TOKEN` | Authentication token for configuration service API | `--config-service-token` fallback |

**Configuration File Variables:**

Environment variables can be used in configuration files via `${VAR_NAME}` syntax. Variables must be whitelisted in the configuration:

```yaml
template:
  environment-whitelist:
    - '^DB_.*'
    - '^API_KEY$'
```

See [Configuration Reference - Environment Variables](./CONFIG_REFERENCE.md#10-environment-variables) for details.

---

## 4. Usage Examples

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
  --log-level warning
```

### CI/CD Validation

```bash
# Validate configuration in CI pipeline
./flapi -c flapi.yaml --validate-config

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

## 5. Signal Handling

| Signal | Behavior |
|--------|----------|
| `SIGINT` (Ctrl+C) | Graceful shutdown - closes connections, stops server |
| `SIGTERM` | Graceful shutdown - same as SIGINT |
| Unhandled exception | Logs error and exits with code 1 |

**Graceful Shutdown:**

On receiving a shutdown signal, the server:
1. Stops accepting new connections
2. Completes in-flight requests (with timeout)
3. Closes database connections
4. Exits cleanly

**Crash Handling:**

- **Windows:** Creates minidump file (`crash_dump_<PID>.dmp`) for debugging
- **Unix/Linux:** Logs error details before exit

> **Implementation:** `src/main.cpp` | **Tests:** *None - see [TEST_TODO.md](./TEST_TODO.md)*

---

## 6. Exit Codes

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

> **Implementation:** `src/main.cpp` | **Tests:** *None - see [TEST_TODO.md](./TEST_TODO.md)*

---

## Related Documentation

- **[Reference Documentation Map](./REFERENCE_MAP.md)** - Navigation guide for all reference docs
- **[Configuration Reference](./CONFIG_REFERENCE.md)** - Configuration file options and format
- **[Config Service API Reference](./CONFIG_SERVICE_API_REFERENCE.md)** - Runtime configuration REST API and CLI client
- **[MCP Reference](./MCP_REFERENCE.md)** - Model Context Protocol specification
- **[Cloud Storage Guide](./CLOUD_STORAGE_GUIDE.md)** - Using cloud storage backends with configuration files
