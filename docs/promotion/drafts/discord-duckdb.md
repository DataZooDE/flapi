<!--
Target: DuckDB Discord, #show-and-tell channel
Timing: Week 1 of the launch sequence (playbook §8 — "seed quietly";
per §2 this is "the friendliest room on this list").
Prerequisites: be a participant in the server, not a drive-by poster;
answer follow-ups in-channel. Keep it casual and short — this is a chat
message, not a launch post.
-->

# Message

Hey all — I'm one of the authors of flAPI and wanted to share it here
since it's built entirely on DuckDB (1.5.3, statically embedded). It turns
a SQL file + a small YAML into a REST endpoint *and* an MCP tool from the
same config — so anything DuckDB can read (Parquet, Postgres, BigQuery,
S3, Iceberg, ...) becomes an API without writing a backend. Single C++
binary, no runtime deps.

A minimal endpoint is just this pair:

```yaml
# sqls/customers.yaml
url-path: /customers/
request:
  - field-name: id
    field-in: query
    validators:
      - type: int
        min: 1
template-source: customers.sql
connection: [customers-parquet]
```

```sql
-- sqls/customers.sql
SELECT * FROM '{{{conn.path}}}'
WHERE 1=1
{{#params.id}} AND c_custkey = {{ params.id }} {{/params.id}}
```

Typed params like that `id` get bound as DuckDB prepared statements rather
than interpolated. Caching runs on DuckLake (full/incremental refresh,
snapshot time-travel), and there's an `embed://` DuckDB filesystem so
`flapi pack` can bake data files into the binary itself. Quickest try:
`uvx --from flapi-io flapi -c flapi.yaml`. Fair warning: it's read-oriented
APIs only, MCP is HTTP-transport only, license is BSL 1.1
(source-available), and it's young — feedback and issues very welcome:
https://github.com/DataZooDE/flapi
