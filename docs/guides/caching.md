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
  retention:
    max-snapshot-age: 14d
  scheduler:
    enabled: true
```

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
the SQL that **serves** requests — and it now reads the cache table.

## Choosing a refresh strategy

The mode is inferred from what you declare. There is no `mode:` key.

| You declare | Mode | Good for |
|---|---|---|
| Neither `primary-key` nor `cursor` | **Full refresh** (CTAS) | Small tables; anything where "recompute it all" is cheap |
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
(`AND updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}'`) and leave it
bare for an integer cursor. Rows that arrive late, with a cursor value below the
watermark, are skipped by a strict `>`. If your source has late rows, re-read an
overlap window (`AND updated_at > TIMESTAMP '…' - INTERVAL 10 MINUTE`) and use
merge mode, so the re-read rows replace themselves instead of duplicating.

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
    AND updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}'
  {{/cache.previousSnapshotTimestamp}}
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET name = s.name, email = s.email, updated_at = s.updated_at
WHEN NOT MATCHED THEN INSERT *;
```

DuckDB's form is `INSERT *` (or `INSERT BY NAME`), not `INSERT (*) VALUES (s.*)`.

### Deletes in merge mode

flAPI does not detect deleted source rows. An incremental refresh only sees rows
the source still returns. Model deletes in the data and handle them in the
`MERGE`. The usual source signal is a soft-delete flag (or a `deleted_at`
column) that bumps the cursor when it is set:

```sql
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT * FROM source_customers
  WHERE 1=1
  {{#cache.previousSnapshotTimestamp}}
    AND updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}'
  {{/cache.previousSnapshotTimestamp}}
) AS s
ON t.id = s.id
WHEN MATCHED AND s.is_deleted THEN DELETE
WHEN MATCHED THEN UPDATE SET name = s.name, updated_at = s.updated_at, is_deleted = s.is_deleted
WHEN NOT MATCHED AND NOT s.is_deleted THEN INSERT *;
```

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

Without a running server, time the one-shot refresh:
`time flapi cache refresh -c flapi.yaml --endpoint /customers`.

What usually makes a refresh shorter, biggest win first:

1. **Go incremental.** If only a fraction of rows changes between refreshes, a
   cursor turns "re-read everything" into "read the delta".
2. **Push the watermark to the source.** The `WHERE` predicate on the cursor must
   reach the source system (Postgres, BigQuery, Parquet min/max statistics) so
   the source reads only the delta. Check with `EXPLAIN` on the rendered SQL
   (`flapii templates expand`). A function wrapped around the cursor column
   usually stops the pushdown.
3. **Select only the columns you serve.** `SELECT *` from a wide source table
   moves data that nothing reads.
4. **Fit the schedule to the duration.** A refresh that takes most of its
   `schedule` interval leaves no headroom. Overlapping refreshes of the same
   table are skipped, not queued.
5. **Keep small deltas cheap.** Frequent small refreshes create many small files.
   `ducklake.data-inlining-row-limit` keeps small inserts in the catalog, and
   `ducklake.compaction` merges files on a schedule.
6. **Bound snapshot growth.** Every refresh is a DuckLake snapshot. Set
   `retention.keep-last-snapshots` / `max-snapshot-age` and run
   `flapii cache gc <path>`.

The `flapi-cache-tuning` skill in the [Claude Code plugin](./claude-code.md)
walks through this loop with before/after numbers.

## When it refreshes

- **At startup**: caches are warmed for endpoints that enable them, according to
  `ducklake.warmup`. With `always` (the default) every populate query runs again.
  With `if-empty` only tables that do not exist yet are built. With `never` none
  are, and a missing table answers `503 cache_not_built` until it is built.
  `--no-cache-warmup` forces `never` for one run, e.g. in CI.
- **On `cache.schedule`** — per endpoint.
- **On demand** — `flapii cache refresh /endpoint`.

- **From a pipeline**: `flapi cache refresh --config flapi.yaml --all` refreshes
  once and exits (`0` ok, `1` a refresh failed, `2` usage). Run it after the
  upstream build. Replicas then attach the same catalog with
  `ducklake.read-only: true`; see [serverless.md](./serverless.md).

A normal GET never triggers a refresh. While a cache is still warming, requests
get **503 with `Retry-After`** rather than an empty result set.

## Operating it

`flapii` talks to the running server's config service, so start flAPI with
`--config-service` and export `FLAPI_CONFIG_SERVICE_TOKEN` first — see
[CONFIG_SERVICE_API_REFERENCE](../CONFIG_SERVICE_API_REFERENCE.md).

```bash
flapii cache get /customers        # current configuration and state
flapii cache list                  # all caches
flapii cache refresh /customers    # force a refresh now
flapii cache audit /customers      # snapshot history
flapii cache gc /customers         # apply retention (path is required)
```

## How it works

- [spec/components/caching.md](../spec/components/caching.md) — CacheManager, HeartbeatWorker, snapshots and time travel
- [spec/DESIGN_DECISIONS.md § 5](../spec/DESIGN_DECISIONS.md) — why DuckLake
- [CONFIG_REFERENCE](../CONFIG_REFERENCE.md) — every cache key
