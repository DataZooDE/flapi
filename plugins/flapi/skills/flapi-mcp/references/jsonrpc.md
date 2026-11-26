# flAPI MCP over JSON-RPC: reference

All calls are `POST /mcp/jsonrpc` with `Content-Type: application/json`, on the HTTP port.
`GET /mcp/jsonrpc` is `405`. `GET /mcp/health` is a plain JSON health document.

## Two protocol eras on one endpoint

| | Legacy (default) | Modern (2026-07-28) |
|---|---|---|
| Selected by | anything else | `params._meta["io.modelcontextprotocol/protocolVersion"]` present |
| Handshake | `initialize` (protocol `2025-11-25`, `2025-06-18`, `2025-03-26`, `2024-11-05`) | `server/discover` (public, no auth) |
| Sessions | `Mcp-Session-Id` minted on first call, optional to send back | none, inbound header ignored |
| `ping`, `logging/setLevel` | yes | removed (`-32601`) |
| Async tasks | never (async tools run synchronously) | yes, if the client declares the tasks extension |
| Extra headers | none | `MCP-Protocol-Version`, `Mcp-Method`, `Mcp-Name` must mirror the body, else `-32020`, HTTP 400 |

A modern request's `_meta` must also carry `io.modelcontextprotocol/clientCapabilities`
(missing: `-32602`, HTTP 400). Every modern result has `resultType` (`complete` or `task`)
and `_meta["io.modelcontextprotocol/serverInfo"]`.

## Async tool as a task (verified)

Tool YAML: `mcp-tool.async: true` (task at once) or `async-after-ms: 5000` (synchronous
for 5 s, then a task).

```bash
B=http://localhost:8080/mcp/jsonrpc
META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{"extensions":{"io.modelcontextprotocol/tasks":{}}}}'
curl -s $B -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/call' -H 'Mcp-Name: top_products' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"top_products","arguments":{},'"$META"'}}'
# {"result":{"resultType":"task","task":{"taskId":"task_c657...","status":"working","ttlMs":3600000,"pollIntervalMs":1000},...}}

curl -s $B -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tasks/get' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tasks/get","params":{"taskId":"task_c657...",'"$META"'}}'
# {"result":{"resultType":"complete","task":{"status":"completed",...},"result":{"structuredContent":{"rows":[...]}}}}
```

- `status` goes `working` -> `completed` (result under `result`), `failed` (`error`) or
  `cancelled`. A tool that fails (SQL error, bad argument, rate limit) ends `failed`:
  `error` is the message with the submitting call's `request_id`, and `result` keeps the
  `isError: true` tool result. `tasks/cancel {taskId}` cancels a queued task; a running SQL statement still
  runs to completion (a write may commit).
- A task id is bound to the principal that created it.
- Tasks are stored in the `flapi_mcp_tasks` table of the main DuckDB database. They survive
  a restart only when `duckdb.db_path` is a file. A task that was running during a crash
  comes back as `failed`.
- Pool and limits: `mcp.tasks.workers` (2), `queue-depth` (32), `default-ttl-ms` (3600000),
  `poll-interval-ms` (1000).

## Errors

| What happened | HTTP | JSON-RPC |
|---|---|---|
| Invalid argument (validator) | 200 | `result.isError: true`, text `Invalid arguments for tool 'x': field: message` |
| SQL failed | 200 | `result.isError: true`, text `Tool execution failed on the server. The details are in the server log under request_id req-...` (the SQL error, file paths and template locations stay in the log line with that id) |
| Unknown tool | 200 | error `-32602` `Tool not found: x` |
| Unknown method | 200 | `-32601` |
| Bad JSON | 400 | `-32700` `Parse error: Invalid JSON` |
| MCP auth on, no credentials | 401 + `WWW-Authenticate` | `-32000` `Authentication required for method: tools/call` |
| MCP auth on, credentials refused (bad/expired JWT, wrong password) | 401 + `WWW-Authenticate` | `-32000` `The credentials sent were not accepted.` |
| Caller lacks the tool's `allowed-roles` | 403 (`insufficient_scope`) | `-32000` `Permission denied: ...` |
| `flapi_*` tool without the config-service token (`X-Config-Token` or `Authorization: Bearer`) | 200 | `-32603` `Tool execution failed: Authentication required for tool: flapi_x` |
| `flapi_*` tool with a wrong token | 200 | `-32603` `Tool execution failed: Authentication validation failed: Invalid authentication token` |
| Per-tool rate limit exceeded | 200 | JSON-RPC error, like the RBAC denial |
| Notification (no `id`) | 202, empty body | none |

## Results

- `tools/call`: `content[0].text` is the JSON array of rows as text, and
  `structuredContent` is `{"rows":[...],"row_count":N}`. After the first call `tools/list`
  also advertises an `outputSchema` learned from the result columns.
- `resources/read`: `{"contents":[{"uri":"flapi://name","mimeType":"application/json","text":"[...]"}]}`.
- `prompts/get`: `{"description":...,"messages":[{"role":"user","content":{"type":"text","text":"..."}}]}`.

## Trace context (SEP-414)

Put a W3C `traceparent` in `params._meta` of `tools/call`. The call's span joins the
caller's trace even with tracing off (it is used for log correlation). See `flapi-operate`.

## Minimal Python client

```python
import json, urllib.request

URL = "http://localhost:8080/mcp/jsonrpc"

def rpc(method, params=None, rid=1, token=None):
    body = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        body["params"] = params
    req = urllib.request.Request(URL, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req) as r:
        return json.load(r)

tools = [t["name"] for t in rpc("tools/list")["result"]["tools"]]
rows = rpc("tools/call", {"name": "customer_lookup",
                          "arguments": {"nation": 3, "limit": 2}})["result"]["structuredContent"]["rows"]
```
