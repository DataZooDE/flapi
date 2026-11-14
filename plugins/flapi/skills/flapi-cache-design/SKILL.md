---
name: flapi-cache-design
description: >
  Designing flAPI DuckLake caches: how full refresh, incremental append and
  incremental merge are inferred from cursor / primary-key, writing the populate
  SQL (first-run CTAS bootstrap, watermark, late rows, overlap windows, soft and
  hard deletes), the serving template, warm-up modes and 503 cache_warming /
  cache_not_built / cache_build_failed, schedules and the scheduler, full rebuilds
  (--full), write-through (invalidate-on-write / refresh-on-write), read-only
  replicas on a shared catalog, prebuilt caches in CI, snapshots, retention and
  time travel.
  Use when a user wants to cache an endpoint, pick a refresh mode, write or debug
  a cache template, or serve a cache from several instances.
version: 26.10.31
user-invocable: false
---

# flAPI Cache Design Cheat Sheet

**flAPI:** v26.10.31 | **Skill:** Develop | **See also:** `flapi-cache-tuning`, `flapi-rest`, `flapi-setup`, `flapi-deploy`, `flapi-testing`

A flAPI cache is a **DuckLake table that you populate with your own SQL**. The
populate template (`cache.template-file`) runs at warm-up, on a schedule or on
demand. The serving template (`template-source`) reads the table on every
request. There is no hit/miss logic on the request path, and a GET never
triggers a refresh.

## Critical gotchas

- **You write the CTAS / INSERT / MERGE.** The mode (`full`, `append`, `merge`)
  is only inferred for the audit log and `{{params.cacheMode}}`. flAPI does not
  generate SQL. There is no `mode:` key and no `refresh:` key.
- **`INSERT` and `MERGE` fail on the first run:** `Catalog Error: Table with name
  ... does not exist!`. Every incremental template needs a bootstrap branch:
  `{{^cache.previousSnapshotTimestamp}} CREATE OR REPLACE TABLE ... AS ... {{/...}}`.
- **Never write `WHERE {{#cache.previousSnapshotTimestamp}} ... {{/...}}`.** On
  the first run the section is empty and DuckDB sees a bare `WHERE`
  (`Parser Error: syntax error at end of input`). Put the whole predicate,
  `WHERE` included, inside the section, or use `WHERE 1=1` with `AND ...` inside.
- **DuckLake `MERGE` allows only ONE `UPDATE`/`DELETE` action.** `WHEN MATCHED
  AND s.is_deleted THEN DELETE` plus `WHEN MATCHED THEN UPDATE` fails with
  `Not implemented Error: MERGE INTO with DuckLake only supports a single
  UPDATE/DELETE action`. Keep tombstones (below) or run a separate `DELETE`.
- **`INSERT (*) VALUES (s.*)` is not DuckDB.** Write `WHEN NOT MATCHED THEN INSERT *`,
  bare `INSERT` (by name), or list the columns.
