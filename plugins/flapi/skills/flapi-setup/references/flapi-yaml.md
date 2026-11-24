# flapi.yaml: every top-level key

The source of truth is `ConfigManager::KnownTopLevelKeys()` and the `parse*Config()`
functions in `src/config_manager.cpp`. Any other top-level key is **ignored with a startup
warning** (`Unknown top-level configuration key ... is ignored`). Two keys are **errors**:
`server:` (use `http-port` / `http-host`) and a top-level `environment-whitelist:` (move it
under `template:`).

## Project and server

| Key | Default | Notes |
|---|---|---|
| `project-name` | **required** | OpenAPI title (`/doc`) |
| `project-description` | `""` | OpenAPI description (optional) |
| `http-port` | `8080` | overridden by `-p/--port`, then `FLAPI_PORT` |
| `http-host` | `0.0.0.0` | overridden by `--host`, then `FLAPI_HOST`; `127.0.0.1` = loopback only |
| `log-level` | `info` | `debug`, `info`, `warning` (alias `warn`), `error`. `--log-level` / `FLAPI_LOG_LEVEL` win. An unknown value is a startup error. |
| `log-format` | `text` | `json` = one JSON object per line, carrying `request_id` (and `trace_id` with tracing) |
| `log-http-requests` | `false` | `true`: one `info` line per HTTP request DuckDB makes (S3/GCS/Azure/HTTP), with the causing `request_id` or `-`. `FLAPI_LOG_HTTP_REQUESTS` wins |
| `stall-timeout-s` | `60` | `GET /health` answers 503 `stalled` when a request is older than this; `0` disables |
| `server-name` | `localhost` | has **no effect**; a load warning says so |
| `version` | – | accepted as metadata, not read |

## template (required)

| Key | Default | Notes |
|---|---|---|
| `template.path` | **required** | directory with endpoint YAML + SQL, relative to `flapi.yaml`; scanned recursively |
| `template.environment-whitelist` | `[]` (= no variables) | list of regexes for `{{env.NAME}}` in YAML **and** SQL templates. In YAML, an unlisted name stops startup. It must be in the main file, not in an include. |

```yaml
template:
  path: ./sqls
  environment-whitelist:
    - '^FLAPI_.*'
    - '^(PG_PASSWORD|API_KEY)$'
```

## connections

A map of `name -> {init, properties, serialize-access}`. Recipes are in
[connections.md](connections.md).

## duckdb

| Key | Default | Notes |
|---|---|---|
| `duckdb.db_path` | in-memory | a file makes the DuckDB database (and a DuckLake cache inside it, if any) persistent |
| `duckdb.default-extensions` | `[httpfs, ducklake, fts, json, postgres, sqlite, parquet]` | installed and loaded at startup; built-in `json`/`parquet` are skipped; `[]` = none; must be a list |
| `duckdb.<any DuckDB setting>` | – | passed through, e.g. `threads`, `max_memory`, `access_mode`, `extension_directory`, `default_order` |

```text
duckdb:
  db_path: ./flapi.db
  threads: 4
  max_memory: 2GB
  default-extensions: [httpfs, parquet, json]
```

Env: `FLAPI_EXTENSION_DIRECTORY` sets `extension_directory` when the config does not.
`FLAPI_EXTENSIONS_OFFLINE=1` never downloads an extension, and a missing one becomes a
startup error. See `flapi-deploy`.

## ducklake (cache catalog)

| Key | Default | Notes |
|---|---|---|
| `ducklake.enabled` | `false` | when true, `metadata-path` and `data-path` are required |
| `ducklake.alias` | `cache` | catalog name used in templates (`{{cache.catalog}}`) |
| `ducklake.metadata-path` | – | a DuckDB file (`./cache.ducklake`) or e.g. `postgres:dbname=...` for a shared catalog |
| `ducklake.data-path` | – | local dir or object-store URL for the Parquet data files |
| `ducklake.override-data-path` | `false` | `OVERRIDE_DATA_PATH` on the DuckLake `ATTACH`: needed to open a catalog prebuilt at another absolute path (the catalog records its data path) |
| `ducklake.read-only` | `false` | replica: attach read-only, no warm-up / scheduled refresh / retention / compaction; cache mutations return 409 |
| `ducklake.warmup` | `always` | `always`, `if-empty`, `never`; endpoint `cache.warmup` and `--cache-warmup` override |
| `ducklake.scheduler.enabled` | `true` | endpoint `cache.schedule`s fire; `false` opts out (startup warning) |
| `ducklake.scheduler.scan-interval` | min(`heartbeat.worker-interval`, shortest `cache.schedule`), floor 1 s | how often due caches are checked; `<n>[s|m|h|d]`, invalid = load error. Startup logs `Cache scheduler on: N scheduled cache(s), checked every X s` |
| `ducklake.retention.keep-last-snapshots` | – | snapshots kept per table; applies to every cache without its own `retention` (an endpoint block replaces it) |
| `ducklake.retention.max-snapshot-age` | – | e.g. `30d`; same inheritance |
| `ducklake.compaction.enabled` | `false` | with a `schedule`, runs merge / rewrite / cleanup once per catalog when due |
| `ducklake.compaction.schedule` | – | `<n>[s|m|h|d]` or `@hourly`, `@daily`, `@midnight`, `@weekly`; anything else is a load error. Nothing runs unless `enabled: true` too |
| `ducklake.data-inlining-row-limit` | – | DuckLake data inlining; a few thousand at most (large values make big merges 60-100x slower) |

