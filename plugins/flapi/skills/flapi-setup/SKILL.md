---
name: flapi-setup
description: >
  Installing flAPI (pip flapi-io, release archives, Docker image), scaffolding a
  project with `flapii project init`, the anatomy of flapi.yaml (template path,
  environment whitelist, connections with init SQL and properties, duckdb, ducklake,
  mcp, logging, CORS, HTTPS), connection recipes for Parquet/CSV, SQLite, Postgres,
  MySQL, BigQuery, Snowflake and S3/GCS/Azure, server flags, --validate-config and
  the first run. Use when someone starts a new flAPI project, wires up a data source,
  or cannot get the server to start.
version: 26.11.23
user-invocable: false
---

# flAPI Setup Cheat Sheet

**flAPI:** v26.11.23 | **Skill:** Setup | **See also:** `flapi-rest`, `flapi-mcp`, `flapi-auth`, `flapi-cache-design`, `flapi-deploy`

## Critical gotchas

- **`project-name` and `template.path` are required** (`project-description` is optional,
  default empty). Leave one out and startup fails with `Missing required key: ...`.
- **Paths in `properties` and in `init` resolve differently.** A property value that starts
  with `./` or `../` is resolved against the directory of `flapi.yaml`. A path inside `init`
  SQL (`ATTACH './data/x.sqlite' ...`) is plain DuckDB SQL: it resolves against the
  **process's working directory**. Run the server from the project directory or use absolute
  paths in `init`. A SQLite `ATTACH` is lazy, so a wrong path shows up on the first request
  (`Unable to open database`), not at startup.
- **`{{env.VAR}}` only works for whitelisted names.** `template.environment-whitelist` is a
  list of regexes. An unlisted variable in any YAML file **stops startup**. A missing or
  empty whitelist allows none. A top-level `environment-whitelist:` is rejected; it goes
  under `template:`. `${VAR}` is not supported.
- **There is no `server:` block and no MCP port.** A `server:` key stops startup. Use the
  top-level `http-port` / `http-host` (or `-p`, `--host`, `FLAPI_PORT`, `FLAPI_HOST`). MCP
  runs on the same port at `/mcp/jsonrpc`. `mcp.port` / `mcp.host` only log a warning.
- **A failing `init` statement stops the server.** All connections share one DuckDB
  instance, and every `init` runs at startup. `CREATE SECRET` that cannot be validated, or an
  `ATTACH` to an unreachable database, means no server, not one broken endpoint.
- **Log levels are `debug | info | warning | error`; `warn` is an alias of `warning`**
  (flag, `FLAPI_LOG_LEVEL` and config). An unknown `log-level:` in the config stops startup.
- **Unknown top-level keys are ignored with a warning**, for example `rate-limit:` (the
  global key is `rate_limit`), `port:` or `https:`. Known keys that do nothing warn too
  (`server-name`, `connections.<n>.log-queries/log-parameters/allow`, a request field's
  `field-type`, `storage:` with a local `template.path`). Do not ignore these warnings.
- **Credentials never go in YAML.** Put them in `CREATE SECRET` statements in `init` that
  read whitelisted env vars. Prefer `CREATE OR REPLACE SECRET` (temporary). `PERSISTENT`
  writes the secret to `~/.duckdb/stored_secrets`. flAPI's DuckDB also loads every
  persistent secret already stored there.
- **`flapi --version` prints the build version** (e.g. `26.10.31`; a build from git
  appends `-<n>-g<sha>`). A running server
  reports the same in `GET /api/v1/_config/health` -> `server.version`.
- **GCS: `GOOGLE_*` env vars are not applied to DuckDB** (startup WARNING). Use an HMAC
  `CREATE SECRET (TYPE gcs, KEY_ID ..., SECRET ...)` in a connection `init`.

## Templates in a bucket (remote `template.path`)

