# MCP Configuration Service Integration Guide

Technical guide for integrating the MCP Configuration Service with MCP clients and understanding how MCPRouteHandlers bridges MCP protocol with the ConfigToolAdapter.

## Architecture Overview

The MCP Configuration Service provides runtime management of REST APIs and MCP endpoints through a layered architecture:

```
MCP Client
    ↓
    ↓ JSON-RPC 2.0
    ↓
MCPServer
├─ MCPRouteHandlers (MCP Protocol Handler)
│  ├─ initialize
│  ├─ list_tools (describes 20 config tools)
│  ├─ call_tool (routes to ConfigToolAdapter)
│  ├─ list_resources
│  └─ read_resource
    ↓
ConfigToolAdapter (MCP Tool Implementation)
├─ Tool Registration
├─ Tool Execution Router
├─ Authentication Validation
├─ Parameter Validation
└─ Error Mapping
    ↓
ConfigManager (Config Operations)
├─ Endpoint CRUD
├─ Template Management
├─ Schema Access
└─ Cache Management
    ↓
DatabaseManager (DuckDB Access)
└─ Query Execution
```

---

## Activation and Enablement

### When Config Tools are Available

The ConfigToolAdapter and MCP configuration tools are **only initialized when the `--config-service` flag is enabled** at server startup.

**Server With Config Service Enabled:**
```bash
./flapi --config-service
```
Result: ConfigToolAdapter is instantiated and 18 config tools are available via MCP (`tools/list` includes `flapi_*` tools)

**Server With Config Service Disabled (Default):**
```bash
./flapi  # or ./flapi --config flapi.yaml
```
Result: ConfigToolAdapter is NOT created, config tools return "Tool not found", only endpoint-based MCP tools are available

### Implementation Details

In `src/api_server.cpp`, the initialization is conditional:
```cpp
if (config_service_enabled) {
    config_tool_adapter = std::make_unique<ConfigToolAdapter>(cm, db_manager);
    // Tools available via MCP
} else {
    config_tool_adapter = nullptr;
    // Tools not available, requests will fail with "Tool not found"
}
```

MCPRouteHandlers handles both cases gracefully:
- If `config_tool_adapter_` is null, config tool calls return error: "Tool execution failed: Config tools not available"
- Endpoint-based tools continue working unchanged

---

## Request/Response Flow

### Complete Request Lifecycle

**1. MCP Client Initiates Call**

The MCP client discovers available tools and calls a specific configuration tool:

```json
{
  "jsonrpc": "2.0",
  "id": "req-001",
  "method": "tools/call",
  "params": {
    "name": "flapi_create_endpoint",
    "arguments": {
      "path": "customers",
      "method": "POST",
      "template_source": "customers_create.sql"
    }
  }
}
```

**2. MCPRouteHandlers Receives Request**

The MCP server's route handler receives the JSON-RPC request:

```cpp
// MCPRouteHandlers::handleToolCall
{
  const std::string& tool_name = request.params["name"];
  const crow::json::wvalue& arguments = request.params["arguments"];
  const std::string& auth_token = request.headers["authorization"];

  return config_tool_adapter_->executeTool(tool_name, arguments, auth_token);
}
```

**3. ConfigToolAdapter Validates and Routes**

```
executeTool() Flow:
├─ Check tool exists in registry
├─ Check auth requirement
├─ Validate auth token format (if required)
├─ Validate parameters per tool
├─ Execute tool handler from function table
└─ Return ConfigToolResult
```

**4. Tool Handler Executes**

For example, `executeCreateEndpoint()`:

```cpp
ConfigToolResult ConfigToolAdapter::executeCreateEndpoint(const crow::json::wvalue& args) {
  // 1. Defensive null checks
  if (!config_manager_) return error();

  // 2. Extract and validate parameters
  std::string path = extractStringParam(args, "path", true, error_msg);
  if (!error_msg.empty()) return error(-32602, error_msg);

  // 3. Validate parameter content (path traversal protection)
  error_msg = isValidEndpointPath(path);
  if (!error_msg.empty()) return error(-32602, error_msg);

  // 4. Delegate to ConfigManager
  if (config_manager_->getEndpointForPath(path) != nullptr) {
    return error(-32603, "Endpoint already exists: " + path);
  }

  // 5. Perform operation
  EndpointConfig new_endpoint;
  new_endpoint.urlPath = path;
  new_endpoint.method = method;
  config_manager_->addEndpoint(new_endpoint);

  // 6. Build success response
  crow::json::wvalue result;
  result["status"] = "success";
  result["path"] = path;
  result["message"] = "Endpoint created successfully";

  return createSuccessResult(result.dump());
}
```

