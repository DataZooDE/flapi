# Where the cache is built: per instance, CI, or one writer

| Pattern | Catalog | Writer | Readers |
|---|---|---|---|
| Per-instance cache (default) | DuckDB file next to the server | the server itself (warm-up + schedule) | the same server |
| Prebuilt in CI, baked into the image | DuckDB file in the image | `flapi cache refresh` in the build | servers with `warmup: if-empty` or `never` |
| Shared catalog, live fleet | Postgres (or another multi-client DuckLake catalog), data on object storage | one job or one server | replicas with `read-only: true` |

## One-shot refresh (no server)

```bash
flapi cache refresh -c flapi.yaml --all
flapi cache refresh -c flapi.yaml --endpoint /orders --endpoint /customers
flapi cache refresh -c flapi.yaml --endpoint /orders --full   # drop and rebuild
```

- Exit `0`: every refresh ok. `1`: a refresh failed, or the catalog could not be
  attached (e.g. it is locked). `2`: usage (no `--all`/`--endpoint`, unknown
  endpoint, endpoint without cache, ducklake disabled or read-only).
- stdout is a JSON summary:
  `{"failed":0,"refreshed":[{"endpoint":"/orders","schema":"cache","table":"orders_cache","status":"ok","rows_affected":830,"duration_ms":412}]}`;
  `"full": true` with `--full`; a failure has `"status":"failed"` and `"error"`.
  Endpoints sharing a table are refreshed once.
- It runs the same populate SQL as the server: incremental templates stay
  incremental, an audit row is written, and a due `ducklake.compaction` runs.
- `time flapi cache refresh ...` is the simplest end-to-end timing.

## The DuckDB-file catalog lock

A DuckDB-file `metadata-path` is opened by one writing process at a time. While a
server has it attached, `flapi cache refresh` on the same file exits 1 with a
JSON `error` that says a running server holds the DuckLake catalog lock and to
refresh through it (`flapii cache refresh <path>`),
and a writer cannot attach while read-only replicas hold it either. So with a file catalog:

- refresh through the running server (`flapii cache refresh <path>`, the
  schedule), or
- stop the server, run the one-shot, start it again, or
- build the catalog in CI and ship it.

## Prebuilt cache in CI / for serverless

```bash
flapi -c flapi.yaml --validate-config
flapi cache refresh -c flapi.yaml --all        # build cache.ducklake + cache_data/
# package cache.ducklake and cache_data/ with the config (image layer, volume, bucket)
```

At runtime set `ducklake.warmup: if-empty` (start serving what is there, build only
missing tables) or `never` (never build at startup; a missing table answers `503
cache_not_built` and `/health` says `degraded` with an `unbuilt` list). On a
scale-to-zero platform `always` repeats the full load on every cold start. See
`flapi-deploy` for images and `flapi pack`.

## Read-only replicas on a shared catalog

```yaml
ducklake:
  enabled: true
  alias: cache
  metadata-path: postgres:dbname=flapi host=10.0.0.5
  data-path: s3://my-bucket/flapi-cache/
  read-only: true
```

- The catalog is attached `READ_ONLY`. Implies `warmup: never`; no scheduled
  refresh, retention or compaction; the audit table is not created.
- `POST .../cache/refresh`, `PUT .../cache`, `PUT .../cache/template`,
  `POST .../cache/gc` and `POST /api/v1/_config/cache/drop-table` answer
  `409 {"error":"read_only_cache"}` with advice for that operation; so do the
  matching `flapii cache` commands and MCP config tools.
- A replica started before the writer built a table answers 503
  `cache_not_built` only until the writer has built it. It checks again at most
  every 5 s on requests, and on every scheduler scan. `GET /api/v1/_config/project`
  shows `ducklake.warmup_effective: never`.
- Every replica's endpoint config must name the same `table` / `schema` and the
  same `alias` as the writer.
- The writer is `flapi cache refresh` (a job after the upstream build) or one
  ordinary server. Credentials for Postgres and object storage come from the
  environment or a `CREATE SECRET` in a connection `init` (`flapi-setup`).

Verified with a DuckDB-file catalog (replica serves, refresh answers 409). A
Postgres catalog with a live writer was not exercised here: check that replicas
pick up the writer's new snapshots in your setup before relying on it.
