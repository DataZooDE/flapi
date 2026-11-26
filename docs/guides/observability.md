# Observability

**Goal:** find out why a request failed, whether the server is ready, what is
slow, what DuckDB fetched, and who called what.

This guide covers the tasks. For every `tracing:` key, the data-protection tiers
and the measured costs, see the reference [OBSERVABILITY.md](../OBSERVABILITY.md).

**Before you start:**

- `flapi` and `flapii` are installed (`pip install flapi-io` installs both; see
  [Getting started](./getting-started.md)).
- `flapii` talks to the server's config service. Tasks 1, 3 and 6 use it, so
  the server runs with `--config-service`, and `FLAPI_CONFIG_SERVICE_TOKEN` is
  exported in every terminal you use. The server and `flapii` both read it.

Without any configuration you already get these:

- Every response, REST and MCP alike, carries an `X-Request-Id` header. flAPI
  always mints it itself and ignores an `X-Request-Id` sent by the caller.
- Every log line written while a request is served carries that id.
- `/health`, `/health/live` and `/mcp/health` answer without a token.

Tracing, the audit log and the DuckDB HTTP log stay off until you turn them on.

---

## The project used in this guide

All tasks except the failed-cache example in task 2 use one project:

```
observability-guide/
├── flapi.yaml
├── data/customers.parquet
└── sqls/
    ├── customers.yaml, customers.sql           a plain endpoint
    ├── broken.yaml, broken.sql                 an endpoint that always fails
    └── nation-counts.yaml, nation-counts.sql,  a cached endpoint
        nation-counts-cache.sql
```

The sample data is the customers table from the flAPI repository. Clone the
repository, or download just the file:

```bash
mkdir -p observability-guide/data observability-guide/sqls
cd observability-guide
curl -L -o data/customers.parquet \
  https://github.com/DataZooDE/flapi/raw/main/examples/data/customers.parquet
```

**`flapi.yaml`.** Each observability block is explained in the task that uses it:

<!-- flapi: config group=main -->
```yaml
project-name: observability-guide
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
ducklake:                      # the cache store, needed by /nation-counts
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
log-level: info                # task 1
log-format: json               # task 1
tracing:                       # task 3
  enabled: true
  exporter: otlp_file
  file:
    path: ./traces.jsonl
  flush:
    mode: on_response
  db_profiling: summary
audit:                         # task 5
  enabled: true
  sink: file
  path: ./audit.jsonl
  redact: [password, tax_id]
```

**`sqls/customers.yaml`** and **`sqls/customers.sql`**. A customer lookup with
an optional, validated `id`:

<!-- flapi: endpoint group=main file=customers.yaml -->
```yaml
url-path: /customers
request:
  - field-name: id
    field-in: query
    required: false
    validators:
      - type: int
        min: 1
template-source: customers.sql
connection: [customers]
```
<!-- flapi: check GET /customers?id=3 -> 200 contains "Customer#000000003" -->
<!-- flapi: check GET /customers?id=abc -> 400 contains "Invalid integer value" -->

<!-- flapi: template group=main file=customers.sql -->
```sql
SELECT c_custkey, c_name, c_nationkey
FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{ params.id }}
{{/params.id}}
```

**`sqls/broken.yaml`** and **`sqls/broken.sql`**. This endpoint fails on purpose,
so that task 1 has an error to find:

<!-- flapi: endpoint group=main file=broken.yaml -->
```yaml
url-path: /broken
template-source: broken.sql
connection: [customers]
```
<!-- flapi: check GET /broken -> 500 contains "request_id" -->
<!-- flapi: check GET /broken -> 500 contains "The details are in the server log" -->

<!-- flapi: template group=main file=broken.sql -->
```sql
SELECT no_such_column FROM read_parquet('{{{ conn.path }}}')
```

**`sqls/nation-counts.yaml`**, **`sqls/nation-counts.sql`** and
**`sqls/nation-counts-cache.sql`**. A cached endpoint: flAPI builds the table
`nation_counts` at startup, rebuilds it every hour, and requests read the table.
See [Caching](./caching.md) for how caches work.

