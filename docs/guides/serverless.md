# Serving batch data on Cloud Run (and other scale-to-zero platforms)

**Goal:** run flAPI on a platform that starts and stops containers on demand
(Cloud Run, App Runner, Container Apps, Knative) without loading data at every
start.

flAPI's [cache](./caching.md) is built for a long-lived server: each instance
fills its own cache when it boots and refreshes it on a schedule. On a
scale-to-zero platform that repeats the load for every new instance, every
restart and every CI run, and puts the whole table on the critical path of
startup. When the data changes in batches (a nightly `dbt build`), produce it
**once** where it changes and let every instance read the result.

---

## 1. Pick a pattern

| Your data | Pattern | Startup cost | Query cost |
|---|---|---|---|
| Small (< ~500 MB), changes rarely | Per-instance cache with `ducklake.warmup: if-empty` on a persistent `db_path`, or plain `always` if a load takes seconds | one load per instance | in-memory |
| Batch-exported Parquet (any size) | **Read in place**: no `cache:`; the template reads the files (§ 2) | none | file scan with row-group pruning |
| Needs snapshots, incremental merge or time travel | **Shared DuckLake**: one writer job (`flapi cache refresh`), read-only replicas (§ 3) | catalog attach only | catalog look-ups + file scan |

## 2. Read Parquet in place

Export the table once per run (e.g. a dbt post-hook running BigQuery
`EXPORT DATA`, sorted by the main filter key) into a **versioned** prefix and
write a marker when it is complete:

```text
gs://my-api-data/orders/v=20261031T0530/part-*.parquet
gs://my-api-data/orders/v=20261031T0530/_SUCCESS
```

Mount the bucket read-only (Cloud Run: a Cloud Storage volume at `/mnt/data`)
and point the endpoint at it. No `cache:` block:

```yaml
# sqls/orders.yaml
url-path: /orders
template-source: orders.sql
connection: [lake]
```

```sql
-- sqls/orders.sql: the version is pinned per revision
SELECT * FROM read_parquet('/mnt/data/orders/v={{{ env.ORDERS_VERSION }}}/*.parquet')
WHERE sold_to = {{ params.sold_to }}
```

`ORDERS_VERSION` must be listed in `template.environment-whitelist`.
Deploying one revision per data version makes the switch atomic, and with a
pre-built image a deploy takes about a minute.

Do **not** overwrite files in place, and do not mount with an infinite
metadata cache (`metadata-cache-ttl-secs=-1`) if anything is ever replaced.
A long-lived instance keeps the old file listing and its queries fail once
those files are deleted. During an overwrite, readers see a mix of old and
new files. Keep old versions for a few days with a bucket lifecycle rule.

Check that sorting pays off before you rely on it:

```sql
SELECT row_group_id, stats_min, stats_max
FROM parquet_metadata('/mnt/data/orders/v=20261031T0530/*.parquet')
WHERE path_in_schema = 'sold_to';
```

## 3. Shared DuckLake: one writer, many readers

The writer is a job, not a server. Run it after the upstream build:

```bash
flapi cache refresh --config flapi.yaml --all     # exit 0 / 1 / 2, JSON summary on stdout
```

Replicas attach the same catalog read-only:

```yaml
ducklake:
  enabled: true
  alias: cache
  metadata-path: postgres:dbname=flapi host=10.0.0.5   # or a DuckDB-file catalog
  data-path: gs://my-api-data/cache/
  read-only: true          # no warm-up, no refresh/retention/compaction, 409 on mutations
```

A Postgres catalog costs network round trips on every query, so put it in
the same region on a machine that is not shared-core, and measure p50 before
choosing this over § 2.

## 4. Fast, offline startup

Use the `-runtime` image. It carries `core_functions`, every default
extension (`httpfs`, `ducklake`, `fts`, `json`, `postgres`, `sqlite`,
`parquet`) and `bigquery`,
and sets `FLAPI_EXTENSION_DIRECTORY`:

```dockerfile
FROM ghcr.io/datazoode/flapi:<version>-runtime
COPY flapi.yaml /app/config/flapi.yaml
COPY sqls/ /app/config/sqls/
ENV FLAPI_CONFIG=/app/config/flapi.yaml FLAPI_EXTENSIONS_OFFLINE=1
```

Not using containers? `flapi pack --in . --out api --with-extensions` produces
one binary that carries its config **and** its extensions and starts offline
([CLI_REFERENCE § 3](../CLI_REFERENCE.md#flapi-pack----create-a-self-contained-binary)).

`FLAPI_EXTENSIONS_OFFLINE=1` turns a missing extension into a clear startup
error instead of a download. If you need other extensions, build your own
layer with `flapi extensions install --config flapi.yaml` (see
[CLI_REFERENCE § 3](../CLI_REFERENCE.md#flapi-extensions-install----pre-install-duckdb-extensions)).
Trim `duckdb.default-extensions` to what your endpoints use.

## 5. Probes and CI

- **Startup probe:** `/health/live`, which answers as soon as the port listens.
- **Liveness probe:** `/health`, which is `503` only for a stalled request
  or a cache that failed to build.
- **CI:** `flapi --validate-config` never opens the database. To smoke-test
  the real server without touching data, start it with `--no-cache-warmup`.
  `/health` then answers `200` with `status: degraded` and lists the unbuilt
  caches. After `terraform apply`, check the live revision.

## How it works

- [CONFIG_REFERENCE § 2.4–2.5](../CONFIG_REFERENCE.md#24-duckdb-configuration): `default-extensions`, `warmup`, `read-only`
- [spec/components/caching.md](../spec/components/caching.md): readiness states and warm-up modes
- [spec/DESIGN_DECISIONS.md § 11](../spec/DESIGN_DECISIONS.md): why the cache does not own serverless deployments
