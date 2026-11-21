---
name: flapi-mcp
description: >
  Exposing flAPI endpoints to AI agents over MCP: `mcp-tool` (name, description,
  result-mime-type, allowed-roles, response shaping, rate-limit, async /
  async-after-ms), `mcp-resource`, `mcp-prompt`, how request fields become the
  tool input schema, writing descriptions an agent can act on, the description
  scanner, dry-run, sessions, async tasks, the top-level `mcp:` block and server
  instructions. Covers connecting Claude Code, Claude Desktop and other clients and
  calling tools/list and tools/call with curl. Use when a user wants an MCP tool,
  resource or prompt, needs an agent to call their SQL, or an MCP call misbehaves.
version: 26.11.21
user-invocable: false
---

# flAPI MCP Cheat Sheet

**flAPI:** v26.11.21 | **Skill:** Develop | **See also:** `flapi-rest`, `flapi-auth`, `flapi-live-config`, `flapi-testing`, `flapi-operate`

MCP is served on the **same port as REST**, at `POST /mcp/jsonrpc` (JSON-RPC 2.0).
An endpoint YAML with an `mcp-tool:` block is a tool. If it also has `url-path:`, it is a
REST endpoint too. One SQL template serves both.

## Critical gotchas

- **`tools/list`, `resources/list` and `prompts/list` follow endpoint changes.** Every
  create/update/delete/reload (config service, `flapii`, the plugin hook or a `flapi_*`
  tool) rebuilds them; no restart. Clients are not notified (`listChanged` stays
  `false`), so re-list after a change; a pagination cursor from before it is rejected.
  **Exception: a remote `template.path` (bucket or http).** Endpoint files there are read-only through
  the config service and the `flapi_*` tools (they answer 501); edit the files in the bucket, then
  restart flAPI. SQL templates are re-read on each request (within `storage.cache.ttl`), YAML only at boot.
- **An MCP call has no default row limit.** REST pages at 100 rows. A `tools/call` with no
  `limit` argument returns every row the SQL produces (1500 rows in the customers
  fixture). Declare a `limit` request field with an `int` validator and a `default`, and/or
  set `mcp-tool.response.max-rows`. `max-rows` truncates silently, after the query ran.
- **Do not write `LIMIT` in the template.** flAPI wraps the query in `LIMIT/OFFSET`, both for
  REST pagination and for an MCP `limit` argument. See `flapi-rest`.
- **The tool's argument types come from validators only.** `int` -> `integer`,
  `number`/`double`/`float` -> `number`, `boolean` -> `boolean`, `enum` -> `enum` list, and
  everything else (or no validator) -> `string`. `field-type:` is read by nothing (load
  warning). A `default:` is emitted in the schema with that type (`10`, `true`).
- **Enum values go under `allowedValues:`** (`values:` is accepted as an alias).
- **`async: true` only takes effect for a modern (2026-07-28) client that declares the tasks
  extension.** Every other (legacy-era) client gets a normal synchronous result.
- **The description scanner warns by default and fails only with `mcp.strict-descriptions:
  true`.** It flags the substring `system:` anywhere, so "file system: ..." trips it.
- **`flapi_*` config tools need the config-service token** on the MCP request:
  `X-Config-Token: <token>` is read first, then `Authorization: Bearer <token>` (only
  while `mcp.auth` is off; with it on, `Authorization` is the MCP credential). With
  `--config-service` they are LISTED in `tools/list` even without the token; a call
  without it is JSON-RPC `-32001 Authentication required for tool: ...`. Without
  `--config-service` they are not listed. Startup warns about a token under 16 characters.
- **There is no `mcp.port` or `mcp.host`.** Setting either only logs a warning.
- **The `customers.parquet` fixture ignores filters on `c_mktsegment`.** `c_mktsegment =
  'BUILDING'` returns 0 rows in DuckDB itself. Filter on `c_nationkey` or `c_custkey` in
  examples and tests.

## One endpoint, REST and MCP

The tool below is also `GET /customer-lookup`.

