# flAPI Telemetry

flAPI collects **anonymous, aggregate usage telemetry** to help us understand
which capabilities are used and where the product breaks. It is designed to be
privacy-preserving by construction: only bounded enumerations, route
*templates*, and numbers ever leave the machine — **never** your data, queries,
URLs, credentials, or configuration.

Telemetry is emitted against the shared DataZoo telemetry schema
(`telemetry_schema: 2`) via the [`posthog-telemetry`](https://github.com/DataZooDE/posthog-telemetry)
library, into PostHog's **EU** ingestion endpoint (`eu.i.posthog.com`). Because
flAPI is a long-running server (not a DuckDB extension), events carry
`install_kind = "server"` and a single session id per server uptime.

## How to turn it off

Any **one** of the following disables all telemetry — a single guard
short-circuits every event, and nothing leaves the machine:

- **Environment variable:** `DATAZOO_DISABLE_TELEMETRY=1` (also `true` / `yes`),
  or `FLAPI_NO_TELEMETRY=1`.
- **Config file** (`flapi.yaml`):
  ```yaml
  telemetry:
    enabled: false
  ```
- **CLI flag:** `flapi --no-telemetry`.

For very high-QPS deployments you can down-sample the per-request/per-tool
events (lifecycle events are always sent in full):

```yaml
telemetry:
  sample_rate: 0.1   # emit 10% of rest_endpoint_served / mcp_tool_called; events carry sample_rate
```

## What is collected

Every event carries a common envelope from the library: `product` (`flapi`),
`product_version`, `product_edition` (`oss`/`enterprise`), `telemetry_schema`,
`os`, `arch`, `platform`, `is_ci`, `is_container`, a per-uptime `$session_id`,
a pseudonymous per-machine `distinct_id` (salted SHA-256 of the OS machine id;
identifies a *machine/install*, not a person), and `$groups` once associated.
flAPI additionally stamps `install_kind = "server"` on every event.

### Events

| Event | When | Properties (beyond the envelope) |
|---|---|---|
| `server_started` | server boot | `endpoint_count` (number), `auth_kind` ∈ `none`\|`basic`\|`bearer`\|`oidc` |
| `rest_endpoint_served` | a REST endpoint is served | `method` (`GET`/`POST`/…), `route_template` (e.g. `/customers/:id` — **the template, never the filled path**), `status_class` ∈ `2xx`\|`3xx`\|`4xx`\|`5xx`, `duration_ms` (number), `cache_hit` (bool) |
| `mcp_tool_called` | an MCP tool call completes | `tool` (registered tool name), `status_class`, `duration_ms` (number) |
| `auth_enforced` | auth is enforced on a request | `auth_kind` ∈ `basic`\|`bearer`\|`oidc`, `outcome` ∈ `allow`\|`deny` |
| `$exception` | a request fails | `error_class` ∈ `server_error`\|`bad_request` (REST), `feature`, `route_template` (template) |

Sampled events additionally carry `sample_rate` (number) so aggregate counts
scale back up.

### Groups

- **`deployment`** — always associated at boot; key is the pseudonymous
  per-machine `distinct_id`. Powers active-deployment and retention analytics.
- **`account`** — associated only when a license id is present
  (`FLAPI_LICENSE_ID`); key is `sha256(license_id)`. The raw license id is never
  sent.

## What is **never** collected

By design, the following never appear in any property — call sites only pass
enums/templates/numbers, and the library additionally clamps every string
property to 512 bytes as a backstop:

- Filled request paths or query strings (only the **route template**).
- SQL text, rendered templates, or query plans.
- Request or response **bodies**, or any **row data**.
- HTTP **headers**, tokens, passwords, or connection strings.
- Table names, file paths, hostnames, or user names.
- Free-form error messages (only an enumerated `error_class`).

## Notes on specific fields

- **`cache_hit`**: flAPI's cache is a materialized DuckLake table that an
  endpoint's SQL queries against — there is no per-request "served-from-cache"
  flag. This field therefore reports whether the **endpoint is cache-backed**
  (`cache.enabled` with a cache table), not a true per-request hit/miss.
- **`$session_id`**: one id per **server uptime**, so PostHog Paths reflect what
  a deployment served during a single run. It is never tied to any end-user
  identity.

## Where this lives in the code

- Facade + gating: `src/flapi_telemetry.{hpp,cpp}` (`flapi::GlobalTelemetry()`).
- Boot / shutdown (server_started, groups, `Flush()` on SIGINT/SIGTERM):
  `src/main.cpp`.
- `rest_endpoint_served` + errors: `src/api_server.cpp` (`handleDynamicRequest`).
- `mcp_tool_called`: `src/mcp_tool_handler.cpp` (`executeTool`).
- `auth_enforced`: `src/auth_middleware.cpp` (`before_handle`).
- Tests (incl. a no-leak assertion against the real transport):
  `test/cpp/test_flapi_telemetry.cpp`.
