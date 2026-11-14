# Config service API: routes and MCP tools

Every route needs `Authorization: Bearer <token>` or `X-Config-Token: <token>` unless
marked "open". All exist only when the server runs with `--config-service`.

## Slugs

`{slug}` is the endpoint's URL path encoded as `/` -> `-`, `-` -> `~1`, `~` -> `~0`.

| Endpoint | Slug |
|---|---|
| `/products` | `-products` |
| `/customer-lookup` | `-customer~1lookup` |
| `/customers/` | `-customers-` |
| MCP-only tool `top_products` | `top_products` |
| MCP resource `segment_summary` / prompt `segment_review` | `segment_summary` / `segment_review` |

## REST routes

| Method and path | Body | Does | flapii |
|---|---|---|---|
| `GET /api/v1/_config/health` (open) | | Server, database, endpoint count, Arrow, storage, credentials | `health` |
| `GET /api/v1/_config/project` | | Project name, description and settings (secrets redacted, including a `password=` in `ducklake.metadata-path`); `ducklake.warmup_effective` is the warm-up that runs (`never` on a replica) | `ping`, `project get`, `config show` |
| `PUT /api/v1/_config/project` | | 405 `Allow: GET` (read-only; edit `flapi.yaml`) | |
| `GET /api/v1/_config/environment-variables` | | Whitelisted env vars and whether they are set | `config env` |
| `GET /api/v1/_config/filesystem` | | Template directory tree | `config filesystem` |
| `GET /api/v1/_config/endpoints` | | All endpoints | `endpoints list` |
| `POST /api/v1/_config/endpoints` | endpoint JSON (YAML keys) | Parse with the real loader, write `<template dir>/<slug>.yaml` (method stored upper-case); 201 `{"status":"created","endpoint","slug","url-path","file"}`; 409 exists (names the existing endpoint and its file), same route shape, an overlapping route whose auth it would loosen (`Overlapping routes with different auth: ...`), or packed bundle; 400 parse error, a url-path outside `[A-Za-z0-9._~-]` / `:name` segments, or a method other than GET/POST/PUT/PATCH/DELETE | `endpoints create` |
| `GET /api/v1/_config/endpoints/{slug}` | | One endpoint's config | `endpoints get` |
| `PUT /api/v1/_config/endpoints/{slug}` | partial JSON with `url-path` (or the `mcp-*` block) | Deep-merge onto the YAML file, re-parse, persist (200 `"status":"updated"`). Changing url-path/method/name 400 (`get` for a GET endpoint is no change); unsupported method 400; an auth change that makes an overlapping route loosen another's protection 409; `{{include}}` file 409; camelCase body: memory only, `"persisted": false`; body without an identity key: 500 `Missing required field in endpoint config` | `endpoints update` |
| `DELETE /api/v1/_config/endpoints/{slug}` | | Remove the endpoint and its YAML file | `endpoints delete` |
| `POST /api/v1/_config/endpoints/{slug}/validate` | raw YAML text | Real parser: `{"valid":..,"errors":[..],"warnings":[..]}` | `endpoints validate <path> -f file` (or YAML on stdin) |
| `POST /api/v1/_config/endpoints/{slug}/reload[?method=M]` | | Re-read the endpoint's YAML file. 200 `{"success":true}`; 400 with `reason` when it does not parse, or "Ambiguous" when several endpoints share the path and no `method` is given; 404 only for an unknown endpoint | `endpoints reload [--method M]` |
| `POST /api/v1/_config/endpoints/reload-file` | `{"file":"/abs/sqls/x.yaml"}` | Reload every endpoint the file declares, or LOAD a file not loaded yet. 200 reloaded / 201 loaded, `{"endpoints":["POST /orders"]}`; a deleted file: 200 `"status":"removed"`, `"removed":[...]` (its endpoints are unloaded; 404 when nothing was loaded from it); 400 outside `template.path` / invalid / not an endpoint file; 409 duplicate url-path+method (route shape) or MCP name, or an overlapping route with looser auth | `endpoints reload --file` |
| `GET /api/v1/_config/endpoints/{slug}/parameters` | | Request fields with validators | `endpoints parameters` |
| `POST /api/v1/_config/endpoints/by-template` | `{"template_path":"/abs/or/rel.sql"}` | Endpoints that use a template file, as `template-source` or cache `template-file` (`role`/`roles`, `config_file_path`, `slug`) | (hook) |
| `GET /api/v1/_config/endpoints/{slug}/template` | | `{"template":"..."}` | `templates get` |
| `PUT /api/v1/_config/endpoints/{slug}/template` | `{"template":"..."}` | Write the `.sql` file (persists) | `templates update` |
| `POST /api/v1/_config/endpoints/{slug}/template/expand` | `{"parameters":{..}}` | `{"expanded":"<sql>"}`, nothing runs | `templates expand` |
| `POST /api/v1/_config/endpoints/{slug}/template/test` | `{"parameters":{..}}` | Executes; `{"success","columns","rows"}` (up to 10 rows) | `templates test` |
| `GET /api/v1/_config/endpoints/{slug}/cache` | | Cache config | `cache get` |
| `PUT /api/v1/_config/endpoints/{slug}/cache` | `{"enabled":true,"table":"..","schedule":"10m"}` | Change cache config | `cache update` |
| `GET\|PUT /api/v1/_config/endpoints/{slug}/cache/template` | `{"template":"..."}` | Cache populate SQL; PUT is 409 `read_only_cache` on a replica | `cache template`, `cache update-template` |
| `POST /api/v1/_config/endpoints/{slug}/cache/refresh[?full=true]` | | Refresh now; `?full=true` drops and rebuilds the table, watermark erased. 200 `{"status":"refreshed","endpoint","mode","full","duration_ms","rows_affected","snapshot_id"}` (`"already_running"` if one of the same table is in flight; refreshes of other tables wait, one at a time per catalog); `full` is true for `?full=true`, a full-mode cache or a first incremental build; 400 `{"error":"Cache refresh failed","reason":...}` if it fails; 409 `read_only_cache` on a replica; 503 `database_invalidated` | `cache refresh [--full]` |
| `POST /api/v1/_config/endpoints/{slug}/cache/gc` | | Expire old snapshots per `retention`, delete freed files. 200 `{"status":"expired"\|"nothing_to_expire"\|"no_policy","expired_snapshots","files_deleted","bytes_freed","message","warning"?}`; 500 `status: "error"`; 409 on a replica | `cache gc` |
| `POST /api/v1/_config/cache/compact` | `{}` or `{"endpoint":"/orders","method":"GET"}` | Compaction now (catalog or one table). 200 `{"status":"compacted","duration_ms","files_rewritten"?}`; 404 unknown endpoint; 409 on a replica | `cache compact [path]` |
| `POST /api/v1/_config/cache/drop-table` | `{"schema":"old","table":"t"}` | Drop a cache table no endpoint uses; expire its snapshots (and apply the retention of live tables that still pin its files; `pinned_by` names what is kept), delete its files. 200 `{"status":"dropped",...}`; 409 `cache_table_in_use` / `protected_table` / `read_only_cache`; 404 `table_not_found` | `cache drop-table [schema.]table` |
| `GET /api/v1/_config/endpoints/{slug}/cache/audit` | | Refresh history: `sync_started_at`, `sync_completed_at`, `duration_ms`, `rows_affected`, `snapshot_id`, `status`, `message`, `full` | `cache audit <path>` |
| `GET /api/v1/_config/cache/audit` | | Same, all endpoints; `?table=schema.table`, `?sync_type=compaction` (or `full`, `append`, `merge`, `garbage_collection`, `drop_table`) | `cache audit` |
| `GET /api/v1/_config/schema` | | Attached databases, tables, columns; `?tables=true` (optional `&connection=`): table list with `qualified_name`; `?connections=true`: configured connections. Failures 500 | `schema get`, `schema tables`, `schema connections` |
| `POST /api/v1/_config/schema/refresh` | | Re-read the schema | `schema refresh` |
| `GET\|PUT /api/v1/_config/log-level` | `{"level":"debug"}` | Runtime log level (`debug`, `info`, `warning`, `error`; `warn` = `warning`) | `config log-level get\|set` |
| `GET /api/v1/_config/metrics[?endpoint=/path]` | | Runtime metrics (see `flapi-operate`) | `metrics [-e path]` |
| `GET /config` | | Whole parsed configuration | |
| `DELETE /config` | | 405 `Allow: GET` (no full reload; 401 first without a token) | |
| `GET /api/v1/doc.yaml` | | OpenAPI document of the endpoints | |

