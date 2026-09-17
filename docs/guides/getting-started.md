# Getting started

**Goal:** a working REST endpoint backed by your own SQL, in about five minutes.

You will not write any backend code. An endpoint is a YAML file plus a SQL file.

---

## 1. Install

```bash
pip install flapi-io     # installs both `flapi` (server) and `flapii` (CLI)
```

Or run the container, which needs nothing installed:

```bash
docker pull ghcr.io/datazoode/flapi:latest
```

Other options — release archives, building from source — are in the
[main README](../../Readme.md).

## 2. Create a project

```bash
flapii project init my-api
cd my-api
```

That scaffolds:

```
my-api/
├── flapi.yaml        # connections, server settings
├── sqls/
│   ├── sample.yaml   # an endpoint definition
│   └── sample.sql    # its SQL template
└── data/             # put your Parquet/CSV here
```

## 3. Point it at your data

Edit `flapi.yaml`:

```yaml
project-name: my-api

template:
  path: ./sqls

connections:
  my-data:
    properties:
      path: ./data/customers.parquet
```

A connection is any data source — a local file, S3, Postgres, BigQuery,
Snowflake. See [CONFIG_REFERENCE § 2.3](../CONFIG_REFERENCE.md) for every
supported shape, and [cloud storage](./cloud-storage.md) for S3/GCS/Azure.

## 4. Define the endpoint

`sqls/customers.yaml`:

```yaml
url-path: /customers
method: GET

request:
  - field-name: id
    field-in: query
    field-type: int
    required: false
    validators:
      - type: int
        min: 1

template-source: customers.sql
connection: [my-data]
```

`sqls/customers.sql`:

```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND customer_id = {{{ params.id }}}
{{/params.id}}
LIMIT 100
```

The `WHERE 1=1` idiom lets each filter be added conditionally. See
[Building a REST endpoint](./rest-endpoints.md) for the template rules that
matter — particularly the difference between `{{ }}` and `{{{ }}}`.

## 5. Run it

```bash
flapi -c ./flapi.yaml
```

```bash
curl 'http://localhost:8080/customers?id=123'
```

Open <http://localhost:8080/doc> for a Swagger UI generated from your config, and
<http://localhost:8080/doc.yaml> for the raw OpenAPI document.

## Where to go next

| You want to… | Read |
|---|---|
| Add validation, pagination, more parameter types | [REST endpoints](./rest-endpoints.md) |
| Let an AI agent call this | [MCP tools](./mcp-tools.md) |
| Make a slow query fast | [Caching](./caching.md) |
| Require a token | [Authentication](./authentication.md) |
| Ship it as one binary | [Self-packaging](./self-packaging.md) |
| See traces and audit records | [Observability](../OBSERVABILITY.md) |

## How it works

A request enters a Crow middleware chain, resolves to an endpoint from a
copy-on-write configuration snapshot, has its parameters validated, renders the
Mustache template, executes on an embedded DuckDB, and serialises the result.

- [spec/REQUEST_LIFECYCLE.md](../spec/REQUEST_LIFECYCLE.md) — the whole path, with diagrams
- [spec/ARCHITECTURE.md](../spec/ARCHITECTURE.md) — how the components fit together
