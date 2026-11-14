# Connection recipes

Each recipe is a `connections:` entry for `flapi.yaml`. Rules that apply to all of them:

- `init` is DuckDB SQL. It runs **once at startup**, in the one DuckDB instance that every
  connection shares. A statement that fails stops the server.
- Relative paths inside `init` resolve against the **working directory** of the process.
  Relative values in `properties` (starting `./` or `../`) resolve against the directory of
  `flapi.yaml`.
- Credentials come from the environment as `{{env.NAME}}`, and every `NAME` must match a
  regex in `template.environment-whitelist`. Otherwise startup fails with
  `environment variable NAME referenced as {{env.NAME}} but not whitelisted`.
- Use `CREATE OR REPLACE SECRET` (temporary, in memory). Avoid `PERSISTENT`, which writes
  the secret to `~/.duckdb/stored_secrets` on disk.
- The default extensions (`duckdb.default-extensions`) are `httpfs, ducklake, fts, json,
  postgres, sqlite, parquet`. Anything else (mysql, azure, bigquery, snowflake) needs an
  `INSTALL` in `init`. For offline hosts, pre-install it (`flapi extensions install`, see
  `flapi-deploy`).

## Parquet, CSV, JSON (local files or globs)

```yaml
connections:
  customers:
    properties:
      path: ./data/customers.parquet        # or ./data/2026-*.parquet
  events:
    properties:
      path: ./data/events.jsonl
```

```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
-- read_csv('{{{ conn.path }}}', header = true)   read_json('{{{ conn.path }}}')
```

A complete CSV project (the CSV is written by the tag below):

```yaml
# flapi: config group=csv
project-name: cities-api
project-description: Cities from a CSV file
template:
  path: ./sqls
connections:
  cities:
    properties:
      path: ./csv/cities.csv
```

```text
# flapi: file group=csv file=csv/cities.csv
name,country,population
Berlin,DE,3850809
Hamburg,DE,1945532
Paris,FR,2102650
```

```yaml
# flapi: endpoint group=csv file=cities.yaml
url-path: /cities
request:
  - field-name: country
    field-in: query
    required: false
    validators:
      - type: string
        regex: '^[A-Z]{2}$'
template-source: cities.sql
connection: [cities]
# flapi: check GET /cities?country=DE -> 200
# flapi: check GET /cities?country=germany -> 400
```

```sql
-- flapi: template group=csv file=cities.sql
SELECT name, country, population
FROM read_csv('{{{ conn.path }}}', header = true)
WHERE 1=1
{{#params.country}}
  AND country = '{{{ params.country }}}'
{{/params.country}}
ORDER BY population DESC
```

## SQLite

```yaml
connections:
  northwind:
    init: |
      INSTALL sqlite;
      LOAD sqlite;
      ATTACH IF NOT EXISTS '/srv/data/northwind.sqlite' AS nw (TYPE sqlite, READ_ONLY);
```

Templates query `nw.Products`, `nw."Order Details"` and so on. flAPI serialises access to
SQLite connections automatically. Set `serialize-access: false` only if you have measured
that concurrent access is safe. A SQLite file **cannot** be read from a packed bundle
(`embed://`), because SQLite opens a real file. See `flapi-deploy`.

## Postgres

```yaml
template:
  path: ./sqls
  environment-whitelist:
    - '^PG_(HOST|USER|PASSWORD)$'
connections:
  shop:
    init: |
      INSTALL postgres;
      LOAD postgres;
      CREATE OR REPLACE SECRET pg_shop (TYPE postgres, HOST '{{env.PG_HOST}}', PORT 5432,
        DATABASE 'shop', USER '{{env.PG_USER}}', PASSWORD '{{env.PG_PASSWORD}}');
      ATTACH '' AS pg (TYPE postgres, SECRET pg_shop, READ_ONLY);
```

```sql
SELECT order_id, total FROM pg.public.orders WHERE customer_id = {{ params.customer_id }}
```

`ATTACH` connects at startup, so an unreachable database stops the server. Filters and
projections are pushed down to Postgres. `postgres_query('pg', 'SELECT ...')` runs raw SQL
on the remote side.

## MySQL

