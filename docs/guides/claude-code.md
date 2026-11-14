# Using flAPI with Claude Code

flAPI ships a [Claude Code](https://claude.com/claude-code) plugin. With it
installed, Claude knows how to set up a flAPI project, write REST endpoints and
MCP tools, design and tune DuckLake caches, secure, test, deploy and operate a
server. It checks its own edits against your config, and it can manage a running
server through flAPI's MCP config tools.

## Install

In Claude Code:

```
/plugin marketplace add DataZooDE/flapi
/plugin install flapi@flapi
```

The plugin needs the `flapi` server binary for validation and the `flapii` CLI for
a running server. `pip install flapi-io` installs both.

To update it, run `/plugin marketplace update flapi`. The plugin version follows
flAPI's release version, so use the plugin release that matches your server.

## What you get

### Skills

Claude loads a skill when your request matches it. You do not have to invoke one.

| Phase | Skill | Covers |
|---|---|---|
| Setup | `flapi-setup` | Installing, `flapii project init`, `flapi.yaml`, connections, environment-variable whitelist, DuckDB extensions, validation |
| Develop | `flapi-rest` | Endpoint YAML, parameters and validators, Mustache SQL templates, pagination, write endpoints, YAML includes, rate limits |
| Develop | `flapi-mcp` | MCP tools, resources and prompts, descriptions an agent can act on, async tasks, one endpoint for REST and MCP, connecting clients |
| Develop | `flapi-auth` | Basic, bearer JWT, OIDC with provider presets, roles, MCP OAuth, the config-service token, CORS |
| Develop | `flapi-cache-design` | Full, append and merge caches, writing the populate SQL, cursors and watermarks, deletes, warm-up, write-through, replicas, prebuilt caches, time travel |
| Develop | `flapi-cache-tuning` | Measuring refresh time and cutting it down: predicate pushdown, incremental loads, schedules, inlining, compaction, retention |
| Develop | `flapi-testing` | Template expansion, `--validate-config`, Tavern REST tests, MCP tests, CI |
| Develop | `flapi-live-config` | The config service, `flapii`, the `flapi_*` MCP config tools, hot reload |
| Deploy | `flapi-deploy` | `flapi pack` single binaries, Docker, Cloud Run / serverless, secrets, offline extensions |
| Operate | `flapi-operate` | Health and readiness, runtime metrics, logs, tracing, cache operations, troubleshooting |

### Slash commands

| Command | Does |
|---|---|
| `/flapi:init` | Scaffolds a project from a description of your data, then validates and starts it |
| `/flapi:new-endpoint` | Creates the YAML, SQL, validators, an optional MCP tool and a test, then tries them |
| `/flapi:add-cache` | Picks a cache mode, writes the populate SQL, refreshes, measures and tunes |
| `/flapi:auth-setup` | Configures authentication and roles, and proves the 401/403/200 paths with curl |
| `/flapi:deploy-check` | Runs a pre-deployment checklist and builds the artifact |
| `/flapi:diagnose` | Goes from an error to a root cause and a verified fix |
| `/flapi:metrics` | Summarizes slow or failing endpoints, rejections and slow or stale caches |

### MCP server

The plugin registers a `flapi` MCP server at `$FLAPI_URL/mcp/jsonrpc` (default
`http://localhost:8080`). It sends `Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN`.
Claude then sees your endpoints as MCP tools. If the server runs with
`--config-service`, Claude also sees the `flapi_*` config tools. With those it can
list and edit endpoints, expand templates, refresh caches and read
`flapi_get_metrics`. Every `flapi_*` tool needs the config-service token, which
this header carries. Endpoint changes show up in `tools/list` at once, without a
restart.

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=$(openssl rand -hex 32)
flapi -c flapi.yaml --config-service --config-service-token "$FLAPI_CONFIG_SERVICE_TOKEN"
# then start Claude Code in the same shell
```

### Hook: validate, then hot-reload

After Claude edits a `.yaml`, `.yml` or `.sql` file inside a flAPI project (a
directory with a `flapi.yaml` in it or above it), the plugin:

1. runs `flapi -c <flapi.yaml> --validate-config`, if `flapi` is on `PATH` or
   `FLAPI_BIN` is set. Any errors go back to Claude, which fixes them.
2. reloads each affected endpoint on the running server, if validation passed and
   `FLAPI_CONFIG_SERVICE_TOKEN` is set. It calls
   `POST /api/v1/_config/endpoints/{path}/reload` on `FLAPI_URL`. A file that no
   longer loads is rejected with its reason (the endpoint keeps its previous
   configuration), and a successful reload also updates the MCP tool list.

If something is missing (no binary, no project, no token, no server), the hook
does nothing. It never blocks an edit. A newly created endpoint file is not loaded
yet, so the hook says so: use `flapii endpoints create` or restart the server.

| Variable | Default | Used by |
|---|---|---|
| `FLAPI_URL` | `http://localhost:8080` | MCP server, hook, `flapii` (when `FLAPI_BASE_URL` is unset) |
| `FLAPI_CONFIG_SERVICE_TOKEN` | — | MCP server, hook, `flapii` |
| `FLAPI_BIN` | `flapi` on `PATH` | hook |
| `FLAPI_CONFIG` | nearest `flapi.yaml` | hook |

## Working on the plugin

The plugin lives in this repository under [`plugins/flapi/`](../../plugins/flapi/).
To try local changes without installing:

```bash
claude --plugin-dir ./plugins/flapi
```

`make check-skills` checks every skill against the real code and binaries:

- configuration keys against the parser
- `flapi`/`flapii` commands and options against `--help`
- cache template variables
- the version stamp
- every tagged example, which it runs through `flapi --validate-config` and a live server

CI runs the same check. Update a skill in the same pull request as the feature it
describes.
