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
| `{{cache.previousSnapshotTimestamp}}` `{{cache.previousSnapshotId}}` | Last refresh — the incremental watermark |
| `{{cache.cursorColumn}}` `{{cache.cursorType}}` | Your declared cursor |
| `{{cache.primaryKeys}}` | Your declared key |
| `{{params.cacheMode}}` | `full`, `append` or `merge` |

Append:

```sql
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT * FROM source_events
WHERE {{#cache.previousSnapshotTimestamp}} event_time > TIMESTAMP '{{cache.previousSnapshotTimestamp}}' {{/cache.previousSnapshotTimestamp}}
```

Wrapping the predicate in `{{#cache.previousSnapshotTimestamp}}` matters: on the
very first run there is no previous snapshot, and the section renders empty so
the first load takes everything.

Merge:

```sql
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT * FROM source_customers
  WHERE {{#cache.previousSnapshotTimestamp}} updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}' {{/cache.previousSnapshotTimestamp}}
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET
  name = s.name, email = s.email, updated_at = s.updated_at
WHEN NOT MATCHED THEN INSERT (*) VALUES (s.*);
```

## When it refreshes

- **At startup** — caches are warmed for endpoints that enable them.
- **On `cache.schedule`** — per endpoint.
- **On demand** — `flapii cache refresh /endpoint`.

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
