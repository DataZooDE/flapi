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

A DuckDB-file `metadata-path` takes a file lock. Verified on v26.10.31:

| Already running | Then | Result |
|---|---|---|
| a writer server | `flapi cache refresh` (one-shot) | exit 1, `error`: the catalog lock is held by a running server; refresh through it (`flapii cache refresh <path>`) |
| a writer server | a replica server (`read-only: true`) | does not start: `another process holds the lock on the DuckDB-file catalog` |
| one or more replicas | another replica | starts; replicas share the file |
| one or more replicas | a writer (one-shot or server) | refused (exit 1); stop the replicas first |

So with a file catalog:

- refresh through the running server (`flapii cache refresh <path>`, the
  schedule), or
- stop the server (or every replica), run the one-shot, start them again, or
- build the catalog in CI and ship it, or
- move the catalog to Postgres (or MySQL / SQLite), which a live writer and
  replicas share.

## Prebuilt cache in CI / for serverless

```bash
flapi -c flapi.yaml --validate-config
flapi cache refresh -c flapi.yaml --all        # build ./state/cache.ducklake + ./state/cache_data/
# ship state/ with the config (image layer, volume, bucket)
```

- **The catalog stores an ABSOLUTE `data_path`.** Opened from anywhere else, startup
  fails with `DATA_PATH parameter "<new>" does not match existing data path in the
  catalog "<old>"`. Either build at the same absolute path the server will use, or
  set `ducklake.override-data-path: true` on the server (verified: a catalog built in
  one directory and copied to another serves after that).
- Relative `metadata-path` / `data-path` resolve against the **config directory**
  for an unpacked config (also for `flapi cache refresh -c dir/flapi.yaml` run from
  elsewhere), and against the **working directory** for a packed binary.
- `embed://data/x.parquet` resolves against the config directory when the config is
  not packed, and a packed binary's `./api cache refresh --all` reads its bundled
  config and data, so the same `embed://` config prebuilds before and after `flapi
  pack`. `pack` always leaves the configured catalog, its `.wal`, the data path and
  any `*.ducklake` out of the bundle; `--exclude GLOB` drops more.

At runtime set `ducklake.warmup: if-empty` (start serving what is there, build only
missing tables) or `never` (never build at startup; a missing table answers `503
cache_not_built` and `/health` says `degraded` with an `unbuilt` list). On a
scale-to-zero platform `always` repeats the full load on every cold start.

**Cold starts and the schedule.** The next refresh is due one `schedule` after the
newest snapshot. A shipped catalog older than its schedule is therefore refreshed
at the first scheduler scan after startup (verified: 5 s after start with
`schedule: 5s`), on every cold start. On scale-to-zero either set
`ducklake.scheduler.enabled: false` (an external job refreshes), or use a schedule
longer than the catalog's age at deploy time.

## Read-only replicas on a shared catalog

```yaml
ducklake:
  enabled: true
  alias: cache
  metadata-path: postgres:dbname=flapi host=10.0.0.5 user=flapi   # PGPASSWORD from the environment
  data-path: s3://my-bucket/flapi-cache/
  read-only: true
```

- The catalog is attached `READ_ONLY`. Implies `warmup: never`; no scheduled
  refresh (startup logs `Cache scheduler off: the DuckLake catalog is read-only`),
  retention or compaction; the audit table is not created. A replica needs no
  `scheduler.enabled: false`; that setting is for a **writer server** that an
  external job refreshes instead.
- `POST .../cache/refresh`, `PUT .../cache`, `PUT .../cache/template`,
  `POST .../cache/gc` and `POST /api/v1/_config/cache/drop-table` answer
  `409 {"error":"read_only_cache"}` with advice for that operation; so do the
  matching `flapii cache` commands and MCP config tools.
- A replica started before the writer built a table answers 503
  `cache_not_built` only until the writer has built it. It checks again at most
  every 5 s on requests, and on every scheduler scan. `GET /api/v1/_config/project`
  shows `ducklake.warmup_effective: never`.
- Replicas need the **same `connections:` names** as the writer (an endpoint naming
  an unknown connection fails validation: `Connection 'items' not found`), though
  the properties can be placeholders since a replica never runs a populate
  template, and the same endpoint YAMLs (`table`, `schema`) and `alias`.
- Postgres catalog: `metadata-path: postgres:dbname=... host=... user=...` with the
  password in `PGPASSWORD` (libpq reads it). A `password=` in the DSN is redacted
  (`<redacted>`) in logs and config API answers, but keep it out of YAML anyway.
- The writer is `flapi cache refresh` (a job after the upstream build) or one
  ordinary server. Credentials for object storage come from the environment or a
  `CREATE SECRET` in a connection `init` (`flapi-setup`).

Verified with a Postgres 16 catalog and a live writer server refreshing every
second: a replica served each new snapshot on its next read (300 of 300 reads
200 during the refreshes), and a table the writer first built after the replica
started was served within a second. With a DuckDB-file catalog, two replicas
served side by side and the writer was refused, as in the table above.
