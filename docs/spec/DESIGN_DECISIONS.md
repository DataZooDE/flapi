# Design Decisions

This document explains the rationale behind key architectural and design decisions in flAPI.

## 1. Singleton Pattern for DatabaseManager

**Decision:** DatabaseManager uses the singleton pattern with `getInstance()`.

**Rationale:**
- DuckDB is an in-process database engine with shared state
- Multiple connections to the same database file require coordination
- Thread-safe access to the connection pool is critical
- Extensions are loaded once and shared across all queries

**Implementation:**
```cpp
// src/include/database_manager.hpp:40
static std::shared_ptr<DatabaseManager> getInstance();
```

**Tradeoffs:**
- (+) Ensures single DuckDB instance per process
- (+) Simplifies connection pooling and extension management
- (+) Thread-safe by design with mutex protection
- (-) Harder to test in isolation (requires test fixtures)
- (-) Global state can complicate debugging

**Source files:** `src/database_manager.hpp`, `src/database_manager.cpp`

---

## 2. Facade Pattern for ConfigManager

**Decision:** ConfigManager acts as a facade that delegates to specialized classes.

**Rationale:**
- Configuration is complex: YAML parsing, validation, serialization, endpoint storage
- Single class would violate Single Responsibility Principle
- Facade provides unified API while internal classes handle specific concerns

**Implementation:**
```cpp
// src/include/config_manager.hpp:560-564
std::unique_ptr<ConfigLoader> config_loader;
std::unique_ptr<EndpointRepository> endpoint_repository;
std::unique_ptr<ConfigValidator> config_validator;
std::unique_ptr<ConfigSerializer> config_serializer;
```

**Delegated responsibilities:**
| Class | Responsibility |
|-------|----------------|
| `ConfigLoader` | YAML file loading, recursive directory scanning |
| `EndpointRepository` | Endpoint storage, lookup by path/slug |
| `ConfigValidator` | Configuration validation rules |
| `ConfigSerializer` | JSON/YAML serialization |
| `EndpointConfigParser` | Parsing endpoint YAML into structs |

**Tradeoffs:**
- (+) Clean separation of concerns
- (+) Each delegated class is testable in isolation
- (+) Easy to extend without modifying facade
- (-) More files to navigate
- (-) Indirection can obscure control flow

**Source files:** `src/config_manager.cpp`, `src/config_loader.cpp`, `src/endpoint_repository.cpp`, `src/config_validator.cpp`, `src/config_serializer.cpp`

---

## 3. Unified REST/MCP Configuration

**Decision:** REST endpoints and MCP tools share the same configuration format.

**Rationale:**
- Many APIs need both REST and MCP access
- Duplicating configurations would cause drift
- Same endpoint can serve both protocols with different interfaces

**Implementation:**
```yaml
# sqls/customers.yaml - serves both REST and MCP
url-path: /customers
method: GET
request:
  - field-name: id
    field-in: query
template-source: customers.sql

mcp-tool:
  name: get_customers
  description: Retrieve customer data
```

**EndpointConfig type detection:**
```cpp
// src/include/config_manager.hpp:223-229
Type getType() const {
    if (!urlPath.empty()) return Type::REST;
    if (mcp_tool.has_value()) return Type::MCP_Tool;
    if (mcp_resource.has_value()) return Type::MCP_Resource;
    if (mcp_prompt.has_value()) return Type::MCP_Prompt;
    return Type::Unknown;
}
```

**Tradeoffs:**
- (+) Single source of truth for endpoint definition
- (+) Automatic MCP tool generation from REST endpoints
- (+) Consistent parameter handling across protocols
- (-) MCP-specific features require optional fields
- (-) Configuration complexity increases

**Source files:** `src/include/config_manager.hpp` (EndpointConfig struct)

---

## 4. Mustache for SQL Templates

**Decision:** Use Mustache templating for SQL generation.

**Rationale:**
- Mustache is logic-less, preventing complex templates
- Clear distinction between double `{{ }}` and triple `{{{ }}}` braces
- Conditional sections for optional parameters
- Familiar to many developers

