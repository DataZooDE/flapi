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
  (`--config-service-token` or `FLAPI_CONFIG_SERVICE_TOKEN`) is never echoed. Pass your own
  token of 8+ characters: the MCP config tools reject shorter ones
  (`Authentication token is too short`), while REST accepts them.
- **Hot reload is per endpoint YAML.** `flapii endpoints reload <path>` re-reads that one
  file. It does not see new files, changes to `flapi.yaml` (connections, auth, ducklake,
  mcp), or MCP tool metadata in `tools/list` (see `flapi-mcp`). Restart for those.
- **SQL template edits need no reload.** The `.sql` file is read on every request.
- **A failed reload answers 404 `Failed to reload endpoint configuration`**, the same code as
  an unknown path. The old configuration keeps serving. The reason (YAML or validator
  error) is only in the server log.
- **REST/`flapii` create, update and delete change memory only.** `flapii endpoints
  create|update|delete` are gone after a restart, and no YAML is written. The MCP tools
  `flapi_create_endpoint` and `flapi_delete_endpoint` do write/remove the YAML file.
  For anything you want to keep, edit the files and reload.
- **`flapii` rewrites JSON keys.** The default `--json-style camel` turns `rows_affected`
  into `rowsAffected`, but also map keys that are data: `/customer-lookup` becomes
  `/customerLookup`, table `Customers` becomes `customers`, status class `2xx` becomes
  `2Xx`. `--json-style hyphen` mangles them differently. Use `curl` when exact names
  matter.
- **`flapii` output is JSON by default.** Add `-o table` for tables.
- **`flapii schema tables` and `schema connections` do not work** in this release (a server
  query error, and an always-empty list). `flapii schema get` works. `-c` is flapii's
  global `--config`, so write `--connection <name>`, never `-c <name>`, on `schema`.
- **The plugin and `flapii` read different URL variables**: the plugin hook and MCP server use
  `FLAPI_URL`, `flapii` uses `FLAPI_BASE_URL` (or `-u`). Set both if the server is not on
  `http://localhost:8080`.

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

A wrong or missing token is 401. MCP config tools accept only `Authorization: Bearer`.
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
| `-u, --base-url <url>` | `FLAPI_BASE_URL` | `http://localhost:8080` | |
| `-t, --config-service-token <t>` | `FLAPI_CONFIG_SERVICE_TOKEN` | — | Sent as `Authorization: Bearer` |
| `--auth-token <t>` | `FLAPI_TOKEN` | — | Fallback if no config-service token |
| `-o, --output <json\|table>` | — | `json` | |
| `--json-style <camel\|hyphen>` | — | `camel` | Rewrites keys (see gotchas) |
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
| `cache` | `list`, `get <path>`, `update <path> [--enabled] [--schedule 5m] [--table] [--schema] [-f file]`, `template <path>`, `update-template <path> -f <file>`, `refresh <path> [--force]`, `gc <path>`, `audit [path]` |
| `schema` | `get [--connection c] [--table t] [--database d]`, `refresh [--connection c] [--force]`, `connections`, `tables [--connection c]` (see gotchas) |
| `mcp` | `tools list\|get <name>`, `resources list\|get <name>`, `prompts list\|get <name>` |

`flapii cache update` also lists `--ttl`, `--max-size` and `--strategy`; the server does not
support them. There is no TTL; use `--schedule`.

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
| An existing endpoint YAML (request fields, validators, auth, cache keys, `mcp-tool` body) | `flapii endpoints reload <path>`. REST and `tools/call` use it at once |
| An `mcp-tool` name, description or fields | Reload for execution; **restart** for `tools/list` |
| A new endpoint YAML file | **Restart** (or `flapii endpoints create`, memory only) |
| A renamed or moved YAML file | Restart |
| `flapi.yaml` (connections, `template`, `auth`, `ducklake`, `mcp`, `rate_limit`) | **Restart**. There is no full-config reload: `DELETE /config` returns 500 `Not implemented` |
| Log level | `flapii config log-level set <debug\|info\|warning\|error>` (until restart) |

The scheduler reads each endpoint's `cache.schedule` on every tick, so a reloaded schedule
applies from the next tick.

## The plugin's validate-and-reload hook

After Claude edits a `.yaml`, `.yml` or `.sql` file under a directory with `flapi.yaml`
(`plugins/flapi/hooks/validate_and_reload.py`, PostToolUse on Edit/Write/MultiEdit):

1. If `flapi` is on `PATH` or `FLAPI_BIN` is set: `flapi -c <flapi.yaml> --validate-config
   --no-telemetry`. On failure the output goes back to Claude (exit 2).
2. If `FLAPI_CONFIG_SERVICE_TOKEN` is set and the server at `FLAPI_URL` answers: for a
   `.sql` file it asks `POST /api/v1/_config/endpoints/by-template` which endpoints use it;
   for a YAML file it reads the top-level `url-path:`. Then it calls
   `POST /api/v1/_config/endpoints/{slug}/reload` for each.
3. A 404 from reload is taken as "not loaded yet", and Claude is told a new file needs
   `flapii endpoints create` or a restart.

Every missing precondition is a silent no-op. Limits that follow from the code:
an MCP-only YAML (no `url-path:`) is never reloaded by the hook (it reports the file as "not
a loaded endpoint"; run `flapii endpoints reload <name>`); and since a failed reload is also
a 404, run with `flapi` on `PATH` so step 1 catches invalid YAML.

## Schema introspection

```bash
flapii schema get                       # every attached database: tables, columns, types
flapii schema refresh
curl -s -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" http://localhost:8080/api/v1/_config/schema
```

The result is keyed by database alias, e.g. `{"nw":{"tables":{"Customers":{"columns":{"City":{"type":"VARCHAR","nullable":true}}}}}}`
(raw curl keeps the real names). MCP: `flapi_get_schema`, `flapi_refresh_schema`.

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
7. MCP metadata, new files or `flapi.yaml` changed: restart.
8. Check the effect with `flapii metrics -e <path>` (`flapi-operate`).
