# flAPI Specification Documentation

This directory contains architecture and design documentation for flAPI.

## Document Map

| Document | Description |
|----------|-------------|
| [ARCHITECTURE.md](./ARCHITECTURE.md) | High-level system architecture, component diagram, and layered design |
| [DESIGN_DECISIONS.md](./DESIGN_DECISIONS.md) | Rationale for key design choices and patterns |
| [REQUEST_LIFECYCLE.md](./REQUEST_LIFECYCLE.md) | End-to-end request flow for REST and MCP protocols |

## Component Documentation

| Document | Description |
|----------|-------------|
| [components/config-system.md](./components/config-system.md) | ConfigManager facade, loading flow, and YAML parsing |
| [components/query-execution.md](./components/query-execution.md) | SQL template processing and DuckDB integration |
| [components/caching.md](./components/caching.md) | DuckLake caching system and HeartbeatWorker |
| [components/mcp-protocol.md](./components/mcp-protocol.md) | MCP server implementation and JSON-RPC dispatch |
| [components/security.md](./components/security.md) | Authentication, authorization, and validators |

## Reference Documentation

For API and configuration reference, see:

- [CONFIG_REFERENCE.md](../CONFIG_REFERENCE.md) - Configuration file format and options
- [CLI_REFERENCE.md](../CLI_REFERENCE.md) - CLI commands and usage
- [MCP_REFERENCE.md](../MCP_REFERENCE.md) - MCP protocol implementation details
- [CONFIG_SERVICE_API_REFERENCE.md](../CONFIG_SERVICE_API_REFERENCE.md) - Runtime configuration API

## Quick Start

- **New to flAPI?** Start with [ARCHITECTURE.md](./ARCHITECTURE.md) for a system overview
- **Understanding "why"?** Read [DESIGN_DECISIONS.md](./DESIGN_DECISIONS.md)
- **Debugging requests?** See [REQUEST_LIFECYCLE.md](./REQUEST_LIFECYCLE.md)
- **Working on a specific component?** Check the relevant doc in [components/](./components/)

---

*Last updated: January 2026*
