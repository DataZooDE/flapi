# flAPI troubleshooting playbooks

Each playbook: **symptom -> commands -> likely causes -> fix**. Error texts below were
taken from a running v26.10.31 server. Set `B=http://localhost:8080` and export
`FLAPI_CONFIG_SERVICE_TOKEN` for the `flapii` steps (server started with
`--config-service`).

Always start with: `curl -s -i "$B/<path>?<query>"` (status, body, `X-Request-Id`) and
`curl -s $B/health`.

---

## 404 Not Found

Body is plain `Not Found`. In metrics it is counted under route `<unmatched>`.
A cached endpoint answering `404 {"error":"snapshot_not_found"}` was asked for an
`AT (VERSION => n)` / `AT (TIMESTAMP => t)` snapshot that does not exist or was
expired by retention (`cache.retention`).

Commands:
```bash
flapii endpoints list -o table                   # what is loaded
grep -n "url-path" sqls/*.yaml sqls/**/*.yaml
grep -i "error\|warning" server.log | head       # files that failed to load at startup
```

Likely causes:
- The path differs from `url-path`. Matching is exact: `/customers/` is not `/customers`.
- The YAML is outside `template.path`, or failed to parse (startup stops on a bad YAML,
  so a running server means the file was not there or not an endpoint).
- The file was added after startup: load it with `flapii endpoints reload --file <yaml>`.
- It is an MCP-only YAML (`mcp-tool:` without `url-path:`): no REST route by design.
- `/api/v1/_config/*` 404: the server runs without `--config-service`.
- `POST .../reload` 404 `{"success":false}`: the endpoint is unknown. A reload of a known
  endpoint whose YAML is now invalid answers **400** with the parser's `reason`.

Fix: correct `url-path` or the request; `flapii endpoints reload --file <yaml>` after adding a
file by hand (or create it through the config service: `flapii endpoints create`, which also
writes the YAML); `flapii endpoints reload <path>` after editing a loaded one.

---

## 400 Bad Request (validation)

Body: `{"errors":[{"field":"nation","message":"Integer is greater than the maximum allowed value"}]}`.
Other messages: `Required field is missing`, `Invalid enum value`. Over MCP the same text
comes back as a tool error (`isError: true`, `Invalid arguments for tool 'x': nation: ...`).

Commands:
```bash
flapii endpoints parameters /customer-lookup     # fields, validators, defaults
flapii endpoints get /customer-lookup
```

Likely causes: the value is outside `min`/`max`, not in `allowedValues` (alias `values`),
fails `regex` (alias `pattern`), a required field is missing, a `field-in: header` field
was sent as a query or body parameter (header fields are read only from request headers,
case-insensitively), or `request-fields-validation: true` rejects an undeclared query
parameter.

Fix: send a valid value, or change the validator and `flapii endpoints reload <path>`.
Validator syntax: `flapi-rest`.

---

## 401 Unauthorized / 403 Forbidden

401 bodies: `401 Unauthorized` with `WWW-Authenticate: Basic realm="flAPI"` (or Bearer) on an
endpoint with `auth:`; `Unauthorized: Invalid or missing token` on `/api/v1/_config/*`. MCP
with `mcp.auth.enabled`: HTTP 401, JSON-RPC `-32000 Authentication required for method: ...`
when no credentials were sent, `-32000 The credentials sent were not accepted.` when they were
refused (an expired or wrongly signed JWT). Every request answering 401 for every user: check
`auth.type` - an unknown scheme (`basci`) is now a load/reload error (`AUTH_UNKNOWN_TYPE`).
403 on REST: `{"error":"Forbidden","message":"The authenticated principal does not hold any
role this endpoint requires."}` when `auth.roles` is set and the caller has none of them
(log: `Authorization denied for user 'x' on REST endpoint: /y`). 403 on MCP: `Permission
denied: Tool 'x' requires one of [analyst]; caller has [reader].` (HTTP 403,
`insufficient_scope`).

Commands:
```bash
curl -s -i "$B/secure"                                   # which challenge?
curl -s -i -u alice:secret "$B/secure"                   # Basic
curl -s -i -H "Authorization: Bearer $JWT" "$B/secure"   # JWT / OIDC
flapii endpoints get /secure                             # its auth block
flapii metrics                                           # rejections.unauthorized / forbidden
```

