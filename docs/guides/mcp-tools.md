# Creating MCP tools for AI agents

**Goal:** give an AI agent safe, useful access to your data, using the SQL you already
have.

Add an `mcp-tool:` block to an endpoint file and the endpoint becomes a tool. MCP
(the Model Context Protocol) is the protocol agents such as Claude use to find tools and
call them. flAPI serves it on the same port as the REST API, at `/mcp/jsonrpc`. You need
no second implementation and no second server.

Read this guide before you connect an agent to real data. Agents use a tool well when its
description is clear, its inputs are typed and its results are small. Treat MCP as open
until you turn on `mcp.auth` ([Protect it](#protect-it)).

---

## Set up the project

The examples use the TPC-H customer table: 1500 customers with key, name, market segment,
phone and account balance. Make a project directory and download the table into it:

```bash
mkdir mcp-demo && cd mcp-demo
mkdir -p data sqls
curl -L -o data/customers.parquet \
  https://github.com/DataZooDE/flapi/raw/main/examples/data/customers.parquet
```

(If you cloned the flAPI repository, copy `examples/data/customers.parquet` instead.)

`flapi.yaml`:
<!-- flapi: config group=agent -->
```yaml
project-name: customer-agent
template:
  path: ./sqls                       # endpoint files and SQL templates
connections:
  customers:
    properties:
      path: ./data/customers.parquet
```

## Make an endpoint a tool

This file defines a REST endpoint and an MCP tool at the same time:

`sqls/customers.yaml`:
<!-- flapi: endpoint group=agent file=customers.yaml allow=MCP_UNAUTHENTICATED_TOOLS -->
```yaml
url-path: /customers                 # the REST endpoint...
mcp-tool:                            # ...and the MCP tool, from one file
  name: find_customers
  description: >
    Look up customers of the wholesale business by key, market segment or
    minimum account balance. Returns one row per customer with key, name,
    segment and balance in USD. Returns every match: pass limit and offset
    (integers) to page through large results.

request:
  - field-name: id
    field-in: query
    description: Customer key (c_custkey), a positive integer
    validators:
      - type: int
        min: 1
  - field-name: segment
    field-in: query
    description: Market segment
    validators:
      - type: enum
        allowedValues: [AUTOMOBILE, BUILDING, FURNITURE, HOUSEHOLD, MACHINERY]
  - field-name: min_balance
    field-in: query
    description: Only customers whose account balance is at least this many USD
    validators:
      - type: number
        min: -1000

template-source: customers.sql
connection: [customers]
```
<!-- flapi: check GET /customers?id=3 -> 200 contains "Customer#000000003" -->
<!-- flapi: check-mcp find_customers {"id": 3} -> ok contains "Customer#000000003" -->
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"tools/list"} -> 200 contains ""type":"integer"" -->
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"tools/list"} -> 200 contains ""enum":["AUTOMOBILE"" -->
<!-- flapi: check-mcp find_customers {"segment": "BUILDING", "limit": 2, "offset": 2} -> ok contains ""row_count":2" -->
<!-- flapi: check-mcp find_customers {"segment": "boats"} -> error contains "segment: Invalid enum value" -->
<!-- flapi: check-mcp find_customers {"segmnt": "BUILDING"} -> error contains "segmnt: Unknown parameter" -->
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"tools/list"} -> 200 contains ""c_acctbal":{"type":"number"}" -->
<!-- flapi: check-mcp find_customers {"id": 3, "_dryRun": true} -> ok contains "rendered_sql" -->
<!-- flapi: check GET /mcp/health -> 200 contains "tools_count" -->

`sqls/customers.sql`:
<!-- flapi: template group=agent file=customers.sql -->
```sql
SELECT c_custkey, c_name, c_mktsegment, c_acctbal
FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{ params.id }}
{{/params.id}}
{{#params.segment}}
  AND c_mktsegment = '{{{ params.segment }}}'
{{/params.segment}}
{{#params.min_balance}}
  AND c_acctbal >= {{ params.min_balance }}
{{/params.min_balance}}
ORDER BY c_custkey
```

Start the server from the project directory. It runs in the foreground, so leave it
running and use a second terminal for the commands that follow:

```bash
flapi -c flapi.yaml                       # or: uvx --from flapi-io flapi -c flapi.yaml
# port 8080 taken? add -p 8081 and use that port in the commands below
```

`mcp-tool.name` and `mcp-tool.description` are required. Tool names must be unique: if two
files declare the same tool name, the load fails and the error names both files. The
`request:` fields become the tool's arguments. Their validators run on every call, just as
they do for REST.

MCP is on by default (`mcp.enabled: true`). At startup, a server with tools and no
`mcp.auth` prints the `MCP_UNAUTHENTICATED_TOOLS` security warning. That is fine on a
laptop.

**Adding files later.** flAPI reads `sqls/` at startup. When you add or edit a file by
hand, restart the server (Ctrl-C, then start it again). If the server runs with
`--config-service`, you can load the file without a restart:
`flapii endpoints reload --file sqls/contacts.yaml`. Endpoints that are created or
changed through the config service update `tools/list` at once. flAPI sends no
`list_changed` notification, so clients must list the tools again to see a change.