```yaml
# flapi: endpoint group=lookup allow=MCP_UNAUTHENTICATED_TOOLS file=customer_lookup.yaml
url-path: /customer-lookup          # drop this line for an MCP-only tool
method: GET
mcp-tool:
  name: customer_lookup             # [a-z0-9_]; what the agent calls
  description: >
    Find customers of one nation, highest account balance first. Returns
    c_custkey, c_name, c_nationkey and c_acctbal. Pass nation (0-24) to filter;
    omit it for all nations. limit caps rows (1-100, default 10).
  result-mime-type: application/json   # default
  response:
    max-rows: 50                     # hard cap after the query, silent
request:
  - field-name: nation
    field-in: query
    description: Nation key, an integer 0-24. Omit for all nations.
    required: false
    validators:
      - type: int
        min: 0
        max: 24
  - field-name: limit
    field-in: query
    description: Maximum rows to return, 1-100. Default 10.
    required: false
    default: 10                      # applies to MCP calls; REST pages still default to 100
    validators:
      - type: int
        min: 1
        max: 100
template-source: customer_lookup.sql
connection: [customers]
# flapi: check GET /customer-lookup?nation=3&limit=2 -> 200
# flapi: check GET /customer-lookup?nation=99 -> 400
# flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"tools/list"} -> 200
# flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"customer_lookup","arguments":{"nation":3,"limit":2}}} -> 200
```

```sql
-- flapi: template group=lookup file=customer_lookup.sql
SELECT c_custkey, c_name, c_nationkey, c_acctbal
FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.nation}}
  AND c_nationkey = {{ params.nation }}
{{/params.nation}}
ORDER BY c_acctbal DESC
```

The `tools/list` entry the server generates from it (verified, `tools/list` on a running
server):

```json
{"name": "customer_lookup",
 "description": "Find customers of one nation, ... limit caps rows (1-100, default 10).\n",
 "inputSchema": {"type": "object", "properties": {
   "nation": {"type": "integer", "minimum": 0, "maximum": 24,
              "description": "Nation key, an integer 0-24. Omit for all nations."},
   "limit":  {"type": "integer", "minimum": 1, "maximum": 100, "default": 10,
              "description": "Maximum rows to return, 1-100. Default 10."}}}}
```

A `required: true` field is listed in `inputSchema.required`. `date`/`time`/`uuid`/`email`
become `string` with `format`. A string validator's `regex` becomes `pattern`, and its
`min`/`max` (or `min-length`/`max-length`) become `minLength`/`maxLength`. A field's
`mcp-header: Name` adds `"x-mcp-header": "Name"` (modern clients mirror it into an
`Mcp-Param-Name` header; never put a secret there, which fails config load).

## mcp-tool keys

| Key | Default | Effect |
|---|---|---|
| `name` | required | Tool name in `tools/list` / `tools/call` |
| `description` | required | What the agent reads. Scanned (below) |
| `result-mime-type` | `application/json` | MIME type of the result |
| `allowed-roles` | unset | Roles that may call it. **Required on every tool once `mcp.auth.enabled: true`**: a tool without it is denied. `[]` denies everyone. Ignored while MCP auth is off |
| `response.max-rows` | unset | Cap rows returned to the agent |
| `response.redact-columns` | `[]` | Replace these columns' values with `"<redacted>"` (the key stays) |
| `response.sample` | `false` | Return only `{sampled, row_count, columns}`, never rows |
| `rate-limit` | off | `{enabled, max, interval}`: per tool, per principal (anonymous callers share one bucket) |
| `tracing.capture` | global | `off`/`metadata`/`payload` for this tool's spans |
| `async` | `false` | Run as an MCP task: return a task id at once (modern clients only) |
| `async-after-ms` | `0` | Integer ms. Run synchronously, become a task if not done in time |

Auth roles and MCP OAuth: see `flapi-auth`.

## Writing descriptions an agent can act on

The tool description and each field `description` are all the agent has.

