# Client Parity Audit — flapii CLI & VSCode Extension vs. flapi Server

**Date:** 2026-06-14
**Server reference:** tag `v26.06.13` (binary tested: `build/release/flapi`, built 2026-06-11, self-reported version `1.0`)
**flapii CLI:** `cli/` — version `0.1.0`
**VSCode extension:** `cli/vscode-extension/` — version `0.1.0`
**Shared package:** `cli/shared/` (`@flapi/shared`) — version `0.1.0`

Scope: **audit + verify only** — no feature changes were made. The clients were
built and exercised against a live server; results below are backed by either a
live request or a source citation.

---

## TL;DR

| Client | Builds? | Tests | Live behaviour | On par with server? |
|--------|---------|-------|----------------|---------------------|
| **flapii CLI** | ✅ yes (tsup) | ✅ 41/41 unit pass; ⚠️ integration blocked by server-binary crash (not a CLI bug) | ✅ all exercised commands work against live server | **Mostly** — works, but missing ~6 commands the server now supports |
| **VSCode extension** | ❌ **NO** — webpack build fails | ❌ no test suite exists | ⛔ cannot be packaged/run | **No** — broken in-repo since 2025-11-02 |

**Headline:** The CLI is healthy and largely keeps up with the server, with a
handful of missing commands. The **VSCode extension does not compile** and has not
compiled since its last commit (`fcebf3a`, "First draft of write support",
2025-11-02) — it is effectively unshippable today.

---

## 1. Build / Run status (evidence)

### Server
- `build/release/flapi` boots with `--config-service` and serves the full ConfigService
  API (verified by live HTTP probes below).
- **Caveat 1 (environment):** the bundled `examples/data/cache.ducklake` is a stale
  DuckLake catalog (version 0.3 vs extension 1.0) and aborts startup
  (`DuckLake catalog version mismatch`). Worked around by moving it aside (restorable
  backup in `/tmp/flapi-ducklake-bak/`).
- **Caveat 2 (instability):** under the CLI integration harness the server binary
  crashes on startup with **varying signals** (SIGSEGV / SIGABRT / SIGBUS). The same
  binary runs fine when launched manually with the same config, so this looks like
  binary/environment staleness — the working tree has a **modified `duckdb` submodule**
  (`git status: M duckdb`) that the 2026-06-11 prebuilt binary predates. Not a client defect.

### flapii CLI
- `npm ci` ✅, `npm run build` (tsup) ✅ — `ESM build success`.
- `npm test` (Vitest, nock-mocked): **41/41 pass** (8 files). *(One spurious failure only
  appears if `FLAPI_BASE_URL` is exported into the test env — a test-isolation quirk in
  `test/unit/config.spec.ts`, not a product bug.)*
- `npm run test:integration`: **blocked** — `test/integration/setup.ts` spawns the real
  `build/release/flapi`, which crashes (see Caveat 2). 19 tests skipped, 0 ran. The harness
  itself is correct; it is gated on a working server binary.
- **Live manual smoke test** against a manually-launched server (`:8099`, `--config-service`):
  `ping`, `endpoints list`, `endpoints get`, `templates get`, `templates expand`,
  `cache get`, `schema connections`, `config log-level get`, `mcp tools list` — **all exit 0
  and return correct data.**

### VSCode extension
- `npm ci` ✅.
- `npm run build` (webpack ext + webview): ❌ **FAILS, exit 1.**
- `npx tsc --noEmit`: ❌ fails (same errors).
- Two error classes, both in **committed** code (no local modifications), both from commit
  `fcebf3a` (2025-11-02):
  1. `src/providers/endpointsProvider.ts:60,61,79,80` —
     `TS2339: Property 'operation' does not exist on type 'EndpointConfig'` (type drift: the
     provider reads `endpoint.operation`, which the `EndpointConfig` type no longer declares).
  2. `src/webview/endpointTesterPanel.ts:1895–2030` — cascade of `TS1005 / TS1110 / TS1127`
     ("`;` expected", "Type expected", "Invalid character"). A malformed/un-terminated
     template literal in the "write operation result" block desyncs the parser. This is the
     half-finished "write support" draft.

---

## 2. Parity matrix (server capability → client coverage)

Legend: ✅ exposed · ⚠️ partial / indirect · ❌ not exposed
The extension column reflects **source intent**; remember the extension **does not build**, so
every ✅ there is currently non-functional.