<!-- flapi: endpoint group=main file=nation-counts.yaml -->
```yaml
url-path: /nation-counts
template-source: nation-counts.sql
connection: [customers]
cache:
  enabled: true
  table: nation_counts
  schedule: 1h
  template-file: nation-counts-cache.sql
```
<!-- flapi: check GET /nation-counts?limit=3 -> 200 contains "total_count" -->

<!-- flapi: template group=main file=nation-counts.sql -->
```sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
```

<!-- flapi: template group=main file=nation-counts-cache.sql -->
```sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT c_nationkey, count(*) AS customers
FROM read_parquet('{{{ conn.path }}}')
GROUP BY c_nationkey
```

Start the server with its log written to a file. It keeps running, so use a
second terminal for everything after this:

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=dev-token
flapi -c flapi.yaml --config-service > flapi.log 2>&1
# or, to watch the log as well:  ... 2>&1 | tee flapi.log
```

## 1. Find out why a request failed

A `500` never carries the SQL or the database error, only an id:

```bash
curl -i http://localhost:8080/broken
```

```
HTTP/1.1 500 Internal Server Error
X-Request-Id: req-9248425d44c3d9f6
X-Trace-Id: 3efbd06af66dffb89f25a1cb3cbe6777

{"request_id":"req-9248425d44c3d9f6","message":"The request failed on the server. The details are in the server log under this request id.","error":"Internal Server Error"}
```

Search the server log for the id:

```bash
grep req-9248425d44c3d9f6 flapi.log
```

```json
{"message":"Request: 127.0.0.1:37986 0x7fc61c01bd10 HTTP/1.1 GET /broken","request_id":"req-9248425d44c3d9f6","level":"info","timestamp":"2026-11-25T12:53:32.495685Z"}
{"message":"Error handling request: Query execution failed during main query: Binder Error: Referenced column \"no_such_column\" not found in FROM clause! ...","request_id":"req-9248425d44c3d9f6","span_id":"5452a8c331fbb598","trace_id":"3efbd06af66dffb89f25a1cb3cbe6777","level":"error","timestamp":"2026-11-25T12:53:32.497912Z"}
{"message":"Response: 0x7fc61c01bd10 /broken 500 0","request_id":"req-9248425d44c3d9f6","span_id":"5452a8c331fbb598","trace_id":"3efbd06af66dffb89f25a1cb3cbe6777","level":"info","timestamp":"2026-11-25T12:53:32.498068Z"}
```

`log-format: json` writes one JSON object per line, which a log collector can
ingest directly. With the default `log-format: text`, the same line reads
`(2026-11-25T12:53:32.497912Z) [error] Error handling request: ... request_id=req-9248425d44c3d9f6`.
`trace_id` and `span_id` are there because tracing is on (task 3).

A `400` means validation failed. Its body names the field that failed
(`{"errors":[{"message":"Invalid integer value","field":"id"}]}`), and the query
never runs.

### Set the log level

The levels are `debug`, `info`, `warning` (or `warn`) and `error`. An unknown
value stops startup instead of falling back to `info`. Precedence, highest first:

```bash
flapi -c flapi.yaml --log-level debug      # 1. the flag
FLAPI_LOG_LEVEL=debug flapi -c flapi.yaml  # 2. the environment variable
# 3. `log-level:` in flapi.yaml           4. the default, info
```

At `debug` the log also shows each request's rendered SQL
(`Processed query: ...`), which is often enough to fix a template.

To change the level of a running server without a restart:

```bash
flapii config log-level set debug
flapii config log-level set info

# the same, without flapii
curl -X PUT -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" \
     -H "Content-Type: application/json" -d '{"level":"debug"}' \
     http://localhost:8080/api/v1/_config/log-level
