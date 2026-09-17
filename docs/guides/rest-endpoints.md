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
    field-type: int
    required: false
    validators:
      - type: int
        min: 1

template-source: customers.sql   # relative to template.path
connection: [my-data]
```

`url-path` may contain path parameters: `/customers/:id` with a matching
`field-in: path` entry.

The full key list is in [CONFIG_REFERENCE § 3](../CONFIG_REFERENCE.md).

## Writing the SQL template

Templates are [Mustache](https://mustache.github.io/). Available variables:

| Variable | Contains |
|---|---|
| `params.*` | Validated request parameters |
| `conn.*` | Properties of the connection(s) you listed |
| `cache.*` | Cache metadata, when caching is enabled |
| `env.*` | Whitelisted environment variables |

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
LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}100{{/params.limit}}
```

`WHERE 1=1` means every filter can be appended with `AND` regardless of which
ones are present. `{{#x}}…{{/x}}` renders only when `x` is present;
`{{^x}}…{{/x}}` renders only when it is absent — which is how the `LIMIT` above
gets a default.

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
    pattern: "^[a-zA-Z0-9_]+$"

  - type: enum
    values: [active, inactive, pending]

  - type: date
    min: "2020-01-01"
```

`email`, `uuid` and `time` are also available. Full list and options:
[CONFIG_REFERENCE § 5](../CONFIG_REFERENCE.md).

## Testing it

```bash
# Render the template without running it
flapii templates expand /customers --params '{"id":"123"}'

# Validate the YAML before the server sees it
flapii endpoints validate sqls/customers.yaml

# Check what the server actually loaded
flapii endpoints list
flapii endpoints get /customers
```

If a request 404s, the usual causes are a `url-path` that does not match, a file
outside `template.path`, or a YAML error at startup — check the server log.

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
