# Caching an expensive query

**Goal:** serve a slow or costly query from a local materialised table that
refreshes on a schedule.

flAPI's cache is a **DuckLake table**, not a per-request memo. Your endpoint's
template queries a table that is already populated; there is no hit/miss branch
on the request path.

---

## 1. Enable DuckLake once, globally

In `flapi.yaml`:

```yaml
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./data/cache.ducklake
  data-path: ./data/cache.ducklake
  retention:                 # default for every cache without its own retention
    max-snapshot-age: 14d
```

The scheduler is **on by default**: every `cache.schedule` is honoured without
further configuration. Set `ducklake.scheduler.enabled: false` only to opt out
(startup then warns that scheduled caches will not refresh). Startup logs
`Cache scheduler on: N scheduled cache(s), checked every X s`.

## 2. Add a cache block to the endpoint

```yaml
url-path: /publicis
template-source: publicis.sql
connection: [bigquery-lakehouse]

cache:
  enabled: true
  table: publicis_cache
  schema: analytics
  schedule: 5m
  template-file: publicis/publicis_cache.sql
```

`template-file` is the SQL that **populates** the cache. `template-source` stays
the SQL that **serves** requests — and it now reads the cache table:

```sql
-- publicis.sql (serving template)
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE 1=1
{{#params.region}}
  AND region = '{{{ params.region }}}'
{{/params.region}}
```

## Choosing a refresh strategy

The mode is inferred from what you declare. There is no `mode:` key.

| You declare | Mode | Good for |
|---|---|---|
| Neither `primary-key` nor `cursor` | **Full refresh** (your template rebuilds the table, e.g. `CREATE OR REPLACE TABLE … AS`) | Small tables; anything where "recompute it all" is cheap |
| `cursor` only | **Incremental append** | Append-only data: events, logs, readings |
| `primary-key` **and** `cursor` | **Incremental merge** (upsert) | Mutable rows: customers, orders |

```yaml
# Incremental append
cache:
  enabled: true
  table: events_cache
  schema: analytics
  schedule: 10m
  cursor:
    column: created_at
    type: timestamp
  template-file: events/events_cache.sql
```

```yaml
# Incremental merge
cache:
  enabled: true
  table: customers_cache
  schema: analytics
  schedule: 15m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  template-file: customers/customers_cache.sql
```

## Writing the populate template

flAPI runs the populate template **verbatim**. It does not wrap a `SELECT` in a
`CREATE TABLE`, so the template itself must write the table. A statement that
is only a `SELECT` is refused before anything runs (`cache template must write
{{cache.catalog}}.{{cache.schema}}.{{cache.table}} … statement N of the template
is a bare SELECT, which builds nothing`), and after the script the cache table
must exist, or the refresh fails and is rolled back. A cache whose first build
fails answers `503 {"error":"cache_build_failed"}` with `Retry-After: 5`; the
reason is in the server log.

Full refresh (no `primary-key`, no `cursor`):

```sql
-- publicis/publicis_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, region, revenue, updated_at
FROM bigquery_scan('my-project.sales.publicis');
```

The whole template runs in **one transaction**: if any statement fails, the
refresh changes nothing and the endpoint keeps serving the previous snapshot.

These variables are available in a cache template:

| Variable | Contains |
|---|---|
| `{{cache.catalog}}` `{{cache.schema}}` `{{cache.table}}` | Where to write |
| `{{cache.snapshotTimestamp}}` `{{cache.snapshotId}}` | This refresh |
| `{{cache.previousSnapshotTimestamp}}` `{{cache.previousSnapshotId}}` | The incremental watermark. With a `cursor:` the timestamp is `max(<cursor>)` over the cached rows, in the cursor's own type; both are absent when no watermark can be read, so a guarded template loads in full. See CONFIG_REFERENCE. |
| `{{cache.cursorColumn}}` `{{cache.cursorType}}` | Your declared cursor |
| `{{cache.primaryKeys}}` | Your declared key |
| `{{params.cacheMode}}` | `full`, `append` or `merge` |

