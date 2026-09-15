# Implementation plan — OpenTelemetry + OpenInference observability for flAPI

**Spec:** `/home/jr/Projects/tmp/research/2026-09-15-flapi-otel-openinference/` (FINDINGS → BRD → HLD)
**Baseline:** HEAD `d2b9e74`.
**Branch:** `feat/otel-observability` (epic), one feature branch per issue.
**Revision:** v2 — revised after the agent-crew pre-implementation review
(`REQUEST_CHANGES`, 12 high-severity findings; run
`.crew/runs/20260915_144839_review_*/result.md`). Findings referenced as `F<n>`.

**Method:** strict red/green TDD. Every increment names the failing test to write *first*.

---

## Definition of done — applies to every issue

An issue is done when **all six** hold. An issue that cannot meet these is descoped, not waived.

1. **Real system tests pass.** At least one `test/integration/test_*.py` driving the real binary
   over real HTTP. Unit tests alone are never sufficient.
2. **Its performance gate passes** — the *proxy* gate (allocation counter / instruction count /
   microbenchmark) for every issue, plus the wall-clock load gate at each phase boundary. §8.2
   defines which applies where. A gate below the measured CI noise floor is not a gate (F11).
3. **A bug-free PR.** CI green on all four targets (x64-linux, arm64-linux cross,
   x64-windows-static, arm64-osx); full unit **and** integration suite green; the ASan+UBSan leg
   green on every target; the **separate** TSan leg green (ASan and TSan are mutually exclusive
   builds — TSan is a fifth leg on x64-linux only, F11); the `FLAPI_WITH_TRACING=OFF` leg green.
4. **No new `skip`, `xfail`, `continue-on-error` or `--ignore`.** Adding one is a failed issue.
   Written down because the repo already lost its load suite exactly that way (§8.1).
5. **An agent-crew round with no unresolved high-severity findings.** Cadence in §8.3.
6. **Docs updated** per the `CLAUDE.md` maintenance table (list in §8.5).

---

## 1. Context — why

flAPI advertises MCP revision `2026-07-28`. That revision, via [SEP-414][sep414] (**Final**),
reserves the *unprefixed* keys `traceparent`, `tracestate` and `baggage` inside `params._meta`.
flAPI parses `params._meta` (`src/mcp_route_handlers.cpp:799-828`) for three
`io.modelcontextprotocol/*` keys and **discards the trace context conforming clients already
send**. That is a conformance defect, not a feature request.

Beyond it:

1. **flAPI is invisible as an ordinary HTTP service.** `grep -rniE 'traceparent|opentelemetry|otlp' src/` returns nothing. No RED metrics, no server spans.
2. **Four parallel, uncorrelated observability mechanisms** (PostHog telemetry, audit JSONL, Crow logs, `arrow_metrics.hpp`), three separate clocks timing overlapping operations (`api_server.cpp:218`, `mcp_tool_handler.cpp:14`, `:35`), and no shared request identity.
3. **Three documented-but-false behaviours**, all verified: the audit log claims REST coverage and has none (`docs/CONFIG_REFERENCE.md:513`); `server.log_level` appears in three example YAMLs and is never read; `GET /api/v1/_config/metrics` is in the OpenAPI spec (`open_api_doc_generator.cpp:563-587`) with no route.
4. **Two latent defects in the path this epic modifies**, both found by the crew and verified here — see §4. They must be fixed *before* instrumentation, not alongside it.

**Outcome:** one span tree making flAPI a conventional traced HTTP service on *stable* semconv
**and** a first-class citizen of agent traces, with audit/log/trace sharing one identity and one
clock, privacy-preserving by default.

---

## 2. Decisions

| # | Decision |
|---|---|
| **D1** | Scope is all of HLD P0–P4, sequenced as the issues in §7. |
| **D2** | Consolidation = shared `RequestContext` + fix the three drift bugs. PostHog, audit and `arrow_metrics` keep their own pipelines. |
| **D3** | **Do not bump vcpkg.** The pinned baseline stays for all 38 other packages. `opentelemetry-cpp` alone is overridden by a **vcpkg overlay port** (`ports/opentelemetry-cpp`, pinned 1.24.0), selected declaratively through `vcpkg-configuration.json` so every platform and CI job picks it up with no environment plumbing. **Forced by measurement, not preference** — see below. |
| **D4** | agent-crew reviews at the cadence in §8.3, not once. |
| **D5** | **SETTLED by the issue -1 spike: keep opentelemetry-cpp.** It builds, links beside static DuckDB with **zero** duplicate symbols, and costs **+4.71 MiB (+6.9 %)**. The libcurl-only alternative is closed; both architectures must never ship. Because the 1.24.0 overlay provides `otlp-file`, flAPI does **not** hand-roll a file exporter — **issue 6 shrinks to configuration plus tests.** |

### D3 in detail — verified

CI pins vcpkg to tag `2024.11.16` (`.github/docker/linux_*/Dockerfile:57`); `vcpkg.json` pins
`builtin-baseline: b2cb0da…`. At that baseline `opentelemetry-cpp = 1.17.0#1`, features
`[elasticsearch, etw, geneva, otlp-grpc, otlp-http, prometheus, user-events, zipkin]`.