## Call it

In the second terminal, list the tools and call one:

```bash
curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 1, "method": "tools/list"}'

curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 2, "method": "tools/call",
  "params": {"name": "find_customers", "arguments": {"id": 3}}}'

curl -s http://localhost:8080/customers?id=3          # the same SQL over REST
curl -s http://localhost:8080/mcp/health              # plain HTTP probe; reports tools_count
```

Tool inputs go under `arguments`, not `parameters`. A full client sends `initialize`
first, and flAPI agrees on the highest protocol version that both sides support (the
default is `2025-11-25`). flAPI returns an `Mcp-Session-Id` header, which a client may
echo back. Nothing depends on the session, because authentication is checked on every
request.

The rest of this guide uses a small shell function for these calls. Define it in the
second terminal:

```bash
mcp() { curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' "${@:2}" -d "$1"; echo; }
```

flAPI also serves the stateless MCP 2026-07-28 revision on the same URL. A modern client
sends no `initialize` and gets no session. Instead, each request carries the protocol
version in `params._meta` and repeats the method in HTTP headers. Clients do this for you;
the details are in [The 2026-07-28 protocol](#the-2026-07-28-protocol).

## Connect a client

| Client | How to connect | Auth | Protocol era | Tested |
|---|---|---|---|---|
| Claude Code | `claude mcp add --transport http` (below) | `--header "Authorization: ..."` | legacy (`initialize`) | yes: `Status: ✔ Connected` |
| Python MCP SDK (`mcp` 2.3) | `streamable_http_client` (below) | headers on the HTTP client | legacy, `2025-11-25` | yes |
| Claude Desktop | the `mcp-remote` bridge (below) | `--header` argument to `mcp-remote` | legacy | the bridge was tested against flAPI; Claude Desktop itself was not |
| Cursor, VS Code | their HTTP MCP server config (URL plus `headers`) | `Authorization` header | depends on the client | no |

### Claude Code

Claude Code speaks HTTP MCP directly:

```bash
# No mcp.auth
claude mcp add --transport http flapi http://localhost:8080/mcp/jsonrpc

# mcp.auth type: basic
claude mcp add --transport http flapi http://localhost:8080/mcp/jsonrpc \
  --header "Authorization: Basic $(printf 'analyst:s3cret' | base64)"

# mcp.auth type: bearer
claude mcp add --transport http flapi http://localhost:8080/mcp/jsonrpc \
  --header "Authorization: Bearer $FLAPI_MCP_TOKEN"

claude mcp get flapi          # Status: ✔ Connected
```

`--scope project` writes the entry to `.mcp.json`, so the team shares it. Keep tokens out
of that file. The flAPI Claude Code plugin registers the server for you, and adds skills,
commands and the `flapi_*` config tools; see
[Using flAPI with Claude Code](./claude-code.md).

### Python MCP SDK

```python
import asyncio, base64
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client
from mcp.shared._httpx_utils import create_mcp_http_client

URL = "http://localhost:8080/mcp/jsonrpc"
HEADERS = {}   # with mcp.auth: {"Authorization": "Basic " + base64.b64encode(b"analyst:s3cret").decode()}

async def main():
    async with create_mcp_http_client(headers=HEADERS) as http, \
               streamable_http_client(URL, http_client=http) as (read, write, *_), \
               ClientSession(read, write) as session:
        await session.initialize()
        print([t.name for t in (await session.list_tools()).tools])
        result = await session.call_tool("find_customers", {"id": 3})
        print(result.is_error, result.structured_content)

asyncio.run(main())
```

Run it with `uvx --with mcp python client.py`.

### Claude Desktop and other stdio-only clients

A client that can only start local (stdio) servers needs a bridge to HTTP.
[`mcp-remote`](https://www.npmjs.com/package/mcp-remote) is one:

```json
{"mcpServers": {"flapi": {"command": "npx",
  "args": ["-y", "mcp-remote", "http://localhost:8080/mcp/jsonrpc",
           "--header", "Authorization: Basic YW5hbHlzdDpzM2NyZXQ="]}}}
```

In production, put TLS in front of flAPI, because Basic credentials and tokens travel with
every request.

## Write tools that agents use well

An agent decides whether to call a tool, and with what, from three things: the
description, the input schema and the first results it gets back.

### Descriptions

The description is the only documentation the agent reads. Say:

- **what the tool returns**: one row per what, which columns, and in which units;
- **when to use it**, compared with your other tools;
- **how to get less**: which filters narrow the result, and that `limit` and `offset` page
  through it (see [Large results](#large-results-and-warehouse-cost)).

Give every `request:` field a `description` too. That text becomes the property's
`description` in the input schema. Keep a description under 2048 bytes; flAPI scans
every description when it loads the configuration
([description hygiene](#description-hygiene)).

### Typed inputs, from your validators

flAPI builds the `inputSchema` from the validators, so the agent sees types and limits
rather than bare strings. `tools/list` shows `find_customers` as:

```json
{"name": "find_customers",
 "description": "Look up customers of the wholesale business by key, ...",
 "inputSchema": {"type": "object", "properties": {
   "id":          {"type": "integer", "minimum": 1, "description": "Customer key (c_custkey), a positive integer"},
   "segment":     {"type": "string", "enum": ["AUTOMOBILE", "BUILDING", "FURNITURE", "HOUSEHOLD", "MACHINERY"], "description": "Market segment"},
   "min_balance": {"type": "number", "minimum": -1000, "description": "Only customers whose account balance is at least this many USD"}}}}
```

| Validator | Schema |
|---|---|
| `int` | `"type": "integer"` with `minimum` / `maximum` |
| `number` (alias `double`, `float`) | `"type": "number"` with `minimum` / `maximum` |
| `string` | `"type": "string"` with `minLength` / `maxLength`. A `regex` adds `pattern` |
| `enum` | `"enum": [...]` (type `string` unless another validator sets one) |
| `date`, `time`, `uuid`, `email` | `"type": "string"` with that `format` |
| `boolean` | `"type": "boolean"` |
| none | `"type": "string"` |

`required: true` lists the field in `required`. A field's `default` becomes the schema's
`default`, in the property's type. `field-in` makes no difference over MCP, because
arguments are one flat object. flAPI rejects arguments that the tool does not declare: a
typo such as `segmnt` must not silently drop a filter.

### Results: `structuredContent` and a learned `outputSchema`

A successful call returns the rows twice. Clients that read text get a text block, and
clients that want JSON get `structuredContent`:

```json
{"content": [{"type": "text", "text": "[{\"c_custkey\":3,\"c_name\":\"Customer#000000003\",...}]"}],
 "structuredContent": {"rows": [{"c_custkey": 3, "c_name": "Customer#000000003", "c_mktsegment": "AUTOMOBILE", "c_acctbal": 7498.12}],
                       "row_count": 1}}
```

After the first successful call of a tool, `tools/list` also advertises an `outputSchema`
for it: `{rows: array of objects, row_count: integer}`. The column types come from the
first row (every number is typed as `number`). A dry run never sets the schema, and a
config reload clears it. A write tool returns the same body as the REST write
(`rows_affected`, plus `data` with the `RETURNING` rows).

### Errors an agent can act on

If the agent can fix a failure, flAPI returns it as a normal result with `isError: true`,
and the text says what to change:

```bash
mcp '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"find_customers","arguments":{"segment":"boats"}}}'
# {"result":{"isError":true,"content":[{"text":"Invalid arguments for tool 'find_customers': segment: Invalid enum value",...
```

| Failure | What the agent gets |
|---|---|
| A validator rejected an argument | `isError`, e.g. `segment: Invalid enum value` |
| An undeclared argument | `isError`, `segmnt: Unknown parameter not defined in endpoint configuration` |
| The per-tool rate limit | `isError`, `Rate limit exceeded for tool '...'. Retry after N seconds. (retry_after_seconds=N)` |
| The SQL failed | `isError`, `Tool execution failed on the server. The details are in the server log under request_id req-...` |
| Unknown tool name | JSON-RPC error `-32602` |
| No credentials, or bad ones | JSON-RPC error `-32000`, HTTP 401 |
| Missing role | JSON-RPC error `-32000` `Permission denied: ...`, HTTP 403 |
| The tool's cache has not been built yet | JSON-RPC error `-32000`, HTTP 503 |

A failed query never returns the SQL or the database error to the caller, because they
can reveal paths, SQL and credentials. To find the cause, grep the server log for the
`request_id`. Validation messages are what an agent can act on, so give every argument a
validator with its valid range.

## Large results and warehouse cost

**A tool call has no default row limit.** A REST GET is paginated (100 rows per page), but
a `tools/call` returns every row that the template produces, and every row costs the agent
context and, on a warehouse, money.

- **`limit` and `offset` work on every tool**, even though they are not in its input
  schema. flAPI applies them outside your SQL, as it does for REST. The agent cannot know
  this unless the description says so, which is why `find_customers` ends with "pass
  limit and offset". Do not write `LIMIT {{ params.limit }}` in a template: that limit
  applies before flAPI's offset.
- **Always set `max-rows`, and say so in the description.** For tools the cut is
  **silent**: `row_count` reports the rows kept, and nothing says that rows were dropped.
  Resources do say so, with `_meta.truncated`.
- **Prefer cached tools over live warehouse queries.** A tool on a
  [cached endpoint](./caching.md) reads the local cache table, so an agent that calls it
  fifty times in a conversation does not run fifty warehouse queries.
- **Use `sample: true` for "how much is there?"** It returns
  `{"sampled": true, "row_count": N, "columns": [...]}` and no rows, as the text block and
  as `structuredContent` (which has no `rows` key then).
- **Put a per-tool rate limit on every tool** ([below](#per-tool-rate-limits)). It is a
  cost guard against an agent stuck in a loop.

`response:` shapes the result after the query has run. It applies to MCP only, so the REST
endpoint is unaffected:

`sqls/contacts.yaml`:
<!-- flapi: endpoint group=agent file=contacts.yaml -->
```yaml
mcp-tool:
  name: customer_contacts
  description: Customer names and phone numbers, at most 20 rows per call (phone redacted).
  response:
    max-rows: 20                 # rows after the 20th are dropped
    redact-columns: [c_phone]    # each value becomes "<redacted>"
template-source: contacts.sql
connection: [customers]
```
<!-- flapi: check-mcp customer_contacts {} -> ok contains ""row_count":20" -->
<!-- flapi: check-mcp customer_contacts {} -> ok contains ""c_phone":"<redacted>"" -->

`sqls/contacts.sql`:
<!-- flapi: template group=agent file=contacts.sql -->
```sql
SELECT c_custkey, c_name, c_phone FROM read_parquet('{{{ conn.path }}}') ORDER BY c_custkey
```

Restart the server, then:

```bash
mcp '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"customer_contacts","arguments":{}}}'
# ... "c_phone":"<redacted>" ... "row_count":20
```

`redact-columns` keeps the column, so the agent can see that it exists. A shaped result keeps
the columns in SELECT order and the numbers as the database returned them. The database still
computes the full result before `max-rows` cuts it, so put a filter in front of a big
query.

## Tool-only endpoints, resources and prompts

The files in this section go into the same project. Restart the server after adding them.

**Tool only.** Leave out `url-path`, and the file defines a tool with no REST endpoint:

`sqls/all-customers.yaml`:
<!-- flapi: endpoint group=agent file=all-customers.yaml -->
```yaml
mcp-tool:
  name: all_customers
  description: Every customer's key and name, ordered by key. Pass limit and offset to page.
template-source: all-customers.sql
connection: [customers]
```
<!-- flapi: check-mcp all_customers {} -> ok contains ""row_count":1500" -->
<!-- flapi: check-mcp all_customers {"limit": 2, "offset": 5} -> ok contains ""c_custkey":6" -->

`sqls/all-customers.sql`:
<!-- flapi: template group=agent file=all-customers.sql -->
```sql
SELECT c_custkey, c_name FROM read_parquet('{{{ conn.path }}}') ORDER BY c_custkey
```

```bash
mcp '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"all_customers","arguments":{"limit":2,"offset":5}}}'
# customers 6 and 7; without limit, all 1500
```

**Resources** are data that the agent (or the user, in the client) reads, rather than an
action it takes: a schema, a glossary, a reference table. A resource is listed by
`resources/list` and read by URI:

`sqls/customer-columns.yaml`:
<!-- flapi: endpoint group=agent file=customer-columns.yaml -->
```yaml
mcp-resource:
  name: customer_columns
  description: Column names and types of the customer table
  mime-type: application/json
template-source: customer-columns.sql
connection: [customers]
```
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"resources/list"} -> 200 contains "flapi://customer_columns" -->
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"flapi://customer_columns"}} -> 200 contains "c_acctbal" -->

`sqls/customer-columns.sql`:
<!-- flapi: template group=agent file=customer-columns.sql -->
```sql
SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM read_parquet('{{{ conn.path }}}'))
```

```bash
mcp '{"jsonrpc":"2.0","id":1,"method":"resources/list"}'
mcp '{"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"uri":"flapi://customer_columns"}}'
```

A **resource template** takes parameters from its URI. Each `{variable}` must be a
`request:` field, and that field's validators run when the resource is read. Templates are
listed by `resources/templates/list`, not by `resources/list`:

`sqls/customer.yaml`:
<!-- flapi: endpoint group=agent file=customer.yaml -->
```yaml
mcp-resource:
  name: customer
  description: One customer's key, name and balance
  uri-template: flapi://customers/{id}
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
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"resources/templates/list"} -> 200 contains "flapi://customers/{id}" -->
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"flapi://customers/7"}} -> 200 contains "Customer#000000007" -->
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"flapi://customers/x"}} -> 200 contains "id: Invalid integer value" -->

`sqls/customer.sql`:
<!-- flapi: template group=agent file=customer.sql -->
```sql
SELECT c_custkey, c_name, c_acctbal FROM read_parquet('{{{ conn.path }}}') WHERE c_custkey = {{ params.id }}
```

```bash
mcp '{"jsonrpc":"2.0","id":1,"method":"resources/templates/list"}'
mcp '{"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"uri":"flapi://customers/7"}}'
mcp '{"jsonrpc":"2.0","id":3,"method":"resources/read","params":{"uri":"flapi://customers/x"}}'   # -32602 id: Invalid integer value
```

A resource read is capped at 1000 rows by default; `mcp-resource.response.max-rows`
changes the cap.

**Prompts** are templates that the user picks in the client. A prompt runs no SQL, so it
needs no `template-source` and no connection:

`sqls/review-customer.yaml`:
<!-- flapi: endpoint group=agent file=review-customer.yaml -->
```yaml
mcp-prompt:
  name: review_customer
  description: Ask for a credit review of one customer
  template: |
    Review the credit standing of customer {{customer_id}}.
    Call find_customers with id={{customer_id}} first, and quote the balance you used.
  arguments: [customer_id]
```
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"prompts/get","params":{"name":"review_customer","arguments":{"customer_id":"7"}}} -> 200 contains "customer 7." -->

```bash
mcp '{"jsonrpc":"2.0","id":1,"method":"prompts/get","params":{"name":"review_customer","arguments":{"customer_id":"7"}}}'
```

`prompts/get` substitutes the arguments and returns one user message. All prompt
arguments are strings.

## Preview a call with `_dryRun`

Add `"_dryRun": true` to the arguments, and flAPI validates the arguments and renders the
SQL, but does not run it:

```bash
mcp '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"find_customers","arguments":{"id":3,"_dryRun":true}}}'
# "rendered_sql": "... WHERE 1=1 AND c_custkey = 3 ...", "bindings": [], "parameters": {"id": 3}
```

The result holds `dry_run: true`, `tool_name`, `rendered_sql`, `bindings` and the typed
`parameters`; `structuredContent` is the same object, with no `rows` (nothing ran). A bare `{{ params.x }}` outside a
section is a prepared-statement parameter, so it appears as `?` with its value in
`bindings`. Values inside a `{{#...}}` section, like `id` here, are rendered into the SQL.
Role checks, rate limits and validators all still apply, and credentials that the
template interpolates are scrubbed from the preview. Use dry runs to see what an agent's
arguments turn into, and to smoke-test every tool in CI.

## Protect it

<a id="limiting-who-can-call-it"></a>

### `mcp.auth` is separate from REST `auth:`

**An endpoint's `auth:` block, and the top-level `auth:` it inherits, protect REST
requests only.** They do not gate `tools/call`. With `mcp.auth` off, a tool on an endpoint
whose REST block requires the `admin` role returns every row to anonymous MCP callers.
The global `rate_limit:` does not cover `/mcp/jsonrpc` either. MCP has its own block,
`mcp.auth`, and once it is on, **every tool must name the roles that may call it**.

This example **starts a new project directory**, next to `mcp-demo`. Stop the first
server (Ctrl-C) first, or start this one with `-p 8081`:

```bash
mkdir mcp-secure && cd mcp-secure
mkdir -p data sqls
curl -L -o data/customers.parquet \
  https://github.com/DataZooDE/flapi/raw/main/examples/data/customers.parquet
```

`flapi.yaml`:
<!-- flapi: config group=secure allow=MCP_TOOL_NO_ALLOWED_ROLES -->
```yaml
project-name: mcp-secured
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
cors:
  allow-origins: ['https://app.example.com']   # see below
mcp:
  auth:
    enabled: true
    type: basic
    users:
      - username: analyst        # password: s3cret
        password: '$pbkdf2-sha256$600000$ZmxhcGktZ3VpZGUtc2FsdA$97qbFjyPX93/VhMF9FkDQ8L3hbz96bFE6BGEAbdsdgE'
        roles: [analyst]
      - username: intern         # password: intern
        password: '$pbkdf2-sha256$600000$ZmxhcGktZ3VpZGUtc2FsdA$QmRDEKBk4QbAcVgz9EYzEE0SPz5BJlL8NLI601WRZiU'
        roles: [reader]
    methods:
      tools/list:
        required: false          # anyone may list; calling still needs credentials
```

- **Passwords are PBKDF2 hashes.** A plaintext password works but triggers
  `AUTH_PLAINTEXT_PASSWORD` at startup. Make your own hashes with the command in
  [Authentication § 2](./authentication.md#2-store-password-hashes-not-passwords); these two
  are for the demo only.
- **`cors.allow-origins`** lists the web pages that may call the server from a browser.
  Without it, any origin may, and with authentication on, the startup auditor warns
  `CORS_WILDCARD_WITH_AUTH`. Claude Code, the SDKs and curl are not browsers and need no
  entry.

`sqls/balances.yaml`:
<!-- flapi: endpoint group=secure file=balances.yaml -->
```yaml
mcp-tool:
  name: my_balances
  description: Customer balances, with the name of the authenticated caller in column who.
  allowed-roles: [analyst]       # without this list, every call is denied
  rate-limit:
    max: 3                       # 3 calls per user per 60 s
    interval: 60
template-source: balances.sql
connection: [customers]
```
<!-- flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"tools/list"} -> 200 contains "my_balances" -->
<!-- flapi: check-mcp my_balances {"limit": 1} -> 401 -->
<!-- flapi: check-mcp my_balances {"limit": 1} user=analyst:wrong -> 401 -->
<!-- flapi: check-mcp my_balances {"limit": 1} user=intern:intern -> 403 contains "requires one of [analyst]; caller has [reader]" -->
<!-- flapi: check-mcp my_balances {"limit": 1} user=analyst:s3cret -> ok contains ""who":"analyst"" -->
<!-- flapi: check-mcp my_balances {"limit": 1} user=analyst:s3cret -> ok -->
<!-- flapi: check-mcp my_balances {"limit": 1} user=analyst:s3cret -> ok -->
<!-- flapi: check-mcp my_balances {"limit": 1} user=analyst:s3cret -> error contains "Rate limit exceeded" -->
<!-- flapi: check-mcp no_roles {} user=analyst:s3cret -> 403 contains "has no allowed-roles configured" -->

`sqls/balances.sql`:
<!-- flapi: template group=secure file=balances.sql -->
```sql
SELECT '{{{ auth.username }}}' AS who, c_custkey, c_acctbal
FROM read_parquet('{{{ conn.path }}}') ORDER BY c_custkey
```

`sqls/no-roles.yaml`:
<!-- flapi: endpoint group=secure file=no-roles.yaml -->
```yaml
mcp-tool:
  name: no_roles
  description: Declares no allowed-roles, so under mcp.auth nobody may call it.
template-source: balances.sql
connection: [customers]
```

Start it with `flapi -c flapi.yaml`, and in the second terminal:

```bash
URL=http://localhost:8080/mcp/jsonrpc
CALL='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"my_balances","arguments":{"limit":1}}}'

curl -s $URL -H 'Content-Type: application/json' -d "$CALL"                      # 401, no credentials
curl -s $URL -H 'Content-Type: application/json' -d "$CALL" -u intern:intern     # 403, role reader
curl -s $URL -H 'Content-Type: application/json' -d "$CALL" -u analyst:s3cret    # rows, who = analyst
curl -s $URL -H 'Content-Type: application/json' -u analyst:s3cret -d '{"jsonrpc":"2.0","id":2,
  "method":"tools/call","params":{"name":"no_roles","arguments":{}}}'             # 403, no allowed-roles
curl -s $URL -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":3,"method":"tools/list"}'   # open
```

- **401 or 403.** A missing or rejected credential gets HTTP 401 with JSON-RPC `-32000`. An
  authenticated caller without one of the tool's `allowed-roles` gets HTTP 403
  `Permission denied: ...`. A tool with no `allowed-roles` refuses every call while
  `mcp.auth` is on, and so do an `mcp-resource` or `mcp-prompt` without one. An empty list
  (`allowed-roles: []`) lets no one in. Without `mcp.auth`, flAPI does not check roles.
- **Per-method requirements.** `mcp.auth.methods.<method>.required: false` opens one
  method, such as `tools/list`. Under `mcp.auth`, `initialize` needs credentials too,
  unless you open it with `methods.initialize.required: false`. Only `server/discover`
  is always public.
- **The caller is in the template.** `auth.username`, `auth.roles` and the other `auth.*`
  variables hold the MCP principal, so one tool can filter rows per caller. A caller
  cannot set `__auth_*` arguments, because flAPI strips them.
- **Bearer tokens.** For agents that hold a token, use `type: bearer`: HS256 JWTs with a
  `roles` claim, signed with `jwt-secret`, or `type: oidc` for tokens from your
  identity provider ([Authentication § OIDC](./authentication.md#oidc); it needs a
  release after v26.11.25).

  ```yaml
  mcp:
    auth:
      enabled: true
      type: bearer
      jwt-secret: '{{env.FLAPI_MCP_JWT_SECRET}}'
      jwt-issuer: my-company
  ```

### Per-tool rate limits

`mcp-tool.rate-limit` gives each tool its own budget, counted per authenticated user. All
anonymous callers share one bucket. If the block is present, the limit is on unless you
set `enabled: false`. `max` defaults to 100 and `interval` to 60 seconds. Dry runs count
against the budget. An agent over the limit gets an `isError` result that tells it how
long to wait; in the example above, the fourth call by `analyst` within a minute does.

### Description hygiene

Every tool description is scanned at load time. A control character, a description over
2048 bytes, or a known prompt-injection phrase (`ignore previous instructions`,
`disregard the above`, `system:`, `you are now`, ...) produces a warning naming the tool
and the phrase:

```text
[warning] MCP tool 'sneaky' description: [DESCRIPTION_PROMPT_INJECTION] MCP tool description contains 'ignore previous instructions' at offset 17, ...
```

Set `mcp.strict-descriptions: true` to make any finding fatal: `--validate-config` fails,
and the server refuses to start. The scanner reads descriptions only, never the rows a
tool returns.

### `MCP_UNAUTHENTICATED_TOOLS`

On every start, flAPI's security auditor prints this warning while `mcp.enabled` is true,
at least one tool exists and `mcp.auth.enabled` is false. It does not stop the server.
Make your CI stop instead:

```bash
flapi -c flapi.yaml --validate-config 2>&1 | grep MCP_UNAUTHENTICATED_TOOLS && exit 1
```

Two more warnings are about MCP:

- `MCP_TOOL_NO_ALLOWED_ROLES`: `mcp.auth` is on and a tool, resource or prompt has no
  `allowed-roles`, so every caller gets a 403 (the `no_roles` tool above).
- `MCP_TOOL_BYPASSES_REST_AUTH`: `mcp.auth` is off and an endpoint whose REST route
  requires authentication is also an `mcp-tool`. The warning names the endpoint: its
  data is open through `/mcp/jsonrpc`.

## Threat model and safe defaults

An agent with a tool is a new kind of client: it reads everything the tool returns, and it
acts on what it reads.

- **Returned rows are untrusted input to the model.** A customer name or a comment column
  can hold "ignore your instructions and ...". The description scanner does not look at
  data. Redact or leave out free-text columns that the agent does not need, and keep tools
  narrow.
- **Data can leave through the agent's other tools.** An agent that holds your
  `find_customers` and also a web-fetch or e-mail tool can send rows anywhere. Give data
  agents only the tools they need, and keep `max-rows` small so one call cannot copy a
  table.
- **Keep the `flapi_*` config tools away from data agents.** A server started with
  `--config-service` lists `flapi_*` tools in `tools/list`, which can create, change and
  reload endpoints. Calls need the config-service token, but the safest setup runs data
  agents against a server without `--config-service`.
- **REST `auth:` is not MCP auth.** See [above](#mcpauth-is-separate-from-rest-auth).

Safe defaults for any server that an agent reaches:

| Setting | Why |
|---|---|
| `mcp.auth.enabled: true` | no anonymous calls |
| `allowed-roles` on every tool, resource and prompt | least privilege, per tool |
| `response.max-rows` on every tool | bounded context, bounded exfiltration |
| `response.redact-columns` for personal and free-text data | the model never sees it |
| `rate-limit` on every tool | stops loops and runaway cost |
| `mcp.strict-descriptions: true` | a poisoned description stops the server |
| CI fails on `MCP_UNAUTHENTICATED_TOOLS` | nobody ships an open server by accident |

## Long-running queries: Tasks

A query that runs for minutes should not hold an agent's HTTP request open. With
`async: true`, a modern client gets a task handle at once and then polls for the result.
Add this to the first project (`mcp-demo`):

`sqls/segment-totals.yaml`:
<!-- flapi: endpoint group=agent file=segment-totals.yaml -->
```yaml
mcp-tool:
  name: segment_totals
  description: Total account balance per market segment (slow; runs as a task for clients that support tasks)
  async: true                  # return a task at once
  # async-after-ms: 5000       # or: answer inline if done within 5 s, else return a task
template-source: segment-totals.sql
connection: [customers]
```
<!-- flapi: check-mcp segment_totals {} -> ok contains "AUTOMOBILE" -->

`sqls/segment-totals.sql`:
<!-- flapi: template group=agent file=segment-totals.sql -->
```sql
SELECT c_mktsegment, sum(c_acctbal) AS total FROM read_parquet('{{{ conn.path }}}') GROUP BY ALL ORDER BY 1
```

A call becomes a task only when all three hold: the request uses the
[2026-07-28 protocol](#the-2026-07-28-protocol), the client declared
`io.modelcontextprotocol/tasks` in `clientCapabilities.extensions`, and the tool opts in.
Any other call runs synchronously, so legacy clients never see a task:

```bash
mcp '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"segment_totals","arguments":{}}}'
# answered inline: AUTOMOBILE ..., BUILDING ...
```

A modern client gets `"resultType": "task"` and `{"task": {"taskId": "task_...", "status":
"working", "pollIntervalMs": 1000, "ttlMs": 3600000}}`. It polls `tasks/get` with that
`taskId` until `status` is `completed` (the tool result, same shape as a synchronous call,
is under `result`), `failed` or `cancelled`. A failed query ends the task `failed`:
`error` has the message (with the request id of the `tools/call`), and `result` still
holds the `isError` tool result. `tasks/cancel` interrupts the query, and the task ends `cancelled`. A
task ID belongs to the principal who created it. The curl commands are in
[The 2026-07-28 protocol](#the-2026-07-28-protocol).

`mcp.tasks` sets the workers, the queue depth, the retention and the suggested poll
interval:

```yaml
mcp:
  tasks:
    workers: 2
    queue-depth: 32            # a full queue answers "Task queue is full; retry shortly"
    default-ttl-ms: 3600000
    poll-interval-ms: 1000
```

Tasks survive a restart only when `duckdb.db_path` is a file. See
[MCP_REFERENCE §11.7](../MCP_REFERENCE.md#117-tasks-extension-long-running-tools).

## Trace an agent's calls

flAPI honours [SEP-414](https://modelcontextprotocol.io/seps/414-request-meta): a
`traceparent` in `params._meta` makes the tool call part of the agent's own trace, so one
trace runs from the agent's reasoning step to the DuckDB query:

```json
{"jsonrpc": "2.0", "id": 7, "method": "tools/call",
 "params": {"name": "find_customers", "arguments": {"id": 3},
            "_meta": {"traceparent": "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"}}}
```

When both are present, the `_meta` value wins over an HTTP `traceparent` header. flAPI
reads it even with tracing off, so the log lines and the audit record carry the agent's
trace ID. See [Observability](./observability.md) and
[OBSERVABILITY.md](../OBSERVABILITY.md).

## The 2026-07-28 protocol

You need this only if you write a client by hand. A request is **modern** when
`params._meta` holds `io.modelcontextprotocol/protocolVersion`. It must also carry
`io.modelcontextprotocol/clientCapabilities` (`{}` at least, else `-32602`), and three
HTTP headers must repeat the body: `MCP-Protocol-Version`, `Mcp-Method` and, for
`tools/call` and `prompts/get` (`params.name`) or `resources/read` (`params.uri`),
`Mcp-Name`. A missing or different header is `-32020` (HTTP 400); an unsupported version
is `-32022`.

```bash
META='"_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
  "io.modelcontextprotocol/clientCapabilities": {"extensions": {"io.modelcontextprotocol/tasks": {}}}}'
H=(-H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28')

# Discovery, the modern initialize. Always public.
curl -s http://localhost:8080/mcp/jsonrpc "${H[@]}" -H 'Mcp-Method: server/discover' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{$META}}"

# Start the task...
curl -s http://localhost:8080/mcp/jsonrpc "${H[@]}" -H 'Mcp-Method: tools/call' -H 'Mcp-Name: segment_totals' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"segment_totals\",\"arguments\":{},$META}}"

# ...then poll it: put the taskId from the previous answer into TASK
TASK=task_0123abcd
curl -s http://localhost:8080/mcp/jsonrpc "${H[@]}" -H 'Mcp-Method: tasks/get' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tasks/get\",\"params\":{\"taskId\":\"$TASK\",$META}}"
```

Every modern result carries `resultType` and the server info in `_meta`. A modern request
never gets a session. On the modern path `ping` and `logging/setLevel` are gone
(`-32601`); on the legacy path `server/discover` and `tasks/*` do not exist. Details:
[MCP_REFERENCE §11](../MCP_REFERENCE.md#11-mcp-2026-07-28-dual-era).

## Common mistakes

- **Relying on REST `auth:` to protect a tool.** It does not. Turn on `mcp.auth` and give
  every tool `allowed-roles`.
- **Turning on `mcp.auth` and forgetting `allowed-roles`.** Every call then answers 403
  `has no allowed-roles configured`.
- **Assuming a tool is paginated.** A tool returns every row. Set `max-rows` and tell the
  agent about `limit` and `offset`.
- **`LIMIT {{ params.limit }}` in the template.** `limit` belongs to flAPI's pagination.
  Your own limit runs first and breaks `offset`.
- **Adding a file and expecting the tool to appear.** Restart the server, or run
  `flapii endpoints reload --file sqls/<file>.yaml` against a server with
  `--config-service`.
- **A one-line description.** "Gets customers" leaves the agent guessing at units, filters
  and size. Say what one row is and how to narrow the result.
- **Arguments without validators.** An unvalidated argument appears as a bare `string` in
  the schema, and nothing but your template stands between it and the SQL.
- **`parameters` instead of `arguments`** in a hand-written `tools/call`.
- **Waiting for `list_changed`.** flAPI never sends it. Clients must list the tools again.

## Next

- [Authentication](./authentication.md): users, password hashes, JWT
- [Caching](./caching.md): serve agents from a cache table, not the warehouse
- [Using flAPI with Claude Code](./claude-code.md): the plugin and the `flapi_*` config tools
- [Observability](./observability.md): request IDs, traces, the audit log
- [REST endpoints](./rest-endpoints.md): the same files, served over HTTP

## How it works

- [MCP_REFERENCE](../MCP_REFERENCE.md): every method, [`tools/call` and dry-run](../MCP_REFERENCE.md#44-toolscall),
  [authentication](../MCP_REFERENCE.md#8-authentication) and
  [MCP 2026-07-28](../MCP_REFERENCE.md#11-mcp-2026-07-28-dual-era) (discovery, mirrored
  headers, tasks, `x-mcp-header`)
- [CONFIG_REFERENCE §2.6](../CONFIG_REFERENCE.md#26-mcp-configuration) (`mcp:`) and
  [§3.2](../CONFIG_REFERENCE.md#32-mcp-tools)–[§3.4](../CONFIG_REFERENCE.md#34-mcp-prompts)
  (`mcp-tool`, `mcp-resource`, `mcp-prompt`)
- [MCP_CONFIG_TOOLS_API](../MCP_CONFIG_TOOLS_API.md): the `flapi_*` config tools
- [spec/components/mcp-protocol.md](../spec/components/mcp-protocol.md): dispatch, sessions,
  SEP-414, the single-span contract
- Source: `src/mcp_route_handlers.cpp` (dispatch, both eras, results), `src/mcp_tool_handler.cpp`
  (validation, execution), `src/mcp_schema_builder.cpp` (`inputSchema`),
  `src/mcp_authorization_policy.cpp` (`allowed-roles`), `src/mcp_response_shaper.cpp`,
  `src/mcp_tool_rate_limiter.cpp`, `src/mcp_description_scanner.cpp`, `src/mcp_dry_run.cpp`,
  `src/mcp_task_manager.cpp`, `src/endpoint_config_parser.cpp` (`mcp-*` keys)