Incremental templates must also create the table on their first run. `INSERT`
and `MERGE` fail if the target does not exist yet. On the first run (and
whenever no watermark can be read) `cache.previousSnapshotTimestamp` is absent,
so an inverted section `{{^…}}` can create the empty table, and the predicate
sits behind `WHERE 1=1` so that nothing dangles when it is left out.

Append (`cursor: {column: id, type: integer}`):

```sql
{{^cache.previousSnapshotTimestamp}}
CREATE TABLE IF NOT EXISTS {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM source_events WHERE false;
{{/cache.previousSnapshotTimestamp}}
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT * FROM source_events
WHERE 1=1
{{#cache.previousSnapshotTimestamp}}
  AND id > {{cache.previousSnapshotTimestamp}}
{{/cache.previousSnapshotTimestamp}};
```

With a cursor, the watermark is `max(<cursor>)` over the rows already cached, in
the cursor's own type. Quote it for a timestamp cursor
(`AND updated_at > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'`) and leave it
bare for an integer cursor.

**Late rows.** A row that arrives with a cursor value below the watermark is
skipped by a strict `>`, permanently. If your source has late rows, re-read an
overlap window: `AND updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE`.
The window must cover the **maximum** lateness; a row later than the window is
still lost. What the re-read rows do depends on the mode:

- **Merge** collapses them on the key, so the overlap is safe.
- **Append + overlap duplicates the whole window on every refresh**, even when
  nothing changed. Either use merge, or exclude rows already cached with an
  anti-join over the same window:

  ```sql
    AND updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE
    AND id NOT IN (
      SELECT id FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
      WHERE updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE
    )
  ```

To check that nothing was lost, run a reconcile query from time to time against
the source and the cache table:

```sql
SELECT s.id, s.updated_at
FROM source_events s
ANTI JOIN cache.analytics.events_cache c ON s.id = c.id;   -- rows the cache is missing
```

Merge (`primary-key: [id]`, `cursor: {column: updated_at, type: timestamp}`):

```sql
{{^cache.previousSnapshotTimestamp}}
CREATE TABLE IF NOT EXISTS {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM source_customers WHERE false;
{{/cache.previousSnapshotTimestamp}}
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT * FROM source_customers
  WHERE 1=1
  {{#cache.previousSnapshotTimestamp}}
    AND updated_at > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'
  {{/cache.previousSnapshotTimestamp}}
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET name = s.name, email = s.email, updated_at = s.updated_at
WHEN NOT MATCHED THEN INSERT *;
```

DuckDB's form is `INSERT *` (or `INSERT BY NAME`), not `INSERT (*) VALUES (s.*)`.

**`rows_affected` counts no-op updates.** With an overlap window, every re-read
row matches and is "updated" to the same values, and each one is counted.
Guard the update so only real changes count:

```sql
WHEN MATCHED AND (t.name IS DISTINCT FROM s.name
               OR t.email IS DISTINCT FROM s.email
               OR t.updated_at IS DISTINCT FROM s.updated_at)
  THEN UPDATE SET name = s.name, email = s.email, updated_at = s.updated_at
```

Measured on a 6-row table re-read with an overlap: unguarded, every refresh
reported `rows_affected: 5` with no source change; guarded, `0`.

### Deletes in merge mode

flAPI does not detect deleted source rows. An incremental refresh only sees rows
the source still returns. Model deletes in the data. The usual source signal is a
soft-delete flag (or a `deleted_at` column) that bumps the cursor when it is set.

**A `MERGE` with both a `DELETE` and an `UPDATE` action fails on DuckLake**:
`WHEN MATCHED AND s.is_deleted THEN DELETE` followed by `WHEN MATCHED THEN UPDATE`
is rejected with `MERGE INTO with DuckLake only supports a single UPDATE/DELETE
action currently`. Use one of these two patterns instead (both verified over
successive refreshes).