- **1.17.0 does not compile at all on this project's toolchain.** It uses `uint8_t` and friends
  without including `<cstdint>` in 136 headers; modern libstdc++ stopped providing it
  transitively. It fails at `api/include/opentelemetry/logs/severity.h:20` on local GCC 16 **and
  on GCC 13 inside `ubuntu:24.04`**, which is exactly what the CI Docker image builds with. This
  is what forces the overlay port — the pinned version is not merely inconvenient, it is
  unusable.
- **No `otlp-file` feature** — the OTLP File exporter postdates 1.17.0. The 1.24.0 overlay has
  it, so the air-gapped topology needs no flAPI-owned exporter after all.
- **No `sdk/configuration` auto-config module** — `OTEL_TRACES_SAMPLER`,
  `OTEL_TRACES_SAMPLER_ARG` and `OTEL_SDK_DISABLED` are resolved by hand.
  `OtlpHttpExporterOptions` *does* read `OTEL_EXPORTER_OTLP_*` natively, so construct it with its
  defaults and overwrite **only** fields the YAML set explicitly, tracked with `std::optional<T>`
  and never sentinel values — otherwise a YAML default of `http://localhost:4318` silently beats
  an operator's injected env var and Kubernetes auto-instrumentation breaks.

The crew split 3–1 in favour of keeping the SDK: the wire format is the easy part, while
`BatchSpanProcessor`, retry/backoff, samplers, W3C propagators and the Recordable model are what
a hand-rolled pipeline gets wrong. **Decided by the spike's number, not by argument** (D5).

---

## 3. Architecture

### 3.1 The layering rule

> **Exactly four `.cpp` files may `#include <opentelemetry/...>`:** `flapi_tracing.cpp`,
> `trace_scope.cpp`, `tracing_middleware.cpp`, `otlp_json_file_exporter.cpp`.
> **No header in `src/include/` includes an OTel header.** otel links **`PRIVATE`** to
> `flapi-lib` — the sole exception to that block's `PUBLIC` convention.

`otlp-http` drags in protobuf and abseil; letting those into a header that `config_manager.hpp`
includes recompiles ~80 TUs plus the test binary and grows the `-Wl,--no-undefined` link surface.
The opaque `SpanScope` (§3.3) buys this.

### 3.2 `RequestContext` — the consolidation spine (no OTel dependency, P0)

```cpp
struct RequestContext {
    // Fixed-width by spec — std::array, not std::string: 32/16 hex chars both exceed
    // libstdc++'s 15-char SSO buffer, so std::string means heap allocation per request.
    std::array<char, 32> trace_id{};    // all-zero == inactive
    std::array<char, 16> span_id{};
    std::array<char, 20> request_id{};  // "req-" + 16 hex; ALWAYS server-minted (F26)
    bool sampled = false;

    std::chrono::steady_clock::time_point t0;   // THE clock — replaces all three
    std::int64_t elapsedMs() const;

    // Views, not copies (F14). Both are safe only because of the COW snapshot (§4.1):
    //  - raw_path views crow::request::url, which outlives the middleware context
    //  - route_template views the pinned endpoint snapshot, or a static literal
    std::string_view raw_path;          // NEVER exported; logs + exclude matching only
    std::string_view route_template;    // "<unmatched>" until the handler resolves it
    const char* http_method = "";       // crow::method_name() — static storage
    const char* auth_kind = "none";     // bounded enum — no allocation

    std::shared_ptr<const std::vector<EndpointConfig>> endpoints;  // pinned for the request
    std::string principal = "anonymous";
    std::string mcp_method, mcp_tool, mcp_session_id;
    std::int64_t row_count = -1;
    int status_code = 0;
    std::vector<std::pair<std::string, std::string>> audit_params;  // only when audit is on

    static void mintRequestId(std::array<char, 20>& out);
};
```

**Ownership and lifetime — the contract (F3, F13).** Previously unspecified; both plausible
readings were broken.

- `TracingMiddleware::context` owns the `RequestContext` **by value** and owns the
  `RequestContextScope` **as a member** — *not* as a local in `before_handle`, which would leave
  TLS null for the entire handler and orphan every inner span. Crow destroys the per-request
  context tuple on every path, so the scope's destructor clears TLS even when `after_handle` is
  skipped (handler throws, `res.end()` short-circuit, connection abort).
- The `shared_ptr`-ownership wording in the old risk #5 is **deleted** — it contradicted §3.2.
- `SpanScope` is **strict RAII**: the destructor ends the span; `end()` releases `Impl` and is a
  no-op afterwards. Without this, a `BadRequestError` thrown between `startSpan` and `end()`
  leaks the OTel `trace::Scope` push and mis-parents the *next* request on that pooled worker.
- **Open, answer before issue 1:** does Crow invoke `after_handle` when the handler throws, when
  a middleware calls `res.end()`, and when the client aborts? Read the pinned Crow's
  `http_connection.h`/`app.h` and pin the answer with a test. The context-member design above is
  chosen precisely so the answer does not change correctness — but it must be confirmed.

**How it reaches each consumer:**

| Consumer | Mechanism | Signature change? |
|---|---|---|
| REST audit line (new coverage) | `TracingMiddleware::after_handle` → `context.rc` | new emission site only |
| MCP audit line | `RequestContextScope::current()` in the existing `emit_audit` lambda | no |
| **658 `CROW_LOG_*` sites** | a `crow::ILogHandler` reading TLS — verified to exist, `crow/logging.h:33,38,127` | **no** |
| `SQLTemplateProcessor` / `QueryExecutor` / `DatabaseManager` | OTel's thread-local active-span stack | **no** |
| `AuthMiddleware` | reads the ambient endpoint resolution instead of resolving again (§4.2) | no |
| `MCPTaskManager` worker | typed `TraceCarrier` on `MCPToolCallRequest`, captured **by value** | yes — one typed field |