- **A populate template must write the table, and no statement may be a bare
  `SELECT`.** Any bare `SELECT` (first, middle or last) is refused before anything
  runs: `statement N of M is a bare SELECT; its result is discarded` or `cache
  template must write {{cache.catalog}}.{{cache.schema}}.{{cache.table}} ...`. So a
  `SELECT error(...)` guard never fires; write it as `CREATE OR REPLACE TEMP TABLE _chk
  AS SELECT CASE WHEN <bad state> THEN error('...') END;` (recipe:
  [references/template-variables.md](references/template-variables.md#guards-and-proving-a-rollback)).
  A script that runs but leaves no table also fails and rolls back.
  Without `template-file`, the endpoint's `template-source` is the populate SQL: a
  single `SELECT` there is materialised (`CREATE OR REPLACE TABLE ... AS`) as a full
  refresh whatever the inferred mode, and startup warns. Declare `template-file` for
  incremental modes.
- **No refresh at startup beyond warm-up.** The first scheduled refresh comes one
  `schedule` after the last real refresh (this run's warm-up, else the newest
  snapshot, else process start). A restart does not re-run every cache.
- **`schema` defaults to `cache`**, so the table is `cache.cache.<table>` with the
  default alias. Always address it as `{{cache.catalog}}.{{cache.schema}}.{{cache.table}}`.
- **`rollback-window` and `delete-handling` are deprecated and have no effect**
  (startup logs a warning). Deletes are SQL in your template.
- **A failed first build answers 503** `{"error": "cache_build_failed"}` with
  `Retry-After: 5` until a refresh succeeds. The DuckDB error is in
  `flapii cache audit <path>`, the server log (logged once) and the body of a
  manual refresh, not in the 503 body.

## Pick the mode

| You declare in `cache:` | Inferred mode | Your template writes |
|---|---|---|
| neither `cursor` nor `primary-key` | `full` | `CREATE OR REPLACE TABLE ... AS SELECT ...` |
| `primary-key` only | `full` | same (a key without a cursor changes nothing) |
| `cursor` | `append` | bootstrap CTAS, then `INSERT ... WHERE cursor > watermark` |
| `cursor` + `primary-key` | `merge` | bootstrap CTAS, then `MERGE ... ON key` |

| Data shape | Mode | Why |
|---|---|---|
| Small (≲ a few M rows) or recomputed aggregates | full | Simplest; always correct |
| Immutable events, logs, readings with a growing id/time | append | Reads only new rows |
| Rows that are updated (orders, customers) | merge | Updates replace by key |
| Rows that are also deleted, with a delete flag or `deleted_at` | merge + tombstones | The delete moves the cursor |
| Rows hard-deleted with no trace | full, or merge + key reconciliation | A delta query cannot see a missing row |
| Late-arriving rows (cursor below the watermark) | append + overlap window + anti-join (no key), or merge + overlap window | The anti-join drops re-read rows; under merge they collapse on the key |

The **cursor** must increase on every insert (append) or every change (merge),
in the column's own type, and should be indexed or partitioned at the source.
Good: an identity/sequence id, `updated_at` maintained by the database, an ingest
timestamp. Bad: a business date a user can edit backwards, a client clock.

## Enable DuckLake (flapi.yaml)

```yaml
ducklake:
  enabled: true
  alias: cache                       # {{cache.catalog}}
  metadata-path: ./cache.ducklake    # DuckDB-file catalog, or postgres:dbname=... for a fleet
  data-path: ./cache_data            # Parquet files; s3:// gs:// work
  warmup: always                     # always | if-empty | never
```

The scheduler is **on by default**: every `cache.schedule` runs without further
config. Startup logs `Cache scheduler on: N scheduled cache(s), checked every X s`.
`ducklake.scheduler.enabled: false` turns it off (with a warning).

## Full refresh

```yaml
# flapi: endpoint group=products file=products.yaml
url-path: /products
template-source: products.sql
connection: [northwind]
request:
  - field-name: category_id
    field-in: query
    required: false
    validators:
      - type: int
        min: 1
cache:
  enabled: true
  table: products_cache
  schedule: 1h
  template-file: products_cache.sql
# flapi: check GET /products -> 200
# flapi: check GET /products?category_id=1 -> 200
```

```sql
-- flapi: template group=products file=products_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT ProductID, ProductName, CategoryID, UnitPrice, UnitsInStock
FROM nw.Products
WHERE Discontinued = '0';
```

```sql
-- flapi: template group=products file=products.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE 1=1
{{#params.category_id}}
  AND CategoryID = {{ params.category_id }}
{{/params.category_id}}
ORDER BY ProductID
```

Without `template-file` the **serving** template is materialised as a full
refresh (startup warns). A serving template that reads the cache table, as here,
cannot build itself (`503 cache_build_failed`). Always set `template-file`.

**Source in a DuckDB file?** `ATTACH ... (READ_ONLY)` in a connection `init` holds a
lock that stops every other process writing that file for the server's lifetime,
and a `DETACH` at the end of a populate template fails (one transaction). Use the
DETACH/ATTACH-at-the-start + atomic-rename pattern in `flapi-setup`
[references/connections.md](../flapi-setup/references/connections.md#duckdb-file-as-a-cache-source),
with a **unique alias per populate template** (`src_orders`), which uncached endpoints
must not read (it exists only after a refresh).

## Incremental append

`{{cache.previousSnapshotTimestamp}}` is the watermark: `max(<cursor column>)`
over the rows already cached, in the cursor's own type. It is **absent** when the
table does not exist, is empty, or the value does not look like `cursor.type`.
Then the `{{^...}}` branch rebuilds the table in full. (Without a `cursor:` it is
the commit time of the newest snapshot that wrote this table, not a data value.)

```yaml
# flapi: endpoint group=order-feed file=order-feed.yaml
url-path: /order-feed
template-source: order-feed.sql
connection: [northwind]
cache:
  enabled: true
  table: order_feed
  schedule: 10m
  cursor:
    column: OrderID
    type: integer
  template-file: order-feed_cache.sql
# flapi: check GET /order-feed -> 200
```

```sql
-- flapi: template group=order-feed file=order-feed_cache.sql
{{^cache.previousSnapshotTimestamp}}
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT OrderID, CustomerID, OrderDate, Freight, ShipCountry
FROM nw.Orders;
{{/cache.previousSnapshotTimestamp}}
{{#cache.previousSnapshotTimestamp}}
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT OrderID, CustomerID, OrderDate, Freight, ShipCountry
FROM nw.Orders
WHERE OrderID > {{{cache.previousSnapshotTimestamp}}};
{{/cache.previousSnapshotTimestamp}}
```

```sql
-- flapi: template group=order-feed file=order-feed.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
ORDER BY OrderID DESC
```

- **Quote the watermark for its type**: bare for an integer, `TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'`
  for a timestamp, `DATE '...'` for a date. Use triple braces inside quotes.
- **One clock and one zone for a `TIMESTAMP` cursor.** A naive `TIMESTAMP` has no
  zone, so the watermark is only as good as the writers agree. One writer writing
  local time (Berlin, UTC+1) next to UTC rows moves `max(cursor)` an hour ahead, and
  a later UTC row below it is **silently skipped**. In flapi's DuckDB
  `now()::TIMESTAMP` is UTC until ICU is loaded (any `AT TIME ZONE` autoloads it),
  then the host zone. Write UTC explicitly (`now() AT TIME ZONE 'UTC'`, or
  `duckdb: {TimeZone: UTC}`), or use a `TIMESTAMPTZ` column (`cursor.type:
  timestamptz`, literal `TIMESTAMPTZ '...'`), and keep an overlap window for clock skew.
- **Bootstrap with `CREATE OR REPLACE`, not "create empty then insert".** The
  watermark is also dropped for an existing table (empty, or a value that does not
  look like `cursor.type`). A `CREATE TABLE IF NOT EXISTS ... WHERE false` guard
  followed by an unconditional `INSERT` then re-inserts the whole source on top of
  the cached rows: duplicates in append mode. The `{{^...}}` CTAS / `{{#...}}`
  delta split rebuilds instead. (Under merge either form is idempotent.)
- **Late rows.** A strict `>` skips a row that arrives later with a cursor value
  at or below the watermark, and the refresh still reports success. Either accept
  that, or re-read an overlap window with `>=` in **both** the window predicate and a
  `NOT EXISTS` anti-join (append has no key; without the anti-join every refresh
  duplicates the window, and `NOT IN` over one NULL id drops every late row). SQL and
  sizing: [references/delete-patterns.md](references/delete-patterns.md#late-rows-in-append-mode).
- **Detect lost rows and duplicates** with an uncached check endpoint (row counts,
  missing keys, `dup_rows`, a content checksum on both sides):
  [references/delete-patterns.md](references/delete-patterns.md#detecting-lost-rows).

## Incremental merge, with deletes as tombstones

Keep deleted rows in the cache with their flag and filter them when serving.
It is one `MERGE` with one `UPDATE` action (DuckLake's limit), it is atomic,
and because the tombstone keeps its `updated_at` the watermark moves past the
delete.

```yaml
# flapi: config group=crm
project-name: crm-cache
project-description: Merge cache with soft deletes from a CSV extract
template:
  path: ./sqls
connections:
  crm:
    properties:
      path: ./src/customers.csv
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
```

```text
# flapi: file group=crm file=src/customers.csv
id,name,tier,updated_at,is_deleted
1,Ada,gold,2026-01-01 10:00:00,false
2,Bob,silver,2026-01-01 11:00:00,false
3,Cy,bronze,2026-01-01 12:00:00,true
```

```yaml
# flapi: endpoint group=crm file=customers.yaml
url-path: /customers
template-source: customers.sql
connection: [crm]
request:
  - field-name: version
    field-in: query
    required: false
    validators:
      - type: int
        min: 0
cache:
  enabled: true
  table: customers_cache
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  template-file: customers_cache.sql
# flapi: check GET /customers -> 200
```

```sql
-- flapi: template group=crm file=customers_cache.sql
{{^cache.previousSnapshotTimestamp}}
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, name, tier, updated_at, is_deleted
FROM read_csv('{{{conn.path}}}');
{{/cache.previousSnapshotTimestamp}}
{{#cache.previousSnapshotTimestamp}}
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT id, name, tier, updated_at, is_deleted
  FROM read_csv('{{{conn.path}}}')
  WHERE updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET name = s.name, tier = s.tier,
  updated_at = s.updated_at, is_deleted = s.is_deleted
WHEN NOT MATCHED THEN INSERT (id, name, tier, updated_at, is_deleted)
  VALUES (s.id, s.name, s.tier, s.updated_at, s.is_deleted);
{{/cache.previousSnapshotTimestamp}}
```

```sql
-- flapi: template group=crm file=customers.sql
SELECT id, name, tier, updated_at
FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
{{#params.version}}AT (VERSION => {{ params.version }}){{/params.version}}
WHERE NOT is_deleted
ORDER BY id
```

- **Count only real changes.** `rows_affected` counts every matched row; guard with
  `WHEN MATCHED AND (t.name IS DISTINCT FROM s.name OR ...) THEN UPDATE SET ...`
  (still one action; unchanged rows are not rewritten).
- **`>=` is right for merge**: a re-read boundary row collapses on the key, and it
  closes the "second row at exactly the watermark" gap. In append mode `>=`
  duplicates. For late rows in merge mode, subtract an overlap window
  (`- INTERVAL 1 HOUR`); re-reading is idempotent.
- Removing rows physically, hard deletes with no flag, and the transaction
  trade-off: [references/delete-patterns.md](references/delete-patterns.md).

## Forcing a full rebuild

`flapii cache refresh <path> --full` (server) or `flapi cache refresh -c flapi.yaml
--endpoint <path> --full` (one-shot) erases the watermark **and** drops the table in
the refresh's transaction, so the `{{^...}}` branch rebuilds from scratch (an append
cache is rebuilt, not re-appended); a failed rebuild restores the old snapshot.
Details: [references/write-through-and-rebuild.md](references/write-through-and-rebuild.md#forcing-a-full-rebuild).

## The serving template

- It only sees `{{cache.catalog}}`, `{{cache.schema}}`, `{{cache.table}}` (plus
  `params`, `conn`, `env`, `auth`). The other cache variables exist only in
  the populate template. Full list: [references/template-variables.md](references/template-variables.md).
- Typed request fields outside a section are bound parameters (`flapi-rest`).
- `limit` / `offset` are reserved for pagination, which wraps your query; do not
  write `LIMIT {{ params.limit }}` in a serving template.
- Time travel: `AT (VERSION => n)` (a catalog snapshot id, e.g. from
  `snapshot_id` in the audit log) or `AT (TIMESTAMP => TIMESTAMP '...')` inside a
  section. An expired or unknown version or timestamp answers `404
  snapshot_not_found`. Snapshot listing endpoint, timestamp parameters (a bound
  `AT (TIMESTAMP => ?)` does not work), UTC: [references/time-travel.md](references/time-travel.md).

## When a refresh runs

| Trigger | How |
|---|---|
| Startup warm-up | `ducklake.warmup` / `cache.warmup`: `always` (default, re-run every populate SQL), `if-empty` (only missing tables), `never`. Override per run: `--cache-warmup if-empty`, `--no-cache-warmup`, env `FLAPI_CACHE_WARMUP`. Precedence: `read-only` > flag/env > `cache.warmup` > `ducklake.warmup`. |
| Schedule | `cache.schedule: 30s / 10m / 6h / 1d`. Checked every `ducklake.scheduler.scan-interval` (default: the shorter of `heartbeat.worker-interval`, 60 s, and the shortest schedule; floor 1 s). Due one schedule after the last real refresh, so not at boot. Endpoints sharing a table share one schedule. |
| On demand | `flapii cache refresh <path> [--full]`, `POST /api/v1/_config/endpoints/{slug}/cache/refresh[?full=true]`, MCP config tool `flapi_refresh_cache`. Counts as the last refresh for the schedule. |
| After a write | `invalidate-on-write` (synchronous) or `refresh-on-write` (after the response) on the write endpoint (below) |
| Pipeline / CI | `flapi cache refresh -c flapi.yaml --all` or `--endpoint <path>` (repeatable) `[--full]`, no server |

- The config-service slug is the path with `/` → `-` and a literal `-` → `~1`:
  `/orders` is `-orders`, `/order-feed` is `-order~1feed`. `flapii` encodes it for you.
- A refresh of a table that is already refreshing is skipped, not queued.
- A failed refresh changes nothing. Do not add your own `BEGIN` / `COMMIT` to a
  populate template (`--full` then only clears the watermark, and warns).
- Never built: `503` + `Retry-After: 5` with `cache_warming`, `cache_build_failed`
  or `cache_not_built`; `/health` is `degraded`. A **later** failed refresh keeps
  serving the last snapshot and marks the cache `stale`.

## Write-through: refresh after a write

Give the POST/PUT/PATCH/DELETE endpoint its own `cache:` block with the same `table`
and `template-file`, plus `invalidate-on-write: true` (populate runs before the
response: read-your-write) or `refresh-on-write: true` (after the response,
coalesced). Example: [references/write-through-and-rebuild.md](references/write-through-and-rebuild.md).

## Replicas and prebuilt caches

- `ducklake.read-only: true` makes an instance a replica of a catalog another process
  writes: no warm-up, schedule, retention or compaction; cache mutations answer
  `409 read_only_cache`; a table the writer builds later is served within ~5 s.
  Replicas need the writer's `connections:` names and the same endpoint YAMLs.
- **DuckDB-file catalog**: replicas can share it, but no writer opens it while a replica
  holds it (and vice versa). Build it in CI and ship it, or use a Postgres catalog for
  a live writer. Writer jobs, the lock table, prebuilt catalogs (absolute `data_path`!):
  [references/deployment-patterns.md](references/deployment-patterns.md).

## Snapshots, time travel, retention

- Every committed write to the cache table is a DuckLake snapshot; the audit row
  records its `snapshot_id` (null for a 0-row refresh, which writes none). The
  audit log keeps the ids of expired snapshots: list the live ones
  ([references/time-travel.md](references/time-travel.md)).
- `cache.retention.keep-last-snapshots: N` / `max-snapshot-age: 7d` (or global
  `ducklake.retention` for caches without their own block; no merge) expire this
  table's old snapshots, every incarnation a CTAS or `--full` left included,
  **after every refresh**, then delete the unreferenced files. Verified in the
  default schema `cache` and across `--full`: five CTAS refreshes under
  `keep-last-snapshots: 2` leave 2 data files. **After `--full` the old incarnation's
  files stay until the last snapshot before the rebuild leaves the window**: with
  `keep-last-snapshots: N` that is N-1 more writing refreshes (measured: N=2 freed
  them on the 1st, N=4 on the 3rd; a 0-row refresh writes no snapshot and does not
  count). Until then the disk holds both copies. `flapii cache gc <path>` runs it on
  demand: `{status, expired_snapshots, files_deleted, bytes_freed}` (`no_policy`
  without retention).
- A table left behind by a `table:`/`schema:` change: `flapii cache drop-table
  <schema>.<table>` (409 `cache_table_in_use` while an endpoint still names it). It
  applies the retention of live tables whose snapshots still pin the dropped table's
  files right away, and `pinned_by` names the endpoint whose policy keeps the rest.
- Refreshes run one at a time per catalog (a refresh of another table waits; one of the
  same table answers `already_running`). Still give each populate template its own
  ATTACH alias: an attach is process-wide.
- The audit row's `full` says the refresh loaded the whole source (`--full`, full mode,
  or the first incremental build).
- `ducklake.compaction: {enabled: true, schedule: 1d}` (interval or `@daily` etc.,
  not cron) merges small files once per catalog; audit `sync_type` `compaction`
  (`GET /api/v1/_config/cache/audit?sync_type=compaction`). `delete-threshold`
  (≈ refreshes between compactions × delta share, e.g. `0.05`) rewrites files with
  more deleted rows than that (merge caches; `flapi-cache-tuning`).

## Developing the populate SQL

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=dev-token-0123456789
flapi -c flapi.yaml --config-service --config-service-token dev-token-0123456789 --log-level debug
flapii cache get /customers                 # table, schema, cursor, primary key, template file
flapii cache template /customers            # the populate SQL the server reads
flapii templates expand /customers --cache  # rendered with the live watermark, not executed
curl -s -X POST -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" -d '{"parameters":{}}' \
  'http://localhost:8080/api/v1/_config/endpoints/-customers/template/expand?target=cache&validate_only=1'
                                            # parsed + bound per statement, nothing runs
flapii cache update-template /customers --file sqls/customers_cache.sql
flapii cache refresh /customers             # run it now
flapii cache audit /customers -o table      # mode, status, duration_ms, rows, snapshot
```

- The populate file is re-read on every refresh: editing it on disk works too.
  `--cache` renders it with the live watermark (which branch runs next);
  `validate_only=1` also binds each statement (the plugin hook calls it after every
  edit). Failure output, debug logging, proving the incremental branch:
  [references/template-variables.md](references/template-variables.md#developing-and-checking-a-populate-template).

## Canonical cache workflow

1. Inspect the source: row count, append-only or mutable, deletes (flag or hard),
   the cursor column and its type, the key, how late rows can arrive.
2. Pick the mode from the tables above; declare `cursor` / `primary-key` to match.
3. Enable `ducklake:` (the scheduler is on by default).
4. Write the populate SQL with the `{{^...}}` CTAS bootstrap and the guarded delta
   branch; write the serving template against `{{cache.catalog}}.{{cache.schema}}.{{cache.table}}`.
5. `flapi -c flapi.yaml --validate-config`, then start with `--config-service`.
6. `flapii cache refresh <path>` twice with a source change in between; read
   `flapii cache audit <path> -o table`: first the CTAS with all rows, then a small
   delta. After a template change, `flapii cache refresh <path> --full` rebuilds.
7. GET the endpoint; for merge, check that deleted keys are gone.
8. Choose warm-up and schedule; measure and tune with `flapi-cache-tuning`.