**5. Response Serialization**

The ConfigToolResult is converted to JSON-RPC format:

```json
{
  "jsonrpc": "2.0",
  "id": "req-001",
  "result": {
    "status": "success",
    "path": "customers",
    "method": "POST",
    "template_source": "customers_create.sql",
    "message": "Endpoint created successfully"
  }
}
```

---

## Authentication Flow

### Authentication Architecture

```
MCP Request with Auth Token
        ↓
executeTool() checks auth requirement
        ↓
    ├─ Auth NOT required → Execute tool
    │
    └─ Auth REQUIRED:
        ├─ Token missing? → Error -32001
        ├─ validateAuthToken():
        │   ├─ Check token non-empty (8+ chars)
        │   ├─ Check token not too long (4096 max)
        │   ├─ Parse scheme (Bearer, Basic, Token, API-Key)
        │   ├─ Validate scheme recognized
        │   └─ Validate token value format
        │
        └─ Token valid? → Execute tool
           Invalid? → Error -32001
```

### Authentication Token Formats

**Bearer Token (JWT)**
```
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...
```
- Validation: Non-empty, Base64 characters only

**Basic Auth (Credentials)**
```
Authorization: Basic dXNlcm5hbWU6cGFzc3dvcmQ=
```
- Validation: Non-empty, Base64 characters only

**Token Auth (API Key)**
```
Authorization: Token abc123def456
```
- Validation: Non-empty, alphanumeric + hyphens/underscores

**API Key**
```
Authorization: API-Key my-secret-key-12345
```
- Validation: Non-empty, alphanumeric + hyphens/underscores

### Authentication Requirements by Tool

**No Authentication Required:**
- All Phase 1 (Discovery) tools
- All Phase 2 read operations (get_template)
- All Phase 3 read operations (list_endpoints, get_endpoint)
- All Phase 4 read operations (get_cache_status, get_cache_audit)

**Authentication Required:**
- Phase 2 write: flapi_update_template
- Phase 3 write: flapi_create_endpoint, flapi_update_endpoint, flapi_delete_endpoint, flapi_reload_endpoint
- Phase 4 write: flapi_refresh_cache, flapi_run_cache_gc

### Integration with ConfigManager Auth