**The "zero signature changes" claim is scoped to the synchronous request stack (F5).** OTel's
TLS active-span stack does not cross thread boundaries. `MCPTaskManager`, `HeartbeatWorker`,
cache refresh, warmup and dry-run all run off-request, where ambient TLS is either absent
(orphan) or **stale** (attached to an unrelated previous trace).

> **Rule:** background threads **start root spans and never implicit children.** Enforced by an
> assertion that `RequestContextScope::current() == nullptr` on entry to any worker-thread task,
> plus the explicit `TraceCarrier` reactivation. The carrier-to-scope contract and its failing
> test land **before issue 11**, not at issue 15 — otherwise P2 inner spans ship first and
> silently attach to stale parents.

**Do not extend `registerActiveExecutor`** (`query_executor.hpp:31-35`) — it is a *cross-thread
reach-in* registry; span context is strictly thread-confined.

### 3.3 `SpanScope`

Opaque, pointer-sized, allocation-free when disabled; strict RAII per §3.2.

```cpp
class SpanScope {
public:
    SpanScope() noexcept = default;          // impl_ == nullptr
    ~SpanScope();                            // ends the span if still open
    explicit operator bool() const noexcept { return impl_ != nullptr; }
    void setAttr(const char* key, std::string_view /* | int64 | double | bool */) noexcept;
    void addEvent(const char*) noexcept;
    void addLink(const SpanContextIds&) noexcept;
    void setError(const char* error_type) noexcept;   // enumerated only
    void end() noexcept;                              // idempotent; releases Impl
    SpanContextIds ids() const noexcept;
private:
    struct Impl; Impl* impl_ = nullptr;       // holds the span AND a trace::Scope
};
```

**Exception safety (F19):** every `SpanScope` method and both middleware handlers
**catch internally and never propagate**. Instrumentation may not fail a request — including on
`bad_alloc` from attribute construction. Failures increment a named atomic counter surfaced by
the metrics route.

Call-site idiom — **guard before building the value**:

```cpp
SpanScope s = Tracing().startSpan("flapi.render_template", SpanKind::Internal);
if (s) s.setAttr(semconv::flapix::kTemplatePath, configRelative(endpoint.templateSource));
```

### 3.4 New modules

| File | Responsibility | OTel dep? | Phase |
|---|---|---|---|
| `request_context.{hpp,cpp}` | §3.2 | no | P0 |
| `trace_context.{hpp,cpp}` | W3C parse/format/precedence; `sep414::` constants | no | P0 |
| `flapi_log_handler.{hpp,cpp}` | `crow::ILogHandler`; text + JSON; **atomic whole-line emission** (F28) | no | P0 |
| `trace_semconv.hpp` | every attribute key as `constexpr`, one pinned revision (F30) | no | P1 |
| `tracing_config.hpp` | HLD §8.1 struct, parsed next to `parseAuditConfig` | no | P1 |
| `trace_env.{hpp,cpp}` | `OTEL_*`/`FLAPI_*` resolution via an injectable `EnvLookup` | no | P1 |
| `trace_capture_policy.{hpp,cpp}` | tier resolution; redact-then-clamp; byte budgets | no | P1 |
| `tracing_middleware.{hpp,cpp}` | leftmost Crow middleware; owns `RequestContext` + SERVER span | via facade | P0/P1 |
| `trace_scope.{hpp,cpp}` + `trace_scope_off.cpp` | §3.3 and its OFF twin | **yes** | P1 |
| `flapi_tracing.{hpp,cpp}` | lifecycle facade + `ITracingBackend` seam | **yes** | P1 |
| `otlp_json_file_exporter.{hpp,cpp}` | the missing `otlp-file` | **yes** | P1 |

`flapi_tracing.hpp` mirrors `flapi_telemetry.hpp` (facade + pure-virtual backend + `active()`
gate). **One deviation:** `Tracing()` is *not* a leaked singleton — it owns a
`BatchSpanProcessor` thread. Function-local static plus a `TracingGuard` RAII in `main()` calling
`shutdown()` before the existing flush at `main.cpp:690`, and in the signal path at `:331`.

`sep414::{kTraceparent,kTracestate,kBaggage}` live in `trace_context.hpp`, **not**
`mcp_constants.hpp:33-36` — they are unprefixed by deliberate SEP-414 exception, and physical
separation is what stops a maintainer "fixing" them into the reverse-DNS block.

### 3.5 `FLAPI_WITH_TRACING=OFF` — corrected build contract (F4)

The previous design was wrong twice: an inline-empty twin emits **no symbols**, so the promised
versioned-namespace link error never fires; and `add_compile_definitions(FLAPI_WITH_TRACING)` is
unconditional — there was no `option()` and therefore no way to select OFF at all, so the
mandated OFF CI leg could not exist.

Corrected:

1. `option(FLAPI_WITH_TRACING "Build with OpenTelemetry tracing" ON)`.
2. A generated `flapi_build_config.hpp` via `#cmakedefine01 FLAPI_WITH_TRACING`, included by
   `trace_scope.hpp` and `flapi_tracing.hpp`. Configuration state travels in a generated header,
   not an ad-hoc define.
