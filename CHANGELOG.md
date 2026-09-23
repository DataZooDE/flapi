# Changelog

All notable changes to flAPI are documented here. Versions follow `vYY.MM.DD` (the date the binary set was cut). Earlier history is in the git log.

## v26.09.23 — a slow query no longer blocks everything else, and `auth.*` works over MCP

### Added: a slow query no longer blocks other requests

Crow runs a request handler on the io thread that owns its connection, so one slow query held
that thread for the whole query and every other connection assigned to it waited — including
`GET /health/live`, which is how a stalled instance is supposed to report itself.

Measured on one ~6.5s query with 40 concurrent probes and a single io thread:

| | probes blocked | stall reported | worst probe |
|---|---|---|---|
| before | 40 / 40 | 0 / 40 | 5.04s |
| after | 0 / 40 | 40 / 40 | 0.005s |

Handlers now run on a bounded worker pool and the response is completed back on the connection's
own thread. The queue is bounded on purpose: when it is full the request is answered `503` rather
than queued behind work the client stopped waiting for. Set `FLAPI_DISABLE_HANDLER_OFFLOAD=1` to
go back to the old behaviour.

`SIGTERM` is now handled properly alongside it: in-flight requests are drained before the process
exits, under a bounded budget so one slow query cannot hold a container open until it is killed.

### Fixed: `auth.*` was empty in every MCP template

The documented multi-tenant pattern

```sql
{{#auth.username}}WHERE tenant = '{{ auth.username }}'{{/auth.username}}
```

worked over REST and rendered **nothing** over MCP, because the MCP path never injected the
authenticated caller's identity. In that conditional form the filter disappeared entirely and an
authenticated caller received **every tenant's rows**; written unconditionally it compared against
`''` and returned none. Both were silent.

`auth.username`, `auth.roles`, `auth.type`, `auth.email` and `auth.authenticated` now render the
same on REST, MCP `tools/call` and MCP `resources/read`. A caller still cannot declare its own
identity — `__auth_*` sent as an argument is discarded on every surface.

**If you have an MCP endpoint whose template filters on `auth.*`, its results will change.** That
is the point: it was returning too much or too little before.

### Fixed: cache retention destroyed other endpoints' history

Every cached endpoint shares one DuckLake catalog, but `keep-last-snapshots` and
`max-snapshot-age` are configured per endpoint — and the expiry ran catalog-wide. One endpoint
reaching its retention limit expired **every other endpoint's snapshots**, discarding their
time-travel history and resetting their incremental watermark. `flapii cache gc` did the same, and
against a hardcoded one-day cutoff that ignored your configuration entirely.

Retention now only expires snapshots belonging to its own cache table, and never one that another
table shares — so an endpoint may keep more history than you asked for, and will say so in the
log, rather than deleting a neighbour's data. It also always keeps the newest snapshot, which is
where the incremental watermark is read from, and applies `keep-last-snapshots` across the table's
whole history rather than across the already-expired subset when both keys are set.

Retention is disabled, with a warning, for a cache table whose name is ambiguous — two schemas
holding the same table name — because DuckLake's catalog exposes no schema name to tell them
apart and expiring the wrong one would delete another endpoint's data.

### Fixed: incremental refresh dropped rows written while it ran

`{{cache.previousSnapshotTimestamp}}` was the instant the previous refresh *committed*. That
refresh read your source earlier than that, so any row written in between was never read by it —
and was then excluded by the next refresh too. On a continuously written source, **every refresh
permanently dropped the rows written during its own execution**, and reported success.

With a `cursor:` configured, the watermark is now `max(<cursor column>)` over the rows actually
cached. Rows above it are exactly the rows not yet loaded.

Two consequences worth knowing:

- The value is the cursor's own type, not a timestamp. `WHERE updated_at > TIMESTAMP '{{...}}'`
  stays right for a timestamp cursor; an integer or string cursor needs quoting to match.
- `{{cache.snapshotTimestamp}}` is a *different* value (still the commit time) and must not be
  used as a watermark.

### Fixed: credentials came back from MCP dry runs and errors