`template.path` may be `s3://`, `gs://`, `az://` or `https://`. Credentials come from the environment:
`AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` / `AWS_REGION`, `GCS_ACCESS_KEY_ID` / `GCS_SECRET_ACCESS_KEY`
(an HMAC key), `AZURE_STORAGE_CONNECTION_STRING`. Plain `https://` has no listing: add a `manifest.txt` at the root
(one endpoint YAML per line). Do not write local `sqls/` files for such a project and do not expect the config
service or `flapi_*` tools to write endpoints: upload the `.yaml`/`.sql` files to the bucket, check with
`flapi -c flapi.yaml --validate-config`, and restart flAPI (YAML is read at boot; SQL within `storage.cache.ttl`).
Whoever can write the bucket can run SQL as the server: give flAPI read-only, prefix-scoped credentials.

## Install

```bash
pip install flapi-io          # ONE package with both commands: flapi (server) + flapii (CLI)
flapi --help
flapii --help
```

| Channel | What you get |
|---|---|
| PyPI `flapi-io` | wheels for linux x86_64/aarch64, macOS arm64 and Windows x64. Each wheel holds both binaries. There is no separate `flapii` package. |
| GitHub release | `flapi-linux-amd64.tar.gz`, `flapi-linux-arm64.tar.gz`, `flapi-macos-arm64.tar.gz`, `flapi-windows-amd64.zip` (server only, with `extensions/`). macOS, after a browser download: `xattr -dr com.apple.quarantine <unpacked dir>`. |
| Docker | `ghcr.io/datazoode/flapi:latest` (the latest release), `:v<version>`, `:v<version>-runtime` (extensions pre-installed). See `flapi-deploy`. |

```bash
docker run -it --rm -p 8080:8080 -v "$PWD":/config ghcr.io/datazoode/flapi:latest -c /config/flapi.yaml
```

## Scaffold a project

```bash
flapii project init my-api            # or: flapii project init  (current directory)
flapii project init my-api --force    # overwrite existing files
flapii project init my-api --no-examples --skip-validation
```

The command creates `flapi.yaml`, `sqls/sample.yaml`, `sqls/sample.sql`, `data/sample.csv`,
`common/auth.yaml`, `common/rate-limit.yaml` and `.gitignore`. The sample endpoint
`GET /sample` reads `./data/sample.csv`, which init also writes, so it answers 200 at once.
`--advanced` and `--ai` (needs a Gemini key) are also available.

## Minimal complete project

Layout: `flapi.yaml`, `sqls/customers.yaml`, `sqls/customers.sql`, `data/customers.parquet`.

```yaml
# flapi: config group=minimal
project-name: customers-api
project-description: Customers served from a Parquet file
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: ./data/customers.parquet     # resolved against this file's directory
```

```yaml
# flapi: endpoint group=minimal file=customers.yaml
url-path: /customers
method: GET
request:
  - field-name: segment
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [AUTOMOBILE, BUILDING, FURNITURE, HOUSEHOLD, MACHINERY]
  - field-name: id
    field-in: query
    required: false
    validators:
      - type: int
        min: 1
template-source: customers.sql
connection: [customers]
# flapi: check GET /customers?segment=BUILDING -> 200
# flapi: check GET /customers?id=7 -> 200
# flapi: check GET /customers?segment=SPACE -> 400
# flapi: check GET /customers?id=0 -> 400
```

```sql
-- flapi: template group=minimal file=customers.sql
SELECT c_custkey, c_name, c_mktsegment, c_acctbal
FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.segment}}
  AND c_mktsegment = '{{{ params.segment }}}'
{{/params.segment}}
{{#params.id}}
  AND c_custkey = {{ params.id }}
{{/params.id}}
ORDER BY c_custkey
```

```bash
flapi -c flapi.yaml --validate-config      # never opens the database; exit 0 = valid
flapi -c flapi.yaml                         # serves on 0.0.0.0:8080
curl 'http://localhost:8080/customers?segment=BUILDING&limit=2'
# {"next":"...","total_count":...,"data":[{"c_custkey":...}, ...]}
```

Open `http://localhost:8080/doc` (Swagger UI) or `/doc.yaml` (OpenAPI). For endpoint design
(parameters, validators, pagination, `{{ }}` vs `{{{ }}}`), see `flapi-rest`.

## SQLite example (Northwind)