3. The compile define stays **global** and is placed **before** the `add_subdirectory` block, so
   the Crow middleware-tuple layout is identical in every TU. (HEAD's own `d2b9e74` added
   `CROW_ENABLE_COMPRESSION` globally to fix exactly this `crow::response` ODR class.)
4. Each of `tracing_on_v1` / `tracing_off_v1` gets **at least one out-of-line anchor symbol that
   every call site necessarily references** — `SpanScope::end()`, defined out-of-line in
   `trace_scope.cpp` and `trace_scope_off.cpp` respectively. This is what makes a mixed-macro
   build a *link error* instead of silent corruption.
5. A deliberately-mixed CI smoke target that **must fail to link**, asserted as a negative test.

`tracing_middleware.cpp` stays in the **unconditional** source list and calls only through
`SpanScope`/`Tracing()`, so `RequestContext`, the audit line and log correlation all survive OFF.

---

## 4. Prerequisites — behaviour-free commits, merged before any instrumentation

Three commits, each CI-green on its own. The first two fix latent defects this epic would
otherwise widen and build on; the third removes a silent-failure trap.

### 4.1 Copy-on-write endpoint table (F1) — a real use-after-free today

Verified: `ConfigManager::endpoints` is a bare `std::vector<EndpointConfig>`
(`config_manager.hpp:668`); `getEndpointForPathAndMethod` returns `&endpoint` into it; the vector
is cleared / `push_back`-ed / erased at runtime by `refreshConfig`, `POST /config` and the
config-service endpoints **with no lock against in-flight requests**. Today the raw pointer lives
only inside `handleDynamicRequest`; the earlier draft of this plan widened that window across
`before_handle → handler → after_handle` and *promoted it to a documented design element*.

Change to `std::shared_ptr<const std::vector<EndpointConfig>>`, swapped atomically on reload. The
middleware pins the snapshot for the request lifetime; the handler reads the same snapshot. This
is also the precondition that makes §3.2's `string_view` fields safe. Add a reload-under-load test.

### 4.2 Pre-compile route regexes once at config load (F12)

Verified: `RouteTranslator::matchAndExtractParams` constructs a fresh `std::regex` on **every
call** (`route_translator.cpp:29`), and `getEndpointForPathAndMethod` linearly scans all
endpoints. With ~18 example endpoints that is ~9–18 regex compilations per REST request, each
heap-allocating — plausibly 50–300 µs of pure routing, and a strong candidate for the repo's
unexplained *"server performance degrades under high concurrent load"* `xfail`.

The pattern derives from `urlPath`, which changes only on reload, so the compiled regex belongs
on the COW snapshot from §4.1. **This must land before any delta is measured** — a 0.5% gate
cannot be evaluated on top of an undiagnosed defect in the very path being modified.

### 4.3 Collapse the five literal `crow::App<...>` spellings

`api_server.hpp:27` defines `FlapiApp`, but the type is spelled out literally in **five** places
across three files: `mcp_route_handlers.hpp:49`, `mcp_route_handlers.cpp:359`,
`open_api_doc_generator.hpp:20` and `:21`, `open_api_doc_generator.cpp:11` and `:278`. Adding a
middleware to the alias alone yields two *distinct, valid* `crow::App` instantiations — a second
middleware tuple, default-constructed and never configured. It presents as "MCP spans are
missing". Add a comment on the alias and a CI grep guard for `crow::App<`.

---

## 5. Testing strategy

Both mechanisms; neither substitutes for the other.

The shared `flapi_server` fixture hardcodes `DATAZOO_DISABLE_TELEMETRY=1` and a fixed config, so
OTel tests use the `@pytest.mark.standalone_server` pattern from `test_warmup_readiness.py`.
Factor it into `test/integration/otel_helpers.py`; register new markers in `pyproject.toml`.

**Primary — file exporter + JSONL parse**, for span content and structure: attributes, parenting,
cardinality, route inventory, the no-leak invariant. Deterministic, no ports, no Python protobuf,
and it exercises a shipped product feature (the air-gapped topology).

> **Ordering correction (F8):** this harness depends on `flush.mode: on_response`, so that
> feature moves from issue 13 to **immediately after issue 6**, and issue 9.3's flush mechanics
> fold into issue 6. Until then, tests drive `ForceFlush` directly.

**Secondary — in-process Python OTLP/HTTP collector**, for the network path the file exporter
cannot cover: retries, unreachable/5xx/hanging collector, `OTEL_EXPORTER_OTLP_HEADERS`
propagation, batch timing. `ThreadingHTTPServer`, modes `ok | http_500 | hang(n) | refuse`,
flAPI configured with `protocol: http/json` so the suite needs no protobuf.

**Parenting** uses a fixed, known traceparent
(`00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01`) so nothing is correlated
heuristically. **No-leak** is a whole-file substring search over the raw JSONL — strictly
stronger than per-attribute inspection, survives future attribute additions, and catches leaks in
span names, event names and status messages. It covers **exception-message sentinels** too
(F27: MCP task errors currently put raw exception text in responses). Pair it with a positive
assertion that expected metadata attributes are present, so an exporter that emits nothing cannot
pass.

---

## 6. Risks

