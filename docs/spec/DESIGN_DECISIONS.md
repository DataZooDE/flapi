# Design Decisions

This document explains the rationale behind key architectural and design decisions in flAPI.

## 1. Singleton Pattern for DatabaseManager

**Decision:** DatabaseManager uses the singleton pattern with `getInstance()`.

**Rationale:**
- DuckDB is an in-process database engine with shared state
- Multiple connections to the same database file require coordination
- Thread-safe access to the connection pool is critical
- Extensions are loaded once and shared across all queries

**Implementation:**
```cpp
// src/include/database_manager.hpp:40
static std::shared_ptr<DatabaseManager> getInstance();
```

**Tradeoffs:**
- (+) Ensures single DuckDB instance per process
- (+) Simplifies connection pooling and extension management
- (+) Thread-safe by design with mutex protection
- (-) Harder to test in isolation (requires test fixtures)
- (-) Global state can complicate debugging

**Source files:** `src/database_manager.hpp`, `src/database_manager.cpp`

---

## 2. Facade Pattern for ConfigManager

**Decision:** ConfigManager acts as a facade that delegates to specialized classes.

**Rationale:**
- Configuration is complex: YAML parsing, validation, serialization, endpoint storage
- Single class would violate Single Responsibility Principle
- Facade provides unified API while internal classes handle specific concerns

**Implementation:**
```cpp
// src/include/config_manager.hpp:560-564
std::unique_ptr<ConfigLoader> config_loader;
std::unique_ptr<EndpointRepository> endpoint_repository;
std::unique_ptr<ConfigValidator> config_validator;
std::unique_ptr<ConfigSerializer> config_serializer;
```

**Delegated responsibilities:**
| Class | Responsibility |
|-------|----------------|
| `ConfigLoader` | YAML file loading, recursive directory scanning |
| `EndpointRepository` | Endpoint storage, lookup by path/slug |
| `ConfigValidator` | Configuration validation rules |
| `ConfigSerializer` | JSON/YAML serialization |
| `EndpointConfigParser` | Parsing endpoint YAML into structs |

**Tradeoffs:**
- (+) Clean separation of concerns
- (+) Each delegated class is testable in isolation
- (+) Easy to extend without modifying facade
- (-) More files to navigate
- (-) Indirection can obscure control flow

**Source files:** `src/config_manager.cpp`, `src/config_loader.cpp`, `src/endpoint_repository.cpp`, `src/config_validator.cpp`, `src/config_serializer.cpp`

---

## 3. Unified REST/MCP Configuration

**Decision:** REST endpoints and MCP tools share the same configuration format.

**Rationale:**
- Many APIs need both REST and MCP access
- Duplicating configurations would cause drift
- Same endpoint can serve both protocols with different interfaces

**Implementation:**
```yaml
# sqls/customers.yaml - serves both REST and MCP
url-path: /customers
method: GET
request:
  - field-name: id
    field-in: query
template-source: customers.sql

mcp-tool:
  name: get_customers
  description: Retrieve customer data
```

**EndpointConfig type detection:**
```cpp
// src/include/config_manager.hpp:223-229
Type getType() const {
    if (!urlPath.empty()) return Type::REST;
    if (mcp_tool.has_value()) return Type::MCP_Tool;
    if (mcp_resource.has_value()) return Type::MCP_Resource;
    if (mcp_prompt.has_value()) return Type::MCP_Prompt;
    return Type::Unknown;
}
```

**Tradeoffs:**
- (+) Single source of truth for endpoint definition
- (+) Automatic MCP tool generation from REST endpoints
- (+) Consistent parameter handling across protocols
- (-) MCP-specific features require optional fields
- (-) Configuration complexity increases

**Source files:** `src/include/config_manager.hpp` (EndpointConfig struct)

---

## 4. Mustache for SQL Templates

**Decision:** Use Mustache templating for SQL generation.

**Rationale:**
- Mustache is logic-less, preventing complex templates
- Clear distinction between double `{{ }}` and triple `{{{ }}}` braces
- Conditional sections for optional parameters
- Familiar to many developers

**Implementation:**
- Triple braces `{{{ }}}` - HTML-escaped output (safe for strings)
- Double braces `{{ }}` - Raw output (for numbers, identifiers)
- Sections `{{#param}}...{{/param}}` - Conditional rendering

**Example:**
```sql
SELECT * FROM customers
WHERE 1=1
{{#params.id}}
  AND customer_id = {{{ params.id }}}
{{/params.id}}
LIMIT {{ params.limit }}
```