**Implementation:**
- Triple braces `{{{ }}}` - HTML-escaped output (safe for strings)
- Double braces `{{ }}` - Raw output (for numbers, identifiers)
- Sections `{{#param}}...{{/param}}` - Conditional rendering

**Example:**
```sql
SELECT * FROM customers
WHERE 1=1
{{#params.id}}
  AND customer_id = {{{ params.id }}}
{{/params.id}}
LIMIT {{ params.limit }}
```

**Tradeoffs:**
- (+) Simple, declarative templates
- (+) Security: triple braces escape quotes
- (+) Familiar syntax
- (-) Limited logic capabilities
- (-) No loops or complex conditionals
- (-) Mustache escaping rules can be confusing

**Source files:** `src/sql_template_processor.cpp`

---

## 5. DuckLake for Caching

**Decision:** Use DuckLake (table versioning) for result caching.

**Rationale:**
- Time-travel queries enable cache rollback
- Snapshot-based storage is space-efficient
- Incremental refresh minimizes data transfer
- Built on DuckDB for consistency

**Cache modes:**
| Mode | Description | Use Case |
|------|-------------|----------|
| Full Refresh | Replace entire table | Small, frequently changing data |
| Incremental Append | Add new rows only | Append-only logs, events |
| Incremental Merge | Insert/update/delete | Complex change tracking |

**Implementation:**
```yaml
cache:
  enabled: true
  table: customers_cache
  schedule: "6h"
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
```

**Tradeoffs:**
- (+) Built-in time-travel for debugging
- (+) Efficient snapshot storage
- (+) Flexible refresh strategies
- (-) Requires DuckLake extension
- (-) Schema management complexity
- (-) Learning curve for cache templates

**Source files:** `src/cache_manager.cpp`, `src/heartbeat_worker.cpp`

---

## 6. Middleware Chain Pattern

**Decision:** Use Crow's middleware chain for cross-cutting concerns.

**Rationale:**
- Authentication, rate limiting, CORS are orthogonal to business logic
- Middleware pattern allows clean composition
- Each middleware can short-circuit the chain
- `before_handle` runs in declaration order and `after_handle` in reverse, and
  **`after_handle` still runs on a short-circuit** — Crow unwinds through every
  outer middleware. This is load-bearing twice over: it is how CORS headers reach
  a 401 response, and how the audit line and server span are still emitted for a
  request rejected before the handler.

**Implementation:**
```cpp
// src/include/flapi_app.hpp - the ONE spelling of the app type
using FlapiApp = crow::App<RequestContextMiddleware,
                           crow::CORSHandler,
                           FlapiCorsMiddleware,
                           RateLimitMiddleware,
                           AuthMiddleware>;
```

**Spell the alias, never `crow::App<...>`.** Two literal spellings produce two
*distinct, valid* `crow::App` instantiations — a second middleware tuple,
default-constructed and never configured. It presents as "MCP spans are missing",
which is a long way from the cause. `scripts/check_crow_app_alias.sh` fails the
build on a bare `crow::App<`.

**Middleware order:**
1. `RequestContextMiddleware` — request identity, SERVER span, audit line. Leftmost
   so that requests rejected later are still observed.
2. `crow::CORSHandler` — CORS headers
3. `FlapiCorsMiddleware` — flAPI's origin handling
4. `RateLimitMiddleware` — request rate limiting
5. `AuthMiddleware` — JWT/Basic/OIDC authentication

**Tradeoffs:**
- (+) Separation of concerns
- (+) Reusable across endpoints
- (+) Easy to add/remove middleware
- (-) Order matters and can cause bugs
- (-) Debugging middleware interactions is harder
- (-) The unwind-on-short-circuit contract is easy to get backwards. Getting it
  backwards once produced a duplicate audit line on every 401, past a test that
  asserted "at least one" line and was therefore blind to it.

**Source files:** `src/include/flapi_app.hpp`, `src/request_context_middleware.cpp`,
`src/cors_middleware.cpp`, `src/auth_middleware.cpp`, `src/rate_limit_middleware.cpp`

---

## 7. JSON-RPC for MCP Transport

**Decision:** Use JSON-RPC 2.0 for MCP protocol transport.

