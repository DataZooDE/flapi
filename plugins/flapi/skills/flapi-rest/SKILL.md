---
name: flapi-rest
description: >
  Building flAPI REST endpoints: endpoint YAML (url-path, method, request
  fields, defaults), validators, Mustache SQL templates (prepared binding vs
  triple braces, optional filters), path parameters, pagination and the
  response envelope, write endpoints (POST/PUT/PATCH, operation, RETURNING,
  transactions), YAML includes, env vars, per-endpoint rate limits and OpenAPI.
  Use when a user wants to expose SQL as an HTTP endpoint, add a filter or
  validator, paginate, write rows from an API, or debug a 400/404/500.
version: 26.10.31
user-invocable: false
---

# flAPI REST Endpoints Cheat Sheet

**flAPI:** v26.10.31 | **Skill:** Develop | **See also:** `flapi-setup`, `flapi-auth`, `flapi-mcp`, `flapi-testing`, `flapi-cache-design`

An endpoint is a YAML file plus a SQL (Mustache) template, both under
`template.path` (usually `./sqls`). Every `.yaml` there that has `url-path`
becomes a route.

## Critical gotchas

- **`{{ params.x }}` outside a section is a bound `?` parameter**, but only when
  the field has a typed validator (`int`, `number`, `boolean`, `date`, `time`,
  `string`, `enum`, `email`, `uuid`). Never quote it: `'{{ params.name }}'`
  becomes the literal string `'?'`. Inside `{{#...}}...{{/...}}` it is NOT bound;
  it is pasted in, HTML-escaped and unquoted.
- **`{{{ params.x }}}` is raw text interpolation, everywhere.** Use it only for
  values pinned by an `enum` or a strict `regex` (identifiers, sort columns, or
  quoted literals inside sections).
- **Do not write `LIMIT`/`OFFSET` in a template.** `with-pagination` defaults to
  `true`, flAPI always wraps your query in `LIMIT <limit> OFFSET <offset>`
  (default `limit=100`), and an inner LIMIT corrupts `total_count` and makes
  `offset` pages come back empty. Set `with-pagination: false` to opt out.
- **`field-in` does not change where a value is read from.** GET reads the query
  string and path; POST/PUT/PATCH read the JSON body, then path, then query.
  `field-in: header` is never read from a header (it only shows in `/doc`).
- **Enum values go under `allowedValues:`; a string regex goes under `regex:`.**
  `values:` is a startup error ("Missing required key: allowedValues"); `pattern:`
  is silently ignored. `field-type:` is read by nothing, so use a validator.
- **`method: DELETE` never runs your template.** It invalidates the endpoint's
  cache (200) or answers 405. Write a delete as a POST/PUT/PATCH endpoint.
- **Endpoint YAML is read at startup**; template `.sql` edits apply on the next
  request. After changing YAML, restart or `flapii endpoints reload <path>`.
- **Routes match exactly**: `/customers/` is a 404 for `url-path: /customers`.
- **The global block is `rate_limit` (underscore); per endpoint it is
  `rate-limit` (hyphen)**, and it needs `enabled: true` (default false).

## Endpoint YAML anatomy

```yaml
url-path: /customers/:id        # required, starts with '/'; ':name' = path parameter
method: GET                      # GET (default) | POST | PUT | PATCH | DELETE
request:
  - field-name: id               # available as params.id
    field-in: path               # query | path | body | header  (documentation only, see gotchas)
    description: Customer key    # shown in /doc and the MCP schema
    required: true               # missing -> 400 "Required field is missing"
    validators:
      - type: int
        min: 1
  - field-name: currency
    field-in: query
    default: EUR                 # scalar; applied before validation when absent
    validators:
      - type: enum
        allowedValues: [EUR, USD]
template-source: customer.sql    # relative to this YAML file
connection: [customers]          # first entry feeds {{ conn.* }}
with-pagination: false           # default true
request-fields-validation: true  # GET: reject query params not declared (limit/offset allowed)
```