Likely causes: wrong credentials; expired JWT or wrong issuer/secret; a `{{env.X}}` password
that was empty at startup; a top-level `auth:` block that every endpoint without its own
`auth:` inherits (opt out with `auth: {enabled: false}`; startup logs `Global auth (type
'basic') applies to N endpoint(s)...`); a bcrypt hash (`AUTH_BCRYPT_UNSUPPORTED`: use
PBKDF2); the wrong config token for an MCP config tool (sent as `X-Config-Token` or
`Authorization: Bearer`); MCP tool without `allowed-roles` while `mcp.auth.enabled: true`
(denied by default). 403: the caller's roles miss every role in `auth.roles`.

Fix: see `flapi-auth`.

---

## 429 Too Many Requests

Body `Rate limit exceeded. Try again later.`, headers `Retry-After: <s>`,
`X-RateLimit-Limit`, `X-RateLimit-Remaining: 0`, `X-RateLimit-Reset`.

Commands:
```bash
curl -s -i "$B/limited" | grep -i "ratelimit\|retry-after"
flapii endpoints get /limited                    # rate-limit block
flapii metrics                                   # rejections.rate_limited
```

Likely causes: the endpoint's `rate-limit` (`max` per `interval` seconds) or the global
`rate_limit` block; many clients behind one IP or one principal.

Fix: raise `max`/`interval` (reload the endpoint; restart for the global block), or make
clients honour `Retry-After`. MCP per-tool limits live in `mcp-tool.rate-limit`.

---

## 500 Internal Server Error

Body (JSON, no SQL, no paths):
`{"error":"Internal Server Error","message":"The request failed on the server. The details
are in the server log under this request id.","request_id":"req-46788500aef28a89"}`. The
log has the DuckDB error under that id, e.g. `Catalog Error: Table with name no_such_table
does not exist!`.

Commands:
```bash
grep "req-46788500aef28a89" server.log                    # the error, by request_id
flapii config log-level set debug                         # then repeat: logs context + SQL
flapii templates expand /broken -p '{"id":"1"}'           # the rendered SQL
flapii templates test /broken -p '{"id":"1"}'             # run it through the server
flapii schema get                                         # tables/columns that exist
```

Likely causes, by error text:
- `Binder Error ... not found` / `Catalog Error: Table with name X does not exist`: wrong
  column/table, or a cache table queried before it was built.
- `Parser Error`: the template rendered invalid SQL (an empty optional section, a quoted
  `'{{ params.x }}'` that became `'?'`, a missing default).
- `Unable to open database` / `IO Error`: a connection path or remote source is wrong or
  unreachable (SQLite `ATTACH` is lazy and fails on first use).
- `HTTP 403` / credentials errors from httpfs: cloud secrets missing (`flapi-setup`).

Fix: correct the template (no reload needed for `.sql`), validate, re-test, reset the log
level (`flapii config log-level set info`).

---

## 503 Service Unavailable

Commands:
```bash
curl -s -i "$B/products"                   # body + Retry-After
curl -s $B/health                          # starting / degraded / stalled
flapii metrics -e /products                # caches[].state, last.status
flapii cache audit /products -o table      # status per refresh; JSON message = the error
grep -i "cache build failed" server.log
```

| Body / health | Cause | Fix |
|---|---|---|
| `cache_warming` "still being built"; health `starting` | Warm-up in progress | Wait; size the startup probe for the build time |
| `cache_build_failed`; health `degraded` (503) | First build failed: SQL error, a bare `SELECT` (`statement N of the template is a bare SELECT`), or the script left no table | Fix the cache template, `flapii cache refresh <path>` |
| `cache_not_built`; health `degraded` (200) with `unbuilt` | Warm-up skipped | `flapii cache refresh <path>` or drop `--no-cache-warmup`. On a read-only replica: refresh on the writer; the replica serves it within ~5 s (log `now exists in the shared catalog`) |
| `The database is busy; retry shortly.` | Lock contention (a write or another process) | Retry; avoid a second process on the same DuckDB/DuckLake |
| `Server is at capacity. Try again shortly.` | Handler queue full | Find slow queries (metrics), add instances |
| health `stalled` | A request older than `stall-timeout-s` | Find the wedged source (log, `requests.oldest_ms`); restart the instance |

---

## Server will not start

Run it in the foreground and read the last `[ERROR]` lines. `flapi -c flapi.yaml
--validate-config` catches config errors, **not** database ones.

