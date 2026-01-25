# Reference Documentation Map

**Last Updated:** January 2025

This guide helps you navigate flAPI's reference documentation and find the right document for your task.

---

## Quick Navigation by Use Case

### I'm getting started with flAPI

1. **First read:** [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § 1 (Overview)
2. **Then:** [CLI_REFERENCE.md](./CLI_REFERENCE.md) § 1 (Quick Start)
3. **Next:** Follow the complete example in [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § Appendix A

### I'm using the REST API

1. **Configuration:** [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) - All sections
2. **Runtime Management:** [CONFIG_SERVICE_API_REFERENCE.md](./CONFIG_SERVICE_API_REFERENCE.md) § 2 (REST API Reference)
3. **CLI Client:** [CONFIG_SERVICE_API_REFERENCE.md](./CONFIG_SERVICE_API_REFERENCE.md) § 3 (CLI Client)
4. **Authentication:** [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § 7 (Authentication)

### I'm using MCP (Model Context Protocol)

1. **Protocol Overview:** [MCP_REFERENCE.md](./MCP_REFERENCE.md) § 1 (Overview)
2. **Getting Started:** [MCP_REFERENCE.md](./MCP_REFERENCE.md) § 2 (Getting Started)
3. **Full Protocol Reference:** [MCP_REFERENCE.md](./MCP_REFERENCE.md) § 3-7 (Methods, Tools, Resources, Prompts)
4. **Configuration:** [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § 2.6 (MCP Configuration)
5. **Authentication:** [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § 7 (Authentication)
6. **Error Codes:** [MCP_REFERENCE.md](./MCP_REFERENCE.md) § Appendix B (Error Reference)

### I'm integrating with MCP configuration tools

1. **Tool Overview:** [MCP_CONFIG_TOOLS_API.md](./MCP_CONFIG_TOOLS_API.md) (Complete reference)
2. **Integration Details:** [MCP_CONFIG_INTEGRATION.md](./MCP_CONFIG_INTEGRATION.md) (Architecture & Flows)
3. **Authentication:** [MCP_CONFIG_INTEGRATION.md](./MCP_CONFIG_INTEGRATION.md) § Authentication Flow
4. **Error Handling:** [MCP_CONFIG_INTEGRATION.md](./MCP_CONFIG_INTEGRATION.md) § Error Handling Strategy

### I'm using cloud storage (S3, GCS, Azure)

1. **Overview & Examples:** [CLOUD_STORAGE_GUIDE.md](./CLOUD_STORAGE_GUIDE.md)
2. **Configuration:** [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § 2.11 (Storage Configuration)
3. **Authentication:** [CLOUD_STORAGE_GUIDE.md](./CLOUD_STORAGE_GUIDE.md) § Authentication

### I'm running the flAPI server

1. **Command-line Options:** [CLI_REFERENCE.md](./CLI_REFERENCE.md) § 2 (Command-Line Options)
2. **Configuration:** [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § 2 (Main Configuration)
3. **Environment Variables:** [CLI_REFERENCE.md](./CLI_REFERENCE.md) § 3 (Environment Variables)

### I'm configuring authentication

**For REST/HTTP:**
- See [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § 7 (Authentication) - primary source for all auth schemes

**For MCP:**
- See [MCP_REFERENCE.md](./MCP_REFERENCE.md) § 8 (Authentication)
- Details in [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § 7 (Authentication schemes and configuration)

**For Config Service API:**
- See [CONFIG_SERVICE_API_REFERENCE.md](./CONFIG_SERVICE_API_REFERENCE.md) § 1 (Authentication)
- Schemes in [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) § 7

### I'm troubleshooting errors

1. **MCP Errors:** [MCP_REFERENCE.md](./MCP_REFERENCE.md) § Appendix B (Error Reference)
2. **Config Service Errors:** [MCP_CONFIG_TOOLS_API.md](./MCP_CONFIG_TOOLS_API.md) § Overview (Error Codes)
3. **HTTP Status Codes:** [CONFIG_SERVICE_API_REFERENCE.md](./CONFIG_SERVICE_API_REFERENCE.md) § Appendix B (HTTP Status Codes)
4. **General Troubleshooting:** [CLAUDE.md](../CLAUDE.md) § Troubleshooting

---

## Document Relationship Diagram

```
                        ┌─────────────────────┐
                        │  CONFIG_REFERENCE   │
                        │  (Master Config)    │
                        └──────────┬──────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    │              │              │
                    ▼              ▼              ▼
            ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
            │     MCP_     │ │   CONFIG_    │ │     CLI_     │
            │  REFERENCE   │ │   SERVICE_   │ │  REFERENCE   │
            │              │ │   API_REF    │ │              │
            └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
                   │                │                │
                   └────────┬───────┴────────┬───────┘
                            │                │
                    ┌───────▼────────┐ ┌────▼──────────┐
                    │  MCP_CONFIG_   │ │  CLOUD_       │
                    │  TOOLS_API     │ │  STORAGE_     │
                    │                │ │  GUIDE        │
                    └────────┬───────┘ └───────────────┘
                             │
                    ┌────────▼──────────┐
                    │  MCP_CONFIG_      │
                    │  INTEGRATION      │
                    │  (Advanced)       │
                    └───────────────────┘
```

### Document Dependencies

- **CONFIG_REFERENCE**: Foundation - all other docs reference configuration options
- **MCP_REFERENCE**: Standalone MCP protocol documentation
- **CONFIG_SERVICE_API_REFERENCE**: Depends on CONFIG_REFERENCE for configuration details
- **MCP_CONFIG_TOOLS_API**: MCP tools for configuration management (references CONFIG_REFERENCE)
- **MCP_CONFIG_INTEGRATION**: Advanced guide for integrating MCP config tools (references MCP_CONFIG_TOOLS_API)
- **CLI_REFERENCE**: Server executable options (references CONFIG_REFERENCE)
- **CLOUD_STORAGE_GUIDE**: Cloud storage usage (references CONFIG_REFERENCE)

---

## Search by Topic

### Authentication
| Topic | Primary | Secondary |
|-------|---------|-----------|
| All auth schemes | [CONFIG_REFERENCE § 7](./CONFIG_REFERENCE.md#7-authentication) | - |
| Basic Auth | [CONFIG_REFERENCE § 7.1](./CONFIG_REFERENCE.md#71-basic-authentication) | [MCP_REFERENCE § 8.1](./MCP_REFERENCE.md#81-basic-auth) |
| JWT/Bearer Auth | [CONFIG_REFERENCE § 7.2-7.3](./CONFIG_REFERENCE.md#72-jwt-authentication) | [MCP_REFERENCE § 8.2](./MCP_REFERENCE.md#82-bearerjwt-auth) |
| OIDC Auth | [CONFIG_REFERENCE § 7.4](./CONFIG_REFERENCE.md#74-oidc-authentication) | [MCP_REFERENCE § 8.3](./MCP_REFERENCE.md#83-oidc-auth) |
| AWS Secrets | [CONFIG_REFERENCE § 7.5](./CONFIG_REFERENCE.md#75-aws-secrets-manager) | - |
| Per-method auth | [MCP_REFERENCE § 8.4](./MCP_REFERENCE.md#84-per-method-auth) | - |

### Endpoints
| Topic | Document |
|-------|----------|
| REST endpoints | [CONFIG_REFERENCE § 3.1](./CONFIG_REFERENCE.md#31-rest-endpoints) |
| MCP tools | [CONFIG_REFERENCE § 3.2](./CONFIG_REFERENCE.md#32-mcp-tools), [MCP_REFERENCE § 5](./MCP_REFERENCE.md#5-tools) |
| MCP resources | [CONFIG_REFERENCE § 3.3](./CONFIG_REFERENCE.md#33-mcp-resources), [MCP_REFERENCE § 6](./MCP_REFERENCE.md#6-resources) |
| MCP prompts | [CONFIG_REFERENCE § 3.4](./CONFIG_REFERENCE.md#34-mcp-prompts), [MCP_REFERENCE § 7](./MCP_REFERENCE.md#7-prompts) |
| Manage endpoints | [CONFIG_SERVICE_API_REFERENCE § 2.2](./CONFIG_SERVICE_API_REFERENCE.md#22-endpoint-management) |

### Caching
| Topic | Document |
|-------|----------|
| Cache configuration | [CONFIG_REFERENCE § 6](./CONFIG_REFERENCE.md#6-cache-configuration) |
| Cache management API | [CONFIG_SERVICE_API_REFERENCE § 2.4](./CONFIG_SERVICE_API_REFERENCE.md#24-cache-management) |
| Cache tools | [MCP_CONFIG_TOOLS_API § Cache Tools](./MCP_CONFIG_TOOLS_API.md#phase-4-cache-tools) |

### Templates
| Topic | Document |
|-------|----------|
| SQL templates | [CONFIG_REFERENCE § 9](./CONFIG_REFERENCE.md#9-sql-templates-mustache) |
| Template management API | [CONFIG_SERVICE_API_REFERENCE § 2.3](./CONFIG_SERVICE_API_REFERENCE.md#23-template-management) |
| Template tools | [MCP_CONFIG_TOOLS_API § Template Tools](./MCP_CONFIG_TOOLS_API.md#phase-2-template-tools) |

### Error Codes
| Protocol | Document |
|----------|----------|
| MCP/JSON-RPC errors | [MCP_REFERENCE § Appendix B](./MCP_REFERENCE.md#appendix-b-error-reference) |
| Config tool errors | [MCP_CONFIG_TOOLS_API § Overview](./MCP_CONFIG_TOOLS_API.md#error-codes) |
| HTTP status codes | [CONFIG_SERVICE_API_REFERENCE § Appendix B](./CONFIG_SERVICE_API_REFERENCE.md#appendix-b-http-status-codes) |

### Validators
| Topic | Document |
|-------|----------|
| All validators | [CONFIG_REFERENCE § 5](./CONFIG_REFERENCE.md#5-validators) |
| Integer validator | [CONFIG_REFERENCE § 5.1](./CONFIG_REFERENCE.md#51-integer-validator) |
| String validator | [CONFIG_REFERENCE § 5.2](./CONFIG_REFERENCE.md#52-string-validator) |
| Enum validator | [CONFIG_REFERENCE § 5.3](./CONFIG_REFERENCE.md#53-enum-validator) |
| Email validator | [CONFIG_REFERENCE § 5.4](./CONFIG_REFERENCE.md#54-email-validator) |
| UUID validator | [CONFIG_REFERENCE § 5.5](./CONFIG_REFERENCE.md#55-uuid-validator) |
| Date validator | [CONFIG_REFERENCE § 5.6](./CONFIG_REFERENCE.md#56-date-validator) |
| Time validator | [CONFIG_REFERENCE § 5.7](./CONFIG_REFERENCE.md#57-time-validator) |

### Storage & Cloud
| Topic | Document |
|-------|----------|
| Cloud storage guide | [CLOUD_STORAGE_GUIDE](./CLOUD_STORAGE_GUIDE.md) |
| VFS configuration | [CONFIG_REFERENCE § 2.11](./CONFIG_REFERENCE.md#211-storage-configuration-vfs) |
| S3 setup | [CLOUD_STORAGE_GUIDE § S3](./CLOUD_STORAGE_GUIDE.md#amazon-s3) |
| GCS setup | [CLOUD_STORAGE_GUIDE § GCS](./CLOUD_STORAGE_GUIDE.md#google-cloud-storage) |
| Azure setup | [CLOUD_STORAGE_GUIDE § Azure](./CLOUD_STORAGE_GUIDE.md#azure-blob-storage) |

### CLI & Server
| Topic | Document |
|-------|----------|
| Server startup | [CLI_REFERENCE § 1](./CLI_REFERENCE.md#1-overview) |
| Command-line options | [CLI_REFERENCE § 2](./CLI_REFERENCE.md#2-command-line-options) |
| flapii CLI client | [CONFIG_SERVICE_API_REFERENCE § 3](./CONFIG_SERVICE_API_REFERENCE.md#3-cli-client-flapii) |

---

## File Sizes and Scope

| Document | Size | Scope | Audience |
|----------|------|-------|----------|
| CONFIG_REFERENCE | 1700+ lines | Complete configuration reference | Developers, DevOps |
| MCP_REFERENCE | 1700+ lines | MCP protocol details | AI integrators, Developers |
| CONFIG_SERVICE_API_REFERENCE | 1550+ lines | REST API & CLI | API users, DevOps |
| MCP_CONFIG_TOOLS_API | 720 lines | 20 config tools | AI integrators |
| MCP_CONFIG_INTEGRATION | 500+ lines | Integration guide | Advanced users |
| CLI_REFERENCE | 400+ lines | Server executable | DevOps, Deployment |
| CLOUD_STORAGE_GUIDE | 200+ lines | Cloud storage setup | Cloud engineers |

---

## How to Contribute

When adding new features or configuration options:

1. **Document in CONFIG_REFERENCE.md** - Add to appropriate section (§ 2 for main config, § 3 for endpoints, etc.)
2. **Link from secondary docs** - If relevant to MCP/REST/CLI, add references in Related Documentation sections
3. **Update REFERENCE_MAP.md** - Add new topic to search tables and quick navigation
4. **Verify cross-references** - Use markdown link format: `[Text](./FILE.md#section)`

---

## Related Documentation

- **[CLAUDE.md](../CLAUDE.md)** - Project instructions and development guidelines
- **[Architecture Documentation](./spec/ARCHITECTURE.md)** - System design and components
- **[Design Decisions](./spec/DESIGN_DECISIONS.md)** - Why certain choices were made
- **[Request Lifecycle](./spec/REQUEST_LIFECYCLE.md)** - How requests flow through the system
- **[Component Docs](./spec/components/)** - Detailed component documentation