Validator types and options: [references/validators.md](references/validators.md).
A failed validator answers 400 `{"errors":[{"field":"id","message":"..."}]}`
before any SQL runs.

## Template variables

| Variable | Contains |
|---|---|
| `params.*` | request values, path params, defaults, plus `params.limit` / `params.offset` |
| `conn.*` | `properties` of the FIRST connection listed |
| `env.*` | environment variables matching `template.environment-whitelist` |
| `auth.*` | `username`, `roles` (comma-joined), `email`, `type`, `authenticated` (see `flapi-auth`) |
| `cache.*` | cache table metadata (see `flapi-cache-design`) |

There is no `context.` prefix: `{{ context.conn.path }}` renders empty.

## Filtered, paginated read (runnable)

```yaml
# flapi: endpoint group=customers file=customers-list.yaml
url-path: /customers
method: GET
request:
  - field-name: nation
    field-in: query
    description: Nation key 0-24
    validators:
      - type: int
        min: 0
        max: 24
  - field-name: min_balance
    field-in: query
    validators:
      - type: number
  - field-name: name
    field-in: query
    validators:
      - type: string
        regex: '^Customer#[0-9]{9}$'
  - field-name: sort
    field-in: query
    default: id
    validators:
      - type: enum
        allowedValues: [id, balance]
template-source: customers-list.sql
connection: [customers]
# flapi: check GET /customers?limit=2 -> 200
# flapi: check GET /customers?nation=5&min_balance=9000&sort=balance -> 200
# flapi: check GET /customers?name=Customer%23000000003 -> 200
# flapi: check GET /customers?sort=c_comment -> 400
# flapi: check GET /customers?nation=99 -> 400
# flapi: check GET /customers?limit=abc -> 400
```

```sql
-- flapi: template group=customers file=customers-list.sql
SELECT c_custkey AS id, c_name AS name, c_nationkey AS nation, c_acctbal AS balance
FROM read_parquet('{{{ conn.path }}}')
WHERE ({{ params.nation }} IS NULL OR c_nationkey = {{ params.nation }})
  AND ({{ params.min_balance }} IS NULL OR c_acctbal >= {{ params.min_balance }})
  AND ({{ params.name }} IS NULL OR c_name = {{ params.name }})
ORDER BY {{{ params.sort }}}
```

Every user value is a bound parameter; an absent optional binds NULL, so
`(? IS NULL OR col = ?)` switches the filter off. The only raw interpolation is
`sort`, which the enum pins to two identifiers.

```bash
curl 'http://localhost:8080/customers?nation=5&limit=2&offset=2'
# {"next":"/customers?offset=4&limit=2&nation=5&sort=id","total_count":57,"data":[...]}
# headers: X-Total-Count, X-Offset, X-Limit, X-Next
curl 'http://localhost:8080/customers?format=csv'   # or Accept: text/csv; Arrow via application/vnd.apache.arrow.stream
```

`limit`/`offset` must be plain non-negative integers (else 400). `next` is
empty on the last page. With `with-pagination: false` the body is a bare JSON
array and no count query runs.

### The section idiom (when the SQL itself must change)

```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.nation}}
  AND c_nationkey = {{{ params.nation }}}
{{/params.nation}}
{{^params.nation}}
  AND c_nationkey < 5
{{/params.nation}}
```

`{{#x}}` renders when `x` is present and non-empty, `{{^x}}` when it is absent.
Values inside sections are interpolated, never bound: keep them to `int`,
`number`, `enum` or strict-`regex` fields, and put string values in quotes
with triple braces (`'{{{ params.code }}}'`) only after such a validator.
Prefer the bound `(? IS NULL OR ...)` form above for plain filters.

## Path parameters (runnable)

```yaml
# flapi: endpoint group=customers file=customer-by-id.yaml
url-path: /customers/:id
method: GET
with-pagination: false
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
template-source: customer-by-id.sql
connection: [customers]
# flapi: check GET /customers/7 -> 200
# flapi: check GET /customers/abc -> 400
```

