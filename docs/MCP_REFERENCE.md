# flAPI - MCP Protocol Reference

**Version:** 1.0.0
**Protocol Version:** 2025-11-25
**flAPI Version:** >= 1.0.0

This document provides a comprehensive reference for flAPI's MCP (Model Context Protocol) implementation, covering protocol details, configuration, and usage.

---

## Table of Contents

1. [Overview](#1-overview)
   - [What is MCP](#what-is-mcp)
   - [Architecture](#architecture)
   - [Protocol Versions](#protocol-versions)
2. [Getting Started](#2-getting-started)
   - [Enabling MCP](#enabling-mcp)
   - [First Connection](#first-connection)
   - [Health Check](#health-check)
3. [Protocol Reference](#3-protocol-reference)
   - [JSON-RPC Format](#31-json-rpc-format)
   - [Session Management](#32-session-management)
   - [Error Codes](#33-error-codes)
   - [Protocol Negotiation](#34-protocol-negotiation)
4. [MCP Methods](#4-mcp-methods)
   - [initialize](#41-initialize)
   - [ping](#42-ping)
   - [tools/list](#43-toolslist)
   - [tools/call](#44-toolscall)
   - [resources/list](#45-resourceslist)
   - [resources/read](#46-resourcesread)
   - [prompts/list](#47-promptslist)
   - [prompts/get](#48-promptsget)
   - [logging/setLevel](#49-loggingsetlevel)
   - [completion/complete](#410-completioncomplete)
5. [Tools](#5-tools)
   - [Tool Configuration](#51-tool-configuration)
   - [Input Schema](#52-input-schema)
   - [Execution Flow](#53-execution-flow)
   - [Write Operations](#54-write-operations)
6. [Resources](#6-resources)
   - [Resource Configuration](#61-resource-configuration)
   - [URI Format](#62-uri-format)
   - [Response Format](#63-response-format)
7. [Prompts](#7-prompts)
   - [Prompt Configuration](#71-prompt-configuration)
   - [Template Syntax](#72-template-syntax)
   - [Arguments](#73-arguments)
8. [Authentication](#8-authentication)
   - [Basic Auth](#81-basic-auth)
   - [Bearer/JWT Auth](#82-bearerjwt-auth)
   - [OIDC Auth](#83-oidc-auth)
   - [Per-Method Auth](#84-per-method-auth)
9. [Content Types](#9-content-types)
   - [Text Content](#91-text-content)
   - [Image Content](#92-image-content)
   - [Audio Content](#93-audio-content)
   - [Resource Content](#94-resource-content)
   - [MIME Type Detection](#95-mime-type-detection)
10. [Testing & Examples](#10-testing--examples)
    - [cURL Examples](#101-curl-examples)
    - [Python Client](#102-python-client)
- [Appendix A: Complete Configuration Example](#appendix-a-complete-configuration-example)
- [Appendix B: Error Reference](#appendix-b-error-reference)
- [Related Documentation](#related-documentation)

---

## 1. Overview

### What is MCP

MCP (Model Context Protocol) is a JSON-RPC 2.0 based protocol designed for AI model integration. It provides a standardized way for AI assistants and agents to interact with data sources, execute tools, and access resources.

flAPI implements MCP as a first-class protocol alongside REST, allowing the same YAML configurations to expose both REST endpoints and MCP tools/resources.

**Key Benefits:**

| Benefit | Description |
|---------|-------------|
| **Unified Configuration** | Same YAML defines both REST and MCP interfaces |
| **AI-Optimized** | Structured tool definitions with input schemas for LLM consumption |
| **Session-Based** | Persistent sessions with authentication context |
| **Type-Safe** | JSON Schema validation for tool inputs |

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     MCP Client                               │
│              (Claude, GPT, Custom Agent)                     │
└─────────────────────┬───────────────────────────────────────┘
                      │ JSON-RPC 2.0 / HTTP
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                  flAPI Server                                │
├─────────────────────────────────────────────────────────────┤
│  POST /mcp/jsonrpc   ─────────►  MCPRouteHandlers           │
│                                        │                     │
│  ┌─────────────────────────────────────┴──────────────────┐ │
│  │                 MCP Session Manager                     │ │
│  │   - Session lifecycle                                   │ │
│  │   - Auth context tracking                               │ │
│  │   - Protocol version negotiation                        │ │
│  └─────────────────────────────────────────────────────────┘ │
│                            │                                 │
│  ┌─────────────────────────┼─────────────────────────────┐  │
│  │    Tools     │   Resources   │   Prompts              │  │
│  │    ─────     │   ─────────   │   ───────              │  │
│  │  mcp-tool:   │  mcp-resource:│  mcp-prompt:           │  │
│  │  configs     │  configs      │  configs               │  │
│  └──────────────┴───────────────┴────────────────────────┘  │
│                            │                                 │
│                     ┌──────┴──────┐                         │
│                     │   DuckDB    │                         │
│                     │   Engine    │                         │
│                     └─────────────┘                         │
└─────────────────────────────────────────────────────────────┘
```

### Protocol Versions

flAPI supports multiple MCP protocol versions for backward compatibility:

| Version | Release | Status | Notes |
|---------|---------|--------|-------|
| `2025-11-25` | Latest | **Default** | Full feature support |
| `2025-06-18` | Q2 2025 | Supported | - |
| `2025-03-26` | Q1 2025 | Supported | - |
| `2024-11-05` | Q4 2024 | Supported | Initial MCP specification |

The server negotiates the highest mutually supported version during `initialize`.

> **Implementation:** `src/mcp_server.cpp`, `src/mcp_route_handlers.cpp` | **Tests:** `test/cpp/mcp_server_test.cpp`, `test/integration/test_mcp_methods.py`

---

## 2. Getting Started

### Enabling MCP

MCP is enabled in `flapi.yaml` under the `mcp` section:

```yaml
# flapi.yaml
mcp:
  enabled: true
  port: 8080                              # Same port as REST (shared)
  host: localhost
  allow-list-changed-notifications: true

  # Instructions for LLM clients (optional)
  instructions-file: ./mcp_instructions.md
  # Or inline:
  # instructions: |
  #   # flapi MCP Server
  #   Use tools for customer lookups.
```

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `enabled` | boolean | `true` | Enable/disable MCP server |
| `port` | integer | `8080` | HTTP port (shared with REST) |
| `host` | string | `localhost` | Bind address |
| `allow-list-changed-notifications` | boolean | `true` | Enable listChanged capability |
| `instructions-file` | string | - | Path to instructions markdown file |
| `instructions` | string | - | Inline instructions for LLM clients |

### First Connection

**MCP Endpoint:** `POST /mcp/jsonrpc`

To establish a session, send an `initialize` request:

```bash
curl -X POST http://localhost:8080/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "protocolVersion": "2025-11-25",
      "clientInfo": {
        "name": "my-client",
        "version": "1.0.0"
      }
    }
  }'
```

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2025-11-25",
    "capabilities": {
      "tools": { "listChanged": true },
      "resources": { "subscribe": false, "listChanged": true },
      "prompts": { "listChanged": true },
      "logging": {}
    },
    "serverInfo": {
      "name": "flapi-mcp-server",
      "version": "0.3.0"
    },
    "instructions": "# flapi MCP Server\n..."
  }
}
```

The response includes an `Mcp-Session-Id` header that must be included in subsequent requests.

### Health Check

Check MCP server status without JSON-RPC:

```bash
curl http://localhost:8080/mcp/health
```

**Response:**

```json
{
  "status": "healthy",
  "server": "flapi-mcp-server",
  "version": "0.3.0",
  "protocol_version": "2025-11-25",
  "mcp_available": true,
  "tools_available": true,
  "resources_available": true,
  "tools_count": 5,
  "resources_count": 2
}
```

---

## 3. Protocol Reference

### 3.1 JSON-RPC Format

All MCP requests use JSON-RPC 2.0 over HTTP POST.

**Request Format:**

```json
{
  "jsonrpc": "2.0",
  "id": "<string|number>",
  "method": "<method_name>",
  "params": { ... }
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `jsonrpc` | string | Yes | Must be `"2.0"` |
| `id` | string/number | Yes | Request identifier (returned in response) |
| `method` | string | Yes | MCP method name |
| `params` | object | No | Method-specific parameters |

**Response Format (Success):**

```json
{
  "jsonrpc": "2.0",
  "id": "<matching_id>",
  "result": { ... }
}
```

**Response Format (Error):**

```json
{
  "jsonrpc": "2.0",
  "id": "<matching_id>",
  "error": {
    "code": -32600,
    "message": "Invalid Request"
  }
}
```

### 3.2 Session Management

MCP uses HTTP headers for session tracking:

| Header | Direction | Description |
|--------|-----------|-------------|
| `Mcp-Session-Id` | Response → Request | Session identifier |
| `Content-Type` | Both | Must be `application/json` |
| `Authorization` | Request | Authentication credentials (if required) |

**Session Lifecycle:**

1. **Creation**: New session created on first `initialize` request
2. **Maintenance**: Session ID returned in `Mcp-Session-Id` header
3. **Usage**: Include session ID header in all subsequent requests
4. **Cleanup**: Send `DELETE /mcp/jsonrpc` with session ID to close
5. **Timeout**: Sessions expire after 30 minutes of inactivity (configurable)

**Close Session:**

```bash
curl -X DELETE http://localhost:8080/mcp/jsonrpc \
  -H "Mcp-Session-Id: <session_id>"
```

> **Implementation:** `src/mcp_session_manager.cpp` | **Tests:** `test/integration/test_mcp_sessions.py`

### 3.3 Error Codes

flAPI uses standard JSON-RPC error codes plus MCP-specific codes:

| Code | Name | Description |
|------|------|-------------|
| `-32700` | Parse Error | Invalid JSON |
| `-32600` | Invalid Request | Not a valid JSON-RPC request |
| `-32601` | Method Not Found | Unknown method name |
| `-32602` | Invalid Params | Invalid method parameters |
| `-32603` | Internal Error | Internal server error |
| `-32001` | Authentication Required | Method requires authentication |
| `-32000` | Session Error | Session-related errors |

### 3.4 Protocol Negotiation

During `initialize`, client and server negotiate the protocol version:

```
Client sends:    "protocolVersion": "2025-11-25"
Server supports: ["2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"]
Negotiated:      "2025-11-25" (highest mutually supported)
```

If the client requests an unknown version, the server uses its latest supported version and logs a warning.

---

## 4. MCP Methods

### 4.1 initialize

Establishes a new MCP session and negotiates capabilities.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2025-11-25",
    "clientInfo": {
      "name": "my-client",
      "version": "1.0.0"
    },
    "capabilities": {
      "sampling": {},
      "roots": {}
    }
  }
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `protocolVersion` | string | Yes | Desired protocol version |
| `clientInfo.name` | string | No | Client name |
| `clientInfo.version` | string | No | Client version |
| `capabilities` | object | No | Client capabilities |

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2025-11-25",
    "capabilities": {
      "tools": { "listChanged": true },
      "resources": { "subscribe": false, "listChanged": true },
      "prompts": { "listChanged": true },
      "logging": {}
    },
    "serverInfo": {
      "name": "flapi-mcp-server",
      "version": "0.3.0"
    },
    "instructions": "..."
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `protocolVersion` | string | Negotiated protocol version |
| `capabilities.tools` | object | Tool capabilities |
| `capabilities.resources` | object | Resource capabilities |
| `capabilities.prompts` | object | Prompt capabilities |
| `capabilities.logging` | object | Logging capabilities |
| `serverInfo` | object | Server identification |
| `instructions` | string | Usage instructions for LLM clients |

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** `test/cpp/mcp_server_test.cpp`, `test/integration/test_mcp_methods.py`

---

### 4.2 ping

Health check for active sessions.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "ping",
  "params": {}
}
```

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {}
}
```

Returns an empty object on success.

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** *None - see [TEST_TODO.md](./TEST_TODO.md)*

---

### 4.3 tools/list

Lists all available MCP tools.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/list",
  "params": {}
}
```

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "tools": [
      {
        "name": "customer_lookup",
        "description": "Retrieve customer information by ID",
        "inputSchema": {
          "type": "object",
          "properties": {
            "id": {
              "type": "string",
              "description": "Customer ID"
            }
          },
          "required": ["id"]
        }
      }
    ]
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `tools[].name` | string | Unique tool identifier |
| `tools[].description` | string | Human-readable description |
| `tools[].inputSchema` | object | JSON Schema for tool inputs |

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** `test/integration/test_mcp_methods.py`, `test/integration/test_mcp_integration.py`

---

### 4.4 tools/call

Executes an MCP tool with provided arguments.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "tools/call",
  "params": {
    "name": "customer_lookup",
    "arguments": {
      "id": "12345"
    }
  }
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | Yes | Tool name to execute |
| `arguments` | object | No | Tool input arguments |

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "[{\"id\":12345,\"name\":\"John Doe\",\"email\":\"john@example.com\"}]"
      }
    ]
  }
}
```

Tool results are returned as content blocks (see [Content Types](#9-content-types)).

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** `test/cpp/mcp_tool_handler_test.cpp`, `test/integration/test_mcp_methods.py`

---

### 4.5 resources/list

Lists all available MCP resources.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "resources/list",
  "params": {}
}
```

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "result": {
    "resources": [
      {
        "name": "customer_schema",
        "description": "Customer database schema definition",
        "mimeType": "application/json",
        "uri": "flapi://customer_schema"
      }
    ]
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `resources[].name` | string | Resource identifier |
| `resources[].description` | string | Human-readable description |
| `resources[].mimeType` | string | Content MIME type |
| `resources[].uri` | string | Resource URI |

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** `test/integration/test_mcp_integration.py`

---

### 4.6 resources/read

Reads the content of an MCP resource.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "method": "resources/read",
  "params": {
    "uri": "flapi://customer_schema"
  }
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `uri` | string | Yes | Resource URI to read |

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "result": {
    "contents": [
      {
        "uri": "flapi://customer_schema",
        "mimeType": "application/json",
        "text": "{\"fields\":[{\"name\":\"id\",\"type\":\"INTEGER\"}]}"
      }
    ]
  }
}
```

> **Implementation:** `src/mcp_route_handlers.cpp`, `src/mcp_content_types.cpp` | **Tests:** `test/integration/test_mcp_integration.py`

---

### 4.7 prompts/list

Lists all available MCP prompts.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "method": "prompts/list",
  "params": {}
}
```

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "result": {
    "prompts": [
      {
        "name": "analyze_customer",
        "description": "Generate customer analysis prompt",
        "arguments": [
          {
            "name": "customer_id",
            "type": "string",
            "description": "Parameter customer_id"
          }
        ]
      }
    ]
  }
}
```

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** *None - see [TEST_TODO.md](./TEST_TODO.md)*

---

### 4.8 prompts/get

Retrieves a prompt with template arguments substituted.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "method": "prompts/get",
  "params": {
    "name": "analyze_customer",
    "arguments": {
      "customer_id": "12345"
    }
  }
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | Yes | Prompt name |
| `arguments` | object | No | Template arguments |

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "result": {
    "description": "Generate customer analysis prompt",
    "messages": [
      {
        "role": "user",
        "content": {
          "type": "text",
          "text": "Analyze customer 12345 and provide insights."
        }
      }
    ]
  }
}
```

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** *None - see [TEST_TODO.md](./TEST_TODO.md)*

---

### 4.9 logging/setLevel

Sets the server log level for the current session.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "method": "logging/setLevel",
  "params": {
    "level": "debug"
  }
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `level` | string | Yes | Log level to set |

**Valid Log Levels:**

| MCP Level | Maps To | Description |
|-----------|---------|-------------|
| `debug` | DEBUG | Detailed debugging information |
| `info` | INFO | General operational information |
| `notice` | INFO | Normal but significant events |
| `warning` | WARNING | Warning conditions |
| `error` | ERROR | Error conditions |
| `critical` | ERROR | Critical conditions |
| `alert` | ERROR | Action must be taken immediately |
| `emergency` | ERROR | System is unusable |

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "result": {}
}
```

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** `test/integration/test_mcp_methods.py`

---

### 4.10 completion/complete

Provides argument completion suggestions for tools and prompts.

**Request:**

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "completion/complete",
  "params": {
    "ref": "customer_lookup",
    "argument": "status",
    "value": "act"
  }
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `ref` | string | Yes | Tool or prompt name |
| `argument` | string | Yes | Argument name to complete |
| `value` | string | No | Partial value prefix for filtering |

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {
    "values": ["active", "inactive"],
    "total": 2,
    "hasMore": false
  }
}
```

Completions are generated from `enum` validators defined on the argument.

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** `test/integration/test_mcp_methods.py`

---

## 5. Tools

### 5.1 Tool Configuration

MCP tools are defined in endpoint YAML files using the `mcp-tool` section:

```yaml
# sqls/customer-lookup.yaml

mcp-tool:
  name: customer_lookup
  description: Retrieve customer information by ID, segment, or other criteria
  result-mime-type: application/json

request:
  - field-name: id
    field-in: query
    description: Customer ID
    required: false
    validators:
      - type: int
        min: 1
        preventSqlInjection: true

  - field-name: status
    field-in: query
    description: Customer status filter
    required: false
    validators:
      - type: enum
        values: ["active", "inactive", "pending"]

template-source: customer-lookup.sql
connection:
  - customer-database
```

**MCP Tool Properties:**

| Property | Type | Required | Description |
|----------|------|----------|-------------|
| `name` | string | Yes | Unique tool identifier |
| `description` | string | Yes | Human-readable description for LLM |
| `result-mime-type` | string | No | Expected result MIME type (default: `application/json`) |

### 5.2 Input Schema

The `inputSchema` for tools is automatically generated from `request` fields:

| Request Field | JSON Schema Property |
|--------------|---------------------|
| `field-name` | Property name |
| `description` | Property `description` |
| `required: true` | Added to `required` array |
| `validators[].type` | Informs `type` (string by default) |

**Example Conversion:**

```yaml
# YAML Request Definition
request:
  - field-name: customer_id
    description: Customer identifier
    required: true
    validators:
      - type: int
        min: 1
```

```json
// Generated Input Schema
{
  "type": "object",
  "properties": {
    "customer_id": {
      "type": "string",
      "description": "Customer identifier"
    }
  },
  "required": ["customer_id"]
}
```

### 5.3 Execution Flow

When a tool is called:

```
tools/call Request
      │
      ▼
┌─────────────────┐
│ Extract name &  │
│ arguments       │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Find endpoint   │
│ configuration   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Validate inputs │
│ against schema  │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Render SQL      │
│ template        │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Execute on      │
│ DuckDB          │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Format as       │
│ ContentResponse │
└────────┴────────┘
```

### 5.4 Write Operations

MCP tools support write operations (INSERT, UPDATE, DELETE) using the same configuration as REST endpoints:

```yaml
mcp-tool:
  name: create_customer
  description: Create a new customer record

request:
  - field-name: name
    field-in: body
    required: true
    validators:
      - type: string
        min-length: 1
        max-length: 100

  - field-name: email
    field-in: body
    required: true
    validators:
      - type: email

template-source: create-customer.sql
connection:
  - customer-database
```

```sql
-- create-customer.sql
INSERT INTO customers (name, email)
VALUES ('{{{ params.name }}}', '{{{ params.email }}}')
RETURNING id, name, email
```

> **Implementation:** `src/mcp_route_handlers.cpp`, `src/endpoint_config_parser.cpp` | **Tests:** `test/cpp/mcp_tool_handler_test.cpp`, `test/integration/test_mcp_methods.py`

---

## 6. Resources

### 6.1 Resource Configuration

MCP resources provide static or semi-static data to AI clients:

```yaml
# sqls/customer-schema.yaml

mcp-resource:
  name: customer_schema
  description: Customer database schema definition and field descriptions
  mime-type: application/json

template-source: customer-schema.sql
connection:
  - customer-database

# Optional rate limiting
rate-limit:
  enabled: true
  max: 10
  interval: 60
```

**MCP Resource Properties:**

| Property | Type | Required | Description |
|----------|------|----------|-------------|
| `name` | string | Yes | Unique resource identifier |
| `description` | string | Yes | Human-readable description |
| `mime-type` | string | Yes | Content MIME type |

### 6.2 URI Format

Resources are identified by URIs following this scheme:

```
flapi://<resource_name>
```

**Examples:**

| Resource Name | URI |
|--------------|-----|
| `customer_schema` | `flapi://customer_schema` |
| `product_catalog` | `flapi://product_catalog` |
| `system_config` | `flapi://system_config` |

### 6.3 Response Format

Resource content is returned in the `contents` array:

```json
{
  "contents": [
    {
      "uri": "flapi://customer_schema",
      "mimeType": "application/json",
      "text": "{\"fields\":[...]}"
    }
  ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `uri` | string | Resource URI |
| `mimeType` | string | Content MIME type |
| `text` | string | Resource content (text-based) |
| `blob` | string | Base64-encoded binary content |

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** `test/integration/test_mcp_integration.py`

---

## 7. Prompts

### 7.1 Prompt Configuration

MCP prompts provide pre-defined prompt templates for AI interactions:

```yaml
# sqls/analyze-customer.yaml

mcp-prompt:
  name: analyze_customer
  description: Generate customer analysis prompt
  template: |
    Analyze customer {{customer_id}} and provide insights on:
    - Purchase history
    - Engagement patterns
    - Churn risk assessment

    Format the response as a structured report.
  arguments:
    - customer_id
```

**MCP Prompt Properties:**

| Property | Type | Required | Description |
|----------|------|----------|-------------|
| `name` | string | Yes | Unique prompt identifier |
| `description` | string | Yes | Human-readable description |
| `template` | string | Yes | Prompt template with placeholders |
| `arguments` | array | No | List of argument names |

### 7.2 Template Syntax

Prompts use Mustache-style placeholders:

```
{{argument_name}}
```

**Example Template:**

```yaml
template: |
  Generate a sales report for {{region}} covering {{time_period}}.

  Include:
  - Total revenue
  - Top 5 products
  - Customer acquisition trends

  {{#include_projections}}
  Also include Q4 projections based on current trends.
  {{/include_projections}}
```

### 7.3 Arguments

Arguments are passed in the `prompts/get` request and substituted into the template:

```json
{
  "method": "prompts/get",
  "params": {
    "name": "sales_report",
    "arguments": {
      "region": "North America",
      "time_period": "Q3 2024",
      "include_projections": true
    }
  }
}
```

> **Implementation:** `src/mcp_route_handlers.cpp` | **Tests:** *None - see [TEST_TODO.md](./TEST_TODO.md)*

---

## 8. Authentication

### 8.1 Basic Auth

Configure Basic authentication for MCP:

```yaml
# flapi.yaml
mcp:
  enabled: true
  auth:
    enabled: true
    type: basic
    users:
      - username: admin
        password: "$apr1$xyz..."  # MD5 hashed
        roles: [admin, read, write]
      - username: reader
        password: plaintext123    # Plain text (not recommended)
        roles: [read]
```

**Request Header:**

```
Authorization: Basic YWRtaW46cGFzc3dvcmQ=
```

### 8.2 Bearer/JWT Auth

Configure JWT authentication:

```yaml
# flapi.yaml
mcp:
  enabled: true
  auth:
    enabled: true
    type: bearer
    jwt-secret: "${JWT_SECRET}"
    jwt-issuer: "my-auth-server"
```

**Request Header:**

```
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...
```

**Expected JWT Claims:**

| Claim | Description |
|-------|-------------|
| `sub` | Username/subject |
| `roles` | Array of role strings |
| `iss` | Issuer (must match `jwt-issuer`) |
| `exp` | Expiration timestamp |

### 8.3 OIDC Auth

Configure OpenID Connect authentication:

```yaml
# flapi.yaml
mcp:
  enabled: true
  auth:
    enabled: true
    type: oidc
    oidc:
      provider-type: auth0  # or: okta, google, azure, generic
      issuer-url: "https://your-tenant.auth0.com/"
      client-id: "your-client-id"
      client-secret: "${OIDC_CLIENT_SECRET}"
      allowed-audiences:
        - "your-api-audience"
      username-claim: email
      roles-claim: "https://your-app/roles"
```

**Provider Presets:**

| Provider | `provider-type` | Auto-configured |
|----------|----------------|-----------------|
| Auth0 | `auth0` | JWKS URL, claims mapping |
| Okta | `okta` | JWKS URL, claims mapping |
| Google | `google` | JWKS URL, claims mapping |
| Azure AD | `azure` | JWKS URL, claims mapping |
| Generic | `generic` | Manual configuration |

### 8.4 Per-Method Auth

Override authentication requirements per method:

```yaml
# flapi.yaml
mcp:
  enabled: true
  auth:
    enabled: true
    type: bearer
    jwt-secret: "${JWT_SECRET}"
    methods:
      initialize:
        required: false    # Allow unauthenticated initialize
      tools/list:
        required: false    # Allow listing tools without auth
      tools/call:
        required: true     # Require auth for tool execution
      resources/read:
        required: true     # Require auth for resource access
```

> **Implementation:** `src/mcp_auth_handler.cpp`, `src/oidc_auth_handler.cpp` | **Tests:** `test/integration/test_oidc_authentication.py`

---

## 9. Content Types

### 9.1 Text Content

Default content type for tool results:

```json
{
  "type": "text",
  "text": "Result data here",
  "mimeType": "application/json"
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | Yes | Always `"text"` |
| `text` | string | Yes | Text content |
| `mimeType` | string | No | MIME type hint |

### 9.2 Image Content

For binary image data (base64 encoded):

```json
{
  "type": "image",
  "data": "iVBORw0KGgoAAAANSUhEUgAA...",
  "mimeType": "image/png"
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | Yes | Always `"image"` |
| `data` | string | Yes | Base64-encoded image data |
| `mimeType` | string | Yes | Image MIME type |

### 9.3 Audio Content

For binary audio data:

```json
{
  "type": "audio",
  "data": "UklGRiQAAABXQVZFZm10...",
  "mimeType": "audio/wav"
}
```

### 9.4 Resource Content

References to MCP resources:

```json
{
  "type": "resource",
  "resource": {
    "uri": "flapi://customer_schema",
    "mimeType": "application/json",
    "text": "{...}"
  }
}
```

### 9.5 MIME Type Detection

flAPI automatically detects MIME types from file extensions:

| Extension | MIME Type |
|-----------|-----------|
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.svg` | `image/svg+xml` |
| `.wav` | `audio/wav` |
| `.mp3` | `audio/mpeg` |
| `.json` | `application/json` |
| `.csv` | `text/csv` |
| `.txt` | `text/plain` |
| `.pdf` | `application/pdf` |
| `.xml` | `application/xml` |
| (unknown) | `application/octet-stream` |

> **Implementation:** `src/mcp_content_types.cpp` | **Tests:** *Image/audio/binary not tested - see [TEST_TODO.md](./TEST_TODO.md)*

---

## 10. Testing & Examples

### 10.1 cURL Examples

**Initialize Session:**

```bash
# Initialize and capture session ID
SESSION_ID=$(curl -s -D - -X POST http://localhost:8080/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {"protocolVersion": "2025-11-25"}
  }' | grep -i "Mcp-Session-Id" | cut -d: -f2 | tr -d ' \r')

echo "Session: $SESSION_ID"
```

**List Tools:**

```bash
curl -X POST http://localhost:8080/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/list",
    "params": {}
  }'
```

**Call Tool:**

```bash
curl -X POST http://localhost:8080/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 3,
    "method": "tools/call",
    "params": {
      "name": "customer_lookup",
      "arguments": {"id": "12345"}
    }
  }'
```

**With Authentication:**

```bash
# Basic Auth
curl -X POST http://localhost:8080/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -H "Authorization: Basic $(echo -n 'admin:password' | base64)" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {"protocolVersion": "2025-11-25"}
  }'

# Bearer Token
curl -X POST http://localhost:8080/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $JWT_TOKEN" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/call",
    "params": {"name": "customer_lookup", "arguments": {"id": "123"}}
  }'
```

### 10.2 Python Client

**Simple Client:**

```python
import requests
import json

class FlapiMCPClient:
    def __init__(self, base_url: str = "http://localhost:8080"):
        self.base_url = base_url
        self.session_id = None
        self.request_id = 0

    def _call(self, method: str, params: dict = None) -> dict:
        self.request_id += 1
        headers = {"Content-Type": "application/json"}
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id

        response = requests.post(
            f"{self.base_url}/mcp/jsonrpc",
            headers=headers,
            json={
                "jsonrpc": "2.0",
                "id": self.request_id,
                "method": method,
                "params": params or {}
            }
        )

        # Capture session ID from response
        if "Mcp-Session-Id" in response.headers:
            self.session_id = response.headers["Mcp-Session-Id"]

        result = response.json()
        if "error" in result:
            raise Exception(f"MCP Error: {result['error']}")
        return result.get("result", {})

    def initialize(self, protocol_version: str = "2025-11-25") -> dict:
        return self._call("initialize", {"protocolVersion": protocol_version})

    def list_tools(self) -> list:
        result = self._call("tools/list")
        return result.get("tools", [])

    def call_tool(self, name: str, arguments: dict = None) -> dict:
        return self._call("tools/call", {"name": name, "arguments": arguments or {}})

    def list_resources(self) -> list:
        result = self._call("resources/list")
        return result.get("resources", [])

    def read_resource(self, uri: str) -> dict:
        return self._call("resources/read", {"uri": uri})

    def ping(self) -> dict:
        return self._call("ping")

# Usage
client = FlapiMCPClient()
client.initialize()

# List available tools
tools = client.list_tools()
print(f"Available tools: {[t['name'] for t in tools]}")

# Call a tool
result = client.call_tool("customer_lookup", {"id": "12345"})
print(f"Result: {result}")
```

**With Authentication:**

```python
class AuthenticatedMCPClient(FlapiMCPClient):
    def __init__(self, base_url: str, username: str = None,
                 password: str = None, token: str = None):
        super().__init__(base_url)
        self.auth_header = None

        if username and password:
            import base64
            credentials = base64.b64encode(f"{username}:{password}".encode()).decode()
            self.auth_header = f"Basic {credentials}"
        elif token:
            self.auth_header = f"Bearer {token}"

    def _call(self, method: str, params: dict = None) -> dict:
        self.request_id += 1
        headers = {"Content-Type": "application/json"}
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        if self.auth_header:
            headers["Authorization"] = self.auth_header

        # ... rest of implementation
```

---

## Appendix A: Complete Configuration Example

```yaml
# flapi.yaml - Complete MCP Configuration

project-name: customer-api
project-description: Customer data API with MCP support

# Template configuration
template:
  path: ./sqls
  environment-whitelist:
    - '^DB_.*'
    - '^JWT_SECRET$'

# Database connections
connections:
  customer-db:
    properties:
      path: ./data/customers.parquet

# DuckDB configuration
duckdb:
  access_mode: READ_WRITE
  threads: 4
  max_memory: 2GB

# MCP configuration
mcp:
  enabled: true
  port: 8080
  host: 0.0.0.0
  allow-list-changed-notifications: true

  # Instructions for LLM clients
  instructions: |
    # Customer API MCP Server

    This server provides access to customer data.

    ## Available Tools
    - customer_lookup: Search for customers by ID or criteria
    - create_customer: Create new customer records

    ## Usage Guidelines
    - Always validate customer IDs before lookup
    - Use pagination for large result sets

  # Authentication
  auth:
    enabled: true
    type: bearer
    jwt-secret: "${JWT_SECRET}"
    jwt-issuer: "customer-api"

    # Per-method overrides
    methods:
      initialize:
        required: false
      tools/list:
        required: false
      tools/call:
        required: true
      resources/read:
        required: true
```

```yaml
# sqls/customer-lookup.yaml - MCP Tool Definition

mcp-tool:
  name: customer_lookup
  description: |
    Retrieve customer information by ID or search criteria.
    Returns customer details including name, email, and status.
  result-mime-type: application/json

request:
  - field-name: id
    field-in: query
    description: Customer ID (exact match)
    required: false
    validators:
      - type: int
        min: 1
        preventSqlInjection: true

  - field-name: name
    field-in: query
    description: Customer name (partial match)
    required: false
    validators:
      - type: string
        max-length: 100

  - field-name: status
    field-in: query
    description: Customer status filter
    required: false
    validators:
      - type: enum
        values: ["active", "inactive", "pending"]

  - field-name: limit
    field-in: query
    description: Maximum results to return
    required: false
    validators:
      - type: int
        min: 1
        max: 100

template-source: customer-lookup.sql
connection:
  - customer-db

auth:
  enabled: true
  type: basic
  users:
    - username: api
      password: secret
      roles: [read]
```

```sql
-- sqls/customer-lookup.sql

SELECT
    id,
    name,
    email,
    status,
    created_at
FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND id = {{ params.id }}
{{/params.id}}
{{#params.name}}
  AND name ILIKE '%' || '{{{ params.name }}}' || '%'
{{/params.name}}
{{#params.status}}
  AND status = '{{{ params.status }}}'
{{/params.status}}
ORDER BY created_at DESC
LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}25{{/params.limit}}
```

---

## Appendix B: Error Reference

| Error Code | Message | Cause | Resolution |
|------------|---------|-------|------------|
| `-32700` | Parse error | Invalid JSON in request body | Check JSON syntax |
| `-32600` | Invalid Request | Missing required JSON-RPC fields | Include `jsonrpc`, `id`, `method` |
| `-32601` | Method not found | Unknown MCP method | Check method name spelling |
| `-32602` | Invalid params | Missing or invalid parameters | Check required params for method |
| `-32603` | Internal error | Server-side error | Check server logs |
| `-32001` | Authentication required | Method requires auth | Provide Authorization header |
| `-32000` | Session error | Session-related issue | Re-initialize session |

---

## Related Documentation

- [Configuration Reference](./CONFIG_REFERENCE.md) - Comprehensive configuration file options
- [CLI Reference](./CLI_REFERENCE.md) - Server executable CLI options
- [Config Service API Reference](./CONFIG_SERVICE_API_REFERENCE.md) - Runtime configuration REST API