A `_dryRun` tool call returns the rendered SQL, and MCP is unauthenticated by default. Connection
passwords, whitelisted environment variables (`{{{ env.API_KEY }}}`) and credential-valued request
defaults were all returned verbatim — and so were the ones a database error quoted back, which is
a path no `_dryRun` flag is needed to reach.

All three sources are redacted now, in previews and in error messages, on **every** surface that
returns one: REST responses, MCP `tools/call` and MCP `resources/read` all scrub the same set.
(REST was leaking the same way, through the same mechanism, for any endpoint without an `auth:`
block.) Where a value is too short to replace without corrupting the surrounding SQL, the preview
is withheld and says so rather than being silently mangled.

### Fixed: MCP config tools that reported success without doing anything

Eleven `flapi_*` config tools returned confident results for work they never performed:

| Tool | What it returned |
|---|---|
| `flapi_expand_template` | a hardcoded `SELECT * FROM data WHERE 1=1` |
| `flapi_test_template` | "Template test passed", for a query it never ran |
| `flapi_update_template` | success, with the file on disk untouched |
| `flapi_refresh_cache` | "Cache refresh has been scheduled" — nothing was scheduled |
| `flapi_run_cache_gc` | "Garbage collection triggered" |
| `flapi_refresh_schema` | "schema_refreshed" |
| `flapi_get_cache_audit` | **invented audit records** |
| `flapi_get_cache_status` | three fields of static YAML, while promising snapshot history |
| `flapi_get_environment` | an empty list, always |
| `flapi_get_filesystem` | an empty tree |
| `flapi_get_schema` | `"tables": null` |

Every one of them had a working REST handler that the adapter was constructing and then
discarding. They now call it, so the MCP and REST surfaces cannot disagree — an agent asking
whether a cache refreshed gets the real answer, and one asking to expand a template gets that
endpoint's own SQL.

`flapi_get_project_config` also reported version `1.0.0` for every build; it now returns the real
project configuration.

**Every `flapi_*` tool now requires the config-service token**, matching the REST route it
delegates to. Twelve of them were classified "read-only discovery" and left unauthenticated, which
was harmless only while their bodies returned hardcoded data — once they were wired to the real
handlers, three of them became unauthenticated disclosures: `flapi_get_environment` returns
environment *values*, `flapi_expand_template` returns rendered SQL, and `flapi_test_template`
*executes* any endpoint's template. If you call these tools, send
`Authorization: Bearer <config-service-token>`; with no config service configured they now decline,
which is correct.

`flapi_test_template` also runs the endpoint's own validators before executing, so it is no longer
a way around the constraints the endpoint itself enforces. `flapi_run_cache_gc` requires `path` —
its "all endpoints" form never worked — and every tool now declares its arguments in `tools/list`,
so an agent can discover them instead of guessing and getting an error.

### Changed: one config-service name per endpoint

The config service addresses an endpoint by a slug, and the encoding is now injective — `/a-b` and
`/a/b` no longer collide. `/customers/` is `-customers-`; a literal `-` is `~1` and a literal `~`
is `~0`.

**The CLI and the VSCode extension must be updated together with the server.** An older `flapii`
sends the previous encoding and every config command will `404`. A percent-encoded url-path is no
longer accepted as a second name for the same endpoint.

## v26.09.20 — SQLite attachments no longer wedge, and readiness notices a stalled request

### Fixed: a concurrent read and write wedged a SQLite attachment

A `GET` issued at the same moment as a `POST` against a DuckDB SQLite attachment deadlocked the
two against each other. **Two requests were enough**, and it reproduced every time. The
attachment could then stop answering for good while the process, DuckDB, `/health/live` and every
other connection stayed perfectly healthy — so the instance kept accepting traffic it could no
longer serve, with nothing in the logs to say why.

Serialising only the writes does not help: the pair that wedges is a reader and a writer. flAPI
now runs **one query at a time** on any connection whose `init` contains an
`ATTACH ... (TYPE sqlite)`. Nothing else is affected — parquet, BigQuery, Postgres and DuckLake
traffic still runs fully in parallel, because the lock is per connection and only taken for
connections that need it.

Measured: 19 of 20 concurrent read+write pairs failed before, 20 of 20 pass now.

