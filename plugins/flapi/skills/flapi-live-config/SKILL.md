---
name: flapi-live-config
description: >
  Managing a running flAPI server: the config service (`--config-service`, its token,
  `FLAPI_CONFIG_SERVICE_TOKEN`, `X-Config-Token`), the full `flapii` command map and
  global options, the REST routes under /api/v1/_config, the `flapi_*` MCP config
  tools (including flapi_get_metrics), hot reload and what it does not pick up,
  schema introspection, runtime log level, and the plugin's validate-and-reload hook.
  Use when a user wants to list, inspect, edit, reload or test endpoints, templates
  and caches on a live server, run flapii, or script the config API.
version: 26.10.31
user-invocable: false
---

# flAPI Live Configuration Cheat Sheet

**flAPI:** v26.10.31 | **Skill:** Develop | **See also:** `flapi-operate`, `flapi-mcp`, `flapi-rest`, `flapi-cache-design`, `flapi-testing`

The config service is a token-protected REST API (`/api/v1/_config/...`) and a set of MCP
tools (`flapi_*`) inside the server. `flapii` is its CLI client. It is **off by default**.

## Critical gotchas

- **Nothing works without `--config-service`.** Every `/api/v1/_config/*` route is a 404
  (the routes are not registered), `flapii` fails, and no `flapi_*` tool is listed.
- **A generated token is printed once, on stdout, in the startup banner.** A token you pass
  (`--config-service-token` or `FLAPI_CONFIG_SERVICE_TOKEN`) is never echoed. Pass your own;
  REST and the MCP config tools accept any non-empty token, and startup warns when it is
  shorter than 16 characters.
- **Hot reload is per endpoint YAML.** `flapii endpoints reload <path>` re-reads that one
  file and rebuilds the MCP lists (`tools/list` etc.). It does not see new files or
  changes to `flapi.yaml` (connections, auth, ducklake, mcp). Restart for those.
- **SQL template edits need no reload.** The `.sql` file is read on every request.
- **A failed reload answers 400** `{"message":"Failed to reload endpoint configuration",
  "reason":"<file>: Failed to parse YAML: ..."}` and the old configuration keeps serving;
  404 means the endpoint is unknown (e.g. a file the server has not loaded yet).
- **Create, update and delete persist.** `POST /endpoints` (`flapii endpoints create`)
  writes `<template dir>/<slug>.yaml` (leading `-` dropped: `/orders` -> `orders.yaml`)
  and answers 201; an existing endpoint is 409. `PUT` deep-merges the body onto the YAML
  file and re-parses it (comments in that file are lost). `DELETE` removes the file.
  Do not also hand-write a file for an endpoint you create this way: the server writes
  its own, and two files with one `url-path` both load after a restart.
- **A `PUT` body must name the endpoint**: include `url-path` (or the `mcp-tool` /
  `mcp-resource` / `mcp-prompt` block). Without it the body is treated as the legacy
  camelCase form and fails with 500 `Missing required field in endpoint config`. Changing
  `url-path`, `method` or the tool name is 400 (delete and re-create); an endpoint whose
  YAML uses `{{include ...}}` is 409; a camelCase body applies in memory only
  (`"persisted": false`).
- **`--json-style` re-cases config documents only** (`endpoints list/get`, `mcp ...`,
  `cache list/get`, `project get`, `templates list`; default `camel`: `fieldIn`,
  `--json-style hyphen`: `field-in`). Data output (`metrics`, `cache audit`, `schema`,
  `health`, `templates expand/test`, `endpoints validate/parameters`, `cache gc`,
  `config filesystem/env`) is printed exactly as the server sent it: snake_case keys such
  as `rows_affected`, `503_cache`, `cache_unavailable`.
- **`flapii` output is JSON by default.** Add `-o table` for tables. Errors print
  `[status] statusText: <server reason>` and the JSON body.
- **`-c` is flapii's global `--config`**, so on `schema` write `--connection <name>` and
  `--table <name>` (there are no `-c` / `-t` short forms there).
- **URL variables:** `flapii` reads `-u`, else `FLAPI_BASE_URL`, else `FLAPI_URL` (what the
  plugin hook and MCP server use). Setting `FLAPI_URL` alone is enough.

