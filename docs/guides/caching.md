# Caching an expensive query

**Goal:** serve a slow or costly query from a local table that flAPI keeps
fresh for you.

flAPI's cache is a **DuckLake table**, not a per-request memo. A *populate*
template writes the table; your endpoint's *serving* template reads it. There is
no hit/miss branch on the request path: every request reads the table, and no
request ever triggers a refresh.

**Before you start:** you have a project with a working endpoint
([Getting started](./getting-started.md), [Building a REST endpoint](./rest-endpoints.md)).

---

## 1. Cache a query in five minutes

Three steps: enable DuckLake once, add a `cache:` block to the endpoint, and
write the template that fills the table.

The examples run as written. Make a project directory `shop/` with a `sqls/`
and a `data/` directory, and fetch the sample data (or clone the
[flAPI repository](https://github.com/DataZooDE/flapi) and use its `examples/data/`):

```bash
mkdir -p shop/sqls shop/data && cd shop
curl -L -o data/customers.parquet https://github.com/DataZooDE/flapi/raw/main/examples/data/customers.parquet
```

**Enable DuckLake** in `shop/flapi.yaml`. This is the catalog every cache table
lives in:

<!-- flapi: config group=quick -->
```yaml
# flapi.yaml
project-name: shop
project-description: Customer balances, served from a cache
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: ./data/customers.parquet
ducklake:
  enabled: true
  alias: cache                      # {{cache.catalog}} in templates
  metadata-path: ./cache.ducklake   # the catalog (a DuckDB file here)
  data-path: ./cache_data           # a directory for the Parquet data files
```

**Add a cache block** to the endpoint. `template-source` serves requests as
before; `template-file` is the new populate template:

<!-- flapi: endpoint group=quick file=segments.yaml -->
```yaml
# sqls/segments.yaml
url-path: /segments
template-source: segments.sql          # serves requests: reads the cache table
connection: [customers]
request:
  - field-name: segment
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [AUTOMOBILE, BUILDING, FURNITURE, HOUSEHOLD, MACHINERY]
cache:
  enabled: true
  table: segments_cache
  schedule: 1h                         # rebuild every hour
  template-file: segments_cache.sql    # fills the cache table
```
<!-- flapi: check GET /segments -> 200 contains "AUTOMOBILE" -->
<!-- flapi: check GET /segments?segment=BUILDING -> 200 contains "BUILDING" -->
<!-- flapi: check GET /health -> 200 contains "ready" -->

**Write the populate template.** flAPI runs it as written, so it must write the
table itself:

<!-- flapi: template group=quick file=segments_cache.sql -->
```sql
-- sqls/segments_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT c_mktsegment       AS segment,
       count(*)           AS customers,
       round(sum(c_acctbal), 2) AS balance
FROM read_parquet('{{{ conn.path }}}')
GROUP BY c_mktsegment;
```

**Point the serving template at the cache table.** A cached endpoint's serving
template gets `{{cache.catalog}}`, `{{cache.schema}}` and `{{cache.table}}`:

<!-- flapi: template group=quick file=segments.sql -->
```sql
-- sqls/segments.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE 1=1
{{#params.segment}}
  AND segment = '{{{ params.segment }}}'
{{/params.segment}}
ORDER BY segment
```

Start the server and ask for the data:

```bash
flapi -c flapi.yaml --config-service      # --config-service lets flapii talk to it
curl -s localhost:8080/segments
# {"data":[{"segment":"AUTOMOBILE","customers":302,"balance":1395695.72}, ...],"next":"","total_count":5}
```

**See that it is served from the cache.** The serving template reads only
`cache.segments_cache`, so data in the answer came from the cache table. The
server confirms it in three places:

- the startup log: `Cache scheduler on: 1 scheduled cache(s), checked every 60 s`;
- `GET /health` counts the cache: `"caches":{"ready":1,"total":1,...}`, `"status":"ready"`;
- the refresh history (the audit log, one row per build):

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=...      # the token the server printed or was given
flapii cache audit /segments -o table
# │ Time                       │ Mode │ Status  │ Duration Ms │ Rows │ Snapshot │ Message                      │
# │ 2026-11-25T12:42:52.445000 │ full │ success │ 13          │ 5    │ 7        │ Cache refreshed successfully │
```

That is a working cache. The rest of this guide deepens it one step at a time.

<a id="writing-the-populate-template"></a>
### The populate template, in detail

- flAPI runs it **verbatim**, all statements in **one transaction**: if any
  statement fails, the refresh changes nothing and the endpoint keeps serving
  the previous snapshot. Do not add your own `BEGIN`/`COMMIT`.
- It must **write** the table: `CREATE OR REPLACE TABLE … AS SELECT …`,
  `INSERT INTO …` or `MERGE INTO …`. A statement that is only a `SELECT` is
  refused before anything runs (`cache template must write
  {{cache.catalog}}.{{cache.schema}}.{{cache.table}} … statement N of the
  template is a bare SELECT, which builds nothing`). A guard `SELECT` before the
  last statement is refused too, with "its result is discarded. Wrap a check in
  CREATE OR REPLACE TEMP TABLE _chk AS SELECT ...". After the script, the cache
  table must exist, or the refresh fails and is rolled back.
- Check a template before the refresh does with
  `POST .../template/expand?target=cache&validate_only=1`: it parses and binds
  each statement without running anything
  ([API reference](../CONFIG_SERVICE_API_REFERENCE.md)).
- `flapii templates expand /segments --cache` prints the populate template as
  the next refresh renders it.
- `cache.schema` defaults to `cache`; `{{cache.catalog}}` is `ducklake.alias`.

**No `template-file`?** Then the endpoint's own `template-source` is the
populate SQL. When it renders to a single `SELECT`, flAPI materialises it as
`CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS <select>`:
a full refresh on every run, whatever `primary-key`/`cursor` imply, recorded as
`full` in the audit log. Startup warns (`cache on <endpoint> has no template-file:
its endpoint template is materialised with a full refresh; ...`). The endpoint
then serves its `template-source`, which reads the source, not the cache. For a
real cache, always declare `template-file` and point `template-source` at the
cache table.

---

## 2. When it refreshes
<a id="when-it-refreshes"></a>

| Trigger | What happens |
|---|---|
| **Startup** | Caches are warmed: `ducklake.warmup` (or per cache `cache.warmup`) `always` (default) re-runs every populate template, `if-empty` builds only tables that do not exist yet, `never` builds none. |
| **Schedule** | `cache.schedule` (`30s`, `5m`, `6h`, `1d`). The first scheduled refresh comes one `schedule` after the last real refresh, not at boot. |
| **On demand** | `flapii cache refresh <path>`, with `--full` to rebuild from scratch. |
| **After a write** | `invalidate-on-write` / `refresh-on-write` on a write endpoint, see [section 4](#4-writes-that-keep-the-cache-fresh). |
| **From a pipeline** | `flapi cache refresh -c flapi.yaml --all`, no server. |
| **A GET request** | **Never.** A request reads the table as it is. |

### At startup

The HTTP listener opens before warm-up finishes. Until its cache is built, a
cached endpoint answers `503` with `Retry-After: 5` instead of an empty result:

```json
{"error": "cache_warming", "message": "Cache for this endpoint is still being built", "table": "segments_cache"}
```

If the **first** build fails, the endpoint keeps answering `503`, now with
`"error": "cache_build_failed"`; the reason is in the server log. A refresh
that fails **later** does not take the endpoint offline: it keeps serving the
last snapshot, and the cache is `stale`. That includes a restart with
`warmup: always` whose rebuild fails while the table from the previous run
still exists.

`/health` sums it up for load balancers and monitors:

| `/health` | Status | When |
|---|---|---|
| `"status":"starting"`, `pending` list | `503` | a cache is still warming |
| `"status":"degraded"`, `failed` list | `503` | a cache's first build failed |
| `"status":"degraded"`, `stale` / `unbuilt` list | `200` | a later refresh fails, or warm-up was skipped and a table does not exist yet |
| `"status":"ready"` | `200` | every cache is built |

With `warmup: never` a missing table is **unbuilt**: its endpoint answers `503`
`cache_not_built` until the scheduler, `flapii cache refresh` or
`flapi cache refresh` builds it. `--no-cache-warmup` forces `never` for one run
(e.g. in CI); `--cache-warmup if-empty` or `FLAPI_CACHE_WARMUP` pick another
mode. Precedence: `ducklake.read-only` > flag/env > `cache.warmup` >
`ducklake.warmup`. Several endpoints that name the same table share one
warm-up.

### On a schedule

The scheduler is **on by default**: every `cache.schedule` is honoured without
further configuration (`ducklake.scheduler.enabled: false` opts out; startup
then warns). It runs per cache table: a cache is due one `schedule` after its
last real refresh (this run's warm-up, else the table's newest snapshot, else
process start), and manual refreshes count. Schedules are checked every
`ducklake.scheduler.scan-interval` (default: the shorter of
`heartbeat.worker-interval`, 60 s, and the shortest `cache.schedule`).
`cache.schedule` must be `<number>[s|m|h|d]`; anything else stops startup.

### On demand

`flapii` talks to the running server's config service, so start flAPI with
`--config-service` and export `FLAPI_CONFIG_SERVICE_TOKEN` first:

```bash
flapii cache refresh /segments           # refresh now
flapii cache refresh /segments --full    # drop the table and rebuild it from scratch
```

```json
{"endpoint": "/segments", "status": "refreshed", "mode": "full", "full": true,
 "rows_affected": 5, "duration_ms": 10, "snapshot_id": "9"}
```

The REST form is `POST /api/v1/_config/endpoints/-segments/cache/refresh`
(`?full=true` for a rebuild), with the token as `X-Config-Token` or
`Authorization: Bearer` ([API reference](../CONFIG_SERVICE_API_REFERENCE.md)).
The slug is the path with `/` turned into `-` (`/segments` is `-segments`);
`flapii` builds it for you.

```bash
curl -s -X POST -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" \
  localhost:8080/api/v1/_config/endpoints/-segments/cache/refresh
```
<!-- flapi: check group=quick POST /api/v1/_config/endpoints/-segments/cache/refresh header=X-Config-Token:skill-check-token -> 200 contains "refreshed" -->
<!-- flapi: check group=quick POST /api/v1/_config/endpoints/-segments/cache/refresh?full=true header=X-Config-Token:skill-check-token -> 200 contains "refreshed" -->

`--full` erases the incremental watermark **and** drops the cache table, in
the same transaction, so the template's first-run branch rebuilds it: an
append cache is rebuilt, not appended to twice. Use it after a template or
source change. Readers keep the previous snapshot until it commits.

A refresh of a table that is already refreshing is not started again:
`POST .../cache/refresh` answers `409` with `Retry-After` and
`{"status":"already_running"}`, and `flapii cache refresh` exits non-zero.

### From a pipeline

`flapi cache refresh -c flapi.yaml --all` (or `--endpoint /segments`, with
`--full` to rebuild) refreshes once and exits (`0` ok, `1` a refresh failed,
`2` usage). Run it after the upstream build. Replicas then attach the same
catalog with `ducklake.read-only: true`; see [serverless.md](./serverless.md).
With a DuckDB-file catalog the one-shot refresh fails (exit `1`) while a server
holds the file open, with `{"error":"a running flapi holds the DuckLake
catalog lock; refresh through the server: flapii cache refresh <path>"}`.

---

## 3. Choose a refresh strategy
<a id="choosing-a-refresh-strategy"></a>

The mode is **inferred** from the keys you declare. There is no `mode:` and no
`refresh:` key.

| You declare | Mode | Your template | Good for |
|---|---|---|---|
| no `cursor` | **full** | rebuilds the table (`CREATE OR REPLACE TABLE … AS`) | small tables; anything cheap to recompute |
| `cursor` only | **append** | inserts rows past the watermark | append-only data: events, logs, readings |
| `cursor` **and** `primary-key` | **merge** | upserts changed rows (`MERGE`) | mutable rows: customers, orders |

A `primary-key` without a `cursor` is still full. The mode is what the audit
log records and what `{{params.cacheMode}}` renders (`full`, `append`, `merge`).
Start with full; go incremental when a refresh gets too slow.

### The watermark

An incremental template reads only what is new since the last refresh, using
`{{cache.previousSnapshotTimestamp}}`. With a `cursor:` it is `max(<cursor>)`
over the rows already cached, in the cursor's own type. It is **absent** on the
first run, after `--full`, for an empty table, and when the value does not look
like `cursor.type` (that refresh then loads the full source, with a warning).

So every incremental template has two branches:

- `{{^cache.previousSnapshotTimestamp}}` (no watermark): build the table from
  the whole source with `CREATE OR REPLACE`;
- `{{#cache.previousSnapshotTimestamp}}` (a watermark): add only the delta.

Use `CREATE OR REPLACE` in the first branch, not "create an empty table, then
insert". The watermark can also be missing for an existing table (empty, or a
value that does not look like `cursor.type`); an unconditional `INSERT` after a
`CREATE TABLE IF NOT EXISTS` would then insert the whole source on top of the
cached rows.

Quote the watermark for its type: bare for an integer cursor,
`TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'` for a timestamp,
`DATE '...'` for a date. Use triple braces inside quotes.

### Append

`cursor` only. This example is a new project directory `orders/`, over the
Northwind sample database:

```bash
mkdir -p orders/sqls orders/data && cd orders
curl -L -o data/northwind.sqlite https://github.com/DataZooDE/flapi/raw/main/examples/data/northwind.sqlite
```

<!-- flapi: config group=append -->
```yaml
# flapi.yaml
project-name: orders
project-description: Northwind orders, appended to a cache
template:
  path: ./sqls
connections:
  northwind:
    init: |
      INSTALL sqlite;
      LOAD sqlite;
      ATTACH IF NOT EXISTS './data/northwind.sqlite' AS nw (TYPE sqlite, READ_ONLY);
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
```

The order id is the cursor:

<!-- flapi: endpoint group=append file=orders.yaml -->
```yaml
# sqls/orders.yaml
url-path: /orders
template-source: orders.sql
connection: [northwind]
cache:
  enabled: true
  table: orders_cache
  schedule: 10m
  cursor:
    column: OrderID
    type: integer
  template-file: orders_cache.sql
```
<!-- flapi: check GET /orders -> 200 contains "OrderID" -->

<!-- flapi: template group=append file=orders_cache.sql -->
```sql
-- sqls/orders_cache.sql
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

<!-- flapi: template group=append file=orders.sql -->
```sql
-- sqls/orders.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
ORDER BY OrderID DESC
```

**Late rows.** A row that arrives with a cursor value at or below the watermark
is skipped by a strict `>`, permanently, and the refresh still reports success.
If your source has late rows, re-read an overlap window:
`AND updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE`.
The window must be at least the **maximum** lateness plus a margin; a row later
than the window is still lost. In append mode the overlap **duplicates the
whole window on every refresh**, even when nothing changed. Either use merge,
or exclude rows already cached with an anti-join over the same window:

```sql
  AND updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE
  AND NOT EXISTS (
    SELECT 1 FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS c
    WHERE c.id = s.id
      AND c.updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE
  )
```

(`s` is the source alias.) Use `NOT EXISTS` rather than `id NOT IN (...)`: a
single NULL id makes `NOT IN` match no row, so every re-read row is silently
dropped.

**One clock for a timestamp cursor.** A naive `TIMESTAMP` has no zone: one
writer writing local time next to UTC rows moves `max(cursor)` ahead, and a
later UTC row below it is skipped. Write UTC everywhere, or use a `TIMESTAMPTZ`
column (`cursor.type: timestamptz`).

To check that nothing was lost, run a reconcile query from time to time:

```sql
SELECT s.id, s.updated_at
FROM source_events s
ANTI JOIN cache.cache.events_cache c ON s.id = c.id;   -- rows the cache is missing
```

### Merge

`cursor` and `primary-key`. This example is a new project directory `crm/`
that reads a CSV extract with a soft-delete flag. Its `flapi.yaml`:

<!-- flapi: config group=crm -->
```yaml
# flapi.yaml
project-name: crm-cache
project-description: Merge cache over a CSV extract
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

The extract, `src/customers.csv`:

<!-- flapi: file group=crm file=src/customers.csv -->
```text
id,name,tier,updated_at,is_deleted
1,Ada,gold,2026-01-01 10:00:00,false
2,Bob,silver,2026-01-01 11:00:00,false
3,Cy,bronze,2026-01-01 12:00:00,true
```

<!-- flapi: endpoint group=crm file=customers.yaml -->
```yaml
# sqls/customers.yaml
url-path: /customers
template-source: customers.sql
connection: [crm]
cache:
  enabled: true
  table: customers_cache
  schedule: 15m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  template-file: customers_cache.sql
```
<!-- flapi: check GET /customers -> 200 contains "Ada" -->

<!-- flapi: template group=crm file=customers_cache.sql -->
```sql
-- sqls/customers_cache.sql
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
WHEN NOT MATCHED THEN INSERT *;
{{/cache.previousSnapshotTimestamp}}
```

<!-- flapi: template group=crm file=customers.sql -->
```sql
-- sqls/customers.sql
SELECT id, name, tier, updated_at
FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE NOT is_deleted
ORDER BY id
```

- DuckDB's form is `INSERT *` (or `INSERT BY NAME`), not `INSERT (*) VALUES (s.*)`.
- **`>=` is right for merge**: a re-read boundary row collapses on the key. For
  late rows, subtract an overlap window (`- INTERVAL 1 HOUR`); re-reading is
  idempotent here. In append mode `>=` duplicates.
- **One `MERGE`, one `UPDATE`/`DELETE` action.** DuckLake rejects a `MERGE` with
  both a `DELETE` and an `UPDATE` action (`MERGE INTO with DuckLake only
  supports a single UPDATE/DELETE action currently`). See the delete patterns
  below.

**`rows_affected` counts no-op updates.** With an overlap window every re-read
row matches and is "updated" to the same values, and each one is counted.
Guard the update so only real changes count (still one action):

```sql
WHEN MATCHED AND (t.name IS DISTINCT FROM s.name
               OR t.tier IS DISTINCT FROM s.tier
               OR t.updated_at IS DISTINCT FROM s.updated_at
               OR t.is_deleted IS DISTINCT FROM s.is_deleted)
  THEN UPDATE SET name = s.name, tier = s.tier, updated_at = s.updated_at, is_deleted = s.is_deleted
```

Measured on a 6-row table re-read with an overlap: unguarded, every refresh
reported `rows_affected: 5` with no source change; guarded, `0`.

### Deletes in merge mode

flAPI does not detect deleted source rows: an incremental refresh only sees
rows the source still returns. Model deletes in the data, usually as a
soft-delete flag (or a `deleted_at` column) that bumps the cursor when it is
set. Then pick one of two patterns.

**Keep tombstones in the cache** (the example above). The deleted row is
updated to `is_deleted = true`, stays in the table, and the serving template
filters it (`WHERE NOT is_deleted`). Because the tombstone keeps its
`updated_at`, the watermark moves past the delete. The cost is storing deleted
rows.

**Remove them: a `DELETE`, then a single-action `MERGE`.** Keep the `MERGE`
last. A second endpoint over the same extract, in its own table:

<!-- flapi: endpoint group=crm file=customers-live.yaml -->
```yaml
# sqls/customers-live.yaml
url-path: /customers-live
template-source: customers-live.sql
connection: [crm]
cache:
  enabled: true
  table: customers_live
  schedule: 15m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  template-file: customers-live_cache.sql
```
<!-- flapi: check GET /customers-live -> 200 contains "Bob" -->

<!-- flapi: template group=crm file=customers-live_cache.sql -->
```sql
-- sqls/customers-live_cache.sql
{{^cache.previousSnapshotTimestamp}}
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, name, tier, updated_at, is_deleted
FROM read_csv('{{{conn.path}}}')
WHERE NOT is_deleted;
{{/cache.previousSnapshotTimestamp}}
{{#cache.previousSnapshotTimestamp}}
DELETE FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE id IN (
  SELECT id FROM read_csv('{{{conn.path}}}')
  WHERE is_deleted AND updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'
);
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT id, name, tier, updated_at, is_deleted
  FROM read_csv('{{{conn.path}}}')
  WHERE NOT is_deleted AND updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET name = s.name, tier = s.tier, updated_at = s.updated_at
WHEN NOT MATCHED THEN INSERT *;
{{/cache.previousSnapshotTimestamp}}
```

<!-- flapi: template group=crm file=customers-live.sql -->
```sql
-- sqls/customers-live.sql
SELECT id, name, tier, updated_at
FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
ORDER BY id
```

Deleted rows leave the cache, so they no longer count towards the watermark
(`max(updated_at)` over the cached rows): when the newest change was a delete,
the next refresh re-reads it. That is harmless (the `DELETE` is idempotent),
but the watermark only advances with surviving rows.

If the source hard-deletes rows and keeps no trace, an incremental cache cannot
see the delete. Use a full refresh for that table, or reconcile periodically:
a scheduled `flapii cache refresh <path> --full`, or
`DELETE FROM <cache> AS c WHERE NOT EXISTS (SELECT 1 FROM source s WHERE s.id = c.id)`
if the key set is cheap to read (not `NOT IN`, which deletes nothing once the
source has a NULL id).

### Variables in a populate template

| Variable | Contains |
|---|---|
| `{{cache.catalog}}` `{{cache.schema}}` `{{cache.table}}` | Where to write (the serving template gets these three too) |
| `{{cache.previousSnapshotTimestamp}}` | The incremental watermark, see above. Without a `cursor:` (full mode) it is the commit time of the table's last refresh |
| `{{cache.previousSnapshotId}}` | The id of that snapshot. Absent for a table no snapshot has touched yet, and after `--full` |
| `{{cache.snapshotTimestamp}}` `{{cache.snapshotId}}` | Commit time (UTC) and id of the newest snapshot that wrote this table **before** this refresh, i.e. its last refresh that changed rows. Never this refresh's own snapshot (that id is in the audit row afterwards). **Not a watermark** |
| `{{cache.cursorColumn}}` `{{cache.cursorType}}` | Your declared cursor |
| `{{cache.primaryKeys}}` | Your declared key, comma-separated |
| `{{cache.schedule}}` | Your declared schedule |
| `{{params.cacheMode}}` | `full`, `append` or `merge` |

---

## 4. Writes that keep the cache fresh

A `POST`, `PUT`, `PATCH` or `DELETE` endpoint that changes the source can
refresh the cache right after the write. It answers `{"rows_affected": n}`,
with `201` for `POST` and `200` for the others.

A write refreshes **only the cache its own `cache:` block names**, and that
block needs `table`: give the write endpoint a `cache:` block with the **same
`table` and `template-file`** as the endpoint that serves the data, plus one of:

| Key | When the cache is refreshed |
|---|---|
| `invalidate-on-write: true` | **Synchronously**, after the write and before the response. The caller's next read sees its own write |
| `refresh-on-write: true` | **In the background**, after the response. Bursts of writes are coalesced into one refresh per endpoint; the next read may still see old data for the length of the refresh |
| both | Synchronous only |

MCP tool calls that write follow the same rules.

This example is a new project directory `items/`. Its source is a DuckDB table
the connection creates, so it needs no data files. Its `flapi.yaml`:

<!-- flapi: config group=write-through -->
```yaml
# flapi.yaml
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

The reading endpoint:

<!-- flapi: endpoint group=write-through file=items.yaml -->
```yaml
# sqls/items.yaml
url-path: /items
template-source: items.sql
connection: [app]
cache:
  enabled: true
  table: items_cache
  template-file: items_cache.sql
```
<!-- flapi: check GET /items -> 200 -->

<!-- flapi: template group=write-through file=items_cache.sql -->
```sql
-- sqls/items_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, name FROM items;
```

<!-- flapi: template group=write-through file=items.sql -->
```sql
-- sqls/items.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}} ORDER BY id
```

The writing endpoint names the same table and populate template:

<!-- flapi: endpoint group=write-through file=items-create.yaml -->
```yaml
# sqls/items-create.yaml
url-path: /items
method: POST
template-source: items-create.sql
connection: [app]
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
```
<!-- flapi: check POST /items body={"name":"widget"} -> 201 contains "rows_affected" -->
<!-- flapi: check GET /items -> 200 contains "widget" -->

<!-- flapi: template group=write-through file=items-create.sql -->
```sql
-- sqls/items-create.sql
INSERT INTO items SELECT COALESCE(MAX(id), 0) + 1, '{{{ params.name }}}' FROM items
```

The `GET` right after the `POST` returns `widget`: the synchronous refresh ran
before the `201`. A `DELETE` endpoint works the same way; this one refreshes
after the response:

<!-- flapi: endpoint group=write-through file=items-delete.yaml -->
```yaml
# sqls/items-delete.yaml
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
```
<!-- flapi: check DELETE /items/1 -> 200 contains "rows_affected" -->

<!-- flapi: template group=write-through file=items-delete.sql -->
```sql
-- sqls/items-delete.sql
DELETE FROM items WHERE id = {{ params.id }}
```

- Without `table` in the write endpoint's block nothing is refreshed, and
  without `template-file` the "populate" SQL would be the write endpoint's own
  template. Always copy both from the reading endpoint.
- The write's latency now includes the populate SQL. With a big table use an
  incremental template, or drop write-through and refresh on a schedule.
- Endpoints that name the same table share one warm-up and one schedule; put
  `schedule` on one of them.
- A write endpoint with a `cache:` block is gated like a read: it answers `503`
  while that cache has never been built.

---

## 5. Snapshots, time travel and retention

Every refresh that writes rows commits a **DuckLake snapshot**; the audit row
records its `snapshot_id` (null for a 0-row refresh, which writes none). Readers
always see a whole snapshot, never a half-finished refresh.

### Time travel

A serving template can read an older snapshot with `AT (VERSION => n)` or
`AT (TIMESTAMP => ...)`, placed right after the table name. Back in the `shop`
project from section 1, add a second cached endpoint:

<!-- flapi: endpoint group=quick file=segments-history.yaml -->
```yaml
# sqls/segments-history.yaml
url-path: /segments-history
template-source: segments-history.sql
connection: [customers]
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
  table: segments_history
  schedule: 1h
  template-file: segments-history_cache.sql
  retention:
    keep-last-snapshots: 5
