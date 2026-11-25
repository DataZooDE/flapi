# Connecting data sources

**Goal:** point flAPI at your data (local files, SQLite, a database server, a
warehouse, a lakehouse table or SAP) and query it from an endpoint.

**Before you start:** flAPI installed and one endpoint served, as in
[Getting started](./getting-started.md). The runnable examples read two sample
files from this repository. Put them in a `data/` directory next to your
`flapi.yaml`:

```bash
mkdir -p data
curl -L -o data/customers.parquet https://github.com/DataZooDE/flapi/raw/main/examples/data/customers.parquet
curl -L -o data/northwind.sqlite https://github.com/DataZooDE/flapi/raw/main/examples/data/northwind.sqlite
```

(Or clone the repository; the files are in `examples/data/`.)

flAPI has no database drivers of its own. It embeds DuckDB 2.0.1, and DuckDB and
its **extensions** (plug-ins that add file formats and database connectors) do the
reading. A connection is a name plus two optional parts: properties your templates
can read, and SQL that sets DuckDB up at startup.

---

## What one process means

Every connection of a flAPI process lives in **one DuckDB instance**. That has
consequences before you write any YAML:

- **One pool of resources.** All sources share DuckDB's memory limit and worker
  threads (`duckdb.max_memory`, `duckdb.threads`), and all requests share flAPI's
  request handler threads (`max(CPU cores, 8)`, at most 64, not configurable). A
  heavy query on one source slows the others.
- **No isolation between connections.** Every template can read every attached
  database and every file the process can reach, and use every **secret** (a DuckDB
  object holding credentials, created with `CREATE SECRET`). An endpoint's
  `connection:` list is not an access boundary.
- **Separate credentials mean separate processes.** If two teams or tenants must not
  see each other's data, run one flAPI per team, each with its own config and
  credentials.

## 1. How a connection works

This is a new project with two connections. Run every command from the directory
that holds `flapi.yaml`; step 3 explains why.

`flapi.yaml`:

<!-- flapi: config group=basics -->
```yaml
project-name: sources-demo
project-description: Connecting data sources
template:
  path: ./sqls
connections:
  customers:                            # the name endpoints refer to
    properties:                         # strings, available in templates as conn.*
      path: ./data/customers.parquet    # ./ and ../ resolve against this file's directory
  northwind:
    init: |                             # DuckDB SQL, run once at startup
      INSTALL sqlite;
      LOAD sqlite;
      ATTACH IF NOT EXISTS './data/northwind.sqlite' AS nw (TYPE sqlite, READ_ONLY);
```

The parts:

- **`properties`** is a map of strings. A template sees the properties of its
  endpoint's connection as `{{{ conn.<key> }}}`. A value that starts with `./` or
  `../` is turned into an absolute path under the directory of `flapi.yaml`.
- **`init`** is DuckDB SQL that runs **once, at startup**, before the server takes
  requests. Use it to load extensions, create secrets and `ATTACH` databases. It is
  not a template: `{{ conn.path }}` in `init` stays literal text. `{{env.NAME}}`
  does work, because it is substituted in the whole YAML file (step 5).
- An endpoint names its connection in **`connection: [name]`**. A name that is not
  under `connections:` fails validation (`Connection 'x' not found in configuration`).

`sqls/customers.yaml`:

<!-- flapi: endpoint group=basics file=customers.yaml -->
```yaml
url-path: /customers
request:
  - field-name: segment
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [AUTOMOBILE, BUILDING, FURNITURE, HOUSEHOLD, MACHINERY]
template-source: customers.sql
connection: [customers]
```
<!-- flapi: check GET /customers?segment=BUILDING&limit=2 -> 200 contains "BUILDING" -->

`sqls/customers.sql`:

<!-- flapi: template group=basics file=customers.sql -->
```sql
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.segment}}
  AND c_mktsegment = '{{{ params.segment }}}'
{{/params.segment}}
ORDER BY c_custkey
```

Start the server:

```bash
flapi -c flapi.yaml
```

and, in a second terminal:

```bash
curl 'http://localhost:8080/customers?segment=BUILDING&limit=2'
```

```json
{"data":[{"c_custkey":1,"c_name":"Customer#000000001","c_mktsegment":"BUILDING"},
          {"c_custkey":8,"c_name":"Customer#000000008","c_mktsegment":"BUILDING"}],
 "next":"/customers?offset=2&limit=2&segment=BUILDING","total_count":337}
```