- Say **what it returns** (columns, order, row cap), **when to use it** and **what each
  argument means**, with units, ranges and allowed values.
- Name sibling tools when the choice matters: "for totals use `orders_summary`".
- Keep it under 2048 bytes. Put long guidance in `mcp.instructions-file`.
- Describe defaults that you actually set (`default:`), not ones you hope for.

The scanner (`src/mcp_description_scanner.cpp`) checks `mcp-tool.description` only (not
resources or prompts):

| Code | Trigger |
|---|---|
| `DESCRIPTION_CONTROL_CHARACTER` | Any control byte except newline, carriage return, tab |
| `DESCRIPTION_TOO_LONG` | More than 2048 bytes |
| `DESCRIPTION_PROMPT_INJECTION` | Case-insensitive substrings: `ignore previous instructions`, `ignore the above`, `disregard previous instructions`, `disregard the above`, `system:`, `you are now` |

Each hit logs `[WARNING] MCP tool 'x' description: [CODE] ...` and startup goes on. With
`mcp.strict-descriptions: true`, the first hit stops startup and `--validate-config` fails.

## Resources, prompts, async tools and the mcp: block

A complete project: server instructions, a resource, a prompt and an async tool.

```yaml
# flapi: config group=agentkit allow=MCP_UNAUTHENTICATED_TOOLS
project-name: agent-kit
project-description: MCP resources, prompts and an async tool
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
  northwind:
    init: |
      INSTALL sqlite;
      LOAD sqlite;
      ATTACH IF NOT EXISTS './data/northwind.sqlite' AS nw (TYPE sqlite, READ_ONLY);
mcp:
  enabled: true                      # default true
  instructions-file: ./mcp_instructions.md   # relative to flapi.yaml; `instructions:` inline wins over it
  strict-descriptions: true          # refuse to start on a flagged description
  page-size: 0                       # >0 paginates tools/list etc. with nextCursor
  tasks:
    workers: 2
    queue-depth: 32
    default-ttl-ms: 3600000
    poll-interval-ms: 1000
```

```markdown
# flapi: file group=agentkit file=mcp_instructions.md
# Customer and product data

- `segment_summary` (resource) has counts per market segment.
- `top_products` may run as a background task; poll it.
```

The instructions are returned in the `initialize` result as `instructions` (and by
`server/discover`).

```yaml
# flapi: endpoint group=agentkit file=segments.yaml
mcp-resource:
  name: segment_summary              # URI: flapi://segment_summary
  description: Customer count and average balance per market segment.
  mime-type: application/json        # default
template-source: segments.sql
connection: [customers]
```

```sql
-- flapi: template group=agentkit file=segments.sql
SELECT c_mktsegment AS segment, count(*) AS customers, round(avg(c_acctbal), 2) AS avg_balance
FROM read_parquet('{{{ conn.path }}}')
GROUP BY ALL
ORDER BY segment
```

```yaml
# flapi: endpoint group=agentkit file=segment_prompt.yaml
mcp-prompt:
  name: segment_review
  description: Ask the agent to review one market segment.
  template: |
    Review the {{segment}} market segment. Read flapi://segment_summary first,
    then summarise how its average balance compares with the others.
  arguments:
    - segment                        # names only; each is advertised as a string
```

```yaml
# flapi: endpoint group=agentkit file=top_products.yaml
mcp-tool:                            # no url-path: MCP only, no REST route
  name: top_products
  description: The ten most expensive Northwind products with their unit price.
  async: true                        # or: async-after-ms: 5000
template-source: top_products.sql
connection: [northwind]
```

```sql
-- flapi: template group=agentkit file=top_products.sql
SELECT ProductName, UnitPrice FROM nw.Products ORDER BY UnitPrice DESC LIMIT 10
```

(`LIMIT` is fine here: the tool has no `limit` field, so nothing wraps it.)

