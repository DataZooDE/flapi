# Managing flAPI from the command line

**Goal:** inspect and change a running flAPI server from your terminal with
`flapii`: endpoints, SQL templates, caches, schema, log level and metrics.
Then the subcommands of the `flapi` binary itself, and a CI job that uses both.

There are two commands, and both come from one package:

| Command | What it is | Talks to |
|---|---|---|
| `flapi` | The server. Also validates configs, packs bundles, refreshes caches once, installs extensions. | Your config files and DuckDB |
| `flapii` | A client for the server's **config service** (`/api/v1/_config/*`). | A running `flapi` over HTTP |

**Prerequisites**

- `pip install flapi-io` (installs both `flapi` and `flapii`), or
  `uvx --from flapi-io flapii --help` to try it without installing.
- Two terminals: one runs the server in the foreground, the other runs `flapii`.

All output below is real, from flAPI 26.11.25 (flapii 26.6.13). Long paths are
shortened to `.../demo`, and the progress lines flapii prints to stderr are
left out unless they are the point.

---

## 1. Create a project

Work in a directory that will hold the project and, next to it, your drafts:

```bash
mkdir -p ~/flapi-work && cd ~/flapi-work
flapii project init demo
```

```text
Setting up flapi project: demo
✔ ✅ Created directories
✔ ✅ Created flapi.yaml
✔ ✅ Created sample.yaml
...
✔ ✓ Configuration is valid
```

You get `demo/flapi.yaml`, an endpoint `GET /sample` (`sqls/sample.yaml` +
`sqls/sample.sql`) over `data/sample.csv`, and `common/auth.yaml` /
`common/rate-limit.yaml` to include later. `project init` runs locally and
needs no server.

| Option | Effect |
|---|---|
| `-f`, `--force` | Overwrite existing files. Without it, existing files are skipped (`flapi.yaml already exists, skipping`), so rerunning is safe. |
| `--skip-validation` | Skip the check at the end |
| `--no-examples`, `--advanced`, `--ai` | Listed by `--help`, **not implemented** in 26.6.13: the output is the same as without them |

It exits `0` on success, `2` on a permission error and `1` on anything else,
including an invalid project name.

To show the cache commands later, give the project a DuckLake catalog and one
cached endpoint. Add the `ducklake:` block to `demo/flapi.yaml`, which then
reads:

<!-- flapi: config group=demo -->
```yaml
# demo/flapi.yaml
project-name: my-flapi-project
project-description: Example flapi configuration

template:
  path: ./sqls
  environment-whitelist:
    - '^(API_PASSWORD|JWT_SECRET)$'

connections:
  sample-data:
    properties:
      path: './data/sample.csv'

ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
```

Then add three files to `demo/sqls/`: the endpoint, the SQL that serves it, and
the SQL that fills the cache.

<!-- flapi: endpoint group=demo file=sample_cached.yaml -->
```yaml
# demo/sqls/sample_cached.yaml
url-path: /sample-cached
method: GET
template-source: sample_cached.sql
connection: [sample-data]
cache:
  enabled: true
  table: sample_cache
  schema: analytics
  schedule: 1h
  template-file: sample_cache.sql
```

<!-- flapi: template group=demo file=sample_cached.sql -->
```sql
-- demo/sqls/sample_cached.sql
SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
```

<!-- flapi: template group=demo file=sample_cache.sql -->
```sql
-- demo/sqls/sample_cache.sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv_auto('{{{ conn.path }}}')
```

How the cache works is explained in [Caching](./caching.md).

## 2. Start the server with the config service

The config service is **off** by default. In the **first terminal**, turn it
on and give it a token. The server stays in the foreground; stop it with
Ctrl+C.

```bash
cd ~/flapi-work/demo
flapi -c flapi.yaml --config-service --config-service-token demo-token-0123456789
```

```text
============================================================
    CONFIG SERVICE ENABLED
============================================================
    Token: (supplied via --config-service-token or FLAPI_CONFIG_SERVICE_TOKEN)
============================================================
```