**Option 1: delete first, then a single-action `MERGE`.** Keep the `MERGE` last:

```sql
{{^cache.previousSnapshotTimestamp}}
CREATE TABLE IF NOT EXISTS {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM source_customers WHERE false;
{{/cache.previousSnapshotTimestamp}}
DELETE FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE id IN (
  SELECT id FROM source_customers
  WHERE is_deleted
  {{#cache.previousSnapshotTimestamp}}
    AND updated_at > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'
  {{/cache.previousSnapshotTimestamp}}
);
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT * FROM source_customers
  WHERE NOT is_deleted
  {{#cache.previousSnapshotTimestamp}}
    AND updated_at > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'
  {{/cache.previousSnapshotTimestamp}}
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET name = s.name, updated_at = s.updated_at, is_deleted = s.is_deleted
WHEN NOT MATCHED THEN INSERT *;
```

The deleted rows leave the cache, so they no longer count towards the watermark
(`max(updated_at)` over the cached rows): when the newest change was a delete,
the next refresh re-reads it. That is harmless (the `DELETE` is idempotent) but
the watermark only advances with surviving rows.

**Option 2: keep tombstones in the cache.** Use the plain single-action merge
from above, so a deleted row is updated to `is_deleted = true` and stays, and
filter it when serving:

```sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE NOT is_deleted
```

The tombstones keep the watermark advancing, at the cost of storing deleted rows.

If the source hard-deletes rows and keeps no trace, an incremental cache cannot
see the delete. Use a full refresh for that table. You can also reconcile
periodically: a scheduled full refresh, or
`DELETE FROM <cache> WHERE id NOT IN (SELECT id FROM source)` if the key set is
cheap to read. The cache keys `delete-handling` and `rollback-window` are
deprecated and have no effect.

## Measuring and tuning refresh time

Every refresh is recorded in the cache audit log, along with how long the
populate statement took and how many rows it wrote:

```bash
flapii cache audit /customers     # sync_type, status, duration_ms, rows_affected, snapshot_id
flapii metrics --endpoint /customers   # last/avg/max refresh duration, data age, state
```

Without a running server, run the one-shot refresh: its JSON summary carries
`duration_ms` and `rows_affected` per cache
(`flapi cache refresh -c flapi.yaml --endpoint /customers`).

`rows_affected` is the **sum over every DML statement** in the template: a
template that deletes 1 row and then merges 2 reports `3`. `CREATE TABLE … AS`
counts the rows it creates.

What usually makes a refresh shorter, biggest win first:

1. **Go incremental.** If only a fraction of rows changes between refreshes, a
   cursor turns "re-read everything" into "read the delta".
2. **Push the watermark to the source.** The `WHERE` predicate on the cursor must
   reach the source system (Postgres, BigQuery, Parquet min/max statistics) so
   the source reads only the delta. Check with `EXPLAIN` on the rendered SQL:
   `flapii templates expand /customers --cache` prints the populate template as
   the next refresh renders it, with the live watermark filled in. A function
   wrapped around the cursor column usually stops the pushdown.
3. **Select only the columns you serve.** `SELECT *` from a wide source table
   moves data that nothing reads.
4. **Fit the schedule to the duration.** A refresh that takes most of its
   `schedule` interval leaves no headroom. Overlapping refreshes of the same
   table are skipped, not queued.
5. **Keep small deltas cheap.** Frequent small refreshes create many small files.
   `ducklake.data-inlining-row-limit` keeps small inserts in the catalog, and
   `ducklake.compaction` (`enabled: true` plus a `schedule` such as `24h` or
   `@daily`) merges small files and cleans up once per schedule.
