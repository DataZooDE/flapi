**Software Architecture Design Document**

MCP Tools for Configuration Management

(Operate flAPI via MCP)

GitHub Issue: DataZooDE/flapi\#11


# **1\. Executive Summary**

This document presents the architectural design for extending flAPI with MCP (Model Context Protocol) tools that enable AI agents to manage flAPI configurations programmatically. While flAPI already supports creating MCP tools on top of data, this feature adds MCP tools to operate flAPI itself, allowing AI agents to introspect, create, modify, and manage API endpoints, templates, caches, and project configurations.

The design prioritizes reuse of existing infrastructure, idiomatic integration with flAPI's declarative paradigm, and developer experience for serverless agentic AI coding environments such as Cursor, Windsurf, and Claude Code.

## **1.1 Design Goals**

* Maximize reuse of existing ConfigService handlers and MCP server infrastructure  
* Maintain idiomatic consistency with flAPI's declarative YAML-first approach  
* Enable zero-friction adoption in modern agentic AI coding environments  
* Support serverless deployment patterns with minimal cold-start overhead  
* Preserve security boundaries through consistent authentication enforcement
* Try to align MCP tools and CLI interface `flapii` as much as possible

# **2\. Context and Background**

## **2.1 Current flAPI Architecture**

flAPI currently operates as a unified server exposing two concurrent interfaces: a REST API (default port 8080\) and an MCP server (default port 8081). Both interfaces share the same underlying DuckDB engine, SQL template processing, caching layer, and authentication mechanisms.

The existing MCP implementation supports declarative tool and resource definitions through YAML configuration files. Configuration files can define REST endpoints (via url-path), MCP tools (via mcp-tool), or MCP resources (via mcp-resource), with automatic detection based on the presence of these keys.

## **2.2 Existing Config Service**

flAPI includes a comprehensive Config Service exposing REST endpoints under /api/v1/\_config/\* for programmatic configuration management. This service is structured around specialized handlers: FilesystemHandler, SchemaHandler, EndpointConfigHandler, TemplateHandler, CacheConfigHandler, and others. Each handler encapsulates domain-specific logic and is already designed for reuse.

## **2.3 The Opportunity**

AI agents (Claude, Cursor, Windsurf) working with flAPI projects currently rely on flapii cli, file system operations and REST API calls. By exposing Config Service functionality as MCP tools, agents gain native protocol-level access with rich schema information, enabling more intelligent interactions and reducing integration friction.

# **3\. Architecture Overview**

## **3.1 Layered Architecture**

The implementation follows a layered approach that cleanly separates concerns while maximizing code reuse:

| Layer | Responsibility | Components |
| :---- | :---- | :---- |
| **MCP Protocol** | JSON-RPC 2.0 transport, tool discovery, schema exposure | McpServer (existing), extended tool registry |
| **Tool Adapter** | Translate MCP tool calls to handler invocations | ConfigToolAdapter (new) |
| **Business Logic** | Domain operations, validation, state management | ConfigService handlers (existing) |
| **Data Layer** | File I/O, DuckDB queries, cache operations | YAML parser, DuckDB, DuckLake |

## **3.2 Key Architectural Decisions**

### **AD-1: Handler Reuse Pattern**

**Decision:** MCP tools delegate to existing ConfigService handlers rather than duplicating logic.

**Rationale:** The ConfigService handlers already encapsulate validated, tested business logic. Duplication would create maintenance burden and potential behavioral divergence between REST and MCP interfaces.

**Consequence:** Tool implementations become thin adapter layers, reducing implementation risk and ensuring behavioral consistency.

### **AD-2: Auto-Registration at Startup**

**Decision:** Config tools are automatically registered when the MCP server starts, alongside declarative tools from YAML configurations.

**Rationale:** This matches the existing pattern for declarative MCP tools and ensures config tools are always available without additional configuration.

**Consequence:** Zero configuration required to enable config management capabilities; tools appear in tools/list responses automatically.

### **AD-3: Unified Authentication**

**Decision:** Config tools respect the existing Config Service token authentication (X-Config-Token / Authorization: Bearer). Also the activation of the config MCP is bound to the config service activation. When config-service is deactivated just the declarative MCP tools are visible.

