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
| `GET /api/v1/_config/project` | | Project name, description and settings (secrets redacted) | `ping`, `project get`, `config show` |
| `PUT /api/v1/_config/project` | | 501 Not implemented | |
| `GET /api/v1/_config/environment-variables` | | Whitelisted env vars and whether they are set | `config env` |
| `GET /api/v1/_config/filesystem` | | Template directory tree | `config filesystem` |
| `GET /api/v1/_config/endpoints` | | All endpoints | `endpoints list` |
| `POST /api/v1/_config/endpoints` | endpoint JSON | Add an endpoint, in memory only (201) | `endpoints create` |
| `GET /api/v1/_config/endpoints/{slug}` | | One endpoint's config | `endpoints get` |
| `PUT /api/v1/_config/endpoints/{slug}` | endpoint JSON | Replace, in memory only | `endpoints update` |
| `DELETE /api/v1/_config/endpoints/{slug}` | | Remove, in memory only (file stays, back on restart) | `endpoints delete` |
| `POST /api/v1/_config/endpoints/{slug}/validate` | raw YAML text | `{"valid":..,"errors":[..],"warnings":[..]}` | `endpoints validate <path> -f file` |
| `POST /api/v1/_config/endpoints/{slug}/reload` | | Re-read the endpoint's YAML file. 200 `{"success":true}` or 404 `{"success":false}` (not found **or** invalid) | `endpoints reload` |
| `GET /api/v1/_config/endpoints/{slug}/parameters` | | Request fields with validators | `endpoints parameters` |
| `POST /api/v1/_config/endpoints/by-template` | `{"template_path":"/abs/or/rel.sql"}` | Endpoints that use a template file | (hook) |
| `GET /api/v1/_config/endpoints/{slug}/template` | | `{"template":"..."}` | `templates get` |
| `PUT /api/v1/_config/endpoints/{slug}/template` | `{"template":"..."}` | Write the `.sql` file (persists) | `templates update` |
| `POST /api/v1/_config/endpoints/{slug}/template/expand` | `{"parameters":{..}}` | `{"expanded":"<sql>"}`, nothing runs | `templates expand` |
| `POST /api/v1/_config/endpoints/{slug}/template/test` | `{"parameters":{..}}` | Executes; `{"success","columns","rows"}` (up to 10 rows) | `templates test` |
| `GET /api/v1/_config/endpoints/{slug}/cache` | | Cache config | `cache get` |
| `PUT /api/v1/_config/endpoints/{slug}/cache` | `{"enabled":true,"table":"..","schedule":"10m"}` | Change cache config | `cache update` |
| `GET\|PUT /api/v1/_config/endpoints/{slug}/cache/template` | `{"template":"..."}` | Cache populate SQL | `cache template`, `cache update-template` |
| `POST /api/v1/_config/endpoints/{slug}/cache/refresh` | | Refresh now (400 with the error if it fails; 409 `read_only_cache` on a replica) | `cache refresh` |
| `POST /api/v1/_config/endpoints/{slug}/cache/gc` | | Expire old snapshots per `retention` | `cache gc` |
| `GET /api/v1/_config/endpoints/{slug}/cache/audit` | | Refresh history: `sync_started_at`, `sync_completed_at`, `duration_ms`, `rows_affected`, `snapshot_id`, `status`, `message` | `cache audit <path>` |
| `GET /api/v1/_config/cache/audit` | | Same, all endpoints | `cache audit` |
| `GET /api/v1/_config/schema` | | Attached databases, tables, columns | `schema get` |
| `POST /api/v1/_config/schema/refresh` | | Re-read the schema | `schema refresh` |
| `GET\|PUT /api/v1/_config/log-level` | `{"level":"debug"}` | Runtime log level (`debug`, `info`, `warning`, `error`) | `config log-level get\|set` |
| `GET /api/v1/_config/metrics[?endpoint=/path]` | | Runtime metrics (see `flapi-operate`) | `metrics [-e path]` |
| `GET /config` | | Whole parsed configuration | |
| `DELETE /config` | | 500 `Not implemented` (no full reload) | |
| `GET /api/v1/doc.yaml` | | OpenAPI document of the endpoints | |

`?tables=true` on `/schema` (behind `flapii schema tables`) currently returns an error
string, and `?connections=true` (`flapii schema connections`) always returns `[]`.

## MCP config tools (`flapi_*`)

Listed in `tools/list` only with `--config-service`. Every call needs
`Authorization: Bearer <config-service token>` (8+ characters) on the MCP request.
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
| `flapi_create_endpoint` | `path` | `method`, `template-source`, `connection` | **writes `<slug>.yaml`** (and a placeholder SQL if missing) under `template.path` |
| `flapi_update_endpoint` | `path` | other config fields | PUT endpoint (memory) |
| `flapi_delete_endpoint` | `path` | | **removes the endpoint's YAML file** too |
| `flapi_reload_endpoint` | `path` | | POST reload |
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