```

A level set this way lasts until the process exits. `log-format` cannot be
changed at runtime. `flapii config log-level get` (`GET .../_config/log-level`)
reports the level the server runs at, whether it came from the flag, the
variable, the config or a runtime change. Up to v26.11.26 it answered `info`
until a level was set at runtime.

## 2. Check that the server is up and ready

| Route | Question it answers | `200` | `503` |
|---|---|---|---|
| `/health/live` | Is the process serving? | `{"status":"live"}` as soon as the port listens | only `fatal`: DuckDB was invalidated and only a restart helps |
| `/health` | Should it get traffic? | `ready`, or `degraded` for a stale or unbuilt cache | `starting` (caches warming), `degraded` (a cache's first build failed), `stalled`, `fatal` |
| `/mcp/health` | Is MCP up, and with how many tools? | always, with `tools_count` and `resources_count` | — |

None of them needs a token, and none produces a trace span or an audit line.

```bash
curl -s http://localhost:8080/health/live    # {"status":"live"}
curl -s http://localhost:8080/mcp/health     # ..."tools_count":...
curl -s http://localhost:8080/health
```

```json
{"status":"ready","requests":{"oldest_ms":0,"in_flight":0},"caches":{"failed":0,"unbuilt":0,"stale":0,"ready":1,"total":1},"uptime_s":4}
```
<!-- flapi: check group=main GET /health/live -> 200 contains "live" -->
<!-- flapi: check group=main GET /health -> 200 contains ""status":"ready"" -->
<!-- flapi: check group=main GET /health -> 200 contains ""ready":1" -->
<!-- flapi: check group=main GET /mcp/health -> 200 contains "tools_count" -->

While the cache is still being built, `/health` answers `503` with
`"status":"starting"` and a `pending` list. The cached endpoint itself answers
`503 {"error":"cache_warming",...}` with `Retry-After: 5`.

### When the first cache build fails

This is a **separate project**: the same `/nation-counts` endpoint, with a
populate query that cannot work. In a new directory with the same
`data/customers.parquet`, **`flapi.yaml`**:

<!-- flapi: config group=failed -->
```yaml
project-name: failed-cache
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
```

**`sqls/nation-counts.yaml`**, **`sqls/nation-counts.sql`** and the broken
**`sqls/nation-counts-cache.sql`**:

<!-- flapi: endpoint group=failed file=nation-counts.yaml -->
```yaml
url-path: /nation-counts
template-source: nation-counts.sql
connection: [customers]
cache:
  enabled: true
  table: nation_counts
  schedule: 1h
  template-file: nation-counts-cache.sql
