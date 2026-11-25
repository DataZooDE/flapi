# Observability

**Goal:** find out why a request failed, whether the server is ready, what is
slow, what DuckDB fetched, and who called what.

This guide is the task view. The full reference, including every `tracing:` key,
the data-protection tiers and the measured costs, is
[OBSERVABILITY.md](../OBSERVABILITY.md).

**Before you start:** a project that runs (see [Getting started](./getting-started.md)).
Tasks 3 and 6 use `flapii`, which talks to the config service: start the server
with `--config-service` and export `FLAPI_CONFIG_SERVICE_TOKEN` (or pass
`--config-service-token`).

What you get without configuring anything:

- every response, REST and MCP alike, carries an `X-Request-Id` header. flAPI
  always mints it; an `X-Request-Id` the caller sends is ignored;
- every log line written while a request is served carries that id;
- `/health`, `/health/live` and `/mcp/health` answer without a token.

Tracing, the audit log and the DuckDB HTTP log are all off until you turn them on.

---

## 1. Find out why a request failed

A `500` never carries the SQL or the database error. It carries an id:

<!-- flapi: config group=logs -->
```yaml
project-name: observability-guide
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
log-level: info
log-format: json        # one JSON object per line; the default is text
```

<!-- flapi: endpoint group=logs file=broken.yaml -->
```yaml
url-path: /broken
template-source: broken.sql
connection: [customers]
```
<!-- flapi: check GET /broken -> 500 contains "request_id" -->
<!-- flapi: check GET /broken -> 500 contains "The details are in the server log" -->

<!-- flapi: template group=logs file=broken.sql -->
```sql
SELECT no_such_column FROM read_parquet('{{{ conn.path }}}')
```

```bash
curl -i http://localhost:8080/broken
```

```
HTTP/1.1 500 Internal Server Error
X-Request-Id: req-b6ee83ba608487d5

{"request_id":"req-b6ee83ba608487d5","message":"The request failed on the server. The details are in the server log under this request id.","error":"Internal Server Error"}
```

Grep the server log for the id:

```bash
grep req-b6ee83ba608487d5 flapi.log
```

```json
{"message":"Request: 127.0.0.1:57118 0x7f28b400cd80 HTTP/1.1 GET /broken","request_id":"req-b6ee83ba608487d5","level":"info","timestamp":"2026-11-25T12:29:31.115301Z"}
{"message":"Error handling request: Query execution failed during main query: Binder Error: Referenced column \"no_such_column\" not found in FROM clause! ...","request_id":"req-b6ee83ba608487d5","level":"error","timestamp":"2026-11-25T12:29:31.118266Z"}
{"message":"Response: 0x7f28b400cd80 /broken 500 0","request_id":"req-b6ee83ba608487d5","level":"info","timestamp":"2026-11-25T12:29:31.118321Z"}
```

With tracing on, each line also has `trace_id` and `span_id`. With the default
`log-format: text` the same lines read
`(2026-11-25T12:29:31.118266Z) [error] Error handling request: ... request_id=req-b6ee83ba608487d5`.

A `400` is different: it is a validation failure, and its body says which field
failed (`{"errors":[{"message":"Invalid integer value","field":"id"}]}`), and
the query never runs.

### Set the log level

Levels are `debug`, `info`, `warning` (or `warn`) and `error`. An unknown value
stops startup rather than falling back to `info`. In order of precedence:

```bash
flapi -c flapi.yaml --log-level debug      # 1. the flag
FLAPI_LOG_LEVEL=debug flapi -c flapi.yaml  # 2. the environment variable
# 3. `log-level:` in flapi.yaml           4. info
```

At `debug` the log also shows the rendered SQL of each request
(`Processed query: ...`), which is usually all you need to fix a template.

To change the level of a running server, without a restart:

```bash
flapii config log-level set debug
flapii config log-level set info

# the same, without flapii
curl -X PUT -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" \
     -H "Content-Type: application/json" -d '{"level":"debug"}' \
     http://localhost:8080/api/v1/_config/log-level
```

