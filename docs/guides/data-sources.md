# Connecting data sources

**Goal:** point flAPI at your data (local files, SQLite, a database server, a
warehouse, a lakehouse table or SAP) and query it from an endpoint.

**Before you start:** a working project from [Getting started](./getting-started.md).
Every runnable example below uses the files in `examples/data` of this repository
(`customers.parquet`, `northwind.sqlite`) or small files the guide asks you to create.
Run them from the directory that holds `flapi.yaml`.

flAPI has no drivers of its own. All connections share one embedded DuckDB 2.0.1,
and DuckDB and its extensions do the reading. A connection is a name plus two
optional parts: properties your templates can read, and SQL that sets DuckDB up.

---

## 1. How a connection works

```yaml
# flapi: config group=basics
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
  `../` is turned into an absolute path under the directory of `flapi.yaml`, so the
  server can start from any working directory.
- **`init`** is DuckDB SQL that runs **once, at startup**, before the server takes
  requests, in the one DuckDB instance every connection shares. Use it to load
  extensions, create secrets and `ATTACH` databases. It is not a template:
  `{{ conn.path }}` in `init` stays literal text. `{{env.NAME}}` does work, because
  it is substituted in the whole YAML file (step 5).
- An endpoint names its connection in **`connection: [name]`**. A name that is not
  under `connections:` fails validation (`Connection 'x' not found in configuration`).

```yaml
# flapi: endpoint group=basics file=customers.yaml
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
# flapi: check GET /customers?segment=BUILDING&limit=2 -> 200 contains "BUILDING"
```

```sql
-- flapi: template group=basics file=customers.sql
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.segment}}
  AND c_mktsegment = '{{{ params.segment }}}'
{{/params.segment}}
ORDER BY c_custkey
```

```bash
flapi -c flapi.yaml
curl 'http://localhost:8080/customers?segment=BUILDING&limit=2'
```

```json
{"data":[{"c_custkey":1,"c_name":"Customer#000000001","c_mktsegment":"BUILDING"},
          {"c_custkey":8,"c_name":"Customer#000000008","c_mktsegment":"BUILDING"}],
 "next":"/customers?offset=2&limit=2&segment=BUILDING","total_count":337}
```

Everything an `init` creates belongs to the whole process: the `nw` database
attached above can be read by any template, whatever its `connection:` says. A
statement in `init` that fails stops startup and names the connection. With
`ATACH` mistyped for `ATTACH`:

```text
[error] Database initialization error: Error creating database, Details: connection 'northwind' init: Failed to parse init statements: Parser Error: syntax error at or near <redacted>
[error] flAPI cannot start until the database can be opened.
```

(Literals in the message are redacted, because an `init` often carries
credentials.) A statement that parses but fails names its position:
`Failed to prepare init statement 2 of 3: ...`.

## 2. Local files: Parquet, CSV, JSON and globs

A file needs no `init`: put its path in `properties` and read it with DuckDB's
`read_parquet`, `read_csv` or `read_json`. A glob (`*`, `**`) reads many files as
one table. Create these three files next to `flapi.yaml`:

```text
# flapi: file group=files file=files/orders/2026-01.csv
order_id,customer_id,order_date,amount
1,ALFKI,2026-01-05,120.50
2,ANATR,2026-01-17,64.00
3,ALFKI,2026-01-28,310.25
```

```text
# flapi: file group=files file=files/orders/2026-02.csv
order_id,customer_id,order_date,amount
4,BERGS,2026-02-02,89.90
5,ALFKI,2026-02-14,42.00
```

```text
# flapi: file group=files file=files/events.jsonl
{"id": 1, "kind": "signup", "customer_id": "ALFKI", "ts": "2026-01-03T10:00:00"}
{"id": 2, "kind": "login", "customer_id": "ALFKI", "ts": "2026-01-04T08:12:00"}
{"id": 3, "kind": "signup", "customer_id": "BERGS", "ts": "2026-01-30T16:45:00"}
```

```yaml
# flapi: config group=files
project-name: files-demo
project-description: CSV and JSON files
template:
  path: ./sqls
