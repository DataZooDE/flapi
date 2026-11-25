# Writing SQL templates

**Goal:** write flAPI SQL templates that return the right rows and cannot be
turned against your database.

**Before you start:** install flAPI as in [Getting started](./getting-started.md).
The examples read `customers.parquet` (columns `c_custkey`, `c_name`,
`c_nationkey`, `c_acctbal`, ...). Put it in a `data/` directory of your
project, or clone the flAPI repository, which has it in `examples/data/`:

```bash
mkdir -p templates-guide/data templates-guide/sqls && cd templates-guide
curl -L -o data/customers.parquet https://github.com/DataZooDE/flapi/raw/main/examples/data/customers.parquet
```

A template is a SQL file with [Mustache](https://mustache.github.io/) tags. On
every request flAPI validates the parameters, renders the template and runs the
result on DuckDB. Most of this guide is about one question: which values reach
DuckDB as **bound parameters**, and which as **SQL text**.

---

## 1. Write a first template

Steps 1 to 4 and step 8 share one project, the `templates-guide` directory from
above.

`flapi.yaml`:

<!-- flapi: config group=customers allow=MCP_UNAUTHENTICATED_TOOLS -->
```yaml
project-name: templates-guide
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
```

An endpoint is a YAML file and a SQL file in `sqls/`.

`sqls/customer.yaml`:

<!-- flapi: endpoint group=customers file=customer.yaml -->
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

`sqls/customer.sql`:

<!-- flapi: template group=customers file=customer.sql -->
```sql
SELECT c_custkey, c_name, c_nationkey, c_acctbal
FROM read_parquet('{{{ conn.path }}}')
WHERE c_custkey = {{ params.id }}
```

Start the server:

```bash
flapi -c flapi.yaml
```

In a second terminal, call the endpoint:

```bash
curl -s http://localhost:8080/customers/7
# {"data":[{"c_custkey":7,"c_name":"Customer#000000007",...}],"next":"","total_count":1}
curl -s http://localhost:8080/customers/abc
# {"errors":[{"message":"Invalid integer value","field":"id"}]}
```

The validator runs first: `abc` is rejected with `400` before any SQL is
rendered. The response is wrapped in `{data, next, total_count}` because GET
endpoints are paginated (step 7).

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

Stop the server and start it again with the config service on, using a token you
choose:

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=my-dev-token
flapi -c flapi.yaml --config-service
```

In the second terminal, export the same token and render the template:

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=my-dev-token
flapii templates expand /customers/:id -p '{"id":"7"}'
```

```json
{
  "bindings": [{"position": 1, "name": "id", "type": "integer", "value": 7}],
  "expanded": "SELECT c_custkey, c_name, c_nationkey, c_acctbal\nFROM read_parquet('/home/me/templates-guide/data/customers.parquet')\nWHERE c_custkey = ?\n"
}
```

Without the exported token, `flapi --config-service` makes up a random one and
`flapii` gets a `401`. If the server is not on `localhost:8080`, pass
`--base-url http://host:port` to `flapii` or set `FLAPI_BASE_URL`.

`{{ params.id }}` became `?`, and `7` travels separately as an integer. DuckDB
runs this as a **prepared statement**, so the value can never change the
statement. `conn.path` was pasted in as text. (A connection property that starts
with `./` or `../` is made absolute against the directory of `flapi.yaml`.)

Without flapii, post to the config service directly. The endpoint is named by
its `url-path` with each `/` replaced by `-`, so `/customers/:id` is
`-customers-:id`:

```bash
curl -s -X POST http://localhost:8080/api/v1/_config/endpoints/-customers-:id/template/expand \
  -H "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN" -H 'Content-Type: application/json' \
  -d '{"parameters": {"id": "7"}}'
```
<!-- flapi: check POST /api/v1/_config/endpoints/-customers-:id/template/expand body={"parameters":{"id":"7"}} header=X-Config-Token:skill-check-token -> 200 contains "c_custkey = ?" -->

Add `?validate_only=1` to also parse and plan the SQL without running it (see the
[API reference](../CONFIG_SERVICE_API_REFERENCE.md)). For an MCP tool, pass
`"_dryRun": true` in the tool arguments (step 8).

## 3. Double or triple braces, bound or text

The rule of thumb:

- **Values** (after `=`, `<`, `IN (`, `LIKE`): write `{{ params.x }}` bare, or
  `'{{{ params.x }}}'` in quotes. Outside sections, flAPI binds both.
- **Names and keywords** (`ORDER BY`, a table, `DATE '...'`): write
  `{{{ params.x }}}` and restrict it with an `enum` or a strict `regex` validator.
- **`conn.*`, `env.*`, `auth.*`**: write `'{{{ ... }}}'`. These are never bound.

Two terms used below. A **section** is a block between `{{#name}}` (or
`{{^name}}`) and `{{/name}}` that Mustache renders or skips (step 4). A
**value site** is a place where SQL expects a value, such as the right side of
`=`; the opposite is a place where SQL expects a name, such as after `FROM` or
`ORDER BY`.

The rest of this step explains the rule. Mustache has two forms:

- **`{{ x }}`** HTML-escapes the value. It replaces `&`, `<`, `>`, `"`, `'`,
  `/`, `` ` `` and `=` with entities: `'` becomes `&#39;`, `/` becomes `&#x2F;`.
- **`{{{ x }}}`** inserts the value as it is.

Neither form escapes SQL. What protects you is that flAPI rewrites a
`params.*` reference into a bound `?` wherever it can. It can when **all** of
these hold:

1. The field has a typed validator: `int`, `number`, `boolean`, `date`, `time`,
   `string`, `email`, `uuid` or `enum`. A field without one is always text.
2. The reference is outside every section.
3. It is at a value site.

At a value site outside sections, a typed field is bound like this:

| You write | DuckDB receives |
|---|---|
| `c = {{ params.x }}` | `c = ?` |
| `c = {{{ params.x }}}` | `c = ?` |
| `c = '{{{ params.x }}}'` | `c = ?` (the quotes go, the value is bound as text) |
| `c LIKE '%{{{ params.x }}}%'` | `c LIKE ('%' \|\| ? \|\| '%')` |
| `c IN ('{{{ params.a }}}', '{{{ params.b }}}')` | `c IN (?, ?)` |

An absent value binds `NULL` at an unquoted site and `''` inside quotes.

These sites stay **text**, and the validator is your only defence there:

| Site | Example |
|---|---|
| Inside a section | `{{#params.x}} AND c = {{{ params.x }}} {{/params.x}}` |
| A typed literal or a keyword that takes a string (`{{{ }}}` only) | `DATE '{{{ params.d }}}'`, `INTERVAL '{{{ params.n }}} days'`, `ATTACH '{{{ params.f }}}'` |
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

Binding is also the fast path. A bound template renders to the same SQL text
for every value, and each request thread keeps up to 64 prepared `SELECT`s keyed
by that text, so DuckDB plans the query once. A value pasted as text makes new
SQL, and a new plan, for every value.

Here is an endpoint that uses each kind.

`sqls/customers.yaml`:

<!-- flapi: endpoint group=customers file=customers.yaml -->
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

`sqls/customers.sql`:

<!-- flapi: template group=customers file=customers.sql -->
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

The `mcp-tool:` block also makes this endpoint an MCP tool (step 8). Because the
project has no MCP authentication, startup prints a `MCP_UNAUTHENTICATED_TOOLS`
security warning. That is expected here; [MCP tools](./mcp-tools.md#limiting-who-can-call-it)
shows how to require a login.

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

Do not use a default for `limit` or `offset`; see step 7.

A section is the natural way to make a filter optional, but everything inside
it is text. For a free-text value that is a problem: `'{{{ params.name }}}'`
inside a section sends `c_name = 'O'Brien'` to DuckDB, which fails with a
`500`. To keep an optional value bound, leave it outside the section and let
`NULL` switch the filter off.

`sqls/customers-exact.yaml`:

<!-- flapi: endpoint group=customers file=customers-exact.yaml -->
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

`sqls/customers-exact.sql`:

<!-- flapi: template group=customers file=customers-exact.sql -->
```sql
SELECT c_custkey, c_name
FROM read_parquet('{{{ conn.path }}}')
WHERE ({{ params.name }} IS NULL OR c_name = {{ params.name }})
ORDER BY c_custkey
```

Without `?name=`, both placeholders bind `NULL` and every row comes back.

A list in one parameter works the same way: bind it as one string and split it
in SQL. The `regex` validator makes sure it is a list of numbers.

`sqls/customers-by-ids.yaml`:

<!-- flapi: endpoint group=customers file=customers-by-ids.yaml -->
```yaml
url-path: /customers-by-ids
request:
  - field-name: ids
    field-in: query
    required: true
    validators:
      - type: string
        regex: '^[0-9]{1,9}(,[0-9]{1,9}){0,99}$'
template-source: customers-by-ids.sql
connection: [customers]
```
<!-- flapi: check GET /customers-by-ids?ids=3,7 -> 200 contains "Customer#000000007" -->
<!-- flapi: check GET /customers-by-ids?ids=3;7 -> 400 -->

`sqls/customers-by-ids.sql`:

<!-- flapi: template group=customers file=customers-by-ids.sql -->
```sql
SELECT c_custkey, c_name
FROM read_parquet('{{{ conn.path }}}')
WHERE list_contains(string_split({{ params.ids }}, ','), CAST(c_custkey AS VARCHAR))
ORDER BY c_custkey
```

## 5. Use the caller and the environment

`auth.*` is set on an authenticated request: a REST request, or an MCP
`tools/call` under `mcp.auth`. flAPI fills it from the credentials it checked;
a caller cannot set it through parameters.

| Variable | Value |
|---|---|
| `auth.username` | The user name (for a JWT, its `sub` claim) |
| `auth.roles` | The roles, joined with commas: `sales,eu` |
| `auth.email` | The email claim, only with `type: oidc`; empty for `basic` and `bearer` |
| `auth.type` | `basic`, `bearer` (also for `type: jwt`) or `oidc` |
| `auth.authenticated` | `true` |

On an anonymous request all of them are empty. `env.*` holds only the variables
that match a pattern in `template.environment-whitelist`; with an empty
whitelist it holds none.

Steps 5 and 6 use a **new project** in its own directory, `who-am-i`, with its
own copy of `data/customers.parquet`. Its `flapi.yaml`:

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
  deals:
    init: |
      CREATE OR REPLACE TABLE deals AS
      SELECT * FROM (VALUES (1, 'alice', 100), (2, 'alice', 250), (3, 'carol', 75)) t(id, owner, amount);
cors:
  allow-origins: [https://app.example.com]
```

The `deals` connection has no file: its `init:` SQL runs at startup and creates
a small table in memory (see [Data sources](./data-sources.md#1-how-a-connection-works)).
The `cors:` block names the web origins whose pages may call this server from a
browser. Without it, flAPI allows every origin and, because the endpoints below
require a login, warns `CORS_WILDCARD_WITH_AUTH` at startup.

`sqls/me.yaml`:

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
    - username: alice       # password: reader-pass
      password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
      roles: [sales, eu]
```
<!-- flapi: check GET /me -> 401 -->
<!-- flapi: check GET /me user=alice:reader-pass -> 200 contains "sales,eu" -->
<!-- flapi: check GET /me user=alice:reader-pass -> 200 contains "eu-west" -->

The password is stored as a PBKDF2 hash; [Authentication](./authentication.md#2-store-password-hashes-not-passwords)
shows the command that makes one.

`sqls/me.sql`:

<!-- flapi: template group=who file=me.sql -->
```sql
SELECT '{{{ auth.username }}}'      AS username,
       '{{{ auth.roles }}}'         AS roles,
       '{{{ auth.type }}}'          AS auth_type,
       '{{{ env.FLAPI_REGION }}}'   AS region,
       count(*)                     AS customers
FROM read_parquet('{{{ conn.path }}}')
```

Start the server with the variable set:

```bash
FLAPI_REGION=eu-west flapi -c flapi.yaml
```

In a second terminal:

```bash
curl -s -u alice:reader-pass http://localhost:8080/me
# [{"username":"alice","roles":"sales,eu","auth_type":"basic","region":"eu-west","customers":1500}]
```

## 6. Restrict rows to the caller

With `auth.*`, one endpoint can return each caller only their own rows. Three
rules:

- **`auth.*` is text, never bound.** Quote it: `'{{{ auth.username }}}'`.
- **Test roles as a list.** `auth.roles` is one string, so split it:
  `list_contains(string_split('{{{ auth.roles }}}', ','), 'admin')`. Do not write
  `'{{{ auth.roles }}}' LIKE '%admin%'`: it also matches a role named `notadmin`.
- **Fail closed.** Write the filter without a section around it. An anonymous
  request has an empty `auth.username`, so it matches no owner. Do not wrap the
  filter in `{{#auth.username}}`: with no user, the filter would vanish and
  every row would come back.

`sqls/deals.yaml`:

<!-- flapi: endpoint group=who file=deals.yaml -->
```yaml
url-path: /deals
with-pagination: false
template-source: deals.sql
connection: [deals]
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
      roles: [sales]
    - username: bob
      password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
      roles: [notadmin]
    - username: root
      password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
      roles: [admin]
```
<!-- flapi: check GET /deals -> 401 -->
<!-- flapi: check GET /deals user=alice:reader-pass -> 200 contains "alice" -->
<!-- flapi: check GET /deals user=bob:reader-pass -> 200 contains "[]" -->
<!-- flapi: check GET /deals user=root:reader-pass -> 200 contains "carol" -->

`sqls/deals.sql`:

<!-- flapi: template group=who file=deals.sql -->
```sql
SELECT id, owner, amount
FROM deals
WHERE owner = '{{{ auth.username }}}'
   OR list_contains(string_split('{{{ auth.roles }}}', ','), 'admin')
ORDER BY id
```

```bash
curl -s -u alice:reader-pass http://localhost:8080/deals
# [{"id":1,"owner":"alice","amount":100},{"id":2,"owner":"alice","amount":250}]
curl -s -u bob:reader-pass http://localhost:8080/deals    # role notadmin, owns nothing
# []
curl -s -u root:reader-pass http://localhost:8080/deals   # role admin: all three rows
```

## 7. Leave `LIMIT` and `OFFSET` to pagination

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

A full page runs your query twice: once for the page, and once as
`SELECT COUNT(*)` over it for `total_count`. On a warehouse that bills per scan,
the second query costs money. Set `total-count: false` on endpoints whose
clients only page forward (no count query runs then), or serve the endpoint from
a [cache](./caching.md). A trace shows the two queries as separate spans,
`main query` and `count query`; see
[Observability](./observability.md#what-the-spans-show).

## 8. Preview an MCP tool call with `_dryRun`

An endpoint with an `mcp-tool:` block is also an MCP tool. Add `"_dryRun": true`
to the arguments and the tool renders the template without running it. With the
`templates-guide` server running:

```bash
curl -s -X POST http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 1, "method": "tools/call",
  "params": {"name": "search_customers", "arguments": {"name": "000000007", "_dryRun": true}}}'
```

The result carries `"dry_run": true`, `rendered_sql` (with `?` placeholders)
and `bindings`, the same shape as `templates expand`. `_dryRun` is removed from
the arguments before validation. The result also has `"rows": []`: nothing ran,
so that is not the query's answer.

## 9. Write data: POST, PUT, PATCH, DELETE

The same template rules apply to writes. A write endpoint answers with
`rows_affected` and, when a statement has a `RETURNING` clause, its rows as
`data`. `POST` answers `201`, the others `200`.

This is a **new project** in its own directory, `notes-api`. Its `flapi.yaml`
creates a DuckDB file `notes.duckdb` with one table, through the connection's
`init:` SQL ([Data sources](./data-sources.md#1-how-a-connection-works)):

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

`sqls/notes-create.yaml`:

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
<!-- flapi: check POST /notes body={"id":1,"title":"again"} -> 500 contains "request_id" -->

`sqls/notes-create.sql`:

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

Run the same `curl` again and the answer is a `500` with only a `request_id`:
`id` 1 exists, and the primary key refuses it. flAPI never puts the database
error in the response.

**When a template fails.** A broken template, or SQL that DuckDB rejects, is a
`500` whose body carries a `request_id` and nothing else. Look that id up in the
server log ([Observability](./observability.md#1-find-out-why-a-request-failed)).
To catch errors before a request does, run `flapi -c flapi.yaml --validate-config`
in CI: it checks every template's sections and warns about undeclared
parameters.

A `DELETE` is written the same way, and `rows_affected` tells you whether
anything matched.

`sqls/notes-delete.yaml`:

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

`sqls/notes-delete.sql`:

<!-- flapi: template group=notes file=notes-delete.sql -->
```sql
DELETE FROM notesdb.notes WHERE id = {{ params.id }}
```

A write template may hold several `;`-separated statements. They run in one
transaction (`operation.transaction`, on by default), each as its own prepared
statement.

> **Known limitation: a POST cannot serve a plain `SELECT` ([#271](https://github.com/DataZooDE/flapi/issues/271)).**
> A `POST` whose template is a single `SELECT` returns no rows: `{"rows_affected":0}`,
> or `"data": []` with `returns-data: true`. A search that takes its input as a
> JSON body hits this. Until it is fixed, make the `SELECT` the second statement;
> with `returns-data: true` the rows of a final `SELECT` become `data`.

`sqls/notes-search.yaml`:

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

`sqls/notes-search.sql`:

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
- **`{{ conn.path }}`, or `{{ }}` on a string inside a section.** HTML escaping
  turns `/srv/data/x.parquet` into `&#x2F;srv&#x2F;data&#x2F;x.parquet` (a
  `./data/...` path too, since it is made absolute first) and `O'Brien` into
  `O&#39;Brien`. Nothing reports an escaping problem; the file is just not found,
  or the row does not match. Use `{{{ }}}` for every non-`params` variable and
  for strings in sections.
- **Free text in a section.** `'{{{ params.name }}}'` inside `{{#params.name}}`
  is pasted as written, so a quote in the value breaks the SQL. Keep the value
  outside the section (step 4), or restrict it with a `regex` validator.
- **`LIMIT {{ params.limit }}` in a GET template.** Pagination owns `limit` and
  `offset` (step 7).
- **A parameter the endpoint does not declare.** `{{ params.region }}` without
  a `region` field has no validator and is never bound. `flapi --validate-config`
  warns: `'params.region' uses request parameter 'region', which endpoint ...
  does not declare in its request: fields`.
- **`context.*`.** It does not exist and renders empty. Use `auth.*` and `conn.*`.
- **`auth.roles` matched with `LIKE`.** It is one comma-joined string; split it
  (step 6).
- **A row filter inside `{{#auth.username}}`.** It disappears for anonymous
  callers, who then see every row (step 6).
- **`{{cache.currentSnapshotTimestamp}}`.** That variable does not exist; the
  cache variables are listed in [Caching](./caching.md#writing-the-populate-template).

## Next

- [Building a REST endpoint](./rest-endpoints.md): validators, pagination, writes
- [Connecting data sources](./data-sources.md): what goes in `connections:`
- [Exposing an endpoint as an MCP tool](./mcp-tools.md)
- [Caching an expensive query](./caching.md): populate templates and `cache.*`
- [Requiring authentication](./authentication.md): where `auth.*` comes from
- [Observability](./observability.md): finding a failed request by its id

## How it works

The template is checked for balanced sections at load time. On each request,
`PreparedTemplateRewriter` replaces each bindable `params.*` reference with `?`
and records a binding, Mustache renders what is left, and DuckDB prepares the
SQL and binds the values with their validator's type.

The injection tests behind these claims are
`test/integration/test_sql_injection_corpus.py` (GET payloads across every
validator type) and `test/integration/test_sql_injection_write_corpus.py`
(POST payloads, including multi-statement `RETURNING` templates).

- [CONFIG_REFERENCE § 9 SQL Templates](../CONFIG_REFERENCE.md#9-sql-templates-mustache)
  and [§ 5.9 the SQL-injection screen](../CONFIG_REFERENCE.md#59-the-sql-injection-screen-is-a-safety-net-not-the-defence)
- [CONFIG_REFERENCE § 4.3 Default values](../CONFIG_REFERENCE.md#43-default-values)
  and [§ 8.2 Write operations](../CONFIG_REFERENCE.md#82-write-operations)
- [spec/components/query-execution.md](../spec/components/query-execution.md)
- Source: `src/prepared_template_rewriter.cpp` (which sites are bound),
  `src/sql_parameter_classifier.cpp` (which validator types bind),
  `src/sql_template_processor.cpp` (the template context),
  `src/query_executor.cpp` (the per-thread prepared-statement cache),
  `src/database_manager.cpp` (write execution)
