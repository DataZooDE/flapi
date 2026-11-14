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
- **A populate template must write the table.** A template that is (or ends in)
  a bare `SELECT` is refused before it runs: `cache template must write
  {{cache.catalog}}.{{cache.schema}}.{{cache.table}} ... statement N of the template
  is a bare SELECT`. A script that runs but leaves no table also fails and rolls back.
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
| Late-arriving rows (cursor below the watermark) | merge with an overlap window | Re-read rows collapse on the key |

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

Without `template-file` the **serving** template is also used as the populate
SQL, which is a bare `SELECT` and is refused (`503 cache_build_failed`). Always
set `template-file`.

## Incremental append

`{{cache.previousSnapshotTimestamp}}` is the watermark: `max(<cursor column>)`
over the rows already cached, in the cursor's own type. It is **absent** when the
table does not exist, is empty, or the value does not look like `cursor.type`.
Then the `{{^...}}` branch rebuilds the table in full.

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

**Detect lost rows**: a strict `>` loses late rows silently while the refresh reports
success. Reconcile with an uncached check endpoint:
[references/delete-patterns.md](references/delete-patterns.md#detecting-lost-rows).

- **Quote the watermark for its type**: bare for an integer, `TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'`
  for a timestamp, `DATE '...'` for a date. Use triple braces inside quotes.
- **Bootstrap with `CREATE OR REPLACE`, not "create empty then insert".** The
  watermark is also dropped for an existing table (empty, or a value that does not
  look like `cursor.type`). A `CREATE TABLE IF NOT EXISTS ... WHERE false` guard
  followed by an unconditional `INSERT` then re-inserts the whole source on top of
  the cached rows: duplicates in append mode. The `{{^...}}` CTAS / `{{#...}}`
  delta split rebuilds instead. (Under merge either form is idempotent.)
- **Late rows.** A strict `>` skips a row that arrives later with a cursor value
  at or below the watermark. Either accept that, or re-read an overlap window and
  drop what is already cached (append has no key, so the anti-join is yours):

```sql
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT id, ts, kind FROM src.events AS s
WHERE s.ts > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE
  AND s.id NOT IN (
    SELECT id FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
    WHERE ts > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE);
```

  Without the `NOT IN` an overlap window duplicates the whole window on **every**
  refresh. Size the window to the maximum lateness (see `flapi-cache-tuning`);
  merge mode makes overlap idempotent without the anti-join.

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

- **Count only real changes.** `rows_affected` counts every matched row, so an
  overlap window reports re-read rows as updates. Guard the update:
  `WHEN MATCHED AND (t.name IS DISTINCT FROM s.name OR t.tier IS DISTINCT FROM s.tier
  OR t.updated_at IS DISTINCT FROM s.updated_at OR t.is_deleted IS DISTINCT FROM s.is_deleted) THEN UPDATE SET ...`
  (still one action, so DuckLake accepts it; unchanged rows are not rewritten).
- **`>=` is right for merge**: a re-read boundary row collapses on the key, and it
  closes the "second row at exactly the watermark" gap. In append mode `>=`
  duplicates. For late rows in merge mode, subtract an overlap window
  (`- INTERVAL 1 HOUR`); re-reading is idempotent.
- Removing rows physically, hard deletes with no flag, and the transaction
  trade-off: [references/delete-patterns.md](references/delete-patterns.md).

## Forcing a full rebuild

```bash
flapii cache refresh /customers --full                  # running server (POST .../cache/refresh?full=true)
flapi cache refresh -c flapi.yaml --endpoint /customers --full   # one-shot, no server
```

`--full` erases the watermark **and** drops the cache table in the same
transaction, so the `{{^cache.previousSnapshotTimestamp}}` branch rebuilds it from
scratch (an append cache is rebuilt, not re-appended). Readers keep the old
snapshot until the commit, and a failed rebuild restores it. The audit row keeps
the YAML's mode label (`append`/`merge`). Details:
[references/write-through-and-rebuild.md](references/write-through-and-rebuild.md#forcing-a-full-rebuild).

## The serving template

- It only sees `{{cache.catalog}}`, `{{cache.schema}}`, `{{cache.table}}` (plus
  `params`, `conn`, `env`, `auth`). The other cache variables exist only in
  the populate template. Full list: [references/template-variables.md](references/template-variables.md).
- Typed request fields outside a section are bound parameters (`flapi-rest`).
- `limit` / `offset` are reserved for pagination, which wraps your query; do not
  write `LIMIT {{ params.limit }}` in a serving template.
- Time travel: `AT (VERSION => n)` (a catalog snapshot id, e.g. from
  `snapshot_id` in the audit log) or `AT (TIMESTAMP => TIMESTAMP '...')`. A
  version that retention expired fails with `No snapshot found at version n` (500).

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
- A failed refresh changes nothing: the previous snapshot keeps serving. Do not
  add your own `BEGIN` / `COMMIT` to a populate template (with one, `--full` can
  only clear the watermark, not drop the table, and warns).
- While a cache has never been built, its endpoint answers `503` with
  `Retry-After: 5`: `cache_warming` (building), `cache_build_failed` (the first
  build failed) or `cache_not_built` (warm-up skipped and no table yet). `/health`
  reports `degraded` with `unbuilt` / `failed` lists. A **later** failed refresh
  (including a failed warm-up over an existing table) keeps serving the last
  snapshot and marks the cache `stale`.

## Write-through: refresh after a write

Give the POST/PUT/PATCH/DELETE endpoint its own `cache:` block with the same
`table` and `template-file`, plus one of:

| Key | When the populate SQL runs | Use when |
|---|---|---|
| `invalidate-on-write: true` | synchronously, before the response | the caller must read its own write |
| `refresh-on-write: true` | after the response, coalesced per endpoint | write latency matters more than read-your-write |

Both set means synchronous only. REST and MCP tool writes follow the same rules.
The write endpoint answers 503 until the cache is first built. Runnable example:
[references/write-through-and-rebuild.md](references/write-through-and-rebuild.md).

## Replicas and prebuilt caches

- `ducklake.read-only: true` makes an instance a replica of a catalog another
  process writes: no warm-up, schedule, retention or compaction, and cache
  refresh/update/gc answer `409 read_only_cache`.
- The writer can be a job: `flapi cache refresh -c flapi.yaml --all` (exit `0` ok,
  `1` a refresh failed or the catalog is locked, `2` usage).
- **A DuckDB-file catalog is locked by the process that opened it for writing**:
  the one-shot fails while a server has it open, and a writer and live replicas
  cannot share it. A live fleet needs a Postgres (multi-client) catalog; a file
  catalog can be built in CI and shipped, with `warmup: if-empty` / `never`.

Recipes: [references/deployment-patterns.md](references/deployment-patterns.md).

## Snapshots, time travel, retention

- Every committed write to the cache table is a DuckLake snapshot; the audit row
  of each refresh records its `snapshot_id`. A 0-row incremental refresh creates
  none for the table (the audit row itself is a snapshot of `audit.sync_events`).
- `cache.retention.keep-last-snapshots: N` / `max-snapshot-age: 7d` expire this
  table's old snapshots **after every refresh**; `flapii cache gc <path>` applies
  the same policy on demand. Expired versions can no longer be time-travelled.
- `ducklake.retention` (global, same keys) applies to every cache without its own
  `retention:`; an endpoint block replaces it (no merge).
- After an expiry the files no snapshot references are deleted, so disk space is
  reclaimed. The audit log is bounded too (newest 1000 rows per cache table).
- `ducklake.compaction: {enabled: true, schedule: 1d}` (or `@hourly`, `@daily`,
  `@midnight`, `@weekly`; not cron) merges small files once per catalog; the
  audit log shows it as `sync_type` `compaction`.

## Developing the populate SQL

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=dev-token
flapi -c flapi.yaml --config-service --config-service-token dev-token --log-level debug
flapii cache get /customers                 # table, schema, cursor, primary key, template file
flapii cache template /customers            # the populate SQL the server reads
flapii templates expand /customers --cache  # rendered with the live watermark, not executed
flapii cache update-template /customers --file sqls/customers_cache.sql
flapii cache refresh /customers             # run it now
flapii cache audit /customers -o table      # mode, status, duration_ms, rows, snapshot
```

- The populate file is re-read on every refresh: editing it on disk works too.
- The audit log is data: `flapii` prints it exactly as the server sends it,
  snake_case (`duration_ms`, `rows_affected`, `snapshot_id`, `message`), whatever
  `--json-style` says.
- A failed `flapii cache refresh` prints `[400] Bad Request: Cache refresh failed:
  <the DuckDB error>`; the same text is in the audit row's `message`.
- `--log-level debug` logs every rendered statement (`Processed query:`), the
  only way to see the populate SQL after Mustache.
- `flapii templates expand <path> --cache` renders the **populate** template with
  the live watermark and snapshot variables, without executing it (returns
  `target`, `mode`, `expanded`, `variables`): check which branch the next refresh
  takes. Without `--cache` it renders the serving template.
- `flapii cache template` / `update-template` need `template-file`; without it they
  answer `400 Cache template not configured`.
- Prove the incremental branch: refresh, change the source, refresh again, and
  check that the second audit row is the mode you expect with a small `rows_affected`.

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
