# Configuration System

This document describes the configuration management architecture in flAPI.

## Overview

The configuration system is responsible for:
- Loading and parsing YAML configuration files
- Discovering endpoints in the `sqls/` directory
- Validating configuration against schema rules
- Providing runtime access to configuration values
- Serializing configurations for API responses

## Architecture

```mermaid
graph TB
    subgraph "ConfigManager Facade"
        CM[ConfigManager]
    end

    subgraph "Delegated Classes"
        CL[ConfigLoader]
        ER[EndpointRepository]
        CV[ConfigValidator]
        CS[ConfigSerializer]
        ECP[EndpointConfigParser]
    end

    subgraph "External"
        YAML[(YAML Files)]
        API[Config Service API]
    end

    YAML --> CL
    CL --> CM
    CM --> ER
    CM --> CV
    CM --> CS
    CM --> ECP
    CM --> API
```

## ConfigManager Facade

`ConfigManager` (src/config_manager.cpp) acts as the primary interface for all configuration operations. It delegates to specialized classes:

```cpp
class ConfigManager {
    // Delegated classes (Facade pattern)
    std::unique_ptr<ConfigLoader> config_loader;
    std::unique_ptr<EndpointRepository> endpoint_repository;
    std::unique_ptr<ConfigValidator> config_validator;
    std::unique_ptr<ConfigSerializer> config_serializer;

public:
    // Unified API
    void loadConfig();
    EndpointRef getEndpointForPath(const std::string& path) const;
    ValidationResult validateEndpointConfig(const EndpointConfig& config) const;
    crow::json::wvalue serializeEndpointConfig(const EndpointConfig& config) const;
};
```

### Copy-on-write endpoint table

Endpoint lookups do **not** return a raw pointer into a live `std::vector`. They
return an `EndpointRef`: a pinned `shared_ptr` to an immutable snapshot, plus a
pointer into it.

```cpp
// A reload clones, mutates and publishes a new snapshot. Readers holding the
// previous one keep seeing a consistent table until they release it.
std::shared_ptr<const std::vector<EndpointConfig>> endpoints_snapshot;
```

This fixed a real use-after-free: the vector was cleared and re-`push_back`-ed at
runtime by `refreshConfig` and the config-service routes, with no lock against
in-flight requests, while handlers held raw pointers into it.

> **`EndpointRef` is `[[nodiscard]]` and must be bound to a named variable.**
> Used as a temporary, the snapshot releases at the end of the full expression
> and the pointer dangles — reintroducing exactly the bug it exists to prevent.
>
> ```cpp
> const auto endpoint = cfg->getEndpointForPathAndMethod(path, method);  // good
> auto* p = cfg->getEndpointForPathAndMethod(path, method).get();        // dangles
> ```

It is also the precondition that makes `RequestContext`'s `string_view` fields
safe to hold for the life of a request. See
[observability.md](./observability.md).


## Delegated Classes

### ConfigLoader (src/config_loader.cpp)

Responsible for loading YAML files from disk:

- Loads main `flapi.yaml` configuration
- Recursively scans `sqls/` directory for endpoint configs
- Resolves relative paths to absolute paths
- Handles environment variable substitution (`{{env.VAR_NAME}}`, checked against `template.environment-whitelist`)

**Key methods:**
```cpp
void loadConfig();
void loadEndpointConfigsRecursively(const std::filesystem::path& dir);
void loadEndpointConfig(const std::string& config_file);
```

### EndpointRepository (src/endpoint_repository.cpp)

Manages the collection of loaded endpoints:

- Stores endpoints in a vector
- Provides lookup by URL path, slug, or MCP name
- Handles endpoint add/remove/replace operations

**Key methods:**
```cpp
const EndpointConfig* getEndpointForPath(const std::string& path) const;
const EndpointConfig* getEndpointForPathAndMethod(const std::string& path, const std::string& method) const;
void addEndpoint(const EndpointConfig& endpoint);
bool removeEndpointByPath(const std::string& path);
```

**Request resolution (`ConfigManager::findEndpoint`).** Of the endpoints whose route
and method (canonical, upper-case) match the request, the most specific answers:
segment by segment from the left, a literal segment beats a `:param`
(`RouteTranslator::compareSpecificity`). It never depends on table order. Rate
limiting, auth and the request handler all call `getEndpointForPathAndMethod`, so they
always agree on the endpoint. At admission (load, reload-file, create, update)
`DuplicateEndpointProblem` refuses same-shape routes and overlapping routes
(`RouteTranslator::routesOverlap`) whose effective auth differs in any way (scheme,
roles or identity source; either direction) unless the more specific one sets
`allow-shadowing`; `LogRouteOverlaps` logs the allowed overlaps. `url-path` and `method`
are validated and canonicalised by every parser (`RouteTranslator::urlPathProblem`,
`ConfigManager::HttpMethodProblem` / `CanonicalHttpMethod`). Endpoint files load in
sorted path order; a reload keeps the endpoint's position.