- A **prompt** has no SQL. `prompts/get` renders `template` with the arguments (Mustache).
- A **resource** runs its SQL on `resources/read` and returns
  `contents[{uri, mimeType, text}]`. `uri-template: flapi://customers/{id}` lists it under
  `resources/templates/list` only (not `resources/list`) and binds `{id}` on read - through
  the `request:` field's default and validators, like `tools/call` (a bad value is `-32602`).
  Declare every `{var}` as a field. A read is capped at `mcp-resource.response.max-rows`
  (else the file's `mcp-tool.response`, else 1000; `_meta.truncated` says so) and honours
  `redact-columns`; an empty result is `"text":"[]"`.
- `allowed-roles` works on `mcp-resource` and `mcp-prompt` as on tools.

## Write tools (create, update, delete)

A POST/PUT/PATCH/DELETE endpoint with an `mcp-tool:` block is a write tool: the
arguments are its body and path fields, and the tool runs the write template
(`flapi-rest`). **An MCP client calls it without asking anyone**: flAPI sends no
read-only/destructive hint, so the agent only knows from the description. Say "Writes
data" in it, enable `mcp.auth` and give every write tool `allowed-roles`.

Runnable CRUD example (REST + MCP, basic auth, `allowed-roles`) and the calls:
[references/write-tools.md](references/write-tools.md).

- Path and body fields become tool arguments with the validators' types
  (`id` integer, `done` boolean). A failed write (duplicate key) is a tool result
  with `"isError": true`, not a JSON-RPC error. Its text is generic ("Tool execution
  failed on the server. The details are in the server log under request_id req-...");
  the database error, SQL and file paths are only in the server log line with that id.
  A bad argument (validator) keeps its specific message.
- `mcp.auth` covers the MCP side only; the REST routes of the same files need
  `auth:` (endpoint or global, `flapi-auth`).
- Several methods on one `url-path` (GET + POST `/tasks`): config-service routes for
  that path need `?method=` (`flapii endpoints reload /tasks --method POST`, likewise `get`, `update`, `delete`);
  without it the answer is 400 `Ambiguous: /tasks is served by GET (...), POST (...)`.
  `flapii endpoints reload --file sqls/tasks-create.yaml` reloads by file instead.

## Calling it with curl

```bash
B=http://localhost:8080/mcp/jsonrpc
curl -s http://localhost:8080/mcp/health          # {"status":"healthy","tools_count":...}
curl -s -i $B -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"curl","version":"1"}}}'
curl -s $B -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
curl -s $B -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"customer_lookup","arguments":{"nation":3,"limit":2}}}'
```

Real `tools/call` result (trimmed):

```json
{"jsonrpc":"2.0","id":3,"result":{
  "content":[{"type":"text","text":"[{\"c_custkey\":1312,\"c_name\":\"Customer#000001312\",\"c_nationkey\":3,\"c_acctbal\":9459.5},...]"}],
  "structuredContent":{"row_count":2,"rows":[{"c_custkey":1312,"c_name":"Customer#000001312","c_nationkey":3,"c_acctbal":9459.5},...]}}}
```

- **Sessions are optional.** `initialize` returns an `Mcp-Session-Id` header; send it back if
  your client wants it. It never authorizes anything, and `tools/list` / `tools/call` work
  without it. `DELETE /mcp/jsonrpc` with the header closes it.
- **A bad argument is a tool error, not a protocol error**: HTTP 200 with
  `"isError":true` and text like `Invalid arguments for tool 'customer_lookup': nation:
  Integer is greater than the maximum allowed value (max 24)`. An unknown tool is JSON-RPC `-32602`
  `Tool not found: x`.
- **Dry-run:** add `"_dryRun": true` to `arguments`. Validators, roles and rate limits still
  run; the SQL does not. The result text is `{"dry_run":true,"tool_name":...,"rendered_sql":...,"bindings":[...],"parameters":{...}}`:
  typed params are `?` in `rendered_sql` with their values in `bindings`, and `parameters`
  are typed (`3`, not `"3"`). `structuredContent.rows` is `[]`.
  The SQL is withheld if it would expose a secret too short to redact.
- Other methods: `resources/list`, `resources/read` (`{"uri":"flapi://segment_summary"}`),
  `prompts/list`, `prompts/get` (`{"name":..,"arguments":{..}}`), `ping`,
  `logging/setLevel`, `completion/complete`.

Async tasks (modern 2026-07-28 era, headers, `tasks/get`, `tasks/cancel`), error codes and a
Python client: [references/jsonrpc.md](references/jsonrpc.md).

## Connecting clients

**Claude Code** (HTTP transport; add `-s project` to write `.mcp.json` for the team):

```bash
claude mcp add --transport http flapi-local http://localhost:8080/mcp/jsonrpc
# with the config tools (server started with --config-service):
claude mcp add --transport http flapi-local http://localhost:8080/mcp/jsonrpc \
  --header "X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN"
# with mcp.auth on, add the MCP credential too:
#   --header "Authorization: Bearer $FLAPI_MCP_TOKEN"   (or "Authorization: Basic <base64 user:pass>")
```

**This plugin** already registers an MCP server named `flapi` at
`${FLAPI_URL:-http://localhost:8080}/mcp/jsonrpc` with
`X-Config-Token: ${FLAPI_CONFIG_SERVICE_TOKEN:-}` (the `flapi_*` config tools) and
`Authorization: Bearer ${FLAPI_MCP_TOKEN:-}` (a JWT for `mcp.auth`; leave it unset when
`mcp.auth` is off, an empty `Bearer ` is then ignored) (`plugins/flapi/.mcp.json`). Export
them before starting Claude Code. The config token must not go in `Authorization`: with
`mcp.auth` on, that header is checked as the MCP credential and every call is a 401
(`-32000 The credentials sent were not accepted.`). `FLAPI_MCP_TOKEN` is a JWT for
`mcp.auth.type: bearer` (`sub`, `iss` = `jwt-issuer`, a `roles` claim matching the tools'
`allowed-roles`) or an OIDC access token; for `type: basic` use `claude mcp add` with a
Basic header instead. A `claude mcp add` (named `flapi-local` above to
avoid a clash) is only needed for another server or without the plugin.

**Claude Desktop** reads stdio servers from `claude_desktop_config.json`. flAPI speaks HTTP
only, so bridge it with `mcp-remote` (needs Node; not tested against flAPI here):

```json
{"mcpServers": {"flapi": {"command": "npx",
  "args": ["-y", "mcp-remote", "http://localhost:8080/mcp/jsonrpc"]}}}
```

**MCP registry / uvx:** `server.json` in the repo publishes `flapi-io` from PyPI with
`runtimeHint: uvx` and transport `streamable-http` at `http://localhost:8080/mcp/jsonrpc`.
`uvx --from flapi-io flapi -c flapi.yaml` starts the server; the client then connects over
HTTP.

**Behind MCP auth** (`mcp.auth.enabled: true`), the client sends the JWT, OIDC token or
Basic credentials as `Authorization` and the config-service token as `X-Config-Token`,
so both the data tools and the `flapi_*` tools work on one connection. See `flapi-auth`.

## Canonical workflow: SQL to agent tool

1. Write or reuse a REST endpoint (`flapi-rest`). No `LIMIT` in the SQL.
2. Add `mcp-tool:` with a `name` and a description that says what comes back and when to
   use it. Give every request field a `description` and a typed validator.
3. Add a `limit` field with `default:` and/or `response.max-rows`.
4. `flapi -c flapi.yaml --validate-config`. Fix any `DESCRIPTION_*` warning.
5. Reload it (`flapii endpoints reload --file <yaml>`, which also loads a new file, or the
   plugin hook does it on save).
6. `tools/list` with curl: check the name, types, ranges and `required`.
7. `tools/call` with `"_dryRun": true`, then for real. Compare with the REST route.
8. Connect the client (`claude mcp add ...` or the plugin's server) and ask the agent to
   use the tool.
9. Lock it down before exposing it: `mcp.auth`, `allowed-roles`, `strict-descriptions`
   (`flapi-auth`), and add an MCP test (`flapi-testing`).