## Start the server with the config service

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=$(openssl rand -hex 32)
flapi -c flapi.yaml --config-service          # reads the token from the environment
# or explicitly:
flapi -c flapi.yaml --config-service --config-service-token "$FLAPI_CONFIG_SERVICE_TOKEN"
```

Authenticate REST calls with either header:

```bash
curl -s -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" http://localhost:8080/api/v1/_config/endpoints
curl -s -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN"     http://localhost:8080/api/v1/_config/endpoints
```

A wrong or missing token is 401. MCP config tools read `X-Config-Token` first, then
`Authorization: Bearer`.
`GET /api/v1/_config/health` needs no token (storage paths are withheld without one).
`GET /config` (the whole parsed config) also needs the token.

A project to try it on. The checks run without a token:

```yaml
# flapi: endpoint group=live file=nations.yaml
url-path: /nations
template-source: nations.sql
connection: [customers]
# flapi: check GET /nations -> 200
# flapi: check GET /api/v1/_config/health -> 200
# flapi: check GET /api/v1/_config/endpoints -> 401
# flapi: check POST /api/v1/_config/endpoints/-nations/reload -> 401
```

```sql
-- flapi: template group=live file=nations.sql
SELECT c_nationkey AS nation, count(*) AS customers
FROM read_parquet('{{{ conn.path }}}')
GROUP BY ALL
ORDER BY nation
```

## Global options of `flapii`

| Option | Env | Default | Notes |
|---|---|---|---|
| `-u, --base-url <url>` | `FLAPI_BASE_URL`, then `FLAPI_URL` | `http://localhost:8080` | |
| `-t, --config-service-token <t>` | `FLAPI_CONFIG_SERVICE_TOKEN` | — | Sent as `Authorization: Bearer` |
| `--auth-token <t>` | `FLAPI_TOKEN` | — | Fallback if no config-service token |
| `-o, --output <json\|table>` | — | `json` | |
| `--json-style <camel\|hyphen>` | — | `camel` | Re-cases config documents only (see gotchas) |
| `--timeout <s>`, `--retries <n>` | `FLAPI_TIMEOUT`, `FLAPI_RETRIES` | | |
| `--insecure` | `FLAPI_VERIFY_TLS=false` | | Skip TLS verification |
| `--debug-http`, `-q, --quiet`, `-y, --yes` | | | |
| `-c, --config <path>` | `FLAPI_CONFIG` | `./flapi.yaml`, searched upward | Local file `flapii` reads client settings from; rarely needed |

`flapii` takes the **URL path** (`/customers`) or MCP name, and encodes the slug itself.

## Command map of `flapii`

| Group | Commands |
|---|---|
| (top) | `ping` (GET project), `health` (GET `_config/health`), `metrics [-e, --endpoint <path>]` |
| `config` | `show`, `validate [-f file] [-d dir]` (client-side check of YAML files; prefer `flapi --validate-config`), `env` (whitelisted env vars), `filesystem`, `log-level get\|set <level>\|list` |
| `project` | `get`, `init [name] [--force] [--skip-validation] [--no-examples] [--advanced] [--ai]` (local scaffold, see `flapi-setup`) |
| `endpoints` | `list`, `get <path>`, `parameters <path>`, `create -f <file>\|--stdin`, `update <path> -f <file>\|--stdin`, `delete <path> [--force]`, `validate <path> -f <file>`, `reload <path>`, `wizard [--ai] [--output-file f] [--dry-run] [--skip-validation]` |
| `templates` | `list`, `get <path>`, `update <path> -f <file>\|--stdin`, `expand <path> -p '<json>'\|--params-file f`, `test <path> -p '<json>'` (runs it, 10 rows) |
| `cache` | `list`, `get <path>`, `update <path> [--enabled] [--schedule 5m] [--table] [--schema] [-f file]`, `template <path>`, `update-template <path> -f <file>`, `refresh <path> [--full]`, `gc <path>`, `audit [path] [-o table]` |
| `schema` | `get [--connection c] [--table t] [--database d]`, `refresh [--connection c] [--force]`, `connections` (configured connections), `tables [--connection c]` (tables only, no views) |
| `mcp` | `tools list\|get <name>`, `resources list\|get <name>`, `prompts list\|get <name>` |

