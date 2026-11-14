---
name: flapi-cache-tuning
description: >
  Measuring and shortening flAPI DuckLake cache refreshes: reading duration_ms and
  rows_affected from `flapii cache audit` and `flapii metrics`, tracing spans and
  DuckDB profiling, timing `flapi cache refresh`, then the levers: incremental
  instead of full, watermark predicate pushdown (verified with EXPLAIN), column
  projection, overlap window size, schedule vs refresh time, data inlining,
  compaction, snapshot retention and file cleanup, DuckDB threads and memory.
  Use when a cache refresh is slow, overlaps its schedule, grows storage, or a
  user asks how to make cache updates faster or cheaper.
version: 26.10.31
user-invocable: false
---

# flAPI Cache Tuning Cheat Sheet

**flAPI:** v26.10.31 | **Skill:** Operate | **See also:** `flapi-cache-design`, `flapi-deploy`, `flapi-testing`

Tune in a loop: **measure → change one lever → refresh twice → measure again.**
Designing the cache (mode, templates, deletes) is `flapi-cache-design`.

## Critical gotchas

- **`duration_ms` times the populate SQL only.** Snapshot and watermark lookups,
  the audit write and retention are excluded, so a REST refresh call takes
  longer than `duration_ms`. Time `flapi cache refresh` for the end-to-end cost.
- **`rows_affected` is the sum of the DML counts** of all statements in the
  template (the whole template runs as one transaction). A `MERGE` counts every
  matched row, changed or not, so an overlap window inflates it.
- **Metrics reset on restart; percentiles are estimates** interpolated inside a
  histogram bucket (~25% wide), capped at the observed `max`. Use the audit log for history.
- **A `cursor.type` that does not match the column drops the watermark:** every refresh
  then loads the full source. `flapii templates expand <path> --cache` shows
  `watermark_dropped_reason`, the refresh answer a `warning`, the audit row's `message` too.
- **The scheduler is on by default** and checks every
  `ducklake.scheduler.scan-interval` (default: the shorter of
  `heartbeat.worker-interval`, 60 s, and the shortest `cache.schedule`; floor 1 s).
  A schedule shorter than an explicit scan interval is rounded up (startup warns).
  A cache is due one `schedule` after its last real refresh, not at boot.
- **A refresh still running when the next one is due is skipped**, not queued.
  A refresh longer than its schedule silently halves the freshness.
- **A function on the cursor column defeats pushdown.** `WHERE CAST(updated_at AS
  VARCHAR) > '...'` or `date_trunc(...)` scans everything; compare the raw column
  with a literal of its own type.
- **SQLite attachments and CSV files never push the watermark down**: DuckDB reads
  every row, then filters. Postgres, Parquet (row-group min/max) and DuckDB tables do.
- **`compaction.schedule` is an interval (`6h`, `1d`) or `@hourly`, `@daily`,
  `@midnight`, `@weekly`, not cron** (`0 3 * * *` stops startup). It needs
  `compaction.enabled: true`, runs once per catalog, and its last run survives
  restarts.
- **Snapshots only expire with a retention policy**: `cache.retention`, or the
  global `ducklake.retention` for caches without their own block. Without one,
  every full refresh keeps its Parquet copy forever.
- **The audit log is bounded**: the newest 1000 rows per cache table, and its own
  snapshots are expired, so a short schedule no longer grows the catalog by itself.

## 1. Measure

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=dev-token
flapii cache refresh /orders
flapii cache audit /orders -o table        # newest first: mode, status, duration, rows, snapshot
flapii cache audit /orders                 # JSON: sync_type, status, duration_ms, rows_affected, snapshot_id, message
flapii metrics --endpoint /orders          # caches[]: last, duration_ms {avg,max}, refreshes, failures, data_age_seconds
```

`flapii` prints audit and metrics exactly as the server sends them (snake_case,
whatever `--json-style` says). The same over REST:

```bash
curl -s -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" \
  http://localhost:8080/api/v1/_config/endpoints/-orders/cache/audit
curl -s -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" \
  "http://localhost:8080/api/v1/_config/metrics?endpoint=/orders"
```

A cache entry in `metrics` (real output):

```json
{"cache": "cache.cache.orders_cache", "state": "ready", "endpoint": "/orders",
 "refreshes": 3, "failures": 0, "data_age_seconds": 7,
 "last": {"mode": "merge", "status": "success", "duration_ms": 27,
          "rows_affected": 2, "snapshot_id": "20", "completed_at": "2026-11-14 10:44:29.932"},
 "duration_ms": {"avg": 26, "max": 32}}
