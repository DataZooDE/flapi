---
name: flapi-deploy
description: >
  Shipping a flAPI project to production: self-packaging into one binary (`flapi pack`,
  `info`, `unpack`, embed:// data, bundled extensions, reproducible builds), offline
  DuckDB extensions, the Docker images (slim and -runtime), Cloud Run and other
  scale-to-zero platforms, Kubernetes probes, read-only DuckLake replicas, the PyPI
  wheel, environment variables and a production checklist (config service, auth, JSON
  logs, telemetry opt-out). Use when someone packages, containerises, deploys or
  hardens a flAPI server.
version: 26.10.31
user-invocable: false
---

# flAPI Deploy Cheat Sheet

**flAPI:** v26.10.31 | **Skill:** Deploy | **See also:** `flapi-setup`, `flapi-auth`, `flapi-cache-design`, `flapi-cache-tuning`, `flapi-operate`

## Critical gotchas

- **In a packed binary, `./data/...` is NOT the bundle.** Relative paths (in `properties`,
  in `init`, and in `ducklake.metadata-path` / `data-path`) resolve against the **working
  directory** at runtime. To read a bundled file, write `embed://data/file.parquet`
  (`read_parquet`, `read_csv` and `read_json` work). A SQLite file cannot be served from the
  bundle (`ATTACH 'embed://...'` fails with `unable to open database file`).
- **`flapi pack` refuses, it does not skip.** Files matching `*.env`, `.env.*`, `secrets/*`,
  `*.pem` or `*.key` (case-insensitive), any symlink, and anything under `.flapi/` all abort
  the pack with exit 1. A password written **inline** in `flapi.yaml` is packed like any
  other text. Keep credentials in `{{env.X}}`.
- **`pack` validates the config first** and refuses an invalid one. `--skip-validation` builds
  a binary that fails on every host it is copied to.
- **`--with-extensions` embeds extensions for the host platform only.** For another
  platform, fill a directory there (`flapi extensions install --dir`) and pass
  `--extensions-dir`.
- **The slim image downloads extensions at startup.** Without network access it fails or
  warns. Use the `:v<version>-runtime` image, or set `FLAPI_EXTENSIONS_OFFLINE=1` with
  pre-installed extensions.
- **`/health` is readiness, not liveness.** It answers 503 while caches warm (`starting`),
  when a cache failed to build (`degraded` + `failed`), and when a request is stuck longer
  than `stall-timeout-s` (`stalled`). Liveness is `/health/live`, which is always 200 once
  the port listens. A liveness probe on `/health` restart-loops a slow warm-up.
- **The images have no `curl`, no `wget` and no `HEALTHCHECK`.** They run as root with
  `WORKDIR /app` and `ENTRYPOINT ["/app/flapi"]`. Pass `-c /config/flapi.yaml` or set
  `FLAPI_CONFIG`. For an in-container probe, use `bash` + `/dev/tcp` (`/bin/sh` is dash).
- **`--config-service` without a token generates one** and prints it in the log. In
  production, leave the config service off, or set `FLAPI_CONFIG_SERVICE_TOKEN` from a
  secret store.
- **Without a `cors:` block every origin gets `Access-Control-Allow-Origin: *`.** Set
  `cors.allow-origins` for browser clients.
- **Scheduled cache refresh needs `ducklake.scheduler.enabled: true`.** Without it,
  `cache.schedule` never fires on the server. See `flapi-cache-tuning`.

## Self-packaging (one binary, `scp` deploy)

```bash
flapi pack --in ./my-api --out api                   # validates, then appends a ZIP of the tree
./api info                                           # offset, size, entry list
./api unpack --to /tmp/api-extracted                 # dump the bundle for debugging
cd /srv && ./api -p 8080                             # serves the bundled flapi.yaml from any cwd
flapi pack --in ./my-api --out api --with-extensions # also embed the extensions it needs
SOURCE_DATE_EPOCH=1700000000 flapi pack --in ./my-api --out api   # bit-identical output
```

| Option | Meaning |
|---|---|
| `--in DIR` / `--out FILE` | required: the tree with `flapi.yaml` + `sqls/` (+ `data/`) and the output binary |
| `--with-extensions` | install the config's extensions (`core_functions`, `duckdb.default-extensions`, `INSTALL`s in `connections.*.init`) for this platform and embed them under `.flapi/extensions/` |
| `--extensions-dir DIR` | embed an already filled extension directory instead (another platform) |
| `--allow-secrets` | bundle secret-looking files anyway (testing only) |
| `--skip-validation` | pack an unvalidated config |
| `--macos-append` | macOS only: append after `__LINKEDIT` instead of writing the reserved `__FLAPI/__bundle` segment. The result is ad-hoc signed and **not notarisable**. |

When the packed binary starts, it extracts embedded extensions once to
`$TMPDIR/flapi-extensions/ext-<hash>/`, uses that as `extension_directory`, and runs offline
(it never downloads). `FLAPI_EXTENSIONS_OFFLINE=0` turns downloads back on. On macOS the
bundle goes into a reserved 16 MiB Mach-O segment, and the binary is re-signed so it stays
notarisable. A larger bundle is an error (the segment size is fixed at build time).

A bundle-ready config reads its data with `embed://`, and keeps writable state outside the
bundle:

```yaml
project-name: packed-api
project-description: Customers API shipped as one binary
template:
  path: ./sqls                                   # resolved inside the bundle
connections:
  customers:
    properties:
      path: embed://data/customers.parquet       # read from the bundle
ducklake:
  enabled: true
  alias: cache
  metadata-path: /var/lib/flapi/cache.ducklake   # absolute: relative = runtime cwd
  data-path: /var/lib/flapi/cache_data
```

`./api --validate-config` currently prints false `Template file does not exist:
sqls/sqls/<file>.sql` warnings for a bundle. The server itself finds the templates. Validate
the source tree (`flapi -c my-api/flapi.yaml --validate-config`) instead.

## Offline / pre-installed extensions

```bash
flapi extensions install --config flapi.yaml --dir /opt/flapi/extensions   # what the config needs
flapi extensions install --dir /opt/flapi/extensions httpfs parquet bigquery@community
FLAPI_EXTENSION_DIRECTORY=/opt/flapi/extensions FLAPI_EXTENSIONS_OFFLINE=1 flapi -c flapi.yaml
```

- `--config` installs `duckdb.default-extensions` + every `INSTALL` in `connections.*.init`.
  The default directory is `FLAPI_EXTENSION_DIRECTORY`, then DuckDB's own default.
- `FLAPI_EXTENSIONS_OFFLINE=1|true|yes` skips `INSTALL`, disables autoinstall, and turns a
  missing default extension into a startup error that names it.
- Trim `duckdb.default-extensions` to what the endpoints use (default: `httpfs, ducklake,
  fts, json, postgres, sqlite, parquet`).

## Docker

| Tag | Contents |
|---|---|
| `ghcr.io/datazoode/flapi:latest` | newest release, slim (binary only) |
| `:v26.10.31` | that release, slim |
| `:v26.10.31-runtime` | same binary with `core_functions httpfs ducklake fts json postgres sqlite parquet bigquery` pre-installed and `FLAPI_EXTENSION_DIRECTORY=/opt/flapi/extensions` set |
| `:main`, `:main-runtime`, `:sha-<commit>` | builds of `main` (not releases) |

There is no `:latest-runtime`. Pin a version.

```bash
docker run -d --name api -p 8080:8080 \
  -v "$PWD":/config:ro \
  -e FLAPI_CONFIG_SERVICE_TOKEN -e PG_PASSWORD \
  ghcr.io/datazoode/flapi:v26.10.31-runtime -c /config/flapi.yaml
docker exec api bash -c 'exec 3<>/dev/tcp/127.0.0.1/8080 && printf "GET /health/live HTTP/1.0\r\n\r\n" >&3 && cat <&3'
```

A relative property path (`./data/x.parquet`) resolves against the mounted config directory.
Mount read-only, and put a DuckLake catalog or `duckdb.db_path` on a writable volume with
absolute paths.

Bake the config into an image:

```dockerfile
FROM ghcr.io/datazoode/flapi:v26.10.31-runtime
COPY flapi.yaml /app/config/flapi.yaml
COPY sqls/ /app/config/sqls/
ENV FLAPI_CONFIG=/app/config/flapi.yaml FLAPI_EXTENSIONS_OFFLINE=1
HEALTHCHECK CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/8080 && printf 'GET /health/live HTTP/1.0\\r\\n\\r\\n' >&3 && grep -q live <&3"]
```

Need an extension that is not in the runtime image (mysql, azure, snowflake)? Add
`RUN /app/flapi extensions install --dir /opt/flapi/extensions mysql`.

## Kubernetes probes

Kubernetes YAML (shown as text, it is not flAPI config):

```text
livenessProbe:
  httpGet: { path: /health/live, port: 8080 }
  periodSeconds: 10
readinessProbe:
  httpGet: { path: /health, port: 8080 }    # 503 while warming / failed cache / stalled request
  periodSeconds: 5
startupProbe:
  httpGet: { path: /health/live, port: 8080 }
  failureThreshold: 30
```

`/health` body: `status` is `ready`, `degraded` (200 for stale or unbuilt caches; 503 for a
failed one), `starting` (503) or `stalled` (503), plus `caches{total,ready,failed,unbuilt}`
and `requests{in_flight,oldest_ms}`. `/health` and `/health/live` need no token. Failed
cache errors stay in the server log, not in the body.

## Serverless (Cloud Run, App Runner, Container Apps, Knative)

Avoid loading data on the boot path. Pick one pattern:

| Data | Pattern |
|---|---|
| Small, changes rarely | per-instance cache, `ducklake.warmup: if-empty` on a persistent path, or `always` if a load takes seconds |
| Batch-exported Parquet | no `cache:`; read the files in place from a mounted, **versioned** prefix (never overwrite files in place) |
| Needs snapshots / merge | shared DuckLake catalog: one writer job, read-only replicas |

Writer job (run after the upstream build). It prints a JSON summary and exits 0 when all
refreshes succeed, 1 when one failed, and 2 on a usage error such as an unknown endpoint:

```bash
flapi cache refresh -c flapi.yaml --all
flapi cache refresh -c flapi.yaml --endpoint /orders --endpoint /customers
```

Replica (the catalog must already exist, or startup fails):

```yaml
# flapi: config group=replica
project-name: orders-replica
project-description: Read-only replica of a shared DuckLake catalog
template:
  path: ./sqls
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake        # shared catalog, e.g. postgres:dbname=flapi host=10.0.0.5
  data-path: ./cache_data                # e.g. gs://my-api-data/cache/
  read-only: true                        # no warm-up/refresh/retention/compaction; mutations -> 409
```

On a replica, `POST /api/v1/_config/endpoints/{slug}/cache/refresh` answers 409
`read_only_cache`, and endpoints read the snapshot the writer committed.

Fast startup: use the `-runtime` image with `FLAPI_EXTENSIONS_OFFLINE=1`, or
`flapi pack --with-extensions`. CI smoke test without touching data: `flapi --validate-config`
never opens the database. `flapi -c flapi.yaml --no-cache-warmup` starts with every cache
unbuilt: `/health` answers 200 `degraded`, and cached endpoints answer 503
`cache_not_built`.

## PyPI

```bash
pip install flapi-io==26.10.31     # console scripts: flapi (server) and flapii (CLI)
```

There is one package, `flapi-io`, with wheels for `manylinux_2_17_x86_64`,
`manylinux_2_17_aarch64`, `macosx_11_0_arm64` and `win_amd64`. No other platform gets a
wheel. Use the GitHub release archives or Docker there.

## Production configuration

```yaml
# flapi: config group=prod
project-name: customers-api
project-description: Production customers API
http-host: 0.0.0.0
http-port: 8080
log-level: info
log-format: json                 # one JSON object per line, with request_id
stall-timeout-s: 30
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: ./data/customers.parquet
cors:
  allow-origins: [https://app.example.com]
rate_limit:
  enabled: true
  max: 600
  interval: 60
  key: user-or-ip
telemetry:
  enabled: false
```

```yaml
# flapi: endpoint group=prod file=customers.yaml
url-path: /customers
request:
  - field-name: id
    field-in: query
    required: false
    validators:
      - type: int
        min: 1
template-source: customers.sql
connection: [customers]
# flapi: check GET /health/live -> 200
# flapi: check GET /health -> 200
# flapi: check GET /customers?id=1 -> 200
# flapi: check GET /customers?id=abc -> 400
```

```sql
-- flapi: template group=prod file=customers.sql
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{ params.id }}
{{/params.id}}
```

Endpoint auth (`auth:` per endpoint, or global `auth:`) is covered in `flapi-auth`. Tracing
and audit logs are covered in `flapi-operate`.

## Environment variables

| Variable | Effect |
|---|---|
| `FLAPI_CONFIG` | config path when `-c` is not given (also for `flapi cache refresh`) |
| `FLAPI_PORT` / `FLAPI_HOST` | port / bind address when `-p` / `--host` are not given; they beat `http-port` / `http-host` |
| `FLAPI_LOG_LEVEL` | `debug`, `info`, `warning`, `error`; beats `log-level`; invalid = exit 1 |
| `FLAPI_CONFIG_SERVICE_TOKEN` | token for `--config-service` (server) and for `flapii` (client) |
| `FLAPI_CACHE_WARMUP` | `always`, `if-empty`, `never` (`--cache-warmup` / `--no-cache-warmup` win) |
| `FLAPI_EXTENSION_DIRECTORY`, `FLAPI_EXTENSIONS_OFFLINE` | where extensions live / never download |
| `FLAPI_NO_TELEMETRY=1`, `DATAZOO_DISABLE_TELEMETRY=1` | telemetry off (as do `--no-telemetry` and `telemetry.enabled: false`) |
| `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION`, `AWS_SESSION_TOKEN`, `AWS_ENDPOINT_URL` | copied into DuckDB's `s3_*` settings at startup |
| `AZURE_STORAGE_CONNECTION_STRING` (or `AZURE_STORAGE_ACCOUNT` + `AZURE_STORAGE_KEY`) | set as DuckDB azure settings at startup (needs the `azure` extension loaded by an `init`); an explicit `CREATE SECRET` is more reliable |
| anything read as `{{env.X}}` | must match `template.environment-whitelist` |

## Canonical deploy workflow

1. `flapi -c flapi.yaml --validate-config` must show no errors, and every warning must be
   understood.
2. Check for secrets: no literal credentials in YAML or SQL, and every `{{env.X}}` is
   whitelisted. `.env`, `*.pem` and `*.key` stay out of the tree that is shipped.
3. Expose the server deliberately. Put `auth:` on every endpoint with non-public data
   (`flapi-auth`). Set `cors.allow-origins` and `rate_limit`. Leave the config service off,
   or protect it with `FLAPI_CONFIG_SERVICE_TOKEN` and keep `/api/v1/_config` off the
   public network.
4. Choose a cache strategy for the target. Use `if-empty`/`never` with a prebuilt catalog
   for scale-to-zero. Replicas use `ducklake.read-only: true`. Set
   `ducklake.scheduler.enabled: true` if anything uses `cache.schedule` (`flapi-cache-design`).
5. Build the artifact: `flapi pack --in . --out api --with-extensions`, or an image `FROM
   ghcr.io/datazoode/flapi:v26.10.31-runtime`, or `pip install flapi-io==26.10.31`.
6. Prove it is self-contained: run it from another directory or a fresh container, then
   `curl /health/live`, `/health` and one real endpoint. Check `./api info` for a bundle.
7. Wire the probes: liveness `/health/live`, readiness `/health`.
8. Operate it: `log-format: json`, and opt out of telemetry if required (`--no-telemetry`,
   `FLAPI_NO_TELEMETRY=1` or `DATAZOO_DISABLE_TELEMETRY=1`). Use tracing and metrics
   (`flapi-operate`).