**Rationale:** Security boundaries must be consistent across all interfaces. Reusing the existing authentication mechanism prevents security gaps.

**Consequence:** MCP clients must provide valid authentication tokens; this aligns with enterprise security requirements.

### **AD-4: Read-Write Separation**

**Decision:** Tools are categorized as read-only (introspection) or mutating (creation/modification) with distinct naming conventions.

**Rationale:** This enables agents to make informed decisions about tool usage and supports future implementation of differentiated authorization policies.

**Consequence:** Tool names follow the pattern flapi\_get\_\* for reads and flapi\_create\_\*, flapi\_update\_\*, flapi\_delete\_\* for mutations.

# **4\. Tool Catalog**

## **4.1 Design Principles for Tool Definitions**

Each tool follows these principles to ensure consistency and usability:

* Descriptive naming that mirrors flapii CLI commands where applicable  
* Input schemas defined using JSON Schema for validation and LLM guidance  
* Output schemas that match existing REST API response structures  
* Comprehensive descriptions that help LLMs understand when to use each tool

## **4.2 Project & Configuration Discovery Tools**

| Tool Name | Description | Handler |
| :---- | :---- | :---- |
| flapi\_get\_project\_config | Get current project configuration including connections, DuckLake settings | ProjectHandler |
| flapi\_get\_environment | List available environment variables matching whitelist patterns | EnvironmentHandler |
| flapi\_get\_filesystem | Get template directory tree with YAML/SQL file detection | FilesystemHandler |
| flapi\_get\_schema | Introspect database schema (tables, columns, types) for a connection | SchemaHandler |
| flapi\_refresh\_schema | Refresh cached database schema information | SchemaHandler |

## **4.3 Endpoint Management Tools**

| Tool Name | Description | Handler |
| :---- | :---- | :---- |
| flapi\_list\_endpoints | List all configured API endpoints with their properties | EndpointConfigHandler |
| flapi\_get\_endpoint | Get detailed configuration for a specific endpoint | EndpointConfigHandler |
| flapi\_create\_endpoint | Create a new API endpoint configuration YAML | EndpointConfigHandler |
| flapi\_update\_endpoint | Modify existing endpoint configuration | EndpointConfigHandler |
| flapi\_delete\_endpoint | Remove endpoint configuration | EndpointConfigHandler |
| flapi\_reload\_endpoint | Hot-reload endpoint without server restart | EndpointConfigHandler |

## **4.4 Template Management Tools**

| Tool Name | Description | Handler |
| :---- | :---- | :---- |
| flapi\_get\_template | Retrieve SQL template content for an endpoint | TemplateHandler |
| flapi\_update\_template | Write or update SQL template content | TemplateHandler |
| flapi\_expand\_template | Expand Mustache template with provided parameters | TemplateHandler |
| flapi\_test\_template | Execute template against connection and return results | TemplateHandler |

## **4.5 Cache Management Tools**

| Tool Name | Description | Handler |
| :---- | :---- | :---- |
| flapi\_get\_cache\_status | Get cache status including snapshots and last refresh | CacheConfigHandler |
| flapi\_refresh\_cache | Trigger manual cache refresh for an endpoint | CacheConfigHandler |
| flapi\_get\_cache\_audit | Retrieve cache sync event audit logs | CacheConfigHandler |
| flapi\_run\_cache\_gc | Trigger garbage collection based on retention policy | CacheConfigHandler |

# **5\. Component Design**

## **5.1 ConfigToolAdapter**

The ConfigToolAdapter serves as the bridge between the MCP protocol layer and the existing ConfigService handlers. This component is responsible for tool registration, request translation, authentication enforcement, and response formatting.

### **Responsibilities**

* Register all config tools with the MCP server at startup  
* Translate MCP tool call arguments to handler request objects  
* Enforce authentication before handler invocation  
* Convert handler responses to MCP tool result format  
* Map handler errors to MCP error codes

### **Integration Points**

The adapter integrates with the existing McpServer through the tool registration interface. Tools are registered with their JSON Schema input definitions, enabling LLMs to understand available parameters and validation constraints.

## **5.2 Tool Schema Generation**

Input schemas for MCP tools are derived from the existing REST API parameter definitions and handler validation logic. This ensures consistency between REST and MCP interfaces while providing rich type information for LLM tool selection.

