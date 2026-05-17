# Changelog

All notable changes to flAPI are documented here. Versions follow `vYY.MM.DD` (the date the binary set was cut). Earlier history is in the git log.

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