| Server capability (ConfigService / MCP) | Server route | flapii CLI | VSCode ext |
|---|---|---|---|
| Project config (get) | `GET /_config/project` | ✅ `project get` / `ping` | ✅ openProject |
| Project config (update) | `PUT /_config/project` | ❌ | ❌ — *(server returns 501; not implemented)* |
| Environment variables | `GET /_config/environment-variables` | ❌ | ⚠️ client method exists; UI ("Variables") removed |
| Endpoints — list | `GET /_config/endpoints` | ✅ `endpoints list` | ✅ explorer |
| Endpoints — get | `GET /_config/endpoints/{slug}` | ✅ `endpoints get` | ✅ |
| Endpoints — create/update/delete | `POST/PUT/DELETE …/{slug}` | ✅ `endpoints create/update/delete` | ⚠️ create only (`newEndpoint`) |
| Endpoints — validate | `POST …/{slug}/validate` | ✅ `endpoints validate` | ✅ validateYaml |
| Endpoints — reload | `POST …/{slug}/reload` | ✅ `endpoints reload` | ✅ reloadEndpoint |
| Endpoints — parameters | `GET …/{slug}/parameters` | ❌ | ✅ openParameters |
| Endpoints — by-template | `POST …/by-template` | ❌ | ✅ (used by SQL tester) |
| Template — get/update | `GET/PUT …/{slug}/template` | ✅ `templates get/update` | ✅ |
| Template — expand | `POST …/{slug}/template/expand` | ✅ `templates expand` | ✅ expandTemplate |
| Template — test (exec) | `POST …/{slug}/template/test` | ✅ `templates test` | ✅ testTemplate |
| Cache — get/update | `GET/PUT …/{slug}/cache` | ✅ `cache get/update` | ✅ openCache |
| Cache — template get/update | `GET/PUT …/{slug}/cache/template` | ✅ `cache template / update-template` | ⚠️ |
| Cache — refresh | `POST …/{slug}/cache/refresh` | ✅ `cache refresh` | ❌ |
| **Cache — GC** | `POST …/{slug}/cache/gc` | ❌ | ❌ |
| **Cache — audit (per-endpoint)** | `GET …/{slug}/cache/audit` | ❌ | ❌ |
| **Cache — audit (global)** | `GET /_config/cache/audit` | ❌ | ❌ |
| Schema — get | `GET /_config/schema` | ✅ `schema get/connections/tables` | ✅ schema browser |
| Schema — refresh | `POST /_config/schema/refresh` | ✅ `schema refresh` | ✅ schema.refresh |
| Log level — get/set | `GET/PUT /_config/log-level` | ✅ `config log-level get/set/list` | ❌ |
| **Filesystem tree** | `GET /_config/filesystem` | ❌ | ⚠️ client method exists |
| **Health / metrics** | `GET /_config/health` | ❌ (`ping` hits `/project`) | ❌ |
| OpenAPI doc | `GET /doc.yaml` | ❌ | ❌ |
| MCP tools/resources/prompts (user-defined) | `GET /_config/endpoints` (MCP slugs) | ✅ `mcp tools/resources/prompts list/get` | ✅ openMcpItem |
| MCP JSON-RPC protocol (`/mcp/jsonrpc`, `/mcp/health`) | — | ❌ (no MCP client/exec) | ❌ |
| 20 `flapi_*` MCP config tools (`--config-service`) | via `/mcp/jsonrpc` | ➖ N/A (consumed by MCP clients, not the CLI) | ➖ N/A |
| Self-packaging (`pack`/`info`/`unpack`) | server-binary subcommands | ➖ N/A (server binary, not flapii) | ➖ N/A |

All "❌ on server route" cells were confirmed live: the route returns **200** while the CLI
reports `error: unknown command` (verified for `cache gc`, `cache audit`, `environment-variables`,
`filesystem`, `health`).

---

## 3. Findings, prioritized

### P0 — Broken / unshippable
1. **VSCode extension does not compile.** Webpack + `tsc` both fail. Two root causes
   (`endpointsProvider.ts` type drift on `operation`; `endpointTesterPanel.ts` malformed
   template literal in the write-support draft). Broken in-repo since 2025-11-02. The extension
   cannot be packaged or run until fixed. *(Files: `cli/vscode-extension/src/providers/endpointsProvider.ts:60-80`,
   `cli/vscode-extension/src/webview/endpointTesterPanel.ts:1895-2030`.)*