Output schemas align with existing REST response structures, maintaining compatibility with any tooling or documentation built around the current API.

## **5.3 Authentication Flow**

The authentication flow for MCP config tools mirrors the existing Config Service authentication:

1. MCP client includes authentication token in tool call metadata  
2. ConfigToolAdapter extracts token from request context  
3. Token is validated using existing ConfigService authentication middleware  
4. Handler is invoked only if authentication succeeds  
5. Authentication failures return standard MCP error responses

# **6\. Serverless & Developer Experience**

## **6.1 Design for Agentic AI Coding**

Modern AI coding assistants like Cursor, Windsurf, and Claude Code benefit from MCP servers that start quickly and provide rich schema information. The config tools are designed with these environments in mind:

* Zero additional configuration required beyond existing flAPI setup  
* Comprehensive tool descriptions that guide agent tool selection  
* Consistent naming conventions that align with CLI commands  
* Error messages that provide actionable guidance

## **6.2 Serverless Deployment Patterns**

flAPI's small footprint and millisecond startup time make it suitable for serverless deployments. The config tools support these patterns through:

* Stateless tool implementations that don't require persistent connections  
* Idempotent operations where applicable  
* File-based configuration that works with container ephemeral storage  
* No additional runtime dependencies beyond the flAPI binary

## **6.3 Example Agent Workflow**

The following illustrates how an AI agent might use these tools to create a new API endpoint:

6. Agent calls flapi\_get\_schema to understand available tables and columns  
7. Agent calls flapi\_list\_endpoints to review existing patterns  
8. Agent calls flapi\_create\_endpoint with generated YAML configuration  
9. Agent calls flapi\_update\_template to write the SQL template  
10. Agent calls flapi\_test\_template to validate with sample parameters  
11. Agent calls flapi\_reload\_endpoint for hot-reload without restart

# **7\. Implementation Strategy**

## **7.1 Phased Approach**

Implementation proceeds in phases to deliver value incrementally while managing risk:

### **Phase 1: Read-Only Discovery Tools**

Implement introspection tools (flapi\_get\_project\_config, flapi\_get\_schema, flapi\_list\_endpoints, flapi\_get\_filesystem) that enable agents to explore and understand flAPI configurations without modification risk.

### **Phase 2: Template Tools**

Add template management tools (flapi\_get\_template, flapi\_expand\_template, flapi\_test\_template) that support iterative SQL development workflows.

### **Phase 3: Mutation Tools**

Implement creation and modification tools (flapi\_create\_endpoint, flapi\_update\_endpoint, flapi\_update\_template) with appropriate validation and safety checks.

### **Phase 4: Cache & Operations Tools**

Add cache management tools and hot-reload capabilities for production operations support.

## **7.2 Extension Points**

The architecture supports future extensions:

* Additional tools can be added by implementing new handler methods  
* Tool permissions can be refined through role-based access control  
* Streaming support for long-running operations (cache refresh)  
* MCP Resources for exposing configuration as readable context

# **8\. Testing Strategy**

## **8.1 Unit Testing**

Each tool implementation is unit tested in isolation, verifying correct argument translation, handler invocation, and response formatting. Mock handlers enable testing of the adapter layer without requiring full system integration.

## **8.2 Integration Testing**

Integration tests verify end-to-end tool operation through the MCP JSON-RPC interface. Test scenarios cover authentication, successful operations, error conditions, and edge cases.

## **8.3 Agent Workflow Testing**

Realistic agent workflows are tested using recorded tool call sequences. This validates that tools compose correctly and provide sufficient information for agent decision-making.

# **9\. Appendix: Design Rationale Summary**

This section summarizes the key design decisions and their rationale for quick reference.

| Design Choice | Alternatives Considered | Why This Approach |
| :---- | :---- | :---- |
| Handler reuse | Duplicate logic in tool implementations | Maintains consistency, reduces maintenance |
| Auto-registration | Explicit YAML configuration | Zero-config experience, always available |
| Unified auth | Separate MCP auth mechanism | Consistent security, simpler operations |
| JSON-RPC over HTTP | stdio transport | Serverless friendly, multi-client |
| CLI-aligned naming | REST-aligned naming | Developer familiarity, discoverability |

*End of Document*