The server also reads the token from `FLAPI_CONFIG_SERVICE_TOKEN`. Without
either, it generates one and prints it in that banner. It listens on port 8080
unless you pass `-p`, `--port`. Security for real deployments is in step 13.

## 3. Point flapii at the server

Everything from here on runs in the **second terminal**:

```bash
cd ~/flapi-work/demo
export FLAPI_CONFIG_SERVICE_TOKEN=demo-token-0123456789
export FLAPI_BASE_URL=http://localhost:8080     # change the port if you used -p
flapii ping
```

```text
✔ ✓ Connected to flapi server
Name: my-flapi-project
Description: Example flapi configuration
Base URL: http://localhost:8080
```

The config service is plain HTTP; flapii is one client of it. Every route
except `health` needs the token:

```bash
curl http://localhost:8080/api/v1/_config/endpoints
curl -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" http://localhost:8080/api/v1/_config/endpoints
curl http://localhost:8080/api/v1/_config/health
```

```text
{"error":"Unauthorized: missing or invalid config-service token (send it as X-Config-Token or Authorization: Bearer)"}
{"/sample-cached":{...},"/sample":{...}}
{"status":"healthy","server":{"version":"26.11.25","name":"flapi"},"endpoints":{"count":2},...}
```

<!-- flapi: check group=demo GET /api/v1/_config/endpoints -> 401 -->
<!-- flapi: check group=demo GET /api/v1/_config/endpoints header=X-Config-Token:skill-check-token -> 200 contains "/sample-cached" -->
<!-- flapi: check group=demo GET /api/v1/_config/health -> 200 -->

`flapii health` returns that health report. The other flapii commands need
the token.

**Where flapii gets the token**, first match wins:

1. `-t`, `--config-service-token <token>`
2. the environment variable `FLAPI_CONFIG_SERVICE_TOKEN`
3. `--auth-token <token>`
4. the environment variable `FLAPI_TOKEN`

The environment variable beats `--auth-token` (it is the default value of
`-t`), so with `FLAPI_CONFIG_SERVICE_TOKEN` exported, `--auth-token` is
ignored. Use `-t` to override. flapii sends the token as both
`Authorization: Bearer` and `X-Config-Token`, and never reads it from a file.

**Where flapii gets the URL**, first match wins:

1. `-u`, `--base-url <url>`
2. `FLAPI_BASE_URL`, then `FLAPI_URL`
3. `cli.http.base-url` in a `flapi.yaml` flapii finds: the file given by
   `-c`, `--config`, else `$FLAPI_CONFIG`, else `flapi.yaml` in the current
   directory or a parent, else `~/.config/flapii/flapi.yaml`
4. `http://localhost:8080`

Keep a default URL in the per-user file, not in your project's `flapi.yaml`.
The server warns about a `cli:` key in its own config:
``Unknown top-level configuration key `cli` is ignored; nothing reads it.``

```bash
mkdir -p ~/.config/flapii
cat > ~/.config/flapii/flapi.yaml <<'EOF'
cli:
  http:
    base-url: http://localhost:8080
    timeout-seconds: 30
EOF
```

flapii only reaches that file when there is no `flapi.yaml` in the current
directory or its parents. `~/.flapi/config.json` holds only the Gemini key of
the endpoint wizard; flapii reads no URL or token from it.

Other global options: `--timeout <seconds>` (default 10, env `FLAPI_TIMEOUT`),
`--retries <count>` (default 2, reads only, env `FLAPI_RETRIES`), `--insecure`
(skip TLS verification), `--debug-http` (log requests, token masked), `-q`,
`--quiet`, and `-y`, `--yes`.

## 4. Endpoints

### List and inspect

```bash
flapii endpoints list -o table
```

```text
┌─────────────────────────┬────────┬───────────────┬────────────┬───────────────┬────────┐
│ Path                    │ Method │ Type          │ Operation  │ Connection    │ Cache  │
├─────────────────────────┼────────┼───────────────┼────────────┼───────────────┼────────┤
│ /sample-cached          │ GET    │ REST          │ Read       │ sample-data   │ ✓      │
│ /sample                 │ GET    │ REST          │ Read       │ sample-data   │ ✗      │
└─────────────────────────┴────────┴───────────────┴────────────┴───────────────┴────────┘
```

