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
| `project-description` | **required** | OpenAPI description |
| `http-port` | `8080` | overridden by `-p/--port`, then `FLAPI_PORT` |
| `http-host` | `0.0.0.0` | overridden by `--host`, then `FLAPI_HOST`; `127.0.0.1` = loopback only |
| `log-level` | `info` | `debug`, `info`, `warning`, `error`. `--log-level` / `FLAPI_LOG_LEVEL` win. An unknown value here falls back to `info`. |
| `log-format` | `text` | `json` = one JSON object per line, carrying `request_id` (and `trace_id` with tracing) |
| `stall-timeout-s` | `60` | `GET /health` answers 503 `stalled` when a request is older than this; `0` disables |
| `server-name` | `localhost` | parsed, but has **no effect** today |
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
| `duckdb.default-extensions` | `[httpfs, ducklake, fts, json, postgres, sqlite, parquet]` | installed and loaded at startup; `[]` = none; must be a list |
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
| `ducklake.override-data-path` | `false` | passed to the DuckLake `ATTACH` |
| `ducklake.read-only` | `false` | replica: attach read-only, no warm-up / scheduled refresh / retention / compaction; cache mutations return 409 |
| `ducklake.warmup` | `always` | `always`, `if-empty`, `never`; endpoint `cache.warmup` and `--cache-warmup` override |
| `ducklake.scheduler.enabled` | `false` | **must be `true` for any endpoint `cache.schedule` to fire.** Without it caches are only built at warm-up or on demand. |
| `ducklake.scheduler.scan-interval` | – | parsed, but has **no effect**; the scan runs every `heartbeat.worker-interval` seconds |
| `ducklake.retention.keep-last-snapshots` | – | snapshots kept per table |
| `ducklake.retention.max-snapshot-age` | – | e.g. `30d` |
| `ducklake.compaction.enabled` | `false` | merges adjacent files; with a `schedule` set it runs once every 24 h, whatever the value |
| `ducklake.data-inlining-row-limit` | – | DuckLake data inlining |

```yaml
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: if-empty
  scheduler:
    enabled: true
  retention:
    keep-last-snapshots: 10
```

Cache design is covered in `flapi-cache-design` and refresh tuning in `flapi-cache-tuning`.

## heartbeat

| Key | Default | Notes |
|---|---|---|
| `heartbeat.enabled` | `false` | runs the warm-up GET for endpoints that set their own `heartbeat.enabled: true` |
| `heartbeat.worker-interval` | `10` (seconds) | the worker's tick. This is also how often scheduled cache refreshes are checked, whether or not `heartbeat.enabled` is set. |

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
| `auth.enabled` (+ `type`, `users`, `jwt-secret`, `jwt-issuer`, `oidc`) | – | global auth; `enabled` is mandatory when the block exists. See `flapi-auth`. |
| `rate_limit.enabled`, `.max`, `.interval` | – | **underscore**; all three are mandatory when the block exists; `key: ip / user / user-or-ip` |
| `cors.allow-origins` | none: `Access-Control-Allow-Origin: *` | a list; once set, only listed origins get CORS headers; `"*"` allows all |
| `cors.allow-headers`, `cors.allow-methods` | – | lists |
| `enforce-https.enabled` | `false` | serve TLS directly; needs `ssl-cert-file` and `ssl-key-file`; must be a map |

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
| `storage.cache.*` | parsed, but has **no effect** today (nothing reads it) |