```yaml
connections:
  crm:
    init: |
      INSTALL mysql;
      LOAD mysql;
      CREATE OR REPLACE SECRET my_crm (TYPE mysql, HOST '{{env.MYSQL_HOST}}', PORT 3306,
        DATABASE 'crm', USER '{{env.MYSQL_USER}}', PASSWORD '{{env.MYSQL_PASSWORD}}');
      ATTACH '' AS crm (TYPE mysql, SECRET my_crm, READ_ONLY);
```

Whitelist `'^MYSQL_(HOST|USER|PASSWORD)$'`. `mysql` is a core DuckDB extension, but it is
not in flAPI's default list.

## BigQuery (community extension)

```yaml
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
SELECT * FROM bq.{{{ conn.dataset }}}.orders WHERE order_date >= '{{{ params.since }}}'
```

Authentication uses Google Application Default Credentials: set
`GOOGLE_APPLICATION_CREDENTIALS` to a service-account key file, or use the platform's
metadata server. That variable is read by the extension, not through `{{env}}`, so it needs
no whitelist entry. Whitelist `'^GCP_PROJECT$'`. The `-runtime` Docker image ships
`bigquery` pre-installed.

## Snowflake (community extension)

```yaml
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

The extension needs the **ADBC Snowflake driver** (`libadbc_driver_snowflake.so` on Linux)
installed on the host. Without it, `ATTACH` fails with `ADBC Snowflake driver ... not
found`, and the server does not start. `snowflake_query(...)` is also available for raw
pass-through SQL.

## S3 (and S3-compatible)

```yaml
template:
  path: ./sqls
  environment-whitelist:
    - '^AWS_(ACCESS_KEY_ID|SECRET_ACCESS_KEY)$'
connections:
  events:
    init: |
      CREATE OR REPLACE SECRET s3_events (TYPE s3, KEY_ID '{{env.AWS_ACCESS_KEY_ID}}',
        SECRET '{{env.AWS_SECRET_ACCESS_KEY}}', REGION 'eu-central-1');
    properties:
      path: s3://my-bucket/events/*.parquet
```

```sql
SELECT * FROM read_parquet('{{{ conn.path }}}') WHERE event_date >= '{{{ params.since }}}'
```

`httpfs` is a default extension. When the server starts, it also copies `AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `AWS_REGION`, `AWS_SESSION_TOKEN` and `AWS_ENDPOINT_URL` from its
environment into DuckDB's `s3_*` settings. That lets an IAM-provided environment work
without a secret. An explicit `CREATE SECRET` is clearer and can be scoped with `SCOPE
's3://bucket'`. `PROVIDER credential_chain` only validates where AWS credentials exist; on a
laptop without them it stops startup.

## GCS

```yaml
connections:
  exports:
    init: |
      CREATE OR REPLACE SECRET gcs_exports (TYPE gcs, KEY_ID '{{env.GCS_HMAC_KEY}}',
        SECRET '{{env.GCS_HMAC_SECRET}}');
    properties:
      path: gs://my-bucket/exports/*.parquet
```

DuckDB reads `gs://` through `httpfs` with **HMAC keys** (whitelist
`'^GCS_HMAC_(KEY|SECRET)$'`). On Cloud Run you can mount the bucket as a volume and read the
local path instead. See `flapi-deploy`.

## Azure Blob

```yaml
connections:
  blobs:
    init: |
      INSTALL azure;
      LOAD azure;
      CREATE OR REPLACE SECRET az_blobs (TYPE azure,
        CONNECTION_STRING '{{env.AZURE_STORAGE_CONNECTION_STRING}}');
    properties:
      path: az://my-container/data/*.parquet
```

`azure` is not a default extension and is not in the `-runtime` image. Whitelist
`'^AZURE_STORAGE_CONNECTION_STRING$'`.

## Other connection keys

| Key | Default | Meaning |
|---|---|---|
| `init` | `""` | DuckDB SQL run once at startup |
| `properties` | `{}` | string map exposed to templates as `conn.<key>` |
| `serialize-access` | auto (true when `init` attaches SQLite) | one query at a time on this connection |
| `log-queries`, `log-parameters` | `false` | parsed but have no effect today (nothing reads them) |
