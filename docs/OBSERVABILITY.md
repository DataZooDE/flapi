# Observability

flAPI emits **OpenTelemetry traces** for every HTTP request on every route, and
correlates them with its audit log and application log through one shared request
identity.

Tracing is **off by default**. Turning it on is an explicit operator action, and
even then flAPI exports **no customer data** unless you opt a specific endpoint
into payload capture.

> This is a different subsystem from the PostHog product telemetry described in
> [TELEMETRY.md](../TELEMETRY.md), with a different purpose, a different consent
> model and a different destination. Neither implies the other.

---

## 1. What you get without configuring anything

Even with tracing disabled, every response carries `X-Request-Id`, and that id
appears in the audit log and in every application log line emitted while serving
that request. A support report quoting the header is enough to find the request.

## 2. Turning tracing on

```yaml
tracing:
  enabled: true                 # off by default; nothing else turns it on
  exporter: otlp_http           # otlp_http | otlp_file | none
  endpoint: http://localhost:4318/v1/traces
  capture: metadata             # off | metadata | payload
```

That is the minimum. Everything below has a working default.

### Full configuration

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | Master switch. Nothing is exported until this is true. |
| `service_name` | `flapi` | `service.name` resource attribute. |
| `service_namespace` | — | `service.namespace`. |
| `exporter` | `otlp_http` | `otlp_http`, `otlp_file`, or `none`. Any unrecognised value behaves as `none`. |
| `endpoint` | SDK default | The **full** OTLP/HTTP trace URL, e.g. `http://host:4318/v1/traces`. An overridden URL is used verbatim — `/v1/traces` is **not** appended — so a base URL posts to `/` and a real collector rejects it. Applies to `otlp_http` only. |
| `protocol` | unset | Only the exact string `http/json` changes anything (it switches the content type). Any other value, including `http/protobuf`, is ignored. |
| `headers` | `{}` | Extra headers, e.g. auth for a SaaS backend. Applies to `otlp_http` only. |
| `timeout_ms` | `10000` | Export timeout. |
| `capture` | `metadata` | See §4. |
| `openinference` | `false` | OpenInference attribute overlay. |
| `db_profiling` | `off` | `off`, `summary` or `detailed`. DuckDB execution metrics on the database span. Costs a round trip per query — see §4. |
| `exclude_routes` | probes + docs | Routes that produce no span. An explicit list **replaces** the defaults rather than adding to them, and each entry is matched as an exact path, not a prefix or glob. |
| `sample.type` | `parentbased_traceidratio` | `always_on`, `always_off`, `parentbased_traceidratio`. |
| `sample.ratio` | `1.0` | Sampling ratio. |
| `flush.mode` | `batch` | `batch` or `on_response`. With `otlp_http`, `on_response` additionally requires `flush.blocking_timeout_ms`. See §6. |
| `flush.timeout_ms` | `2000` | Batch interval / flush timeout. |
| `flush.max_queue_size` | `2048` | Queue bound, in **spans** — not requests. |
| `flush.blocking_timeout_ms` | — | Opt-in for request-billed scale-to-zero. Caps how long a request may block exporting its spans. 1–1000; rejected at startup outside that. |
| `file.path` | `traces.jsonl` | For `exporter: otlp_file`. Relative to the working directory. The SDK treats it as a strftime-style pattern, so `%` sequences are expanded — avoid them unless you want rotation. |
| `resource_attributes` | `{}` | e.g. `deployment.environment`. |
| `payload.max_value_bytes` | `8192` | Per-value clamp at the payload tier. |

The whole `tracing:` block is read **once, at startup**. There is no hot
reload for it — changing tracing configuration means restarting flAPI.

`max_queue_size` is counted in **spans, not requests**. With inner spans a single
request produces four to six, so 2048 is roughly 350 requests of buffer.

## 3. What is traced

One `SERVER` span per HTTP request, on **every** route — including requests
rejected in middleware (401, 403, 429), CORS preflights, static routes, the
config service, and unmatched 404s. Instrumenting only the request handler would
miss all of those, which is why the span lives in the first middleware.

Children of that span:

| Span | Kind | What it tells you |
|---|---|---|
| `flapi.render_template` | INTERNAL | Mustache rendering, template size |
| `duckdb.query` | CLIENT | DuckDB execution, rows returned, SQL verb in `db.operation.name` |
| outbound `GET`/`POST` | CLIENT | flAPI's own OIDC / JWKS calls |

Both query paths are covered: the unprepared path and the prepared path that any
endpoint with typed request fields takes. A paginated endpoint produces a
separate span for its count query, so the two costs are visible apart.