```

`state` is `starting | ready | stale | failed | unbuilt`; `stale` means the last
refresh failed and the previous snapshot is still served. MCP config tools:
`flapi_get_cache_audit`, `flapi_get_metrics` (optional `endpoint`).

Read the numbers:

| Signal | Meaning |
|---|---|
| `rows_affected` ≈ source row count on every refresh | the delta branch is not taken (no cursor, or watermark dropped: see the server log) |
| `rows_affected` small but `duration_ms` large | the source is still scanned in full: pushdown problem |
| `duration_ms` close to the schedule interval | refreshes start to be skipped |
| `data_age_seconds` ≫ schedule | scheduler disabled, refreshes skipped, or failing (`failures`) |

End to end, without a server (the catalog must not be open elsewhere):

```bash
time flapi cache refresh -c flapi.yaml --endpoint /orders
```

Its JSON summary carries `duration_ms` and `rows_affected` per endpoint; add
`--full` to time a rebuild from scratch.

### Tracing

With `tracing.enabled: true` (see `flapi-testing` / docs/OBSERVABILITY.md), a
refresh triggered over REST or `flapii` is one `SERVER` span for the
`POST .../cache/refresh` request with `duckdb.query` children: the populate
script is one span (its `db.operation.name` is the verb of the first statement),
next to the watermark `SELECT`s, the audit `INSERT` and the retention `CALL`.
Warm-up and scheduled refreshes have no request, so their `duckdb.query` spans
are root spans. `tracing.db_profiling: summary` adds DuckDB's own metrics
(`flapi.db.latency_ms`, `flapi.db.bytes_read`, `flapi.db.blocked_thread_time_ms`)
to each database span. There is no dedicated cache-refresh span.

`flapii templates expand <path> --cache` renders the populate template with the
live watermark without executing it; `--log-level debug` also logs each rendered
script (`Processed query:`).

## 2. Symptom → lever

| Symptom | Lever |
|---|---|
| Every refresh rereads the whole source | Go incremental: `cursor` (append) or `cursor` + `primary-key` (merge) |
| Incremental, but still slow with few rows changed | Push the watermark to the source; check with `EXPLAIN` (§3) |
| Wide source, few served columns | Project only the needed columns |
| Merge rewrites rows that did not change | `WHEN MATCHED AND (t.a IS DISTINCT FROM s.a OR ...) THEN UPDATE` |
| Overlap window rereads a lot | Shrink it to the measured maximum lateness |
| Refresh time ≥ schedule | Lengthen `schedule`, or make the refresh cheaper first |
| Many tiny refreshes, many small files | `ducklake.data-inlining-row-limit`; `ducklake.compaction` |
| Metadata / snapshot count, storage keep growing | `cache.retention` or global `ducklake.retention`; `flapii cache gc <path>` |
| CPU-bound transform, or spilling | `duckdb.threads`, `duckdb.max_memory` |
| Startup spends minutes warming | `ducklake.warmup: if-empty` with a persistent catalog, or build in CI (`flapi-cache-design`) |
| Write endpoints slow since write-through | `invalidate-on-write` runs the populate synchronously: make it incremental, or switch to `refresh-on-write` (after the response) |

## 3. Push the watermark to the source

Get the delta query exactly as the next refresh would run it, without running it:

```bash
flapii templates expand /orders --cache     # {target, mode, expanded, variables}: live watermark filled in
```

Then `EXPLAIN` it in a DuckDB shell that attaches the same source (same
`ATTACH` / secrets as the connection's `init`). An endpoint cannot return
`EXPLAIN`: pagination wraps every serving query in `SELECT * FROM (...)`.

```sql
EXPLAIN
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('examples/data/customers.parquet')
WHERE c_custkey > 1000;
```

Pushed down, the predicate sits **inside** the scan node:

```text
│        READ_PARQUET       │
│        Projections:       │
│         c_custkey ...     │
│          Filters:         │
│      c_custkey>1000       │
```

Not pushed down, a separate `FILTER` node sits **above** the scan
(`SQLITE_SCAN`, `READ_CSV`, or any scan when the column is wrapped in a function).
`EXPLAIN ANALYZE` shows rows read per node. Fixes:

- Compare the raw column to a typed literal: `updated_at > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'`.
- Declare `cursor.type` as the column's real type so the watermark is a valid literal.
- For SQLite, send the delta query to SQLite itself:
  `SELECT * FROM sqlite_query('src', 'SELECT id, updated_at FROM orders WHERE updated_at > ''{{{cache.previousSnapshotTimestamp}}}''')`
  (it returns text columns; cast them). Postgres has `postgres_query(...)`, and
  its scanner pushes filters by default (`pg_experimental_filter_pushdown`).
- Index (Postgres) or partition/cluster (BigQuery, Parquet sort order) the source
  on the cursor column.

## 4. Project, then merge only real changes

```yaml
# flapi: endpoint group=lean file=customers-lean.yaml
url-path: /customers-lean
template-source: customers-lean.sql
connection: [customers]
cache:
  enabled: true
  table: customers_lean
  cursor:
    column: c_custkey
    type: integer
  template-file: customers-lean_cache.sql
# flapi: check GET /customers-lean -> 200
```

```sql
-- flapi: template group=lean file=customers-lean_cache.sql
{{^cache.previousSnapshotTimestamp}}
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('{{{ conn.path }}}');
{{/cache.previousSnapshotTimestamp}}
{{#cache.previousSnapshotTimestamp}}
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('{{{ conn.path }}}')
WHERE c_custkey > {{{cache.previousSnapshotTimestamp}}};
{{/cache.previousSnapshotTimestamp}}
```