**Tradeoffs:**
- (+) Simple, declarative templates
- (+) Security: triple braces escape quotes
- (+) Familiar syntax
- (-) Limited logic capabilities
- (-) No loops or complex conditionals
- (-) Mustache escaping rules can be confusing

**Source files:** `src/sql_template_processor.cpp`

---

## 5. DuckLake for Caching

**Decision:** Use DuckLake (table versioning) for result caching.

**Rationale:**
- Time-travel queries enable cache rollback
- Snapshot-based storage is space-efficient
- Incremental refresh minimizes data transfer
- Built on DuckDB for consistency

**Cache modes:**
| Mode | Description | Use Case |
|------|-------------|----------|
| Full Refresh | Replace entire table | Small, frequently changing data |
| Incremental Append | Add new rows only | Append-only logs, events |
| Incremental Merge | Insert/update/delete | Complex change tracking |

**Implementation:**
```yaml
cache:
  enabled: true
  table: customers_cache
  schedule: "6h"
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
```

**Tradeoffs:**
- (+) Built-in time-travel for debugging
- (+) Efficient snapshot storage
- (+) Flexible refresh strategies
- (-) Requires DuckLake extension
- (-) Schema management complexity
- (-) Learning curve for cache templates

**Source files:** `src/cache_manager.cpp`, `src/heartbeat_worker.cpp`

---

## 6. Middleware Chain Pattern

**Decision:** Use Crow's middleware chain for cross-cutting concerns.

**Rationale:**
- Authentication, rate limiting, CORS are orthogonal to business logic
- Middleware pattern allows clean composition
- Each middleware can short-circuit the chain

**Implementation:**
```cpp
// src/api_server.cpp - middleware registration
crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware> app;
```

**Middleware order:**
1. `CORSHandler` - CORS headers (runs first)
2. `RateLimitMiddleware` - Request rate limiting
3. `AuthMiddleware` - JWT/Basic/OIDC authentication

**Tradeoffs:**
- (+) Separation of concerns
- (+) Reusable across endpoints
- (+) Easy to add/remove middleware
- (-) Order matters and can cause bugs
- (-) Debugging middleware interactions is harder

**Source files:** `src/auth_middleware.cpp`, `src/rate_limit_middleware.cpp`

---

## 7. JSON-RPC for MCP Transport

**Decision:** Use JSON-RPC 2.0 for MCP protocol transport.

**Rationale:**
- MCP specification requires JSON-RPC 2.0
- Simple request/response pattern
- Well-defined error handling
- Language-agnostic

**Implementation:**
```cpp
// src/mcp_route_handlers.cpp - method dispatch
MCPResponse dispatchMCPRequest(const MCPRequest& request) {
    if (request.method == "initialize") return handleInitializeRequest(request);
    if (request.method == "tools/list") return handleToolsListRequest(request);
    if (request.method == "tools/call") return handleToolsCallRequest(request);
    // ...
}
```

**Tradeoffs:**
- (+) Protocol compliance
- (+) Structured error responses
- (+) Session management support
- (-) More verbose than REST
- (-) Requires JSON-RPC parsing layer

**Source files:** `src/mcp_route_handlers.cpp`, `src/mcp_types.hpp`

---

## 8. Defense-in-Depth Security

**Decision:** Multiple layers of input protection.

**Rationale:**
- SQL injection is a critical risk
- No single protection is foolproof
- Validators and escaping complement each other

**Security layers:**
1. **Validators** - Whitelist validation (type, range, pattern)
2. **Triple braces** - Quote escaping in templates
3. **Parameterized queries** - Where applicable
4. **Input constraints** - Length limits, allowed characters

**Implementation:**
```yaml
request:
  - field-name: customer_name
    validators:
      - type: string
        max-length: 200
        pattern: "^[a-zA-Z0-9 ]+$"
```

**Tradeoffs:**
- (+) Multiple barriers to exploitation
- (+) Clear validation rules in configuration
- (+) Fails fast with descriptive errors
- (-) Can be overly restrictive
- (-) Validator configuration overhead

**Source files:** `src/request_validator.cpp`, `src/include/config_manager.hpp` (ValidatorConfig)

---

## Summary

These design decisions prioritize:
1. **Simplicity** - Declarative configuration over complex code
2. **Security** - Defense-in-depth against injection attacks
3. **Flexibility** - Support multiple protocols from unified config
4. **Maintainability** - Clear separation of concerns
5. **Performance** - Caching and efficient query execution

For implementation details, see the [component documentation](./components/).