| # | Risk | Mitigation |
|---|---|---|
| 1 | **Use-after-free on the endpoint table** under config reload | §4.1 COW snapshot, landed first |
| 2 | **Unexplained high-concurrency degradation** in the path being instrumented | §4.2 pre-compiled regexes + a profile as issue 0's deliverable |
| 3 | **Five literal `crow::App<...>` spellings** — silent second middleware tuple | §4.3 |
| 4 | **`FLAPI_WITH_TRACING` ODR** — layout-changing macro across three TUs | §3.5: generated header, global define, out-of-line anchor, mixed-build link-fail test |
| 5 | **protobuf + abseil into a link with static DuckDB, `-Wl,--no-undefined`, and Mach-O segment surgery for `flapi pack`.** abseil's ABI varies with the C++ standard it was built at (port defaults to C++14; flAPI is C++17) | Issue -1 spike is the abort gate: PRIVATE link, no OTel in public headers, `nm -C --defined-only` duplicate-symbol diff against `duckdb_static`, size delta as a number, `flapi pack` round-trip, all four targets |
| 6 | **TLS state leaking onto pooled Crow workers** on paths that skip `after_handle` | §3.2 lifetime contract: scope is a context *member*; tests for throw / 401 / 429 / abort, and `current()==nullptr` between two sequential same-thread requests |
| 7 | **`on_response` flush blocking the request thread** with a network exporter — a hanging collector depletes the Crow pool, contradicting NFR-4 | Restrict `on_response` to file/stdout exporters; add an `on_response` row to §8.2 and the `on_response` × hanging-collector case to 9.4 |
| 8 | **`tracing:` config hot-reload** — `getAuditLogger()` already ignores reloads (`config_manager.cpp:271-277`) | Declare `tracing:` **boot-only**; state it in the docs and pin it with a test |

---

## 7. Work breakdown

Each row names the failing test **first**. Ordering corrected per F7/F8/F9.

### Issue -1 — SPIKE: link `opentelemetry-cpp` alongside static DuckDB — **DONE, PROCEED**

Full results in [`spike/README.md`](../../spike/README.md); spike source kept as the
re-run check for future port upgrades.

| Gate | Result |
|---|---|
| 1.17.0 at the pinned baseline | ❌ **does not compile**, on CI's GCC 13 as well as locally |
| 1.24.0 via overlay port | ✅ clean, and ships `otlp-file` |
| Link under `-Wl,--no-undefined` + static DuckDB | ✅ |
| Duplicate strong symbols vs `libduckdb_static.a` | ✅ **0** |
| Binary size | **+4,906,272 B = +4.71 MiB, +6.9 %** |
| `flapi pack` round-trip | ✅ |
| `FLAPI_WITH_TRACING=OFF` | ✅ 0 otel symbols, within 36 KB of baseline |
| Runtime (provider, both exporters, W3C extract+inject, samplers) | ✅ |

**Remaining spike work, carried into issue 5:** the other three targets
(arm64-linux cross — where protobuf's host `protoc` is the usual failure point —
x64-windows-static, arm64-osx), and a Debug ASan+UBSan run. Also found: with
`FLAPI_WITH_TRACING=OFF` vcpkg still *installs* otel because the manifest dependency is
unconditional. Make it a **vcpkg manifest feature** so the OFF leg is genuinely otel-free
and does not pay protobuf/abseil build time in CI.

### Issue 0 — A working load harness, a baseline, and a profile

Split into four deliverables (F11, F12). Every NFR gate is currently unmeasurable (§8.1).

- **0a — CI variance measurement.** Run an unchanged binary N times on the target runner and
  record the p99 run-to-run spread. **Every wall-clock budget in §8.2 is derived from this
  number**, not assumed. Decides whether per-PR load gating is viable at all, or whether the
  wall-clock gate is per-phase only.
- **0b — The harness.** Replace the Python generator with **k6** (single static binary, vendored
  by download; `concurrent.futures` + `requests` measures the GIL, not flAPI — the likely reason
  it "hangs in CI"). `test/load/` scenarios against examples/northwind: cached reads, uncached
  reads, writes, Arrow IPC, MCP `tools/call`, health probes at realistic frequency. Sustained,
  not bursts. `make load-test` / `make load-baseline`, writing p50/p95/p99, throughput, error
  rate and RSS to `test/load/baselines/<sha>.json`. Best-of-3 or median-of-5.
- **0c — Proxy gates.** An allocation counter, a `perf stat` instruction count and Catch2
  microbenchmarks, where a sub-1% delta *is* meaningful. These carry DoD rule 2 per issue.
- **0d — Profile the `xfail`, then delete it.** Deliverable is a **profile**, not a
  reproduce-or-delete coin flip. Land §4.2, re-run the POST-concurrency case, and remove the
  marker with numbers attached. **Also assign, explicitly, the removal of
  `continue-on-error: true` from the integration job, the `--ignore=test_load_testing.py`, and
  the module-level `skip`** — nobody owned these before (F11d).

*Ships: the ability to make any performance claim in this plan truthfully.*

### P0 — correlation + conformance *(no new dependency, no exporter)*

**Issue 1 — Shared `RequestContext`**
- 1.1 `test_request_context.cpp` — `mintRequestId` format; `elapsedMs` monotonic; the struct is trivially relocatable where claimed
- 1.2 same — `RequestContextScope`: null outside, correct inside, restores on nested exit, per-thread. **Plus: handler throws → TLS clean; 401 and 429 short-circuits → TLS clean; two sequential same-thread requests → `current()==nullptr` between them** (F3)
- 1.3 `audit_logger_test.cpp` — `auditEventFrom(rc)`; null-context path still self-mints
- 1.4 `test_audit_log.py` — **REST** request emits one JSONL line whose `request_id` matches the `X-Request-Id` header. **`X-Request-Id` is always server-minted; an inbound one is never honoured** (F26) — test both directions → **closes drift bug #1**
- 1.5 same — a 401 and a 429 each emit an audit line (today: nothing)
- 1.6 Allocation gate (0c): tracing-off **and** audit-off probe path, with a stated numeric bound (F14)