An MCP `tools/call` is **one** span carrying both the `http.*` and the
`mcp.*`/`gen_ai.*` attribute sets, named `tools/call <tool>`.

### Route labels are always bounded

`http.route` is always a template or a fixed literal, never a filled path, and
every unmatched path collapses to a single `<unmatched>` bucket. A vulnerability
scanner hitting a thousand random URLs produces **one** route label, not a
thousand — that is both a cardinality and a cost-amplification concern.

### Health probes

`/health`, `/health/live`, `/mcp/health`, `/doc` and `/doc.yaml` produce no span
by default. In Kubernetes these are the highest-volume route in the deployment
and of near-zero diagnostic value once green.

To change the set, give `exclude_routes` the **complete** list you want — your
list replaces the defaults, it does not extend them, and each entry must match a
request path exactly, trailing slash included. `/health` does not match
`/health/`.

Note that `/doc` and `/doc.yaml` are excluded from *tracing* but still produce an
audit line; the health probes produce neither.

## 4. Seeing inside a slow query

`db_profiling` attaches DuckDB's own execution metrics — the numbers behind
`EXPLAIN ANALYZE` — to the database span.

```yaml
tracing:
  db_profiling: summary     # off (default) | summary | detailed
```

| Attribute | Tier | Meaning |
|---|---|---|
| `flapi.db.latency_ms` | summary | Query execution time as DuckDB measures it |
| `flapi.db.blocked_thread_time_ms` | summary | Time blocked rather than working — contention |
| `flapi.db.result_set_bytes` | summary | Materialised result size |
| `flapi.db.bytes_read` | summary | Bytes read from storage |
| `flapi.db.cpu_time_ms` | detailed | CPU across all threads |
| `flapi.db.rows_scanned` | detailed | Rows scanned, cumulative |

DuckDB's `SYSTEM_PEAK_BUFFER_MEMORY` is deliberately **not** exported: it is a
database-wide figure, not this query's, and on a concurrent workload it would
read as if one query had used all of it.

A metric DuckDB does not report is **omitted**, never exported as `0` — a
fabricated zero reads as "instant" on every dashboard.

### What it costs

Measured on a trivial query, interleaved round-robin across the three
configurations so machine drift hits each equally (n=400 each, median of the
database span):

| | Database span, median | Δ |
|---|---|---|
| `off` | 253 µs | — |
| `summary` | 359 µs | **+106 µs** |
| `detailed` | 336 µs | +83 µs |

**The two tiers cost the same.** `detailed` measuring *lower* than `summary` is
not a real saving — it is the measurement telling you the difference is below its
own noise. Choose a tier by what you want to see, never to save time.

Nearly all of the ~100 µs is switching profiling on, not measuring: DuckDB's
profiling settings are connection-scoped (`SetLocal`, no global setter) and flAPI
opens a connection per query, so every query pays a `SET`. If connections were
pooled this would be close to free — they are not, today.

Note the overhead is per *query*, and fixed. On a request doing real work it does
not show; on a trivial one it is a third of the database time. A paginated
endpoint runs two queries and pays it twice.

It is off by default, and applies only to requests that are actually sampled — a
1% sampling ratio pays 1% of this, not all of it.

### What is never exported

DuckDB can also report `QUERY_NAME` (the SQL text) and `EXTRA_INFO` (per operator,
the rendered filter predicate — which on the prepared path carries bound parameter
values). **flAPI requests neither.** It asks DuckDB for a fixed allowlist and reads
back only those keys, rather than exporting whatever the metric map contains, so a
future DuckDB metric cannot leak through. A test asserts a bound parameter value
never appears in the exported trace with `detailed` enabled.

## 5. Data protection

| Tier | Exports | Never exports |
|---|---|---|
| `off` | nothing; no provider is constructed | — |
| **`metadata`** (default) | span structure and timings, route templates, HTTP method and status, whether a query string was present, `User-Agent` (see below), the auth kind, trace-context provenance, tool and MCP method names, the SQL verb, rows returned, response byte count, template basename and size, the count of bound parameters, enumerated error kinds | any argument value, any result row, any filled path or query string, any other header value, any credential |
| `payload` | metadata **plus** the values of **declared** request fields | credentials, headers, filled paths and query strings — excluded at **every** tier |

Four properties are enforced by tests, not asserted in prose. Each has a test
that fails if the property stops holding:

1. **A global `capture: off` beats any per-endpoint opt-in.** One lever is
   guaranteed to stop export, whatever an endpoint says.
