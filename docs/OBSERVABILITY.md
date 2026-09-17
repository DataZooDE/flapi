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
  endpoint: http://localhost:4318
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
| `endpoint` | SDK default | OTLP/HTTP base URL. Applies to `otlp_http` only. |
| `protocol` | unset | Only the exact string `http/json` changes anything (it switches the content type). Any other value, including `http/protobuf`, is ignored. |
| `headers` | `{}` | Extra headers, e.g. auth for a SaaS backend. Applies to `otlp_http` only. |
| `timeout_ms` | `10000` | Export timeout. |
| `capture` | `metadata` | See §4. |
| `openinference` | `false` | OpenInference attribute overlay. |
| `exclude_routes` | probes + docs | Routes that produce no span. An explicit list **replaces** the defaults rather than adding to them, and each entry is matched as an exact path, not a prefix or glob. |
| `sample.type` | `parentbased_traceidratio` | `always_on`, `always_off`, `parentbased_traceidratio`. |
| `sample.ratio` | `1.0` | Sampling ratio. |
| `flush.mode` | `batch` | `batch` or `on_response`. `on_response` requires `exporter: otlp_file`. See §5. |
| `flush.timeout_ms` | `2000` | Batch interval / flush timeout. |
| `flush.max_queue_size` | `2048` | Queue bound, in **spans** — not requests. |
| `file.path` | `traces.jsonl` | For `exporter: otlp_file`. Relative to the working directory. |
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

> **Known gap.** The DuckDB span is currently produced only on the unprepared
> query path. Any endpoint with typed request fields runs through the prepared
> path (`QueryExecutor::executeWithBindings` → `executePrepared`), which emits no
> span today, so most endpoints show template rendering and the server span but
> no database child. Tracked as a follow-up; the timing is still contained in the
> parent span's duration.

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
request path exactly.

Note that `/doc` and `/doc.yaml` are excluded from *tracing* but still produce an
audit line; the health probes produce neither.

## 4. Data protection

| Tier | Exports | Never exports |
|---|---|---|
| `off` | nothing; no provider is constructed | — |
| **`metadata`** (default) | span structure and timings, route templates, HTTP method and status, tool and MCP method names, the SQL verb, rows returned, response byte count, template basename and size, the count of bound parameters, enumerated error kinds | any argument value, any result row, any filled path or query string, any header value, any credential |
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

Only the values of **declared request fields** are captured. An arbitrary query
parameter a caller appends is not part of the endpoint's contract and is not
exported. Redaction reuses your existing **`audit.redact`** list, so you configure
it once, and redaction happens *before* clamping so a truncated value cannot leave
a partial secret behind.

```yaml
audit:
  redact: [password, tax_id, ssn]   # applied to spans as well as audit lines
```

**Match your redact entries exactly.** Both the built-in credential list and your
`audit.redact` entries are compared as whole, lower-cased key names — not as
substrings. `api_key` is redacted; `x-api-key` and `user_api_key` are *not*,
unless you list them too.

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

## 5. Deployment

### Kubernetes / on-prem

`flush.mode: batch`, exporter pointed at a collector. Set
`service.instance.id` from the pod name via `resource_attributes`.

### Serverless (Cloud Run, App Runner)

With CPU allocated only during request processing, the instance is throttled
after the response is sent, so a background export thread may never be scheduled
and spans are lost.

**Allocate CPU always.** That is the only remedy that works with an OTLP/HTTP
collector. `flush.mode: on_response` is *not* an alternative here: it is honoured
only with `exporter: otlp_file`, and with `otlp_http` flAPI logs a warning and
falls back to batch export. Flushing a network export on the request thread would
couple your p99 to the collector's availability, which is why it is refused
rather than supported.

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
observability.** Log rotation is your existing tooling's job; flAPI does not
implement it.

Note that the file exporter's own default is to buffer for 30 seconds. flAPI
overrides this to flush per record under `flush.mode: on_response`, and on
`flush.timeout_ms` otherwise, so a crash does not silently cost you the last
half-minute of spans.

## 6. Getting an id out of a failed request

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

## 7. Correlation

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

## 8. Checking that export is actually working

`GET /api/v1/_config/metrics` reports whether spans are reaching the collector.
It requires the config-service token.

```bash
curl -s -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" \
     http://localhost:8080/api/v1/_config/metrics
```

```json
{
  "tracing": { "enabled": true, "spans_exported": 10482, "spans_dropped": 0 },
  "arrow":   { "total_requests": 12, "successful_requests": 12, "failed_requests": 0,
               "total_rows": 48210, "active_streams": 0 },
  "endpoints": { "count": 18 }
}
```

`spans_dropped` rising means the export queue is saturating — the collector is
slow, unreachable, or `flush.max_queue_size` is too small for your span rate.
`spans_exported` flat at zero with `enabled: true` means the exporter is
misconfigured. Both counters are cumulative since startup.

See [CONFIG_SERVICE_API_REFERENCE.md](./CONFIG_SERVICE_API_REFERENCE.md) for the
full route reference.

## 9. Cost

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

## 10. Environment variables

Configuration is YAML-first. Only two environment paths affect tracing:

| Variable | Effect |
|---|---|
| `OTEL_SDK_DISABLED` | Set to the exact string `true`, disables tracing even when `tracing.enabled: true`. Always wins. Other values, including `TRUE` and `1`, are ignored. |
| `OTEL_EXPORTER_OTLP_ENDPOINT`, `OTEL_EXPORTER_OTLP_HEADERS` | Used as the OTLP/HTTP exporter's defaults **only when** the corresponding `endpoint` / `headers` keys are absent from YAML. Setting them in YAML wins. |

**Everything else is YAML-only.** `OTEL_SERVICE_NAME`, `OTEL_RESOURCE_ATTRIBUTES`,
`OTEL_TRACES_SAMPLER`, `OTEL_TRACES_EXPORTER`, `OTEL_EXPORTER_OTLP_TIMEOUT` and the
`OTEL_BSP_*` batch knobs have **no effect** — sampler, resource attributes,
service name, exporter selection and flush behaviour are built from the `tracing:`
block, and `timeout_ms` always overwrites the exporter timeout. If you rely on
Kubernetes OTel Operator injection, set the equivalents in `tracing:` explicitly;
do not assume the injected environment is being read.

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