```sql
-- flapi: template group=customers file=customer-by-id.sql
SELECT c_custkey AS id, c_name AS name, c_phone AS phone, c_acctbal AS balance
FROM read_parquet('{{{ conn.path }}}')
WHERE c_custkey = {{ params.id }}
```

A missing row is `200 []`, not 404. Several parameters work too:
`/customers/:id/orders/:order_id` gives `params.id` and `params.order_id`.

## Write endpoints (runnable)

POST, PUT and PATCH are writes (`operation.type: write` is implied). Defaults:
`transaction: true` (BEGIN/COMMIT, ROLLBACK on error), `validate-before-write:
true` (missing required fields AND undeclared body keys are 400),
`returns-data: false` (rows from `RETURNING` are still returned). POST answers
201, PUT/PATCH 200, body `{"rows_affected": n, "data": [...]}`.

```yaml
# flapi: config group=notes
project-name: notes-api
project-description: Writable notes table in a local DuckDB file
template:
  path: ./sqls
connections:
  notes:
    init: |
      ATTACH IF NOT EXISTS './notes.duckdb' AS notesdb;
      CREATE TABLE IF NOT EXISTS notesdb.notes (
        id INTEGER PRIMARY KEY,
        title VARCHAR NOT NULL,
        done BOOLEAN DEFAULT false
      );
```

```yaml
# flapi: endpoint group=notes file=notes-create.yaml
url-path: /notes
method: POST
request:
  - field-name: id
    field-in: body
    required: true
    validators:
      - type: int
        min: 1
  - field-name: title
    field-in: body
    required: true
    validators:
      - type: string
        min-length: 1
        max-length: 200
        preventSqlInjection: false   # bound below, so apostrophes and brackets are safe
operation:
  type: write
  returns-data: true
  transaction: true
template-source: notes-create.sql
connection: [notes]
# flapi: check POST /notes body={"id":1,"title":"O'Brien (draft)"} -> 201
# flapi: check POST /notes body={"id":1,"title":"duplicate key"} -> 500
# flapi: check POST /notes body={"id":2,"title":"x","owner":"me"} -> 400
# flapi: check POST /notes body={"title":"no id"} -> 400
```

```sql
-- flapi: template group=notes file=notes-create.sql
INSERT INTO notesdb.notes (id, title)
VALUES ({{ params.id }}, {{ params.title }})
RETURNING id, title, done;
```

```yaml
# flapi: endpoint group=notes file=notes-update.yaml
url-path: /notes/:id
method: PATCH
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
  - field-name: done
    field-in: body
    required: true
    validators:
      - type: boolean
template-source: notes-update.sql
connection: [notes]
# flapi: check PATCH /notes/1 body={"done":true} -> 200
```

```sql
-- flapi: template group=notes file=notes-update.sql
UPDATE notesdb.notes SET done = {{ params.done }} WHERE id = {{ params.id }}
RETURNING id, title, done;
```

```yaml
# flapi: endpoint group=notes file=notes-list.yaml
url-path: /notes
method: GET
template-source: notes-list.sql
connection: [notes]
# flapi: check GET /notes -> 200
```

```sql
-- flapi: template group=notes file=notes-list.sql
SELECT id, title, done FROM notesdb.notes ORDER BY id
```

- A write template may hold several `;`-separated statements; each runs as its
  own prepared statement. With `returns-data: true` and a final `SELECT`, that
  SELECT's rows become `data`.
- A database error (constraint, missing table) is a 500 whose text is scrubbed
  of interpolated secrets; SQLite/DuckDB lock contention is a 503 + `Retry-After`.
- Deletes: `url-path: /notes/:id/delete`, `method: POST`, template
  `DELETE FROM notesdb.notes WHERE id = {{ params.id }};`.