### ConfigValidator (src/config_validator.cpp)

Validates configuration against rules:

- Required field validation
- Type validation
- Reference validation (connections, templates)
- Custom validator rules

**Key methods:**
```cpp
ValidationResult validateEndpointConfig(const EndpointConfig& config) const;
ValidationResult validateEndpointConfigFromYaml(const std::string& yaml_content) const;
```

**Validation Result:**
```cpp
struct ValidationResult {
    bool valid;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};
```

### ConfigSerializer (src/config_serializer.cpp)

Handles serialization to/from different formats:

- JSON serialization for API responses
- YAML serialization for file export
- Supports both hyphen-case and camelCase styles

**Key methods:**
```cpp
crow::json::wvalue serializeEndpointConfig(const EndpointConfig& config, EndpointJsonStyle style) const;
EndpointConfig deserializeEndpointConfig(const crow::json::rvalue& json) const;
std::string serializeEndpointConfigToYaml(const EndpointConfig& config) const;
```

### EndpointConfigParser (src/endpoint_config_parser.cpp)

Parses endpoint YAML into `EndpointConfig` structs:

- Parses request field definitions
- Parses validator configurations
- Parses cache, auth, and rate limit settings
- Handles MCP-specific fields

**Key methods:**
```cpp
void parseEndpointConfig(const std::filesystem::path& config_file);
void parseEndpointRequestFields(const YAML::Node& endpoint_config, EndpointConfig& endpoint);
void parseEndpointValidators(const YAML::Node& req, RequestFieldConfig& field);
```

## Configuration Loading Flow

```mermaid
sequenceDiagram
    participant Main
    participant CM as ConfigManager
    participant CL as ConfigLoader
    participant ECP as EndpointConfigParser
    participant ER as EndpointRepository

    Main->>CM: loadConfig()
    CM->>CL: loadConfig()
    CL->>CL: parseMainConfig()
    CL->>CL: parseConnections()
    CL->>CL: parseDuckDBConfig()
    CL->>CL: loadEndpointConfigsRecursively()

    loop For each YAML in sqls/
        CL->>ECP: parseEndpointConfig(file)
        ECP->>ECP: Parse YAML fields
        ECP->>ECP: parseEndpointRequestFields()
        ECP->>ECP: parseEndpointValidators()
        ECP->>ER: addEndpoint(config)
    end
```

## Key Configuration Structures

### Main Configuration (flapi.yaml)

```yaml
project-name: my-project
http-port: 8080                 # top-level; there is no `server:` block
connections:
  my-data:
    properties:
      path: ./data/file.parquet
duckdb:
  db_path: ./cache.db
  threads: 4
template:
  path: ./sqls
  environment-whitelist:
    - DB_.*
    - API_KEY
log-level: info          # read from YAML; CLI > env > YAML > default
log-format: json
audit:
  enabled: true
  sink: file
  path: /var/log/flapi/audit.jsonl
  redact: [password, tax_id]
tracing:                 # see below - parsed once, at startup
  enabled: false
```

#### The `tracing:` block

Parsed by `ConfigManager::parseTracingConfig()` into `TracingConfig`
(`src/include/tracing_config.hpp`). Every key is **boot-only**: the provider,
capture policy and middleware snapshots are all taken once at startup, so a
reload does not affect them.

Optional fields are held as `std::optional` on purpose. A YAML *default* must not
be indistinguishable from an operator's explicit setting, or the exporter would
silently overwrite an injected `OTEL_EXPORTER_OTLP_ENDPOINT` and Kubernetes
auto-instrumentation would break.