```yaml
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: if-empty
  retention:
    keep-last-snapshots: 10
```

Cache design is covered in `flapi-cache-design` and refresh tuning in `flapi-cache-tuning`.

## heartbeat

| Key | Default | Notes |
|---|---|---|
| `heartbeat.enabled` | `false` | runs the warm-up GET for endpoints that set their own `heartbeat.enabled: true` |
| `heartbeat.worker-interval` | `60` (seconds) | the worker's tick, with or without a `heartbeat` block; `<= 0` is a load error. Caps the default scheduler scan interval. |

## mcp

| Key | Default | Notes |
|---|---|---|
| `mcp.enabled` | `true` | MCP JSON-RPC at `/mcp/jsonrpc` on the HTTP port |
| `mcp.instructions` / `mcp.instructions-file` | – | server instructions for LLM clients |
| `mcp.strict-descriptions` | `false` | refuse to start on suspicious tool descriptions |
| `mcp.page-size` | `0` (no pagination) | page size for MCP list results (`tools/list` etc.) |
| `mcp.tasks.*` | workers 2, queue-depth 32 | async MCP tasks |
| `mcp.auth.*` | disabled | see `flapi-auth` / `flapi-mcp` |
| `mcp.port`, `mcp.host` | – | **ignored with a warning**; MCP shares the HTTP port |

## Security, limits, CORS, TLS

| Key | Default | Notes |
|---|---|---|
| `auth.enabled` (+ `type`, `users`, `roles`, `jwt-secret`, `jwt-issuer`, `oidc`) | – | global auth, inherited by every REST endpoint without its own `auth:` block; `enabled` is mandatory when the block exists. See `flapi-auth`. |
| `rate_limit.enabled`, `.max`, `.interval` | – | **underscore**; all three are mandatory when the block exists; `key: ip / user / user-or-ip` |
| `cors.allow-origins` | none: `Access-Control-Allow-Origin: *` | a list; once set, only listed origins get CORS headers; `"*"` allows all (`CORS_WILDCARD_WITH_AUTH` warning when any auth is on) |
| `cors.allow-headers`, `cors.allow-methods` | – | lists |
| `enforce-https.enabled` | `false` | serve TLS directly; needs `ssl-cert-file` and `ssl-key-file`; must be a map |
| `duckdb-hardening.external-access` | `auto` | `auto` / `full` / `restricted`; `auto` = `restricted` when `--config` is remote. Restricted: templates reach only the config dir or URL prefix, `template.path`, the DuckLake `data-path`, `embed://` in a bundle and the lists below; DuckDB settings are locked; `INSTALL`/`LOAD`/`CREATE SECRET` in a template are refused. **Top level**, not under `duckdb:` |
| `duckdb-hardening.allowed-directories`, `.allowed-paths` | `[]` | extra prefixes / files under `restricted` (relative = working directory); they are writable too |

```yaml
rate_limit:
  enabled: true
  max: 100
  interval: 60
  key: user-or-ip
cors:
  allow-origins: [https://app.example.com]
enforce-https:
  enabled: true
  ssl-cert-file: ./ssl/cert.pem
  ssl-key-file: ./ssl/key.pem
```

## Observability and telemetry

| Key | Default | Notes |
|---|---|---|
| `audit.enabled`, `.sink` (`stdout` / `file`), `.path`, `.redact` | off | per-request JSONL audit; see `flapi-operate` |
| `tracing.enabled` (+ `exporter`, `endpoint`, `capture`, ...) | off | OpenTelemetry; read once at startup; see `flapi-operate` |
| `telemetry.enabled` | `true` | anonymous PostHog usage events; `false` opts out (as do `--no-telemetry`, `FLAPI_NO_TELEMETRY=1`, `DATAZOO_DISABLE_TELEMETRY=1`) |
| `telemetry.sample_rate` | `1.0` | down-samples per-request events |

## Rarely needed

| Key | Notes |
|---|---|
| `cache-schema` | fallback schema for cache tables; endpoint `cache.schema` (default `cache`) normally wins |
| `storage.cache.*` | caches REMOTE template files (`template.path` in s3://, gs://, az://, https://); with a local `template.path` this block has no effect and a load warning says so. Plain https needs a `manifest.txt` listing the endpoint YAMLs at the root of `template.path` |