```bash
flapii endpoints get /sample
flapii endpoints parameters /sample -o table
```

`endpoints get` returns the endpoint as the server parsed it, defaults
included (`withPagination`, `rateLimit`, `auth.inherited`, ...). When a GET
and a POST share a path, pick one with `--method POST`; the same option
exists on `update`, `delete`, `validate`, `reload` and the template and cache
commands.

### Create one from a file

Write the new endpoint as drafts in `~/flapi-work/drafts/`, outside the
project, so the server does not load a half-written file:

```yaml
# ~/flapi-work/drafts/by-category.yaml
url-path: /by-category
method: GET
request:
  - field-name: category
    field-in: query
    required: true
    validators:
      - type: enum
        allowedValues: [tools, toys, misc]
template-source: by_category.sql
connection: [sample-data]
```

```sql
-- ~/flapi-work/drafts/by_category.sql
SELECT id, name, price FROM read_csv_auto('{{{ conn.path }}}')
WHERE category = '{{{ params.category }}}'
```

`endpoints validate` sends the YAML to the server, which parses it as it would
at startup. The argument is the endpoint's URL path; the YAML comes from
`--file`:

```bash
flapii endpoints validate /by-category --file ../drafts/by-category.yaml
```

```json
{
  "valid": true,
  "errors": [],
  "warnings": [
    "Template file does not exist: .../demo/sqls/by_category.sql"
  ]
}
```

The warning is expected: the server looks for the template in its own
`template.path`. Create the endpoint, then upload the template:

```bash
flapii endpoints create --file ../drafts/by-category.yaml
flapii templates update /by-category --file ../drafts/by_category.sql
curl 'http://localhost:8080/by-category?category=toys'
```

```text
{"data":[{"id":3,"name":"Gizmo","price":4.25},{"id":4,"name":"Doohickey","price":12}],"next":"","total_count":2}
```

`create` writes `sqls/by-category.yaml` on the server's disk and registers the
endpoint at once; `templates update` writes `sqls/by_category.sql`. Between the
two, requests to the new endpoint fail with `500`. `--file` takes YAML or JSON;
`--stdin` reads the payload from a pipe.

### Update

Change the draft (drop `misc` from `allowedValues`) and send it:

```bash
sed -i 's/\[tools, toys, misc\]/[tools, toys]/' ../drafts/by-category.yaml
flapii endpoints update /by-category --file ../drafts/by-category.yaml
curl 'http://localhost:8080/by-category?category=misc'
```

```text
{"errors":[{"message":"Invalid enum value","field":"category"}]}
```

`update` replaces the endpoint's whole config and writes its YAML file. It
patches the file in place where it can, keeping comments; a file it cannot
patch, and every file `create` writes, comes out in flow style
(`{url-path: /by-category, method: GET, request: [...]}`) with no comments.

### Hand edits and reload

flAPI re-reads a **SQL template** on the next request, so an edit to
`sqls/*.sql` (or a `git pull`) is live at once:

```bash
sed -i 's/SELECT \*/SELECT id, name/' sqls/sample.sql
curl 'http://localhost:8080/sample?id=2'
```

```text
{"data":[{"id":2,"name":"Gadget"}],"next":"","total_count":1}
```

An **endpoint YAML** is read only at startup and on reload:

```bash
flapii endpoints reload /sample
```

A new hand-written file needs `--file`, because the server does not know its
path yet. Add a second endpoint by hand:

```yaml
# ~/flapi-work/demo/sqls/cheap.yaml
url-path: /cheap
method: GET
template-source: cheap.sql
connection: [sample-data]
```

```sql
-- ~/flapi-work/demo/sqls/cheap.sql
SELECT name, price FROM read_csv_auto('{{{ conn.path }}}')
WHERE price < 10
```

```bash
flapii endpoints reload --file sqls/cheap.yaml
curl http://localhost:8080/cheap
```

