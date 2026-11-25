# Writing SQL templates

**Goal:** write flAPI SQL templates that return the right rows and cannot be
turned against your database.

**Before you start:** a running project, as in [Getting started](./getting-started.md).
The examples use its `customers` connection (`./data/customers.parquet`, with
columns `c_custkey`, `c_name`, `c_nationkey`, `c_acctbal`, ...).

A template is a SQL file with [Mustache](https://mustache.github.io/) tags. On
every request flAPI validates the parameters, renders the template and runs the
result on DuckDB. Most of this guide is about one question: which values reach
DuckDB as **bound parameters**, and which as **SQL text**.

---

## 1. Write a first template

An endpoint is a YAML file and a SQL file in `sqls/`:

<!-- flapi: endpoint group=first file=customer.yaml -->
```yaml
url-path: /customers/:id
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
template-source: customer.sql
connection: [customers]
```
<!-- flapi: check GET /customers/7 -> 200 contains "Customer#000000007" -->
<!-- flapi: check GET /customers/abc -> 400 -->

<!-- flapi: template group=first file=customer.sql -->
```sql
SELECT c_custkey, c_name, c_nationkey, c_acctbal
FROM read_parquet('{{{ conn.path }}}')
WHERE c_custkey = {{ params.id }}
```

```bash
curl -s http://localhost:8080/customers/7
# {"data":[{"c_custkey":7,"c_name":"Customer#000000007",...}],"next":"","total_count":1}
curl -s http://localhost:8080/customers/abc
# {"errors":[{"message":"Invalid integer value","field":"id"}]}
```

The validator runs first: `abc` is rejected with `400` before any SQL is
rendered. The response is wrapped in `{data, next, total_count}` because GET
endpoints are paginated (step 6).

`params.*` and `conn.*` appear here. These are all the variables a template can use:

| Variable | Contains |
|---|---|
| `params.*` | Request parameters: query, path, header and body fields |
| `conn.*` | The `properties` of the endpoint's **first** connection (`conn.path` here) |
| `auth.*` | The caller, on an authenticated request (step 5) |
| `env.*` | Environment variables matching `template.environment-whitelist` (step 5) |
| `cache.*` | Cache table names, and in a cache populate template the snapshot and watermark values ([Caching](./caching.md)) |

There is no `context.*`: `{{{ context.auth.username }}}` and `{{{ context.conn.path }}}`
render as empty strings.

## 2. Look at the SQL flAPI runs

Start the server with the config service and ask it to render the template:

```bash
flapi -c flapi.yaml --config-service --config-service-token "$FLAPI_CONFIG_SERVICE_TOKEN"
flapii templates expand /customers/:id -p '{"id":"7"}'
```

```json
{
  "bindings": [{"position": 1, "name": "id", "type": "integer", "value": 7}],
  "expanded": "SELECT c_custkey, c_name, c_nationkey, c_acctbal\nFROM read_parquet('/srv/my-api/data/customers.parquet')\nWHERE c_custkey = ?\n"
}
```

`{{ params.id }}` became `?`, and `7` travels separately as an integer. DuckDB
runs this as a **prepared statement**, so the value can never change the
statement. `conn.path` was pasted in as text (a relative `path` comes out
as an absolute one).

The same answer comes from `POST /api/v1/_config/endpoints/{slug}/template/expand`
(add `?validate_only=1` to also parse and plan it without running it; see the
[API reference](../CONFIG_SERVICE_API_REFERENCE.md)). For an MCP tool, pass
`"_dryRun": true` in the tool arguments (step 7).

## 3. Double or triple braces, bound or text

Mustache has two forms:

- **`{{ x }}`** HTML-escapes the value: `'` becomes `&#39;`, `/` becomes `&#x2F;`,
  `<` becomes `&lt;`.
- **`{{{ x }}}`** inserts the value as it is.

Neither form escapes SQL. What protects you is that flAPI rewrites a
`params.*` reference into a bound `?` wherever it can. It can when **all** of
these hold:

1. The field has a typed validator: `int`, `number`, `boolean`, `date`, `time`,
   `string`, `email`, `uuid` or `enum`. A field without one is always text.
2. The reference is **outside** every `{{#...}}` / `{{^...}}` section.
3. It is at a **value site**: where SQL expects a value, not a name.

At a value site outside sections, a typed field is bound like this:

| You write | DuckDB receives |
|---|---|
| `c = {{ params.x }}` | `c = ?` |
| `c = {{{ params.x }}}` | `c = ?` |
| `c = '{{{ params.x }}}'` | `c = ?` (the quotes go, the value is bound as text) |
| `c LIKE '%{{{ params.x }}}%'` | `c LIKE ('%' \|\| ? \|\| '%')` |

An absent value binds `NULL` at an unquoted site and `''` inside quotes.

These sites stay **text**, and the validator is your only defence there:

| Site | Example |
|---|---|
| Inside a section | `{{#params.x}} AND c = {{{ params.x }}} {{/params.x}}` |
| A typed literal or a keyword that takes a string | `DATE '{{{ params.d }}}'`, `INTERVAL '{{{ params.n }}} days'`, `ATTACH '{{{ params.f }}}'` |
| Where a name is expected (`{{{ }}}` only) | `FROM {{{ params.t }}}`, `JOIN`, `INTO`, `AS`, `BY`, ... |
| Glued to an identifier (`{{{ }}}` only) | `sales_{{{ params.year }}}` |
| An `enum` field without quotes (`{{{ }}}` only) | `ORDER BY {{{ params.sort }}}` |
| A DuckLake time-travel clause (`{{{ }}}` only) | `AT (VERSION => {{{ params.v }}})` |
| A SQL comment | `-- {{ params.x }}` |
| Every variable that is not `params.*` | `{{{ conn.path }}}`, `{{{ auth.username }}}`, `{{{ env.X }}}` |

The "`{{{ }}}` only" rows matter: the rewriter checks the surrounding SQL only
for triple braces. A double-brace reference to a typed field outside a section
is turned into `?` wherever it is, so at most of those sites it breaks the
statement (see [Common mistakes](#common-mistakes)).

So the rule of thumb is:

- **Values** (`=`, `<`, `IN (...)`, `LIKE`): `{{ params.x }}` bare, or
  `'{{{ params.x }}}'` quoted. Outside sections, both are bound.
- **Names and keywords** (`ORDER BY`, a table, `DATE '...'`): `{{{ params.x }}}`
  with an `enum` or a strict `regex` validator.
- **`conn.*`, `env.*`, `auth.*`**: `'{{{ ... }}}'`, so `/` in a path survives.

Here is an endpoint that uses each kind:

<!-- flapi: endpoint group=search file=customers.yaml allow=MCP_UNAUTHENTICATED_TOOLS -->
```yaml
url-path: /customers
request:
  - field-name: name
    field-in: query
    validators:
      - type: string
        max-length: 50
  - field-name: nation
    field-in: query
    validators:
      - type: int
        min: 0
        max: 24
  - field-name: sort
    field-in: query
    validators:
      - type: enum
        allowedValues: [c_custkey, c_name, c_acctbal]
template-source: customers.sql
connection: [customers]
mcp-tool:
  name: search_customers
  description: Find customers by part of their name and by nation
```
<!-- flapi: check GET /customers?name=000000007 -> 200 contains "Customer#000000007" -->
<!-- flapi: check GET /customers?name=O%27Brien -> 200 -->
<!-- flapi: check GET /customers?nation=15&sort=c_acctbal -> 200 -->
<!-- flapi: check GET /customers?nation=99 -> 400 -->
<!-- flapi: check GET /customers?sort=c_phone -> 400 -->
<!-- flapi: check GET /customers?limit=2&offset=2 -> 200 contains "offset=4&limit=2" -->
<!-- flapi: check-mcp search_customers {"name": "000000007", "_dryRun": true} -> ok contains "rendered_sql" -->

<!-- flapi: template group=search file=customers.sql -->
```sql
SELECT c_custkey, c_name, c_nationkey, c_acctbal
FROM read_parquet('{{{ conn.path }}}')
WHERE c_name LIKE '%{{{ params.name }}}%'
{{#params.nation}}
  AND c_nationkey = {{{ params.nation }}}
{{/params.nation}}
ORDER BY {{#params.sort}}{{{ params.sort }}}{{/params.sort}}{{^params.sort}}c_custkey{{/params.sort}}
```

- `name` is bound: `?name=O'Brien` is just a string that matches nothing.
  Without `?name=`, it binds `''` and `LIKE '%%'` matches every row, so the
  filter needs no section.
- `nation` is inside a section, so it is pasted as text. The `int` validator
  (0 to 24) is what keeps it safe: `?nation=99` is a `400`.
- `sort` is a column name, which cannot be a parameter. It is text, and the
  `enum` validator allows only three names.

```bash
flapii templates expand /customers -p '{"name":"Brien","nation":"15","sort":"c_acctbal"}'
# "expanded": "... WHERE c_name LIKE ('%' || ? || '%')\n  AND c_nationkey = 15\nORDER BY c_acctbal\n"
# "bindings": [{"position": 1, "name": "name", "type": "varchar", "value": "Brien"}]
```

## 4. Sections, inverted sections and defaults

- `{{#params.x}} ... {{/params.x}}` renders its body when `x` was sent. A string
  sent empty (`?name=`) counts as sent.
- `{{^params.x}} ... {{/params.x}}` renders its body when `x` was not sent.
- Together they give a default, as `ORDER BY` above does.

A field can also have a default of its own. It is applied before validation, so
the template always sees a value:

```yaml
request:
  - field-name: sort
    field-in: query
    default: c_custkey
    validators:
      - type: enum
        allowedValues: [c_custkey, c_name, c_acctbal]
```

Do not use a default for `limit` or `offset`; see step 6.

A section is the natural way to make a filter optional, but everything inside
it is text. For a free-text value that is a problem: `'{{{ params.name }}}'`
inside a section sends `c_name = 'O'Brien'` to DuckDB, which fails with a
`500`. To keep an optional value bound, leave it outside the section and let
`NULL` switch the filter off:

<!-- flapi: endpoint group=exact file=customers-exact.yaml -->
```yaml
url-path: /customers-exact
request:
  - field-name: name
    field-in: query
    validators:
      - type: string
        max-length: 50
template-source: customers-exact.sql
connection: [customers]
```
<!-- flapi: check GET /customers-exact?name=Customer%23000000007 -> 200 contains "Customer#000000007" -->
<!-- flapi: check GET /customers-exact?name=O%27Brien -> 200 -->
<!-- flapi: check GET /customers-exact?limit=1 -> 200 contains "Customer#000000001" -->

<!-- flapi: template group=exact file=customers-exact.sql -->
```sql
SELECT c_custkey, c_name
FROM read_parquet('{{{ conn.path }}}')
WHERE ({{ params.name }} IS NULL OR c_name = {{ params.name }})
ORDER BY c_custkey
```

Without `?name=`, both placeholders bind `NULL` and every row comes back.

## 5. Use the caller, the environment and the connection

`auth.*` is set on an authenticated request:

| Variable | Value |
|---|---|
| `auth.username` | The user name (for a JWT, its `sub` claim) |
| `auth.roles` | The roles, joined with commas: `read,sales` |
| `auth.email` | The email, when the token carries one; otherwise empty |
| `auth.type` | `basic`, `bearer` (also for `type: jwt`) or `oidc` |
| `auth.authenticated` | `true` |

On an anonymous request none of them is set. `env.*` holds only the variables
that match a pattern in `template.environment-whitelist`; with an empty
whitelist it holds none.

<!-- flapi: config group=who env="FLAPI_REGION=eu-west" -->
```yaml
project-name: who-am-i
template:
  path: ./sqls
  environment-whitelist: ['^FLAPI_REGION$']
connections:
  customers:
    properties:
      path: './data/customers.parquet'
cors:
  allow-origins: [https://app.example.com]
```

<!-- flapi: endpoint group=who file=me.yaml -->
```yaml
url-path: /me
with-pagination: false
template-source: me.sql
connection: [customers]
auth:
  enabled: true
  type: basic
  users:
    - username: reader      # password: reader-pass
      password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
      roles: [read, sales]
```
<!-- flapi: check GET /me -> 401 -->
<!-- flapi: check GET /me user=reader:reader-pass -> 200 contains "read,sales" -->
<!-- flapi: check GET /me user=reader:reader-pass -> 200 contains "eu-west" -->

<!-- flapi: template group=who file=me.sql -->
```sql
SELECT '{{{ auth.username }}}'      AS username,
       '{{{ auth.roles }}}'         AS roles,
       '{{{ auth.type }}}'          AS auth_type,
       '{{{ env.FLAPI_REGION }}}'   AS region,
       count(*)                     AS customers
FROM read_parquet('{{{ conn.path }}}')
```

```bash
FLAPI_REGION=eu-west flapi -c flapi.yaml
curl -s -u reader:reader-pass http://localhost:8080/me
# [{"username":"reader","roles":"read,sales","auth_type":"basic","region":"eu-west","customers":1500}]
```

These values are never bound: they are text, like `conn.*`. To test a role,
compare inside SQL, for example `list_contains(string_split('{{{ auth.roles }}}', ','), 'admin')`.
Authentication itself is covered in [Authentication](./authentication.md).

## 6. Leave `LIMIT` and `OFFSET` to pagination

GET endpoints are paginated by default (`with-pagination: true`, 100 rows per
page). flAPI wraps your query and applies `?limit=` and `?offset=` itself, so
those two names are reserved:

- Do not declare `limit` or `offset` as request fields.
- Do not write `LIMIT {{ params.limit }}` in a GET template. It applies before
  flAPI's offset, so `?limit=2&offset=2` returns `"data": []`.

`GET /customers?limit=2&offset=2` on the endpoint from step 3 returns rows 3 and
4 and `"next": "/customers?offset=4&limit=2"`. If you need a bare array and your
own `LIMIT`, set `with-pagination: false`. See
[REST endpoints](./rest-endpoints.md#pagination-is-on-by-default).

## 7. Preview an MCP tool call with `_dryRun`

An endpoint with an `mcp-tool:` block is also an MCP tool. Add `"_dryRun": true`
to the arguments and the tool renders the template without running it:

```bash
curl -s -X POST http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 1, "method": "tools/call",
  "params": {"name": "search_customers", "arguments": {"name": "000000007", "_dryRun": true}}}'
```

The result carries `"dry_run": true`, `rendered_sql` (with `?` placeholders)
and `bindings`, the same shape as `templates expand`. `_dryRun` is removed from
the arguments before validation.

## 8. Write data: POST, PUT, PATCH, DELETE

The same template rules apply to writes. A write endpoint answers with
`rows_affected` and, when a statement has a `RETURNING` clause, its rows as
`data`. `POST` answers `201`, the others `200`.

<!-- flapi: config group=notes -->
```yaml
project-name: notes-api
template:
  path: ./sqls
connections:
  notes:
    init: |
      ATTACH IF NOT EXISTS './notes.duckdb' AS notesdb;
      CREATE TABLE IF NOT EXISTS notesdb.notes (id INTEGER PRIMARY KEY, title VARCHAR NOT NULL);
```

<!-- flapi: endpoint group=notes file=notes-create.yaml -->
```yaml
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
        max-length: 200
operation:
  returns-data: true
template-source: notes-create.sql
connection: [notes]
```
<!-- flapi: check POST /notes body={"id":1,"title":"O'Brien's list"} -> 201 contains "O'Brien's list" -->
<!-- flapi: check POST /notes body={"id":2,"title":"second"} -> 201 -->

<!-- flapi: template group=notes file=notes-create.sql -->
```sql
INSERT INTO notesdb.notes (id, title)
VALUES ({{ params.id }}, {{ params.title }})
RETURNING id, title;
```

```bash
curl -s -X POST http://localhost:8080/notes -H 'Content-Type: application/json' \
  -d '{"id":1,"title":"O'\''Brien'\''s list"}'
# {"rows_affected":1,"data":[{"id":1,"title":"O'Brien's list"}]}
```

Both values are bound, so the apostrophes need no escaping. `operation.returns-data: true`
makes `data` always present (`[]` when `RETURNING` matched no row); without it,
`data` appears only when rows came back.

A `DELETE` is written the same way, and `rows_affected` tells you whether
anything matched:

<!-- flapi: endpoint group=notes file=notes-delete.yaml -->
```yaml
url-path: /notes/:id
method: DELETE
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
template-source: notes-delete.sql
connection: [notes]
```
<!-- flapi: check DELETE /notes/2 -> 200 contains "{"rows_affected":1}" -->
<!-- flapi: check DELETE /notes/99 -> 200 contains "{"rows_affected":0}" -->

<!-- flapi: template group=notes file=notes-delete.sql -->
```sql
DELETE FROM notesdb.notes WHERE id = {{ params.id }}
```

A write template may hold several `;`-separated statements. They run in one
transaction (`operation.transaction`, on by default), each as its own prepared
statement.

**Known limitation ([#271](https://github.com/DataZooDE/flapi/issues/271)).**
A `POST` whose template is a single `SELECT` returns no rows: `{"rows_affected":0}`,
or `"data": []` with `returns-data: true`. A search that takes its input as a
JSON body hits this. Until it is fixed, make the `SELECT` the second statement;
with `returns-data: true` the rows of a final `SELECT` become `data`:

<!-- flapi: endpoint group=notes file=notes-search.yaml -->
```yaml
url-path: /notes/search
method: POST
request:
  - field-name: q
    field-in: body
    required: true
    validators:
      - type: string
        max-length: 100
operation:
  returns-data: true
template-source: notes-search.sql
connection: [notes]
```
<!-- flapi: check POST /notes/search body={"q":"Brien"} -> 201 contains "O'Brien's list" -->

<!-- flapi: template group=notes file=notes-search.sql -->
```sql
SET VARIABLE q = {{ params.q }};
SELECT id, title FROM notesdb.notes
WHERE title ILIKE '%' || getvariable('q') || '%'
```

## Common mistakes

- **`'{{ params.x }}'` with double braces inside quotes.** Outside a section it
  becomes the string `'?'`, not a placeholder, and the request fails with `500`
  (the log says `Failed to bind parameter`). The same happens in
  `DATE '{{ params.d }}'`. Write `{{ params.x }}` without quotes, or
  `'{{{ params.x }}}'`.
- **`ORDER BY {{ params.sort }}`.** Double braces become `?`, and DuckDB answers
  `Parameter not supported in ORDER BY clause` (a `500`). Use
  `{{{ params.sort }}}` with an `enum` validator. The same goes for table and
  column names.
- **`{{ conn.path }}` or `{{ }}` on a string inside a section.** HTML escaping
  turns `./data/x.parquet` into `.&#x2F;data&#x2F;x.parquet` and `O'Brien` into
  `O&#39;Brien`. There is no error, just no file or no match. Use `{{{ }}}` for
  every non-`params` variable and for strings in sections.
- **Free text in a section.** `'{{{ params.name }}}'` inside `{{#params.name}}`
  is pasted as written, so a quote in the value breaks the SQL. Keep the value
  outside the section (step 4), or restrict it with a `regex` validator.
- **`LIMIT {{ params.limit }}` in a GET template.** Pagination owns `limit` and
  `offset` (step 6).
- **A parameter the endpoint does not declare.** `{{ params.region }}` without
  a `region` field has no validator and is never bound. `flapi --validate-config`
  warns: `'params.region' uses request parameter 'region', which endpoint ...
  does not declare in its request: fields`.
- **`context.*`.** It does not exist and renders empty. Use `auth.*` and `conn.*`.
- **Treating `auth.roles` as a list.** It is one comma-joined string.
- **`{{cache.currentSnapshotTimestamp}}`.** That variable does not exist; the
  cache variables are listed in [Caching](./caching.md#writing-the-populate-template).

## Next

- [Building a REST endpoint](./rest-endpoints.md): validators, pagination, writes
- [Exposing an endpoint as an MCP tool](./mcp-tools.md)
- [Caching an expensive query](./caching.md): populate templates and `cache.*`
- [Requiring authentication](./authentication.md): where `auth.*` comes from

## How it works

The template is checked for balanced sections at load time. On each request,
`PreparedTemplateRewriter` replaces each bindable `params.*` reference with `?`
and records a binding, Mustache renders what is left, and DuckDB prepares the
SQL and binds the values with their validator's type.

- [CONFIG_REFERENCE § 9 SQL Templates](../CONFIG_REFERENCE.md#9-sql-templates-mustache)
  and [§ 5.9 the SQL-injection screen](../CONFIG_REFERENCE.md#59-the-sql-injection-screen-is-a-safety-net-not-the-defence)
- [CONFIG_REFERENCE § 4.3 Default values](../CONFIG_REFERENCE.md#43-default-values)
  and [§ 8.2 Write operations](../CONFIG_REFERENCE.md#82-write-operations)
- [spec/components/query-execution.md](../spec/components/query-execution.md)
- Source: `src/prepared_template_rewriter.cpp` (which sites are bound),
  `src/sql_parameter_classifier.cpp` (which validator types bind),
  `src/sql_template_processor.cpp` (the template context),
  `src/database_manager.cpp` (write execution)
