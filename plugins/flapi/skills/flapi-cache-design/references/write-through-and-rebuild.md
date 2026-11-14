# Write-through: refresh the cache after a write

The write endpoint gets its **own** `cache:` block naming the same table and
populate template. After a successful POST/PUT/PATCH/DELETE flAPI re-runs that
populate SQL:

- `invalidate-on-write: true`: synchronously, before it answers. The caller
  reads its own write on the next GET.
- `refresh-on-write: true`: after the response, in the background; refreshes
  requested while one runs are coalesced per endpoint. The next GET may still
  see the old data for the length of the refresh.
- Both set: synchronous only. REST and MCP tool calls follow the same rules.

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

A `DELETE` endpoint runs its template through the same write path (validation,
template, `{"rows_affected": n}` with 200). Here it refreshes after the response:

```yaml
# flapi: endpoint group=cache-items file=items-delete.yaml
url-path: /items/:id
method: DELETE
template-source: items-delete.sql
connection: [app]
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
cache:
  enabled: true
  table: items_cache
  template-file: items_cache.sql
  refresh-on-write: true
# flapi: check DELETE /items/1 -> 200
```

```sql
-- flapi: template group=cache-items file=items-delete.sql
DELETE FROM items WHERE id = {{ params.id }}
```

- The write's latency now includes the populate SQL. With a big table use an
  incremental template, or drop write-through and refresh on a schedule.
- Endpoints whose `cache:` names the same table share one warm-up and one
  schedule; put `schedule` on one of them.
- A write endpoint with a `cache:` block is also gated: it answers 503 while that
  cache has never been built.

## Forcing a full rebuild

```bash
flapii cache refresh /customers --full                             # running server
curl -X POST -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" \
  "http://localhost:8080/api/v1/_config/endpoints/-customers/cache/refresh?full=true"
flapi cache refresh -c flapi.yaml --endpoint /customers --full     # one-shot, no server
```

In one transaction the watermark is erased (so `{{cache.previousSnapshotTimestamp}}`
is absent and the `{{^...}}` CTAS branch renders) and `DROP TABLE IF EXISTS` runs
on the cache table, so even an append template that never replaces the table
starts empty. Readers keep the previous snapshot until the commit; a failed
rebuild leaves it in place.

- Use it after changing the populate SQL or the source schema, after a bad
  delta, and periodically for hard deletes a delta query cannot see.
- The audit row keeps the YAML's mode label (`append`/`merge`); the one-shot
  summary says `"full": true`.
- A template with its own `BEGIN`/`COMMIT` cannot share the transaction: there
  `--full` only clears the watermark (a warning says so), so its full branch
  must replace the table itself (`CREATE OR REPLACE`).
