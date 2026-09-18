# flAPI documentation

flAPI turns SQL templates and YAML into REST APIs and MCP tools. These docs are
organised by **what you are trying to do**. Each guide gets you working, then
links to the reference for every option and to `spec/` for how it works inside.

New to flAPI? Start with [Getting started](./guides/getting-started.md).

---

## I want to…

### Get something running

| Goal | Start here |
|---|---|
| Install flAPI and serve my first endpoint | [guides/getting-started.md](./guides/getting-started.md) |
| Understand the config file layout | [CONFIG_REFERENCE.md § 1](./CONFIG_REFERENCE.md#1-overview) |
| Run the server with the right flags | [CLI_REFERENCE.md](./CLI_REFERENCE.md) |

### Build an API

| Goal | Start here |
|---|---|
| Add a REST endpoint | [guides/rest-endpoints.md](./guides/rest-endpoints.md) |
| Write a SQL template safely | [guides/rest-endpoints.md#writing-the-sql-template](./guides/rest-endpoints.md#writing-the-sql-template) · [CONFIG_REFERENCE.md § 9](./CONFIG_REFERENCE.md) |
| Validate incoming parameters | [CONFIG_REFERENCE.md § 5](./CONFIG_REFERENCE.md) |
| Reuse config across endpoints, or read secrets from the environment | [guides/yaml-includes.md](./guides/yaml-includes.md) |
| Read from S3, GCS or Azure | [guides/cloud-storage.md](./guides/cloud-storage.md) |

### Expose it to an AI agent

| Goal | Start here |
|---|---|
| Turn an endpoint into an MCP tool | [guides/mcp-tools.md](./guides/mcp-tools.md) |
| Look up an MCP method, error code or capability | [MCP_REFERENCE.md](./MCP_REFERENCE.md) |
| Manage configuration through MCP tools | [MCP_CONFIG_TOOLS_API.md](./MCP_CONFIG_TOOLS_API.md) · [MCP_CONFIG_INTEGRATION.md](./MCP_CONFIG_INTEGRATION.md) |

### Make it fast

| Goal | Start here |
|---|---|
| Cache an expensive query | [guides/caching.md](./guides/caching.md) |
| Choose full refresh vs incremental | [guides/caching.md#choosing-a-refresh-strategy](./guides/caching.md#choosing-a-refresh-strategy) |
| Understand cache snapshots and time travel | [spec/components/caching.md](./spec/components/caching.md) |

### Secure it

| Goal | Start here |
|---|---|
| Require authentication | [guides/authentication.md](./guides/authentication.md) · [CONFIG_REFERENCE.md § 7](./CONFIG_REFERENCE.md) |
| Restrict an endpoint by role | [guides/authentication.md#roles](./guides/authentication.md#roles) |
| Understand the layered defences | [spec/components/security.md](./spec/components/security.md) |

### Operate it

| Goal | Start here |
|---|---|
| Trace requests, find a slow one, join logs to traces | [OBSERVABILITY.md](./OBSERVABILITY.md) |
| See where the time went inside a query | [OBSERVABILITY.md § 4](./OBSERVABILITY.md#4-seeing-inside-a-slow-query) |
| Get an id out of a failed request | [OBSERVABILITY.md § 7](./OBSERVABILITY.md#7-getting-an-id-out-of-a-failed-request) |
| Check that span export is working | [OBSERVABILITY.md § 9](./OBSERVABILITY.md#9-checking-that-export-is-actually-working) |
| Keep an audit trail | [OBSERVABILITY.md § 8](./OBSERVABILITY.md#8-correlation) · [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) |
| Change configuration at runtime | [CONFIG_SERVICE_API_REFERENCE.md](./CONFIG_SERVICE_API_REFERENCE.md) |
| Understand what product telemetry flAPI sends, and turn it off | [../TELEMETRY.md](../TELEMETRY.md) |

### Deploy it

| Goal | Start here |
|---|---|
| Ship one self-contained binary | [guides/self-packaging.md](./guides/self-packaging.md) |
| Run in Kubernetes, serverless or air-gapped | [OBSERVABILITY.md § 6](./OBSERVABILITY.md#6-deployment) |

---

## Reference

Exhaustive, option-by-option. Reach for these once you know what you are looking
for.

| Document | Covers |
|---|---|
| [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) | Every key in `flapi.yaml` and in an endpoint YAML |
| [CLI_REFERENCE.md](./CLI_REFERENCE.md) | `flapi` server flags, subcommands, environment variables |
| [MCP_REFERENCE.md](./MCP_REFERENCE.md) | The MCP protocol as flAPI implements it |
| [CONFIG_SERVICE_API_REFERENCE.md](./CONFIG_SERVICE_API_REFERENCE.md) | The runtime configuration REST API and `flapii` |
| [MCP_CONFIG_TOOLS_API.md](./MCP_CONFIG_TOOLS_API.md) | The MCP configuration tools |
| [MCP_CONFIG_INTEGRATION.md](./MCP_CONFIG_INTEGRATION.md) | How those tools are wired together |
| [CLOUD_STORAGE_GUIDE.md](./CLOUD_STORAGE_GUIDE.md) | S3 / GCS / Azure in depth: every provider, auth and failure mode |
| [OBSERVABILITY.md](./OBSERVABILITY.md) | Tracing, audit and log correlation |

---

## How it works

Implementation deep dives. Read these when you are changing flAPI, debugging
something strange, or deciding whether a design will fit.

| Document | Covers |
|---|---|
| [spec/ARCHITECTURE.md](./spec/ARCHITECTURE.md) | The layers and how components fit together |
| [spec/REQUEST_LIFECYCLE.md](./spec/REQUEST_LIFECYCLE.md) | One request, end to end, REST and MCP |
| [spec/DESIGN_DECISIONS.md](./spec/DESIGN_DECISIONS.md) | Why things are the way they are, and what was rejected |
| [spec/components/config-system.md](./spec/components/config-system.md) | Config loading, validation, the copy-on-write endpoint table |
| [spec/components/query-execution.md](./spec/components/query-execution.md) | Template rendering and DuckDB execution |
| [spec/components/caching.md](./spec/components/caching.md) | DuckLake caching internals |
| [spec/components/mcp-protocol.md](./spec/components/mcp-protocol.md) | MCP dispatch, sessions, SEP-414 trace context |
| [spec/components/security.md](./spec/components/security.md) | Auth, validation, and telemetry egress |
| [spec/components/observability.md](./spec/components/observability.md) | Span tree, request context, capture tiers, exporters |

---

## Elsewhere

- [../Readme.md](../Readme.md) — what flAPI is and a five-minute start
- [../CHANGELOG.md](../CHANGELOG.md) — what changed in each release
- [../TELEMETRY.md](../TELEMETRY.md) — product telemetry and how to disable it
- [archive/](./archive/) — historical design docs and completed plans, **not maintained**
