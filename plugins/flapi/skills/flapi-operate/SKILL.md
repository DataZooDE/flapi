---
name: flapi-operate
description: >
  Running flAPI in production and diagnosing it: /health readiness states (ready,
  starting, degraded, stalled) and /health/live, 503 cache_warming / cache_not_built
  with Retry-After, runtime metrics (`flapii metrics`, GET /api/v1/_config/metrics,
  flapi_get_metrics), logs (log-level, log-format json, X-Request-Id), tracing, audit
  and log correlation, cache operations (refresh, gc, audit) and troubleshooting
  playbooks for 404, 400, 401/403, 429, 500, 503, startup failures, DuckLake locks,
  slow requests and memory. Use when a server misbehaves, a probe fails, an endpoint
  is slow or erroring, or a user asks how to monitor flAPI.
version: 26.10.31
user-invocable: false
---

# flAPI Operations Cheat Sheet

**flAPI:** v26.10.31 | **Skill:** Operate | **See also:** `flapi-live-config`, `flapi-cache-design`, `flapi-cache-tuning`, `flapi-deploy`, `flapi-auth`, `flapi-mcp`

## Critical gotchas

- **`/health` is readiness, `/health/live` is liveness.** `/health` answers 503 while caches
  build, when a cache failed, and when a request has run longer than `stall-timeout-s`.
  Point a liveness probe at `/health` and Kubernetes kills a server that is only warming up.
- **`degraded` can be 200 or 503.** 503 when a cache **failed** to build. 200 when a cache is
  **stale** (a refresh failed, the last snapshot is served) or **unbuilt** (warm-up skipped).
- **A failed cache build answers `"error":"cache_warming"`** with the message `Cache for this
  endpoint failed to build`. Read the message, not just the code. The SQL error is only in
  the server log and in `flapii cache audit`.
- **A GET never builds a cache.** With `--no-cache-warmup` or `warmup: never`, an endpoint is
  503 `cache_not_built` until something refreshes it (`flapii cache refresh <path>`,
  schedule, or `flapi cache refresh`).
- **Metrics need the config service**, and reset on restart. Percentiles are bucket upper
  bounds (100 us ... 60 s), so `p50` can be larger than `max`. 503 answers (warming, busy)
  count as `5xx` and as `server_errors`; so do MCP tool failures, including bad arguments.
- **`flapi cache refresh` (the one-shot) cannot run next to a server on the same DuckLake.**
  It fails with `Conflicting lock is held in .../flapi (PID n)`. Use `flapii cache refresh`
  against the running server instead.
- **`--validate-config` does not open the database.** A missing extension, a bad `ATTACH`
  or a DuckLake lock passes validation and fails at startup.
- **`flapii` shows only `Bad Request` when a refresh fails.** The reason is in
  `flapii cache audit <path>` (`message`) and the server log.
- **SIGTERM waits for an in-flight warm-up query to finish.** A long cache build delays
  shutdown, so give the container a long enough grace period.
- **Log levels are `debug`, `info`, `warning`, `error`.** `warn` is rejected.

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
| `{"error":"cache_warming","message":"Cache for this endpoint failed to build"}` | 5 | Populate SQL failed | `flapii cache audit <path>`, fix SQL, `flapii cache refresh <path>` |
| `{"error":"cache_not_built","message":"...(warm-up skipped)"}` | 5 | `--no-cache-warmup` or `warmup: never` | `flapii cache refresh <path>` |
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

Real response from a test server, trimmed (raw REST keys; `flapii` camelCases them):

```json
{
  "requests": {
    "since_unix_ms": 1794652929023, "total": 22, "server_errors": 2,
    "rejections": {"unauthorized": 1, "forbidden": 0, "rate_limited": 0},
    "series": [
      {"protocol": "rest", "method": "GET", "route": "/customer-lookup", "requests": 14,
       "status": {"2xx": 11, "3xx": 0, "4xx": 3, "5xx": 0},
       "latency_ms": {"avg": 3.06, "p50": 5, "p95": 10, "p99": 10, "max": 5.139},
       "last_request_unix_ms": 1794653073515},
      {"protocol": "rest", "method": "GET", "route": "<unmatched>", "requests": 1,
       "status": {"2xx": 0, "3xx": 0, "4xx": 1, "5xx": 0}, "latency_ms": {"...": 0}},
      {"protocol": "mcp", "tool": "customer_lookup", "requests": 5, "failures": 1,
       "latency_ms": {"avg": 2.72, "p50": 5, "p95": 10, "p99": 10, "max": 5.516}}
    ]
  },
  "caches": [
    {"cache": "cache.cache.products_cache", "endpoint": "/products", "state": "ready",
     "refreshes": 2, "failures": 0, "data_age_seconds": 0,
     "last": {"mode": "full", "status": "success", "duration_ms": 31, "rows_affected": 77,
              "snapshot_id": "6", "completed_at": "2026-11-14 10:45:35.745"},
     "duration_ms": {"avg": 35, "max": 40}}
  ],
  "endpoints": {"count": 5},
  "arrow": {"total_requests": 0, "...": 0},
  "tracing": {"enabled": false, "spans_submitted": 0, "spans_exported": 0, "spans_dropped": 0, "spans_flush_timeouts": 0}
}
```