```text
✔ ✓ Endpoint configuration reloaded: GET /cheap
{"data":[{"name":"Widget","price":9.99},{"name":"Gizmo","price":4.25},{"name":"Thingamajig","price":7.75}],"next":"","total_count":3}
```

`flapii endpoints reload /cheap` before that answers
`[404] Not Found: Endpoint not found: /cheap`.

### Delete

```bash
flapii endpoints delete /cheap --force
```

Without `--force` (or the global `--yes`) it stops with
`Use --force or --yes to delete endpoint /cheap` and exit code `1`. Delete
unregisters the endpoint and removes `cheap.yaml`; `cheap.sql` stays on disk.

`flapii endpoints wizard` creates an endpoint interactively (`--ai` asks
Gemini to draft it; `--output-file` writes the YAML instead). Not for scripts.

## 5. Templates

```bash
flapii templates list
flapii templates get /by-category
```

**Expand** renders the template without running it, so you see the SQL and
which values flAPI binds as prepared-statement parameters:

```bash
flapii templates expand /by-category --params '{"category":"toys"}'
```

```json
{
  "bindings": [
    { "value": "toys", "type": "varchar", "name": "category", "position": 1 }
  ],
  "expanded": "SELECT id, name, price FROM read_csv_auto('.../demo/data/sample.csv')\nWHERE category = ?\n"
}
```

`expand` runs no validators: `{"category":"cars"}` expands just the same.
`--cache` expands the cache populate template instead, with the live
watermark, as the next refresh would run it.

**Test** validates the parameters, runs the query and returns the rows:

```bash
flapii templates test /by-category --params '{"category":"toys"}'
```

```json
{
  "columns": ["name", "price", "id"],
  "rows": [
    { "name": "Gizmo", "price": 4.25, "id": 3 },
    { "name": "Doohickey", "price": 12, "id": 4 }
  ],
  "success": true
}
```

A failing test exits `1` with the reason, for example
`[400] Bad Request: Parameter validation failed` or a DuckDB error such as
`Binder Error: Referenced column "nme" not found in FROM clause!`.
`--params-file <file>` reads the JSON from a file. `templates update` checks
the Mustache syntax before writing; a broken template is refused.

## 6. Caches

```bash
flapii cache get /sample-cached
```

```json
{
  "templateFile": ".../demo/sqls/sample_cache.sql",
  "schedule": "1h",
  "schema": "analytics",
  "table": "sample_cache",
  "enabled": true
}
```

`flapii cache list` returns the same for every endpoint, uncached ones
included (`"enabled": false`). Use its JSON: the table view shows `N/A` for
the schedule and table.

**Refresh now**, or rebuild from scratch with `--full` (drops the table and
erases the incremental watermark):

```bash
flapii cache refresh /sample-cached
flapii cache refresh /sample-cached --full
```

```json
{
  "snapshot_id": "8",
  "mode": "full",
  "status": "refreshed",
  "rows_affected": 5,
  "duration_ms": 18,
  "full": true,
  "endpoint": "/sample-cached"
}
```

On an endpoint without a cache, refresh exits `1` with
`[400] Bad Request: Cache is not enabled for this endpoint`.

**Audit** lists the refreshes, newest first (warm-up, scheduled and manual):

```bash
flapii cache audit /sample-cached -o table
```

```text
┌────────────────────────────┬──────┬─────────┬─────────────┬──────┬──────────┬──────────────────────────────┐
│ Time                       │ Mode │ Status  │ Duration Ms │ Rows │ Snapshot │ Message                      │
├────────────────────────────┼──────┼─────────┼─────────────┼──────┼──────────┼──────────────────────────────┤
│ 2026-11-25T12:40:34.388000 │ full │ success │ 18          │ 5    │ 8        │ Cache refreshed successfully │
│ 2026-11-25T12:40:34.132000 │ full │ success │ 17          │ 5    │ 6        │ Cache refreshed successfully │
│ 2026-11-25T12:39:42.963000 │ full │ success │ 32          │ 5    │ 4        │ Cache refreshed successfully │
└────────────────────────────┴──────┴─────────┴─────────────┴──────┴──────────┴──────────────────────────────┘
```