`./data/northwind.sqlite` has the tables `Products`, `Categories`, `Orders`, `Order Details`,
`Customers`, `Employees`, `Suppliers`, `Shippers`, `Territories` and `Regions`.

```yaml
# flapi: config group=northwind
project-name: northwind-api
project-description: Northwind products from SQLite
template:
  path: ./sqls
connections:
  northwind:
    init: |
      INSTALL sqlite;
      LOAD sqlite;
      ATTACH IF NOT EXISTS './data/northwind.sqlite' AS nw (TYPE sqlite, READ_ONLY);
```

```yaml
# flapi: endpoint group=northwind file=products.yaml
url-path: /products
method: GET
request:
  - field-name: category_id
    field-in: query
    required: false
    validators:
      - type: int
        min: 1
        max: 8
template-source: products.sql
connection: [northwind]
# flapi: check GET /products -> 200
# flapi: check GET /products?category_id=1 -> 200
# flapi: check GET /products?category_id=99 -> 400
```

```sql
-- flapi: template group=northwind file=products.sql
SELECT p.ProductID, p.ProductName, c.CategoryName, p.UnitPrice, p.UnitsInStock
FROM nw.Products p
JOIN nw.Categories c ON c.CategoryID = p.CategoryID
WHERE 1=1
{{#params.category_id}}
  AND p.CategoryID = {{ params.category_id }}
{{/params.category_id}}
ORDER BY p.ProductID
```

flAPI runs one query at a time on any connection whose `init` attaches SQLite. This
avoids a read/write deadlock in the SQLite scanner. Override it with
`serialize-access: true|false` on the connection.

## flapi.yaml anatomy

```yaml
project-name: my-api                  # required
project-description: What it serves   # optional
http-port: 8080                       # -p / FLAPI_PORT win over this
http-host: 0.0.0.0                    # --host / FLAPI_HOST win over this
log-level: info                       # debug | info | warning (or warn) | error
log-format: text                      # text | json (one JSON object per line)
template:
  path: ./sqls                        # required; endpoint YAML + SQL, scanned recursively
  environment-whitelist:              # regexes for {{env.NAME}}; empty = none allowed
    - '^PG_(HOST|PASSWORD)$'
connections:
  warehouse:
    init: |                           # DuckDB SQL run once at startup
      INSTALL postgres;
      LOAD postgres;
    properties:                       # free-form; templates read {{{ conn.<key> }}}
      schema: public
mcp:
  enabled: true                       # served at /mcp/jsonrpc on http-port
cors:
  allow-origins: [https://app.example.com]   # no cors block = Access-Control-Allow-Origin: *
```

The `duckdb:` block (YAML; its keys are DuckDB settings passed through, so it is shown as text):

```text
duckdb:
  db_path: ./flapi.db                 # omit = in-memory
  threads: 4                          # any other key is passed to DuckDB as a setting
  max_memory: 2GB
  default-extensions: [httpfs, parquet, json]   # default: httpfs ducklake fts json postgres sqlite parquet
```

Every key, with its default and its gotcha (including `ducklake`, `heartbeat`,
`enforce-https`, `rate_limit`, `auth`, `audit`, `tracing` and `telemetry`), is in
[references/flapi-yaml.md](references/flapi-yaml.md).

## Connection recipes

A connection is a name, optional `init` SQL and optional `properties`. The endpoint lists it
in `connection: [name]`, and the template reads its properties as `conn.<key>`. All `init`
blocks run in the **same** DuckDB, so a catalog attached in one connection is visible to
every endpoint.