```

<!-- flapi: template group=failed file=nation-counts.sql -->
```sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
```

<!-- flapi: template group=failed file=nation-counts-cache.sql -->
```sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT no_such_column FROM read_parquet('{{{ conn.path }}}')
```

`/health` and the endpoint both stay `503`:

```json
{"status":"degraded","failed":[{"state":"failed","schema":"cache","table":"nation_counts"}],"caches":{"failed":1,"unbuilt":0,"stale":0,"ready":0,"total":1},...}
```
<!-- flapi: check group=failed GET /health -> 503 contains ""status":"degraded"" -->
<!-- flapi: check group=failed GET /health -> 503 contains ""failed":1" -->
<!-- flapi: check group=failed GET /nation-counts -> 503 contains "cache_build_failed" -->

`/health` is unauthenticated, so the error text is only in the server log and in
`flapii cache audit /nation-counts` (task 6). When a cache was built once and a
later refresh fails, the cache keeps serving its last snapshot (the previous
version of the table). `/health` then answers `200` with `"status":"degraded"`
and a `stale` list.

`stalled` means the oldest request in flight is older than `stall-timeout-s`
(top level of `flapi.yaml`, default `60`, `0` turns the check off), which
usually means something downstream stopped answering. `/health` answers `503`;
`/health/live` does not, so the instance leaves rotation without a restart.

### Probes

Point liveness (and a startup probe) at `/health/live` and readiness at
`/health`. Do not point liveness at `/health`: a slow cache warm-up would turn
into a restart loop. Kubernetes:

```text
livenessProbe:  { httpGet: { path: /health/live, port: 8080 } }
readinessProbe: { httpGet: { path: /health, port: 8080 } }
```

Cloud Run: use `/health/live` for the startup and liveness probes. The
[Serverless guide](./serverless.md#5-probes-and-ci) has the details.

## 3. See what is slow

### Runtime metrics

With the config service on, flAPI counts requests, status classes, rejections
and latency for each endpoint and MCP tool. For each cache it records refresh
time and data age. Tracing does not have to be on.

```bash
flapii metrics -o table
flapii metrics --endpoint /customers     # one route or MCP tool name, as JSON
```

```
5 requests, 1 server errors, 0 cache-not-ready 503s · rejected: 0 × 401, 0 × 403, 0 × 429
┌────────────────────┬──────────┬───────────────┬──────────┬────────┬────────┬────────┬────────┬────────┐
│ Endpoint           │ Requests │ Errors        │ Rejected │ Avg Ms │ P50 Ms │ P95 Ms │ P99 Ms │ Max Ms │
├────────────────────┼──────────┼───────────────┼──────────┼────────┼────────┼────────┼────────┼────────┤
│ GET /nation-counts │ 1        │ 0 4xx / 0 5xx │ 0        │ 15     │ ~14    │ ~15    │ ~15    │ 15     │
│ GET /customers     │ 3        │ 1 4xx / 0 5xx │ 0        │ 3.32   │ ~3.60  │ ~6.10  │ ~6.10  │ 6.10   │
│ GET /broken        │ 1        │ 0 4xx / 1 5xx │ 0        │ 2.37   │ ~2.25  │ ~2.37  │ ~2.37  │ 2.37   │
└────────────────────┴──────────┴───────────────┴──────────┴────────┴────────┴────────┴────────┴────────┘

🗄️  Caches
┌───────────────────────────┬───────┬──────┬─────────────┬─────────┬──────┬────────┬──────────────┬───────┐
│ Cache                     │ State │ Mode │ Last Status │ Last Ms │ Rows │ Avg Ms │ Refreshes    │ Age S │
├───────────────────────────┼───────┼──────┼─────────────┼─────────┼──────┼────────┼──────────────┼───────┤
│ cache.cache.nation_counts │ ready │ full │ success     │ 21      │ 25   │ 21     │ 1 (0 failed) │ 5     │
└───────────────────────────┴───────┴──────┴─────────────┴─────────┴──────┴────────┴──────────────┴───────┘
```

`flapii metrics` reads `GET /api/v1/_config/metrics` (`?endpoint=/customers`
narrows it). That route needs the config-service token, as
`Authorization: Bearer <token>` or `X-Config-Token: <token>`, and answers `401`
without it. An MCP client reads the same document with the `flapi_get_metrics`
tool.
<!-- flapi: check group=main GET /api/v1/_config/metrics -> 401 -->
<!-- flapi: check group=main GET /api/v1/_config/metrics header=X-Config-Token:skill-check-token -> 200 contains "nation_counts" -->
<!-- flapi: check group=main GET /api/v1/_config/metrics?endpoint=/customers header=X-Config-Token:skill-check-token -> 200 contains "since_unix_ms" -->

The counters start at zero when the process starts (`requests.since_unix_ms`).
The percentiles are estimated from a histogram, hence the `~`: good enough to
find the slow endpoint, not for SLO reports. The format is described field by field in
[CONFIG_SERVICE_API_REFERENCE](../CONFIG_SERVICE_API_REFERENCE.md#get-apiv1_configmetrics).

### Traces

Metrics show which endpoint is slow. A trace shows where the time goes inside one
request. The terms used below:

- A **span** is one timed step.
- A **trace** is the tree of spans for one request.
- **OTLP** is the OpenTelemetry wire format.
- The **exporter** is where flAPI sends spans: a collector over HTTP, or a file.
- **`traceparent`** is the W3C header that carries a trace id from one service
  to the next.

The project above writes spans to `traces.jsonl` (`exporter: otlp_file`), so no
collector is needed. Each span is written as it ends (`flush.mode: on_response`),
and `db_profiling: summary` adds DuckDB's own timings to the query span.
`tracing:` is read once at startup: after changing it, restart.

#### What the spans show

A traced response carries `X-Trace-Id`. To list the spans of one trace:

```bash
curl -si "http://localhost:8080/customers?limit=2" | grep X-Trace-Id
jq -c '.resourceSpans[].scopeSpans[].spans[]
       | select(.traceId == "d1f84460518e617ef049d77d61efa4f0")
       | {name, ms: (((.endTimeUnixNano|tonumber) - (.startTimeUnixNano|tonumber)) / 1e6), attributes}' \
   traces.jsonl