**Rationale:**
- MCP specification requires JSON-RPC 2.0
- Simple request/response pattern
- Well-defined error handling
- Language-agnostic

**Implementation:**
```cpp
// src/mcp_route_handlers.cpp - method dispatch
MCPResponse dispatchMCPRequest(const MCPRequest& request) {
    if (request.method == "initialize") return handleInitializeRequest(request);
    if (request.method == "tools/list") return handleToolsListRequest(request);
    if (request.method == "tools/call") return handleToolsCallRequest(request);
    // ...
}
```

**Tradeoffs:**
- (+) Protocol compliance
- (+) Structured error responses
- (+) Session management support
- (-) More verbose than REST
- (-) Requires JSON-RPC parsing layer

**Source files:** `src/mcp_route_handlers.cpp`, `src/mcp_types.hpp`

---

## 8. Defense-in-Depth Security

**Decision:** Multiple layers of input protection.

**Rationale:**
- SQL injection is a critical risk
- No single protection is foolproof
- Validators and escaping complement each other

**Security layers:**
1. **Validators** - Whitelist validation (type, range, pattern)
2. **Triple braces** - Quote escaping in templates
3. **Parameterized queries** - Where applicable
4. **Input constraints** - Length limits, allowed characters

**Implementation:**
```yaml
request:
  - field-name: customer_name
    validators:
      - type: string
        max-length: 200
        pattern: "^[a-zA-Z0-9 ]+$"
```

**Tradeoffs:**
- (+) Multiple barriers to exploitation
- (+) Clear validation rules in configuration
- (+) Fails fast with descriptive errors
- (-) Can be overly restrictive
- (-) Validator configuration overhead

**Source files:** `src/request_validator.cpp`, `src/include/config_manager.hpp` (ValidatorConfig)

---

## 9. Self-Packaging via Appended ZIP

**Decision:** Bundle the config tree (`flapi.yaml` + endpoint YAMLs +
SQL templates + small data files) into the flapi executable itself,
producing a single self-contained artifact deployable via `scp`.
Four sub-decisions form the design:

### 9a. ZIP appended after the executable

**Why ZIP, not tar/7z/custom container:**

- The ZIP End-of-Central-Directory (EOCD) record is _designed_ to be
  found by a reverse scan from EOF -- that is precisely why ZIPs
  appended to SFX headers, AppImage payloads, etc. remain valid.
- ELF and PE loaders ignore trailing bytes after their own structures;
  the appended ZIP is invisible at execution time. (Mach-O is the
  awkward case -- see 9d.)
- `libarchive` reads and writes ZIPs from memory buffers without
  touching disk, and is already vcpkg-available.

**Why-not the alternatives:**

- **tar:** no EOCD-equivalent self-locator; we'd have to invent one.
- **7z:** modern C++ surface is nicer (`bit7z`), but the format
  doesn't tolerate arbitrary leading bytes as gracefully and lacks
  ZIP's reverse-scan property.
- **Custom container:** every byte of complexity is one we'd need to
  defend against drift. ZIP is documented, ubiquitous, debuggable
  with `unzip -l`.

**The spike caught one ZIP-specific gotcha:** libarchive's default
output rounds up to a 10240-byte tar-block boundary, padding with
zeros. That pushes the EOCD record off file-EOF and a naive reverse
scan misses it. Fix in `archive_io.cpp`:
```cpp
archive_write_set_bytes_in_last_block(a, 1);
```

### 9b. Reuse the existing `IFileProvider` abstraction

flapi already routes every file read through `IFileProvider`
(`src/include/vfs_adapter.hpp`), with two implementations: local disk
and DuckDB's VFS (for `s3://`, `gs://`, etc). Adding a third --
`EmbeddedArchiveFileProvider` -- serves config files straight from
the decompressed in-memory archive. The `FileProviderFactory` picks
it whenever a bundle is detected at startup.

**Why-not extract-to-tmpdir:**

- Race conditions on parallel `pack` invocations on the same host.
- File-permission surprises (umask, immutable tmpfs).
- Disk pressure on small container images.
- No way for SQL templates to reference files inside an extracted
  tree _by their bundle-relative path_, which we want for
  reproducible config.

