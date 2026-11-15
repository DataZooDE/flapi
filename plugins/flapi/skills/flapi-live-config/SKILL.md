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
  file and rebuilds the MCP lists (`tools/list` etc.). `flapii endpoints reload --file
  <yaml>` (`POST /api/v1/_config/endpoints/reload-file`) does the same by FILE and also LOADS
  a new hand-written file in `template.path`; for a DELETED file it unloads the endpoints
  that file declared (200 `"status":"removed"`, `"removed":["GET /x"]`). A reload that
  changes `cache.table` makes the endpoint 503 `cache_not_built` until a refresh (or
  serves at once if that table exists) and drops the old table from `/health`. Changes to
  `flapi.yaml` (connections, auth, ducklake, mcp) need a restart.
- **Several endpoints can share a url-path** (`GET /orders` + `POST /orders`). Every slug
  route then needs `?method=` (`flapii ... --method POST`); without it the answer is 400
  `Ambiguous: /orders is served by GET (...), POST (...); specify ?method=GET|POST`. The
  endpoint list keys them `"GET /orders"` / `"POST /orders"`. The `/cache/*` routes (and
  `flapi cache refresh --endpoint`) count only endpoints WITH a cache: a cached GET beside an
  uncached POST needs no `?method=` there. Two files with the same MCP
  tool/resource/prompt name fail the load, naming both.
- **SQL template edits need no reload.** The `.sql` file is read on every request.
- **A failed reload answers 400** `{"message":"Failed to reload endpoint configuration",
  "reason":"<file>: Failed to parse YAML: ..."}` and the old configuration keeps serving;
  404 means the endpoint is unknown (a file the server has not loaded yet: use `reload --file`).
- **Create, update and delete persist.** `POST /endpoints` (`flapii endpoints create`)
  writes `<template dir>/<slug>.yaml` (leading `-` dropped: `/orders` -> `orders.yaml`)
  and answers 201; an existing endpoint is 409. `PUT` deep-merges the body onto the YAML
  file and re-parses it (comments in that file are lost). `DELETE` removes the file.
  Do not also hand-write a file for an endpoint you create this way: the server writes
  its own, and two files with one `url-path` + method stop the next startup: `Duplicate
  endpoint GET /x: declared in a.yaml and b.yaml` (reload and create refuse it with 409).
  Routes are compared by shape and methods canonically (`get` is GET), so `/items/:id` and
  `/items/:item_id` are the same endpoint (`... (same route as /items/:id)`).
- **Overlapping routes: the most specific answers.** `/items/special` beside `/items/:id`
  (or `/a/:x/b` beside `/:z/:x/b`) both match some URLs; the one with a literal where the
  other has a `:param`, comparing segments left to right, answers them, whatever the file
  order or reload history. Auth, rate limiting and the handler always agree on it. When
  their effective auth differs in ANY way - either direction, other type or roles, or
  another identity source (users/passwords, jwt-secret, jwt-issuer, OIDC, AWS secret) -
  startup, reload, create and update refuse it (409 `Overlapping routes with different
  auth: GET /items/special (open) takes precedence over GET /items/:id (basic, roles
  [admin]) for /items/special (auth differs in: enabled); /items/:id still serves every
  other URL its pattern matches ...`) unless the more specific one sets
  `allow-shadowing: true` (then a WARNING, which the hook passes to you as context).
  Same auth: an INFO line. An update that changes either side's auth is 409 too and
  leaves the file as it was. A literal route protects one path string, not a record
  (`/customers/07` still reaches `/customers/:id`): filter rows by `auth.*` in the
  general endpoint, or give both the same auth.
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
| `-q, --quiet` | | | No spinners or progress/success lines; the data a command prints and a failure (one line: `[status] reason`) still come out, exit code 1 on failure |
| `--debug-http`, `-y, --yes` | | | |
| `-V, --version` | | | Prints the `flapii` package version |
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
curl -s -X POST -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" -d '{"parameters":{}}' \
  'http://localhost:8080/api/v1/_config/endpoints/-customer~1lookup/template/expand?validate_only=1'
                                                               # parsed + bound, not run
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
| An existing endpoint YAML (request fields, validators, auth, cache keys, `mcp-*` blocks) | `flapii endpoints reload <path> [--method M]` or `flapii endpoints reload --file <yaml>`. REST, `tools/call` and `tools/list` use it at once |
| A new endpoint YAML file in `template.path` | `flapii endpoints reload --file <yaml>` (loads it; `endpoints create -f` of a file already there answers 409 and points here) |
| A renamed or moved YAML file | Restart |
| `flapi.yaml` (connections, `template`, `auth`, `ducklake`, `mcp`, `rate_limit`) | **Restart**. There is no full-config reload: `DELETE /config` is 405 (`Allow: GET`) |
| Log level | `flapii config log-level set <debug\|info\|warning\|error>` (until restart; `warn` is accepted as `warning`) |

