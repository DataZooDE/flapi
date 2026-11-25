# Caching an expensive query

**Goal:** serve a slow or costly query from a local table that flAPI keeps
fresh for you.

flAPI's cache is a **DuckLake table**, not a per-request memo. A *populate*
template writes the table; your endpoint's *serving* template reads it. There is
no hit/miss branch on the request path: every request reads the table, and no
request ever triggers a refresh.

**Before you start:** you know what a flAPI endpoint is, a YAML file plus a SQL
template ([Getting started](./getting-started.md),
[Building a REST endpoint](./rest-endpoints.md)), and `flapi` and `flapii` are
installed (`pip install flapi-io`). This guide builds every example from scratch,
each in its own project directory.

### Before production

The five-minute cache in section 1 is fine on a laptop. Before you deploy one:

- **Set retention.** Without it, every refresh's snapshot and its files are kept
  forever ([section 5](#retention-and-gc)).
- **Choose `warmup` with your warehouse bill in mind.** The default `always`
  re-runs every populate query on every restart and cold start; `if-empty` on
  persistent storage serves what is already there ([section 2](#at-startup)).
- **Pick a topology:** one instance; one cache per instance (N replicas cost N
  times the warehouse queries and run N schedulers); or one writer with
  read-only replicas ([section 7](#7-run-it-in-production), [Serverless](./serverless.md)).
- **Put `ducklake.data-path` (and a file catalog) on a persistent disk**, or
  every restart rebuilds from the source.
- **Monitor staleness:** a cache whose refreshes fail still answers `200`
  ([section 7](#monitoring)).

---

## 1. Cache a query in five minutes

Three steps: enable DuckLake once, add a `cache:` block to the endpoint, and
write the template that fills the table.

Make a project directory `shop/` and fetch the sample data (or clone the
[flAPI repository](https://github.com/DataZooDE/flapi) and use its `examples/data/`):

```bash
mkdir -p shop/sqls shop/data shop/src && cd shop
curl -L -o data/customers.parquet https://github.com/DataZooDE/flapi/raw/main/examples/data/customers.parquet
```

**Enable DuckLake** in `shop/flapi.yaml`. DuckLake is the catalog every cache
table lives in:

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
  targets:                          # used in section 5
    properties:
      path: ./src/targets.csv
ducklake:
  enabled: true
  alias: cache                      # the catalog name: {{cache.catalog}}
  metadata-path: ./cache.ducklake   # the catalog (a DuckDB file here)
  data-path: ./cache_data           # a directory for the Parquet data files
```

A cache table's full name has three parts: the catalog (`ducklake.alias`,
here `cache`), a schema (`cache.schema`, default `cache`) and the table
(`cache.table`). The table below is therefore `cache.cache.segments_cache`.
Templates spell it `{{cache.catalog}}.{{cache.schema}}.{{cache.table}}`.

**Define the endpoint with a `cache:` block.** `template-source` names the
serving template; `template-file` names the populate template:

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

**Point the serving template at the cache table:**

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

Start the server. Export a token for the config service first, so `flapii`
can use the same one:

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=$(openssl rand -hex 16)
flapi -c flapi.yaml --config-service       # port 8080; -p 9090 (or FLAPI_PORT) for another
```

In a **second terminal**, in `shop/`, export the same token (and
`FLAPI_BASE_URL=http://localhost:9090` if you changed the port), then ask for
the data:

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=<the same token>
curl -s localhost:8080/segments
# {"data":[{"segment":"AUTOMOBILE","customers":302,"balance":1395695.72}, ...],"next":"","total_count":5}
curl -s "localhost:8080/segments?segment=BUILDING"
```

**See that it is served from the cache.** The serving template reads only
`cache.cache.segments_cache`, so the data came from the cache table. The
server confirms it three ways:

```bash
curl -s localhost:8080/health
# {"status":"ready","caches":{"ready":1,"total":1,"stale":0,"failed":0,"unbuilt":0},...}
flapii cache audit /segments -o table      # one row per build
# │ Time                       │ Mode │ Status  │ Duration Ms │ Rows │ Snapshot │ Message                      │
# │ 2026-11-25T12:42:52.445000 │ full │ success │ 13          │ 5    │ 7        │ Cache refreshed successfully │
```

and the first terminal logged `Cache scheduler on: 1 scheduled cache(s), checked every 60 s`.

That is a working cache. The rest of this guide deepens it one step at a time.

<a id="writing-the-populate-template"></a>
### The populate template, in detail

- flAPI runs it **verbatim**, all statements in **one transaction**: if any
  statement fails, the refresh changes nothing and the endpoint keeps serving
  the previous snapshot. Do not add your own `BEGIN`/`COMMIT`.
- It must **write** the table: `CREATE OR REPLACE TABLE … AS SELECT …`,
  `INSERT INTO …` or `MERGE INTO …`. A statement that is only a `SELECT` is
  refused before anything runs, and after the script the cache table must
  exist, or the refresh is rolled back (exact messages:
  [CONFIG_REFERENCE §6.2](../CONFIG_REFERENCE.md#62-cache-readiness-during-warmup)).
- `flapii templates expand /segments --cache` prints it as the next refresh
  renders it; `POST .../template/expand?target=cache&validate_only=1` also
  binds each statement without running it
  ([API reference](../CONFIG_SERVICE_API_REFERENCE.md)).
- **Always declare `template-file`.** Without it the serving template becomes
  the populate SQL: a single `SELECT` is materialised with `CREATE OR REPLACE
  TABLE … AS`, always as a full refresh, startup warns (`cache on <endpoint> has
  no template-file …`), and a serving template that reads the cache table cannot
  build itself.

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
`warmup: always` whose rebuild fails while the previous table still exists.

`/health` sums it up:

| `/health` | Status | When |
|---|---|---|
| `"status":"starting"`, `pending` list | `503` | a cache is still warming |
| `"status":"degraded"`, `failed` list | `503` | a cache's first build failed |
| `"status":"degraded"`, `stale` / `unbuilt` list | `200` | a later refresh fails, or warm-up was skipped and a table does not exist yet |
| `"status":"ready"` | `200` | every cache is built |

With `warmup: never` a missing table is **unbuilt**: its endpoint answers `503`
`cache_not_built` until a scheduled or manual refresh builds it.
`--no-cache-warmup` forces `never` for one run (e.g. in CI); `--cache-warmup
if-empty` or `FLAPI_CACHE_WARMUP` pick another mode. Several endpoints that name
the same table share one warm-up.

### On a schedule

The scheduler is **on by default** (`ducklake.scheduler.enabled: false` opts
out; startup then warns). It runs per cache table: a cache is due one
`schedule` after its last real refresh (this run's warm-up, else the table's
newest snapshot, else process start), and manual refreshes count. Due caches
are looked for every `ducklake.scheduler.scan-interval`, which defaults to
min(`heartbeat.worker-interval` (60 s by default), the shortest
`cache.schedule`). `cache.schedule` must be `<number>[s|m|h|d]`; anything else
stops startup.

### On demand

`flapii` talks to the running server's config service (`--config-service`,
token in `FLAPI_CONFIG_SERVICE_TOKEN`, as in section 1):

```bash
flapii cache refresh /segments           # refresh now
flapii cache refresh /segments --full    # drop the table and rebuild it from scratch
```

```json
{"endpoint": "/segments", "status": "refreshed", "mode": "full", "full": true,
 "rows_affected": 5, "duration_ms": 10, "snapshot_id": "9"}
```

The REST form takes the endpoint's **slug**: the path with every `/` written
`-` and every literal `-` written `~1` (`/segments` is `-segments`,
`/customers-live` is `-customers~1live`). `flapii` builds it for you.

```bash
curl -s -X POST -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" \
  localhost:8080/api/v1/_config/endpoints/-segments/cache/refresh
curl -s -X POST -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" \
  "localhost:8080/api/v1/_config/endpoints/-segments/cache/refresh?full=true"
```
<!-- flapi: check group=quick POST /api/v1/_config/endpoints/-segments/cache/refresh header=X-Config-Token:skill-check-token -> 200 contains "refreshed" -->
<!-- flapi: check group=quick POST /api/v1/_config/endpoints/-segments/cache/refresh?full=true header=X-Config-Token:skill-check-token -> 200 contains "refreshed" -->

`--full` (`?full=true`) erases the incremental watermark **and** drops the cache
table in the same transaction, so the template's first-run branch rebuilds it:
an append cache is rebuilt, not appended to twice. Use it after a template or
source change. Readers keep the previous snapshot until it commits.

A refresh of a table that is already refreshing is not started again: the
REST call answers `409` with `Retry-After` and `{"status":"already_running"}`,
and `flapii cache refresh` exits non-zero.

### From a pipeline

`flapi cache refresh -c flapi.yaml --all` (or `--endpoint /segments`, with
`--full` to rebuild) refreshes once, without a server, and exits (`0` ok, `1`
a refresh failed, `2` usage). It prints a summary per cache, without mode or
snapshot id:

```json
{"failed": 0, "refreshed": [{"endpoint": "/segments", "schema": "cache", "table": "segments_cache",
  "status": "ok", "rows_affected": 5, "duration_ms": 18}]}
```

Run it after the upstream build; replicas then attach the same catalog with
`ducklake.read-only: true` ([serverless.md](./serverless.md)). With a
DuckDB-file catalog it fails (exit `1`) while a server holds the file open:
`a running flapi holds the DuckLake catalog lock; refresh through the server`.

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
log records and what `{{params.cacheMode}}` renders. Start with full; go
incremental when a refresh gets too slow.

### The watermark

An incremental template reads only what is new, using
`{{cache.previousSnapshotTimestamp}}`: with a `cursor:` it is `max(<cursor>)`
over the rows already cached, in the cursor's own type. It is **absent** on the
first run, after `--full`, for an empty table, and when the value does not look
like `cursor.type` (that refresh then loads the full source, with a warning).

So every incremental template has two branches:

- `{{^cache.previousSnapshotTimestamp}}` (no watermark): build the table from
  the whole source with `CREATE OR REPLACE`. Not "create an empty table, then
  insert": the watermark can be missing for a table that has rows, and an
  unconditional `INSERT` would then duplicate the whole source.
- `{{#cache.previousSnapshotTimestamp}}` (a watermark): add only the delta.

Quote the watermark for its type: bare for an integer cursor,
`TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'` for a timestamp,
`DATE '...'` for a date. Use triple braces inside quotes.

### Append

`cursor` only. This example is a new project directory `orders/` next to
`shop/` (stop the shop server first, or give this one `-p 8081`), over the
Northwind sample database:

```bash
cd ..
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

```bash
flapi -c flapi.yaml --config-service      # first terminal
curl -s "localhost:8080/orders?limit=3"   # second terminal
flapii cache refresh /orders              # "mode": "append", "rows_affected": 0 (no new orders)
```

**Late rows.** A row that arrives with a cursor value at or below the watermark
is skipped by a strict `>`, permanently, and the refresh still reports success.
If your source has late rows, re-read an overlap window
(`>= ... - INTERVAL 10 MINUTE`) and exclude rows already cached with a
`NOT EXISTS` anti-join (not `NOT IN`, which drops everything once an id is
NULL), or use merge, where the overlap is harmless. For a timestamp cursor,
write UTC everywhere or use `cursor.type: timestamptz`: a writer in local time
moves `max(cursor)` ahead and later UTC rows are skipped. SQL and a reconcile
query that finds lost rows:
[late rows in append mode](../../plugins/flapi/skills/flapi-cache-design/references/delete-patterns.md#late-rows-in-append-mode).

### Merge

`cursor` and `primary-key`. This example is a new project directory `crm/`
that reads a CSV extract with a soft-delete flag:

```bash
cd ..
mkdir -p crm/sqls crm/src && cd crm
```

`crm/flapi.yaml`:

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

Start it and watch a merge: change the extract (Ada deleted, Bob upgraded, Dee
new), refresh, and read again:

```bash
flapi -c flapi.yaml --config-service      # first terminal
curl -s localhost:8080/customers          # Ada, Bob
cat > src/customers.csv <<'EOF'
id,name,tier,updated_at,is_deleted
1,Ada,gold,2026-01-01 13:00:00,true
2,Bob,gold,2026-01-01 13:00:00,false
3,Cy,bronze,2026-01-01 12:00:00,true
4,Dee,silver,2026-01-01 14:00:00,false
EOF
flapii cache refresh /customers           # "mode": "merge", "full": false
curl -s localhost:8080/customers          # Bob (gold), Dee
```

- DuckDB's form is `INSERT *` (or `INSERT BY NAME`), not `INSERT (*) VALUES (s.*)`.
- **`>=` is right for merge**: a re-read boundary row collapses on the key, and
  an overlap window for late rows (`- INTERVAL 1 HOUR`) is idempotent. In append
  mode `>=` duplicates.
- **`rows_affected` counts no-op updates** (every re-read row is "updated"). To
  count only real changes, guard the action:
  `WHEN MATCHED AND (t.name IS DISTINCT FROM s.name OR …) THEN UPDATE SET …`.
- **One `MERGE`, one `UPDATE`/`DELETE` action.** DuckLake rejects a `MERGE` with
  both (`MERGE INTO with DuckLake only supports a single UPDATE/DELETE action
  currently`). See the delete patterns below.

### Deletes in merge mode

flAPI does not detect deleted source rows: an incremental refresh only sees
rows the source still returns. Model deletes in the data, usually as a
soft-delete flag (or a `deleted_at` column) that bumps the cursor when it is
set. Then pick one of two patterns.

**Keep tombstones in the cache** (the example above). The deleted row is
updated to `is_deleted = true`, stays in the table, and the serving template
filters it. The tombstone keeps its `updated_at`, so the watermark moves past
the delete. The cost is storing deleted rows.

**Remove them: a `DELETE`, then a single-action `MERGE`.** Keep the `MERGE`
last. A second endpoint in `crm/`, over the same extract, in its own table:

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

```bash
curl -s localhost:8080/customers-live
flapii cache refresh /customers-live      # after editing the extract as above: Ada leaves the table
```

Deleted rows leave the cache, so they no longer count towards the watermark:
when the newest change was a delete, the next refresh re-reads it. That is
harmless (the `DELETE` is idempotent).

If the source hard-deletes rows and keeps no trace, an incremental cache cannot
see the delete: use a full refresh for that table, a scheduled
`flapii cache refresh <path> --full`, or a reconcile `DELETE … WHERE NOT EXISTS`
([delete patterns](../../plugins/flapi/skills/flapi-cache-design/references/delete-patterns.md)).

### Variables in a populate template

| Variable | Contains |
|---|---|
| `{{cache.catalog}}` `{{cache.schema}}` `{{cache.table}}` | Where to write (the serving template gets these three too) |
| `{{cache.previousSnapshotTimestamp}}` | The incremental watermark, see above. Without a `cursor:` (full mode) it is the commit time of the table's last refresh |
| `{{cache.previousSnapshotId}}` | The id of that snapshot. Absent for a table no snapshot has touched yet, and after `--full` |
| `{{cache.snapshotTimestamp}}` `{{cache.snapshotId}}` | Commit time (UTC) and id of the newest snapshot that wrote this table **before** this refresh. Never this refresh's own snapshot. **Not a watermark** |
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

This example is a new project directory `items/`:

```bash
cd ..
mkdir -p items/sqls && cd items
```

Its source is a table the connection creates in flAPI's own DuckDB. With no
`duckdb.db_path` that DuckDB is **in memory**: the items are lost on restart,
and the next warm-up rebuilds the cache from the empty table. `items/flapi.yaml`:

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

A `DELETE` endpoint works the same way; this one refreshes after the response:

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

Try it:

```bash
flapi -c flapi.yaml                       # first terminal
curl -s localhost:8080/items              # {"data":[],...}
curl -s -X POST -H 'Content-Type: application/json' -d '{"name":"widget"}' localhost:8080/items
# {"rows_affected":1}   (status 201)
curl -s localhost:8080/items              # widget is there: the refresh ran before the 201
curl -s -X DELETE localhost:8080/items/1  # {"rows_affected":1}; the refresh follows the response
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
`AT (TIMESTAMP => ...)`, right after the table name. Back in `shop/`, add an
endpoint over a small CSV of sales targets that you will edit. The extract,
`src/targets.csv`:

<!-- flapi: file group=quick file=src/targets.csv -->
```text
segment,target
AUTOMOBILE,300
BUILDING,350
```

<!-- flapi: endpoint group=quick file=targets.yaml -->
```yaml
# sqls/targets.yaml
url-path: /targets
template-source: targets.sql
connection: [targets]
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
  table: targets_cache
  schedule: 1h
  template-file: targets_cache.sql
  retention:
    keep-last-snapshots: 5
```
<!-- flapi: check GET /targets -> 200 contains "BUILDING" -->
<!-- flapi: check GET /targets?version=999999 -> 404 contains "snapshot_not_found" -->
<!-- flapi: check GET /targets?at=2000-01-01%2000:00:00 -> 404 contains "snapshot_not_found" -->
<!-- flapi: check GET /targets?at=2999-01-01%2000:00:00 -> 400 contains "snapshot_in_future" -->
<!-- flapi: check GET /targets?at=yesterday -> 400 -->

<!-- flapi: template group=quick file=targets_cache.sql -->
```sql
-- sqls/targets_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT segment, target FROM read_csv('{{{ conn.path }}}');
```

<!-- flapi: template group=quick file=targets.sql -->
```sql
-- sqls/targets.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
{{#params.version}}AT (VERSION => {{ params.version }}){{/params.version}}
{{#params.at}}AT (TIMESTAMP => TIMESTAMP '{{{ params.at }}}'){{/params.at}}
ORDER BY segment
```

The running shop server does not know the new files. Restart it (warm-up builds
the new cache), or load them into the running server and build the cache, which
a reload does not do:

```bash
flapii endpoints reload --file sqls/targets.yaml
flapii cache refresh /targets
```

Now change the data between two snapshots and read both:

```bash
curl -s localhost:8080/targets            # BUILDING 350
sed -i.bak 's/BUILDING,350/BUILDING,400/' src/targets.csv
flapii cache refresh /targets
curl -s localhost:8080/targets            # BUILDING 400
flapii cache audit /targets -o table      # the Snapshot column: e.g. 6 (new) and 4 (old)
curl -s "localhost:8080/targets?version=4"                       # BUILDING 350 again
curl -s "localhost:8080/targets?at=2026-11-25%2013:16:11.8"      # as of that UTC time
curl -s "localhost:8080/targets?version=999999"                  # 404 snapshot_not_found
curl -s "localhost:8080/targets?at=2999-01-01%2000:00:00"        # 400 snapshot_in_future
```

- Take the version from **your** audit log; the ids above are an example.
- A naive timestamp is UTC, like the audit log's times. Interpolate it **inside
  a section**, guarded by a strict regex as above: a bound parameter does not
  work there, because DuckLake resolves `AT` before parameters are bound.
- An unknown or expired snapshot answers `404 snapshot_not_found`, a timestamp
  in the future `400 snapshot_in_future`, a value the regex rejects `400`.
- The audit log also keeps ids of snapshots retention has expired. List the
  live ones with an uncached endpoint over `ducklake_snapshots('<alias>')`:

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

```bash
curl -s localhost:8080/snapshots
# {"snapshot_id":6,"snapshot_time":"2026-11-25T13:16:11.975179Z","changes":{"tables_created":["\"cache\".targets_cache"],...}}
```

The list is catalog-wide (every cache table and the audit table). Protect this
endpoint like the data itself.

### Retention and gc

Without a policy, snapshots and their files accumulate. Set one per cache
(`cache.retention`, as above) or once for every cache without its own block:

```yaml
ducklake:
  retention:
    keep-last-snapshots: 10
    max-snapshot-age: 14d
```

An endpoint's `retention:` block replaces the global one as a whole. Retention
runs **after every refresh**: it expires the table's old snapshots, then
deletes the data files no kept snapshot of any table can still see. After a
full rebuild the old copy stays on disk until the last snapshot before it
leaves the window. To apply it now:

```bash
flapii cache gc /targets     # {status, expired_snapshots, files_deleted, bytes_freed}
```

`"status": "no_policy"` means the cache has no retention. When another table's
snapshots still pin the files, the answer says which (details:
[CONFIG_REFERENCE §2.5](../CONFIG_REFERENCE.md#25-ducklake-configuration)).

---

## 6. Measure and tune refresh time
<a id="measuring-and-tuning-refresh-time"></a>

Every refresh is recorded in the audit log, with how long the populate
statement took and how many rows it wrote. In the `crm/` project:

```bash
flapii cache audit /customers          # sync_type, status, duration_ms, rows_affected, snapshot_id
flapii metrics --endpoint /customers   # last/avg/max refresh duration, data_age_seconds, state
```

`rows_affected` is the **sum over every DML statement** in the template: a
template that deletes 1 row and then merges 2 reports `3`. `CREATE TABLE … AS`
counts the rows it creates.

What usually makes a refresh shorter, biggest win first:

1. **Go incremental.** If only a fraction of rows changes between refreshes, a
   cursor turns "re-read everything" into "read the delta".
2. **Push the watermark to the source.** The cursor predicate must reach the
   source (Postgres, BigQuery, Parquet min/max statistics). Check with `EXPLAIN`
   on the rendered SQL from `flapii templates expand /customers --cache`. A
   function wrapped around the cursor column usually stops the pushdown.
3. **Select only the columns you serve.**
4. **Fit the schedule to the duration.** Overlapping refreshes of the same table
   are skipped, not queued.
5. **Keep small deltas cheap.** `ducklake.data-inlining-row-limit` (a few
   thousand rows) keeps small inserts in the catalog, and `ducklake.compaction`
   (`enabled: true` plus a `schedule` such as `@daily`) merges small files;
   `flapii cache compact` runs it now.
6. **Watch merge-mode creep.** Every update or delete writes a delete file that
   readers apply, so merge refreshes slow down over time. Compact (set
   `ducklake.compaction.delete-threshold`, see
   [CONFIG_REFERENCE §2.5](../CONFIG_REFERENCE.md#25-ducklake-configuration)) or
   rebuild now and then with `--full`.
7. **Bound snapshot growth** with retention ([section 5](#retention-and-gc)).

The `flapi-cache-tuning` skill in the [Claude Code plugin](./claude-code.md)
walks through this loop with before/after numbers.

---

## 7. Run it in production

### Topology and cost

| Topology | Warehouse load | Notes |
|---|---|---|
| **One instance** | one populate per schedule | simplest; a restart is a gap |
| **One cache per instance** (N replicas, each its own catalog) | N times the populates; N schedulers | no coordination, N times the bill |
| **One writer, read-only replicas** (`ducklake.read-only: true`) | one populate per schedule | needs a shared catalog (Postgres/SQLite/MySQL, or a DuckDB file built offline): [Serverless](./serverless.md) |

A rough sizing:

- **Warehouse queries per day** ≈ (instances that refresh) × 24 h / `schedule`
  per cache, plus restarts × caches when `warmup` is `always`.
- **Disk** ≈ table size × retained snapshots for a full refresh (an
  incremental refresh adds only its delta).
- **Memory and CPU are shared with serving.** Refreshes and requests run in the
  same DuckDB, bounded by one `duckdb.max_memory` and `threads`.

### Monitoring

A cache whose refreshes fail keeps serving its last snapshot and `/health`
still answers **`200`** (`"status":"degraded"` with a `stale` list), so a load
balancer will not notice. Alert on:

- `/health` with a `stale` or `failed` list, or `status` other than `ready`;
- `data_age_seconds` above about twice the `schedule`, per cache, from
  `flapii metrics` (`GET /api/v1/_config/metrics`, the `caches` list, with the
  config-service token);
- audit rows with `status: error` (`flapii cache audit <path>`), and the
  `failures` count in the metrics;
- for append caches, a scheduled reconcile query that counts source rows the
  cache is missing ([late rows](../../plugins/flapi/skills/flapi-cache-design/references/delete-patterns.md#detecting-lost-rows)).

### Failure semantics

- **A refresh is one transaction.** If it fails, or the process dies mid-way,
  the catalog stays at the previous snapshot and that is what is served after
  the restart (subject to `warmup`). Parquet files an interrupted refresh had
  already written are not in the catalog; whether retention's cleanup ever
  removes them is not verified, so check `data-path` after a crash.
- **A DuckDB-file catalog has one writer.** Only one process can open it:
  a second server or a one-shot `flapi cache refresh` fails while one holds it.
  Use a Postgres/SQLite/MySQL catalog to share it live.
- **Refresh and serving.** Requests during a refresh read the previous snapshot
  and are not blocked. Refreshes in one catalog run one at a time. A populate
  competes with requests for the shared memory and threads; over a SQLite
  source it also waits for writes to that source, and they for it.

---

## Operating it

The commands, shown for `/segments` in `shop/`:

```bash
flapii cache get /segments        # current configuration and state
flapii cache list                 # all caches
flapii cache refresh /segments    # refresh now
flapii cache refresh /segments --full   # drop and rebuild from scratch
flapii cache audit /segments -o table   # refresh history: mode, status, duration, rows, snapshot
flapii cache gc /segments         # apply retention (path is required)
flapii cache drop-table old.segments_cache   # drop a table no endpoint uses any more
```

After changing `cache.schema` or `cache.table`, the old table stays in the
catalog: drop it with `flapii cache drop-table <old-schema>.<table>` (`409
cache_table_in_use` while an endpoint still names it).

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