The ConfigToolAdapter enforces its own token validation independently of ConfigManager. The token is validated for format and structure but not cryptographically verified (that's typically done by an auth middleware at a higher level).

For production deployments, integrate with AuthMiddleware:

```cpp
// In MCPRouteHandlers::before_handle
AuthMiddleware::context auth_context;
auth_middleware.before_handle(request, response, auth_context);

// Use auth_context in MCP handler
if (!auth_context.authenticated) {
  return error(-32001, "Authentication required");
}

// Pass authenticated token to ConfigToolAdapter
config_tool_adapter_->executeTool(tool_name, args, auth_context.auth_token);
```

---

## Error Handling Strategy

### Error Response Structure

All errors include structured information:

```json
{
  "jsonrpc": "2.0",
  "id": "req-001",
  "error": {
    "code": -32603,
    "message": "{\"error\":\"Endpoint not found\",\"path\":\"customers\",\"hint\":\"Use flapi_list_endpoints to see available endpoints\"}"
  }
}
```

Error message contains JSON object with:
- `error`: What failed
- `path`/`method`/`template`: Resource context
- `reason`: Exception details (if applicable)
- `hint`: Resolution guidance

### Error Codes and Handling

| Code | Meaning | Handling |
|------|---------|----------|
| -32601 | Tool/handler not found | Check tool name spelling |
| -32602 | Invalid parameters | Validate required params, types |
| -32603 | Internal server error | Check logs, verify resource state |
| -32001 | Auth required/invalid | Provide valid authentication token |

### Error Handling in Tool Implementation

```cpp
ConfigToolResult executeXXX(const crow::json::wvalue& args) {
  std::string path;  // For error logging
  try {
    // 1. Defensive null checks
    if (!config_manager_) {
      return createErrorResult(-32603, "Configuration service unavailable");
    }

    // 2. Parameter extraction validation
    std::string error_msg = "";
    path = extractStringParam(args, "path", true, error_msg);
    if (!error_msg.empty()) {
      return createErrorResult(-32602, error_msg);
    }

    // 3. Security validation
    error_msg = isValidEndpointPath(path);
    if (!error_msg.empty()) {
      return createErrorResult(-32602, error_msg);
    }

    // 4. Business logic validation
    auto ep = config_manager_->getEndpointForPath(path);
    if (!ep) {
      crow::json::wvalue error_detail;
      error_detail["error"] = "Endpoint not found";
      error_detail["path"] = path;
      error_detail["hint"] = "Use flapi_list_endpoints to see available endpoints";
      return createErrorResult(-32603, error_detail.dump());
    }

    // 5. Operation with exception handling
    try {
      config_manager_->deleteEndpoint(path);
    } catch (const std::exception& e) {
      crow::json::wvalue error_detail;
      error_detail["error"] = "Failed to delete endpoint";
      error_detail["path"] = path;
      error_detail["reason"] = std::string(e.what());
      return createErrorResult(-32603, error_detail.dump());
    }

    // 6. Success response
    return createSuccessResult(result.dump());

  } catch (const std::exception& e) {
    // Top-level catch for unexpected errors
    CROW_LOG_ERROR << "Tool execution failed for path '" << path << "': " << e.what();
    return createErrorResult(-32603, "Unexpected server error: " + std::string(e.what()));
  }
}
```

---

## Response Serialization

### JSON Response Format

All tool responses are JSON objects serialized to strings:

```cpp
// Success: Return JSON string
crow::json::wvalue result;
result["status"] = "success";
result["path"] = path;
result["count"] = items.size();
return createSuccessResult(result.dump());  // Returns JSON string

// Error: Return JSON string in error message
crow::json::wvalue error_detail;
error_detail["error"] = "Failed to create";
error_detail["path"] = path;
error_detail["reason"] = e.what();
return createErrorResult(-32603, error_detail.dump());  // JSON string in message
```

### Crow JSON Serialization

Crow's `json::wvalue` provides efficient JSON building:

```cpp
// Arrays (pre-allocate for efficiency)
std::vector<crow::json::wvalue> items;
items.reserve(endpoints.size());
for (const auto& ep : endpoints) {
  crow::json::wvalue item;
  item["name"] = ep.getName();
  items.emplace_back(std::move(item));
}
auto list = crow::json::wvalue::list();
for (auto& item : items) {
  list.emplace_back(std::move(item));
}
result["endpoints"] = std::move(list);

// Nested objects
crow::json::wvalue response;
response["status"] = "success";
response["data"]["endpoint"]["path"] = path;
response["data"]["changes"]["old_value"] = old;
response["data"]["changes"]["new_value"] = new_val;
```

### Serialization Performance

- Use `emplace_back()` instead of `push_back()` for efficiency
- Pre-allocate vector capacity: `.reserve(size)` before loop
- Use move semantics: `std::move(object)` to avoid copies
- Build lists before assigning to avoid repeated allocations

---

## Request Routing

### Tool Registry and Handler Dispatch

ConfigToolAdapter maintains tool registry:

```cpp
// Tool registry (unordered_map for O(1) lookup)
std::unordered_map<std::string, ConfigToolDef> tools_;
std::unordered_map<std::string, bool> tool_auth_required_;
std::unordered_map<std::string, ToolHandler> tool_handlers_;

// Handler is std::function that executes tool
using ToolHandler = std::function<ConfigToolResult(const crow::json::wvalue&)>;
```

### Tool Registration

During construction, all 20 tools are registered:

```cpp
void registerDiscoveryTools() {
  // Register tool definition
  ConfigToolDef get_project = {
    name: "flapi_get_project_config",
    description: "Get project metadata and configuration",
    input_schema: buildInputSchema({}, {}),
    output_schema: buildOutputSchema()
  };
  tools_["flapi_get_project_config"] = get_project;
  tool_auth_required_["flapi_get_project_config"] = false;

  // Register handler (lambda with this capture)
  tool_handlers_["flapi_get_project_config"] =
    [this](const crow::json::wvalue& args) {
      return this->executeGetProjectConfig(args);
    };
}
```

### Request Routing in executeTool()

```cpp
ConfigToolResult executeTool(const std::string& tool_name,
                            const crow::json::wvalue& arguments,
                            const std::string& auth_token) {
  // 1. Check tool exists
  if (tools_.find(tool_name) == tools_.end()) {
    return error(-32601, "Tool not found: " + tool_name);
  }

  // 2. Check authentication
  if (tool_auth_required_.at(tool_name)) {
    if (auth_token.empty()) {
      return error(-32001, "Authentication required");
    }
    std::string token_error = validateAuthToken(auth_token);
    if (!token_error.empty()) {
      return error(-32001, "Authentication validation failed: " + token_error);
    }
  }

  // 3. Validate parameters
  std::string validation_error = validateArguments(tool_name, arguments);
  if (!validation_error.empty()) {
    return error(-32602, validation_error);
  }

  // 4. Execute tool (O(1) lookup in handler table)
  auto handler_it = tool_handlers_.find(tool_name);
  if (handler_it == tool_handlers_.end()) {
    return error(-32601, "Tool handler not found: " + tool_name);
  }

  return handler_it->second(arguments);  // Execute handler
}
```

---

## Integration Checklist

### For MCP Client Developers

- [ ] Discover available tools via `list_tools`
- [ ] Check tool `input_schema` for required parameters
- [ ] Provide authentication token for write operations
- [ ] Handle structured error responses with hints
- [ ] Parse success response JSON for results
- [ ] Implement retry logic for transient errors (503, timeouts)
- [ ] Cache tool definitions to reduce discovery calls

### For MCP Server Integrators

- [ ] Initialize ConfigToolAdapter with ConfigManager and DatabaseManager
- [ ] Register MCP route handlers for tool discovery and execution
- [ ] Implement authentication middleware before MCP handler
- [ ] Pass authenticated token to ConfigToolAdapter
- [ ] Map HTTP auth headers to tool auth token parameter
- [ ] Log all tool calls for audit trail
- [ ] Monitor tool execution latency

### For Server Administrators

- [ ] Enable authentication for write operations (create, update, delete, reload, refresh)
- [ ] Rotate authentication tokens regularly
- [ ] Review audit logs for suspicious activity
- [ ] Monitor cache hit rates and refresh performance
- [ ] Set appropriate file permissions on template and config directories
- [ ] Backup configuration before bulk endpoint operations

---

## Common Integration Patterns

### Pattern 1: Auto-Discovery and Tool Registration

MCP clients can discover tools and build UIs automatically:

```python
# Python MCP Client
client = MCPClient("http://localhost:8081")
tools = client.list_tools()

for tool in tools:
    if tool.name.startswith("flapi_"):
        print(f"Tool: {tool.name}")
        print(f"Description: {tool.description}")
        print(f"Parameters: {tool.input_schema}")
```

### Pattern 2: Error Handling with User Guidance

Clients can extract hints and present to users:

```javascript
// JavaScript MCP Client
try {
  const result = await client.callTool("flapi_create_endpoint", {
    path: "customers",
    method: "POST"
  });
} catch (error) {
  const details = JSON.parse(error.message);
  console.log(`Error: ${details.error}`);
  console.log(`Hint: ${details.hint}`);  // User-friendly guidance
}
```

### Pattern 3: Defensive Tool Calling

Always verify prerequisites:

```go
// Go MCP Client
// Before updating endpoint, verify it exists
endpoint, err := client.CallTool("flapi_get_endpoint", map[string]interface{}{
  "path": "customers",
})
if err != nil {
  return fmt.Errorf("endpoint not found: %v", err)
}

// Now safe to update
result, err := client.CallTool("flapi_update_endpoint", map[string]interface{}{
  "path": "customers",
  "method": "POST",
})
```

### Pattern 4: Batch Operations

Sequence multiple tool calls for complex workflows:

```bash
#!/bin/bash
# Create endpoint, set template, enable cache, reload

set -e  # Exit on error

# 1. Create endpoint
curl -X POST http://localhost:8081/mcp/tools/call \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"name": "flapi_create_endpoint", "arguments": {"path": "orders", "method": "GET"}}'

# 2. Update template
curl -X POST http://localhost:8081/mcp/tools/call \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"name": "flapi_update_template", "arguments": {"endpoint": "orders", "content": "SELECT * FROM orders"}}'

# 3. Reload endpoint
curl -X POST http://localhost:8081/mcp/tools/call \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"name": "flapi_reload_endpoint", "arguments": {"path": "orders"}}'

echo "Endpoint created and configured"
```

---

## Performance Considerations

### Latency Breakdown

Typical tool execution latency:

| Component | Time |
|-----------|------|
| MCP Protocol parsing | 1-2ms |
| Auth token validation | 1ms |
| Parameter validation | 1ms |
| ConfigManager lookup | 1-5ms |
| Tool execution | 5-50ms |
| JSON serialization | 2-10ms |
| **Total** | **12-68ms** |

### Optimization Tips

1. **Cache tool definitions**: `list_tools` called once, results cached
2. **Batch operations**: Combine multiple tasks in single request when possible
3. **Prefetch config**: Call `flapi_list_endpoints` once, use results to validate paths
4. **Monitor endpoint count**: Performance degrades with very large endpoint counts (100+)

### Scaling Limits

- Max concurrent connections: Limited by server thread pool
- Max endpoints: 1000+ (before noticeable latency)
- Max endpoints per request: No practical limit
- Max template size: Depends on DuckDB memory limit

---

## Troubleshooting

### Common Issues

**Issue: "Authentication required" error on read operation**
- Cause: Read tool incorrectly marked as auth-required
- Solution: Check `tool_auth_required_` map for tool name

**Issue: "Invalid parameters" on every call**
- Cause: Parameters not matching expected format
- Solution: Check `validateArguments()` for required params

**Issue: "Endpoint not found" after creation**
- Cause: ConfigManager not persisted to disk
- Solution: Call `flapi_reload_endpoint` to sync

**Issue: Slow tool execution (>100ms)**
- Cause: Large endpoint count, complex templates, DB queries
- Solution: Profile with logging, optimize templates

**Issue: JSON parsing error in response**
- Cause: Tool returned invalid JSON string
- Solution: Check error logs for exception in tool handler

---

## Security Considerations

### Attack Surface

1. **Parameter injection**: All user input validated via `validateArguments()` and `extractStringParam()`
2. **Path traversal**: Protected by `isValidEndpointPath()` checking for "..", backslashes, URL encoding
3. **Token validation**: Format checking prevents malformed tokens
4. **Exception disclosure**: Error messages include context but not stack traces
5. **Configuration modification**: Auth required for all write operations

### Best Practices

- Use HTTPS/TLS for all MCP communication
- Rotate authentication tokens regularly
- Implement rate limiting at HTTP layer
- Log all authenticated tool calls
- Sanitize error messages in production
- Restrict ConfigToolAdapter access to authenticated users only
- Use principle of least privilege for endpoint operations

---

## Related Documentation

**Reference Docs:**
- **[Reference Documentation Map](./REFERENCE_MAP.md)** - Navigation guide for all reference docs
- **[MCP Configuration Tools API Reference](./MCP_CONFIG_TOOLS_API.md)** - Complete 20-tool reference
- **[MCP Reference](./MCP_REFERENCE.md)** - MCP protocol specification and error codes
- **[Configuration Reference](./CONFIG_REFERENCE.md)** - Configuration file options
- **[Config Service API Reference](./CONFIG_SERVICE_API_REFERENCE.md)** - REST API and CLI client

**Component Docs:**
- **[Configuration System](./spec/components/config-system.md)** - ConfigManager internals
- **[MCP Protocol](./spec/components/mcp-protocol.md)** - MCP server architecture
- **[Security](./spec/components/security.md)** - Auth and validation details