- `route` is the endpoint's `url-path`; every unknown path is one `<unmatched>` series.
  Config-service routes are not counted per route.
- Cache `state`: `starting`, `ready`, `stale`, `failed`, `unbuilt`. A failed refresh has
  `last.status: "error"` and no `rows_affected`. `data_age_seconds` is the age of the data
  being served.
- For refresh history beyond the last restart, use the durable audit:
  `flapii cache audit [path]` (per refresh: `syncStartedAt`, `syncCompletedAt`,
  `durationMs` of the populate statement, `rowsAffected` of its last statement, `snapshotId`,
  `status`, `message` with the error text).

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
log-level: info            # debug | info | warning | error
log-format: json           # one JSON object per line, with request_id
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
  serving it (`"request_id":"req-3f18..."` in JSON, `request_id=req-...` in text), and in
  the audit line. To debug a failed call: take the header, then
  `grep req-3f1812a93deaa6c1 server.log`. The 500 body also carries the DuckDB error.
- Change the level at runtime: `flapii config log-level set debug` (until restart). `debug`
  logs the template context and the rendered SQL of each request.
- Precedence: `--log-level`, then `FLAPI_LOG_LEVEL`, then `log-level:` in the config.

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
flapii cache audit /products              # refresh history with durations, rows, errors
flapii cache audit                        # all endpoints
flapii cache gc /products                 # expire snapshots per cache.retention
flapi cache refresh -c flapi.yaml --all   # one-shot job; server must NOT be running on this DuckLake
flapi cache refresh -c flapi.yaml --endpoint /products   # exit 0 ok, 1 a refresh failed, 2 usage
flapi -c flapi.yaml --no-cache-warmup     # start without building caches
flapi -c flapi.yaml --cache-warmup if-empty   # always | if-empty | never (env FLAPI_CACHE_WARMUP)
```

Over MCP: `flapi_refresh_cache`, `flapi_get_cache_audit`, `flapi_run_cache_gc`,
`flapi_get_cache_status` (each takes `path`). On a read-only replica
(`ducklake.read-only: true`) refresh and GC return 409 `read_only_cache`. Designing and
tuning caches: `flapi-cache-design`, `flapi-cache-tuning`.

## Troubleshooting by symptom

Full playbooks (symptom, commands, causes, fix):
[references/troubleshooting.md](references/troubleshooting.md).

| Symptom | First command | Usual cause |
|---|---|---|
| 404 `Not Found` | `flapii endpoints list -o table` | `url-path` mismatch (trailing `/`), file not loaded, new file without restart |
| 400 `{"errors":[{"field":..,"message":..}]}` | `flapii endpoints parameters <path>` | Value outside the validator; `values:` vs `allowedValues:` |
| 401 | `curl -i` (look at `WWW-Authenticate`) | Missing/wrong credentials; config routes need the config token |
| 403 | MCP response text | MCP `allowed-roles` do not match the caller's roles |
| 429 `Rate limit exceeded` | `curl -i` (`X-RateLimit-*`, `Retry-After`) | `rate-limit.max` per `interval` reached |
| 500 `Internal Server Error: Query execution failed: ...` | `flapii templates expand <path> -p '{..}'` | SQL/template error, missing table, connection |
| 503 | `curl -s localhost:8080/health` | Cache warming/failed/unbuilt, lock contention, overload |
| Exits at startup | run in foreground, read the last `[ERROR]` | Env var not whitelisted, missing extension, DuckLake lock, bad YAML |
| Slow | `flapii metrics -o table` | Uncached query on a remote source, wide results |

## Canonical workflow: from a failing request to a fix

1. Reproduce with `curl -s -i` and note the status, the body and `X-Request-Id`.
2. `curl -s localhost:8080/health`: is the server ready, warming, degraded or stalled?
3. `grep <request-id> <log>` (or raise the level: `flapii config log-level set debug` and
   repeat) to see the rendered SQL and the error.
4. Pick the playbook for the status code in
   [references/troubleshooting.md](references/troubleshooting.md).
5. Reproduce the SQL alone: `flapii templates expand <path> -p '{..}'`, then
   `flapii templates test <path> -p '{..}'`.
6. Fix the YAML/SQL. Validate with `flapi -c flapi.yaml --validate-config`, then
   `flapii endpoints reload <path>` (SQL edits need nothing; see `flapi-live-config`).
7. Confirm with the original `curl` and with `flapii metrics -e <path>`.
8. Reset the log level: `flapii config log-level set info`.
