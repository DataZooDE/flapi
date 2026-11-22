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
| Small (< ~500 MB), changes rarely | Per-instance cache with `ducklake.warmup: if-empty` on a persistent `ducklake.metadata-path` and `data-path`, or plain `always` if a load takes seconds | one load per instance | local DuckLake table |
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
  metadata-path: postgres:dbname=flapi host=10.0.0.5 user=flapi   # password from PGPASSWORD
  data-path: gs://my-api-data/cache/
  read-only: true          # no warm-up, no refresh/retention/compaction, 409 on mutations
```

- Replicas need the writer's `connections:` **names** (the properties can be
  placeholders: a replica never runs a populate template) and the same endpoint
  YAMLs. A replica logs `Cache scheduler off` by itself; it needs no
  `ducklake.scheduler.enabled: false`.
- Put the Postgres password in `PGPASSWORD`, not in the DSN. (A `password=` in
  `metadata-path` is shown as `<redacted>` in logs and config API answers.)
- With a Postgres catalog the writer can run while replicas serve: each commit is
  visible on a replica's next read, and a table first built after a replica started
  is served within about 5 s (verified with a writer refreshing every second, no
  failed reads).

A Postgres catalog costs network round trips on every query, so put it in
the same region on a machine that is not shared-core, and measure p50 before
choosing this over § 2.

**A DuckDB-file catalog is shared by replicas only.** Several read-only replicas
can open the same file, but a writer cannot open it while any replica holds it:
the one-shot fails (exit `1`) with `{"error":"a running flapi holds the DuckLake
catalog lock; ..."}`. A replica cannot start while a writer server holds it
either. So the writer runs only between deploys or with the fleet scaled to
zero, and the replicas start after it finishes. For a writer that runs while
replicas serve, use a Postgres catalog.

### Prebuilding the catalog in CI

```bash
flapi cache refresh -c flapi.yaml --all     # writes ./state/cache.ducklake + ./state/cache_data/
```

- **The catalog stores its data path as an absolute path.** Copied anywhere else,
  startup fails with `DATA_PATH parameter ... does not match existing data path in
  the catalog`. Build at the absolute path the server uses, or set
  `ducklake.override-data-path: true`.
- Relative `metadata-path` / `data-path` resolve against the config directory (in
  the image below: `/app/config`), but against the working directory for a packed
  binary.
- `embed://data/x` works unpacked too (it resolves against the config directory),
  and a packed binary's `./api cache refresh --all` uses its bundle, so one config
  prebuilds before and after `flapi pack`. `pack` never bundles the DuckLake catalog
  or its data path.
- **A shipped catalog older than its `schedule` is refreshed at the first scheduler
  scan after every cold start.** On scale-to-zero set
  `ducklake.scheduler.enabled: false` (the job refreshes) or a schedule longer than
  the catalog's age, together with `ducklake.warmup: if-empty` or `never`.

## 4. Fast, offline startup

Every release carries the default extensions that are not built into flapi
(`httpfs`, `ducklake`, `fts`, `postgres`, `sqlite`; `core_functions`, `parquet`,
`json` and `icu` are built in) in `extensions/` next to the binary: the release
archives and the Docker images (`/app/extensions`). flapi
finds that directory on its own, so a fresh container starts without downloading
anything and with no network at all. The `-runtime` image adds `bigquery` when the
community extension is published for its DuckDB version.

```dockerfile
FROM ghcr.io/datazoode/flapi:<version>
COPY flapi.yaml /app/config/flapi.yaml
COPY sqls/ /app/config/sqls/
COPY data/ /app/config/data/
# a catalog prebuilt in CI (flapi.yaml: metadata-path ./state/cache.ducklake,
# data-path ./state/cache_data, override-data-path: true, warmup: if-empty)
COPY state/ /app/config/state/
ENV FLAPI_CONFIG=/app/config/flapi.yaml FLAPI_EXTENSIONS_OFFLINE=1
```

- Keep the binary and its `extensions/` together when you copy a release
  around; a symlink to the binary is fine. An `extensions/` for another DuckDB
  version is ignored with one warning.
- On macOS, a release archive downloaded with a browser is quarantined, and so
  is everything unpacked from it (binary and extensions): run
  `xattr -dr com.apple.quarantine <unpacked dir>` once before starting it.
- `FLAPI_EXTENSION_DIRECTORY` (or `duckdb.extension_directory`) replaces the
  shipped directory; anything you install yourself goes to `~/.duckdb/extensions`,
  which is searched first. See
  [CONFIG_REFERENCE: where DuckDB extensions come from](../CONFIG_REFERENCE.md#24-duckdb-configuration).
- The `flapi-io` wheels do not carry them (a wheel would be ~155 MB, above
  PyPI's 100 MB file limit): `pip install flapi-io` downloads on first start.

Not using containers? `flapi pack --in . --out api --with-extensions` produces
one binary that carries its config **and** its extensions and starts offline
([CLI_REFERENCE § 3](../CLI_REFERENCE.md#flapi-pack----create-a-self-contained-binary)).

`FLAPI_EXTENSIONS_OFFLINE=1` turns a missing extension into a clear startup
error instead of a download. If you need other extensions, build your own
layer with `flapi extensions install --config flapi.yaml` (see
[CLI_REFERENCE § 3](../CLI_REFERENCE.md#flapi-extensions-install----pre-install-duckdb-extensions)).
Trim `duckdb.default-extensions` to what your endpoints use.

## 5. Probes and CI

- **Startup and liveness probes:** `/health/live`, which answers `200` as soon
  as the port listens. Do **not** point liveness at `/health`: it answers `503`
  with `status: starting` while caches warm, so a large warm-up turns into a
  restart loop.
- **Readiness (traffic) probe:** `/health`. It is `503` while caches are
  starting, when a cache's first build failed (no table to serve), and when a request has
  been in flight longer than `stall-timeout-s`; it is `200` (`ready`, or
  `degraded` for a stale or unbuilt cache) otherwise. See
  [CLI_REFERENCE § 6](../CLI_REFERENCE.md#6-runtime-health-endpoints).
- **CI:** `flapi --validate-config` never opens the database. To smoke-test
  the real server without touching data, start it with `--no-cache-warmup`.
  `/health` then answers `200` with `status: degraded` and lists the unbuilt
  caches. After `terraform apply`, check the live revision.

## How it works

- [CONFIG_REFERENCE § 2.4–2.5](../CONFIG_REFERENCE.md#24-duckdb-configuration): `default-extensions`, `warmup`, `read-only`
- [spec/components/caching.md](../spec/components/caching.md): readiness states and warm-up modes
- [spec/DESIGN_DECISIONS.md § 11](../spec/DESIGN_DECISIONS.md): why the cache does not own serverless deployments
