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
- The file was added after startup. Reload only re-reads files that were loaded.
- It is an MCP-only YAML (`mcp-tool:` without `url-path:`): no REST route by design.
- `/api/v1/_config/*` 404: the server runs without `--config-service`.
- `POST .../reload` 404 `{"success":false}`: endpoint unknown **or** its YAML is now invalid
  (see the server log line `Validation failed for ...`).

Fix: correct `url-path` or the request; restart after adding files; `flapii endpoints reload
<path>` after editing a loaded one.

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

Likely causes: the value is outside `min`/`max`, not in `allowedValues`, fails `regex`, a
required field is missing, or `request-fields-validation: true` rejects an undeclared
query parameter.

Fix: send a valid value, or change the validator and `flapii endpoints reload <path>`.
Validator syntax: `flapi-rest`.

---

## 401 Unauthorized / 403 Forbidden

401 bodies: `401 Unauthorized` with `WWW-Authenticate: Basic realm="flAPI"` (or Bearer) on an
endpoint with `auth:`; `Unauthorized: Invalid or missing token` on `/api/v1/_config/*`. MCP
with `mcp.auth.enabled`: HTTP 401, JSON-RPC `-32000 Authentication required for method: ...`.
403 only comes from MCP: `Permission denied: Tool 'x' requires one of [analyst]; caller has
[reader].` (HTTP 403, `insufficient_scope`).

Commands:
```bash
curl -s -i "$B/secure"                                   # which challenge?
curl -s -i -u alice:secret "$B/secure"                   # Basic
curl -s -i -H "Authorization: Bearer $JWT" "$B/secure"   # JWT / OIDC
flapii endpoints get /secure                             # its auth block
flapii metrics                                           # rejections.unauthorized / forbidden
```

Likely causes: wrong credentials; expired JWT or wrong issuer/secret; a `{{env.X}}` password
that was empty at startup; the config token passed to an MCP config tool via `X-Config-Token`
(MCP accepts only `Authorization: Bearer`) or shorter than 8 characters; MCP tool without
`allowed-roles` while `mcp.auth.enabled: true` (denied by default).

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

Body: `Internal Server Error: Query execution failed: Binder Error: Referenced column
"no_such_col" not found in FROM clause! ... LINE 1: SELECT * FROM (SELECT no_such_col ...`

Commands:
```bash
grep "<X-Request-Id>" server.log                          # error with request_id
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
flapii cache audit /products               # message = the populate SQL error
grep -i "cache build failed\|warmup failed" server.log
```

| Body / health | Cause | Fix |
|---|---|---|
| `cache_warming` "still being built"; health `starting` | Warm-up in progress | Wait; size the startup probe for the build time |
| `cache_warming` "failed to build"; health `degraded` (503) | Populate SQL failed | Fix the cache template, `flapii cache refresh <path>` |
| `cache_not_built`; health `degraded` (200) with `unbuilt` | Warm-up skipped | `flapii cache refresh <path>` or drop `--no-cache-warmup` |
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
| `Missing required key: allowedValues` | `values:` used for an enum | Use `allowedValues:` |
| `Missing required key: project-description` | Required top-level key missing | Add it |
| `has a description that failed strict validation (mcp.strict-descriptions is true)` | Description scanner | Rephrase the description (`flapi-mcp`) |
| `IO Error: Extension ".../nosuchext.duckdb_extension" not found` | `LOAD` of an extension that is not installed / offline | `INSTALL` it in `init`, or `flapi extensions install -c flapi.yaml` at build time |
| `Could not set lock on file ".../cache.ducklake": Conflicting lock is held in .../flapi (PID n)` | Another flAPI process (or `flapi cache refresh`) holds the DuckLake | Stop it (`ps -p n`), or give each process its own `metadata-path`; use replicas (`ducklake.read-only`) for scale-out |
| `the server could not start: bind: Address already in use` | Port taken | `-p <port>` / `FLAPI_PORT`, or stop the other process |
| `flapi: invalid log level 'warn'` | Wrong level name | `debug`, `info`, `warning`, `error` |

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
