---
name: flapi-operate
description: >
  Running flAPI in production and diagnosing it: /health readiness states (ready,
  starting, degraded, stalled) and /health/live, 503 cache_warming / cache_not_built /
  cache_build_failed with Retry-After, 500 bodies with request_id, runtime metrics (`flapii metrics`, GET /api/v1/_config/metrics,
  flapi_get_metrics), logs (log-level, log-format json, X-Request-Id, log-http-requests
  for DuckDB's S3/HTTP requests), tracing, audit
  and log correlation, cache operations (refresh, gc, audit) and troubleshooting
  playbooks for 404, 400, 401/403, 429, 500, 503, startup failures, DuckLake locks,
  slow requests and memory. Use when a server misbehaves, a probe fails, an endpoint
  is slow or erroring, or a user asks how to monitor flAPI.
version: 26.11.23
user-invocable: false
---

# flAPI Operations Cheat Sheet

**flAPI:** v26.11.23 | **Skill:** Operate | **See also:** `flapi-live-config`, `flapi-cache-design`, `flapi-cache-tuning`, `flapi-deploy`, `flapi-auth`, `flapi-mcp`

## Critical gotchas

- **`/health` is readiness, `/health/live` is liveness.** `/health` answers 503 while caches
  build, when a cache failed, and when a request has run longer than `stall-timeout-s`.
  Point a liveness probe at `/health` and Kubernetes kills a server that is only warming up.
- **`fatal` (503 on `/health` AND `/health/live`) means restart.** An internal DuckDB error
  invalidated the database; nothing works until the process restarts, so even liveness
  fails. `reason` has the DuckDB text, the log has a critical line with the cause; every
  data and config request answers 503 `database_invalidated`.
- **`degraded` can be 200 or 503.** 503 when a cache **failed** to build. 200 when a cache is
  **stale** (a refresh failed, the last snapshot is served) or **unbuilt** (warm-up skipped).
- **A failed first cache build answers `"error":"cache_build_failed"`** (`Retry-After: 5`).
  The SQL error is in the server log (logged once), in `flapii cache audit <path>` and in the
  answer of a manual `flapii cache refresh <path>`, never in the 503 body.
- **A 500 body never carries SQL or paths.** It is `{"error":"Internal Server Error",
  "message":"... details are in the server log under this request id.","request_id":"req-..."}`.
  `grep <request_id> server.log` gives the DuckDB error.
- **A GET never builds a cache.** With `--no-cache-warmup` or `warmup: never`, an endpoint is
  503 `cache_not_built` until something refreshes it (`flapii cache refresh <path>`,
  schedule, or `flapi cache refresh`).
- **Metrics need the config service**, and reset on restart. Percentiles are estimates,
  interpolated linearly inside a histogram bucket (ten per decade, 100 us ... 60 s, ~25%
  wide) and capped at the observed `max`. A cache 503 (`cache_warming`,
  `cache_not_built`, `cache_build_failed`) counts as `503_cache` per series and as
  `requests.cache_unavailable`, **not** as `5xx` / `server_errors`; other 503s (busy,
  capacity) and MCP tool failures still count as server errors.
- **`flapi cache refresh` (the one-shot) cannot run next to a server on the same DuckLake
  file catalog.** It exits 1 with a JSON `error` saying a running server holds the catalog
  lock; refresh through that server with `flapii cache refresh <path>`.
- **`--validate-config` does not open the database.** A missing extension, a bad `ATTACH`
  or a DuckLake lock passes validation and fails at startup.
- **`flapii` errors show the server's reason**: a status line such as `✘ [400] Bad Request:
  Cache refresh failed`, followed by the JSON body whose `reason` holds the DuckDB error.
- **SIGTERM interrupts running populate scripts** (shutdown within ~10 s); later refreshes
  are refused while shutting down. The interrupted refresh rolls back.
- **Log levels are `debug`, `info`, `warning` (or `warn`), `error`**, on `--log-level`,
  `FLAPI_LOG_LEVEL` and `log-level:`, and on the runtime `flapii config log-level set` /
  `PUT /api/v1/_config/log-level`. An unknown value in the config stops startup.
- **`flapi --version`** and `server.version` in `GET /api/v1/_config/health` give the real
  build version: quote it in a bug report.

## Health and readiness

```bash
curl -s -i http://localhost:8080/health        # readiness, JSON
curl -s http://localhost:8080/health/live      # {"status":"live"}, always 200 while the process serves
curl -s http://localhost:8080/mcp/health       # MCP: tools_count, resources_count
```

| `status` | HTTP | Meaning | Extra keys |
|---|---|---|---|
| `ready` | 200 | Every cache built and fresh | |
| `starting` | 503 | A cache is still being built (warm-up) | `pending: [{table, schema}]` |
| `degraded` | 503 | A cache failed to build | `failed: [{table, schema, state}]` |
| `degraded` | 200 | Serving, but a cache is stale or unbuilt | `stale: [...]`, `unbuilt: [...]` |
| `stalled` | 503 | The oldest in-flight request is older than `stall-timeout-s` (default 60, `0` = off) | `stalled_after_s` |

Real responses (no secrets, no SQL in them; `/health` is unauthenticated):

```json
{"status":"ready","uptime_s":5,"requests":{"in_flight":0,"oldest_ms":0},"caches":{"total":1,"ready":1,"failed":0,"unbuilt":0}}
{"status":"starting","pending":[{"schema":"cache","table":"slow_cache"}],"caches":{"total":1,"ready":0,"failed":0,"unbuilt":0},...}
{"status":"degraded","failed":[{"state":"failed","schema":"cache","table":"slow_cache"}],"caches":{"total":1,"ready":0,"failed":1,"unbuilt":0},...}
{"status":"degraded","unbuilt":[{"state":"unbuilt","schema":"cache","table":"products_cache"}],"caches":{"total":1,"ready":0,"failed":0,"unbuilt":1},...}
```

Kubernetes: `livenessProbe` on `/health/live`, `readinessProbe` (and `startupProbe` for long
warm-ups) on `/health`. Health probes produce no trace span and no audit line.

### 503s an endpoint can return

| Body | `Retry-After` | Cause | Fix |
|---|---|---|---|
| `{"error":"cache_warming","message":"Cache for this endpoint is still being built"}` | 5 | Warm-up running | Wait; `/health` turns `ready` |
| `{"error":"cache_build_failed","message":"Cache for this endpoint failed to build"}` | 5 | First populate failed (SQL error, bare `SELECT`, table not written) | `flapii cache audit <path>`, fix SQL, `flapii cache refresh <path>` |
| `{"error":"cache_not_built","message":"...(warm-up skipped)"}` | 5 | `--no-cache-warmup`, `warmup: never`, or a reload that named a new `cache.table` | `flapii cache refresh <path>` |
| `{"error":"cache_not_built","message":"...the writer has not built it yet"}` | 5 | Read-only replica; the writer has not built the table | Refresh on the writer |
| `{"error":"database_invalidated",...}` | 30 | An internal DuckDB error invalidated the database (`/health` 503 `fatal`) | Restart flapi; report the `reason` |
| `{"error":"The database is busy; retry shortly."}` | 1 | Lock contention outlived 5 retries | Retry; find the writer holding the lock |
| `Server is at capacity. Try again shortly.` (text) | 1 | Handler queue full | Scale out; find slow queries |

## Runtime metrics

```bash
flapii metrics                          # JSON
flapii metrics -o table                 # per-endpoint table + per-cache table
flapii metrics --endpoint /products     # one route or MCP tool name
curl -s -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" \
  "http://localhost:8080/api/v1/_config/metrics?endpoint=/products"
```

Over MCP: `flapi_get_metrics` with optional `{"endpoint": "/products"}`. Without a token the
route is 401 (and counted as a rejection).

Real response from a test server, trimmed (`flapii` prints it unchanged, snake_case,
whatever `--json-style` says; `-o table` adds `N cache-not-ready` to the summary line):

```json
{
  "requests": {
    "since_unix_ms": 1794689683288, "total": 6, "server_errors": 0, "cache_unavailable": 0,
    "rejections": {"unauthorized": 1, "forbidden": 0, "rate_limited": 0},
    "series": [
      {"protocol": "rest", "method": "GET", "route": "/orders", "requests": 6, "rejected": 0,
       "status": {"2xx": 5, "3xx": 0, "4xx": 1, "5xx": 0, "503_cache": 0},
       "latency_ms": {"avg": 15.125, "p50": 14.833, "p95": 29.9, "p99": 30.668, "max": 30.668},
       "last_request_unix_ms": 1794689758394}
    ]
  },
  "caches": [
    {"cache": "cache.cache.orders_c", "endpoint": "/orders", "state": "ready",
     "refreshes": 20, "failures": 0, "data_age_seconds": 9,
     "last": {"mode": "merge", "status": "success", "full": false, "duration_ms": 80,
              "rows_affected": 1, "snapshot_id": "42", "completed_at": "2026-11-14 20:55:49.309"},
     "duration_ms": {"avg": 54, "max": 109}}
  ],
  "endpoints": {"count": 4},
  "arrow": {"total_requests": 0, "...": 0},
  "tracing": {"enabled": false, "spans_submitted": 0, "spans_exported": 0, "spans_dropped": 0, "spans_flush_timeouts": 0}
}
```

- `route` is the endpoint's `url-path`; every unknown path is one `<unmatched>` series
  (MCP tools are `"protocol": "mcp", "tool": ...` series). Config-service routes are not
  counted per route. `rejected` counts that series' 401/403 (REST) or role denials (MCP); the global
  `rejections` also counts config-service 401s. `cache_unavailable` / `503_cache` count
  503s from a cache that is not ready. `last.full` is true for a refresh that loaded the
  whole source.
- `latency_ms` (ms, three decimals) runs from the start of the request, so it includes
  auth: a Basic-auth endpoint shows the ~165 ms PBKDF2 check in `max` for each first
  request with a password (later ones hit the verification cache) and for every wrong one.
- Cache `state`: `starting`, `ready`, `stale`, `failed`, `unbuilt`. A failed refresh has
  `last.status: "error"` and no `rows_affected`. `data_age_seconds` is the age of the data
  being served.
- For refresh history beyond the last restart, use the durable audit:
  `flapii cache audit [path]` (per refresh, snake_case: `event_id`, `sync_type`,
  `sync_started_at`, `sync_completed_at`, `duration_ms` of the populate script,
  `rows_affected` summed over its statements, `snapshot_id`, `status`, `message` with the
  error text; newest 1000 per cache table). `-o table` for a quick view.

## Logs, request ids, tracing and audit

A project with JSON logs, a file audit log and a stall timeout:

```yaml
# flapi: config group=ops
project-name: ops-demo
project-description: Operations settings
template:
  path: ./sqls
connections:
  northwind:
    init: |
      INSTALL sqlite;
      LOAD sqlite;
      ATTACH IF NOT EXISTS './data/northwind.sqlite' AS nw (TYPE sqlite, READ_ONLY);
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
log-level: info            # debug | info | warning (or warn) | error
log-format: json           # one JSON object per line, with request_id
log-http-requests: false   # true: one line per DuckDB S3/GCS/Azure/HTTP request
stall-timeout-s: 60        # /health -> 503 "stalled" past this; 0 disables
audit:
  enabled: true
  sink: file               # stdout (default) | file
  path: ./audit.jsonl
tracing:
  enabled: false           # see below
```

```yaml
# flapi: endpoint group=ops file=products.yaml
url-path: /products
template-source: products.sql
connection: [northwind]
cache:
  enabled: true
  table: products_cache
  schedule: 1h
  template-file: products_cache.sql
# flapi: check GET /health -> 200
# flapi: check GET /health/live -> 200
# flapi: check GET /products -> 200
# flapi: check GET /api/v1/_config/metrics -> 401
```

```sql
-- flapi: template group=ops file=products_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT ProductID, ProductName, UnitPrice FROM nw.Products
```

```sql
-- flapi: template group=ops file=products.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
```

- Every response has `X-Request-Id: req-...`. The same id is on every log line written while
  serving it, the access-log `Response:` line included (`"request_id":"req-3f18..."` in
  JSON, `request_id=req-...` in text), and in the audit line. The `Request:` line is written
  before the id exists; it shares the connection pointer (`0x7fd2800095e0`) with its
  `Response:` line. To debug a failed call: take the header, then
  `grep req-3f1812a93deaa6c1 server.log`. A 500 body carries the same id as `request_id`;
  the DuckDB error is only in the log.
- Change the level at runtime: `flapii config log-level set debug` (until restart). `debug`
  logs the template context and the rendered SQL of each request.
- Precedence: `--log-level`, then `FLAPI_LOG_LEVEL`, then `log-level:` in the config.
- Which object store requests did a call cause? `log-http-requests: true` (or
  `FLAPI_LOG_HTTP_REQUESTS=1`, which wins over the key both ways) logs one `info` line per
  HTTP request DuckDB makes: `duckdb http GET <url> -> 206 4ms request_id=req-...`. The id
  is the request whose query made it (DuckDB worker threads included), `-` for startup
  reads; `ERR` = no response (refused, timeout), retries are separate lines. Headers are
  never printed, URL credentials are redacted. Only the env var covers the fetch of a
  remote `-c s3://.../flapi.yaml`. Read at startup; off by default and free when off.

Tracing (OpenTelemetry, off by default):

```yaml
tracing:
  enabled: true
  exporter: otlp_http                     # otlp_http | otlp_file | none
  endpoint: http://localhost:4318/v1/traces   # the FULL traces URL; a base URL silently loses spans
  capture: metadata                       # off | metadata | payload
  db_profiling: summary                   # off | summary | detailed: DuckDB timings on the query span
```

A traced response also carries `X-Trace-Id` (the caller's trace id if it sent
`traceparent`). An MCP client can pass `traceparent` in `params._meta`. With `otlp_http`,
export failures do not show in the metrics counters: alert on
`[OTLP TRACE HTTP Exporter] ERROR` in the log. Details: `docs/OBSERVABILITY.md`.

## Cache operations

```bash
flapii cache list                         # cache config of every endpoint
flapii cache get /products
flapii cache refresh /products            # build/refresh now, on the running server
flapii cache refresh /products --full     # drop the table and rebuild from scratch
flapii cache audit /products -o table     # refresh history with durations, rows, errors
flapii cache audit                        # all endpoints
flapii cache gc /products                 # expire snapshots per retention, delete their files (JSON counts; no_policy without retention)
flapii cache drop-table old.products_cache  # drop a cache table no endpoint uses (after a schema: change)
flapii cache compact [/products]          # DuckLake compaction now: whole catalog, or one table
flapi cache refresh -c flapi.yaml --all   # one-shot job; server must NOT be running on this DuckLake
flapi cache refresh -c flapi.yaml --endpoint /products   # exit 0 ok, 1 failed or catalog locked, 2 usage
flapi cache refresh -c flapi.yaml --endpoint /orders --method GET   # only when cached endpoints on the path differ
flapi -c flapi.yaml --no-cache-warmup     # start without building caches
flapi -c flapi.yaml --cache-warmup if-empty   # always | if-empty | never (env FLAPI_CACHE_WARMUP)
```

Over MCP: `flapi_refresh_cache`, `flapi_get_cache_audit`, `flapi_run_cache_gc`,
`flapi_get_cache_status` (each takes `path`). On a read-only replica
(`ducklake.read-only: true`) refresh, GC, cache config/template updates and drop-table
return 409 `read_only_cache`. Compaction history: `GET /api/v1/_config/cache/audit?sync_type=compaction`. Designing and
tuning caches: `flapi-cache-design`, `flapi-cache-tuning`.

## Troubleshooting by symptom

Full playbooks (symptom, commands, causes, fix):
[references/troubleshooting.md](references/troubleshooting.md).

| Symptom | First command | Usual cause |
|---|---|---|
| 404 `Not Found` | `flapii endpoints list -o table` | `url-path` mismatch (trailing `/`), file not loaded, new file without restart |
| 400 `{"errors":[{"field":..,"message":..}]}` | `flapii endpoints parameters <path>` | Value outside the validator; a `field-in: header` value sent as a query param |
| 401 | `curl -i` (look at `WWW-Authenticate`) | Missing/wrong credentials; inherited global `auth:`; config routes need the config token |
| 403 `{"error":"Forbidden",...}` | server log (`Authorization denied for user ...`) | Caller lacks every role in `auth.roles` (REST) or MCP `allowed-roles` |
| 429 `Rate limit exceeded` | `curl -i` (`X-RateLimit-*`, `Retry-After`) | `rate-limit.max` per `interval` reached |
| 500 `{"error":"Internal Server Error",...,"request_id":"req-..."}` | `grep <request_id> server.log` | SQL/template error, missing table, connection |
| 503 | `curl -s localhost:8080/health` | Cache warming/build failed/unbuilt, lock contention, overload |
| Exits at startup | run in foreground, read the last `[ERROR]` | Env var not whitelisted, missing extension, DuckLake lock, bad YAML |
| Slow | `flapii metrics -o table` | Uncached query on a remote source, wide results |

## Canonical workflow: from a failing request to a fix

1. Reproduce with `curl -s -i` and note the status, the body and `X-Request-Id` (a 500 body
   repeats it as `request_id`).
2. `curl -s localhost:8080/health`: is the server ready, warming, degraded or stalled?
3. `grep <request-id> <log>` for the error (raise the level with
   `flapii config log-level set debug` and repeat to see the rendered SQL).
4. Pick the playbook for the status code in
   [references/troubleshooting.md](references/troubleshooting.md).
5. Reproduce the SQL alone: `flapii templates expand <path> -p '{..}'`, then
   `flapii templates test <path> -p '{..}'`.
6. Fix the YAML/SQL. Validate with `flapi -c flapi.yaml --validate-config`, then
   `flapii endpoints reload <path>` (SQL edits need nothing; see `flapi-live-config`).
7. Confirm with the original `curl` and with `flapii metrics -e <path>`.
8. Reset the log level: `flapii config log-level set info`.