The full key list, defaults and semantics live in
[../../OBSERVABILITY.md § 2](../../OBSERVABILITY.md#2-turning-tracing-on).

Per-endpoint, `mcp-tool.tracing.capture` overrides the global tier in either
direction — except that a global `off` always wins.

### Endpoint Configuration (sqls/*.yaml)

```yaml
url-path: /customers
method: GET
request:
  - field-name: id
    field-in: query
    required: false
    validators:
      - type: int
        min: 1
template-source: customers.sql
connection: [my-data]
cache:
  enabled: true
  table: customers_cache
  schedule: "1h"
auth:
  enabled: true
  type: basic
  roles: [admin, user]     # caller needs one of them, else 403
  users: [...]
```

**Global auth inheritance.** `ConfigManager::applyGlobalAuthDefault` copies the top-level `auth:` block
(when `enabled: true`) into every REST endpoint that has no `auth:` block of its own and marks it
`auth.inherited`. It runs wherever an endpoint is parsed: initial load, reload, and config-service
creates. An endpoint's own block replaces the global one; `auth: {enabled: false}` opts out.

**Load-time warnings for inert keys.** Keys that are parsed but read by nothing warn instead of being
silently accepted: `field-type` on a request field, `server-name`, `connections.<n>.log-queries`,
`log-parameters` and `allow`, unknown top-level keys, and a `storage:` block when `template.path` is
local (it only configures caching of remote templates). Validators accept `pattern` for `regex` and
`values` for `allowedValues`; the canonical key wins when both are set. `auth.type: jwt` is normalised
to `bearer`, `log-level: warn` to `warning`; an unknown `log-level` stops startup.

### EndpointConfig Struct

```cpp
struct EndpointConfig {
    std::string urlPath;
    std::string method;
    std::vector<RequestFieldConfig> request_fields;
    std::string templateSource;
    std::vector<std::string> connection;
    CacheConfig cache;
    AuthConfig auth;
    RateLimitConfig rate_limit;
    OperationConfig operation;

    // MCP-specific (optional)
    std::optional<MCPToolInfo> mcp_tool;
    std::optional<MCPResourceInfo> mcp_resource;
    std::optional<MCPPromptInfo> mcp_prompt;

    // Helper methods
    Type getType() const;
    std::string getName() const;
    std::string getSlug() const;
};
```

## Runtime Configuration Updates

The ConfigService API allows runtime configuration changes:

```
POST   /api/v1/_config/endpoints        - Create endpoint
PUT    /api/v1/_config/endpoints/:slug  - Update endpoint
DELETE /api/v1/_config/endpoints/:slug  - Delete endpoint
```

These operations go through the same loader as a YAML file on disk, so `mcp-tool`, validators,
`rate-limit` and the global-auth default all apply, and they **persist**:

1. **Create** parses the body with the real endpoint loader, writes `<template dir>/<name>.yaml` (the
   url-path segments joined with `_`, `endpoint_files::FileStemFor`), and
   answers `201 {"status":"created","endpoint","slug","url-path","file"}`. An existing endpoint is
   `409` (also when any YAML in the templates directory declares the same url-path and method), as is
   any write while serving a packed bundle; a body that does not parse is `400`. Loading two files that
   serve the same url-path and method is a startup error naming both (`DuplicateEndpointProblem`); a
   reload that would create such a pair fails with that reason.
2. **Update** deep-merges the body onto the endpoint's YAML file and re-parses the result. Changing
   `url-path`, `method` or the name is `400` (delete and re-create instead); an endpoint built from
   `{{include}}` files is `409`. A partial body (`{"template-source": "x.sql"}`) is a patch like any
   other: url-path comes from the target. A camelCase body is applied in memory only and the answer says
   `"persisted": false`; what it leaves out (url-path, template-source, method) is taken from the target. The rewritten YAML loses its comments.
3. **Delete** removes the endpoint and its YAML file.

Errors are JSON `{"error": "..."}`. A reload that fails to parse is `400` with a `reason`; `404` means
only that the endpoint is unknown. Every change rebuilds the MCP `tools/list`, `resources/list` and
`prompts/list` answers.

## Path Resolution

Paths are resolved relative to the base configuration directory:

```cpp
std::string makePathRelativeToBasePathIfNecessary(const std::string& value) const {
    std::filesystem::path path(value);
    if (path.is_relative()) {
        return (base_path / path).string();
    }
    return value;
}
```

## Environment Variable Substitution

Environment variables in configuration are substituted at load time:

```yaml
connections:
  db:
    properties:
      password: '{{env.DB_PASSWORD}}'  # Replaced with env value
```

Variables must be whitelisted, under `template:`. A variable that matches no pattern stops
startup with an error naming it (never a silent literal), and an empty or missing whitelist
allows none:
```yaml
template:
  environment-whitelist:
    - DB_.*
    - API_.*
```

## Source Files

| File | Purpose |
|------|---------|
| `src/config_manager.cpp` | Facade implementation |
| `src/config_loader.cpp` | YAML loading |
| `src/endpoint_config_parser.cpp` | Endpoint parsing |
| `src/endpoint_repository.cpp` | Endpoint storage |
| `src/config_validator.cpp` | Validation rules |
| `src/config_serializer.cpp` | Serialization |
| `src/include/config_manager.hpp` | Struct definitions, `EndpointRef`, snapshot |
| `src/tracing_config.cpp` | The boot-only `tracing:` block |

## Related Documentation

- [DESIGN_DECISIONS.md](../DESIGN_DECISIONS.md#2-facade-pattern-for-configmanager) - Why facade pattern
- [../../CONFIG_REFERENCE.md](../../CONFIG_REFERENCE.md) - Configuration options
- [DESIGN_DECISIONS.md](../DESIGN_DECISIONS.md#10e-copy-on-write-endpoint-table) - Why copy-on-write
- [observability.md](./observability.md) - What depends on the pinned snapshot
