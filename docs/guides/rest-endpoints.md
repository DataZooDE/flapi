# Building a REST endpoint

**Goal:** go from a SQL query to a validated, documented HTTP endpoint.

Every endpoint is two files in `sqls/`: a YAML definition and a SQL template.

---

## The shape

```yaml
# sqls/customers.yaml
url-path: /customers
method: GET                      # GET, POST, PUT, DELETE, PATCH

request:
  - field-name: id
    field-in: query              # query | path | body | header
    required: false
    validators:
      - type: int
        min: 1

template-source: customers.sql   # relative to template.path
connection: [my-data]
```

`url-path` may contain path parameters: `/customers/:id` with a matching
`field-in: path` entry. A field's type comes from its validators (`type: int`
above). `field-type` does nothing; loading warns that it "has no effect".

A `field-in: header` field is read from the request headers, case-insensitively
(`field-name: X-Tenant` matches `x-tenant: acme`), and only from there: a query
or body parameter with the same name does not satisfy it.

## Pagination is on by default

`with-pagination` defaults to `true`. flAPI wraps your query, and every GET
response is an object, not a bare array:

```json
{"next": "/customers?offset=4&limit=2", "total_count": 10, "data": [ ... ]}
```

The page size defaults to **100** rows; `?limit=` and `?offset=` choose a page,
and the same values come back in the `X-Total-Count`, `X-Limit`, `X-Offset` and
`X-Next` headers. **`limit` and `offset` are reserved for this.** Do not declare
them as request fields and do not use `{{ params.limit }}` in a template: a
template `LIMIT {{ params.limit }}` applies the limit *before* flAPI's offset, so
`?limit=2&offset=2` returns `"data": []`. Set `with-pagination: false` if you
want to return a bare array and control `LIMIT` yourself.

The full key list is in [CONFIG_REFERENCE § 3](../CONFIG_REFERENCE.md).

## Writing the SQL template

Templates are [Mustache](https://mustache.github.io/). Available variables:

| Variable | Contains |
|---|---|
| `params.*` | Validated request parameters |
| `conn.*` | Properties of the connection(s) you listed |
| `cache.*` | Cache metadata, when caching is enabled |
| `env.*` | Whitelisted environment variables |

Every `{{#x}}` or `{{^x}}` section needs its `{{/x}}`, closed in reverse order, and `{{{` needs `}}}`.
flAPI checks this before using a template - at startup, on reload, on a config-service template update,
in `flapi --validate-config` and on every request - and names the file and line
(`Invalid template sqls/orders.sql:12:3: ...`). Partials (`{{> x}}`) and set-delimiter tags
(`{{= =}}`) are not supported.

### Triple braces vs double braces

This is the rule people get wrong.

- **`{{{ value }}}`** renders the raw value. Use it inside single-quoted SQL
  string literals.
- **`{{ value }}`** HTML-escapes the value (`<` becomes `&lt;`). Use it only for
  numbers or known-safe identifiers.

> **Neither form escapes SQL.** Mustache does not know what a SQL string literal
> is. Your defence against injection is the **validator** on each field — typed,
> range-checked, pattern-matched — plus quoting string parameters in the
> template. When in doubt, tighten the validator rather than relying on the
> braces.

### The conditional filter idiom

```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.status}}
  AND status = '{{{ params.status }}}'
{{/params.status}}
{{#params.min_price}}
  AND price >= {{ params.min_price }}
{{/params.min_price}}
ORDER BY created_at DESC
```

`WHERE 1=1` means every filter can be appended with `AND` regardless of which
ones are present. `{{#x}}…{{/x}}` renders only when `x` is present;
`{{^x}}…{{/x}}` renders only when it is absent (for a default value). There is no
`LIMIT` here: pagination adds it (see above).

## Validators

Validators run before the template is rendered, and a failure returns 400 without
touching the database.

```yaml
validators:
  - type: int
    min: 1
    max: 999999

  - type: string
    min-length: 1
    max-length: 200
    regex: "^[a-zA-Z0-9_]+$"

  - type: enum
    allowedValues: [active, inactive, pending]

  - type: date
    min: "2020-01-01"
```

`regex` and `allowedValues` are the canonical spellings; `pattern` and `values`
are accepted as aliases (the canonical key wins if both are present).
`email`, `uuid` and `time` are also available. Full list and options:
[CONFIG_REFERENCE § 5](../CONFIG_REFERENCE.md).

## Testing it

The `flapii` commands below talk to a running server's config service. Start
flAPI with `--config-service` and export `FLAPI_CONFIG_SERVICE_TOKEN`, or they
will fail to connect.

```bash
# Render the template without running it
flapii templates expand /customers --params '{"id":"123"}'

# Validate the YAML before the server sees it: the argument is the endpoint's
# url-path, the YAML comes from --file (or stdin)
flapii endpoints validate /customers --file sqls/customers.yaml

# Check what the server actually loaded
flapii endpoints list
flapii endpoints get /customers
```

If a request 404s, the usual causes are a `url-path` that does not match, a file
outside `template.path`, or a YAML error at startup — check the server log.

A `500` never carries the SQL or database error. Its body is

```json
{"error": "Internal Server Error",
 "message": "The request failed on the server. The details are in the server log under this request id.",
 "request_id": "req-46788500aef28a89"}
```

so grep the server log for the `request_id` to see what went wrong.

## Writes, including DELETE

`POST`, `PUT`, `PATCH` and `DELETE` endpoints all run their template as a write:
parameters are validated, the template is rendered and executed, and the answer
is `200 {"rows_affected": n}`. `n` is the number of rows the statements changed
(DuckDB's count, summed over a multi-statement template; with `RETURNING`, the
number of returned rows), so a `DELETE` of a missing id answers `0`. A `DELETE`
endpoint is written like the others:

```yaml
# sqls/customer-delete.yaml
url-path: /customers/:id
method: DELETE
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
template-source: customer-delete.sql   # DELETE FROM customers WHERE id = {{ params.id }}
connection: [my-data]
```

If the endpoint also has a cache, `cache.invalidate-on-write: true` refreshes it
before the response is sent, and `cache.refresh-on-write: true` refreshes it
in the background after the response; see [Caching](./caching.md).

## Common next steps

- **Same endpoint as an AI tool:** add an `mcp-tool:` block — [MCP tools](./mcp-tools.md)
- **Slow query:** [Caching](./caching.md)
- **Needs a token:** [Authentication](./authentication.md)
- **Reuse config across endpoints:** [YAML includes](./yaml-includes.md)

## How it works

Parameters are extracted by `field-in`, validated, then bound — typed fields go
through prepared statements rather than string interpolation.

- [spec/components/query-execution.md](../spec/components/query-execution.md) — rendering and execution
- [spec/components/security.md](../spec/components/security.md) — the layered defences
- [spec/REQUEST_LIFECYCLE.md](../spec/REQUEST_LIFECYCLE.md) — the full request path