`flapii cache update` also lists `--ttl`, `--max-size` and `--strategy`; the server does not
support them. There is no TTL; use `--schedule`. `cache refresh --full` drops and rebuilds
the table (watermark erased); the old `-f/--force` is a hidden, deprecated normal refresh.

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=...            # same token as the server
flapii endpoints list -o table
flapii endpoints get /customer-lookup
flapii templates expand /customer-lookup -p '{"nation":"3"}'   # rendered SQL, no execution
flapii templates test /customer-lookup -p '{"nation":"3"}'     # executes it
flapii endpoints reload /customer-lookup
flapii cache refresh /products
flapii cache audit /products
flapii config log-level set debug
flapii mcp tools list
```

## REST routes and MCP config tools

Every `/api/v1/_config` route, the slug format, request bodies and the 20 `flapi_*`
tools with their arguments: [references/api.md](references/api.md).

The slug in a route is the URL path with `/` -> `-`, `-` -> `~1`, `~` -> `~0`:
`/customer-lookup` -> `-customer~1lookup`, `/customers/` -> `-customers-`. An MCP-only
tool, resource or prompt uses its `name` (`top_products`). The `GET-customers` form in
older docs is wrong (404).

## Hot reload: what is picked up

| You changed | What to do |
|---|---|
| A `.sql` endpoint template | Nothing. Next request uses it |
| A cache template (`template-file`) | Nothing for the file; run `flapii cache refresh <path>` to rebuild now |
| An existing endpoint YAML (request fields, validators, auth, cache keys, `mcp-*` blocks) | `flapii endpoints reload <path>`. REST, `tools/call` and `tools/list` use it at once |
| A new endpoint YAML file | **Restart**, or create it through `flapii endpoints create -f` instead of writing the file |
| A renamed or moved YAML file | Restart |
| `flapi.yaml` (connections, `template`, `auth`, `ducklake`, `mcp`, `rate_limit`) | **Restart**. There is no full-config reload: `DELETE /config` is 405 (`Allow: GET`) |
| Log level | `flapii config log-level set <debug\|info\|warning\|error>` (until restart; `warn` is accepted as `warning`) |

The scheduler reads each endpoint's `cache.schedule` on every tick, so a reloaded schedule
applies from the next tick.

## The plugin's validate-and-reload hook

After Claude edits a `.yaml`, `.yml` or `.sql` file under a directory with `flapi.yaml`
(`plugins/flapi/hooks/validate_and_reload.py`, PostToolUse on Edit/Write/MultiEdit):

1. If `flapi` is on `PATH` or `FLAPI_BIN` is set: `flapi -c <flapi.yaml> --validate-config
   --no-telemetry`. On failure the output goes back to Claude (exit 2).
2. If `FLAPI_CONFIG_SERVICE_TOKEN` is set and the server at `FLAPI_URL` (else
   `FLAPI_BASE_URL`) answers: for a
   `.sql` file it asks `POST /api/v1/_config/endpoints/by-template` which endpoints use it;
   for a YAML file it reads the top-level `url-path:`. Then it calls
   `POST /api/v1/_config/endpoints/{slug}/reload` for each.
3. A 404 from reload is taken as "not loaded yet", and Claude is told a new file needs
   `flapii endpoints create` or a restart (create writes its own `<slug>.yaml`; if the
   hand-written file has another name, restart instead). A 400 (YAML that does not
   parse) is reported back with the server's `reason` (exit 2).

Every missing precondition is a silent no-op. Limits that follow from the code:
an MCP-only YAML (no `url-path:`) is never reloaded by the hook (it reports the file as "not
a loaded endpoint"; run `flapii endpoints reload <name>`); keep `flapi` on `PATH` so step 1
catches invalid YAML before the server sees it.

## Schema introspection

```bash
flapii schema get                       # every attached database: tables, columns, types
flapii schema refresh
curl -s -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" http://localhost:8080/api/v1/_config/schema
```

The result is keyed by database alias, e.g. `{"nw":{"tables":{"Customers":{"columns":{"City":{"type":"VARCHAR","nullable":true}}}}}}`.
`flapii schema tables` (`?tables=true`) lists `name, schema, database, type, connection,
qualified_name` per table (no views; `--connection` filters); `flapii schema connections`
(`?connections=true`) lists the configured connections as `{name, type, status:"configured"}`.
MCP: `flapi_get_schema`, `flapi_refresh_schema`.

## Canonical workflow: change a live endpoint

1. Start the server with `--config-service` and your own token; export the same
   `FLAPI_CONFIG_SERVICE_TOKEN` (and `FLAPI_BASE_URL`/`FLAPI_URL` if not on 8080).
2. `flapii ping` to check URL and token.
3. `flapii endpoints get <path>` to see what is loaded.
4. Edit the YAML or SQL file on disk (the plugin hook validates and reloads it).
5. Without the hook: `flapi -c flapi.yaml --validate-config`, then
   `flapii endpoints reload <path>` for YAML (SQL needs nothing).
6. `flapii templates expand <path> -p '{...}'`, then `flapii templates test` or `curl` the
   endpoint.
7. New files (not created through `flapii endpoints create`) or `flapi.yaml` changed: restart.
8. Check the effect with `flapii metrics -e <path>` (`flapi-operate`).