```

For `GET /customers?limit=2`:

```
GET /customers                SERVER    6.1 ms
├── flapi.render_template     INTERNAL  0.1 ms
├── main query                CLIENT    3.4 ms
└── count query               CLIENT    0.4 ms
```

| Span | Attributes for triage |
|---|---|
| `GET /customers`, the request | `http.route`, `http.response.status_code`, `error.type` |
| `main query`, the DuckDB query | `db.response.returned_rows`, `flapi.db.latency_ms`, `flapi.db.bytes_read`, `flapi.db.blocked_thread_time_ms` |
| `count query` | the `total_count` query of a full page; a shorter page needs none |

If the request span is long and the queries are short, the time went into
something else, such as rendering, serialisation or a slow client. If
`main query` is long, compare `flapi.db.bytes_read` with
`flapi.db.blocked_thread_time_ms`: the first points to a big scan, the second to
contention. For `/broken`, `error.type` is `internal_error` on the request span
and `query_failed` on the query span. No span carries the error text, the SQL or
a parameter value. The full span tree and attribute list are in
[OBSERVABILITY § 3](../OBSERVABILITY.md#3-what-is-traced) and
[§ 4](../OBSERVABILITY.md#4-seeing-inside-a-slow-query).

**MCP and agents.** An MCP `tools/call` is a single span, `tools/call <tool>`.
If an agent sends its own `traceparent` in `params._meta` (SEP-414), flAPI's
span becomes a child of the agent's span, and `X-Trace-Id` returns the agent's
trace id. This works even with tracing off, so the agent's trace id still lands
in flAPI's audit and log lines. See [MCP tools](./mcp-tools.md) and
[OBSERVABILITY.md](../OBSERVABILITY.md).

### Tracing to Jaeger, locally

This example is not run in CI. Jaeger accepts OTLP/HTTP on port 4318 and shows
traces on port 16686:

```bash
docker run --rm -p 16686:16686 -p 4318:4318 jaegertracing/all-in-one:1.62.0
```

In `flapi.yaml`, replace the `tracing:` block:

```yaml
tracing:
  enabled: true
  exporter: otlp_http
  endpoint: http://localhost:4318/v1/traces   # the FULL trace URL
  service_name: flapi-local
```

Restart flAPI, send a few requests and open <http://localhost:16686>, service
`flapi-local`. An OpenTelemetry Collector works the same way: point `endpoint`
at its OTLP/HTTP receiver, including the `/v1/traces` path. flAPI uses
`endpoint` exactly as written, so `http://localhost:4318` would post to `/`.

### Tracing in production

Not run in CI. A production `tracing:` block for a hosted backend:

```yaml
tracing:
  enabled: true
  exporter: otlp_http
  endpoint: https://otlp.example.com/v1/traces
  headers:
    Authorization: "Bearer {{env.FLAPI_OTLP_TOKEN}}"   # whitelist FLAPI_OTLP_TOKEN under template.environment-whitelist
  service_name: flapi
  sample:
    type: parentbased_traceidratio   # follow the caller's decision, else sample
    ratio: 0.1                       # 10% of new traces
  capture: metadata
```

Add `resource_attributes` to label the spans, e.g. `deployment.environment:
production`, or `service.instance.id` set to the pod name.

`capture` sets how much is exported:

- `metadata` (the default) exports structure and timings, no values.
- `payload` also exports the values of declared request fields. Credentials
  are still redacted, and startup warns that you now export personal data.
- `off` exports nothing.

Details are in [OBSERVABILITY § 5](../OBSERVABILITY.md#5-data-protection).
`OTEL_SERVICE_NAME`, `OTEL_TRACES_SAMPLER` and similar variables have no effect;
see [§ 11](../OBSERVABILITY.md#11-environment-variables).

To verify export, read the `tracing` block of the metrics:

```bash
curl -s -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" \
     http://localhost:8080/api/v1/_config/metrics | jq .tracing
```

`spans_submitted`, `spans_exported` and `spans_dropped` show spans leaving the
process. They do **not** show HTTP errors from the collector: the OTLP/HTTP
exporter reports success even when the collector answers 4xx or 5xx. The only
reliable signal is the flAPI log line `[OTLP TRACE HTTP Exporter] ERROR`, so
alert on that. See [§ 9](../OBSERVABILITY.md#9-checking-that-export-is-actually-working).

## 4. See what DuckDB fetches

When a template reads from S3, GCS or HTTP(S), DuckDB makes the requests, and
by default nothing shows them. To log one line per DuckDB HTTP request, set this
at the top level of `flapi.yaml`:

```yaml
log-http-requests: true
```

or set `FLAPI_LOG_HTTP_REQUESTS=1` without editing the file. The variable wins
over the key (`0` turns logging off). It is also the only switch that covers what is
fetched before the key is read: a remote `--config` and the YAML includes. Both
are read at startup, and startup warns while logging is on. This example reads a
CSV over HTTP from a template whose URL ends in `?sig=...`:

```
(2026-11-25T12:30:20.832838Z) [info] duckdb http HEAD http://127.0.0.1:18600/cities.csv?redacted -> 200 3ms request_id=req-d4f79aa32416b040
(2026-11-25T12:30:20.834271Z) [info] duckdb http GET http://127.0.0.1:18600/cities.csv?redacted -> 200 1ms request_id=req-d4f79aa32416b040
(2026-11-25T12:30:20.848073Z) [info] duckdb http HEAD http://127.0.0.1:18600/nosuch.csv?redacted -> 404 1ms request_id=req-4b08f3dbb709bea0
```

- `request_id` is the request that caused the fetch, so `grep req-...` lists that
  request's object-store traffic. `-` means no request caused it: startup or a
  scheduled cache refresh.
- `?redacted` replaces the whole query string. A URL without a query string is
  logged without it.
- One file often shows up several times. DuckDB checks the file (`HEAD`)
  separately from reading it, and a full page runs a second query
  (`count query`) for `total_count`.

**This is a debugging switch.** Turn it on to answer a question, then off again:

- **URL paths are not redacted.** A token in a path, such as a Slack webhook,
  is logged as is. So is a request parameter a template puts into a path:
  `nosuch` above came from `?file=nosuch`.
- Each DuckDB HTTP request is a synchronous log write. A glob over thousands of
  objects writes thousands of lines.

The line format, `ERR` statuses, Azure and the interaction with DuckDB's own
logging are in [OBSERVABILITY § 8](../OBSERVABILITY.md#which-object-store-requests-did-a-request-cause).

## 5. Keep an audit trail

With `audit:` enabled as in the project's `flapi.yaml`, flAPI writes one JSON
line per REST request and MCP call. Each line records who called, what, with
which parameters, the outcome and the latency:

```bash
cat audit.jsonl
```

```json
{"params":{"id":"3"},"latency_ms":3,"target":"/customers","row_count":1,"status":"success","method":"GET","principal":"anonymous","span_id":"7e9f64b9b93597f9","request_id":"req-bc8a5c39c9237e45","trace_id":"373c9c086ef9553fefc36d88f1fc99ea","timestamp":"2026-11-25T12:53:32.473659Z"}
{"params":{"id":"abc"},"latency_ms":0,"target":"/customers","row_count":-1,"status":"error:400","method":"GET","principal":"anonymous","span_id":"7c257f1a3097e480","request_id":"req-15b1a41f0ee12c4d","trace_id":"a1ba7bf180c2b389bfa63c590277e260","timestamp":"2026-11-25T12:53:32.479336Z"}
{"params":{},"latency_ms":2,"target":"/broken","row_count":-1,"status":"error:500","method":"GET","principal":"anonymous","span_id":"5452a8c331fbb598","request_id":"req-9248425d44c3d9f6","trace_id":"3efbd06af66dffb89f25a1cb3cbe6777","timestamp":"2026-11-25T12:53:32.498142Z"}
```

- `sink` is `stdout` (the default, mixed into the log stream), `file` or `null`.
  `path` is required for `file`. A relative path is resolved against the
  working directory. The file is created if missing, but its directory is not:
  if the file cannot be opened, flAPI does not start.
- `status`:
  - `success`;
  - `denied` (401 or 403);
  - `rate_limited` (429);
  - `error:<HTTP status>` for REST;
  - for MCP, `dry_run` or `error:<reason>`.
- `principal` is the authenticated user, or `anonymous`. `target` is the route
  or the MCP tool name, and `method` is the HTTP method or `tools/call`.
- `row_count` is the number of rows the request returned: the rows of the page
  for a REST read (GET, CSV, Arrow or a read over POST), not `total_count`, and
  the rows of an MCP tool result. For a REST write it is `rows_affected`, the
  rows the write changed. It is `-1` when there is no count: a denial, a `400`,
  a failed query. Up to v26.11.26 every REST line said `-1`.
- `trace_id` and `span_id` are present when the request has a trace context.
- `redact` replaces the value of each listed parameter with `<redacted>`.
  Names are matched whole, ignoring case, `-` and `_`, so `tax_id` covers
  `Tax-Id` but not `customer_tax_id`. Credential-shaped names such as
  `password` or `api_key` are redacted whatever the list says. The same list
  redacts trace payloads.

**Rotation.** flAPI opens the file once, in append mode, and never reopens it.
If you rename the file, flAPI keeps writing to the renamed file. Rotate with
`copytruncate` (logrotate), or restart flAPI after moving the file. A write
error after startup is not reported.

The health probes write no audit line. Reference:
[CONFIG_REFERENCE § 2.9.2](../CONFIG_REFERENCE.md#292-audit-log).

## 6. Cache refresh history

`/health` and `flapii metrics` show the current state. In addition, every cache
refresh is recorded durably, with its mode, outcome, duration, rows and DuckLake
snapshot (the table version the refresh produced):

```bash
flapii cache audit /nation-counts --output table
flapii cache audit /nation-counts          # JSON, newest first
flapii cache audit                         # every cache
```

```
┌────────────────────────────┬──────┬─────────┬─────────────┬──────┬──────────┬──────────────────────────────┐
│ Time                       │ Mode │ Status  │ Duration Ms │ Rows │ Snapshot │ Message                      │
├────────────────────────────┼──────┼─────────┼─────────────┼──────┼──────────┼──────────────────────────────┤
│ 2026-11-25T12:30:56.522000 │ full │ success │ 17          │ 25   │ 6        │ Cache refreshed successfully │
│ 2026-11-25T12:30:47.022000 │ full │ success │ 28          │ 25   │ 4        │ Cache refreshed successfully │
└────────────────────────────┴──────┴─────────┴─────────────┴──────┴──────────┴──────────────────────────────┘
```

Use it to follow refresh time over days, not only since the last restart, and to
read the error of a failed build. `--table <schema.table>` selects one cache
table, whichever endpoint refreshed it. Tuning refresh time is covered in
[Caching](./caching.md#measuring-and-tuning-refresh-time).

## Running it in production

**What to alert on:**

| Signal | Where |
|---|---|
| `/health` is `503` with `stalled` or `degraded` | the readiness probe, or a synthetic check |
| 5xx rate per endpoint | `requests.series[].status["5xx"]` in `/api/v1/_config/metrics` |
| a cache `failed`, or `stale` with growing `data_age_seconds` | `/health` (`failed`, `stale`), `caches[]` in the metrics |
| trace export failing | the log line `[OTLP TRACE HTTP Exporter] ERROR`; rising `spans_dropped` or `spans_flush_timeouts` |
| `level: error` log lines | your log pipeline, with `log-format: json` |

Before you build alerts on these, know the limits:

- **There is no Prometheus endpoint.** The metrics are one JSON document, so
  scrape and convert it, or alert on traces and logs instead.
- **The metrics need the config-service token.** That token also unlocks the
  mutating config API (endpoint, template and cache changes). There is no
  read-only token, so give the token only to trusted systems, and keep
  `/api/v1/_config/*` off the public ingress.
- **Counters are per replica and in memory.** They reset on restart, so add them
  up across replicas, and use `flapii cache audit` for history.
- **Audit write errors are silent** after startup (task 5), so watch the file's
  growth.

**Behind a proxy or gateway:**

- flAPI always mints `X-Request-Id` and ignores one sent by the caller. To join
  a gateway's request id, log flAPI's `X-Request-Id` response header at the
  gateway.
- An HTTP `traceparent` header is honoured on every route, REST included, so a
  traced gateway becomes the parent span. For MCP, a `traceparent` in `_meta`
  takes precedence.
- `X-Forwarded-For` is not read. Rate limiting by IP sees the proxy's address,
  so all clients share one budget, and the audit log records no client address.
- `/health` is unauthenticated and lists cache table names. Restrict it at the
  ingress to your probes.

## Common mistakes

- **A tracing `endpoint` without `/v1/traces`.** flAPI posts to `/`, and the
  export counters still look healthy.
- **Liveness on `/health`.** A slow cache warm-up then turns into restarts.
- **Expecting the error in the 500 body.** The error is in the log, under the
  `request_id`.
- **`FLAPI_LOG_LEVEL=DEBUG`.** Startup refuses it. The flag, the variable and
  `log-level:` take lower case. `flapii config log-level set` accepts any case.
- **Changing `tracing:` without a restart.** flAPI reads the block only at startup.
- **Leaving `log-http-requests` on.** URL paths, and the parameters in them, end
  up in your log store.
- **Rotating the audit file with a rename.** flAPI keeps writing to the renamed
  file.

## Next

- [Serverless](./serverless.md): probes, and why spans can be lost when CPU is
  allocated only during requests
- [Caching](./caching.md): cache readiness and refresh tuning
- [Authentication](./authentication.md): what `principal` reports
- [MCP tools](./mcp-tools.md): exposing endpoints to agents

## How it works

The first middleware runs before rate limiting and auth. It mints the
`X-Request-Id`, reads the trace context and starts the request span. That is
why 401, 403 and 429 rejections and 404s are traced too. The same ids are
stamped on the audit line and on every log line of the request.

- [OBSERVABILITY.md](../OBSERVABILITY.md), the reference:
  - [§ 2](../OBSERVABILITY.md#2-turning-tracing-on): every `tracing:` key
  - [§ 3](../OBSERVABILITY.md#3-what-is-traced): the span tree
  - [§ 5](../OBSERVABILITY.md#5-data-protection): capture tiers and redaction
  - [§ 6](../OBSERVABILITY.md#6-deployment): serverless flushing
  - [§ 9](../OBSERVABILITY.md#9-checking-that-export-is-actually-working): export counters
- [spec/components/observability.md](../spec/components/observability.md): the
  request context, span lifetime and exporters
- Source:
  - `src/request_context_middleware.cpp`: the request id, the trace context and
    the request span
  - `src/query_executor.cpp`: the query span and `db_profiling`
  - `src/api_server.cpp`: `/health` and `/health/live`
  - `src/runtime_metrics.cpp` and `src/config_service.cpp`: metrics and the log
    level
  - `src/audit_logger.cpp`
  - `src/duckdb_http_log_sink.cpp`
