<!--
Target: r/mcp and r/modelcontextprotocol (post separately, adapt intro line)
Timing: Week 3, same day as the Show HN (playbook §8 — these subs are
launch-friendly, per §3 the fastest-growing relevant audience).
Prerequisites: account should have 2-3 weeks of genuine participation in
MCP threads first (answering questions, not promoting). Stay in the
comments after posting.
-->

# Title

I built a server that turns any SQL query into an MCP tool — with per-tool RBAC

# Body

Disclosure: I'm one of the authors.

The pattern I kept seeing: teams want to give an LLM access to warehouse
data, so someone writes a bespoke Python MCP server per use case — and now
you own auth, validation, rate limiting, and a deployment for each one.

flAPI's approach: you write a SQL file (Mustache-templated) and a YAML
config, and you get an MCP tool *and* a REST endpoint from the same
definition. DuckDB is embedded, so the SQL can query Parquet/CSV, Postgres,
BigQuery, S3/GCS/Azure, Iceberg, Delta — anything in DuckDB's 50+ source
ecosystem.

```yaml
mcp-tool:
  name: get_customers
  description: Retrieve customer information by ID
request:
  - field-name: id
    field-in: query
    validators:
      - type: int
        min: 1
template-source: customers.sql
connection: [customers-parquet]
```

The governance bits, which I think matter more for MCP than for REST
because the caller is a model:

- **Per-tool RBAC, fail-closed.** Tools carry `allowed-roles`; roles come
  from Basic auth users or JWT/OIDC claims. Under auth, a tool with no
  allowed-roles is denied — not silently allowed.
- **Typed parameters become prepared statements.** Params with typed
  validators (int, date, uuid, enum, ...) are rewritten to `?` placeholders
  and bound as DuckDB prepared statements, so injection is structurally
  impossible for those sites. (Untyped params and raw `{{{ }}}`
  interpolation still rely on validators + template discipline — I want to
  scope that claim honestly.)
- **Tool-description hygiene scanner.** At config load time it scans your
  MCP tool descriptions for prompt-injection patterns — protection against
  poisoned/copy-pasted configs. To be clear, it does *not* scan live
  traffic; it's a linter for tool descriptions, not a runtime firewall.
- Response shaping per tool (max-rows, redact-columns, sampling), per-tool
  rate limits, and a dry-run mode.

Caching is DuckLake-based (full or incremental refresh, snapshot
time-travel), so a tool over a slow BigQuery query can serve from a local
snapshot instead of hammering the warehouse on every model call.

Try it (single static binary, C++, DuckDB embedded):

```
uvx --from flapi-io flapi -c flapi.yaml
```

(Package is `flapi-io` — `flapi` was taken on PyPI. Binaries for
Linux x86_64/ARM64, macOS ARM64, Windows, and Docker also exist.)

**Limitations, honestly:**

- MCP transport is Streamable HTTP only (`/mcp/jsonrpc`, SSE for
  streaming). **No stdio** — Claude Desktop and other stdio-only clients
  need a proxy like mcp-remote.
- Read-oriented data tools; this is not a general CRUD/action backend.
- DuckDB-centric: if DuckDB can't reach your source, neither can flAPI.
- License is BSL 1.1 (source-available, not open source). Production use
  is permitted; offering flAPI itself as a hosted service isn't. Converts
  to MPL-2.0 over time.
- Young project — expect rough edges, file issues.

Repo: https://github.com/DataZooDE/flapi

Happy to answer anything about the RBAC model or the prepared-statement
binding — those were the two design areas where "the caller is an LLM"
changed our decisions the most.