```
<!-- flapi: check GET /segments-history -> 200 contains "MACHINERY" -->
<!-- flapi: check GET /segments-history?version=999999 -> 404 contains "snapshot_not_found" -->
<!-- flapi: check GET /segments-history?at=2000-01-01%2000:00:00 -> 404 contains "snapshot_not_found" -->
<!-- flapi: check GET /segments-history?at=2999-01-01%2000:00:00 -> 400 contains "snapshot_in_future" -->
<!-- flapi: check GET /segments-history?at=yesterday -> 400 -->

<!-- flapi: template group=quick file=segments-history_cache.sql -->
```sql
-- sqls/segments-history_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT c_mktsegment AS segment, count(*) AS customers
FROM read_parquet('{{{ conn.path }}}')
GROUP BY c_mktsegment;
```

<!-- flapi: template group=quick file=segments-history.sql -->
```sql
-- sqls/segments-history.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
{{#params.version}}AT (VERSION => {{ params.version }}){{/params.version}}
{{#params.at}}AT (TIMESTAMP => TIMESTAMP '{{{ params.at }}}'){{/params.at}}
ORDER BY segment
```

- An unknown or expired snapshot answers `404 {"error":"snapshot_not_found"}`;
  a timestamp in the future answers `400 {"error":"snapshot_in_future"}`.
- Interpolate the timestamp **inside a section**, guarded by a strict regex as
  above. A bound parameter does not work there: DuckLake resolves `AT` before
  parameters are bound.
- A naive timestamp is UTC, like the audit log's times.
- Snapshot ids come from the audit log (`flapii cache audit <path>`), which also
  keeps the ids of snapshots retention has expired. List the live ones with an
  uncached endpoint over `ducklake_snapshots('<alias>')`:

<!-- flapi: endpoint group=quick file=snapshots.yaml -->
```yaml
# sqls/snapshots.yaml
url-path: /snapshots
template-source: snapshots.sql
connection: [customers]
```
<!-- flapi: check GET /snapshots -> 200 contains "snapshot_id" -->

<!-- flapi: template group=quick file=snapshots.sql -->
```sql
-- sqls/snapshots.sql
SELECT snapshot_id, snapshot_time, changes
FROM ducklake_snapshots('cache')
ORDER BY snapshot_id DESC
```

The list is catalog-wide: it holds the snapshots of every cache table and of the
audit table. Protect this endpoint like the data itself.

### Retention and gc

Without a policy, snapshots and their files accumulate. Set one per cache
(`cache.retention`, as above) or once for every cache without its own block:

```yaml
ducklake:
  retention:
    keep-last-snapshots: 10
    max-snapshot-age: 14d
```

An endpoint's `retention:` block replaces the global one as a whole; they are
not merged. Retention runs **after every refresh**, expires this table's old
snapshots (including the earlier incarnations a full refresh or `--full`
rebuild leaves behind), and then deletes the data files no kept snapshot of any
table can still see. After a `--full`, the old copy stays until the last
snapshot before the rebuild leaves the window: with `keep-last-snapshots: N`,
N-1 more refreshes that write. Until then the disk holds both copies.

```bash
flapii cache gc /segments-history     # apply retention now (the path is required)
```

It answers `{status, expired_snapshots, files_deleted, bytes_freed}`;
`"status": "no_policy"` when the cache has no retention policy. When it expires
nothing because ANOTHER cached table's retained snapshot can still see this
table's old files, it says so: `pinned_by` names that endpoint and its policy,
`pinned_bytes` is what it holds, and the files go when that table's retention
expires those snapshots.

Time travel to a snapshot retention has expired answers `404 snapshot_not_found`.

---

## 6. Measure and tune refresh time
<a id="measuring-and-tuning-refresh-time"></a>

Every refresh is recorded in the cache audit log, with how long the populate
statement took and how many rows it wrote:

```bash
flapii cache audit /customers          # sync_type, status, duration_ms, rows_affected, snapshot_id
flapii metrics --endpoint /customers   # last/avg/max refresh duration, data age, state
```

Without a running server, the one-shot refresh prints the same per cache
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
   `ducklake.data-inlining-row-limit` keeps small inserts in the catalog (keep
   it to a few thousand rows), and `ducklake.compaction` (`enabled: true` plus
   a `schedule` such as `24h` or `@daily`) merges small files and cleans up
   once per schedule. `flapii cache compact` runs it now.
6. **Watch merge-mode refresh time creep.** Every update or delete in a
   DuckLake table writes a delete file next to the data files, and readers apply
   all of them. Over many refreshes refresh time grows: measured on a 5M-row
   merge cache updating ~500k rows per refresh, 78 delete files after 12
   refreshes and refresh time rising from ~1.2 s to ~6.3 s. Compact the table,
   or rebuild it now and then (`flapii cache refresh <path> --full`).
   Compaction rewrites a file only above DuckLake's 95% deleted rows by default.
   Set `ducklake.compaction.delete-threshold` so it does: about refreshes
   between compactions × the share of rows each refresh changes (1% per
   refresh, five refreshes per compaction: `0.05`). Lower values rewrite more
   often (more compaction time and briefly twice the table's disk).
7. **Bound snapshot growth** with retention ([section 5](#retention-and-gc)).

The `flapi-cache-tuning` skill in the [Claude Code plugin](./claude-code.md)
walks through this loop with before/after numbers.

---

## Operating it

```bash
flapii cache get /customers        # current configuration and state
flapii cache list                  # all caches
flapii cache refresh /customers    # refresh now
flapii cache refresh /customers --full   # drop and rebuild from scratch
flapii cache audit /customers -o table   # refresh history: mode, status, duration, rows, snapshot
flapii cache gc /customers         # apply retention (path is required)
flapii cache drop-table old.customers_cache   # drop a table no endpoint uses any more
```

After changing `cache.schema` or `cache.table`, the old table stays in the
catalog: drop it with `flapii cache drop-table <old-schema>.<table>` (`409
cache_table_in_use` while an endpoint still names it).

### A source published by atomic rename

A writer that publishes a DuckDB file by atomic rename (write `next.duckdb`,
`os.replace` it over `source.duckdb`) leaves a server that attached the file once,
at startup (a connection `init` `ATTACH ... (READ_ONLY)`), reading the OLD inode
until it restarts. An uncached endpoint that reads that alias never sees the
update. The supported pattern is a small cached endpoint whose populate template
**re-attaches** on every refresh:

```yaml
# sqls/source_stats.yaml
url-path: /source-stats
method: GET
template-source: source_stats.sql
connection: [source]
cache:
  enabled: true
  table: source_stats
  schedule: 5m
  template-file: source_stats_cache.sql
```

```sql
-- sqls/source_stats_cache.sql: the refresh re-attaches, so it reads the file now at the path
DETACH DATABASE IF EXISTS src_stats;
ATTACH '{{{conn.path}}}' AS src_stats (READ_ONLY);
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT count(*) AS orders, max(updated_at) AS newest FROM src_stats.orders;
```

```sql
-- sqls/source_stats.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
```

Use a dedicated alias (`src_stats`) that no `init` attaches: an attach is
process-wide, so a shared alias is detached under whatever else reads it. Serve
the data from the cache table rather than from an uncached template that reads
the alias: an uncached template runs on every request and sees the alias only
if something attached it. Validating such a populate template
(`?target=cache&validate_only=1`) against an alias the server attached BEFORE a
rename answers a `stale` warning instead of a bind error: the check ran against
the old file, and the refresh, which re-attaches, reads the new one.

---

## Common mistakes

- **A `refresh:` or `mode:` key.** Neither exists; the mode is inferred from
  `cursor` and `primary-key` ([section 3](#3-choose-a-refresh-strategy)).
- **`rollback-window` and `delete-handling` are deprecated and have no effect**
  (startup warns). Time travel over retained snapshots replaces the
  first; handle deletes in the template ([Deletes in merge mode](#deletes-in-merge-mode)).
- **`cache.currentSnapshotTimestamp` does not exist** and renders empty. The last refresh's commit time is `{{cache.snapshotTimestamp}}`; the
  incremental watermark is `{{cache.previousSnapshotTimestamp}}`.
- **A populate template that is only a `SELECT`.** It is refused (the cache
  answers `503 cache_build_failed`); write the table with
  `CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS ...`.
- **No `template-file`.** The serving template is materialised instead, and a
  serving template that reads the cache table cannot build itself.
- **`LIMIT {{ params.limit }}` in a serving template.** `limit` and `offset` are
  reserved for pagination, which wraps your query; your `LIMIT` applies first,
  so `?limit=2&offset=2` returns `data: []`.
- **An incremental template without the `{{^cache.previousSnapshotTimestamp}}`
  branch.** `INSERT` and `MERGE` fail on the first run, before the table exists.
- **Double braces around a quoted watermark.** `{{cache.previousSnapshotTimestamp}}`
  HTML-escapes; use `'{{{cache.previousSnapshotTimestamp}}}'` inside quotes.

## Next

- Expose the cached endpoint to AI agents: [MCP tools](./mcp-tools.md)
- Cache in object storage, serve from replicas: [Cloud storage](./cloud-storage.md), [Serverless](./serverless.md)
- Watch refreshes in traces and metrics: [Observability](./observability.md)
- Every cache key: [CONFIG_REFERENCE §6](../CONFIG_REFERENCE.md#6-cache-configuration)

## How it works

- [spec/components/caching.md](../spec/components/caching.md): CacheManager, HeartbeatWorker, snapshots and time travel
- [spec/DESIGN_DECISIONS.md § 5](../spec/DESIGN_DECISIONS.md): why DuckLake
- [CONFIG_REFERENCE §2.5](../CONFIG_REFERENCE.md#25-ducklake-configuration): DuckLake, warm-up, scheduler, retention and compaction keys