6. **Watch merge-mode refresh time creep.** Every update or delete in a
   DuckLake table writes a delete file next to the data files, and readers apply
   all of them. Over many refreshes refresh time grows: measured on a 5M-row
   merge cache updating ~500k rows per refresh, 78 delete files after 12
   refreshes and refresh time rising from ~1.2 s to ~6.3 s. Compact the table,
   or rebuild it with an occasional full rebuild (`flapii cache refresh <path> --full`).
7. **Bound snapshot growth.** Every refresh is a DuckLake snapshot. Set
   `retention.keep-last-snapshots` / `max-snapshot-age` (per cache, or once under
   `ducklake.retention` for all caches without their own). Retention runs after
   every refresh and on `flapii cache gc <path>`; expired files are deleted from
   disk.

The `flapi-cache-tuning` skill in the [Claude Code plugin](./claude-code.md)
walks through this loop with before/after numbers.

## When it refreshes

- **At startup**: caches are warmed for endpoints that enable them, according to
  `ducklake.warmup`. With `always` (the default) every populate query runs again
  (once per cache table, even when several endpoints share it). With `if-empty`
  only tables that do not exist yet are built. With `never` none are, and a
  missing table answers `503 cache_not_built` until it is built.
  `--no-cache-warmup` forces `never` for one run, e.g. in CI. If a warm-up fails
  over a table an earlier run built, the endpoint keeps serving it (state `stale`).
- **On `cache.schedule`** — per cache table. The scheduler does not refresh at
  boot: the first scheduled refresh comes one `schedule` after the last real
  refresh (this run's warm-up, else the table's newest snapshot, else process
  start); manual refreshes count. Schedules are checked every
  `ducklake.scheduler.scan-interval` (default: the shorter of
  `heartbeat.worker-interval`, which defaults to 60 s, and the shortest
  `cache.schedule`).
- **On demand** — `flapii cache refresh /endpoint`. Add `--full` (REST:
  `?full=true`) for a **full rebuild**: the watermark is erased and the cache
  table is dropped and rebuilt in the same transaction, so an append cache is
  rebuilt rather than re-appended. Use it after a template or source change.
- **After a write** — `invalidate-on-write: true` refreshes before the write's
  response is sent; `refresh-on-write: true` refreshes in the background after
  it (coalesced per endpoint). With both, the synchronous refresh wins. MCP tool
  writes follow the same rules.

- **From a pipeline**: `flapi cache refresh --config flapi.yaml --all` refreshes
  once and exits (`0` ok, `1` a refresh failed, `2` usage). Run it after the
  upstream build. Replicas then attach the same catalog with
  `ducklake.read-only: true`; see [serverless.md](./serverless.md). With a
  DuckDB-file catalog the one-shot refresh fails (exit `1`) while any server
  holds the file open, with `{"error":"a running flapi holds the DuckLake
  catalog lock; refresh through the server: flapii cache refresh <path>"}`.
  `flapi cache refresh --full` rebuilds in full.

A normal GET never triggers a refresh. While a cache is still warming, requests
get **503 `cache_warming` with `Retry-After`** rather than an empty result set.

## Operating it

`flapii` talks to the running server's config service, so start flAPI with
`--config-service` and export `FLAPI_CONFIG_SERVICE_TOKEN` first — see
[CONFIG_SERVICE_API_REFERENCE](../CONFIG_SERVICE_API_REFERENCE.md).

```bash
flapii cache get /customers        # current configuration and state
flapii cache list                  # all caches
flapii cache refresh /customers    # refresh now
flapii cache refresh /customers --full   # drop and rebuild from scratch
flapii cache audit /customers -o table   # refresh history: mode, status, duration, rows, snapshot
flapii cache gc /customers         # apply retention (path is required)
```

## How it works

- [spec/components/caching.md](../spec/components/caching.md) — CacheManager, HeartbeatWorker, snapshots and time travel
- [spec/DESIGN_DECISIONS.md § 5](../spec/DESIGN_DECISIONS.md) — why DuckLake
- [CONFIG_REFERENCE](../CONFIG_REFERENCE.md) — every cache key