The runtime level lasts until the process exits. `log-format` cannot be changed
at runtime.

## 2. Check that the server is up and ready

There are two probes, and they answer different questions.

| Route | Answers | `200` | `503` |
|---|---|---|---|
| `/health/live` | Is the process serving? | `{"status":"live"}` as soon as the port listens | only `fatal`: DuckDB was invalidated and only a restart helps |
| `/health` | Should it get traffic? | `ready`, or `degraded` for a stale or unbuilt cache | `starting` (caches warming), `degraded` (a cache's first build failed), `stalled`, `fatal` |
| `/mcp/health` | Is MCP up, and how many tools? | always, with `tools_count` and `resources_count` | — |

None of them needs a token, and none produces a trace span or an audit line.

<!-- flapi: endpoint group=probes file=nation-counts.yaml -->
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
<!-- flapi: check GET /health/live -> 200 contains "live" -->
<!-- flapi: check GET /health -> 200 contains ""status":"ready"" -->
<!-- flapi: check GET /health -> 200 contains ""ready":1" -->
<!-- flapi: check GET /mcp/health -> 200 contains "tools_count" -->
<!-- flapi: check GET /api/v1/_config/metrics -> 401 -->

<!-- flapi: template group=probes file=nation-counts.sql -->
```sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
```

<!-- flapi: template group=probes file=nation-counts-cache.sql -->
```sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT c_nationkey, count(*) AS customers
FROM read_parquet('{{{ conn.path }}}')
GROUP BY c_nationkey
```

```bash
curl -s http://localhost:8080/health
```

```json
{"status":"ready","requests":{"oldest_ms":0,"in_flight":0},"caches":{"failed":0,"unbuilt":0,"stale":0,"ready":1,"total":1},"uptime_s":4}
```

While the cache is still building, `/health` answers `503` with
`"status":"starting"` and a `pending` list, and the cached endpoint itself
answers `503 {"error":"cache_warming",...}` with `Retry-After: 5`. If the first
build fails, both stay `503`:

<!-- flapi: endpoint group=failed-cache file=nation-counts.yaml -->
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
<!-- flapi: check GET /health -> 503 contains ""status":"degraded"" -->
<!-- flapi: check GET /health -> 503 contains ""failed":1" -->
<!-- flapi: check GET /nation-counts -> 503 contains "cache_build_failed" -->

<!-- flapi: template group=failed-cache file=nation-counts.sql -->
```sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
```

<!-- flapi: template group=failed-cache file=nation-counts-cache.sql -->
```sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT no_such_column FROM read_parquet('{{{ conn.path }}}')
```

```json
{"status":"degraded","failed":[{"state":"failed","schema":"cache","table":"nation_counts"}],"caches":{"failed":1,"unbuilt":0,"stale":0,"ready":0,"total":1},...}
```

The error itself is only in the server log (`/health` is unauthenticated);
`flapii cache audit /nation-counts` shows it too (task 6). A cache that was
built once and whose *refresh* fails keeps serving its last snapshot: `/health`
is then `200` with `"status":"degraded"` and a `stale` list.

`stalled` means the oldest request in flight is older than `stall-timeout-s`
(top level of `flapi.yaml`, default `60`, `0` turns the check off): something
downstream stopped answering. `/health` reports it as `503`; `/health/live` does
not, so the process is taken out of rotation but not restarted.

### Kubernetes

Point liveness at `/health/live` and readiness at `/health` (Kubernetes YAML,
not flAPI config):

```text
livenessProbe:
  httpGet: { path: /health/live, port: 8080 }
  periodSeconds: 10
readinessProbe:
  httpGet: { path: /health, port: 8080 }   # 503 while warming, failed first build, stalled
  periodSeconds: 5
startupProbe:
  httpGet: { path: /health/live, port: 8080 }
  failureThreshold: 30
```

### Cloud Run

Use `/health/live` for the startup and the liveness probe (Cloud Run service
YAML, not flAPI config):

```text
startupProbe:
  httpGet: { path: /health/live, port: 8080 }
  periodSeconds: 2
  failureThreshold: 30
livenessProbe:
  httpGet: { path: /health/live, port: 8080 }
  periodSeconds: 10
```

A startup probe on `/health` would hold traffic until every cache is built, but
a cache whose first build fails then answers `503` for ever, and Cloud Run
restarts the instance in a loop. Cached endpoints answer `503 cache_warming`
with `Retry-After` until they are ready, so clients can retry. See
[Serverless](./serverless.md#5-probes-and-ci).

## 3. See what is slow

### Runtime metrics

With the config service on, flAPI counts requests, status classes, rejections
and latency per endpoint and MCP tool, and refresh time and data age per cache.
Tracing does not need to be on.

```bash
flapii metrics -o table
flapii metrics --endpoint /customers     # one route or MCP tool name, JSON
```

```
7 requests, 1 server errors, 0 cache-not-ready 503s · rejected: 0 × 401, 0 × 403, 0 × 429
┌────────────────────┬──────────┬───────────────┬──────────┬────────┬────────┬────────┬────────┬────────┐
│ Endpoint           │ Requests │ Errors        │ Rejected │ Avg Ms │ P50 Ms │ P95 Ms │ P99 Ms │ Max Ms │
├────────────────────┼──────────┼───────────────┼──────────┼────────┼────────┼────────┼────────┼────────┤
│ GET /customers     │ 5        │ 1 4xx / 0 5xx │ 0        │ 4.76   │ ~5.33  │ ~9.13  │ ~9.13  │ 9.13   │
│ tool get_customers │ 1        │ 0             │ 0        │ 3.15   │ ~2.85  │ ~3.15  │ ~3.15  │ 3.15   │
│ GET /broken        │ 1        │ 0 4xx / 1 5xx │ 0        │ 2.99   │ ~2.85  │ ~2.99  │ ~2.99  │ 2.99   │
└────────────────────┴──────────┴───────────────┴──────────┴────────┴────────┴────────┴────────┴────────┘
```

`flapii metrics` reads `GET /api/v1/_config/metrics` (add `?endpoint=/customers`
to narrow it), which needs the config-service token; without it the answer is
`401`. An MCP client reads the same document with the `flapi_get_metrics` tool.
Counters start at zero when the process starts (`requests.since_unix_ms`), and
the percentiles are histogram estimates, hence the `~`: good enough to find the
slow endpoint, not for SLO reports. The field-by-field format is in
[CONFIG_SERVICE_API_REFERENCE](../CONFIG_SERVICE_API_REFERENCE.md#get-apiv1_configmetrics).

### Traces

To see where the time goes inside one request, turn on OpenTelemetry tracing.
The `tracing:` block is read once at startup; changing it needs a restart.

The quickest way to look at spans needs no collector: write them to a file.

<!-- flapi: config group=trace-file -->
```yaml
project-name: observability-guide
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
tracing:
  enabled: true
  exporter: otlp_file
  file:
    path: ./traces.jsonl      # one OTLP/JSON document per line
  flush:
    mode: on_response         # write each span as it ends
  db_profiling: summary       # DuckDB's own timings on the query span
```

<!-- flapi: endpoint group=trace-file file=customers.yaml -->
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

<!-- flapi: template group=trace-file file=customers.sql -->
```sql
SELECT c_custkey, c_name FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{ params.id }}
{{/params.id}}
```

A traced response carries `X-Trace-Id` next to `X-Request-Id`.

#### With Jaeger, locally

Not run in CI. Start Jaeger, which accepts OTLP/HTTP on port 4318 and shows
traces on 16686:

```bash
docker run --rm -p 16686:16686 -p 4318:4318 jaegertracing/all-in-one:1.62.0
```

```yaml
tracing:
  enabled: true
  exporter: otlp_http
  endpoint: http://localhost:4318/v1/traces   # the FULL trace URL
  service_name: flapi-local
```

Restart flAPI, send a few requests, and open <http://localhost:16686>, service
`flapi-local`. An OpenTelemetry Collector works the same way: point `endpoint`
at its OTLP/HTTP receiver, path `/v1/traces` included.

`endpoint` is used exactly as written. `http://localhost:4318` posts to `/`,
which a collector rejects, and flAPI's export counters still look healthy
(see [OBSERVABILITY § 9](../OBSERVABILITY.md#9-checking-that-export-is-actually-working)).
When no spans arrive, look in the flAPI log for
`[OTLP TRACE HTTP Exporter] ERROR`.

#### What the spans show

One REST request, `GET /customers?id=3`, with `db_profiling: summary`:

```
GET /customers                SERVER    3.7 ms
├── flapi.render_template     INTERNAL  0.2 ms
└── main query                CLIENT    1.6 ms
```

| Span | Attributes worth reading |
|---|---|
| `GET /customers` (the request) | `http.route` (the template, never the filled path), `http.response.status_code`, `http.response.body.size`, `flapi.auth.kind`, `user_agent.original`, `flapi.trace.context_source` (`none`, `header`, `meta` or `meta_over_header`), `error.type` on a failure |
| `flapi.render_template` | `flapi.template.path`, `flapi.template.bytes`, `flapi.params.prepared_count` |
| `main query` (the DuckDB query) | `db.system.name: duckdb`, `db.operation.name` (`SELECT`, ...), `db.response.returned_rows`; with `db_profiling`: `flapi.db.latency_ms`, `flapi.db.blocked_thread_time_ms`, `flapi.db.bytes_read` (`detailed` adds `flapi.db.cpu_time_ms` and `flapi.db.rows_scanned`) |

A full page of a paginated GET runs a second query, `count query`, for
`total_count`, so the two costs show up apart. If the request span is long and
`main query` short, the time went elsewhere (rendering, serialisation, a slow
client). If `main query` is long, `flapi.db.bytes_read` and
`flapi.db.blocked_thread_time_ms` tell a big scan from contention.

The failing request from task 1 has `error.type: internal_error` on the request
span and `error.type: query_failed` on the query span. Neither carries the error
text, the SQL or any parameter value: at the default `capture: metadata` no
customer data is exported (see
[OBSERVABILITY § 5](../OBSERVABILITY.md#5-data-protection)).

#### Joining an agent's trace (SEP-414)

An MCP `tools/call` is one span named `tools/call <tool>`, with
`mcp.method.name`, `gen_ai.tool.name` and `gen_ai.operation.name: execute_tool`.
An agent that traces itself puts its W3C `traceparent` into `params._meta`:

```json
{"jsonrpc": "2.0", "id": 1, "method": "tools/call",
 "params": {"name": "get_customers", "arguments": {"id": 2},
            "_meta": {"traceparent": "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"}}}
```

flAPI's span then becomes a child of the agent's span `00f067aa0ba902b7`, in
trace `4bf92f3577b34da6a3ce929d0e0e4736`, with
`flapi.trace.context_source: meta` (`meta_over_header` when an HTTP
`traceparent` was also sent); the response's `X-Trace-Id` is the agent's
trace id. `_meta` wins over an HTTP `traceparent` header, because behind a
gateway the header carries the gateway's span. The ids reach the audit log and
the application log even with tracing off, so an agent's trace can be joined to
flAPI's log lines without an exporter.

## 4. See what DuckDB fetches

When a template reads from S3, GCS or HTTP(S), DuckDB makes the requests and
nothing shows them by default. Turn on one log line per DuckDB HTTP request:

```yaml
log-http-requests: true      # top level of flapi.yaml
```

or, without editing the file, `FLAPI_LOG_HTTP_REQUESTS=1`. The variable wins
over the key in both directions (`0` turns it off), and it is the only switch
that also covers what is fetched before the key is read: a remote
`--config` and the YAML includes. Both are read once, at startup, and startup
warns while it is on.

```
(2026-11-25T12:30:20.832838Z) [info] duckdb http HEAD http://127.0.0.1:18600/cities.csv?redacted -> 200 3ms request_id=req-d4f79aa32416b040
(2026-11-25T12:30:20.834271Z) [info] duckdb http GET http://127.0.0.1:18600/cities.csv?redacted -> 200 1ms request_id=req-d4f79aa32416b040
(2026-11-25T12:30:20.848073Z) [info] duckdb http HEAD http://127.0.0.1:18600/nosuch.csv?redacted -> 404 1ms request_id=req-4b08f3dbb709bea0
```

The format is `duckdb http <METHOD> <url> -> <status> <N>ms request_id=<id>`.
`<status>` is `ERR` when no response came back; DuckDB's retries are separate
lines. `request_id` is the request whose query caused the fetch, so
`grep req-...` lists one request's object-store traffic; `-` means no request
caused it (startup, a scheduled cache refresh). With `log-format: json` it is the
record's `request_id` field.

**This is a debugging switch.** Turn it on to answer a question, then off:

- The whole query string becomes `?redacted` and userinfo (`user:pw@`) is
  dropped, but **URL paths are not redacted.** A token in a path (a Slack
  webhook, a bot token) is logged as is, and so is a request parameter that a
  template puts into a path: `nosuch` above came from `?file=nosuch`.
- Every DuckDB HTTP request is a synchronous log write, and a glob over
  thousands of objects writes thousands of lines per call.
- The lines are `info`; `log-level: warning` hides them.
- It takes over DuckDB's own logging: `duckdb_logs` stays empty while it is on.

Azure (`az://`, `abfss://`) is not verified. The details are in
[OBSERVABILITY § 8](../OBSERVABILITY.md#which-object-store-requests-did-a-request-cause).

## 5. Keep an audit trail

The audit log writes one JSON line per REST request and MCP call: who, what,
which parameters, the outcome and the latency.

<!-- flapi: config group=audit -->
```yaml
project-name: observability-guide
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
audit:
  enabled: true
  sink: file               # file | stdout (the default)
  path: ./audit.jsonl      # required for sink: file; created if missing
  redact: [password, tax_id]
```

<!-- flapi: endpoint group=audit file=customers.yaml -->
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
<!-- flapi: check GET /customers?id=3 -> 200 -->
<!-- flapi: check GET /customers?id=abc -> 400 -->

<!-- flapi: template group=audit file=customers.sql -->
```sql
SELECT c_custkey, c_name FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{ params.id }}
{{/params.id}}
```

```json
{"params":{"id":"3"},"latency_ms":3,"target":"/customers","row_count":-1,"status":"success","method":"GET","principal":"anonymous","request_id":"req-dd1838dc93b20d96","timestamp":"2026-11-25T12:29:31.111080Z"}
{"params":{"id":"abc"},"latency_ms":0,"target":"/customers","row_count":-1,"status":"error:400","method":"GET","principal":"anonymous","request_id":"req-2d95a3284cb95556","timestamp":"2026-11-25T12:29:31.123367Z"}
{"params":{"id":"2"},"latency_ms":2,"target":"get_customers","row_count":1,"status":"success","method":"tools/call","principal":"anonymous","request_id":"req-8561eb10995f71c1","timestamp":"2026-11-25T12:29:31.130822Z"}
```

- `status` is `success`, or `error:<HTTP status>`.
- `principal` is the authenticated user, `anonymous` without auth.
- `target` is the REST path or the MCP tool name; `method` is the HTTP method or
  `tools/call`.
- `row_count` is filled for MCP tool calls; REST requests log `-1`.
- `trace_id` and `span_id` are added when the request has a trace context.
- `redact` replaces the value of each listed parameter with `<redacted>`. The
  names match whole, ignoring case, `-` and `_` (`tax_id` covers `Tax-Id`, not
  `customer_tax_id`); credential-shaped names such as `password` or `api_key`
  are redacted whatever the list says. The same list redacts trace payloads.

The health probes write no audit line. The file grows without limit: rotate it
with your usual tooling. Reference: [CONFIG_REFERENCE § 2.9.2](../CONFIG_REFERENCE.md#292-audit-log).

## 6. Cache refresh history

`/health` and `flapii metrics` show the state now. Every refresh is also written
durably, with its mode, outcome, duration, rows and snapshot:

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

Use it to watch refresh time over days rather than since the last restart, and
to find the error of a failed build. `--table <schema.table>` selects one cache
table, whichever endpoint refreshed it. Tuning refresh time is covered in
[Caching](./caching.md#measuring-and-tuning-refresh-time).

## Common mistakes

- **A tracing `endpoint` without `/v1/traces`.** It is used verbatim, spans go
  to `/`, and the export counters still read healthy. Use the full trace URL.
- **Liveness on `/health`.** It answers `503` while caches warm and after a
  failed first build, so a supervisor restarts an instance that is only busy.
  Liveness goes on `/health/live`.
- **Expecting the error in the 500 body.** It is in the log, under the
  `request_id`.
- **`FLAPI_LOG_LEVEL=DEBUG`.** Startup refuses it: the flag, the variable and
  `log-level:` take lower-case `debug`, `info`, `warning`/`warn` and `error`.
  (`flapii config log-level set` accepts any case.)
- **Trusting `flapii config log-level get` after startup.** It reports `info`
  until the level has been set at runtime, even when the server was started with
  `--log-level debug`.
- **Changing `tracing:` and expecting it to apply.** The block is read at
  startup only; restart.
- **Leaving `log-http-requests` on.** URL paths, and the parameters a template
  puts into them, end up in your log store.
- **Expecting `OTEL_SERVICE_NAME` or `OTEL_TRACES_SAMPLER` to work.** Service
  name, sampling and the exporter come from the `tracing:` block only, and an
  `OTEL_EXPORTER_OTLP_ENDPOINT` in the environment does not turn tracing on
  ([OBSERVABILITY § 11](../OBSERVABILITY.md#11-environment-variables)).

## Next

- [Serverless](./serverless.md): Cloud Run probes, and why spans can be lost
  when CPU is only allocated during requests
- [Caching](./caching.md): cache readiness and refresh tuning
- [Authentication](./authentication.md): what `principal` and `flapi.auth.kind`
  report

## How it works

The `X-Request-Id`, the trace context and the request span are created in the
first middleware, before rate limiting and auth, so 401/403/429 rejections and
404s are traced too. The same ids are stamped onto the audit line and every log
line of the request.

- [OBSERVABILITY.md](../OBSERVABILITY.md): the reference, with
  [§ 2](../OBSERVABILITY.md#2-turning-tracing-on) every `tracing:` key,
  [§ 3](../OBSERVABILITY.md#3-what-is-traced) the span tree,
  [§ 5](../OBSERVABILITY.md#5-data-protection) the capture tiers and redaction,
  [§ 6](../OBSERVABILITY.md#6-deployment) serverless flushing,
  [§ 9](../OBSERVABILITY.md#9-checking-that-export-is-actually-working) export counters
- [spec/components/observability.md](../spec/components/observability.md): request
  context, span lifetime, exporters
- Source: `src/request_context_middleware.cpp` (request id, trace context, the
  request span), `src/query_executor.cpp` (the query span and `db_profiling`),
  `src/api_server.cpp` (`/health`, `/health/live`), `src/runtime_metrics.cpp`
  and `src/config_service.cpp` (metrics, log level), `src/audit_logger.cpp`,
  `src/duckdb_http_log_sink.cpp`