- The `preventSqlInjection` screen rejects string values that contain SQL
  keywords, `--`, `/*`, `1=1`, or a quote next to an operator/parenthesis. Turn it
  off only for fields used exclusively through bound `{{ }}`.
- Cache write-through (`cache.invalidate-on-write`): see `flapi-cache-design`.

## YAML includes and environment variables

Includes are expanded as text before the YAML is parsed, so the file is not
valid YAML on its own. Paths are relative to the including file.

```yaml
# flapi: file group=includes file=common/shared.yaml
request:
  - field-name: nation
    field-in: query
    validators:
      - type: int
        min: 0
        max: 24
rate-limit:
  enabled: true
  max: 2
  interval: 60
```

```text
# flapi: endpoint group=includes file=by-nation.yaml
url-path: /by-nation
{{include:request from ../common/shared.yaml}}
{{include:rate-limit from ../common/shared.yaml}}
template-source: by-nation.sql
connection: [customers]
# flapi: check GET /by-nation?nation=5 -> 200
# flapi: check GET /by-nation?nation=6 -> 200
# flapi: check GET /by-nation?nation=7 -> 429
```

```sql
-- flapi: template group=includes file=by-nation.sql
SELECT count(*) AS customers FROM read_parquet('{{{ conn.path }}}')
WHERE ({{ params.nation }} IS NULL OR c_nationkey = {{ params.nation }})
```

- `{{include from file.yaml}}` splices a whole file; `{{include:key from file.yaml}}`
  one top-level key; append `if env.VAR` / `if !env.VAR` / `if true` for a
  conditional include. Includes do not recurse.
- `{{env.VAR}}` works in any YAML value, only if `VAR` matches a regex in
  `template.environment-whitelist` (root config). An unlisted name stops startup;
  an empty whitelist allows none; `${VAR}` is left as literal text; a
  `{{env.X}}` inside an included file is not substituted.

```yaml
template:
  path: ./sqls
  environment-whitelist: ['^FLAPI_.*', '^DB_PASSWORD$']
connections:
  warehouse:
    properties:
      password: '{{env.DB_PASSWORD}}'
```

In SQL templates the same whitelist applies: `{{{ env.FLAPI_REGION }}}`.

## Per-endpoint rate limit

```yaml
rate-limit:
  enabled: true      # default false once the block exists
  max: 100           # requests per window
  interval: 60       # seconds
  key: user-or-ip    # ip (default) | user | user-or-ip
```

Buckets are per route and method. Over the limit: 429, `Retry-After`,
`X-RateLimit-Limit/Remaining/Reset`; it is checked before auth and validation.
An endpoint with its own block (even `enabled: false`) ignores the global
`rate_limit:` block in `flapi.yaml`, which applies to every other endpoint.

## OpenAPI

`GET /doc` serves an HTML page, `GET /doc.yaml` the OpenAPI 3 document built
from every endpoint's `url-path`, `method`, request fields and `description`.

## Canonical endpoint workflow

1. Look at the data first: `duckdb -c "DESCRIBE SELECT * FROM 'data/x.parquet'"`.
2. Write `sqls/<name>.yaml`: `url-path`, `method`, one `request` entry per
   input with a typed validator, `template-source`, `connection`.
3. Write `sqls/<name>.sql`: bound `{{ params.x }}` with `(? IS NULL OR ...)` for
   optional filters, `{{{ }}}` only for enum/regex-pinned identifiers, no LIMIT.
4. `flapi -c flapi.yaml --validate-config` (catches YAML/key/connection errors,
   not SQL errors; see `flapi-testing`).
5. Start `flapi -c flapi.yaml --config-service`, then
   `flapii templates expand /<path> --params '{"x":"1"}'` to see the SQL and
   `flapii templates test /<path>` to run it.
6. `curl` the happy path, a 400 (bad value), and a page (`?limit=2&offset=2`).
7. Add a Tavern test (`flapi-testing`); add auth (`flapi-auth`) or an
   `mcp-tool:` block (`flapi-mcp`) as needed.