| Error text | Cause | Fix |
|---|---|---|
| `environment variable DB_PASSWORD referenced as {{env.NAME}} but not whitelisted` | `{{env.X}}` not matched by `template.environment-whitelist` | Add a regex such as `'^DB_.*'` |
| `Failed to parse YAML: ... yaml-cpp: error at line 2, column 18: illegal map value` (with the file name) | YAML syntax / indentation | Fix that file |
| `Invalid compaction schedule '0 3 * * *'` | Cron in `ducklake.compaction.schedule` | An interval (`1d`) or `@hourly`/`@daily`/`@midnight`/`@weekly` |
| `heartbeat.worker-interval must be a positive number of seconds` / `Invalid scheduler scan-interval` | `0`, negative or unparseable | A positive value (`60`, `30s`) |
| `'issuer-url' (...{tenant}...) still contains the {tenant} placeholder. Set tenant-id` | OIDC preset without `tenant-id` / `realm` / `domain` | Add the key the message names |
| `has a description that failed strict validation (mcp.strict-descriptions is true)` | Description scanner | Rephrase the description (`flapi-mcp`) |
| `IO Error: Extension ".../nosuchext.duckdb_extension" not found` | `LOAD` of an extension that is not installed / offline | `INSTALL` it in `init`, or `flapi extensions install -c flapi.yaml` at build time |
| `Failed to attach DuckLake catalog: another process holds the lock on the DuckDB-file catalog ...` (raw: `Conflicting lock is held in .../flapi (PID n)`) | Another flAPI process (a writer, a replica, or `flapi cache refresh`) holds the DuckDB-file catalog | Stop it (`ps -p n`), or give each process its own `metadata-path`; a writer with replicas needs a Postgres/SQLite/MySQL catalog |
| `Failed to attach DuckLake catalog: directory ... does not exist (ducklake.read-only: ...)` | Replica started before the catalog exists | Start the writer first, or fix `metadata-path` / `data-path` (a writer creates them) |
| `Cannot list endpoint files under s3://... : listing failed (...) and the manifest ... could not be read` | A remote `template.path` (bucket or http dir): empty/missing prefix, bad credentials, or an http dir with no `manifest.txt` | Check the quoted listing error (403 = credentials: `AWS_*`, `GCS_ACCESS_KEY_ID`/`GCS_SECRET_ACCESS_KEY` for gs://, `AZURE_STORAGE_*` for az://); for plain http(s) add `manifest.txt` (one endpoint YAML per line) |
| `Bootstrap DuckDB: \`LOAD httpfs\` failed` | Remote `--config`/`template.path` on a host without httpfs (offline) | Pre-install httpfs into `FLAPI_EXTENSION_DIRECTORY` and set `FLAPI_EXTENSIONS_OFFLINE=1` |
| `the server could not start: bind: Address already in use` | Port taken | `-p <port>` / `FLAPI_PORT`, or stop the other process |
| `invalid log level 'verbose'; must be one of: debug, info, warning (or warn), error` | Unknown `log-level` | One of those |

Exit code 1 for all of these (not a crash).

---

## Slow requests

Commands:
```bash
flapii metrics -o table                    # p95/p99/max per endpoint and tool
flapii metrics -e /slow-endpoint
curl -s -o /dev/null -w "%{time_total}\n" "$B/slow-endpoint?..."
flapii templates expand /slow-endpoint -p '{..}'     # then EXPLAIN ANALYZE it in duckdb
```

Likely causes: every request scans a remote source (Postgres/BigQuery/S3) with no cache;
filters applied after a wide scan; large pages (`limit`) or MCP calls with no row cap;
`count(*)` of pagination on a heavy query (a second query per page); lock waits behind a
writer.

Fix: put a DuckLake cache in front (`flapi-cache-design`), tune its refresh
(`flapi-cache-tuning`), push filters into the source query, cap rows, and turn on tracing
with `db_profiling: summary` to see time spent inside DuckDB.

---

## High memory

Commands:
```bash
ps -o rss,cmd -p "$(pgrep -f 'flapi -c')"
flapii metrics                             # arrow.active_streams, big endpoints
```

Likely causes: no `max_memory` limit (DuckDB defaults to 80% of RAM); huge result sets
materialised for JSON; many concurrent heavy queries; caches with many old snapshots.

Fix: any key under `duckdb:` other than `db_path` and `default-extensions` is passed to DuckDB
as a setting, so `max_memory: 2GB` and `threads: 4` there are applied at startup (verified:
`current_setting('max_memory')` reported `953.6 MiB` for `1GB`). Cap rows per request, set
`cache.retention` and run `flapii cache gc <path>`.