2. **Credential-shaped keys are redacted at every tier**, regardless of your
   configuration — `Authorization`, `Cookie`, `password`, `api_key`,
   `access_token`, `client_secret`, `private_key`, `connection_string` and
   similar. This does not depend on your redact list being complete.
3. **Filled paths and query strings are never exported, at any tier.** A query
   string on a data API is by definition a filter over customer data.
4. **The payload tier genuinely captures**, which is what makes the three
   exclusions above meaningful rather than vacuous. There is a test asserting a
   declared parameter value *does* appear — without it, the exclusion tests would
   pass simply because nothing is captured.

> **One header is exported: `User-Agent`**, as `user_agent.original`, clamped to
> 256 bytes. It is caller-controlled, so treat it as untrusted text in whatever
> consumes your traces. It is the single exception to "no header values" — it
> carries no credential and is what tells you a spike came from one broken
> client. Nothing else, `Authorization` and `Cookie` included, is ever read into
> a span.
>
> With `openinference: true` the MCP **session id** is also exported, as
> `session.id`. It identifies a conversation; if that is sensitive in your
> deployment, leave the overlay off.

Only the values of **declared request fields** are captured. An arbitrary query
parameter a caller appends is not part of the endpoint's contract and is not
exported. Redaction reuses your existing **`audit.redact`** list, so you configure
it once, and redaction happens *before* clamping so a truncated value cannot leave
a partial secret behind.

```yaml
audit:
  redact: [password, tax_id, ssn]   # applied to spans as well as audit lines
```

**The two lists match differently, on purpose.**

| List | Match | Effect |
|---|---|---|
| Built-in credential stems | **Substring** of the normalised key | `api_key`, `x-api-key`, `user_api_key` and `auth_token` are all redacted. You cannot forget one. |
| Your `audit.redact` entries | **Whole** normalised key | `tax_id` redacts `tax_id` and `Tax-Id`, but **not** `customer_tax_id`. |

"Normalised" means lower-cased with `-` and `_` removed, so case and separator
style never matter for either list.

The built-in list is a substring match because it must hold regardless of your
configuration; your list is a whole-key match because you chose those names and
silently redacting everything containing them would be surprising. If you want a
prefix or suffix family redacted, list each member.

Two consequences worth knowing:

- Field names containing `token` are redacted — **except** the LLM counters
  (`max_tokens`, `input_tokens`, `output_tokens`, `token_count`, `tokens_used`),
  which are exempted by exact name because this is an MCP/LLM tool surface and
  redacting them would gut the payload tier for its main workload.
- Short stems (`pin`, `sid`, `sig`, `otp`) are deliberately *not* in the built-in
  list. As substrings they would redact `design`, `signal` and half of an
  ordinary data API. Add them to `audit.redact` if your schema needs them.

### Per-endpoint opt-in

```yaml
mcp-tool:
  name: customer_lookup
  response:
    redact-columns: [email, tax_id]
  tracing:
    capture: payload        # or `off`, to exclude a sensitive endpoint
```

At the payload tier flAPI becomes a processor exporting personal data to a third
destination, with DPA/AVV implications you need to have considered. flAPI logs a
warning at startup saying exactly that, deliberately.

## 6. Deployment

### Kubernetes / on-prem

`flush.mode: batch`, exporter pointed at a collector. Set
`service.instance.id` from the pod name via `resource_attributes`.

### Serverless (Cloud Run, App Runner)

With CPU allocated only during request processing, the instance is throttled
after the response is sent, so a background export thread may never be scheduled
and spans are lost.

There are two remedies, and they trade money against latency.

**Either allocate CPU always.** The export thread then runs normally and nothing
below applies. It costs more, and it defeats the point of scale-to-zero billing.

**Or block the request while its spans are exported:**

```yaml
tracing:
  exporter: otlp_http
  flush:
    mode: on_response
    blocking_timeout_ms: 300     # required; 1-1000
```

Without `blocking_timeout_ms` this combination keeps falling back to batch
export, with a warning. There is deliberately no default: there is no safe
universal answer to how much latency you will trade for telemetry, so you have
to say.

**What it costs.** Measured against a local collector, added to the request:

| Collector | Budget | Added latency |
|---|---|---|
| Healthy | 300 ms | **~3 ms** (p50 and p99) |
| Slow (150 ms) | 300 ms | +150 ms — only what it actually takes |
| Hanging | 300 ms | +300 ms exactly |
| Hanging | 100 ms | +100 ms exactly |