Without a path, `cache audit` covers every cache; `--table analytics.sample_cache`
narrows it to one table.

**Garbage-collect** old snapshots under the retention policy:

```bash
flapii cache gc /sample-cached
```

```json
{
  "message": "no retention policy configured; nothing expired (set cache.retention or ducklake.retention)",
  "status": "no_policy",
  "expired_snapshots": 0,
  "files_deleted": 0,
  "bytes_freed": 0
}
```

Also there: `cache template` / `cache update-template` (the populate SQL),
`cache update` (the cache block), `cache compact` (DuckLake compaction now) and
`cache drop-table <table>` (an orphaned cache table).

## 7. Schema introspection

```bash
flapii schema connections
flapii schema tables --connection cache
```

```json
{ "connections": [ { "status": "configured", "type": "csv", "name": "sample-data" } ] }
{
  "tables": [
    { "qualified_name": "cache.analytics.sample_cache", "database": "cache", "schema": "analytics", "type": "table", "name": "sample_cache", "connection": "" },
    { "qualified_name": "cache.audit.sync_events", "database": "cache", "schema": "audit", "type": "table", "name": "sync_events", "connection": "" }
  ]
}
```

`--connection` filters by **attached database name**: the DuckLake alias
(`cache`) or a database a connection's `init` attaches. A connection that only
holds a file path, like `sample-data`, attaches nothing and lists no tables.

`flapii schema get` returns every database, table and column. Its
`--connection`, `--table` and `-d`, `--database` options filter nothing in
26.11.25; filter with `jq`:

```bash
flapii schema get | jq '.cache.tables.sample_cache.columns | keys'
```

```json
["category", "id", "name", "price"]
```

`schema refresh` re-reads the schema after you attach something new.

## 8. Runtime log level

```bash
flapii config log-level set debug
```

```text
New level: debug
Note: Log level will reset on server restart
```

`flapii config log-level list` shows the levels (`debug`, `info`, `warning`
or `warn`, `error`); anything else exits `1`. `flapii config log-level get`
reports what the last `set` stored, not what the server started with: a server
started with `--log-level warning` reports `Level: info` until the first `set`.

## 9. Metrics

Make a few requests first, including one the validator rejects:

```bash
for id in 1 2 3; do curl -s "http://localhost:8080/sample?id=$id" > /dev/null; done
curl -s 'http://localhost:8080/sample?id=0'
curl -s http://localhost:8080/sample-cached > /dev/null
flapii -o table metrics
```

```text
{"errors":[{"message":"Integer is less than the minimum allowed value (min 1)","field":"id"}]}

📈 Requests
════════════════════════════════════════════════════════════
11 requests, 0 server errors, 0 cache-not-ready 503s · rejected: 1 × 401, 0 × 403, 0 × 429
┌────────────────────┬──────────┬───────────────┬──────────┬────────┬────────┬────────┬────────┬────────┐
│ Endpoint           │ Requests │ Errors        │ Rejected │ Avg Ms │ P50 Ms │ P95 Ms │ P99 Ms │ Max Ms │
├────────────────────┼──────────┼───────────────┼──────────┼────────┼────────┼────────┼────────┼────────┤
│ GET /sample-cached │ 2        │ 0 4xx / 0 5xx │ 0        │ 9.66   │ ~10    │ ~11    │ ~11    │ 11     │
│ GET /cheap         │ 1        │ 0 4xx / 0 5xx │ 0        │ 5.33   │ ~5.33  │ ~5.33  │ ~5.33  │ 5.33   │
│ GET /sample        │ 6        │ 1 4xx / 0 5xx │ 0        │ 3.44   │ ~4.29  │ ~4.29  │ ~4.29  │ 4.29   │
│ GET /by-category   │ 2        │ 1 4xx / 0 5xx │ 0        │ 2.16   │ ~0.16  │ ~4.19  │ ~4.19  │ 4.19   │
└────────────────────┴──────────┴───────────────┴──────────┴────────┴────────┴────────┴────────┴────────┘
Percentiles are interpolated within histogram buckets and capped at the observed max.

🗄️  Caches
════════════════════════════════════════════════════════════
┌──────────────────────────────┬───────┬──────┬─────────────┬─────────┬──────┬────────┬──────────────┬───────┐
│ Cache                        │ State │ Mode │ Last Status │ Last Ms │ Rows │ Avg Ms │ Refreshes    │ Age S │
├──────────────────────────────┼───────┼──────┼─────────────┼─────────┼──────┼────────┼──────────────┼───────┤
│ cache.analytics.sample_cache │ ready │ full │ success     │ 33      │ 5    │ 33     │ 1 (0 failed) │ 22    │
└──────────────────────────────┴───────┴──────┴─────────────┴─────────┴──────┴────────┴──────────────┴───────┘
```