**Issue 2 — Structured log correlation and a readable `server.log_level`**
- 2.1 `test_flapi_log_handler.cpp` — stamps ids when a context is ambient, nothing when not; JSON mode valid. **Concurrent emission from pooled workers produces whole, non-interleaved lines** (F28)
- 2.2 `config_manager_test.cpp` — `server.log_level` read from YAML; precedence CLI > env > YAML > default → **closes drift bug #2**
- 2.3 `test_log_correlation.py` — every line during a request carries the response header's id

**Issue 3 — W3C trace context extraction (SEP-414 `_meta` + headers)**
- 3.1 `test_trace_context.cpp` — table test: valid; version `ff`; 54/56 chars; non-hex; all-zero ids; trailing garbage; oversized `tracestate` clamped. **Plus resource-exhaustion: huge/repeated headers, oversized `baggage`, wrong `_meta` value types** (F20)
- 3.2 same — precedence incl. **disagree → `MetaOverHeader`**
- 3.3 MCP unit test — `_meta.traceparent` lifted from the **unprefixed** key; namespaced keys untouched
- 3.4 `test_tracing_correlation.py` — REST header and MCP `_meta` both reach the audit `trace_id`; disagreement → `_meta` wins

> **Ships P0 in full: the SEP-414 conformance fix and a joinable audit log, no new dependency.**

### P1 — spans and export

**Issue 5 — `SpanScope`, the facade, the OFF contract**
5.1 `test_trace_scope.cpp`, **compiling and passing in both modes**: falsy default; `sizeof == sizeof(void*)`; zero allocations when disabled; **destructor ends an open span**; `end()` idempotent; **the deliberately-mixed build fails to link** (F4). 5.2 `test_flapi_tracing.cpp` with an injected `InMemorySpanBackend`; inactive by default. 5.3 `test_trace_env.cpp` — **the full precedence matrix** including `OTEL_SDK_DISABLED` × file exporter (F18); endpoint alone never enables. 5.4 `config_manager_test.cpp` — the `tracing:` block; **`tracing:` is boot-only and a reload is ignored** (risk 8).

**Issue 6 — OTLP-JSON file exporter + flush mechanics** *(9.3 folded in, F8)*
6.1 `test_otlp_json_file_exporter.cpp` — JSONL shape; ids lowercase hex; `startTimeUnixNano` a decimal **string**; typed attribute values; status enums; `path: "-"` → stdout. 6.2 **one `write` + one `flush` per `Export()` batch, not per span** — assert against an injected stream. 6.3 `ForceFlush` durable; `Shutdown` idempotent; export-after-shutdown counts a drop. 6.4 Exporter path hardening: unwritable path, disappearing directory, full disk (F21). 6.5 `test_tracing_file_exporter.py` + SIGTERM flush. 6.6 **`flush.mode: on_response`, restricted to file/stdout exporters** (F7).

**Issue 7 — `TracingMiddleware` and the route inventory** *(7.5/7.6 split out, F11)*
7.1 `test_tracing_middleware.cpp` — `GET /health` → one SERVER span. 7.2 `test_tracing_http_coverage.py` — **the regression test**: 401, 403, 429, `OPTIONS`, 404, 500 each exactly one span. *A handler-level implementation fails all six.* 7.3 **Excluded routes create no span and no scope** — children become roots, documented; the ≥500 synthetic span's parent chain pinned explicitly (F16 — the old "non-recording span" wording was self-contradictory). 7.4 `test_tracing_cardinality.py` — 1,000 random unmatched paths → exactly one `http.route`.
- **7.3a `http.route` is NOT resolved in the middleware** (F2). Verified: `AuthMiddleware::before_handle` *already* calls `getEndpointForPathAndMethod` (`auth_middleware.cpp:147`), so the old "one resolution per request" claim was false — the middleware would have made it three, and added a full O(N)×regex scan to `/health`, which does zero resolution today. Instead: `handleDynamicRequest` (which already resolves) writes the template into the ambient context; static routes take Crow's rule string; everything else is `<unmatched>`. `AuthMiddleware` consumes the ambient resolution. **Regression test: exactly one resolution per request.**

**Issue 7b — Parenting and route-template acceptance** *(split from 7)*
BR-1 acceptance (`test_tracing_parenting.py`) and the full static-route inventory
(`test_tracing_routes.py`), including `/api/v1/_config/*` placeholders.

**Issue 8 — Capture policy, redaction, the no-leak invariant**
8.1 tier matrix incl. **global `off` beats endpoint `payload`**. 8.2 **redact-then-clamp**, UTF-8-safe. 8.3 reuse of `audit.redact_keys` and `redact_columns`. 8.4 `test_tracing_no_leak.py` — sentinels in a query param, path segment, `Authorization` bearer, body, result cell **and an exception message** (F27); whole-file assertion.

**Issue 9 — MCP single-span contract, `error.type`, resilience**
9.1 exactly one SERVER span carrying both `http.*` and `mcp.*`, with the "batching or SSE breaks this" invariant comment at the span site. 9.2 the full `FailureKind` → `error.type` table. 9.4 `test_tracing_resilience.py` — refusing / 5xx / hanging collector, **plus `on_response` × hanging collector** (F7), **plus a slow-but-working collector driving queue saturation** — the only test that validates the drop counter and the "no unbounded memory growth" claim (F25).

