<!--
Target: r/DuckDB
Timing: Week 3, same day as the Show HN (playbook §8 — launch-friendly,
"small but perfectly targeted; near-zero competition here" per §3).
Prerequisites: some genuine prior participation helps but this sub is
friendly to show-posts about things built on DuckDB. Stay in the comments.
-->

# Title

I built a REST + MCP server on top of DuckDB — single static binary, SQL templates in, API out

# Body

Disclosure: I'm one of the authors.

If you use DuckDB, you already know the trick: one engine, 50+ sources —
Parquet/CSV, Postgres, BigQuery, S3/GCS/Azure, Iceberg, Delta. flAPI puts
an HTTP server in front of that. You write a Mustache-templated SQL file
plus a small YAML config, and it becomes a REST endpoint and an MCP tool
(same definition, both servers run concurrently). No Python, no glue code.

```sql
-- customers.sql
SELECT * FROM '{{{conn.path}}}'
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{ params.id }}
{{/params.id}}
```

DuckDB-specific bits this sub might care about:

- **Embedded DuckDB 1.5.3**, statically linked into a single C++17 binary.
  Extensions (postgres_scanner, httpfs, bigquery, iceberg, ...) load per
  connection config.
- **Typed params → prepared statements.** Request parameters with typed
  validators are rewritten to `?` placeholders and bound via DuckDB
  prepared statements — no string interpolation for those sites, so
  injection is structurally impossible there. (Raw `{{{ }}}` sites and
  untyped params still rely on validators + template discipline.)
- **DuckLake-backed caching.** Endpoint results materialize into a
  DuckLake catalog with scheduled full or incremental refresh (cursor
  and/or primary-key driven merge) and snapshot time-travel. Your endpoint
  SQL reads `{{cache.catalog}}.{{cache.schema}}.{{cache.table}}` instead
  of re-hitting BigQuery on every request.
- **`embed://` filesystem.** `flapi pack` can fold your whole config tree
  (YAML + SQL + small data files) into the binary; a custom DuckDB
  filesystem serves the bundle from memory, so
  `read_csv('embed://data/cities.csv')` just works. `scp` the binary,
  that's the deploy.
- There's also an SAP ERP/BW path via the ERPL extension (working example
  in the repo), though that extension has documented stability caveats,
  so I won't oversell it.

Try it:

```
uvx --from flapi-io flapi -c flapi.yaml
```

(`flapi` was taken on PyPI, hence `flapi-io`. Prebuilt binaries for Linux
x86_64/ARM64, macOS ARM64, Windows, plus Docker.)

**Limitations:**

- Read-oriented data APIs — not a CRUD backend.
- MCP is over Streamable HTTP only (no stdio transport; stdio clients
  need a proxy like mcp-remote).
- License is BSL 1.1 — source-available, not open source. Production use
  is permitted; reselling flAPI as a hosted service isn't. Each version
  converts to MPL-2.0.
- Young project; the DuckDB core paths are solid, the edges less so.

Repo: https://github.com/DataZooDE/flapi

Would genuinely love feedback from people who've pushed DuckDB extensions
or DuckLake harder than we have — especially around incremental merge
patterns.