connections:
  orders:
    properties:
      path: ./files/orders/*.csv        # a glob: every month's file
  events:
    properties:
      path: ./files/events.jsonl
  northwind:
    init: |
      INSTALL sqlite;
      LOAD sqlite;
      ATTACH IF NOT EXISTS './data/northwind.sqlite' AS nw (TYPE sqlite, READ_ONLY);
```

```yaml
# flapi: endpoint group=files file=orders.yaml
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
# flapi: check GET /orders -> 200 contains "order_id":5,"
# flapi: check GET /orders?month=2026-02 -> 200 contains "BERGS"
# flapi: check GET /orders?month=February -> 400
```

```sql
-- flapi: template group=files file=orders.sql
SELECT order_id, customer_id, order_date, amount,
       regexp_extract(filename, '([0-9]{4}-[0-9]{2})\.csv$', 1) AS month
FROM read_csv('{{{ conn.path }}}', header = true, filename = true)
WHERE 1=1
{{#params.month}}
  AND regexp_extract(filename, '([0-9]{4}-[0-9]{2})\.csv$', 1) = '{{{ params.month }}}'
{{/params.month}}
ORDER BY order_id
```

`filename = true` adds a `filename` column naming the file each row came from;
here it gives each order its month. `curl 'http://localhost:8080/orders?month=2026-02'`:

```json
{"data":[{"order_id":4,"customer_id":"BERGS","order_date":"2026-02-02","amount":89.9,"month":"2026-02"},
          {"order_id":5,"customer_id":"ALFKI","order_date":"2026-02-14","amount":42,"month":"2026-02"}],
 "next":"","total_count":2}
```

JSON works the same way. `read_json` detects newline-delimited JSON by itself:

```yaml
# flapi: endpoint group=files file=events.yaml
url-path: /events
request:
  - field-name: kind
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [signup, login]
template-source: events.sql
connection: [events]
# flapi: check GET /events?kind=signup -> 200 contains "BERGS"
```

```sql
-- flapi: template group=files file=events.sql
SELECT id, kind, customer_id, ts
FROM read_json('{{{ conn.path }}}')
WHERE 1=1
{{#params.kind}}
  AND kind = '{{{ params.kind }}}'
{{/params.kind}}
ORDER BY id
```

`curl 'http://localhost:8080/events?kind=signup'` (DuckDB detected `ts` as a timestamp):

```json
{"data":[{"id":1,"kind":"signup","customer_id":"ALFKI","ts":"2026-01-03T10:00:00.000000"},
          {"id":3,"kind":"signup","customer_id":"BERGS","ts":"2026-01-30T16:45:00.000000"}],
 "next":"","total_count":2}
```

Parquet, CSV and JSON readers are built into flAPI; nothing is downloaded for
them. DuckDB pushes the columns you select and your filters into the Parquet reader,
so it reads only those columns and skips row groups whose statistics rule a filter out. For
Hive-partitioned layouts (`./data/events/year=*/month=*/*.parquet`), DuckDB turns
the directory names into columns you can filter on.

Files in a bucket (`s3://`, `gs://`, `az://`) or behind `https://` are read the
same way once credentials are in place: see [Reading from cloud storage](./cloud-storage.md).

## 3. SQLite

SQLite is attached in `init`, as the `northwind` connection in step 1 does. The
`basics` project already has it; add an endpoint:

```yaml
# flapi: endpoint group=basics file=products.yaml
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
# flapi: check GET /products?category_id=1&limit=2 -> 200 contains "Chai"
```

```sql
-- flapi: template group=basics file=products.sql
SELECT p.ProductID, p.ProductName, c.CategoryName, p.UnitPrice
FROM nw.Products p
JOIN nw.Categories c ON c.CategoryID = p.CategoryID
WHERE 1=1
{{#params.category_id}}
  AND p.CategoryID = {{ params.category_id }}
{{/params.category_id}}
ORDER BY p.ProductID
```

`curl 'http://localhost:8080/products?category_id=1&limit=2'`:

```json
{"data":[{"ProductID":1,"ProductName":"Chai","CategoryName":"Beverages","UnitPrice":18},
          {"ProductID":2,"ProductName":"Chang","CategoryName":"Beverages","UnitPrice":19}],
 "next":"/products?offset=2&category_id=1&limit=2","total_count":12}
```

Two things to know:

- **A path inside `init` is not rewritten.** `'./data/northwind.sqlite'` is
  resolved by DuckDB against the server's **working directory**, not the
  directory of `flapi.yaml`. Use an absolute path in production. A SQLite file
  that is not there does not stop startup: the first request fails with
  `Unable to open database "./data/northwind.sqlite"` in the log.
- **One query at a time.** A connection whose `init` attaches SQLite is serialised
  (a read and a write at the same moment deadlock a SQLite attachment). Other
  connections still run in parallel. `serialize-access: false` turns it off; see
  [CONFIG_REFERENCE § 2.3](../CONFIG_REFERENCE.md#23-connections).

A DuckDB file is attached the same way (`ATTACH '/srv/data/warehouse.duckdb' AS
wh (READ_ONLY)`), but the server then holds its lock for as long as it runs, so
no other process can write it.

## 4. Several connections in one template

A template can read everything the process has: every attached database and every
file. `connection:` decides two things only. **`conn.*` comes from the first
connection in the list**, and **every listed connection that is serialised takes
its lock** for the query. So name every connection the template reads, the one
whose `conn.*` you use first.

Join the CSV orders from step 2 to the Northwind customers in SQLite:

```yaml
# flapi: endpoint group=files file=order-customers.yaml
url-path: /order-customers
template-source: order-customers.sql
connection: [orders, northwind]         # conn.path is the orders glob
# flapi: check GET /order-customers -> 200 contains "Alfreds Futterkiste"
```

```sql
-- flapi: template group=files file=order-customers.sql
SELECT o.order_id, o.amount, c.CompanyName, c.Country
FROM read_csv('{{{ conn.path }}}', header = true) o
JOIN nw.Customers c ON c.CustomerID = o.customer_id
ORDER BY o.order_id
```

`curl 'http://localhost:8080/order-customers?limit=2'`:

```json
{"data":[{"order_id":1,"amount":120.5,"CompanyName":"Alfreds Futterkiste","Country":"Germany"},
          {"order_id":2,"amount":64,"CompanyName":"Ana Trujillo Emparedados y helados","Country":"Mexico"}],
 "next":"/order-customers?offset=2&limit=2","total_count":5}
```

For a second file, the template cannot use `conn.*` of the second connection.
Either write its path into the template, or give that connection an `init` that
creates a view (`CREATE OR REPLACE VIEW customers AS SELECT * FROM
read_parquet('/srv/data/customers.parquet')`) and query the view.

## 5. Postgres and MySQL

*Not run here; needs a reachable server.*

Credentials come from the environment. Write `{{env.NAME}}` anywhere in the YAML
and list the variable in `template.environment-whitelist` (regular expressions,
matched against the whole name):

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
- `postgres` is one of flAPI's default extensions; `mysql` is not, so its
  `INSTALL` downloads it on first start (step 10 covers offline hosts).
- Prefer `CREATE OR REPLACE SECRET` (in memory) to `PERSISTENT`, which writes the
  secret to `~/.duckdb/stored_secrets`, where every template can use it.

A variable that the whitelist does not match stops startup:

```text
[error] Configuration error: ... environment variable PG_PASSWORD referenced as {{env.NAME}} but not whitelisted.
Add a pattern matching it to `template.environment-whitelist` in the main configuration file; an empty or missing whitelist allows no variables.
```

## 6. BigQuery

*Not run here; needs a Google Cloud project.*

`bigquery` is a DuckDB community extension. It is pre-installed only in the
`-runtime` Docker image (`ghcr.io/datazoode/flapi:<tag>-runtime`); a release
archive, the `flapi-io` wheel and the default image must install it, which the
`INSTALL` below does on first start:

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

```sql
SELECT order_id, total FROM bq.{{{ conn.dataset }}}.orders
WHERE order_date >= '{{{ params.since }}}'
```

The extension authenticates with Google Application Default Credentials: set
`GOOGLE_APPLICATION_CREDENTIALS` to a service-account key file, or run where a
metadata server provides them. The extension reads that variable itself, so it
needs no whitelist entry. BigQuery bills every query; for an API, serve from a
cache (step 12).

## 7. Iceberg and Delta tables

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

## 8. SAP via ERPL

*Not run here; needs an SAP system and the SAP NetWeaver RFC SDK on the host.*

[ERPL](https://erpl.io) is a DuckDB extension from its own repository, and it is
**unsigned**. flAPI always starts DuckDB with `allow_unsigned_extensions`
enabled, so it loads:

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

## 9. Object storage

S3, GCS, Azure Blob and HTTPS files are read with `httpfs` (a default extension)
and credentials from the environment or a DuckDB secret. That guide is
[Reading from cloud storage](./cloud-storage.md); it also covers loading
`flapi.yaml` itself from a bucket.

## 10. Extensions

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
start. Anything else (`mysql`, `iceberg`, `delta`, `bigquery`, `erpl`) is
downloaded by the `INSTALL` in your `init`, or by DuckDB's autoload the first time
a template uses it.

**Pre-installing.** `flapi extensions install` installs what a config needs, the
default list plus every `INSTALL` in a connection `init`, and exits:

```bash
flapi extensions install --config flapi.yaml --dir /opt/flapi/extensions
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
✓ bigquery@community
```

Names work too (`flapi extensions install --dir /opt/flapi/extensions iceberg
bigquery@community`). The directory is keyed by DuckDB version and platform, so
install with the same `flapi` binary you deploy.

**Offline hosts.** At runtime point flAPI at that directory and forbid downloads:

```bash
export FLAPI_EXTENSION_DIRECTORY=/opt/flapi/extensions
export FLAPI_EXTENSIONS_OFFLINE=1
flapi -c flapi.yaml
```

`FLAPI_EXTENSION_DIRECTORY` is used when `flapi.yaml` sets no
`duckdb.extension_directory`. With `FLAPI_EXTENSIONS_OFFLINE=1` (or `true`, `yes`,
`on`) nothing is downloaded, and a default extension that is not installed stops
startup with `Extension(s) not installed and FLAPI_EXTENSIONS_OFFLINE forbids
downloading: ...`. Without it a missing default extension is only a warning.

**In a packed binary.** `flapi pack --in . --out flapi-prod --with-extensions`
installs the same set for the platform `pack` runs on and embeds it. The packed
binary extracts it once to `$TMPDIR/flapi-extensions/ext-<hash>/` and starts
offline. See [Shipping one self-contained binary](./self-packaging.md).

## 11. Remote configs and `duckdb-hardening`

When `flapi.yaml` is loaded from a bucket or a URL (`flapi -c s3://...`), templates
run with DuckDB's external access **restricted**: they may read and write only
under the config's own prefix, `template.path`, the DuckLake `data-path` and what
you list. A local config runs with full access unless you ask for the same:

```yaml
# flapi: config group=hardened
project-name: hardened-demo
project-description: Restricted external access
template:
  path: ./sqls
duckdb-hardening:
  external-access: restricted       # auto (default) | full | restricted
  allowed-directories:              # extra prefixes besides the config's own directory
    - /srv/reference-data/
  allowed-paths:                    # extra single files
    - /srv/lookup/countries.csv
connections:
  orders:
    properties:
      path: ./files/orders.csv
```

```text
# flapi: file group=hardened file=files/orders.csv
order_id,customer_id,amount
1,ALFKI,120.50
2,ANATR,64.00
```

```yaml
# flapi: endpoint group=hardened file=orders.yaml
url-path: /orders
template-source: orders.sql
connection: [orders]
# flapi: check GET /orders -> 200 contains "ANATR"
```

```sql
-- flapi: template group=hardened file=orders.sql
SELECT order_id, customer_id, amount
FROM read_csv('{{{ conn.path }}}', header = true)
ORDER BY order_id
```

Startup logs the policy (`external access: restricted (duckdb-hardening.external-access:
restricted; allowed: ...)`), and a template that reads anywhere else answers 500
with `Permission Error: Cannot access file "/etc/hostname" - file system operations
are disabled by configuration` in the log. What this means for data sources:

- **`init` runs before the policy, with full access.** Its `ATTACH`es and secrets
  keep working, and DuckDB allows every attached database file. A template cannot
  `ATTACH`, `INSTALL`, `LOAD` or `CREATE SECRET` itself.
- **Extensions are preloaded.** Before restricting, flAPI loads `httpfs`,
  `ducklake` (cache on), `azure` (something names `az://`), every extension an
  `init` loads or installs, and `aws` for a `credential_chain` secret. After that
  nothing can load, so an extension that only a template uses (`iceberg_scan`,
  `delta_scan`, `read_xlsx`, ...) must be in `duckdb.default-extensions` or loaded
  in an `init`.
- **Other places** go into `allowed-directories` (prefixes) or `allowed-paths`
  (single files). Relative entries resolve against the **working directory**; URLs
  such as `s3://reference-data/` are kept as written. DuckDB checks the resolved
  path, so a symlinked `data/` that points outside the allowed prefixes is refused
  until its target is listed.

The full rules are in [CONFIG_REFERENCE § 2.4.1](../CONFIG_REFERENCE.md#241-duckdb-hardening-duckdb-hardening).

## 12. Live query or cache?

An endpoint without a cache runs its template against the source on every request.
That is right for local files, SQLite and a nearby database. For a warehouse that
bills per query (BigQuery, Snowflake), a slow system (SAP) or a source you must
not load, materialise the data into a DuckLake table on a schedule and serve
requests from it: [Caching an expensive query](./caching.md).

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