The result: `ConfigLoader`, `SqlTemplateProcessor`, endpoint
handlers, and everything else read through `IFileProvider` exactly
as before. The factory's dispatch is the only seam.

### 9c. `embed://` DuckDB FileSystem for SQL-side reads

SQL templates often contain `read_csv('data/foo.csv')` or
`read_parquet('s3://...')`. These bypass `IFileProvider` entirely
and hit DuckDB's `VirtualFileSystem`. Registering a custom
`duckdb::FileSystem` for the `embed://` scheme makes
`read_csv('embed://data/foo.csv')` resolve to the same in-memory
ArchiveEntries map.

**Why a separate filesystem (vs. forcing every read through
IFileProvider):**

- DuckDB's CSV/Parquet readers stream data with seek-based access.
  Going through `IFileProvider::ReadFile` would materialise the
  whole file as a `std::string` first -- defeating streaming.
- The DuckDB `embed://` filesystem subclasses `duckdb::FileSystem`
  and serves bytes from the shared `ArchiveEntries` pointer
  directly, so DuckDB streams as if from a real file.

**Spike-caught requirement:** `Glob()` and `SeekPosition()` _must_
be overridden, not just `OpenFile` / `Read` / `Seek`. `read_csv()`
expands paths via `Glob` before opening; the base `FileSystem`
throws "not implemented" rather than no-oping.

### 9d. Reserved-segment + re-codesign on macOS

Mach-O is the awkward case: appending bytes after `__LINKEDIT`
invalidates the codesign signature, breaking notarisation.

**Decision:** allocate a placeholder `__FLAPI/__bundle` section at
link time (16 MiB default, configurable via
`FLAPI_RESERVED_BUNDLE_MIB`). `flapi pack` overwrites the section
in place rather than appending, then invokes `codesign --force
--sign $CODESIGN_IDENTITY` (defaulting to `-` for ad-hoc) so the
signature covers the new bytes.

**Why-not:** the trailing-append-and-resign approach used by the
spike works for ad-hoc use but produces a binary that fails
notarisation. The reserved-segment approach is the
industry-standard fix used by AppImage, PyInstaller, Wails.

**Tradeoffs:**
- (+) Single artifact deploys via `scp`; container images shrink
  (`COPY sqls/` step removed).
- (+) Reproducible builds (`SOURCE_DATE_EPOCH` + sorted entries) --
  CI asserts `sha256(pack(x)) == sha256(pack(x))`.
- (+) Existing operators see _zero_ change -- if no bundle is
  appended, all code paths fall back to filesystem mode.
- (-) Bundled binaries are immutable; live edits require a rebuild.
- (-) Reserved-segment size is a hard cap at link time (default
  16 MiB; rebuild with a larger value if needed).
- (-) Universal/fat Mach-O binaries are out of scope (parser handles
  thin 64-bit only); release artifacts are per-architecture thin.

**Source files:** `src/archive_io.cpp`, `src/bundle_locator.cpp`,
`src/selfpath.cpp`, `src/embedded_archive_file_provider.cpp`,
`src/duckdb_embed_fs.cpp`, `src/macho_bundle.cpp`, `src/pack.cpp`