Counts run since server start and include the requests of earlier steps.
`-e`, `--endpoint <path>` narrows to one route or MCP tool; the rejection
counts (401/403/429) stay server-wide. The metrics are JSON behind the config
token (`GET /api/v1/_config/metrics`); there is no Prometheus endpoint. For
traces, see [Observability](./observability.md).

## 10. MCP declarations

`flapii mcp tools list`, `flapii mcp resources list` and
`flapii mcp prompts list` (and `get <name>`) show the `mcp-tool`,
`mcp-resource` and `mcp-prompt` blocks your endpoint YAMLs declare. The demo
declares none, so they return `[]`. See [MCP tools](./mcp-tools.md).

## 11. Output for scripts

- `-o json` (the default) or `-o table`. It is a global option, so it works on
  any command (`flapii -o table metrics`); `endpoints list`, `endpoints get`,
  `endpoints parameters`, `endpoints validate` and `cache audit` also take
  `--output` after the command.
- Progress lines go to **stderr**, the JSON to **stdout**, so `jq` works:

  ```bash
  flapii endpoints list | jq -r 'keys[]'
  ```

  ```text
  /by-category
  /sample
  /sample-cached
  ```

- `-q`, `--quiet` drops the progress lines; a failure still prints one line.
  It also hides the `Use --force` hint of `endpoints delete`.
- `--json-style hyphen` returns keys as the YAML spells them (`url-path`)
  instead of camelCase.

**Exit codes.** `0` on success, `1` on any failure: an HTTP error, an
unreachable server, a failed validation or test, an unknown command or option.
The message says which:

| Message | Meaning |
|---|---|
| `The config service needs a token. ... No token was sent (HTTP 401).` | flapii found no token |
| `The config service rejected the token flapii sent.` | the token differs from the server's |
| `Cannot reach flAPI at http://localhost:8080. Is it running?` | wrong URL, or no server |
| `The config service may be disabled on this server.` (with a 404) | server started without `--config-service` |
| `Request timed out after 10s; the server may still be working` | raise `--timeout`; a refresh keeps running |

## 12. The server's own subcommands

These run the `flapi` binary once and exit. None needs the config service.
Stop the server in the first terminal (Ctrl+C) before trying them.

### Validate a config

```bash
flapi -c flapi.yaml --validate-config
```

```text
✓ Configuration file loaded successfully
✓ Parsed 3 endpoint(s)
✓ Validation PASSED
```

It parses every YAML and checks every template's Mustache syntax, and exits
`1` on an error (`ERROR: Template file does not exist: .../sqls/missing.sql`).
It opens **no data source** and runs no SQL, so a misspelled column passes;
the CI job in step 14 catches those. `flapii config validate` is a weaker,
local check of the endpoint files.

### Refresh caches once: `flapi cache refresh`

For a scheduled job (cron, Airflow, a Cloud Run Job after `dbt build`) when no
server holds the catalog:

```bash
flapi cache refresh -c flapi.yaml --endpoint /sample-cached
```

```json
{"failed":0,"refreshed":[{"rows_affected":5,"duration_ms":20,"status":"ok","table":"sample_cache","schema":"analytics","endpoint":"/sample-cached"}]}
```

`--all` refreshes every cache, `--full` rebuilds from scratch. Exit `0` if all
refreshed, `1` if one failed, `2` for a usage error (no `--endpoint` or
`--all`, an endpoint without a cache).

