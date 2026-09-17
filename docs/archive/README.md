# Archive — historical, not maintained

These documents describe work that has **already shipped**. They are kept because
they record *why* something was built the way it was, which git history alone does
not convey well.

**Do not treat anything here as current.** Where an archived document and a live
document disagree, the live one is right. If you are looking for how flAPI behaves
today, start at [../README.md](../README.md).

| Document | What it was | Where the current truth lives |
|---|---|---|
| [otel-observability.md](./otel-observability.md) | Implementation plan for the OpenTelemetry epic (P0–P4) | [../OBSERVABILITY.md](../OBSERVABILITY.md), [../spec/components/observability.md](../spec/components/observability.md) |
| [114-readiness-during-warmup.md](./114-readiness-during-warmup.md) | Plan for health/readiness during cache warmup (issue #114) | [../CONFIG_REFERENCE.md](../CONFIG_REFERENCE.md), [../spec/components/caching.md](../spec/components/caching.md) |
| [flapi-09-arrow-content-type.md](./flapi-09-arrow-content-type.md) | Design for Arrow streaming responses (issue #9) | [../CONFIG_REFERENCE.md](../CONFIG_REFERENCE.md) |
| [flapi-10-fs-abstraction.md](./flapi-10-fs-abstraction.md) | Design for the filesystem abstraction (issue #10) | [../spec/DESIGN_DECISIONS.md](../spec/DESIGN_DECISIONS.md) |
| [flapi-11-mcp-configuration-service.md](./flapi-11-mcp-configuration-service.md) | Design for the MCP configuration service (issue #11) | [../MCP_CONFIG_TOOLS_API.md](../MCP_CONFIG_TOOLS_API.md) |
| [vfs-cloud-storage.md](./vfs-cloud-storage.md) | Design for VFS-backed cloud storage | [../CLOUD_STORAGE_GUIDE.md](../CLOUD_STORAGE_GUIDE.md) |

## What was deleted rather than archived

A `docs/bak/` tree (28 files) and a duplicate agent guide (`FLAPI_AGENT.md`) were
removed outright, along with two point-in-time reports that had become misleading
(an integration-test failure snapshot and a client-parity audit pinned to an old
tag). They were unlinked from everywhere and contradicted the live reference docs
— including four OIDC documents superseded by
[CONFIG_REFERENCE.md § 7](../CONFIG_REFERENCE.md). Git history retains them.