The budget bounds the request precisely, and a merely-slow collector costs only
its own latency rather than the whole budget. Measured from concurrency 1 to 40,
the added cost **does not grow with concurrency** — the flush does not serialise
across workers.

The export is one round trip per request, not one per span: the processor is
still a `BatchSpanProcessor` and the request force-flushes it once, so a
request's whole span tree leaves together, sweeping out anything buffered by
concurrent or background work at the same time.

**The failure mode to understand.** Against a *hanging* collector every worker
blocks for the full budget, so throughput degrades to roughly
`workers / blocking_timeout_ms` — about 40 req/s at 8 workers and 300 ms. Watch
`spans_flush_timeouts` in [§9](#9-checking-that-export-is-actually-working): a
rising value means the collector is costing your callers latency *and* still
losing spans, which is the worst of both and a reason to stop blocking.

This is for request-billed scale-to-zero specifically. On a long-lived
deployment, leave it off — you would be coupling your p99 to a third party's
availability for no benefit, since the background export thread runs fine there.

`SIGTERM` force-flushes on shutdown, with a fixed 2 s budget (not
`flush.timeout_ms`).

### Air-gapped — the zero-egress guarantee

```yaml
tracing:
  exporter: otlp_file
  file:
    path: /var/lib/flapi/traces.jsonl
```

With `exporter: otlp_file`, and the PostHog telemetry disabled per
[TELEMETRY.md](../TELEMETRY.md), **flAPI opens no outbound network connection for
observability.** (Authentication is separate: if you configure OIDC, JWKS
fetches still dial out.) Log rotation is your existing tooling's job; flAPI does not
implement it.

Note that the file exporter's own default is to buffer for 30 seconds. flAPI
overrides this to flush per record under `flush.mode: on_response`, and on
`flush.timeout_ms` otherwise, so a crash does not silently cost you the last
half-minute of spans.

## 7. Getting an id out of a failed request

Every response carries `X-Request-Id`. A traced response also carries
`X-Trace-Id`, and it is the **caller's** trace id when the caller supplied one,
so both sides can join on the same value.

```
$ curl -i https://flapi.example/customers/42
HTTP/1.1 500 Internal Server Error
X-Request-Id: req-9f2c1a7b8e4d5063
X-Trace-Id:   4bf92f3577b34da6a3ce929d0e0e4736
```

Quote either in a support request. `X-Request-Id` works with tracing disabled;
`X-Trace-Id` appears only when a span was produced.

## 8. Correlation

`trace_id` and `span_id` appear in the audit log and in application log lines
emitted while serving a request, so a trace can be joined to an audit entry and
to the log. With `log-format: json` the log is directly ingestible.

```yaml
log-level: info
log-format: json     # request_id / trace_id / span_id on every line
audit:
  enabled: true
  sink: file
  path: /var/log/flapi/audit.jsonl
```

## 9. Checking that export is actually working

`GET /api/v1/_config/metrics` reports whether spans are reaching the collector.
It requires the config-service token.

```bash
curl -s -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" \
     http://localhost:8080/api/v1/_config/metrics
```

```json
{
  "tracing": { "enabled": true, "spans_submitted": 10482, "spans_exported": 10482,
               "spans_dropped": 0, "spans_flush_timeouts": 0 },
  "arrow":   { "total_requests": 12, "successful_requests": 12, "failed_requests": 0,
               "total_rows": 48210, "active_streams": 0 },
  "endpoints": { "count": 18 }
}
```

`spans_submitted` counts spans handed to the processor. `spans_submitted -
spans_exported` is what is **queued or lost to a full queue** — it does *not*
reveal HTTP failures, because `spans_exported` derives from the same result the
OTLP/HTTP exporter hardcodes to success (below).

`spans_dropped` counts export batches the SDK reported as failed — which, with
`otlp_http`, is fewer than you would expect. `OtlpHttpExporter::Export` computes
the real result, logs the failure, then returns success anyway
(`otlp_http_exporter.cc:193`, `:206`), so a collector answering 503 to every
batch looks identical to a healthy one from **every** counter's point of view.
We verified that: against a collector rejecting everything, the counters read
`submitted: 38, exported: 38, dropped: 0` while zero spans arrived and the SDK
logged `Export 18 trace span(s) error: 1`.

**For `otlp_http`, the flAPI log is the only reliable signal of export failure.**
Alert on `[OTLP TRACE HTTP Exporter] ERROR`. The counters will not tell you.

:::caution The most common cause of "no spans arrive"
`endpoint` must be the **full trace URL**. `http://localhost:4318` posts to `/`,
which a real collector 404s — and because the exporter discards its own result,
every counter still reads healthy. Use `http://localhost:4318/v1/traces`.
:::

> **A flat `spans_dropped` does not prove nothing was lost.** Spans discarded
> because the batch queue was already full are dropped *before* export is
> attempted, and are not counted here. If `spans_exported` is lower than your
> request rate implies while `spans_dropped` stays at zero, suspect
> `flush.max_queue_size` rather than the collector.

`spans_exported` flat at zero with `enabled: true` usually means the exporter is
misconfigured — though `exporter: none` also reports `enabled: true` and exports
nothing, by design. Both counters are cumulative since startup.

See [CONFIG_SERVICE_API_REFERENCE.md](./CONFIG_SERVICE_API_REFERENCE.md) for the
full route reference.

## 10. Cost

Measured on the reference load mix (see `test/load/README.md`); the recorded
runs are in `test/load/baselines/`. Re-measure on your own hardware before
relying on these:

| Configuration | Cost |
|---|---|
| Tracing disabled | No measurable difference. No provider, no exporter thread. |
| Tracing at `metadata` | ~16 µs per request. Unmeasurable on a realistic query. |
| Binary size | +4.9 MiB against a `FLAPI_WITH_TRACING=OFF` build. |

A tracing-free build is supported: `cmake -DFLAPI_WITH_TRACING=OFF`. The facade
compiles to no-ops and no OpenTelemetry symbol is linked.

## 11. Environment variables

Configuration is YAML-first, but the OTLP/HTTP exporter is constructed from
opentelemetry-cpp's own defaults, which read the environment. flAPI then
overrides only what your YAML sets.

| Variable | Effect |
|---|---|
| `OTEL_SDK_DISABLED` | Exactly `true` disables tracing even with `tracing.enabled: true`. Always wins. Other values, including `TRUE` and `1`, are ignored. |
| `OTEL_EXPORTER_OTLP_ENDPOINT`, `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | Used unless YAML sets `endpoint`. |
| `OTEL_EXPORTER_OTLP_HEADERS`, `..._TRACES_HEADERS` | Merged in. A header you also set in YAML is **replaced** by the YAML value, not sent twice. |
| `OTEL_EXPORTER_OTLP_PROTOCOL`, `..._TRACES_PROTOCOL` | Read by the SDK. flAPI overrides it only when YAML sets `protocol: http/json` — so an env protocol otherwise wins. |
| `OTEL_EXPORTER_OTLP_CERTIFICATE`, `..._CLIENT_KEY`, `..._CLIENT_CERTIFICATE`, `..._COMPRESSION`, and the SDK's retry knobs | Honoured by the SDK; flAPI does not touch them. |

**These have no effect:** `OTEL_SERVICE_NAME`, `OTEL_RESOURCE_ATTRIBUTES`,
`OTEL_TRACES_SAMPLER` and `OTEL_TRACES_SAMPLER_ARG`, `OTEL_TRACES_EXPORTER`, the
`OTEL_BSP_*` batch knobs, and `OTEL_EXPORTER_OTLP_TIMEOUT` (`timeout_ms` always
overwrites it). Service name, resource attributes, sampler, exporter selection
and flush behaviour come from the `tracing:` block only. If you rely on
Kubernetes OTel Operator injection, set those in YAML explicitly rather than
assuming the injected environment is read.

**One deliberate divergence:** an `OTEL_EXPORTER_OTLP_ENDPOINT` in the
environment does **not** by itself enable tracing. A platform-wide environment
variable is not an operator's consent to ship data off the machine, so
`tracing.enabled` is still required.

---

## How it works

The implementation deep dives live in `docs/spec/`:

- [spec/components/observability.md](./spec/components/observability.md) — the span
  tree, `RequestContext` lifetime, the opaque `SpanScope` handle, capture-tier
  resolution and exporter mechanics.
- [spec/REQUEST_LIFECYCLE.md](./spec/REQUEST_LIFECYCLE.md) — where the server span
  and the request id are created in the middleware chain, and why that is the
  first middleware rather than the request handler.
- [spec/DESIGN_DECISIONS.md](./spec/DESIGN_DECISIONS.md#10-opentelemetry-observability)
  — why the OpenTelemetry SDK, why no OpenTelemetry type appears in any header,
  and why the `tracing:` block is boot-only.
- [spec/components/security.md](./spec/components/security.md) — the telemetry
  egress layer: redaction, clamping and the no-leak invariants.