It opens the DuckLake catalog for writing. A file catalog (`metadata-path`
ending in `.ducklake`) is locked by the server that has it open, so with the
server running you get, with exit `1`:

```json
{"error":"a running flapi holds the DuckLake catalog lock; refresh through the server: flapii cache refresh /sample-cached (if the holders are read-only replicas, stop the replicas, or use a Postgres/SQLite/MySQL DuckLake catalog, which a writer and replicas can share)"}
```

A server is running: use `flapii cache refresh <path>`. No server, or
read-only replicas on a Postgres/SQLite/MySQL catalog: use
`flapi cache refresh --endpoint <path>`.

### Pre-install extensions: `flapi extensions install`

Install the DuckDB extensions a config needs at image build time, into a
directory **outside** the project:

```bash
flapi extensions install --config flapi.yaml --dir ../flapi-extensions
```

It prints one `✓` line per extension (`httpfs`, `ducklake`, `postgres`, ...;
about 200 MB in this run, which `pack` would otherwise bundle). Set `FLAPI_EXTENSION_DIRECTORY` to the same directory at
runtime. Details: [CLI_REFERENCE §3](../CLI_REFERENCE.md#3-self-packaging-subcommands).

### Pack a single binary: `flapi pack`, `info`, `unpack`

`pack` bundles **every file** under `--in`, except the DuckLake catalog. Run it
from outside the project, and exclude scratch directories:

```bash
cd ~/flapi-work
flapi pack --in ./demo --out demo-api --exclude 'extensions/**'
./demo-api info
```

```text
Excluded 2 file(s) (DuckLake catalog / --exclude), e.g. extensions/x/a.duckdb_extension
Packed 13 entries (4141 bytes) into demo-api
Entries (13):
  .gitignore (248 bytes)
  common/auth.yaml (514 bytes)
  ...
```

`unpack --to <dir>` writes the bundle back out. The full walk-through is in
[Self-packaging](./self-packaging.md).

## 13. The config service in production

**What it changes.** Every write (`endpoints create/update/delete`,
`templates update`, `cache update`) changes the server's memory **and** its
files on its own disk. So:

- In a container, the change is gone on the next restart unless `sqls/` is a
  persistent volume.
- With several replicas, only the one that answered changed.
- Nothing goes back to git.

**Where it refuses.** A packed binary answers `409` ("its configuration is
read-only"). A `template.path` in remote storage (S3, GCS, ...) answers `501`
for template writes. Updating an endpoint whose YAML uses `{{include}}`
answers `409`, because the update would flatten the include.

**Recommendation.** Keep git as the source of truth. Use the config service on
a laptop and in staging to try changes, copy what works into the repository,
and deploy production from git (or a packed binary). In production, leave
`--config-service` off, or keep it for read-only use (`metrics`,
`cache refresh`, `cache audit`).

**Security.**

- `/api/v1/_config/*` is served on the **same port** as your API. If you enable
  it in production, block that prefix at your reverse proxy or load balancer
  for everyone but operators.
- The token is the key to rewriting your API. Use 32+ random characters
  (`openssl rand -hex 32`); startup warns below 16. Rotating it means
  restarting the server with the new token.
- With `--config-service`, the MCP server also offers 20 `flapi_*` tools
  (list, create, update and delete endpoints, update templates, refresh
  caches). They need the same token. See
  [MCP_CONFIG_TOOLS_API.md](../MCP_CONFIG_TOOLS_API.md).

## 14. A CI job

Three stages, each catching what the one before cannot:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd demo

# 1. Parse every YAML and template. Opens no data source.
flapi -c flapi.yaml --validate-config

# 2. Start a throwaway server with the config service on a spare port.
export FLAPI_CONFIG_SERVICE_TOKEN=$(openssl rand -hex 32)
export FLAPI_BASE_URL=http://localhost:18080
flapi -c flapi.yaml -p 18080 --config-service > server.log 2>&1 &
SERVER=$!
trap 'kill $SERVER' EXIT
until curl -sf "$FLAPI_BASE_URL/health/live" > /dev/null; do sleep 0.5; done

# 3a. Run each endpoint's query against real data, with fixture params.
flapii -q templates test /sample --params '{"id": 1}' > /dev/null
flapii -q templates test /by-category --params '{"category": "toys"}' > /dev/null

# 3b. Or bind-check the SQL without running it (?validate_only=1).
#     The slug of /by-category is -by~1category: '/' becomes '-', a literal '-' becomes '~1'.
curl -sf -X POST -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" \
  -d '{"parameters": {"category": "toys"}}' \
  "$FLAPI_BASE_URL/api/v1/_config/endpoints/-by~1category/template/expand?validate_only=1" \
  | jq -e '.valid'
```

`templates test` exits `1` on a validation or SQL error. `validate_only`
answers `200` either way, so check `.valid` (as `jq -e` does). A misspelled
column comes back as:

```json
{"valid": false, "errors": [{"type": "sql_syntax", "kind": "bind", "statement": 1,
  "message": "SQL validation failed: statement 1: Binder Error: Referenced column \"nme\" not found in FROM clause!..."}]}
```

Add `&target=cache` to check a cache's populate template the same way.

---

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| `[404] Not Found` with "The config service may be disabled" | Server started without `--config-service` | Restart with `--config-service --config-service-token <t>` |
| `No token was sent (HTTP 401)` | flapii has no token | Export `FLAPI_CONFIG_SERVICE_TOKEN`, or pass `-t` |
| `rejected the token flapii sent` | Token differs from the server's | Use the token from the server's command line or banner |
| `--auth-token` seems ignored | `FLAPI_CONFIG_SERVICE_TOKEN` is exported and wins | Pass `-t <token>` |
| `Cannot reach flAPI at http://localhost:8080` | Wrong port or host | `-u http://host:port` or `FLAPI_BASE_URL` |
| `endpoints validate` hangs | No `--file`, so it waits for YAML on stdin | Pass `--file <yaml>` |
| A new endpoint answers `500` | Created, template not uploaded yet | `flapii templates update <path> --file <sql>` |
| A YAML edit has no effect | Endpoint YAML is read only at startup and on reload | `flapii endpoints reload <path>`; new files: `--file` |
| `reload /x` → `404 Endpoint not found` | The server never loaded that file | `flapii endpoints reload --file sqls/x.yaml` |
| `error: unknown option '--endpoint'` from flapii | `--endpoint` belongs to the server's one-shot `flapi cache refresh` | flapii takes the path as an argument: `flapii cache refresh <path>` |
| `flapi cache refresh` → `holds the DuckLake catalog lock` | A server has the file catalog open | Refresh through it: `flapii cache refresh <path>` |
| `log-level get` says `info` on a `warning` server | `get` reports the last `set` | `set` once to align |
| `[DEP0169] DeprecationWarning: url.parse()` on stderr | Printed by a library inside the pip-installed flapii | Harmless; ignore it |
| `flapii --version` prints `26.6.13`, not the server's version | It is the CLI's own version | `flapii health` shows `server.version` |

## Next

- Endpoints in depth: [Building a REST endpoint](./rest-endpoints.md)
- What the cache commands drive: [Caching](./caching.md)
- Shipping one file: [Self-packaging](./self-packaging.md)

## How it works

`flapii` is a TypeScript client (Commander.js, compiled with Bun) over the REST
routes under `/api/v1/_config/`. Each command is one HTTP call; the server's
`ConfigService` checks the token, changes its in-memory config, writes the YAML
or SQL file when the command changes one, and answers JSON.

- [CLI_REFERENCE.md](../CLI_REFERENCE.md): every server flag, `pack`, `cache refresh`, `extensions install`, exit codes
- [CONFIG_SERVICE_API_REFERENCE.md](../CONFIG_SERVICE_API_REFERENCE.md): the HTTP routes behind each flapii command, slugs, `validate_only`
- Source: `cli/src/commands/` (one file per command group), `cli/src/lib/config.ts` (URL and token), `cli/src/lib/errors.ts` (messages, exit codes), `src/config_service.cpp` (server side), `src/cache_refresh_command.cpp` (`flapi cache refresh`)