**Issue 10 — counters, then the metrics route** *(counters split ahead of 9.4, F8)*
10a the drop/export counters 9.4 depends on. 10b `GET /api/v1/_config/metrics` implemented, bearer-gated, matching the OpenAPI spec → **closes drift bug #3**.

**Issue 13 → moved to issue 6.6.**

### P2 — inner visibility

**Issue 11 — Inner spans: render / bind / query / serialize**
Preceded by the background-thread root-span contract and its test (§3.2, F5). 11.1 `flapi.render_template`, config-relative path. 11.2 `flapi.bind_params` with prepared/interpolated counts; DuckDB `CLIENT` span with `db.*` and `flapi.cache.backed` — **never `hit`**. 11.3 `db.query.text` only when `interpolated_count == 0` and the flag is on; default off. 11.4 full parent chain in one request. 11.5 `flapi.serialize` for Arrow — note it is **buffered in memory**, not socket-streamed (`request_handler.cpp:311-314`), so one span brackets it and per-batch granularity is events. 11.6 `flapi.authorize` / `flapi.validate_arguments` as **events**.

**Issue 12 — Outbound `CLIENT` spans (OIDC / JWKS)**
`traceparent` injected; `url.full` query string stripped; `flapi.http_client.purpose`; no span on a JWKS cache hit (record `flapi.jwks.cache_hit` instead).

### P3 — evaluation

**Issue 14 — OpenInference overlay and the payload tier**
Overlay only when `openinference: true` **and** tier is `payload`; no `CHAIN` on the REST span. `retrieval.documents.*` with `document.score` **omitted**. Per-endpoint opt-in; global `off` wins; `MCPResponseShaper` runs **before** capture. **Memory contract (F15, F22):** one per-span payload byte budget plus a per-attribute clamp, applied after redaction and enforced **during serialisation** (streaming, UTF-8-safe) rather than after materialisation — otherwise a single 50 MB TEXT cell spikes RSS regardless of the cap. Covers params, path segments, bodies, result cells and OpenInference attributes, not just retrieval documents. The export queue is bounded **in bytes**, or `max_queue_size` auto-clamps when the payload tier is on. State one RSS ceiling number. Test one wide row *and* many medium fields.

### P4 — completeness

**Issue 15 — Async task span links**
`TraceCarrier` by value; task span is a **new root with a link**; `tasks/get` polls linked, not children; `tasks/cancel` closes with `error.type = cancelled`; persisted trace/span columns on `flapi_mcp_tasks`; TSan over the handoff. **Also fold in the stale header docs** — `mcp_task_manager.hpp:25-27` claims tasks are in-memory and do not survive restart, but `mcp_task_manager.cpp:84-101` already creates and recovers the table (F29).

**Issue 16 — Trace ids in error responses; background-work traces**
16.1 failing test named: `test_tracing_error_trace_id.py` — a 400, a 429 and a 500 each return a body containing the active `trace_id`, matching the exported span. 16.2 heartbeat / cache refresh / warmup root spans, under the §3.2 background-thread rule.

---

## 8. Verification

### 8.1 The starting position

- `test/integration/test_load_testing.py` carries a module-level `pytest.mark.skip`.
- CI additionally passes `--ignore=test_load_testing.py`.
- Cases inside are `xfail(strict=False)` for *"Server performance degrades under high concurrent load"*.
- The whole `integration-tests` job is `continue-on-error: true`.

So the hot path is unmeasured, the load suite is off, and a known performance defect is parked in
the exact code path this epic modifies. Issue 0 fixes all four; DoD rule 4 stops it recurring.

### 8.2 Gates

**Proxy gates (per issue, DoD rule 2).** Allocation count, `perf stat` instruction count, Catch2
microbenchmark. These are where a sub-1% claim is meaningful.

**Wall-clock load gates (per phase boundary).** Budget = **max(stated target, 2 × the p99
variance measured in issue 0a)**. A budget under the noise floor is not a gate (F11).

| Gate | Configuration | Target (subject to 0a) |
|---|---|---|
| NFR-1 | tracing off, `FLAPI_WITH_TRACING=ON` | proxy: no allocation beyond §3.2's bound on the probe path; wall-clock: within noise |
| After issue 1 | `RequestContext` landed, tracing + audit off | the most at-risk gate; proxy-gated primarily |
| Issue 1, audit on | REST audit, buffered vs unbuffered | numbers recorded (see §9.2) |
| NFR-2 | tracing on, `capture: metadata`, `otlp_http` | p99 ≤ +2% |
| P2 inner spans | 5–6 spans/request | p99 ≤ +2% cumulative; **export-queue drop rate 0** at sustained load |
| **`on_response`** | file/stdout exporter only | its own row; latency is coupled by design — state the number (F7) |
| NFR-4 | collector refusing / 5xx / hanging / **slow** | p99 and error rate unchanged beyond `flush.timeout_ms`; drop counter moves; **RSS bounded** |
| P3 payload tier | `capture: payload`, overlay on | RSS under the stated ceiling (§7 issue 14) |
| Build | binary size | the number from issue -1, against the budget in §10 |

### 8.3 agent-crew cadence