The scheduler reads each endpoint's `cache.schedule` on every tick, so a reloaded schedule
applies from the next tick.

## The plugin's validate-and-reload hook

After Claude edits a `.yaml`, `.yml` or `.sql` file under a directory with `flapi.yaml`, or
under the `template.path` of the config `FLAPI_CONFIG` names (a `../sqls` shared by a writer
and a replica) (`plugins/flapi/hooks/validate_and_reload.py`, PostToolUse on Edit/Write/MultiEdit, and on Bash for step 6):

1. If `flapi` is on `PATH` or `FLAPI_BIN` is set: `flapi -c <flapi.yaml> --validate-config
   --no-telemetry`. On failure the output goes back to Claude (exit 2). `SECURITY WARNINGS`
   lines it prints, and `Overlapping routes with different auth` warnings (an
   `allow-shadowing` split), are passed to Claude even when validation passes.
2. `flapi.yaml` itself: Claude is told to restart the server (it is not hot-reloaded). A file
   outside `template.path` is only validated (an endpoint YAML there is reported as not loaded).
3. If `FLAPI_CONFIG_SERVICE_TOKEN` is set and the server at `FLAPI_URL` (else
   `FLAPI_BASE_URL`) answers: first, WITHOUT the token, `GET /doc.yaml` must name this
   `flapi.yaml`'s `project-name` - otherwise the hook names the URL and project it found and
   sends nothing there. Then a server running from a packed bundle (`"bundled": true` in
   `GET /api/v1/_config/project`), one whose `template-path` is another directory, or one
   started before `flapi.yaml` changed when its `ducklake.enabled/alias/read-only`,
   `auth.enabled` or `mcp.enabled` differ from the file ("restart the server first"), is not
   reloaded - the hook says why. Otherwise a YAML file is reloaded BY FILE with
   `POST /api/v1/_config/endpoints/reload-file` - which also loads a new file, handles
   MCP-only files, and changes exactly that file's endpoint when another file shares the
   url-path. For a `.sql` file it asks `POST /api/v1/_config/endpoints/by-template` (serving
   template or cache `template-file`) and reloads each declaring YAML by file.
4. It then VALIDATES every affected template, running nothing: the serving template with
   `?validate_only=1` twice - no parameters, then a sample value per typed request field
   (`int` -> `min` or 1, `number` -> `min` or 1.0, `string` -> a value matching its regex,
   `enum` -> first value, `date` -> today, `boolean` -> true), so SQL inside `{{#params.x}}`
   is checked too - and a cache `template-file` with `?target=cache&validate_only=1` (live
   watermark, parsed per statement, bare-SELECT rule, bound in a rolled-back transaction).
   Parse/bind errors and reload failures (400/409 with the server's reason) go back to Claude
   (exit 2); the message names the parameter sets checked. A parameter used inside
   `AT (VERSION => ...)` / `AT (TIMESTAMP => ...)` gets no sample (an invented snapshot does
   not exist). A cached endpoint whose cache table does not exist yet (on the no-parameter
   run) is a note: "cache not built yet; run `flapii cache refresh <path>`"; a failing
   sample run is reported as an error, never as "not built". A dropped watermark during
   validation is a note: the next refresh would load the full source.
5. A 401/403 is reported as a rejected `FLAPI_CONFIG_SERVICE_TOKEN` (exit 2); no server
   running is a silent no-op, as is a missing token.
6. PostToolUse on Bash: a command that deleted an endpoint YAML under `template.path` (`rm`,
   `git rm`, `unlink`, `trash`, `mv` elsewhere) -> the file goes to `reload-file`, which
   unloads what it declared (`200 {"status":"removed","removed":[...]}`), and the hook
   reports them. The same happens for an Edit/Write whose YAML no longer exists. A 404 means
   nothing was loaded from that file (an older server also answers 404 and keeps serving the
   file's endpoints until it restarts).

Keep `flapi` on `PATH` so step 1 catches invalid YAML before the server sees it.

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
   `flapii endpoints reload --file <yaml>` for YAML, new files included (SQL needs nothing).
6. `flapii templates expand <path> -p '{...}'`, then `flapii templates test` or `curl` the
   endpoint.
7. `flapi.yaml` changed: restart.
8. Check the effect with `flapii metrics -e <path>` (`flapi-operate`).
