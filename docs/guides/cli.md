# Managing flAPI from the command line

**Goal:** inspect and change a running flAPI server from your terminal with
`flapii`: endpoints, SQL templates, caches, schema, log level and metrics,
without a restart. Plus the subcommands of the `flapi` binary itself.

There are two commands, and both come from one package:

| Command | What it is | Talks to |
|---|---|---|
| `flapi` | The server. Also packs bundles, refreshes caches once, installs extensions. | Your config files and DuckDB |
| `flapii` | A client for the server's **config service** (`/api/v1/_config/*`). | A running `flapi` over HTTP |

**Prerequisites**

- `pip install flapi-io` (installs both `flapi` and `flapii`), or
  `uvx --from flapi-io flapii --help` to try it without installing.
- A project. This guide uses the one `flapii project init` creates (step 1).

All output below is real, from flAPI 26.11.25. Long paths are shortened to
`.../demo`.

---

## 1. Create a project

```bash
flapii project init demo
```

```text
Setting up flapi project: demo
✔ ✅ Created directories
✔ ✅ Created flapi.yaml
✔ ✅ Created sample.yaml
✔ ✅ Created sample.sql
✔ ✅ Created sample.csv
✔ ✅ Created .gitignore
✔ ✅ Created auth.yaml
✔ ✅ Created rate-limit.yaml
- Validating configuration...
✔ ✓ Configuration is valid
```

You get `flapi.yaml`, an endpoint `GET /sample` (`sqls/sample.yaml` +
`sqls/sample.sql`) over `data/sample.csv`, and `common/auth.yaml` /
`common/rate-limit.yaml` to include later. Options:

| Option | Effect |
|---|---|
| `--no-examples` | Only the directories and `flapi.yaml` |
| `--advanced` | Extra template files |
| `--skip-validation` | Skip the check at the end |
| `-f`, `--force` | Overwrite existing files. Without it, files that already exist are skipped (`⚠ ⏭️ flapi.yaml already exists, skipping`), so rerunning it is safe. |
| `--ai` | Fill the config with Gemini's help (needs a Gemini key, see [the wizard](#the-wizard)) |

`project init` runs locally; it needs no server. Exit code `0` on success, `2`
for a permission error, `3` for an invalid argument, `1` otherwise.

To show the cache commands later, give the project a DuckLake catalog and one
cached endpoint. Add the `ducklake:` block to `demo/flapi.yaml`, which then
reads:

<!-- flapi: config group=demo -->
```yaml
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

<!-- flapi: check group=demo GET /api/v1/_config/endpoints -> 401 -->
<!-- flapi: check group=demo GET /api/v1/_config/health -> 200 -->
<!-- flapi: check group=demo GET /api/v1/_config/endpoints header=X-Config-Token:skill-check-token -> 200 contains "/sample-cached" -->

How the cache works is explained in [Caching](./caching.md).

## 2. Start the server with the config service

The config service is **off** by default. Turn it on and give it a token:

```bash
cd demo
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
either, it generates one and prints it in that banner. Use a long random
token in anything but a laptop demo: it is the key to rewriting your API.

## 3. Point flapii at the server

`flapii` needs two things: the server's URL and the token.

**The token** comes from, first match wins:

1. `-t`, `--config-service-token <token>`
2. `--auth-token <token>`
3. the environment variable `FLAPI_CONFIG_SERVICE_TOKEN`
4. the environment variable `FLAPI_TOKEN`

flapii sends it as both `Authorization: Bearer <token>` and
`X-Config-Token: <token>`. It never reads the token from a file.

**The URL** comes from, first match wins:

1. `-u`, `--base-url <url>`
2. `FLAPI_BASE_URL`, then `FLAPI_URL`
3. `cli.http.base-url` in a `flapi.yaml` flapii finds: the file given by
   `-c`, `--config`, else `$FLAPI_CONFIG`, else `flapi.yaml` in the current
   directory or a parent, else `~/.config/flapii/flapi.yaml`
4. `http://localhost:8080`