| Source | Extension | How the template reads it |
|---|---|---|
| Parquet / CSV / JSON file, glob | `parquet` / built-in CSV / `json` | `read_parquet('{{{ conn.path }}}')`, `read_csv(...)`, `read_json(...)` |
| SQLite | `sqlite` (default) | `ATTACH ... (TYPE sqlite, READ_ONLY)` in init, then `nw.Table` |
| DuckDB file | built-in | `ATTACH ... (READ_ONLY)`; this **locks the file against every other writer** while the server runs. Written by another process: attach in the populate template + atomic rename (references) |
| Postgres | `postgres` (default) | `CREATE SECRET (TYPE postgres ...)` + `ATTACH '' AS pg (TYPE postgres, SECRET ...)` |
| MySQL | `mysql` (core, **not** a default) | `CREATE SECRET (TYPE mysql ...)` + `ATTACH '' AS my (TYPE mysql, SECRET ...)` |
| BigQuery | `bigquery` (**community**) | `ATTACH 'project=<id>' AS bq (TYPE bigquery, READ_ONLY)` |
| Snowflake | `snowflake` (**community**, needs the ADBC Snowflake driver on the host) | `CREATE SECRET (TYPE snowflake ...)` + `ATTACH '' AS sf (TYPE snowflake, SECRET ...)` |
| S3 / GCS | `httpfs` (default) | `read_parquet('s3://...')` / `'gs://...'` + `CREATE SECRET (TYPE s3 / gcs)` |
| Azure Blob | `azure` (**not** a default) | `read_parquet('az://...')` + `CREATE SECRET (TYPE azure ...)` |

The full YAML for each recipe, with whitelisted env vars, is in
[references/connections.md](references/connections.md).

## Server flags

```bash
flapi -c flapi.yaml                    # --config; default ./flapi.yaml (env FLAPI_CONFIG)
flapi -c flapi.yaml -p 9090 --host 127.0.0.1
flapi -c flapi.yaml --log-level debug  # env FLAPI_LOG_LEVEL
flapi -c flapi.yaml --validate-config  # check config + endpoints, exit
flapi -c flapi.yaml --config-service --config-service-token "$FLAPI_CONFIG_SERVICE_TOKEN"
flapi -c flapi.yaml --no-telemetry
flapi -c flapi.yaml --cache-warmup if-empty   # always | if-empty | never (env FLAPI_CACHE_WARMUP)
flapi -c flapi.yaml --no-cache-warmup
```

Precedence is **CLI flag > env var > flapi.yaml > default** for the port, host, log level and
config path. An invalid `FLAPI_PORT` or log level exits with status 1.

Subcommands: `flapi cache refresh`, `flapi extensions install`, `flapi pack`, `flapi info`,
`flapi unpack`. See `flapi-deploy` and `flapi-cache-tuning`.

`flapii` (the CLI) talks to a running server's config service. Start the server with
`--config-service`. Then `export FLAPI_CONFIG_SERVICE_TOKEN=<token>` for the client (or
pass `-t`). The default base URL is `http://localhost:8080` (`-u`, `FLAPI_BASE_URL`, or
`FLAPI_URL` when `FLAPI_BASE_URL` is unset). The server logs `flAPI server listening on
http://<host>:<port> (REST API; MCP at .../mcp/jsonrpc)` once it has bound the port.

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=dev-token-0123456789
flapi -c flapi.yaml --config-service &
flapii ping
flapii endpoints list
```

## Canonical setup workflow

1. `pip install flapi-io`. Check with `flapi --help` and `flapii --help`.
2. `flapii project init my-api && cd my-api`, or write `flapi.yaml` + `sqls/` by hand
   (see the minimal project above).
3. Add one connection per source. Put `INSTALL`/`LOAD`/`CREATE SECRET`/`ATTACH` in `init`.
   Read credentials as `{{env.X}}` and add each `X` to `template.environment-whitelist`.
4. Trim `duckdb.default-extensions` to what you use. Each default is installed (downloaded)
   at startup when it is missing; `json` and `parquet` are built in.
5. Write one endpoint YAML + SQL over the real data, with a validator on every parameter
   (`flapi-rest`).
6. Run `flapi -c flapi.yaml --validate-config` until it reports no errors, and read every
   warning.
7. Run `flapi -c flapi.yaml` from the project directory, `curl` the endpoint, open `/doc`.
8. To manage endpoints with `flapii`, restart with `--config-service` and export
   `FLAPI_CONFIG_SERVICE_TOKEN`.
9. Next steps: auth (`flapi-auth`), MCP tools (`flapi-mcp`), caching (`flapi-cache-design`),
   shipping (`flapi-deploy`).