The cost is real — concurrent requests to a SQLite-backed endpoint queue rather than overlap — so
you can override the detection in either direction:

```yaml
connections:
  my-backend:
    serialize-access: true      # an unrecognised backend with the same problem

  read-only-snapshot:
    serialize-access: false     # a SQLite attachment you have measured and want parallel
```

### Added: readiness fails while a request is stalled

```yaml
stall-timeout-s: 60   # 0 disables
```

`GET /health` returns `503 {"status":"stalled"}` once the oldest in-flight request outlives the
budget, with `requests.in_flight` and `requests.oldest_ms` in the body. This measures the symptom
rather than probing each backend — a probe against a wedged connection blocks too, leaking a
thread per health check — so it catches any stall, whatever caused it.

The default is deliberately high: flAPI's answer to a genuinely long query is the MCP Tasks
extension, so a *synchronous* request still running after a minute is pathological rather than
slow. **Liveness deliberately still passes** — the process is fine, and killing it rather than
draining it turns a stall into an outage.

### Known limitation

A handler runs on the Crow io thread that owns its connection, so a slow synchronous query blocks
other connections landing on that thread — `GET /health` included. Readiness can therefore be
blocked by the very stall it reports: with one ~5 s query in flight, 39 of 40 health requests
answered `503 stalled` in under a millisecond and one waited out the query. A hung probe is still
treated as a failed probe by Kubernetes and Cloud Run, so the instance does leave rotation — but
by timeout rather than by the documented response. Tracked in
[#120](https://github.com/DataZooDE/flapi/issues/120).

## v26.09.19 — Blocking span export for scale-to-zero, and a documentation correction

### Fixed: `tracing.endpoint` must be the full trace URL

The examples published with v26.09.18 used `endpoint: http://localhost:4318`. **That does not
work.** opentelemetry-cpp uses an overridden URL verbatim and does not append `/v1/traces`, so
flAPI posted to `/` and any real collector rejected it — silently, because
`OtlpHttpExporter::Export` discards its own result and reports success regardless. Anyone who
copied the documented configuration got no traces and no signal that anything was wrong.

Use `endpoint: http://localhost:4318/v1/traces`.

### Added: opt-in blocking span export

For request-billed, scale-to-zero platforms (Cloud Run, App Runner), where CPU is throttled the
moment the response is sent and the background export thread may never be scheduled:

```yaml
tracing:
  flush:
    mode: on_response
    blocking_timeout_ms: 300     # required for otlp_http; 1-1000
```

Off unless you set the budget — there is no safe default for how much latency you will trade for
telemetry. Adds ~3 ms against a healthy collector, is bounded exactly at the budget against a
hanging one, and does not grow with concurrency. Against a hanging collector throughput degrades
to roughly `workers / blocking_timeout_ms`.

### Added

- **`spans_submitted`** in `GET /api/v1/_config/metrics`. `spans_dropped` cannot detect a
  rejecting collector — the SDK drops silently on a full queue and reports export failures as
  success — so `submitted - exported` is the honest view of queue loss. For `otlp_http`, the flAPI
  log (`[OTLP TRACE HTTP Exporter] ERROR`) remains the only reliable failure signal.

### Fixed

- **MCP spans reported `http.route: <unmatched>`**, putting every agent call in the same bucket as
  a vulnerability scanner's 404s. Now `/mcp/jsonrpc`.
- **The `SIGTERM` force-flush budget was hardcoded at 2 s**, so tuning `flush.timeout_ms` had no
  effect on the path that matters most under a short shutdown grace. Now configurable and
  range-checked.
- **SQLite lock contention** (partial fix for #116): lock errors are retried with bounded backoff
  on both read and write paths, and exhausted retries return `503` with `Retry-After` rather than
  `500` — contention is not a fault in the request. The rollback path no longer runs `ROLLBACK`
  when no transaction is active, which had compounded every real error with a spurious one.

### Known limitation

Concurrent mixed read/write traffic against a **SQLite-backed** endpoint can still wedge that
attachment permanently, while `/health/live` continues to return 200 and other connections keep
serving. A restart clears it and no data is lost. Root-caused and tracked in
[#116](https://github.com/DataZooDE/flapi/issues/116).

## v26.09.18 — OpenTelemetry observability, and health checks during cache warmup

### Observability and tracing

flAPI now emits OpenTelemetry traces for every request, correlated with the audit log and the
application log through one shared request identity. **Off by default**, and when enabled it
exports no customer data unless an endpoint is explicitly opted in. See
[docs/OBSERVABILITY.md](docs/OBSERVABILITY.md).

- **MCP trace context (SEP-414).** flAPI now honours the unprefixed `traceparent`, `tracestate` and
  `baggage` keys inside `params._meta`, so a tool call joins the agent's existing trace instead of
  starting a disconnected one. `_meta` wins over the HTTP header: behind a gateway the HTTP hop
  carries the gateway's span, not the agent's. Parsed even when tracing is disabled, so the ids
  still reach the audit and application logs. This closes a conformance gap — flAPI advertised MCP
  `2026-07-28` while discarding the trace context conforming clients already sent.
- **One SERVER span per request on every route**, created in the first middleware. A
  handler-level span would miss every 401, 403, 429, CORS preflight and 404 — the requests most
  likely to be complained about.
- **Inner spans** for template rendering, DuckDB execution (both the plain and prepared paths) and
  flAPI's own outbound OIDC/JWKS calls.
- **`X-Request-Id` on every response**, always server-minted; an inbound one is never honoured.
  The same id appears in the audit log and in every application log line for that request, with
  `trace_id`/`span_id` alongside when tracing is on. `X-Trace-Id` is returned when a span exists.
- **REST audit coverage.** The audit log previously covered MCP only, while the documentation
  claimed otherwise. It now covers every request on every route, including ones rejected before the
  handler; denials are never suppressed.
- **`server.log_level` is now read** from YAML, with precedence CLI > env > YAML > default. It had
  appeared in three example configs without being honoured.
- **`GET /api/v1/_config/metrics`** implemented, bearer-gated. It had been in the OpenAPI document
  with no route behind it.
- **Capture tiers** (`off` / `metadata` / `payload`, default `metadata`), with a global `off`
  beating any per-endpoint opt-in. Filled paths, query strings, header values and credentials are
  never exported at any tier, and error status is an enumerated `error.type` rather than an
  exception message. Redaction is shared between the audit log and the span path so the two cannot
  drift; built-in credential stems match as substrings, the operator's `audit.redact` list matches
  whole keys.
- **DuckDB execution profiling** (`tracing.db_profiling: off | summary | detailed`) attaches the
  metrics behind `EXPLAIN ANALYZE` to the database span, read through DuckDB's C API. A fixed
  allowlist: `QUERY_NAME` is the SQL text and `EXTRA_INFO` is the rendered filter predicate, so
  neither is ever requested. Gated on the span actually recording, so a 1% sampling ratio pays 1%
  of the cost.
- **`FLAPI_WITH_TRACING=OFF`** builds no-op twins and links no OpenTelemetry symbol. Request ids,
  log correlation, REST audit coverage and the metrics route all still work — they are not tracing
  features. A mixed-macro build fails to link rather than corrupting silently.

### Correctness fixes made along the way

- **Copy-on-write endpoint table.** `getEndpointForPathAndMethod` returned a raw pointer into a
  `std::vector` that config reload and the config-service routes cleared and re-`push_back`-ed with
  no lock against in-flight requests — a use-after-free. Reads now pin an immutable snapshot
  through `EndpointRef`.
- **Route regexes are compiled once** at config load instead of on every call.
- **One `FlapiApp` alias** for the Crow middleware tuple, enforced by CI. A second literal
  `crow::App<...>` spelling silently creates a second, unconfigured middleware tuple.

### Build

- `opentelemetry-cpp` 1.24.0 via an in-repo vcpkg overlay port. The pinned baseline's 1.17.0 does
  not compile — 136 of its headers use fixed-width integer types without including `<cstdint>`.
- **The project now builds at C++20**, because abseil (via opentelemetry-cpp) exports a
  `cxx_std_20` requirement. The bundled DuckDB is deliberately built at C++17 inside the same
  binary: C++20 removed `std::uncaught_exception()`, which DuckDB still calls behind a
  `__cplusplus` guard that MSVC defeats. CMake saves and restores `CMAKE_CXX_STANDARD` around it.
- `opentelemetry-cpp` links `PRIVATE`, and no header under `src/include/` includes an
  OpenTelemetry header, so protobuf and abseil stay out of ~80 translation units.

### Documentation

- Restructured around tasks: a new index at [docs/README.md](docs/README.md), eight guides under
  `docs/guides/`, and the `docs/spec/` tree brought back in line with the code.
- Roughly 15,000 lines of unlinked material that contradicted the live reference were removed;
  completed plans and shipped design notes moved to `docs/archive/`.

### Health checks during cache warmup

- The HTTP listener now opens before cache warmup completes, so platforms can connect during long
  startup cache builds.
- Added always-on `GET /health/live` for liveness and `GET /health` for readiness. `/health`
  returns `503` while caches are still starting or degraded.
- Cache-enabled data endpoints now return `503` with `Retry-After: 5` while their cache is starting
  or failed, instead of risking a `200` response from a half-built cache.
- Cache warmup failures are captured per endpoint and surfaced in health output; warmup continues
  to later caches.
- Concurrent refreshes for the same DuckLake cache table are suppressed so scheduler refreshes do
  not collide with startup warmup.

### Build correctness

- `CROW_ENABLE_COMPRESSION` is now defined once for every translation unit via CMake instead of by
  three headers. The macro adds a member to `crow::response`, so a translation unit that reached
  `<crow.h>` through a different include order saw a different layout — an ODR violation that made
  `response::is_completed()` read an unrelated byte across the library/test boundary.
- The unit tests that use the `#define private public` access hack now include `<crow.h>` before it,
  so crow is never parsed with rewritten access specifiers.

## v26.08.31 — MCP 2026-07-28 dual-era support and the Tasks extension

- **MCP revision `2026-07-28`, served dual-era.** The modern stateless path and the legacy
  `initialize`/session path are served from the same endpoint, chosen per request by whether the
  client sent the modern `_meta` block. Existing clients keep working unchanged; newer ones get the
  stateless path automatically. flAPI had never really trusted the session — every `tools/call`
  already re-authenticated from the HTTP request — so dropping it lost nothing and made the server
  genuinely stateless behind a load balancer.
- **Tasks extension** for long-running analytical queries: a query that would outlive a proxy or
  client timeout becomes a durable, pollable task rather than a dropped connection.
- Typed tool schemas and structured results, OAuth discovery, per-tool RBAC, shadow/dry-run,
  response shaping, per-tool rate limiting, and a prompt-injection hygiene scanner.
- Cursor pagination, parameterised resource templates (`flapi://customers/{id}`), and
  `x-mcp-header` for per-tenant edge routing.
- Closed a gap where method authorization could be skipped by omitting the session header.
- **DuckDB bumped to v1.5.5.**

## v26.08.07 — Feedback banner and issue links on errors

- An interactive start prints a small banner once a day pointing at the issue tracker; under a
  container or systemd there is no terminal, so it never prints and the startup log line carries
  the pointer instead. Silence it with `DATAZOO_NO_BANNER=1`.
- Every JSON error response carries a `report_issue` link, so a user hitting a problem has
  somewhere to send it without hunting for the repository.

## v26.07.17 — Self-packaging, 12-factor configuration, and REST type coverage

### Self-packaging (single-binary deploy)

- `flapi pack` folds a whole config tree — YAML, SQL templates, small data files — into the binary
  itself, so `scp flapi-prod user@host` becomes the deploy. `info` inspects a bundle and `unpack`
  extracts it.
- A ZIP is appended after the executable, or on macOS written into a reserved `__FLAPI/__bundle`
  Mach-O segment allocated at link time, so the output stays **notarisable**. Fat/universal macOS
  binaries supported.
- `EmbeddedArchiveFileProvider` serves config and templates from the bundle, and an
  `embed://` DuckDB filesystem lets `read_csv('embed://data/cities.csv')` reach the same bytes from
  SQL.
- Reproducible: set `SOURCE_DATE_EPOCH` and the output is bit-identical across runs.
- Secrets are refused at pack time by default (`*.env`, `secrets/*`, `*.pem`, `*.key`).

### Configuration

- 12-factor environment variables: `FLAPI_CONFIG`, `FLAPI_LOG_LEVEL`, `FLAPI_PORT`, `FLAPI_HOST`.
- The baked-in version is derived from the git tag rather than hardcoded.

### REST serialization correctness

A sweep of DuckDB types that were previously serialized wrongly or not at all (#89):

- Native `LIST`/`STRUCT`/`ARRAY`/`UNION` columns serialized per row.
- `MAP` columns as `{key: value}` rather than a positional pair array.
- `UUID`, `HUGEINT`, `BLOB` and `BIT` corrected.
- `VARINT`/`BIGNUM`, `GEOMETRY` and `VARIANT` now serialized rather than dropped.

### Other

- Client parity restored between the `flapii` CLI, the VS Code extension and the server (#75–#77).
- PostHog telemetry moved to the shared schema v2.
- Wheel license metadata corrected to BUSL-1.1, and the MCP registry marker added.
- MCP logging capability emitted as an empty object rather than `null`.

## v26.05.18 — Prepared-statement coverage swept across every code path

Follow-up to v26.05.17. After v26.05.17 shipped, an internal audit found that the prepared-statement path was only wired into the GET endpoint executor — POST/PUT/PATCH writes and the Arrow-streaming endpoint still rendered Mustache templates as strings. This release closes that gap.

### Coverage extension

- **POST/PUT/PATCH writes** now take the prepared path. `executeWrite` calls the rewriter first, splits the rewritten SQL into statements (quote-aware), distributes the binding plan across statements by counting `?` placeholders per statement, and prepares + binds + executes each one. Multi-statement INSERT…;SELECT…RETURNING templates keep working — each statement is its own prepared statement with the right slice of the binding plan.
- **Arrow streaming (`executeQueryRaw`)** now routes through the prepared path with the same fall-back-to-string behaviour when the binding plan is empty.
- **`countSqlPlaceholders` helper** in `src/sql_utils.cpp` — quote-aware `?` counter (skips placeholders inside `'…'`, `"…"`, `$tag$…$tag$`) used by the write-path distributor. Covered by 6 new Catch2 cases.

### Coverage extension — tests

- **Read-path corpus extended from 37 → 99 parameterised payloads** (`test/integration/test_sql_injection_corpus.py`). Adds endpoints for `double`, `boolean`, `date`, `time`, `uuid`, `enum`, `email` so every validator type is exercised end-to-end. Plus a `/lookup-int-paged` endpoint that proves pagination + prepared bindings work together (count + paginated wraps both bind correctly).
- **New write-path corpus** (`test/integration/test_sql_injection_write_corpus.py`, 19 cases). Fires the classic injection payloads at a `POST /widgets/` endpoint and asserts the payload lands as a literal string column value, never as a side-effect that drops the table or smuggles extra rows. Includes a multi-statement INSERT-then-SELECT-RETURNING endpoint to exercise binding-plan slicing.

### Validator hardening (defense in depth)

- `validateDate` and `validateTime` now demand the entire input string be consumed — `2024-03-15' OR 1=1` no longer parses to `2024-03-15` and silently drops the suffix. Same fix as `validateInt` in v26.05.17.

### HTTP status correctness

- New `flapi::BadRequestError` exception class. `QueryExecutor::executeWithBindings` throws it on bind-conversion failure (caller supplied an invalid value for a typed param); `RequestHandler` catches it and returns **HTTP 400** with a JSON body, instead of the previous `500 Internal Server Error`. Server-side prepare/execute failures still return 500 (they're not client errors).

### Tests

- **586 C++ unit assertions** (Catch2; +6 for `countSqlPlaceholders`).
- **483 integration tests passing** (+81 from the corpus extensions). 21 skip in environments without the relevant fixtures.

---

## v26.05.17 — Security roadmap (Waves 0–3) + BSL relicense

**Headline:** in-product MCP + general security hardening — per-tool RBAC, shadow/dry-run, response shaping, description hygiene, prepared-statement SQL-injection defense, PBKDF2 password hashing, audit log, per-user rate limit, CORS allowlist, TLS wire-up, startup auditor. Simple things stay simple — every new control is opt-in via single-line YAML.

### License

- **Apache 2.0 → BSL v1.1** ([e1b465e](https://github.com/DataZooDE/flapi/commit/e1b465e)). The Business Source License is source-available; non-production use is permitted without a commercial agreement. The Change License (MPL 2.0) takes effect five years after first publication of each version. See [`LICENSE`](./LICENSE) for the full text.

### MCP hardening — Wave 2 (#24)

- **Per-tool RBAC** ([8886cd2](https://github.com/DataZooDE/flapi/commit/8886cd2), #27). `mcp-tool.allowed-roles: [admin, analyst]` in the endpoint YAML restricts a tool to JWT/OIDC principals carrying one of those roles. **Deny-by-default**: when `mcp.auth.enabled: true`, every tool MUST declare `allowed-roles` — a tool without one refuses every call. Endpoints with `mcp.auth.enabled` unset keep working role-free for `flapii project init` demos.
- **Dry-run / shadow mode** ([385f793](https://github.com/DataZooDE/flapi/commit/385f793), #29). Pass `"_dryRun": true` in `tools/call` arguments. flAPI runs validators + template expansion + EXPLAIN and returns the rendered SQL + plan as JSON, but never executes the query. The same controls that gate a real call (RBAC, rate limit) gate a dry-run too.
- **Tool-description hygiene scanner** ([63a1af7](https://github.com/DataZooDE/flapi/commit/63a1af7), #28). At config-load time, descriptions are scanned for control characters, JSON-breakout patterns, and known role-override phrases ("ignore previous instructions"). Strict-mode opt-in via `mcp.strict-descriptions: true` — refuses to start when any tool fails the scan.
- **Per-tool response shaping** ([9c9cd55](https://github.com/DataZooDE/flapi/commit/9c9cd55), #30). New `mcp-tool.response` block: `max-rows` caps the result-set size, `redact-columns: [...]` replaces listed columns with a redaction sentinel, `sample: true` returns only summary metadata (`row_count`, `columns`, `sampled: true`).
- **Per-tool rate limit** ([0a7d69c](https://github.com/DataZooDE/flapi/commit/0a7d69c), #34). New `mcp-tool.rate-limit: { enabled, max, interval }` keyed on the authenticated principal (with an anonymous fallback bucket per tool).

### General security wins — Wave 1 (#23)

- **PBKDF2-SHA256 password hashing** ([db87b8e](https://github.com/DataZooDE/flapi/commit/db87b8e), #36). `auth.users[*].password` accepts the MCF string `$pbkdf2-sha256$<iter>$<b64-salt>$<b64-hash>` (OpenSSL `PKCS5_PBKDF2_HMAC` with 600 k iterations, 16-byte salt, 32-byte key — OWASP 2023 minimum). Compatible with Python `passlib` and any other PBKDF2-SHA256 generator. Plaintext and 32-char-hex MD5 hashes still verify, but the startup auditor emits a deprecation warning.
- **Config-driven CORS allowlist** ([f1a6751](https://github.com/DataZooDE/flapi/commit/f1a6751), #32). The legacy wildcard `Access-Control-Allow-Origin: *` is gone — default is same-origin only. Opt into specific origins via `cors.allow-origins: [...]`. `flapii project init` still ships `["*"]` so first-run demos work; the auditor warns when `*` meets `auth.enabled: true`.
- **JSONL request audit log** ([1c762d4](https://github.com/DataZooDE/flapi/commit/1c762d4), #31). `audit: { enabled, sink: stdout|file, path, redact: [...] }` emits one JSON line per request (REST and MCP) with principal, method/target, params (redacted per config), status, row count, latency. Off by default, one-line to enable.
- **Per-user rate limit** ([b44c92d](https://github.com/DataZooDE/flapi/commit/b44c92d), #33). New `rate-limit.key: ip | user | user-or-ip`. The default stays `ip` for backward compatibility; `user-or-ip` is the recommended setting for share-NAT scenarios where many users share a single egress IP.
- **TLS in embedded server** ([e38c715](https://github.com/DataZooDE/flapi/commit/e38c715), #35). The `HTTPSConfig` struct is now wired into Crow's `ssl_file()` chain. Reverse-proxy termination is still recommended for production, but direct TLS is supported for self-contained deployments.

### SQL-injection defense — Wave 3 (#25)

- **Prepared-statement path for typed scalar params** ([8bf073d](https://github.com/DataZooDE/flapi/commit/8bf073d) + [ca16217](https://github.com/DataZooDE/flapi/commit/ca16217), #37). `{{ params.X }}` (double-brace) references on fields with typed validators (`int`, `double`, `boolean`, `date`, `time`, `uuid`, `enum`, `email`, `string`) are now rewritten to `?` and bound via `duckdb_bind_*`. The value travels as a primitive, not text — SQL injection becomes structurally impossible for those sites. Triple-brace `{{{ params.X }}}` is unchanged (for `LIKE` patterns and other text-mode use sites). The integer validator was also tightened: `1; DROP TABLE` no longer slips through as `1`.
- **W3.3: SQL-keyword regex demotion** ([ca16217](https://github.com/DataZooDE/flapi/commit/ca16217)). For numeric/temporal bindable fields, the historic keyword regex is demoted to a debug-level log line — the prepared bind is the hard defense, and the regex's false positives (`latitude=1.111`) are gone. Varchar-classified fields keep the regex because flAPI templates routinely embed them via triple-brace.
- **37-payload integration corpus** at `test/integration/test_sql_injection_corpus.py` — every classic injection pattern (UNION, OR 1=1, comment-evasion, xkcd 327) returns zero rows; legitimate values still match.

### Honest defaults & honest docs — Wave 0 (#22)

- **Startup security auditor** ([655d61f](https://github.com/DataZooDE/flapi/commit/655d61f), #26). At boot, flAPI scans the loaded config and emits structured warnings for: plaintext passwords, MD5 passwords, MCP exposed without auth on a non-loopback bind, and CORS wildcard combined with `auth.enabled`.
- **Documentation correctness**. The misleading claim that `{{{ }}}` "prevents SQL injection" is gone from `docs/CONFIG_REFERENCE.md`. The actual layered defense (validators → prepared bind → regex fallback for triple-brace and untyped fields) is documented in [§ 9 SQL Templates](docs/CONFIG_REFERENCE.md).

### Fixes

- **Windows release link** ([4619687](https://github.com/DataZooDE/flapi/commit/4619687)). `mcp_authorization_policy.hpp` forward-declared `EndpointConfig` as `class` while the actual type is `struct`; MSVC encodes that keyword into mangled symbols, so the call site and definition emitted different names. Fixed by aligning the forward decl.
- **Auth-context param leak** ([4619687](https://github.com/DataZooDE/flapi/commit/4619687)). `RequestValidator::validateRequestFields` rejected every authenticated write request as containing five phantom unknown fields (`__auth_username` / `_email` / `_roles` / `_type` / `_authenticated`). The reserved `__auth_*` prefix is now silently skipped.
- **Release linker fix** ([1116f25](https://github.com/DataZooDE/flapi/commit/1116f25)). Explicit `safeGet<int>` template instantiation in `config_manager.cpp` for cross-TU release linking (debug inlined; release with `-Wl,--no-undefined` exposed the missing definition).
- **Cross-platform smoke tests in CI** ([2f25366](https://github.com/DataZooDE/flapi/commit/2f25366), [822ea3e](https://github.com/DataZooDE/flapi/commit/822ea3e), [b3c9744](https://github.com/DataZooDE/flapi/commit/b3c9744)). Each platform binary (linux-amd64, linux-arm64, macos-arm64, windows-amd64) is now booted in CI before release; the four smoke jobs gate `create-release`.
- **Auth template variable names** ([8b2b8d8](https://github.com/DataZooDE/flapi/commit/8b2b8d8)). Doc fix: it's `auth.username`, not `context.auth.username`.

### Tests

- **580 C++ unit assertions** (Catch2).
- **402 integration tests passing** (37 of them parameterised SQL-injection payloads). 21 skip in environments without the relevant fixtures (AWS Secrets Manager, OIDC issuers).

---

For earlier history see `git log` or the GitHub release notes for prior tags (`v26.04.22` and below).