### P1 — Functional parity gaps (server has it; clients don't)
2. **CLI missing commands** for live server capabilities: `cache gc`, `cache audit`
   (per-endpoint + global), `endpoints parameters`, `environment-variables`, `filesystem`,
   and a real `health` command (today `ping` queries `/project`, not `/health`, so it never
   surfaces DB / Arrow-IPC / VFS / credential status).
3. **Dead/broken client method:** `FlapiApiClient.testEndpoint()`
   (`cli/shared/src/apiClient.ts:262`) POSTs to `/_config/endpoints/{slug}/test`, which **does
   not exist** on the server (only `/template/test` does, `config_service.cpp:393`). Any caller
   would get a 404. The CLI's `templates test` correctly targets `template/test`, so the method
   is currently unused — but it is a latent landmine.

### P2 — Architectural drift (violates CLAUDE.md "share a common API client")
4. **Two divergent `FlapiApiClient` implementations.** `cli/shared/src/apiClient.ts` (axios,
   full surface, used by the CLI) vs. `cli/vscode-extension/src/shared/apiClient.ts` (separate
   impl, validate/reload/template subset). The mandated single shared client does not exist in
   practice; the extension does not consume `@flapi/shared`'s client.
5. **Dead code in the extension:** the "Variables provider" was removed but its refresh call /
   wiring remains (`extension.ts` ~898-908); unused `include_variables` fetch.
6. **Stubbed TODOs in the extension:** single-YAML backend parse
   (`codelens/endpointTestProvider.ts:186`), multi-endpoint selector
   (`webview/sqlTemplateTesterPanel.ts:118`).

### P3 — Hygiene / staleness
7. **No test suite for the extension** (no framework, no specs) — the broken build would have
   been caught by even a smoke compile in CI.
8. **Version skew:** server `v26.06.13`; all three JS packages pinned at `0.1.0`. Extension
   untouched ~7 months; CLI's last feature work 2026-01-16.
9. **Examples ship a stale DuckLake catalog** that aborts a fresh server boot (env, but
   user-facing for anyone running `examples/`).

---

## 4. Verdict & minimum work to reach parity

**flapii CLI — "on par": mostly yes.** It builds, its unit tests pass, and every exercised
command works against a live server. To call it fully on par, add the six missing commands in
P1 #2 (the shared client already has methods for `parameters`, `environment-variables`,
`filesystem`), fix the `testEndpoint()` route in #3, and wire a real `health` command.

**VSCode extension — "on par": no.** It is broken at the build level and cannot run. Minimum to
restore: fix the two compile errors (P0 #1), then re-assess parity — on paper its feature set
is actually *wider* than the CLI's (parameters, by-template, interactive testers), but none of
it ships until it compiles. Strongly recommend adding a CI compile/typecheck gate (P3 #7) and
collapsing onto the single shared API client (P2 #4).

---

## 5. Resolution

The P0/P1 findings were filed and fixed in a single PR:

- **#75** (P0) — VSCode extension now builds: escaped the nested template literals in
  `endpointTesterPanel.ts`, and made `@flapi/shared` build automatically (`prepare` on shared,
  `prebuild` on the extension) so its types can't go stale. `npm run build` + `tsc --noEmit` both pass.
- **#76** (P1) — added the missing CLI commands: `health`, `config env`, `config filesystem`,
  `endpoints parameters <path>`, `cache audit [path]`, `cache gc <path>`. All verified against a
  live server.
- **#77** (P1) — `FlapiApiClient.testEndpoint()` now targets the real `…/template/test` route.

P2/P3 items (single shared API client, extension test suite + CI compile gate, version alignment,
stale example DuckLake catalog) remain open follow-ups, intentionally out of this PR's scope.

## Reproduction notes
- Server (manual): `./build/release/flapi -c examples/flapi.yaml --port 8099 --config-service --config-service-token testtoken123`
  (after moving `examples/data/cache.ducklake*` aside).
- CLI: `cd cli && npm ci && npm run build && npm test`; live: `FLAPI_BASE_URL=… FLAPI_CONFIG_SERVICE_TOKEN=… node dist/index.js <cmd>`.
- Extension: `cd cli/vscode-extension && npm ci && npm run build` (fails) / `npx tsc --noEmit` (fails).