```sql
-- flapi: template group=lean file=customers-lean.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}} ORDER BY c_custkey
```

Three columns instead of eight, and a key predicate Parquet can prune on. For
merge, a guarded update keeps unchanged rows out of the rewrite and out of
`rows_affected` (measured: a 1-day overlap re-read 3 rows, `rows_affected` 1):

```sql
WHEN MATCHED AND (t.name IS DISTINCT FROM s.name OR t.tier IS DISTINCT FROM s.tier
  OR t.updated_at IS DISTINCT FROM s.updated_at) THEN UPDATE SET
  name = s.name, tier = s.tier, updated_at = s.updated_at
```

## 5. Overlap window and schedule

- The window must cover the **maximum** lateness you observe, no more. Measure
  it at the source: `max(ingested_at - event_time)` over a week.
- Every refresh rereads the window: cost ≈ rows in the window × refreshes per day.
  With a 1-hour window and a 1-minute schedule each row is read ~60 times.
- Keep `duration_ms.max` well below the schedule interval (rule of thumb: < 50 %).
  After a restart the next scheduled refresh is one schedule after the last real
  one (warm-up, manual and config-service refreshes count).
- Endpoints naming the same table share one schedule and one warm-up.

## 6. Storage: inlining, compaction, retention, cleanup

```yaml
# flapi: config group=tuned
project-name: tuned-cache
project-description: Cache with storage and engine tuning
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: ./data/customers.parquet
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  data-inlining-row-limit: 1000
  warmup: if-empty
  retention:                       # every cache without its own retention block
    keep-last-snapshots: 48
  compaction:
    enabled: true
    schedule: '@daily'
```

```yaml
# flapi: endpoint group=tuned file=segments.yaml
url-path: /segments
template-source: segments.sql
connection: [customers]
cache:
  enabled: true
  table: segments
  schedule: 15m
  template-file: segments_cache.sql
  retention:
    keep-last-snapshots: 24
    max-snapshot-age: 7d
# flapi: check GET /segments -> 200
```

```sql
-- flapi: template group=tuned file=segments_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT c_mktsegment AS segment, count(*) AS customers, round(avg(c_acctbal), 2) AS avg_balance
FROM read_parquet('{{{ conn.path }}}')
GROUP BY c_mktsegment;
```

```sql
-- flapi: template group=tuned file=segments.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}} ORDER BY segment
```

- **`data-inlining-row-limit`** is passed to DuckLake's `ATTACH` as
  `DATA_INLINING_ROW_LIMIT`: writes up to that many rows are stored in the
  catalog instead of a new Parquet file. DuckLake already inlines small writes
  by default; raise the limit when deltas are larger than that but frequent.
- **Compaction** runs `ducklake_merge_adjacent_files`, `ducklake_rewrite_data_files`
  and a file cleanup once per catalog when due; the audit log records it as
  `sync_type` `compaction`. A one-shot `flapi cache refresh` also compacts when due.
- **Retention** (`cache.retention`, else `ducklake.retention`; an endpoint block
  replaces the global one) runs after every refresh of that endpoint and on
  `flapii cache gc <path>`; it expires only this table's snapshots, so time travel
  to them stops working. Without any retention `gc` does nothing.
- **Files of expired snapshots are deleted** right after the expiry
  (`ducklake_cleanup_old_files`), so disk usage follows retention. A full refresh
  (`CREATE OR REPLACE`) writes a new copy of the table every time: measured, six
  full refreshes under `keep-last-snapshots: 3` leave 3 Parquet files.
- **`duckdb.threads` / `duckdb.max_memory`** (any key under `duckdb:` is passed to
  DuckDB as a setting, e.g. `threads: 4`, `max_memory: 2GB`) apply to the whole
  process: serving and refreshing share them. More threads help scans and
  aggregates; a memory limit makes large merges spill instead of failing.


## Canonical tuning loop

1. Baseline: `flapii cache refresh <path>` twice (so the second one is the
   steady-state delta), then `flapii cache audit <path> -o table`; note
   `duration_ms`, `rows_affected`, and `flapii metrics --endpoint <path>` (`duration_ms.max`,
   `data_age_seconds`).
2. Classify with the symptom table: full rescans, no pushdown, wide rows,
   no-op updates, window too wide, schedule too tight, storage growth.
3. For a slow delta, render it (`flapii templates expand <path> --cache`) and
   `EXPLAIN` it in DuckDB; fix the predicate until the filter is inside the scan node.
4. Change **one** lever; `flapi -c flapi.yaml --validate-config`; restart if
   you changed flapi.yaml (a populate template is re-read on each refresh; after
   changing what it builds, `flapii cache refresh <path> --full`).
5. Change the source, refresh twice, re-read the audit: same rows, lower
   `duration_ms`. Check the data is still right (reconcile counts, see `flapi-cache-design`).
6. Set `schedule` to at least twice `duration_ms.max`; add retention (it also
   reclaims the files).
7. Report before/after `duration_ms`, `rows_affected` and the schedule.
