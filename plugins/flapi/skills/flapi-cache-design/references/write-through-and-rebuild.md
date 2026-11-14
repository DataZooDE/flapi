# Write-through: invalidate on write

The write endpoint gets its **own** `cache:` block naming the same table and
populate template. After a successful POST/PUT/PATCH, flAPI re-runs that
populate SQL synchronously, before it answers. REST and MCP tool calls both do
this. `refresh-on-write` has no effect.

```yaml
# flapi: config group=cache-items
project-name: items-write-through
project-description: Cache refreshed after every write
template:
  path: ./sqls
connections:
  app:
    init: |
      CREATE TABLE IF NOT EXISTS items (id INTEGER, name VARCHAR);
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
```

```yaml
# flapi: endpoint group=cache-items file=items.yaml
url-path: /items
template-source: items.sql
connection: [app]
cache:
  enabled: true
  table: items_cache
  template-file: items_cache.sql
# flapi: check GET /items -> 200
```

```yaml
# flapi: endpoint group=cache-items file=items-create.yaml
url-path: /items
method: POST
template-source: items-create.sql
connection: [app]
operation:
  type: Write
request:
  - field-name: name
    field-in: body
    required: true
    validators:
      - type: string
        regex: "^[a-z]+$"
cache:
  enabled: true
  table: items_cache
  template-file: items_cache.sql
  invalidate-on-write: true
# flapi: check POST /items body={"name":"widget"} -> 201
# flapi: check GET /items -> 200
```

```sql
-- flapi: template group=cache-items file=items_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, name FROM items;
```

```sql
-- flapi: template group=cache-items file=items.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}} ORDER BY id
```

```sql
-- flapi: template group=cache-items file=items-create.sql
INSERT INTO items SELECT COALESCE(MAX(id), 0) + 1, '{{{ params.name }}}' FROM items
```

- The write's latency now includes the populate SQL. With a big table use an
  incremental template, or drop write-through and refresh on a schedule.
- Every endpoint whose `cache:` names the table is warmed at startup, so the
  table is built once per such endpoint. Put `schedule` on one of them only.
- A write endpoint with a `cache:` block is also gated: it answers 503 while that
  cache has never been built.

## Forcing a full rebuild

Put a rebuild branch on a whitelisted environment variable
(`template.environment-whitelist: ['^FLAPI_.*']`) and run the one-shot refresh
with it set. Environment variables are read at render time, so this works for
`flapi cache refresh`; a running server only sees its own startup environment.

```sql
{{#env.FLAPI_CACHE_REBUILD}}
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, name, tier, updated_at, is_deleted FROM read_csv('{{{conn.path}}}');
{{/env.FLAPI_CACHE_REBUILD}}
{{^env.FLAPI_CACHE_REBUILD}}
-- ... the usual {{^cache.previousSnapshotTimestamp}} / {{#...}} template ...
{{/env.FLAPI_CACHE_REBUILD}}
```

```bash
FLAPI_CACHE_REBUILD=1 flapi cache refresh -c flapi.yaml --endpoint /customers
```

On a running server: `flapii cache update-template <path> --file rebuild.sql`
(a CTAS-only file), `flapii cache refresh <path>`, then put the normal template
back the same way. The audit row still says `merge`/`append`: the mode label
comes from the YAML, not from what the SQL did.