| When | Focus | Blocking? |
|---|---|---|
| ~~On this plan~~ | **done** — v2 is the result (`REQUEST_CHANGES`, 12 high) | — |
| After issue -1 | the go/no-go and the D5 binding choice | yes |
| After issue 0 | the harness, the baseline, the `xfail` profile | yes |
| End of each phase (P0, P1, P2, P3, P4) | the accumulated diff | yes |
| Issue 8 | the no-leak invariant and capture tiers, as a security review | yes |
| Any high-severity finding | re-review after the fix | yes |

Persona bias: **security** (8, 14), **performance** (0, 1, 7, 11), **correctness** (3, 9, 15),
**maintainability** (5's OFF twin, 6).

### 8.4 Per increment

```bash
make debug && build/debug/test/cpp/flapi_tests "[tracing]"     # must fail first
make release && make test
cd test/integration && uv run pytest test_tracing_*.py -v
make load-test -- --compare baselines/<sha>.json
```
New C++ test files go in the `add_executable(flapi_tests ...)` list — there is no globbing.

### 8.5 Epic acceptance

Conformance (BR-1); HTTP completeness (BR-21); cardinality (BR-23); privacy (BR-7 — the no-leak
test *is* the guarantee); resilience (NFR-4); zero-cost-when-off; binary size + `flapi pack`;
every §8.2 gate met; TSan green; every crew round closed; and a `docker compose` demo with Jaeger
and with Phoenix.

**Docs:** a new `docs/OBSERVABILITY.md`; `docs/CONFIG_REFERENCE.md` (the `tracing:` block, the
corrected audit-coverage claim, the now-real `server.log_level`);
`docs/CONFIG_SERVICE_API_REFERENCE.md`; `docs/spec/ARCHITECTURE.md` and `REQUEST_LIFECYCLE.md`;
`docs/spec/DESIGN_DECISIONS.md` (D1–D5 and the 1.17.0 constraint); `TELEMETRY.md` (a pointer
noting tracing is a separate subsystem with a separate consent model).

---

## 9. Robustness, memory and performance notes

### 9.1 Per-request allocation

`RequestContext` is built on every request on every route regardless of tracing — including
`/health`. §3.2 therefore uses fixed-width `std::array` for the three ids (32 and 16 hex both
exceed libstdc++'s 15-byte SSO), `const char*` for closed enums, `string_view` for `raw_path` and
`route_template` (safe only on §4.1's pinned snapshot), a flat vector for `audit_params`
populated only when audit is on, and by-value ownership in the middleware context with a raw TLS
pointer — no `make_shared`, no refcount atomics. Gate: issue 1.6.

### 9.2 Audit on the REST hot path — corrected

**The earlier claim in this plan was wrong.** `AuditLogger::log` calls `std::ostream::flush()`
(`audit_logger.cpp:71`) — a stdlib buffer flush lowering to one `write(2)` — **not** `fsync`. The
"1000 serialized fsyncs per second will not keep up" cost model overstated by one to two orders
of magnitude. The real per-line cost is a mutex plus a ~1–5 µs write, and the likely dominant
term is `serialiseEvent` plus the `ostringstream` ISO-8601 timestamp (`audit_logger.cpp:78-86`).

Consequently:

- **Profile before designing any queue.** Optimise serialisation first if that is where the time
  is. The async queue may still earn its keep; it must be re-derived from a measurement.
- The knob is **`audit.buffered`**, not `audit.sync` — there are no durability semantics today,
  and `sync` would promise a guarantee the code does not provide.
- If async lands: a **standalone** queue with its own drain in the signal path — *not* shared
  with `TelemetryTaskQueue`, whose shutdown path is coupled to `DATAZOO_DISABLE_TELEMETRY=1`,
  which the integration fixture sets unconditionally, so audit lines would vanish in exactly the
  suite meant to verify them (F17). Plus order-preservation and SIGTERM-drain tests, and
  drop/backpressure counters. Audit I/O failure must not fail the request unless explicitly
  configured fail-closed.

### 9.3 Contracts pinned by tests

Exception safety (§3.3, catch-internally-never-propagate + a counter); attribute lifetime
(`setAttr` copies immediately, never retains the view — pin with an ASan test building an
attribute from a temporary); queue sizing stated **in bytes** as well as spans; TLS cleanliness
across pooled workers; atomic whole-line log emission; the boot-only `tracing:` reload test.

### 9.4 Deliberately accepted

Three in-process HTTP stacks become two-and-a-bit (OTLP reuses the already-linked libcurl);
`arrow_metrics.hpp` stays a parallel mechanism per D2, surfaced through issue 10b.

---

## 10. Open questions

1. **Binary-size budget** — what delta harms the single-binary positioning? **Issue -1's pass/fail is undecidable without a number.** Owner: JR.
2. **Audit durability contract** — there is none today (`ostream::flush`, not `fsync`). Is one wanted? Owner: JR.
3. **Dedicated CI runner for the load job?** If not, issue 0a's variance may make per-PR wall-clock gating impractical regardless of budget.
4. **`db.query.text` at metadata tier** — allow only when `interpolated_count == 0`, default off. *(Recommend: yes.)*
5. **Semconv revision** — pin a release in `trace_semconv.hpp` **before issue 7**, not at the end, since attribute names bake into fixtures and dashboards from there on (F30).
6. **OSS vs enterprise for the OpenInference overlay.** *(Recommend: OSS.)*

[sep414]: https://modelcontextprotocol.io/seps/414-request-meta
