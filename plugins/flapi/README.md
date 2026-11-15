# flAPI plugin for Claude Code

Teaches Claude to build, cache, secure, deploy and operate [flAPI](https://github.com/DataZooDE/flapi)
services: REST endpoints and MCP tools from SQL + YAML, DuckLake caching, auth, single-binary deploys,
metrics and diagnosis.

```
/plugin marketplace add DataZooDE/flapi
/plugin install flapi@flapi
```

| Part | What it is |
|---|---|
| `skills/` | 10 skills Claude loads when your task matches: setup, rest, mcp, auth, cache-design, cache-tuning, testing, live-config, deploy, operate |
| `commands/` | `/flapi:init`, `/flapi:new-endpoint`, `/flapi:add-cache`, `/flapi:auth-setup`, `/flapi:deploy-check`, `/flapi:diagnose`, `/flapi:metrics` |
| `.mcp.json` | an MCP server entry for your running flAPI (`$FLAPI_URL`, default `http://localhost:8080`) |
| `hooks/` | after Claude edits a flAPI YAML/SQL file: `flapi --validate-config`, then hot-reload and check the affected endpoints on a running config-service server |

Full guide: [docs/guides/claude-code.md](../../docs/guides/claude-code.md). Local development:
`claude --plugin-dir ./plugins/flapi`; `make check-skills` verifies every example against the real binary.

## Privacy

The plugin collects and sends nothing by itself. Its MCP entry and its hook talk only to the flAPI
server you point them at (`FLAPI_URL`, default `http://localhost:8080`), with the tokens you set
(`FLAPI_CONFIG_SERVICE_TOKEN`, `FLAPI_MCP_TOKEN`). Before sending a token, the hook checks that the
server serves the project you are editing. What flAPI itself reports, and how to turn it off, is
described in [TELEMETRY.md](../../TELEMETRY.md).