A statement in `init` that fails stops startup and names the connection. With
`ATACH` mistyped for `ATTACH`:

```text
[error] Database initialization error: Error creating database, Details: connection 'northwind' init: Failed to parse init statements: Parser Error: syntax error at or near <redacted>
[error] flAPI cannot start until the database can be opened.
```

Literals in the message are redacted, because an `init` often carries credentials.
A statement that parses but fails names its position:
`Failed to execute init statement 3 of 4: ...`.

## 2. Local files and globs

A file needs no `init`: put its path in `properties` and read it with DuckDB's
`read_parquet`, `read_csv` or `read_json`. All three readers are built into flAPI.
A glob (`*`, `**`) reads many files as one table.

This is a second new project. Besides `data/northwind.sqlite` from the top of this
guide, it needs two CSV files in a `files/orders/` directory next to `flapi.yaml`.

`files/orders/2026-01.csv`:

<!-- flapi: file group=files file=files/orders/2026-01.csv -->
```text
order_id,customer_id,order_date,amount
1,ALFKI,2026-01-05,120.50
2,ANATR,2026-01-17,64.00
3,ALFKI,2026-01-28,310.25
```

`files/orders/2026-02.csv`:

<!-- flapi: file group=files file=files/orders/2026-02.csv -->
```text
order_id,customer_id,order_date,amount
4,BERGS,2026-02-02,89.90
5,ALFKI,2026-02-14,42.00
```

`flapi.yaml`:

<!-- flapi: config group=files -->
```yaml
project-name: files-demo
project-description: CSV files and a join with SQLite
template:
  path: ./sqls
connections:
  orders:
    properties:
      path: ./files/orders/*.csv        # a glob: every month's file
  northwind:
    init: |
      INSTALL sqlite;
      LOAD sqlite;
      ATTACH IF NOT EXISTS './data/northwind.sqlite' AS nw (TYPE sqlite, READ_ONLY);
```

`sqls/orders.yaml`:

<!-- flapi: endpoint group=files file=orders.yaml -->
```yaml
url-path: /orders
request:
  - field-name: month
    field-in: query
    required: false
    validators:
      - type: string
        regex: '^[0-9]{4}-[0-9]{2}$'
template-source: orders.sql
connection: [orders]
```
<!-- flapi: check GET /orders -> 200 contains "order_id":5," -->
<!-- flapi: check GET /orders?month=2026-02 -> 200 contains "BERGS" -->
<!-- flapi: check GET /orders?month=February -> 400 -->

`sqls/orders.sql`:

<!-- flapi: template group=files file=orders.sql -->
```sql
SELECT order_id, customer_id, order_date, amount,
       regexp_extract(filename, '([0-9]{4}-[0-9]{2})\.csv$', 1) AS month
FROM read_csv('{{{ conn.path }}}', header = true, filename = true)
WHERE 1=1
{{#params.month}}
  AND regexp_extract(filename, '([0-9]{4}-[0-9]{2})\.csv$', 1) = '{{{ params.month }}}'
{{/params.month}}
ORDER BY order_id
```

`filename = true` adds a column naming the file each row came from; here it gives
each order its month. With the server running, in a second terminal:

```bash
curl 'http://localhost:8080/orders?month=2026-02'
```

```json
{"data":[{"order_id":4,"customer_id":"BERGS","order_date":"2026-02-02","amount":89.9,"month":"2026-02"},
          {"order_id":5,"customer_id":"ALFKI","order_date":"2026-02-14","amount":42,"month":"2026-02"}],
 "next":"","total_count":2}
```

For Hive-partitioned Parquet (`./data/events/year=*/month=*/*.parquet`), DuckDB
turns the directory names into columns, and a filter on them skips whole
directories. Files in a bucket (`s3://`, `gs://`, `az://`) or behind `https://`
are read the same way once credentials are in place: see
[Reading from cloud storage](./cloud-storage.md).

## 3. SQLite

SQLite is attached in `init`, as the `northwind` connection of the step 1 project
does. In that project (`sources-demo`), add an endpoint.

`sqls/products.yaml`:

<!-- flapi: endpoint group=basics file=products.yaml -->
```yaml
url-path: /products
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
```
<!-- flapi: check GET /products?category_id=1&limit=2 -> 200 contains "Chai" -->