Errors from endpoint create, update and delete are JSON `{"error":"..."}`.

## MCP config tools (`flapi_*`)

Listed in `tools/list` only with `--config-service`. Every call needs the config-service
token on the MCP request: `X-Config-Token` (read first) or `Authorization: Bearer`. Every
create/update/delete/reload rebuilds `tools/list`, `resources/list` and `prompts/list`.
Arguments named `path` / `endpoint` take the URL path (`/customers`), not a slug.

| Tool | Required args | Optional | Same as |
|---|---|---|---|
| `flapi_get_project_config` | | | GET project |
| `flapi_get_environment` | | | GET environment-variables |
| `flapi_get_filesystem` | | | GET filesystem |
| `flapi_get_schema` | | | GET schema |
| `flapi_refresh_schema` | | | POST schema/refresh |
| `flapi_list_endpoints` | | | GET endpoints |
| `flapi_get_endpoint` | `path` | | GET endpoint |
| `flapi_create_endpoint` | `path` | `method`, `template-source`, `connection`, `auth` (a YAML `auth:` block as JSON) | **writes `<name>.yaml`** (`/a/b` -> `a_b.yaml`; and a placeholder SQL if missing) under `template.path`, through the same loader and checks as REST create (inherits a top-level `auth:` block; duplicate / overlapping-route refusals name the other endpoint). Result has `"auth"` and, when unauthenticated, a `"warning"` |
| `flapi_update_endpoint` | `path` | `config` (endpoint keys, YAML spelling), or the keys at top level (`template-source`/`template_source`, `connection`, ...) | **merged into the YAML and persisted**, like REST `PUT`; changing url-path/method is refused |
| `flapi_delete_endpoint` | `path` | | **removes the endpoint's YAML file** too |
| `flapi_reload_endpoint` | `path` | `method` (needed when several endpoints share the path) | POST reload |
| `flapi_get_template` | `endpoint` | | GET template |
| `flapi_update_template` | `endpoint`, `content` | | PUT template |
| `flapi_expand_template` | `endpoint` | `params` | POST expand |
| `flapi_test_template` | `endpoint` | `params` | POST test (executes) |
| `flapi_get_cache_status` | `path` | | GET cache |
| `flapi_refresh_cache` | `path` | | POST cache/refresh |
| `flapi_get_cache_audit` | `path` | | GET cache/audit |
| `flapi_run_cache_gc` | `path` | | POST cache/gc |
| `flapi_get_metrics` | | `endpoint` (path or MCP tool name) | GET metrics |

Call one with curl:

```bash
curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"flapi_get_metrics","arguments":{"endpoint":"/products"}}}'
```

The result's `content[0].text` is the same JSON document as the REST route (snake_case).