**Tests:** 35+ Catch2 unit tests across the listed source files;
8 pytest integration tests (`test/integration/test_self_packaging.py`);
4 macOS-specific tests (skipped on non-Darwin); CI smoke jobs on all
four supported platforms (#49 / `.github/workflows/build.yaml`).

---

## 10. OpenTelemetry Observability

**Decision:** Emit W3C-correlated OpenTelemetry traces for every request, sharing
one request identity with the audit log and the application log, off by default.

**Rationale:**
- flAPI advertises MCP revision `2026-07-28`, which via SEP-414 reserves
  `traceparent`/`tracestate`/`baggage` in `params._meta`. flAPI parsed `_meta` and
  *discarded* the trace context conforming clients already sent. That is a
  conformance defect, not a feature request.
- As an ordinary HTTP service flAPI was invisible: no server spans, no RED
  metrics, no way to answer "which request was slow".
- Four uncorrelated mechanisms (PostHog telemetry, audit JSONL, Crow logs, Arrow
  metrics) and three separate clocks timed overlapping work with no shared id.

**Tradeoffs:**
- (+) One span tree, joinable to audit and logs by a shared id
- (+) flAPI stops being a trace terminator for the services it calls
- (-) +4.9 MiB binary, and protobuf/abseil in the link
- (-) A second consent model to explain alongside product telemetry

**Source files:** `src/flapi_tracing.cpp`, `src/trace_scope.cpp`,
`src/request_context_middleware.cpp`, `src/trace_context.cpp`,
`src/trace_capture_policy.cpp`, `src/redaction.cpp`

**Tests:** `test/cpp/test_trace_*.cpp`, `test/integration/test_tracing_*.py`

---

### 10a. Use the OpenTelemetry C++ SDK, not a hand-rolled OTLP client

**Decision:** Depend on `opentelemetry-cpp` rather than writing OTLP over the
already-linked libcurl.

**Rationale:**
- The wire format is the easy part. `BatchSpanProcessor`, retry and backoff,
  samplers, W3C propagators and the Recordable model are what a hand-rolled
  pipeline gets wrong, quietly, under load.
- A custom `SpanExporter` layered on the SDK would *not* have removed abseil: the
  vcpkg port depends on it unconditionally. Only dropping the SDK removes it, and
  that trade was not worth the correctness risk.

**Tradeoffs:**
- (+) Sampling, batching and retry are somebody else's tested code
- (-) protobuf + abseil enter a link that also contains static DuckDB
- (-) abseil's exported `cxx_std_20` requirement drove decision 10c

---

### 10b. Pin opentelemetry-cpp 1.24.0 through a vcpkg overlay port

**Decision:** Carry `ports/opentelemetry-cpp/` in-repo rather than bumping the
vcpkg baseline.

**Rationale:**
- The pinned baseline offers 1.17.0, which **does not compile**: 136 of its
  headers use fixed-width integer types without including `<cstdint>`. Verified
  failing on GCC 13 in the CI image, not only on a newer local toolchain.
- 1.24.0 also provides the `otlp-file` feature, which is what makes the
  air-gapped topology a shipped feature rather than a flAPI-specific exporter we
  would have had to write and maintain.
- An overlay port changes one dependency. Moving the baseline changes all of them,
  in a release that was not asking for it.

**Implementation:**
```json
// vcpkg-configuration.json
{ "overlay-ports": ["./ports"] }
```

**Tradeoffs:**
- (+) One dependency moves; the rest of the baseline is untouched
- (+) `otlp-file` without a bespoke exporter
- (-) An in-repo port to maintain, with a pinned SHA512 and a patch

---

### 10c. C++20 project-wide, with DuckDB pinned to C++17

**Decision:** Build flAPI at C++20 and DuckDB's subdirectory at C++17, saving and
restoring `CMAKE_CXX_STANDARD` around it.

**Rationale:**
- abseil sets `ABSL_PROPAGATE_CXX_STD`, exporting
  `INTERFACE_COMPILE_FEATURES "cxx_std_20"`. That raised **only** `flapi-lib`,
  while tests and `main` stayed at C++17 — and consuming C++20-built abseil from
  C++17 is an ABI split. It presented as a **segfault** in `auth_middleware_test`
  reading `endpoint->auth.type`, not as a compile error. One standard everywhere
  was the fix; `scripts/check_cxx_standard_uniform.sh` guards it.
- But C++20 removed `std::uncaught_exception()`, which DuckDB calls at
  `src/common/exception.cpp:55` behind `#if __cplusplus >= 201703L`. MSVC reports
  `__cplusplus` as `199711L` without `/Zc:__cplusplus`, so the guard passes and
  the call does not compile. Windows CI broke on exactly this.

**Implementation:**
```cmake
# CMakeLists.txt - DuckDB is built at C++17, NOT flAPI's C++20
set(FLAPI_CXX_STANDARD ${CMAKE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD 17)
add_subdirectory(duckdb EXCLUDE_FROM_ALL)
set(CMAKE_CXX_STANDARD ${FLAPI_CXX_STANDARD})
```

**Why-not the alternatives:**
- *Pass `/Zc:__cplusplus` to DuckDB on MSVC* — keeps one standard everywhere,
  which is genuinely tidier. Rejected for now only because it changes DuckDB's
  own compilation on one platform; worth revisiting.
- *Stay at C++17 and suppress abseil's propagation* — puts back the exact ABI
  split that segfaulted.

**Tradeoffs:**
- (+) Windows builds; abseil's requirement is satisfied uniformly for flAPI
- (-) Two standards in one binary. flAPI does reach DuckDB's **C++** API — it
  includes `duckdb.hpp` and subclasses `duckdb::FileSystem` for `embed://` — so
  this boundary carries real vtables, not just C calls. It is believed safe
  because standard-library layouts are stable across `-std` levels and DuckDB's
  headers contain no `__cplusplus`-gated members, but it is a boundary that has
  already produced one segfault in this epic. The `embed://` tests that cross it
  run on all platforms deliberately.

---

### 10d. No OpenTelemetry type in any header

**Decision:** `SpanScope` is an opaque, pointer-sized handle; `opentelemetry-cpp`
links `PRIVATE`.

**Rationale:**
- `otlp-http` drags in protobuf and abseil. Letting those into a header that
  `config_manager.hpp` includes recompiles ~80 translation units plus the test
  binary, and grows the `-Wl,--no-undefined` link surface.
- `PRIVATE` is the sole exception to that CMake block's `PUBLIC` convention, and
  is what makes the quarantine real rather than a convention.

**Implementation:**
```cpp
// src/include/trace_scope.hpp
class SpanScope {
    struct Impl; Impl* impl_ = nullptr;   // sizeof == sizeof(void*)
};
```

**Tradeoffs:**
- (+) One TU rebuilds when the OTel version changes, not eighty
- (+) `FLAPI_WITH_TRACING=OFF` becomes a real build, not a wish
- (-) An extra indirection, and a hand-written twin to keep in step
- (-) `setAttr` needs an explicit `const char*` overload; without it every string
  literal binds to the `bool` overload and exports as `true`

---

### 10e. Copy-on-write endpoint table

**Decision:** Hold endpoints as `shared_ptr<const vector<EndpointConfig>>`, swapped
atomically on reload; readers pin a snapshot through `EndpointRef`.

**Rationale:**
- This fixed a **real use-after-free**. `getEndpointForPathAndMethod` returned a
  raw pointer into a `std::vector` that `refreshConfig` and the config-service
  routes cleared and re-`push_back`-ed with no lock against in-flight requests.
- Tracing would have widened that window from inside one handler to across
  `before_handle → handler → after_handle`, so it had to be fixed first, not
  alongside.
- It is also the precondition that makes `RequestContext`'s `string_view` fields
  safe.

**Tradeoffs:**
- (+) Readers never observe a mutating table; reload needs no reader lock
- (+) A reload costs one vector copy, on a path that is not hot
- (-) `EndpointRef` is `[[nodiscard]]` and **must be bound to a named variable**.
  Used as a temporary, the snapshot releases at the end of the full expression
  and the pointer dangles — the very bug it exists to prevent.

---

### 10f. The `tracing:` block is boot-only

**Decision:** Tracing configuration is read once at startup and never re-read.

**Rationale:**
- The provider owns a processor thread and an exporter; swapping them under live
  traffic is a source of races for no real operator benefit.
- `getAuditLogger()` already ignored reloads, so boot-only matches the neighbouring
  behaviour instead of inventing a second rule.
- `ConfigManager::refreshConfig()` throws `Not implemented` today in any case.

**Tradeoffs:**
- (+) No lifecycle races on the provider, policy or middleware snapshots
- (-) Changing an exporter endpoint needs a restart, and the docs must say so
  plainly or operators will assume otherwise

---

## Summary

These design decisions prioritize:
1. **Simplicity** - Declarative configuration over complex code
2. **Security** - Defense-in-depth against injection attacks
3. **Flexibility** - Support multiple protocols from unified config
4. **Maintainability** - Clear separation of concerns
5. **Performance** - Caching and efficient query execution
6. **Deployability** - One artifact ships everything (#9 self-packaging)
7. **Observability** - One request identity across traces, audit and logs (#10)

For implementation details, see the [component documentation](./components/).
