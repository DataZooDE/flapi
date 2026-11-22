# Time travel over a cache table

Every committed write to a cache table is a DuckLake snapshot. A serving template
can read an older one with `AT (VERSION => n)` or `AT (TIMESTAMP => ...)`. Retention
(`cache.retention`, `ducklake.retention`) expires old snapshots, after which they
can no longer be read.

Verified on flAPI with DuckDB 2.0.1 (DuckLake 1.1): a full-refresh cache refreshed five times
under `keep-last-snapshots: 2`.

## Endpoint with a version and a timestamp parameter

```yaml
# flapi: config group=timetravel
project-name: timetravel
project-description: Cache table read at an older snapshot
template:
  path: ./sqls
connections:
  items:
    properties:
      path: ./src/items.csv
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
```

```text
# flapi: file group=timetravel file=src/items.csv
id,name
1,a
2,b
```

```yaml
# flapi: endpoint group=timetravel file=items.yaml
url-path: /items
template-source: items.sql
connection: [items]
request:
  - field-name: version
    field-in: query
    required: false
    validators:
      - type: int
        min: 0
  - field-name: at
    field-in: query
    required: false
    validators:
      - type: string
        regex: '^\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(\.\d{1,6})?$'
cache:
  enabled: true
  table: items_cache
  template-file: items_cache.sql
  retention:
    keep-last-snapshots: 2
# flapi: check GET /items -> 200
# flapi: check GET /items?version=999999 -> 404
# flapi: check GET /items?at=2000-01-01%2000:00:00 -> 404
# flapi: check GET /items?at=2999-01-01%2000:00:00 -> 400
# flapi: check GET /items?at=yesterday -> 400
```

```sql
-- flapi: template group=timetravel file=items_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, name FROM read_csv('{{{conn.path}}}');
```

```sql
-- flapi: template group=timetravel file=items.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
{{#params.version}}AT (VERSION => {{ params.version }}){{/params.version}}
{{#params.at}}AT (TIMESTAMP => TIMESTAMP '{{{ params.at }}}'){{/params.at}}
ORDER BY id
```

- **The timestamp must be interpolated inside a section**, guarded by a strict regex.
  A bound parameter does not work: `AT (TIMESTAMP => CAST({{ params.at }} AS
  TIMESTAMP))` outside a section fails at prepare time with `No snapshot found at
  timestamp NULL` (a 500), because DuckLake resolves `AT` before parameters are bound.
- **A naive timestamp is UTC.** `?at=2026-11-14 19:15:33` means 19:15:33 UTC on a
  server in any time zone. The audit log's `completed_at` and the snapshot list
  below are UTC too (`snapshot_time` carries a `Z`).
- `AT` must come right after the table name, before `WHERE`.

## What an expired or unknown snapshot answers

`404 {"error":"snapshot_not_found","message":"The requested cache snapshot does not
exist; it may have been expired by the retention policy"}`. The server log has the
DuckDB message, one of:

| Request | DuckDB message |
|---|---|
| `AT (VERSION => n)`, n expired or never existed | `Invalid Input Error: No snapshot found at version n` |
| `AT (TIMESTAMP => t)`, t before the oldest kept snapshot | `Invalid Input Error: No snapshot found at timestamp t` |
| `AT (TIMESTAMP => t)`, t inside an expired incarnation of a CTAS table | `Catalog Error: Table with name items_cache does not exist at timestamp t!` |

A timestamp in the future answers `400 {"error":"snapshot_in_future"}`
(DuckLake 1.1 refuses it with `Cannot time travel to a future timestamp`; under
DuckDB 1.5 it read the newest snapshot). A timestamp between the newest snapshot
and now reads the newest one (200).

## Listing the snapshots that still exist

The audit log (`flapii cache audit <path>`) keeps the `snapshot_id` of every
refresh, **including snapshots retention has expired**, so an id from it can 404.
List the live ones with an uncached endpoint (no `cache:` block, so name the
catalog alias):

```yaml
# flapi: endpoint group=timetravel file=snapshots.yaml
url-path: /items/snapshots
template-source: snapshots.sql
connection: [items]
# flapi: check GET /items/snapshots -> 200
```

```sql
-- flapi: template group=timetravel file=snapshots.sql
SELECT snapshot_id, snapshot_time, changes
FROM ducklake_snapshots('cache')
ORDER BY snapshot_id DESC
```

```json
{"snapshot_id": 15, "snapshot_time": "2026-11-14T19:15:34.402314Z", "changes": {"inlined_insert": ["2"]}}
{"snapshot_id": 14, "snapshot_time": "2026-11-14T19:15:34.345123Z",
 "changes": {"tables_created": ["\"cache\".items_cache"], "tables_dropped": ["8"], "inlined_insert": ["9"]}}
```

The list is catalog-wide: it also holds the snapshots of other cache tables and of
the audit table (`inlined_insert` into table id `2` above is the audit row). A
snapshot whose `changes` name your table (by `schema.table` for a create, by table
id for row changes) is one you can time-travel to. Protect this endpoint like the
data itself.