Because the server and flapii read the same `FLAPI_CONFIG_SERVICE_TOKEN`, the
simplest setup is one exported variable for both:

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=demo-token-0123456789
export FLAPI_BASE_URL=http://localhost:8080
flapii ping
```

```text
- Pinging flapi server...
✔ ✓ Connected to flapi server

📊 Project Information
════════════════════════════════════════════════════════════
Name: my-flapi-project
Description: Example flapi configuration
Base URL: http://localhost:8080
```

To keep a default URL on disk, use the per-user file, not your project's
`flapi.yaml`: the server warns about a `cli:` key in its own config
(`Unknown top-level configuration key 'cli' is ignored`).

```bash
mkdir -p ~/.config/flapii
cat > ~/.config/flapii/flapi.yaml <<'EOF'
cli:
  http:
    base-url: http://localhost:8080
    timeout-seconds: 30
EOF
```

flapii only reaches that file when no `flapi.yaml` is in the current
directory or its parents.

`~/.flapi/config.json` is not where flapii looks for the URL or the token. It
holds only the Gemini API key of the AI wizard ([below](#the-wizard)).

Other global options: `--timeout <seconds>` (default 10, env `FLAPI_TIMEOUT`),
`--retries <count>` (default 2, only for reads, env `FLAPI_RETRIES`),
`--insecure` (skip TLS verification), `--debug-http` (log requests with the
token masked), `-q`, `--quiet`, and `-y`, `--yes`.

**`ping` vs `health`.** `flapii ping` reads the project info and proves the
URL and the token are right. `flapii health` returns the server's health
report (database, endpoint count, storage, credentials, version):

```bash
flapii health
```

```json
{
  "endpoints": { "count": 2 },
  "database": { "status": "connected" },
  "server": { "version": "26.11.25", "name": "flapi" },
  "status": "healthy"
}
```

(trimmed: it also reports Arrow, storage and credential status). `health` is
the one config-service route that needs no token (without one, the template
path is left out); every other flapii command against a server needs it.

## 4. Endpoints

### List and inspect

```bash
flapii endpoints list -o table
```

```text
🌐 API Endpoints
════════════════════════════════════════════════════════════
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

```text
🔧 Parameters: /sample
════════════════════════════════════════════════════════════
id (in: query) optional
  Sample ID to filter
```

`endpoints get` returns the endpoint as the server parsed it, defaults
included (`withPagination`, `rateLimit`, `auth.inherited`, ...). When a GET
and a POST share a path, pick one with `--method POST`; the same option
exists on `update`, `delete`, `validate`, `reload` and the template and
cache commands.

### Create one from a file

Write the endpoint YAML and its SQL template anywhere, outside `sqls/`:

```yaml
# by-category.yaml
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
-- by_category.sql
SELECT id, name, price FROM read_csv_auto('{{{ conn.path }}}')
WHERE category = '{{{ params.category }}}'
```

Check it first. `endpoints validate` sends the YAML to the server, which
parses it as it would at startup; the path argument is the endpoint's URL
path, and the YAML comes from `--file` (or stdin):

```bash
flapii endpoints validate /by-category --file by-category.yaml
```

```text
✔ ✓ Endpoint /by-category is valid
⚠ Warnings:
⚠   - Template file does not exist: .../demo/sqls/by_category.sql
```

The warning is real: the server looks for the template in its own
`template.path`. Create the endpoint, then upload the template:

```bash
flapii endpoints create --file by-category.yaml
flapii templates update /by-category --file by_category.sql
curl 'http://localhost:8080/by-category?category=toys'
```

```text
✔ ✓ Endpoint created successfully
✔ ✓ Template /by-category updated successfully
{"data":[{"id":3,"name":"Gizmo","price":4.25},{"id":4,"name":"Doohickey","price":12}],"next":"","total_count":2}
```

`create` writes `sqls/by-category.yaml` on the server's disk and registers the
endpoint at once; `templates update` writes `sqls/by_category.sql`. Between the
two, requests to the new endpoint fail with `500`. `--file` accepts YAML or
JSON; `--stdin` reads the payload from a pipe.

### Update, reload, delete

```bash
flapii endpoints update /by-category --file by-category-v2.yaml
```

`update` replaces the endpoint's whole config and rewrites its YAML file.