`sqls/products.sql`:

<!-- flapi: template group=basics file=products.sql -->
```sql
SELECT p.ProductID, p.ProductName, c.CategoryName, p.UnitPrice
FROM nw.Products p
JOIN nw.Categories c ON c.CategoryID = p.CategoryID
WHERE 1=1
{{#params.category_id}}
  AND p.CategoryID = {{ params.category_id }}
{{/params.category_id}}
ORDER BY p.ProductID
```

Restart the server, then in a second terminal:

```bash
curl 'http://localhost:8080/products?category_id=1&limit=2'
```

```json
{"data":[{"ProductID":1,"ProductName":"Chai","CategoryName":"Beverages","UnitPrice":18},
          {"ProductID":2,"ProductName":"Chang","CategoryName":"Beverages","UnitPrice":19}],
 "next":"/products?offset=2&category_id=1&limit=2","total_count":12}
```

Two things to know:

- **A path inside `init` is not rewritten.** `'./data/northwind.sqlite'` is
  resolved by DuckDB against the server's **working directory**, not the directory
  of `flapi.yaml`. Use an absolute path in production. A SQLite file that is not
  there does not stop startup: the first request fails with
  `Unable to open database "./data/northwind.sqlite"` in the log.
- **One query at a time.** A connection whose `init` attaches SQLite is
  **serialised**: flAPI runs only one query on it at a time, because a read and a
  write at the same moment can deadlock a SQLite attachment. Other connections still
  run in parallel. `serialize-access: false` turns it off; see
  [CONFIG_REFERENCE § 2.3](../CONFIG_REFERENCE.md#23-connections).

A DuckDB file is attached the same way (`ATTACH '/srv/data/warehouse.duckdb' AS
wh (READ_ONLY)`), but the server then holds its lock for as long as it runs, so
no other process can write it.

## 4. Several connections in one template

A template can read everything the process has (see
[What one process means](#what-one-process-means)). `connection:` decides two
things only: **`conn.*` comes from the first connection in the list**, and **every
listed connection that is serialised takes its lock** for the query. So list every
connection the template reads, and put the one whose `conn.*` you use first.

In the `files-demo` project of step 2, join the CSV orders to the Northwind
customers in SQLite.

`sqls/order-customers.yaml`:

<!-- flapi: endpoint group=files file=order-customers.yaml -->
```yaml
url-path: /order-customers
template-source: order-customers.sql
connection: [orders, northwind]         # conn.path is the orders glob
```
<!-- flapi: check GET /order-customers -> 200 contains "Alfreds Futterkiste" -->

`sqls/order-customers.sql`:

<!-- flapi: template group=files file=order-customers.sql -->
```sql
SELECT o.order_id, o.amount, c.CompanyName, c.Country
FROM read_csv('{{{ conn.path }}}', header = true) o
JOIN nw.Customers c ON c.CustomerID = o.customer_id
ORDER BY o.order_id
```

```bash
curl 'http://localhost:8080/order-customers?limit=2'
```

```json
{"data":[{"order_id":1,"amount":120.5,"CompanyName":"Alfreds Futterkiste","Country":"Germany"},
          {"order_id":2,"amount":64,"CompanyName":"Ana Trujillo Emparedados y helados","Country":"Mexico"}],
 "next":"/order-customers?offset=2&limit=2","total_count":5}
```

A template cannot reach the `conn.*` of a second connection. For a second file,
either write its path into the template, or give that connection an `init` that
creates a view (`CREATE OR REPLACE VIEW customers AS SELECT * FROM
read_parquet('/srv/data/customers.parquet')`) and query the view.

## 5. Postgres and MySQL

*Not run here; needs a reachable server.*

Credentials come from the environment. Write `{{env.NAME}}` anywhere in the YAML
and list the variable in `template.environment-whitelist` (regular expressions,
matched against the whole name). The `CREATE SECRET` statements hold the
credentials in memory; the `ATTACH` uses them.

```yaml
template:
  path: ./sqls
  environment-whitelist:
    - '^PG_(HOST|USER|PASSWORD)$'
    - '^MYSQL_(HOST|USER|PASSWORD)$'
connections:
  shop:
    init: |
      INSTALL postgres;
      LOAD postgres;
      CREATE OR REPLACE SECRET pg_shop (TYPE postgres, HOST '{{env.PG_HOST}}', PORT 5432,
        DATABASE 'shop', USER '{{env.PG_USER}}', PASSWORD '{{env.PG_PASSWORD}}');
      ATTACH '' AS pg (TYPE postgres, SECRET pg_shop, READ_ONLY);
  crm:
    init: |
      INSTALL mysql;
      LOAD mysql;
      CREATE OR REPLACE SECRET my_crm (TYPE mysql, HOST '{{env.MYSQL_HOST}}', PORT 3306,
        DATABASE 'crm', USER '{{env.MYSQL_USER}}', PASSWORD '{{env.MYSQL_PASSWORD}}');
      ATTACH '' AS crm (TYPE mysql, SECRET my_crm, READ_ONLY);
```

```sql
SELECT order_id, total FROM pg.public.orders
WHERE customer_id = {{ params.customer_id }}
```

- The `ATTACH` connects at startup: a database that cannot be reached stops the
  server.
- DuckDB pushes filters and column lists down to the server. `postgres_query('pg',
  'SELECT ...')` and `mysql_query('crm', 'SELECT ...')` run SQL on the remote side
  as written.
- `postgres` is one of flAPI's default extensions; `mysql` is not, so its `INSTALL`
  downloads it on first start (step 11 covers offline hosts).
- Do not write `CREATE PERSISTENT SECRET`. It stores the secret unencrypted in
  `~/.duckdb/stored_secrets`, and every DuckDB-based process of that user, every
  flAPI included, loads it at startup. An in-memory secret is not scoped either: any
  template of this process can use it.

A variable that the whitelist does not match stops startup:

```text
[error] Configuration error: ... environment variable PG_PASSWORD referenced as {{env.NAME}} but not whitelisted.
Add a pattern matching it to `template.environment-whitelist` in the main configuration file; an empty or missing whitelist allows no variables.
```

### Production checklist for a database source

- **A read-only role** on the database side, granted only the tables you serve.
  flAPI's `READ_ONLY` cannot stop a template that uses `postgres_query` to run
  arbitrary SQL with the role's rights.
- **`READ_ONLY` in the `ATTACH`**, so no template can write through the attachment.
- **TLS.** Postgres reads libpq's environment, so `PGSSLMODE=require` (or
  `verify-full` with `PGSSLROOTCERT`) applies to the attachment; for MySQL see the
  `ssl_mode` options of DuckDB's mysql extension. Not verified here against a TLS
  server.
- **Absolute paths in `init`** for files (SQLite, DuckDB, certificates).
- **Credentials only through `{{env.NAME}}`**, never written into the YAML.

## 6. BigQuery

*Not run here; needs a Google Cloud project.*

`bigquery` is a DuckDB **community extension**: built by third parties and
published, signed by DuckDB, in its
[community repository](https://duckdb.org/community_extensions/). It is
pre-installed only in the `-runtime` Docker image
(`ghcr.io/datazoode/flapi:<tag>-runtime`, the default image plus `bigquery`). With a
release archive, the `flapi-io` wheel or the default image, the `INSTALL` below
downloads it on first start:

```yaml
template:
  path: ./sqls
  environment-whitelist:
    - '^GCP_PROJECT$'
connections:
  lakehouse:
    init: |
      INSTALL bigquery FROM community;
      LOAD bigquery;
      ATTACH 'project={{env.GCP_PROJECT}}' AS bq (TYPE bigquery, READ_ONLY);
    properties:
      dataset: analytics
```

```yaml
request:
  - field-name: since
    field-in: query
    required: true
    validators:
      - type: date
```

```sql
SELECT order_id, total FROM bq.{{{ conn.dataset }}}.orders
WHERE order_date >= '{{{ params.since }}}'
```

The `date` validator is what makes the quoted parameter safe: it rejects anything
that is not a date with a 400 before the SQL is rendered.

The extension authenticates with Google Application Default Credentials: set
`GOOGLE_APPLICATION_CREDENTIALS` to a service-account key file, or run where a
metadata server provides them. The extension reads that variable itself, so it
needs no whitelist entry. BigQuery bills every query; see step 13.

## 7. Snowflake

*Not run here; needs a Snowflake account.*

`snowflake` is a community extension too (`installed_from: community`, signed by
DuckDB). It also needs the **ADBC Snowflake driver** (`libadbc_driver_snowflake.so`
on Linux), a native library you install on the host yourself; neither flAPI nor
any flAPI image ships it.

```yaml
template:
  path: ./sqls
  environment-whitelist:
    - '^SF_(ACCOUNT|USER|PASSWORD)$'
connections:
  dwh:
    init: |
      INSTALL snowflake FROM community;
      LOAD snowflake;
      CREATE OR REPLACE SECRET sf_dwh (TYPE snowflake, ACCOUNT '{{env.SF_ACCOUNT}}',
        USER '{{env.SF_USER}}', PASSWORD '{{env.SF_PASSWORD}}',
        DATABASE 'ANALYTICS', WAREHOUSE 'COMPUTE_WH');
      ATTACH '' AS sf (TYPE snowflake, SECRET sf_dwh, READ_ONLY);
```

Verified with flAPI 26.11.25: the extension installs and the secret is accepted.
Without the driver, startup stops at the `ATTACH` with `IO Error: ADBC Snowflake
driver (...) not found`, followed by the places it searched. Querying a live
account is not verified here.

**Databricks and MotherDuck.** There is no `databricks` extension for DuckDB 2.0.1
in the core or community repositories; DuckDB's `uc_catalog` (Unity Catalog)
extension installs, but flAPI has not been tested with it. The `motherduck` core
extension also installs (`ATTACH 'md:'` with a `motherduck_token`); it too is
untested with flAPI.

## 8. Iceberg and Delta tables

*Not run here; needs a table to point at.*

Both are DuckDB core extensions, not in flAPI's default list:

```yaml
connections:
  lake:
    init: |
      INSTALL iceberg;
      LOAD iceberg;
      INSTALL delta;
      LOAD delta;
    properties:
      orders: s3://my-lake/warehouse/sales/orders        # an Iceberg table
      events: /srv/lake/events                           # a Delta table
```

```sql
SELECT * FROM iceberg_scan('{{{ conn.orders }}}')
-- or: SELECT * FROM delta_scan('{{{ conn.events }}}')
```

`iceberg_scan` reads the table's metadata to find its files; point it at the table
directory (or a specific `metadata.json`). In a bucket, the credentials come from
[Reading from cloud storage](./cloud-storage.md).

## 9. SAP via ERPL

*Not run here; needs an SAP system and the SAP NetWeaver RFC SDK on the host.*

[ERPL](https://erpl.io) is a DuckDB extension from its own repository, and it is
**unsigned**. flAPI always starts DuckDB with `allow_unsigned_extensions` enabled,
so it loads:

```yaml
template:
  path: ./sqls
  environment-whitelist:
    - '^SAP_(ASHOST|SYSNR|CLIENT|USER|PASSWORD)$'
connections:
  sap:
    init: |
      INSTALL 'erpl' FROM 'http://get.erpl.io';
      LOAD 'erpl';
      CREATE OR REPLACE SECRET sap_erp (TYPE sap_rfc,
        ASHOST '{{env.SAP_ASHOST}}', SYSNR '{{env.SAP_SYSNR}}', CLIENT '{{env.SAP_CLIENT}}',
        USER '{{env.SAP_USER}}', PASSWD '{{env.SAP_PASSWORD}}', LANG 'EN');
```

```sql
SELECT MATNR, WERKS, LABST FROM sap_read_table('MARD')
```

`flapi extensions install --config` and `flapi pack --with-extensions` do not pick
up an `INSTALL` from a URL repository like this one: on a host without network,
put the extension into the extension directory yourself.

## 10. Object storage

S3, GCS, Azure Blob and HTTPS files are read with `httpfs` (a default extension)
and credentials from the environment or a DuckDB secret. That guide is
[Reading from cloud storage](./cloud-storage.md); it also covers loading
`flapi.yaml` itself from a bucket.

## 11. Extensions

**The default set.** At startup flAPI loads `duckdb.default-extensions`, which is
`[httpfs, ducklake, fts, json, postgres, sqlite, parquet]` unless you set it.
Setting it **replaces** the list; `[]` loads none:

```yaml
duckdb:
  default-extensions: [httpfs, ducklake, sqlite, iceberg]
```

**What ships.** `core_functions`, `parquet`, `json` and `icu` are built into the
binary and never downloaded. Release archives and the Docker image carry `httpfs`,
`ducklake`, `fts`, `postgres_scanner` and `sqlite_scanner` in an `extensions/`
directory next to the binary; the `-runtime` image adds `bigquery`. The `flapi-io`
wheel carries none, so a pip-installed flAPI downloads the defaults on its first
start.

**Everything else.** DuckDB autoloads (downloads and loads on first use) only its
own known core extensions, such as `iceberg`, `delta` or `mysql`. Community
extensions (`bigquery`, `snowflake`) and `erpl` are never autoloaded: they need an
explicit `INSTALL` and `LOAD` in an `init`.

**Pre-installing.** `flapi extensions install` installs what a config needs (the
default list plus every `INSTALL` in a connection `init`) into a directory, and
exits. Use any directory you can write to:

```bash
flapi extensions install --config flapi.yaml --dir ./extensions
```

It prints one `✓` line per extension and exits `1` if any failed. Names work too
(`flapi extensions install --dir ./extensions iceberg bigquery@community`). The
directory is keyed by DuckDB version and platform, so install with the same `flapi`
binary you deploy.

**Offline hosts.** At runtime point flAPI at that directory and forbid downloads:

```bash
export FLAPI_EXTENSION_DIRECTORY="$PWD/extensions"
export FLAPI_EXTENSIONS_OFFLINE=1
flapi -c flapi.yaml
```

`FLAPI_EXTENSION_DIRECTORY` is used when `flapi.yaml` sets no
`duckdb.extension_directory`. With `FLAPI_EXTENSIONS_OFFLINE=1` (or `true`, `yes`,
`on`) nothing is downloaded, and a default extension that is not installed stops
startup with `Extension(s) not installed and FLAPI_EXTENSIONS_OFFLINE forbids
downloading: ...`. Without it a missing default extension is only a warning.

**In a packed binary.** `flapi pack` folds a config directory into a copy of the
flapi executable, giving one file to deploy. `flapi pack --in . --out flapi-prod
--with-extensions` also installs the extension set for the platform `pack` runs on
and embeds it; the packed binary extracts it once to
`$TMPDIR/flapi-extensions/ext-<hash>/` and starts offline. See
[Shipping one self-contained binary](./self-packaging.md).

## 12. Remote configs and `duckdb-hardening`

When `flapi.yaml` is loaded from a bucket or a URL (`flapi -c s3://...`), templates
run with DuckDB's external access **restricted**. A local config can opt in:

```yaml
duckdb-hardening:
  external-access: restricted       # auto (default: restricted only for a remote config) | full | restricted
  allowed-directories: [/srv/reference-data/]
```

What it means for data sources:

- **Templates read only allowed places:** the config's own directory or prefix,
  `template.path`, the DuckLake `data-path`, every attached database file, and
  `allowed-directories` / `allowed-paths`. Symlinks are resolved first, so a
  symlinked `data/` pointing elsewhere is refused until its target is listed.
- **`init` runs before the policy, with full access.** Its `ATTACH`es and secrets
  keep working; a template cannot `ATTACH`, `INSTALL`, `LOAD` or `CREATE SECRET`.
- **Extensions are preloaded, then frozen.** flAPI loads `httpfs`, `ducklake`, every
  extension an `init` loads or installs (and `azure`/`aws` when needed); nothing
  loads afterwards. An extension only a template uses (`iceberg_scan`,
  `read_xlsx`, ...) must be in `duckdb.default-extensions` or loaded in an `init`.

The full rules are in [CONFIG_REFERENCE § 2.4.1](../CONFIG_REFERENCE.md#241-duckdb-hardening-duckdb-hardening).

## 13. Live query or cache?

An endpoint without a cache runs its template against the source on every request.
With a cache, a scheduled refresh copies the data into a **DuckLake** table (flAPI's
local, snapshot-versioned cache store) and requests read only that table; see
[Caching an expensive query](./caching.md).

| | Live query | Cache |
|---|---|---|
| Source queries per request | 1, plus a `SELECT COUNT(*)` for `total_count` when the page is full (`total-count: false` drops it) | 0; one populate run per `schedule` |
| Freshness | current | as of the last refresh |
| Source down | that request fails (500) | the last snapshot is still served |
| `/health` | knows nothing about the source; only `stalled` (below) | `starting` while the first build runs, `degraded` when it failed or is stale |

| Source | Usually |
|---|---|
| Local Parquet/CSV/JSON, SQLite, DuckDB file | live |
| Postgres / MySQL near the server, with indexes | live; cache if the query is heavy or the database must be protected |
| BigQuery, Snowflake (billed per query or per warehouse minute) | cache: a live paginated endpoint can cost two billed queries per page |
| SAP via ERPL | cache |
| Files in object storage | live for small reads; cache for wide globs |

## 14. When a source fails

- **At startup**, a failing `init` (unreachable database, missing extension, bad
  credentials) stops the server, so an orchestrator sees a crash loop instead of a
  half-working instance.
- **After startup**, a source that goes away makes each request that touches it
  fail with a 500 whose `request_id` leads to the error in the log; endpoints on
  other sources keep working. Whether DuckDB's postgres or mysql extension
  reconnects by itself after the database comes back is not verified here; if
  requests keep failing, restart flAPI.
- **There is no query timeout.** A REST query runs until the source answers.
  `stall-timeout-s` (default 60 s) only makes `GET /health` answer `503` with
  `"status":"stalled"` while a request has been in flight longer than that, so a
  load balancer stops sending traffic; the query itself keeps running.
- **Rotating credentials needs a restart.** Secrets and `ATTACH`es are created by
  `init`, which runs only at startup; a config reload does not re-run it.

How to follow a `request_id` and read `/health`: [Observability](./observability.md).

## Common mistakes

- **`${VAR}` is not substituted.** flAPI does not support it, and it does not warn:
  `password: '${PG_PASSWORD}'` is the literal text `${PG_PASSWORD}`. Write
  `{{env.PG_PASSWORD}}`.
- **An environment variable that is not whitelisted stops startup.** Every
  `{{env.NAME}}` in a YAML file must match a pattern under
  `template.environment-whitelist` in the main `flapi.yaml`; an empty or missing
  whitelist allows none.
- **Relative paths have two bases.** A `properties` value starting with `./` or
  `../` resolves against the directory of `flapi.yaml`. A path inside `init` SQL is
  DuckDB's and resolves against the server's working directory. Start flAPI from
  another directory and an `init` path breaks while a property does not.
- **A failing `init` stops startup.** A typo, an extension that cannot be
  downloaded, an unreachable Postgres in `ATTACH`: the server does not start, and
  the log names the connection and the statement number.
- **`{{ conn.x }}` in `init`.** `init` is not rendered with the connection's
  properties; the braces reach DuckDB as text.
- **`LIMIT {{ params.limit }}` in a template.** `limit` and `offset` belong to
  pagination; see [Building a REST endpoint](./rest-endpoints.md).

## Next

- [Building a REST endpoint](./rest-endpoints.md): parameters, validators, pagination
- [Caching an expensive query](./caching.md): serve a slow source from DuckLake
- [Reading from cloud storage](./cloud-storage.md): S3, GCS, Azure, remote configs
- [Observability](./observability.md): request ids, `/health`, what DuckDB fetches
- [Shipping one self-contained binary](./self-packaging.md): pack config, data and extensions

## How it works

- [CONFIG_REFERENCE § 2.3 Connections](../CONFIG_REFERENCE.md#23-connections),
  [§ 2.4 DuckDB](../CONFIG_REFERENCE.md#24-duckdb-configuration) (default extensions and
  where they come from), [§ 2.4.1 DuckDB hardening](../CONFIG_REFERENCE.md#241-duckdb-hardening-duckdb-hardening),
  [§ 2.2 the environment whitelist](../CONFIG_REFERENCE.md#22-template-configuration)
- [CLI_REFERENCE § 3](../CLI_REFERENCE.md#3-self-packaging-subcommands): `flapi extensions install`, `flapi pack`
- Source: `src/config_manager.cpp` (`parseConnections`, `parseDuckDBConfig`,
  `makePathRelativeToBasePathIfNecessary`), `src/database_manager.cpp`
  (`loadDefaultExtensions`, `initializeConnections`, `applyExternalAccessPolicy`,
  `lockSerialisedAccess`), `src/extension_installer.cpp` (`DefaultExtensions`,
  `CollectExtensionSpecs`), `src/sql_template_processor.cpp` (`createTemplateContext`:
  `conn.*` from the first connection)
