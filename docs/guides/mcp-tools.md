# Creating MCP tools for AI agents

**Goal:** give an AI agent safe, useful access to your data, using the SQL you already
have.

Add an `mcp-tool:` block to an endpoint file and the endpoint becomes a tool. MCP
(the Model Context Protocol) is the protocol agents such as Claude use to find tools and
call them. flAPI serves it on the same port as the REST API, at `/mcp/jsonrpc`. You need
no second implementation and no second server.

Read this guide before you connect an agent to real data. Agents use a tool well when its
description is clear, its inputs are typed and its results are small. Treat MCP as open
until you turn on `mcp.auth`.

---

## Make an endpoint a tool

The examples in this guide use a project directory with the TPC-H customer table: 1500
customers with key, name, market segment, phone and account balance. Copy
`examples/data/customers.parquet` from the flAPI repository to `data/customers.parquet` in
a new directory, and put this `flapi.yaml` next to it:

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

Then add the endpoint file. This one is a REST endpoint and an MCP tool at the same time:

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

`mcp-tool.name` and `mcp-tool.description` are required. Tool names must be unique: if two
files declare the same tool name, the load fails and the error names both files. The
`request:` fields become the tool's arguments. Their validators run on every call, just as
they do for REST.

MCP is on by default (`mcp.enabled: true`). A server with tools and no `mcp.auth` prints the
`MCP_UNAUTHENTICATED_TOOLS` security warning at startup. That is correct for a laptop.
Before anyone else can reach the server, read [Protect it](#protect-it).

When you create, update, delete or reload an endpoint, `tools/list` changes at once, with no
restart. flAPI sends no `list_changed` notification, so clients must list the tools again
to see the change.

## Call it

There are two ways to speak MCP to flAPI, and both use `POST /mcp/jsonrpc`.

### Legacy clients: `initialize`, then call

Most clients today use the session-based protocol (`2024-11-05` to `2025-11-25`). The
client sends `initialize` first, and flAPI agrees on the highest protocol version that both
sides support (the default is `2025-11-25`):

```bash
curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 1, "method": "initialize",
  "params": {"protocolVersion": "2025-11-25", "capabilities": {},
             "clientInfo": {"name": "curl", "version": "1"}}}'

curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 2, "method": "tools/list"}'

curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 3, "method": "tools/call",
  "params": {"name": "find_customers", "arguments": {"id": 3}}}'
```

Tool inputs go under `arguments`, not `parameters`. flAPI returns an `Mcp-Session-Id`
header, and you may echo it back. Nothing depends on the session: authentication is checked
on every request. `GET /mcp/health` is a plain HTTP probe. It reports `tools_count`.

### Modern clients: MCP 2026-07-28, stateless

In MCP 2026-07-28, the client sends no `initialize` and has no session. Instead, every
request carries its own preamble in `params._meta`, and the request body is mirrored into
HTTP headers so that a proxy can route it without reading the body:

| Where | What | If it is missing or wrong |
|---|---|---|
| `_meta["io.modelcontextprotocol/protocolVersion"]` | `"2026-07-28"` | Without it, flAPI treats the request as legacy. An unsupported version gets `-32022` (HTTP 400) |
| `_meta["io.modelcontextprotocol/clientCapabilities"]` | an object, `{}` at least | `-32602` (HTTP 400) |
| `MCP-Protocol-Version` header | the same version | `-32020` HeaderMismatch (HTTP 400) |
| `Mcp-Method` header | the JSON-RPC method | `-32020` |
| `Mcp-Name` header | `params.name` (`tools/call`, `prompts/get`) or `params.uri` (`resources/read`) | `-32020` |

```bash
META='"_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                "io.modelcontextprotocol/clientCapabilities": {}}'

# Discovery: what initialize used to do. No authentication is required for it.
curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: server/discover' \
  -d "{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": \"server/discover\", \"params\": {$META}}"

curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/call' -H 'Mcp-Name: find_customers' \
  -d "{\"jsonrpc\": \"2.0\", \"id\": 2, \"method\": \"tools/call\",
       \"params\": {\"name\": \"find_customers\", \"arguments\": {\"id\": 3}, $META}}"
```

`server/discover` returns `supportedVersions`, `capabilities` and the server info. Every
modern result also carries `resultType` (`"complete"`, or `"task"` for a
[task](#long-running-queries-tasks)). A modern request never gets a session. On the modern
path, `ping` and `logging/setLevel` are gone (`-32601`). On the legacy path,
`server/discover` and `tasks/*` do not exist (`-32601`).

## Write tools that agents use well

An agent decides whether to call a tool, and with what, from three things: the
description, the input schema and the first results it gets back.

### Descriptions

The description is the only documentation the agent reads. Say:

- **what the tool returns**: one row per what, which columns, and in which units;
- **when to use it**, compared with your other tools;
- **how to get less**: which filters narrow the result, and that `limit` and `offset` page
  through it ([see below](#row-caps-limit-and-offset)).

Give every `request:` field a `description` too. That text becomes the property's
`description` in the input schema. Keep a description under 2048 bytes. When flAPI loads
the configuration, it checks every description (see
[the description scanner](#description-hygiene)).

### Typed inputs, from your validators

flAPI builds the `inputSchema` from the validators, so the agent sees types and limits
rather than bare strings. The tool above is listed as:

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
arguments are one flat object.

flAPI rejects arguments that the tool does not declare. A typo such as `segmnt` must not
silently drop a filter, and an undeclared value has no validator behind it.

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
config reload clears it, because the columns may have changed. A write tool returns the
same body as the REST write (`rows_affected`, plus `data` with the `RETURNING` rows).

### Errors an agent can act on

If the agent can fix a failure, flAPI returns it as a normal result with `isError: true`,
and the text says what to change:

| Failure | What the agent gets |
|---|---|
| A validator rejected an argument | `isError`, e.g. `Invalid arguments for tool 'find_customers': segment: Invalid enum value` |
| An undeclared argument | `isError`, `segmnt: Unknown parameter not defined in endpoint configuration` |
| The per-tool rate limit | `isError`, `Rate limit exceeded for tool '...'. Retry after N seconds. (retry_after_seconds=N)` |
| The SQL failed | `isError`, `Tool execution failed on the server. The details are in the server log under request_id req-...` |
| Unknown tool name | JSON-RPC error `-32602` |
| No credentials, or bad ones | JSON-RPC error `-32000`, HTTP 401 |
| Missing role | JSON-RPC error `-32000` `Permission denied: ...`, HTTP 403 |
| The tool's cache has not been built yet | JSON-RPC error `-32000`, HTTP 503 |

A failed query never returns the SQL or the database error to the caller, because they
can reveal paths, SQL and credentials. To find the cause, grep the server log for the
`request_id`. Validation messages are what the agent can act on, so add a validator for
every argument that has a valid range.

### Row caps, `limit` and `offset`

**A tool call has no default row limit.** A REST GET is paginated (100 rows per page), but
a `tools/call` returns every row that the template produces. The tool-only `all_customers`
example below returns all 1500 customers.

An agent can still page. **`limit` and `offset` are honoured on every tool, even though
they are not in its input schema**, and flAPI applies them outside your SQL, as it does for
REST. The agent cannot know this unless the description says so. That is why the
description above ends with "pass limit and offset".

Do not write `LIMIT {{ params.limit }}` in a template, for the same reason as on REST:
that limit applies before flAPI's offset.

### Response shaping

Agent context is expensive, and some columns should never reach a model. The
`response:` block shapes the result after the query has run. It applies to MCP only, so
the REST endpoint is unaffected:

`sqls/contacts.yaml`:
<!-- flapi: endpoint group=agent file=contacts.yaml -->
```yaml
mcp-tool:
  name: customer_contacts
  description: Customer names and phone numbers, at most 20 rows per call.
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

- `max-rows` truncates **silently**. The database still computes the full result, and
  `row_count` reports the rows that were kept. Nothing in the response says that rows were
  cut, so say so in the description, and put a filter or a pagination hint in front of a
  big query.
- `redact-columns` keeps the column but replaces each of its values with `"<redacted>"`.
  The agent can see that the column exists.
- `sample: true` returns no rows at all, only `{"columns": [...], "row_count": N,
  "sampled": true}`. Use it for a tool that answers "how much is there?".

## Tool-only endpoints, resources and prompts

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
<!-- flapi: check GET /all_customers -> 404 -->

`sqls/all-customers.sql`:
<!-- flapi: template group=agent file=all-customers.sql -->
```sql
SELECT c_custkey, c_name FROM read_parquet('{{{ conn.path }}}') ORDER BY c_custkey
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

A **resource template** takes parameters from its URI. Each `{variable}` must be a
`request:` field, and that field's validators run when the resource is read. A template is
listed by `resources/templates/list`, and `resources/list` does not show it:

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

A resource read is capped at 1000 rows by default. Set `mcp-resource.response.max-rows` to
change the cap. Unlike a tool, a capped read says so, with `_meta.truncated`.

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

`prompts/get` substitutes the arguments and returns one user message. All prompt
arguments are strings.

## Long-running queries: Tasks

A query that runs for minutes should not hold an agent's HTTP request open. If you set
`async: true`, a modern client gets a task handle at once and then polls for the result:

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

A call becomes a task only when all three of these hold: the request is modern, the client
declared `io.modelcontextprotocol/tasks` in
`clientCapabilities.extensions`, and the tool opts in. Any other call runs synchronously,
so legacy clients never see a task: a legacy `tools/call` of `segment_totals` returns the
totals inline.

```bash
META='"_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
  "io.modelcontextprotocol/clientCapabilities": {"extensions": {"io.modelcontextprotocol/tasks": {}}}}'

curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/call' -H 'Mcp-Name: segment_totals' \
  -d "{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": \"tools/call\",
       \"params\": {\"name\": \"segment_totals\", \"arguments\": {}, $META}}"
# -> "resultType": "task", "task": {"taskId": "task_...", "status": "working", "pollIntervalMs": 1000, "ttlMs": 3600000}

curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tasks/get' \
  -d "{\"jsonrpc\": \"2.0\", \"id\": 2, \"method\": \"tasks/get\", \"params\": {\"taskId\": \"task_...\", $META}}"
# -> "task": {"status": "completed", ...}, "result": {"content": [...], "structuredContent": {...}}
```

The tool's own result is under `result`, with the same shape as a synchronous call. A
query that fails still completes the task, and its `result` carries `isError`.
`tasks/cancel` cancels a task that is still queued. If the query is already running,
`tasks/cancel` marks the task for cancellation, but the query runs to completion. A task
ID belongs to the principal who created it. `mcp.tasks` sets the number of workers, the
queue depth, the retention and the suggested poll interval:

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

<a id="limiting-who-can-call-it"></a>

## Protect it

### `mcp.auth` is separate from REST `auth:`

**An endpoint's `auth:` block, and the top-level `auth:` it inherits, protect REST
requests only.** They do not gate `tools/call`. The global `rate_limit:` does not cover
`/mcp/jsonrpc` either. MCP has its own block, `mcp.auth`, and once it is on, **every tool
must name the roles that may call it**.

The example below **starts a new project directory**, with the same
`data/customers.parquet`. It has two users, an analyst and an intern:

`flapi.yaml`:
<!-- flapi: config group=secure -->
```yaml
project-name: mcp-secured
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
cors:
  allow-origins: ['https://app.example.com']
mcp:
  auth:
    enabled: true
    type: basic
    users:                       # PBKDF2 hashes; never ship plaintext passwords
      - username: analyst        # password: s3cret (generate your own hash; see Authentication)
        password: '$pbkdf2-sha256$600000$ZmxhcGktZ3VpZGUtc2FsdA$97qbFjyPX93/VhMF9FkDQ8L3hbz96bFE6BGEAbdsdgE'
        roles: [analyst]
      - username: intern         # password: intern
        password: '$pbkdf2-sha256$600000$ZmxhcGktZ3VpZGUtc2FsdA$QmRDEKBk4QbAcVgz9EYzEE0SPz5BJlL8NLI601WRZiU'
        roles: [reader]
    methods:
      tools/list:
        required: false          # anyone may list; calling still needs credentials
```

`sqls/balances.yaml`:
<!-- flapi: endpoint group=secure file=balances.yaml -->
```yaml
mcp-tool:
  name: my_balances
  description: Customer balances, with the name of the authenticated caller in column who.
  allowed-roles: [analyst]       # without this list, every call is denied
  rate-limit:
    max: 3                       # 3 calls per principal per 60 s
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

- **401 or 403.** A missing or rejected credential gets HTTP 401 with JSON-RPC `-32000`. An
  authenticated caller without one of the tool's `allowed-roles` gets HTTP 403
  `Permission denied: ...`. A tool with no `allowed-roles` refuses every call while
  `mcp.auth` is on, and the same goes for `mcp-resource` and `mcp-prompt`. An empty list
  (`allowed-roles: []`) lets no one in. Without `mcp.auth`, flAPI does not check roles.
- **Per-method requirements.** `mcp.auth.methods.<method>.required: false` opens one
  method, such as `tools/list`. `initialize` and `server/discover` are always reachable.
- **The caller is in the template.** `auth.username`, `auth.roles` and the other `auth.*`
  variables hold the MCP principal, so one tool can filter rows per caller. A caller
  cannot set `__auth_*` arguments, because flAPI strips them.
- **Bearer and OIDC.** For agents that hold a token, use `type: bearer` (HS256 JWTs with a
  `roles` claim) or `type: oidc`. With OIDC, flAPI serves
  `/.well-known/oauth-protected-resource`, so OAuth-capable clients can discover the
  identity provider:

  ```yaml
  mcp:
    auth:
      enabled: true
      type: bearer
      jwt-secret: '{{env.FLAPI_MCP_JWT_SECRET}}'
      jwt-issuer: my-company
  ```

  See [Authentication](./authentication.md) and
  [MCP_REFERENCE §8](../MCP_REFERENCE.md#8-authentication).

### Per-tool rate limits

`mcp-tool.rate-limit` gives each tool its own budget, counted per authenticated user. All
anonymous callers share one bucket. If the block is present, the limit is on unless you
set `enabled: false`. `max` defaults to 100 and `interval` to 60 seconds. Dry runs count
against the budget. An agent over the limit gets an `isError` result that tells it how
long to wait.

### Description hygiene

Every tool description is scanned at load time. A control character, a description over
2048 bytes, or a known prompt-injection phrase (`ignore previous instructions`,
`disregard the above`, `system:`, `you are now`, ...) produces a warning naming the tool
and the phrase:

```text
[warning] MCP tool 'sneaky' description: [DESCRIPTION_PROMPT_INJECTION] MCP tool description contains 'ignore previous instructions' at offset 17, ...
```

Set `mcp.strict-descriptions: true` to make any finding fatal: `--validate-config` fails,
and the server refuses to start. Turn this on when endpoint files come from more than one
author, or from a repository that agents can write to.

### `MCP_UNAUTHENTICATED_TOOLS`

On every start, flAPI's security auditor prints this warning while `mcp.enabled` is true,
at least one tool exists and `mcp.auth.enabled` is false. It does not stop the server, so
treat it as a release blocker in your own checks. Anyone who can reach the port can call
every tool.

## Preview a call with `_dryRun`

Add `"_dryRun": true` to the arguments, and flAPI validates the arguments and renders the
SQL, but does not run it:

```bash
curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 1, "method": "tools/call",
  "params": {"name": "find_customers", "arguments": {"id": 3, "_dryRun": true}}}'
```

The text block (and `structuredContent`) holds `dry_run: true`, `tool_name`,
`rendered_sql` (typed parameters appear as `?`, with their values in `bindings`) and the
typed `parameters`. `structuredContent` also holds `rows: []`. Role checks, rate limits and
validators all still apply. Credentials that the template interpolates are scrubbed from
the preview. Use dry runs to check what an agent's arguments turn into before you let the
query run, and to smoke-test every tool in CI.

## Connect real clients

### Claude Code

Claude Code speaks HTTP MCP directly:

```bash
# No mcp.auth
claude mcp add --transport http flapi http://localhost:8080/mcp/jsonrpc

# mcp.auth type: basic
claude mcp add --transport http flapi http://localhost:8080/mcp/jsonrpc \
  --header "Authorization: Basic $(printf 'analyst:s3cret' | base64)"

# mcp.auth type: bearer or oidc
claude mcp add --transport http flapi http://localhost:8080/mcp/jsonrpc \
  --header "Authorization: Bearer $FLAPI_MCP_TOKEN"
```

Run `claude mcp get flapi` to check the result. It shows `Status: ✔ Connected` once the
server answers. `--scope project` writes the entry to `.mcp.json`, so the team shares it.
Keep tokens out of that file.

The flAPI Claude Code plugin registers the server for you, and adds skills, commands and
the `flapi_*` config tools. See [Using flAPI with Claude Code](./claude-code.md).

### Other clients

Any client that supports MCP over HTTP needs only the URL `http(s)://<host>:<port>/mcp/jsonrpc`
and, with `mcp.auth`, an `Authorization` header. A client that can only start local
(stdio) servers needs a bridge between stdio and HTTP. In production, put TLS in front of
flAPI, because Basic credentials and tokens travel with every request.

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

## Common mistakes

- **Relying on REST `auth:` to protect a tool.** It does not. Turn on `mcp.auth` and give
  every tool `allowed-roles`.
- **Turning on `mcp.auth` and forgetting `allowed-roles`.** Every call then answers 403
  `has no allowed-roles configured`.
- **Assuming a tool is paginated.** A tool returns every row. Tell the agent about
  `limit` and `offset`, or cap the result with `max-rows`.
- **`LIMIT {{ params.limit }}` in the template.** `limit` belongs to flAPI's pagination.
  Your own limit runs first and breaks `offset`.
- **A one-line description.** "Gets customers" leaves the agent guessing at units, filters
  and size. Say what one row is and how to narrow the result.
- **Arguments without validators.** An unvalidated argument appears as a bare `string` in
  the schema, and nothing but your template stands between it and the SQL.
- **`parameters` instead of `arguments`** in a hand-written `tools/call`.
- **A modern request without the mirrored headers.** You get `-32020`. Send
  `MCP-Protocol-Version`, `Mcp-Method` and, for `tools/call`, `Mcp-Name`.
- **Waiting for `list_changed`.** flAPI never sends it. Clients must list the tools again
  after an endpoint changes.

## Next

- [Authentication](./authentication.md): users, password hashes, JWT and OIDC
- [Using flAPI with Claude Code](./claude-code.md): the plugin and the `flapi_*` config tools
- [Observability](./observability.md): request IDs, traces, the audit log
- [REST endpoints](./rest-endpoints.md): the same files, served over HTTP
- [MCP_CONFIG_TOOLS_API](../MCP_CONFIG_TOOLS_API.md): let an agent manage endpoints

## How it works

- [MCP_REFERENCE](../MCP_REFERENCE.md): every method, [`tools/call` and dry-run](../MCP_REFERENCE.md#44-toolscall),
  [authentication](../MCP_REFERENCE.md#8-authentication) and
  [MCP 2026-07-28](../MCP_REFERENCE.md#11-mcp-2026-07-28-dual-era) (discovery, mirrored
  headers, tasks, `x-mcp-header`)
- [CONFIG_REFERENCE §2.6](../CONFIG_REFERENCE.md#26-mcp-configuration) (`mcp:`) and
  [§3.2](../CONFIG_REFERENCE.md#32-mcp-tools)–[§3.4](../CONFIG_REFERENCE.md#34-mcp-prompts)
  (`mcp-tool`, `mcp-resource`, `mcp-prompt`)
- [spec/components/mcp-protocol.md](../spec/components/mcp-protocol.md): dispatch, sessions,
  SEP-414, the single-span contract
- Source: `src/mcp_route_handlers.cpp` (dispatch, both eras, results), `src/mcp_tool_handler.cpp`
  (validation, execution), `src/mcp_schema_builder.cpp` (`inputSchema`),
  `src/mcp_authorization_policy.cpp` (`allowed-roles`), `src/mcp_response_shaper.cpp`,
  `src/mcp_tool_rate_limiter.cpp`, `src/mcp_description_scanner.cpp`, `src/mcp_dry_run.cpp`,
  `src/mcp_task_manager.cpp`, `src/endpoint_config_parser.cpp` (`mcp-*` keys)