If you edit files in `sqls/` by hand, tell the server:

```bash
flapii endpoints reload /sample                   # an endpoint it already serves
flapii endpoints reload --file sqls/cheap.yaml    # a new hand-written file
```

```text
✔ ✓ Endpoint configuration reloaded successfully
✔ ✓ Endpoint configuration loaded: GET /cheap
```

`reload /cheap` for an endpoint the server has never loaded answers
`[404] Not Found: Endpoint not found: /cheap`; use `--file` for new files.
flAPI does not watch the directory, so nothing changes until you reload.

```bash
flapii endpoints delete /cheap --force
```

Without `--force` (or the global `--yes`), `delete` stops with
`Use --force or --yes to delete endpoint /cheap` and exit code `1`. Delete
unregisters the endpoint and removes its YAML file; the SQL template stays on
disk.

### The wizard

`flapii endpoints wizard` asks for the name, path, method, parameters and cache
settings and creates the endpoint. `--ai` lets Gemini draft it from a sentence
(key from `FLAPI_GEMINI_KEY`, or prompted once and stored in
`~/.flapi/config.json`). `--output-file <path>` writes the YAML instead of
creating it, `--dry-run` shows what it would do. It is interactive, so do not
use it in scripts.

## 5. Templates

```bash
flapii templates list
flapii templates get /by-category
```

```json
{
  "template": "SELECT id, name, price FROM read_csv_auto('{{{ conn.path }}}')\nWHERE category = '{{{ params.category }}}'\n"
}
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

`expand` does not run the validators: `{"category":"cars"}` expands just the
same. `--cache` expands the endpoint's cache populate template instead, with
the live watermark, as the next refresh would run it.

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

A failing test exits `1` and prints the reason:

```text
✘ [400] Bad Request: Parameter validation failed
```

```text
✘ [400] Bad Request: SQL execution error: Query execution failed during main query: Catalog Error: Table with name nope_table does not exist!
```

`--params-file <file>` reads the JSON from a file. `templates update` checks
the Mustache syntax before it writes; a broken template is refused.

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
included (`"enabled": false`). Use the JSON output: the table view leaves the
schedule and table columns at `N/A`.

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

**Audit** shows the refresh history, newest first: scheduled refreshes,
warm-up and manual ones.

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

**Garbage-collect** old snapshots per the retention policy:

```bash
flapii cache gc /sample-cached
```

```json
{
  "message": "no retention policy configured; nothing expired (set cache.retention or ducklake.retention)",
  "bytes_freed": 0,
  "expired_snapshots": 0,
  "files_deleted": 0,
  "status": "no_policy"
}
```

Also available: `cache template` / `cache update-template` (read or replace the
populate SQL), `cache update` (the cache block), `cache compact` (DuckLake
compaction now) and `cache drop-table <table>` (an orphaned cache table).

## 7. Schema introspection

What the server's DuckDB can see:

```bash
flapii schema connections
```

```json
{
  "connections": [
    { "status": "configured", "type": "csv", "name": "sample-data" }
  ]
}
```

```bash
flapii schema tables --connection cache
```

```json
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

`flapii schema get` returns every database, table and column:

```bash
flapii schema get | jq '.cache.tables.sample_cache.columns | keys'
```

```json
["category", "id", "name", "price"]
```

Its `--connection`, `--table` and `-d`, `--database` options currently filter
nothing (the server ignores them); filter with `jq` as above. `schema refresh`
re-reads the schema after you attach something new.

## 8. Runtime log level

```bash
flapii config log-level set debug
```

```text
✓ Log level updated
════════════════════════════════════════════════════════════
New level: debug
Message: Log level updated successfully

Note: Log level will reset on server restart
```

`flapii config log-level list` shows the levels (`debug`, `info`, `warning`
or `warn`, `error`); anything else exits `1`. The change lasts until the
server restarts.

`flapii config log-level get` reports what the last `set` stored, not what
the server started with: a server started with `--log-level warning` reports
`Level: info` until the first `set`.

## 9. Metrics

```bash
flapii -o table metrics -e /sample
```

