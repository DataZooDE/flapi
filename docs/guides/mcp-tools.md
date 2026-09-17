# Exposing an endpoint as an MCP tool

**Goal:** let an AI agent call the same SQL you already expose over REST.

You do not write a second implementation. Add an `mcp-tool:` block to an existing
endpoint and it becomes both.

---

## Make an endpoint a tool

```yaml
# sqls/customers.yaml
url-path: /customers              # REST endpoint
mcp-tool:                         # ...and an MCP tool
  name: get_customers
  description: Retrieve customer information by ID
  result-mime-type: application/json

request:
  - field-name: id
    field-in: query
    description: Customer ID          # the agent reads this
    required: false
    validators:
      - type: int
        min: 1
        max: 1000000

template-source: customers.sql
connection: [customers-parquet]
```

The tool's input schema is derived from `request:` — field names, types and the
`description` of each. **Write those descriptions for the agent**, not for
yourself; they are the only thing it has to decide what to pass.

MCP runs alongside REST with no separate configuration.

## Call it

```bash
curl -s http://localhost:8081/mcp/health

curl -s http://localhost:8081/mcp -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 1, "method": "tools/list"
}'

curl -s http://localhost:8081/mcp -H 'Content-Type: application/json' -d '{
  "jsonrpc": "2.0", "id": 2,
  "method": "tools/call",
  "params": { "name": "get_customers", "arguments": { "id": 123 } }
}'
```

## Resources and prompts

A config file can define an MCP **resource** instead of a tool — data the agent
reads rather than an action it takes:

```yaml
mcp-resource:
  name: customer_schema
  description: Customer database schema definition
  mime-type: application/json

template-source: customer-schema.sql
connection: [customers-parquet]
```

Resource templates can be parameterised (`flapi://customers/{id}`). See
[MCP_REFERENCE](../MCP_REFERENCE.md) for resources, prompts, pagination,
sessions and the full method list.

## Shaping what the agent sees

An agent's context is expensive, and a wide result set wastes it.

```yaml
mcp-tool:
  name: get_customers
  response:
    redact-columns: [email, tax_id]     # removed before the agent sees them
```

## Limiting who can call it

Tools honour the same `auth:` and `rate-limit:` blocks as REST endpoints, plus
per-tool roles. See [Authentication](./authentication.md) and
[MCP_REFERENCE § Auth](../MCP_REFERENCE.md).

## Managing configuration through MCP

flAPI also exposes its own configuration as MCP tools, so an agent can list,
create and validate endpoints. That is a separate surface:
[MCP_CONFIG_TOOLS_API](../MCP_CONFIG_TOOLS_API.md).

## Tracing an agent's calls

flAPI honours [SEP-414](https://modelcontextprotocol.io/seps/414-request-meta):
a `traceparent` in `params._meta` joins the tool call to the agent's trace, so
one trace spans the agent and the database query. It works even with tracing
disabled, for log correlation. See [Observability](../OBSERVABILITY.md).

## How it works

- [spec/components/mcp-protocol.md](../spec/components/mcp-protocol.md) — dispatch, sessions, SEP-414, the single-span contract
- [spec/DESIGN_DECISIONS.md § 3](../spec/DESIGN_DECISIONS.md) — why one config serves both protocols
- [MCP_CONFIG_INTEGRATION.md](../MCP_CONFIG_INTEGRATION.md) — how the config tools are wired
