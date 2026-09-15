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
| `exporter` | `otlp_http` | `otlp_http`, `otlp_file`, or `none`. |
| `endpoint` | SDK default | OTLP/HTTP base URL. |
| `protocol` | `http/protobuf` | `http/protobuf` or `http/json`. |
| `headers` | `{}` | Extra headers, e.g. auth for a SaaS backend. |
| `timeout_ms` | `10000` | Export timeout. |
| `capture` | `metadata` | See §4. |
| `openinference` | `false` | OpenInference attribute overlay. |
| `metrics` | `true` | Duration histograms. |
| `client_spans` | `true` | Spans for flAPI's own outbound OIDC/JWKS calls. |
| `exclude_routes` | probes + docs | Routes that produce no span. |
| `sample.type` | `parentbased_traceidratio` | `always_on`, `always_off`, `parentbased_traceidratio`. |
| `sample.ratio` | `1.0` | Sampling ratio. |
| `flush.mode` | `batch` | `batch` or `on_response`. See §5. |
| `flush.timeout_ms` | `2000` | Batch interval / flush timeout. |
| `flush.max_queue_size` | `2048` | Queue bound, in **spans** — not requests. |
| `file.path` | — | For `exporter: otlp_file`. |
| `resource_attributes` | `{}` | e.g. `deployment.environment`. |
| `payload.max_value_bytes` | `8192` | Per-value clamp at the payload tier. |
| `payload.max_documents` | `50` | Document cap at the payload tier. |

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
| `SELECT` / `INSERT` / … | CLIENT | DuckDB execution, rows returned |
| outbound `GET`/`POST` | CLIENT | flAPI's own OIDC / JWKS calls |

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
and of near-zero diagnostic value once green. Override with `exclude_routes`.

## 4. Data protection

| Tier | Exports | Never exports |
|---|---|---|
| `off` | nothing; no provider is constructed | — |
| **`metadata`** (default) | span structure, timings, route templates, tool names, parameter **names** and types, row and column counts, byte counts, cache-backing, enumerated error kinds | any argument value, any result row, any filled path or query string, any header, any credential |
| `payload` | *not implemented yet* — see below | credentials and headers — excluded at **every** tier |

**What is true today:** the payload tier is **not implemented**. flAPI captures
no argument values and no result rows at any setting. Configuring
`capture: payload` logs a warning at startup and behaves as `metadata`; it does
not silently start exporting data, and it does not silently do nothing either.

So the guarantee currently in force is stronger than redaction: **no values are
captured at all.** That IS enforced end to end — an integration test plants
sentinels in a query string, a path segment, an `Authorization` header and a
request body and asserts none appears anywhere in the raw exported bytes,
including span names and status messages.

The redaction machinery below is built and unit-tested, but is not yet on any
value path because there are no values to redact. It will be wired up with the
payload tier.

### Planned, not yet available

```yaml
# NOT YET IMPLEMENTED - accepted and warned about, behaves as metadata
mcp-tool:
  name: customer_lookup
  response:
    redact_columns: [email, tax_id]   # existing; will be reused by tracing
  tracing:
    capture: payload
```

When the payload tier lands, flAPI becomes a processor exporting personal data to
a third destination, with DPA/AVV implications you need to have considered. The
design that will govern it: a global `capture: off` beats any per-endpoint
opt-in; redaction reuses your existing `audit.redact_keys` so it is configured
once; and a fixed set of credential-shaped keys (`Authorization`, `Cookie`,
`password`, `api_key`, `access_token`, `client_secret`, `private_key`,
`connection_string`) is redacted regardless of configuration.

## 5. Deployment

### Kubernetes / on-prem

`flush.mode: batch`, exporter pointed at a collector. Set
`service.instance.id` from the pod name via `resource_attributes`.

### Serverless (Cloud Run, App Runner)

With CPU allocated only during request processing, the instance is throttled
after the response is sent, so a background export thread may never be scheduled
and spans are lost. Either allocate CPU always, or use `flush.mode: on_response`.
`SIGTERM` force-flushes in both cases.

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

## 6. Correlation

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

## 7. Cost

Measured on the reference load mix (see `test/load/README.md`):

| Configuration | Cost |
|---|---|
| Tracing disabled | No measurable difference. No provider, no exporter thread. |
| Tracing at `metadata` | ~16 µs per request. Unmeasurable on a realistic query. |
| Binary size | +4.9 MiB against a `FLAPI_WITH_TRACING=OFF` build. |

A tracing-free build is supported: `cmake -DFLAPI_WITH_TRACING=OFF`. The facade
compiles to no-ops and no OpenTelemetry symbol is linked.

## 8. Environment variables

flAPI honours the standard `OTEL_*` variables, so Kubernetes OTel Operator
injection works. Precedence is **explicit flAPI YAML > `OTEL_*` > defaults**.

**One deliberate divergence:** an `OTEL_EXPORTER_OTLP_ENDPOINT` in the
environment does **not** by itself enable tracing. A platform-wide environment
variable is not an operator's consent to ship data off the machine, so
`tracing.enabled` is still required. `OTEL_SDK_DISABLED=true` always wins.