```text
📈 Requests
════════════════════════════════════════════════════════════
4 requests, 0 server errors, 0 cache-not-ready 503s · rejected: 2 × 401, 0 × 403, 0 × 429
┌─────────────┬──────────┬───────────────┬──────────┬────────┬────────┬────────┬────────┬────────┐
│ Endpoint    │ Requests │ Errors        │ Rejected │ Avg Ms │ P50 Ms │ P95 Ms │ P99 Ms │ Max Ms │
├─────────────┼──────────┼───────────────┼──────────┼────────┼────────┼────────┼────────┼────────┤
│ GET /sample │ 4        │ 1 4xx / 0 5xx │ 0        │ 2.93   │ ~4.00  │ ~4.11  │ ~4.11  │ 4.11   │
└─────────────┴──────────┴───────────────┴──────────┴────────┴────────┴────────┴────────┴────────┘
Percentiles are interpolated within histogram buckets and capped at the observed max.
```

Counters run since server start. Without `-e`, `--endpoint` you get every
route and MCP tool, plus each cache's refresh count, failures, last duration
and `data_age_seconds`. The rejection counts (401/403/429) are server-wide.
For Prometheus or tracing, see [Observability](./observability.md).

## 10. Output for scripts

- `-o json` (the default) or `-o table`. A global option, so it works on any
  command (`flapii -o table metrics`); `endpoints list`, `endpoints get`,
  `endpoints validate` and `cache audit` also take `--output` after the
  command.
- Progress lines and spinners (`✔ ✓ Endpoints retrieved`) go to **stderr**; the
  JSON goes to **stdout**. Piping to `jq` works without extra flags:

  ```bash
  flapii endpoints list | jq -r 'keys[]'
  ```

  ```text
  /sample
  /sample-cached
  ```

- `-q`, `--quiet` drops the progress lines. A failure still prints one line.
- `--json-style hyphen` returns the YAML spelling of keys (`url-path`,
  `template-source`) instead of camelCase.

**Exit codes.** `0` on success, `1` on any failure: an HTTP error, an
unreachable server, a failed validation or test, an unknown command or
option. The message tells which:

```text
✘ The config service needs a token. Start flAPI with `--config-service --config-service-token <token>`, and give the same token to flapii with --config-service-token or the FLAPI_CONFIG_SERVICE_TOKEN environment variable. No token was sent (HTTP 401).
✘ [401] Unauthorized: Unauthorized: missing or invalid config-service token (send it as X-Config-Token or Authorization: Bearer)
```

A token that is set but wrong says `The config service rejected the token
flapii sent`. A server that is not there says
`Cannot reach flAPI at http://localhost:8080. Is it running?`. A request that
outlives `--timeout` says the server may still be working; a refresh keeps
running.

A CI smoke test against a freshly deployed server:

```bash
set -e
flapii -q ping
flapii -q templates test /by-category --params '{"category":"toys"}' > /dev/null
flapii -q cache refresh /sample-cached > /dev/null
```

## 11. The server's own subcommands

These run the `flapi` binary once and exit. None needs the config service.

### Validate a config in CI

```bash
flapi -c flapi.yaml --validate-config
```

```text
Validating configuration file: flapi.yaml
✓ Configuration file loaded successfully
✓ Parsed 3 endpoint(s)

============================================================
✓ Validation PASSED
```

It loads every endpoint and checks the Mustache syntax of every template
without opening a data source. A missing template prints
`✗ Endpoint: /sample` with
`ERROR: Template file does not exist: .../sqls/missing.sql` and exits `1`.
Run this in CI on every change to `flapi.yaml` or `sqls/`.

`flapii config validate` is a lighter, local check of the endpoint YAML files
(it needs no server); prefer `flapi --validate-config`, which is the server's
own parser.

### Refresh caches once: `flapi cache refresh`

For a scheduled job (cron, Airflow, a Cloud Run Job after `dbt build`) where
no server runs, or where the serving replicas attach the catalog read-only:

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
ending in `.ducklake`) is locked by the server that has it open, so while
your server runs you get:

```json
{"error":"a running flapi holds the DuckLake catalog lock; refresh through the server: flapii cache refresh /sample-cached (if the holders are read-only replicas, stop the replicas, or use a Postgres/SQLite/MySQL DuckLake catalog, which a writer and replicas can share)"}
```

So: a server is running → `flapii cache refresh <path>`; no server, or
read-only replicas on a Postgres/SQLite/MySQL catalog →
`flapi cache refresh --endpoint <path>`.

### Pre-install extensions: `flapi extensions install`

Download DuckDB extensions at image build time, so the server starts
without network access:

```bash
flapi extensions install --config flapi.yaml --dir ./extensions
```

```text
✓ core_functions (built into flapi, nothing to install)
✓ httpfs
✓ ducklake
✓ fts
✓ json (built into flapi, nothing to install)
✓ postgres
✓ sqlite
✓ parquet (built into flapi, nothing to install)
```

You can also name extensions (`flapi extensions install --dir ./extensions httpfs bigquery@community`)
or install flAPI's default set with `--defaults`. At runtime, set
`FLAPI_EXTENSION_DIRECTORY` to the same directory. Install with the same
`flapi` binary you ship: the directory is keyed by DuckDB version and
platform.

### Pack a single binary: `flapi pack`, `info`, `unpack`

```bash
flapi pack --in ./demo --out demo-api
./demo-api info
./demo-api unpack --to /tmp/inspect
```

`pack` folds the config tree into a copy of the binary; `info` lists what a
binary carries; `unpack` writes it back out. The full walk-through is in
[Self-packaging](./self-packaging.md).

---

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| `[404] Not Found` with "The config service may be disabled" | Server started without `--config-service` | Restart with `--config-service --config-service-token <t>` |
| `No token was sent (HTTP 401)` | flapii has no token | Export `FLAPI_CONFIG_SERVICE_TOKEN`, or pass `-t` |
| `rejected the token flapii sent` | Token differs from the server's | Use the one from the server's command line or banner |
| `Cannot reach flAPI at http://localhost:8080` | Wrong port or host | `-u http://host:port` or `FLAPI_BASE_URL` |
| `endpoints validate` hangs | No `--file`, so it waits for YAML on stdin | Pass `--file <yaml>` |
| A new endpoint answers `500` | Created with `endpoints create`, template not uploaded yet | `flapii templates update <path> --file <sql>` |
| `reload /x` → `404 Endpoint not found` | The server never loaded that file | `flapii endpoints reload --file sqls/x.yaml` |
| Hand edits to `sqls/` have no effect | flAPI does not watch files | `flapii endpoints reload <path>` |
| `error: unknown option '--endpoint'` from flapii | `--endpoint` belongs to the server's one-shot `flapi cache refresh` | flapii takes the path as an argument: `flapii cache refresh <path>` |
| `flapi cache refresh` → `holds the DuckLake catalog lock` | A server has the file catalog open | Refresh through it: `flapii cache refresh <path>` |
| `log-level get` says `info` on a `warning` server | `get` reports the last `set` | Trust the server's `--log-level`; `set` once to align |

## Next

- Endpoints in depth: [Building a REST endpoint](./rest-endpoints.md)
- What the cache commands drive: [Caching](./caching.md)
- The same management operations as MCP tools for an agent: [MCP tools](./mcp-tools.md)
- Shipping one file: [Self-packaging](./self-packaging.md)

## How it works

`flapii` is a TypeScript client (Commander.js, compiled with Bun) over the
REST routes under `/api/v1/_config/`. Each command is one HTTP call; the
server's `ConfigService` checks the token, changes its in-memory config,
writes the YAML or SQL file when the command changes one, and answers
JSON. Nothing in flapii parses SQL or templates itself.

- [CLI_REFERENCE.md](../CLI_REFERENCE.md): every server flag, `pack`, `cache refresh`, `extensions install`, exit codes
- [CONFIG_SERVICE_API_REFERENCE.md](../CONFIG_SERVICE_API_REFERENCE.md): the HTTP routes behind each flapii command
- Source: `cli/src/commands/` (one file per command group), `cli/src/lib/config.ts` (URL and token resolution), `cli/src/lib/errors.ts` (messages and exit codes), `src/config_service.cpp` (server side), `src/cache_refresh_command.cpp` (`flapi cache refresh`)
